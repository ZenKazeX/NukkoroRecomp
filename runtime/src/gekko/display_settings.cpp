#include "gekko/display_settings.h"

#include "gekko/display_geometry.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>
#include <vector>

namespace gekko {

namespace {

// The display.cfg keys this service owns (gekko/display.cpp parse_display_file reads them). A key's aliases are
// recognised when the file is rewritten; a key the file lacks is appended under its first name.
enum DisplaySettingsKey : unsigned {
    kKeyFullscreen = 1u << 0,
    kKeyWidth = 1u << 1,
    kKeyHeight = 1u << 2,

    kKeyRenderRate = 1u << 5,
    kKeyVsync = 1u << 6,
    kKeyFrameInterpolation = 1u << 7,
    kKeyDrawDistance = 1u << 8,
    kKeyFpsCounter = 1u << 9,
    kKeyExclusive = 1u << 10,
    kKeyAspect = 1u << 11,
    kKeyRenderScale = 1u << 12,
    kKeyMsaa = 1u << 13,
    kKeyAnisotropy = 1u << 14,
    kKeyGameplayAspect = 1u << 15,
    kKeyFullscreenWidth = 1u << 18,
    kKeyFullscreenHeight = 1u << 19,
};

struct KeyName {
    unsigned key;
    const char* name;
};

// First spelling of each key = the one appended.
constexpr KeyName kKeyNames[] = {
    {kKeyFullscreen, "fullscreen"},
    {kKeyExclusive, "exclusive"},
    {kKeyWidth, "width"},
    {kKeyWidth, "window_width"},
    {kKeyHeight, "height"},
    {kKeyHeight, "window_height"},
    {kKeyFullscreenWidth, "fullscreen_width"},
    {kKeyFullscreenHeight, "fullscreen_height"},
    {kKeyAspect, "aspect"},
    {kKeyRenderScale, "render_scale"},
    {kKeyMsaa, "msaa"},
    {kKeyAnisotropy, "anisotropy"},
    {kKeyGameplayAspect, "gameplay_aspect"},
    {kKeyRenderRate, "render_rate"},
    {kKeyVsync, "vsync"},
    {kKeyFrameInterpolation, "frame_interpolation"},
    {kKeyDrawDistance, "draw_distance"},
    {kKeyFpsCounter, "fps_counter"},
};

std::string lower_trimmed(const std::string& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    std::string out = text.substr(begin, end - begin);
    for (char& ch : out) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return out;
}

unsigned key_for_name(const std::string& lowered) {
    for (const KeyName& entry : kKeyNames)
        if (lowered == entry.name) return entry.key;
    return 0u;
}

// The shortest spelling that reads back as the same value (60, 59.94, 2.5).
std::string format_number(double value) {
    // Whole numbers as integers: the shortest default spelling of 100 is "1e+02" (it reads back, but no person writes
    // the file like that).
    if (std::fabs(value) < 1e15 && value == std::floor(value)) return std::to_string(static_cast<long long>(value));
    std::string spelled;
    for (int digits = 1; digits <= 17; ++digits) {
        std::ostringstream out;
        out.precision(digits);
        out << value;
        spelled = out.str();
        if (std::strtod(spelled.c_str(), nullptr) == value) break;
    }
    return spelled;
}

std::string key_value(unsigned key, const DisplaySettings& s) {
    switch (key) {
        case kKeyFullscreen: return s.fullscreen ? "true" : "false";
        case kKeyExclusive: return s.exclusive ? "true" : "false";
        case kKeyAspect: {
            if (!(s.aspect > 0.0)) return "auto";
            const char* name = aspect_name(s.aspect);
            return name != nullptr ? name : format_number(s.aspect);
        }
        case kKeyRenderScale: return format_number(s.render_scale);
        case kKeyMsaa: return std::to_string(s.msaa);
        case kKeyAnisotropy: return std::to_string(s.anisotropy);
        case kKeyGameplayAspect: {
            if (!(s.gameplay_aspect > 0.0)) return "follow";
            if (std::fabs(s.gameplay_aspect - 4.0 / 3.0) < 1e-9) return "original";
            const char* name = aspect_name(s.gameplay_aspect);
            return name != nullptr ? name : format_number(s.gameplay_aspect);
        }
        case kKeyWidth: return std::to_string(s.window_width);
        case kKeyHeight: return std::to_string(s.window_height);
        case kKeyFullscreenWidth: return std::to_string(s.fullscreen_width);
        case kKeyFullscreenHeight: return std::to_string(s.fullscreen_height);
        case kKeyRenderRate: return s.render_rate <= 0.0 ? "unlocked" : format_number(s.render_rate);
        case kKeyVsync: return s.vsync ? "true" : "false";
        case kKeyFrameInterpolation: return s.frame_interpolation ? "true" : "false";
        case kKeyDrawDistance: return format_number(static_cast<double>(s.draw_distance));
        case kKeyFpsCounter: return s.fps_counter ? "true" : "false";
        default: return {};
    }
}

// The keys whose values differ between a and b.
unsigned changed_keys(const DisplaySettings& a, const DisplaySettings& b) {
    unsigned keys = 0u;
    if (a.fullscreen != b.fullscreen) keys |= kKeyFullscreen;
    if (a.exclusive != b.exclusive) keys |= kKeyExclusive;
    if (a.aspect != b.aspect) keys |= kKeyAspect;
    if (a.render_scale != b.render_scale) keys |= kKeyRenderScale;
    if (a.msaa != b.msaa) keys |= kKeyMsaa;
    if (a.anisotropy != b.anisotropy) keys |= kKeyAnisotropy;
    if (a.gameplay_aspect != b.gameplay_aspect) keys |= kKeyGameplayAspect;
    if (a.window_width != b.window_width) keys |= kKeyWidth;
    if (a.window_height != b.window_height) keys |= kKeyHeight;
    if (a.fullscreen_width != b.fullscreen_width) keys |= kKeyFullscreenWidth;
    if (a.fullscreen_height != b.fullscreen_height) keys |= kKeyFullscreenHeight;
    if (a.render_rate != b.render_rate) keys |= kKeyRenderRate;
    if (a.vsync != b.vsync) keys |= kKeyVsync;
    if (a.frame_interpolation != b.frame_interpolation) keys |= kKeyFrameInterpolation;
    if (a.draw_distance != b.draw_distance) keys |= kKeyDrawDistance;
    if (a.fps_counter != b.fps_counter) keys |= kKeyFpsCounter;
    return keys;
}

// Which owners' groups the changed keys reach (gekko/display_settings.h).
unsigned groups_for_keys(unsigned keys) {
    unsigned groups = 0u;
    // Only the display mode and the window size touch the window; the ratio and the scale never do (a ratio picked
    // in the menu reshapes a window once by setting its size - gekko/settings_display.cpp).
    if (keys & (kKeyFullscreen | kKeyExclusive | kKeyWidth | kKeyHeight | kKeyFullscreenWidth | kKeyFullscreenHeight))
        groups |= kDisplayWindowChange;
    if (keys & (kKeyRenderRate | kKeyVsync | kKeyFrameInterpolation | kKeyMsaa | kKeyAnisotropy))
        groups |= kDisplayPresentChange;  // the renderer takes MSAA / filtering at its next frame
    if (keys & kKeyFpsCounter) groups |= kDisplayOverlayChange;
    if (keys & (kKeyAspect | kKeyRenderScale | kKeyDrawDistance | kKeyGameplayAspect))
        groups |= kDisplayGameChange;
    return groups;
}

}  // namespace

unsigned display_settings_key(const char* name) noexcept {
    if (name == nullptr) return 0u;
    return key_for_name(lower_trimmed(name));
}

bool operator==(const DisplaySettings& a, const DisplaySettings& b) noexcept {
    return changed_keys(a, b) == 0u;
}

void DisplaySettingsService::initialize(const DisplaySettings& settings, std::filesystem::path cfg_path,
                                        unsigned override_keys) {
    std::lock_guard<std::mutex> lock(mutex_);
    current_ = confirmed_ = initial_ = settings;
    cfg_path_ = std::move(cfg_path);
    initialized_ = true;
    provisional_ = false;
    ++generation_;
    pending_ = 0u;
    touched_keys_ = 0u;
    override_keys_ = override_keys;
}

bool DisplaySettingsService::initialized() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return initialized_;
}

