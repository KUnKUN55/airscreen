#pragma once

#include "capture.h"
#include "scene.h"

#include <windows.h>
#include <string>
#include <vector>

namespace airscreen {

class VideoPipeline;
struct Settings;

// Everything a button, menu entry or keyboard shortcut can do while teaching. The names are what settings.ini
// stores, so they never change once shipped.
enum class Action {
    None = 0,
    ShowIPad,
    ShowComputer,
    ShowBoth,
    SwapScreens,
    ToggleSecondary,
    ToggleText,
    ShowText,
    HideText,
    ToggleBox,
    ToggleExtras,
    NextScene,
    PrevScene,
    Scene1, // .. Scene9
    Scene9 = Scene1 + 8,
    ToggleRecord,
    PauseRecord,
    TogglePanel,
    Preview,
    EditText,
    kCount
};

const char *action_key(Action a);
Action action_from_key(const std::string &key);
std::wstring action_label(Action a);

// Shortcut: modifiers (MOD_CONTROL, MOD_ALT, MOD_SHIFT, MOD_WIN) and a virtual key.
struct Hotkey {
    Action action = Action::None;
    UINT mods = 0;
    UINT vk = 0;
};
std::vector<Hotkey> parse_hotkeys(const std::string &s);
std::string format_hotkeys(const std::vector<Hotkey> &keys);
std::vector<Hotkey> default_hotkeys();
std::wstring hotkey_text(UINT mods, UINT vk);

// What the control panel shows; read fresh every time it paints, so it is never out of date.
struct TeachStatus {
    bool ipad = false; // the iPad's picture is arriving
    std::wstring ipad_name;
    bool computer = false;     // a computer screen is being captured
    bool computer_wait = false; // started, no picture yet
    std::wstring computer_name;
    bool recording = false;
    bool paused = false;
    double rec_seconds = 0;
    bool mic = false;
    std::wstring message; // last problem worth telling (a capture that ended, a save that failed)
};

// The app side of the teaching tools: the control panel and the editor drive everything through this.
class TeachHost {
public:
    virtual ~TeachHost() = default;
    virtual Profile &profile() = 0;
    virtual Settings &settings() = 0;
    virtual VideoPipeline &video() = 0;
    virtual void save_settings() = 0;
    // The scene on air (index into profile().scenes) and its working copy, which quick actions adjust
    // without touching the saved scene.
    virtual int live_index() = 0;
    virtual const Scene &live_scene() = 0;
    virtual void go_live(int index) = 0;
    // The saved scene at `index` was edited: save soon, and if `to_air` and it's on air, show it now.
    virtual void scene_changed(int index, bool to_air) = 0;
    // The scene list itself changed (added, removed, reordered, renamed).
    virtual void scenes_changed() = 0;
    virtual void run(Action a) = 0;
    // Puts `text` in a text layer of the scene on air (id 0: a new one at the top), shown or hidden, and with `keep`
    // also in the saved scene.
    virtual void set_live_text(uint32_t layer_id, const std::wstring &text, bool show, bool keep) = 0;
    virtual TeachStatus status() = 0;
    virtual void choose_computer() = 0;
    virtual void stop_computer() = 0;
    virtual void open_editor(bool preview_only) = 0;
    virtual void open_settings() = 0;
    virtual void switch_profile(const std::wstring &name) = 0;
    virtual void layout(DualLayout l) = 0;
    virtual void keep_live_changes() = 0;
    // Settings changed in the settings dialog: re-register shortcuts, restyle the panel and so on.
    virtual void prefs_changed() = 0;
    // Shortcuts Windows refused (another app holds them), for the settings dialog to show.
    virtual std::wstring hotkey_problems() = 0;
    virtual HWND main_window() = 0;
};

// Small modal dialogs (dialogs.cpp).
bool prompt_text(HWND owner, const wchar_t *title, const wchar_t *label, std::wstring &value);
// Lists the displays and windows to share, with a preview of each and a warning when a whole display is
// picked. False when cancelled.
bool pick_capture_target(HWND owner, TeachHost *host, CaptureTarget &out);
void edit_live_text(HWND owner, TeachHost *host);
void teach_settings(HWND owner, TeachHost *host);

// Keeps AirScreen's own control windows out of screen captures (Windows 10 2004 and later), so sharing a
// whole display never shows the teacher's controls.
void hide_from_capture(HWND hwnd, bool hide);
// Puts a saved window rectangle back on a screen that still exists; a rectangle mostly off-screen is moved
// onto the nearest one.
RECT visible_rect(RECT saved, int min_w, int min_h);

} // namespace airscreen
