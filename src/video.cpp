#include "video.h"
#include "recorder.h"
#include "settings.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi.h>
#include <dxgi1_3.h>
#include <dxgi1_5.h>
#include <d3dcompiler.h>
#include <avrt.h>
#include <wrl/client.h>
#include <algorithm>
#include <vector>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <chrono>

#pragma comment(lib, "avrt.lib")

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/error.h>
}

using Microsoft::WRL::ComPtr;

namespace airscreen {
namespace {

const char *kVs = R"(
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD; };
cbuffer CB : register(b0) { float4 letterbox; };
VSOut main(float2 pos : POSITION, float2 uv : TEXCOORD) {
    VSOut o;
    o.pos = float4(pos.x * letterbox.x, pos.y * letterbox.y, 0, 1);
    o.uv = uv;
    return o;
}
)";

const char *kPs = R"(
Texture2D texY : register(t0);
Texture2D texUV : register(t1);
SamplerState samp : register(s0);
cbuffer ColorCB : register(b1) { float full_range; float3 pad; };
float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD) : SV_TARGET {
    float y = texY.Sample(samp, uv).r;
    float2 chroma = texUV.Sample(samp, uv).rg;
    float d = chroma.r - 0.5;
    float e = chroma.g - 0.5;
    float r, g, b;
    if (full_range > 0.5) {
        r = y + 1.5748 * e;
        g = y - 0.1873 * d - 0.4681 * e;
        b = y + 1.8556 * d;
    } else {
        float c = y - 0.0627451;
        r = 1.164383 * c + 1.792741 * e;
        g = 1.164383 * c - 0.213249 * d - 0.532909 * e;
        b = 1.164383 * c + 2.112402 * d;
    }
    return float4(saturate(r), saturate(g), saturate(b), 1);
}
)";

// Catmull-Rom bicubic in 9 bilinear taps, read from the mip level nearest above the output size: sharper than
// bilinear when enlarging, and with the mip chain no aliasing when shrinking.
const char *kPsScale = R"(
Texture2D tex : register(t0);
SamplerState samp : register(s0);
cbuffer ScaleCB : register(b2) { float2 tex_size; float lod; float pad; };
float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD) : SV_TARGET {
    float2 p = uv * tex_size;
    float2 t1 = floor(p - 0.5) + 0.5;
    float2 f = p - t1;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2;
    float2 t0 = (t1 - 1.0) / tex_size;
    float2 t3 = (t1 + 2.0) / tex_size;
    float2 t12 = (t1 + w2 / w12) / tex_size;
    float3 c = 0;
    c += tex.SampleLevel(samp, float2(t0.x, t0.y), lod).rgb * w0.x * w0.y;
    c += tex.SampleLevel(samp, float2(t12.x, t0.y), lod).rgb * w12.x * w0.y;
    c += tex.SampleLevel(samp, float2(t3.x, t0.y), lod).rgb * w3.x * w0.y;
    c += tex.SampleLevel(samp, float2(t0.x, t12.y), lod).rgb * w0.x * w12.y;
    c += tex.SampleLevel(samp, float2(t12.x, t12.y), lod).rgb * w12.x * w12.y;
    c += tex.SampleLevel(samp, float2(t3.x, t12.y), lod).rgb * w3.x * w12.y;
    c += tex.SampleLevel(samp, float2(t0.x, t3.y), lod).rgb * w0.x * w3.y;
    c += tex.SampleLevel(samp, float2(t12.x, t3.y), lod).rgb * w12.x * w3.y;
    c += tex.SampleLevel(samp, float2(t3.x, t3.y), lod).rgb * w3.x * w3.y;
    return float4(saturate(c), 1);
}
)";

int64_t now_us() {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (int64_t) (now.QuadPart / freq.QuadPart * 1000000 + now.QuadPart % freq.QuadPart * 1000000 / freq.QuadPart);
}

template <typename T> void release(T *&p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

struct Vertex {
    float x, y, u, v;
};

AVPixelFormat hw_get_format(AVCodecContext *ctx, const AVPixelFormat *fmt) {
    (void) ctx;
    for (int i = 0; fmt[i] != AV_PIX_FMT_NONE; ++i) {
        if (fmt[i] == AV_PIX_FMT_D3D11) {
            return AV_PIX_FMT_D3D11;
        }
    }
    for (int i = 0; fmt[i] != AV_PIX_FMT_NONE; ++i) {
        if (fmt[i] == AV_PIX_FMT_NV12 || fmt[i] == AV_PIX_FMT_YUV420P) {
            return fmt[i];
        }
    }
    return AV_PIX_FMT_NONE;
}

} // namespace

VideoPipeline::VideoPipeline() = default;

VideoPipeline::~VideoPipeline() {
    shutdown();
}

bool VideoPipeline::init(HWND hwnd) {
    stop_worker();
    {
        std::lock_guard<std::mutex> lock(mu_);
        hwnd_ = hwnd;
        if (!create_device()) {
            return false;
        }
        if (!create_swapchain(hwnd_, true, &swapchain_, &rtv_)) {
            return false;
        }
        if (!create_pipeline()) {
            return false;
        }
        if (!studio_.init(device_, ctx_) || !studio_.set_canvas_size(2560, 1440)) {
            append_log("studio init failed");
            return false;
        }
    }
    start_worker();
    return true;
}

void VideoPipeline::stop_worker() {
    render_run_ = false;
    render_cv_.notify_all();
    if (render_.joinable()) {
        render_.join();
    }
    worker_run_ = false;
    q_cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    std::lock_guard<std::mutex> qlock(q_mu_);
    q_.clear();
}

void VideoPipeline::start_worker() {
    if (worker_run_ || worker_.joinable()) {
        return;
    }
    worker_run_ = true;
    worker_ = std::thread([this] { decode_loop(); });
    render_run_ = true;
    render_ = std::thread([this] { render_loop(); });
}

