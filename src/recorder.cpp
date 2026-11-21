#include "recorder.h"
#include "settings.h"

#include <chrono>

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
// How long the mix waits for late audio before encoding a stretch. iPhone audio and the mic both arrive well
// within this; it only delays the audio track's encoding, not what's on screen.
constexpr int64_t kMixLatencyUs = 300000;
// A source quiet for this long (the iPhone stops sending audio when nothing plays) no longer holds the mix.
constexpr int64_t kSourceIdleUs = 500000;
// The clock offset is the smallest delay seen over the last one to two windows, so it follows the network
// getting slower within a few seconds instead of clinging to the best case forever.
constexpr int64_t kOffsetWindowUs = 2000000;
constexpr int64_t kRepeatUs = 500000;

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

// Stream-copies the fragmented recording into a plain MP4 (one moov) that every player, Windows' included,
// opens. Nothing is re-encoded. If it fails the fragmented file is kept under the final name instead, which
// VLC and browsers still play.
void finalize_clip(std::wstring part, std::wstring final_path) {
    std::wstring tmp = final_path + L".tmp";
    std::string in_name = utf8(part);
    std::string out_name = utf8(tmp);
    AVFormatContext *in = nullptr;
    AVFormatContext *out = nullptr;
    AVPacket *pkt = av_packet_alloc();
    bool ok = false;
    int err = avformat_open_input(&in, in_name.c_str(), nullptr, nullptr);
    if (err >= 0 && (err = avformat_find_stream_info(in, nullptr)) >= 0 &&
        (err = avformat_alloc_output_context2(&out, nullptr, "mp4", out_name.c_str())) >= 0) {
        for (unsigned i = 0; i < in->nb_streams && err >= 0; ++i) {
            AVStream *st = avformat_new_stream(out, nullptr);
            err = st ? avcodec_parameters_copy(st->codecpar, in->streams[i]->codecpar) : AVERROR(ENOMEM);
            if (st) {
                st->codecpar->codec_tag = 0;
                st->time_base = in->streams[i]->time_base;
            }
        }
        if (err >= 0 && (err = avio_open(&out->pb, out_name.c_str(), AVIO_FLAG_WRITE)) >= 0 &&
            (err = avformat_write_header(out, nullptr)) >= 0) {
            while ((err = av_read_frame(in, pkt)) >= 0) {
                AVStream *ist = in->streams[pkt->stream_index];
                AVStream *ost = out->streams[pkt->stream_index];
                av_packet_rescale_ts(pkt, ist->time_base, ost->time_base);
                pkt->pos = -1;
                err = av_interleaved_write_frame(out, pkt);
                av_packet_unref(pkt);
                if (err < 0) {
                    break;
                }
            }
            if (err == AVERROR_EOF) {
                err = av_write_trailer(out);
                ok = err >= 0;
            }
        }
    }
    if (out && out->pb) {
        avio_closep(&out->pb);
    }
    avformat_free_context(out);
    avformat_close_input(&in);
    av_packet_free(&pkt);

    if (ok && MoveFileExW(tmp.c_str(), final_path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(part.c_str());
        rlog("saved %s", utf8(final_path).c_str());
    } else {
        DeleteFileW(tmp.c_str());
        MoveFileExW(part.c_str(), final_path.c_str(), MOVEFILE_REPLACE_EXISTING);
        rlog("could not finalize %s (%s); kept the fragmented file", utf8(final_path).c_str(),
             ok ? "rename failed" : av_error(err).c_str());
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
    // Audio mix: interleaved stereo float, mix[0] being sample mix_pos (48 kHz, from the part's start).
    std::vector<float> mix;
    int64_t mix_pos = 0;
    int64_t src_next[2] = {INT64_MIN, INT64_MIN}; // where each source (0 iPhone, 1 mic) continues
    int64_t src_last_local[2] = {0, 0};           // local time of each source's latest chunk
    int w = 0;
    int h = 0;
    int64_t part_base_us = 0;
    int64_t last_vpts = -1;
    AVFrame *last_frame = nullptr; // latest picture, for repeating while the screen is still
    int64_t last_video_local = 0;  // local time a picture was last encoded
    bool header_written = false;
    std::wstring final_path;
    std::wstring part_path; // fragmented MP4 being written; becomes final_path when the clip ends
};

Recorder::Recorder() = default;

Recorder::~Recorder() {
    stop();
    if (remux_.joinable()) {
        remux_.join(); // let the last clip finish turning into a plain MP4 before the app exits
    }
    for (auto &p : pool_) {
        p.first->Release();
        p.second->Release();
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
    last_video_us_ = 0; // offset_us_ carries over: note_clock() keeps it current whenever video flows
    base_local_us_ = 0;
    end_local_us_ = 0;
    {
        std::lock_guard<std::mutex> lock(pause_mu_);
        pause_start_ = 0;
        paused_total_ = 0;
    }
    dropped_ = 0;
    run_ = true;
    active_ = true;
    thread_ = std::thread([this] { writer_loop(); });
    rlog("armed: %s", utf8(path).c_str());
    // A mic that won't open doesn't stop the clip; the UI reports mic_error().
    restart_mic_locked();
    return true;
}

void Recorder::stop() {
    std::lock_guard<std::mutex> life(life_mu_);
    mic_.stop();
    if (!thread_.joinable()) {
        active_ = false;
        return;
    }
    if (base_local_us_.load() && !end_local_us_.load()) {
        end_local_us_ = clip_us();
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

// Wall-clock length of the clip so far. iPhone frames only arrive when the screen changes, so counting by
// frames would freeze the timer while you talk over a still screen.
double Recorder::seconds() const {
    int64_t start = base_local_us_.load();
    if (start == 0) {
        return 0;
    }
    int64_t end = end_local_us_.load();
    return (double) ((end ? end : clip_us()) - start) / 1e6;
}

void Recorder::set_paused(bool paused) {
    std::lock_guard<std::mutex> lock(pause_mu_);
    if (paused == (pause_start_ != 0) || (paused && !active_.load())) {
        return;
    }
    if (paused) {
        pause_start_ = local_us();
        rlog("paused");
    } else {
        paused_total_ += local_us() - pause_start_;
        pause_start_ = 0;
        rlog("resumed");
    }
}

bool Recorder::paused() const {
    std::lock_guard<std::mutex> lock(pause_mu_);
    return pause_start_ != 0;
}

int64_t Recorder::clip_us() const {
    std::lock_guard<std::mutex> lock(pause_mu_);
    return (pause_start_ ? pause_start_ : local_us()) - paused_total_;
}

bool Recorder::to_clip(int64_t local, int64_t *clip) const {
    std::lock_guard<std::mutex> lock(pause_mu_);
    if (pause_start_) {
        return false;
    }
    *clip = local - paused_total_;
    return true;
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

// A pair of CPU-readable staging textures for one canvas frame: Y at full size, CbCr at half.
bool Recorder::take_staging(ID3D11Device *device, int w, int h, Item &it) {
    std::lock_guard<std::mutex> lock(pool_mu_);
    if (w != pool_w_ || h != pool_h_) {
        for (auto &p : pool_) {
            p.first->Release();
            p.second->Release();
        }
        pool_.clear();
        pool_w_ = w;
        pool_h_ = h;
        pool_out_ = 0;
    }
    if (!pool_.empty()) {
        it.staging = pool_.back().first;
        it.staging_uv = pool_.back().second;
        pool_.pop_back();
        ++pool_out_;
        return true;
    }
    if (pool_out_ >= kMaxStaging) {
        return false;
    }
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = (UINT) w;
    td.Height = (UINT) h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *y = nullptr, *uv = nullptr;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &y))) {
        return false;
    }
    td.Width = (UINT) (w / 2);
    td.Height = (UINT) (h / 2);
    td.Format = DXGI_FORMAT_R8G8_UNORM;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &uv))) {
        y->Release();
        return false;
    }
    it.staging = y;
    it.staging_uv = uv;
    ++pool_out_;
    return true;
}

