#include "recorder.h"
#include "settings.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

namespace airscreen {
namespace {

// ~270 ms at 60 fps: covers the encoder's start-up at the head of a clip. Past this a frame is skipped,
// never the stream.
constexpr size_t kMaxQueuedVideo = 16;
constexpr int kMaxStaging = kMaxQueuedVideo + 2;
constexpr int kAudioRate = 48000;
constexpr int kAudioChannels = 2;
constexpr int kVideoTimebase = 90000;
constexpr int kGopSeconds = 2;

void rlog(const char *fmt, ...) {
    char buf[384];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    append_log((std::string("recorder: ") + buf).c_str());
}

std::string av_error(int err) {
    char es[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, es, sizeof(es));
    return es;
}

std::string utf8(const std::wstring &w) {
    if (w.empty()) {
        return {};
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int) w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int) w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

int64_t local_us() {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (int64_t) (now.QuadPart / freq.QuadPart * 1000000 + now.QuadPart % freq.QuadPart * 1000000 / freq.QuadPart);
}

// Copies an NV12 image (two planes with their own pitches) into an encoder frame.
void copy_nv12(AVFrame *dst, const uint8_t *y, int y_pitch, const uint8_t *uv, int uv_pitch, int w, int h) {
    for (int row = 0; row < h; ++row) {
        memcpy(dst->data[0] + (size_t) row * dst->linesize[0], y + (size_t) row * y_pitch, (size_t) w);
    }
    for (int row = 0; row < h / 2; ++row) {
        memcpy(dst->data[1] + (size_t) row * dst->linesize[1], uv + (size_t) row * uv_pitch, (size_t) w);
    }
}

} // namespace

struct Recorder::Out {
    AVFormatContext *fmt = nullptr;
    AVCodecContext *venc = nullptr;
    AVCodecContext *aenc = nullptr;
    AVStream *vst = nullptr;
    AVStream *ast = nullptr;
    AVPacket *pkt = nullptr;
    SwrContext *swr = nullptr;
    int swr_rate = 0;
    int swr_ch = 0;
    AVAudioFifo *fifo = nullptr;
    int64_t a_next = 0; // pts (in samples) of the first sample waiting in the fifo
    bool a_anchored = false;
    int w = 0;
    int h = 0;
    int64_t part_base_us = 0;
    int64_t last_vpts = -1;
    bool header_written = false;
};

Recorder::Recorder() = default;

Recorder::~Recorder() {
    stop();
    for (auto *t : pool_) {
        t->Release();
    }
    pool_.clear();
}

bool Recorder::start(const std::wstring &path) {
    stop();
    std::lock_guard<std::mutex> life(life_mu_);
    {
        std::lock_guard<std::mutex> lock(info_mu_);
        path_ = path;
        cur_path_ = path;
        error_.clear();
        encoder_.clear();
        part_ = 1;
    }
    base_us_ = INT64_MIN;
    last_video_us_ = 0;
    dropped_ = 0;
    run_ = true;
    active_ = true;
    thread_ = std::thread([this] { writer_loop(); });
    rlog("armed: %s", utf8(path).c_str());
    return true;
}

void Recorder::stop() {
    std::lock_guard<std::mutex> life(life_mu_);
    if (!thread_.joinable()) {
        active_ = false;
        return;
    }
    {
        std::lock_guard<std::mutex> lock(q_mu_);
        run_ = false;
    }
    q_cv_.notify_all();
    thread_.join();
    active_ = false;
    std::lock_guard<std::mutex> lock(q_mu_);
    for (auto &it : q_) {
        release_item(it);
    }
    q_.clear();
    queued_video_ = 0;
}

double Recorder::seconds() const {
    int64_t base = base_us_.load();
    if (base == INT64_MIN) {
        return 0;
    }
    return (double) (last_video_us_.load() - base) / 1e6;
}

std::wstring Recorder::path() const {
    std::lock_guard<std::mutex> lock(info_mu_);
    return cur_path_;
}

std::string Recorder::last_error() const {
    std::lock_guard<std::mutex> lock(info_mu_);
    return error_;
}

std::string Recorder::encoder_name() const {
    std::lock_guard<std::mutex> lock(info_mu_);
    return encoder_;
}

void Recorder::set_error(const std::string &msg) {
    rlog("error: %s", msg.c_str());
    std::lock_guard<std::mutex> lock(info_mu_);
    if (error_.empty()) {
        error_ = msg;
    }
}

std::wstring Recorder::part_path(int part) const {
    std::lock_guard<std::mutex> lock(info_mu_);
    if (part <= 1) {
        return path_;
    }
    std::wstring p = path_;
    size_t dot = p.find_last_of(L'.');
    std::wstring suffix = L"_" + std::to_wstring(part);
    if (dot == std::wstring::npos) {
        return p + suffix;
    }
    return p.substr(0, dot) + suffix + p.substr(dot);
}

ID3D11Texture2D *Recorder::take_staging(ID3D11Device *device, int w, int h) {
    std::lock_guard<std::mutex> lock(pool_mu_);
    if (w != pool_w_ || h != pool_h_) {
        for (auto *t : pool_) {
            t->Release();
        }
        pool_.clear();
        pool_w_ = w;
        pool_h_ = h;
        pool_out_ = 0;
    }
    if (!pool_.empty()) {
        ID3D11Texture2D *t = pool_.back();
        pool_.pop_back();
        ++pool_out_;
        return t;
    }
    if (pool_out_ >= kMaxStaging) {
        return nullptr;
    }
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = (UINT) w;
    td.Height = (UINT) h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_NV12;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *t = nullptr;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &t))) {
        return nullptr;
    }
    ++pool_out_;
    return t;
}