void VideoPipeline::shutdown() {
    capture_.stop(); // before taking mu_: a frame being handed over holds the capture's lock and wants mu_
    stop_worker();
    std::lock_guard<std::mutex> lock(mu_);
    decode_close();
    studio_.shutdown();
    comp_live_ = false;
    release(comp_srv_);
    release(comp_);
    comp_w_ = comp_h_ = 0;
    release(preview_rtv_);
    release(preview_sc_);
    preview_hwnd_ = nullptr;
    if (staging_) {
        staging_->Release();
        staging_ = nullptr;
        staging_w_ = staging_h_ = 0;
    }
    release(rgba_srv_);
    release(rgba_rtv_);
    release(rgba_);
    rgba_w_ = rgba_h_ = 0;
    release(ps_scale_);
    release(unit_cbuf_);
    release(scale_cbuf_);
    if (srv_y_) {
        srv_y_->Release();
        srv_y_ = nullptr;
    }
    if (srv_uv_) {
        srv_uv_->Release();
        srv_uv_ = nullptr;
    }
    if (nv12_) {
        nv12_->Release();
        nv12_ = nullptr;
    }
    tex_w_ = tex_h_ = 0;
    if (cbuf_) {
        cbuf_->Release();
        cbuf_ = nullptr;
    }
    if (color_cbuf_) {
        color_cbuf_->Release();
        color_cbuf_ = nullptr;
    }
    if (raster_) {
        raster_->Release();
        raster_ = nullptr;
    }
    if (sampler_) {
        sampler_->Release();
        sampler_ = nullptr;
    }
    if (vb_) {
        vb_->Release();
        vb_ = nullptr;
    }
    if (layout_) {
        layout_->Release();
        layout_ = nullptr;
    }
    if (vs_) {
        vs_->Release();
        vs_ = nullptr;
    }
    if (ps_) {
        ps_->Release();
        ps_ = nullptr;
    }
    if (rtv_) {
        rtv_->Release();
        rtv_ = nullptr;
    }
    if (swapchain_) {
        swapchain_->Release();
        swapchain_ = nullptr;
    }
    if (ctx_) {
        ctx_->Release();
        ctx_ = nullptr;
    }
    if (device_) {
        device_->Release();
        device_ = nullptr;
    }
    hwnd_ = nullptr;
}

void VideoPipeline::resize() {
    std::lock_guard<std::mutex> lock(mu_);
    if (!swapchain_ || !ctx_) {
        return;
    }
    ctx_->OMSetRenderTargets(0, nullptr, nullptr);
    if (rtv_) {
        rtv_->Release();
        rtv_ = nullptr;
    }
    UINT flags = tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    HRESULT hr = swapchain_->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, flags);
    if (FAILED(hr)) {
        return;
    }
    ID3D11Texture2D *back = nullptr;
    if (SUCCEEDED(swapchain_->GetBuffer(0, IID_PPV_ARGS(&back)))) {
        device_->CreateRenderTargetView(back, nullptr, &rtv_);
        back->Release();
    }
    request_render();
}

void VideoPipeline::flush() {
    {
        std::lock_guard<std::mutex> qlock(q_mu_);
        q_.clear();
    }
    std::lock_guard<std::mutex> lock(mu_);
    if (av_codec_) {
        avcodec_flush_buffers((AVCodecContext *) av_codec_);
    }
}

void VideoPipeline::clear() {
    {
        std::lock_guard<std::mutex> qlock(q_mu_);
        q_.clear();
    }
    std::lock_guard<std::mutex> lock(mu_);
    has_frame_ = false;
    if (comp_live_.load()) {
        request_render(); // the computer screen carries the scene on its own
        return;
    }
    studio_.reset();
    if (rtv_ && ctx_ && swapchain_) {
        float black[4] = {0.07f, 0.08f, 0.10f, 1};
        ctx_->OMSetRenderTargets(1, &rtv_, nullptr);
        ctx_->ClearRenderTargetView(rtv_, black);
        swapchain_->Present(0, tearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0);
    }
}

void VideoPipeline::set_cover(bool cover) {
    cover_.store(cover);
}

void VideoPipeline::set_recorder(Recorder *rec) {
    std::lock_guard<std::mutex> lock(mu_);
    recorder_ = rec;
}

void VideoPipeline::request_render() {
    {
        std::lock_guard<std::mutex> lock(render_mu_);
        render_dirty_ = true;
    }
    render_cv_.notify_one();
}

// Draws the text and pictures `scene` needs that aren't cached yet. GDI+ runs here, off the D3D lock; only the
// upload takes it.
void VideoPipeline::prepare(const Scene &scene, bool preview) {
    std::vector<Studio::Need> needs;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!device_) {
            return;
        }
        needs = studio_.needs(scene, preview);
    }
    if (needs.empty()) {
        return;
    }
    std::vector<std::vector<uint32_t>> px(needs.size());
    for (size_t i = 0; i < needs.size(); ++i) {
        px[i] = Studio::make_raster(needs[i]);
    }
    std::lock_guard<std::mutex> lock(mu_);
    if (!device_) {
        return;
    }
    for (size_t i = 0; i < needs.size(); ++i) {
        studio_.put_raster(needs[i].key, px[i], needs[i].w, needs[i].h);
    }
}

void VideoPipeline::set_scene(const Scene &scene, double fade_s) {
    prepare(scene, false);
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!device_) {
            return;
        }
        studio_.set_scene(scene, (double) now_us() / 1e6, fade_s);
    }
    request_render();
}

void VideoPipeline::set_canvas(int canvas_h) {
    int ch = std::clamp(canvas_h, 360, 2160) & ~1;
    int cw = (ch * 16 / 9) & ~1;
    Scene live, preview;
    bool previewing;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!device_ || (studio_.canvas_w() == cw && studio_.canvas_h() == ch)) {
            return;
        }
        studio_.set_canvas_size(cw, ch);
        live = studio_.scene();
        preview = preview_scene_;
        previewing = preview_sc_ != nullptr;
        preview_dirty_ = true;
    }
    // Text is drawn at canvas size, so everything on show is drawn again for the new one.
    prepare(live, false);
    if (previewing) {
        prepare(preview, true);
    }
    request_render();
}

