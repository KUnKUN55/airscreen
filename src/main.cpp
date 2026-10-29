#include <winsock2.h> // before anything pulls in windows.h, for the adapter lookup below
#include "receiver.h"
#include "settings.h"
#include "ui.h"
#include "control.h"
#include "editor.h"
#include "teach.h"
#include "app/resource.h"
#include "raop.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <iphlpapi.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <objbase.h>
#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <dbghelp.h>
#include <timeapi.h>
#include <string>
#include <vector>
#include <cmath>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "comdlg32.lib")

namespace {

constexpr UINT WM_AIR_EVENT = WM_APP + 1;
constexpr UINT WM_TRAY = WM_APP + 2;
constexpr UINT kTrayId = 1;
constexpr UINT kTimerOverlay = 1;
constexpr UINT kTimerDisconnect = 2;
constexpr UINT kTimerDrift = 3;
constexpr UINT kTimerRec = 4;
constexpr UINT kTimerPin = 5;
constexpr UINT kTimerSave = 6;
constexpr UINT kPinMs = 90000; // a PIN nobody typed in this long is stale
constexpr UINT kOverlayHideMs = 2200;
constexpr UINT kDisconnectMs = 400;
constexpr UINT kSaveMs = 800; // edits are saved this long after the last one
constexpr int kMaxBarScenes = 8;
UINT g_taskbar_created = 0;

struct NameDlgState {
    std::wstring initial;
    std::wstring result;
    bool ok = false;
};

struct App : airscreen::TeachHost {
    HWND hwnd = nullptr;
    HWND video_hwnd = nullptr;
    HWND settings_hwnd = nullptr;
    HINSTANCE inst = nullptr;
    NOTIFYICONDATAW nid = {};
    airscreen::Settings settings_;
    airscreen::Receiver receiver;
    airscreen::ShellUi shell;
    bool fullscreen = false;
    bool running = true;
    bool settings_hot = false;
    bool mirroring = false; // the iPad's picture is showing
    bool start_failed = false;
    int hover = 0;
    LONG windowed_style = 0;
    LONG windowed_ex = 0;
    RECT windowed_rc = {};
    std::wstring overlay_status = L"Ready to connect";
    std::wstring overlay_pin;
    std::wstring overlay_detail;
    bool connected = false;
    bool recording = false;
    int scene_hot = -1; // scene bar segment under the mouse
    HWND scenes_hwnd = nullptr;
    std::vector<airscreen::MicCapture::Device> mics; // as listed when the menu last opened
    HICON icon = nullptr;
    HICON icon_sm = nullptr;

    // Teaching studio.
    airscreen::Profile profile_;
    int live_ = 0;               // index of the scene on air
    airscreen::Scene live_scene_; // its working copy: quick actions change this, not the saved scene
    airscreen::ControlPanel panel;
    airscreen::SceneEditor editor;
    std::wstring computer_name;
    std::wstring message; // last problem worth showing on the control panel
    std::vector<airscreen::Hotkey> hotkeys;
    std::wstring hotkey_problems_;
    int registered_hotkeys = 0;
    bool save_pending = false;

