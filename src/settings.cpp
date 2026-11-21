#include "settings.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <shlobj.h>
#include <shellapi.h>
#include <algorithm>
#include <cctype>
#include <string>
#include <fstream>
#include <random>
#include <cstdio>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

namespace airscreen {
namespace {

std::string wide_to_utf8(const std::wstring &w) {
    if (w.empty()) {
        return {};
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int) w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int) w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::string trim(std::string s) {
    auto notspace = [](unsigned char c) { return !isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notspace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notspace).base(), s.end());
    return s;
}

} // namespace

std::wstring app_data_dir() {
    wchar_t path[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, path))) {
        return L".";
    }
    std::wstring dir = std::wstring(path) + L"\\AirScreen";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::string app_data_dir_utf8() {
    return wide_to_utf8(app_data_dir());
}

std::string settings_path() {
    return app_data_dir_utf8() + "\\settings.ini";
}

std::string keyfile_path() {
    return app_data_dir_utf8() + "\\airplay.key";
}

std::string log_path() {
    return app_data_dir_utf8() + "\\airscreen.log";
}

void rotate_log() {
    std::wstring path = app_data_dir() + L"\\airscreen.log";
    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) && (fad.nFileSizeHigh || fad.nFileSizeLow > 2000000)) {
        MoveFileExW(path.c_str(), (path + L".old").c_str(), MOVEFILE_REPLACE_EXISTING);
    }
}

const wchar_t *kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

bool start_with_windows() {
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, L"AirScreen", RRF_RT_REG_SZ, nullptr, nullptr, nullptr) ==
           ERROR_SUCCESS;
}

void set_start_with_windows(bool on) {
    if (!on) {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, L"AirScreen");
        return;
    }
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --tray";
    RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, L"AirScreen", REG_SZ, cmd.c_str(),
                    (DWORD) ((cmd.size() + 1) * sizeof(wchar_t)));
}

void append_log(const char *msg) {
    if (!msg) {
        return;
    }
    std::ofstream out(log_path(), std::ios::app);
    if (out) {
        out << msg << '\n';
        out.flush();
    }
}

