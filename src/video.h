#pragma once

#include "capture.h"
#include "studio.h"

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

    // Studio: what the window shows and clips record is a 16:9 canvas holding the live scene (see Studio).
    void set_canvas(int canvas_h);
    // Makes `scene` live, cross-fading for `fade_s` seconds (0 cuts). The text and pictures it needs are drawn
    // first, here on the calling thread, so the render thread never waits on GDI+.
    void set_scene(const Scene &scene, double fade_s);
    // Asks the render thread for a fresh frame (new picture, resize, scene change, recording start).
    void request_render();
    // The iPad's session ended: its picture goes; the computer screen, if shared, carries on.
    void clear();

    // The computer screen: one display or window at a time, shared by every scene that shows it. Frames
    // beyond `max_fps` are skipped to leave the GPU to the apps being taught with.
    bool start_computer(const CaptureTarget &target, bool cursor, int max_fps, std::string *err);
    void stop_computer();
    bool computer_running() const { return capture_.running(); }
    bool computer_live() const { return comp_live_.load(); } // a picture has arrived
    // Called on the capture thread when the first picture arrives, and when the shared window closes or the
    // display goes away. Set once, before any capture starts.
    void set_computer_events(std::function<void()> first, std::function<void()> closed) {
        comp_first_fn_ = std::move(first);
        comp_closed_fn_ = std::move(closed);
    }

    // A second view for the editor and preview window, only drawn while one is attached.
    enum PreviewMode { PreviewScene = 0, PreviewLive = 1, PreviewSource = 2 };
    bool attach_preview(HWND hwnd);
    void detach_preview();
    void preview_resize();
    void set_preview_mode(int mode, SourceKind source);
    void set_preview_scene(const Scene &scene, const std::vector<Studio::Mark> &marks);

    int width() const { return width_.load(); }
    int height() const { return height_.load(); }
    bool has_frame() const { return has_frame_.load(); }
    bool live() const { return has_frame_.load() || comp_live_.load(); }

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
    bool create_swapchain(HWND hwnd, bool main, IDXGISwapChain1 **sc, ID3D11RenderTargetView **rtv);
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
    void render_loop();
    void render_once();
    // Fits (or, with `cover`, crops) a picture into a window's swap chain, bicubic from the right mip.
    void present(IDXGISwapChain1 *sc, ID3D11RenderTargetView *rtv, HWND hwnd, ID3D11ShaderResourceView *srv, int w,
                 int h, int mips, bool gen_mips, bool cover, bool main);
    void prepare(const Scene &scene, bool preview);
    void on_computer_frame(ID3D11Texture2D *tex, int w, int h);
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
    std::atomic<bool> has_frame_{false};
    std::atomic<bool> cover_{false};
    std::atomic<bool> smooth_{false};
    std::mutex mu_;
    std::function<void()> first_frame_fn_;
    Recorder *recorder_ = nullptr; // guarded by mu_
    Studio studio_;                // guarded by mu_
    bool src_dirty_ = false;       // nv12_ holds a picture not yet converted for the studio; guarded by mu_

    // Computer screen: the latest capture, BGRA with mips; guarded by mu_.
    ScreenCapture capture_;
    ID3D11Texture2D *comp_ = nullptr;
    ID3D11ShaderResourceView *comp_srv_ = nullptr;
    int comp_w_ = 0, comp_h_ = 0, comp_mips_ = 1;
    bool comp_dirty_ = false; // needs its mips rebuilt
    std::atomic<bool> comp_live_{false};
    int64_t comp_last_us_ = 0;      // capture thread only
    int64_t comp_interval_us_ = 0;  // set before capture starts
    std::function<void()> comp_first_fn_;
    std::function<void()> comp_closed_fn_;

    // Preview; guarded by mu_.
    HWND preview_hwnd_ = nullptr;
    IDXGISwapChain1 *preview_sc_ = nullptr;
    ID3D11RenderTargetView *preview_rtv_ = nullptr;
    int preview_mode_ = PreviewScene;
    SourceKind preview_source_ = SourceKind::IPad;
    Scene preview_scene_;
    std::vector<Studio::Mark> preview_marks_;
    bool preview_dirty_ = false;   // must redraw now (an edit), not at the capped rate
    bool preview_pending_ = false; // a frame was skipped by the cap; draw it soon
    int64_t preview_last_us_ = 0;

    // The render thread composes and presents; decoding only fills nv12_ and wakes it.
    std::mutex render_mu_;
    std::condition_variable render_cv_;
    bool render_dirty_ = false; // guarded by render_mu_
    std::atomic<bool> render_run_{false};
    std::thread render_;

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