std::filesystem::path DisplaySettingsService::cfg_path() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cfg_path_;
}

DisplaySettings DisplaySettingsService::current() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_;
}

DisplaySettings DisplaySettingsService::confirmed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return confirmed_;
}

bool DisplaySettingsService::provisional() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return provisional_;
}

std::uint64_t DisplaySettingsService::generation() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return generation_;
}

bool DisplaySettingsService::validate(const DisplaySettings& s, std::string* error) {
    const auto fail = [&](const char* message) {
        if (error != nullptr) *error = std::string("display settings: ") + message;
        return false;
    };
    const int min_w = static_cast<int>(kMinPictureWidth), min_h = static_cast<int>(kMinPictureHeight);
    const int max_side = static_cast<int>(kMaxRenderSide);
    if (s.window_width < min_w || s.window_height < min_h || s.window_width > max_side || s.window_height > max_side)
        return fail("the window must be 320x240 .. 16384x16384");
    if ((s.fullscreen_width == 0) != (s.fullscreen_height == 0) ||
        (s.fullscreen_width != 0 && (s.fullscreen_width < min_w || s.fullscreen_height < min_h)))
        return fail("the exclusive mode must be 0x0 (the desktop's) or at least 320x240");
    if (!std::isfinite(s.render_scale) || s.render_scale < kMinRenderScale || s.render_scale > kMaxRenderScale)
        return fail("the resolution scale must be 25..200 %");
    if (!std::isfinite(s.aspect) || (s.aspect != 0.0 && (s.aspect < 0.25 || s.aspect > 8.0)))
        return fail("the aspect must be auto (0) or 0.25..8");
    if (s.msaa != 1 && s.msaa != 2 && s.msaa != 4 && s.msaa != 8 && s.msaa != 16 && s.msaa != 32)
        return fail("msaa must be 1 (off), 2, 4, 8, 16 or 32 samples");
    if (s.anisotropy != 1 && s.anisotropy != 2 && s.anisotropy != 4 && s.anisotropy != 8 && s.anisotropy != 16)
        return fail("anisotropy must be 1 (off), 2, 4, 8 or 16");
    if (!std::isfinite(s.render_rate) || s.render_rate < 0.0 || s.render_rate > 1000.0)
        return fail("the render rate must be unlocked (0) or in (0,1000]");
    if (!(s.draw_distance >= 1.0f && s.draw_distance <= 8.0f)) return fail("the draw distance must be 1..8");
    return true;
}

