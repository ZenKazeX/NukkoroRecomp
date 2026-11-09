#include "gekko/input_tape.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

// A recorded run of controller input in game time (gekko/input_tape.h).

namespace gekko {

void InputTape::begin_recording(std::uint64_t start_tick, int width, int height) {
    entries_.clear();
    // The output and the logic aspect in effect carry over: a recording restarted (a restored state) starts with them.
    if (!outputs_.empty()) {
        Output last = outputs_.back();
        last.tick = start_tick;
        outputs_.assign(1, last);
    }
    if (!logic_.empty()) {
        Logic last = logic_.back();
        last.tick = start_tick;
        logic_.assign(1, last);
    }
    settings_.clear();
    checks_.clear();
    start_tick_ = start_tick;
    stop_tick_ = 0;
    saved_tick_ = 0;
    width_ = width;
    height_ = height;
    have_last_ = false;
    recording_ = true;
    version_ = 3;
}

void InputTape::note_output(std::uint64_t tick, int width, int height) {
    if (width <= 0 || height <= 0) return;
    if (!outputs_.empty() && outputs_.back().width == width && outputs_.back().height == height) return;
    if (!outputs_.empty() && outputs_.back().tick == tick) {
        outputs_.back().width = width;
        outputs_.back().height = height;
        return;
    }
    outputs_.push_back(Output{tick, width, height});
}

void InputTape::note_logic(std::uint64_t tick, float aspect) {
    if (!(aspect > 0.0f)) return;
    if (!logic_.empty() && logic_.back().aspect == aspect) return;
    if (!logic_.empty() && logic_.back().tick == tick) {
        logic_.back().aspect = aspect;
        return;
    }
    logic_.push_back(Logic{tick, aspect});
}

void InputTape::note_setting(std::uint64_t tick, const std::string& key, const std::string& value) {
    if (!recording_ || key.empty()) return;
    settings_.push_back(Setting{tick, key, value});
}

void InputTape::note_check(std::uint64_t tick, const std::string& text) {
    if (!recording_) return;
    checks_.push_back(Check{tick, text});
}

void InputTape::set_meta(const std::string& key, const std::string& value) {
    std::string v = value;
    for (char& ch : v)
        if (ch == '\n' || ch == '\r') ch = ' ';
    for (auto& kv : meta_) {
        if (kv.first == key) {
            kv.second = v;
            return;
        }
    }
    meta_.emplace_back(key, v);
}

std::string InputTape::meta(const std::string& key) const {
    for (const auto& kv : meta_)
        if (kv.first == key) return kv.second;
    return {};
}

std::uint64_t InputTape::last_tick() const noexcept {
    std::uint64_t t = start_tick_;
    if (!entries_.empty()) t = std::max(t, entries_.back().tick);
    if (!checks_.empty()) t = std::max(t, checks_.back().tick);
    return t;
}

void InputTape::observe(std::uint64_t tick, const Samples& latched) {
    if (!recording_) return;
    for (std::size_t port = 0; port < kPorts; ++port) {
        if (have_last_ && latched[port] == last_[port]) continue;
        entries_.push_back(Entry{tick, static_cast<std::uint8_t>(port), latched[port]});
    }
    last_ = latched;
    have_last_ = true;
}

bool InputTape::write(const std::filesystem::path& path, std::uint64_t stop_tick, std::uint64_t saved_tick,
                      std::string* error) const {
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    const auto kept = [saved_tick](std::uint64_t tick) { return saved_tick == 0u || tick <= saved_tick; };
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (error != nullptr) *error = "cannot write " + temporary.string();
            return false;
        }
        out << "shadow-input-tape 3\n";
        out << "start " << start_tick_ << "\n";
        out << "display " << width_ << "x" << height_ << "\n";
        for (const auto& kv : meta_) out << "meta " << kv.first << " " << kv.second << "\n";
        char line[200];
        for (const Output& o : outputs_) {
            if (!kept(o.tick)) break;
            std::snprintf(line, sizeof(line), "output %llu %dx%d\n", static_cast<unsigned long long>(o.tick),
                          o.width, o.height);
            out << line;
        }
        for (const Logic& l : logic_) {
            if (!kept(l.tick)) break;
            std::snprintf(line, sizeof(line), "logic %llu %.9g\n", static_cast<unsigned long long>(l.tick),
                          static_cast<double>(l.aspect));
            out << line;
        }
        for (const Setting& s : settings_) {
            if (!kept(s.tick)) break;
            out << "setting " << s.tick << " " << s.key << " " << s.value << "\n";
        }
        for (const Check& c : checks_) {
            if (!kept(c.tick)) break;
            out << "check " << c.tick << " " << c.text << "\n";
        }
        for (const Entry& e : entries_) {
            // A boundary requested at `stop_tick` ends the recording the checkpoint needs: the host services up to
            // the boundary latch what follows too, so every entry up to the boundary's tick belongs to it.
            if (!kept(e.tick)) break;
            std::snprintf(line, sizeof(line), "sample %llu %u %u %04x %d %d %d %d\n",
                          static_cast<unsigned long long>(e.tick), static_cast<unsigned>(e.port),
                          e.sample.connected ? 1u : 0u, static_cast<unsigned>(e.sample.controls),
                          static_cast<int>(e.sample.stick_x), static_cast<int>(e.sample.stick_y),
                          static_cast<int>(e.sample.substick_x), static_cast<int>(e.sample.substick_y));
            out << line;
        }
        if (stop_tick != 0u) out << "stop " << stop_tick << "\n";
        if (saved_tick != 0u) out << "saved " << saved_tick << "\n";
        out << "end\n";
        if (!out) {
            if (error != nullptr) *error = "write failed for " + temporary.string();
            return false;
        }
    }
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        std::filesystem::remove(path, ec);
        std::filesystem::rename(temporary, path, ec);
        if (ec) {
            if (error != nullptr) *error = "cannot replace " + path.string();
            return false;
        }
    }
    return true;
}

