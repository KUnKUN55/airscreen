#include "control.h"
#include "settings.h"

#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <sstream>

#pragma comment(lib, "gdiplus.lib")

namespace airscreen {
namespace {

constexpr int kScene = 100; // + scene index
constexpr int kQuick = 300; // + index into the quick buttons
constexpr int kRec = 400, kPause = 401, kStop = 402, kPreview = 403, kEdit = 404, kMore = 405, kLaser = 406;
constexpr UINT kTimerTick = 1;

enum : UINT {
    kMenuEditText = 1,
    kMenuSideBySide,
    kMenuPipTL,
    kMenuPipTR,
    kMenuPipBL,
    kMenuPipBR,
    kMenuSwap,
    kMenuSecondary,
    kMenuChoose,
    kMenuStopComputer,
    kMenuKeep,
    kMenuCompact,
    kMenuTop,
    kMenuSettings,
    kMenuHide,
    kMenuLaser,
    kMenuLaserColor = 60, // + color index
    kMenuLaserSize = 70,  // + size index
    kMenuSnippet = 100,  // + snippet index
    kMenuProfile = 300,  // + profile index
    kMenuApp = 40000,    // and up: the host's app_menu(), handled by the main window
};

// Forest and cream: a warm paper ground, soft raised cream buttons, deep forest green for what is on, terracotta
// for recording and a lit red for the laser. Calm enough to sit beside slides all lesson.
const COLORREF kBgTop = RGB(246, 241, 232);
const COLORREF kBgBottom = RGB(234, 226, 211);
const COLORREF kCard = RGB(252, 249, 243);
const COLORREF kEdge = RGB(222, 210, 190);
const COLORREF kPlate = RGB(250, 246, 238);
const COLORREF kPlateHot = RGB(255, 252, 246);
const COLORREF kText = RGB(38, 52, 43);
const COLORREF kDim = RGB(128, 114, 94);
const COLORREF kLabel = RGB(150, 116, 82); // section headings: bark brown
const COLORREF kOff = RGB(182, 172, 156);
const COLORREF kAccent = RGB(46, 104, 78);
const COLORREF kAccentHot = RGB(60, 126, 95);
const COLORREF kOnText = RGB(250, 244, 232);
const COLORREF kLive = RGB(64, 164, 104);
const COLORREF kRecRed = RGB(188, 70, 48);
const COLORREF kAmber = RGB(200, 138, 44);
const COLORREF kLaserRed = RGB(222, 64, 52);

Gdiplus::Color gc(COLORREF c, BYTE a = 255) {
    return Gdiplus::Color(a, GetRValue(c), GetGValue(c), GetBValue(c));
}

COLORREF shade(COLORREF c, int pct) {
    return RGB(GetRValue(c) * pct / 100, GetGValue(c) * pct / 100, GetBValue(c) * pct / 100);
}

void round_path(Gdiplus::GraphicsPath &p, Gdiplus::RectF r, float rad) {
    float d = (std::min)({rad * 2.f, r.Width, r.Height});
    if (d < 1.f) {
        p.AddRectangle(r);
        return;
    }
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    p.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    p.CloseFigure();
}

void fill_rr(Gdiplus::Graphics &g, Gdiplus::RectF r, float rad, const Gdiplus::Brush &b) {
    Gdiplus::GraphicsPath p;
    round_path(p, r, rad);
    g.FillPath(&b, &p);
}

// A soft shadow under a raised shape: a few widening, fading copies dropped a little.
void soft_shadow(Gdiplus::Graphics &g, Gdiplus::RectF r, float rad, float unit, Gdiplus::Color tint) {
    for (int i = 4; i >= 1; --i) {
        float e = unit * (float) i;
        Gdiplus::RectF s(r.X - e * 0.5f, r.Y + e * 0.45f, r.Width + e, r.Height + e * 0.6f);
        Gdiplus::SolidBrush b(Gdiplus::Color((BYTE) (tint.GetA() / (i + 1)), tint.GetR(), tint.GetG(), tint.GetB()));
        fill_rr(g, s, rad + e * 0.5f, b);
    }
}

// A laser pointer: a short beam ending in a lit spot with three rays.
void draw_laser_icon(Gdiplus::Graphics &g, Gdiplus::RectF b, Gdiplus::Color c, float w) {
    Gdiplus::Pen p(c, w);
    p.SetStartCap(Gdiplus::LineCapRound);
    p.SetEndCap(Gdiplus::LineCapRound);
    auto P = [&](float u, float v) { return Gdiplus::PointF(b.X + u * b.Width, b.Y + v * b.Height); };
    g.DrawLine(&p, P(0.12f, 0.88f), P(0.52f, 0.48f));
    Gdiplus::SolidBrush spot(c);
    float r = 0.17f * b.Width;
    Gdiplus::PointF s = P(0.68f, 0.32f);
    g.FillEllipse(&spot, s.X - r, s.Y - r, r * 2, r * 2);
    g.DrawLine(&p, P(0.68f, 0.02f), P(0.68f, 0.09f));
    g.DrawLine(&p, P(0.98f, 0.32f), P(0.91f, 0.32f));
    g.DrawLine(&p, P(0.90f, 0.10f), P(0.85f, 0.15f));
}

std::wstring clock_text(double s) {
    int t = (int) s;
    wchar_t b[32];
    swprintf_s(b, L"%02d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
    return b;
}

HFONT make_font(int px, int weight) {
    return CreateFontW(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}

} // namespace

bool ControlPanel::create(HINSTANCE inst, TeachHost *host) {
    inst_ = inst;
    host_ = host;
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = (HICON) GetClassLongPtrW(host->main_window(), GCLP_HICON);
    wc.hIconSm = (HICON) GetClassLongPtrW(host->main_window(), GCLP_HICONSM);
    wc.lpszClassName = L"AirScreenControl";
    RegisterClassExW(&wc);

    Settings &s = host->settings();
    compact_ = s.panel_compact;
    UINT sys_dpi = GetDpiForSystem();
    RECT r = {CW_USEDEFAULT, CW_USEDEFAULT, 0, 0};
    bool saved = s.panel_w > 0 && s.panel_h > 0;
    hwnd_ = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, L"AirScreen · Teaching controls",
                            WS_OVERLAPPEDWINDOW, r.left, r.top, MulDiv(372, sys_dpi, 96), MulDiv(440, sys_dpi, 96),
                            nullptr, nullptr, inst, this);
    if (!hwnd_) {
        return false;
    }
    dpi_ = (int) GetDpiForWindow(hwnd_);
    // A light title bar in the panel's own cream (Windows 11 takes the colours; Windows 10 keeps its default).
    BOOL dark = FALSE;
    DwmSetWindowAttribute(hwnd_, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    COLORREF caption = kBgTop, caption_text = kText;
    DwmSetWindowAttribute(hwnd_, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof(caption));
    DwmSetWindowAttribute(hwnd_, 36 /* DWMWA_TEXT_COLOR */, &caption_text, sizeof(caption_text));
    if (saved) {
        RECT want = {s.panel_x, s.panel_y, s.panel_x + s.panel_w, s.panel_y + s.panel_h};
        RECT fit = visible_rect(want, dpi(280), dpi(120));
        SetWindowPos(hwnd_, nullptr, fit.left, fit.top, fit.right - fit.left, fit.bottom - fit.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        // A size saved by an older version may be too short for this layout (the laser row, the taller status
        // card): grow it so the last row of buttons is never cut off.
        RECT cr;
        GetClientRect(hwnd_, &cr);
        HDC dc = GetDC(hwnd_);
        layout(dc, cr.right);
        ReleaseDC(hwnd_, dc);
        if (cr.bottom < content_h_) {
            fit_height();
        }
    } else {
        // First time: the top-right corner of the main window's screen, out of the way of the content.
        MONITORINFO mi = {sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(host->main_window(), MONITOR_DEFAULTTONEAREST), &mi);
        RECT wr;
        GetWindowRect(hwnd_, &wr);
        int w = wr.right - wr.left;
        SetWindowPos(hwnd_, nullptr, mi.rcWork.right - w - dpi(24), mi.rcWork.top + dpi(24), 0, 0,
                     SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE);
        fit_height();
    }
    tips_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0, 0,
                            0, hwnd_, nullptr, inst, nullptr);
    if (tips_) {
        SendMessageW(tips_, TTM_SETMAXTIPWIDTH, 0, dpi(320));
    }
    apply_prefs();
    return true;
}

void ControlPanel::destroy() {
    if (hwnd_) {
        save_rect();
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

void ControlPanel::show(bool activate) {
    if (!hwnd_) {
        return;
    }
    if (IsIconic(hwnd_)) {
        ShowWindow(hwnd_, activate ? SW_RESTORE : SW_SHOWNOACTIVATE);
    } else {
        ShowWindow(hwnd_, activate ? SW_SHOW : SW_SHOWNA);
    }
    if (activate) {
        SetForegroundWindow(hwnd_);
    }
    apply_prefs();
    SetTimer(hwnd_, kTimerTick, 500, nullptr);
    refresh();
}

void ControlPanel::hide() {
    if (!hwnd_) {
        return;
    }
    save_rect();
    KillTimer(hwnd_, kTimerTick);
    ShowWindow(hwnd_, SW_HIDE);
}

void ControlPanel::toggle() {
    if (visible()) {
        hide();
    } else {
        show(false);
    }
}

bool ControlPanel::visible() const {
    return hwnd_ && IsWindowVisible(hwnd_);
}

void ControlPanel::refresh() {
    if (visible()) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void ControlPanel::apply_prefs() {
    if (!hwnd_) {
        return;
    }
    Settings &s = host_->settings();
    SetWindowPos(hwnd_, s.panel_topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    hide_from_capture(hwnd_, s.hide_controls);
    if (s.panel_compact != compact_) {
        compact_ = s.panel_compact;
        fit_height();
    }
    refresh();
}

void ControlPanel::save_rect() {
    if (!hwnd_) {
        return;
    }
    WINDOWPLACEMENT wp = {sizeof(wp)};
    if (GetWindowPlacement(hwnd_, &wp)) {
        Settings &s = host_->settings();
        RECT r = wp.rcNormalPosition;
        s.panel_x = r.left;
        s.panel_y = r.top;
        s.panel_w = r.right - r.left;
        s.panel_h = r.bottom - r.top;
        host_->save_settings();
    }
}

// Sizes the window to its content (first run, switching compact view).
void ControlPanel::fit_height() {
    RECT cr;
    GetClientRect(hwnd_, &cr);
    HDC dc = GetDC(hwnd_);
    layout(dc, cr.right);
    ReleaseDC(hwnd_, dc);
    RECT wr, want = {0, 0, cr.right, content_h_};
    GetWindowRect(hwnd_, &wr);
    AdjustWindowRectExForDpi(&want, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_APPWINDOW, dpi_);
    SetWindowPos(hwnd_, nullptr, 0, 0, wr.right - wr.left, want.bottom - want.top,
                 SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
}

std::vector<Action> ControlPanel::quick_actions() const {
    std::vector<Action> out;
    std::stringstream ss(host_->settings().quick);
    std::string k;
    while (std::getline(ss, k, ',') && out.size() < 8) {
        Action a = action_from_key(k);
        // The laser has its own button next to recording; a second one among the quick buttons would only crowd.
        if (a != Action::None && a != Action::Laser) {
            out.push_back(a);
        }
    }
    return out;
}

bool ControlPanel::action_lit(Action a, const Scene &s, const TeachStatus &st) const {
    switch (a) {
    case Action::ShowIPad:
        return s.shows(SourceKind::IPad) && !s.shows(SourceKind::Computer);
    case Action::ShowComputer:
        return s.shows(SourceKind::Computer) && !s.shows(SourceKind::IPad);
    case Action::ShowBoth:
        return s.shows(SourceKind::IPad) && s.shows(SourceKind::Computer);
    case Action::ToggleSecondary: {
        int n = 0;
        for (auto &l : s.layers) {
            n += l.type == LayerType::Source && l.visible;
        }
        return n > 1;
    }
    case Action::ToggleText:
    case Action::ShowText:
        return s.shows_text();
    case Action::ToggleBox:
        return s.shows_box();
    case Action::ToggleExtras:
        return s.shows_extras();
    case Action::PauseRecord:
        return st.paused;
    case Action::ToggleRecord:
        return st.recording;
    case Action::Laser:
        return st.laser;
    default:
        return false;
    }
}

void ControlPanel::layout(HDC hdc, int width) {
    buttons_.clear();
    labels_.clear();
    const bool compact = host_->settings().panel_compact;
    const int pad = dpi(12), gap = dpi(6);
    const int inner = (std::max)(dpi(120), width - pad * 2);
    TeachStatus st = host_->status();
    const Scene &live = host_->live_scene();
    Profile &prof = host_->profile();
    int y = pad;
    status_rc_ = {pad, y, width - pad, y + dpi(compact ? 34 : 58)};
    y = status_rc_.bottom + dpi(compact ? 10 : 14);

    HFONT font = make_font(dpi(13), FW_SEMIBOLD);
    HGDIOBJ old = SelectObject(hdc, font);

    // Scenes: chips that wrap; the one on air is white.
    if (!compact) {
        labels_.push_back({{pad, y}, L"SCENES"});
        y += dpi(18);
    }
    int x = pad, chip_h = dpi(30);
    for (size_t i = 0; i < prof.scenes.size(); ++i) {
        std::wstring name = prof.scenes[i].name.empty() ? L"Scene" : prof.scenes[i].name;
        SIZE sz = {};
        GetTextExtentPoint32W(hdc, name.c_str(), (int) name.size(), &sz);
        int w = (std::min)((int) sz.cx + dpi(24), inner);
        if (x > pad && x + w > pad + inner) {
            x = pad;
            y += chip_h + gap;
        }
        Button b;
        b.id = kScene + (int) i;
        b.rc = {x, y, x + w, y + chip_h};
        b.label = name;
        wchar_t tip[160];
        swprintf_s(tip, L"Put \"%s\" on air%s", name.c_str(), i < 9 ? L"" : L"");
        b.tip = tip;
        if (i < 9 && host_->settings().hotkeys) {
            b.tip += L"   (" + hotkey_text(MOD_CONTROL | MOD_ALT, (UINT) ('1' + i)) + L")";
        }
        b.style = Chip;
        b.on = (int) i == host_->live_index();
        buttons_.push_back(b);
        x += w + gap;
    }
    y += chip_h + dpi(compact ? 8 : 14);

    // Quick buttons, as chosen in Settings, four to a row.
    std::vector<Action> quick = quick_actions();
    if (!compact) {
        labels_.push_back({{pad, y}, L"QUICK"});
        y += dpi(18);
    }
    const int bh = dpi(34);
    if (compact) {
        // One row: the first quick buttons, record, the laser and the menu.
        int n = (int) (std::min)(quick.size(), (size_t) 3);
        int cols = n + 3;
        int bw = (inner - gap * (cols - 1)) / cols;
        for (int i = 0; i < n; ++i) {
            Button b;
            b.id = kQuick + i;
            b.rc = {pad + (bw + gap) * i, y, pad + (bw + gap) * i + bw, y + bh};
            b.label = action_label(quick[(size_t) i]);
            b.tip = b.label;
            b.style = Toggle;
            b.on = action_lit(quick[(size_t) i], live, st);
            buttons_.push_back(b);
        }
        Button r;
        r.id = st.recording ? kStop : kRec;
        r.rc = {pad + (bw + gap) * n, y, pad + (bw + gap) * n + bw, y + bh};
        r.label = st.recording ? L"■ Stop" : L"● Rec";
        r.tip = st.recording ? L"Stop recording and save the clip" : L"Record what's on air";
        r.style = Danger;
        r.on = st.recording;
        r.enabled = st.recording || st.ipad || st.computer;
        buttons_.push_back(r);
        Button l;
        l.id = kLaser;
        l.rc = {pad + (bw + gap) * (n + 1), y, pad + (bw + gap) * (n + 1) + bw, y + bh};
        l.label = L"";
        l.tip = st.laser ? L"Turn the laser pointer off   (Ctrl+Alt+L)" : L"Laser pointer   (Ctrl+Alt+L)";
        l.style = Laser;
        l.on = st.laser;
        buttons_.push_back(l);
        Button m;
        m.id = kMore;
        m.rc = {pad + (bw + gap) * (n + 2), y, pad + inner, y + bh};
        m.label = L"⋯";
        m.tip = L"More: text, layouts, computer screen, settings";
        buttons_.push_back(m);
        y += bh;
    } else {
        int cols = 4;
        int bw = (inner - gap * (cols - 1)) / cols;
        for (size_t i = 0; i < quick.size(); ++i) {
            int c = (int) (i % cols);
            if (i > 0 && c == 0) {
                y += bh + gap;
            }
            Button b;
            b.id = kQuick + (int) i;
            b.rc = {pad + (bw + gap) * c, y, pad + (bw + gap) * c + bw, y + bh};
            Action a = quick[i];
            b.label = action_label(a);
            switch (a) { // short names on the buttons; the full ones in the tooltips
            case Action::ShowIPad:
                b.label = L"iPad";
                break;
            case Action::ShowComputer:
                b.label = st.computer || st.computer_wait ? L"Computer" : L"Computer…";
                break;
            case Action::ShowBoth:
                b.label = L"Both";
                break;
            case Action::SwapScreens:
                b.label = L"⇄ Swap";
                break;
            case Action::ToggleSecondary:
                b.label = L"2nd screen";
                break;
            case Action::ToggleText:
                b.label = L"Text";
                break;
            case Action::ToggleBox:
                b.label = L"Text bg";
                break;
            case Action::ToggleExtras:
                b.label = L"Extras";
                break;
            case Action::EditText:
                b.label = L"✎ Text…";
                break;
            case Action::Laser:
                b.label = L"Laser";
                break;
            default:
                break;
            }
            b.tip = action_label(a);
            if (a == Action::ShowComputer && !st.computer && !st.computer_wait) {
                b.tip = L"Choose a display or window of this PC to share";
            }
            b.style = Toggle;
            b.on = action_lit(a, live, st);
            buttons_.push_back(b);
        }
        if (!quick.empty()) {
            y += bh + dpi(14);
        }

        // Recording, and the laser pointer beside it: the two things reached for while actually teaching.
        labels_.push_back({{pad, y}, L"RECORD  ·  POINT"});
        y += dpi(18);
        const int laser_w = (inner - gap * 2) / 3;
        const int rec_inner = inner - laser_w - gap; // what is left for the recording buttons
        {
            Button l;
            l.id = kLaser;
            l.rc = {pad + inner - laser_w, y, pad + inner, y + bh};
            l.label = st.laser ? L"Laser on" : L"Laser";
            l.tip = st.laser ? L"Turn the laser pointer off   (Ctrl+Alt+L)"
                             : L"A glowing dot follows the mouse on every display, for pointing while you teach   "
                               L"(Ctrl+Alt+L).  Colour and size: ⋯ → Laser pointer";
            l.style = Laser;
            l.on = st.laser;
            buttons_.push_back(l);
        }
        if (st.recording) {
            int half = (rec_inner - gap) / 2;
            Button p;
            p.id = kPause;
            p.rc = {pad, y, pad + half, y + bh};
            p.label = st.paused ? L"▶ Resume" : L"❚❚ Pause";
            p.tip = st.paused ? L"Carry on recording" : L"Pause the clip; nothing is recorded until you resume";
            p.style = Toggle;
            p.on = st.paused;
            buttons_.push_back(p);
            Button s;
            s.id = kStop;
            s.rc = {pad + half + gap, y, pad + rec_inner, y + bh};
            s.label = L"■ Stop";
            s.tip = L"Stop recording and save the clip";
            s.style = Danger;
            s.on = true;
            buttons_.push_back(s);
        } else {
            Button r;
            r.id = kRec;
            r.rc = {pad, y, pad + rec_inner, y + bh};
            r.label = L"● Record";
            r.tip = st.ipad || st.computer ? L"Record what's on air to the Videos\\AirScreen folder"
                                           : L"Share the iPad or the computer screen first";
            r.style = Danger;
            r.enabled = st.ipad || st.computer;
            buttons_.push_back(r);
        }
        y += bh + dpi(14);

        // Less frequent things.
        int third = (inner - gap * 2) / 3;
        const struct {
            int id;
            const wchar_t *label, *tip;
        } bottom[3] = {{kPreview, L"Preview", L"See what's on air, a scene, or a screen before showing it"},
                       {kEdit, L"Edit scenes", L"Arrange screens, text, logos and frames in each scene"},
                       {kMore, L"⋯", L"More: text, layouts, computer screen, settings"}};
        for (int i = 0; i < 3; ++i) {
            Button b;
            b.id = bottom[i].id;
            int l = pad + (third + gap) * i;
            b.rc = {l, y, i == 2 ? pad + inner : l + third, y + bh};
            b.label = bottom[i].label;
            b.tip = bottom[i].tip;
            buttons_.push_back(b);
        }
        y += bh;
    }
    content_h_ = y + pad;
    SelectObject(hdc, old);
    DeleteObject(font);
}

void ControlPanel::paint(HDC target) {
    RECT cr;
    GetClientRect(hwnd_, &cr);
    if (cr.right <= 0 || cr.bottom <= 0) {
        return;
    }
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bmp = dc ? CreateCompatibleBitmap(target, cr.right, cr.bottom) : nullptr;
    if (!bmp) {
        if (dc) {
            DeleteDC(dc);
        }
        return;
    }
    HGDIOBJ old_bmp = SelectObject(dc, bmp);
    layout(dc, cr.right);
    sync_tips();
    SetBkMode(dc, TRANSPARENT);
    const bool compact = host_->settings().panel_compact;
    TeachStatus st = host_->status();

    HFONT big = make_font(dpi(compact ? 13 : 15), FW_SEMIBOLD);
    HFONT sub = make_font(dpi(12), FW_NORMAL);
    HFONT label = make_font(dpi(10), FW_BOLD);
    HFONT btn = make_font(dpi(13), FW_SEMIBOLD);
    bool on_air = st.ipad || st.computer;
    COLORREF dot = on_air ? kLive : (st.computer_wait ? kAmber : kOff);
    const int inset = dpi(compact ? 10 : 14); // text inside the status card
    const int ds = dpi(10);

    // Where each laser icon goes, worked out with the button font so icon and word centre together.
    std::vector<Gdiplus::RectF> icons(buttons_.size());
    std::vector<char> worded(buttons_.size(), 0);
    {
        HGDIOBJ of = SelectObject(dc, btn);
        float is = (float) dpi(16);
        for (size_t i = 0; i < buttons_.size(); ++i) {
            const Button &b = buttons_[i];
            if (b.style != Laser) {
                continue;
            }
            SIZE sz = {};
            if (!b.label.empty()) {
                GetTextExtentPoint32W(dc, b.label.c_str(), (int) b.label.size(), &sz);
            }
            float total = is + (b.label.empty() ? 0.f : (float) dpi(7) + (float) sz.cx);
            float bw = (float) (b.rc.right - b.rc.left);
            if (total > bw - dpi(12)) {
                total = is; // too narrow for the word: the icon alone
            } else {
                worded[i] = !b.label.empty();
            }
            float x0 = (float) b.rc.left + (bw - total) * 0.5f;
            icons[i] = Gdiplus::RectF(x0, (float) (b.rc.top + b.rc.bottom) * 0.5f - is * 0.5f, is, is);
        }
        SelectObject(dc, of);
    }

    {
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        Gdiplus::RectF all(0, 0, (float) cr.right, (float) cr.bottom);
        Gdiplus::LinearGradientBrush ground(all, gc(kBgTop), gc(kBgBottom), 90.f);
        g.FillRectangle(&ground, all);
        // A faint sprig in the top-right corner of the paper, under everything.
        {
            Gdiplus::Matrix m0;
            g.GetTransform(&m0);
            float L = (float) dpi(compact ? 46 : 74);
            const float angles[3] = {150.f, 122.f, 176.f};
            const float lens[3] = {1.f, 0.78f, 0.7f};
            for (int i = 0; i < 3; ++i) {
                g.SetTransform(&m0);
                g.TranslateTransform((float) cr.right + (float) dpi(6), (float) dpi(-4) + (float) i * dpi(8));
                g.RotateTransform(angles[i]);
                float len = L * lens[i], hw = len * 0.22f;
                Gdiplus::GraphicsPath leaf;
                leaf.AddBezier(Gdiplus::PointF(0, 0), Gdiplus::PointF(len * 0.22f, -hw * 1.25f),
                               Gdiplus::PointF(len * 0.7f, -hw), Gdiplus::PointF(len, 0));
                leaf.AddBezier(Gdiplus::PointF(len, 0), Gdiplus::PointF(len * 0.7f, hw),
                               Gdiplus::PointF(len * 0.22f, hw * 1.25f), Gdiplus::PointF(0, 0));
                Gdiplus::SolidBrush lb(i == 1 ? Gdiplus::Color(46, 176, 132, 86) : Gdiplus::Color(40, 76, 132, 98));
                g.FillPath(&lb, &leaf);
            }
            g.SetTransform(&m0);
        }

        // The status card.
        Gdiplus::RectF card((float) status_rc_.left, (float) status_rc_.top,
                            (float) (status_rc_.right - status_rc_.left), (float) (status_rc_.bottom - status_rc_.top));
        float card_r = (float) dpi(14);
        soft_shadow(g, card, card_r, (float) dpi(2), Gdiplus::Color(40, 92, 70, 44));
        Gdiplus::SolidBrush card_b(gc(kCard));
        fill_rr(g, card, card_r, card_b);
        {
            Gdiplus::GraphicsPath p;
            round_path(p, card, card_r);
            Gdiplus::Pen edge(gc(kEdge, 160), 1.f);
            g.DrawPath(&edge, &p);
        }
        // A thin band down the card's left edge in the state's colour.
        {
            Gdiplus::GraphicsPath clip;
            round_path(clip, card, card_r);
            g.SetClip(&clip);
            Gdiplus::SolidBrush band(gc(st.recording ? (st.paused ? kAmber : kRecRed) : dot));
            g.FillRectangle(&band, card.X, card.Y, (float) dpi(4), card.Height);
            g.ResetClip();
        }
        // The state dot, with a halo when on air.
        float dcx = (float) (status_rc_.left + inset) + ds * 0.5f;
        float dcy = (float) (status_rc_.top + dpi(compact ? 17 : 19));
        if (on_air) {
            Gdiplus::SolidBrush halo(gc(dot, 60));
            g.FillEllipse(&halo, dcx - ds, dcy - ds, (float) ds * 2, (float) ds * 2);
        }
        Gdiplus::SolidBrush dot_b(gc(dot));
        g.FillEllipse(&dot_b, dcx - ds * 0.5f, dcy - ds * 0.5f, (float) ds, (float) ds);

        for (size_t i = 0; i < buttons_.size(); ++i) {
            const Button &b = buttons_[i];
            bool hot = (int) i == hot_ && b.enabled;
            bool down = hot && (int) i == pressed_;
            Gdiplus::RectF r((float) b.rc.left, (float) b.rc.top, (float) (b.rc.right - b.rc.left),
                             (float) (b.rc.bottom - b.rc.top));
            float rad = b.style == Chip ? r.Height * 0.5f : (float) dpi(11);
            COLORREF top = hot ? kPlateHot : kPlate, bottom = shade(top, 96), edge = kEdge;
            bool raised = b.enabled;
            if ((b.style == Chip || b.style == Toggle) && b.on) {
                top = hot ? kAccentHot : RGB(62, 124, 94);
                bottom = shade(kAccent, 90);
                edge = shade(kAccent, 85);
            } else if (b.style == Danger && b.enabled && b.on) {
                top = hot ? RGB(214, 96, 70) : RGB(204, 84, 60);
                bottom = shade(kRecRed, 90);
                edge = shade(kRecRed, 85);
            } else if (b.style == Laser && b.on) {
                // Lit: a red glow round the button, so it's plain at a glance that the pointer is live.
                for (int k = 3; k >= 1; --k) {
                    float e = (float) dpi(3) * (float) k;
                    Gdiplus::SolidBrush gb(Gdiplus::Color((BYTE) (34 / k), 236, 70, 56));
                    fill_rr(g, Gdiplus::RectF(r.X - e, r.Y - e, r.Width + e * 2, r.Height + e * 2), rad + e, gb);
                }
                top = hot ? RGB(246, 98, 82) : RGB(236, 84, 68);
                bottom = RGB(192, 48, 40);
                edge = RGB(176, 44, 36);
            } else if (!b.enabled) {
                top = bottom = RGB(240, 234, 222);
                raised = false;
            }
            if (down) {
                top = shade(top, 92);
                bottom = shade(bottom, 92);
            }
            if (raised && !down) {
                soft_shadow(g, r, rad, (float) dpi(1), Gdiplus::Color(hot ? 54 : 40, 92, 70, 44));
                // A light lip on the top edge: the cream neumorphic lift.
                Gdiplus::SolidBrush lip(Gdiplus::Color(200, 255, 255, 255));
                fill_rr(g, Gdiplus::RectF(r.X, r.Y - 1.f, r.Width, r.Height), rad, lip);
            }
            Gdiplus::LinearGradientBrush fill(Gdiplus::RectF(r.X, r.Y - 1, r.Width, r.Height + 2), gc(top), gc(bottom),
                                              90.f);
            fill_rr(g, r, rad, fill);
            {
                Gdiplus::GraphicsPath p;
                round_path(p, r, rad);
                Gdiplus::Pen pen(gc(edge, b.enabled ? 200 : 120), 1.f);
                g.DrawPath(&pen, &p);
            }
            if ((int) i == focus_ && keyboard_ && GetFocus() == hwnd_) {
                Gdiplus::RectF f(r.X - dpi(3), r.Y - dpi(3), r.Width + dpi(6), r.Height + dpi(6));
                Gdiplus::GraphicsPath p;
                round_path(p, f, rad + dpi(3));
                Gdiplus::Pen ring(gc(kAccent), (float) dpi(2));
                g.DrawPath(&ring, &p);
            }
            if (b.style == Laser) {
                draw_laser_icon(g, icons[i], b.on ? Gdiplus::Color(255, 255, 248, 240) : gc(kLaserRed),
                                (float) dpi(2) * 0.85f);
            }
        }
    }

    // Text, with ClearType, over the shapes.
    std::wstring head;
    if (on_air) {
        head = L"On air  ·  ";
        head += st.ipad && st.computer ? L"iPad + computer" : (st.ipad ? L"iPad" : L"Computer");
    } else {
        head = st.computer_wait ? L"Starting the computer screen…" : L"Not sharing yet";
    }
    if (st.laser) {
        head += L"  ·  laser";
    }
    std::wstring rec;
    if (st.recording) {
        rec = (st.paused ? L"❚❚ PAUSED  " : L"● REC  ") + clock_text(st.rec_seconds);
    }
    RECT hr = {status_rc_.left + inset + ds + dpi(10), status_rc_.top + dpi(compact ? 8 : 9),
               status_rc_.right - inset, status_rc_.bottom};
    HGDIOBJ of = SelectObject(dc, big);
    if (!rec.empty()) {
        SetTextColor(dc, st.paused ? kAmber : kRecRed);
        DrawTextW(dc, rec.c_str(), -1, &hr, DT_RIGHT | DT_TOP | DT_SINGLELINE);
        SIZE rs = {};
        GetTextExtentPoint32W(dc, rec.c_str(), (int) rec.size(), &rs);
        hr.right -= rs.cx + dpi(10);
    }
    SetTextColor(dc, kText);
    DrawTextW(dc, head.c_str(), -1, &hr, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (!compact) {
        SelectObject(dc, sub);
        std::wstring detail;
        COLORREF dc_col = kDim;
        if (!st.message.empty()) {
            detail = st.message;
            dc_col = kAmber;
        } else if (on_air || st.computer_wait) {
            if (st.ipad) {
                detail = L"iPad: " + (st.ipad_name.empty() ? std::wstring(L"connected") : st.ipad_name);
            }
            if (st.computer || st.computer_wait) {
                detail += (detail.empty() ? L"" : L"   ·   ") + std::wstring(L"Sharing: ") + st.computer_name;
            }
        } else {
            detail = L"Mirror from the iPad, or press Computer… to share this PC";
        }
        RECT r2 = {hr.left, status_rc_.top + dpi(31), status_rc_.right - inset, status_rc_.bottom};
        SetTextColor(dc, dc_col);
        DrawTextW(dc, detail.c_str(), -1, &r2, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }

    SelectObject(dc, label);
    SetTextColor(dc, kLabel);
    for (auto &l : labels_) {
        TextOutW(dc, l.first.x + dpi(2), l.first.y, l.second.c_str(), (int) l.second.size());
    }

    SelectObject(dc, btn);
    for (size_t i = 0; i < buttons_.size(); ++i) {
        const Button &b = buttons_[i];
        if (b.label.empty()) {
            continue;
        }
        COLORREF text = b.enabled ? kText : kOff;
        if ((b.style == Chip || b.style == Toggle) && b.on) {
            text = kOnText;
        } else if (b.style == Danger && b.enabled) {
            text = b.on ? RGB(255, 248, 240) : kRecRed;
        } else if (b.style == Laser) {
            text = b.on ? RGB(255, 248, 240) : kLaserRed;
        }
        RECT tr = b.rc;
        if (b.style == Laser) {
            if (!worded[i]) {
                continue;
            }
            tr.left = (LONG) (icons[i].X + icons[i].Width) + dpi(7);
            SetTextColor(dc, text);
            DrawTextW(dc, b.label.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            continue;
        }
        InflateRect(&tr, -dpi(6), 0);
        SetTextColor(dc, text);
        DrawTextW(dc, b.label.c_str(), -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    SelectObject(dc, of);
    for (HFONT f : {big, sub, label, btn}) {
        DeleteObject(f);
    }
    BitBlt(target, 0, 0, cr.right, cr.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old_bmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}

void ControlPanel::sync_tips() {
    if (!tips_) {
        return;
    }
    bool same = tip_rects_.size() == buttons_.size();
    for (size_t i = 0; same && i < buttons_.size(); ++i) {
        same = tip_rects_[i].first == buttons_[i].id && EqualRect(&tip_rects_[i].second, &buttons_[i].rc);
    }
    if (same) {
        return;
    }
    for (auto &t : tip_rects_) {
        TTTOOLINFOW ti = {sizeof(ti)};
        ti.hwnd = hwnd_;
        ti.uId = (UINT_PTR) t.first;
        SendMessageW(tips_, TTM_DELTOOL, 0, (LPARAM) &ti);
    }
    tip_rects_.clear();
    for (auto &b : buttons_) {
        TTTOOLINFOW ti = {sizeof(ti)};
        ti.uFlags = TTF_SUBCLASS;
        ti.hwnd = hwnd_;
        ti.uId = (UINT_PTR) b.id;
        ti.rect = b.rc;
        ti.lpszText = (LPWSTR) b.tip.c_str();
        SendMessageW(tips_, TTM_ADDTOOLW, 0, (LPARAM) &ti);
        tip_rects_.push_back({b.id, b.rc});
    }
}

int ControlPanel::hit(POINT pt) const {
    for (size_t i = 0; i < buttons_.size(); ++i) {
        if (PtInRect(&buttons_[i].rc, pt)) {
            return (int) i;
        }
    }
    return -1;
}

void ControlPanel::click(int id) {
    if (id >= kScene && id < kQuick) {
        host_->go_live(id - kScene);
    } else if (id >= kQuick && id < kRec) {
        std::vector<Action> q = quick_actions();
        size_t i = (size_t) (id - kQuick);
        if (i < q.size()) {
            host_->run(q[i]);
        }
    } else if (id == kRec || id == kStop) {
        host_->run(Action::ToggleRecord);
    } else if (id == kPause) {
        host_->run(Action::PauseRecord);
    } else if (id == kPreview) {
        host_->open_editor(true);
    } else if (id == kEdit) {
        host_->open_editor(false);
    } else if (id == kMore) {
        more_menu();
    } else if (id == kLaser) {
        host_->run(Action::Laser);
    }
    refresh();
}

void ControlPanel::more_menu() {
    Profile &prof = host_->profile();
    Settings &s = host_->settings();
    TeachStatus st = host_->status();
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, kMenuEditText, L"Edit text on air…");
    HMENU snips = CreatePopupMenu();
    for (size_t i = 0; i < prof.snippets.size() && i < 100; ++i) {
        std::wstring item = prof.snippets[i].name + L":  " + prof.snippets[i].text.substr(0, 40);
        for (auto &c : item) {
            if (c == L'\n') {
                c = L' ';
            }
        }
        AppendMenuW(snips, MF_STRING, kMenuSnippet + (UINT) i, item.c_str());
    }
    if (prof.snippets.empty()) {
        AppendMenuW(snips, MF_STRING | MF_GRAYED, 0, L"No saved text yet (save some in Edit scenes)");
    }
    AppendMenuW(m, MF_POPUP, (UINT_PTR) snips, L"Show saved text");
    HMENU lay = CreatePopupMenu();
    AppendMenuW(lay, MF_STRING, kMenuSideBySide, L"Side by side");
    AppendMenuW(lay, MF_STRING, kMenuPipTL, L"Small second screen, top left");
    AppendMenuW(lay, MF_STRING, kMenuPipTR, L"Small second screen, top right");
    AppendMenuW(lay, MF_STRING, kMenuPipBL, L"Small second screen, bottom left");
    AppendMenuW(lay, MF_STRING, kMenuPipBR, L"Small second screen, bottom right");
    AppendMenuW(lay, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(lay, MF_STRING, kMenuSwap, L"Swap main and second screen");
    AppendMenuW(lay, MF_STRING, kMenuSecondary, L"Show / hide the second screen");
    AppendMenuW(m, MF_POPUP, (UINT_PTR) lay, L"Two screens");
    HMENU comp = CreatePopupMenu();
    AppendMenuW(comp, MF_STRING, kMenuChoose, st.computer || st.computer_wait ? L"Share something else…" : L"Choose what to share…");
    AppendMenuW(comp, MF_STRING | (st.computer || st.computer_wait ? 0 : MF_GRAYED), kMenuStopComputer,
                L"Stop sharing the computer screen");
    AppendMenuW(m, MF_POPUP, (UINT_PTR) comp, L"Computer screen");
    HMENU las = CreatePopupMenu();
    AppendMenuW(las, MF_STRING | (st.laser ? MF_CHECKED : 0), kMenuLaser, L"Laser pointer on");
    AppendMenuW(las, MF_SEPARATOR, 0, nullptr);
    const wchar_t *colors[] = {L"Red", L"Green", L"Blue", L"Yellow"};
    for (UINT i = 0; i < 4; ++i) {
        AppendMenuW(las, MF_STRING | (s.laser_color == (int) i ? MF_CHECKED : 0), kMenuLaserColor + i, colors[i]);
    }
    AppendMenuW(las, MF_SEPARATOR, 0, nullptr);
    const wchar_t *sizes[] = {L"Small", L"Medium", L"Large"};
    for (UINT i = 0; i < 3; ++i) {
        AppendMenuW(las, MF_STRING | (s.laser_size == (int) i ? MF_CHECKED : 0), kMenuLaserSize + i, sizes[i]);
    }
    AppendMenuW(m, MF_POPUP, (UINT_PTR) las, L"Laser pointer");
    AppendMenuW(m, MF_STRING, kMenuKeep, L"Keep these changes in the scene");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    HMENU profs = CreatePopupMenu();
    std::vector<std::wstring> names = list_profiles();
    for (size_t i = 0; i < names.size() && i < 100; ++i) {
        AppendMenuW(profs, MF_STRING | (names[i] == prof.name ? MF_CHECKED : 0), kMenuProfile + (UINT) i,
                    names[i].c_str());
    }
    AppendMenuW(m, MF_POPUP, (UINT_PTR) profs, L"Teaching profile");
    AppendMenuW(m, MF_STRING | (s.panel_compact ? MF_CHECKED : 0), kMenuCompact, L"Compact view");
    AppendMenuW(m, MF_STRING | (s.panel_topmost ? MF_CHECKED : 0), kMenuTop, L"Always on top");
    AppendMenuW(m, MF_STRING, kMenuSettings, L"Teaching settings…");
    AppendMenuW(m, MF_POPUP, (UINT_PTR) host_->app_menu(), L"AirScreen settings");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, kMenuHide, L"Hide this panel (sharing carries on)");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd_);
    UINT cmd = (UINT) TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(m);
    switch (cmd) {
    case 0:
        break;
    case kMenuEditText:
        host_->run(Action::EditText);
        break;
    case kMenuSideBySide:
        host_->layout(DualLayout::SideBySide);
        break;
    case kMenuPipTL:
        host_->layout(DualLayout::PipTopLeft);
        break;
    case kMenuPipTR:
        host_->layout(DualLayout::PipTopRight);
        break;
    case kMenuPipBL:
        host_->layout(DualLayout::PipBottomLeft);
        break;
    case kMenuPipBR:
        host_->layout(DualLayout::PipBottomRight);
        break;
    case kMenuSwap:
        host_->run(Action::SwapScreens);
        break;
    case kMenuSecondary:
        host_->run(Action::ToggleSecondary);
        break;
    case kMenuChoose:
        host_->choose_computer();
        break;
    case kMenuStopComputer:
        host_->stop_computer();
        break;
    case kMenuKeep:
        host_->keep_live_changes();
        break;
    case kMenuCompact:
        s.panel_compact = !s.panel_compact;
        host_->save_settings();
        apply_prefs();
        break;
    case kMenuTop:
        s.panel_topmost = !s.panel_topmost;
        host_->save_settings();
        apply_prefs();
        break;
    case kMenuSettings:
        host_->open_settings();
        break;
    case kMenuHide:
        hide();
        break;
    case kMenuLaser:
        host_->run(Action::Laser);
        break;
    default:
        if (cmd >= kMenuLaserColor && cmd < kMenuLaserColor + 4) {
            s.laser_color = (int) (cmd - kMenuLaserColor);
            host_->save_settings();
            host_->prefs_changed();
            break;
        }
        if (cmd >= kMenuLaserSize && cmd < kMenuLaserSize + 3) {
            s.laser_size = (int) (cmd - kMenuLaserSize);
            host_->save_settings();
            host_->prefs_changed();
            break;
        }
        if (cmd >= kMenuSnippet && cmd < kMenuSnippet + prof.snippets.size()) {
            Scene live = host_->live_scene();
            if (Layer *t = main_text(live)) {
                host_->set_live_text(t->id, prof.snippets[cmd - kMenuSnippet].text, true, false);
            } else {
                MessageBoxW(hwnd_, L"This scene has no text layer. Add one in Edit scenes.", L"AirScreen",
                            MB_OK | MB_ICONINFORMATION);
            }
        } else if (cmd >= kMenuProfile && cmd < kMenuProfile + names.size()) {
            host_->switch_profile(names[cmd - kMenuProfile]);
        } else if (cmd >= kMenuApp) {
            SendMessageW(host_->main_window(), WM_COMMAND, cmd, 0);
        }
        break;
    }
    refresh();
}

LRESULT CALLBACK ControlPanel::proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    ControlPanel *self = (ControlPanel *) GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == WM_NCCREATE) {
        self = (ControlPanel *) ((CREATESTRUCTW *) lp)->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR) self);
        self->hwnd_ = hwnd;
    }
    if (self) {
        return self->handle(msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT ControlPanel::handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd_, &ps);
        paint(dc);
        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_TIMER:
        InvalidateRect(hwnd_, nullptr, FALSE); // the recording clock and live status
        return 0;
    case WM_GETMINMAXINFO: {
        auto *mm = (MINMAXINFO *) lp;
        mm->ptMinTrackSize.x = dpi(280);
        mm->ptMinTrackSize.y = dpi(120);
        return 0;
    }
    case WM_DPICHANGED: {
        dpi_ = HIWORD(wp);
        RECT *r = (RECT *) lp;
        SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        if (tips_) {
            SendMessageW(tips_, TTM_SETMAXTIPWIDTH, 0, dpi(320));
        }
        return 0;
    }
    case WM_MOUSEACTIVATE:
        // Buttons work without taking the keyboard from the app being taught (a slide show, a PDF, the
        // writing app); the title bar still activates the panel for keyboard use.
        return LOWORD(lp) == HTCLIENT ? MA_NOACTIVATE : MA_ACTIVATE;
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd_, 0};
        TrackMouseEvent(&tme);
        int h = hit({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        if (h != hot_) {
            hot_ = h;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (hot_ != -1) {
            hot_ = -1;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        pressed_ = hit({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        if (pressed_ >= 0) {
            SetCapture(hwnd_);
        }
        keyboard_ = false;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP: {
        int h = hit({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        int p = pressed_;
        pressed_ = -1;
        if (GetCapture() == hwnd_) {
            ReleaseCapture();
        }
        if (h >= 0 && h == p && buttons_[(size_t) h].enabled) {
            focus_ = h;
            click(buttons_[(size_t) h].id);
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case WM_RBUTTONUP: {
        int h = hit({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        std::vector<Action> q = quick_actions();
        if (h >= 0 && buttons_[(size_t) h].id >= kQuick && buttons_[(size_t) h].id < kRec &&
            q[(size_t) (buttons_[(size_t) h].id - kQuick)] == Action::ShowComputer) {
            host_->choose_computer(); // right-click Computer: pick another display or window
        } else {
            more_menu();
        }
        return 0;
    }
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS;
    case WM_KEYDOWN: {
        int n = (int) buttons_.size();
        if (n == 0) {
            break;
        }
        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        auto step = [&](int dir) {
            keyboard_ = true;
            for (int i = 0; i < n; ++i) {
                focus_ = ((focus_ < 0 ? (dir > 0 ? -1 : 0) : focus_) + dir + n) % n;
                if (buttons_[(size_t) focus_].enabled) {
                    break;
                }
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
        };
        if (wp == VK_TAB) {
            step(shift ? -1 : 1);
        } else if (wp == VK_RIGHT || wp == VK_DOWN) {
            step(1);
        } else if (wp == VK_LEFT || wp == VK_UP) {
            step(-1);
        } else if ((wp == VK_RETURN || wp == VK_SPACE) && focus_ >= 0 && focus_ < n) {
            keyboard_ = true;
            if (buttons_[(size_t) focus_].enabled) {
                click(buttons_[(size_t) focus_].id);
            }
        } else if (wp >= '1' && wp <= '9') {
            host_->go_live((int) (wp - '1'));
        }
        return 0;
    }
    case WM_CLOSE:
        hide(); // closing only hides: sharing and recording carry on
        return 0;
    case WM_EXITSIZEMOVE:
        save_rect();
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd_, kTimerTick);
        return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

} // namespace airscreen
