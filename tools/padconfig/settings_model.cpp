#include "settings_model.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>

#include "gekko/display_geometry.h"

namespace padconfig {

namespace cf = gekko::config_file;
namespace sp = shadow::peripheral;

namespace {

// ---- display.cfg as gekko/display.cpp parse_display_file reads it -------------------------------------------------

std::string trim_copy(const std::string& text) {
    std::size_t begin = 0, end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    return text.substr(begin, end - begin);
}

std::string lower_copy(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

bool display_bool(const std::string& text, bool* value) {
    const std::string v = lower_copy(text);
    if (v == "1" || v == "true" || v == "yes" || v == "on") return *value = true, true;
    if (v == "0" || v == "false" || v == "no" || v == "off") return *value = false, true;
    return false;
}

bool display_int(const std::string& text, int* value) {
    if (text.empty()) return false;
    char* end = nullptr;
    const long parsed = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0') return false;
    *value = static_cast<int>(parsed);
    return true;
}

bool display_float(const std::string& text, float* value) {
    if (text.empty()) return false;
    char* end = nullptr;
    const float parsed = std::strtof(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0' || !std::isfinite(parsed)) return false;
    *value = parsed;
    return true;
}

const char* spelled(bool v) { return v ? "true" : "false"; }

std::string samples(int n) { return n <= 1 ? "off" : std::to_string(n); }

// ---- options.cfg as gekko/pc_settings.cpp reads it ----------------------------------------------------------------

bool options_int(const std::string& text, int& out) {
    char* end = nullptr;
    const long v = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0') return false;
    out = static_cast<int>(v);
    return true;
}

bool options_bool(const std::string& text, bool& out) {
    if (text == "1" || text == "true" || text == "on" || text == "yes") return out = true, true;
    if (text == "0" || text == "false" || text == "off" || text == "no") return out = false, true;
    return false;
}

bool options_valid(const OptionPrefs& s) {
    return check_options(s).empty();
}

}  // namespace

// ---- shared -------------------------------------------------------------------------------------------------------

std::string format_number(double value) {
    if (std::fabs(value) < 1e15 && value == std::floor(value)) return std::to_string(static_cast<long long>(value));
    std::string spelled_value;
    for (int digits = 1; digits <= 17; ++digits) {
        std::ostringstream out;
        out.precision(digits);
        out << value;
        spelled_value = out.str();
        if (std::strtod(spelled_value.c_str(), nullptr) == value) break;
    }
    return spelled_value;
}

std::string remove_keys(const std::string& text, const std::vector<std::string>& keys) {
    if (keys.empty()) return text;
    std::string out;
    out.reserve(text.size());
    std::size_t begin = 0;
    while (begin < text.size()) {
        std::size_t end = text.find('\n', begin);
        const std::size_t next = end == std::string::npos ? text.size() : end + 1;
        if (end == std::string::npos) end = text.size();
        std::string body = text.substr(begin, end - begin);
        if (begin == 0 && body.compare(0, 3, "\xEF\xBB\xBF") == 0) body.erase(0, 3);
        const std::size_t hash = body.find('#');
        if (hash != std::string::npos) body.resize(hash);
        const std::size_t eq = body.find('=');
        bool drop = false;
        if (eq != std::string::npos) {
            const std::string key = lower_copy(trim_copy(body.substr(0, eq)));
            drop = std::find(keys.begin(), keys.end(), key) != keys.end();
        }
        if (drop && begin == 0 && text.compare(0, 3, "\xEF\xBB\xBF") == 0) out.append("\xEF\xBB\xBF");
        if (!drop) out.append(text, begin, next - begin);
        begin = next;
    }
    return out;
}

bool read_file(const std::filesystem::path& path, std::string& text, bool* exists, std::string* error) {
    text.clear();
    std::error_code ec;
    const bool there = std::filesystem::exists(path, ec);
    if (exists != nullptr) *exists = there;
    if (!there) return true;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error != nullptr) *error = path.filename().string() + " cannot be read. Is it open in another program?";
        return false;
    }
    text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool write_file(const std::filesystem::path& path, const std::string& text, std::string* error) {
    const std::string name = path.filename().string();
    std::filesystem::path temporary = path;
    temporary += ".new";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.flush();
        if (!out) {
            std::error_code ec;
            out.close();
            std::filesystem::remove(temporary, ec);
            if (error != nullptr) *error = name + " could not be written. Is the folder read-only?";
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        std::filesystem::remove(temporary, ec);
        if (error != nullptr) *error = name + " could not be replaced. Is it open in another program?";
        return false;
    }
    return true;
}

// ---- display.cfg --------------------------------------------------------------------------------------------------

bool read_display(const std::string& text, DisplayPrefs& out, std::string* error, std::vector<std::string>* warnings) {
    DisplayPrefs c;
    out = DisplayPrefs{};
    std::istringstream stream(text);
    std::string line;
    int number = 0;
    while (std::getline(stream, line)) {
        ++number;
        if (const auto hash = line.find('#'); hash != std::string::npos) line.resize(hash);
        line = trim_copy(line);
        if (line.empty()) continue;
        const auto fail = [&](const std::string& message) {
            if (error != nullptr) *error = "display.cfg line " + std::to_string(number) + ": " + message;
            return false;
        };
        const auto split = line.find('=');
        if (split == std::string::npos) {
            if (error != nullptr) *error = "display.cfg line " + std::to_string(number) + " is not key=value";
            return false;
        }
        const std::string key = lower_copy(trim_copy(line.substr(0, split)));
        const std::string value = trim_copy(line.substr(split + 1));
        if (key == "width" || key == "window_width") {
            if (!display_int(value, &c.width) || c.width < 160) return fail("invalid width");
        } else if (key == "height" || key == "window_height") {
            if (!display_int(value, &c.height) || c.height < 120) return fail("invalid height");
        } else if (key == "fullscreen") {
            if (!display_bool(value, &c.fullscreen)) return fail("invalid fullscreen");
        } else if (key == "exclusive") {
            if (!display_bool(value, &c.exclusive)) return fail("invalid exclusive");
        } else if (key == "gameplay_aspect") {
            const std::string v = lower_copy(value);
            double ratio = -1.0;
            if (v == "follow") ratio = 0.0;
            else if (v == "original") ratio = 4.0 / 3.0;
            else if (const std::size_t colon = v.find(':'); colon != std::string::npos) {
                float w = 0.0f, h = 0.0f;
                if (display_float(v.substr(0, colon), &w) && display_float(v.substr(colon + 1), &h) && h > 0.0f)
                    ratio = static_cast<double>(w) / h;
            } else {
                float r = 0.0f;
                if (display_float(v, &r)) ratio = r;
            }
            if (!(ratio == 0.0 || (ratio >= 0.25 && ratio <= 8.0)))
                return fail("invalid gameplay_aspect (follow, original, W:H or a number)");
        } else if (key == "msaa" || key == "anisotropy") {
            std::string v = lower_copy(value);
            if (!v.empty() && v.back() == 'x') v.pop_back();
            int n = 0;
            if (v == "off") n = 1;
            else if (!display_int(v, &n)) n = 0;
            const bool msaa = key == "msaa";
            const bool ok = msaa ? (n == 1 || n == 2 || n == 4 || n == 8 || n == 16 || n == 32)
                                 : (n == 1 || n == 2 || n == 4 || n == 8 || n == 16);
            if (!ok) return fail(msaa ? "invalid msaa (off, 2, 4, 8, 16, 32)" : "invalid anisotropy (off, 2, 4, 8, 16)");
            (msaa ? c.msaa : c.anisotropy) = n;
        } else if (key == "render_scale") {
            char* end = nullptr;
            const double scale = std::strtod(value.c_str(), &end);
            if (value.empty() || end == value.c_str() || *end != '\0' || !std::isfinite(scale) || scale <= 0.0)
                return fail("invalid render_scale (25..200)");
            const double kept = std::clamp(scale, gekko::kMinRenderScale, gekko::kMaxRenderScale);
            if (kept != scale && warnings != nullptr)
                warnings->push_back("display.cfg line " + std::to_string(number) + ": render_scale=" + value +
                                    " is outside 25..200 %; the game uses " + format_number(kept) + " %");
            c.render_scale = kept;
        } else if (key == "render_width" || key == "render_height" || key == "manual_width" || key == "manual_height" ||
                   key == "resolution_mode" || key == "auto_resolution") {
            if (warnings != nullptr)
                warnings->push_back("display.cfg line " + std::to_string(number) + ": " + key +
                                    " is no longer a setting; the game ignores it");
        } else if (key == "fullscreen_width") {
            if (!display_int(value, &c.fullscreen_width) || (c.fullscreen_width != 0 && c.fullscreen_width < 160))
                return fail("invalid fullscreen_width (0 = the desktop's mode, or at least 160)");
        } else if (key == "fullscreen_height") {
            if (!display_int(value, &c.fullscreen_height) || (c.fullscreen_height != 0 && c.fullscreen_height < 120))
                return fail("invalid fullscreen_height (0 = the desktop's mode, or at least 120)");
        } else if (key == "aspect") {
            double ratio = 0.0;
            if (!gekko::parse_aspect_value(value, &ratio))
                return fail("invalid aspect (auto, W:H such as 32:10, or a number such as 2.37)");
            c.aspect = ratio;
        } else if (key == "aspect_ratio" || key == "custom_aspect") {
            double ratio = 0.0;
            if (!gekko::parse_aspect_value(value, &ratio) || !(ratio > 0.0))
                return fail("invalid aspect_ratio (W:H or a number of 0.25..8)");
            c.aspect = ratio;
        } else if (key == "hud_scale") {
            float v = 0.0f;
            if (!display_float(value, &v) || v <= 0.05f) return fail("invalid hud_scale");
        } else if (key == "hud_inset" || key == "hud_safe_zone") {
            float v = 0.0f;
            if (!display_float(value, &v) || v < 0.0f) return fail("invalid hud_inset");
        } else if (key == "render_rate") {
            char* end = nullptr;
            const double rate = std::strtod(value.c_str(), &end);
            if (lower_copy(value) == "unlocked") c.render_rate = 0.0;
            else if (!value.empty() && end != value.c_str() && *end == '\0' && std::isfinite(rate) && rate > 0.0 &&
                     rate <= 1000.0)
                c.render_rate = rate;
            else
                return fail("invalid render_rate (unlocked or a value in (0,1000])");
        } else if (key == "vsync") {
            if (!display_bool(value, &c.vsync)) return fail("invalid vsync");
        } else if (key == "frame_interpolation") {
            if (!display_bool(value, &c.frame_interpolation)) return fail("invalid frame_interpolation");
        } else if (key == "draw_distance") {
            float scale = 0.0f;
            if (!display_float(value, &scale) || !(scale >= 1.0f && scale <= 8.0f))
                return fail("invalid draw_distance (1 = the original game .. 8)");
            c.draw_distance = scale;
        } else if (key == "fps_counter") {
            if (!display_bool(value, &c.fps_counter)) return fail("invalid fps_counter");
        } else {
            return fail("unknown key");
        }
    }
    out = c;
    return true;
}

std::string check_display(const DisplayPrefs& p) {
    const int min_w = static_cast<int>(gekko::kMinPictureWidth), min_h = static_cast<int>(gekko::kMinPictureHeight);
    const int max_side = static_cast<int>(gekko::kMaxRenderSide);
    if (p.width < min_w || p.height < min_h || p.width > max_side || p.height > max_side)
        return "The window size must be at least 320 x 240 and at most 16384 x 16384.";
    if ((p.fullscreen_width == 0) != (p.fullscreen_height == 0) ||
        (p.fullscreen_width != 0 && (p.fullscreen_width < min_w || p.fullscreen_height < min_h ||
                                     p.fullscreen_width > max_side || p.fullscreen_height > max_side)))
        return "The full screen resolution must be the desktop's, or at least 320 x 240.";
    if (!std::isfinite(p.aspect) || (p.aspect != 0.0 && (p.aspect < 0.25 || p.aspect > 8.0)))
        return "The picture shape is not one the game can use.";
    if (!std::isfinite(p.render_scale) || p.render_scale < gekko::kMinRenderScale || p.render_scale > gekko::kMaxRenderScale)
        return "The resolution scale must be from 25 % to 200 %.";
    if (p.msaa != 1 && p.msaa != 2 && p.msaa != 4 && p.msaa != 8 && p.msaa != 16 && p.msaa != 32)
        return "Edge smoothing must be off, 2x, 4x, 8x, 16x or 32x.";
    if (p.anisotropy != 1 && p.anisotropy != 2 && p.anisotropy != 4 && p.anisotropy != 8 && p.anisotropy != 16)
        return "Texture sharpness must be off, 2x, 4x, 8x or 16x.";
    if (!std::isfinite(p.render_rate) || p.render_rate < 0.0 || p.render_rate > 1000.0)
        return "The frame rate must be unlimited or from 1 to 1000 frames per second.";
    if (!(p.draw_distance >= 1.0 && p.draw_distance <= 8.0)) return "The draw distance must be from 1 to 8.";
    return {};
}

std::string write_display(const std::string& text, const DisplayPrefs& before, const DisplayPrefs& after) {
    std::vector<cf::KeyValue> values;
    std::vector<std::string> removed;
    const auto put = [&](bool changed, std::vector<std::string> spellings, std::string value) {
        if (changed) values.push_back({std::move(spellings), std::move(value)});
    };
    put(before.fullscreen != after.fullscreen, {"fullscreen"}, spelled(after.fullscreen));
    put(before.exclusive != after.exclusive, {"exclusive"}, spelled(after.exclusive));
    put(before.width != after.width, {"width", "window_width"}, std::to_string(after.width));
    put(before.height != after.height, {"height", "window_height"}, std::to_string(after.height));
    put(before.fullscreen_width != after.fullscreen_width, {"fullscreen_width"}, std::to_string(after.fullscreen_width));
    put(before.fullscreen_height != after.fullscreen_height, {"fullscreen_height"}, std::to_string(after.fullscreen_height));
    if (before.aspect != after.aspect) {
        // The older spellings of a fixed ratio would override the new value from a later line: the new one replaces them.
        removed = {"aspect_ratio", "custom_aspect"};
        const char* name = after.aspect > 0.0 ? gekko::aspect_name(after.aspect) : nullptr;
        put(true, {"aspect"}, !(after.aspect > 0.0) ? std::string("auto") : name != nullptr ? std::string(name) : format_number(after.aspect));
    }
    put(before.render_scale != after.render_scale, {"render_scale"}, format_number(after.render_scale));
    put(before.msaa != after.msaa, {"msaa"}, samples(after.msaa));
    put(before.anisotropy != after.anisotropy, {"anisotropy"}, samples(after.anisotropy));
    put(before.render_rate != after.render_rate, {"render_rate"},
        after.render_rate <= 0.0 ? std::string("unlocked") : format_number(after.render_rate));
    put(before.vsync != after.vsync, {"vsync"}, spelled(after.vsync));
    put(before.frame_interpolation != after.frame_interpolation, {"frame_interpolation"}, spelled(after.frame_interpolation));
    put(before.draw_distance != after.draw_distance, {"draw_distance"}, format_number(after.draw_distance));
    put(before.fps_counter != after.fps_counter, {"fps_counter"}, spelled(after.fps_counter));
    if (values.empty()) return text;
    return cf::rewrite(remove_keys(text, removed), values);
}

// ---- options.cfg --------------------------------------------------------------------------------------------------

void read_options(const std::string& text, OptionPrefs& out, std::vector<std::string>* warnings) {
    OptionPrefs s;
    std::vector<int> bad;
    for (const cf::Entry& e : cf::parse(text, &bad)) {
        bool ok = true;
        OptionPrefs t = s;
        if (e.key == "master_volume") ok = options_int(e.value, t.master_volume);
        else if (e.key == "music_volume") ok = options_int(e.value, t.music_volume);
        else if (e.key == "sfx_volume") ok = options_int(e.value, t.sfx_volume);
        else if (e.key == "voice_volume") ok = options_int(e.value, t.voice_volume);
        else if (e.key == "rumble_strength") ok = options_int(e.value, t.rumble_strength);
        else if (e.key == "invert_camera_x") ok = options_bool(e.value, t.invert_camera_x);
        else if (e.key == "invert_camera_y") ok = options_bool(e.value, t.invert_camera_y);
        else if (e.key == "subtitles") ok = options_bool(e.value, t.subtitles);
        else if (e.key == "language") {
            ok = options_int(e.value, t.language);
            if (ok && t.language == 6) t.language = 1;  // the withdrawn Hungarian text reads as English
        } else if (e.key == "speaker") ok = options_int(e.value, t.speaker);
        else if (e.key == "voice") ok = options_int(e.value, t.voice);
        else if (e.key == "vibration" || e.key == "autosave") {
            bool on = false;
            ok = options_bool(e.value, on);
            (e.key == "vibration" ? t.vibration : t.autosave) = on ? 1 : 0;
        } else {
            if (warnings != nullptr)
                warnings->push_back("options.cfg line " + std::to_string(e.line) + ": unknown key '" + e.key + "' (kept)");
            continue;
        }
        if (ok && options_valid(t)) s = t;
        else if (warnings != nullptr)
            warnings->push_back("options.cfg line " + std::to_string(e.line) + ": bad value for " + e.key +
                                " (the game uses the default)");
    }
    for (const int line : bad)
        if (warnings != nullptr) warnings->push_back("options.cfg line " + std::to_string(line) + ": not key = value");
    out = s;
}

std::string check_options(const OptionPrefs& s) {
    const auto percent = [](int v) { return v >= 0 && v <= 100; };
    if (!percent(s.master_volume) || !percent(s.music_volume) || !percent(s.sfx_volume) || !percent(s.voice_volume))
        return "Volumes must be from 0 % to 100 %.";
    if (s.rumble_strength < 25 || s.rumble_strength > 100) return "Rumble strength must be from 25 % to 100 %.";
    if (s.language < -1 || s.language > 5) return "That text language is not one the game has.";
    if (s.speaker < -1 || s.speaker > 2) return "That sound output is not one the game has.";
    if (s.vibration < -1 || s.vibration > 1) return "Vibration must be on or off.";
    if (s.autosave < -1 || s.autosave > 1) return "Autosave must be on or off.";
    if (s.voice < -1 || s.voice > 1) return "The spoken language must be Japanese or English.";
    return {};
}

std::string write_options(const std::string& text, const OptionPrefs& before, const OptionPrefs& after) {
    std::vector<cf::KeyValue> values;
    std::vector<std::string> removed;
    const auto number = [&](int b, int a, const char* key) {
        if (b != a) values.push_back({{key}, std::to_string(a)});
    };
    const auto flag = [&](bool b, bool a, const char* key) {
        if (b != a) values.push_back({{key}, spelled(a)});
    };
    // The game's own options: -1 removes the line, so the game decides again.
    const auto game = [&](int b, int a, const char* key, bool is_flag) {
        if (b == a) return;
        if (a < 0) removed.push_back(key);
        else values.push_back({{key}, is_flag ? std::string(spelled(a != 0)) : std::to_string(a)});
    };
    number(before.master_volume, after.master_volume, "master_volume");
    number(before.music_volume, after.music_volume, "music_volume");
    number(before.sfx_volume, after.sfx_volume, "sfx_volume");
    number(before.voice_volume, after.voice_volume, "voice_volume");
    number(before.rumble_strength, after.rumble_strength, "rumble_strength");
    flag(before.invert_camera_x, after.invert_camera_x, "invert_camera_x");
    flag(before.invert_camera_y, after.invert_camera_y, "invert_camera_y");
    flag(before.subtitles, after.subtitles, "subtitles");
    game(before.language, after.language, "language", false);
    game(before.speaker, after.speaker, "speaker", false);
    game(before.vibration, after.vibration, "vibration", true);
    game(before.voice, after.voice, "voice", false);
    game(before.autosave, after.autosave, "autosave", true);
    if (values.empty() && removed.empty()) return text;
    const std::string kept = remove_keys(text, removed);
    return values.empty() ? kept : cf::rewrite(kept, values);
}

// ---- controls.cfg -------------------------------------------------------------------------------------------------

void read_controls(const std::string& text, ControlsConfig& out, std::vector<std::string>* warnings) {
    out = sp::default_controls();
    std::vector<std::string> problems;
    sp::parse_controls(text, out, &problems);
    if (warnings != nullptr)
        for (const std::string& p : problems) warnings->push_back("controls.cfg " + p);
}

std::string check_controls(const ControlsConfig& config) {
    std::string error;
    if (sp::validate_controls(config, &error)) return {};
    if (error.find("sensitivity") != std::string::npos) return "Mouse speed must be a number from 0.01 to 50.";
    if (error.find("walk") != std::string::npos) return "Walk speed must be from 1 to 100.";
    return "The keyboard and mouse settings are not ones the game can use (" + error + ").";
}

std::string write_controls(const std::string& text, const ControlsConfig& before, const ControlsConfig& after) {
    const std::vector<sp::ControlsSetting> a = sp::controls_settings(before), b = sp::controls_settings(after);
    std::vector<cf::KeyValue> values;
    for (std::size_t i = 0; i < a.size() && i < b.size(); ++i)
        if (a[i].value != b[i].value) values.push_back({{b[i].key}, b[i].value});
    if (values.empty()) return text;
    return cf::rewrite(text, values);
}

// ---- saving -------------------------------------------------------------------------------------------------------

namespace {

#define PADCONFIG_MERGE(field) \
    if (before.field != after.field) out.field = after.field;

DisplayPrefs merge(DisplayPrefs out, const DisplayPrefs& before, const DisplayPrefs& after) {
    PADCONFIG_MERGE(fullscreen) PADCONFIG_MERGE(exclusive) PADCONFIG_MERGE(width) PADCONFIG_MERGE(height)
    PADCONFIG_MERGE(fullscreen_width) PADCONFIG_MERGE(fullscreen_height) PADCONFIG_MERGE(aspect)
    PADCONFIG_MERGE(render_scale) PADCONFIG_MERGE(msaa) PADCONFIG_MERGE(anisotropy) PADCONFIG_MERGE(render_rate)
    PADCONFIG_MERGE(vsync) PADCONFIG_MERGE(frame_interpolation) PADCONFIG_MERGE(draw_distance) PADCONFIG_MERGE(fps_counter)
    return out;
}

OptionPrefs merge(OptionPrefs out, const OptionPrefs& before, const OptionPrefs& after) {
    PADCONFIG_MERGE(master_volume) PADCONFIG_MERGE(music_volume) PADCONFIG_MERGE(sfx_volume)
    PADCONFIG_MERGE(voice_volume) PADCONFIG_MERGE(rumble_strength) PADCONFIG_MERGE(invert_camera_x)
    PADCONFIG_MERGE(invert_camera_y) PADCONFIG_MERGE(subtitles) PADCONFIG_MERGE(language) PADCONFIG_MERGE(speaker)
    PADCONFIG_MERGE(vibration) PADCONFIG_MERGE(voice) PADCONFIG_MERGE(autosave)
    return out;
}

ControlsConfig merge(ControlsConfig out, const ControlsConfig& before, const ControlsConfig& after) {
    for (std::size_t i = 0; i < out.binds.size(); ++i) PADCONFIG_MERGE(binds[i])
    PADCONFIG_MERGE(mouse_sensitivity_x) PADCONFIG_MERGE(mouse_sensitivity_y) PADCONFIG_MERGE(mouse_invert_x)
    PADCONFIG_MERGE(mouse_invert_y) PADCONFIG_MERGE(mouse_capture) PADCONFIG_MERGE(walk_deflection)
    return out;
}

#undef PADCONFIG_MERGE

bool refuse(std::string* error, const std::string& message) {
    if (error != nullptr) *error = message;
    return false;
}

const char* kReadBack = " could not be written so that the game reads it back the same way; nothing was changed.";

}  // namespace

bool save_display(const std::filesystem::path& path, const DisplayPrefs& before, const DisplayPrefs& after,
                  std::string* error) {
    if (before == after) return true;
    if (const std::string problem = check_display(after); !problem.empty()) return refuse(error, problem);
    std::string text;
    if (!read_file(path, text, nullptr, error)) return false;
    DisplayPrefs file;
    std::string why;
    if (!read_display(text, file, &why))
        return refuse(error, "display.cfg has a line the game cannot read (" + why +
                                 "). The game will not start with it: fix or delete display.cfg, then save again.");
    const std::string written = write_display(text, before, after);
    DisplayPrefs check;
    if (!read_display(written, check, nullptr) || !(check == merge(file, before, after)))
        return refuse(error, std::string("display.cfg") + kReadBack);
    return written == text || write_file(path, written, error);
}

bool save_options(const std::filesystem::path& path, const OptionPrefs& before, const OptionPrefs& after,
                  std::string* error) {
    if (before == after) return true;
    if (const std::string problem = check_options(after); !problem.empty()) return refuse(error, problem);
    std::string text;
    if (!read_file(path, text, nullptr, error)) return false;
    OptionPrefs file, check;
    read_options(text, file);
    const std::string written = write_options(text, before, after);
    read_options(written, check);
    if (!(check == merge(file, before, after))) return refuse(error, std::string("options.cfg") + kReadBack);
    return written == text || write_file(path, written, error);
}

bool save_controls(const std::filesystem::path& path, const ControlsConfig& before, const ControlsConfig& after,
                   std::string* error) {
    if (before == after) return true;
    if (const std::string problem = check_controls(after); !problem.empty()) return refuse(error, problem);
    std::string text;
    if (!read_file(path, text, nullptr, error)) return false;
    ControlsConfig file, check;
    read_controls(text, file);
    const std::string written = write_controls(text, before, after);
    read_controls(written, check);
    if (!(check == merge(file, before, after))) return refuse(error, std::string("controls.cfg") + kReadBack);
    return written == text || write_file(path, written, error);
}

}  // namespace padconfig
