#include "studio.h"
#include "settings.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <string>

namespace airscreen {
namespace {

const char *kVs = R"(
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD; };
VSOut main(float2 pos : POSITION, float2 uv : TEXCOORD) {
    VSOut o;
    o.pos = float4(pos, 0, 1);
    o.uv = uv;
    return o;
}
)";

// Every pixel shader shares this header: one constant block, one texture, one linear clamp sampler.
const char *kCommon = R"(
cbuffer P : register(b0) { float4 c[16]; };
Texture2D tex : register(t0);
SamplerState samp : register(s0);

// Catmull-Rom in 9 bilinear taps at one mip level.
float3 bicubic(float2 uv, float2 size, float lod) {
    float2 p = uv * size;
    float2 t1 = floor(p - 0.5) + 0.5;
    float2 f = p - t1;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2;
    float2 a = (t1 - 1.0) / size, b = (t1 + w2 / w12) / size, d = (t1 + 2.0) / size;
    float3 r = 0;
    r += tex.SampleLevel(samp, float2(a.x, a.y), lod).rgb * w0.x * w0.y;
    r += tex.SampleLevel(samp, float2(b.x, a.y), lod).rgb * w12.x * w0.y;
    r += tex.SampleLevel(samp, float2(d.x, a.y), lod).rgb * w3.x * w0.y;
    r += tex.SampleLevel(samp, float2(a.x, b.y), lod).rgb * w0.x * w12.y;
    r += tex.SampleLevel(samp, float2(b.x, b.y), lod).rgb * w12.x * w12.y;
    r += tex.SampleLevel(samp, float2(d.x, b.y), lod).rgb * w3.x * w12.y;
    r += tex.SampleLevel(samp, float2(a.x, d.y), lod).rgb * w0.x * w3.y;
    r += tex.SampleLevel(samp, float2(b.x, d.y), lod).rgb * w12.x * w3.y;
    r += tex.SampleLevel(samp, float2(d.x, d.y), lod).rgb * w3.x * w3.y;
    return saturate(r);
}

float hash(float2 p) { return frac(sin(dot(p, float2(12.9898, 78.233))) * 43758.5453); }

float sd_round(float2 p, float2 half_size, float r) {
    float2 q = abs(p) - half_size + r;
    return length(max(q, 0)) + min(max(q.x, q.y), 0) - r;
}
)";

// c0 top, c1 bottom, c2 bloom A (a = strength), c3 bloom B, c4 = (time, width, height). The same field as the
// idle window: a diagonal wash with two soft blooms drifting over it, dithered so it never bands.
const char *kPsBg = R"(
float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD) : SV_TARGET {
    float t = c[4].x, W = c[4].y, H = c[4].z;
    float k = saturate((0.342 * pos.x + 0.940 * pos.y) / (0.342 * W + 0.940 * H));
    float3 col = lerp(c[0].rgb, c[1].rgb, k);
    float2 a = float2((0.80 + 0.07 * sin(t * 0.33)) * W, (0.16 + 0.08 * cos(t * 0.27)) * H);
    float2 b = float2((0.28 + 0.08 * cos(t * 0.21)) * W, (0.02 + 0.07 * sin(t * 0.39)) * H);
    float da = dot(pos.xy - a, pos.xy - a) / (0.62 * W * 0.62 * W);
    float db = dot(pos.xy - b, pos.xy - b) / (0.46 * W * 0.46 * W);
    if (da < 1) col = lerp(col, c[2].rgb, (1 - da) * (1 - da) * c[2].a);
    if (db < 1) col = lerp(col, c[3].rgb, (1 - db) * (1 - db) * c[3].a);
    col += (hash(pos.xy + frac(t)) - 0.5) / 255.0;
    return float4(col, 1);
}
)";

// c0 = cover mapping (scale.xy, offset.xy), c1 = (lod, texel.x, texel.y, dim). A tiny mip of the picture,
// smoothed with four offset taps, stretched to cover the canvas and dimmed: ambient colour, no detail.
const char *kPsBlur = R"(
float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD) : SV_TARGET {
    float2 u = uv * c[0].xy + c[0].zw;
    float lod = c[1].x;
    float2 o = c[1].yz * 0.5;
    float3 col = tex.SampleLevel(samp, u + float2(-o.x, -o.y), lod).rgb + tex.SampleLevel(samp, u + float2(o.x, -o.y), lod).rgb +
                 tex.SampleLevel(samp, u + float2(-o.x, o.y), lod).rgb + tex.SampleLevel(samp, u + float2(o.x, o.y), lod).rgb;
    col *= 0.25;
    float l = dot(col, float3(0.2126, 0.7152, 0.0722));
    col = lerp(float3(l, l, l), col, 1.15) * c[1].w + 0.015;
    col += (hash(pos.xy) - 0.5) / 255.0;
    return float4(saturate(col), 1);
}
)";

// c0 = picture rect (x, y, w, h) in target pixels, c1 = (corner radius, shadow size, lod, opacity),
// c2 = (texture size at lod), c3 = (uv scale, uv offset) to crop a filled box. Inside the rounded rect: the
// picture, bicubic. Outside: a soft shadow below it.
const char *kPsCard = R"(
float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD) : SV_TARGET {
    float2 half_size = c[0].zw * 0.5;
    float2 ctr = c[0].xy + half_size;
    float r = c[1].x, s = c[1].y;
    float d = sd_round(pos.xy - ctr, half_size, r);
    if (d > 0.5) {
        if (s <= 0) discard;
        float ds = sd_round(pos.xy - ctr - float2(0, s * 0.3), half_size, r);
        float a = 1 - smoothstep(-s * 0.2, s, ds);
        return float4(0, 0, 0, a * a * 0.6 * c[1].w);
    }
    float2 p = (pos.xy - c[0].xy) / c[0].zw * c[3].xy + c[3].zw;
    float3 col = bicubic(p, c[2].xy, c[1].z);
    return float4(col, saturate(0.5 - d) * c[1].w);
}
)";

