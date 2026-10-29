#include "scene.h"
#include "settings.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace airscreen {
namespace {

std::string utf8(const std::wstring &w) {
    if (w.empty()) {
        return {};
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int) w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int) w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring utf16(const std::string &s) {
    if (s.empty()) {
        return {};
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int) s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int) s.size(), w.data(), n);
    return w;
}

// One value per line: backslashes and line breaks are escaped so multi-line text survives.
std::string esc(const std::wstring &w) {
    std::string s = utf8(w), out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\\') {
            out += "\\\\";
        } else if (c == '\n') {
            out += "\\n";
        } else if (c != '\r') {
            out += c;
        }
    }
    return out;
}

std::wstring unesc(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            ++i;
            out += s[i] == 'n' ? '\n' : s[i];
        } else {
            out += s[i];
        }
    }
    return utf16(out);
}

std::string hex(uint32_t v) {
    char b[16];
    snprintf(b, sizeof(b), "%08x", v);
    return b;
}

float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

std::wstring file_for(const std::wstring &name) {
    std::wstring safe;
    for (wchar_t c : name) {
        safe += (c < 32 || wcschr(L"<>:\"/\\|?*", c)) ? L'_' : c;
    }
    if (safe.empty()) {
        safe = L"Profile";
    }
    return profiles_dir() + L"\\" + safe + L".scenes";
}

const char *type_name(LayerType t) {
    switch (t) {
    case LayerType::Source:
        return "source";
    case LayerType::Image:
        return "image";
    case LayerType::Frame:
        return "frame";
    default:
        return "text";
    }
}

LayerType type_from(const std::string &s) {
    if (s == "source") {
        return LayerType::Source;
    }
    if (s == "image") {
        return LayerType::Image;
    }
    if (s == "frame") {
        return LayerType::Frame;
    }
    return LayerType::Text;
}

Layer *first_source(Scene &s) {
    for (auto &l : s.layers) {
        if (l.type == LayerType::Source) {
            return &l;
        }
    }
    return nullptr;
}

} // namespace

Layer *Scene::find(uint32_t layer_id) {
    for (auto &l : layers) {
        if (l.id == layer_id) {
            return &l;
        }
    }
    return nullptr;
}

const Layer *Scene::find(uint32_t layer_id) const {
    return const_cast<Scene *>(this)->find(layer_id);
}

bool Scene::shows(SourceKind kind) const {
    for (auto &l : layers) {
        if (l.type == LayerType::Source && l.visible && l.source == kind) {
            return true;
        }
    }
    return false;
}

bool Scene::shows_text() const {
    for (auto &l : layers) {
        if (l.type == LayerType::Text && l.visible) {
            return true;
        }
    }
    return false;
}

bool Scene::shows_box() const {
    for (auto &l : layers) {
        if (l.type == LayerType::Text && l.box != BoxStyle::None && l.box_on) {
            return true;
        }
    }
    return false;
}

bool Scene::shows_extras() const {
    for (auto &l : layers) {
        if ((l.type == LayerType::Image || l.type == LayerType::Frame) && l.visible) {
            return true;
        }
    }
    return false;
}

bool Scene::has_secondary() const {
    int n = 0;
    for (auto &l : layers) {
        n += l.type == LayerType::Source;
    }
    return n > 1;
}

int Profile::index_of(uint32_t scene_id) const {
    for (size_t i = 0; i < scenes.size(); ++i) {
        if (scenes[i].id == scene_id) {
            return (int) i;
        }
    }
    return -1;
}

int Profile::default_index() const {
    int i = index_of(default_scene);
    return i >= 0 ? i : 0;
}