void Recorder::return_staging(Item &it) {
    if (!it.staging) {
        return;
    }
    std::lock_guard<std::mutex> lock(pool_mu_);
    if (it.w == pool_w_ && it.h == pool_h_) {
        --pool_out_;
        pool_.push_back({it.staging, it.staging_uv});
    } else {
        it.staging->Release();
        it.staging_uv->Release();
    }
    it.staging = nullptr;
    it.staging_uv = nullptr;
}

void Recorder::release_item(Item &it) {
    return_staging(it);
}

void Recorder::on_canvas(ID3D11Device *device, ID3D11DeviceContext *ctx, ID3D11Texture2D *y, ID3D11Texture2D *uv,
                         int w, int h, int64_t local_time_us) {
    if (!active_.load() || !y || !uv || w <= 0 || h <= 0 || !to_clip(local_time_us, &local_time_us) ||
        !video_slot_free()) {
        return;
    }
    Item it;
    it.video = true;
    it.pts_us = local_time_us;
    it.w = w;
    it.h = h;
    if (!take_staging(device, w, h, it)) {
        return;
    }
    // Queued on the GPU now, read back on the writer thread once it has landed.
    ctx->CopyResource(it.staging, y);
    ctx->CopyResource(it.staging_uv, uv);
    ctx_ = ctx;
    int64_t expect = INT64_MIN;
    if (base_us_.compare_exchange_strong(expect, local_time_us)) {
        base_local_us_ = local_time_us;
    }
    last_video_us_ = local_time_us;
    {
        std::lock_guard<std::mutex> lock(q_mu_);
        ++queued_video_;
        q_.push_back(std::move(it));
    }
    q_cv_.notify_one();
}

