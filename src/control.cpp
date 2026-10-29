#include "control.h"
#include "settings.h"

#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <algorithm>
#include <sstream>

namespace airscreen {
namespace {

constexpr int kScene = 100; // + scene index
constexpr int kQuick = 300; // + index into the quick buttons
constexpr int kRec = 400, kPause = 401, kStop = 402, kPreview = 403, kEdit = 404, kMore = 405;
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
    kMenuSnippet = 100,  // + snippet index
    kMenuProfile = 300,  // + profile index
};

const COLORREF kBg = RGB(28, 28, 32);
const COLORREF kPlate = RGB(46, 46, 52);
const COLORREF kPlateHot = RGB(62, 62, 70);
const COLORREF kText = RGB(236, 236, 240);
const COLORREF kDim = RGB(142, 142, 150);
const COLORREF kOff = RGB(100, 100, 108);
const COLORREF kAccent = RGB(10, 132, 255);
const COLORREF kLive = RGB(48, 209, 88);
const COLORREF kRecRed = RGB(255, 69, 58);
const COLORREF kAmber = RGB(255, 159, 10);

std::wstring clock_text(double s) {
    int t = (int) s;
    wchar_t b[32];
    swprintf_s(b, L"%02d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
    return b;
}

void fill_round(HDC dc, const RECT &r, int radius, COLORREF fill, COLORREF edge) {
    HBRUSH b = CreateSolidBrush(fill);
    HPEN p = CreatePen(PS_SOLID, 1, edge);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(p);
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
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd_, 20, &dark, sizeof(dark));
    if (saved) {
        RECT want = {s.panel_x, s.panel_y, s.panel_x + s.panel_w, s.panel_y + s.panel_h};
        RECT fit = visible_rect(want, dpi(280), dpi(120));
        SetWindowPos(hwnd_, nullptr, fit.left, fit.top, fit.right - fit.left, fit.bottom - fit.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
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
        if (a != Action::None) {
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
    status_rc_ = {pad, y, width - pad, y + dpi(compact ? 22 : 40)};
    y = status_rc_.bottom + dpi(compact ? 8 : 12);

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
        // One row: the first quick buttons, record, and the menu.
        int n = (int) (std::min)(quick.size(), (size_t) 4);
        int cols = n + 2;
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
        Button m;
        m.id = kMore;
        m.rc = {pad + (bw + gap) * (n + 1), y, pad + inner, y + bh};
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

        // Recording.
        labels_.push_back({{pad, y}, L"RECORDING"});
        y += dpi(18);
        if (st.recording) {
            int half = (inner - gap) / 2;
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
            s.rc = {pad + half + gap, y, pad + inner, y + bh};
            s.label = L"■ Stop";
            s.tip = L"Stop recording and save the clip";
            s.style = Danger;
            s.on = true;
            buttons_.push_back(s);
        } else {
            Button r;
            r.id = kRec;
            r.rc = {pad, y, pad + inner, y + bh};
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
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bmp = CreateCompatibleBitmap(target, cr.right, cr.bottom);
    HGDIOBJ old_bmp = SelectObject(dc, bmp);
    HBRUSH bg = CreateSolidBrush(kBg);
    FillRect(dc, &cr, bg);
    DeleteObject(bg);
    layout(dc, cr.right);
    sync_tips();
    SetBkMode(dc, TRANSPARENT);
    const bool compact = host_->settings().panel_compact;
    TeachStatus st = host_->status();

    // Status: a dot that says on air / waiting / off, what is shown, and the recording clock.
    HFONT big = make_font(dpi(compact ? 13 : 15), FW_SEMIBOLD);
    HFONT sub = make_font(dpi(12), FW_NORMAL);
    HFONT label = make_font(dpi(10), FW_SEMIBOLD);
    HFONT btn = make_font(dpi(13), FW_SEMIBOLD);
    bool on_air = st.ipad || st.computer;
    COLORREF dot = on_air ? kLive : (st.computer_wait ? kAmber : kOff);
    int ds = dpi(10);
    RECT dr = {status_rc_.left, status_rc_.top + dpi(compact ? 6 : 5), status_rc_.left + ds,
               status_rc_.top + dpi(compact ? 6 : 5) + ds};
    HBRUSH db = CreateSolidBrush(dot);
    HGDIOBJ ob = SelectObject(dc, db), op = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, dr.left, dr.top, dr.right, dr.bottom);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(db);

    std::wstring head;
    if (on_air) {
        head = L"On air  ·  ";
        head += st.ipad && st.computer ? L"iPad + computer" : (st.ipad ? L"iPad" : L"Computer");
    } else {
        head = st.computer_wait ? L"Starting the computer screen…" : L"Not sharing yet";
    }
    std::wstring rec;
    if (st.recording) {
        rec = (st.paused ? L"❚❚ PAUSED  " : L"● REC  ") + clock_text(st.rec_seconds);
    }
    RECT hr = status_rc_;
    hr.left += ds + dpi(8);
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
        RECT r2 = {status_rc_.left + ds + dpi(8), status_rc_.top + dpi(21), status_rc_.right, status_rc_.bottom};
        SetTextColor(dc, dc_col);
        DrawTextW(dc, detail.c_str(), -1, &r2, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }

    SelectObject(dc, label);
    SetTextColor(dc, kDim);
    for (auto &l : labels_) {
        TextOutW(dc, l.first.x, l.first.y, l.second.c_str(), (int) l.second.size());
    }

    SelectObject(dc, btn);
    for (size_t i = 0; i < buttons_.size(); ++i) {
        const Button &b = buttons_[i];
        bool hot = (int) i == hot_ && b.enabled;
        bool down = hot && (int) i == pressed_;
        COLORREF fill = hot ? kPlateHot : kPlate, text = b.enabled ? kText : kOff, edge = fill;
        if (b.style == Chip && b.on) {
            fill = edge = RGB(245, 245, 247);
            text = RGB(24, 24, 28);
        } else if (b.style == Toggle && b.on) {
            fill = edge = hot ? RGB(40, 150, 255) : kAccent;
            text = RGB(255, 255, 255);
        } else if (b.style == Danger && b.enabled) {
            if (b.on) {
                fill = edge = hot ? RGB(255, 99, 88) : kRecRed;
                text = RGB(255, 255, 255);
            } else {
                text = kRecRed;
            }
        }
        if (down) {
            fill = RGB(GetRValue(fill) * 85 / 100, GetGValue(fill) * 85 / 100, GetBValue(fill) * 85 / 100);
        }
        int radius = b.style == Chip ? b.rc.bottom - b.rc.top : dpi(10);
        fill_round(dc, b.rc, radius, fill, edge);
        if ((int) i == focus_ && keyboard_ && GetFocus() == hwnd_) {
            RECT f = b.rc;
            InflateRect(&f, dpi(2), dpi(2));
            HPEN fp = CreatePen(PS_SOLID, dpi(2), kAccent);
            HGDIOBJ p0 = SelectObject(dc, fp), b0 = SelectObject(dc, GetStockObject(NULL_BRUSH));
            RoundRect(dc, f.left, f.top, f.right, f.bottom, radius + dpi(4), radius + dpi(4));
            SelectObject(dc, p0);
            SelectObject(dc, b0);
            DeleteObject(fp);
        }
        RECT tr = b.rc;
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
    AppendMenuW(m, MF_STRING, kMenuSettings, L"Settings…");
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
    default:
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
