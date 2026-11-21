#pragma once

#include <string>
#include <cstdint>

namespace airscreen {

struct Settings {
    std::string name = "AirScreen";
    bool always_on_top = false;
    bool require_pin = false;
    bool fill_screen = true;
    bool dark_mode = false;
    int pin = 0; // 0 = random each time UxPlay assigns
    int width = 1920;
    int height = 1080;
    int refresh_hz = 60;
    int max_fps = 60;
    bool high_quality = true; // advertise a 4K display and accept HEVC, so iOS mirrors at native sharpness
    bool smooth = false;      // present on vsync with a tiny buffer instead of the instant the frame decodes
    bool mic_enabled = false; // mix a microphone into recorded clips
    std::string mic_device;   // WASAPI endpoint id (UTF-8); empty = Windows default recording device
    int mic_gain_pct = 100;
    // Studio: the 16:9 canvas the window shows and clips record.
    int canvas_h = 1440;  // 1080, 1440 or 2160
    // The fixed scenes of earlier versions. Only read, to seed the first teaching profile with what was typed.
    bool framed = true;
    int background = 0;
    std::string background_image;
    std::string scene_text[6] = {"เริ่มในอีกสักครู่", "", "พักสักครู่", "เดี๋ยวกลับมาต่อ", "ขอบคุณที่รับชม", ""};
    bool screen_caption = false;
    std::string screen_text[2] = {"", ""};
    int win_x = 0, win_y = 0, win_w = 0, win_h = 0; // 0 size = default placement

    // Teaching studio
    std::string profile = "General Presentation"; // scenes in use (UTF-8 profile name)
    int panel_auto = 1;        // open the control panel by itself: 1 when sharing starts, 2 when recording starts
    bool teach_mode = false;   // open the control panel when AirScreen starts
    bool panel_topmost = true; // keep the control panel above other windows
    bool panel_compact = false;
    int panel_x = 0, panel_y = 0, panel_w = 0, panel_h = 0;
    int editor_x = 0, editor_y = 0, editor_w = 0, editor_h = 0;
    bool hide_controls = true; // keep AirScreen's control windows out of screen captures
    int apply_mode = 0;        // editing the scene on air: 0 shows changes at once, 1 waits for Apply
    int transition = 0;        // switching scenes: 0 cross-fade, 1 cut
    bool capture_cursor = true;
    int capture_fps = 30;          // computer screen frame cap: 30 or 60
    std::string last_source;       // "display:2" or "window:<title>": offered first next time
    std::string quick = "ipad,computer,both,text"; // control panel quick buttons, by action name
    bool hotkeys = false;          // shortcuts that work while other apps are in front
    std::string hotkey_map;        // "action:mods:vk,..."; empty = the defaults
    int laser_color = 0;           // laser pointer: 0 red, 1 green, 2 blue, 3 yellow
    int laser_size = 1;            // 0 small, 1 medium, 2 large
    bool check_updates = true;     // ask GitHub now and then whether a newer AirScreen is out
    std::string update_seen;       // the newest version already announced, so each one is announced once
};

std::wstring app_data_dir();
std::string app_data_dir_utf8();
std::string settings_path();
std::string keyfile_path();
std::string log_path();

Settings load_settings();
void save_settings(const Settings &s);
void append_log(const char *msg);
void rotate_log();

bool start_with_windows();
void set_start_with_windows(bool on);

std::string find_mac_address();
std::string random_mac_address();

bool ensure_firewall_rule();

} // namespace airscreen
