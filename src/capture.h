#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

struct ID3D11Device;
struct ID3D11Texture2D;
struct HWND__;
struct HMONITOR__;

namespace airscreen {

// Something on this PC that can be shared: a whole display or one app window.
struct CaptureTarget {
    enum Kind { Display = 0, Window = 1 };
    Kind kind = Display;
    HMONITOR__ *monitor = nullptr;
    HWND__ *window = nullptr;
    int number = 0;       // display number as Windows numbers them (Display)
    bool primary = false; // the main display (Display)
    bool minimised = false; // (Window): shows nothing until restored
    std::wstring name;    // "Display 2 · 2560 × 1440", or the window's title
    std::wstring detail;  // the app's file name (Window)
    int x = 0, y = 0, w = 0, h = 0; // where it is on the desktop
};

std::vector<CaptureTarget> list_displays();
// Top-level app windows a person would recognise, never AirScreen's own.
std::vector<CaptureTarget> list_windows();

// Captures a display or a window with Windows.Graphics.Capture (Windows 10 1903 and later). Windows draws its
// own border around whatever is being captured, so it's always clear what is shared; nothing here gets around
// that or any other system prompt. Frames arrive on a capture thread as textures on the caller's device.
class ScreenCapture {
public:
    // A new picture: `tex` (BGRA) holds it in its top-left `w` x `h`; valid only during the call.
    using FrameFn = std::function<void(ID3D11Texture2D *tex, int w, int h)>;
    // The window was closed or the display went away; no more frames will come.
    using ClosedFn = std::function<void()>;

    ScreenCapture();
    ~ScreenCapture();
    ScreenCapture(const ScreenCapture &) = delete;
    ScreenCapture &operator=(const ScreenCapture &) = delete;

    static bool supported();
    bool start(ID3D11Device *device, const CaptureTarget &target, bool cursor, FrameFn on_frame, ClosedFn on_closed,
               std::string *err);
    // Must not be called while holding a lock that FrameFn takes: it waits for a frame in flight to finish.
    void stop();
    bool running() const { return impl_ != nullptr; }

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace airscreen