bool VideoPipeline::start_computer(const CaptureTarget &target, bool cursor, int max_fps, std::string *err) {
    stop_computer();
    ID3D11Device *dev;
    {
        std::lock_guard<std::mutex> lock(mu_);
        dev = device_;
    }
    comp_interval_us_ = max_fps > 0 ? 1000000 / max_fps - 2000 : 0;
    comp_last_us_ = 0;
    return capture_.start(
        dev, target, cursor, [this](ID3D11Texture2D *tex, int w, int h) { on_computer_frame(tex, w, h); },
        [this] {
            if (comp_closed_fn_) {
                comp_closed_fn_();
            }
        },
        err);
}

void VideoPipeline::stop_computer() {
    capture_.stop(); // not under mu_ (see shutdown)
    {
        std::lock_guard<std::mutex> lock(mu_);
        comp_live_ = false;
        comp_dirty_ = false;
        release(comp_srv_);
        release(comp_);
        comp_w_ = comp_h_ = 0;
        preview_dirty_ = true;
    }
    request_render();
}

// Capture thread: copy the new picture over; the render thread rebuilds its mips and composes.
void VideoPipeline::on_computer_frame(ID3D11Texture2D *tex, int w, int h) {
    int64_t now = now_us();
    if (comp_interval_us_ > 0 && now - comp_last_us_ < comp_interval_us_) {
        return;
    }
    comp_last_us_ = now;
    bool first;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!device_ || !ctx_) {
            return;
        }
        if (!comp_ || comp_w_ != w || comp_h_ != h) {
            release(comp_srv_);
            release(comp_);
            int mips = 1;
            for (int m = (std::max)(w, h); m > 1; m >>= 1) {
                ++mips;
            }
            D3D11_TEXTURE2D_DESC td = {};
            td.Width = (UINT) w;
            td.Height = (UINT) h;
            td.MipLevels = (UINT) mips;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
            if (FAILED(device_->CreateTexture2D(&td, nullptr, &comp_)) ||
                FAILED(device_->CreateShaderResourceView(comp_, nullptr, &comp_srv_))) {
                release(comp_);
                comp_w_ = comp_h_ = 0;
                return;
            }
            comp_w_ = w;
            comp_h_ = h;
            comp_mips_ = mips;
        }
        D3D11_BOX box = {0, 0, 0, (UINT) w, (UINT) h, 1};
        ctx_->CopySubresourceRegion(comp_, 0, 0, 0, 0, tex, 0, &box);
        comp_dirty_ = true;
        first = !comp_live_.exchange(true);
    }
    request_render();
    if (first && comp_first_fn_) {
        comp_first_fn_();
    }
}

bool VideoPipeline::attach_preview(HWND hwnd) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!device_) {
            return false;
        }
        release(preview_rtv_);
        release(preview_sc_);
        if (!create_swapchain(hwnd, false, &preview_sc_, &preview_rtv_)) {
            release(preview_rtv_);
            release(preview_sc_);
            return false;
        }
        preview_hwnd_ = hwnd;
        studio_.set_preview_size(1280, 720);
        preview_dirty_ = true;
    }
    request_render();
    return true;
}

void VideoPipeline::detach_preview() {
    std::lock_guard<std::mutex> lock(mu_);
    if (ctx_) {
        ctx_->OMSetRenderTargets(0, nullptr, nullptr);
    }
    release(preview_rtv_);
    release(preview_sc_);
    preview_hwnd_ = nullptr;
    preview_marks_.clear();
    if (device_) {
        studio_.set_preview_size(0, 0); // the preview target is only kept while someone looks at it
    }
}

void VideoPipeline::preview_resize() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!preview_sc_ || !ctx_) {
            return;
        }
        ctx_->OMSetRenderTargets(0, nullptr, nullptr);
        release(preview_rtv_);
        if (FAILED(preview_sc_->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0))) {
            return;
        }
        ID3D11Texture2D *back = nullptr;
        if (SUCCEEDED(preview_sc_->GetBuffer(0, IID_PPV_ARGS(&back)))) {
            device_->CreateRenderTargetView(back, nullptr, &preview_rtv_);
            back->Release();
        }
        preview_dirty_ = true;
    }
    request_render();
}

void VideoPipeline::set_preview_mode(int mode, SourceKind source) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        preview_mode_ = mode;
        preview_source_ = source;
        preview_dirty_ = true;
    }
    request_render();
}

void VideoPipeline::set_preview_scene(const Scene &scene, const std::vector<Studio::Mark> &marks) {
    prepare(scene, true);
    {
        std::lock_guard<std::mutex> lock(mu_);
        preview_scene_ = scene;
        preview_marks_ = marks;
        preview_dirty_ = true;
    }
    request_render();
}

void VideoPipeline::set_first_frame_fn(std::function<void()> fn) {
    std::lock_guard<std::mutex> lock(mu_);
    first_frame_fn_ = std::move(fn);
}

void VideoPipeline::set_source_size(int width, int height) {
    width_.store(width);
    height_.store(height);
}

bool VideoPipeline::create_device() {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 3,
                                   D3D11_SDK_VERSION, &device_, &got, &ctx_);
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                               levels, 3, D3D11_SDK_VERSION, &device_, &got, &ctx_);
    }
    if (FAILED(hr)) {
        return false;
    }
    ComPtr<ID3D11Multithread> mt;
    if (SUCCEEDED(device_->QueryInterface(IID_PPV_ARGS(&mt)))) {
        mt->SetMultithreadProtected(TRUE);
    }
    return true;
}