// c0 = rect, c1.x = opacity: a premultiplied texture (text, a logo) stretched over the rect.
const char *kPsRect = R"(
float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD) : SV_TARGET {
    float2 p = (pos.xy - c[0].xy) / c[0].zw;
    return tex.SampleLevel(samp, p, 0) * c[1].x;
}
)";

// c0 = rect, c1 = (radius, border px, mode, opacity), c2 = colour A, c3 = colour B, c4 = border colour,
// c5 = (lod, 1/width, 1/height, spread). Modes: 0 solid A, 1 gradient A to B (top to bottom), 2 frosted glass
// (what is behind, blurred, tinted by A), 3 border only. Colours are straight RGBA.
const char *kPsBox = R"(
float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD) : SV_TARGET {
    float2 hs = c[0].zw * 0.5;
    float d = sd_round(pos.xy - c[0].xy - hs, hs, c[1].x);
    float cover = saturate(0.5 - d);
    if (cover <= 0) discard;
    float mode = c[1].z;
    float4 fill = c[2];
    if (mode > 0.5 && mode < 1.5) {
        fill = lerp(c[2], c[3], saturate((pos.y - c[0].y) / c[0].w));
    } else if (mode > 1.5 && mode < 2.5) {
        float2 u = pos.xy * c[5].yz;
        float2 o = c[5].yz * c[5].w;
        float lod = c[5].x;
        float3 b = tex.SampleLevel(samp, u, lod).rgb * 0.2;
        b += (tex.SampleLevel(samp, u + float2(o.x, 0), lod).rgb + tex.SampleLevel(samp, u - float2(o.x, 0), lod).rgb +
              tex.SampleLevel(samp, u + float2(0, o.y), lod).rgb + tex.SampleLevel(samp, u - float2(0, o.y), lod).rgb) * 0.125;
        b += (tex.SampleLevel(samp, u + o, lod).rgb + tex.SampleLevel(samp, u - o, lod).rgb +
              tex.SampleLevel(samp, u + float2(o.x, -o.y), lod).rgb + tex.SampleLevel(samp, u + float2(-o.x, o.y), lod).rgb) * 0.075;
        fill = float4(lerp(b, c[2].rgb, c[2].a), 1);
    } else if (mode > 2.5) {
        fill = float4(c[4].rgb, 0);
    }
    float4 col = fill;
    if (c[1].y > 0) {
        float inner = saturate(0.5 - (d + c[1].y));
        col = lerp(c[4], fill, inner);
    }
    return float4(col.rgb, col.a * cover * c[1].w);
}
)";

// c0 = (dim): a backdrop picture.
const char *kPsImg = R"(
float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD) : SV_TARGET {
    return float4(tex.SampleLevel(samp, uv, 0).rgb * c[0].x, 1);
}
)";

// c0 = (alpha, zoom): the scene being left, easing out and drifting a touch closer as it goes.
const char *kPsFade = R"(
float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD) : SV_TARGET {
    float2 u = (uv - 0.5) / c[0].y + 0.5;
    return float4(tex.SampleLevel(samp, u, 0).rgb, c[0].x);
}
)";

const char *kPsY = R"(
float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD) : SV_TARGET {
    float3 rgb = tex.SampleLevel(samp, uv, 0).rgb;
    return float4(16.0 / 255.0 + dot(rgb, float3(0.2126, 0.7152, 0.0722)) * (219.0 / 255.0), 0, 0, 1);
}
)";

// Rendered at half size: each pixel's centre falls between four canvas texels, so one bilinear tap averages them.
const char *kPsUv = R"(
float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD) : SV_TARGET {
    float3 rgb = tex.SampleLevel(samp, uv, 0).rgb;
    float cb = dot(rgb, float3(-0.1146, -0.3854, 0.5));
    float cr = dot(rgb, float3(0.5, -0.4542, -0.0458));
    return float4(0.5 + cb * (224.0 / 255.0), 0.5 + cr * (224.0 / 255.0), 0, 1);
}
)";

struct Palette {
    float top[4], bottom[4], a[4], b[4];
};

// The idle window's moods: night behind screens, blue to begin, amber for a pause, green to close.
Palette palette(int mood) {
    auto P = [](int r, int g, int b, int a = 255) {
        return std::array<float, 4>{r / 255.f, g / 255.f, b / 255.f, a / 255.f};
    };
    std::array<float, 4> v[4];
    switch (mood) {
    case 2:
        v[0] = P(110, 68, 24), v[1] = P(42, 24, 10), v[2] = P(217, 151, 74, 153), v[3] = P(184, 106, 90, 115);
        break;
    case 3:
        v[0] = P(26, 80, 64), v[1] = P(8, 32, 26), v[2] = P(84, 178, 126, 133), v[3] = P(74, 134, 216, 92);
        break;
    case 1:
        v[0] = P(28, 50, 114), v[1] = P(11, 21, 54), v[2] = P(74, 134, 216, 158), v[3] = P(109, 91, 196, 128);
        break;
    default:
        // Behind a screen: deep night blue with faint blooms, so the screen is the brightest thing.
        v[0] = P(20, 28, 56), v[1] = P(6, 8, 18), v[2] = P(52, 92, 170, 110), v[3] = P(86, 70, 160, 80);
        break;
    }
    Palette p;
    memcpy(p.top, v[0].data(), sizeof(p.top));
    memcpy(p.bottom, v[1].data(), sizeof(p.bottom));
    memcpy(p.a, v[2].data(), sizeof(p.a));
    memcpy(p.b, v[3].data(), sizeof(p.b));
    return p;
}

