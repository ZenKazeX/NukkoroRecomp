// Test harness for format_checks_snippet.inc: the snippet is included unchanged into a stand-in for afs.cpp's anonymous
// namespace (le32, lower as in that file), then every file below the given folders is checked and printed as
//   <game path> TAB OK|ERR [TAB reason]
// so the output can be diffed against `check_formats.py list` (same format, no reason column).
//   clang++ -std=c++20 -O1 tools/mods/format_checks_test.cpp -o format_checks_test.exe
//   format_checks_test.exe [--why] <dir> [<dir> ...]
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace shadow::mods {

namespace fs = std::filesystem;

namespace {

std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 |
           static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

#include "format_checks_snippet.inc"

}  // namespace

}  // namespace shadow::mods

int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    bool why_column = false;
    std::vector<std::string> lines;
    for (int a = 1; a < argc; ++a) {
        if (std::string_view(argv[a]) == "--why") {
            why_column = true;
            continue;
        }
        const fs::path root = argv[a];
        std::error_code ec;
        for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
            if (it->is_directory() && !it->path().filename().string().empty() && it->path().filename().string()[0] == '.') {
                it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file()) continue;
            std::string ext = shadow::mods::lower(it->path().extension().string());
            if (!shadow::mods::is_data_kind(ext)) continue;
            std::string rel = fs::relative(it->path(), root).generic_string();
            std::string why;
            const bool ok = shadow::mods::check_data_file(it->path(), ext, rel, it->file_size(), why);
            lines.push_back(rel + "\t" + (ok ? "OK" : "ERR") + (why_column && !ok ? "\t" + why : ""));
        }
    }
    std::sort(lines.begin(), lines.end());
    for (const std::string& l : lines) std::printf("%s\n", l.c_str());
    return 0;
}
