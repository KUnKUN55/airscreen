/* Idle window: a full-window color field that shows receiver state (waiting, pairing, connected, failed),
 * the receiver name, and the toolbar. Mirrored video replaces it once the first frame arrives. */

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shlwapi.h>

#include "ui.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shlwapi.lib")

using Gdiplus::Bitmap;
using Gdiplus::Color;
using Gdiplus::Font;
using Gdiplus::FontFamily;
using Gdiplus::FontStyleRegular;
using Gdiplus::Graphics;
using Gdiplus::GraphicsPath;
using Gdiplus::LinearGradientBrush;
using Gdiplus::LineCapRound;
using Gdiplus::LineJoinRound;
using Gdiplus::Pen;
using Gdiplus::PointF;
using Gdiplus::RectF;
using Gdiplus::SmoothingModeAntiAlias;
using Gdiplus::SolidBrush;
using Gdiplus::StringFormat;
using Gdiplus::UnitPixel;

namespace airscreen {
namespace {

enum class Mood { Wait, Pair, Live, Fail };

// One field per connection state: the ground itself says what is happening.
struct Field {
    Color top;
    Color bottom;
    Color bloom_a;
    Color bloom_b;
    Color ink; // label ink on a control flooded white
};

// Forest and earth: a deep woodland green while waiting, warm bark while pairing, a cool pine once mirroring,
// and wet slate when something failed. Blooms of moss and sunlit tan keep the fields from reading as flat paint.
Field field_for(Mood mood) {
    switch (mood) {
    case Mood::Pair:
        return {Color(255, 124, 84, 52), Color(255, 50, 32, 20), Color(150, 226, 184, 120), Color(110, 170, 120, 76),
                Color(255, 74, 46, 26)};
    case Mood::Live:
        return {Color(255, 30, 84, 76), Color(255, 9, 34, 32), Color(130, 104, 176, 150), Color(92, 196, 168, 116),
                Color(255, 16, 56, 50)};
    case Mood::Fail:
        return {Color(255, 62, 60, 56), Color(255, 24, 23, 22), Color(96, 196, 112, 88), Color(110, 120, 112, 98),
                Color(255, 36, 34, 32)};
    default:
        return {Color(255, 40, 94, 72), Color(255, 13, 40, 31), Color(150, 128, 176, 116), Color(104, 206, 164, 112),
                Color(255, 28, 66, 50)};
    }
}

const Color c_label(255, 250, 244, 232); // warm cream, like the paper in the leaves
const Color c_soft(222, 240, 230, 210);  // secondary text: the same cream, a touch quieter, never gray

const wchar_t *pick_face(std::initializer_list<const wchar_t *> faces) {
    for (auto *name : faces) {
        FontFamily fam(name);
        if (fam.GetLastStatus() == Gdiplus::Ok) {
            return name;
        }
    }
    return L"Segoe UI";
}

const wchar_t *face_text() {
    static const wchar_t *face = pick_face({L"Segoe UI Variable Text", L"Segoe UI"});
    return face;
}

const wchar_t *face_strong() {
    static const wchar_t *face = pick_face({L"Segoe UI Variable Text Semibold", L"Segoe UI Semibold"});
    return face;
}

const wchar_t *face_display() {
    static const wchar_t *face = pick_face({L"Segoe UI Variable Display Semibold", L"Segoe UI Semibold"});
    return face;
}

StringFormat &line_format() {
    // Typographic metrics so text starts exactly at x; an ellipsis when a line meets its width. LineLimit is
    // cleared because it silently drops a display-size line whose layout box rounds a hair short.
    static StringFormat *fmt = [] {
        StringFormat *f = StringFormat::GenericTypographic()->Clone();
        f->SetFormatFlags((f->GetFormatFlags() & ~Gdiplus::StringFormatFlagsLineLimit) |
                          Gdiplus::StringFormatFlagsNoWrap | Gdiplus::StringFormatFlagsMeasureTrailingSpaces);
        f->SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
        return f;
    }();
    return *fmt;
}

float ascent(const Font &f) {
    FontFamily fam;
    f.GetFamily(&fam);
    return f.GetSize() * (float) fam.GetCellAscent(f.GetStyle()) / (float) fam.GetEmHeight(f.GetStyle());
}

float width_of(Graphics &g, const std::wstring &s, const Font &f) {
    RectF box;
    g.MeasureString(s.c_str(), (INT) s.size(), &f, PointF(0, 0), &line_format(), &box);
    return box.Width;
}

// One line with its baseline at `baseline`, trimmed to max_w. Returns the width used.
float draw_line(Graphics &g, const std::wstring &s, const Font &f, float x, float baseline, float max_w,
                const Color &c) {
    if (s.empty() || max_w <= 0) {
        return 0;
    }
    SolidBrush brush(c);
    RectF rc(x, baseline - ascent(f), max_w, f.GetHeight(&g) * 1.5f);
    g.DrawString(s.c_str(), (INT) s.size(), &f, rc, &line_format(), &brush);
    return (std::min)(width_of(g, s, f), max_w);
}

void add_round_rect(GraphicsPath &p, RectF r, float rad) {
    float d = rad * 2.f;
    if (d > r.Width) {
        d = r.Width;
    }
    if (d > r.Height) {
        d = r.Height;
    }
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    p.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    p.CloseFigure();
}

void fill_round(Graphics &g, const RectF &r, float rad, const Color &c) {
    GraphicsPath p;
    add_round_rect(p, r, rad);
    SolidBrush b(c);
    g.FillPath(&b, &p);
}

void draw_round(Graphics &g, const RectF &r, float rad, const Color &c, float w) {
    GraphicsPath p;
    add_round_rect(p, r, rad);
    Pen pen(c, w);
    g.DrawPath(&pen, &p);
}

bool drift_allowed() {
    BOOL on = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0);
    return on != FALSE;
}

// Written straight into the DIB: two soft blooms over a diagonal wash. Per-pixel math is several times faster
// than GDI+ gradient brushes plus a stretch, and lets the long gradients carry dither so they never band.
void fill_field(uint32_t *out, int w, int h, const Field &f, double t) {
    struct Bloom {
        float x, y, inv_r2, a, r, g, b;
    };
    auto bloom = [&](const Color &c, float u, float v, float radius) {
        float r = radius * (float) w;
        return Bloom{u * (float) w,         v * (float) h,         1.f / (r * r),        (float) c.GetA() / 255.f,
                     (float) c.GetR(), (float) c.GetG(), (float) c.GetB()};
    };
    const Bloom blooms[2] = {
        bloom(f.bloom_a, 0.80f + 0.07f * (float) sin(t * 0.11), 0.16f + 0.08f * (float) cos(t * 0.09), 0.62f),
        bloom(f.bloom_b, 0.28f + 0.08f * (float) cos(t * 0.07), 0.02f + 0.07f * (float) sin(t * 0.13), 0.46f),
    };
    // A 70-degree wash from top to bottom, as a projection onto that direction.
    float span = 0.342f * (float) w + 0.940f * (float) h;
    float kx = 0.342f / span;
    float ky = 0.940f / span;
    float r0 = f.top.GetR(), g0 = f.top.GetG(), b0 = f.top.GetB();
    float dr = f.bottom.GetR() - r0, dg = f.bottom.GetG() - g0, db = f.bottom.GetB() - b0;
    uint32_t noise = 0x9e3779b9u;
    for (int y = 0; y < h; ++y) {
        float dy2[2];
        for (int i = 0; i < 2; ++i) {
            float dy = (float) y - blooms[i].y;
            dy2[i] = dy * dy;
        }
        uint32_t *row = out + (size_t) y * (size_t) w;
        for (int x = 0; x < w; ++x) {
            float k = (float) x * kx + (float) y * ky;
            float r = r0 + dr * k, g = g0 + dg * k, b = b0 + db * k;
            for (int i = 0; i < 2; ++i) {
                const Bloom &bl = blooms[i];
                float dx = (float) x - bl.x;
                float d2 = (dx * dx + dy2[i]) * bl.inv_r2;
                if (d2 < 1.f) {
                    float q = (1.f - d2) * (1.f - d2) * bl.a;
                    r += (bl.r - r) * q;
                    g += (bl.g - g) * q;
                    b += (bl.b - b) * q;
                }
            }
            noise = noise * 1664525u + 1013904223u;
            float n = (float) (noise >> 24) / 127.5f - 0.5f; // about one level of dither
            auto to8 = [n](float v) { return (uint32_t) std::clamp((int) (v + n), 0, 255); };
            row[x] = 0xff000000u | (to8(r) << 16) | (to8(g) << 8) | to8(b);
        }
    }
}

// A soft drop shadow for whatever `draw` paints: rendered at quarter scale into an alpha mask (a blurred shadow
// has no fine detail to lose), box-blurred twice each way (close to a Gaussian), then laid into the DIB in `tint`.
template <class Draw>
void cast_shadow(uint32_t *out, int w, int h, const Color &tint, float offset, float blur, float strength,
                 Draw &&draw) {
    const int sw = (std::max)(1, w / 4);
    const int sh = (std::max)(1, h / 4);
    const float sx = (float) sw / (float) w;
    const float sy = (float) sh / (float) h;
    Bitmap mask(sw, sh, PixelFormat32bppPARGB);
    {
        Graphics mg(&mask);
        mg.Clear(Color(0, 0, 0, 0));
        mg.SetSmoothingMode(SmoothingModeAntiAlias);
        mg.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
        mg.ScaleTransform(sx, sy);
        mg.TranslateTransform(0, offset);
        draw(mg);
    }

    std::vector<float> a((size_t) sw * (size_t) sh);
    Gdiplus::Rect all(0, 0, sw, sh);
    Gdiplus::BitmapData bd;
    if (mask.LockBits(&all, Gdiplus::ImageLockModeRead, PixelFormat32bppPARGB, &bd) != Gdiplus::Ok) {
        return;
    }
    for (int y = 0; y < sh; ++y) {
        const BYTE *src = (const BYTE *) bd.Scan0 + (ptrdiff_t) y * bd.Stride;
        for (int x = 0; x < sw; ++x) {
            a[(size_t) y * sw + x] = (float) src[x * 4 + 3] / 255.f;
        }
    }
    mask.UnlockBits(&bd);

    const int r = (std::max)(1, (int) std::lround(blur * sx));
    const float norm = 1.f / (float) (2 * r + 1);
    std::vector<float> tmp(a.size());
    for (int pass = 0; pass < 2; ++pass) {
        for (int y = 0; y < sh; ++y) {
            const float *src = &a[(size_t) y * sw];
            float *dst = &tmp[(size_t) y * sw];
            float sum = 0;
            for (int i = -r; i <= r; ++i) {
                sum += src[std::clamp(i, 0, sw - 1)];
            }
            for (int x = 0; x < sw; ++x) {
                dst[x] = sum * norm;
                sum += src[(std::min)(x + r + 1, sw - 1)] - src[(std::max)(x - r, 0)];
            }
        }
        for (int x = 0; x < sw; ++x) {
            float sum = 0;
            for (int i = -r; i <= r; ++i) {
                sum += tmp[(size_t) std::clamp(i, 0, sh - 1) * sw + x];
            }
            for (int y = 0; y < sh; ++y) {
                a[(size_t) y * sw + x] = sum * norm;
                sum += tmp[(size_t) (std::min)(y + r + 1, sh - 1) * sw + x] -
                       tmp[(size_t) (std::max)(y - r, 0) * sw + x];
            }
        }
    }

    // Skip the empty field: only rows that hold shadow, only the columns any shadow reaches.
    const float eps = 0.004f;
    std::vector<char> row_live(sh, 0);
    int x0 = sw, x1 = -1;
    for (int y = 0; y < sh; ++y) {
        for (int x = 0; x < sw; ++x) {
            if (a[(size_t) y * sw + x] > eps) {
                row_live[y] = 1;
                x0 = (std::min)(x0, x);
                x1 = (std::max)(x1, x);
            }
        }
    }
    if (x1 < 0) {
        return;
    }
    const int X0 = (std::max)(0, (int) ((float) (x0 - 1) / sx));
    const int X1 = (std::min)(w, (int) ((float) (x1 + 2) / sx));
    const float tr = tint.GetR(), tg = tint.GetG(), tb = tint.GetB();
    for (int Y = 0; Y < h; ++Y) {
        float v = ((float) Y + 0.5f) * sy - 0.5f;
        int v0 = std::clamp((int) std::floor(v), 0, sh - 1);
        int v1 = (std::min)(v0 + 1, sh - 1);
        if (!row_live[v0] && !row_live[v1]) {
            continue;
        }
        float fv = std::clamp(v - (float) v0, 0.f, 1.f);
        const float *ra = &a[(size_t) v0 * sw];
        const float *rb = &a[(size_t) v1 * sw];
        uint32_t *row = out + (size_t) Y * (size_t) w;
        for (int X = X0; X < X1; ++X) {
            float u = ((float) X + 0.5f) * sx - 0.5f;
            int u0 = std::clamp((int) std::floor(u), 0, sw - 1);
            int u1 = (std::min)(u0 + 1, sw - 1);
            float fu = std::clamp(u - (float) u0, 0.f, 1.f);
            float s = (ra[u0] + (ra[u1] - ra[u0]) * fu) * (1.f - fv) + (rb[u0] + (rb[u1] - rb[u0]) * fu) * fv;
            if (s <= eps) {
                continue;
            }
            // Thin text blurs out to a faint mask; the gain keeps small lines shadowed without choking big ones.
            float k = (std::min)(1.f, s * 1.6f) * strength;
            uint32_t p = row[X];
            float pr = (float) ((p >> 16) & 255), pg = (float) ((p >> 8) & 255), pb = (float) (p & 255);
            pr += (tr - pr) * k;
            pg += (tg - pg) * k;
            pb += (tb - pb) * k;
            row[X] = 0xff000000u | ((uint32_t) pr << 16) | ((uint32_t) pg << 8) | (uint32_t) pb;
        }
    }
}

void round_pen(Pen &p) {
    p.SetStartCap(LineCapRound);
    p.SetEndCap(LineCapRound);
    p.SetLineJoin(LineJoinRound);
}

void icon_pencil(Graphics &g, RectF b, const Color &c, float w) {
    Pen p(c, w);
    round_pen(p);
    auto P = [&](float u, float v) { return PointF(b.X + u * b.Width, b.Y + v * b.Height); };
    PointF body[] = {P(0.16f, 0.84f), P(0.38f, 0.78f), P(0.91f, 0.25f), P(0.75f, 0.09f), P(0.22f, 0.62f)};
    g.DrawPolygon(&p, body, 5);
    g.DrawLine(&p, P(0.78f, 0.37f), P(0.63f, 0.22f));
}

void icon_expand(Graphics &g, RectF b, const Color &c, float w) {
    Pen p(c, w);
    round_pen(p);
    auto P = [&](float u, float v) { return PointF(b.X + u * b.Width, b.Y + v * b.Height); };
    PointF corners[4][3] = {{P(0.14f, 0.40f), P(0.14f, 0.14f), P(0.40f, 0.14f)},
                            {P(0.60f, 0.14f), P(0.86f, 0.14f), P(0.86f, 0.40f)},
                            {P(0.86f, 0.60f), P(0.86f, 0.86f), P(0.60f, 0.86f)},
                            {P(0.40f, 0.86f), P(0.14f, 0.86f), P(0.14f, 0.60f)}};
    for (auto &corner : corners) {
        g.DrawLines(&p, corner, 3);
    }
}

void icon_pin(Graphics &g, RectF b, const Color &c, float w) {
    Pen p(c, w);
    round_pen(p);
    auto P = [&](float u, float v) { return PointF(b.X + u * b.Width, b.Y + v * b.Height); };
    PointF body[] = {P(0.40f, 0.12f), P(0.40f, 0.44f), P(0.24f, 0.60f), P(0.76f, 0.60f), P(0.60f, 0.44f),
                     P(0.60f, 0.12f)};
    g.DrawLine(&p, P(0.34f, 0.12f), P(0.66f, 0.12f));
    g.DrawLines(&p, body, 6);
    g.DrawLine(&p, P(0.5f, 0.60f), P(0.5f, 0.90f));
}

void icon_lock(Graphics &g, RectF b, const Color &c, float w) {
    Pen p(c, w);
    round_pen(p);
    auto P = [&](float u, float v) { return PointF(b.X + u * b.Width, b.Y + v * b.Height); };
    draw_round(g, RectF(b.X + 0.20f * b.Width, b.Y + 0.44f * b.Height, 0.60f * b.Width, 0.46f * b.Height),
               0.12f * b.Width, c, w);
    g.DrawArc(&p, b.X + 0.32f * b.Width, b.Y + 0.12f * b.Height, 0.36f * b.Width, 0.40f * b.Height, 180, 180);
    g.DrawLine(&p, P(0.32f, 0.32f), P(0.32f, 0.44f));
    g.DrawLine(&p, P(0.68f, 0.32f), P(0.68f, 0.44f));
}

void icon_moon(Graphics &g, RectF b, const Color &c, float w) {
    Pen p(c, w);
    round_pen(p);
    // Crescent: the outer circle's arc outside a smaller offset circle, closed back along that circle's arc.
    GraphicsPath path;
    path.AddArc(b.X + 0.14f * b.Width, b.Y + 0.16f * b.Height, 0.72f * b.Width, 0.72f * b.Height, 14.2f, 248.5f);
    path.AddArc(b.X + 0.38f * b.Width, b.Y + 0.06f * b.Height, 0.60f * b.Width, 0.60f * b.Height, 221.1f, -165.4f);
    path.CloseFigure();
    g.DrawPath(&p, &path);
}

// A laser pointer: a short beam ending in a lit spot.
void icon_laser(Graphics &g, RectF b, const Color &c, float w) {
    Pen p(c, w);
    round_pen(p);
    auto P = [&](float u, float v) { return PointF(b.X + u * b.Width, b.Y + v * b.Height); };
    g.DrawLine(&p, P(0.12f, 0.88f), P(0.52f, 0.48f));
    SolidBrush spot(c);
    float r = 0.17f * b.Width;
    PointF s = P(0.68f, 0.32f);
    g.FillEllipse(&spot, s.X - r, s.Y - r, r * 2, r * 2);
    Pen ray(Color((BYTE) (c.GetA() * 0.7f), c.GetR(), c.GetG(), c.GetB()), w * 0.8f);
    round_pen(ray);
    g.DrawLine(&ray, P(0.68f, 0.02f), P(0.68f, 0.10f));
    g.DrawLine(&ray, P(0.98f, 0.32f), P(0.90f, 0.32f));
    g.DrawLine(&ray, P(0.90f, 0.10f), P(0.84f, 0.16f));
}

// One leaf, drawn along +x from its stem at (x, y) and turned by `deg` (clockwise, as the screen's y runs down):
// two curves for the blade, a light midrib and paired veins clipped to the blade, like the pressed-paper leaves
// of the theme.
struct LeafTone {
    Color a, b; // the lit side and the shaded side
};

void draw_leaf(Graphics &g, float x, float y, float len, float half_w, float deg, const LeafTone &tone, bool dark) {
    auto dim = [dark](const Color &c) {
        if (!dark) {
            return c;
        }
        return Color(c.GetA(), (BYTE) (c.GetR() * 0.5f), (BYTE) (c.GetG() * 0.5f), (BYTE) (c.GetB() * 0.5f));
    };
    Gdiplus::Region old_clip;
    g.GetClip(&old_clip);
    Gdiplus::Matrix old;
    g.GetTransform(&old);
    g.TranslateTransform(x, y);
    g.RotateTransform(deg);

    GraphicsPath blade;
    blade.AddBezier(PointF(0, 0), PointF(len * 0.22f, -half_w * 1.25f), PointF(len * 0.70f, -half_w * 1.0f),
                    PointF(len, 0));
    blade.AddBezier(PointF(len, 0), PointF(len * 0.70f, half_w * 1.0f), PointF(len * 0.22f, half_w * 1.25f),
                    PointF(0, 0));
    blade.CloseFigure();
    LinearGradientBrush fill(RectF(0, -half_w * 1.1f, len, half_w * 2.2f), dim(tone.a), dim(tone.b), 90.f);
    g.FillPath(&fill, &blade);

    Pen stem(dim(tone.b), (std::max)(1.2f, half_w * 0.08f));
    stem.SetStartCap(LineCapRound);
    g.DrawLine(&stem, -len * 0.10f, half_w * 0.04f, len * 0.02f, 0.f);

    g.SetClip(&blade, Gdiplus::CombineModeIntersect);
    Color vein_c(dark ? 34 : 64, 255, 248, 230);
    Pen rib(vein_c, (std::max)(1.f, half_w * 0.06f));
    g.DrawLine(&rib, len * 0.02f, 0.f, len * 0.96f, 0.f);
    Pen vein(Color((BYTE) (vein_c.GetA() * 0.7f), 255, 248, 230), (std::max)(0.8f, half_w * 0.035f));
    for (int i = 1; i <= 6; ++i) {
        float u = (float) i / 7.5f;
        float reach = half_w * 0.95f * (1.f - u * 0.5f);
        g.DrawLine(&vein, len * u, 0.f, len * (u + 0.14f), -reach);
        g.DrawLine(&vein, len * u, 0.f, len * (u + 0.14f), reach);
    }
    g.SetTransform(&old);
    g.SetClip(&old_clip);
}

// Sprays of leaves reaching in from the corners. Positions are offsets from a corner in units of the window's
// short side, so the sprays keep their shape at any window size.
void draw_leaves(Graphics &g, int w, int h, bool dark, bool lower) {
    static const LeafTone tones[] = {
        {Color(255, 74, 132, 98), Color(255, 30, 76, 56)},   // forest
        {Color(255, 150, 186, 142), Color(255, 86, 128, 92)}, // sage
        {Color(255, 104, 154, 104), Color(255, 48, 98, 66)},  // moss
        {Color(255, 226, 196, 150), Color(255, 168, 126, 84)}, // dry tan
        {Color(255, 164, 110, 70), Color(255, 102, 62, 36)},  // bark brown
    };
    struct Leaf {
        float du, dv, len, wr, deg;
        int tone;
    };
    static const Leaf top_right[] = {
        {-0.26f, -0.05f, 0.17f, 0.24f, 84, 4}, {-0.16f, -0.04f, 0.22f, 0.22f, 96, 3},
        {0.03f, 0.34f, 0.22f, 0.21f, 176, 0},  {-0.07f, -0.03f, 0.27f, 0.22f, 108, 1},
        {0.02f, 0.16f, 0.28f, 0.20f, 156, 2},  {0.01f, -0.02f, 0.34f, 0.21f, 128, 0},
    };
    static const Leaf bottom_right[] = {
        {-0.22f, 0.04f, 0.16f, 0.22f, -98, 1},
        {-0.10f, 0.03f, 0.22f, 0.22f, -114, 3},
        {0.03f, -0.13f, 0.22f, 0.20f, -164, 2},
        {0.02f, 0.02f, 0.27f, 0.21f, -138, 0},
    };
    float S = (float) (std::min)(w, h);
    for (auto &l : top_right) {
        float len = l.len * S;
        draw_leaf(g, (float) w + l.du * S, l.dv * S, len, len * l.wr, l.deg, tones[l.tone], dark);
    }
    if (lower) {
        for (auto &l : bottom_right) {
            float len = l.len * S;
            draw_leaf(g, (float) w + l.du * S, (float) h + l.dv * S, len, len * l.wr, l.deg, tones[l.tone], dark);
        }
    }
}

void chevron(Graphics &g, float x, float mid, float s, const Color &c, float w) {
    Pen p(c, w);
    round_pen(p);
    PointF pts[] = {PointF(x, mid - s), PointF(x + s * 0.6f, mid), PointF(x, mid + s)};
    g.DrawLines(&p, pts, 3);
}

void fill_round_grad(Graphics &g, const RectF &r, float rad, const Color &a, const Color &b, float angle) {
    GraphicsPath p;
    add_round_rect(p, r, rad);
    LinearGradientBrush br(r, a, b, angle);
    g.FillPath(&br, &p);
}

// The AirScreen logo (src/app/logo_hi.png, embedded as a resource), decoded once. The stream stays alive with
// the bitmap, as GDI+ requires.
Bitmap *logo_bitmap() {
    static Bitmap *bmp = [] () -> Bitmap * {
        HMODULE mod = GetModuleHandleW(nullptr);
        HRSRC res = FindResourceW(mod, MAKEINTRESOURCEW(103 /* IDR_LOGO */), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
        HGLOBAL data = res ? LoadResource(mod, res) : nullptr;
        const BYTE *bytes = data ? (const BYTE *) LockResource(data) : nullptr;
        if (!bytes) {
            return nullptr;
        }
        IStream *stream = SHCreateMemStream(bytes, SizeofResource(mod, res));
        if (!stream) {
            return nullptr;
        }
        auto *b = new Bitmap(stream);
        if (b->GetLastStatus() != Gdiplus::Ok) {
            delete b;
            stream->Release();
            return nullptr;
        }
        return b; // lives for the process, with its stream
    }();
    return bmp;
}

void draw_logo_image(Graphics &g, const RectF &box) {
    Bitmap *bmp = logo_bitmap();
    if (!bmp) {
        return;
    }
    auto mode = g.GetInterpolationMode();
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    g.DrawImage(bmp, box);
    g.SetInterpolationMode(mode);
}

void draw_mark(Graphics &g, RectF box) {
    // Fallback window icon only; the shipped icon is src/app/app.ico, made from the same picture.
    draw_logo_image(g, box);
}

HICON icon_from_size(int size) {
    Bitmap bmp(size, size, PixelFormat32bppARGB);
    Graphics g(&bmp);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.Clear(Color(0, 0, 0, 0));
    draw_mark(g, RectF(0, 0, (float) size, (float) size));
    HICON icon = nullptr;
    bmp.GetHICON(&icon);
    return icon;
}

} // namespace

ULONG_PTR gdiplus_startup() {
    Gdiplus::GdiplusStartupInput in;
    ULONG_PTR token = 0;
    Gdiplus::GdiplusStartup(&token, &in, nullptr);
    return token;
}

void gdiplus_shutdown(ULONG_PTR token) {
    if (token) {
        Gdiplus::GdiplusShutdown(token);
    }
}

void ShellUi::startup() {
    ensure_icons();
}

void ShellUi::shutdown() {
    destroy_icons();
    drop_cache();
}

void ShellUi::ensure_icons() {
    if (!icon32_) {
        icon32_ = icon_from_size(32);
    }
    if (!icon16_) {
        icon16_ = icon_from_size(16);
    }
}

void ShellUi::destroy_icons() {
    if (icon32_) {
        DestroyIcon(icon32_);
        icon32_ = nullptr;
    }
    if (icon16_) {
        DestroyIcon(icon16_);
        icon16_ = nullptr;
    }
}

void ShellUi::drop_cache() {
    if (cache_dc_) {
        SelectObject(cache_dc_, cache_old_);
        DeleteDC(cache_dc_);
        cache_dc_ = nullptr;
    }
    if (cache_bmp_) {
        DeleteObject(cache_bmp_);
        cache_bmp_ = nullptr;
    }
    cache_bits_ = nullptr;
    cache_w_ = cache_h_ = 0;
    cache_key_.clear();
    base_.clear();
    base_.shrink_to_fit();
    base_key_.clear();
}

bool ShellUi::ensure_cache(HDC hdc, int w, int h) {
    if (cache_bmp_ && cache_w_ == w && cache_h_ == h) {
        return true;
    }
    drop_cache();
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h; // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    cache_dc_ = CreateCompatibleDC(hdc);
    if (!cache_dc_) {
        return false;
    }
    cache_bmp_ = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &cache_bits_, nullptr, 0);
    if (!cache_bmp_ || !cache_bits_) {
        DeleteDC(cache_dc_);
        cache_dc_ = nullptr;
        cache_bmp_ = nullptr;
        cache_bits_ = nullptr;
        return false;
    }
    cache_old_ = SelectObject(cache_dc_, cache_bmp_);
    cache_w_ = w;
    cache_h_ = h;
    return true;
}