void Recorder::return_staging(ID3D11Texture2D *tex, int w, int h) {
    if (!tex) {
        return;
    }
    std::lock_guard<std::mutex> lock(pool_mu_);
    if (w == pool_w_ && h == pool_h_) {
        --pool_out_;
        pool_.push_back(tex);
    } else {
        tex->Release();
    }
}

void Recorder::release_item(Item &it) {
    if (it.staging) {
        return_staging(it.staging, it.w, it.h);
        it.staging = nullptr;
    }
    if (it.sw_frame) {
        av_frame_free((AVFrame **) &it.sw_frame);
    }
}

void Recorder::on_video_frame(ID3D11Device *device, ID3D11DeviceContext *ctx, const void *frame_ptr, int w, int h) {
    if (!active_.load() || !frame_ptr || w <= 0 || h <= 0) {
        return;
    }
    auto *frame = (const AVFrame *) frame_ptr;
    int64_t pts = frame->pts != AV_NOPTS_VALUE ? frame->pts : local_us();
    {
        std::lock_guard<std::mutex> lock(q_mu_);
        if (!run_) {
            return;
        }
        if (queued_video_ >= kMaxQueuedVideo) {
            if ((++dropped_ % 60) == 1) {
                rlog("encoder behind, skipped %llu frames so far", (unsigned long long) dropped_);
            }
            return;
        }
    }
    Item it;
    it.video = true;
    it.pts_us = pts;
    it.w = w;
    it.h = h;
    if (frame->format == AV_PIX_FMT_D3D11) {
        auto *tex = (ID3D11Texture2D *) frame->data[0];
        if (!tex) {
            return;
        }
        it.staging = take_staging(device, w, h);
        if (!it.staging) {
            return;
        }
        D3D11_BOX box = {0, 0, 0, (UINT) w, (UINT) h, 1};
        ctx->CopySubresourceRegion(it.staging, 0, 0, 0, 0, tex, (UINT) (intptr_t) frame->data[1], &box);
        ctx_ = ctx;
    } else if (frame->format == AV_PIX_FMT_NV12 || frame->format == AV_PIX_FMT_YUV420P ||
               frame->format == AV_PIX_FMT_YUVJ420P) {
        it.sw_frame = av_frame_clone(frame);
        if (!it.sw_frame) {
            return;
        }
    } else {
        return;
    }
    int64_t expect = INT64_MIN;
    base_us_.compare_exchange_strong(expect, pts);
    last_video_us_ = pts;
    {
        std::lock_guard<std::mutex> lock(q_mu_);
        ++queued_video_;
        q_.push_back(std::move(it));
    }
    q_cv_.notify_one();
}