template <typename T> void release(T *&p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

bool has_thai(const std::wstring &s) {
    for (wchar_t ch : s) {
        if (ch >= 0x0E00 && ch <= 0x0E7F) {
            return true;
        }
    }
    return false;
}

const wchar_t *pick_face(std::initializer_list<const wchar_t *> faces) {
    for (auto *name : faces) {
        Gdiplus::FontFamily fam(name);
        if (fam.GetLastStatus() == Gdiplus::Ok) {
            return name;
        }
    }
    return L"Segoe UI";
}

// FNV-1a over everything that changes a picture's pixels.
struct Hasher {
    uint64_t h = 1469598103934665603ull;
    Hasher &bytes(const void *p, size_t n) {
        auto *b = (const unsigned char *) p;
        for (size_t i = 0; i < n; ++i) {
            h = (h ^ b[i]) * 1099511628211ull;
        }
        return *this;
    }
    template <typename T> Hasher &operator<<(const T &v) { return bytes(&v, sizeof(v)); }
    Hasher &operator<<(const std::wstring &s) { return bytes(s.data(), s.size() * sizeof(wchar_t)) << s.size(); }
};

void argb(uint32_t c, float alpha, float *out) {
    out[0] = ((c >> 16) & 255) / 255.f;
    out[1] = ((c >> 8) & 255) / 255.f;
    out[2] = (c & 255) / 255.f;
    out[3] = alpha;
}

// The text drawn into a w*h picture: wrapped to the box, shrunk until it fits, with a soft shadow when it has
// no box behind it to keep it readable. GDI+ has no font fallback, so Thai gets a face that carries both scripts.
std::vector<uint32_t> raster_text(const Layer &l, int w, int h, int canvas_h) {
    std::vector<uint32_t> px((size_t) w * (size_t) h, 0);
    if (l.text.empty() || w <= 0 || h <= 0) {
        return px;
    }
    using namespace Gdiplus;
    Bitmap bmp(w, h, w * 4, PixelFormat32bppPARGB, (BYTE *) px.data());
    Graphics g(&bmp);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAlias);
    g.Clear(Color(0, 0, 0, 0));

    static const wchar_t *latin_bold = pick_face({L"Segoe UI Variable Display Semibold", L"Segoe UI Semibold"});
    static const wchar_t *latin_text = pick_face({L"Segoe UI Variable Text", L"Segoe UI"});
    static const wchar_t *thai_face = pick_face({L"Leelawadee UI", L"Tahoma"});
    bool thai = has_thai(l.text);
    const wchar_t *face = thai ? thai_face : (l.bold ? latin_bold : latin_text);
    int style = thai && l.bold ? FontStyleBold : FontStyleRegular;

    float pad = (std::min)({l.padding * (float) canvas_h, (float) w * 0.25f, (float) h * 0.25f});
    RectF area(pad, pad, (float) w - pad * 2, (float) h - pad * 2);
    StringFormat fmt(StringFormat::GenericTypographic());
    fmt.SetFormatFlags(fmt.GetFormatFlags() & ~StringFormatFlagsNoWrap);
    fmt.SetAlignment(l.align == 1 ? StringAlignmentCenter : (l.align == 2 ? StringAlignmentFar : StringAlignmentNear));
    fmt.SetLineAlignment(StringAlignmentCenter);
    fmt.SetTrimming(StringTrimmingNone);

    float em = (std::max)(4.f, l.font * (float) canvas_h * 0.8f);
    for (int i = 0; i < 30 && em > 5; ++i) {
        Font probe(face, em, style, UnitPixel);
        RectF box;
        g.MeasureString(l.text.c_str(), (INT) l.text.size(), &probe, RectF(0, 0, area.Width, 100000.f), &fmt, &box);
        if (box.Height <= area.Height + 0.5f) {
            break;
        }
        em *= 0.92f;
    }
    Font font(face, em, style, UnitPixel);
    if (!l.has_box()) {
        SolidBrush shadow(Color(110, 0, 0, 0));
        RectF rs = area;
        rs.Y += em * 0.05f;
        rs.X += em * 0.02f;
        g.DrawString(l.text.c_str(), (INT) l.text.size(), &font, rs, &fmt, &shadow);
    }
    SolidBrush brush(Color((BYTE) (l.color >> 24), (BYTE) (l.color >> 16), (BYTE) (l.color >> 8), (BYTE) l.color));
    g.DrawString(l.text.c_str(), (INT) l.text.size(), &font, area, &fmt, &brush);
    return px;
}

uint64_t text_key(const Layer &l, int pw, int ph, int canvas_h) {
    Hasher k;
    k << 'T' << l.text << l.font << l.color << l.bold << l.align << l.padding << l.has_box() << pw << ph << canvas_h;
    return k.h;
}

uint64_t picture_key(const std::wstring &path, int pw, int ph, bool cover) {
    Hasher k;
    k << (cover ? 'B' : 'I') << path << pw << ph;
    return k.h;
}

uint64_t label_key(SourceKind kind) {
    Hasher k;
    k << 'L' << (int) kind;
    return k.h;
}

constexpr int kLabelW = 900, kLabelH = 150;

void layer_pixels(const Layer &l, int cw, int ch, int &pw, int &ph) {
    pw = std::clamp((int) std::lround(l.w * (float) cw), 8, 4096);
    ph = std::clamp((int) std::lround(l.h * (float) ch), 8, 4096);
}

} // namespace

