// The PC-only settings. Contract: include/gekko/pc_settings.h.

#include "gekko/pc_settings.h"

#include <cstdio>
#include <cstdlib>

#include "gekko/audio_categories.h"
#include "gekko/audio_runtime.h"
#include "gekko/config_file.h"
#include "gekko/xinput_pad.h"

namespace gekko {

namespace {

bool parse_int(const std::string& text, int& out) {
    char* end = nullptr;
    const long v = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0') return false;
    out = static_cast<int>(v);
    return true;
}

bool parse_bool(const std::string& text, bool& out) {
    if (text == "1" || text == "true" || text == "on" || text == "yes") return out = true, true;
    if (text == "0" || text == "false" || text == "off" || text == "no") return out = false, true;
    return false;
}

const char* spelled(bool v) { return v ? "true" : "false"; }

}  // namespace

bool operator==(const PcSettings& a, const PcSettings& b) noexcept {
    return a.master_volume == b.master_volume && a.music_volume == b.music_volume && a.sfx_volume == b.sfx_volume &&
           a.voice_volume == b.voice_volume && a.rumble_strength == b.rumble_strength &&
           a.invert_camera_x == b.invert_camera_x && a.invert_camera_y == b.invert_camera_y &&
           a.subtitles == b.subtitles && a.language == b.language && a.speaker == b.speaker && a.vibration == b.vibration && a.voice == b.voice &&
           a.autosave == b.autosave;
}

bool PcSettingsService::validate(const PcSettings& s, std::string* error) {
    const auto fail = [&](const char* m) {
        if (error != nullptr) *error = std::string("options: ") + m;
        return false;
    };
    if (s.master_volume < 0 || s.master_volume > 100) return fail("master_volume must be 0..100");
    if (s.music_volume < 0 || s.music_volume > 100) return fail("music_volume must be 0..100");
    if (s.sfx_volume < 0 || s.sfx_volume > 100) return fail("sfx_volume must be 0..100");
    if (s.voice_volume < 0 || s.voice_volume > 100) return fail("voice_volume must be 0..100");
    if (s.rumble_strength < 25 || s.rumble_strength > 100) return fail("rumble_strength must be 25..100");
    if (s.language < -1 || s.language > 6) return fail("language must be 0..6 (or unset; 6 = Hungarian)");
    if (s.speaker < -1 || s.speaker > 2) return fail("speaker must be 0..2 (or unset)");
    if (s.vibration < -1 || s.vibration > 1) return fail("vibration must be 0 or 1 (or unset)");
    if (s.autosave < -1 || s.autosave > 1) return fail("autosave must be 0 or 1 (or unset)");
    if (s.voice < -1 || s.voice > 1) return fail("voice must be 0 (Japanese) or 1 (English) (or unset)");
    return true;
}

void PcSettingsService::load(const std::filesystem::path& path, std::vector<std::string>* warnings) {
    PcSettings s;
    std::string text, error;
    if (!config_file::read(path, text, &error)) {
        if (warnings != nullptr) warnings->push_back(error + " (defaults used)");
        text.clear();
    }
    std::vector<int> bad;
    for (const config_file::Entry& e : config_file::parse(text, &bad)) {
        bool ok = true;
        PcSettings t = s;
        if (e.key == "master_volume") ok = parse_int(e.value, t.master_volume);
        else if (e.key == "music_volume") ok = parse_int(e.value, t.music_volume);
        else if (e.key == "sfx_volume") ok = parse_int(e.value, t.sfx_volume);
        else if (e.key == "voice_volume") ok = parse_int(e.value, t.voice_volume);
        else if (e.key == "rumble_strength") ok = parse_int(e.value, t.rumble_strength);
        else if (e.key == "invert_camera_x") ok = parse_bool(e.value, t.invert_camera_x);
        else if (e.key == "invert_camera_y") ok = parse_bool(e.value, t.invert_camera_y);
        else if (e.key == "subtitles") ok = parse_bool(e.value, t.subtitles);
        else if (e.key == "language") ok = parse_int(e.value, t.language);
        else if (e.key == "speaker") ok = parse_int(e.value, t.speaker);
        else if (e.key == "voice") ok = parse_int(e.value, t.voice);
        else if (e.key == "vibration") {
            bool on = false;
            ok = parse_bool(e.value, on);
            t.vibration = on ? 1 : 0;
        } else if (e.key == "autosave") {
            bool on = false;
            ok = parse_bool(e.value, on);
            t.autosave = on ? 1 : 0;
        }
        else {
            if (warnings != nullptr)
                warnings->push_back(path.string() + ":" + std::to_string(e.line) + ": unknown key '" + e.key +
                                    "' (kept)");
            continue;
        }
        if (ok && validate(t, &error)) {
            s = t;
        } else if (warnings != nullptr) {
            warnings->push_back(path.string() + ":" + std::to_string(e.line) + ": bad value for " + e.key +
                                " (default used)");
        }
    }
    for (const int line : bad)
        if (warnings != nullptr) warnings->push_back(path.string() + ":" + std::to_string(line) + ": not key = value");
    {
        std::lock_guard<std::mutex> lock(mutex_);
        current_ = saved_ = s;
        path_ = path;
        loaded_ = true;
    }
    push(s);
}

bool PcSettingsService::loaded() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return loaded_;
}