void Recorder::on_audio_pcm(const int16_t *pcm, int frames, int channels, int rate, uint64_t ntp_ns) {
    if (!active_.load() || !pcm || frames <= 0 || channels <= 0 || rate <= 0) {
        return;
    }
    if (base_us_.load() == INT64_MIN) {
        return; // the clip starts with its first picture
    }
    Item it;
    it.video = false;
    it.pts_us = ntp_ns ? (int64_t) (ntp_ns / 1000) : INT64_MIN;
    it.channels = channels;
    it.rate = rate;
    it.pcm.assign(pcm, pcm + (size_t) frames * (size_t) channels);
    {
        std::lock_guard<std::mutex> lock(q_mu_);
        if (!run_ || q_.size() > 256) {
            return;
        }
        q_.push_back(std::move(it));
    }
    q_cv_.notify_one();
}

void Recorder::writer_loop() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    bool failed = false;
    for (;;) {
        Item it;
        {
            std::unique_lock<std::mutex> lock(q_mu_);
            q_cv_.wait(lock, [this] { return !q_.empty() || !run_; });
            if (q_.empty()) {
                break; // stopping, and everything queued has been written
            }
            it = std::move(q_.front());
            q_.pop_front();
            if (it.video) {
                --queued_video_;
            }
        }
        if (!failed) {
            if (it.video) {
                failed = !handle_video(it);
            } else if (out_) {
                handle_audio(it);
            }
            if (failed) {
                active_ = false; // the UI notices and reports last_error()
                std::lock_guard<std::mutex> lock(q_mu_);
                run_ = false;
            }
        }
        release_item(it);
    }
    close_output();
    uint64_t dropped;
    {
        std::lock_guard<std::mutex> lock(q_mu_);
        dropped = dropped_;
    }
    rlog("stopped after %.1f s, %llu frames skipped", seconds(), (unsigned long long) dropped);
}

bool Recorder::handle_video(Item &it) {
    if (!out_ || out_->w != it.w || out_->h != it.h) {
        if (out_) {
            // The phone rotated: finish this file and carry on in the next one.
            close_output();
            int part;
            {
                std::lock_guard<std::mutex> lock(info_mu_);
                part = ++part_;
            }
            std::wstring next = part_path(part);
            std::lock_guard<std::mutex> lock(info_mu_);
            cur_path_ = next;
        }
        if (!open_output(it.w, it.h)) {
            close_output();
            return false;
        }
        out_->part_base_us = it.pts_us;
    }
    Out &o = *out_;

    AVFrame *f = av_frame_alloc();
    if (!f) {
        return true;
    }
    f->format = AV_PIX_FMT_NV12;
    f->width = it.w;
    f->height = it.h;
    if (av_frame_get_buffer(f, 0) < 0) {
        av_frame_free(&f);
        return true;
    }
    bool ok = false;
    if (it.staging && ctx_) {
        D3D11_MAPPED_SUBRESOURCE map = {};
        HRESULT hr;
        // Poll instead of blocking inside Map, which would hold the device lock the decode thread needs.
        while ((hr = ctx_->Map(it.staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &map)) ==
               DXGI_ERROR_WAS_STILL_DRAWING) {
            Sleep(1);
        }
        if (SUCCEEDED(hr)) {
            const uint8_t *y = (const uint8_t *) map.pData;
            copy_nv12(f, y, (int) map.RowPitch, y + (size_t) map.RowPitch * it.h, (int) map.RowPitch, it.w, it.h);
            ctx_->Unmap(it.staging, 0);
            ok = true;
        }
    } else if (it.sw_frame) {
        auto *src = (AVFrame *) it.sw_frame;
        if (src->format == AV_PIX_FMT_NV12) {
            copy_nv12(f, src->data[0], src->linesize[0], src->data[1], src->linesize[1], it.w, it.h);
        } else {
            for (int row = 0; row < it.h; ++row) {
                memcpy(f->data[0] + (size_t) row * f->linesize[0], src->data[0] + (size_t) row * src->linesize[0],
                       (size_t) it.w);
            }
            for (int row = 0; row < it.h / 2; ++row) {
                uint8_t *dst = f->data[1] + (size_t) row * f->linesize[1];
                const uint8_t *u = src->data[1] + (size_t) row * src->linesize[1];
                const uint8_t *v = src->data[2] + (size_t) row * src->linesize[2];
                for (int x = 0; x < it.w / 2; ++x) {
                    dst[x * 2] = u[x];
                    dst[x * 2 + 1] = v[x];
                }
            }
        }
        ok = true;
    }
    if (!ok) {
        av_frame_free(&f);
        return true;
    }

    int64_t pts = (it.pts_us - o.part_base_us) * kVideoTimebase / 1000000;
    if (pts <= o.last_vpts) {
        pts = o.last_vpts + 1;
    }
    o.last_vpts = pts;
    f->pts = pts;
    int err = avcodec_send_frame(o.venc, f);
    av_frame_free(&f);
    if (err < 0 && err != AVERROR(EAGAIN)) {
        set_error("video encode failed: " + av_error(err));
        return false;
    }
    return write_packets(o.venc, o.vst, false);
}

