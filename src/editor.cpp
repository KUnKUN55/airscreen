#include "editor.h"
#include "settings.h"
#include "video.h"

#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <algorithm>
#include <cmath>
#include <cwchar>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")

namespace airscreen {
namespace {

enum : int {
    IDE_SCENES = 2000,
    IDE_NEW,
    IDE_DUP,
    IDE_RENAME,
    IDE_DELETE,
    IDE_UP,
    IDE_DOWN,
    IDE_DEFAULT,
    IDE_PROFILE,
    IDE_MODE_SCENE = 2020,
    IDE_MODE_LIVE,
    IDE_MODE_IPAD,
    IDE_MODE_COMP,
    IDE_APPLY_MODE,
    IDE_APPLY,
    IDE_GOLIVE,
    IDE_EXPAND,
    IDE_SCENE_COMBO,
    IDE_ADD = 2040,
    IDE_ALIGN,
    IDE_ORDER,
    IDE_LDUP,
    IDE_LDEL,
    IDE_SNAP,
    IDE_LAYERS = 2060,
    IDE_LAYERS_LABEL,
    IDP_SCENE_NAME = 2100,
    IDP_BACKDROP,
    IDP_MOOD,
    IDP_BG_COLOR,
    IDP_BG_PICTURE,
    IDP_NAME,
    IDP_VISIBLE,
    IDP_LOCKED,
    IDP_OPACITY,
    IDP_X,
    IDP_Y,
    IDP_W,
    IDP_H,
    IDP_SOURCE,
    IDP_FILL,
    IDP_ROUNDED,
    IDP_TEXT,
    IDP_FONT,
    IDP_COLOR,
    IDP_BOLD,
    IDP_ALIGN,
    IDP_SAVE_SNIP,
    IDP_INSERT_SNIP,
    IDP_BOX,
    IDP_BOX_COLOR,
    IDP_BOX_COLOR2,
    IDP_BOX_OPACITY,
    IDP_BLUR,
    IDP_RADIUS,
    IDP_PADDING,
    IDP_BORDER_COLOR,
    IDP_THICKNESS,
    IDP_PICTURE,
};

// Which settings a row belongs to; the panel shows only the groups of what is selected.
enum : unsigned {
    G_SCENE = 1,
    G_LAYER = 2,
    G_SOURCE = 4,
    G_TEXT = 8,
    G_BOX = 16,
    G_FRAME = 32,
    G_IMAGE = 64,
    G_PAD = 128,
};

enum : UINT {
    M_ADD_IPAD = 1,
    M_ADD_COMPUTER,
    M_ADD_TEXT,
    M_ADD_IMAGE,
    M_ADD_FRAME,
    M_AL_CENTER = 20,
    M_AL_TOP,
    M_AL_BOTTOM,
    M_AL_TL,
    M_AL_TR,
    M_AL_BL,
    M_AL_BR,
    M_AL_FILL,
    M_OR_FRONT = 40,
    M_OR_FORWARD,
    M_OR_BACKWARD,
    M_OR_BACK,
    M_SNIP = 100,     // + index: insert
    M_SNIP_DEL = 300, // + index: remove
};

const wchar_t *kTypeName[] = {L"Screen", L"Text", L"Picture", L"Frame"};

std::wstring get_text(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring s(n, 0);
    if (n > 0) {
        GetWindowTextW(h, s.data(), n + 1);
    }
    return s;
}

std::wstring pct(float v) {
    wchar_t b[32];
    swprintf_s(b, L"%.1f", v * 100.f);
    return b;
}

bool parse_pct(HWND h, float &out) {
    std::wstring s = get_text(h);
    wchar_t *end = nullptr;
    double v = wcstod(s.c_str(), &end);
    if (s.empty() || end == s.c_str()) {
        return false;
    }
    out = (float) (v / 100.0);
    return true;
}

// Edit controls want CR LF; the scene keeps plain line feeds.
std::wstring to_crlf(const std::wstring &s) {
    std::wstring o;
    for (wchar_t c : s) {
        if (c == L'\n') {
            o += L"\r\n";
        } else {
            o += c;
        }
    }
    return o;
}

std::wstring from_crlf(const std::wstring &s) {
    std::wstring o;
    for (wchar_t c : s) {
        if (c != L'\r') {
            o += c;
        }
    }
    return o;
}

} // namespace

void SceneEditor::open(HINSTANCE inst, TeachHost *host, bool preview_only) {
    inst_ = inst;
    host_ = host;
    if (hwnd_) {
        if (preview_only != preview_only_) {
            preview_only_ = preview_only;
            layout();
        }
        if (preview_only) {
            set_mode(ModeLive);
        }
        if (IsIconic(hwnd_)) {
            ShowWindow(hwnd_, SW_RESTORE);
        }
        SetForegroundWindow(hwnd_);
        return;
    }
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.lpfnWndProc = proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH) (COLOR_BTNFACE + 1);
        wc.hIcon = (HICON) GetClassLongPtrW(host->main_window(), GCLP_HICON);
        wc.hIconSm = (HICON) GetClassLongPtrW(host->main_window(), GCLP_HICONSM);
        wc.lpszClassName = L"AirScreenEditor";
        RegisterClassExW(&wc);
        WNDCLASSEXW pc = {sizeof(pc)};
        pc.style = CS_DBLCLKS;
        pc.lpfnWndProc = preview_proc;
        pc.hInstance = inst;
        pc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        pc.lpszClassName = L"AirScreenEditorPreview";
        RegisterClassExW(&pc);
        WNDCLASSEXW rc = {sizeof(rc)};
        rc.lpfnWndProc = props_proc;
        rc.hInstance = inst;
        rc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        rc.hbrBackground = (HBRUSH) (COLOR_BTNFACE + 1);
        rc.lpszClassName = L"AirScreenEditorProps";
        RegisterClassExW(&rc);
        registered = true;
    }
    preview_only_ = preview_only;
    cur_ = (std::max)(0, host->live_index());
    sel_ = 0;
    pending_ = false;
    scroll_ = 0;
    Settings &s = host->settings();
    UINT sys = GetDpiForSystem();
    hwnd_ = CreateWindowExW(0, L"AirScreenEditor", L"AirScreen · Scenes and preview", WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, CW_USEDEFAULT, MulDiv(1200, sys, 96), MulDiv(780, sys, 96), nullptr,
                            nullptr, inst, this);
    if (!hwnd_) {
        char b[64];
        snprintf(b, sizeof(b), "editor: window not created (error %lu)", GetLastError());
        append_log(b);
        return;
    }
    dpi_ = (int) GetDpiForWindow(hwnd_);
    if (s.editor_w > 0 && s.editor_h > 0) {
        RECT want = {s.editor_x, s.editor_y, s.editor_x + s.editor_w, s.editor_y + s.editor_h};
        RECT fit = visible_rect(want, dpi(760), dpi(480));
        SetWindowPos(hwnd_, nullptr, fit.left, fit.top, fit.right - fit.left, fit.bottom - fit.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    build();
    layout();
    fill_scene_list();
    fill_layer_list();
    load_props();
    host->video().attach_preview(preview_);
    hide_from_capture(hwnd_, s.hide_controls);
    set_mode(preview_only ? ModeLive : ModeScene);
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
}

void SceneEditor::close() {
    if (!hwnd_) {
        return;
    }
    host_->video().detach_preview(); // before its window goes
    WINDOWPLACEMENT wp = {sizeof(wp)};
    if (GetWindowPlacement(hwnd_, &wp)) {
        Settings &s = host_->settings();
        RECT r = wp.rcNormalPosition;
        s.editor_x = r.left;
        s.editor_y = r.top;
        s.editor_w = r.right - r.left;
        s.editor_h = r.bottom - r.top;
        host_->save_settings();
    }
    HWND h = hwnd_;
    DestroyWindow(h);
}

void SceneEditor::live_changed() {
    if (!hwnd_) {
        return;
    }
    fill_scene_list();
    pending_ = false;
    update_buttons();
}

void SceneEditor::scenes_changed() {
    if (!hwnd_) {
        return;
    }
    cur_ = std::clamp(cur_, 0, (int) host_->profile().scenes.size() - 1);
    if (!scene().find(sel_)) {
        sel_ = 0;
    }
    fill_scene_list();
    fill_layer_list();
    load_props();
    update_preview();
    update_buttons();
}

Scene &SceneEditor::scene() {
    auto &v = host_->profile().scenes;
    cur_ = std::clamp(cur_, 0, (int) v.size() - 1);
    return v[(size_t) cur_];
}

Layer *SceneEditor::selected() {
    return sel_ ? scene().find(sel_) : nullptr;
}

void SceneEditor::apply_font(HWND h) {
    SendMessageW(h, WM_SETFONT, (WPARAM) font_, TRUE);
}

HWND SceneEditor::make(const wchar_t *cls, const wchar_t *text, DWORD style, int id, HWND parent) {
    HWND h = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, parent ? parent : hwnd_,
                             (HMENU) (INT_PTR) id, inst_, nullptr);
    apply_font(h);
    return h;
}