Settings load_settings() {
    Settings s;
    std::ifstream in(settings_path());
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#' || line[0] == '[') {
            continue;
        }
        auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));
        if (key == "Name" && !val.empty()) {
            s.name = val;
        } else if (key == "AlwaysOnTop") {
            s.always_on_top = (val == "1" || val == "true");
        } else if (key == "RequirePin") {
            s.require_pin = (val == "1" || val == "true");
        } else if (key == "FillScreen") {
            s.fill_screen = (val == "1" || val == "true");
        } else if (key == "DarkMode") {
            s.dark_mode = (val == "1" || val == "true");
        } else if (key == "Width") {
            s.width = atoi(val.c_str()) > 0 ? atoi(val.c_str()) : s.width;
        } else if (key == "Height") {
            s.height = atoi(val.c_str()) > 0 ? atoi(val.c_str()) : s.height;
        } else if (key == "MaxFps") {
            s.max_fps = atoi(val.c_str()) > 0 ? atoi(val.c_str()) : s.max_fps;
        } else if (key == "HighQuality") {
            s.high_quality = (val == "1" || val == "true");
        } else if (key == "Smooth") {
            s.smooth = (val == "1" || val == "true");
        } else if (key == "MicEnabled") {
            s.mic_enabled = (val == "1" || val == "true");
        } else if (key == "MicDevice") {
            s.mic_device = val;
        } else if (key == "CanvasHeight") {
            int v = atoi(val.c_str());
            s.canvas_h = (v == 1080 || v == 1440 || v == 2160) ? v : s.canvas_h;
        } else if (key == "Framed") {
            s.framed = (val == "1" || val == "true");
        } else if (key == "ScreenCaption") {
            s.screen_caption = (val == "1" || val == "true");
        } else if (key == "ScreenText0") {
            s.screen_text[0] = val;
        } else if (key == "ScreenText1") {
            s.screen_text[1] = val;
        } else if (key == "Background") {
            int v = atoi(val.c_str());
            s.background = (v >= 0 && v <= 2) ? v : 0;
        } else if (key == "BackgroundImage") {
            s.background_image = val;
        } else if (key.size() == 10 && key.compare(0, 9, "SceneText") == 0 && key[9] >= '0' && key[9] <= '5') {
            s.scene_text[key[9] - '0'] = val;
        } else if (key == "MicGain") {
            int g = atoi(val.c_str());
            s.mic_gain_pct = g > 0 && g <= 400 ? g : s.mic_gain_pct;
        } else if (key == "Window") {
            sscanf(val.c_str(), "%d,%d,%d,%d", &s.win_x, &s.win_y, &s.win_w, &s.win_h);
        } else if (key == "Profile" && !val.empty()) {
            s.profile = val;
        } else if (key == "PanelAuto") {
            s.panel_auto = atoi(val.c_str()) & 3;
        } else if (key == "TeachMode") {
            s.teach_mode = (val == "1" || val == "true");
        } else if (key == "PanelTopmost") {
            s.panel_topmost = (val == "1" || val == "true");
        } else if (key == "PanelCompact") {
            s.panel_compact = (val == "1" || val == "true");
        } else if (key == "Panel") {
            sscanf(val.c_str(), "%d,%d,%d,%d", &s.panel_x, &s.panel_y, &s.panel_w, &s.panel_h);
        } else if (key == "Editor") {
            sscanf(val.c_str(), "%d,%d,%d,%d", &s.editor_x, &s.editor_y, &s.editor_w, &s.editor_h);
        } else if (key == "HideControls") {
            s.hide_controls = (val == "1" || val == "true");
        } else if (key == "ApplyMode") {
            s.apply_mode = atoi(val.c_str()) == 1 ? 1 : 0;
        } else if (key == "Transition") {
            s.transition = atoi(val.c_str()) == 1 ? 1 : 0;
        } else if (key == "CaptureCursor") {
            s.capture_cursor = (val == "1" || val == "true");
        } else if (key == "CaptureFps") {
            s.capture_fps = atoi(val.c_str()) >= 60 ? 60 : 30;
        } else if (key == "LastSource") {
            s.last_source = val;
        } else if (key == "Quick") {
            s.quick = val;
        } else if (key == "Hotkeys") {
            s.hotkeys = (val == "1" || val == "true");
        } else if (key == "HotkeyMap") {
            s.hotkey_map = val;
        } else if (key == "LaserColor") {
            int v = atoi(val.c_str());
            s.laser_color = v < 0 || v > 3 ? 0 : v;
        } else if (key == "LaserSize") {
            int v = atoi(val.c_str());
            s.laser_size = v < 0 || v > 2 ? 1 : v;
        } else if (key == "CheckUpdates") {
            s.check_updates = (val == "1" || val == "true");
        } else if (key == "UpdateSeen") {
            s.update_seen = val;
        }
    }
    return s;
}