void Recorder::handle_audio(Item &it) {
    Out &o = *out_;
    if (!o.aenc || !o.fifo) {
        return;
    }
    if (!o.swr || o.swr_rate != it.rate || o.swr_ch != it.channels) {
        swr_free(&o.swr);
        AVChannelLayout in_layout, out_layout;
        av_channel_layout_default(&in_layout, it.channels);
        av_channel_layout_default(&out_layout, kAudioChannels);
        int err = swr_alloc_set_opts2(&o.swr, &out_layout, AV_SAMPLE_FMT_FLTP, kAudioRate, &in_layout,
                                      AV_SAMPLE_FMT_S16, it.rate, 0, nullptr);
        av_channel_layout_uninit(&in_layout);
        av_channel_layout_uninit(&out_layout);
        if (err < 0 || swr_init(o.swr) < 0) {
            swr_free(&o.swr);
            return;
        }
        o.swr_rate = it.rate;
        o.swr_ch = it.channels;
    }
    int in_frames = (int) (it.pcm.size() / (size_t) it.channels);
    int out_cap = swr_get_out_samples(o.swr, in_frames);
    if (out_cap <= 0) {
        return;
    }
    uint8_t **conv = nullptr;
    if (av_samples_alloc_array_and_samples(&conv, nullptr, kAudioChannels, out_cap, AV_SAMPLE_FMT_FLTP, 0) < 0) {
        return;
    }
    const uint8_t *src = (const uint8_t *) it.pcm.data();
    int got = swr_convert(o.swr, conv, out_cap, &src, in_frames);

    if (got > 0) {
        // Place the samples by the sender's clock, which video uses too, so the two stay in sync over a long
        // clip. Small differences are jitter and are ignored; a real gap (audio paused) is filled with silence.
        int64_t fifo_end = o.a_next + av_audio_fifo_size(o.fifo);
        if (it.pts_us != INT64_MIN) {
            int64_t at = (it.pts_us - o.part_base_us) * kAudioRate / 1000000;
            if (!o.a_anchored) {
                o.a_next = (std::max)(at, (int64_t) 0);
                o.a_anchored = true;
            } else if (at - fifo_end > kAudioRate / 20) {
                int64_t gap = at - fifo_end;
                if (gap > (int64_t) kAudioRate * 10) {
                    // Long silence: close out what's buffered and restart the track at the new position.
                    encode_audio_fifo(true);
                    o.a_next = at;
                } else {
                    std::vector<float> zeros((size_t) gap, 0.0f);
                    void *planes[kAudioChannels];
                    for (auto &p : planes) {
                        p = zeros.data();
                    }
                    av_audio_fifo_write(o.fifo, planes, (int) gap);
                }
            }
        } else if (!o.a_anchored) {
            o.a_next = (std::max)((last_video_us_.load() - o.part_base_us) * kAudioRate / 1000000, (int64_t) 0);
            o.a_anchored = true;
        }
        av_audio_fifo_write(o.fifo, (void **) conv, got);
        encode_audio_fifo(false);
    }
    av_freep(&conv[0]);
    av_freep(&conv);
}