std::filesystem::path PcSettingsService::path() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return path_;
}

PcSettings PcSettingsService::current() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_;
}

bool PcSettingsService::apply(const PcSettings& settings, std::string* error) {
    if (!validate(settings, error)) return false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        current_ = settings;
    }
    push(settings);
    return true;
}

void PcSettingsService::push(const PcSettings& s) const {
    // The evidence line of what reached the engine owners (settings validation): printed per push.
    std::fprintf(stderr,
                 "[options] in effect: master volume %d%% (gain %.2f), music %d%% sfx %d%% voices %d%%, rumble %d%%, invert camera x %d y %d; game options "
                 "language %d speaker %d vibration %d voice %d autosave %d; subtitles %d\n",
                 s.master_volume, static_cast<double>(s.master_volume) / 100.0, s.music_volume, s.sfx_volume, s.voice_volume,
                 s.rumble_strength, s.invert_camera_x ? 1 : 0,
                 s.invert_camera_y ? 1 : 0, s.language, s.speaker, s.vibration, s.voice, s.autosave,
                 s.subtitles ? 1 : 0);
    set_audio_output_gain(static_cast<float>(s.master_volume) / 100.0f);
    audio_category::set_percent(audio_category::Kind::Music, s.music_volume);
    audio_category::set_percent(audio_category::Kind::Sfx, s.sfx_volume);
    audio_category::set_percent(audio_category::Kind::Voice, s.voice_volume);
    set_xinput_preferences(static_cast<unsigned>(s.rumble_strength), s.invert_camera_x, s.invert_camera_y);
}

bool PcSettingsService::save(std::string* error) {
    PcSettings s, before;
    std::filesystem::path path;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        s = current_;
        before = saved_;
        path = path_;
    }
    if (path.empty()) {
        if (error != nullptr) *error = "options: no options.cfg path (not loaded)";
        return false;
    }
    std::vector<config_file::KeyValue> values;
    if (s.master_volume != before.master_volume) values.push_back({{"master_volume"}, std::to_string(s.master_volume)});
    if (s.music_volume != before.music_volume) values.push_back({{"music_volume"}, std::to_string(s.music_volume)});
    if (s.sfx_volume != before.sfx_volume) values.push_back({{"sfx_volume"}, std::to_string(s.sfx_volume)});
    if (s.voice_volume != before.voice_volume) values.push_back({{"voice_volume"}, std::to_string(s.voice_volume)});
    if (s.rumble_strength != before.rumble_strength)
        values.push_back({{"rumble_strength"}, std::to_string(s.rumble_strength)});
    if (s.invert_camera_x != before.invert_camera_x) values.push_back({{"invert_camera_x"}, spelled(s.invert_camera_x)});
    if (s.invert_camera_y != before.invert_camera_y) values.push_back({{"invert_camera_y"}, spelled(s.invert_camera_y)});
    if (s.subtitles != before.subtitles) values.push_back({{"subtitles"}, spelled(s.subtitles)});
    if (s.language != before.language && s.language >= 0) values.push_back({{"language"}, std::to_string(s.language)});
    if (s.speaker != before.speaker && s.speaker >= 0) values.push_back({{"speaker"}, std::to_string(s.speaker)});
    if (s.vibration != before.vibration && s.vibration >= 0) values.push_back({{"vibration"}, spelled(s.vibration != 0)});
    if (s.voice != before.voice && s.voice >= 0) values.push_back({{"voice"}, std::to_string(s.voice)});
    if (s.autosave != before.autosave && s.autosave >= 0) values.push_back({{"autosave"}, spelled(s.autosave != 0)});
    if (values.empty()) return true;
    if (!config_file::update(path, values, error)) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    saved_ = s;
    return true;
}

PcSettingsService& pc_settings() {
    static PcSettingsService service;
    return service;
}

}  // namespace gekko