// `main` is the program window, which may tear for the lowest latency; the preview never does.
bool VideoPipeline::create_swapchain(HWND hwnd, bool main, IDXGISwapChain1 **sc, ID3D11RenderTargetView **rtv) {
    ComPtr<IDXGIDevice> dxgi_dev;
    device_->QueryInterface(IID_PPV_ARGS(&dxgi_dev));
    ComPtr<IDXGIAdapter> adapter;
    dxgi_dev->GetAdapter(&adapter);
    ComPtr<IDXGIFactory2> factory;
    adapter->GetParent(IID_PPV_ARGS(&factory));

    bool tearing = false;
    if (main) {
        BOOL allow_tearing = FALSE;
        ComPtr<IDXGIFactory5> factory5;
        if (SUCCEEDED(factory.As(&factory5))) {
            factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow_tearing,
                                         sizeof(allow_tearing));
        }
        tearing = allow_tearing != FALSE;
    }

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.Flags = tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    desc.Scaling = DXGI_SCALING_STRETCH;

    HRESULT hr = factory->CreateSwapChainForHwnd(device_, hwnd, &desc, nullptr, nullptr, sc);
    if (FAILED(hr)) {
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        hr = factory->CreateSwapChainForHwnd(device_, hwnd, &desc, nullptr, nullptr, sc);
        if (FAILED(hr) && tearing) {
            tearing = false;
            desc.Flags = 0;
            hr = factory->CreateSwapChainForHwnd(device_, hwnd, &desc, nullptr, nullptr, sc);
        }
        if (FAILED(hr)) {
            tearing = false;
            if (main) {
                tearing_ = false;
            }
            return false;
        }
    }
    if (main) {
        tearing_ = tearing;
    }
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    ComPtr<IDXGISwapChain2> sc2;
    if (SUCCEEDED((*sc)->QueryInterface(IID_PPV_ARGS(&sc2)))) {
        sc2->SetMaximumFrameLatency(1);
    }

    ID3D11Texture2D *back = nullptr;
    (*sc)->GetBuffer(0, IID_PPV_ARGS(&back));
    device_->CreateRenderTargetView(back, nullptr, rtv);
    back->Release();
    return true;
}

bool VideoPipeline::create_pipeline() {
    ComPtr<ID3DBlob> vsb, psb, err;
    HRESULT hr = D3DCompile(kVs, strlen(kVs), "vs", nullptr, nullptr, "main", "vs_4_0", 0, 0, &vsb, &err);
    if (FAILED(hr)) {
        return false;
    }
    hr = D3DCompile(kPs, strlen(kPs), "ps", nullptr, nullptr, "main", "ps_4_0", 0, 0, &psb, &err);
    if (FAILED(hr)) {
        return false;
    }
    device_->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs_);
    device_->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps_);

    D3D11_INPUT_ELEMENT_DESC ied[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    device_->CreateInputLayout(ied, 2, vsb->GetBufferPointer(), vsb->GetBufferSize(), &layout_);

    Vertex quad[] = {
        {-1, 1, 0, 0}, {1, 1, 1, 0}, {-1, -1, 0, 1}, {1, -1, 1, 1},
    };
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(quad);
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA srd = {quad};
    device_->CreateBuffer(&bd, &srd, &vb_);

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = 16;
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    device_->CreateBuffer(&cbd, nullptr, &cbuf_);

    D3D11_BUFFER_DESC colord = {};
    colord.ByteWidth = 16;
    colord.Usage = D3D11_USAGE_DYNAMIC;
    colord.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    colord.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    device_->CreateBuffer(&colord, nullptr, &color_cbuf_);
    color_cbuf_dirty_ = true;
    device_->CreateBuffer(&colord, nullptr, &scale_cbuf_);

    float unit[4] = {1, 1, 0, 0};
    D3D11_BUFFER_DESC unitd = {};
    unitd.ByteWidth = 16;
    unitd.Usage = D3D11_USAGE_IMMUTABLE;
    unitd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA unitsrd = {unit};
    device_->CreateBuffer(&unitd, &unitsrd, &unit_cbuf_);

    // The bicubic pass is a quality upgrade; without it frames go straight from NV12 to the screen.
    ComPtr<ID3DBlob> scaleb;
    if (SUCCEEDED(D3DCompile(kPsScale, strlen(kPsScale), "ps_scale", nullptr, nullptr, "main", "ps_4_0",
                             D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &scaleb, &err))) {
        device_->CreatePixelShader(scaleb->GetBufferPointer(), scaleb->GetBufferSize(), nullptr, &ps_scale_);
    } else {
        append_log("bicubic shader unavailable; using bilinear scaling");
    }

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    device_->CreateSamplerState(&sd, &sampler_);

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    device_->CreateRasterizerState(&rd, &raster_);
    return true;
}

bool VideoPipeline::ensure_nv12(int w, int h) {
    if (nv12_ && tex_w_ == w && tex_h_ == h) {
        return true;
    }
    if (srv_y_) {
        srv_y_->Release();
        srv_y_ = nullptr;
    }
    if (srv_uv_) {
        srv_uv_->Release();
        srv_uv_ = nullptr;
    }
    if (nv12_) {
        nv12_->Release();
        nv12_ = nullptr;
    }
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_NV12;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, &nv12_))) {
        return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC svd = {};
    svd.Format = DXGI_FORMAT_R8_UNORM;
    svd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    svd.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(nv12_, &svd, &srv_y_);
    svd.Format = DXGI_FORMAT_R8G8_UNORM;
    device_->CreateShaderResourceView(nv12_, &svd, &srv_uv_);
    tex_w_ = w;
    tex_h_ = h;
    return srv_y_ && srv_uv_;
}