void Recorder::encode_audio_fifo(bool flush) {
    Out &o = *out_;
    int fs = o.aenc->frame_size > 0 ? o.aenc->frame_size : 1024;
    while (av_audio_fifo_size(o.fifo) >= fs || (flush && av_audio_fifo_size(o.fifo) > 0)) {
        int n = (std::min)(fs, av_audio_fifo_size(o.fifo));
        AVFrame *f = av_frame_alloc();
        f->format = AV_SAMPLE_FMT_FLTP;
        f->sample_rate = kAudioRate;
        av_channel_layout_default(&f->ch_layout, kAudioChannels);
        f->nb_samples = fs;
        if (av_frame_get_buffer(f, 0) < 0) {
            av_frame_free(&f);
            return;
        }
        av_audio_fifo_read(o.fifo, (void **) f->data, n);
        for (int ch = 0; ch < kAudioChannels && n < fs; ++ch) {
            memset(f->data[ch] + (size_t) n * sizeof(float), 0, (size_t) (fs - n) * sizeof(float));
        }
        f->pts = o.a_next;
        o.a_next += n;
        int err = avcodec_send_frame(o.aenc, f);
        av_frame_free(&f);
        if (err < 0 && err != AVERROR(EAGAIN)) {
            return;
        }
        write_packets(o.aenc, o.ast, false);
    }
}

bool Recorder::write_packets(void *codec_ctx, void *stream, bool flush) {
    Out &o = *out_;
    auto *enc = (AVCodecContext *) codec_ctx;
    auto *st = (AVStream *) stream;
    if (flush) {
        avcodec_send_frame(enc, nullptr);
    }
    for (;;) {
        int err = avcodec_receive_packet(enc, o.pkt);
        if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) {
            return true;
        }
        if (err < 0) {
            set_error("encoder error: " + av_error(err));
            return false;
        }
        av_packet_rescale_ts(o.pkt, enc->time_base, st->time_base);
        o.pkt->stream_index = st->index;
        err = av_interleaved_write_frame(o.fmt, o.pkt);
        if (err < 0) {
            set_error("could not write to the file (disk full?): " + av_error(err));
            return false;
        }
    }
}

bool Recorder::open_video_encoder(int w, int h) {
    Out &o = *out_;
    int64_t bitrate = (int64_t) w * h * 60 / 8; // ~0.125 bits per pixel at 60 fps
    bitrate = (std::max)((int64_t) 8000000, (std::min)(bitrate, (int64_t) 60000000));

    // Hardware first: they cost almost nothing on a modern GPU. libx264 is the always-there fallback.
    const char *names[] = {"h264_nvenc", "h264_qsv", "h264_amf", "libx264"};
    for (const char *name : names) {
        const AVCodec *codec = avcodec_find_encoder_by_name(name);
        if (!codec) {
            continue;
        }
        AVCodecContext *c = avcodec_alloc_context3(codec);
        c->width = w;
        c->height = h;
        c->pix_fmt = AV_PIX_FMT_NV12;
        c->time_base = AVRational{1, kVideoTimebase};
        c->framerate = AVRational{60, 1};
        c->gop_size = 60 * kGopSeconds;
        c->max_b_frames = 0;
        c->bit_rate = bitrate;
        c->rc_max_rate = bitrate * 3 / 2;
        c->rc_buffer_size = (int) (bitrate * 2);
        c->color_range = AVCOL_RANGE_MPEG;
        c->colorspace = AVCOL_SPC_BT709;
        c->color_primaries = AVCOL_PRI_BT709;
        c->color_trc = AVCOL_TRC_BT709;
        if (o.fmt->oformat->flags & AVFMT_GLOBALHEADER) {
            c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }
        AVDictionary *opts = nullptr;
        if (strcmp(name, "h264_nvenc") == 0) {
            av_dict_set(&opts, "preset", "p5", 0);
            av_dict_set(&opts, "tune", "hq", 0);
            av_dict_set(&opts, "rc", "vbr", 0);
            av_dict_set(&opts, "cq", "19", 0);
            av_dict_set(&opts, "profile", "high", 0);
            av_dict_set(&opts, "spatial-aq", "1", 0);
        } else if (strcmp(name, "h264_qsv") == 0) {
            av_dict_set(&opts, "preset", "medium", 0);
            av_dict_set(&opts, "profile", "high", 0);
        } else if (strcmp(name, "h264_amf") == 0) {
            av_dict_set(&opts, "quality", "quality", 0);
            av_dict_set(&opts, "rc", "vbr_peak", 0);
            av_dict_set(&opts, "profile", "high", 0);
        } else {
            av_dict_set(&opts, "preset", "veryfast", 0);
            av_dict_set(&opts, "crf", "20", 0);
            av_dict_set(&opts, "profile", "high", 0);
        }
        int err = avcodec_open2(c, codec, &opts);
        av_dict_free(&opts);
        if (err < 0) {
            rlog("%s unavailable: %s", name, av_error(err).c_str());
            avcodec_free_context(&c);
            continue;
        }
        o.venc = c;
        {
            std::lock_guard<std::mutex> lock(info_mu_);
            encoder_ = name;
        }
        rlog("video %dx%d with %s at %lld kbps", w, h, name, (long long) (bitrate / 1000));
        return true;
    }
    set_error("no H.264 encoder could be opened");
    return false;
}

