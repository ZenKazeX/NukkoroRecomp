// The native owner of the GameCube driver's raster -> image standard function (gekko/native_rw_image.h).

#include "gekko/native_rw_image.h"

#include "gekko/cpu.h"
#include "gekko/native_guest_call.h"
#include "gekko/native_rw_pixel.h"

#include <array>
#include <cstdio>

namespace gekko::rw_image {

namespace {

// RenderWare core called through the seam (each one returns).
constexpr u32 kRasterLock = 0x8048AFC0u;
constexpr u32 kRasterLockPalette = 0x8048ACE4u;
constexpr u32 kRasterUnlockPalette = 0x8048ABD8u;
constexpr u32 kRasterUnlock = 0x8048AB90u;
// Return sites inside the original function (the instruction after each of its calls): never fetched.
constexpr u32 kAfterLock = 0x80493A88u;           // liveness-roots: value-only
constexpr u32 kAfterLockPalette = 0x80493AB0u;    // liveness-roots: value-only
constexpr u32 kAfterUnlockPalette = 0x80493B40u;  // liveness-roots: value-only
constexpr u32 kAfterUnlock = 0x80493B50u;         // liveness-roots: value-only

// RwRaster
constexpr u32 kRasterPixels = 0x04u, kRasterPalette = 0x08u, kRasterWidth = 0x0Cu, kRasterHeight = 0x10u, kRasterDepth = 0x14u,
              kRasterStride = 0x18u, kRasterPrivate = 0x22u, kRasterFormat = 0x23u;
// RwImage
constexpr u32 kImageDepth = 0x0Cu, kImageStride = 0x10u, kImagePixels = 0x14u, kImagePalette = 0x18u;

bool fail(Cpu& c, const char* why) {
    char detail[160];
    std::snprintf(detail, sizeof(detail), "RwImageGetRaster: %s (the original reports an RwError)", why);
    c.request_stop(StopReason::ExecutionFault, detail);
    return false;
}

// The pixel function of the raster format byte, as the converters pick it. false: the format the original rejects.
bool pixel_format(Cpu& c, u32 raster, u32& format) {
    format = (u32{c.mem_read8(raster + kRasterFormat)} << 8) & 0x0F00u;
    switch (format) {
    case 0x100u: case 0x200u: case 0x300u: case 0x500u: case 0x600u: case 0xA00u: return true;
    default: return false;
    }
}

rw_pixel::Rgba colour(u32 format, u32 pixel) {
    rw_pixel::Rgba rgba{};
    rw_pixel::pixel_to_rgb(pixel, format, rgba);
    return rgba;
}

void put(Cpu& c, u32 at, const rw_pixel::Rgba& v) {
    c.mem_write8(at, v.r);
    c.mem_write8(at + 1u, v.g);
    c.mem_write8(at + 2u, v.b);
    c.mem_write8(at + 3u, v.a);
}

// The raster palette halfword i as RGBA.
rw_pixel::Rgba palette_entry(Cpu& c, u32 raster, u32 format, u32 i) {
    return colour(format, c.mem_read16(c.mem_read32(raster + kRasterPalette) + i * 2u));
}

// Index rows of a raster of depth 4 to index bytes: two bytes a source byte, high nibble first.
void unpack_nibbles(Cpu& c, u32 image, u32 raster) {
    const s32 width = static_cast<s32>(c.mem_read32(raster + kRasterWidth)), height = static_cast<s32>(c.mem_read32(raster + kRasterHeight));
    for (s32 y = 0; y < height; ++y) {
        u32 src = c.mem_read32(raster + kRasterPixels) + c.mem_read32(raster + kRasterStride) * static_cast<u32>(y);
        u32 dst = c.mem_read32(image + kImagePixels) + c.mem_read32(image + kImageStride) * static_cast<u32>(y);
        for (s32 x = 0; x < width; x += 2, ++src, dst += 2u) {
            c.mem_write8(dst, static_cast<u8>(c.mem_read8(src) >> 4));
            c.mem_write8(dst + 1u, static_cast<u8>(c.mem_read8(src) & 0x0Fu));
        }
    }
}

void copy_bytes(Cpu& c, u32 image, u32 raster) {
    const s32 width = static_cast<s32>(c.mem_read32(raster + kRasterWidth)), height = static_cast<s32>(c.mem_read32(raster + kRasterHeight));
    for (s32 y = 0; y < height; ++y) {
        u32 src = c.mem_read32(raster + kRasterPixels) + c.mem_read32(raster + kRasterStride) * static_cast<u32>(y);
        u32 dst = c.mem_read32(image + kImagePixels) + c.mem_read32(image + kImageStride) * static_cast<u32>(y);
        for (s32 x = 0; x < width; ++x, ++src, ++dst) c.mem_write8(dst, c.mem_read8(src));
    }
}

// Image depth 4 (0x80493118) and 8 (0x8049336C): index images.
bool to_index_image(Cpu& c, u32 image, u32 raster, u32 image_depth) {
    u32 format = 0;
    if (!pixel_format(c, raster, format)) return fail(c, "raster format without a pixel function");
    const u32 depth = c.mem_read32(raster + kRasterDepth);
    if (depth == 4u || (image_depth == 8u && depth == 8u)) {
        const u32 entries = depth == 4u ? 16u : 256u;
        for (u32 i = 0; i < entries; ++i) put(c, c.mem_read32(image + kImagePalette) + i * 4u, palette_entry(c, raster, format, i));
        if (depth == 4u) unpack_nibbles(c, image, raster);
        else copy_bytes(c, image, raster);
        return true;
    }
    return fail(c, "raster depth the index image cannot take");
}

// Image depth 32 (0x80493660).
bool to_rgba_image(Cpu& c, u32 image, u32 raster) {
    u32 format = 0;
    if (!pixel_format(c, raster, format)) return fail(c, "raster format without a pixel function");
    const u32 depth = c.mem_read32(raster + kRasterDepth);
    const s32 width = static_cast<s32>(c.mem_read32(raster + kRasterWidth)), height = static_cast<s32>(c.mem_read32(raster + kRasterHeight));
    if (depth != 4u && depth != 8u && depth != 16u && depth != 32u) return fail(c, "raster depth the RGBA image cannot take");
    std::array<rw_pixel::Rgba, 256> table{};
    if (depth == 4u || depth == 8u)
        for (u32 i = 0; i < (depth == 4u ? 16u : 256u); ++i) table[i] = palette_entry(c, raster, format, i);
    for (s32 y = 0; y < height; ++y) {
        u32 src = c.mem_read32(raster + kRasterPixels) + c.mem_read32(raster + kRasterStride) * static_cast<u32>(y);
        u32 dst = c.mem_read32(image + kImagePixels) + c.mem_read32(image + kImageStride) * static_cast<u32>(y);
        switch (depth) {
        case 4u:
            for (s32 x = 0; x < width; x += 2, ++src, dst += 8u) {
                put(c, dst, table[c.mem_read8(src) >> 4]);
                put(c, dst + 4u, table[c.mem_read8(src) & 0x0Fu]);
            }
            break;
        case 8u:
            for (s32 x = 0; x < width; ++x, ++src, dst += 4u) put(c, dst, table[c.mem_read8(src)]);
            break;
        case 16u:
            for (s32 x = 0; x < width; ++x, src += 2u, dst += 4u) put(c, dst, colour(format, c.mem_read16(src)));
            break;
        default:
            for (s32 x = 0; x < width; ++x, src += 4u, dst += 4u) put(c, dst, colour(format, c.mem_read32(src)));
            break;
        }
    }
    return true;
}

}  // namespace

bool serve_image_get_raster(Cpu& c) {
    const u32 image = c.gpr[3], raster = c.gpr[4];
    u32 ignored = 0;
    bool locked = false, palette_locked = false;
    if ((c.mem_read8(raster + kRasterPrivate) & 0x02u) == 0u) {
        if (!guest_call(c, kRasterLock, kAfterLock, {raster, 0u, 2u}, ignored)) return false;
        locked = true;
    }
    if (((u32{c.mem_read8(raster + kRasterFormat)} << 8) & 0x6000u) != 0u && (c.mem_read8(raster + kRasterPrivate) & 0x08u) == 0u) {
        if (!guest_call(c, kRasterLockPalette, kAfterLockPalette, {raster, 2u}, ignored)) return false;
        palette_locked = true;
    }
    const u32 depth = c.mem_read32(image + kImageDepth);
    bool ok = false;
    switch (depth) {
    case 4u: case 8u: ok = to_index_image(c, image, raster, depth); break;
    case 32u: ok = to_rgba_image(c, image, raster); break;
    default: return fail(c, "image depth other than 4, 8 or 32");
    }
    if (!ok) return false;
    if (palette_locked && !guest_call(c, kRasterUnlockPalette, kAfterUnlockPalette, {raster}, ignored)) return false;
    if (locked && !guest_call(c, kRasterUnlock, kAfterUnlock, {raster}, ignored)) return false;
    c.gpr[3] = 1u;
    return true;
}

void install(Cpu& cpu) {
    cpu.install_hle(kImageGetRaster, [](Cpu& c) {
        serve_image_get_raster(c);  // false: the run is stopped with the diagnostic
        return true;
    }, "RWIMAGEGETRASTER (native)");
}

}  // namespace gekko::rw_image
