#include "teach.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace airscreen {
namespace {

struct ActionInfo {
    const char *key;
    const wchar_t *label;
};

const ActionInfo kActions[] = {
    {"", L""},
    {"ipad", L"Show iPad"},
    {"computer", L"Show computer"},
    {"both", L"Show both"},
    {"swap", L"Swap screens"},
    {"secondary", L"Show / hide second screen"},
    {"text", L"Show / hide text"},
    {"textshow", L"Show text"},
    {"texthide", L"Hide text"},
    {"box", L"Text background on / off"},
    {"extras", L"Logos and frames on / off"},
    {"next", L"Next scene"},
    {"prev", L"Previous scene"},
    {"scene1", L"Scene 1"},
    {"scene2", L"Scene 2"},
    {"scene3", L"Scene 3"},
    {"scene4", L"Scene 4"},
    {"scene5", L"Scene 5"},
    {"scene6", L"Scene 6"},
    {"scene7", L"Scene 7"},
    {"scene8", L"Scene 8"},
    {"scene9", L"Scene 9"},
    {"record", L"Start / stop recording"},
    {"pause", L"Pause / resume recording"},
    {"panel", L"Show / hide control panel"},
    {"preview", L"Open preview"},
    {"edittext", L"Edit text on air"},
    {"laser", L"Laser pointer on / off"},
};
static_assert(sizeof(kActions) / sizeof(kActions[0]) == (size_t) Action::kCount, "one entry per action");

} // namespace

const char *action_key(Action a) {
    int i = (int) a;
    return i > 0 && i < (int) Action::kCount ? kActions[i].key : "";
}

Action action_from_key(const std::string &key) {
    for (int i = 1; i < (int) Action::kCount; ++i) {
        if (key == kActions[i].key) {
            return (Action) i;
        }
    }
    return Action::None;
}

std::wstring action_label(Action a) {
    int i = (int) a;
    return i > 0 && i < (int) Action::kCount ? kActions[i].label : L"";
}

std::vector<Hotkey> parse_hotkeys(const std::string &s) {
    std::vector<Hotkey> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        size_t a = item.find(':'), b = item.rfind(':');
        if (a == std::string::npos || a == b) {
            continue;
        }
        Hotkey k;
        k.action = action_from_key(item.substr(0, a));
        k.mods = (UINT) strtoul(item.substr(a + 1, b - a - 1).c_str(), nullptr, 10);
        k.vk = (UINT) strtoul(item.substr(b + 1).c_str(), nullptr, 10);
        if (k.action != Action::None) {
            out.push_back(k); // vk 0 = deliberately unassigned
        }
    }
    return out;
}

std::string format_hotkeys(const std::vector<Hotkey> &keys) {
    std::string s;
    for (auto &k : keys) {
        if (!s.empty()) {
            s += ',';
        }
        char b[64];
        snprintf(b, sizeof(b), "%s:%u:%u", action_key(k.action), k.mods, k.vk);
        s += b;
    }
    return s;
}

// Ctrl+Alt with a letter or digit: not taken by Windows, Office, browsers, PDF readers or drawing apps, and
// never typed by accident, so they are safe while another app has the keyboard.
std::vector<Hotkey> default_hotkeys() {
    const UINT m = MOD_CONTROL | MOD_ALT;
    std::vector<Hotkey> k = {
        {Action::ShowIPad, m, 'I'},         {Action::ShowComputer, m, 'C'}, {Action::ShowBoth, m, 'B'},
        {Action::ToggleSecondary, m, 'S'},  {Action::ToggleText, m, 'T'},   {Action::TogglePanel, m, 'P'},
        {Action::NextScene, m, VK_RIGHT},   {Action::PrevScene, m, VK_LEFT}, {Action::ToggleRecord, m, 'R'},
        {Action::PauseRecord, m, VK_SPACE}, {Action::Laser, m, 'L'},
    };
    for (int i = 0; i < 9; ++i) {
        k.push_back({(Action) ((int) Action::Scene1 + i), m, (UINT) ('1' + i)});
    }
    return k;
}

std::vector<Hotkey> load_hotkeys(const std::string &map) {
    if (map.empty()) {
        return default_hotkeys();
    }
    std::vector<Hotkey> keys = parse_hotkeys(map);
    for (const Hotkey &d : default_hotkeys()) {
        bool known = std::any_of(keys.begin(), keys.end(), [&](const Hotkey &k) { return k.action == d.action; });
        bool taken = std::any_of(keys.begin(), keys.end(),
                                 [&](const Hotkey &k) { return k.vk && k.vk == d.vk && k.mods == d.mods; });
        if (!known && !taken) {
            keys.push_back(d);
        }
    }
    return keys;
}

std::wstring hotkey_text(UINT mods, UINT vk) {
    if (!vk) {
        return L"(none)";
    }
    std::wstring s;
    if (mods & MOD_CONTROL) {
        s += L"Ctrl+";
    }
    if (mods & MOD_ALT) {
        s += L"Alt+";
    }
    if (mods & MOD_SHIFT) {
        s += L"Shift+";
    }
    if (mods & MOD_WIN) {
        s += L"Win+";
    }
    UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    LONG lp = (LONG) (scan << 16);
    if (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN || vk == VK_PRIOR || vk == VK_NEXT ||
        vk == VK_HOME || vk == VK_END || vk == VK_INSERT || vk == VK_DELETE) {
        lp |= 1 << 24; // the arrow-block keys, not the keypad's
    }
    wchar_t name[64] = {};
    if (GetKeyNameTextW(lp, name, 64) > 0) {
        s += name;
    } else {
        wchar_t b[16];
        swprintf_s(b, L"#%u", vk);
        s += b;
    }
    return s;
}

void hide_from_capture(HWND hwnd, bool hide) {
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif
    if (!SetWindowDisplayAffinity(hwnd, hide ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE) && hide) {
        SetWindowDisplayAffinity(hwnd, WDA_NONE); // older Windows: stays visible rather than turning black
    }
}

RECT visible_rect(RECT r, int min_w, int min_h) {
    int w = (std::max)((int) (r.right - r.left), min_w);
    int h = (std::max)((int) (r.bottom - r.top), min_h);
    r.right = r.left + w;
    r.bottom = r.top + h;
    HMONITOR mon = MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    RECT wa = mi.rcWork;
    w = (std::min)(w, (int) (wa.right - wa.left));
    h = (std::min)(h, (int) (wa.bottom - wa.top));
    int x = r.left, y = r.top;
    x = (std::max)((int) wa.left, (std::min)(x, (int) wa.right - w));
    y = (std::max)((int) wa.top, (std::min)(y, (int) wa.bottom - h));
    return RECT{x, y, x + w, y + h};
}

} // namespace airscreen