bool Recorder::open_audio_encoder() {
    Out &o = *out_;
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec) {
        return false;
    }
    AVCodecContext *c = avcodec_alloc_context3(codec);
    c->sample_fmt = AV_SAMPLE_FMT_FLTP;
    c->sample_rate = kAudioRate;
    av_channel_layout_default(&c->ch_layout, kAudioChannels);
    c->bit_rate = 192000;
    c->time_base = AVRational{1, kAudioRate};
    if (o.fmt->oformat->flags & AVFMT_GLOBALHEADER) {
        c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    if (avcodec_open2(c, codec, nullptr) < 0) {
        avcodec_free_context(&c);
        return false;
    }
    o.aenc = c;
    o.fifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP, kAudioChannels, kAudioRate);
    return o.fifo != nullptr;
}

bool Recorder::open_output(int w, int h) {
    out_ = new Out();
    Out &o = *out_;
    o.w = w;
    o.h = h;
    std::string file = utf8(path());
    if (avformat_alloc_output_context2(&o.fmt, nullptr, "mp4", file.c_str()) < 0 || !o.fmt) {
        set_error("could not create the MP4 muxer");
        return false;
    }
    if (!open_video_encoder(w, h)) {
        return false;
    }
    o.vst = avformat_new_stream(o.fmt, nullptr);
    avcodec_parameters_from_context(o.vst->codecpar, o.venc);
    o.vst->time_base = o.venc->time_base;
    if (open_audio_encoder()) {
        o.ast = avformat_new_stream(o.fmt, nullptr);
        avcodec_parameters_from_context(o.ast->codecpar, o.aenc);
        o.ast->time_base = o.aenc->time_base;
    } else {
        rlog("AAC encoder unavailable; recording video only");
    }
    int err = avio_open(&o.fmt->pb, file.c_str(), AVIO_FLAG_WRITE);
    if (err < 0) {
        set_error("could not create " + file + ": " + av_error(err));
        return false;
    }
    AVDictionary *opts = nullptr;
    // Fragmented: every keyframe closes a fragment, so a crash or power cut loses at most two seconds.
    av_dict_set(&opts, "movflags", "+frag_keyframe+empty_moov+default_base_moof", 0);
    err = avformat_write_header(o.fmt, &opts);
    av_dict_free(&opts);
    if (err < 0) {
        set_error("could not start the MP4 file: " + av_error(err));
        return false;
    }
    o.header_written = true;
    o.pkt = av_packet_alloc();
    rlog("writing %s", file.c_str());
    return true;
}

void Recorder::close_output() {
    if (!out_) {
        return;
    }
    Out &o = *out_;
    if (o.header_written && o.pkt) {
        if (o.aenc && o.fifo) {
            encode_audio_fifo(true);
            write_packets(o.aenc, o.ast, true);
        }
        if (o.venc) {
            write_packets(o.venc, o.vst, true);
        }
        av_write_trailer(o.fmt);
    }
    if (o.fmt && o.fmt->pb) {
        avio_closep(&o.fmt->pb);
    }
    avformat_free_context(o.fmt);
    avcodec_free_context(&o.venc);
    avcodec_free_context(&o.aenc);
    av_packet_free(&o.pkt);
    swr_free(&o.swr);
    if (o.fifo) {
        av_audio_fifo_free(o.fifo);
    }
    delete out_;
    out_ = nullptr;
}

} // namespace airscreen