void Recorder::note_clock(int64_t sender_us, int64_t local_time_us) {
    // Local clock minus sender clock from the least-delayed frame of the last few seconds: where the iPhone's
    // audio timestamps land on the local timeline the clip is built on.
    int64_t d = local_time_us - sender_us;
    if (win_start_ == 0 || local_time_us - win_start_ >= kOffsetWindowUs) {
        prev_min_ = win_start_ == 0 ? d : cur_min_;
        cur_min_ = d;
        win_start_ = local_time_us;
    } else {
        cur_min_ = (std::min)(cur_min_, d);
    }
    offset_us_ = (std::min)(cur_min_, prev_min_);
}

bool Recorder::video_slot_free() {
    std::lock_guard<std::mutex> lock(q_mu_);
    if (!run_) {
        return false;
    }
    if (queued_video_ >= kMaxQueuedVideo) {
        if ((++dropped_ % 60) == 1) {
            rlog("encoder behind, skipped %llu frames so far", (unsigned long long) dropped_);
        }
        return false;
    }
    return true;
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
    int64_t off = offset_us_.load();
    it.pts_us = ntp_ns && off != INT64_MIN ? (int64_t) (ntp_ns / 1000) + off : INT64_MIN; // onto the local clock
    if (it.pts_us == INT64_MIN ? paused() : !to_clip(it.pts_us, &it.pts_us)) {
        return;
    }
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

void Recorder::on_mic_pcm(const float *pcm, int frames, int64_t local_time_us) {
    if (!active_.load() || frames <= 0) {
        return;
    }
    float gain = mic_gain_.load();
    Item it;
    it.mic = true;
    if (!to_clip(local_time_us, &it.pts_us)) {
        return; // paused
    }
    it.fpcm.resize((size_t) frames * MicCapture::kChannels);
    for (size_t i = 0; i < it.fpcm.size(); ++i) {
        it.fpcm[i] = pcm[i] * gain;
    }
    {
        std::lock_guard<std::mutex> lock(q_mu_);
        if (!run_ || q_.size() > 512) {
            return;
        }
        q_.push_back(std::move(it));
    }
    q_cv_.notify_one();
}

bool Recorder::set_mic(bool enabled, const std::wstring &device_id, float gain) {
    mic_gain_ = gain; // takes effect on the next chunk, no restart needed
    std::lock_guard<std::mutex> life(life_mu_);
    bool changed = enabled != mic_enabled_ || device_id != mic_device_;
    mic_enabled_ = enabled;
    mic_device_ = device_id;
    if (!changed || !thread_.joinable()) {
        return true; // otherwise applied when the next clip starts
    }
    return restart_mic_locked();
}

bool Recorder::restart_mic_locked() {
    mic_.stop();
    std::string err;
    if (mic_enabled_ &&
        !mic_.start(mic_device_, [this](const float *pcm, int frames, int64_t at) { on_mic_pcm(pcm, frames, at); })) {
        err = mic_.error();
    }
    std::lock_guard<std::mutex> lock(info_mu_);
    mic_error_ = err;
    return err.empty();
}

std::string Recorder::mic_error() const {
    std::lock_guard<std::mutex> lock(info_mu_);
    return mic_error_;
}

void Recorder::writer_loop() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    bool failed = false;
    for (;;) {
        Item it;
        bool have = false;
        {
            std::unique_lock<std::mutex> lock(q_mu_);
            // Wake at least every 20 ms so the audio mix keeps flowing even when nothing arrives.
            q_cv_.wait_for(lock, std::chrono::milliseconds(20), [this] { return !q_.empty() || !run_; });
            if (q_.empty() && !run_) {
                break; // stopping, and everything queued has been written
            }
            if (!q_.empty()) {
                it = std::move(q_.front());
                q_.pop_front();
                have = true;
                if (it.video) {
                    --queued_video_;
                }
            }
        }
        if (!failed) {
            if (have && it.video) {
                failed = !handle_video(it);
            } else if (have && out_) {
                handle_audio(it);
            }
            if (out_ && !failed) {
                failed = !repeat_if_still();
            }
            if (out_ && !failed) {
                pump_mix(false);
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
        std::vector<float> carry;
        int64_t carry_at = 0;
        int64_t carry_next[2] = {INT64_MIN, INT64_MIN};
        if (out_) {
            // The phone rotated: finish this file and carry on in the next one. Audio already mixed past the
            // cut belongs to the new file, so move it across rather than leave it trailing in the old one.
            int64_t cut = (it.pts_us - out_->part_base_us) * kAudioRate / 1000000;
            pump_mix(true, cut);
            carry_at = (std::max)(out_->mix_pos - cut, (int64_t) 0);
            carry.swap(out_->mix);
            for (int s = 0; s < 2; ++s) {
                carry_next[s] = out_->src_next[s] == INT64_MIN ? INT64_MIN : out_->src_next[s] - cut;
            }
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
        if (!carry.empty()) {
            out_->mix.assign((size_t) carry_at * kAudioChannels, 0.0f);
            out_->mix.insert(out_->mix.end(), carry.begin(), carry.end());
            out_->src_next[0] = carry_next[0];
            out_->src_next[1] = carry_next[1];
        }
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
    if (it.staging && it.staging_uv && ctx_) {
        // Poll instead of blocking inside Map, which would hold the device lock the render thread needs.
        auto map_read = [&](ID3D11Texture2D *t, D3D11_MAPPED_SUBRESOURCE &m) {
            HRESULT hr;
            while ((hr = ctx_->Map(t, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m)) ==
                   DXGI_ERROR_WAS_STILL_DRAWING) {
                Sleep(1);
            }
            return SUCCEEDED(hr);
        };
        D3D11_MAPPED_SUBRESOURCE my = {}, muv = {};
        if (map_read(it.staging, my)) {
            if (map_read(it.staging_uv, muv)) {
                copy_nv12(f, (const uint8_t *) my.pData, (int) my.RowPitch, (const uint8_t *) muv.pData,
                          (int) muv.RowPitch, it.w, it.h);
                ctx_->Unmap(it.staging_uv, 0);
                ok = true;
            }
            ctx_->Unmap(it.staging, 0);
        }
    }
    if (!ok) {
        av_frame_free(&f);
        return true;
    }

    av_frame_free(&o.last_frame);
    o.last_frame = av_frame_clone(f); // shares the buffers; kept to repeat while the screen is still
    av_frame_free(&f);
    return encode_last_frame(it.pts_us);
}

// iOS sends nothing while the screen doesn't change. Repeat the last picture twice a second meanwhile, so the
// file keeps growing (and stays crash-safe) under a narration, and editors don't see a frozen gap.
bool Recorder::repeat_if_still() {
    Out &o = *out_;
    int64_t now = clip_us(); // stands still while paused, so nothing repeats into the pause
    if (!o.last_frame || now - o.last_video_local < kRepeatUs) {
        return true;
    }
    return encode_last_frame(now);
}

// Encodes the most recent picture at local time `pts_us`.
bool Recorder::encode_last_frame(int64_t pts_us) {
    Out &o = *out_;
    if (!o.last_frame) {
        return true;
    }
    int64_t pts = (pts_us - o.part_base_us) * kVideoTimebase / 1000000;
    if (pts <= o.last_vpts) {
        pts = o.last_vpts + 1;
    }
    o.last_vpts = pts;
    o.last_video_local = clip_us();
    AVFrame *f = av_frame_clone(o.last_frame);
    if (!f) {
        return true;
    }
    f->pts = pts;
    int err = avcodec_send_frame(o.venc, f);
    av_frame_free(&f);
    if (err < 0 && err != AVERROR(EAGAIN)) {
        set_error("video encode failed: " + av_error(err));
        return false;
    }
    return write_packets(o.venc, o.vst, false);
}

// Adds stereo float samples into the mix at sample position `at` (relative to the part's start). Each source
// is kept continuous: positions within 50 ms of where its previous chunk ended are treated as jitter, so the
// audio never gets tiny gaps or overlaps (clicks); a bigger jump (a pause in the sound) is honoured.
void Recorder::mix_in(int src, const float *pcm, int frames, int64_t at, bool timed) {
    Out &o = *out_;
    int64_t &next = o.src_next[src];
    if (!timed) {
        at = next != INT64_MIN ? next : watermark();
    } else if (next != INT64_MIN && std::llabs(at - next) < kAudioRate / 20) {
        at = next;
    }
    next = at + frames;
    o.src_last_local[src] = clip_us();
    if (at < o.mix_pos) {
        int64_t skip = o.mix_pos - at;
        if (skip >= frames) {
            return; // arrived after that stretch was already encoded
        }
        pcm += skip * kAudioChannels;
        frames -= (int) skip;
        at = o.mix_pos;
    }
    if (at - o.mix_pos > (int64_t) kAudioRate * 10) {
        return; // nonsense timestamp; don't grow the buffer by minutes
    }
    size_t start = (size_t) (at - o.mix_pos) * kAudioChannels;
    size_t need = start + (size_t) frames * kAudioChannels;
    if (o.mix.size() < need) {
        o.mix.resize(need, 0.0f);
    }
    float *dst = o.mix.data() + start;
    for (size_t i = 0, n = (size_t) frames * kAudioChannels; i < n; ++i) {
        dst[i] += pcm[i];
    }
}

// Sample position up to which every source has had its chance to arrive: now less a grace period, and never
// past where a source that is still delivering has got to. Without that second bound, iPhone audio placed by
// a clock estimate that runs behind (the network got slower and stayed slow) would land on stretches already
// encoded as silence.
int64_t Recorder::watermark() const {
    if (!out_) {
        return 0;
    }
    const Out &o = *out_;
    int64_t now = clip_us();
    int64_t wm = (now - o.part_base_us - kMixLatencyUs) * kAudioRate / 1000000;
    for (int s = 0; s < 2; ++s) {
        if (o.src_last_local[s] && now - o.src_last_local[s] < kSourceIdleUs && o.src_next[s] != INT64_MIN) {
            wm = (std::min)(wm, o.src_next[s]);
        }
    }
    return wm;
}

void Recorder::handle_audio(Item &it) {
    Out &o = *out_;
    if (!o.aenc) {
        return;
    }
    if (it.mic) {
        int frames = (int) (it.fpcm.size() / kAudioChannels);
        int64_t at = (it.pts_us - o.part_base_us) * kAudioRate / 1000000;
        mix_in(1, it.fpcm.data(), frames, at, true);
        return;
    }
    if (!o.swr || o.swr_rate != it.rate || o.swr_ch != it.channels) {
        swr_free(&o.swr);
        AVChannelLayout in_layout, out_layout;
        av_channel_layout_default(&in_layout, it.channels);
        av_channel_layout_default(&out_layout, kAudioChannels);
        int err = swr_alloc_set_opts2(&o.swr, &out_layout, AV_SAMPLE_FMT_FLT, kAudioRate, &in_layout,
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
    std::vector<float> conv((size_t) out_cap * kAudioChannels);
    uint8_t *dst = (uint8_t *) conv.data();
    const uint8_t *src = (const uint8_t *) it.pcm.data();
    int got = swr_convert(o.swr, &dst, out_cap, &src, in_frames);
    if (got > 0) {
        bool timed = it.pts_us != INT64_MIN;
        int64_t at = timed ? (it.pts_us - o.part_base_us) * kAudioRate / 1000000 : 0;
        mix_in(0, conv.data(), got, at, timed);
    }
}

// Encodes the mix up to the watermark (or, when the file is closing, everything buffered). Stretches no
// source covered become silence, so the track is continuous and the mic keeps recording while the iPhone
// is quiet.
void Recorder::pump_mix(bool flush, int64_t limit) {
    Out &o = *out_;
    if (!o.aenc) {
        return;
    }
    const int fs = o.aenc->frame_size > 0 ? o.aenc->frame_size : 1024;
    int64_t end = flush ? o.mix_pos + (int64_t) (o.mix.size() / kAudioChannels) : watermark();
    end = (std::min)(end, limit);
    if (flush) {
        end = o.mix_pos + (end - o.mix_pos + fs - 1) / fs * fs;
    }
    while (o.mix_pos + fs <= end) {
        AVFrame *f = av_frame_alloc();
        f->format = AV_SAMPLE_FMT_FLTP;
        f->sample_rate = kAudioRate;
        av_channel_layout_default(&f->ch_layout, kAudioChannels);
        f->nb_samples = fs;
        if (av_frame_get_buffer(f, 0) < 0) {
            av_frame_free(&f);
            return;
        }
        size_t have = (std::min)(o.mix.size() / kAudioChannels, (size_t) fs);
        auto *l = (float *) f->data[0];
        auto *r = (float *) f->data[1];
        for (size_t i = 0; i < (size_t) fs; ++i) {
            float a = i < have ? o.mix[i * 2] : 0.0f;
            float b = i < have ? o.mix[i * 2 + 1] : 0.0f;
            l[i] = a > 1.0f ? 1.0f : (a < -1.0f ? -1.0f : a);
            r[i] = b > 1.0f ? 1.0f : (b < -1.0f ? -1.0f : b);
        }
        o.mix.erase(o.mix.begin(), o.mix.begin() + (ptrdiff_t) (have * kAudioChannels));
        f->pts = o.mix_pos;
        o.mix_pos += fs;
        int err = avcodec_send_frame(o.aenc, f);
        av_frame_free(&f);
        if (err < 0 && err != AVERROR(EAGAIN)) {
            return;
        }
        if (!write_packets(o.aenc, o.ast, false)) {
            return;
        }
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
            // Above 1440p the slower preset and adaptive quantisation could not keep up with 60 fps on mid-range
            // cards (the log showed "encoder behind" and skipped frames on 4K canvases); p4 without AQ holds 4K60
            // with little visible difference at this bitrate.
            const bool big = (int64_t) w * h > 2560 * 1440;
            av_dict_set(&opts, "preset", big ? "p4" : "p5", 0);
            av_dict_set(&opts, "tune", "hq", 0);
            av_dict_set(&opts, "rc", "vbr", 0);
            av_dict_set(&opts, "cq", "19", 0);
            av_dict_set(&opts, "profile", "high", 0);
            if (!big) {
                av_dict_set(&opts, "spatial-aq", "1", 0);
            }
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
    return true;
}

bool Recorder::open_output(int w, int h) {
    out_ = new Out();
    Out &o = *out_;
    o.w = w;
    o.h = h;
    o.final_path = path();
    o.part_path = o.final_path + L".part";
    std::string file = utf8(o.part_path);
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
    // Fragmented while recording: every keyframe closes a fragment, so a crash or power cut loses at most
    // two seconds. Windows' own players can't open fragmented MP4, so close_output() remuxes it afterwards.
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
        if (o.aenc) {
            pump_mix(true);
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
    if (o.header_written) {
        // Off the writer thread, so stopping (and the UI waiting on it) doesn't sit through a multi-GB copy.
        if (remux_.joinable()) {
            remux_.join();
        }
        remux_ = std::thread(finalize_clip, o.part_path, o.final_path);
    }
    avformat_free_context(o.fmt);
    avcodec_free_context(&o.venc);
    avcodec_free_context(&o.aenc);
    av_packet_free(&o.pkt);
    swr_free(&o.swr);
    av_frame_free(&o.last_frame);
    delete out_;
    out_ = nullptr;
}

} // namespace airscreen
