#pragma once

#include <windows.h>
#include <string>
#include <vector>

namespace airscreen {

enum class UiHit {
    None = 0,
    Name,
    Rename,
    Fullscreen,
    Topmost,
    Pin,
    Laser,
    Dark,
    Update,
};

struct UiView {
    std::wstring name;
    std::wstring status;
    std::wstring pin;
    std::wstring detail;
    bool connected = false;
    bool always_on_top = false;
    bool require_pin = false;
    bool fullscreen = false;
    bool dark = false;
    bool laser = false; // the laser pointer is on
    std::wstring update; // a newer version to offer ("1.4.0"), or empty
    int inset_right = 0;
    bool failed = false;
};

class ShellUi {
public:
    void startup();
    void shutdown();

    HICON icon32() const { return icon32_; }
    HICON icon16() const { return icon16_; }

    void paint(HWND hwnd, HDC hdc, RECT client, const UiView &view, int hover);
    UiHit hit_test(POINT pt) const;
    RECT name_rect() const { return name_rc_; }
    RECT edit_rect() const { return edit_rc_; }

    bool cursor_hand(UiHit hit) const;

    // While idle only two things move (the breathing state dot and the floating logo). The rest is rendered once
    // per size, state and hover into a cached picture, and the animation timer repaints just this rectangle.
    bool animating() const { return animating_; }
    RECT anim_rect(int i) const { return anim_rc_[i & 1]; }

private:
    void ensure_icons();
    void destroy_icons();
    void drop_cache();
    bool ensure_cache(HDC hdc, int w, int h);
    void paint_still(int w, int h, const UiView &view, int hover);

    HICON icon32_ = nullptr;
    HICON icon16_ = nullptr;
    RECT name_rc_{};
    RECT edit_rc_{};
    RECT btn_rc_[5]{};
    RECT dark_rc_{};
    RECT update_rc_{};
    RECT client_{};
    int dpi_ = 96;

    HDC cache_dc_ = nullptr;
    HBITMAP cache_bmp_ = nullptr;
    HGDIOBJ cache_old_ = nullptr;
    void *cache_bits_ = nullptr;
    int cache_w_ = 0, cache_h_ = 0;
    std::wstring cache_key_;
    // The field, the leaves and every shadow: what hovering never changes, kept apart so hover redraws are cheap.
    std::vector<unsigned> base_;
    std::wstring base_key_;

    // The moving parts, placed by the still pass.
    bool animating_ = false;
    RECT anim_rc_[2]{};
    bool breathe_ = false;
    float dot_x_ = 0, dot_y_ = 0, dot_r_ = 0;
    unsigned dot_argb_ = 0;
    bool show_logo_ = false;
    bool float_logo_ = false;
    float logo_x_ = 0, logo_y_ = 0, logo_s_ = 0;
    unsigned glow_argb_ = 0;
};

ULONG_PTR gdiplus_startup();
void gdiplus_shutdown(ULONG_PTR token);

} // namespace airscreen