void ShellUi::paint(HWND hwnd, HDC hdc, RECT client, const UiView &view, int hover) {
    client_ = client;
    int w = client.right - client.left;
    int h = client.bottom - client.top;
    if (w < 2 || h < 2) {
        return;
    }
    int dpi = hwnd ? (int) GetDpiForWindow(hwnd) : 96;
    dpi = (std::max)(dpi, 96);
    if (dpi != dpi_) {
        dpi_ = dpi;
        cache_key_.clear();
        base_key_.clear();
    }
    auto D = [this](float v) { return v * (float) dpi_ / 96.f; };
    if (!ensure_cache(hdc, w, h)) {
        // Out of memory for a picture this size: plain ground rather than nothing at all.
        HBRUSH b = CreateSolidBrush(RGB(16, 44, 34));
        FillRect(hdc, &client, b);
        DeleteObject(b);
        return;
    }

    wchar_t head[128];
    swprintf_s(head, L"%d|%d|%d|%d|%d%d%d%d%d%d%d|", w, h, dpi_, hover, (int) view.connected,
               (int) view.always_on_top, (int) view.require_pin, (int) view.fullscreen, (int) view.dark,
               (int) view.laser, (int) view.failed);
    std::wstring key = head + view.name + L'\x1f' + view.status + L'\x1f' + view.pin + L'\x1f' + view.detail + L'' + view.update;
    if (key != cache_key_) {
        paint_still(w, h, view, hover);
        GdiFlush();
        cache_key_ = key;
    }

    // Only what Windows asked to have repainted: on an animation tick, that is the small moving area.
    RECT clip = client;
    RECT box;
    if (GetClipBox(hdc, &box) != ERROR && box.right > box.left && box.bottom > box.top) {
        IntersectRect(&clip, &box, &client);
    }
    int cw = clip.right - clip.left, ch = clip.bottom - clip.top;
    if (cw <= 0 || ch <= 0) {
        return;
    }
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bm = mem ? CreateCompatibleBitmap(hdc, cw, ch) : nullptr;
    if (!bm) {
        if (mem) {
            DeleteDC(mem);
        }
        BitBlt(hdc, clip.left, clip.top, cw, ch, cache_dc_, clip.left, clip.top, SRCCOPY);
        return;
    }
    HGDIOBJ old = SelectObject(mem, bm);
    BitBlt(mem, 0, 0, cw, ch, cache_dc_, clip.left, clip.top, SRCCOPY);
    {
        Graphics g(mem);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.TranslateTransform((float) -clip.left, (float) -clip.top);
        double t = drift_allowed() ? (double) GetTickCount64() / 1000.0 : 0.0;
        if (show_logo_) {
            float fy = float_logo_ ? D(5) * (float) sin(t * 0.9) : 0.f;
            float k = float_logo_ ? 0.5f + 0.5f * (float) sin(t * 1.6) : 0.4f;
            float s = logo_s_, cx = logo_x_ + s * 0.5f, cy = logo_y_ + s * 0.5f + fy;
            GraphicsPath glow;
            glow.AddEllipse(cx - s * 0.52f, cy - s * 0.36f, s * 1.04f, s * 0.72f);
            Gdiplus::PathGradientBrush pg(&glow);
            Color gc(glow_argb_);
            pg.SetCenterColor(Color((BYTE) (36 + 46 * k), gc.GetR(), gc.GetG(), gc.GetB()));
            Color edge(0, gc.GetR(), gc.GetG(), gc.GetB());
            int n = 1;
            pg.SetSurroundColors(&edge, &n);
            g.FillPath(&pg, &glow);
            draw_logo_image(g, RectF(logo_x_, logo_y_ + fy, s, s));
        }
        if (dot_r_ > 0) {
            Color dot(dot_argb_);
            if (breathe_) {
                // A slow breathing halo while waiting: alive, never insistent.
                float k = 0.5f + 0.5f * (float) sin(t * 2.2);
                float hr = dot_r_ + dot_r_ * k;
                fill_round(g, RectF(dot_x_ - hr, dot_y_ - hr, hr * 2, hr * 2), hr,
                           Color((BYTE) (70 * (1.f - k) + 20), dot.GetR(), dot.GetG(), dot.GetB()));
            }
            fill_round(g, RectF(dot_x_ - dot_r_, dot_y_ - dot_r_, dot_r_ * 2, dot_r_ * 2), dot_r_, dot);
        }
    }
    BitBlt(hdc, clip.left, clip.top, cw, ch, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bm);
    DeleteDC(mem);
}