    void post_ui(airscreen::UiEvent ev, const std::string &msg) {
        auto *s = new std::string(msg);
        PostMessageW(hwnd, WM_AIR_EVENT, (WPARAM) ev, (LPARAM) s);
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

    // Something is on air: the iPad's picture, the computer screen's, or both.
    bool live() { return mirroring || receiver.video().computer_live(); }
    HWND dialog_owner() {
        if (editor.is_open() && GetForegroundWindow() == editor.hwnd()) {
            return editor.hwnd();
        }
        return panel.visible() ? panel.hwnd() : hwnd;
    }

    // TeachHost
    airscreen::Profile &profile() override { return profile_; }
    airscreen::Settings &settings() override { return settings_; }
    airscreen::VideoPipeline &video() override { return receiver.video(); }
    void save_settings() override { airscreen::save_settings(settings_); }
    int live_index() override { return live_; }
    const airscreen::Scene &live_scene() override { return live_scene_; }
    void go_live(int index) override;
    void scene_changed(int index, bool to_air) override;
    void scenes_changed() override;
    void run(airscreen::Action a) override;
    void set_live_text(uint32_t layer_id, const std::wstring &text, bool show, bool keep) override;
    airscreen::TeachStatus status() override;
    void choose_computer() override;
    void stop_computer() override;
    void open_editor(bool preview_only) override { editor.open(inst, this, preview_only); }
    void open_settings() override { airscreen::teach_settings(dialog_owner(), this); }
    void switch_profile(const std::wstring &name) override;
    void layout(airscreen::DualLayout l) override;
    void keep_live_changes() override;
    void prefs_changed() override;
    std::wstring hotkey_problems() override { return hotkey_problems_; }
    HWND main_window() override { return hwnd; }
};

App *g_app = nullptr;

UINT window_dpi(HWND hwnd) {
    UINT dpi = GetDpiForWindow(hwnd);
    return dpi ? dpi : 96;
}

void set_dark_title(HWND hwnd) {
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
}

void apply_topmost(HWND hwnd, bool on) {
    SetWindowPos(hwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void update_title(App *app) {
    std::wstring t;
    if (app->recording) {
        auto &rec = app->receiver.recorder();
        int s = (int) rec.seconds();
        wchar_t r[64];
        swprintf_s(r, L"%s %02d:%02d:%02d%s - ", rec.paused() ? L"❚❚ PAUSED" : L"● REC", s / 3600, s / 60 % 60, s % 60,
                   rec.mic_running() ? L" + MIC" : L"");
        t = r;
    }
    t += L"AirScreen - " + app->utf16(app->settings_.name);
    if (app->live()) {
        t += L" - Live";
    } else if (app->connected) {
        t += L" - Connected";
    } else {
        t += L" - Waiting";
    }
    SetWindowTextW(app->hwnd, t.c_str());
}

std::wstring recordings_dir() {
    std::wstring dir;
    PWSTR videos = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, KF_FLAG_CREATE, nullptr, &videos))) {
        dir = videos;
    }
    CoTaskMemFree(videos);
    if (dir.empty()) {
        dir = airscreen::app_data_dir();
    }
    dir += L"\\AirScreen";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

void tray_balloon(App *app, const std::wstring &title, const std::wstring &text) {
    NOTIFYICONDATAW n = app->nid;
    n.uFlags = NIF_INFO;
    n.dwInfoFlags = NIIF_INFO;
    wcsncpy_s(n.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(n.szInfo, text.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

// Called when a clip ends for any reason: the user stopped it, mirroring ended, or the writer failed.
void finish_recording(App *app, const std::string &error) {
    if (!app->recording) {
        return;
    }
    app->recording = false;
    KillTimer(app->hwnd, kTimerRec);
    update_title(app);
    app->panel.refresh();
    std::wstring path = app->receiver.recorder().path();
    if (!error.empty()) {
        MessageBoxW(app->dialog_owner(), (L"Recording stopped: " + app->utf16(error)).c_str(), L"AirScreen",
                    MB_OK | MB_ICONWARNING);
    } else {
        size_t slash = path.find_last_of(L'\\');
        tray_balloon(app, L"Clip saved", slash == std::wstring::npos ? path : path.substr(slash + 1));
    }
}

void toggle_recording(App *app) {
    auto &rec = app->receiver.recorder();
    if (app->recording) {
        rec.stop();
        finish_recording(app, rec.last_error());
        return;
    }
    if (!app->live()) {
        MessageBoxW(app->dialog_owner(),
                    L"Start mirroring from the iPad, or share the computer screen, then record.", L"AirScreen",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t name[64];
    swprintf_s(name, L"\\AirScreen_%04d-%02d-%02d_%02d-%02d-%02d.mp4", st.wYear, st.wMonth, st.wDay, st.wHour,
               st.wMinute, st.wSecond);
    rec.start(recordings_dir() + name);
    app->receiver.video().request_render(); // first frame now, even if the screen is still
    app->recording = true;
    SetTimer(app->hwnd, kTimerRec, 500, nullptr);
    update_title(app);
    if (!rec.mic_error().empty()) {
        tray_balloon(app, L"Recording without the microphone", app->utf16(rec.mic_error()));
    }
    if (app->settings_.panel_auto & 2) {
        app->panel.show(false);
    }
    app->panel.refresh();
}

void toggle_pause(App *app) {
    if (!app->recording) {
        return;
    }
    auto &rec = app->receiver.recorder();
    rec.set_paused(!rec.paused());
    update_title(app);
    app->panel.refresh();
}

constexpr int kMicGains[] = {50, 100, 150, 200, 300};

// Pushes the mic settings to the recorder; mid-clip that switches the mic on, off or to another device.
void apply_mic(App *app) {
    auto &rec = app->receiver.recorder();
    bool ok = rec.set_mic(app->settings_.mic_enabled, app->utf16(app->settings_.mic_device),
                          app->settings_.mic_gain_pct / 100.0f);
    if (app->recording) {
        if (!ok) {
            tray_balloon(app, L"Microphone unavailable", app->utf16(rec.mic_error()));
        }
        update_title(app);
    }
}

void toggle_mic(App *app) {
    app->settings_.mic_enabled = !app->settings_.mic_enabled;
    airscreen::save_settings(app->settings_);
    apply_mic(app);
    if (app->recording && app->receiver.recorder().mic_error().empty()) {
        tray_balloon(app, app->settings_.mic_enabled ? L"Microphone on" : L"Microphone off",
                     L"Ctrl+M switches it during a recording");
    }
}

HMENU build_mic_menu(App *app) {
    app->mics = airscreen::MicCapture::list_devices();
    HMENU m = CreatePopupMenu();
    const auto &s = app->settings_;
    AppendMenuW(m, MF_STRING | (!s.mic_enabled ? MF_CHECKED : 0), IDM_MIC_OFF, L"Off");
    AppendMenuW(m, MF_STRING | (s.mic_enabled && s.mic_device.empty() ? MF_CHECKED : 0), IDM_MIC_DEFAULT,
                L"Windows default microphone");
    for (size_t i = 0; i < app->mics.size() && i < 99; ++i) {
        bool on = s.mic_enabled && app->utf16(s.mic_device) == app->mics[i].id;
        AppendMenuW(m, MF_STRING | (on ? MF_CHECKED : 0), IDM_MIC_DEV_BASE + (UINT) i, app->mics[i].name.c_str());
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    for (size_t i = 0; i < sizeof(kMicGains) / sizeof(kMicGains[0]); ++i) {
        wchar_t label[32];
        swprintf_s(label, L"Mic volume %d%%", kMicGains[i]);
        AppendMenuW(m, MF_STRING | (s.mic_gain_pct == kMicGains[i] ? MF_CHECKED : 0), IDM_MIC_GAIN_BASE + (UINT) i,
                    label);
    }
    return m;
}

void apply_video_fill(App *app) {
    app->receiver.video().set_cover(app->fullscreen && app->settings_.fill_screen);
}

int bar_scenes(App *app) {
    return (int) (std::min)(app->profile_.scenes.size(), (size_t) kMaxBarScenes);
}

void layout_chrome(App *app) {
    if (!app->hwnd) {
        return;
    }
    RECT rc = {};
    GetClientRect(app->hwnd, &rc);
    int cw = rc.right - rc.left;
    int ch = rc.bottom - rc.top;
    if (app->video_hwnd) {
        SetWindowPos(app->video_hwnd, HWND_BOTTOM, 0, 0, cw, ch,
                     SWP_NOACTIVATE);
    }
    UINT dpi = window_dpi(app->hwnd);
    int size = MulDiv(40, dpi, 96);
    int margin = MulDiv(12, dpi, 96);
    int x = cw - margin - size;
    int y = margin;
    if (x < margin) {
        x = margin;
    }
    if (app->settings_hwnd) {
        SetWindowPos(app->settings_hwnd, HWND_TOP, x, y, size, size, SWP_NOACTIVATE);
        HRGN rgn = CreateRoundRectRgn(0, 0, size + 1, size + 1, size, size);
        SetWindowRgn(app->settings_hwnd, rgn, TRUE);
    }
    if (app->scenes_hwnd) {
        // Scene bar: bottom centre, a pill with a segment per scene (the first eight).
        int n = (std::max)(1, bar_scenes(app));
        int bw = (std::min)(MulDiv(n * 128 + 8, dpi, 96), cw - margin * 2);
        int bh = MulDiv(44, dpi, 96);
        int bx = (cw - bw) / 2;
        int by = ch - margin * 2 - bh;
        SetWindowPos(app->scenes_hwnd, HWND_TOP, bx, by, bw, bh, SWP_NOACTIVATE);
        HRGN rgn = CreateRoundRectRgn(0, 0, bw + 1, bh + 1, bh, bh);
        SetWindowRgn(app->scenes_hwnd, rgn, TRUE);
    }
}

void show_settings_button(App *app, bool show) {
    if (!app->settings_hwnd) {
        return;
    }
    ShowWindow(app->settings_hwnd, show ? SW_SHOWNA : SW_HIDE);
    if (show) {
        SetWindowPos(app->settings_hwnd, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    // The scene bar comes and goes with the gear; it sits over the window only, never in a recording.
    if (app->scenes_hwnd) {
        ShowWindow(app->scenes_hwnd, show ? SW_SHOWNA : SW_HIDE);
        if (show) {
            SetWindowPos(app->scenes_hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }
}

void note_activity(App *app) {
    if (!app->live()) {
        show_settings_button(app, false);
        KillTimer(app->hwnd, kTimerOverlay);
        return;
    }
    show_settings_button(app, true);
    SetTimer(app->hwnd, kTimerOverlay, kOverlayHideMs, nullptr);
}

void set_fullscreen(App *app, bool on) {
    if (!app->hwnd || app->fullscreen == on) {
        if (on) {
            apply_video_fill(app);
        }
        return;
    }
    if (on) {
        app->windowed_style = GetWindowLongW(app->hwnd, GWL_STYLE);
        app->windowed_ex = GetWindowLongW(app->hwnd, GWL_EXSTYLE);
        GetWindowRect(app->hwnd, &app->windowed_rc);
        MONITORINFO mi = {sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(app->hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongW(app->hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN);
        SetWindowLongW(app->hwnd, GWL_EXSTYLE, WS_EX_APPWINDOW | WS_EX_TOPMOST);
        SetWindowPos(app->hwnd, HWND_TOPMOST, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        app->fullscreen = true;
    } else {
        SetWindowLongW(app->hwnd, GWL_STYLE, app->windowed_style | WS_CLIPCHILDREN);
        SetWindowLongW(app->hwnd, GWL_EXSTYLE, app->windowed_ex);
        SetWindowPos(app->hwnd, HWND_NOTOPMOST, app->windowed_rc.left, app->windowed_rc.top,
                     app->windowed_rc.right - app->windowed_rc.left,
                     app->windowed_rc.bottom - app->windowed_rc.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        apply_topmost(app->hwnd, app->settings_.always_on_top);
        app->fullscreen = false;
    }
    apply_video_fill(app);
    layout_chrome(app);
    app->receiver.video().resize();
    note_activity(app);
    if (app->fullscreen && app->panel.visible() && app->settings_.panel_topmost) {
        app->panel.apply_prefs(); // keep the control panel above the full-screen output
    }
}

void toggle_fullscreen(App *app) {
    set_fullscreen(app, !app->fullscreen);
}

void set_boost(bool on) {
    static bool boosted = false;
    if (on == boosted) {
        return;
    }
    boosted = on;
    SetPriorityClass(GetCurrentProcess(), on ? HIGH_PRIORITY_CLASS : NORMAL_PRIORITY_CLASS);
    on ? timeBeginPeriod(1) : timeEndPeriod(1);
}

void show_video_surface(App *app) {
    set_boost(true);
    if (!IsWindowVisible(app->hwnd)) {
        ShowWindow(app->hwnd, SW_SHOWNORMAL); // someone started sharing while we sat in the tray
    }
    if (app->video_hwnd) {
        ShowWindow(app->video_hwnd, SW_SHOW);
    }
    layout_chrome(app);
    note_activity(app);
    update_title(app);
    app->panel.refresh();
}

// Nothing is on air any more: back to the home screen. Sharing stopped doesn't close anything else.
void return_to_menu(App *app) {
    if (app->live()) {
        update_title(app);
        app->panel.refresh();
        return; // the other screen is still on air
    }
    set_boost(false);
    if (app->video_hwnd) {
        ShowWindow(app->video_hwnd, SW_HIDE);
    }
    if (app->fullscreen) {
        set_fullscreen(app, false);
    }
    KillTimer(app->hwnd, kTimerOverlay);
    show_settings_button(app, false);
    layout_chrome(app);
    update_title(app);
    app->panel.refresh();
    InvalidateRect(app->hwnd, nullptr, FALSE);
    UpdateWindow(app->hwnd);
}

// The iPad stopped mirroring.
void ipad_ended(App *app) {
    app->connected = false;
    app->mirroring = false;
    app->overlay_status = L"Ready to connect";
    // The PIN stays: iOS drops the connection the moment it shows its PIN prompt and reconnects once the PIN
    // is typed, so this disconnect is exactly when the PIN needs to be on screen. It goes when the device
    // reconnects, or after kPinMs.
    return_to_menu(app);
}

INT_PTR CALLBACK rename_dlg(HWND h, UINT msg, WPARAM w, LPARAM l) {
    auto *st = (NameDlgState *) GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (msg) {
    case WM_INITDIALOG: {
        st = (NameDlgState *) l;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR) st);
        SetDlgItemTextW(h, IDC_NAME, st->initial.c_str());
        SendDlgItemMessageW(h, IDC_NAME, EM_SETSEL, 0, -1);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(w) == IDOK) {
            wchar_t buf[128] = {};
            GetDlgItemTextW(h, IDC_NAME, buf, 128);
            st->result = buf;
            st->ok = !st->result.empty();
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

void do_rename(App *app) {
    NameDlgState st;
    st.initial = app->utf16(app->settings_.name);
    if (DialogBoxParamW(app->inst, MAKEINTRESOURCEW(IDD_RENAME), app->hwnd, rename_dlg, (LPARAM) &st) == IDOK &&
        st.ok) {
        std::string name = app->utf8(st.result);
        if (app->receiver.rename(name)) {
            app->settings_.name = name;
            airscreen::save_settings(app->settings_);
            update_title(app);
        }
    }
}

// ---- Teaching studio ----

double scene_fade(App *app) {
    return app->settings_.transition == 0 ? 0.5 : 0.0;
}

// Quick changes (text on/off, screens) get a short soft fade, never a jump cut, unless cuts are chosen.
double toggle_fade(App *app) {
    return app->settings_.transition == 0 ? 0.2 : 0.0;
}

void push_live(App *app, double fade) {
    app->receiver.video().set_scene(app->live_scene_, fade);
    app->panel.refresh();
    if (app->scenes_hwnd) {
        InvalidateRect(app->scenes_hwnd, nullptr, FALSE);
    }
}

void schedule_save(App *app) {
    app->save_pending = true;
    SetTimer(app->hwnd, kTimerSave, kSaveMs, nullptr);
}

void save_now(App *app) {
    KillTimer(app->hwnd, kTimerSave);
    if (!app->save_pending) {
        return;
    }
    app->save_pending = false;
    std::string err;
    if (!airscreen::save_profile(app->profile_, &err)) {
        // Never in the way of teaching: the scenes stay in memory, and the next edit tries again.
        airscreen::append_log(("profile save failed: " + err).c_str());
        app->message = L"Scenes couldn't be saved: " + app->utf16(err);
    } else if (app->message.rfind(L"Scenes couldn't", 0) == 0) {
        app->message.clear();
    }
    app->panel.refresh();
}

void App::go_live(int index) {
    if (profile_.scenes.empty()) {
        return;
    }
    if (index < 0 || index >= (int) profile_.scenes.size()) {
        return;
    }
    live_ = index;
    live_scene_ = profile_.scenes[(size_t) index];
    push_live(this, scene_fade(this));
    editor.live_changed();
}

void App::scene_changed(int index, bool to_air) {
    schedule_save(this);
    if (to_air && index == live_ && index < (int) profile_.scenes.size()) {
        live_scene_ = profile_.scenes[(size_t) index];
        push_live(this, 0); // an edit shows as it is made, no fade
    }
    if (scenes_hwnd) {
        InvalidateRect(scenes_hwnd, nullptr, FALSE);
    }
}

void App::scenes_changed() {
    schedule_save(this);
    int i = profile_.index_of(live_scene_.id);
    if (i < 0) {
        go_live((std::min)(live_, (int) profile_.scenes.size() - 1)); // the scene on air was deleted
    } else {
        live_ = i;
        live_scene_.name = profile_.scenes[(size_t) i].name;
    }
    layout_chrome(this);
    panel.refresh();
    editor.scenes_changed();
    if (scenes_hwnd) {
        InvalidateRect(scenes_hwnd, nullptr, FALSE);
    }
}

void App::run(airscreen::Action a) {
    using airscreen::Action;
    auto &v = receiver.video();
    switch (a) {
    case Action::ShowIPad:
        airscreen::show_only(live_scene_, airscreen::SourceKind::IPad);
        break;
    case Action::ShowComputer:
    case Action::ShowBoth:
        if (!v.computer_running()) {
            choose_computer(); // nothing to show yet: pick a display or window first
            if (!v.computer_running()) {
                return;
            }
        }
        if (a == Action::ShowComputer) {
            airscreen::show_only(live_scene_, airscreen::SourceKind::Computer);
        } else {
            airscreen::show_both(live_scene_, profile_);
        }
        break;
    case Action::SwapScreens:
        airscreen::swap_screens(live_scene_);
        break;
    case Action::ToggleSecondary:
        airscreen::toggle_secondary(live_scene_);
        break;
    case Action::ToggleText:
        airscreen::toggle_text(live_scene_);
        break;
    case Action::ShowText:
        airscreen::set_text_visible(live_scene_, true);
        break;
    case Action::HideText:
        airscreen::set_text_visible(live_scene_, false);
        break;
    case Action::ToggleBox:
        airscreen::toggle_box(live_scene_);
        break;
    case Action::ToggleExtras:
        airscreen::toggle_extras(live_scene_);
        break;
    case Action::NextScene:
    case Action::PrevScene: {
        int n = (int) profile_.scenes.size();
        go_live((live_ + (a == Action::NextScene ? 1 : n - 1)) % n);
        return;
    }
    case Action::ToggleRecord:
        toggle_recording(this);
        return;
    case Action::PauseRecord:
        toggle_pause(this);
        return;
    case Action::TogglePanel:
        panel.toggle();
        return;
    case Action::Preview:
        open_editor(true);
        return;
    case Action::EditText:
        airscreen::edit_live_text(dialog_owner(), this);
        return;
    default:
        if (a >= Action::Scene1 && a <= Action::Scene9) {
            go_live((int) a - (int) Action::Scene1);
        }
        return;
    }
    push_live(this, toggle_fade(this));
}

void App::set_live_text(uint32_t layer_id, const std::wstring &text, bool show, bool keep) {
    airscreen::Layer *l = layer_id ? live_scene_.find(layer_id) : nullptr;
    if (!l) {
        airscreen::Layer t = airscreen::make_layer(airscreen::LayerType::Text, profile_);
        t.name = L"Title";
        t.x = 0.04f, t.y = 0.035f, t.w = 0.92f, t.h = 0.105f;
        live_scene_.layers.push_back(t);
        l = &live_scene_.layers.back();
    }
    l->text = text;
    l->visible = show;
    push_live(this, toggle_fade(this));
    if (keep && live_ < (int) profile_.scenes.size()) {
        airscreen::Scene &saved = profile_.scenes[(size_t) live_];
        if (airscreen::Layer *sl = saved.find(l->id)) {
            sl->text = text;
            sl->visible = show;
        } else {
            saved.layers.push_back(*l);
        }
        schedule_save(this);
        editor.scenes_changed();
    }
}

airscreen::TeachStatus App::status() {
    airscreen::TeachStatus st;
    auto &v = receiver.video();
    st.ipad = mirroring && v.has_frame();
    st.ipad_name = utf16(receiver.client_name());
    st.computer = v.computer_live();
    st.computer_wait = v.computer_running() && !st.computer;
    st.computer_name = computer_name;
    st.recording = recording;
    st.paused = recording && receiver.recorder().paused();
    st.rec_seconds = recording ? receiver.recorder().seconds() : 0;
    st.mic = receiver.recorder().mic_running();
    st.message = message;
    return st;
}

void App::choose_computer() {
    if (!airscreen::ScreenCapture::supported()) {
        MessageBoxW(dialog_owner(), L"Sharing the computer screen needs Windows 10 version 1903 or later.",
                    L"AirScreen", MB_OK | MB_ICONINFORMATION);
        return;
    }
    airscreen::CaptureTarget t;
    if (!airscreen::pick_capture_target(dialog_owner(), this, t)) {
        return;
    }
    std::string err;
    computer_name = t.name;
    if (!receiver.video().start_computer(t, settings_.capture_cursor, settings_.capture_fps, &err)) {
        computer_name.clear();
        message = L"The computer screen couldn't be shared: " + utf16(err);
        airscreen::append_log(("computer capture failed: " + err).c_str());
        MessageBoxW(dialog_owner(), message.c_str(), L"AirScreen", MB_OK | MB_ICONWARNING);
    } else {
        message.clear();
        airscreen::append_log(("computer capture started: " + utf8(t.name)).c_str());
    }
    panel.refresh(); // "starting" until the first picture arrives (ComputerReady)
}

// Stops the computer screen; with nothing else on air a recording ends here too, as when the iPad leaves.
void computer_ended(App *app, const std::wstring &why) {
    app->receiver.video().stop_computer();
    app->computer_name.clear();
    app->message = why;
    if (app->recording && !app->mirroring) {
        app->receiver.recorder().stop();
        finish_recording(app, app->receiver.recorder().last_error());
    }
    return_to_menu(app);
}

void App::stop_computer() {
    computer_ended(this, L"");
}

void App::switch_profile(const std::wstring &name) {
    if (name == profile_.name) {
        return;
    }
    save_now(this);
    airscreen::Profile p;
    std::string err;
    if (!airscreen::load_profile(name, p, &err)) {
        MessageBoxW(dialog_owner(), (L"That profile couldn't be opened: " + utf16(err)).c_str(), L"AirScreen",
                    MB_OK | MB_ICONWARNING);
        return;
    }
    profile_ = std::move(p);
    settings_.profile = utf8(profile_.name);
    airscreen::save_settings(settings_);
    live_ = -1;
    go_live(profile_.default_index());
    scenes_changed();
}

void App::layout(airscreen::DualLayout l) {
    airscreen::dual_layout(live_scene_, profile_, l);
    push_live(this, toggle_fade(this));
}

void App::keep_live_changes() {
    if (live_ < (int) profile_.scenes.size()) {
        profile_.scenes[(size_t) live_] = live_scene_;
        schedule_save(this);
        editor.scenes_changed();
    }
}

// Shortcuts: always active in AirScreen's own windows, and in every app when the setting is on. Windows
// refuses a combination another app already holds; those are listed in the settings rather than failing
// silently.
void register_hotkeys(App *app) {
    for (int i = 0; i < app->registered_hotkeys; ++i) {
        UnregisterHotKey(app->hwnd, i + 1);
    }
    app->registered_hotkeys = 0;
    app->hotkey_problems_.clear();
    app->hotkeys = app->settings_.hotkey_map.empty() ? airscreen::default_hotkeys()
                                                     : airscreen::parse_hotkeys(app->settings_.hotkey_map);
    if (!app->settings_.hotkeys) {
        return;
    }
    for (size_t i = 0; i < app->hotkeys.size(); ++i) {
        const auto &k = app->hotkeys[i];
        if (!k.vk) {
            continue;
        }
        if (!RegisterHotKey(app->hwnd, (int) i + 1, k.mods | MOD_NOREPEAT, k.vk)) {
            app->hotkey_problems_ += airscreen::hotkey_text(k.mods, k.vk) + L" (" + airscreen::action_label(k.action) +
                                     L") is taken by another app.\n";
        }
    }
    app->registered_hotkeys = (int) app->hotkeys.size();
}

void App::prefs_changed() {
    register_hotkeys(this);
    panel.apply_prefs();
    if (editor.is_open()) {
        airscreen::hide_from_capture(editor.hwnd(), settings_.hide_controls);
    }
}

// A key pressed in one of AirScreen's windows, matched against the shortcuts. Never while typing in a box.
bool local_shortcut(App *app, const MSG &msg) {
    if (msg.message != WM_KEYDOWN && msg.message != WM_SYSKEYDOWN) {
        return false;
    }
    HWND root = GetAncestor(msg.hwnd, GA_ROOT);
    if (root != app->hwnd && root != app->panel.hwnd() && root != app->editor.hwnd()) {
        return false;
    }
    wchar_t cls[32] = {};
    GetClassNameW(msg.hwnd, cls, 32);
    if (!_wcsicmp(cls, L"Edit") || !_wcsicmp(cls, L"msctls_hotkey32") || !_wcsicmp(cls, L"ComboBox")) {
        return false;
    }
    UINT mods = 0;
    mods |= (GetKeyState(VK_CONTROL) & 0x8000) ? MOD_CONTROL : 0;
    mods |= (GetKeyState(VK_MENU) & 0x8000) ? MOD_ALT : 0;
    mods |= (GetKeyState(VK_SHIFT) & 0x8000) ? MOD_SHIFT : 0;
    mods |= ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) ? MOD_WIN : 0;
    if (!mods) {
        return false;
    }
    for (const auto &k : app->hotkeys) {
        if (k.vk && k.vk == (UINT) msg.wParam && k.mods == mods) {
            app->run(k.action);
            return true;
        }
    }
    return false;
}

HMENU build_scene_menu(App *app) {
    HMENU m = CreatePopupMenu();
    for (size_t i = 0; i < app->profile_.scenes.size() && i < 10; ++i) {
        std::wstring label;
        for (wchar_t c : app->profile_.scenes[i].name) {
            label += c == L'&' ? std::wstring(L"&&") : std::wstring(1, c); // a literal "&" in a menu
        }
        if (i < 9) {
            label += L"\t" + std::to_wstring(i + 1);
        }
        AppendMenuW(m, MF_STRING | ((int) i == app->live_ ? MF_CHECKED : 0), IDM_SCENE_BASE + (UINT) i,
                    label.c_str());
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_SCENE_EDIT, L"Edit scenes…");
    AppendMenuW(m, MF_STRING, IDM_PREVIEW, L"Preview…");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    const int sizes[3] = {1080, 1440, 2160};
    const wchar_t *names[3] = {L"Canvas 1080p (smaller files)", L"Canvas 1440p", L"Canvas 4K (sharpest)"};
    for (int i = 0; i < 3; ++i) {
        // A different canvas size mid-clip would split the file; change it between recordings.
        AppendMenuW(m, MF_STRING | (app->settings_.canvas_h == sizes[i] ? MF_CHECKED : 0) |
                           (app->recording ? MF_GRAYED : 0),
                    IDM_CANVAS_BASE + i, names[i]);
    }
    return m;
}

void popup_settings_menu(App *app, POINT screen_pt, bool tray) {
    HMENU m = CreatePopupMenu();
    if (tray) {
        AppendMenuW(m, MF_STRING, IDM_TRAY_SHOW, L"Show AirScreen");
    }
    bool computer = app->receiver.video().computer_running();
    AppendMenuW(m, MF_STRING | (app->live() || app->recording ? 0 : MF_GRAYED), IDM_REC_TOGGLE,
                app->recording ? L"Stop recording\tCtrl+R" : L"Record clip\tCtrl+R");
    if (app->recording) {
        AppendMenuW(m, MF_STRING, IDM_REC_PAUSE,
                    app->receiver.recorder().paused() ? L"Resume recording\tCtrl+Shift+R" : L"Pause recording\tCtrl+Shift+R");
    }
    AppendMenuW(m, MF_POPUP, (UINT_PTR) build_scene_menu(app), L"Scenes");
    AppendMenuW(m, MF_STRING | (app->panel.visible() ? MF_CHECKED : 0), IDM_PANEL, L"Teaching controls\tCtrl+T");
    AppendMenuW(m, MF_STRING, IDM_COMPUTER, computer ? L"Share something else on this PC…" : L"Share the computer screen…");
    if (computer) {
        AppendMenuW(m, MF_STRING, IDM_COMPUTER_STOP, L"Stop sharing the computer screen");
    }
    AppendMenuW(m, MF_POPUP, (UINT_PTR) build_mic_menu(app),
                app->settings_.mic_enabled ? L"Microphone in clips (on)\tCtrl+M" : L"Microphone in clips (off)\tCtrl+M");
    AppendMenuW(m, MF_STRING, IDM_REC_FOLDER, L"Open recordings folder");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (app->settings_.high_quality ? MF_CHECKED : 0), IDM_QUALITY,
                L"High quality (4K, HEVC)");
    AppendMenuW(m, MF_STRING | (app->settings_.smooth ? MF_CHECKED : 0), IDM_SMOOTH,
                L"Smooth playback (+60 ms delay)");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_TEACH_PREFS, L"Teaching settings…");
    AppendMenuW(m, MF_STRING, IDM_TRAY_RENAME, L"Rename receiver…");
    AppendMenuW(m, MF_STRING | (app->settings_.always_on_top ? MF_CHECKED : 0), IDM_TRAY_TOP, L"Always on top");
    AppendMenuW(m, MF_STRING | (app->settings_.require_pin ? MF_CHECKED : 0), IDM_TRAY_PIN, L"Require pairing PIN");
    AppendMenuW(m, MF_STRING | (app->fullscreen ? MF_CHECKED : 0), IDM_TRAY_FULL, L"Fullscreen");
    AppendMenuW(m, MF_STRING | (app->settings_.fill_screen ? MF_CHECKED : 0), IDM_TRAY_FILL,
                L"Fill screen (no black bars)");
    AppendMenuW(m, MF_STRING | (airscreen::start_with_windows() ? MF_CHECKED : 0), IDM_TRAY_STARTUP,
                L"Start with Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_TRAY_FIREWALL, L"Allow through firewall");
    AppendMenuW(m, MF_STRING, IDM_TRAY_LOG, L"Open log file");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_TRAY_EXIT, L"Exit");
    SetForegroundWindow(app->hwnd);
    KillTimer(app->hwnd, kTimerOverlay);
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, screen_pt.x, screen_pt.y, 0, app->hwnd, nullptr);
    DestroyMenu(m);
    note_activity(app);
}

void show_tray_menu(App *app) {
    POINT pt;
    GetCursorPos(&pt);
    popup_settings_menu(app, pt, true);
}

void show_corner_menu(App *app) {
    RECT rc = {};
    GetWindowRect(app->settings_hwnd, &rc);
    POINT pt = {rc.left, rc.bottom + MulDiv(4, window_dpi(app->hwnd), 96)};
    popup_settings_menu(app, pt, false);
}

// "This PC  192.168.1.20  ·  Wi-Fi": the address of the adapter that reaches the router, so when the iPad
// can't see the PC it's easy to check both are on the same network.
std::wstring network_hint() {
    ULONG size = 16 * 1024;
    std::vector<BYTE> buf(size);
    auto *list = (IP_ADAPTER_ADDRESSES *) buf.data();
    ULONG flags = GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, list, &size) == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        list = (IP_ADAPTER_ADDRESSES *) buf.data();
    }
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, list, &size) != NO_ERROR) {
        return {};
    }
    for (auto *a = list; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK || !a->FirstGatewayAddress ||
            !a->FirstUnicastAddress) {
            continue;
        }
        auto *sin = (sockaddr_in *) a->FirstUnicastAddress->Address.lpSockaddr;
        wchar_t ip[32];
        const BYTE *b = (const BYTE *) &sin->sin_addr;
        swprintf_s(ip, L"%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
        return std::wstring(L"This PC  ") + ip + L"   ·   " + a->FriendlyName;
    }
    return {};
}

void paint_idle(App *app, HDC hdc, RECT rc) {
    airscreen::UiView v;
    v.name = app->utf16(app->settings_.name);
    v.status = app->overlay_status;
    v.pin = app->overlay_pin;
    v.connected = app->connected;
    v.always_on_top = app->settings_.always_on_top;
    v.require_pin = app->settings_.require_pin;
    v.fullscreen = app->fullscreen;
    v.dark = app->settings_.dark_mode;
    v.failed = app->start_failed;
    v.inset_right = 0;
    static std::wstring net = network_hint(); // adapters rarely change while the window is up
    v.detail = net;
    app->shell.paint(app->hwnd, hdc, rc, v, app->hover);
}

void handle_tray_cmd(App *app, UINT id) {
    switch (id) {
    case IDM_TRAY_SHOW:
        ShowWindow(app->hwnd, SW_RESTORE);
        SetForegroundWindow(app->hwnd);
        break;
    case IDM_TRAY_RENAME:
        do_rename(app);
        break;
    case IDM_TRAY_TOP:
        app->settings_.always_on_top = !app->settings_.always_on_top;
        airscreen::save_settings(app->settings_);
        if (!app->fullscreen) {
            apply_topmost(app->hwnd, app->settings_.always_on_top);
        }
        break;
    case IDM_TRAY_PIN:
        app->settings_.require_pin = !app->settings_.require_pin;
        app->receiver.set_require_pin(app->settings_.require_pin);
        airscreen::save_settings(app->settings_);
        break;
    case IDM_TRAY_FULL:
        toggle_fullscreen(app);
        break;
    case IDM_TRAY_FILL:
        app->settings_.fill_screen = !app->settings_.fill_screen;
        airscreen::save_settings(app->settings_);
        apply_video_fill(app);
        break;
    case IDM_TRAY_STARTUP:
        airscreen::set_start_with_windows(!airscreen::start_with_windows());
        break;
    case IDM_REC_TOGGLE:
        toggle_recording(app);
        break;
    case IDM_REC_PAUSE:
        toggle_pause(app);
        break;
    case IDM_SCENE_EDIT:
        app->open_editor(false);
        break;
    case IDM_PREVIEW:
        app->open_editor(true);
        break;
    case IDM_PANEL:
        app->panel.toggle();
        break;
    case IDM_COMPUTER:
        app->choose_computer();
        break;
    case IDM_COMPUTER_STOP:
        app->stop_computer();
        break;
    case IDM_TEACH_PREFS:
        app->open_settings();
        break;
    case IDM_MIC_OFF:
    case IDM_MIC_DEFAULT:
        app->settings_.mic_enabled = id == IDM_MIC_DEFAULT;
        if (id == IDM_MIC_DEFAULT) {
            app->settings_.mic_device.clear();
        }
        airscreen::save_settings(app->settings_);
        apply_mic(app);
        break;
    case IDM_REC_FOLDER: {
        auto dir = recordings_dir();
        ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        break;
    }
    case IDM_QUALITY:
        app->settings_.high_quality = !app->settings_.high_quality;
        airscreen::save_settings(app->settings_);
        app->receiver.set_high_quality(app->settings_.high_quality);
        if (app->connected) {
            MessageBoxW(app->hwnd, L"Stop and restart mirroring on the iPhone to apply the new quality.",
                        L"AirScreen", MB_OK | MB_ICONINFORMATION);
        }
        break;
    case IDM_SMOOTH:
        app->settings_.smooth = !app->settings_.smooth;
        airscreen::save_settings(app->settings_);
        app->receiver.set_smooth(app->settings_.smooth);
        break;
    case IDM_TRAY_FIREWALL:
        if (!airscreen::ensure_firewall_rule()) {
            MessageBoxW(app->hwnd, L"Could not add a firewall rule. Allow AirScreen when Windows asks, or run as administrator.",
                        L"AirScreen", MB_OK | MB_ICONINFORMATION);
        } else {
            MessageBoxW(app->hwnd, L"Firewall rule added for AirScreen.", L"AirScreen", MB_OK);
        }
        break;
    case IDM_TRAY_LOG: {
        auto p = app->utf16(airscreen::log_path());
        ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        break;
    }
    case IDM_TRAY_EXIT:
        DestroyWindow(app->hwnd);
        return;
    default:
        if (id >= IDM_SCENE_BASE && id < IDM_SCENE_BASE + 10) {
            app->go_live((int) (id - IDM_SCENE_BASE));
        } else if (id >= IDM_CANVAS_BASE && id < IDM_CANVAS_BASE + 3) {
            const int sizes[3] = {1080, 1440, 2160};
            app->settings_.canvas_h = sizes[id - IDM_CANVAS_BASE];
            airscreen::save_settings(app->settings_);
            app->receiver.video().set_canvas(app->settings_.canvas_h);
        } else if (id >= IDM_MIC_DEV_BASE && id < IDM_MIC_DEV_BASE + app->mics.size()) {
            app->settings_.mic_device = app->utf8(app->mics[id - IDM_MIC_DEV_BASE].id);
            app->settings_.mic_enabled = true;
            airscreen::save_settings(app->settings_);
            apply_mic(app);
        } else if (id >= IDM_MIC_GAIN_BASE && id < IDM_MIC_GAIN_BASE + sizeof(kMicGains) / sizeof(kMicGains[0])) {
            app->settings_.mic_gain_pct = kMicGains[id - IDM_MIC_GAIN_BASE];
            airscreen::save_settings(app->settings_);
            apply_mic(app);
        }
        break;
    }
    if (app->hwnd) {
        InvalidateRect(app->hwnd, nullptr, FALSE);
    }
}

void apply_hit(App *app, airscreen::UiHit hit) {
    if (app->live()) {
        return;
    }
    switch (hit) {
    case airscreen::UiHit::Name:
    case airscreen::UiHit::Rename:
        do_rename(app);
        break;
    case airscreen::UiHit::Fullscreen:
        toggle_fullscreen(app);
        break;
    case airscreen::UiHit::Topmost:
        handle_tray_cmd(app, IDM_TRAY_TOP);
        break;
    case airscreen::UiHit::Pin:
        handle_tray_cmd(app, IDM_TRAY_PIN);
        break;
    case airscreen::UiHit::Dark:
        app->settings_.dark_mode = !app->settings_.dark_mode;
        airscreen::save_settings(app->settings_);
        InvalidateRect(app->hwnd, nullptr, FALSE);
        break;
    default:
        break;
    }
}

void paint_gear(HWND hwnd, HDC hdc, bool hot) {
    RECT rc = {};
    GetClientRect(hwnd, &rc);
    int w = rc.right;
    int h = rc.bottom;
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
    HGDIOBJ old_bmp = SelectObject(mem, bmp);

    // Continuity plate body at rest; plate-hi on hover.
    COLORREF fill = hot ? RGB(58, 58, 60) : RGB(44, 44, 46);
    HBRUSH b = CreateSolidBrush(fill);
    FillRect(mem, &rc, b);
    DeleteObject(b);

    // Hairline rim: glass edge, brighter on hover.
    HPEN rim = CreatePen(PS_SOLID, 1, hot ? RGB(120, 120, 124) : RGB(96, 96, 100));
    HGDIOBJ old_pen = SelectObject(mem, rim);
    HGDIOBJ old_br = SelectObject(mem, GetStockObject(NULL_BRUSH));
    Ellipse(mem, 0, 0, w - 1, h - 1);
    SelectObject(mem, old_br);
    SelectObject(mem, old_pen);
    DeleteObject(rim);

    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, RGB(245, 245, 247));
    // Optical: MDL2 gear sits heavy; size a hair under the circle for balance.
    int px = MulDiv(17, window_dpi(hwnd), 96);
    HFONT font = CreateFontW(px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe MDL2 Assets");
    HGDIOBJ old_font = SelectObject(mem, font);
    RECT text_rc = rc;
    text_rc.top -= 1; // optical vertical center
    DrawTextW(mem, L"", -1, &text_rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(mem, old_font);
    DeleteObject(font);
    BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old_bmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

int scene_segment(HWND hwnd, int x) {
    if (!g_app) {
        return -1;
    }
    int n = (std::max)(1, bar_scenes(g_app));
    RECT rc = {};
    GetClientRect(hwnd, &rc);
    int pad = MulDiv(4, window_dpi(hwnd), 96);
    int seg = (rc.right - pad * 2) / n;
    if (seg <= 0 || x < pad || x >= pad + seg * n) {
        return -1;
    }
    return (x - pad) / seg;
}

void paint_scenes(HWND hwnd, HDC hdc, int current, int hot) {
    RECT rc = {};
    GetClientRect(hwnd, &rc);
    int w = rc.right, h = rc.bottom;
    UINT dpi = window_dpi(hwnd);
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
    HGDIOBJ old_bmp = SelectObject(mem, bmp);
    HBRUSH plate = CreateSolidBrush(RGB(30, 30, 34));
    FillRect(mem, &rc, plate);
    DeleteObject(plate);

    int n = (std::max)(1, bar_scenes(g_app));
    int pad = MulDiv(4, dpi, 96);
    int seg = (w - pad * 2) / n;
    HFONT font = CreateFontW(-MulDiv(13, dpi, 96), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HGDIOBJ old_font = SelectObject(mem, font);
    SetBkMode(mem, TRANSPARENT);
    for (int i = 0; i < n && i < (int) g_app->profile_.scenes.size(); ++i) {
        RECT s = {pad + seg * i, pad, pad + seg * (i + 1), h - pad};
        bool on = i == current;
        if (on || i == hot) {
            // The live scene is a white pill; hover is a faint one.
            HBRUSH b = CreateSolidBrush(on ? RGB(245, 245, 247) : RGB(58, 58, 64));
            HPEN none = (HPEN) GetStockObject(NULL_PEN);
            HGDIOBJ ob = SelectObject(mem, b), op = SelectObject(mem, none);
            int r = s.bottom - s.top;
            RoundRect(mem, s.left, s.top, s.right + 1, s.bottom + 1, r, r);
            SelectObject(mem, ob);
            SelectObject(mem, op);
            DeleteObject(b);
        }
        std::wstring label = std::to_wstring(i + 1) + L"   " + g_app->profile_.scenes[(size_t) i].name;
        RECT t = s;
        InflateRect(&t, -MulDiv(8, dpi, 96), 0);
        SetTextColor(mem, on ? RGB(24, 24, 28) : RGB(225, 225, 230));
        DrawTextW(mem, label.c_str(), -1, &t, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    SelectObject(mem, old_font);
    DeleteObject(font);
    BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old_bmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

LRESULT CALLBACK scenes_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    App *app = g_app;
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (app) {
            paint_scenes(hwnd, hdc, app->live_, app->scene_hot);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);
        int hot = scene_segment(hwnd, GET_X_LPARAM(lparam));
        if (app && hot != app->scene_hot) {
            app->scene_hot = hot;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        if (app) {
            note_activity(app);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (app) {
            app->scene_hot = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (app) {
            app->go_live(scene_segment(hwnd, GET_X_LPARAM(lparam)));
            note_activity(app);
        }
        return 0;
    case WM_SETCURSOR:
        SetCursor(LoadCursor(nullptr, IDC_HAND));
        return TRUE;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT CALLBACK video_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    HWND parent = GetParent(hwnd);
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEACTIVATE:
        SetFocus(parent);
        return MA_NOACTIVATE;
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONUP: {
        POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        MapWindowPoints(hwnd, parent, &pt, 1);
        return SendMessageW(parent, msg, wparam, MAKELPARAM(pt.x, pt.y));
    }
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT CALLBACK gear_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    App *app = g_app;
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        paint_gear(hwnd, hdc, app && app->settings_hot);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);
        if (app && !app->settings_hot) {
            app->settings_hot = true;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        if (app) {
            note_activity(app);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (app) {
            app->settings_hot = false;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (app) {
            show_corner_menu(app);
        }
        return 0;
    case WM_SETCURSOR:
        SetCursor(LoadCursor(nullptr, IDC_HAND));
        return TRUE;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    App *app = g_app;
    switch (msg) {
    case WM_CREATE:
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        paint_idle(app, hdc, rc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        if (app) {
            layout_chrome(app);
            app->receiver.video().resize();
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_DPICHANGED: {
        RECT *sugg = (RECT *) lparam;
        SetWindowPos(hwnd, nullptr, sugg->left, sugg->top, sugg->right - sugg->left, sugg->bottom - sugg->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        if (app) {
            layout_chrome(app);
        }
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto *mm = (MINMAXINFO *) lparam;
        mm->ptMinTrackSize.x = 720;
        mm->ptMinTrackSize.y = 480;
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (app) {
            note_activity(app);
            POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            int hov = (int) app->shell.hit_test(pt);
            if (hov != app->hover) {
                app->hover = hov;
                if (!app->live()) {
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (app && app->hover) {
            app->hover = 0;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_SETCURSOR:
        if (app && !app->live() && LOWORD(lparam) == HTCLIENT) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            if (app->shell.cursor_hand(app->shell.hit_test(pt))) {
                SetCursor(LoadCursor(nullptr, IDC_HAND));
                return TRUE;
            }
        }
        break;
    case WM_LBUTTONUP:
        if (app && !app->live()) {
            POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            apply_hit(app, app->shell.hit_test(pt));
        }
        return 0;
    case WM_TIMER:
        if (!app) {
            return 0;
        }
        if (wparam == kTimerOverlay && app->live()) {
            POINT pt = {};
            GetCursorPos(&pt);
            if (WindowFromPoint(pt) != app->settings_hwnd) {
                show_settings_button(app, false);
            }
        } else if (wparam == kTimerPin) {
            KillTimer(hwnd, kTimerPin);
            app->overlay_pin.clear();
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (wparam == kTimerDisconnect) {
            KillTimer(hwnd, kTimerDisconnect);
            ipad_ended(app);
        } else if (wparam == kTimerDrift && !app->live()) {
            // Full rate only while you're looking at it: a background window ticks at 5 fps, a minimized
            // or hidden one not at all.
            static unsigned tick = 0;
            bool front = GetForegroundWindow() == hwnd;
            if (IsWindowVisible(hwnd) && !IsIconic(hwnd) && (front || ++tick % 4 == 0)) {
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        } else if (wparam == kTimerRec) {
            if (app->recording && !app->receiver.recorder().active()) {
                app->receiver.recorder().stop();
                finish_recording(app, app->receiver.recorder().last_error());
            } else {
                update_title(app);
            }
        } else if (wparam == kTimerSave) {
            save_now(app);
        }
        return 0;
    case WM_LBUTTONDBLCLK:
        if (app && !app->live()) {
            POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            auto hit = app->shell.hit_test(pt);
            if (hit == airscreen::UiHit::Name || hit == airscreen::UiHit::Rename) {
                do_rename(app);
                return 0;
            }
            if (hit != airscreen::UiHit::None) {
                return 0; // a double-click on a tile is just two clicks; only the open field goes full screen
            }
        }
        toggle_fullscreen(app);
        return 0;
    case WM_RBUTTONUP:
        if (app) {
            POINT pt = {};
            GetCursorPos(&pt);
            popup_settings_menu(app, pt, false);
        }
        return 0;
    case WM_KEYDOWN: {
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (wparam == VK_F11) {
            toggle_fullscreen(app);
        } else if (wparam == VK_F2) {
            do_rename(app);
        } else if (wparam == 'R' && ctrl && shift) {
            toggle_pause(app);
        } else if (wparam == VK_F9 || (wparam == 'R' && ctrl)) {
            toggle_recording(app);
        } else if (wparam == 'M' && ctrl) {
            toggle_mic(app);
        } else if (wparam == 'T' && ctrl) {
            app->panel.toggle();
        } else if (wparam >= '1' && wparam <= '9' && !ctrl) {
            app->go_live((int) (wparam - '1'));
        } else if (wparam >= VK_NUMPAD1 && wparam <= VK_NUMPAD9) {
            app->go_live((int) (wparam - VK_NUMPAD1));
        } else if (wparam == VK_ESCAPE && app->fullscreen) {
            set_fullscreen(app, false);
        }
        return 0;
    }
    case WM_HOTKEY:
        if (app && wparam >= 1 && wparam <= app->hotkeys.size()) {
            app->run(app->hotkeys[wparam - 1].action);
        }
        return 0;
    case WM_AIR_EVENT: {
        auto ev = (airscreen::UiEvent) wparam;
        auto *s = (std::string *) lparam;
        std::string msg = s ? *s : "";
        delete s;
        switch (ev) {
        case airscreen::UiEvent::Connected:
            KillTimer(hwnd, kTimerDisconnect);
            set_boost(true);
            app->connected = true;
            app->overlay_pin.clear(); // the device is back: the PIN was typed
            KillTimer(hwnd, kTimerPin);
            app->overlay_status = L"Connected";
            if (!msg.empty()) {
                app->overlay_status = L"Connected: " + app->utf16(msg);
            }
            update_title(app);
            InvalidateRect(hwnd, nullptr, FALSE);
            break;
        case airscreen::UiEvent::VideoReady:
            KillTimer(hwnd, kTimerDisconnect);
            app->connected = true;
            app->mirroring = true;
            show_video_surface(app);
            if (app->settings_.panel_auto & 1) {
                app->panel.show(false);
            }
            break;
        case airscreen::UiEvent::Disconnected:
            SetTimer(hwnd, kTimerDisconnect, kDisconnectMs, nullptr);
            break;
        case airscreen::UiEvent::Pin:
            app->overlay_pin = app->utf16(msg);
            SetTimer(hwnd, kTimerPin, kPinMs, nullptr);
            // The PIN is useless unless it's seen: bring the window up, even from the tray.
            if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) {
                ShowWindow(hwnd, SW_RESTORE);
            }
            SetForegroundWindow(hwnd);
            FlashWindow(hwnd, TRUE);
            InvalidateRect(hwnd, nullptr, FALSE);
            break;
        case airscreen::UiEvent::Log:
            app->overlay_detail = app->utf16(msg);
            break;
        case airscreen::UiEvent::RecordingStopped:
            finish_recording(app, msg);
            break;
        case airscreen::UiEvent::ComputerReady:
            if (app->receiver.video().computer_live()) {
                show_video_surface(app);
                if (app->settings_.panel_auto & 1) {
                    app->panel.show(false);
                }
            }
            break;
        case airscreen::UiEvent::ComputerClosed:
            if (app->receiver.video().computer_running()) {
                airscreen::append_log("computer capture ended: the window closed or the display went away");
                computer_ended(app, L"The shared " + std::wstring(app->computer_name.empty() ? L"screen" : L"window") +
                                        L" closed, so the computer screen stopped.");
            }
            break;
        case airscreen::UiEvent::VideoSize:
        case airscreen::UiEvent::ClientName:
            if (ev == airscreen::UiEvent::ClientName && !msg.empty()) {
                app->overlay_status = L"Connected: " + app->utf16(msg);
                update_title(app);
            }
            break;
        }
        app->panel.refresh();
        return 0;
    }
    case WM_TRAY:
        if (LOWORD(lparam) == WM_RBUTTONUP || LOWORD(lparam) == WM_CONTEXTMENU) {
            show_tray_menu(app);
        } else if (LOWORD(lparam) == WM_LBUTTONDBLCLK || LOWORD(lparam) == WM_LBUTTONUP) {
            ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
        }
        return 0;
    case WM_COMMAND:
        handle_tray_cmd(app, LOWORD(wparam));
        return 0;
    case WM_DESTROY: {
        if (app->fullscreen) {
            set_fullscreen(app, false);
        }
        WINDOWPLACEMENT wp = {sizeof(wp)};
        if (GetWindowPlacement(hwnd, &wp)) {
            RECT r = wp.rcNormalPosition;
            app->settings_.win_x = r.left;
            app->settings_.win_y = r.top;
            app->settings_.win_w = r.right - r.left;
            app->settings_.win_h = r.bottom - r.top;
            airscreen::save_settings(app->settings_);
        }
        save_now(app);
        app->editor.close();
        app->panel.destroy();
        for (int i = 0; i < app->registered_hotkeys; ++i) {
            UnregisterHotKey(hwnd, i + 1);
        }
        if (app->recording) {
            app->receiver.recorder().stop(); // finalize the clip before the window goes away
            app->recording = false;
        }
        app->receiver.video().stop_computer();
        KillTimer(hwnd, kTimerRec);
        KillTimer(hwnd, kTimerDrift);
        KillTimer(hwnd, kTimerOverlay);
        KillTimer(hwnd, kTimerDisconnect);
        KillTimer(hwnd, kTimerSave);
        Shell_NotifyIconW(NIM_DELETE, &app->nid);
        HICON shell32 = app->shell.icon32();
        HICON shell16 = app->shell.icon16();
        if (app->icon_sm && app->icon_sm != app->icon && app->icon_sm != shell16) {
            DestroyIcon(app->icon_sm);
        }
        app->icon_sm = nullptr;
        if (app->icon && app->icon != shell32) {
            DestroyIcon(app->icon);
        }
        app->icon = nullptr;
        app->receiver.stop();
        app->shell.shutdown();
        PostQuitMessage(0);
        return 0;
    }
    }
    if (msg == g_taskbar_created && app) {
        Shell_NotifyIconW(NIM_ADD, &app->nid);
        Shell_NotifyIconW(NIM_SETVERSION, &app->nid);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep) {
    static volatile LONG once = 0;
    if (InterlockedExchange(&once, 1) != 0) {
        return EXCEPTION_EXECUTE_HANDLER;
    }
    char buf[160];
    snprintf(buf, sizeof(buf), "CRASH code=0x%08X addr=%p",
             (unsigned) ep->ExceptionRecord->ExceptionCode,
             ep->ExceptionRecord->ExceptionAddress);
    airscreen::append_log(buf);

    std::wstring dump = airscreen::app_data_dir() + L"\\crash.dmp";
    HANDLE file = CreateFileW(dump.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION info = {};
        info.ThreadId = GetCurrentThreadId();
        info.ExceptionPointers = ep;
        info.ClientPointers = FALSE;
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, MiniDumpWithDataSegs, &info, nullptr,
                          nullptr);
        CloseHandle(file);
        airscreen::append_log("wrote crash.dmp");
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

bool activate_existing() {
    HWND w = FindWindowW(L"AirScreenWindow", nullptr);
    if (!w) {
        return false;
    }
    ShowWindow(w, SW_RESTORE);
    SetForegroundWindow(w);
    return true;
}

// The teaching profile in use, or the sample scenes the first time (seeded with any text typed into the old
// fixed scenes). True when the samples were just created.
bool load_teaching(App *app) {
    std::wstring name = app->utf16(app->settings_.profile);
    if (name.empty()) {
        name = L"General Presentation";
    }
    std::string err;
    bool fresh = false;
    if (!airscreen::load_profile(name, app->profile_, &err)) {
        if (airscreen::profile_exists(name)) {
            // Keep the unreadable file for a look later; teaching goes on with the samples.
            airscreen::append_log(("profile unreadable (" + err + "); starting from the sample scenes").c_str());
            airscreen::set_aside_profile(name);
            app->message = L"The scenes file couldn't be read (kept as .unreadable); using the sample scenes.";
        } else {
            fresh = true;
        }
        app->profile_ = airscreen::default_profile(name, app->settings_);
        if (!airscreen::save_profile(app->profile_, &err)) {
            airscreen::append_log(("profile save failed: " + err).c_str());
        }
    }
    app->settings_.profile = app->utf8(app->profile_.name);
    app->live_ = app->profile_.default_index();
    app->live_scene_ = app->profile_.scenes[(size_t) app->live_];
    return fresh;
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdline, int show) {
    SetUnhandledExceptionFilter(crash_filter);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    HANDLE instance = CreateMutexW(nullptr, TRUE, L"Local\\AirScreen.Receiver.1");
    if (instance && GetLastError() == ERROR_ALREADY_EXISTS) {
        activate_existing();
        CloseHandle(instance);
        return 0;
    }

    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES | ICC_BAR_CLASSES |
                                                 ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES | ICC_HOTKEY_CLASS};
    InitCommonControlsEx(&icc);

    ULONG_PTR gdip = airscreen::gdiplus_startup();
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    (void) hr;
    airscreen::rotate_log();
    airscreen::append_log("AirScreen starting");
    g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    bool start_in_tray = wcsstr(cmdline, L"--tray") != nullptr;
    bool teach = wcsstr(cmdline, L"--teach") != nullptr;

    App app;
    g_app = &app;
    app.inst = inst;
    app.settings_ = airscreen::load_settings();
    bool first_run = load_teaching(&app);
    app.shell.startup();

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    if (FAILED(LoadIconMetric(inst, MAKEINTRESOURCEW(IDI_APP), LIM_LARGE, &app.icon)) || !app.icon) {
        app.icon = app.shell.icon32();
    }
    if (FAILED(LoadIconMetric(inst, MAKEINTRESOURCEW(IDI_APP), LIM_SMALL, &app.icon_sm)) || !app.icon_sm) {
        app.icon_sm = app.shell.icon16() ? app.shell.icon16() : app.icon;
    }
    wc.hIcon = app.icon;
    wc.hIconSm = app.icon_sm;
    wc.hbrBackground = (HBRUSH) GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"AirScreenWindow";
    if (!RegisterClassExW(&wc)) {
        airscreen::append_log("RegisterClassEx failed");
        MessageBoxW(nullptr, L"Could not register the AirScreen window class.", L"AirScreen", MB_OK | MB_ICONERROR);
        return 1;
    }

    WNDCLASSEXW vc = wc;
    vc.style = CS_DBLCLKS;
    vc.lpfnWndProc = video_proc;
    vc.lpszClassName = L"AirScreenVideo";
    vc.hIcon = nullptr;
    if (!RegisterClassExW(&vc)) {
        airscreen::append_log("RegisterClassEx video failed");
        return 1;
    }

    WNDCLASSEXW gc = wc;
    gc.style = 0;
    gc.lpfnWndProc = gear_proc;
    gc.lpszClassName = L"AirScreenGear";
    gc.hCursor = LoadCursor(nullptr, IDC_HAND);
    gc.hIcon = nullptr;
    gc.hbrBackground = nullptr;
    if (!RegisterClassExW(&gc)) {
        airscreen::append_log("RegisterClassEx gear failed");
        return 1;
    }
    WNDCLASSEXW sc = gc;
    sc.lpfnWndProc = scenes_proc;
    sc.lpszClassName = L"AirScreenScenes";
    RegisterClassExW(&sc);

    app.hwnd = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, L"AirScreen",
                               WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 1100, 720,
                               nullptr, nullptr, inst, nullptr);
    if (!app.hwnd) {
        airscreen::append_log("CreateWindowEx failed");
        MessageBoxW(nullptr, L"Could not create the AirScreen window.", L"AirScreen", MB_OK | MB_ICONERROR);
        return 1;
    }
    app.video_hwnd = CreateWindowExW(0, L"AirScreenVideo", L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 0, 0, app.hwnd,
                                     nullptr, inst, nullptr);
    app.settings_hwnd = CreateWindowExW(0, L"AirScreenGear", L"Settings", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 40, 40,
                                        app.hwnd, (HMENU)(INT_PTR) IDC_SETTINGS, inst, nullptr);
    app.scenes_hwnd = CreateWindowExW(0, L"AirScreenScenes", L"Scenes", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 100, 40,
                                      app.hwnd, nullptr, inst, nullptr);
    RECT saved = {app.settings_.win_x, app.settings_.win_y, app.settings_.win_x + app.settings_.win_w,
                  app.settings_.win_y + app.settings_.win_h};
    if (app.settings_.win_w > 0 && app.settings_.win_h > 0 && MonitorFromRect(&saved, MONITOR_DEFAULTTONULL)) {
        WINDOWPLACEMENT wp = {sizeof(wp)};
        GetWindowPlacement(app.hwnd, &wp);
        wp.rcNormalPosition = airscreen::visible_rect(saved, 720, 480);
        wp.showCmd = SW_HIDE;
        SetWindowPlacement(app.hwnd, &wp);
    }
    set_dark_title(app.hwnd);
    apply_topmost(app.hwnd, app.settings_.always_on_top);
    update_title(&app);
    layout_chrome(&app);

    (void) show;
    if (!start_in_tray) {
        ShowWindow(app.hwnd, SW_SHOWNORMAL);
        UpdateWindow(app.hwnd);
        SetForegroundWindow(app.hwnd);
    }

    app.nid.cbSize = sizeof(app.nid);
    app.nid.hWnd = app.hwnd;
    app.nid.uID = kTrayId;
    app.nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    app.nid.uCallbackMessage = WM_TRAY;
    app.nid.hIcon = app.icon_sm ? app.icon_sm : app.icon;
    wcscpy_s(app.nid.szTip, L"AirScreen");
    Shell_NotifyIconW(NIM_ADD, &app.nid);
    app.nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &app.nid);
    SetTimer(app.hwnd, kTimerDrift, 50, nullptr); // 20 fps: enough for the slow AirPlay rings and drift

    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#ifdef PROCESS_POWER_THROTTLING_CURRENT_VERSION
    PROCESS_POWER_THROTTLING_STATE pt = {};
    pt.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    pt.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    pt.StateMask = 0;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof(pt));
#endif

    // Let the first paint land before the receiver starts, but bounded: the idle animation keeps the queue
    // from ever running dry, so draining it until empty would never get to starting AirPlay.
    MSG pump;
    ULONGLONG pump_until = GetTickCount64() + 250;
    while (GetTickCount64() < pump_until && PeekMessageW(&pump, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&pump);
        DispatchMessageW(&pump);
    }

    auto ui = [&app](airscreen::UiEvent ev, const std::string &msg) { app.post_ui(ev, msg); };
    ntp_global_init();
    app.overlay_status = L"Starting AirPlay…";
    InvalidateRect(app.hwnd, nullptr, FALSE);
    if (!app.receiver.start(app.video_hwnd, app.settings_, ui)) {
        app.start_failed = true;
        app.overlay_status = L"Could not start. Check the log from Settings";
        InvalidateRect(app.hwnd, nullptr, FALSE);
        MessageBoxW(app.hwnd,
                    L"Could not start the AirPlay receiver.\n\nAllow AirScreen through the firewall (tray menu) and check %APPDATA%\\AirScreen\\airscreen.log",
                    L"AirScreen", MB_OK | MB_ICONERROR);
    } else {
        app.overlay_status = L"Ready to connect";
        apply_video_fill(&app);
        apply_mic(&app);
        auto &video = app.receiver.video();
        video.set_computer_events([&app] { app.post_ui(airscreen::UiEvent::ComputerReady, {}); },
                                  [&app] { app.post_ui(airscreen::UiEvent::ComputerClosed, {}); });
        video.set_canvas(app.settings_.canvas_h);
        video.set_scene(app.live_scene_, 0);
        InvalidateRect(app.hwnd, nullptr, FALSE);
    }

    // The teaching controls: made once, shown and hidden from then on.
    app.panel.create(inst, &app);
    register_hotkeys(&app);
    if (teach || app.settings_.teach_mode || first_run) {
        app.panel.show(false);
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (local_shortcut(&app, msg)) {
            continue;
        }
        // Shortcuts work whichever of our windows (main, video surface, gear button) has focus.
        if (msg.message == WM_KEYDOWN && msg.hwnd != app.hwnd && app.hwnd && IsChild(app.hwnd, msg.hwnd)) {
            msg.hwnd = app.hwnd;
        }
        if (app.editor.is_open() && IsDialogMessageW(app.editor.hwnd(), &msg)) {
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CoUninitialize();
    airscreen::gdiplus_shutdown(gdip);
    set_boost(false);
    if (instance) {
        CloseHandle(instance);
    }
    return 0;
}
