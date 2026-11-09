#pragma once

// RenderWare's raster -> image conversion, owned natively (docs/agents/gx-rw-plan.md, rwR2 step 3; removal plan service
// graphics.rw-gamecube-driver).
//
// Original behaviour (main.dol, PAL GUPP8P): the GameCube driver registers GcnImageGetRaster 0x80493A40 as standard function 6 of the
// engine's device table (data word 0x8051D43C); RwImageSetFromRaster (0x8048A9F8) calls it. (image r3, raster r4) -> 1.
//   1. If the raster is not locked (private flags +0x22 bit 0x02 clear) it is locked for reading: RwRasterLock(raster, 0, 2) 0x8048AFC0.
//   2. If the raster format byte (+0x23) has a palette bit (0x20 or 0x40) and its palette is not locked (+0x22 bit 0x08 clear), the
//      palette is locked for reading: RwRasterLockPalette(raster, 2) 0x8048ACE4.
//   3. The image depth (+0x0C) picks the converter: 4 -> 0x80493118, 8 -> 0x8049336C, 32 -> 0x80493660; any other depth is a RenderWare
//      error in the original.
//   4. The palette is unlocked (0x8048ABD8) if step 2 locked it, the raster is unlocked (0x8048AB90) if step 1 locked it; r3 = 1.
// The converters read the raster (RwRaster: pixels +4, palette +8, width +0xC, height +0x10, depth +0x14, stride +0x18, format byte
// +0x23) and write the image (RwImage: width +4, height +8, depth +0xC, stride +0x10, pixels +0x14, palette +0x18) row by row:
//   image depth 4  : raster depth 4 only. The 16 raster palette halfwords become 16 RGBA entries of the image palette (below); the
//                    pixels are the index nibbles, two bytes per source byte (high nibble first), as many pairs a row as it takes to
//                    pass the raster width (an odd width writes one byte too many).
//   image depth 8  : raster depth 4 (as above, with the 16 palette entries) or raster depth 8 (256 palette entries, indices copied).
//   image depth 32 : raster depth 4 / 8: the palette entries become RGBA and each index is replaced by its entry (four bytes);
//                    raster depth 16 / 32: each halfword / word becomes RGBA through the pixel function.
// The pixel function (colour channels r, g, b, a stored as the four bytes) is picked by the raster format & 0x0F00:
//   0x100 1555, 0x200 565, 0x300 4444, 0x500 8888, 0x600 888, 0xA00 555 - the expansions of rw_pixel::pixel_to_rgb. Luminance 0x400
//   and every other value are a RenderWare error (RwErrorSet) in the original, as are the depth pairs not listed above.
//
// Native implementation: the same loops on guest memory. The four RenderWare lock/unlock functions are still the game's own, called
// through the guest-call seam (gekko/native_guest_call.h; each is RW core and does not block). The error branches stop the run with
// a diagnostic instead of reporting an RwError and carrying on with a null pixel function (the game never passes them: 0 calls on the
// Westopolis replay, and its images only come from the formats above).

#include "gekko/types.h"

#include <cstddef>

namespace gekko {
class Cpu;
}

namespace gekko::rw_image {

constexpr u32 kImageGetRaster = 0x80493A40u;

// Must equal this owner's rows of extract/native-owners.txt (native_file_api_tests).
struct Owners {
    struct Address {
        u32 address;
        const char* name;
    };
    static const Address* addresses(std::size_t& count) noexcept {
        static constexpr Address kAddresses[] = {{kImageGetRaster, "RwImageGetRaster (GameCube driver standard function 6)"}};
        count = sizeof(kAddresses) / sizeof(kAddresses[0]);
        return kAddresses;
    }
};

// (image r3, raster r4) -> r3 = 1. false: the refusal contract (the run is stopped with a diagnostic; nothing converted).
bool serve_image_get_raster(Cpu& cpu);

void install(Cpu& cpu);

}  // namespace gekko::rw_image