bool VideoPipeline::ensure_rgba(int w, int h) {
    if (rgba_ && rgba_w_ == w && rgba_h_ == h) {
        return true;
    }
    release(rgba_srv_);
    release(rgba_rtv_);
    release(rgba_);
    rgba_w_ = rgba_h_ = 0;
    int mips = 1;
    for (int m = (w > h ? w : h); m > 1; m >>= 1) {
        ++mips;
    }
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = (UINT) mips;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, &rgba_))) {
        return false;
    }
    D3D11_RENDER_TARGET_VIEW_DESC rvd = {};
    rvd.Format = td.Format;
    rvd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    if (FAILED(device_->CreateRenderTargetView(rgba_, &rvd, &rgba_rtv_)) ||
        FAILED(device_->CreateShaderResourceView(rgba_, nullptr, &rgba_srv_))) {
        release(rgba_srv_);
        release(rgba_rtv_);
        release(rgba_);
        return false;
    }
    rgba_w_ = w;
    rgba_h_ = h;
    rgba_mips_ = mips;
    return true;
}

bool VideoPipeline::ensure_staging(int w, int h) {
    if (staging_ && staging_w_ == w && staging_h_ == h) {
        return true;
    }
    if (staging_) {
        staging_->Release();
        staging_ = nullptr;
    }
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_NV12;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, &staging_))) {
        staging_w_ = staging_h_ = 0;
        return false;
    }
    staging_w_ = w;
    staging_h_ = h;
    return true;
}

bool VideoPipeline::decode_init(bool hevc) {
    decode_close();
    hevc_ = hevc;
    const AVCodec *codec = avcodec_find_decoder(hevc ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264);
    if (!codec) {
        return false;
    }
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    ctx->flags2 |= AV_CODEC_FLAG2_FAST | AV_CODEC_FLAG2_SHOW_ALL;
    // Slice threads only: they add no delay, and keep a software fallback fast enough for 4K HEVC.
    ctx->thread_type = FF_THREAD_SLICE;
    ctx->thread_count = 0;
    ctx->pkt_timebase = AVRational{1, 1000000};
    ctx->extra_hw_frames = 8;

    AVBufferRef *hw = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (hw) {
        auto *hwctx = (AVHWDeviceContext *) hw->data;
        auto *d3 = (AVD3D11VADeviceContext *) hwctx->hwctx;
        d3->device = device_;
        device_->AddRef();
        if (av_hwdevice_ctx_init(hw) == 0) {
            ctx->hw_device_ctx = av_buffer_ref(hw);
            ctx->get_format = hw_get_format;
            av_hw_device_ = hw;
        } else {
            av_buffer_unref(&hw);
        }
    }

    if (avcodec_open2(ctx, codec, nullptr) < 0) {
        avcodec_free_context(&ctx);
        if (av_hw_device_) {
            av_buffer_unref((AVBufferRef **) &av_hw_device_);
        }
        return false;
    }
    av_codec_ = ctx;
    av_frame_ = av_frame_alloc();
    av_pkt_ = av_packet_alloc();
    return true;
}

void VideoPipeline::decode_close() {
    if (av_pkt_) {
        av_packet_free((AVPacket **) &av_pkt_);
    }
    if (av_frame_) {
        av_frame_free((AVFrame **) &av_frame_);
    }
    if (av_codec_) {
        AVCodecContext *c = (AVCodecContext *) av_codec_;
        avcodec_free_context(&c);
        av_codec_ = nullptr;
    }
    if (av_hw_device_) {
        av_buffer_unref((AVBufferRef **) &av_hw_device_);
        av_hw_device_ = nullptr;
    }
}

void VideoPipeline::submit(const uint8_t *data, int len, uint64_t ntp_ns, bool hevc) {
    if (!data || len <= 0 || !worker_run_) {
        return;
    }
    EncodedPacket pkt;
    pkt.data.assign(data, data + len);
    pkt.arrival_us = now_us();
    pkt.remote_clock = ntp_ns != 0;
    pkt.pts_us = pkt.remote_clock ? (int64_t) (ntp_ns / 1000) : pkt.arrival_us;
    pkt.hevc = hevc;
    bool overflow = false;
    {
        std::lock_guard<std::mutex> qlock(q_mu_);
        if (q_.size() >= kMaxQueued) {
            // Seconds behind: the decoder has stalled. Start over rather than play stale video.
            q_.clear();
            overflow = true;
        }
        q_.push_back(std::move(pkt));
    }
    if (overflow) {
        append_log("video backlog overflow; dropped queued packets");
    }
    q_cv_.notify_one();
}

void VideoPipeline::pace(const EncodedPacket &pkt) {
    if (!smooth_.load() || !pkt.remote_clock) {
        offset_valid_ = false;
        return;
    }
    // Map the sender clock onto ours using the least-delayed packet seen: that one met no queueing on the
    // network, so offset + a fixed cushion is when every frame can be shown on an even beat.
    constexpr int64_t kCushionUs = 60000;
    constexpr int64_t kDecodeBudgetUs = 3000;
    int64_t d = pkt.arrival_us - pkt.pts_us;
    if (!offset_valid_ || d < offset_us_ || d - offset_us_ > 1000000) {
        offset_us_ = d;
        offset_valid_ = true;
    } else {
        offset_us_ += 2; // follow slow drift between the two clocks (~120 us/s)
    }
    int64_t due = pkt.pts_us + offset_us_ + kCushionUs - kDecodeBudgetUs;
    for (;;) {
        int64_t wait = due - now_us();
        if (wait <= 0 || wait > 500000 || !worker_run_) {
            return;
        }
        if (wait > 1500) {
            Sleep((DWORD) ((wait - 1000) / 1000 < 10 ? (wait - 1000) / 1000 : 10));
        } else {
            SwitchToThread();
        }
    }
}

