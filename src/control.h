#pragma once

#include "teach.h"

#include <windows.h>
#include <string>
#include <vector>

namespace airscreen {

// The floating teaching control panel: what's on air, the scenes, a few quick buttons and the recording
// controls, in a small window that can stay on top of the apps being taught with. Closing it only hides it;
// sharing and recording carry on, and it comes back as it was, showing the state at that moment.
class ControlPanel {
public:
    bool create(HINSTANCE inst, TeachHost *host);
    void destroy();
    void show(bool activate);
    void hide();
    void toggle();
    bool visible() const;
    // Repaint from the host's current state (cheap; call on any change).
    void refresh();
    // Re-reads always-on-top, compact view and capture hiding from settings.
    void apply_prefs();
    HWND hwnd() const { return hwnd_; }

private:
    enum Style { Chip, Toggle, Danger, Plain, Laser };
    struct Button {
        int id = 0;
        RECT rc{};
        std::wstring label;
        std::wstring tip;
        Style style = Plain;
        bool on = false;
        bool enabled = true;
    };

    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
    void layout(HDC hdc, int width);
    void paint(HDC hdc);
    void click(int id);
    void more_menu();
    void save_rect();
    int hit(POINT pt) const;
    void sync_tips();
    std::vector<Action> quick_actions() const;
    bool action_lit(Action a, const Scene &s, const TeachStatus &st) const;
    int dpi(int v) const { return MulDiv(v, dpi_, 96); }

    HINSTANCE inst_ = nullptr;
    TeachHost *host_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND tips_ = nullptr;
    int dpi_ = 96;
    std::vector<Button> buttons_;
    std::vector<std::pair<int, RECT>> tip_rects_; // what the tooltip control currently knows
    int hot_ = -1;
    int pressed_ = -1;
    int focus_ = -1;
    bool keyboard_ = false; // show the focus ring only once the keyboard is used
    int content_h_ = 0;
    RECT status_rc_{};
    std::vector<std::pair<POINT, std::wstring>> labels_; // section headings
    void fit_height();
    bool compact_ = false; // the layout the window was last sized for
};

} // namespace airscreen
