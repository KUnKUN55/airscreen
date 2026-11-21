#pragma once

#include <windows.h>
#include <deque>

namespace airscreen {

// An on-screen laser pointer: a glowing dot with a short fading trail that follows the mouse over every
// display, for pointing at things while teaching. It is a click-through window that never takes focus, and
// unlike the control windows it is never hidden from capture: students should see it when a whole display is
// shared (in AirScreen's computer screen, or straight from Zoom, Teams or Meet).
class LaserPointer {
public:
    ~LaserPointer() { destroy(); }
    // color: 0 red, 1 green, 2 blue, 3 yellow; size: 0 small, 1 medium, 2 large.
    void set_style(int color, int size);
    void set_on(HINSTANCE inst, bool on);
    bool on() const { return on_; }
    void destroy();

private:
    struct Point {
        POINT pt;
        DWORD t;
    };

    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    bool create(HINSTANCE inst);
    void tick();
    bool ensure_buffer(int w, int h);

    HWND hwnd_ = nullptr;
    bool on_ = false;
    int color_ = 0;
    int size_ = 1;
    std::deque<Point> trail_; // oldest first; the last one is where the dot is
    bool still_drawn_ = false; // the dot alone is on screen as it is now: nothing to redraw
    int frames_ = 0;

    HDC dc_ = nullptr;
    HBITMAP bmp_ = nullptr;
    HGDIOBJ old_ = nullptr;
    void *bits_ = nullptr;
    int cap_w_ = 0, cap_h_ = 0;
};

} // namespace airscreen
