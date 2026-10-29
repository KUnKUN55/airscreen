#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace airscreen {

struct Settings;

// A teaching scene: a backdrop and a stack of layers on the 16:9 canvas. Positions and sizes are fractions of
// the canvas (0..1) and text sizes fractions of its height, so a scene looks the same at 1080p, 1440p or 4K
// and in the editor's small preview.
enum class LayerType { Source = 0, Text = 1, Image = 2, Frame = 3 };
enum class SourceKind { IPad = 0, Computer = 1 };
// Behind a text layer (or filling a frame): nothing, an opaque plate, an outline only, a see-through tint,
// frosted glass (whatever is behind it, blurred), or a two-colour gradient.
enum class BoxStyle { None = 0, Solid = 1, Outline = 2, Translucent = 3, Blur = 4, Gradient = 5 };
enum class Backdrop { Colors = 0, Blur = 1, Image = 2, Solid = 3 };

struct Layer {
    uint32_t id = 0;
    LayerType type = LayerType::Text;
    std::wstring name;
    bool visible = true;
    bool locked = false;
    float x = 0, y = 0, w = 1, h = 1;
    float opacity = 1;

    // Source
    SourceKind source = SourceKind::IPad;
    bool fill = false;   // crop to fill the box rather than fit inside it
    bool rounded = true; // rounded corners and a soft shadow

    // Text
    std::wstring text;
    float font = 0.05f;          // line height, fraction of canvas height (shrinks to fit the box)
    uint32_t color = 0xffffffff; // ARGB, also the border of a frame
    bool bold = true;
    int align = 0; // 0 left, 1 centre, 2 right

    // Box behind text, or a frame's fill
    BoxStyle box = BoxStyle::Translucent;
    bool box_on = true; // the quick on/off switch; keeps the style
    uint32_t box_color = 0xff0a0c14;
    uint32_t box_color2 = 0xff1d4ed8;
    float box_opacity = 0.7f; // 0..1
    float blur = 0.5f;        // 0..1
    float radius = 0.014f;    // corners, fraction of canvas height
    float padding = 0.02f;    // text inset, fraction of canvas height

    // Image
    std::wstring path;

    // Frame
    float thickness = 0.004f; // border, fraction of canvas height

    bool has_box() const { return box_on && box != BoxStyle::None; }
};

struct Scene {
    uint32_t id = 0;
    std::wstring name;
    Backdrop backdrop = Backdrop::Colors;
    int mood = 0; // Colors: 0 night, 1 blue, 2 amber, 3 green
    uint32_t bg_color = 0xff000000;
    std::wstring bg_image;
    std::vector<Layer> layers; // back to front

    Layer *find(uint32_t layer_id);
    const Layer *find(uint32_t layer_id) const;
    // True when a layer of this kind is showing: what the control panel lights up.
    bool shows(SourceKind kind) const;
    bool shows_text() const;
    bool shows_box() const;
    bool shows_extras() const;
    bool has_secondary() const;
};

// Text saved for reuse: lesson titles, break notices and so on.
struct Snippet {
    std::wstring name;
    std::wstring text;
};

// A teaching profile: its own scenes and saved texts (Chemistry, Mathematics, ...), one file each.
struct Profile {
    std::wstring name;
    std::vector<Scene> scenes;
    uint32_t default_scene = 0; // scene id shown when the app starts
    std::vector<Snippet> snippets;
    uint32_t next_id = 1;

    uint32_t new_id() { return next_id++; }
    int index_of(uint32_t scene_id) const;
    int default_index() const;
};

std::wstring profiles_dir();
std::vector<std::wstring> list_profiles();
bool load_profile(const std::wstring &name, Profile &out, std::string *err);
// Written to a temporary file and swapped in, so a failure never leaves a half-written profile behind.
bool save_profile(const Profile &p, std::string *err);
bool delete_profile(const std::wstring &name);
// Renames a profile file that can't be read to "<file>.unreadable", so it's kept but out of the way.
bool set_aside_profile(const std::wstring &name);
bool profile_exists(const std::wstring &name);

// The sample scenes (iPad teaching, presentation, dual screen, title, minimal, break), seeded with the text
// from the old fixed scenes in `legacy` so an upgrade keeps what was typed there.
Profile default_profile(const std::wstring &name, const Settings &legacy);

Layer make_layer(LayerType type, Profile &p);
Scene make_scene(const std::wstring &name, Profile &p);
// A copy with fresh ids, so the copy and the original never share cached pictures by accident.
Scene copy_scene(const Scene &s, Profile &p);

// Quick actions on a scene's screens: the bottom-most source layer is the main screen, the others secondary.
void show_only(Scene &s, SourceKind kind);
void show_both(Scene &s, Profile &p);
void swap_screens(Scene &s);
void toggle_secondary(Scene &s);
enum class DualLayout { SideBySide, PipTopLeft, PipTopRight, PipBottomLeft, PipBottomRight };
void dual_layout(Scene &s, Profile &p, DualLayout layout);
void toggle_text(Scene &s);
void set_text_visible(Scene &s, bool on);
void toggle_box(Scene &s);
void toggle_extras(Scene &s);
// The first text layer, made visible if `show`; null when the scene has none.
Layer *main_text(Scene &s);

enum class Align { Center, TopLeft, TopRight, BottomLeft, BottomRight, Fill, Top, Bottom };
void align_layer(Layer &l, Align a);

} // namespace airscreen