std::vector<uint32_t> Studio::load_picture(const std::wstring &path, int w, int h, bool cover) {
    using namespace Gdiplus;
    std::vector<uint32_t> px;
    if (path.empty() || w <= 0 || h <= 0) {
        return px;
    }
    Bitmap src(path.c_str());
    if (src.GetLastStatus() != Ok || src.GetWidth() == 0 || src.GetHeight() == 0) {
        return px;
    }
    float sw = (float) src.GetWidth(), sh = (float) src.GetHeight();
    px.assign((size_t) w * (size_t) h, cover ? 0xff000000u : 0u);
    Bitmap dst(w, h, w * 4, PixelFormat32bppPARGB, (BYTE *) px.data());
    Graphics g(&dst);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    ImageAttributes clamp;
    clamp.SetWrapMode(WrapModeTileFlipXY); // no dark fringe from sampling past the edge
    if (cover) {
        // Fill: scale to cover, crop the overflow evenly from both sides.
        float scale = (std::max)((float) w / sw, (float) h / sh);
        float cw = (float) w / scale, ch = (float) h / scale;
        g.SetCompositingMode(CompositingModeSourceCopy);
        g.DrawImage(&src, RectF(0, 0, (float) w, (float) h), (sw - cw) * 0.5f, (sh - ch) * 0.5f, cw, ch, UnitPixel,
                    &clamp);
        for (auto &p : px) {
            p |= 0xff000000u; // backdrops are opaque, whatever the file's alpha
        }
    } else {
        // Fit: the whole picture, centred, its own transparency kept (logos).
        float scale = (std::min)((float) w / sw, (float) h / sh);
        float dw = sw * scale, dh = sh * scale;
        g.DrawImage(&src, RectF(((float) w - dw) * 0.5f, ((float) h - dh) * 0.5f, dw, dh), 0, 0, sw, sh, UnitPixel,
                    &clamp);
    }
    return px;
}

std::vector<uint32_t> Studio::make_raster(const Need &need) {
    switch (need.kind) {
    case Need::Picture:
        return load_picture(need.path, need.w, need.h, false);
    case Need::Cover:
        return load_picture(need.path, need.w, need.h, true);
    default:
        return raster_text(need.layer, need.w, need.h, need.canvas_h);
    }
}

std::vector<Studio::Need> Studio::needs(const Scene &scene, bool preview) const {
    std::vector<Need> out;
    if (cw_ <= 0 || ch_ <= 0) {
        return out;
    }
    auto want = [&](Need n) {
        if (!cache_.count(n.key)) {
            for (auto &o : out) {
                if (o.key == n.key) {
                    return;
                }
            }
            out.push_back(std::move(n));
        }
    };
    if (scene.backdrop == Backdrop::Image && !scene.bg_image.empty()) {
        Need n;
        n.kind = Need::Cover;
        n.path = scene.bg_image;
        n.w = cw_;
        n.h = ch_;
        n.key = picture_key(n.path, cw_, ch_, true);
        want(n);
    }
    for (auto &l : scene.layers) {
        int pw, ph;
        layer_pixels(l, cw_, ch_, pw, ph);
        Need n;
        n.w = pw;
        n.h = ph;
        n.canvas_h = ch_;
        // Hidden text is drawn too, so showing it later is instant.
        if (l.type == LayerType::Text && !l.text.empty()) {
            n.kind = Need::Text;
            n.layer = l;
            n.key = text_key(l, pw, ph, ch_);
            want(n);
        } else if (l.type == LayerType::Image && !l.path.empty()) {
            n.kind = Need::Picture;
            n.path = l.path;
            n.key = picture_key(l.path, pw, ph, false);
            want(n);
        } else if (l.type == LayerType::Source && preview) {
            n.kind = Need::Label;
            n.layer = Layer();
            n.layer.text = l.source == SourceKind::IPad ? L"iPad · not connected" : L"Computer · not shared";
            n.layer.font = 0.42f;
            n.layer.align = 1;
            n.layer.bold = true;
            n.layer.box = BoxStyle::Solid; // no shadow
            n.layer.padding = 0;
            n.layer.color = 0xffb8bfcc;
            n.w = kLabelW;
            n.h = kLabelH;
            n.canvas_h = kLabelH;
            n.key = label_key(l.source);
            want(n);
        }
    }
    return out;
}

void Studio::put_raster(uint64_t key, const std::vector<uint32_t> &pixels, int w, int h) {
    if (!dev_ || pixels.size() != (size_t) w * (size_t) h || w <= 0 || h <= 0 || cache_.count(key)) {
        return;
    }
    if (cache_.size() >= 96) {
        // Forget the pictures drawn longest ago; a scene that needs one again simply has it redrawn.
        std::vector<std::pair<uint64_t, uint64_t>> age;
        for (auto &e : cache_) {
            age.push_back({e.second.used, e.first});
        }
        std::sort(age.begin(), age.end());
        for (size_t i = 0; i < 32 && i < age.size(); ++i) {
            auto &c = cache_[age[i].second];
            release(c.srv);
            release(c.tex);
            cache_.erase(age[i].second);
        }
    }
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = (UINT) w;
    td.Height = (UINT) h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA srd = {pixels.data(), (UINT) w * 4, 0};
    Cached c;
    c.w = w;
    c.h = h;
    c.used = frame_;
    if (SUCCEEDED(dev_->CreateTexture2D(&td, &srd, &c.tex)) &&
        SUCCEEDED(dev_->CreateShaderResourceView(c.tex, nullptr, &c.srv))) {
        cache_[key] = c;
    } else {
        release(c.tex);
    }
}

