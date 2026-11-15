// Mod file overlay for host-side readers (gekko/mod_overlay.h).
#include "gekko/mod_overlay.h"

#include <cctype>
#include <map>

namespace gekko::mod_overlay {
namespace {

std::map<std::string, std::filesystem::path, std::less<>> g_files;  // lower-case disc path -> host file

std::string folded(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

}  // namespace

void set(const std::vector<std::pair<std::string, std::filesystem::path>>& files) {
    g_files.clear();
    for (const auto& [path, host] : files) g_files[folded(path)] = host;
}

bool active() noexcept { return !g_files.empty(); }

const std::filesystem::path* find(std::string_view game_path) noexcept {
    if (g_files.empty()) return nullptr;
    const auto it = g_files.find(folded(game_path));
    return it == g_files.end() ? nullptr : &it->second;
}

}  // namespace gekko::mod_overlay