void SceneEditor::add_row(unsigned groups, const wchar_t *label, std::initializer_list<HWND> ctl, int height) {
    Row r;
    r.groups = groups;
    r.label = make(L"STATIC", label, SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX, -1, props_);
    r.ctl = ctl;
    r.height = height;
    rows_.push_back(r);
}

void SceneEditor::build() {
    font_ = CreateFontW(-dpi(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    const DWORD btn = WS_TABSTOP | BS_PUSHBUTTON;
    // Left: the profile's scenes.
    left_ = {make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, IDE_PROFILE),
             scenes_ = make(L"LISTBOX", L"", WS_TABSTOP | WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                            IDE_SCENES),
             make(L"BUTTON", L"New", btn, IDE_NEW),
             make(L"BUTTON", L"Duplicate", btn, IDE_DUP),
             make(L"BUTTON", L"Rename", btn, IDE_RENAME),
             make(L"BUTTON", L"Delete", btn, IDE_DELETE),
             make(L"BUTTON", L"▲ Up", btn, IDE_UP),
             make(L"BUTTON", L"▼ Down", btn, IDE_DOWN),
             make(L"BUTTON", L"Start with this scene", btn, IDE_DEFAULT)};

    // Top: what the preview shows, and getting changes on air.
    const DWORD radio = WS_TABSTOP | BS_AUTORADIOBUTTON | BS_PUSHLIKE;
    make(L"BUTTON", L"Scene", radio | WS_GROUP, IDE_MODE_SCENE);
    make(L"BUTTON", L"On air", radio, IDE_MODE_LIVE);
    make(L"BUTTON", L"iPad", radio, IDE_MODE_IPAD);
    make(L"BUTTON", L"Computer", radio, IDE_MODE_COMP);
    scene_combo_ = make(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, IDE_SCENE_COMBO);
    preview_only_ctl_ = {scene_combo_};
    HWND apply_mode = make(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST, IDE_APPLY_MODE);
    SendMessageW(apply_mode, CB_ADDSTRING, 0, (LPARAM) L"Edits to the scene on air show at once");
    SendMessageW(apply_mode, CB_ADDSTRING, 0, (LPARAM) L"Preview edits first, then Apply");
    full_only_ = {apply_mode, make(L"BUTTON", L"Apply to air", btn, IDE_APPLY)};
    make(L"BUTTON", L"Put on air", btn, IDE_GOLIVE);
    make(L"BUTTON", L"", btn, IDE_EXPAND);

    // Under the preview: layer tools.
    bottom_ = {make(L"BUTTON", L"+ Add ▾", btn, IDE_ADD),        make(L"BUTTON", L"Align ▾", btn, IDE_ALIGN),
               make(L"BUTTON", L"Order ▾", btn, IDE_ORDER),      make(L"BUTTON", L"Duplicate", btn, IDE_LDUP),
               make(L"BUTTON", L"Delete", btn, IDE_LDEL),
               make(L"BUTTON", L"Snap to guides", WS_TABSTOP | BS_AUTOCHECKBOX, IDE_SNAP)};
    CheckDlgButton(hwnd_, IDE_SNAP, BST_CHECKED);

    preview_ = CreateWindowExW(0, L"AirScreenEditorPreview", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 10, 10,
                               hwnd_, nullptr, inst_, this);

    // Right: layers, then the selected layer's settings.
    layers_ = make(L"LISTBOX", L"",
                   WS_TABSTOP | WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWFIXED,
                   IDE_LAYERS);
    SetWindowSubclass(layers_, layers_subclass, 1, (DWORD_PTR) this);
    props_ = CreateWindowExW(WS_EX_CONTROLPARENT, L"AirScreenEditorProps", L"",
                             WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_CLIPCHILDREN, 0, 0, 10, 10, hwnd_, nullptr, inst_,
                             this);
    right_ = {make(L"STATIC", L"Layers (front first)   ·   click ● to show or hide", SS_LEFT | SS_NOPREFIX,
                   IDE_LAYERS_LABEL),
              layers_, props_};

    const DWORD ed = WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL;
    const DWORD cb = WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL;
    const DWORD chk = WS_TABSTOP | BS_AUTOCHECKBOX;
    const DWORD tb = WS_TABSTOP | TBS_HORZ | TBS_NOTICKS;
    auto P = [&](const wchar_t *cls, const wchar_t *text, DWORD style, int id) {
        return make(cls, text, style, id, props_);
    };

    add_row(G_SCENE, L"SCENE", {});
    add_row(G_SCENE, L"Name", {P(L"EDIT", L"", ed, IDP_SCENE_NAME)});
    HWND backdrop = P(L"COMBOBOX", L"", cb, IDP_BACKDROP);
    for (auto *s : {L"Studio colours", L"Main screen, blurred", L"Picture", L"Solid colour"}) {
        SendMessageW(backdrop, CB_ADDSTRING, 0, (LPARAM) s);
    }
    add_row(G_SCENE, L"Backdrop", {backdrop});
    HWND mood = P(L"COMBOBOX", L"", cb, IDP_MOOD);
    for (auto *s : {L"Night", L"Blue", L"Amber", L"Green"}) {
        SendMessageW(mood, CB_ADDSTRING, 0, (LPARAM) s);
    }
    add_row(G_SCENE, L"Colours", {mood});
    add_row(G_SCENE, L"Solid", {P(L"BUTTON", L"Colour…", btn, IDP_BG_COLOR)});
    add_row(G_SCENE, L"Picture", {P(L"BUTTON", L"Choose picture…", btn, IDP_BG_PICTURE)});
    add_row(G_SCENE, L"", {P(L"STATIC", L"Click a layer in the preview or the list to change it.", SS_LEFT, -1)}, 40);

    add_row(G_LAYER, L"LAYER", {});
    add_row(G_LAYER, L"Name", {P(L"EDIT", L"", ed, IDP_NAME)});
    add_row(G_LAYER, L"", {P(L"BUTTON", L"Visible", chk, IDP_VISIBLE), P(L"BUTTON", L"Locked", chk, IDP_LOCKED)});
    add_row(G_LAYER, L"Opacity", {P(TRACKBAR_CLASSW, L"", tb, IDP_OPACITY)});
    add_row(G_LAYER, L"X / Y %", {P(L"EDIT", L"", ed, IDP_X), P(L"EDIT", L"", ed, IDP_Y)});
    add_row(G_LAYER, L"W / H %", {P(L"EDIT", L"", ed, IDP_W), P(L"EDIT", L"", ed, IDP_H)});

    add_row(G_SOURCE, L"SCREEN", {});
    HWND source = P(L"COMBOBOX", L"", cb, IDP_SOURCE);
    SendMessageW(source, CB_ADDSTRING, 0, (LPARAM) L"iPad (AirPlay)");
    SendMessageW(source, CB_ADDSTRING, 0, (LPARAM) L"Computer screen");
    add_row(G_SOURCE, L"Shows", {source});
    add_row(G_SOURCE, L"", {P(L"BUTTON", L"Fill (crop)", chk, IDP_FILL), P(L"BUTTON", L"Rounded", chk, IDP_ROUNDED)});

    add_row(G_TEXT, L"TEXT", {});
    add_row(G_TEXT, L"Text",
            {P(L"EDIT", L"", WS_TABSTOP | WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
               IDP_TEXT)},
            76);
    add_row(G_TEXT, L"Size", {P(TRACKBAR_CLASSW, L"", tb, IDP_FONT)});
    add_row(G_TEXT, L"", {P(L"BUTTON", L"Colour…", btn, IDP_COLOR), P(L"BUTTON", L"Bold", chk, IDP_BOLD)});
    HWND align = P(L"COMBOBOX", L"", cb, IDP_ALIGN);
    for (auto *s : {L"Left", L"Centre", L"Right"}) {
        SendMessageW(align, CB_ADDSTRING, 0, (LPARAM) s);
    }
    add_row(G_TEXT, L"Align", {align});
    add_row(G_TEXT, L"Saved text",
            {P(L"BUTTON", L"Save…", btn, IDP_SAVE_SNIP), P(L"BUTTON", L"Use ▾", btn, IDP_INSERT_SNIP)});

    add_row(G_BOX, L"BACKGROUND", {});
    HWND box = P(L"COMBOBOX", L"", cb, IDP_BOX);
    for (auto *s : {L"None", L"Solid", L"Transparent (outline)", L"Translucent", L"Frosted (blur)", L"Gradient"}) {
        SendMessageW(box, CB_ADDSTRING, 0, (LPARAM) s);
    }
    add_row(G_BOX, L"Style", {box});
    add_row(G_BOX, L"Colours",
            {P(L"BUTTON", L"Colour…", btn, IDP_BOX_COLOR), P(L"BUTTON", L"2nd…", btn, IDP_BOX_COLOR2)});
    add_row(G_BOX, L"Strength", {P(TRACKBAR_CLASSW, L"", tb, IDP_BOX_OPACITY)});
    add_row(G_BOX, L"Blur", {P(TRACKBAR_CLASSW, L"", tb, IDP_BLUR)});
    add_row(G_BOX, L"Corners", {P(TRACKBAR_CLASSW, L"", tb, IDP_RADIUS)});
    add_row(G_PAD, L"Padding", {P(TRACKBAR_CLASSW, L"", tb, IDP_PADDING)});

    add_row(G_FRAME, L"BORDER", {});
    add_row(G_FRAME, L"Colour", {P(L"BUTTON", L"Colour…", btn, IDP_BORDER_COLOR)});
    add_row(G_FRAME, L"Thickness", {P(TRACKBAR_CLASSW, L"", tb, IDP_THICKNESS)});

    add_row(G_IMAGE, L"PICTURE", {});
    add_row(G_IMAGE, L"File", {P(L"BUTTON", L"Choose picture…", btn, IDP_PICTURE)});

    auto range = [&](int id, int lo, int hi) {
        SendDlgItemMessageW(props_, id, TBM_SETRANGE, TRUE, MAKELPARAM(lo, hi));
    };
    range(IDP_OPACITY, 0, 100);
    range(IDP_FONT, 10, 200);
    range(IDP_BOX_OPACITY, 0, 100);
    range(IDP_BLUR, 0, 100);
    range(IDP_RADIUS, 0, 100);
    range(IDP_PADDING, 0, 100);
    range(IDP_THICKNESS, 0, 50);
}

void SceneEditor::layout() {
    if (!hwnd_) {
        return;
    }
    RECT cr;
    GetClientRect(hwnd_, &cr);
    const int W = cr.right, H = cr.bottom, pad = dpi(8), bh = dpi(28);
    const int lw = preview_only_ ? 0 : dpi(210), rw = preview_only_ ? 0 : dpi(330);
    auto place = [&](int id, int x, int y, int w, int h) {
        HWND c = GetDlgItem(hwnd_, id);
        if (c) {
            SetWindowPos(c, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
        }
    };
    auto show = [&](const std::vector<HWND> &v, bool on) {
        for (HWND h : v) {
            ShowWindow(h, on ? SW_SHOW : SW_HIDE);
        }
    };
    show(left_, !preview_only_);
    show(right_, !preview_only_);
    show(bottom_, !preview_only_);
    show(full_only_, !preview_only_);
    show(preview_only_ctl_, preview_only_);
    SetDlgItemTextW(hwnd_, IDE_EXPAND, preview_only_ ? L"Edit scenes ▸" : L"◂ Preview only");

    // Top bar, across the whole window.
    int x = pad, y = pad;
    if (preview_only_) {
        SetWindowPos(scene_combo_, nullptr, x, y, dpi(220), dpi(300), SWP_NOZORDER | SWP_NOACTIVATE);
        x += dpi(228);
    }
    const int ids[4] = {IDE_MODE_SCENE, IDE_MODE_LIVE, IDE_MODE_IPAD, IDE_MODE_COMP};
    for (int i = 0; i < 4; ++i) {
        place(ids[i], x, y, dpi(86), bh);
        x += dpi(86);
    }
    int rx = W - pad;
    auto from_right = [&](int id, int w) {
        rx -= w;
        place(id, rx, y, w, id == IDE_APPLY_MODE ? dpi(200) : bh);
        rx -= dpi(6);
    };
    from_right(IDE_EXPAND, dpi(120));
    from_right(IDE_GOLIVE, dpi(100));
    if (!preview_only_) {
        from_right(IDE_APPLY, dpi(100));
        from_right(IDE_APPLY_MODE, (std::min)(dpi(270), (std::max)(dpi(120), rx - x - dpi(12))));
    }
    const int top = y + bh + pad;

    // Left.
    if (!preview_only_) {
        place(IDE_PROFILE, pad, top, lw - pad, dpi(20));
        int by = H - pad - bh;
        place(IDE_DEFAULT, pad, by, lw - pad, bh);
        by -= bh + dpi(4);
        int half = (lw - pad - dpi(4)) / 2;
        place(IDE_UP, pad, by, half, bh);
        place(IDE_DOWN, pad + half + dpi(4), by, half, bh);
        by -= bh + dpi(4);
        place(IDE_RENAME, pad, by, half, bh);
        place(IDE_DELETE, pad + half + dpi(4), by, half, bh);
        by -= bh + dpi(4);
        place(IDE_NEW, pad, by, half, bh);
        place(IDE_DUP, pad + half + dpi(4), by, half, bh);
        place(IDE_SCENES, pad, top + dpi(24), lw - pad, by - dpi(6) - (top + dpi(24)));
    }

    // Right.
    if (!preview_only_) {
        int x0 = W - rw;
        place(IDE_LAYERS_LABEL, x0, top, rw - pad, dpi(20));
        int lh = dpi(150);
        place(IDE_LAYERS, x0, top + dpi(22), rw - pad, lh);
        SendMessageW(layers_, LB_SETITEMHEIGHT, 0, dpi(24));
        SetWindowPos(props_, nullptr, x0, top + dpi(22) + lh + pad, rw - pad, H - (top + dpi(22) + lh + pad) - pad,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        layout_props();
    }

    // Centre: the preview, with the layer tools under it.
    int cx = lw + pad, cw = W - lw - rw - pad * 2;
    int bottom = preview_only_ ? 0 : bh + pad;
    SetWindowPos(preview_, nullptr, cx, top, (std::max)(cw, dpi(80)), (std::max)(H - top - pad - bottom, dpi(60)),
                 SWP_NOZORDER | SWP_NOACTIVATE);
    if (!preview_only_) {
        int tx = cx, ty = H - pad - bh;
        const struct {
            int id, w;
        } tools[] = {{IDE_ADD, 84}, {IDE_ALIGN, 76}, {IDE_ORDER, 76}, {IDE_LDUP, 80}, {IDE_LDEL, 66}, {IDE_SNAP, 130}};
        for (auto &t : tools) {
            place(t.id, tx, ty, dpi(t.w), bh);
            tx += dpi(t.w) + dpi(6);
        }
    }
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void SceneEditor::layout_props() {
    RECT pr;
    GetClientRect(props_, &pr);
    unsigned groups = G_SCENE;
    if (Layer *l = selected()) {
        groups = G_LAYER;
        switch (l->type) {
        case LayerType::Source:
            groups |= G_SOURCE;
            break;
        case LayerType::Text:
            groups |= G_TEXT | G_BOX | G_PAD;
            break;
        case LayerType::Frame:
            groups |= G_BOX | G_FRAME;
            break;
        case LayerType::Image:
            groups |= G_IMAGE;
            break;
        }
    }
    const int pad = dpi(8), lw = dpi(78), gap = dpi(6);
    const int cw = (std::max)(dpi(60), (int) pr.right - pad * 2 - lw - gap);
    int y = pad - scroll_;
    HDWP dwp = BeginDeferWindowPos((int) rows_.size() * 3);
    for (auto &r : rows_) {
        bool on = (r.groups & groups) != 0;
        UINT vis = on ? SWP_SHOWWINDOW : SWP_HIDEWINDOW;
        int h = dpi(r.height ? r.height : 28);
        if (r.ctl.empty() && on) {
            h = dpi(24); // a heading
            dwp = DeferWindowPos(dwp, r.label, nullptr, pad, y + dpi(4), pr.right - pad * 2, dpi(18),
                                 SWP_NOZORDER | SWP_NOACTIVATE | vis);
            y += h;
            continue;
        }
        bool unlabelled = GetWindowTextLengthW(r.label) == 0; // the controls take the whole row
        dwp = DeferWindowPos(dwp, r.label, nullptr, pad, y, lw, dpi(24),
                             SWP_NOZORDER | SWP_NOACTIVATE | (on && !unlabelled ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
        int n = (int) r.ctl.size();
        int each = n ? (cw - gap * (n - 1)) / n : 0;
        for (int i = 0; i < n; ++i) {
            wchar_t cls[32] = {};
            GetClassNameW(r.ctl[(size_t) i], cls, 32);
            int ch = !_wcsicmp(cls, L"ComboBox") ? dpi(240) : h - dpi(4);
            int cx = pad + lw + gap + (each + gap) * i;
            if (unlabelled) {
                int all = pr.right - pad * 2;
                each = (all - gap * (n - 1)) / n;
                cx = pad + (each + gap) * i;
            }
            dwp = DeferWindowPos(dwp, r.ctl[(size_t) i], nullptr, cx, y, each, ch, SWP_NOZORDER | SWP_NOACTIVATE | vis);
        }
        if (on) {
            y += h;
        }
    }
    EndDeferWindowPos(dwp);
    // Moved controls (combo boxes especially) don't always repaint themselves after a deferred move.
    RedrawWindow(props_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    props_h_ = y + scroll_ + pad;
    SCROLLINFO si = {sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS, 0, props_h_, (UINT) pr.bottom, scroll_};
    SetScrollInfo(props_, SB_VERT, &si, TRUE);
}

void SceneEditor::fill_scene_list() {
    Profile &p = host_->profile();
    int live = host_->live_index();
    SendMessageW(scenes_, WM_SETREDRAW, FALSE, 0);
    SendMessageW(scenes_, LB_RESETCONTENT, 0, 0);
    SendMessageW(scene_combo_, CB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < p.scenes.size(); ++i) {
        std::wstring item;
        item += (int) i == live ? L"● " : L"    ";
        item += p.scenes[i].name;
        if (p.scenes[i].id == p.default_scene) {
            item += L"   ★";
        }
        SendMessageW(scenes_, LB_ADDSTRING, 0, (LPARAM) item.c_str());
        SendMessageW(scene_combo_, CB_ADDSTRING, 0, (LPARAM) item.c_str());
    }
    SendMessageW(scenes_, LB_SETCURSEL, (WPARAM) cur_, 0);
    SendMessageW(scene_combo_, CB_SETCURSEL, (WPARAM) cur_, 0);
    SendMessageW(scenes_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(scenes_, nullptr, TRUE);
    SetDlgItemTextW(hwnd_, IDE_PROFILE, (L"Profile: " + p.name + L"   (● on air, ★ at start)").c_str());
}

void SceneEditor::fill_layer_list() {
    Scene &s = scene();
    SendMessageW(layers_, WM_SETREDRAW, FALSE, 0);
    SendMessageW(layers_, LB_RESETCONTENT, 0, 0);
    int sel = -1;
    for (size_t i = 0; i < s.layers.size(); ++i) {
        const Layer &l = s.layers[s.layers.size() - 1 - i]; // front first
        SendMessageW(layers_, LB_ADDSTRING, 0, (LPARAM) l.id);
        if (l.id == sel_) {
            sel = (int) i;
        }
    }
    SendMessageW(layers_, LB_SETCURSEL, (WPARAM) sel, 0);
    SendMessageW(layers_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(layers_, nullptr, TRUE);
}

void SceneEditor::select_scene(int index) {
    auto &v = host_->profile().scenes;
    if (index < 0 || index >= (int) v.size()) {
        return;
    }
    cur_ = index;
    sel_ = 0;
    pending_ = false;
    fill_scene_list();
    fill_layer_list();
    load_props();
    update_preview();
    update_buttons();
}

void SceneEditor::select_layer(uint32_t id) {
    if (id && !scene().find(id)) {
        id = 0;
    }
    bool changed = id != sel_;
    sel_ = id;
    if (changed) {
        scroll_ = 0;
        fill_layer_list();
        load_props();
    }
    update_preview();
    update_buttons();
}

void SceneEditor::load_props() {
    loading_ = true;
    Scene &s = scene();
    auto set = [&](int id, const std::wstring &t) { SetDlgItemTextW(props_, id, t.c_str()); };
    auto check = [&](int id, bool on) { CheckDlgButton(props_, id, on ? BST_CHECKED : BST_UNCHECKED); };
    auto combo = [&](int id, int i) { SendDlgItemMessageW(props_, id, CB_SETCURSEL, (WPARAM) i, 0); };
    auto track = [&](int id, float v) {
        SendDlgItemMessageW(props_, id, TBM_SETPOS, TRUE, (LPARAM) std::lround(v));
    };
    set(IDP_SCENE_NAME, s.name);
    combo(IDP_BACKDROP, (int) s.backdrop);
    combo(IDP_MOOD, s.mood);
    if (Layer *l = selected()) {
        set(IDP_NAME, l->name);
        check(IDP_VISIBLE, l->visible);
        check(IDP_LOCKED, l->locked);
        track(IDP_OPACITY, l->opacity * 100);
        set(IDP_X, pct(l->x));
        set(IDP_Y, pct(l->y));
        set(IDP_W, pct(l->w));
        set(IDP_H, pct(l->h));
        combo(IDP_SOURCE, (int) l->source);
        check(IDP_FILL, l->fill);
        check(IDP_ROUNDED, l->rounded);
        set(IDP_TEXT, to_crlf(l->text));
        track(IDP_FONT, l->font * 1000);
        check(IDP_BOLD, l->bold);
        combo(IDP_ALIGN, l->align);
        combo(IDP_BOX, l->box_on ? (int) l->box : 0);
        track(IDP_BOX_OPACITY, l->box_opacity * 100);
        track(IDP_BLUR, l->blur * 100);
        track(IDP_RADIUS, l->radius * 1000);
        track(IDP_PADDING, l->padding * 1000);
        track(IDP_THICKNESS, l->thickness * 2000);
        std::wstring file = l->path.empty() ? L"Choose picture…" : l->path.substr(l->path.find_last_of(L'\\') + 1);
        set(IDP_PICTURE, file);
    }
    layout_props();
    loading_ = false;
}

void SceneEditor::update_buttons() {
    if (!hwnd_) {
        return;
    }
    bool on_air = cur_ == host_->live_index();
    EnableWindow(GetDlgItem(hwnd_, IDE_APPLY), pending_);
    EnableWindow(GetDlgItem(hwnd_, IDE_GOLIVE), !on_air || pending_);
    SetDlgItemTextW(hwnd_, IDE_GOLIVE, on_air ? L"On air" : L"Put on air");
    SendDlgItemMessageW(hwnd_, IDE_APPLY_MODE, CB_SETCURSEL, (WPARAM) host_->settings().apply_mode, 0);
    bool layer = selected() != nullptr;
    EnableWindow(GetDlgItem(hwnd_, IDE_ALIGN), layer);
    EnableWindow(GetDlgItem(hwnd_, IDE_ORDER), layer);
    EnableWindow(GetDlgItem(hwnd_, IDE_LDUP), layer);
    EnableWindow(GetDlgItem(hwnd_, IDE_LDEL), layer);
    EnableWindow(GetDlgItem(hwnd_, IDE_DELETE), host_->profile().scenes.size() > 1);
    const int ids[4] = {IDE_MODE_SCENE, IDE_MODE_LIVE, IDE_MODE_IPAD, IDE_MODE_COMP};
    for (int i = 0; i < 4; ++i) {
        CheckDlgButton(hwnd_, ids[i], i == mode_ ? BST_CHECKED : BST_UNCHECKED);
    }
}

void SceneEditor::set_mode(int mode) {
    mode_ = mode;
    int pm = mode == ModeScene ? VideoPipeline::PreviewScene
                               : (mode == ModeLive ? VideoPipeline::PreviewLive : VideoPipeline::PreviewSource);
    host_->video().set_preview_mode(pm, mode == ModeComputer ? SourceKind::Computer : SourceKind::IPad);
    update_preview();
    update_buttons();
}

void SceneEditor::update_preview() {
    if (!hwnd_) {
        return;
    }
    std::vector<Studio::Mark> marks;
    RECT vr = view_rect();
    float k = vr.right > vr.left ? 1280.f / (float) (vr.right - vr.left) : 1.f; // preview pixels per screen pixel
    Layer *l = preview_only_ ? nullptr : selected();
    if (l) {
        uint32_t edge = l->locked ? 0xff8e8e96 : 0xff0a84ff;
        marks.push_back({l->x, l->y, l->w, l->h, edge, 2 * k, k});
        if (!l->locked) {
            float xs[3] = {l->x, l->x + l->w / 2, l->x + l->w}, ys[3] = {l->y, l->y + l->h / 2, l->y + l->h};
            for (int yi = 0; yi < 3; ++yi) {
                for (int xi = 0; xi < 3; ++xi) {
                    if (xi == 1 && yi == 1) {
                        continue;
                    }
                    marks.push_back({xs[xi], ys[yi], 0, 0, 0xff0a84ff, 0, 5 * k});
                    marks.push_back({xs[xi], ys[yi], 0, 0, 0xffffffff, 0, 3.5f * k});
                }
            }
        }
    }
    for (float g : guide_x_) {
        marks.push_back({g, 0, 0, 1, 0xffff2d95, 0, 0.6f * k});
    }
    for (float g : guide_y_) {
        marks.push_back({0, g, 1, 0, 0xffff2d95, 0, 0.6f * k});
    }
    host_->video().set_preview_scene(scene(), marks);
}

// After every edit: save, show it on air if that's the choice, redraw the preview.
void SceneEditor::edited(bool structure) {
    if (structure) {
        fill_layer_list();
    }
    bool on_air = cur_ == host_->live_index();
    bool instant = host_->settings().apply_mode == 0;
    host_->scene_changed(cur_, instant);
    if (on_air && !instant) {
        pending_ = true;
    }
    update_preview();
    update_buttons();
}

bool SceneEditor::pick_color(uint32_t &argb) {
    static COLORREF custom[16] = {RGB(255, 255, 255), RGB(10, 12, 20),  RGB(29, 78, 216),  RGB(10, 132, 255),
                                  RGB(48, 209, 88),   RGB(255, 159, 10), RGB(255, 69, 58), RGB(250, 204, 21)};
    CHOOSECOLORW cc = {sizeof(cc)};
    cc.hwndOwner = hwnd_;
    cc.rgbResult = RGB((argb >> 16) & 255, (argb >> 8) & 255, argb & 255);
    cc.lpCustColors = custom;
    cc.Flags = CC_RGBINIT | CC_FULLOPEN;
    if (!ChooseColorW(&cc)) {
        return false;
    }
    argb = 0xff000000u | ((uint32_t) GetRValue(cc.rgbResult) << 16) | ((uint32_t) GetGValue(cc.rgbResult) << 8) |
           GetBValue(cc.rgbResult);
    return true;
}

// Asks for a picture and keeps a copy beside the settings, so moving or deleting the original later doesn't
// break the scene.
bool SceneEditor::pick_picture(std::wstring &path) {
    wchar_t file[MAX_PATH] = {};
    OPENFILENAMEW ofn = {sizeof(ofn)};
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = L"Pictures (*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff)\0*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Choose a picture";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) {
        return false;
    }
    std::wstring src = file;
    if (Studio::load_picture(src, 16, 16, false).empty()) {
        MessageBoxW(hwnd_, L"That picture couldn't be opened. Try a PNG or JPEG.", L"AirScreen", MB_OK | MB_ICONWARNING);
        return false;
    }
    std::wstring dir = app_data_dir() + L"\\images";
    CreateDirectoryW(dir.c_str(), nullptr);
    const wchar_t *name = wcsrchr(file, L'\\');
    wchar_t prefix[32];
    swprintf_s(prefix, L"\\%llx_", (unsigned long long) GetTickCount64());
    std::wstring copy = dir + prefix + (name ? name + 1 : file);
    path = CopyFileW(src.c_str(), copy.c_str(), FALSE) ? copy : src;
    return true;
}

void SceneEditor::add_layer(LayerType type) {
    Profile &p = host_->profile();
    Scene &s = scene();
    Layer l = make_layer(type, p);
    if (type == LayerType::Image && !pick_picture(l.path)) {
        return;
    }
    size_t at = s.layers.size();
    if (type == LayerType::Source) {
        // Screens go above the other screens but under text and logos.
        at = 0;
        for (size_t i = 0; i < s.layers.size(); ++i) {
            if (s.layers[i].type == LayerType::Source) {
                at = i + 1;
            }
        }
        if (at > 0) {
            l.x = 0.62f, l.y = 0.58f, l.w = 0.34f, l.h = 0.36f; // a second screen starts small, bottom right
        }
    }
    s.layers.insert(s.layers.begin() + (ptrdiff_t) at, l);
    sel_ = l.id;
    edited(true);
    load_props();
}

void SceneEditor::layer_menu(int which) {
    HMENU m = CreatePopupMenu();
    if (which == IDE_ADD) {
        AppendMenuW(m, MF_STRING, M_ADD_IPAD, L"iPad screen");
        AppendMenuW(m, MF_STRING, M_ADD_COMPUTER, L"Computer screen");
        AppendMenuW(m, MF_STRING, M_ADD_TEXT, L"Text");
        AppendMenuW(m, MF_STRING, M_ADD_IMAGE, L"Picture or logo…");
        AppendMenuW(m, MF_STRING, M_ADD_FRAME, L"Frame");
    } else if (which == IDE_ALIGN) {
        AppendMenuW(m, MF_STRING, M_AL_CENTER, L"Centre");
        AppendMenuW(m, MF_STRING, M_AL_TOP, L"Top centre");
        AppendMenuW(m, MF_STRING, M_AL_BOTTOM, L"Bottom centre");
        AppendMenuW(m, MF_STRING, M_AL_TL, L"Top left corner");
        AppendMenuW(m, MF_STRING, M_AL_TR, L"Top right corner");
        AppendMenuW(m, MF_STRING, M_AL_BL, L"Bottom left corner");
        AppendMenuW(m, MF_STRING, M_AL_BR, L"Bottom right corner");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, M_AL_FILL, L"Fill the whole canvas");
    } else {
        AppendMenuW(m, MF_STRING, M_OR_FRONT, L"Bring to front");
        AppendMenuW(m, MF_STRING, M_OR_FORWARD, L"Bring forward");
        AppendMenuW(m, MF_STRING, M_OR_BACKWARD, L"Send backward");
        AppendMenuW(m, MF_STRING, M_OR_BACK, L"Send to back");
    }
    RECT r;
    GetWindowRect(GetDlgItem(hwnd_, which), &r);
    UINT cmd = (UINT) TrackPopupMenu(m, TPM_RETURNCMD | TPM_BOTTOMALIGN, r.left, r.top, 0, hwnd_, nullptr);
    DestroyMenu(m);
    if (!cmd) {
        return;
    }
    if (cmd >= M_ADD_IPAD && cmd <= M_ADD_FRAME) {
        if (cmd == M_ADD_IPAD || cmd == M_ADD_COMPUTER) {
            add_layer(LayerType::Source);
            if (Layer *l = selected()) {
                l->source = cmd == M_ADD_IPAD ? SourceKind::IPad : SourceKind::Computer;
                l->name = cmd == M_ADD_IPAD ? L"iPad screen" : L"Computer screen";
                edited(true);
                load_props();
            }
        } else {
            add_layer(cmd == M_ADD_TEXT ? LayerType::Text : (cmd == M_ADD_IMAGE ? LayerType::Image : LayerType::Frame));
        }
        return;
    }
    Layer *l = selected();
    if (!l) {
        return;
    }
    if (cmd >= M_AL_CENTER && cmd <= M_AL_FILL) {
        const Align al[] = {Align::Center,  Align::Top,        Align::Bottom,      Align::TopLeft,
                            Align::TopRight, Align::BottomLeft, Align::BottomRight, Align::Fill};
        align_layer(*l, al[cmd - M_AL_CENTER]);
        edited(false);
        load_props();
        return;
    }
    auto &v = scene().layers;
    size_t i = 0;
    while (i < v.size() && v[i].id != sel_) {
        ++i;
    }
    if (i == v.size()) {
        return;
    }
    Layer moving = v[i];
    v.erase(v.begin() + (ptrdiff_t) i);
    size_t to = i;
    if (cmd == M_OR_FRONT) {
        to = v.size();
    } else if (cmd == M_OR_FORWARD) {
        to = (std::min)(i + 1, v.size());
    } else if (cmd == M_OR_BACKWARD) {
        to = i > 0 ? i - 1 : 0;
    } else {
        to = 0;
    }
    v.insert(v.begin() + (ptrdiff_t) to, moving);
    edited(true);
}

void SceneEditor::snippet_menu() {
    Profile &p = host_->profile();
    Layer *l = selected();
    if (!l) {
        return;
    }
    HMENU m = CreatePopupMenu();
    HMENU del = CreatePopupMenu();
    for (size_t i = 0; i < p.snippets.size() && i < 200; ++i) {
        std::wstring item = p.snippets[i].name + L":  " + p.snippets[i].text.substr(0, 48);
        std::replace(item.begin(), item.end(), L'\n', L' ');
        AppendMenuW(m, MF_STRING, M_SNIP + (UINT) i, item.c_str());
        AppendMenuW(del, MF_STRING, M_SNIP_DEL + (UINT) i, p.snippets[i].name.c_str());
    }
    if (p.snippets.empty()) {
        AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"Nothing saved yet: use Save… first");
    } else {
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_POPUP, (UINT_PTR) del, L"Remove saved text");
    }
    RECT r;
    GetWindowRect(GetDlgItem(props_, IDP_INSERT_SNIP), &r);
    UINT cmd = (UINT) TrackPopupMenu(m, TPM_RETURNCMD, r.left, r.bottom, 0, hwnd_, nullptr);
    DestroyMenu(m);
    if (cmd >= M_SNIP && cmd < M_SNIP + p.snippets.size()) {
        l->text = p.snippets[cmd - M_SNIP].text;
        edited(false);
        load_props();
    } else if (cmd >= M_SNIP_DEL && cmd < M_SNIP_DEL + p.snippets.size()) {
        p.snippets.erase(p.snippets.begin() + (ptrdiff_t) (cmd - M_SNIP_DEL));
        host_->scenes_changed();
    }
}

void SceneEditor::scene_op(int id) {
    Profile &p = host_->profile();
    switch (id) {
    case IDE_NEW: {
        wchar_t def[32];
        swprintf_s(def, L"Scene %d", (int) p.scenes.size() + 1);
        std::wstring name = def;
        if (!prompt_text(hwnd_, L"New scene", L"Name of the new scene:", name) || name.empty()) {
            return;
        }
        Scene s = make_scene(name, p);
        p.scenes.insert(p.scenes.begin() + cur_ + 1, s);
        cur_ += 1;
        host_->scenes_changed();
        select_scene(cur_);
        break;
    }
    case IDE_DUP: {
        Scene s = copy_scene(scene(), p);
        s.name += L" copy";
        p.scenes.insert(p.scenes.begin() + cur_ + 1, s);
        cur_ += 1;
        host_->scenes_changed();
        select_scene(cur_);
        break;
    }
    case IDE_RENAME: {
        std::wstring name = scene().name;
        if (prompt_text(hwnd_, L"Rename scene", L"Scene name:", name) && !name.empty()) {
            scene().name = name;
            host_->scenes_changed();
            load_props();
        }
        break;
    }
    case IDE_DELETE: {
        if (p.scenes.size() <= 1) {
            return;
        }
        std::wstring q = L"Delete the scene \"" + scene().name + L"\"?";
        if (MessageBoxW(hwnd_, q.c_str(), L"AirScreen", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) {
            return;
        }
        p.scenes.erase(p.scenes.begin() + cur_);
        cur_ = (std::min)(cur_, (int) p.scenes.size() - 1);
        sel_ = 0;
        host_->scenes_changed();
        select_scene(cur_);
        break;
    }
    case IDE_UP:
    case IDE_DOWN: {
        int to = cur_ + (id == IDE_UP ? -1 : 1);
        if (to < 0 || to >= (int) p.scenes.size()) {
            return;
        }
        std::swap(p.scenes[(size_t) cur_], p.scenes[(size_t) to]);
        cur_ = to;
        host_->scenes_changed();
        select_scene(cur_);
        break;
    }
    case IDE_DEFAULT:
        p.default_scene = scene().id;
        host_->scenes_changed();
        break;
    }
}

void SceneEditor::on_command(int id, int code, HWND ctl) {
    Profile &p = host_->profile();
    if (id == IDE_SCENES && code == LBN_SELCHANGE) {
        select_scene((int) SendMessageW(scenes_, LB_GETCURSEL, 0, 0));
        return;
    }
    if (id == IDE_SCENES && code == LBN_DBLCLK) {
        scene_op(IDE_RENAME);
        return;
    }
    if (id == IDE_SCENE_COMBO && code == CBN_SELCHANGE) {
        select_scene((int) SendMessageW(scene_combo_, CB_GETCURSEL, 0, 0));
        set_mode(ModeScene);
        return;
    }
    if (id == IDE_LAYERS && code == LBN_SELCHANGE) {
        int i = (int) SendMessageW(layers_, LB_GETCURSEL, 0, 0);
        uint32_t lid = i >= 0 ? (uint32_t) SendMessageW(layers_, LB_GETITEMDATA, (WPARAM) i, 0) : 0;
        select_layer(lid);
        return;
    }
    if (code == BN_CLICKED) {
        switch (id) {
        case IDE_NEW:
        case IDE_DUP:
        case IDE_RENAME:
        case IDE_DELETE:
        case IDE_UP:
        case IDE_DOWN:
        case IDE_DEFAULT:
            scene_op(id);
            return;
        case IDE_MODE_SCENE:
        case IDE_MODE_LIVE:
        case IDE_MODE_IPAD:
        case IDE_MODE_COMP:
            set_mode(id - IDE_MODE_SCENE);
            return;
        case IDE_APPLY:
            host_->scene_changed(cur_, true);
            pending_ = false;
            update_buttons();
            return;
        case IDE_GOLIVE:
            if (cur_ == host_->live_index()) {
                host_->scene_changed(cur_, true);
            } else {
                host_->go_live(cur_);
            }
            pending_ = false;
            update_buttons();
            return;
        case IDE_EXPAND:
            preview_only_ = !preview_only_;
            layout();
            update_preview();
            return;
        case IDE_ADD:
        case IDE_ALIGN:
        case IDE_ORDER:
            layer_menu(id);
            return;
        case IDE_LDUP:
            if (Layer *l = selected()) {
                Layer c = *l;
                c.id = p.new_id();
                c.name += L" copy";
                c.x += 0.02f;
                c.y += 0.02f;
                auto &v = scene().layers;
                for (size_t i = 0; i < v.size(); ++i) {
                    if (v[i].id == sel_) {
                        v.insert(v.begin() + (ptrdiff_t) i + 1, c);
                        break;
                    }
                }
                sel_ = c.id;
                edited(true);
                load_props();
            }
            return;
        case IDE_LDEL: {
            auto &v = scene().layers;
            v.erase(std::remove_if(v.begin(), v.end(), [&](const Layer &l) { return l.id == sel_; }), v.end());
            sel_ = 0;
            edited(true);
            load_props();
            return;
        }
        }
    }
    if (id == IDE_APPLY_MODE && code == CBN_SELCHANGE) {
        host_->settings().apply_mode = (int) SendMessageW(ctl, CB_GETCURSEL, 0, 0) == 1 ? 1 : 0;
        host_->save_settings();
        if (host_->settings().apply_mode == 0 && pending_) {
            host_->scene_changed(cur_, true); // switching to instant applies what was waiting
            pending_ = false;
        }
        update_buttons();
        return;
    }
    if (loading_ || id < IDP_SCENE_NAME) {
        return;
    }

    // Properties.
    Scene &s = scene();
    Layer *l = selected();
    switch (id) {
    case IDP_SCENE_NAME:
        if (code == EN_CHANGE) {
            s.name = get_text(ctl);
            edited(false);
            fill_scene_list();
        }
        return;
    case IDP_BACKDROP:
        if (code == CBN_SELCHANGE) {
            s.backdrop = (Backdrop) SendMessageW(ctl, CB_GETCURSEL, 0, 0);
            if (s.backdrop == Backdrop::Image && s.bg_image.empty() && !pick_picture(s.bg_image)) {
                s.backdrop = Backdrop::Colors;
                load_props();
            }
            edited(false);
        }
        return;
    case IDP_MOOD:
        if (code == CBN_SELCHANGE) {
            s.mood = (int) SendMessageW(ctl, CB_GETCURSEL, 0, 0);
            s.backdrop = Backdrop::Colors;
            load_props();
            edited(false);
        }
        return;
    case IDP_BG_COLOR:
        if (pick_color(s.bg_color)) {
            s.backdrop = Backdrop::Solid;
            load_props();
            edited(false);
        }
        return;
    case IDP_BG_PICTURE:
        if (pick_picture(s.bg_image)) {
            s.backdrop = Backdrop::Image;
            load_props();
            edited(false);
        }
        return;
    }
    if (!l) {
        return;
    }
    float v;
    switch (id) {
    case IDP_NAME:
        if (code == EN_CHANGE) {
            l->name = get_text(ctl);
            edited(false);
            InvalidateRect(layers_, nullptr, FALSE);
        }
        break;
    case IDP_VISIBLE:
        l->visible = IsDlgButtonChecked(props_, id) == BST_CHECKED;
        edited(false);
        InvalidateRect(layers_, nullptr, FALSE);
        break;
    case IDP_LOCKED:
        l->locked = IsDlgButtonChecked(props_, id) == BST_CHECKED;
        edited(false);
        InvalidateRect(layers_, nullptr, FALSE);
        break;
    case IDP_X:
    case IDP_Y:
    case IDP_W:
    case IDP_H:
        if (code == EN_CHANGE && parse_pct(ctl, v)) {
            float *f[] = {&l->x, &l->y, &l->w, &l->h};
            if (id >= IDP_W) {
                v = std::clamp(v, 0.01f, 4.f);
            }
            *f[id - IDP_X] = v;
            edited(false);
        }
        break;
    case IDP_SOURCE:
        if (code == CBN_SELCHANGE) {
            l->source = (SourceKind) SendMessageW(ctl, CB_GETCURSEL, 0, 0);
            if (l->name == L"iPad screen" || l->name == L"Computer screen") {
                l->name = l->source == SourceKind::IPad ? L"iPad screen" : L"Computer screen";
                SetDlgItemTextW(props_, IDP_NAME, l->name.c_str());
            }
            edited(true);
        }
        break;
    case IDP_FILL:
        l->fill = IsDlgButtonChecked(props_, id) == BST_CHECKED;
        edited(false);
        break;
    case IDP_ROUNDED:
        l->rounded = IsDlgButtonChecked(props_, id) == BST_CHECKED;
        edited(false);
        break;
    case IDP_TEXT:
        if (code == EN_CHANGE) {
            l->text = from_crlf(get_text(ctl));
            edited(false);
        }
        break;
    case IDP_COLOR:
        if (pick_color(l->color)) {
            edited(false);
        }
        break;
    case IDP_BORDER_COLOR:
        if (pick_color(l->color)) {
            edited(false);
        }
        break;
    case IDP_BOLD:
        l->bold = IsDlgButtonChecked(props_, id) == BST_CHECKED;
        edited(false);
        break;
    case IDP_ALIGN:
        if (code == CBN_SELCHANGE) {
            l->align = (int) SendMessageW(ctl, CB_GETCURSEL, 0, 0);
            edited(false);
        }
        break;
    case IDP_SAVE_SNIP: {
        std::wstring name = l->name.empty() ? L"Text" : l->name;
        if (!l->text.empty() && prompt_text(hwnd_, L"Save text", L"Save this text for reuse in any scene as:", name) &&
            !name.empty()) {
            p.snippets.push_back({name, l->text});
            host_->scenes_changed();
        }
        break;
    }
    case IDP_INSERT_SNIP:
        snippet_menu();
        break;
    case IDP_BOX:
        if (code == CBN_SELCHANGE) {
            l->box = (BoxStyle) SendMessageW(ctl, CB_GETCURSEL, 0, 0);
            l->box_on = true;
            edited(false);
        }
        break;
    case IDP_BOX_COLOR:
        if (pick_color(l->box_color)) {
            edited(false);
        }
        break;
    case IDP_BOX_COLOR2:
        if (pick_color(l->box_color2)) {
            edited(false);
        }
        break;
    case IDP_PICTURE: {
        std::wstring path;
        if (pick_picture(path)) {
            l->path = path;
            edited(false);
            load_props();
        }
        break;
    }
    }
}

void SceneEditor::on_scroll(HWND ctl) {
    if (loading_) {
        return;
    }
    Layer *l = selected();
    if (!l) {
        return;
    }
    float v = (float) SendMessageW(ctl, TBM_GETPOS, 0, 0);
    switch (GetDlgCtrlID(ctl)) {
    case IDP_OPACITY:
        l->opacity = v / 100.f;
        break;
    case IDP_FONT:
        l->font = v / 1000.f;
        break;
    case IDP_BOX_OPACITY:
        l->box_opacity = v / 100.f;
        break;
    case IDP_BLUR:
        l->blur = v / 100.f;
        break;
    case IDP_RADIUS:
        l->radius = v / 1000.f;
        break;
    case IDP_PADDING:
        l->padding = v / 1000.f;
        break;
    case IDP_THICKNESS:
        l->thickness = v / 2000.f;
        break;
    default:
        return;
    }
    edited(false);
}

RECT SceneEditor::view_rect() const {
    RECT cr = {};
    GetClientRect(preview_, &cr);
    int cw = cr.right, ch = cr.bottom;
    int vw = cw, vh = cw * 9 / 16;
    if (vh > ch) {
        vh = ch;
        vw = ch * 16 / 9;
    }
    int ox = (cw - vw) / 2, oy = (ch - vh) / 2;
    return RECT{ox, oy, ox + vw, oy + vh};
}

bool SceneEditor::to_canvas(POINT pt, float &fx, float &fy) const {
    RECT v = view_rect();
    if (v.right <= v.left || v.bottom <= v.top) {
        return false;
    }
    fx = (float) (pt.x - v.left) / (float) (v.right - v.left);
    fy = (float) (pt.y - v.top) / (float) (v.bottom - v.top);
    return true;
}

// 0 top-left, then clockwise: 1 top, 2 top-right, 3 right, 4 bottom-right, 5 bottom, 6 bottom-left, 7 left.
int SceneEditor::handle_at(POINT pt) const {
    const Layer *l = sel_ ? const_cast<SceneEditor *>(this)->scene().find(sel_) : nullptr;
    if (!l || l->locked) {
        return -1;
    }
    RECT v = view_rect();
    float vw = (float) (v.right - v.left), vh = (float) (v.bottom - v.top);
    float xs[3] = {l->x, l->x + l->w / 2, l->x + l->w}, ys[3] = {l->y, l->y + l->h / 2, l->y + l->h};
    const int map[8][2] = {{0, 0}, {1, 0}, {2, 0}, {2, 1}, {2, 2}, {1, 2}, {0, 2}, {0, 1}};
    int tol = MulDiv(7, dpi_, 96);
    for (int i = 0; i < 8; ++i) {
        int hx = v.left + (int) std::lround(xs[map[i][0]] * vw), hy = v.top + (int) std::lround(ys[map[i][1]] * vh);
        if (std::abs(pt.x - hx) <= tol && std::abs(pt.y - hy) <= tol) {
            return i;
        }
    }
    return -1;
}

uint32_t SceneEditor::layer_at(float fx, float fy) const {
    const Scene &s = const_cast<SceneEditor *>(this)->scene();
    for (size_t i = s.layers.size(); i-- > 0;) {
        const Layer &l = s.layers[i];
        if (l.visible && fx >= l.x && fx <= l.x + l.w && fy >= l.y && fy <= l.y + l.h) {
            return l.id;
        }
    }
    return 0;
}

// Moves or resizes the selected layer to follow the mouse, snapping its edges and centre to the canvas edges
// and centre and to the other layers.
void SceneEditor::drag_to(POINT pt, bool snap, bool keep_aspect) {
    Layer *l = selected();
    float fx, fy;
    if (!l || !to_canvas(pt, fx, fy)) {
        return;
    }
    RECT v = view_rect();
    float tolx = (float) dpi(8) / (float) (std::max)(1L, v.right - v.left);
    float toly = (float) dpi(8) / (float) (std::max)(1L, v.bottom - v.top);
    std::vector<float> tx = {0, 0.5f, 1}, ty = {0, 0.5f, 1};
    for (auto &o : scene().layers) {
        if (o.id != l->id && o.visible) {
            tx.insert(tx.end(), {o.x, o.x + o.w / 2, o.x + o.w});
            ty.insert(ty.end(), {o.y, o.y + o.h / 2, o.y + o.h});
        }
    }
    guide_x_.clear();
    guide_y_.clear();
    // The smallest correction that lines one of `edges` up with a target, within tolerance.
    auto best = [](const std::vector<float> &edges, const std::vector<float> &targets, float tol, float &shift,
                   float &line) {
        float bd = tol;
        bool found = false;
        for (float e : edges) {
            for (float t : targets) {
                if (std::fabs(t - e) < bd) {
                    bd = std::fabs(t - e);
                    shift = t - e;
                    line = t;
                    found = true;
                }
            }
        }
        return found;
    };
    float dx = fx - start_fx_, dy = fy - start_fy_;
    if (drag_ == DragMove) {
        float nx = start_.x + dx, ny = start_.y + dy, sh, line;
        if (snap && best({nx, nx + l->w / 2, nx + l->w}, tx, tolx, sh, line)) {
            nx += sh;
            guide_x_.push_back(line);
        }
        if (snap && best({ny, ny + l->h / 2, ny + l->h}, ty, toly, sh, line)) {
            ny += sh;
            guide_y_.push_back(line);
        }
        l->x = nx;
        l->y = ny;
    } else {
        float x0 = start_.x, y0 = start_.y, x1 = x0 + start_.w, y1 = y0 + start_.h;
        int h = handle_;
        bool L = h == 0 || h == 6 || h == 7, R = h == 2 || h == 3 || h == 4;
        bool T = h == 0 || h == 1 || h == 2, B = h == 4 || h == 5 || h == 6;
        float sh, line;
        if (L) {
            x0 += dx;
            if (snap && best({x0}, tx, tolx, sh, line)) {
                x0 += sh;
                guide_x_.push_back(line);
            }
        }
        if (R) {
            x1 += dx;
            if (snap && best({x1}, tx, tolx, sh, line)) {
                x1 += sh;
                guide_x_.push_back(line);
            }
        }
        if (T) {
            y0 += dy;
            if (snap && best({y0}, ty, toly, sh, line)) {
                y0 += sh;
                guide_y_.push_back(line);
            }
        }
        if (B) {
            y1 += dy;
            if (snap && best({y1}, ty, toly, sh, line)) {
                y1 += sh;
                guide_y_.push_back(line);
            }
        }
        if (keep_aspect && (L || R) && (T || B) && start_.h > 0) {
            float aspect = start_.w / start_.h;
            float nw = std::fabs(x1 - x0);
            float nh = nw / aspect;
            if (T) {
                y0 = y1 - nh;
            } else {
                y1 = y0 + nh;
            }
            guide_x_.clear();
            guide_y_.clear();
        }
        const float min_size = 0.01f;
        if (x1 - x0 < min_size) {
            (L ? x0 : x1) = L ? x1 - min_size : x0 + min_size;
        }
        if (y1 - y0 < min_size) {
            (T ? y0 : y1) = T ? y1 - min_size : y0 + min_size;
        }
        l->x = x0;
        l->y = y0;
        l->w = x1 - x0;
        l->h = y1 - y0;
    }
    loading_ = true;
    SetDlgItemTextW(props_, IDP_X, pct(l->x).c_str());
    SetDlgItemTextW(props_, IDP_Y, pct(l->y).c_str());
    SetDlgItemTextW(props_, IDP_W, pct(l->w).c_str());
    SetDlgItemTextW(props_, IDP_H, pct(l->h).c_str());
    loading_ = false;
    edited(false);
}

LRESULT SceneEditor::preview_msg(UINT msg, WPARAM wp, LPARAM lp) {
    POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    bool editing = !preview_only_ && mode_ == ModeScene;
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(preview_, &ps); // drawn by the render thread
        EndPaint(preview_, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        host_->video().preview_resize();
        update_preview();
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_LBUTTONDOWN: {
        SetFocus(preview_);
        if (!editing) {
            return 0;
        }
        float fx, fy;
        if (!to_canvas(pt, fx, fy)) {
            return 0;
        }
        int h = handle_at(pt);
        if (h >= 0) {
            drag_ = DragResize;
            handle_ = h;
        } else {
            select_layer(layer_at(fx, fy));
            Layer *l = selected();
            drag_ = l && !l->locked ? DragMove : DragNone;
        }
        if (drag_ != DragNone) {
            start_ = *selected();
            start_fx_ = fx;
            start_fy_ = fy;
            SetCapture(preview_);
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (drag_ != DragNone) {
            bool snap = IsDlgButtonChecked(hwnd_, IDE_SNAP) == BST_CHECKED && !(GetKeyState(VK_MENU) & 0x8000);
            drag_to(pt, snap, (wp & MK_SHIFT) != 0);
        }
        return 0;
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
        if (drag_ != DragNone) {
            drag_ = DragNone;
            guide_x_.clear();
            guide_y_.clear();
            if (msg == WM_LBUTTONUP) {
                ReleaseCapture();
            }
            update_preview();
        }
        return 0;
    case WM_LBUTTONDBLCLK:
        if (editing && selected() && selected()->type == LayerType::Text) {
            HWND t = GetDlgItem(props_, IDP_TEXT);
            SetFocus(t);
            SendMessageW(t, EM_SETSEL, 0, -1);
        }
        return 0;
    case WM_SETCURSOR:
        if (editing && LOWORD(lp) == HTCLIENT) {
            POINT c;
            GetCursorPos(&c);
            ScreenToClient(preview_, &c);
            int h = drag_ == DragResize ? handle_ : handle_at(c);
            LPCWSTR cur = IDC_ARROW;
            if (h >= 0) {
                cur = (h == 0 || h == 4) ? IDC_SIZENWSE : (h == 2 || h == 6) ? IDC_SIZENESW : (h == 1 || h == 5) ? IDC_SIZENS : IDC_SIZEWE;
            } else {
                float fx, fy;
                Layer *l = selected();
                if (drag_ == DragMove || (l && !l->locked && to_canvas(c, fx, fy) && layer_at(fx, fy) == l->id)) {
                    cur = IDC_SIZEALL;
                }
            }
            SetCursor(LoadCursor(nullptr, cur));
            return TRUE;
        }
        break;
    case WM_KEYDOWN: {
        Layer *l = editing ? selected() : nullptr;
        if (!l) {
            return 0;
        }
        float step = (GetKeyState(VK_SHIFT) & 0x8000) ? 0.01f : 0.001f;
        bool moved = true;
        if (l->locked && wp != VK_ESCAPE) {
            return 0;
        }
        switch (wp) {
        case VK_LEFT:
            l->x -= step;
            break;
        case VK_RIGHT:
            l->x += step;
            break;
        case VK_UP:
            l->y -= step;
            break;
        case VK_DOWN:
            l->y += step;
            break;
        case VK_DELETE:
            on_command(IDE_LDEL, BN_CLICKED, nullptr);
            return 0;
        case VK_ESCAPE:
            select_layer(0);
            return 0;
        case 'D':
            if (GetKeyState(VK_CONTROL) & 0x8000) {
                on_command(IDE_LDUP, BN_CLICKED, nullptr);
            }
            return 0;
        default:
            moved = false;
        }
        if (moved) {
            edited(false);
            load_props();
        }
        return 0;
    }
    }
    return DefWindowProcW(preview_, msg, wp, lp);
}

LRESULT CALLBACK SceneEditor::layers_subclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    auto *self = (SceneEditor *) ref;
    if (msg == WM_LBUTTONDOWN && GET_X_LPARAM(lp) < self->dpi(26)) {
        // The dot at the start of each row shows or hides the layer without selecting it.
        LRESULT r = SendMessageW(hwnd, LB_ITEMFROMPOINT, 0, lp);
        if (!HIWORD(r)) {
            uint32_t id = (uint32_t) SendMessageW(hwnd, LB_GETITEMDATA, LOWORD(r), 0);
            if (Layer *l = self->scene().find(id)) {
                l->visible = !l->visible;
                self->edited(false);
                self->loading_ = true;
                if (id == self->sel_) {
                    CheckDlgButton(self->props_, IDP_VISIBLE, l->visible ? BST_CHECKED : BST_UNCHECKED);
                }
                self->loading_ = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

LRESULT CALLBACK SceneEditor::preview_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto *self = (SceneEditor *) GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == WM_NCCREATE) {
        self = (SceneEditor *) ((CREATESTRUCTW *) lp)->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR) self);
        self->preview_ = hwnd;
    }
    if (self && self->host_ && self->hwnd_) {
        return self->preview_msg(msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK SceneEditor::props_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto *self = (SceneEditor *) GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == WM_NCCREATE) {
        self = (SceneEditor *) ((CREATESTRUCTW *) lp)->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR) self);
    }
    if (!self) {
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    switch (msg) {
    case WM_COMMAND:
        self->on_command(LOWORD(wp), HIWORD(wp), (HWND) lp);
        return 0;
    case WM_HSCROLL:
        if (lp) {
            self->on_scroll((HWND) lp);
        }
        return 0;
    case WM_SIZE:
        if (self->hwnd_) {
            self->layout_props();
        }
        return 0;
    case WM_MOUSEWHEEL:
    case WM_VSCROLL: {
        RECT pr;
        GetClientRect(hwnd, &pr);
        int max_scroll = (std::max)(0, self->props_h_ - (int) pr.bottom);
        int pos = self->scroll_;
        if (msg == WM_MOUSEWHEEL) {
            pos -= GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA * self->dpi(48);
        } else {
            switch (LOWORD(wp)) {
            case SB_LINEUP:
                pos -= self->dpi(28);
                break;
            case SB_LINEDOWN:
                pos += self->dpi(28);
                break;
            case SB_PAGEUP:
                pos -= pr.bottom;
                break;
            case SB_PAGEDOWN:
                pos += pr.bottom;
                break;
            case SB_THUMBTRACK:
            case SB_THUMBPOSITION:
                pos = HIWORD(wp);
                break;
            }
        }
        pos = std::clamp(pos, 0, max_scroll);
        if (pos != self->scroll_) {
            self->scroll_ = pos;
            self->layout_props();
            InvalidateRect(hwnd, nullptr, TRUE);
        }
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK SceneEditor::proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto *self = (SceneEditor *) GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == WM_NCCREATE) {
        self = (SceneEditor *) ((CREATESTRUCTW *) lp)->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR) self);
        self->hwnd_ = hwnd;
    }
    if (self) {
        return self->handle(msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT SceneEditor::handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        BOOL dark = FALSE;
        DwmSetWindowAttribute(hwnd_, 20, &dark, sizeof(dark));
        return 0;
    }
    case WM_SIZE:
        if (font_) {
            layout();
            update_preview();
        }
        return 0;
    case WM_GETMINMAXINFO: {
        auto *mm = (MINMAXINFO *) lp;
        mm->ptMinTrackSize.x = dpi(760);
        mm->ptMinTrackSize.y = dpi(480);
        return 0;
    }
    case WM_DPICHANGED: {
        dpi_ = HIWORD(wp);
        HFONT old = font_;
        font_ = CreateFontW(-dpi(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        EnumChildWindows(
            hwnd_,
            [](HWND c, LPARAM f) {
                SendMessageW(c, WM_SETFONT, (WPARAM) f, TRUE);
                return TRUE;
            },
            (LPARAM) font_);
        DeleteObject(old);
        RECT *r = (RECT *) lp;
        SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_COMMAND:
        on_command(LOWORD(wp), HIWORD(wp), (HWND) lp);
        return 0;
    case WM_MEASUREITEM: {
        auto *mi = (MEASUREITEMSTRUCT *) lp;
        mi->itemHeight = dpi(24);
        return TRUE;
    }
    case WM_DRAWITEM: {
        auto *di = (DRAWITEMSTRUCT *) lp;
        if (di->CtlID != IDE_LAYERS || di->itemID == (UINT) -1) {
            return TRUE;
        }
        const Layer *l = scene().find((uint32_t) di->itemData);
        bool sel = (di->itemState & ODS_SELECTED) != 0;
        HBRUSH bg = GetSysColorBrush(sel ? COLOR_HIGHLIGHT : COLOR_WINDOW);
        FillRect(di->hDC, &di->rcItem, bg);
        if (!l) {
            return TRUE;
        }
        SetBkMode(di->hDC, TRANSPARENT);
        COLORREF fg = GetSysColor(sel ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT);
        RECT r = di->rcItem;
        RECT eye = {r.left + dpi(6), r.top, r.left + dpi(24), r.bottom};
        SetTextColor(di->hDC, l->visible ? (sel ? fg : RGB(10, 132, 255)) : GetSysColor(COLOR_GRAYTEXT));
        DrawTextW(di->hDC, l->visible ? L"●" : L"○", -1, &eye, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        RECT name = {r.left + dpi(28), r.top, r.right - dpi(90), r.bottom};
        SetTextColor(di->hDC, l->visible ? fg : GetSysColor(sel ? COLOR_HIGHLIGHTTEXT : COLOR_GRAYTEXT));
        std::wstring label = l->name.empty() ? kTypeName[(int) l->type] : l->name;
        DrawTextW(di->hDC, label.c_str(), -1, &name, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        RECT kind = {r.right - dpi(88), r.top, r.right - dpi(6), r.bottom};
        std::wstring k = l->type == LayerType::Source ? (l->source == SourceKind::IPad ? L"iPad" : L"Computer")
                                                      : kTypeName[(int) l->type];
        if (l->locked) {
            k = L"🔒 " + k;
        }
        SetTextColor(di->hDC, sel ? fg : GetSysColor(COLOR_GRAYTEXT));
        DrawTextW(di->hDC, k.c_str(), -1, &kind, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        if (di->itemState & ODS_FOCUS) {
            DrawFocusRect(di->hDC, &di->rcItem);
        }
        return TRUE;
    }
    case WM_CLOSE:
        close();
        return 0;
    case WM_DESTROY:
        if (font_) {
            DeleteObject(font_);
            font_ = nullptr;
        }
        rows_.clear();
        left_.clear();
        right_.clear();
        bottom_.clear();
        full_only_.clear();
        preview_only_ctl_.clear();
        hwnd_ = nullptr;
        preview_ = nullptr;
        props_ = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

} // namespace airscreen