void ShellUi::paint_still(int w, int h, const UiView &view, int hover) {
    auto D = [this](float v) { return v * (float) dpi_ / 96.f; };
    uint32_t *bits = (uint32_t *) cache_bits_;

    Mood mood = view.failed         ? Mood::Fail
                : !view.pin.empty() ? Mood::Pair
                : view.connected    ? Mood::Live
                                    : Mood::Wait;
    Field f = field_for(mood);
    if (view.dark) {
        // Dark mode: the same fields at about a third of their brightness with faint blooms, so waiting becomes a
        // night forest and the other states still read by hue.
        auto dim = [](Color &c, float k, float alpha) {
            c = Color((BYTE) (c.GetA() * alpha), (BYTE) (c.GetR() * k), (BYTE) (c.GetG() * k), (BYTE) (c.GetB() * k));
        };
        dim(f.top, 0.38f, 1.f);
        dim(f.bottom, 0.38f, 1.f);
        dim(f.bloom_a, 0.6f, 0.45f);
        dim(f.bloom_b, 0.6f, 0.45f);
    }

    float m = std::clamp((float) w * 0.06f, D(32), D(72));
    float x = m;
    float avail = (float) w - m * 2.f;
    bool controls = !view.fullscreen && view.pin.empty();
    // Wide windows give the right side to the logo among the leaves; narrow ones keep everything in one column.
    bool art = w >= (int) D(880) && h >= (int) D(500);
    float col_w = art ? avail * 0.56f : avail;
    float row_h = D(40);
    float row_y = (float) h - m - row_h;
    bool steps = controls && mood == Mood::Wait;
    float chip_h = D(44);
    float steps_y = row_y - D(26) - chip_h;
    float bottom = steps ? steps_y - D(34) : controls ? row_y - D(44) : (float) h - m;
    name_rc_ = {};
    edit_rc_ = {};
    dark_rc_ = {};
    update_rc_ = {};
    for (auto &r : btn_rc_) {
        r = {};
    }

    float ls = std::clamp((std::min)((avail - col_w) * 0.80f, (float) h * 0.56f), D(200), D(440));
    float icx = x + col_w + (avail - col_w) * 0.5f;
    float icy = (float) h * 0.44f;
    RectF logo(icx - ls * 0.5f, icy - ls * 0.5f, ls, ls);

    // The name (or PIN) lockup, set straight on the field. It runs twice, once into the shadow mask and once for
    // real, so the shadow always sits under exactly what is drawn.
    auto field_text = [&](Graphics &g, bool real) {
        if (real) {
            g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
        }
        if (!view.pin.empty()) {
            Font hint_f(face_strong(), D(17), FontStyleRegular, UnitPixel);
            // The PIN takes the lockup: digits sit on the floor line, spaced to be read back one at a time.
            const float track = 0.06f;
            auto spaced_width = [&](const Font &font, float size) {
                float tw = 0;
                for (wchar_t ch : view.pin) {
                    tw += width_of(g, std::wstring(1, ch), font) + size * track;
                }
                return tw - size * track;
            };
            float px = std::clamp((std::min)((float) w * 0.16f, (float) h * 0.30f), D(64), D(220));
            {
                Font probe(face_display(), px, FontStyleRegular, UnitPixel);
                float tw = spaced_width(probe, px);
                if (tw > col_w) {
                    px *= col_w / tw;
                }
            }
            Font pin_f(face_display(), px, FontStyleRegular, UnitPixel);
            draw_line(g, L"Enter this PIN on your iPad or iPhone", hint_f, x, bottom - px * 0.95f, col_w, c_soft);
            float cx = x;
            for (wchar_t ch : view.pin) {
                cx += draw_line(g, std::wstring(1, ch), pin_f, cx, bottom, col_w, c_label) + px * track;
            }
            if (real) {
                edit_rc_ = {(LONG) x, (LONG) (bottom - px * 0.74f), (LONG) cx, (LONG) bottom};
            }
            return;
        }

        bool tagline = mood == Mood::Wait || mood == Mood::Fail;
        float px = std::clamp((std::min)((float) w * 0.10f, (float) h * 0.19f), D(44), D(160));
        {
            Font probe(face_display(), px, FontStyleRegular, UnitPixel);
            float nw = width_of(g, view.name, probe);
            if (nw > col_w) {
                px = (std::max)(D(36), px * col_w / nw);
            }
        }
        Font name_f(face_display(), px, FontStyleRegular, UnitPixel);
        float name_base = tagline ? bottom - D(17) * 0.72f - D(20) - px * 0.22f : bottom - px * 0.22f;
        float name_right = x + draw_line(g, view.name, name_f, x, name_base, col_w, c_label);
        if (real) {
            float pencil_s = std::clamp(px * 0.26f, D(18), D(34));
            if (hover == (int) UiHit::Name) {
                RectF ib(name_right + px * 0.16f, name_base - px * 0.36f - pencil_s * 0.5f, pencil_s, pencil_s);
                icon_pencil(g, ib, c_soft, (std::max)(D(1.6f), pencil_s * 0.09f));
            }
            name_rc_ = {(LONG) x, (LONG) (name_base - px * 0.74f), (LONG) (name_right + px * 0.16f + pencil_s),
                        (LONG) (name_base + px * 0.22f)};
            edit_rc_ = name_rc_;
        }
        if (tagline) {
            if (real) {
                g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
            }
            Font tag_f(face_text(), D(17), FontStyleRegular, UnitPixel);
            draw_line(g,
                      mood == Mood::Fail ? L"AirPlay could not start. Open the log from the settings menu."
                                         : L"Ready to mirror your iPad or iPhone",
                      tag_f, x, bottom, col_w, c_soft);
        }
    };

    // The ground, the leaves and the shadows under them and under the type: the slow part, and the same whatever
    // the mouse is over, so it is kept and only redone when the window size, state or text changes.
    std::wstring bkey;
    {
        wchar_t head[96];
        swprintf_s(head, L"%d|%d|%d|%d|%d|", w, h, dpi_, (int) mood, (int) view.dark);
        bkey = head + view.name + L'\x1f' + view.pin;
    }
    const size_t npx = (size_t) w * (size_t) h;
    if (bkey == base_key_ && base_.size() == npx) {
        memcpy(bits, base_.data(), npx * 4);
    } else {
        fill_field(bits, w, h, f, 0.0);
        Color shade((BYTE) (f.bottom.GetR() / 3), (BYTE) (f.bottom.GetG() / 3), (BYTE) (f.bottom.GetB() / 3));
        cast_shadow(bits, w, h, shade, D(5), D(18), 0.6f, [&](Graphics &mg) {
            draw_leaves(mg, w, h, view.dark, art);
            field_text(mg, false);
        });
        {
            Graphics g(cache_dc_);
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
            draw_leaves(g, w, h, view.dark, art);
            // The leaves throw a second, tighter shadow of their own onto anything drawn after: none needed, the
            // glass controls sit above them.
        }
        GdiFlush();
        try {
            base_.assign(bits, bits + npx);
            base_key_ = bkey;
        } catch (...) {
            base_.clear();
            base_key_.clear();
        }
    }

    Graphics g(cache_dc_);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);

    // Glass: a faint cream fill, a brighter top edge and a hairline border, so controls float on the field.
    auto glass = [&](const RectF &r, float rad, int fill_a, int edge_a) {
        fill_round(g, r, rad, Color((BYTE) fill_a, 255, 246, 228));
        GraphicsPath p;
        add_round_rect(p, r, rad);
        RectF top(r.X, r.Y, r.Width, r.Height * 0.55f);
        LinearGradientBrush sheen(top, Color(30, 255, 250, 238), Color(0, 255, 250, 238), 90.f);
        g.SetClip(&p);
        g.FillRectangle(&sheen, top);
        g.ResetClip();
        draw_round(g, r, rad, Color((BYTE) edge_a, 255, 246, 228), D(1));
    };

    // State pill, top left: a lit dot says what's happening before any word is read. The dot itself is drawn
    // each frame (it breathes); here only its place is set.
    {
        Font state_f(face_strong(), D(14), FontStyleRegular, UnitPixel);
        std::wstring s = !view.pin.empty() ? L"Pairing" : view.status;
        float pill_h = D(34);
        float tw = (std::min)(width_of(g, s, state_f), avail - D(140));
        RectF pill(x, m - D(4), D(14) + D(10) + D(10) + tw + D(16), pill_h);
        glass(pill, pill_h * 0.5f, 40, 72);
        Color dot = mood == Mood::Fail   ? Color(255, 238, 124, 98)
                    : mood == Mood::Pair ? Color(255, 246, 196, 116)
                    : mood == Mood::Live ? Color(255, 128, 232, 172)
                                         : Color(255, 190, 228, 160);
        dot_x_ = pill.X + D(14) + D(5);
        dot_y_ = pill.Y + pill_h * 0.5f;
        dot_r_ = D(5);
        dot_argb_ = dot.GetValue();
        draw_line(g, s, state_f, dot_x_ + D(5) + D(10), dot_y_ + D(14) * 0.36f, tw + 2, c_label);
    }
    breathe_ = mood == Mood::Wait && drift_allowed();

    show_logo_ = art;
    float_logo_ = art && mood == Mood::Wait && drift_allowed();
    logo_x_ = logo.X;
    logo_y_ = logo.Y;
    logo_s_ = ls;
    glow_argb_ = view.dark ? Color(255, 120, 170, 120).GetValue() : Color(255, 255, 236, 196).GetValue();
    if (art) {
        // Shortcuts and the PC's address under the logo, small enough to ignore until needed.
        Font hint_f(face_text(), D(13), FontStyleRegular, UnitPixel);
        Color faint(176, 246, 236, 216);
        const wchar_t *keys = L"Ctrl+R record   ·   Ctrl+M mic   ·   Ctrl+Alt+L laser   ·   F11 full screen";
        float base = logo.Y + ls * 0.98f + D(18);
        float kw = (std::min)(width_of(g, keys, hint_f), avail - col_w);
        draw_line(g, keys, hint_f, icx - kw * 0.5f, base, avail - col_w, faint);
        if (!view.detail.empty()) {
            float dw = (std::min)(width_of(g, view.detail, hint_f), avail - col_w);
            draw_line(g, view.detail, hint_f, icx - dw * 0.5f, base + D(22), avail - col_w, faint);
        }
    }

    if (steps) {
        // How to connect, as three numbered chips in the order you tap them.
        Font chip_f(face_strong(), D(14), FontStyleRegular, UnitPixel);
        Font num_f(face_strong(), D(12), FontStyleRegular, UnitPixel);
        const std::wstring labels[3] = {L"Open Control Center", L"Screen Mirroring", view.name};
        float circle = D(22);
        float pad = D(12);
        float gapc = D(26);
        float lw[3], total = 0;
        for (int i = 0; i < 3; ++i) {
            lw[i] = width_of(g, labels[i], chip_f);
            total += pad + circle + D(10) + lw[i] + pad * 1.4f;
        }
        total += gapc * 2;
        float shrink = total > col_w ? (col_w - gapc * 2) / (total - gapc * 2) : 1.f;
        float cx = x;
        for (int i = 0; i < 3; ++i) {
            float cw = (pad + circle + D(10) + lw[i] + pad * 1.4f) * shrink;
            RectF chip(cx, steps_y, cw, chip_h);
            glass(chip, D(16), 30, 60);
            float ccy = steps_y + chip_h * 0.5f;
            RectF circ(chip.X + pad, ccy - circle * 0.5f, circle, circle);
            fill_round(g, circ, circle * 0.5f, Color(240, 250, 244, 232));
            std::wstring n(1, (wchar_t) (L'1' + i));
            float nw = width_of(g, n, num_f);
            draw_line(g, n, num_f, circ.X + (circle - nw) * 0.5f, ccy + D(12) * 0.36f, circle, f.ink);
            draw_line(g, labels[i], chip_f, circ.GetRight() + D(10), ccy + D(14) * 0.36f,
                      chip.GetRight() - circ.GetRight() - D(10) - pad * 0.6f, c_label);
            cx += cw;
            if (i < 2) {
                chevron(g, cx + gapc * 0.5f - D(2), ccy, D(4), c_soft, D(1.6f));
                cx += gapc;
            }
        }
    }

    if (controls) {
        // Rename | Full Screen, Always on Top, Require PIN | Laser. The laser sits apart at the end: it is for
        // teaching, the others are about the window.
        const int kN = 5;
        const wchar_t *labels[kN] = {L"Rename", L"Full Screen", L"Always on Top", L"Require PIN", L"Laser"};
        bool on[kN] = {false, view.fullscreen, view.always_on_top, view.require_pin, view.laser};
        Font btn_f(face_strong(), D(13), FontStyleRegular, UnitPixel);
        float gap = D(8);
        float split = D(18);
        float icon_s = D(16);
        float icon_gap = D(8);
        float label_w[kN];
        for (int i = 0; i < kN; ++i) {
            label_w[i] = width_of(g, labels[i], btn_f);
        }
        // Icons and words; just words; or, when even that is too wide, round icon buttons (the tooltips of the
        // window still say what each does).
        int mode = 0;
        auto button_w = [&](int i, int md) {
            if (md == 2) {
                return row_h;
            }
            float pad = md == 0 ? D(14) : D(10);
            return pad * 2 + (md == 0 || i == 4 ? icon_s + icon_gap : 0) + label_w[i];
        };
        auto row_width = [&](int md) {
            float tw = gap * 2 + split * 2;
            for (int i = 0; i < kN; ++i) {
                tw += button_w(i, md);
            }
            return tw;
        };
        while (mode < 2 && row_width(mode) > col_w) {
            ++mode;
        }

        float bx = x;
        for (int i = 0; i < kN; ++i) {
            float bw = button_w(i, mode);
            RectF br(bx, row_y, bw, row_h);
            bool hot = hover == (int) UiHit::Rename + i;
            float rad = row_h * 0.5f;
            Color ink = c_label;
            if (i == 4 && on[i]) {
                // The laser on: lit red, with a soft glow, so it is plain from across the room that it's live.
                for (int k = 3; k >= 1; --k) {
                    float e = D(3) * (float) k;
                    RectF gr(br.X - e, br.Y - e, br.Width + e * 2, br.Height + e * 2);
                    fill_round(g, gr, rad + e, Color((BYTE) (26 / k), 255, 80, 64));
                }
                fill_round_grad(g, br, rad, hot ? Color(255, 248, 104, 88) : Color(255, 236, 88, 70),
                                Color(255, 196, 52, 44), 90.f);
                ink = Color(255, 255, 248, 240);
            } else if (on[i]) {
                // On floods the whole control in cream: readable as filled, not by hue.
                fill_round(g, br, rad, hot ? Color(236, 250, 244, 232) : c_label);
                ink = f.ink;
            } else if (i == 0) {
                if (hot) {
                    fill_round(g, br, rad, Color(40, 255, 246, 228));
                }
                draw_round(g, br, rad, Color(140, 250, 244, 232), D(1.25f));
            } else {
                glass(br, rad, hot ? 70 : 38, hot ? 120 : 70);
            }
            float pad = mode == 0 ? D(14) : D(10);
            float cx = br.X + pad;
            if (mode != 1 || i == 4) { // the laser keeps its icon: it should be found at a glance
                if (mode == 2) {
                    cx = br.X + (bw - icon_s) * 0.5f;
                }
                RectF ib(cx, row_y + (row_h - icon_s) * 0.5f, icon_s, icon_s);
                float sw = D(1.6f);
                if (i == 0) {
                    icon_pencil(g, ib, ink, sw);
                } else if (i == 1) {
                    icon_expand(g, ib, ink, sw);
                } else if (i == 2) {
                    icon_pin(g, ib, ink, sw);
                } else if (i == 3) {
                    icon_lock(g, ib, ink, sw);
                } else {
                    icon_laser(g, ib, i == 4 && !on[i] ? Color(255, 255, 132, 112) : ink, sw);
                }
                cx += icon_s + icon_gap;
            }
            if (mode != 2) {
                draw_line(g, labels[i], btn_f, cx, row_y + row_h * 0.5f + D(13) * 0.35f, label_w[i] + 2, ink);
            }
            btn_rc_[i] = {(LONG) br.X, (LONG) br.Y, (LONG) br.GetRight(), (LONG) br.GetBottom()};
            bx += bw + (i == 0 || i == 3 ? split : gap);
        }

        // Dark mode: a round tile in the top-right corner, level with the state pill, flooded when on.
        float ds = D(38);
        RectF dr((float) w - m - ds, m - D(4) + (D(34) - ds) * 0.5f, ds, ds);
        bool dark_hot = hover == (int) UiHit::Dark;
        if (view.dark) {
            fill_round(g, dr, ds * 0.5f, dark_hot ? Color(236, 250, 244, 232) : c_label);
        } else {
            glass(dr, ds * 0.5f, dark_hot ? 80 : 46, dark_hot ? 130 : 80);
        }
        icon_moon(g, RectF(dr.X + (ds - icon_s) * 0.5f, dr.Y + (ds - icon_s) * 0.5f, icon_s, icon_s),
                  view.dark ? f.ink : c_label, D(1.6f));
        dark_rc_ = {(LONG) dr.X, (LONG) dr.Y, (LONG) dr.GetRight(), (LONG) dr.GetBottom()};

        // A newer version: a cream pill beside the dark-mode tile that opens its release page.
        if (!view.update.empty()) {
            Font up_f(face_strong(), D(13), FontStyleRegular, UnitPixel);
            std::wstring text = L"Update to " + view.update;
            float tw = width_of(g, text, up_f);
            float uh = D(34);
            float uw = D(14) + D(14) + D(8) + tw + D(16);
            RectF ur(dr.X - D(10) - uw, m - D(4), uw, uh);
            if (ur.X > x + D(260)) { // never over the state pill
                bool hot = hover == (int) UiHit::Update;
                fill_round(g, ur, uh * 0.5f, hot ? Color(255, 255, 252, 242) : c_label);
                // An up arrow in a circle, in the field's ink.
                float ac = ur.X + D(14) + D(7), am = ur.Y + uh * 0.5f;
                Pen ap(f.ink, D(1.7f));
                round_pen(ap);
                g.DrawEllipse(&ap, ac - D(7), am - D(7), D(14), D(14));
                g.DrawLine(&ap, ac, am + D(3.5f), ac, am - D(3.5f));
                g.DrawLine(&ap, ac - D(3), am - D(0.5f), ac, am - D(3.5f));
                g.DrawLine(&ap, ac + D(3), am - D(0.5f), ac, am - D(3.5f));
                draw_line(g, text, up_f, ac + D(7) + D(8), am + D(13) * 0.36f, tw + 2, f.ink);
                update_rc_ = {(LONG) ur.X, (LONG) ur.Y, (LONG) ur.GetRight(), (LONG) ur.GetBottom()};
            }
        }
    }

    field_text(g, true);

    // What the animation timer repaints, each on its own (one far corner to the other would be most of the
    // window): the dot with its halo, and the logo with its glow and float.
    RECT all = {0, 0, w, h};
    anim_rc_[0] = anim_rc_[1] = {};
    if (breathe_ && dot_r_ > 0) {
        float e = dot_r_ * 2.f + D(2);
        RECT a = {(LONG) (dot_x_ - e), (LONG) (dot_y_ - e), (LONG) (dot_x_ + e) + 1, (LONG) (dot_y_ + e) + 1};
        IntersectRect(&anim_rc_[0], &a, &all);
    }
    if (float_logo_) {
        RECT l = {(LONG) (logo_x_ - ls * 0.04f), (LONG) (logo_y_ - D(8)), (LONG) (logo_x_ + ls * 1.04f) + 1,
                  (LONG) (logo_y_ + ls + D(8)) + 1};
        IntersectRect(&anim_rc_[1], &l, &all);
    }
    animating_ = !IsRectEmpty(&anim_rc_[0]) || !IsRectEmpty(&anim_rc_[1]);
}

UiHit ShellUi::hit_test(POINT pt) const {
    if (PtInRect(&name_rc_, pt)) {
        return UiHit::Name;
    }
    static const UiHit order[5] = {UiHit::Rename, UiHit::Fullscreen, UiHit::Topmost, UiHit::Pin, UiHit::Laser};
    for (int i = 0; i < 5; ++i) {
        if (PtInRect(&btn_rc_[i], pt)) {
            return order[i];
        }
    }
    if (PtInRect(&dark_rc_, pt)) {
        return UiHit::Dark;
    }
    if (PtInRect(&update_rc_, pt)) {
        return UiHit::Update;
    }
    return UiHit::None;
}

bool ShellUi::cursor_hand(UiHit hit) const {
    return hit != UiHit::None;
}

} // namespace airscreen
