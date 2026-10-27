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

constexpr double kFadeSeconds = 0.5;

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

// c0 = picture rect (x, y, w, h) in canvas pixels, c1 = (corner radius, shadow size, lod, 0),
// c2 = (texture size at lod). Inside the rounded rect: the picture, bicubic. Outside: a soft shadow below it.
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
        return float4(0, 0, 0, a * a * 0.6);
    }
    float2 p = (pos.xy - c[0].xy) / c[0].zw;
    float3 col = bicubic(p, c[2].xy, c[1].z);
    return float4(col, saturate(0.5 - d));
}
)";

const char *kPsTex = R"(
float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD) : SV_TARGET {
    return tex.SampleLevel(samp, uv, 0);
}
)";

// c0 = (dim): your background picture, darkened so whatever sits on it reads.
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

// Same moods as the idle window: blue to begin, amber for a pause, green to close.
Palette palette(Studio::Scene s) {
    auto P = [](int r, int g, int b, int a = 255) {
        return std::array<float, 4>{r / 255.f, g / 255.f, b / 255.f, a / 255.f};
    };
    std::array<float, 4> v[4];
    switch (s) {
    case Studio::Break:
        v[0] = P(110, 68, 24), v[1] = P(42, 24, 10), v[2] = P(217, 151, 74, 153), v[3] = P(184, 106, 90, 115);
        break;
    case Studio::Outro:
        v[0] = P(26, 80, 64), v[1] = P(8, 32, 26), v[2] = P(84, 178, 126, 133), v[3] = P(74, 134, 216, 92);
        break;
    case Studio::Screen:
        // Behind the picture: deep night blue with faint blooms, so the screen is the brightest thing.
        v[0] = P(20, 28, 56), v[1] = P(6, 8, 18), v[2] = P(52, 92, 170, 110), v[3] = P(86, 70, 160, 80);
        break;
    default:
        v[0] = P(28, 50, 114), v[1] = P(11, 21, 54), v[2] = P(74, 134, 216, 158), v[3] = P(109, 91, 196, 128);
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

} // namespace

std::vector<uint32_t> Studio::build_overlay(Scene scene, const Text &text, int w, int h) {
    if (w <= 0 || h <= 0 || (scene == Screen && text.title.empty() && text.subtitle.empty())) {
        return {}; // nothing to draw: no overlay at all
    }
    std::vector<uint32_t> px((size_t) w * (size_t) h, 0);
    using namespace Gdiplus;
    Bitmap bmp(w, h, w * 4, PixelFormat32bppPARGB, (BYTE *) px.data());
    Graphics g(&bmp);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAlias);
    g.Clear(Color(0, 0, 0, 0));

    // GDI+ has no font fallback, so Thai text gets a face that carries both scripts.
    bool thai = has_thai(text.title) || has_thai(text.subtitle);
    static const wchar_t *latin_title = pick_face({L"Segoe UI Variable Display Semibold", L"Segoe UI Semibold"});
    static const wchar_t *latin_text = pick_face({L"Segoe UI Variable Text", L"Segoe UI"});
    static const wchar_t *thai_face = pick_face({L"Leelawadee UI", L"Tahoma"});
    const wchar_t *title_face = thai ? thai_face : latin_title;
    const wchar_t *sub_face = thai ? thai_face : latin_text;
    int title_style = thai ? FontStyleBold : FontStyleRegular;

    StringFormat fmt(StringFormat::GenericTypographic());
    fmt.SetFormatFlags(fmt.GetFormatFlags() | StringFormatFlagsNoWrap);
    fmt.SetTrimming(StringTrimmingEllipsisCharacter);

    if (scene == Screen) {
        // A lower third: a dark glass plate in the bottom-left corner with a blue accent edge, the title over a
        // smaller line. Small enough to leave the picture alone.
        Font t_f(title_face, (float) h * 0.036f, title_style, UnitPixel);
        Font s_f(sub_face, (float) h * 0.025f, FontStyleRegular, UnitPixel);
        float max_text = (float) w * 0.42f;
        auto width = [&](const std::wstring &s, const Font &f) {
            if (s.empty()) {
                return 0.f;
            }
            RectF box;
            g.MeasureString(s.c_str(), (INT) s.size(), &f, PointF(0, 0), &fmt, &box);
            return (std::min)(box.Width, max_text);
        };
        float pad_x = (float) h * 0.026f, pad_y = (float) h * 0.020f, accent = (float) h * 0.006f;
        float t_h = text.title.empty() ? 0 : t_f.GetSize() * 1.28f;
        float s_h = text.subtitle.empty() ? 0 : s_f.GetSize() * 1.38f;
        float gap = (t_h > 0 && s_h > 0) ? (float) h * 0.004f : 0;
        float box_w = accent + pad_x * 2 + (std::max)(width(text.title, t_f), width(text.subtitle, s_f));
        float box_h = pad_y * 2 + t_h + gap + s_h;
        float bx = (float) w * 0.04f, by = (float) h * 0.93f - box_h;
        float r = (float) h * 0.014f;

        GraphicsPath plate;
        plate.AddArc(bx, by, r * 2, r * 2, 180, 90);
        plate.AddArc(bx + box_w - r * 2, by, r * 2, r * 2, 270, 90);
        plate.AddArc(bx + box_w - r * 2, by + box_h - r * 2, r * 2, r * 2, 0, 90);
        plate.AddArc(bx, by + box_h - r * 2, r * 2, r * 2, 90, 90);
        plate.CloseFigure();
        SolidBrush fill(Color(165, 10, 12, 20));
        g.FillPath(&fill, &plate);
        g.SetClip(&plate);
        SolidBrush blue(Color(255, 10, 132, 255));
        g.FillRectangle(&blue, bx, by, accent, box_h);
        g.ResetClip();
        Pen rim(Color(40, 255, 255, 255), (std::max)(1.f, (float) h / 1080.f));
        g.DrawPath(&rim, &plate);

        float tx = bx + accent + pad_x, ty = by + pad_y;
        SolidBrush white(Color(255, 255, 255, 255)), soft(Color(205, 255, 255, 255));
        if (t_h > 0) {
            g.DrawString(text.title.c_str(), (INT) text.title.size(), &t_f, RectF(tx, ty, max_text, t_h * 1.2f), &fmt,
                         &white);
            ty += t_h + gap;
        }
        if (s_h > 0) {
            g.DrawString(text.subtitle.c_str(), (INT) text.subtitle.size(), &s_f, RectF(tx, ty, max_text, s_h * 1.2f),
                         &fmt, &soft);
        }
        return px;
    }

    float x = (float) w * 0.08f;
    float max_w = (float) w * 0.84f;
    float title_px = (float) h * 0.085f;
    {
        Font probe(title_face, title_px, title_style, UnitPixel);
        RectF box;
        g.MeasureString(text.title.c_str(), (INT) text.title.size(), &probe, PointF(0, 0), &fmt, &box);
        if (box.Width > max_w) {
            title_px *= max_w / box.Width;
        }
    }
    float sub_px = (float) h * 0.042f;
    Font title_f(title_face, title_px, title_style, UnitPixel);
    Font sub_f(sub_face, sub_px, FontStyleRegular, UnitPixel);

    // An accent bar, the title and a line under it, stacked as one block a touch above centre.
    float bar_h = (float) h * 0.007f;
    float gap1 = title_px * 0.42f;
    float title_h = title_px * 1.25f;
    float gap2 = title_px * 0.18f;
    float sub_h = text.subtitle.empty() ? 0 : sub_px * 1.4f;
    float total = bar_h + gap1 + title_h + gap2 + sub_h;
    float y = ((float) h - total) * 0.5f - (float) h * 0.02f;

    float shadow = (float) h * 0.004f;
    auto text_line = [&](const std::wstring &s, const Font &f, float top, const Color &c) {
        if (s.empty()) {
            return;
        }
        RectF rc(x, top, max_w, f.GetSize() * 1.6f);
        SolidBrush sh(Color(80, 0, 0, 0));
        RectF rs = rc;
        rs.Y += shadow;
        g.DrawString(s.c_str(), (INT) s.size(), &f, rs, &fmt, &sh);
        SolidBrush br(c);
        g.DrawString(s.c_str(), (INT) s.size(), &f, rc, &fmt, &br);
    };

    {
        GraphicsPath p;
        float bw = (float) w * 0.045f;
        p.AddArc(x, y, bar_h, bar_h, 90, 180);
        p.AddArc(x + bw - bar_h, y, bar_h, bar_h, 270, 180);
        p.CloseFigure();
        SolidBrush b(Color(230, 255, 255, 255));
        g.FillPath(&b, &p);
    }
    y += bar_h + gap1;
    text_line(text.title, title_f, y, Color(255, 255, 255, 255));
    y += title_h + gap2;
    text_line(text.subtitle, sub_f, y, Color(210, 255, 255, 255));
    return px;
}

