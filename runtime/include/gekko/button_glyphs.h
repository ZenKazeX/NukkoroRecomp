#pragma once

// Modern controller buttons in the game's prompts (user request of 2026-10-29: the new button layout, the game
// telling which controller is connected, and a manual choice).
//
// Original behaviour: every on-screen button is a cell of a paletted (C8 / C4, RGB5A3 palette) texture in the
// game's files - the front-end gamemenu sheet (select / exit), the pause screen, the Chaos Control / Chaos Blast
// prompt, the weapon window, the vehicle arrow, the cutscene skip and the B-button window; 27 distinct sheets in
// 187 files on the PAL disc (exports/button-layouts/README.md). The front-end and HUD casts hold a PS2, a GameCube
// and an Xbox pattern each; the GameCube disc draws the GameCube one.
//
// Native implementation: the sheets are rewritten as the game reads them (AssetSystem's read patch), so the game
// loads, lays out and animates the same casts and only the texels differ. For each sheet and family a palette was
// fitted offline (source/tools/button_layouts/make_button_glyph_data.py -> button_glyph_data.inc) that keeps every
// pixel outside the button cells and holds the new caps; a cell that means a GameCube button gets the modern
// button the player's binding (pad.cfg) puts that GameCube button on, by position (south / east / west / north).
// A sheet is recognised by the hash of its bytes, so a file this table does not know is never touched.
//
// The family: pad.cfg button_style (or --button-style); original keeps the disc's art. Auto reads the USB vendor of the connected game
// controllers (Raw Input; works with no window and behind XInput mappers, which leave the real device listed):
// Sony -> PlayStation, Nintendo -> Switch, anything else or nothing -> Xbox. It is re-read at most once a second
// as files load, so a controller change shows from the next screen or stage that loads its textures; textures
// already loaded keep the buttons they were read with.
//
// The sharp sheets (user, 2026-10-29: the checkpoint's button was grey, and "increase the resolution of the
// buttons by 20%"): a 16-colour sheet (the vehicle arrow, the weapon window) cannot hold a modern cap's colours,
// and a cell holds few pixels. So each patched sheet also has a full-colour RGBA8 version, the sheet scaled by
// kSharpNum / kSharpDen (5/4: every sheet edge stays a whole pixel) with the caps painted at that size, handed to
// the sharp sink keyed by the patched texture's content; the native renderer serves it in place of the paletted
// texture (NativeAssetManager::pin_texture). The paletted patch stays what the game reads and what any other
// renderer draws.

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <unordered_map>
#include <vector>

#include "gekko/asset_runtime.h"
#include "gekko/types.h"
#include "peripheral/gamepad_binding.h"

namespace gekko::assets {
class AssetSystem;
}

namespace gekko::button_glyphs {

using shadow::peripheral::ButtonStyle;

enum class Family : u8 { Original, Xbox, PlayStation, Switch };  // Original: the disc's art
const char* family_name(Family family) noexcept;

// The family a set of connected game controllers' USB vendor ids calls for.
Family family_of_vendors(const std::vector<u16>& vendors) noexcept;
// The USB vendor ids of the game controllers (HID usage page 1, joystick or gamepad) connected now; empty
// where the platform cannot tell.
std::vector<u16> connected_controller_vendors();

class ButtonGlyphs {
public:
    // The style and the binding the cells follow. Before attach().
    void configure(ButtonStyle style, const shadow::peripheral::GamepadBinding& binding);

    // After the mount: finds the files that hold a button sheet, keeps each sheet's original bytes (verified by
    // hash), and installs the read patch. Original installs nothing. Returns the files that will be patched.
    u32 attach(assets::AssetSystem& assets);

    // The read patch: rewrites the button sheets inside [offset, offset + length) of asset `id` in `dest`.
    void patch(assets::AssetId id, u64 offset, u8* dest, u64 length);

    // The family in effect: the style's, or for Auto the connected controllers' (re-read at most once a second).
    Family family();

    struct Report {
        u32 files = 0;            // files holding a known sheet
        u32 sheets = 0;           // distinct sheets among them
        u32 mismatched = 0;       // listed files whose bytes are not the sheet's (another region): left alone
        u64 patched_reads = 0;    // reads that carried part of a sheet
        u32 family_changes = 0;   // Auto: times the family changed after the first
        u32 sharp_served = 0;     // sharp sheets the sink took
        Family family = Family::Original;
    };
    const Report& report() const noexcept { return report_; }

    // Tests: the sheet bytes the game would read for table sheet `sheet` in `family` (empty: unknown).
    const std::vector<u8>& patched_span(u32 sheet, Family family);

    // A patched sheet as the renderer sees it (the raster header and the patched palette and texels, which name
    // it) and its sharp version.
    struct SharpSheet {
        u32 gx_format = 0, tlut_format = 0, depth = 0, width = 0, height = 0;
        const u8* texels = nullptr;
        std::size_t texel_bytes = 0;
        const u8* palette = nullptr;
        std::size_t palette_bytes = 0;
        u32 sharp_width = 0, sharp_height = 0;
        std::vector<u8> rgba8;  // sharp_width * sharp_height * 4, rows top first, R G B A
    };
    // Takes a sharp sheet; false when it cannot be kept now (asked again on the sheet's next read).
    using SharpSink = std::function<bool(SharpSheet&& sheet)>;
    void set_sharp_sink(SharpSink sink) { sharp_sink_ = std::move(sink); }
    // Tests: the sharp sheet of table sheet `sheet` in `family` (false: unknown or not a modern family).
    bool sharp_sheet(u32 sheet, Family family, SharpSheet& out);
    // Tests: the table sheet a file's span belongs to, and the span's offset (false: not a listed file).
    static bool table_sheet_of(const char* path, u32& sheet, u64& span_at);

private:
    struct Hit {
        u32 sheet;
        u64 at;  // the span's offset in the file
    };
    struct RasterHeader {
        u32 gx_format = 0, tlut_format = 0, depth = 0;
    };
    ButtonStyle style_ = ButtonStyle::Original;
    std::array<u8, 4> position_of_{};  // GC a, b, x, y -> 0 south, 1 east, 2 west, 3 north
    std::unordered_map<u32, std::vector<Hit>> files_;          // AssetId value -> sheets in it
    std::vector<std::vector<u8>> original_;                    // per table sheet (empty: not on this disc)
    std::vector<std::array<std::vector<u8>, 3>> patched_;      // per sheet, per modern family
    std::vector<RasterHeader> header_;                         // per sheet: the 16 bytes before its palette
    std::vector<std::array<bool, 3>> served_;                  // per sheet, per family: the sink took it
    // Per sheet: the disc's own art decoded and scaled to the sharp size (empty: not on the disc or not decodable).
    // It does not depend on the family or the binding, so attach() starts it on a worker thread and the first read
    // of a sheet on the game thread finds it made (docs/GPU_FILL.md "stage start": it was 27 % of a 17 ms frame).
    std::shared_future<std::vector<std::vector<u8>>> scaled_bases_;
    SharpSink sharp_sink_;
    Family detected_ = Family::Xbox;
    bool detected_once_ = false;
    std::chrono::steady_clock::time_point next_detect_{};
    Report report_;
};

}  // namespace gekko::button_glyphs
