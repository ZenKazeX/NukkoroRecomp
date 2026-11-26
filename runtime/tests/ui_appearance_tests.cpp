// appearance.cfg (tools/ui/appearance.h): the theme and zoom the settings program and the mod manager share.
// Checks the defaults, what a file is read as, that a rewrite keeps every other line, comment and line ending, the
// text size steps and their limits, and that the file is written only when something changes.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "appearance.h"
#include "test_scratch.h"

namespace {

namespace fs = std::filesystem;
using ui::Appearance;
using ui::Theme;

int failures = 0, checks = 0;
void check(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", what);
    }
}

std::string read_all(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void reading() {
    const Appearance none = ui::parse_appearance("");
    check(none.theme == Theme::System && none.text_size == 100, "no file: the colours follow Windows, normal text size");
    const Appearance a = ui::parse_appearance("theme = dark\nzoom = 150\n");
    check(a.theme == Theme::Dark && a.text_size == 150, "theme and zoom are read");
    check(ui::parse_appearance("  THEME=Light # mine\r\n").theme == Theme::Light, "any case, spaces, comments and CRLF");
    check(ui::parse_appearance("theme = purple\n").theme == Theme::System, "an unknown theme leaves the default");
    check(ui::parse_appearance("zoom = big\n").text_size == 100, "a text size that is not a number is ignored");
    check(ui::parse_appearance("zoom = 150%\n").text_size == 100, "nor one with other characters");
    check(ui::parse_appearance("zoom = 900\n").text_size == 200, "a text size above 200 becomes 200");
    check(ui::parse_appearance("zoom = 10\n").text_size == 100, "below 100 becomes 100");
    check(ui::parse_appearance("theme = dark\ntheme = light\n").theme == Theme::Light, "the last line of a key wins");
    check(ui::parse_appearance("theme = Shadow\n").theme == Theme::Shadow, "the Shadow theme is read");
    check(ui::update_appearance("", Appearance{Theme::Shadow, 100}).find("theme = shadow\n") != std::string::npos, "and written");
    check(ui::parse_appearance("# theme = dark\nvsync = true\n").theme == Theme::System, "comments and other keys are not read");
}

void writing() {
    const std::string fresh = ui::update_appearance("", Appearance{Theme::Dark, 125});
    check(fresh.rfind("# ", 0) == 0, "a new file starts with a comment saying what it is");
    check(fresh.find("theme = dark\n") != std::string::npos && fresh.find("zoom = 125\n") != std::string::npos,
          "and holds both keys");
    check(ui::parse_appearance(fresh) == Appearance{Theme::Dark, 125}, "and reads back the same");

    const std::string mine = "# my notes\r\ntheme = light   # was light\r\nunknown = 7\r\n";
    const std::string updated = ui::update_appearance(mine, Appearance{Theme::Dark, 175});
    check(updated == "# my notes\r\ntheme = dark\r\nunknown = 7\r\nzoom = 175\r\n",
          "the theme line is rewritten, other lines and CRLF kept, the missing key added");
    check(ui::update_appearance("theme = dark", Appearance{Theme::Dark, 100}) == "theme = dark\nzoom = 100\n",
          "a last line without a line break gets one before the added key");
    check(ui::update_appearance(updated, Appearance{Theme::Dark, 175}) == updated, "the same settings change nothing");
    check(ui::update_appearance("zoom = 130\n", Appearance{Theme::System, 130}).find("zoom = 125") != std::string::npos,
          "a size between the steps is written as the nearest step");
}

void steps() {
    check(ui::step_text_size(100, 1) == 125, "one step larger");
    check(ui::step_text_size(150, -1) == 125, "one step smaller");
    check(ui::step_text_size(200, 1) == 200, "never above 200");
    check(ui::step_text_size(100, -1) == 100, "never below 100");
    check(ui::step_text_size(112, 0) == 100 && ui::step_text_size(113, 0) == 125, "0 steps snaps to the nearest size");
    check(ui::step_text_size(100, 10) == 200, "many steps stop at the end");
}

void files(const fs::path& dir) {
    check(ui::load_appearance(dir) == Appearance{}, "no appearance.cfg: the defaults");
    check(!fs::exists(dir / "appearance.cfg"), "reading does not create the file");
    check(ui::save_appearance(dir, Appearance{Theme::Light, 150}), "saving creates it");
    check(ui::load_appearance(dir) == Appearance{Theme::Light, 150}, "and it reads back");
    check(!fs::exists(dir / "appearance.cfg.new"), "no temporary file is left");

    std::ofstream(dir / "appearance.cfg", std::ios::binary | std::ios::trunc) << "# kept\ntheme = light\nzoom = 150\nextra = 1\n";
    const auto before = fs::last_write_time(dir / "appearance.cfg");
    check(ui::save_appearance(dir, Appearance{Theme::Light, 150}), "saving the same settings succeeds");
    check(fs::last_write_time(dir / "appearance.cfg") == before, "without writing the file");
    check(ui::save_appearance(dir, Appearance{Theme::Dark, 150}), "a change is saved");
    check(read_all(dir / "appearance.cfg") == "# kept\ntheme = dark\nzoom = 150\nextra = 1\n",
          "with only the changed line different");
    check(!ui::save_appearance(dir / "no-such-folder", Appearance{Theme::Dark, 100}), "a folder that is missing is reported");
}

}  // namespace

int main() {
    const fs::path dir = gekko::test_support::scratch_directory("ui-appearance-tests");
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    reading();
    writing();
    steps();
    files(dir);
    fs::remove_all(dir, ec);
    if (failures != 0) {
        std::printf("%d of %d checks failed\n", failures, checks);
        return 1;
    }
    std::printf("PASS ui appearance (%d checks)\n", checks);
    return 0;
}
