/* Laser pointer: a small layered, click-through, topmost window that is moved to wherever the mouse is and
 * redrawn about 60 times a second while it moves. Only the box around the dot and its trail is drawn, so it
 * stays cheap on a 4K display. */

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shellscalingapi.h>

#include "laser.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shcore.lib")

namespace airscreen {
namespace {

const wchar_t kClass[] = L"AirScreenLaser";
const UINT_PTR kTimer = 1;
const DWORD kTrailMs = 280;  // how long the trail lingers behind the dot
const int kJump = 900;       // a move this far in one tick (another display, a jump) starts a fresh trail
const int kMaxBox = 3000;

struct Rgb {
    BYTE r, g, b;
};
const Rgb kColors[] = {{255, 38, 38}, {30, 230, 90}, {40, 150, 255}, {255, 210, 0}};
const float kRadius[] = {5.0f, 7.5f, 11.0f}; // core radius at 96 dpi

UINT dpi_at(POINT p) {
    UINT x = 96, y = 96;
    HMONITOR mon = MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST);
    if (FAILED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &x, &y)) || !x) {
        x = 96;
    }
    return x;
}

} // namespace

void LaserPointer::set_style(int color, int size) {
    color_ = (std::max)(0, (std::min)(color, 3));
    size_ = (std::max)(0, (std::min)(size, 2));
    still_drawn_ = false;
}

bool LaserPointer::create(HINSTANCE inst) {
    if (hwnd_) {
        return true;
    }
    WNDCLASSEXW wc = {sizeof(wc)};
    if (!GetClassInfoExW(inst, kClass, &wc)) {
        wc = {sizeof(wc)};
        wc.lpfnWndProc = proc;
        wc.hInstance = inst;
        wc.lpszClassName = kClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);
    }
    // Layered + transparent: clicks and the mouse go straight through to whatever is underneath.
    hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                            kClass, L"AirScreen laser pointer", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, this);
    return hwnd_ != nullptr;
}

void LaserPointer::set_on(HINSTANCE inst, bool on) {
    if (on == on_) {
        return;
    }
    if (on && !create(inst)) {
        return;
    }
    on_ = on;
    trail_.clear();
    still_drawn_ = false;
    if (on) {
        tick();
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        SetTimer(hwnd_, kTimer, 15, nullptr);
    } else if (hwnd_) {
        KillTimer(hwnd_, kTimer);
        ShowWindow(hwnd_, SW_HIDE);
    }
}

void LaserPointer::destroy() {
    on_ = false;
    if (hwnd_) {
        KillTimer(hwnd_, kTimer);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (dc_) {
        SelectObject(dc_, old_);
        DeleteDC(dc_);
        dc_ = nullptr;
    }
    if (bmp_) {
        DeleteObject(bmp_);
        bmp_ = nullptr;
    }
    bits_ = nullptr;
    cap_w_ = cap_h_ = 0;
}

LRESULT CALLBACK LaserPointer::proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR) ((CREATESTRUCTW *) lp)->lpCreateParams);
    }
    auto *self = (LaserPointer *) GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_TIMER:
        if (self && wp == kTimer) {
            self->tick();
        }
        return 0;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool LaserPointer::ensure_buffer(int w, int h) {
    if (bits_ && w <= cap_w_ && h <= cap_h_) {
        return true;
    }
    int cw = (std::max)(w, cap_w_), ch = (std::max)(h, cap_h_);
    cw = (cw + 63) & ~63;
    ch = (ch + 63) & ~63;
    if (!dc_) {
        dc_ = CreateCompatibleDC(nullptr);
        if (!dc_) {
            return false;
        }
    }
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = cw;
    bi.bmiHeader.biHeight = -ch; // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc_, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) {
        return false;
    }
    if (bmp_) {
        SelectObject(dc_, old_);
        DeleteObject(bmp_);
    }
    old_ = SelectObject(dc_, bmp);
    bmp_ = bmp;
    bits_ = bits;
    cap_w_ = cw;
    cap_h_ = ch;
    return true;
}

