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
    bool framed = true;   // Screen scene: rounded picture with a shadow, rather than full size on black
    int background = 0;   // 0 studio colours, 1 blurred screen, 2 your picture
    std::string background_image; // copy of the chosen picture in %APPDATA%\AirScreen (UTF-8 path)
    std::string scene_text[6] = {"เริ่มในอีกสักครู่", "", "พักสักครู่", "เดี๋ยวกลับมาต่อ", "ขอบคุณที่รับชม", ""};
    // (title, subtitle) for Intro, Break and Outro, UTF-8
    bool screen_caption = false; // a lower-third caption on the Screen scene
    std::string screen_text[2] = {"", ""}; // its (title, subtitle), UTF-8
    int win_x = 0, win_y = 0, win_w = 0, win_h = 0; // 0 size = default placement
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
