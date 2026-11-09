#pragma once

// GAME-LEVEL OWNERS (NE-RW1, HW_REPORT W.2; NATIVE_AUDIT.md C14). Functions of the GAME itself - not of RenderWare and
// not of the Nintendo SDK - owned natively in the lifted build (extract/native-owners-r2.txt): they read and write the
// game's own objects and reach RenderWare only through game::RwApi. They used to live inside the RenderWare GameCube
// driver scaffold, which is never extended with capability and only shrinks.
//
//   Weapon::LockonMark's draw (0x800620D4): the target projected as the game's projection gives it; drawn while the
//     target is in front and its mark within 32 canvas units of the screen the output shows; two additive textured
//     quads - the inner mark 64 units square, the ring scaled by the mark's ring scale - Projected; the render states
//     set and restored as the game's code does. Calls no guest code.
//   The lens flare's builder (fn_80432360): the flare and its camera read out of the effect library's objects, laid
//     out by runtime/lift/effects/lens_flare.h against the view the output shows, written back for the game's draw.
//   The watch camera's noise sheet scale (WatchCamera::Monitor fn_80268660's call of fn_8040C770, a lowered site in
//     extract/lift-sites-r2.txt): the sheet sized by runtime/lift/effects/watch_camera_sheet.h to the picture the view
//     displays instead of the camera's authored window, then fn_8040C770's scale of its matrix.

#include <cstdint>
#include <vector>

#include "effects/lens_flare.h"
#include "game/rw_api.h"
#include "gekko/types.h"

namespace gekko {
class Cpu;
}

namespace gekko::game {

class GameOwners {
public:
    struct Stats {
        // The lock-on marks served, drawn, and hidden (behind the eye or more than their margin off the shown screen).
        u64 lockon_marks = 0, lockon_marks_drawn = 0, lockon_marks_hidden = 0;
        // The lens flare: builds, the sun in front of the camera, ghosts / glow shown, ghosts shown with the sun past
        // the raster's own square (where the console's test hid them), builds declined because the installed effect
        // system or the table was not the shipped kind.
        u64 flare_builds = 0, flare_in_front = 0, flare_ghosts_shown = 0, flare_glow_shown = 0, flare_beyond_raster = 0,
            flare_unknown_system = 0;
        // The watch camera's noise sheet: scales served, scales other than the game's (a view not at the authored
        // aspect), and scales where the game's own arithmetic (the f1, f2 it passed) differs from its reconstruction
        // here on the window it read - 0 unless the reconstruction is wrong.
        u64 watch_sheets = 0, watch_sheets_resized = 0, watch_sheet_game_mismatches = 0;
    };

    explicit GameOwners(RwApi& rw);
    GameOwners(const GameOwners&) = delete;
    GameOwners& operator=(const GameOwners&) = delete;

    // The owners the lowered calls reach: one per process, installed before guest code runs.
    void install() noexcept;
    void uninstall() noexcept;
    static GameOwners* installed() noexcept;

    bool lockon_mark_draw(Cpu& c);
    bool lens_flare_build(Cpu& c);
    // fn_8040C770(matrix, sx, sy, sz) at 0x8026872C: `game_x`, `game_y` are the game's sheet size of the camera's own
    // window; the matrix is scaled by the displayed picture's.
    void watch_camera_sheet_scale(Cpu& c, u32 matrix, double game_x, double game_y, double sz);
    const Stats& stats() const noexcept { return stats_; }

private:
    float read_f32(Cpu& c, u32 address);
    RwApi& rw_;
    Stats stats_;
    std::vector<shadow::effects::FlareElement> flare_elements_;  // reused frame to frame
    std::vector<shadow::effects::FlareVertex> flare_ghosts_;
    // Diagnostics: SHADOW_FLARE_TRACE=<n> prints the first n builds' decisions (stderr); SHADOW_FLARE_CONSOLE_SCREEN=1
    // tests the camera raster as the console's builder did (a reference baseline; lens_flare_tests proves that layout
    // equals the console's).
    std::uint64_t trace_flare_ = 0;
    bool flare_console_screen_ = false;
    // SHADOW_WATCH_SHEET_TRACE=<n> prints the first n sheet scales (stderr); SHADOW_WATCH_SHEET_GAME=1 sizes the sheet
    // to the camera's authored window, as the console did (the before / after baseline).
    std::uint64_t trace_watch_sheet_ = 0;
    bool watch_sheet_game_ = false;
};

// The entries the HLE table calls (LiftedRender::install_owners).
bool lockon_mark_draw(Cpu& c);
bool lens_flare_build(Cpu& c);

}  // namespace gekko::game