const Studio::Cached *Studio::cached(uint64_t key) {
    auto it = cache_.find(key);
    if (it == cache_.end()) {
        return nullptr;
    }
    it->second.used = frame_;
    return &it->second;
}

bool Studio::init(ID3D11Device *device, ID3D11DeviceContext *ctx) {
    dev_ = device;
    ctx_ = ctx;
    ID3DBlob *blob = nullptr, *err = nullptr;
    if (FAILED(D3DCompile(kVs, strlen(kVs), "studio_vs", nullptr, nullptr, "main", "vs_4_0", 0, 0, &blob, &err))) {
        release(err);
        return false;
    }
    dev_->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &vs_);
    D3D11_INPUT_ELEMENT_DESC ied[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    dev_->CreateInputLayout(ied, 2, blob->GetBufferPointer(), blob->GetBufferSize(), &layout_);
    release(blob);

    struct {
        const char *src;
        ID3D11PixelShader **out;
    } shaders[] = {{kPsBg, &ps_bg_},     {kPsBlur, &ps_blur_}, {kPsCard, &ps_card_}, {kPsRect, &ps_rect_},
                   {kPsBox, &ps_box_},   {kPsFade, &ps_fade_}, {kPsY, &ps_y_},       {kPsUv, &ps_uv_},
                   {kPsImg, &ps_img_}};
    for (auto &s : shaders) {
        std::string code = std::string(kCommon) + s.src;
        if (FAILED(D3DCompile(code.data(), code.size(), "studio_ps", nullptr, nullptr, "main", "ps_4_0",
                              D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &err))) {
            if (err) {
                append_log(("studio shader: " + std::string((const char *) err->GetBufferPointer())).c_str());
            }
            release(err);
            return false;
        }
        dev_->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, s.out);
        release(blob);
    }

    float quad[] = {-1, 1, 0, 0, 1, 1, 1, 0, -1, -1, 0, 1, 1, -1, 1, 1};
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(quad);
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA srd = {quad};
    dev_->CreateBuffer(&bd, &srd, &vb_);

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = 16 * 16;
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    dev_->CreateBuffer(&cbd, nullptr, &cb_);

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    dev_->CreateSamplerState(&sd, &sampler_);

    D3D11_BLEND_DESC bl = {};
    bl.RenderTarget[0].BlendEnable = TRUE;
    bl.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    dev_->CreateBlendState(&bl, &blend_alpha_);
    bl.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    dev_->CreateBlendState(&bl, &blend_premult_);

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.ScissorEnable = TRUE;
    dev_->CreateRasterizerState(&rd, &raster_);
    return vs_ && layout_ && vb_ && cb_ && sampler_ && blend_alpha_ && blend_premult_ && raster_;
}

void Studio::shutdown() {
    for (auto &e : cache_) {
        release(e.second.srv);
        release(e.second.tex);
    }
    cache_.clear();
    free_target(canvas_);
    free_target(preview_);
    release(snap_srv_);
    release(snap_);
    release(y_rtv_);
    release(y_);
    release(uv_rtv_);
    release(uv_);
    for (auto **ps : {&ps_bg_, &ps_blur_, &ps_card_, &ps_rect_, &ps_box_, &ps_fade_, &ps_y_, &ps_uv_, &ps_img_}) {
        release(*ps);
    }
    release(vs_);
    release(layout_);
    release(vb_);
    release(cb_);
    release(sampler_);
    release(blend_alpha_);
    release(blend_premult_);
    release(raster_);
    cw_ = ch_ = 0;
    rendered_ = false;
    dev_ = nullptr;
    ctx_ = nullptr;
}

bool Studio::make_texture(int w, int h, DXGI_FORMAT fmt, bool mips, bool rt, ID3D11Texture2D **tex,
                          ID3D11RenderTargetView **rtv, ID3D11ShaderResourceView **srv) {
    int levels = 1;
    if (mips) {
        for (int m = (std::max)(w, h); m > 1; m >>= 1) {
            ++levels;
        }
    }
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = (UINT) w;
    td.Height = (UINT) h;
    td.MipLevels = (UINT) levels;
    td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | (rt || mips ? D3D11_BIND_RENDER_TARGET : 0);
    td.MiscFlags = mips ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;
    if (FAILED(dev_->CreateTexture2D(&td, nullptr, tex))) {
        return false;
    }
    if (rtv) {
        D3D11_RENDER_TARGET_VIEW_DESC rvd = {};
        rvd.Format = fmt;
        rvd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        if (FAILED(dev_->CreateRenderTargetView(*tex, &rvd, rtv))) {
            return false;
        }
    }
    if (srv && FAILED(dev_->CreateShaderResourceView(*tex, nullptr, srv))) {
        return false;
    }
    return true;
}

bool Studio::make_target(Target &t, int w, int h) {
    free_target(t);
    const DXGI_FORMAT rgba = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (!make_texture(w, h, rgba, true, true, &t.tex, &t.rtv, &t.srv) ||
        !make_texture(w, h, rgba, true, true, &t.behind, nullptr, &t.behind_srv)) {
        free_target(t);
        return false;
    }
    t.w = w;
    t.h = h;
    return true;
}

void Studio::free_target(Target &t) {
    release(t.srv);
    release(t.rtv);
    release(t.tex);
    release(t.behind_srv);
    release(t.behind);
    t.w = t.h = 0;
}

