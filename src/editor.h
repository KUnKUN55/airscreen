#pragma once

#include "teach.h"

#include <windows.h>
#include <initializer_list>
#include <string>
#include <vector>

namespace airscreen {

// The scene editor, which is also the preview window. The middle shows a live preview drawn by the studio
// itself (so it matches the output exactly): the scene being edited, what is on air, or one screen as it
// arrives. Layers are moved and resized right on it, with snapping guides; the panel on the right shows only
// the settings of the selected layer. Opened as a plain preview, the editing panels stay hidden.
//
// Edits are saved to the profile as they happen. Whether they reach the air at once or wait for Apply is the
// teacher's choice (Settings::apply_mode); a scene that isn't on air is never shown by editing it.
class SceneEditor {
public:
    void open(HINSTANCE inst, TeachHost *host, bool preview_only);
    void close();
    bool is_open() const { return hwnd_ != nullptr; }
    // The scene on air changed, or the list did (from the control panel or a shortcut).
    void live_changed();
    void scenes_changed();
    HWND hwnd() const { return hwnd_; }

private:
    enum Mode { ModeScene = 0, ModeLive = 1, ModeIPad = 2, ModeComputer = 3 };
    enum Drag { DragNone, DragMove, DragResize };
    struct Row {
        unsigned groups;
        HWND label = nullptr;
        std::vector<HWND> ctl;
        int height = 0; // DIP, 0 = default
    };

    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK preview_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK props_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK layers_subclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
    LRESULT preview_msg(UINT msg, WPARAM wp, LPARAM lp);

    void build();
    void layout();
    void layout_props();
    void apply_font(HWND h);
    HWND make(const wchar_t *cls, const wchar_t *text, DWORD style, int id, HWND parent = nullptr);
    void add_row(unsigned groups, const wchar_t *label, std::initializer_list<HWND> ctl, int height = 0);

    Scene &scene();
    Layer *selected();
    void select_scene(int index);
    void select_layer(uint32_t id);
    void fill_scene_list();
    void fill_layer_list();
    void load_props();
    void on_command(int id, int code, HWND ctl);
    void on_scroll(HWND ctl);
    void edited(bool structure);
    void update_preview();
    void update_buttons();
    void set_mode(int mode);
    void add_layer(LayerType type);
    void layer_menu(int which);
    void snippet_menu();
    bool pick_color(uint32_t &argb);
    bool pick_picture(std::wstring &path);
    void scene_op(int id);

    // Preview geometry: the 16:9 canvas inside the preview window.
    RECT view_rect() const;
    bool to_canvas(POINT pt, float &fx, float &fy) const;
    int handle_at(POINT pt) const;
    uint32_t layer_at(float fx, float fy) const;
    void drag_to(POINT pt, bool snap, bool keep_aspect);
    int dpi(int v) const { return MulDiv(v, dpi_, 96); }

    HINSTANCE inst_ = nullptr;
    TeachHost *host_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND preview_ = nullptr;
    HWND props_ = nullptr;
    HWND scenes_ = nullptr;
    HWND layers_ = nullptr;
    HWND scene_combo_ = nullptr;
    HFONT font_ = nullptr;
    int dpi_ = 96;
    bool preview_only_ = false;
    int mode_ = ModeScene;
    int cur_ = 0;          // scene being edited
    uint32_t sel_ = 0;     // selected layer, 0 = the scene itself
    bool loading_ = false; // filling controls: ignore their change notifications
    bool pending_ = false; // edits to the scene on air waiting for Apply
    int scroll_ = 0;       // properties panel scroll, pixels
    int props_h_ = 0;      // properties content height, pixels
    std::vector<Row> rows_;
    std::vector<HWND> left_, right_, bottom_, full_only_, preview_only_ctl_;

    // Dragging on the preview.
    Drag drag_ = DragNone;
    int handle_ = -1;
    float start_fx_ = 0, start_fy_ = 0;
    Layer start_;
    std::vector<float> guide_x_, guide_y_; // snapping guides on show, canvas fractions
};

} // namespace airscreen
