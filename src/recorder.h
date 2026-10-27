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
#include <utility>
#include <vector>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace airscreen {

// Records the mirrored stream to MP4 (H.264 + AAC).
//
// What is recorded is the studio canvas, exactly as composed (scene, framing, fades), encoded on the GPU when
// possible (NVENC, Quick Sync, AMF) with a keyframe every two seconds. Everything runs on one local clock:
// canvas frames and the mic are stamped locally, and the iPhone audio is moved onto it with note_clock().
//
// The render thread only queues a GPU copy into staging textures; the readback, encoding and disk writes
// all happen on the recorder's own thread, so recording never holds up what's on screen. While recording,
// the file is a fragmented "<name>.mp4.part", playable up to the last fragment even if AirScreen is killed
// mid-clip; when the clip ends it is remuxed into a plain "<name>.mp4" that Windows' players open.
class Recorder {
public:
    Recorder();
    ~Recorder();

    // `path` is the first file; a change of canvas size continues in "<name>_2.mp4" and so on.
    bool start(const std::wstring &path);
    void stop();
    bool active() const { return active_.load(); }
    double seconds() const;
    std::wstring path() const;
    std::string last_error() const;
    std::string encoder_name() const;

    // Render thread, D3D11 lock held: the canvas as Y (R8) and CbCr (R8G8, half size) planes at local time.
    void on_canvas(ID3D11Device *device, ID3D11DeviceContext *ctx, ID3D11Texture2D *y, ID3D11Texture2D *uv, int w,
                   int h, int64_t local_time_us);
    // Decode thread: a picture stamped `sender_us` arrived at `local_time_us`; keeps the clock mapping current.
    void note_clock(int64_t sender_us, int64_t local_time_us);
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
        ID3D11Texture2D *staging = nullptr;    // Y plane copy, from the pool
        ID3D11Texture2D *staging_uv = nullptr; // CbCr plane copy, from the pool
        std::vector<int16_t> pcm; // iPhone audio
        int channels = 0;
        int rate = 0;
        bool mic = false;
        std::vector<float> fpcm; // mic: 48 kHz stereo float, gain applied
    };
    struct Out;

    void writer_loop();
    bool handle_video(Item &it);
    bool encode_last_frame(int64_t pts_us);
    bool repeat_if_still();
    bool video_slot_free();
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
    bool take_staging(ID3D11Device *device, int w, int h, Item &it);
    void return_staging(Item &it);
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

    // Shared between the render thread (producer) and the writer: the clip's zero point, local clock.
    std::atomic<int64_t> base_us_{INT64_MIN};
    std::atomic<int64_t> last_video_us_{0};
    std::atomic<int64_t> base_local_us_{0}; // local time of the clip's first picture; 0 = none yet
    std::atomic<int64_t> end_local_us_{0};  // local time the clip was stopped
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
    std::vector<std::pair<ID3D11Texture2D *, ID3D11Texture2D *>> pool_; // (Y, CbCr) staging pairs
    int pool_w_ = 0;
    int pool_h_ = 0;
    int pool_out_ = 0; // pairs of the current size handed out and not yet returned
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