void VideoPipeline::decode_loop() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    DWORD mmcss_idx = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Games", &mmcss_idx);
    if (mmcss) {
        AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_HIGH);
    }
    while (worker_run_) {
        EncodedPacket pkt;
        bool more = false;
        {
            std::unique_lock<std::mutex> qlock(q_mu_);
            q_cv_.wait(qlock, [this] { return !q_.empty() || !worker_run_; });
            if (!worker_run_) {
                break;
            }
            pkt = std::move(q_.front());
            q_.pop_front();
            more = !q_.empty();
        }
        pace(pkt);
        if (smooth_.load() && more) {
            std::lock_guard<std::mutex> qlock(q_mu_);
            // Pacing waited for this frame; only skip it if the next one is already due too.
            more = !q_.empty() && q_.front().arrival_us + 100000 < now_us();
        }
        // When packets piled up (a Wi-Fi burst), decode all of them but show only the newest: catching up
        // at once instead of playing the backlog slowly is what keeps latency from building up.
        decode_packet(pkt, !more);
    }
    if (mmcss) {
        AvRevertMmThreadCharacteristics(mmcss);
    }
}

void VideoPipeline::decode_packet(const EncodedPacket &pkt, bool present) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!device_ || pkt.data.empty()) {
        return;
    }
    const uint8_t *data = pkt.data.data();
    int len = (int) pkt.data.size();
    bool hevc = pkt.hevc;
    if (!av_codec_ || hevc_ != hevc) {
        if (!decode_init(hevc)) {
            return;
        }
    }
    auto *ctx = (AVCodecContext *) av_codec_;
    auto *avpkt = (AVPacket *) av_pkt_;
    auto *frame = (AVFrame *) av_frame_;
    av_packet_unref(avpkt);
    if (av_new_packet(avpkt, len) < 0) {
        return;
    }
    memcpy(avpkt->data, data, (size_t) len);
    avpkt->pts = avpkt->dts = pkt.pts_us;
    int send = avcodec_send_packet(ctx, avpkt);
    if (send < 0 && send != AVERROR(EAGAIN)) {
        decode_init(hevc);
        ctx = (AVCodecContext *) av_codec_;
        avpkt = (AVPacket *) av_pkt_;
        frame = (AVFrame *) av_frame_;
        if (!ctx || av_new_packet(avpkt, len) < 0) {
            return;
        }
        memcpy(avpkt->data, data, (size_t) len);
        avpkt->pts = avpkt->dts = pkt.pts_us;
        send = avcodec_send_packet(ctx, avpkt);
        if (send < 0 && send != AVERROR(EAGAIN)) {
            char es[64];
            av_strerror(send, es, sizeof(es));
            char line[96];
            snprintf(line, sizeof(line), "decode send failed: %s", es);
            append_log(line);
            av_packet_unref(avpkt);
            return;
        }
    }
    av_packet_unref(avpkt);
    if (!ctx) {
        return;
    }
    while (avcodec_receive_frame(ctx, frame) == 0) {
        int w = frame->width & ~1;
        int h = frame->height & ~1;
        if (w <= 0 || h <= 0) {
            av_frame_unref(frame);
            continue;
        }
        width_.store(w);
        height_.store(h);
        if (frame->format == AV_PIX_FMT_YUVJ420P || frame->color_range == AVCOL_RANGE_JPEG) {
            if (!full_range_) {
                full_range_ = true;
                color_cbuf_dirty_ = true;
            }
        } else if (frame->color_range == AVCOL_RANGE_MPEG) {
            if (full_range_) {
                full_range_ = false;
                color_cbuf_dirty_ = true;
            }
        }
        if (!ensure_nv12(w, h)) {
            av_frame_unref(frame);
            continue;
        }
        if (frame->format == AV_PIX_FMT_D3D11) {
            auto *tex = (ID3D11Texture2D *) frame->data[0];
            intptr_t index = (intptr_t) frame->data[1];
            if (!tex) {
                av_frame_unref(frame);
                continue;
            }
            // Decoder surfaces are padded (1080 -> 1088 and so on); copy only the visible picture.
            D3D11_BOX box = {0, 0, 0, (UINT) w, (UINT) h, 1};
            ctx_->CopySubresourceRegion(nv12_, 0, 0, 0, 0, tex, (UINT) index, &box);
        } else if (frame->format == AV_PIX_FMT_NV12) {
            if (ensure_staging(w, h)) {
                D3D11_MAPPED_SUBRESOURCE map = {};
                if (SUCCEEDED(ctx_->Map(staging_, 0, D3D11_MAP_WRITE, 0, &map))) {
                    for (int y = 0; y < h; ++y) {
                        memcpy((uint8_t *) map.pData + y * map.RowPitch, frame->data[0] + y * frame->linesize[0], w);
                    }
                    uint8_t *dst_uv = (uint8_t *) map.pData + map.RowPitch * h;
                    for (int y = 0; y < h / 2; ++y) {
                        memcpy(dst_uv + y * map.RowPitch, frame->data[1] + y * frame->linesize[1], w);
                    }
                    ctx_->Unmap(staging_, 0);
                    ctx_->CopyResource(nv12_, staging_);
                }
            }
        } else if (frame->format == AV_PIX_FMT_YUV420P || frame->format == AV_PIX_FMT_YUVJ420P) {
            upload_yuv420(frame->data[0], frame->linesize[0], frame->data[1], frame->data[2], frame->linesize[1], w, h);
        }
        if (recorder_ && frame->pts != AV_NOPTS_VALUE) {
            recorder_->note_clock(frame->pts, now_us()); // lines iPhone audio up with the local clock
        }
        src_dirty_ = true;
        bool first = !has_frame_.exchange(true);
        if (present || first) {
            request_render(); // the render thread composes and shows it
        }
        av_frame_unref(frame);
        if (first && first_frame_fn_) {
            first_frame_fn_();
        }
    }
}