bool InputTape::load(const std::filesystem::path& path, std::string* error) {
    const auto fail = [error, &path](const std::string& why) {
        if (error != nullptr) *error = path.string() + ": " + why;
        return false;
    };
    std::ifstream in(path, std::ios::binary);
    if (!in) return fail("cannot open");
    std::string line;
    if (!std::getline(in, line)) return fail("empty");
    if (!line.empty() && line.back() == '\r') line.pop_back();
    int version = 0;
    if (line == "shadow-input-tape 1") version = 1;
    else if (line == "shadow-input-tape 2") version = 2;
    else if (line == "shadow-input-tape 3") version = 3;
    else return fail("not a version 1, 2 or 3 input tape");
    std::vector<Entry> entries;
    std::vector<Output> outputs;
    std::vector<Logic> logic;
    std::vector<Setting> settings;
    std::vector<Check> checks;
    std::vector<std::pair<std::string, std::string>> meta;
    std::uint64_t start = 0, stop = 0, saved = 0, previous = 0;
    int width = 0, height = 0;
    bool ended = false;
    std::size_t number = 1;
    while (std::getline(in, line)) {
        ++number;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::istringstream fields(line);
        std::string kind;
        fields >> kind;
        if (kind == "start") {
            fields >> start;
        } else if (kind == "display") {
            std::string size;
            fields >> size;
            if (std::sscanf(size.c_str(), "%dx%d", &width, &height) != 2) return fail("bad display line");
        } else if (kind == "output" && version >= 2) {
            unsigned long long tick = 0;
            int w = 0, h = 0;
            if (std::sscanf(line.c_str(), "output %llu %dx%d", &tick, &w, &h) != 3 || w <= 0 || h <= 0)
                return fail("bad output at line " + std::to_string(number));
            if (!outputs.empty() && tick < outputs.back().tick)
                return fail("outputs out of order at line " + std::to_string(number));
            outputs.push_back(Output{tick, w, h});
        } else if (kind == "logic" && version >= 3) {
            unsigned long long tick = 0;
            float aspect = 0.0f;
            if (std::sscanf(line.c_str(), "logic %llu %g", &tick, &aspect) != 2 || !(aspect > 0.0f))
                return fail("bad logic at line " + std::to_string(number));
            if (!logic.empty() && tick < logic.back().tick)
                return fail("logic out of order at line " + std::to_string(number));
            logic.push_back(Logic{tick, aspect});
        } else if (kind == "meta" && version >= 3) {
            std::string key;
            fields >> key;
            std::string value;
            std::getline(fields, value);
            if (!value.empty() && value.front() == ' ') value.erase(0, 1);
            meta.emplace_back(key, value);
        } else if (kind == "setting" && version >= 3) {
            Setting s;
            fields >> s.tick >> s.key;
            std::getline(fields, s.value);
            if (!s.value.empty() && s.value.front() == ' ') s.value.erase(0, 1);
            if (s.key.empty()) return fail("bad setting at line " + std::to_string(number));
            settings.push_back(s);
        } else if (kind == "check" && version >= 3) {
            Check c;
            fields >> c.tick;
            std::getline(fields, c.text);
            if (!c.text.empty() && c.text.front() == ' ') c.text.erase(0, 1);
            if (!checks.empty() && c.tick < checks.back().tick)
                return fail("checks out of order at line " + std::to_string(number));
            checks.push_back(c);
        } else if (kind == "sample") {
            unsigned long long tick = 0;
            unsigned port = 0, connected = 0, controls = 0;
            int sx = 0, sy = 0, cx = 0, cy = 0;
            if (std::sscanf(line.c_str(), "sample %llu %u %u %x %d %d %d %d", &tick, &port, &connected, &controls,
                            &sx, &sy, &cx, &cy) != 8 ||
                port >= kPorts || connected > 1u || (controls & ~shadow::peripheral::kAllControls) != 0u ||
                sx < -128 || sx > 127 || sy < -128 || sy > 127 || cx < -128 || cx > 127 || cy < -128 || cy > 127) {
                return fail("bad sample at line " + std::to_string(number));
            }
            if (tick < previous) return fail("samples out of order at line " + std::to_string(number));
            previous = tick;
            Entry e;
            e.tick = tick;
            e.port = static_cast<std::uint8_t>(port);
            e.sample.connected = connected != 0u;
            e.sample.controls = controls;
            e.sample.stick_x = static_cast<std::int8_t>(sx);
            e.sample.stick_y = static_cast<std::int8_t>(sy);
            e.sample.substick_x = static_cast<std::int8_t>(cx);
            e.sample.substick_y = static_cast<std::int8_t>(cy);
            entries.push_back(e);
        } else if (kind == "stop") {
            fields >> stop;
        } else if (kind == "saved") {
            fields >> saved;
        } else if (kind == "end") {
            ended = true;
            break;
        } else {
            return fail("unknown record '" + kind + "' at line " + std::to_string(number));
        }
    }
    if (!ended) return fail("no end line (a truncated tape)");
    if (version == 2 && outputs.empty()) return fail("a version 2 tape without an output record");
    // A version 2 tape (or a version 3 one without logic records): its output's aspect is the logic's, as the
    // recording run used it (the game output fed both then).
    if (logic.empty())
        for (const Output& o : outputs)
            logic.push_back(Logic{o.tick, static_cast<float>(o.width) / static_cast<float>(o.height)});
    entries_ = std::move(entries);
    outputs_ = std::move(outputs);
    logic_ = std::move(logic);
    settings_ = std::move(settings);
    checks_ = std::move(checks);
    meta_ = std::move(meta);
    version_ = version;
    output_cursor_ = 0;
    logic_cursor_ = 0;
    setting_cursor_ = 0;
    check_cursor_ = 0;
    missed_checks_ = 0;
    start_tick_ = start;
    stop_tick_ = stop;
    saved_tick_ = saved;
    width_ = width;
    height_ = height;
    cursor_ = 0;
    current_ = Samples{};
    current_[0].connected = true;  // the peripheral layer's start state (ManagerImpPc::reset)
    late_entries_ = 0;
    playing_ = true;
    recording_ = false;
    return true;
}