std::vector<uint32_t> Studio::load_cover(const std::wstring &path, int w, int h) {
    using namespace Gdiplus;
    std::vector<uint32_t> px;
    if (path.empty() || w <= 0 || h <= 0) {
        return px;
    }
    Bitmap src(path.c_str());
    if (src.GetLastStatus() != Ok || src.GetWidth() == 0 || src.GetHeight() == 0) {
        return px;
    }
    // Fill the canvas: scale to cover, crop the overflow evenly from both sides.
    float sw = (float) src.GetWidth(), sh = (float) src.GetHeight();
    float scale = (std::max)((float) w / sw, (float) h / sh);
    float cw = (float) w / scale, ch = (float) h / scale;
    RectF from((sw - cw) * 0.5f, (sh - ch) * 0.5f, cw, ch);
    px.assign((size_t) w * (size_t) h, 0xff000000u);
    Bitmap dst(w, h, w * 4, PixelFormat32bppPARGB, (BYTE *) px.data());
    Graphics g(&dst);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    g.SetCompositingMode(CompositingModeSourceCopy);
    ImageAttributes clamp;
    clamp.SetWrapMode(WrapModeTileFlipXY); // no dark fringe from sampling past the edge
    g.DrawImage(&src, RectF(0, 0, (float) w, (float) h), from.X, from.Y, from.Width, from.Height, UnitPixel, &clamp);
    for (auto &p : px) {
        p |= 0xff000000u; // backgrounds are opaque, whatever the file's alpha
    }
    return px;
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
    } shaders[] = {{kPsBg, &ps_bg_},     {kPsBlur, &ps_blur_}, {kPsCard, &ps_card_}, {kPsTex, &ps_tex_},
                   {kPsFade, &ps_fade_}, {kPsY, &ps_y_},       {kPsUv, &ps_uv_},     {kPsImg, &ps_img_}};
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
    dev_->CreateRasterizerState(&rd, &raster_);
    return vs_ && layout_ && vb_ && cb_ && sampler_ && blend_alpha_ && blend_premult_;
}