void VideoPipeline::upload_yuv420(const uint8_t *y, int y_stride, const uint8_t *u, const uint8_t *v, int uv_stride, int w, int h) {
    if (!ensure_staging(w, h)) {
        return;
    }
    D3D11_MAPPED_SUBRESOURCE map = {};
    if (SUCCEEDED(ctx_->Map(staging_, 0, D3D11_MAP_WRITE, 0, &map))) {
        for (int row = 0; row < h; ++row) {
            memcpy((uint8_t *) map.pData + row * map.RowPitch, y + row * y_stride, w);
        }
        uint8_t *dst_uv = (uint8_t *) map.pData + map.RowPitch * h;
        for (int row = 0; row < h / 2; ++row) {
            uint8_t *dst = dst_uv + row * map.RowPitch;
            const uint8_t *su = u + row * uv_stride;
            const uint8_t *sv = v + row * uv_stride;
            for (int x = 0; x < w / 2; ++x) {
                dst[x * 2] = su[x];
                dst[x * 2 + 1] = sv[x];
            }
        }
        ctx_->Unmap(staging_, 0);
        ctx_->CopyResource(nv12_, staging_);
    }
}

void VideoPipeline::render_loop() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    DWORD mmcss_idx = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Games", &mmcss_idx);
    while (render_run_) {
        bool animating, pending;
        {
            std::lock_guard<std::mutex> lock(mu_);
            animating = live() && studio_.animating((double) now_us() / 1e6);
            pending = preview_pending_ && preview_sc_;
        }
        {
            // A fade or a drifting title card runs on a steady ~60 fps tick (new video just rides along); a
            // preview frame held back by its cap comes a little later; otherwise new video wakes us at once,
            // and with nothing new we sleep.
            std::unique_lock<std::mutex> lock(render_mu_);
            if (animating) {
                render_cv_.wait_for(lock, std::chrono::milliseconds(16), [this] { return !render_run_.load(); });
            } else {
                render_cv_.wait_for(lock, std::chrono::milliseconds(pending ? 34 : 250),
                                    [this] { return render_dirty_ || !render_run_; });
            }
            if (!render_run_) {
                break;
            }
            if (!render_dirty_ && !animating && !pending) {
                continue;
            }
            render_dirty_ = false;
        }
        std::lock_guard<std::mutex> lock(mu_);
        render_once();
    }
    if (mmcss) {
        AvRevertMmThreadCharacteristics(mmcss);
    }
}

