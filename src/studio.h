#pragma once

#include "scene.h"

#include <d3d11.h>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace airscreen {

// The studio composes what the window shows and what a clip records: a 16:9 canvas holding the live scene
// (see scene.h), cross-faded when it changes. It can also draw any scene into a smaller preview target for the
// editor, without touching the live canvas.
//
// Text and pictures are drawn into textures by GDI+ on the UI thread (needs() then make_raster(), off the D3D
// lock) and cached by content, so switching scenes or showing a caption again costs nothing. Boxes, blur,
// frames, the screens and fades are all drawn on the GPU.
//
// It only draws; VideoPipeline owns the device, decides when to render, and hands the canvas to the screen and
// the recorder. All methods run with the pipeline's D3D11 lock held, except the static ones.
class Studio {
public:
    struct Source {
        ID3D11ShaderResourceView *srv = nullptr; // null: not connected
        int w = 0, h = 0, mips = 1;
    };
    struct Sources {
        Source ipad, computer;
        const Source &get(SourceKind k) const { return k == SourceKind::Computer ? computer : ipad; }
    };
    // Editor guides drawn over a preview: a rectangle (or, with zero size, a point) in canvas fractions,
    // filled or outlined, grown by a fixed number of preview pixels so handles and hairlines stay crisp.
    struct Mark {
        float x = 0, y = 0, w = 0, h = 0;
        uint32_t color = 0xff0a84ff;
        float border_px = 0; // 0 filled
        float grow_px = 0;
    };
    // A picture a scene needs: what to draw, at what size, under which cache key.
    struct Need {
        enum Kind { Text, Picture, Cover, Label } kind = Text;
        uint64_t key = 0;
        Layer layer;       // Text, Label
        std::wstring path; // Picture, Cover
        int w = 0, h = 0, canvas_h = 0;
    };

    static std::vector<uint32_t> make_raster(const Need &need);
    // Loads a picture file (PNG, JPEG, BMP, GIF, TIFF) scaled to w*h: `cover` crops to fill it (opaque),
    // otherwise it fits inside with transparent margins. Empty on failure.
    static std::vector<uint32_t> load_picture(const std::wstring &path, int w, int h, bool cover);

    bool init(ID3D11Device *device, ID3D11DeviceContext *ctx);
    void shutdown();

    bool set_canvas_size(int w, int h);
    int canvas_w() const { return cw_; }
    int canvas_h() const { return ch_; }

    // Pictures `scene` needs (at canvas size) that aren't cached yet; `preview` adds the placeholder labels.
    std::vector<Need> needs(const Scene &scene, bool preview) const;
    void put_raster(uint64_t key, const std::vector<uint32_t> &pixels, int w, int h);

    // Makes `scene` live; `fade_s` > 0 cross-fades from what is on the canvas now, 0 cuts.
    void set_scene(const Scene &scene, double now_s, double fade_s);
    const Scene &scene() const { return live_; }
    // Nothing to fade from: the next frame starts clean.
    void reset() {
        fading_ = false;
        rendered_ = false;
    }

    // True while something on the canvas moves on its own (a fade, a title card's drifting background), so the
    // caller keeps rendering at the display rate; otherwise only new video needs a new frame.
    bool animating(double now_s) const;

    // Draws the live scene on the canvas.
    void render(const Sources &src, double now_s);
    ID3D11ShaderResourceView *canvas_srv() const { return canvas_.srv; }
    int canvas_mips() const { return canvas_mips_; }
    // Converts the canvas to BT.709 limited-range NV12 planes for the encoder: Y at full size, interleaved
    // CbCr at half size.
    bool export_planes(ID3D11Texture2D **y, ID3D11Texture2D **uv);

