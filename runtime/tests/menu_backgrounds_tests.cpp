// Tests of the full-height menu backgrounds (gekko/menu_backgrounds.h). With the game data (argument: sys/main.dol):
// each of the five retail layouts is rewritten in exactly the four bytes of its background's scaleY, 0.94 -> 1.0, and
// reads back at full height; Select Mode (already full height) and a damaged project are refused.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "gekko/menu_backgrounds.h"

namespace {

namespace mb = gekko::menu_backgrounds;
using u8 = std::uint8_t;

int g_failures = 0;

void check(bool condition, const std::string& what) {
    if (!condition) {
        std::printf("FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

bool read_file(const std::filesystem::path& path, std::vector<u8>& bytes) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

void screens(const std::filesystem::path& files) {
    for (const mb::Screen& screen : mb::kScreens) {
        const std::string name(screen.path);
        std::vector<u8> gncp, out;
        if (!read_file(files / name, gncp)) {
            check(false, name + " is readable");
            continue;
        }
        float before = 0.0f, after = 0.0f;
        check(mb::background_scale_y(gncp, screen.scene, before) && before == 0.94f,
              name + ": the retail background is at 0.94");
        std::string error;
        const bool filled = mb::fill(gncp, screen.scene, out, error);
        check(filled, name + ": filled: " + error);
        check(out.size() == gncp.size(), name + ": the project keeps its size");
        std::size_t changed = 0;
        for (std::size_t i = 0; i < gncp.size() && i < out.size(); ++i) changed += gncp[i] != out[i] ? 1u : 0u;
        check(changed >= 1u && changed <= 4u, name + ": only the scaleY word changes (" + std::to_string(changed) +
                                                  " bytes)");
        check(mb::background_scale_y(out, screen.scene, after) && after == 1.0f, name + ": the background is at 1.0");
        std::vector<u8> twice;
        check(!mb::fill(out, screen.scene, twice, error), name + ": a filled project is not filled again");
    }
    // Select Mode is already full height: its background is not the retail 0.94 one.
    std::vector<u8> select, out;
    std::string error;
    if (read_file(files / "csdFiles/SelectMode.gncp", select)) {
        float scale = 0.0f;
        check(mb::background_scale_y(select, "SelectMode", scale) && scale == 1.0f, "Select Mode is at 1.0");
        check(!mb::fill(select, "SelectMode", out, error), "Select Mode is refused");
    }
    // A scene that does not exist, and a project without its chunk.
    std::vector<u8> honban;
    if (read_file(files / "csdFiles/honban.gncp", honban)) {
        check(!mb::fill(honban, "NoSuchScene", out, error), "a missing scene is refused");
        check(!mb::fill(honban, "Menu", out, error), "a scene without the background cast is refused");
    }
    const std::vector<u8> junk(256, 0x5A);
    check(!mb::fill(junk, "Loop", out, error), "a project without nCPJ is refused");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1) {
        const std::filesystem::path files = std::filesystem::path(argv[1]).parent_path().parent_path() / "files";
        if (std::filesystem::exists(files / "csdFiles")) {
            screens(files);
        } else {
            std::printf("game data not found beside %s: the disc checks were skipped\n", argv[1]);
        }
    } else {
        std::printf("no game data given: the disc checks were skipped\n");
    }
    if (g_failures != 0) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("menu_backgrounds_tests: all passed\n");
    return 0;
}
