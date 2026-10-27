#pragma once

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
// all happen on the recorder's own thread, so recording never holds up what's on screen. The file is a
// fragmented MP4, which stays playable up to the last fragment even if AirScreen is killed mid-clip.
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

private:
    struct Item {
        bool video = false;
        int64_t pts_us = 0;
        int w = 0;
        int h = 0;
        ID3D11Texture2D *staging = nullptr; // GPU frames: owned reference
        void *sw_frame = nullptr;           // CPU-decoded frames: AVFrame clone
        std::vector<int16_t> pcm;
        int channels = 0;
        int rate = 0;
    };
    struct Out;

    void writer_loop();
    bool handle_video(Item &it);
    void handle_audio(Item &it);
    bool open_output(int w, int h);
    void close_output();
    bool open_video_encoder(int w, int h);
    bool open_audio_encoder();
    void encode_audio_fifo(bool flush);
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
};

} // namespace airscreen