void save_settings(const Settings &s) {
    std::ofstream out(settings_path(), std::ios::trunc);
    out << "[AirScreen]\n";
    out << "Name=" << s.name << "\n";
    out << "AlwaysOnTop=" << (s.always_on_top ? 1 : 0) << "\n";
    out << "RequirePin=" << (s.require_pin ? 1 : 0) << "\n";
    out << "FillScreen=" << (s.fill_screen ? 1 : 0) << "\n";
    out << "DarkMode=" << (s.dark_mode ? 1 : 0) << "\n";
    out << "Width=" << s.width << "\n";
    out << "Height=" << s.height << "\n";
    out << "MaxFps=" << s.max_fps << "\n";
    out << "HighQuality=" << (s.high_quality ? 1 : 0) << "\n";
    out << "Smooth=" << (s.smooth ? 1 : 0) << "\n";
    out << "MicEnabled=" << (s.mic_enabled ? 1 : 0) << "\n";
    out << "MicDevice=" << s.mic_device << "\n";
    out << "MicGain=" << s.mic_gain_pct << "\n";
    out << "CanvasHeight=" << s.canvas_h << "\n";
    out << "Framed=" << (s.framed ? 1 : 0) << "\n";
    out << "ScreenCaption=" << (s.screen_caption ? 1 : 0) << "\n";
    out << "ScreenText0=" << s.screen_text[0] << "\n";
    out << "ScreenText1=" << s.screen_text[1] << "\n";
    out << "Background=" << s.background << "\n";
    out << "BackgroundImage=" << s.background_image << "\n";
    for (int i = 0; i < 6; ++i) {
        out << "SceneText" << i << "=" << s.scene_text[i] << "\n";
    }
    out << "Window=" << s.win_x << "," << s.win_y << "," << s.win_w << "," << s.win_h << "\n";
    out << "Profile=" << s.profile << "\n";
    out << "PanelAuto=" << s.panel_auto << "\n";
    out << "TeachMode=" << (s.teach_mode ? 1 : 0) << "\n";
    out << "PanelTopmost=" << (s.panel_topmost ? 1 : 0) << "\n";
    out << "PanelCompact=" << (s.panel_compact ? 1 : 0) << "\n";
    out << "Panel=" << s.panel_x << "," << s.panel_y << "," << s.panel_w << "," << s.panel_h << "\n";
    out << "Editor=" << s.editor_x << "," << s.editor_y << "," << s.editor_w << "," << s.editor_h << "\n";
    out << "HideControls=" << (s.hide_controls ? 1 : 0) << "\n";
    out << "ApplyMode=" << s.apply_mode << "\n";
    out << "Transition=" << s.transition << "\n";
    out << "CaptureCursor=" << (s.capture_cursor ? 1 : 0) << "\n";
    out << "CaptureFps=" << s.capture_fps << "\n";
    out << "LastSource=" << s.last_source << "\n";
    out << "Quick=" << s.quick << "\n";
    out << "Hotkeys=" << (s.hotkeys ? 1 : 0) << "\n";
    out << "HotkeyMap=" << s.hotkey_map << "\n";
    out << "LaserColor=" << s.laser_color << "\n";
    out << "LaserSize=" << s.laser_size << "\n";
    out << "CheckUpdates=" << (s.check_updates ? 1 : 0) << "\n";
    out << "UpdateSeen=" << s.update_seen << "\n";
}

std::string find_mac_address() {
    ULONG buflen = sizeof(IP_ADAPTER_ADDRESSES);
    auto *addresses = (IP_ADAPTER_ADDRESSES *) malloc(buflen);
    if (!addresses) {
        return {};
    }
    if (GetAdaptersAddresses(AF_UNSPEC, 0, nullptr, addresses, &buflen) == ERROR_BUFFER_OVERFLOW) {
        free(addresses);
        addresses = (IP_ADAPTER_ADDRESSES *) malloc(buflen);
        if (!addresses) {
            return {};
        }
    }
    std::string mac;
    if (GetAdaptersAddresses(AF_UNSPEC, 0, nullptr, addresses, &buflen) == NO_ERROR) {
        for (auto *a = addresses; a; a = a->Next) {
            if (a->PhysicalAddressLength != 6 || a->OperStatus != IfOperStatusUp) {
                continue;
            }
            if (a->IfType != IF_TYPE_ETHERNET_CSMACD && a->IfType != IF_TYPE_IEEE80211) {
                continue;
            }
            char buf[32];
            snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x",
                     a->PhysicalAddress[0], a->PhysicalAddress[1], a->PhysicalAddress[2],
                     a->PhysicalAddress[3], a->PhysicalAddress[4], a->PhysicalAddress[5]);
            mac = buf;
            break;
        }
    }
    free(addresses);
    return mac;
}

std::string random_mac_address() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(0, 255);
    unsigned b0 = ((unsigned) dist(gen) & 0xFEu) | 0x02u; // locally administered unicast
    char buf[32];
    snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x",
             b0, dist(gen), dist(gen), dist(gen), dist(gen), dist(gen));
    return buf;
}

bool ensure_firewall_rule() {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    // Delete first so repeated clicks (or a moved install) leave exactly one rule.
    std::wstring cmd = L"/c netsh advfirewall firewall delete rule name=\"AirScreen\" & "
                       L"netsh advfirewall firewall add rule name=\"AirScreen\" dir=in action=allow program=\"";
    cmd += exe;
    cmd += L"\" enable=yes profile=any";
    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"runas";
    sei.lpFile = L"cmd.exe";
    sei.lpParameters = cmd.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) {
        return false;
    }
    if (sei.hProcess) {
        WaitForSingleObject(sei.hProcess, 15000);
        CloseHandle(sei.hProcess);
    }
    return true;
}

} // namespace airscreen