void LaserPointer::tick() {
    if (!on_ || !hwnd_) {
        return;
    }
    POINT p;
    if (!GetCursorPos(&p)) {
        return;
    }
    DWORD now = GetTickCount();
    if (trail_.empty() || trail_.back().pt.x != p.x || trail_.back().pt.y != p.y) {
        if (!trail_.empty() && (std::abs(trail_.back().pt.x - p.x) > kJump || std::abs(trail_.back().pt.y - p.y) > kJump)) {
            trail_.clear();
        }
        trail_.push_back({p, now});
        still_drawn_ = false;
    } else {
        trail_.back().t = now; // the dot itself never fades
    }
    while (trail_.size() > 1 && now - trail_.front().t > kTrailMs) {
        trail_.pop_front();
    }
    // Every second or so, go back above windows that took the top since (the control panel, menus).
    if (++frames_ % 60 == 0) {
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    if (trail_.size() == 1 && still_drawn_) {
        return; // nothing has changed on screen
    }

    const float scale = dpi_at(p) / 96.0f;
    const float r = kRadius[size_] * scale; // the bright core
    const float glow = r * 2.6f;            // the soft halo around it
    const int margin = (int) std::ceil(glow) + 2;

    LONG minx = p.x, miny = p.y, maxx = p.x, maxy = p.y;
    for (auto &q : trail_) {
        minx = (std::min)(minx, q.pt.x);
        miny = (std::min)(miny, q.pt.y);
        maxx = (std::max)(maxx, q.pt.x);
        maxy = (std::max)(maxy, q.pt.y);
    }
    int left = minx - margin, top = miny - margin;
    int w = (int) (maxx - minx) + margin * 2, h = (int) (maxy - miny) + margin * 2;
    if (w > kMaxBox || h > kMaxBox) {
        trail_.erase(trail_.begin(), trail_.end() - 1);
        left = p.x - margin, top = p.y - margin;
        w = h = margin * 2;
    }
    if (!ensure_buffer(w, h)) {
        return;
    }
    const int stride = cap_w_ * 4;
    for (int y = 0; y < h; ++y) {
        memset((BYTE *) bits_ + (size_t) y * stride, 0, (size_t) w * 4);
    }

    const Rgb c = kColors[color_];
    {
        Gdiplus::Bitmap canvas(cap_w_, cap_h_, stride, PixelFormat32bppPARGB, (BYTE *) bits_);
        Gdiplus::Graphics g(&canvas);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        g.SetClip(Gdiplus::Rect(0, 0, w, h));
        g.TranslateTransform((float) -left, (float) -top);

        // Trail: newer segments wider and brighter, tapering away behind the dot.
        for (size_t i = 1; i < trail_.size(); ++i) {
            DWORD age = now - trail_[i - 1].t;
            float a = 1.0f - (std::min)(1.0f, age / (float) kTrailMs);
            if (a <= 0.02f) {
                continue;
            }
            Gdiplus::Pen pen(Gdiplus::Color((BYTE) (a * 190), c.r, c.g, c.b), r * 2.0f * (0.25f + 0.75f * a));
            // Flat ends: round ones overlap at every join and leave darker beads along the trail.
            pen.SetStartCap(Gdiplus::LineCapFlat);
            pen.SetEndCap(Gdiplus::LineCapFlat);
            pen.SetLineJoin(Gdiplus::LineJoinRound);
            g.DrawLine(&pen, (float) trail_[i - 1].pt.x, (float) trail_[i - 1].pt.y, (float) trail_[i].pt.x,
                       (float) trail_[i].pt.y);
        }

        const float cx = (float) p.x, cy = (float) p.y;
        Gdiplus::GraphicsPath halo;
        halo.AddEllipse(cx - glow, cy - glow, glow * 2, glow * 2);
        Gdiplus::PathGradientBrush hb(&halo);
        hb.SetCenterPoint(Gdiplus::PointF(cx, cy));
        hb.SetCenterColor(Gdiplus::Color(150, c.r, c.g, c.b));
        Gdiplus::Color edge(0, c.r, c.g, c.b);
        INT n = 1;
        hb.SetSurroundColors(&edge, &n);
        g.FillPath(&hb, &halo);

        Gdiplus::SolidBrush core(Gdiplus::Color(255, c.r, c.g, c.b));
        g.FillEllipse(&core, cx - r, cy - r, r * 2, r * 2);
        const float hot = r * 0.45f; // a white-hot centre, like a real laser spot
        Gdiplus::SolidBrush centre(Gdiplus::Color(220, 255, 255, 255));
        g.FillEllipse(&centre, cx - hot, cy - hot, hot * 2, hot * 2);
        g.Flush(Gdiplus::FlushIntentionSync);
    }
    GdiFlush();

    POINT dst = {left, top};
    SIZE sz = {w, h};
    POINT src = {0, 0};
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(hwnd_, nullptr, &dst, &sz, dc_, &src, 0, &bf, ULW_ALPHA);
    still_drawn_ = trail_.size() == 1;
}

} // namespace airscreen