    // Draws any scene into the preview target (placeholders for screens that aren't connected).
    bool set_preview_size(int w, int h);
    void render_preview(const Scene &scene, const Sources &src, const std::vector<Mark> &marks, double now_s);
    ID3D11ShaderResourceView *preview_srv() const { return preview_.srv; }
    int preview_w() const { return preview_.w; }
    int preview_h() const { return preview_.h; }
    int preview_mips() const { return preview_mips_; }

private:
    struct Target {
        ID3D11Texture2D *tex = nullptr;
        ID3D11RenderTargetView *rtv = nullptr;
        ID3D11ShaderResourceView *srv = nullptr;
        ID3D11Texture2D *behind = nullptr; // copy of what is drawn so far, for frosted boxes
        ID3D11ShaderResourceView *behind_srv = nullptr;
        int w = 0, h = 0;
    };
    struct Cached {
        ID3D11Texture2D *tex = nullptr;
        ID3D11ShaderResourceView *srv = nullptr;
        int w = 0, h = 0;
        uint64_t used = 0;
    };

    bool make_texture(int w, int h, DXGI_FORMAT fmt, bool mips, bool rt, ID3D11Texture2D **tex,
                      ID3D11RenderTargetView **rtv, ID3D11ShaderResourceView **srv);
    bool make_target(Target &t, int w, int h);
    void free_target(Target &t);
    void set_cb(const float *data, size_t floats);
    void draw(ID3D11PixelShader *ps, ID3D11ShaderResourceView *srv, bool blend, bool premult);
    void viewport(int w, int h);
    void scissor(float x0, float y0, float x1, float y1);
    const Cached *cached(uint64_t key);
    void draw_scene(const Scene &scene, const Sources &src, Target &t, double now_s, bool preview);
    void draw_box(const Layer &l, Target &t, float x, float y, float w, float h, bool fill_only);
    void draw_texture(const Cached &c, float x, float y, float w, float h, float opacity);
    void draw_marks(const std::vector<Mark> &marks, const Target &t);

    ID3D11Device *dev_ = nullptr;
    ID3D11DeviceContext *ctx_ = nullptr;

    ID3D11VertexShader *vs_ = nullptr;
    ID3D11InputLayout *layout_ = nullptr;
    ID3D11Buffer *vb_ = nullptr;
    ID3D11Buffer *cb_ = nullptr; // one 64-float constant buffer, rewritten per pass
    ID3D11SamplerState *sampler_ = nullptr;
    ID3D11BlendState *blend_alpha_ = nullptr;
    ID3D11BlendState *blend_premult_ = nullptr;
    ID3D11RasterizerState *raster_ = nullptr; // scissor on: each layer only touches its own pixels

    ID3D11PixelShader *ps_bg_ = nullptr;   // drifting gradient field
    ID3D11PixelShader *ps_blur_ = nullptr; // blurred, dimmed cover of a screen
    ID3D11PixelShader *ps_card_ = nullptr; // a screen with rounded corners and a soft shadow
    ID3D11PixelShader *ps_rect_ = nullptr; // premultiplied texture in a rectangle (text, pictures)
    ID3D11PixelShader *ps_box_ = nullptr;  // rounded box: solid, gradient, frosted or outline, with a border
    ID3D11PixelShader *ps_img_ = nullptr;  // backdrop picture
    ID3D11PixelShader *ps_fade_ = nullptr; // previous scene fading out over the new one
    ID3D11PixelShader *ps_y_ = nullptr;    // RGB -> Y
    ID3D11PixelShader *ps_uv_ = nullptr;   // RGB -> CbCr (2x2 average)

    int cw_ = 0, ch_ = 0;
    int canvas_mips_ = 1;
    Target canvas_;
    ID3D11Texture2D *snap_ = nullptr; // canvas as it was when the current fade began
    ID3D11ShaderResourceView *snap_srv_ = nullptr;
    ID3D11Texture2D *y_ = nullptr;
    ID3D11RenderTargetView *y_rtv_ = nullptr;
    ID3D11Texture2D *uv_ = nullptr;
    ID3D11RenderTargetView *uv_rtv_ = nullptr;
    Target preview_;
    int preview_mips_ = 1;

    std::unordered_map<uint64_t, Cached> cache_;
    uint64_t frame_ = 0;

    Scene live_;
    bool fading_ = false;
    bool rendered_ = false; // the canvas holds a finished frame (something to fade from)
    double fade_start_ = 0;
    double fade_len_ = 0.5;
};

} // namespace airscreen