std::wstring profiles_dir() {
    std::wstring dir = app_data_dir() + L"\\profiles";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::vector<std::wstring> list_profiles() {
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((profiles_dir() + L"\\*.scenes").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return names;
    }
    do {
        // The name inside the file is the real one; the file name has unsafe characters replaced.
        std::ifstream in(profiles_dir() + L"\\" + fd.cFileName);
        std::string line;
        std::wstring name;
        for (int i = 0; i < 4 && std::getline(in, line); ++i) {
            if (line.compare(0, 8, "Profile=") == 0) {
                name = unesc(line.substr(8));
                break;
            }
        }
        if (!name.empty()) {
            names.push_back(name);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(names.begin(), names.end());
    return names;
}

bool profile_exists(const std::wstring &name) {
    return GetFileAttributesW(file_for(name).c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool delete_profile(const std::wstring &name) {
    return DeleteFileW(file_for(name).c_str()) != FALSE;
}

bool set_aside_profile(const std::wstring &name) {
    std::wstring f = file_for(name);
    return MoveFileExW(f.c_str(), (f + L".unreadable").c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
}

bool save_profile(const Profile &p, std::string *err) {
    std::ostringstream o;
    o << "AirScreenScenes=1\n";
    o << "Profile=" << esc(p.name) << "\n";
    o << "DefaultScene=" << p.default_scene << "\n";
    o << "NextId=" << p.next_id << "\n";
    for (auto &sn : p.snippets) {
        o << "[Snippet]\nName=" << esc(sn.name) << "\nText=" << esc(sn.text) << "\n";
    }
    for (auto &s : p.scenes) {
        o << "[Scene]\n";
        o << "Id=" << s.id << "\nName=" << esc(s.name) << "\n";
        o << "Backdrop=" << (int) s.backdrop << "\nMood=" << s.mood << "\nBgColor=" << hex(s.bg_color) << "\n";
        o << "BgImage=" << esc(s.bg_image) << "\n";
        for (auto &l : s.layers) {
            char rect[96];
            snprintf(rect, sizeof(rect), "%.5f,%.5f,%.5f,%.5f", l.x, l.y, l.w, l.h);
            o << "[Layer]\n";
            o << "Id=" << l.id << "\nType=" << type_name(l.type) << "\nName=" << esc(l.name) << "\n";
            o << "Visible=" << (l.visible ? 1 : 0) << "\nLocked=" << (l.locked ? 1 : 0) << "\n";
            o << "Rect=" << rect << "\nOpacity=" << l.opacity << "\n";
            switch (l.type) {
            case LayerType::Source:
                o << "Source=" << (l.source == SourceKind::Computer ? "computer" : "ipad") << "\n";
                o << "Fill=" << (l.fill ? 1 : 0) << "\nRounded=" << (l.rounded ? 1 : 0) << "\n";
                break;
            case LayerType::Image:
                o << "Path=" << esc(l.path) << "\n";
                break;
            case LayerType::Text:
                o << "Text=" << esc(l.text) << "\nFont=" << l.font << "\nBold=" << (l.bold ? 1 : 0) << "\n";
                o << "Align=" << l.align << "\nPadding=" << l.padding << "\n";
                [[fallthrough]];
            case LayerType::Frame:
                o << "Color=" << hex(l.color) << "\nBox=" << (int) l.box << "\nBoxOn=" << (l.box_on ? 1 : 0) << "\n";
                o << "BoxColor=" << hex(l.box_color) << "\nBoxColor2=" << hex(l.box_color2) << "\n";
                o << "BoxOpacity=" << l.box_opacity << "\nBlur=" << l.blur << "\nRadius=" << l.radius << "\n";
                o << "Thickness=" << l.thickness << "\n";
                break;
            }
        }
    }
    std::wstring path = file_for(p.name);
    std::wstring tmp = path + L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        std::string data = o.str();
        out.write(data.data(), (std::streamsize) data.size());
        out.flush();
        if (!out) {
            if (err) {
                *err = "could not write " + utf8(tmp);
            }
            DeleteFileW(tmp.c_str());
            return false;
        }
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        if (err) {
            char b[64];
            snprintf(b, sizeof(b), " (error %lu)", GetLastError());
            *err = "could not replace " + utf8(path) + b;
        }
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

bool load_profile(const std::wstring &name, Profile &out, std::string *err) {
    std::ifstream in(file_for(name), std::ios::binary);
    if (!in) {
        if (err) {
            *err = "no profile file";
        }
        return false;
    }
    Profile p;
    p.name = name;
    Scene *scene = nullptr;
    Layer *layer = nullptr;
    Snippet *snip = nullptr;
    std::string line;
    bool magic = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line == "[Scene]") {
            p.scenes.emplace_back();
            scene = &p.scenes.back();
            layer = nullptr;
            snip = nullptr;
            continue;
        }
        if (line == "[Layer]") {
            if (scene) {
                scene->layers.emplace_back();
                layer = &scene->layers.back();
            }
            continue;
        }
        if (line == "[Snippet]") {
            p.snippets.emplace_back();
            snip = &p.snippets.back();
            scene = nullptr;
            layer = nullptr;
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        float f = (float) atof(v.c_str());
        int i = atoi(v.c_str());
        uint32_t c = (uint32_t) strtoul(v.c_str(), nullptr, 16);
        if (layer) {
            Layer &l = *layer;
            if (k == "Id") {
                l.id = (uint32_t) strtoul(v.c_str(), nullptr, 10);
            } else if (k == "Type") {
                l.type = type_from(v);
            } else if (k == "Name") {
                l.name = unesc(v);
            } else if (k == "Visible") {
                l.visible = i != 0;
            } else if (k == "Locked") {
                l.locked = i != 0;
            } else if (k == "Rect") {
                sscanf(v.c_str(), "%f,%f,%f,%f", &l.x, &l.y, &l.w, &l.h);
            } else if (k == "Opacity") {
                l.opacity = clampf(f, 0, 1);
            } else if (k == "Source") {
                l.source = v == "computer" ? SourceKind::Computer : SourceKind::IPad;
            } else if (k == "Fill") {
                l.fill = i != 0;
            } else if (k == "Rounded") {
                l.rounded = i != 0;
            } else if (k == "Path") {
                l.path = unesc(v);
            } else if (k == "Text") {
                l.text = unesc(v);
            } else if (k == "Font") {
                l.font = clampf(f, 0.01f, 0.4f);
            } else if (k == "Bold") {
                l.bold = i != 0;
            } else if (k == "Align") {
                l.align = std::clamp(i, 0, 2);
            } else if (k == "Padding") {
                l.padding = clampf(f, 0, 0.2f);
            } else if (k == "Color") {
                l.color = c;
            } else if (k == "Box") {
                l.box = (BoxStyle) std::clamp(i, 0, 5);
            } else if (k == "BoxOn") {
                l.box_on = i != 0;
            } else if (k == "BoxColor") {
                l.box_color = c;
            } else if (k == "BoxColor2") {
                l.box_color2 = c;
            } else if (k == "BoxOpacity") {
                l.box_opacity = clampf(f, 0, 1);
            } else if (k == "Blur") {
                l.blur = clampf(f, 0, 1);
            } else if (k == "Radius") {
                l.radius = clampf(f, 0, 0.5f);
            } else if (k == "Thickness") {
                l.thickness = clampf(f, 0, 0.1f);
            }
        } else if (scene) {
            Scene &s = *scene;
            if (k == "Id") {
                s.id = (uint32_t) strtoul(v.c_str(), nullptr, 10);
            } else if (k == "Name") {
                s.name = unesc(v);
            } else if (k == "Backdrop") {
                s.backdrop = (Backdrop) std::clamp(i, 0, 3);
            } else if (k == "Mood") {
                s.mood = std::clamp(i, 0, 3);
            } else if (k == "BgColor") {
                s.bg_color = c;
            } else if (k == "BgImage") {
                s.bg_image = unesc(v);
            }
        } else if (snip) {
            if (k == "Name") {
                snip->name = unesc(v);
            } else if (k == "Text") {
                snip->text = unesc(v);
            }
        } else if (k == "AirScreenScenes") {
            magic = true;
        } else if (k == "Profile") {
            p.name = unesc(v);
        } else if (k == "DefaultScene") {
            p.default_scene = (uint32_t) strtoul(v.c_str(), nullptr, 10);
        } else if (k == "NextId") {
            p.next_id = (uint32_t) strtoul(v.c_str(), nullptr, 10);
        }
    }
    if (!magic || p.scenes.empty()) {
        if (err) {
            *err = magic ? "the profile has no scenes" : "not a scenes file";
        }
        return false;
    }
    // Keep ids unique and next_id past all of them, whatever a hand edit did to the file.
    uint32_t top = 0;
    for (auto &s : p.scenes) {
        top = (std::max)(top, s.id);
        for (auto &l : s.layers) {
            top = (std::max)(top, l.id);
            l.w = clampf(l.w, 0.01f, 4.f);
            l.h = clampf(l.h, 0.01f, 4.f);
        }
    }
    p.next_id = (std::max)(p.next_id, top + 1);
    for (auto &s : p.scenes) {
        if (s.id == 0) {
            s.id = p.new_id();
        }
        for (auto &l : s.layers) {
            if (l.id == 0) {
                l.id = p.new_id();
            }
        }
    }
    out = std::move(p);
    return true;
}

Layer make_layer(LayerType type, Profile &p) {
    Layer l;
    l.id = p.new_id();
    l.type = type;
    switch (type) {
    case LayerType::Source:
        l.name = L"iPad screen";
        l.x = 0.04f, l.y = 0.05f, l.w = 0.92f, l.h = 0.90f;
        break;
    case LayerType::Text:
        l.name = L"Text";
        l.text = L"ข้อความ / Text";
        l.x = 0.04f, l.y = 0.04f, l.w = 0.5f, l.h = 0.11f;
        break;
    case LayerType::Image:
        l.name = L"Logo";
        l.x = 0.84f, l.y = 0.04f, l.w = 0.12f, l.h = 0.12f;
        break;
    case LayerType::Frame:
        l.name = L"Frame";
        l.x = 0.1f, l.y = 0.1f, l.w = 0.3f, l.h = 0.3f;
        l.box = BoxStyle::None;
        l.radius = 0.02f;
        break;
    }
    return l;
}

Scene make_scene(const std::wstring &name, Profile &p) {
    Scene s;
    s.id = p.new_id();
    s.name = name;
    s.layers.push_back(make_layer(LayerType::Source, p));
    return s;
}

Scene copy_scene(const Scene &src, Profile &p) {
    Scene s = src;
    s.id = p.new_id();
    for (auto &l : s.layers) {
        l.id = p.new_id();
    }
    return s;
}

Profile default_profile(const std::wstring &name, const Settings &legacy) {
    Profile p;
    p.name = name;
    auto w = [](const std::string &s) { return utf16(s); };
    std::wstring title = legacy.screen_text[0].empty() ? L"หัวข้อบทเรียน" : w(legacy.screen_text[0]);
    if (!legacy.screen_text[1].empty()) {
        title += L"\n" + w(legacy.screen_text[1]);
    }
    int mood = legacy.background == 1 ? -1 : 0; // the old "blurred screen" choice carries over
    auto backdrop = [&](Scene &s) {
        if (mood < 0) {
            s.backdrop = Backdrop::Blur;
        }
        if (legacy.background == 2 && !legacy.background_image.empty()) {
            s.backdrop = Backdrop::Image;
            s.bg_image = w(legacy.background_image);
        }
    };
    auto screen = [&](SourceKind kind, float x, float y, float sw, float sh) {
        Layer l = make_layer(LayerType::Source, p);
        l.source = kind;
        l.name = kind == SourceKind::IPad ? L"iPad screen" : L"Computer screen";
        l.x = x, l.y = y, l.w = sw, l.h = sh;
        l.rounded = legacy.framed;
        return l;
    };
    auto title_layer = [&](bool visible) {
        Layer t = make_layer(LayerType::Text, p);
        t.name = L"Lesson title";
        t.text = title;
        t.visible = visible;
        t.x = 0.04f, t.y = 0.035f, t.w = 0.92f, t.h = 0.105f;
        t.font = 0.052f;
        return t;
    };

    Scene ipad;
    ipad.id = p.new_id();
    ipad.name = L"iPad Teaching";
    backdrop(ipad);
    ipad.layers.push_back(screen(SourceKind::IPad, 0.04f, 0.05f, 0.92f, 0.90f));
    ipad.layers.push_back(title_layer(false));
    p.scenes.push_back(ipad);

    Scene comp;
    comp.id = p.new_id();
    comp.name = L"Computer Presentation";
    backdrop(comp);
    comp.layers.push_back(screen(SourceKind::Computer, 0.04f, 0.05f, 0.92f, 0.90f));
    comp.layers.push_back(title_layer(false));
    p.scenes.push_back(comp);

    Scene dual;
    dual.id = p.new_id();
    dual.name = L"Dual Screen";
    backdrop(dual);
    dual.layers.push_back(screen(SourceKind::Computer, 0.025f, 0.10f, 0.60f, 0.80f));
    dual.layers.push_back(screen(SourceKind::IPad, 0.645f, 0.10f, 0.33f, 0.80f));
    dual.layers.push_back(title_layer(false));
    p.scenes.push_back(dual);

    Scene titled;
    titled.id = p.new_id();
    titled.name = L"Title & Teaching";
    backdrop(titled);
    titled.layers.push_back(screen(SourceKind::IPad, 0.04f, 0.17f, 0.92f, 0.79f));
    titled.layers.push_back(title_layer(true));
    p.scenes.push_back(titled);

    Scene minimal;
    minimal.id = p.new_id();
    minimal.name = L"Minimal";
    minimal.backdrop = Backdrop::Solid;
    Layer full = screen(SourceKind::IPad, 0, 0, 1, 1);
    full.rounded = false;
    minimal.layers.push_back(full);
    p.scenes.push_back(minimal);

    Scene brk;
    brk.id = p.new_id();
    brk.name = L"Break";
    brk.mood = 2;
    backdrop(brk);
    if (brk.backdrop == Backdrop::Blur) {
        brk.backdrop = Backdrop::Colors; // nothing to blur on a title card
    }
    Layer bt = make_layer(LayerType::Text, p);
    bt.name = L"Break notice";
    bt.text = w(legacy.scene_text[2]);
    if (!legacy.scene_text[3].empty()) {
        bt.text += L"\n" + w(legacy.scene_text[3]);
    }
    if (bt.text.empty()) {
        bt.text = L"พักสักครู่";
    }
    bt.x = 0.08f, bt.y = 0.3f, bt.w = 0.84f, bt.h = 0.4f;
    bt.font = 0.085f;
    bt.align = 1;
    bt.box = BoxStyle::None;
    brk.layers.push_back(bt);
    p.scenes.push_back(brk);

    p.default_scene = ipad.id;

    auto snippet = [&](const wchar_t *n, std::wstring t) {
        if (!t.empty()) {
            p.snippets.push_back({n, t});
        }
    };
    snippet(L"Lesson title", title);
    snippet(L"Subtopic", L"หัวข้อย่อย");
    snippet(L"Note", L"หมายเหตุ: ");
    snippet(L"Break", L"พักสักครู่ 10 นาที");
    std::wstring start = w(legacy.scene_text[0]);
    snippet(L"Starting", start.empty() ? L"เริ่มในอีกสักครู่" : start);
    std::wstring end = w(legacy.scene_text[4]);
    snippet(L"Ending", end.empty() ? L"ขอบคุณที่รับชม" : end);
    return p;
}

void show_only(Scene &s, SourceKind kind) {
    Layer *main = first_source(s);
    if (!main) {
        return;
    }
    main->source = kind;
    main->name = kind == SourceKind::IPad ? L"iPad screen" : L"Computer screen";
    main->visible = true;
    for (auto &l : s.layers) {
        if (l.type == LayerType::Source && &l != main) {
            l.visible = false;
        }
    }
}

void show_both(Scene &s, Profile &p) {
    Layer *main = first_source(s);
    if (!main) {
        return;
    }
    if (!s.has_secondary()) {
        dual_layout(s, p, DualLayout::PipBottomRight);
        return;
    }
    main->visible = true;
    SourceKind other = main->source == SourceKind::IPad ? SourceKind::Computer : SourceKind::IPad;
    bool seen = false;
    for (auto &l : s.layers) {
        if (l.type == LayerType::Source && &l != main) {
            l.visible = true;
            if (!seen && l.source == main->source) {
                l.source = other; // two copies of one screen is never what "both" means
            }
            seen = true;
        }
    }
}

void swap_screens(Scene &s) {
    Layer *main = first_source(s);
    if (!main) {
        return;
    }
    for (auto &l : s.layers) {
        if (l.type == LayerType::Source && &l != main) {
            std::swap(main->source, l.source);
            std::swap(main->name, l.name);
            break;
        }
    }
}

void toggle_secondary(Scene &s) {
    Layer *main = first_source(s);
    bool any = false;
    for (auto &l : s.layers) {
        if (l.type == LayerType::Source && &l != main && l.visible) {
            any = true;
        }
    }
    for (auto &l : s.layers) {
        if (l.type == LayerType::Source && &l != main) {
            l.visible = !any;
        }
    }
}

void dual_layout(Scene &s, Profile &p, DualLayout layout) {
    Layer *main = first_source(s);
    if (!main) {
        s.layers.insert(s.layers.begin(), make_layer(LayerType::Source, p));
        main = &s.layers.front();
    }
    uint32_t main_id = main->id;
    Layer *second = nullptr;
    for (auto &l : s.layers) {
        if (l.type == LayerType::Source && l.id != main_id) {
            second = &l;
            break;
        }
    }
    if (!second) {
        Layer l = make_layer(LayerType::Source, p);
        l.source = main->source == SourceKind::IPad ? SourceKind::Computer : SourceKind::IPad;
        l.name = l.source == SourceKind::IPad ? L"iPad screen" : L"Computer screen";
        // Right above the main screen, under any text.
        size_t at = 0;
        for (size_t i = 0; i < s.layers.size(); ++i) {
            if (s.layers[i].id == main_id) {
                at = i + 1;
            }
        }
        s.layers.insert(s.layers.begin() + (ptrdiff_t) at, l);
        main = s.find(main_id);
        second = &s.layers[at];
    }
    main->visible = second->visible = true;
    const float m = 0.03f;
    if (layout == DualLayout::SideBySide) {
        float wv = (1 - m * 3) / 2;
        main->x = m, main->y = 0.12f, main->w = wv, main->h = 0.76f;
        second->x = m * 2 + wv, second->y = 0.12f, second->w = wv, second->h = 0.76f;
        return;
    }
    main->x = m, main->y = 0.05f, main->w = 1 - m * 2, main->h = 0.90f;
    float pw = 0.30f, ph = 0.34f;
    bool left = layout == DualLayout::PipTopLeft || layout == DualLayout::PipBottomLeft;
    bool top = layout == DualLayout::PipTopLeft || layout == DualLayout::PipTopRight;
    second->w = pw, second->h = ph;
    second->x = left ? m * 1.5f : 1 - m * 1.5f - pw;
    second->y = top ? 0.07f : 1 - 0.07f - ph;
}

void set_text_visible(Scene &s, bool on) {
    for (auto &l : s.layers) {
        if (l.type == LayerType::Text) {
            l.visible = on;
        }
    }
}

void toggle_text(Scene &s) {
    set_text_visible(s, !s.shows_text());
}

void toggle_box(Scene &s) {
    bool on = !s.shows_box();
    for (auto &l : s.layers) {
        if (l.type == LayerType::Text) {
            l.box_on = on;
            if (on && l.box == BoxStyle::None) {
                l.box = BoxStyle::Translucent;
            }
        }
    }
}

void toggle_extras(Scene &s) {
    bool on = !s.shows_extras();
    for (auto &l : s.layers) {
        if (l.type == LayerType::Image || l.type == LayerType::Frame) {
            l.visible = on;
        }
    }
}

Layer *main_text(Scene &s) {
    for (auto &l : s.layers) {
        if (l.type == LayerType::Text) {
            return &l;
        }
    }
    return nullptr;
}

void align_layer(Layer &l, Align a) {
    const float m = 0.03f;
    switch (a) {
    case Align::Center:
        l.x = (1 - l.w) / 2;
        l.y = (1 - l.h) / 2;
        break;
    case Align::TopLeft:
        l.x = m, l.y = m;
        break;
    case Align::TopRight:
        l.x = 1 - m - l.w, l.y = m;
        break;
    case Align::BottomLeft:
        l.x = m, l.y = 1 - m - l.h;
        break;
    case Align::BottomRight:
        l.x = 1 - m - l.w, l.y = 1 - m - l.h;
        break;
    case Align::Top:
        l.x = (1 - l.w) / 2, l.y = m;
        break;
    case Align::Bottom:
        l.x = (1 - l.w) / 2, l.y = 1 - m - l.h;
        break;
    case Align::Fill:
        l.x = 0, l.y = 0, l.w = 1, l.h = 1;
        break;
    }
}

} // namespace airscreen