void Studio::shutdown() {
    for (int i = 0; i < kSceneCount; ++i) {
        release(overlay_srv_[i]);
        release(overlay_[i]);
    }
    release(canvas_srv_);
    release(canvas_rtv_);
    release(canvas_);
    release(snap_srv_);
    release(snap_);
    release(y_rtv_);
    release(y_);
    release(uv_rtv_);
    release(uv_);
    for (auto **ps : {&ps_bg_, &ps_blur_, &ps_card_, &ps_tex_, &ps_fade_, &ps_y_, &ps_uv_, &ps_img_}) {
        release(*ps);
    }
    release(bg_srv_);
    release(bg_);
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

bool Studio::make_target(int w, int h, DXGI_FORMAT fmt, bool mips, ID3D11Texture2D **tex,
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
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
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

bool Studio::set_canvas_size(int w, int h) {
    w &= ~1;
    h &= ~1;
    if (w == cw_ && h == ch_ && canvas_) {
        return true;
    }
    release(canvas_srv_);
    release(canvas_rtv_);
    release(canvas_);
    release(snap_srv_);
    release(snap_);
    release(y_rtv_);
    release(y_);
    release(uv_rtv_);
    release(uv_);
    cw_ = ch_ = 0;
    rendered_ = false;
    fading_ = false;
    const DXGI_FORMAT rgba = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (!make_target(w, h, rgba, true, &canvas_, &canvas_rtv_, &canvas_srv_) ||
        !make_target(w, h, rgba, true, &snap_, nullptr, &snap_srv_) ||
        !make_target(w, h, DXGI_FORMAT_R8_UNORM, false, &y_, &y_rtv_, nullptr) ||
        !make_target(w / 2, h / 2, DXGI_FORMAT_R8G8_UNORM, false, &uv_, &uv_rtv_, nullptr)) {
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

void Studio::set_overlay(Scene scene, const std::vector<uint32_t> &pixels, int w, int h) {
    release(overlay_srv_[scene]);
    release(overlay_[scene]);
    if (pixels.size() != (size_t) w * (size_t) h || w <= 0 || h <= 0) {
        return;
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
    if (SUCCEEDED(dev_->CreateTexture2D(&td, &srd, &overlay_[scene]))) {
        dev_->CreateShaderResourceView(overlay_[scene], nullptr, &overlay_srv_[scene]);
    }
}

void Studio::set_background_image(const std::vector<uint32_t> &pixels, int w, int h) {
    release(bg_srv_);
    release(bg_);
    if (pixels.size() != (size_t) w * (size_t) h || w <= 0 || h <= 0) {
        return;
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
    if (SUCCEEDED(dev_->CreateTexture2D(&td, &srd, &bg_))) {
        dev_->CreateShaderResourceView(bg_, nullptr, &bg_srv_);
    }
}

void Studio::set_scene(Scene scene, double now_s) {
    if (scene == scene_) {
        return;
    }
    if (rendered_ && snap_ && canvas_) {
        ctx_->CopyResource(snap_, canvas_); // fade from exactly what was last shown, mid-fade included
        fading_ = true;
        fade_start_ = now_s;
    }
    scene_ = scene;
}

bool Studio::animating(double now_s) const {
    return (fading_ && now_s - fade_start_ < kFadeSeconds + 0.05) || scene_ != Screen;
}

void Studio::set_cb(ID3D11Buffer *cb, const float *data, size_t floats) {
    D3D11_MAPPED_SUBRESOURCE map = {};
    if (SUCCEEDED(ctx_->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        memset(map.pData, 0, 16 * 16);
        memcpy(map.pData, data, (std::min)(floats, (size_t) 64) * sizeof(float));
        ctx_->Unmap(cb, 0);
    }
}

void Studio::viewport(int w, int h) {
    D3D11_VIEWPORT vp = {};
    vp.Width = (float) w;
    vp.Height = (float) h;
    vp.MaxDepth = 1;
    ctx_->RSSetViewports(1, &vp);
}

void Studio::draw(ID3D11PixelShader *ps, ID3D11ShaderResourceView *srv, ID3D11Buffer *cb, bool blend,
                  bool premult) {
    UINT stride = 16, offset = 0;
    ctx_->IASetInputLayout(layout_);
    ctx_->IASetVertexBuffers(0, 1, &vb_, &stride, &offset);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx_->VSSetShader(vs_, nullptr, 0);
    ctx_->PSSetShader(ps, nullptr, 0);
    ctx_->PSSetConstantBuffers(0, 1, &cb);
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

void Studio::draw_scene(Scene scene, ID3D11ShaderResourceView *src, int src_w, int src_h, int src_mips,
                        double now_s) {
    const float W = (float) cw_, H = (float) ch_;
    auto field = [&](Scene mood, bool still = false) {
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
        set_cb(cb_, c, 20);
        draw(ps_bg_, nullptr, cb_, false, false);
    };

    auto picture = [&](float dim) {
        float c[4] = {dim, 0, 0, 0};
        set_cb(cb_, c, 4);
        draw(ps_img_, bg_srv_, cb_, false, false);
    };
    bool use_image = background_ == Image && bg_srv_;

    if (scene != Screen) {
        if (use_image) {
            picture(0.55f); // dark enough for white title text over any photo
        } else {
            field(scene);
        }
        if (overlay_srv_[scene]) {
            draw(ps_tex_, overlay_srv_[scene], cb_, true, true);
        }
        return;
    }

    bool have = src && src_w > 0 && src_h > 0;
    if (!framed_) {
        float black[4] = {0, 0, 0, 1};
        ctx_->ClearRenderTargetView(canvas_rtv_, black);
    } else if (use_image) {
        picture(0.9f);
    } else if (background_ == Blur && have) {
        // Ambient backdrop: the picture itself, blurred down to colour and dimmed, covering the canvas.
        float sa = (float) src_w / (float) src_h, ca = W / H;
        float sx = 1, sy = 1;
        if (sa > ca) {
            sx = ca / sa;
        } else {
            sy = sa / ca;
        }
        sx /= 1.15f; // zoom in a little so the softened edges stay off the canvas
        sy /= 1.15f;
        int lod = std::clamp((int) std::floor(std::log2((float) src_w / 28.f)), 0, src_mips - 1);
        float tw = (float) (std::max)(1, src_w >> lod), th = (float) (std::max)(1, src_h >> lod);
        float c[8] = {sx, sy, (1 - sx) * 0.5f, (1 - sy) * 0.5f, (float) lod, 1.f / tw, 1.f / th, 0.32f};
        set_cb(cb_, c, 8);
        draw(ps_blur_, src, cb_, false, false);
    } else {
        field(Screen, true); // held still: the Screen scene only redraws when the picture changes
    }
    if (!have) {
        return;
    }

    // The picture, fitted inside the margin; framed gets rounded corners and a shadow.
    float margin = framed_ ? H * 0.06f : 0.f;
    float bw = W - margin * 2, bh = H - margin * 2;
    float sa = (float) src_w / (float) src_h;
    float rw = bw, rh = bw / sa;
    if (rh > bh) {
        rh = bh;
        rw = bh * sa;
    }
    float rx = std::floor((W - rw) * 0.5f), ry = std::floor((H - rh) * 0.5f);
    float scale = (std::min)(rw / (float) src_w, rh / (float) src_h);
    int lod = scale < 1.f ? (std::min)((int) std::floor(std::log2(1.f / scale)), src_mips - 1) : 0;
    float radius = framed_ ? (std::min)(rw, rh) * 0.035f : 0.f;
    float shadow = framed_ ? H * 0.035f : 0.f;
    float c[12] = {rx, ry, rw, rh, radius, shadow, (float) lod, 0,
                   (float) (std::max)(1, src_w >> lod), (float) (std::max)(1, src_h >> lod), 0, 0};
    set_cb(cb_, c, 12);
    draw(ps_card_, src, cb_, true, false);
    if (overlay_srv_[Screen]) {
        draw(ps_tex_, overlay_srv_[Screen], cb_, true, true); // the caption, if one is set
    }
}

void Studio::render(ID3D11ShaderResourceView *src, int src_w, int src_h, int src_mips, double now_s) {
    if (!canvas_) {
        return;
    }
    viewport(cw_, ch_);
    ctx_->OMSetRenderTargets(1, &canvas_rtv_, nullptr);
    draw_scene(scene_, src, src_w, src_h, src_mips, now_s);
    if (fading_) {
        double p = (now_s - fade_start_) / kFadeSeconds;
        if (p >= 1) {
            fading_ = false;
        } else {
            float e = (float) (p * p * (3 - 2 * p)); // ease in-out
            float c[4] = {1.f - e, 1.f + 0.035f * e, 0, 0};
            set_cb(cb_, c, 4);
            draw(ps_fade_, snap_srv_, cb_, true, false);
        }
    }
    ID3D11RenderTargetView *none = nullptr;
    ctx_->OMSetRenderTargets(1, &none, nullptr);
    rendered_ = true;
}

bool Studio::export_planes(ID3D11Texture2D **y, ID3D11Texture2D **uv) {
    if (!canvas_ || !y_ || !uv_) {
        return false;
    }
    float c[4] = {};
    set_cb(cb_, c, 4);
    viewport(cw_, ch_);
    ctx_->OMSetRenderTargets(1, &y_rtv_, nullptr);
    draw(ps_y_, canvas_srv_, cb_, false, false);
    viewport(cw_ / 2, ch_ / 2);
    ctx_->OMSetRenderTargets(1, &uv_rtv_, nullptr);
    draw(ps_uv_, canvas_srv_, cb_, false, false);
    ID3D11RenderTargetView *none = nullptr;
    ctx_->OMSetRenderTargets(1, &none, nullptr);
    *y = y_;
    *uv = uv_;
    return true;
}

} // namespace airscreen