bool InputTape::output_at(std::uint64_t tick, int* width, int* height) {
    if (!playing_) return false;
    bool due = false;
    while (output_cursor_ < outputs_.size() && outputs_[output_cursor_].tick <= tick) {
        const Output& o = outputs_[output_cursor_++];
        *width = o.width;
        *height = o.height;
        due = true;
    }
    return due;
}

bool InputTape::logic_at(std::uint64_t tick, float* aspect) {
    if (!playing_) return false;
    bool due = false;
    while (logic_cursor_ < logic_.size() && logic_[logic_cursor_].tick <= tick) {
        *aspect = logic_[logic_cursor_++].aspect;
        due = true;
    }
    return due;
}

std::vector<InputTape::Setting> InputTape::settings_at(std::uint64_t tick) {
    std::vector<Setting> due;
    if (!playing_) return due;
    while (setting_cursor_ < settings_.size() && settings_[setting_cursor_].tick <= tick)
        due.push_back(settings_[setting_cursor_++]);
    return due;
}

bool InputTape::check_at(std::uint64_t tick, std::string* text) {
    if (!playing_) return false;
    while (check_cursor_ < checks_.size() && checks_[check_cursor_].tick < tick) {
        ++check_cursor_;
        ++missed_checks_;  // passed without a host service at its tick
    }
    if (check_cursor_ < checks_.size() && checks_[check_cursor_].tick == tick) {
        *text = checks_[check_cursor_++].text;
        return true;
    }
    return false;
}

void InputTape::samples_at(std::uint64_t tick, Samples& latched) {
    if (!playing_) return;
    if (cursor_ == 0u) current_ = latched;  // ports the tape never names keep what the state latched
    while (cursor_ < entries_.size() && entries_[cursor_].tick <= tick) {
        const Entry& e = entries_[cursor_++];
        if (e.tick != tick) ++late_entries_;
        current_[e.port] = e.sample;
    }
    latched = current_;
}

}  // namespace gekko
