#pragma once

// RenderWare's colour <-> raster pixel conversion, owned natively (docs/agents/gx-rw-plan.md, rwR2 step 1; removal plan
// service graphics.rw-gamecube-driver).
//
// Original behaviour (main.dol, PAL GUPP8P): the GameCube driver registers two standard functions of the engine's device
// table (RwEngineInstance standard slots 2 and 3):
//   RGBTOPIXEL 0x80492BFC (RwUInt32* out, const RwRGBA* colour, RwInt32 rasterFormat) -> 1
//   PIXELTORGB 0x80492F10 (RwRGBA* out, const RwUInt32* pixel, RwInt32 rasterFormat) -> 1
// Both switch on rasterFormat & 0x0F00 (the pixel format of RenderWare's rwRASTERFORMAT*):
//   0x000 default and 0x200 565    R5 G6 B5:        p = R<<8 & 0xF800 | G<<3 & 0x07E0 | B>>3
//   0x100 1555 and 0x300 4444      GX RGB5A3:       an opaque colour (A == 0xFF) is 1AAAARRRRRGGGGGBBBBB-style 0x8000 | R<<7 & 0x7C00 |
//                                                   G<<2 & 0x03E0 | B>>3; any other alpha is 0AAARRRRGGGGBBBB: A<<7 & 0x7000 |
//                                                   R<<4 & 0x0F00 | G & 0x00F0 | B>>4
//   0xA00 555                      0x8000 | R<<7 & 0x7C00 | G<<2 & 0x03E0 | B>>3
//   0x500 8888                     A<<24 | R<<16 | G<<8 | B
//   0x600 888                      0xFF000000 | R<<16 | G<<8 | B
// and the inverse for PIXELTORGB, which reads the pixel as a 32-bit word (the value sits in its low bits):
//   0x000 / 0x200  R = p>>8 & 0xF8, G = p>>3 & 0xFC, B = p<<3 & 0xF8, A = 0xFF
//   0x100 / 0x300  bit 15 set: R = p>>7 & 0xF8, G = p>>2 & 0xF8, B = p<<3 & 0xF8, A = 0xFF; clear: R = p>>4 & 0xF0, G = p & 0xF0,
//                  B = p<<4 & 0xF0, and A = 0 for 0x100 (the original's 1555 branch) but p>>7 & 0xE0 for 0x300
//   0xA00          R = p>>7 & 0xF8, G = p>>2 & 0xF8, B = p<<3 & 0xF8, A = 0xFF
//   0x500          A = p>>24, R = p>>16, G = p>>8, B = p (bytes)       0x600  the same with A = 0xFF
// The colour is stored as the four bytes R, G, B, A. 0x400 (luminance 8) and every other value of the field raise a RenderWare
// error in the original (RwErrorSet through 0x8047EC78 / 0x8047EBD4) and store an uninitialised stack word.
//
// Native implementation: the same arithmetic on guest memory. The error branches stop the run with a diagnostic instead of
// reporting an RwError and storing stack garbage (the game never passes them: 0 calls on the Westopolis replay, and its textures
// only use the formats above).

#include "gekko/types.h"

#include <cstddef>

namespace gekko {
class Cpu;
}

namespace gekko::rw_pixel {

constexpr u32 kRgbToPixel = 0x80492BFCu;
constexpr u32 kPixelToRgb = 0x80492F10u;
constexpr u32 kImageFindRasterFormat = 0x804948D8u;  // standard function 9

// Must equal this owner's rows of extract/native-owners.txt (native_file_api_tests).
struct Owners {
    struct Address {
        u32 address;
        const char* name;
    };
    static const Address* addresses(std::size_t& count) noexcept {
        static constexpr Address kAddresses[] = {{kRgbToPixel, "RwRGBToPixel (GameCube driver standard function 2)"},
                                                 {kPixelToRgb, "RwPixelToRGB (GameCube driver standard function 3)"},
                                                 {kImageFindRasterFormat, "RwImageFindRasterFormat (GameCube driver standard function 9)"}};
        count = sizeof(kAddresses) / sizeof(kAddresses[0]);
        return kAddresses;
    }
};

struct Rgba {
    u8 r, g, b, a;
};

// The pure conversions. false: a pixel format the original reports as an error (0x400 and the values it does not list).
bool rgb_to_pixel(const Rgba& colour, u32 raster_format, u32& pixel) noexcept;
bool pixel_to_rgb(u32 pixel, u32 raster_format, Rgba& colour) noexcept;

// One guest call each (r3, r4, r5 as above; r3 = 1 on return). false: the error branch (nothing written).
bool serve_rgb_to_pixel(Cpu& cpu);
bool serve_pixel_to_rgb(Cpu& cpu);

// RwImageFindRasterFormat (standard function 9): (RwRaster* raster, const RwImage* image, RwInt32 flags) -> 1.
// Fills the raster's description from the image, as the original does:
//   raster->cType = flags & 7, raster->depth = 0;
//   type 0, 4, 5 (normal / texture / camera texture): width = min(image width, 0x400), height likewise; with flags & 0x8000 each is
//     rounded down to a power of two; cFormat = (scan | flags & 0x9000) >> 8 where scan is the image's alpha scan below;
//   type 1, 2 (Z buffer / camera): width and height are the image's, cFormat = 0;
//   type 3 and 6, 7: a RenderWare error in the original (stop here, nothing written but cType and depth).
// The alpha scan (0x804947AC): a palette image (depth 4 or 8) looks up each pixel's palette alpha, any other depth reads each pixel's
// fourth byte; alpha 0xFF is opaque, 0x00..0x0F is a "cut-out" alpha (marks the image translucent), any other alpha makes the
// answer 0x300 at once (with 0x4000 for depth 4 and 0x2000 for depth 8 when palettised); otherwise 0x200 (all opaque) or 0x100
// (cut-out), with the palette bit (0x4000 depth 4, 0x2000 depth 8) added.
// false: the error types (the owner stops the run).
bool serve_image_find_raster_format(Cpu& cpu);

void install(Cpu& cpu);

}  // namespace gekko::rw_pixel