void VideoPipeline::render_once() {
    bool is_live = live();
    bool previewing = preview_sc_ && preview_rtv_ && preview_hwnd_;
    if (!is_live && !previewing) {
        return;
    }
    UINT stride = sizeof(Vertex), offset = 0;
    auto bind_quad = [&] {
        ctx_->IASetInputLayout(layout_);
        ctx_->IASetVertexBuffers(0, 1, &vb_, &stride, &offset);
        ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        ctx_->VSSetShader(vs_, nullptr, 0);
        ctx_->PSSetSamplers(0, 1, &sampler_);
        ctx_->RSSetState(raster_);
        float factor[4] = {};
        ctx_->OMSetBlendState(nullptr, factor, 0xffffffff);
    };

    // 1. A new iPad picture: NV12 -> RGB at the stream's own resolution, then mips (for the blurred backdrop
    //    and for scaling down without shimmer).
    if (has_frame_.load() && src_dirty_ && nv12_ && tex_w_ > 0 && tex_h_ > 0 && unit_cbuf_ &&
        ensure_rgba(tex_w_, tex_h_)) {
        if (color_cbuf_ && color_cbuf_dirty_) {
            D3D11_MAPPED_SUBRESOURCE map = {};
            if (SUCCEEDED(ctx_->Map(color_cbuf_, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
                float *f = (float *) map.pData;
                f[0] = full_range_ ? 1.0f : 0.0f;
                f[1] = f[2] = f[3] = 0;
                ctx_->Unmap(color_cbuf_, 0);
            }
            color_cbuf_dirty_ = false;
        }
        bind_quad();
        D3D11_VIEWPORT svp = {};
        svp.Width = (float) tex_w_;
        svp.Height = (float) tex_h_;
        svp.MaxDepth = 1;
        ctx_->RSSetViewports(1, &svp);
        ctx_->OMSetRenderTargets(1, &rgba_rtv_, nullptr);
        ctx_->VSSetConstantBuffers(0, 1, &unit_cbuf_);
        ctx_->PSSetShader(ps_, nullptr, 0);
        ctx_->PSSetConstantBuffers(1, 1, &color_cbuf_);
        ID3D11ShaderResourceView *srvs[2] = {srv_y_, srv_uv_};
        ctx_->PSSetShaderResources(0, 2, srvs);
        ctx_->Draw(4, 0);
        ID3D11ShaderResourceView *none2[2] = {};
        ctx_->PSSetShaderResources(0, 2, none2);
        ID3D11RenderTargetView *no_rtv = nullptr;
        ctx_->OMSetRenderTargets(1, &no_rtv, nullptr);
        ctx_->GenerateMips(rgba_srv_);
        src_dirty_ = false;
    }
    // 1b. A new computer picture only needs its mips.
    if (comp_dirty_ && comp_srv_) {
        ctx_->GenerateMips(comp_srv_);
        comp_dirty_ = false;
    }
    Studio::Sources src;
    if (has_frame_.load() && rgba_srv_) {
        src.ipad = {rgba_srv_, rgba_w_, rgba_h_, rgba_mips_};
    }
    if (comp_live_.load() && comp_srv_) {
        src.computer = {comp_srv_, comp_w_, comp_h_, comp_mips_};
    }
    double now = (double) now_us() / 1e6;

    // 2. The live scene, on the 16:9 canvas.
    if (is_live || (previewing && preview_mode_ == PreviewLive)) {
        studio_.render(src, now);
        // 3. Recording gets the canvas exactly as composed: no window, no controls.
        if (is_live && recorder_ && recorder_->active()) {
            ID3D11Texture2D *y = nullptr, *uv = nullptr;
            if (studio_.export_planes(&y, &uv)) {
                recorder_->on_canvas(device_, ctx_, y, uv, studio_.canvas_w(), studio_.canvas_h(), now_us());
            }
        }
        // 4. The window: the canvas fitted (or, full screen with Fill, cropped) to it.
        if (is_live && rtv_ && swapchain_) {
            present(swapchain_, rtv_, hwnd_, studio_.canvas_srv(), studio_.canvas_w(), studio_.canvas_h(),
                    studio_.canvas_mips(), true, cover_.load(), true);
        }
    }

    // 5. The preview: an edited scene, the live canvas, or one screen as it arrives. Capped near 30 fps so
    //    a preview left open while teaching costs little.
    if (previewing) {
        int64_t t = now_us();
        if (!preview_dirty_ && t - preview_last_us_ < 33000) {
            preview_pending_ = true;
            return;
        }
        preview_dirty_ = false;
        preview_pending_ = false;
        preview_last_us_ = t;
        if (preview_mode_ == PreviewLive) {
            present(preview_sc_, preview_rtv_, preview_hwnd_, studio_.canvas_srv(), studio_.canvas_w(),
                    studio_.canvas_h(), studio_.canvas_mips(), false, false, false);
        } else if (preview_mode_ == PreviewSource) {
            const Studio::Source &s = src.get(preview_source_);
            present(preview_sc_, preview_rtv_, preview_hwnd_, s.srv, s.w, s.h, s.mips, false, false, false);
        } else {
            studio_.render_preview(preview_scene_, src, preview_marks_, now);
            present(preview_sc_, preview_rtv_, preview_hwnd_, studio_.preview_srv(), studio_.preview_w(),
                    studio_.preview_h(), studio_.preview_mips(), true, false, false);
        }
    }
}

void VideoPipeline::present(IDXGISwapChain1 *sc, ID3D11RenderTargetView *rtv, HWND hwnd, ID3D11ShaderResourceView *srv,
                            int vw, int vh, int mips, bool gen_mips, bool cover, bool main) {
    RECT rc = {};
    GetClientRect(hwnd, &rc);
    int cw = rc.right - rc.left;
    int ch = rc.bottom - rc.top;
    if (cw <= 0 || ch <= 0 || !ps_scale_ || !scale_cbuf_ || !cbuf_) {
        return;
    }
    UINT stride = sizeof(Vertex), offset = 0;
    ctx_->IASetInputLayout(layout_);
    ctx_->IASetVertexBuffers(0, 1, &vb_, &stride, &offset);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx_->VSSetShader(vs_, nullptr, 0);
    ctx_->PSSetSamplers(0, 1, &sampler_);
    ctx_->RSSetState(raster_);
    float factor[4] = {};
    ctx_->OMSetBlendState(nullptr, factor, 0xffffffff);
    D3D11_VIEWPORT vp = {};
    vp.Width = (float) cw;
    vp.Height = (float) ch;
    vp.MaxDepth = 1;
    ctx_->RSSetViewports(1, &vp);
    ctx_->OMSetRenderTargets(1, &rtv, nullptr);
    float bars[4] = {0.03f, 0.035f, 0.05f, 1};
    float empty[4] = {0.09f, 0.10f, 0.12f, 1};
    ctx_->ClearRenderTargetView(rtv, main ? bars : empty);
    if (srv && vw > 0 && vh > 0) {
        float sx = 1, sy = 1;
        float va = (float) vw / (float) vh;
        float wa = (float) cw / (float) ch;
        if (cover) {
            if (wa > va) {
                sy = wa / va;
            } else {
                sx = va / wa;
            }
        } else if (wa > va) {
            sx = va / wa;
        } else {
            sy = wa / va;
        }
        D3D11_MAPPED_SUBRESOURCE map = {};
        if (SUCCEEDED(ctx_->Map(cbuf_, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
            float *f = (float *) map.pData;
            f[0] = sx;
            f[1] = sy;
            f[2] = f[3] = 0;
            ctx_->Unmap(cbuf_, 0);
        }
        float scale = (std::min)((float) cw * sx / (float) vw, (float) ch * sy / (float) vh);
        int lod = 0;
        if (scale < 1.0f) {
            lod = (std::min)((int) std::floor(std::log2(1.0f / scale)), mips - 1);
        }
        if (lod > 0 && gen_mips) {
            ctx_->GenerateMips(srv);
        }
        if (SUCCEEDED(ctx_->Map(scale_cbuf_, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
            float *f = (float *) map.pData;
            f[0] = (float) (std::max)(1, vw >> lod);
            f[1] = (float) (std::max)(1, vh >> lod);
            f[2] = (float) lod;
            f[3] = 0;
            ctx_->Unmap(scale_cbuf_, 0);
        }
        ctx_->VSSetConstantBuffers(0, 1, &cbuf_);
        ctx_->PSSetShader(ps_scale_, nullptr, 0);
        ctx_->PSSetConstantBuffers(2, 1, &scale_cbuf_);
        ctx_->PSSetShaderResources(0, 1, &srv);
        ctx_->Draw(4, 0);
        ID3D11ShaderResourceView *none = nullptr;
        ctx_->PSSetShaderResources(0, 1, &none);
    }
    ID3D11RenderTargetView *no_rtv = nullptr;
    ctx_->OMSetRenderTargets(1, &no_rtv, nullptr);

    // Smooth mode lines frames up with the display's refresh; otherwise show them the instant they're ready.
    HRESULT hr;
    if (!main) {
        hr = sc->Present(0, 0);
    } else if (smooth_.load()) {
        hr = sc->Present(1, 0);
    } else {
        hr = sc->Present(0, tearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0);
        if (hr == DXGI_ERROR_INVALID_CALL && tearing_) {
            tearing_ = false;
            hr = sc->Present(0, 0);
        }
    }
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        append_log("DXGI device lost during present");
    }
}

} // namespace airscreen
