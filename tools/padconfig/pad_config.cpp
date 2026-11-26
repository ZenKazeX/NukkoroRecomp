#include "pad_config.h"

#include <cctype>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <system_error>
#include <vector>

#include "gekko/config_file.h"

namespace padconfig {

namespace sp = shadow::peripheral;

namespace {

struct ButtonName {
    std::uint16_t mask;
    const char* name;
};
constexpr ButtonName kButtons[] = {
    {sp::kGamepadSouth, "A"},           {sp::kGamepadEast, "B"},
    {sp::kGamepadWest, "X"},            {sp::kGamepadNorth, "Y"},
    {sp::kGamepadLeftShoulder, "LB"},   {sp::kGamepadRightShoulder, "RB"},
    {sp::kGamepadStart, "Start"},       {sp::kGamepadBack, "Back"},
    {sp::kGamepadLeftStick, "L3"},      {sp::kGamepadRightStick, "R3"},
    {sp::kGamepadDpadUp, "D-Pad Up"},   {sp::kGamepadDpadDown, "D-Pad Down"},
    {sp::kGamepadDpadLeft, "D-Pad Left"}, {sp::kGamepadDpadRight, "D-Pad Right"},
};

}  // namespace

std::string button_names(std::uint16_t mask) {
    std::string out;
    for (const ButtonName& b : kButtons) {
        if ((mask & b.mask) == 0u) continue;
        if (!out.empty()) out += " + ";
        out += b.name;
    }
    return out.empty() ? "Not set" : out;
}

std::uint16_t newly_pressed(std::uint16_t before, std::uint16_t now) noexcept {
    const auto fresh = static_cast<std::uint16_t>(now & ~before);
    for (const ButtonName& b : kButtons)
        if ((fresh & b.mask) != 0u) return b.mask;
    return 0u;
}

void assign(GamepadBinding& binding, std::size_t index, std::uint16_t mask) noexcept {
    const std::uint16_t old = binding.*kControls[index].field;
    if (old == mask) return;
    for (std::size_t i = 0; i < kControls.size(); ++i) {
        if (i == index) continue;
        std::uint16_t& other = binding.*kControls[i].field;
        if (mask == 0u || (other & mask) == 0u) continue;
        const auto rest = static_cast<std::uint16_t>(other & ~mask);
        other = rest != 0u ? rest : old;
    }
    binding.*kControls[index].field = mask;
}

bool same(const GamepadBinding& a, const GamepadBinding& b) noexcept {
    for (const Control& c : kControls)
        if (a.*c.field != b.*c.field) return false;
    return a.trigger_threshold == b.trigger_threshold && a.swap_sticks == b.swap_sticks && a.button_style == b.button_style;
}

std::string format_binding(const GamepadBinding& binding) {
    std::string out = "# Controller buttons for Shadow the Hedgehog. Written by Settings.exe.\n"
                      "# Values are controller button masks; 0 means not set.\n\n";
    char line[96];
    for (const Control& c : kControls) {
        const std::uint16_t mask = binding.*c.field;
        std::snprintf(line, sizeof(line), "%-12s = 0x%04X   # %s\n", c.key, static_cast<unsigned>(mask), button_names(mask).c_str());
        out += line;
    }
    std::snprintf(line, sizeof(line), "\ntrigger_threshold = %u\nswap_sticks       = %d\n",
                  static_cast<unsigned>(binding.trigger_threshold), binding.swap_sticks ? 1 : 0);
    out += line;
    out += "button_style      = ";
    out += sp::button_style_name(binding.button_style);
    out += "\n";
    return out;
}

LoadResult load_binding(const std::filesystem::path& path, GamepadBinding& binding, std::string* error) {
    binding = GamepadBinding{};
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return LoadResult::Absent;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) *error = "pad.cfg cannot be read";
        return LoadResult::Invalid;
    }
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    GamepadBinding read;
    if (!sp::parse_gamepad_binding(text, read, error)) return LoadResult::Invalid;
    binding = read;
    return LoadResult::Loaded;
}

std::string update_binding_text(const std::string& text, const GamepadBinding& old, const GamepadBinding& now) {
    std::vector<gekko::config_file::KeyValue> values;
    char value[16];
    for (const Control& c : kControls) {
        if (old.*c.field == now.*c.field) continue;
        std::snprintf(value, sizeof(value), "0x%04X", static_cast<unsigned>(now.*c.field));
        values.push_back({{c.key}, value});
    }
    if (old.trigger_threshold != now.trigger_threshold)
        values.push_back({{"trigger_threshold"}, std::to_string(static_cast<unsigned>(now.trigger_threshold))});
    if (old.swap_sticks != now.swap_sticks) values.push_back({{"swap_sticks"}, now.swap_sticks ? "1" : "0"});
    if (old.button_style != now.button_style) values.push_back({{"button_style"}, sp::button_style_name(now.button_style)});
    if (values.empty()) return text;
    const std::string written = gekko::config_file::rewrite(text, values);
    // A changed button's line keeps its comment; when that comment is the old button's name (as this program writes
    // it), it names the new one instead.
    std::string out;
    std::size_t begin = 0;
    while (begin < written.size()) {
        std::size_t end = written.find('\n', begin);
        const std::size_t next = end == std::string::npos ? written.size() : end + 1;
        if (end == std::string::npos) end = written.size();
        std::string line = written.substr(begin, end - begin);
        const std::size_t hash = line.find('#'), eq = line.find('=');
        if (hash != std::string::npos && eq != std::string::npos && eq < hash) {
            std::string key = line.substr(0, eq);
            key.erase(0, key.find_first_not_of(" \t"));
            key.erase(key.find_last_not_of(" \t") + 1);
            for (char& ch : key) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            for (const Control& c : kControls) {
                if (key != c.key || old.*c.field == now.*c.field) continue;
                std::size_t body_end = line.size();
                if (body_end > hash && line[body_end - 1] == '\r') --body_end;
                std::string comment = line.substr(hash + 1, body_end - hash - 1);
                comment.erase(0, comment.find_first_not_of(" \t"));
                comment.erase(comment.find_last_not_of(" \t") + 1);
                if (comment == button_names(old.*c.field))
                    line = line.substr(0, hash) + "# " + button_names(now.*c.field) + line.substr(body_end);
            }
        }
        out += line;
        out.append(written, end, next - end);
        begin = next;
    }
    return out;
}

bool save_binding(const std::filesystem::path& path, const GamepadBinding& binding, std::string* error) {
    // An existing file the game reads keeps its comments and other lines: only the keys whose value changes are
    // written. A missing file, or one the game refuses (it would not start with it), is written whole.
    std::string text = format_binding(binding);
    {
        std::error_code ec;
        if (std::filesystem::exists(path, ec)) {
            std::ifstream in(path, std::ios::binary);
            const std::string current{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
            GamepadBinding old;
            if (in && sp::parse_gamepad_binding(current, old, nullptr)) text = update_binding_text(current, old, binding);
        }
    }
    GamepadBinding check;
    std::string parse_error;
    if (!sp::parse_gamepad_binding(text, check, &parse_error) || !same(check, binding)) {
        if (error) *error = "the game could not read this mapping back: " + parse_error;
        return false;
    }
    std::filesystem::path temporary = path;
    temporary += ".new";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.flush();
        if (!out) {
            if (error) *error = "pad.cfg could not be written";
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        std::filesystem::remove(temporary, ec);
        if (error) *error = "pad.cfg could not be replaced. Is it open somewhere else?";
        return false;
    }
    return true;
}

}  // namespace padconfig
