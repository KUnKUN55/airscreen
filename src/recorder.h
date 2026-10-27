#pragma once

#include "mic.h"

#include <atomic>
#include <climits>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace airscreen {

// Records the mirrored stream to MP4 (H.264 + AAC).
//
// The iPhone's own bitstream can't simply be copied: iOS sends a keyframe only when a session starts or the
// screen rotates, so a clip started mid-session would have nothing to decode from. Instead decoded frames are
// re-encoded, on the GPU when possible (NVENC, Quick Sync, AMF), with a keyframe every two seconds.
//
// The decode thread only queues a GPU copy into a staging texture; the readback, encoding and disk writes
// all happen on the recorder's own thread, so recording never holds up what's on screen. While recording,
// the file is a fragmented "<name>.mp4.part", playable up to the last fragment even if AirScreen is killed
// mid-clip; when the clip ends it is remuxed into a plain "<name>.mp4" that Windows' players open.
class Recorder {
public:
    Recorder();
    ~Recorder();

    // `path` is the first file; a rotation (new frame size) continues in "<name>_2.mp4" and so on.
    bool start(const std::wstring &path);
    void stop();
    bool active() const { return active_.load(); }
    double seconds() const;
    std::wstring path() const;
    std::string last_error() const;
    std::string encoder_name() const;

    // Decode thread, with the video pipeline's D3D11 lock held. `frame` is an AVFrame.
    void on_video_frame(ID3D11Device *device, ID3D11DeviceContext *ctx, const void *frame, int w, int h);
    // Audio decode thread.
    void on_audio_pcm(const int16_t *pcm, int frames, int channels, int rate, uint64_t ntp_ns);

    // Microphone mixed into the clip's audio, e.g. to narrate a tutorial. Changing the device or turning it
    // on/off mid-clip restarts capture; gain (1.0 = as captured) applies immediately. Returns false if the
    // mic couldn't be opened, with the reason in mic_error(); the clip keeps recording without it.
    bool set_mic(bool enabled, const std::wstring &device_id, float gain);
    bool mic_running() const { return mic_.running(); }
    std::string mic_error() const;

private:
    struct Item {
        bool video = false;
        int64_t pts_us = 0;
        int w = 0;
        int h = 0;
        ID3D11Texture2D *staging = nullptr; // GPU frames: owned reference
        void *sw_frame = nullptr;           // CPU-decoded frames: AVFrame clone
        std::vector<int16_t> pcm; // iPhone audio
        int channels = 0;
        int rate = 0;
        bool mic = false;
        std::vector<float> fpcm; // mic: 48 kHz stereo float, gain applied
    };
    struct Out;

    void writer_loop();
    bool handle_video(Item &it);
    void handle_audio(Item &it);
    void mix_in(int src, const float *pcm, int frames, int64_t at, bool timed);
    void pump_mix(bool flush, int64_t limit = INT64_MAX);
    int64_t watermark() const;
    void on_mic_pcm(const float *pcm, int frames, int64_t local_time_us);
    bool restart_mic_locked();
    bool open_output(int w, int h);
    void close_output();
    bool open_video_encoder(int w, int h);
    bool open_audio_encoder();
    bool write_packets(void *codec_ctx, void *stream, bool flush);
    void release_item(Item &it);
    ID3D11Texture2D *take_staging(ID3D11Device *device, int w, int h);
    void return_staging(ID3D11Texture2D *tex, int w, int h);
    void set_error(const std::string &msg);
    std::wstring part_path(int part) const;

    std::mutex life_mu_; // start/stop come from the UI and from session teardown
    std::atomic<bool> active_{false};
    std::atomic<bool> run_{false};
    std::thread thread_;

    mutable std::mutex q_mu_;
    std::condition_variable q_cv_;
    std::deque<Item> q_;
    size_t queued_video_ = 0;
    uint64_t dropped_ = 0;

    // Shared between the decode thread (producer) and the writer: the clip's zero point on the sender clock.
    std::atomic<int64_t> base_us_{INT64_MIN};
    std::atomic<int64_t> last_video_us_{0};
    std::atomic<int64_t> offset_us_{INT64_MIN}; // local clock minus sender clock, from the video frames
    int64_t win_start_ = 0;                     // windowed minimum behind offset_us_; decode thread only
    int64_t cur_min_ = 0;
    int64_t prev_min_ = 0;

    MicCapture mic_;
    bool mic_enabled_ = false; // guarded by life_mu_
    std::wstring mic_device_;  // guarded by life_mu_
    std::string mic_error_;    // guarded by info_mu_
    std::atomic<float> mic_gain_{1.0f};

    std::mutex pool_mu_;
    std::vector<ID3D11Texture2D *> pool_;
    int pool_w_ = 0;
    int pool_h_ = 0;
    int pool_out_ = 0; // textures of the current size handed out and not yet returned
    ID3D11DeviceContext *ctx_ = nullptr; // borrowed from the video pipeline, used for Map on the writer thread

    mutable std::mutex info_mu_;
    std::wstring path_;
    std::wstring cur_path_;
    std::string error_;
    std::string encoder_;
    int part_ = 1;

    Out *out_ = nullptr; // writer thread only
    std::thread remux_;  // finalizes the previous file; started and joined by the writer thread, then ~Recorder
};

} // namespace airscreen