bool Studio::set_canvas_size(int w, int h) {
    w &= ~1;
    h &= ~1;
    if (w == cw_ && h == ch_ && canvas_.tex) {
        return true;
    }
    free_target(canvas_);
    release(snap_srv_);
    release(snap_);
    release(y_rtv_);
    release(y_);
    release(uv_rtv_);
    release(uv_);
    for (auto &e : cache_) { // every text picture is drawn at canvas size
        release(e.second.srv);
        release(e.second.tex);
    }
    cache_.clear();
    cw_ = ch_ = 0;
    rendered_ = false;
    fading_ = false;
    if (!make_target(canvas_, w, h) ||
        !make_texture(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, true, true, &snap_, nullptr, &snap_srv_) ||
        !make_texture(w, h, DXGI_FORMAT_R8_UNORM, false, true, &y_, &y_rtv_, nullptr) ||
        !make_texture(w / 2, h / 2, DXGI_FORMAT_R8G8_UNORM, false, true, &uv_, &uv_rtv_, nullptr)) {
        append_log("studio: could not create the canvas");
        return false;
    }
    canvas_mips_ = 1;
    for (int m = (std::max)(w, h); m > 1; m >>= 1) {
        ++canvas_mips_;
    }
    cw_ = w;
    ch_ = h;
    return true;
}

bool Studio::set_preview_size(int w, int h) {
    if (w == preview_.w && h == preview_.h && preview_.tex) {
        return true;
    }
    if (w <= 0 || h <= 0) {
        free_target(preview_);
        return true;
    }
    if (!make_target(preview_, w, h)) {
        return false;
    }
    preview_mips_ = 1;
    for (int m = (std::max)(w, h); m > 1; m >>= 1) {
        ++preview_mips_;
    }
    return true;
}

void Studio::set_scene(const Scene &scene, double now_s, double fade_s) {
    if (fade_s > 0 && rendered_ && snap_ && canvas_.tex) {
        ctx_->CopyResource(snap_, canvas_.tex); // fade from exactly what was last shown, mid-fade included
        fading_ = true;
        fade_start_ = now_s;
        fade_len_ = fade_s;
    }
    live_ = scene;
}

bool Studio::animating(double now_s) const {
    if (fading_ && now_s - fade_start_ < fade_len_ + 0.05) {
        return true;
    }
    if (live_.backdrop != Backdrop::Colors) {
        return false;
    }
    for (auto &l : live_.layers) {
        if (l.type == LayerType::Source && l.visible) {
            return false; // behind a screen the field holds still: only new video needs a new frame
        }
    }
    return true; // a title card's drifting background
}

