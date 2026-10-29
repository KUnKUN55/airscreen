#include "teach.h"
#include "settings.h"
#include "app/resource.h"

#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <algorithm>
#include <sstream>

#pragma comment(lib, "uxtheme.lib")

namespace airscreen {
namespace {

HINSTANCE module() {
    return GetModuleHandleW(nullptr);
}

std::wstring utf16(const std::string &s) {
    if (s.empty()) {
        return {};
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int) s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int) s.size(), w.data(), n);
    return w;
}

std::string utf8(const std::wstring &w) {
    if (w.empty()) {
        return {};
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int) w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int) w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring dlg_text(HWND h, int id) {
    HWND c = GetDlgItem(h, id);
    int n = GetWindowTextLengthW(c);
    std::wstring s(n, 0);
    if (n > 0) {
        GetWindowTextW(c, s.data(), n + 1);
    }
    return s;
}

std::wstring crlf(const std::wstring &s) {
    std::wstring o;
    for (wchar_t c : s) {
        o += c == L'\n' ? std::wstring(L"\r\n") : std::wstring(1, c);
    }
    return o;
}

std::wstring lf(const std::wstring &s) {
    std::wstring o;
    for (wchar_t c : s) {
        if (c != L'\r') {
            o += c;
        }
    }
    return o;
}

// ---- Text prompt ----

struct Prompt {
    const wchar_t *title;
    const wchar_t *label;
    std::wstring *value;
};