bool DisplaySettingsService::apply(const DisplaySettings& settings, std::string* error, bool provisional) {
    if (!validate(settings, error)) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    const unsigned keys = changed_keys(current_, settings);
    current_ = settings;
    pending_ |= groups_for_keys(keys);
    ++generation_;
    if (provisional) {
        provisional_ = true;
    } else {
        const unsigned committed = changed_keys(confirmed_, current_);
        touched_keys_ |= committed;
        override_keys_ &= ~committed;
        confirmed_ = current_;
        provisional_ = false;
    }
    return true;
}

void DisplaySettingsService::commit() {
    std::lock_guard<std::mutex> lock(mutex_);
    const unsigned committed = changed_keys(confirmed_, current_);
    touched_keys_ |= committed;
    override_keys_ &= ~committed;
    confirmed_ = current_;
    provisional_ = false;
    ++generation_;
}

void DisplaySettingsService::rollback() {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_ |= groups_for_keys(changed_keys(current_, confirmed_));
    current_ = confirmed_;
    provisional_ = false;
    ++generation_;
}

std::string DisplaySettingsService::session_overrides() const {
    unsigned keys = 0u;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        keys = override_keys_;
    }
    std::string out;
    for (const KeyName& entry : kKeyNames) {
        if ((keys & entry.key) == 0u) continue;
        if (!out.empty()) out += ",";
        out += entry.name;
        keys &= ~entry.key;  // an alias is named once
    }
    return out;
}

