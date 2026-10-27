#pragma once

#include <d3d11.h>
#include <dxgi1_2.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

struct HWND__;
typedef HWND__ *HWND;

namespace airscreen {

class Recorder;

class VideoPipeline {
public:
    VideoPipeline();
    ~VideoPipeline();

    bool init(HWND hwnd);
    void shutdown();
    void resize();
    void flush();

    void submit(const uint8_t *data, int len, uint64_t ntp_ns, bool hevc);
    void set_source_size(int width, int height);
    void set_cover(bool cover);
    void set_first_frame_fn(std::function<void()> fn);
    // Smooth: hold each frame until its place on the sender's clock (plus a small cushion) so Wi-Fi jitter
    // doesn't reach the screen. Off: show every frame the moment it decodes, for the lowest latency.
    void set_smooth(bool on) { smooth_.store(on); }
    void set_recorder(Recorder *rec);
    void clear();

    int width() const { return width_.load(); }
    int height() const { return height_.load(); }
    bool has_frame() const { return has_frame_.load(); }

    ID3D11Device *device() const { return device_; }

private:
    struct EncodedPacket {
        std::vector<uint8_t> data;
        int64_t pts_us = 0;     // sender clock when known, else local arrival
        int64_t arrival_us = 0; // local clock
        bool remote_clock = false;
        bool hevc = false;
    };

    bool create_device();
    bool create_swapchain();
    bool create_pipeline();
    bool ensure_nv12(int w, int h);
    bool ensure_rgba(int w, int h);
    bool ensure_staging(int w, int h);
    bool decode_init(bool hevc);
    void decode_close();
    void decode_packet(const EncodedPacket &pkt, bool present);
    void decode_loop();
    void pace(const EncodedPacket &pkt);
    void start_worker();
    void stop_worker();
    void present_nv12();
    void upload_yuv420(const uint8_t *y, int y_stride, const uint8_t *u, const uint8_t *v, int uv_stride, int w, int h);

    HWND hwnd_ = nullptr;
    ID3D11Device *device_ = nullptr;
    ID3D11DeviceContext *ctx_ = nullptr;
    IDXGISwapChain1 *swapchain_ = nullptr;
    ID3D11RenderTargetView *rtv_ = nullptr;
    ID3D11VertexShader *vs_ = nullptr;
    ID3D11PixelShader *ps_ = nullptr;       // NV12 -> RGB
    ID3D11PixelShader *ps_scale_ = nullptr; // RGB -> screen, bicubic
    ID3D11InputLayout *layout_ = nullptr;
    ID3D11Buffer *vb_ = nullptr;
    ID3D11SamplerState *sampler_ = nullptr;
    ID3D11Texture2D *nv12_ = nullptr;
    ID3D11ShaderResourceView *srv_y_ = nullptr;
    ID3D11ShaderResourceView *srv_uv_ = nullptr;
    // Full-resolution RGB copy of the frame with a mip chain, so shrinking a 4K stream into a small window
    // doesn't shimmer and enlarging it stays crisp.
    ID3D11Texture2D *rgba_ = nullptr;
    ID3D11RenderTargetView *rgba_rtv_ = nullptr;
    ID3D11ShaderResourceView *rgba_srv_ = nullptr;
    int rgba_w_ = 0;
    int rgba_h_ = 0;
    int rgba_mips_ = 1;
    ID3D11RasterizerState *raster_ = nullptr;
    ID3D11Buffer *cbuf_ = nullptr;
    ID3D11Buffer *unit_cbuf_ = nullptr;
    ID3D11Buffer *color_cbuf_ = nullptr;
    ID3D11Buffer *scale_cbuf_ = nullptr;
    ID3D11Texture2D *staging_ = nullptr;
    bool full_range_ = true;
    bool color_cbuf_dirty_ = true;

    void *av_hw_device_ = nullptr;
    void *av_codec_ = nullptr;
    void *av_frame_ = nullptr;
    void *av_pkt_ = nullptr;
    bool hevc_ = false;
    bool tearing_ = false;

    std::atomic<int> width_{0};
    std::atomic<int> height_{0};
    int tex_w_ = 0;
    int tex_h_ = 0;
    int staging_w_ = 0;
    int staging_h_ = 0;
    int present_cw_ = 0;
    int present_ch_ = 0;
    float letter_x_ = 0;
    float letter_y_ = 0;
    std::atomic<bool> has_frame_{false};
    std::atomic<bool> cover_{false};
    std::atomic<bool> smooth_{false};
    std::mutex mu_;
    std::function<void()> first_frame_fn_;
    Recorder *recorder_ = nullptr; // guarded by mu_

    // Packets are never dropped one at a time: losing one corrupts every frame after it until the next
    // keyframe, which iOS rarely sends. A backlog is decoded in full and only its newest frame is shown.
    static constexpr size_t kMaxQueued = 240;
    std::mutex q_mu_;
    std::condition_variable q_cv_;
    std::deque<EncodedPacket> q_;
    std::atomic<bool> worker_run_{false};
    std::thread worker_;

    // Smooth-mode clock mapping (decode thread only): local = sender + offset_us_.
    bool offset_valid_ = false;
    int64_t offset_us_ = 0;
};

} // namespace airscreen