INT_PTR CALLBACK prompt_proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    auto *st = (Prompt *) GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (msg) {
    case WM_INITDIALOG:
        st = (Prompt *) l;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR) st);
        SetWindowTextW(h, st->title);
        SetDlgItemTextW(h, IDC_PROMPT_LABEL, st->label);
        SetDlgItemTextW(h, IDC_PROMPT_EDIT, st->value->c_str());
        SendDlgItemMessageW(h, IDC_PROMPT_EDIT, EM_LIMITTEXT, 120, 0);
        SendDlgItemMessageW(h, IDC_PROMPT_EDIT, EM_SETSEL, 0, -1);
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(w) == IDOK) {
            std::wstring v = dlg_text(h, IDC_PROMPT_EDIT);
            v.erase(0, v.find_first_not_of(L" \t"));
            v.erase(v.find_last_not_of(L" \t") + 1);
            *st->value = v;
            EndDialog(h, IDOK);
            return TRUE;
        }
        if (LOWORD(w) == IDCANCEL) {
            EndDialog(h, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

// ---- Computer screen picker ----

struct Picker {
    TeachHost *host;
    CaptureTarget *out;
    std::vector<CaptureTarget> items;
    HTHUMBNAIL thumb = nullptr;
    HBITMAP snap = nullptr;
    int snap_w = 0, snap_h = 0;
    RECT box{}; // preview area, dialog client coordinates
};

std::string target_key(const CaptureTarget &t) {
    if (t.kind == CaptureTarget::Display) {
        return "display:" + std::to_string(t.number);
    }
    return "window:" + utf8(t.name);
}

void picker_fill(HWND h, Picker *st) {
    st->items = list_displays();
    std::vector<CaptureTarget> wins = list_windows();
    st->items.insert(st->items.end(), wins.begin(), wins.end());
    HWND list = GetDlgItem(h, IDC_PICK_LIST);
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    int pick = 0;
    const std::string &last = st->host->settings().last_source;
    for (size_t i = 0; i < st->items.size(); ++i) {
        const CaptureTarget &t = st->items[i];
        std::wstring item = t.kind == CaptureTarget::Display ? L"Whole " + t.name : L"Window: " + t.name;
        if (t.minimised) {
            item += L"  (minimised)";
        }
        if (!t.detail.empty()) {
            item += L"   (" + t.detail + L")";
        }
        SendMessageW(list, LB_ADDSTRING, 0, (LPARAM) item.c_str());
        if (!last.empty() && target_key(t) == last) {
            pick = (int) i;
        }
    }
    SendMessageW(list, LB_SETCURSEL, (WPARAM) pick, 0);
}

void picker_show(HWND h, Picker *st) {
    if (st->thumb) {
        DwmUnregisterThumbnail(st->thumb);
        st->thumb = nullptr;
    }
    if (st->snap) {
        DeleteObject(st->snap);
        st->snap = nullptr;
    }
    int i = (int) SendDlgItemMessageW(h, IDC_PICK_LIST, LB_GETCURSEL, 0, 0);
    EnableWindow(GetDlgItem(h, IDOK), i >= 0);
    if (i < 0 || i >= (int) st->items.size()) {
        SetDlgItemTextW(h, IDC_PICK_WARN, L"");
        InvalidateRect(h, &st->box, TRUE);
        return;
    }
    const CaptureTarget &t = st->items[(size_t) i];
    int bw = st->box.right - st->box.left, bh = st->box.bottom - st->box.top;
    if (t.kind == CaptureTarget::Window) {
        // A live thumbnail from the window manager: no capture happens until Share is pressed.
        if (SUCCEEDED(DwmRegisterThumbnail(h, (HWND) t.window, &st->thumb))) {
            SIZE src = {};
            DwmQueryThumbnailSourceSize(st->thumb, &src);
            float s = src.cx > 0 && src.cy > 0 ? (std::min)((float) bw / src.cx, (float) bh / src.cy) : 1.f;
            int w = (int) (src.cx * s), hh = (int) (src.cy * s);
            DWM_THUMBNAIL_PROPERTIES p = {};
            p.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_OPACITY;
            p.rcDestination = {st->box.left + (bw - w) / 2, st->box.top + (bh - hh) / 2,
                               st->box.left + (bw - w) / 2 + w, st->box.top + (bh - hh) / 2 + hh};
            p.fVisible = TRUE;
            p.opacity = 255;
            DwmUpdateThumbnailProperties(st->thumb, &p);
        }
        SetDlgItemTextW(h, IDC_PICK_WARN,
                        t.minimised
                            ? L"This window is minimised. Restore it, or nothing will show until you do."
                            : L"Only this window is shared. Other windows, pop-ups and notifications are not.");
    } else {
        // A still of the display, made here only for this preview.
        float s = (std::min)((float) bw / t.w, (float) bh / t.h);
        st->snap_w = (std::max)(1, (int) (t.w * s));
        st->snap_h = (std::max)(1, (int) (t.h * s));
        HDC screen = GetDC(nullptr);
        HDC mem = CreateCompatibleDC(screen);
        st->snap = CreateCompatibleBitmap(screen, st->snap_w, st->snap_h);
        HGDIOBJ old = SelectObject(mem, st->snap);
        SetStretchBltMode(mem, HALFTONE);
        StretchBlt(mem, 0, 0, st->snap_w, st->snap_h, screen, t.x, t.y, t.w, t.h, SRCCOPY | CAPTUREBLT);
        SelectObject(mem, old);
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        SetDlgItemTextW(h, IDC_PICK_WARN,
                        L"⚠ The whole display is shared: every window you open on it, notifications and pop-ups "
                        L"included. To share just one app, pick its window instead.");
    }
    InvalidateRect(h, &st->box, TRUE);
}

INT_PTR CALLBACK picker_proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    auto *st = (Picker *) GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (msg) {
    case WM_INITDIALOG: {
        st = (Picker *) l;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR) st);
        HWND frame = GetDlgItem(h, IDC_PICK_PREVIEW);
        GetWindowRect(frame, &st->box);
        MapWindowPoints(nullptr, h, (POINT *) &st->box, 2);
        ShowWindow(frame, SW_HIDE); // drawn by the dialog itself
        CheckDlgButton(h, IDC_PICK_CURSOR, st->host->settings().capture_cursor ? BST_CHECKED : BST_UNCHECKED);
        if (st->host->settings().hide_controls) {
            hide_from_capture(h, true);
        }
        picker_fill(h, st);
        picker_show(h, st);
        return TRUE;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        HBRUSH bg = CreateSolidBrush(RGB(24, 26, 30));
        FillRect(dc, &st->box, bg);
        DeleteObject(bg);
        if (st->snap) {
            HDC mem = CreateCompatibleDC(dc);
            HGDIOBJ old = SelectObject(mem, st->snap);
            int bw = st->box.right - st->box.left, bh = st->box.bottom - st->box.top;
            BitBlt(dc, st->box.left + (bw - st->snap_w) / 2, st->box.top + (bh - st->snap_h) / 2, st->snap_w,
                   st->snap_h, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteDC(mem);
        }
        EndPaint(h, &ps);
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case IDC_PICK_LIST:
            if (HIWORD(w) == LBN_SELCHANGE) {
                picker_show(h, st);
            } else if (HIWORD(w) == LBN_DBLCLK) {
                SendMessageW(h, WM_COMMAND, IDOK, 0);
            }
            return TRUE;
        case IDC_PICK_REFRESH:
            picker_fill(h, st);
            picker_show(h, st);
            return TRUE;
        case IDOK: {
            int i = (int) SendDlgItemMessageW(h, IDC_PICK_LIST, LB_GETCURSEL, 0, 0);
            if (i < 0 || i >= (int) st->items.size()) {
                return TRUE;
            }
            *st->out = st->items[(size_t) i];
            Settings &s = st->host->settings();
            s.capture_cursor = IsDlgButtonChecked(h, IDC_PICK_CURSOR) == BST_CHECKED;
            s.last_source = target_key(*st->out);
            st->host->save_settings();
            EndDialog(h, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(h, IDCANCEL);
            return TRUE;
        }
        break;
    case WM_DESTROY:
        if (st && st->thumb) {
            DwmUnregisterThumbnail(st->thumb);
            st->thumb = nullptr;
        }
        if (st && st->snap) {
            DeleteObject(st->snap);
            st->snap = nullptr;
        }
        break;
    }
    return FALSE;
}

// ---- Text on air ----

struct LiveText {
    TeachHost *host;
    std::vector<uint32_t> ids; // text layers of the scene on air; 0 = a new one
};

void live_text_load(HWND h, LiveText *st) {
    int i = (int) SendDlgItemMessageW(h, IDC_LT_LAYER, CB_GETCURSEL, 0, 0);
    uint32_t id = i >= 0 && i < (int) st->ids.size() ? st->ids[(size_t) i] : 0;
    const Layer *l = id ? st->host->live_scene().find(id) : nullptr;
    SetDlgItemTextW(h, IDC_LT_TEXT, l ? crlf(l->text).c_str() : L"");
    CheckDlgButton(h, IDC_LT_SHOW, BST_CHECKED); // opening this dialog almost always means "show it"
}

INT_PTR CALLBACK live_text_proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    auto *st = (LiveText *) GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (msg) {
    case WM_INITDIALOG: {
        st = (LiveText *) l;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR) st);
        const Scene &s = st->host->live_scene();
        SetWindowTextW(h, (L"Text on air  ·  " + s.name).c_str());
        HWND combo = GetDlgItem(h, IDC_LT_LAYER);
        for (auto &layer : s.layers) {
            if (layer.type == LayerType::Text) {
                std::wstring first = layer.text.substr(0, layer.text.find(L'\n'));
                std::wstring item = (layer.name.empty() ? L"Text" : layer.name) + L"  ·  " + first.substr(0, 40);
                SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM) item.c_str());
                st->ids.push_back(layer.id);
            }
        }
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM) L"New text, top of the screen");
        st->ids.push_back(0);
        SendMessageW(combo, CB_SETCURSEL, 0, 0);
        HWND snips = GetDlgItem(h, IDC_LT_SNIP);
        for (auto &sn : st->host->profile().snippets) {
            SendMessageW(snips, CB_ADDSTRING, 0, (LPARAM) sn.name.c_str());
        }
        SendMessageW(snips, CB_SETCURSEL, 0, 0);
        EnableWindow(GetDlgItem(h, IDC_LT_USE), !st->host->profile().snippets.empty());
        live_text_load(h, st);
        SetFocus(GetDlgItem(h, IDC_LT_TEXT));
        SendDlgItemMessageW(h, IDC_LT_TEXT, EM_SETSEL, 0, -1);
        return FALSE;
    }
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case IDC_LT_LAYER:
            if (HIWORD(w) == CBN_SELCHANGE) {
                live_text_load(h, st);
            }
            return TRUE;
        case IDC_LT_USE: {
            int i = (int) SendDlgItemMessageW(h, IDC_LT_SNIP, CB_GETCURSEL, 0, 0);
            auto &sn = st->host->profile().snippets;
            if (i >= 0 && i < (int) sn.size()) {
                SetDlgItemTextW(h, IDC_LT_TEXT, crlf(sn[(size_t) i].text).c_str());
            }
            return TRUE;
        }
        case IDOK: {
            int i = (int) SendDlgItemMessageW(h, IDC_LT_LAYER, CB_GETCURSEL, 0, 0);
            uint32_t id = i >= 0 && i < (int) st->ids.size() ? st->ids[(size_t) i] : 0;
            st->host->set_live_text(id, lf(dlg_text(h, IDC_LT_TEXT)),
                                    IsDlgButtonChecked(h, IDC_LT_SHOW) == BST_CHECKED,
                                    IsDlgButtonChecked(h, IDC_LT_KEEP) == BST_CHECKED);
            EndDialog(h, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(h, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

// ---- Settings ----

struct Prefs {
    TeachHost *host;
    HWND pages[4] = {};
    std::vector<Hotkey> keys;
};

// Every action, in the order the quick button and shortcut lists show them.
std::vector<Action> listed_actions() {
    std::vector<Action> v;
    for (int i = 1; i < (int) Action::kCount; ++i) {
        v.push_back((Action) i);
    }
    return v;
}

INT_PTR CALLBACK pref_panel_proc(HWND h, UINT msg, WPARAM, LPARAM l) {
    if (msg == WM_INITDIALOG) {
        auto *st = (Prefs *) l;
        Settings &s = st->host->settings();
        EnableThemeDialogTexture(h, ETDT_ENABLETAB);
        CheckDlgButton(h, IDC_PP_SHARE, (s.panel_auto & 1) ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(h, IDC_PP_RECORD, (s.panel_auto & 2) ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(h, IDC_PP_STARTUP, s.teach_mode ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(h, IDC_PP_TOP, s.panel_topmost ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(h, IDC_PP_COMPACT, s.panel_compact ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(h, IDC_PP_HIDE, s.hide_controls ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(h, IDC_PP_CURSOR, s.capture_cursor ? BST_CHECKED : BST_UNCHECKED);
        auto fill = [&](int id, std::initializer_list<const wchar_t *> items, int sel) {
            for (auto *t : items) {
                SendDlgItemMessageW(h, id, CB_ADDSTRING, 0, (LPARAM) t);
            }
            SendDlgItemMessageW(h, id, CB_SETCURSEL, (WPARAM) sel, 0);
        };
        fill(IDC_PP_TRANSITION, {L"Cross-fade", L"Cut"}, s.transition);
        fill(IDC_PP_APPLY, {L"Show changes at once", L"Preview first, then Apply"}, s.apply_mode);
        fill(IDC_PP_FPS, {L"Up to 30 frames a second (lighter)", L"Up to 60 frames a second (smoother)"},
             s.capture_fps >= 60 ? 1 : 0);
        return TRUE;
    }
    return FALSE;
}

INT_PTR CALLBACK pref_quick_proc(HWND h, UINT msg, WPARAM, LPARAM l) {
    if (msg == WM_INITDIALOG) {
        auto *st = (Prefs *) l;
        EnableThemeDialogTexture(h, ETDT_ENABLETAB);
        std::vector<Action> acts = listed_actions();
        std::vector<Action> chosen;
        std::stringstream ss(st->host->settings().quick);
        std::string k;
        while (std::getline(ss, k, ',')) {
            if (action_from_key(k) != Action::None) {
                chosen.push_back(action_from_key(k));
            }
        }
        for (int slot = 0; slot < 8; ++slot) {
            HWND c = GetDlgItem(h, IDC_PQ_SLOT0 + slot);
            SendMessageW(c, CB_ADDSTRING, 0, (LPARAM) L"(empty)");
            int sel = 0;
            for (size_t i = 0; i < acts.size(); ++i) {
                SendMessageW(c, CB_ADDSTRING, 0, (LPARAM) action_label(acts[i]).c_str());
                if ((size_t) slot < chosen.size() && chosen[(size_t) slot] == acts[i]) {
                    sel = (int) i + 1;
                }
            }
            SendMessageW(c, CB_SETCURSEL, (WPARAM) sel, 0);
        }
        return TRUE;
    }
    return FALSE;
}

void keys_refresh(HWND h, Prefs *st) {
    HWND list = GetDlgItem(h, IDC_PK_LIST);
    int sel = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    ListView_DeleteAllItems(list);
    for (size_t i = 0; i < st->keys.size(); ++i) {
        std::wstring a = action_label(st->keys[i].action);
        std::wstring k = hotkey_text(st->keys[i].mods, st->keys[i].vk);
        LVITEMW it = {};
        it.mask = LVIF_TEXT;
        it.iItem = (int) i;
        it.pszText = (LPWSTR) a.c_str();
        ListView_InsertItem(list, &it);
        ListView_SetItemText(list, (int) i, 1, (LPWSTR) k.c_str());
    }
    if (sel >= 0) {
        ListView_SetItemState(list, sel, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(list, sel, FALSE);
    }
}

void keys_note(HWND h, Prefs *st, const std::wstring &extra) {
    std::wstring note = extra;
    std::wstring problems = st->host->hotkey_problems();
    if (!problems.empty()) {
        note += (note.empty() ? L"" : L"\n") + problems;
    }
    if (note.empty()) {
        note = L"Pick an action, press the keys in the box, then Set. Use Ctrl, Alt or Win with a key so a "
               L"shortcut never fires while you type or write.";
    }
    SetDlgItemTextW(h, IDC_PK_NOTE, note.c_str());
}

INT_PTR CALLBACK pref_keys_proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    auto *st = (Prefs *) GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (msg) {
    case WM_INITDIALOG: {
        st = (Prefs *) l;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR) st);
        EnableThemeDialogTexture(h, ETDT_ENABLETAB);
        Settings &s = st->host->settings();
        CheckDlgButton(h, IDC_PK_ENABLE, s.hotkeys ? BST_CHECKED : BST_UNCHECKED);
        std::vector<Hotkey> saved = s.hotkey_map.empty() ? default_hotkeys() : parse_hotkeys(s.hotkey_map);
        for (Action a : listed_actions()) {
            Hotkey k{a, 0, 0};
            for (auto &h2 : saved) {
                if (h2.action == a) {
                    k = h2;
                }
            }
            st->keys.push_back(k);
        }
        HWND list = GetDlgItem(h, IDC_PK_LIST);
        ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        RECT r;
        GetClientRect(list, &r);
        LVCOLUMNW col = {};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.cx = r.right * 60 / 100;
        col.pszText = (LPWSTR) L"Action";
        ListView_InsertColumn(list, 0, &col);
        col.cx = r.right * 38 / 100;
        col.pszText = (LPWSTR) L"Shortcut";
        ListView_InsertColumn(list, 1, &col);
        keys_refresh(h, st);
        keys_note(h, st, L"");
        return TRUE;
    }
    case WM_NOTIFY: {
        auto *nm = (NMHDR *) l;
        if (nm->idFrom == IDC_PK_LIST && nm->code == LVN_ITEMCHANGED) {
            int i = ListView_GetNextItem(nm->hwndFrom, -1, LVNI_SELECTED);
            if (i >= 0 && i < (int) st->keys.size()) {
                const Hotkey &k = st->keys[(size_t) i];
                BYTE hk = 0;
                hk |= (k.mods & MOD_SHIFT) ? HOTKEYF_SHIFT : 0;
                hk |= (k.mods & MOD_CONTROL) ? HOTKEYF_CONTROL : 0;
                hk |= (k.mods & MOD_ALT) ? HOTKEYF_ALT : 0;
                bool ext = k.vk == VK_LEFT || k.vk == VK_RIGHT || k.vk == VK_UP || k.vk == VK_DOWN;
                hk |= ext ? HOTKEYF_EXT : 0;
                SendDlgItemMessageW(h, IDC_PK_HOTKEY, HKM_SETHOTKEY, MAKEWORD(k.vk, hk), 0);
            }
        }
        return TRUE;
    }
    case WM_COMMAND: {
        int i = ListView_GetNextItem(GetDlgItem(h, IDC_PK_LIST), -1, LVNI_SELECTED);
        switch (LOWORD(w)) {
        case IDC_PK_SET: {
            if (i < 0) {
                keys_note(h, st, L"Pick an action in the list first.");
                return TRUE;
            }
            WORD v = (WORD) SendDlgItemMessageW(h, IDC_PK_HOTKEY, HKM_GETHOTKEY, 0, 0);
            UINT vk = LOBYTE(v), hk = HIBYTE(v), mods = 0;
            mods |= (hk & HOTKEYF_SHIFT) ? MOD_SHIFT : 0;
            mods |= (hk & HOTKEYF_CONTROL) ? MOD_CONTROL : 0;
            mods |= (hk & HOTKEYF_ALT) ? MOD_ALT : 0;
            bool fkey = vk >= VK_F1 && vk <= VK_F24;
            if (!vk) {
                keys_note(h, st, L"Press the keys in the box first.");
                return TRUE;
            }
            if (!(mods & (MOD_CONTROL | MOD_ALT)) && !fkey) {
                keys_note(h, st, L"Add Ctrl or Alt to that key, so the shortcut never fires while you type.");
                return TRUE;
            }
            std::wstring extra;
            for (size_t j = 0; j < st->keys.size(); ++j) {
                if ((int) j != i && st->keys[j].vk == vk && st->keys[j].mods == mods) {
                    extra = hotkey_text(mods, vk) + L" was used for \"" + action_label(st->keys[j].action) +
                            L"\"; it now does this instead.";
                    st->keys[j].vk = 0;
                    st->keys[j].mods = 0;
                }
            }
            st->keys[(size_t) i].vk = vk;
            st->keys[(size_t) i].mods = mods;
            keys_refresh(h, st);
            keys_note(h, st, extra);
            return TRUE;
        }
        case IDC_PK_CLEAR:
            if (i >= 0) {
                st->keys[(size_t) i].vk = 0;
                st->keys[(size_t) i].mods = 0;
                keys_refresh(h, st);
            }
            return TRUE;
        case IDC_PK_RESET: {
            std::vector<Hotkey> d = default_hotkeys();
            for (auto &k : st->keys) {
                k.vk = k.mods = 0;
                for (auto &dk : d) {
                    if (dk.action == k.action) {
                        k = dk;
                    }
                }
            }
            keys_refresh(h, st);
            keys_note(h, st, L"Back to the defaults (Ctrl+Alt with a letter or digit).");
            return TRUE;
        }
        }
        break;
    }
    }
    return FALSE;
}

void profiles_fill(HWND h, Prefs *st) {
    HWND list = GetDlgItem(h, IDC_PF_LIST);
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    std::wstring cur = st->host->profile().name;
    int sel = 0;
    std::vector<std::wstring> names = list_profiles();
    for (size_t i = 0; i < names.size(); ++i) {
        std::wstring item = names[i] + (names[i] == cur ? L"   (in use)" : L"");
        SendMessageW(list, LB_ADDSTRING, 0, (LPARAM) item.c_str());
        SendMessageW(list, LB_SETITEMDATA, i, 0);
        if (names[i] == cur) {
            sel = (int) i;
        }
    }
    SendMessageW(list, LB_SETCURSEL, (WPARAM) sel, 0);
}

INT_PTR CALLBACK pref_profiles_proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    auto *st = (Prefs *) GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (msg) {
    case WM_INITDIALOG:
        st = (Prefs *) l;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR) st);
        EnableThemeDialogTexture(h, ETDT_ENABLETAB);
        profiles_fill(h, st);
        return TRUE;
    case WM_COMMAND: {
        std::vector<std::wstring> names = list_profiles();
        int i = (int) SendDlgItemMessageW(h, IDC_PF_LIST, LB_GETCURSEL, 0, 0);
        std::wstring picked = i >= 0 && i < (int) names.size() ? names[(size_t) i] : L"";
        Profile &cur = st->host->profile();
        auto fail = [&](const std::wstring &msg2) { MessageBoxW(h, msg2.c_str(), L"AirScreen", MB_OK | MB_ICONWARNING); };
        switch (LOWORD(w)) {
        case IDC_PF_LIST:
            if (HIWORD(w) != LBN_DBLCLK) {
                return TRUE;
            }
            [[fallthrough]];
        case IDC_PF_USE:
            if (!picked.empty() && picked != cur.name) {
                st->host->switch_profile(picked);
                profiles_fill(h, st);
            }
            return TRUE;
        case IDC_PF_NEW:
        case IDC_PF_DUP: {
            bool dup = LOWORD(w) == IDC_PF_DUP;
            std::wstring name = dup ? picked + L" copy" : L"Mathematics";
            if (!prompt_text(h, dup ? L"Duplicate profile" : L"New profile", L"Profile name:", name) || name.empty()) {
                return TRUE;
            }
            if (profile_exists(name)) {
                fail(L"There is already a profile called \"" + name + L"\".");
                return TRUE;
            }
            Profile p;
            std::string err;
            if (!dup) {
                p = default_profile(name, st->host->settings());
            } else if (picked == cur.name) {
                p = cur;
            } else if (!load_profile(picked, p, &err)) {
                fail(L"That profile couldn't be read: " + utf16(err));
                return TRUE;
            }
            p.name = name;
            if (!save_profile(p, &err)) {
                fail(L"The profile couldn't be saved: " + utf16(err));
                return TRUE;
            }
            profiles_fill(h, st);
            return TRUE;
        }
        case IDC_PF_RENAME: {
            if (picked.empty()) {
                return TRUE;
            }
            std::wstring name = picked;
            if (!prompt_text(h, L"Rename profile", L"Profile name:", name) || name.empty() || name == picked) {
                return TRUE;
            }
            if (profile_exists(name)) {
                fail(L"There is already a profile called \"" + name + L"\".");
                return TRUE;
            }
            std::string err;
            Profile p;
            bool in_use = picked == cur.name;
            if (in_use) {
                p = cur;
            } else if (!load_profile(picked, p, &err)) {
                fail(L"That profile couldn't be read: " + utf16(err));
                return TRUE;
            }
            p.name = name;
            if (!save_profile(p, &err)) {
                fail(L"The profile couldn't be saved: " + utf16(err));
                return TRUE;
            }
            delete_profile(picked);
            if (in_use) {
                cur.name = name;
                st->host->settings().profile = utf8(name);
                st->host->save_settings();
                st->host->scenes_changed();
            }
            profiles_fill(h, st);
            return TRUE;
        }
        case IDC_PF_DELETE:
            if (picked.empty()) {
                return TRUE;
            }
            if (picked == cur.name) {
                fail(L"This profile is in use. Switch to another one first.");
                return TRUE;
            }
            if (MessageBoxW(h, (L"Delete the profile \"" + picked + L"\" and all its scenes?").c_str(), L"AirScreen",
                            MB_OKCANCEL | MB_ICONQUESTION) == IDOK) {
                delete_profile(picked);
                profiles_fill(h, st);
            }
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

void prefs_show_page(HWND h, Prefs *st) {
    int sel = TabCtrl_GetCurSel(GetDlgItem(h, IDC_PREF_TABS));
    for (int i = 0; i < 4; ++i) {
        ShowWindow(st->pages[i], i == sel ? SW_SHOW : SW_HIDE);
    }
}

INT_PTR CALLBACK prefs_proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    auto *st = (Prefs *) GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (msg) {
    case WM_INITDIALOG: {
        st = (Prefs *) l;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR) st);
        HWND tabs = GetDlgItem(h, IDC_PREF_TABS);
        const wchar_t *names[4] = {L"Control panel", L"Quick buttons", L"Shortcuts", L"Profiles"};
        for (int i = 0; i < 4; ++i) {
            TCITEMW ti = {};
            ti.mask = TCIF_TEXT;
            ti.pszText = (LPWSTR) names[i];
            TabCtrl_InsertItem(tabs, i, &ti);
        }
        const int ids[4] = {IDD_PREF_PANEL, IDD_PREF_QUICK, IDD_PREF_KEYS, IDD_PREF_PROFILES};
        DLGPROC procs[4] = {pref_panel_proc, pref_quick_proc, pref_keys_proc, pref_profiles_proc};
        RECT r;
        GetWindowRect(tabs, &r);
        MapWindowPoints(nullptr, h, (POINT *) &r, 2);
        TabCtrl_AdjustRect(tabs, FALSE, &r);
        for (int i = 0; i < 4; ++i) {
            st->pages[i] = CreateDialogParamW(module(), MAKEINTRESOURCEW(ids[i]), h, procs[i], (LPARAM) st);
            SetWindowPos(st->pages[i], HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE);
        }
        prefs_show_page(h, st);
        return TRUE;
    }
    case WM_NOTIFY:
        if (((NMHDR *) l)->idFrom == IDC_PREF_TABS && ((NMHDR *) l)->code == TCN_SELCHANGE) {
            prefs_show_page(h, st);
        }
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(w) == IDOK) {
            Settings &s = st->host->settings();
            HWND p = st->pages[0];
            s.panel_auto = (IsDlgButtonChecked(p, IDC_PP_SHARE) == BST_CHECKED ? 1 : 0) |
                           (IsDlgButtonChecked(p, IDC_PP_RECORD) == BST_CHECKED ? 2 : 0);
            s.teach_mode = IsDlgButtonChecked(p, IDC_PP_STARTUP) == BST_CHECKED;
            s.panel_topmost = IsDlgButtonChecked(p, IDC_PP_TOP) == BST_CHECKED;
            s.panel_compact = IsDlgButtonChecked(p, IDC_PP_COMPACT) == BST_CHECKED;
            s.hide_controls = IsDlgButtonChecked(p, IDC_PP_HIDE) == BST_CHECKED;
            s.capture_cursor = IsDlgButtonChecked(p, IDC_PP_CURSOR) == BST_CHECKED;
            s.transition = (int) SendDlgItemMessageW(p, IDC_PP_TRANSITION, CB_GETCURSEL, 0, 0) == 1 ? 1 : 0;
            s.apply_mode = (int) SendDlgItemMessageW(p, IDC_PP_APPLY, CB_GETCURSEL, 0, 0) == 1 ? 1 : 0;
            s.capture_fps = SendDlgItemMessageW(p, IDC_PP_FPS, CB_GETCURSEL, 0, 0) == 1 ? 60 : 30;

            std::vector<Action> acts = listed_actions();
            std::string quick;
            for (int slot = 0; slot < 8; ++slot) {
                int i = (int) SendDlgItemMessageW(st->pages[1], IDC_PQ_SLOT0 + slot, CB_GETCURSEL, 0, 0);
                if (i > 0 && i <= (int) acts.size()) {
                    quick += (quick.empty() ? "" : ",") + std::string(action_key(acts[(size_t) i - 1]));
                }
            }
            s.quick = quick;

            s.hotkeys = IsDlgButtonChecked(st->pages[2], IDC_PK_ENABLE) == BST_CHECKED;
            s.hotkey_map = format_hotkeys(st->keys);
            st->host->save_settings();
            st->host->prefs_changed();
            EndDialog(h, IDOK);
            return TRUE;
        }
        if (LOWORD(w) == IDCANCEL) {
            EndDialog(h, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

} // namespace

bool prompt_text(HWND owner, const wchar_t *title, const wchar_t *label, std::wstring &value) {
    Prompt st{title, label, &value};
    return DialogBoxParamW(module(), MAKEINTRESOURCEW(IDD_PROMPT), owner, prompt_proc, (LPARAM) &st) == IDOK;
}

bool pick_capture_target(HWND owner, TeachHost *host, CaptureTarget &out) {
    Picker st{host, &out};
    return DialogBoxParamW(module(), MAKEINTRESOURCEW(IDD_PICKER), owner, picker_proc, (LPARAM) &st) == IDOK;
}

void edit_live_text(HWND owner, TeachHost *host) {
    LiveText st{host, {}};
    DialogBoxParamW(module(), MAKEINTRESOURCEW(IDD_LIVETEXT), owner, live_text_proc, (LPARAM) &st);
}

void teach_settings(HWND owner, TeachHost *host) {
    Prefs st{host};
    DialogBoxParamW(module(), MAKEINTRESOURCEW(IDD_PREFS), owner, prefs_proc, (LPARAM) &st);
}

} // namespace airscreen