bool DisplaySettingsService::overridden(const char* key_name) const {
    const unsigned key = display_settings_key(key_name);
    std::lock_guard<std::mutex> lock(mutex_);
    return key != 0u && (override_keys_ & key) != 0u;
}

void DisplaySettingsService::report_window(const DisplayWindowState& state) {
    std::lock_guard<std::mutex> lock(mutex_);
    window_ = state;
}

DisplayWindowState DisplaySettingsService::window_state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return window_;
}

unsigned DisplaySettingsService::take_changes(unsigned groups, DisplaySettings* settings) {
    std::lock_guard<std::mutex> lock(mutex_);
    const unsigned taken = pending_ & groups;
    pending_ &= ~groups;
    if (settings != nullptr) *settings = current_;
    return taken;
}

bool DisplaySettingsService::save(std::string* error) const {
    DisplaySettings settings;
    std::filesystem::path path;
    unsigned keys = 0u;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        settings = confirmed_;  // never a provisional value
        path = cfg_path_;
        keys = touched_keys_;
    }
    const auto fail = [&](const std::string& message) {
        if (error != nullptr) *error = path.string() + ": " + message;
        return false;
    };
    if (path.empty()) return fail("no display.cfg path (not initialized)");

    std::string text;
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return fail("cannot be read");
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        if (in.bad()) return fail("cannot be read");
    }

    // Lines as they are, each with its own terminator; only a managed key's value range is replaced.
    std::string out;
    out.reserve(text.size() + 256u);
    bool crlf = false;
    unsigned written = 0u;
    std::size_t begin = 0;
    while (begin < text.size()) {
        std::size_t end = text.find('\n', begin);
        const std::size_t next = end == std::string::npos ? text.size() : end + 1;
        if (end == std::string::npos) end = text.size();
        std::size_t body_end = end;
        if (body_end > begin && text[body_end - 1] == '\r') {
            --body_end;
            crlf = true;
        }
        const std::string body = text.substr(begin, body_end - begin);
        const std::size_t comment = body.find('#');
        const std::size_t content_end = comment == std::string::npos ? body.size() : comment;
        const std::size_t split = body.find('=');
        unsigned key = 0u;
        if (split != std::string::npos && split < content_end)
            key = key_for_name(lower_trimmed(body.substr(0, split)));
        if (key != 0u && (keys & key) != 0u) {
            std::size_t value_begin = split + 1;
            while (value_begin < content_end && (body[value_begin] == ' ' || body[value_begin] == '\t'))
                ++value_begin;
            std::size_t value_end = content_end;
            while (value_end > value_begin && std::isspace(static_cast<unsigned char>(body[value_end - 1])))
                --value_end;
            out.append(body, 0, value_begin);
            out += key_value(key, settings);
            out.append(body, value_end, std::string::npos);
            out.append(text, body_end, next - body_end);  // the line's own terminator
            written |= key;
        } else {
            out.append(text, begin, next - begin);
        }
        begin = next;
    }
    const char* const newline = crlf ? "\r\n" : "\n";
    const unsigned missing = keys & ~written;
    if (missing != 0u && !out.empty() && out.back() != '\n') out += newline;
    for (const KeyName& entry : kKeyNames) {
        if ((missing & entry.key) == 0u) continue;
        if (written & entry.key) continue;  // an alias already appended it
        out += entry.name;
        out += '=';
        out += key_value(entry.key, settings);
        out += newline;
        written |= entry.key;
    }
    if (out == text) return true;

    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) return fail("cannot write " + temporary.string());
        file.write(out.data(), static_cast<std::streamsize>(out.size()));
        file.close();
        if (!file) {
            std::filesystem::remove(temporary, ec);
            return fail("cannot write " + temporary.string());
        }
    }
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        const std::string reason = ec.message();
        std::filesystem::remove(temporary, ec);
        return fail("cannot replace the file (" + reason + ")");
    }
    return true;
}

DisplaySettingsService& display_settings() {
    static DisplaySettingsService service;
    return service;
}

}  // namespace gekko