void Studio::set_cb(const float *data, size_t floats) {
    D3D11_MAPPED_SUBRESOURCE map = {};
    if (SUCCEEDED(ctx_->Map(cb_, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        memset(map.pData, 0, 16 * 16);
        memcpy(map.pData, data, (std::min)(floats, (size_t) 64) * sizeof(float));
        ctx_->Unmap(cb_, 0);
    }
}

void Studio::viewport(int w, int h) {
    D3D11_VIEWPORT vp = {};
    vp.Width = (float) w;
    vp.Height = (float) h;
    vp.MaxDepth = 1;
    ctx_->RSSetViewports(1, &vp);
    D3D11_RECT r = {0, 0, w, h};
    ctx_->RSSetScissorRects(1, &r);
}

void Studio::scissor(float x0, float y0, float x1, float y1) {
    D3D11_RECT r = {(LONG) std::floor(x0), (LONG) std::floor(y0), (LONG) std::ceil(x1), (LONG) std::ceil(y1)};
    ctx_->RSSetScissorRects(1, &r);
}

void Studio::draw(ID3D11PixelShader *ps, ID3D11ShaderResourceView *srv, bool blend, bool premult) {
    UINT stride = 16, offset = 0;
    ctx_->IASetInputLayout(layout_);
    ctx_->IASetVertexBuffers(0, 1, &vb_, &stride, &offset);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx_->VSSetShader(vs_, nullptr, 0);
    ctx_->PSSetShader(ps, nullptr, 0);
    ctx_->PSSetConstantBuffers(0, 1, &cb_);
    ctx_->PSSetSamplers(0, 1, &sampler_);
    ctx_->PSSetShaderResources(0, 1, &srv);
    ctx_->RSSetState(raster_);
    float factor[4] = {};
    ctx_->OMSetBlendState(blend ? (premult ? blend_premult_ : blend_alpha_) : nullptr, factor, 0xffffffff);
    ctx_->Draw(4, 0);
    ID3D11ShaderResourceView *none = nullptr;
    ctx_->PSSetShaderResources(0, 1, &none);
    ctx_->OMSetBlendState(nullptr, factor, 0xffffffff);
}

void Studio::draw_texture(const Cached &c, float x, float y, float w, float h, float opacity) {
    float k[8] = {x, y, w, h, opacity, 0, 0, 0};
    set_cb(k, 8);
    scissor(x, y, x + w, y + h);
    draw(ps_rect_, c.srv, true, true);
}

// A text layer's box or a frame: fill per the layer's box style, plus (frames, outlines) a border.
void Studio::draw_box(const Layer &l, Target &t, float x, float y, float w, float h, bool is_text) {
    const float H = (float) t.h;
    float k[24] = {x, y, w, h, l.radius * H, 0, 0, l.opacity};
    int mode = 3;
    if (l.has_box()) {
        switch (l.box) {
        case BoxStyle::Solid:
            mode = 0;
            argb(l.box_color, 1.f, k + 8);
            break;
        case BoxStyle::Translucent:
            mode = 0;
            argb(l.box_color, l.box_opacity, k + 8);
            break;
        case BoxStyle::Gradient:
            mode = 1;
            argb(l.box_color, l.box_opacity, k + 8);
            argb(l.box_color2, l.box_opacity, k + 12);
            break;
        case BoxStyle::Blur:
            mode = 2;
            argb(l.box_color, l.box_opacity * 0.6f, k + 8);
            break;
        default: // Outline
            mode = 3;
            break;
        }
    }
    if (is_text) {
        if (mode == 3) {
            if (!l.has_box()) {
                return;
            }
            k[5] = (std::max)(1.f, H * 0.0025f); // outline: a hairline in the box colour
            argb(l.box_color, (std::max)(l.box_opacity, 0.35f), k + 16);
        }
    } else {
        k[5] = l.thickness * H; // a frame's border
        argb(l.color, (float) (l.color >> 24) / 255.f, k + 16);
        if (mode == 3 && k[5] <= 0) {
            return;
        }
    }
    k[6] = (float) mode;
    ID3D11ShaderResourceView *srv = nullptr;
    if (mode == 2) {
        // Frosted glass: blur whatever has been drawn so far, only inside the box.
        ID3D11RenderTargetView *none = nullptr;
        ctx_->OMSetRenderTargets(1, &none, nullptr);
        ctx_->CopyResource(t.behind, t.tex);
        ctx_->GenerateMips(t.behind_srv);
        ctx_->OMSetRenderTargets(1, &t.rtv, nullptr);
        float lod = 1.f + l.blur * 4.f;
        k[20] = lod;
        k[21] = 1.f / (float) t.w;
        k[22] = 1.f / (float) t.h;
        k[23] = std::exp2(lod) * 1.2f;
        srv = t.behind_srv;
    }
    set_cb(k, 24);
    scissor(x - 1, y - 1, x + w + 1, y + h + 1);
    draw(ps_box_, srv, true, false);
}

void Studio::draw_scene(const Scene &scene, const Sources &src, Target &t, double now_s, bool preview) {
    const float W = (float) t.w, H = (float) t.h;
    auto full = [&] { scissor(0, 0, W, H); };
    auto field = [&](int mood, bool still) {
        Palette p = palette(mood);
        float c[20];
        memcpy(c, p.top, 16);
        memcpy(c + 4, p.bottom, 16);
        memcpy(c + 8, p.a, 16);
        memcpy(c + 12, p.b, 16);
        c[16] = still ? 0.f : (float) now_s;
        c[17] = W;
        c[18] = H;
        c[19] = 0;
        set_cb(c, 20);
        draw(ps_bg_, nullptr, false, false);
    };

    // 1. The backdrop.
    const Layer *main_screen = nullptr;
    for (auto &l : scene.layers) {
        if (l.type == LayerType::Source && l.visible && !main_screen) {
            main_screen = &l;
        }
    }
    full();
    switch (scene.backdrop) {
    case Backdrop::Solid: {
        float col[4];
        argb(scene.bg_color, 1, col);
        ctx_->ClearRenderTargetView(t.rtv, col);
        break;
    }
    case Backdrop::Image: {
        const Cached *c = scene.bg_image.empty() ? nullptr : cached(picture_key(scene.bg_image, cw_, ch_, true));
        if (c) {
            float k[4] = {1, 0, 0, 0};
            set_cb(k, 4);
            draw(ps_img_, c->srv, false, false);
        } else {
            field(scene.mood, true);
        }
        break;
    }
    case Backdrop::Blur: {
        const Source *s = main_screen ? &src.get(main_screen->source) : nullptr;
        if (s && s->srv && s->w > 0 && s->h > 0) {
            // Ambient backdrop: the main screen itself, blurred down to colour and dimmed, covering the canvas.
            float sa = (float) s->w / (float) s->h, ca = W / H;
            float sx = 1, sy = 1;
            if (sa > ca) {
                sx = ca / sa;
            } else {
                sy = sa / ca;
            }
            sx /= 1.15f; // zoom in a little so the softened edges stay off the canvas
            sy /= 1.15f;
            int lod = std::clamp((int) std::floor(std::log2((float) s->w / 28.f)), 0, s->mips - 1);
            float tw = (float) (std::max)(1, s->w >> lod), th = (float) (std::max)(1, s->h >> lod);
            float c[8] = {sx, sy, (1 - sx) * 0.5f, (1 - sy) * 0.5f, (float) lod, 1.f / tw, 1.f / th, 0.32f};
            set_cb(c, 8);
            draw(ps_blur_, s->srv, false, false);
        } else {
            field(scene.mood, true);
        }
        break;
    }
    default:
        field(scene.mood, main_screen != nullptr || preview);
        break;
    }

    // 2. The layers, back to front.
    for (auto &l : scene.layers) {
        if (!l.visible || l.opacity <= 0.001f) {
            continue;
        }
        float lx = l.x * W, ly = l.y * H, lw = l.w * W, lh = l.h * H;
        switch (l.type) {
        case LayerType::Source: {
            const Source &s = src.get(l.source);
            if (!s.srv || s.w <= 0 || s.h <= 0) {
                if (preview) {
                    // A stand-in so the layout can be arranged before anything is connected.
                    Layer ph;
                    ph.box = BoxStyle::Translucent;
                    ph.box_color = 0xff262b36;
                    ph.box_opacity = 0.92f;
                    ph.radius = l.rounded ? (std::min)(l.w * W, l.h * H) * 0.035f / H : 0;
                    ph.thickness = 1.5f / H;
                    ph.color = 0xff5a6273;
                    draw_box(ph, t, lx, ly, lw, lh, false);
                    if (const Cached *c = cached(label_key(l.source))) {
                        float bw = (std::min)(lw * 0.8f, lh * 0.8f * (float) kLabelW / (float) kLabelH);
                        float bh = bw * (float) kLabelH / (float) kLabelW;
                        draw_texture(*c, lx + (lw - bw) / 2, ly + (lh - bh) / 2, bw, bh, 1.f);
                    }
                }
                break;
            }
            float sa = (float) s.w / (float) s.h;
            float rx = lx, ry = ly, rw = lw, rh = lh;
            float us = 1, vs = 1, uo = 0, vo = 0;
            if (l.fill) {
                float la = lw / lh;
                if (sa > la) {
                    us = la / sa;
                    uo = (1 - us) * 0.5f;
                } else {
                    vs = sa / la;
                    vo = (1 - vs) * 0.5f;
                }
            } else {
                rw = lw;
                rh = lw / sa;
                if (rh > lh) {
                    rh = lh;
                    rw = lh * sa;
                }
                rx = std::floor(lx + (lw - rw) * 0.5f);
                ry = std::floor(ly + (lh - rh) * 0.5f);
            }
            float scale = (std::min)(rw / ((float) s.w * us), rh / ((float) s.h * vs));
            int lod = scale < 1.f ? (std::min)((int) std::floor(std::log2(1.f / scale)), s.mips - 1) : 0;
            float radius = l.rounded ? (std::min)(rw, rh) * 0.035f : 0.f;
            float shadow = l.rounded ? H * 0.035f : 0.f;
            float c[16] = {rx, ry, rw, rh, radius, shadow, (float) lod, l.opacity,
                           (float) (std::max)(1, s.w >> lod), (float) (std::max)(1, s.h >> lod), 0, 0,
                           us, vs, uo, vo};
            set_cb(c, 16);
            scissor(rx - shadow * 1.5f, ry - shadow * 1.5f, rx + rw + shadow * 1.5f, ry + rh + shadow * 1.5f);
            draw(ps_card_, s.srv, true, false);
            break;
        }
        case LayerType::Text: {
            draw_box(l, t, lx, ly, lw, lh, true);
            int pw, ph;
            layer_pixels(l, cw_, ch_, pw, ph);
            if (const Cached *c = l.text.empty() ? nullptr : cached(text_key(l, pw, ph, ch_))) {
                draw_texture(*c, lx, ly, lw, lh, l.opacity);
            }
            break;
        }
        case LayerType::Image: {
            int pw, ph;
            layer_pixels(l, cw_, ch_, pw, ph);
            if (const Cached *c = l.path.empty() ? nullptr : cached(picture_key(l.path, pw, ph, false))) {
                draw_texture(*c, lx, ly, lw, lh, l.opacity);
            }
            break;
        }
        case LayerType::Frame:
            draw_box(l, t, lx, ly, lw, lh, false);
            break;
        }
    }
    full();
}

void Studio::draw_marks(const std::vector<Mark> &marks, const Target &t) {
    const float W = (float) t.w, H = (float) t.h;
    for (auto &m : marks) {
        float x = m.x * W - m.grow_px, y = m.y * H - m.grow_px;
        float w = m.w * W + m.grow_px * 2, h = m.h * H + m.grow_px * 2;
        float k[20] = {x, y, w, h, 0, m.border_px, m.border_px > 0 ? 3.f : 0.f, 1};
        argb(m.color, (float) (m.color >> 24) / 255.f, k + 8);
        argb(m.color, (float) (m.color >> 24) / 255.f, k + 16);
        set_cb(k, 20);
        scissor(x - 1, y - 1, x + w + 1, y + h + 1);
        draw(ps_box_, nullptr, true, false);
    }
    scissor(0, 0, W, H);
}

void Studio::render(const Sources &src, double now_s) {
    if (!canvas_.tex) {
        return;
    }
    ++frame_;
    viewport(cw_, ch_);
    ctx_->OMSetRenderTargets(1, &canvas_.rtv, nullptr);
    draw_scene(live_, src, canvas_, now_s, false);
    if (fading_) {
        double p = (now_s - fade_start_) / fade_len_;
        if (p >= 1) {
            fading_ = false;
        } else {
            float e = (float) (p * p * (3 - 2 * p)); // ease in-out
            float c[4] = {1.f - e, 1.f + 0.035f * e * (float) (fade_len_ / 0.5), 0, 0};
            set_cb(c, 4);
            draw(ps_fade_, snap_srv_, true, false);
        }
    }
    ID3D11RenderTargetView *none = nullptr;
    ctx_->OMSetRenderTargets(1, &none, nullptr);
    rendered_ = true;
}

void Studio::render_preview(const Scene &scene, const Sources &src, const std::vector<Mark> &marks, double now_s) {
    if (!preview_.tex || cw_ <= 0) {
        return;
    }
    ++frame_;
    viewport(preview_.w, preview_.h);
    ctx_->OMSetRenderTargets(1, &preview_.rtv, nullptr);
    draw_scene(scene, src, preview_, now_s, true);
    draw_marks(marks, preview_);
    ID3D11RenderTargetView *none = nullptr;
    ctx_->OMSetRenderTargets(1, &none, nullptr);
}

bool Studio::export_planes(ID3D11Texture2D **y, ID3D11Texture2D **uv) {
    if (!canvas_.tex || !y_ || !uv_) {
        return false;
    }
    float c[4] = {};
    set_cb(c, 4);
    viewport(cw_, ch_);
    ctx_->OMSetRenderTargets(1, &y_rtv_, nullptr);
    draw(ps_y_, canvas_.srv, false, false);
    viewport(cw_ / 2, ch_ / 2);
    ctx_->OMSetRenderTargets(1, &uv_rtv_, nullptr);
    draw(ps_uv_, canvas_.srv, false, false);
    ID3D11RenderTargetView *none = nullptr;
    ctx_->OMSetRenderTargets(1, &none, nullptr);
    *y = y_;
    *uv = uv_;
    return true;
}

} // namespace airscreen
