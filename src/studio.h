#pragma once

#include <d3d11.h>
#include <cstdint>
#include <string>
#include <vector>

namespace airscreen {

// The studio composes what the window shows and what a clip records: a fixed 16:9 canvas holding one of a few
// scenes, cross-faded when you switch between them.
//
//   Intro   title card on an animated background ("starting soon")
//   Screen  the iPad/iPhone picture, framed with rounded corners over a blurred copy of itself
//   Break   "be right back" card
//   Outro   closing card
//
// It only draws; VideoPipeline owns the device, decides when to render, and hands the canvas to the screen and
// the recorder. All methods run with the pipeline's D3D11 lock held, except build_overlay(), which is pure CPU.
class Studio {
public:
    enum Scene { Intro = 0, Screen = 1, Break = 2, Outro = 3, kSceneCount = 4 };

    struct Text {
        std::wstring title;
        std::wstring subtitle;
    };

    // GDI+ render of a title card's text: premultiplied BGRA, top-down, w*h pixels.
    static std::vector<uint32_t> build_overlay(Scene scene, const Text &text, int w, int h);
    // Loads a picture file (PNG, JPEG, BMP, GIF, TIFF) scaled and centre-cropped to fill w*h; empty on failure.
    static std::vector<uint32_t> load_cover(const std::wstring &path, int w, int h);

    bool init(ID3D11Device *device, ID3D11DeviceContext *ctx);
    void shutdown();

    bool set_canvas_size(int w, int h);
    int canvas_w() const { return cw_; }
    int canvas_h() const { return ch_; }
    void set_overlay(Scene scene, const std::vector<uint32_t> &pixels, int w, int h);
    void set_framed(bool framed) { framed_ = framed; }

    // What sits behind the scenes. Colors: a drifting gradient per scene. Blur: the Screen scene uses a blurred
    // copy of the picture (title cards keep their colours). Image: your own picture behind every scene, dimmed
    // under title text so it stays readable.
    enum Background { Colors = 0, Blur = 1, Image = 2 };
    void set_background(Background bg) { background_ = bg; }
    // Opaque BGRA at canvas size (already cropped to fill it), or empty to drop the image.
    void set_background_image(const std::vector<uint32_t> &pixels, int w, int h);
    // Switches scene; `now_s` starts the cross-fade from what is on the canvas at that moment.
    void set_scene(Scene scene, double now_s);
    Scene scene() const { return scene_; }
    // Back to the Screen scene with nothing to fade from: a new session starts clean.
    void reset() {
        scene_ = Screen;
        fading_ = false;
        rendered_ = false;
    }

    // True while something on the canvas moves on its own (a fade, a title card's drifting background), so the
    // caller keeps rendering at the display rate; otherwise only new video needs a new frame.
    bool animating(double now_s) const;

    // Draws the canvas. `src` is the latest picture (RGB with mips), or null before any has arrived.
    void render(ID3D11ShaderResourceView *src, int src_w, int src_h, int src_mips, double now_s);

    ID3D11ShaderResourceView *canvas_srv() const { return canvas_srv_; }
    int canvas_mips() const { return canvas_mips_; }
    // Converts the canvas to BT.709 limited-range NV12 planes for the encoder: Y at full size, interleaved
    // CbCr at half size.
    bool export_planes(ID3D11Texture2D **y, ID3D11Texture2D **uv);

private:
    bool make_target(int w, int h, DXGI_FORMAT fmt, bool mips, ID3D11Texture2D **tex, ID3D11RenderTargetView **rtv,
                     ID3D11ShaderResourceView **srv);
    void set_cb(ID3D11Buffer *cb, const float *data, size_t floats);
    void draw(ID3D11PixelShader *ps, ID3D11ShaderResourceView *srv, ID3D11Buffer *cb, bool blend, bool premult);
    void viewport(int w, int h);
    void draw_scene(Scene scene, ID3D11ShaderResourceView *src, int src_w, int src_h, int src_mips, double now_s);

    ID3D11Device *dev_ = nullptr;
    ID3D11DeviceContext *ctx_ = nullptr;

    ID3D11VertexShader *vs_ = nullptr;
    ID3D11InputLayout *layout_ = nullptr;
    ID3D11Buffer *vb_ = nullptr;
    ID3D11Buffer *cb_ = nullptr; // one 64-float constant buffer, rewritten per pass
    ID3D11SamplerState *sampler_ = nullptr;
    ID3D11BlendState *blend_alpha_ = nullptr;
    ID3D11BlendState *blend_premult_ = nullptr;
    ID3D11RasterizerState *raster_ = nullptr;

    ID3D11PixelShader *ps_bg_ = nullptr;     // drifting gradient field
    ID3D11PixelShader *ps_blur_ = nullptr;   // blurred, dimmed cover of the picture
    ID3D11PixelShader *ps_card_ = nullptr;   // picture with rounded corners and a soft shadow
    ID3D11PixelShader *ps_tex_ = nullptr;    // premultiplied texture (title text)
    ID3D11PixelShader *ps_img_ = nullptr;    // background picture, dimmed
    ID3D11PixelShader *ps_fade_ = nullptr;   // previous scene fading out over the new one
    ID3D11PixelShader *ps_y_ = nullptr;      // RGB -> Y
    ID3D11PixelShader *ps_uv_ = nullptr;     // RGB -> CbCr (2x2 average)

    int cw_ = 0, ch_ = 0;
    int canvas_mips_ = 1;
    ID3D11Texture2D *canvas_ = nullptr;
    ID3D11RenderTargetView *canvas_rtv_ = nullptr;
    ID3D11ShaderResourceView *canvas_srv_ = nullptr;
    ID3D11Texture2D *snap_ = nullptr; // canvas as it was when the current fade began
    ID3D11ShaderResourceView *snap_srv_ = nullptr;
    ID3D11Texture2D *y_ = nullptr;
    ID3D11RenderTargetView *y_rtv_ = nullptr;
    ID3D11Texture2D *uv_ = nullptr;
    ID3D11RenderTargetView *uv_rtv_ = nullptr;

    ID3D11Texture2D *overlay_[kSceneCount] = {};
    ID3D11ShaderResourceView *overlay_srv_[kSceneCount] = {};

    ID3D11Texture2D *bg_ = nullptr;
    ID3D11ShaderResourceView *bg_srv_ = nullptr;

    Scene scene_ = Screen;
    bool framed_ = true;
    Background background_ = Colors;
    bool fading_ = false;
    bool rendered_ = false; // the canvas holds a finished frame (something to fade from)
    double fade_start_ = 0;
};

} // namespace airscreen
