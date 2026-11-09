// The GameCube RenderWare driver's raster life cycle (gekko/native_rw_raster.h).

#include "gekko/native_rw_raster.h"

#include "gekko/cpu.h"
#include "gekko/native_gx_sync.h"
#include "gekko/native_guest_call.h"

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>

namespace gekko::rw_raster {

namespace {

// ---- the rows (extract/native-owners.txt)
constexpr u32 kTextureSetRaster = 0x8049819Cu;   // GcnTextureSetRaster(texture, raster)
constexpr u32 kRasterSubRaster = 0x804981B8u;    // GcnRasterSubRaster(sub, parent)
constexpr u32 kRasterSetContext = 0x8049A570u;   // GcnRasterSetContext(_, context)
constexpr u32 kRasterGetMipLevels = 0x804966D8u; // GcnRasterGetMipLevels(out, raster)
constexpr u32 kTokenReached = 0x8049B7DCu;       // (token16) -> the draw-sync token has been reached
constexpr u32 kRasterDestroy = 0x80498058u;      // GcnRasterDestroy(_, raster)
constexpr u32 kRasterSetImage = 0x80494614u;     // GcnRasterSetImage(raster, image) -> 1 / 0
constexpr u32 kPaletteLock = 0x804972B8u;        // GcnRasterLockPalette(out, raster, flags)
constexpr u32 kPaletteUnlock = 0x80497388u;      // GcnRasterUnlockPalette(_, raster)
constexpr u32 kRasterUnlock = 0x80497104u;       // GcnRasterUnlock(_, raster)
constexpr u32 kRasterLock = 0x80496CA4u;         // GcnRasterLock(out, raster, flags | level << 8)
constexpr u32 kTextureRead = 0x8049A8DCu;        // GcnNativeTextureRead(stream, out texture)
constexpr u32 kClearQuad = 0x80499E58u;          // ClearQuad(device, rect, colour*, flags): the raster-level clear quad
constexpr u32 kAlphaMode = 0x804987C8u;          // the device alpha-test mode setter (native-boot's owner)
constexpr u32 kClearRect = 0x8049A124u;          // GcnRasterClearRect(_, rect, pixel): fill a rectangle of the current raster
constexpr u32 kClearWrapper = 0x8049A52Cu;       // device raster clear: the whole current raster
constexpr u32 kRenderWrapper = 0x8049A508u;      // device raster clear/render entry: the caller's rectangle
constexpr u32 kRwRasterLock = 0x8048AFC0u;       // RenderWare core RwRasterLock(raster, level, mode) -> pixels
constexpr u32 kRwRasterUnlock = 0x8048AB90u;     // RenderWare core RwRasterUnlock(raster)
constexpr u32 kRasterCreate = 0x80497CA8u;       // GcnRasterCreate(_, raster, type | format word)
constexpr u32 kSetLevels = 0x80497A70u;           // GcnRasterSetMipLevels(raster, count)
constexpr u32 kRasterFormat = 0x80497458u;       // GcnRasterFormat(raster, type | format word)
constexpr u32 kTileConvert = 0x8049677Cu;        // GcnRasterTile: linear pixels -> the driver's tiles
constexpr u32 kTileUnconvert = 0x80496A10u;      // GcnRasterUntile: the driver's tiles -> linear pixels
constexpr u32 kLevelBytes = 0x80496478u;         // GcnRasterLevelBytes(raster, level)
constexpr u32 kRasterNotify = 0x8048E310u;       // RenderWare core: the engine's raster-changed hook

const Address kAddresses[] = {
    {kTextureSetRaster, "GcnTextureSetRaster"},
    {kRasterSubRaster, "GcnRasterSubRaster"},
    {kRasterSetContext, "GcnRasterSetContext"},
    {kRasterGetMipLevels, "GcnRasterGetMipLevels"},
    {kTokenReached, "draw-sync token reached"},
    {kRasterDestroy, "GcnRasterDestroy"},
    {kRasterSetImage, "GcnRasterSetImage"},
    {0x80493C38u, "image to 4-bit paletted raster"},
    {0x80493E90u, "image to 8-bit paletted raster"},
    {0x804940ECu, "image to 16-bit raster"},
    {0x80494380u, "image to 32-bit raster"},
    {0x80493B74u, "image quantiser"},
    {0x80491448u, "quantiser end"},
    {0x80491124u, "quantiser tree free"},
    {0x80492B04u, "log2 loop"},
    {0x804965A8u, "all mip levels bytes"},
    {0x8049A57Cu, "GcnRasterGetSize"},
    {kPaletteLock, "GcnRasterLockPalette"},
    {kPaletteUnlock, "GcnRasterUnlockPalette"},
    {kRasterUnlock, "GcnRasterUnlock"},
    {kRasterLock, "GcnRasterLock"},
    {kClearRect, "GcnRasterClearRect"},
    {kClearWrapper, "GcnRasterClear"},
    {kRenderWrapper, "GcnRasterClearEntry"},
    {kRasterCreate, "GcnRasterCreate"},
    {kRasterFormat, "GcnRasterFormat"},
    {kSetLevels, "GcnRasterSetMipLevels"},
    {kTileConvert, "GcnRasterTile"},
    {kTileUnconvert, "GcnRasterUntile"},
    {kLevelBytes, "GcnRasterLevelBytes"},
};

// ---- guest data
constexpr u32 kSdaPluginOffset = 0x6200u;   // the raster plugin's offset inside a raster
constexpr u32 kSdaEngine = 0x615Cu;         // the engine instance (its memory functions at +0x108 malloc, +0x10C free)
constexpr u32 kSdaTextureCurrent = 0x620Cu; // the texture the driver last bound: a pointer to a word naming its raster
constexpr u32 kSdaTexturePlugin = 0x6218u;  // the texture plugin's offset inside a texture
constexpr u32 kSdaContext = 0x6210u;        // the raster context
constexpr u32 kSdaTokenRead = 0x6220u;      // the last GXReadDrawSync result
constexpr u32 kSdaTokenNext = 0x2AF0u;      // the next draw-sync token to issue
constexpr u32 kMemoryFree = 0x10Cu;

// raster fields (RwRaster): parent 0x00, pixels 0x04, width 0x0C, height 0x10, cType 0x20, cFlags 0x21, privateFlags
// 0x22, cFormat 0x23; depth 0x14, stride 0x18. Platform data at raster + plugin offset: mip count +0x32, token +0x30,
// pixel block +0x18.
constexpr u32 kParent = 0x00u, kPixels = 0x04u, kWidth = 0x0Cu, kHeight = 0x10u, kDepth = 0x14u, kStride = 0x18u;
constexpr u32 kType = 0x20u, kFlags = 0x21u, kFormat = 0x23u;
constexpr u32 kExtToken = 0x30u, kExtMips = 0x32u, kExtBlock = 0x18u;
constexpr u8 kFlagDontAllocate = 0x80u;   // cFlags: the pixels are not the driver's
constexpr u8 kFormatMipmap = 0x80u;       // cFormat: the raster has a mip chain
constexpr u32 kTokenRing = 0xE000u;   // tokens count 0 .. 0xDFFF and wrap

void stop(Cpu& c, const std::string& why) {
    c.request_stop(StopReason::ExecutionFault, "native RW raster: " + why);
}

// The original's error report: `RwErrorSet(&{1, code})` (0x8047EBD4; the variadic 0x8047EC78 it calls first only hands back
// its first argument). The record sits in a scratch frame below the stack pointer. The result is false when the call itself
// could not run (the run is stopped).
constexpr u32 kRwErrorSet = 0x8047EBD4u;
constexpr u32 kErrorDepth = 0x8000000Cu, kErrorType = 0x8000000Du, kErrorLock = 0x8000000Eu, kErrorUnlock = 0x80000011u,
              kErrorMemory = 0x80000013u, kErrorFormat = 2u;
bool report_error(Cpu& c, u32 code, u32 link) {
    const u32 sp = c.gpr[1];
    const u32 frame = sp - 0x40u;
    c.gpr[1] = frame;
    c.mem_write32(frame + 8u, 1u);
    c.mem_write32(frame + 0xCu, code);
    u32 ignored = 0;
    const bool ok = guest_call(c, kRwErrorSet, link, {frame + 8u}, ignored);
    c.gpr[1] = sp;
    return ok;
}

std::string hex(u32 value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08X", value);
    return text;
}

// 0x8049B7DC: has the GPU passed `token`? It reads the draw-sync token (GXReadDrawSync, NativeGxSync's) into
// SDA 0x6220, which stays the last value read, and compares the token against the ring window (cur, next]:
// outstanding tokens are those issued (next) and not yet seen (cur).
bool token_reached(Cpu& c, NativeGxSync& sync, u16 token) {
    const u32 sda = c.gpr[13];
    const u16 current = sync.token();
    c.mem_write16(sda + kSdaTokenRead, current);
    if (current >= kTokenRing) return false;
    const u16 next = c.mem_read16(sda + kSdaTokenNext);
    if (next >= current) return token <= current || token > next;
    return token > next && token <= current;
}

// 0x80498058. A raster that is its own parent and owns its pixels (cFlags without the don't-allocate bit) of type
// 0 (normal), 4 (texture) or 5 (camera texture) waits for the last draw that used it, drops the driver's binding of
// it and frees the pixel block; types 1 and 2 (z-buffer, camera) have nothing to free; any other type is an error the
// original reports through RwErrorSet (refused here: no raster of that type exists in the game).
void raster_destroy(Cpu& c, NativeGxSync& sync) {
    const u32 raster = c.gpr[4];
    const u32 sda = c.gpr[13];
    if (c.mem_read32(raster + kParent) != raster || (c.mem_read8(raster + kFlags) & kFlagDontAllocate) != 0u) {
        c.gpr[3] = 1u;
        return;
    }
    const u32 type = c.mem_read8(raster + kType);
    if (type == 3u || type >= 6u) {
        if (report_error(c, kErrorType, kRasterDestroy + 4u)) c.gpr[3] = 0u;
        return;
    }
    if (type == 1u || type == 2u) {
        c.gpr[3] = 1u;
        return;
    }
    const u32 ext = raster + c.mem_read32(sda + kSdaPluginOffset);
    const u16 token = c.mem_read16(ext + kExtToken);
    u16 next = c.mem_read16(sda + kSdaTokenNext);
    if (token == next) {
        sync.set_draw_sync(next);
        next = static_cast<u16>((static_cast<u32>(next) + 1u) % kTokenRing);
        c.mem_write16(sda + kSdaTokenNext, next);
    }
    if (!token_reached(c, sync, token)) {
        stop(c, "the draw-sync token " + std::to_string(token) + " of raster " + hex(raster) + " is not reached (the original waits)");
        return;
    }
    const u32 bound = c.mem_read32(sda + kSdaTextureCurrent);
    if (bound != 0u && c.mem_read32(bound) == raster) {
        // GcnTextureSetRaster(bound, 0)
        c.mem_write32(bound, 0u);
        c.mem_write32(bound + c.mem_read32(sda + kSdaTexturePlugin) + 0x20u, 0x01000000u);
    }
    const u32 engine = c.mem_read32(sda + kSdaEngine);
    u32 ignored = 0;   // the engine's free function returns nothing
    if (!guest_call(c, c.mem_read32(engine + kMemoryFree), kRasterDestroy + 4u, {c.mem_read32(ext + kExtBlock)}, ignored)) return;
    c.gpr[3] = 1u;
}

// 0x804966D8: the number of mip levels. The platform data keeps it (+0x32, 0xFF when never set); otherwise a raster
// with the mipmap format bit has floor(log2(max(width, height))) + 1 levels (as a byte), any other has one.
void get_mip_levels(Cpu& c) {
    const u32 out = c.gpr[3];
    const u32 raster = c.gpr[4];
    const u32 sda = c.gpr[13];
    const u32 parent = c.mem_read32(raster + kParent);
    const u32 stored = c.mem_read8(parent + c.mem_read32(sda + kSdaPluginOffset) + kExtMips);
    u32 levels;
    if (stored != 0xFFu) {
        levels = stored + 1u;
    } else if ((c.mem_read8(raster + kFormat) & kFormatMipmap) != 0u) {
        const std::int32_t width = static_cast<std::int32_t>(c.mem_read32(raster + kWidth));
        const std::int32_t height = static_cast<std::int32_t>(c.mem_read32(raster + kHeight));
        const std::int32_t largest = width > height ? width : height;
        if (largest < 0) {   // the original's shift loop never ends on a negative size
            stop(c, "GcnRasterGetMipLevels of a raster with a negative size");
            return;
        }
        u32 log = 0xFFFFFFFFu;   // floor(log2 v), -1 for 0
        for (u32 v = static_cast<u32>(largest); v != 0u; v >>= 1) ++log;
        levels = ((log & 0xFFu) + 1u) & 0xFFu;
    } else {
        levels = 1u;
    }
    c.mem_write32(out, levels);
    c.gpr[3] = 1u;
}

// 0x804972B8 (lock the palette) and 0x80497388 (unlock it). Only a raster of type 0 or 4 (low three bits) has one; any
// other type is an error the original reports through RwErrorSet (refused here). A raster that is its own parent: the lock
// (when nothing is locked yet, +8 == 0) marks the private flags (r5 bit 1 -> 0x08, bit 0 -> 0x10), points +8 at the palette
// words (platform data +0x20) and hands that pointer out through r3; the unlock flushes the palette when it was locked for
// writing (DCFlushRange: no effect on guest memory, not made), clears both marks and the lock pointer.
constexpr u32 kPrivateFlags = 0x22u, kLockPointer = 0x08u, kExtPalette = 0x20u;

bool palette_type_ok(Cpu& c, u32 raster, u32 error, u32 link) {
    const u32 type = c.mem_read8(raster + kType) & 7u;
    if (type == 0u || type == 4u) return true;
    if (report_error(c, error, link)) c.gpr[3] = 0u;   // the original reports an error and answers 0
    return false;
}

void palette_lock(Cpu& c) {
    const u32 out = c.gpr[3], raster = c.gpr[4], flags = c.gpr[5];
    if (!palette_type_ok(c, raster, kErrorLock, kPaletteLock + 4u)) return;
    if (c.mem_read32(raster + kParent) == raster && c.mem_read32(raster + kLockPointer) == 0u) {
        const u32 ext = raster + c.mem_read32(c.gpr[13] + kSdaPluginOffset);
        u8 priv = c.mem_read8(raster + kPrivateFlags);
        if ((flags & 2u) != 0u) priv = static_cast<u8>(priv | 0x08u);
        if ((flags & 1u) != 0u) priv = static_cast<u8>(priv | 0x10u);
        c.mem_write8(raster + kPrivateFlags, priv);
        c.mem_write32(raster + kLockPointer, c.mem_read32(ext + kExtPalette));
        c.mem_write32(out, c.mem_read32(raster + kLockPointer));
    }
    c.gpr[3] = 1u;
}

void palette_unlock(Cpu& c) {
    const u32 raster = c.gpr[4];
    if (!palette_type_ok(c, raster, kErrorUnlock, kPaletteUnlock + 4u)) return;
    if (c.mem_read32(raster + kParent) == raster) {
        c.mem_write8(raster + kPrivateFlags, static_cast<u8>(c.mem_read8(raster + kPrivateFlags) & ~0x18u));
        c.mem_write32(raster + kLockPointer, 0u);
    }
    c.gpr[3] = 1u;
}

// ---- pixel layout: the driver's tiles
//
// 0x8049677C / 0x80496A10: (tiles, linear, width, height, depth, stride) in r3..r8 (r3 is the destination of the first,
// the source of the second). The GameCube texture memory holds a level as tiles of 32 bytes: depth 4 = 8x8 pixels,
// depth 8 = 8x4, depth 16 = 4x4, depth 32 = 4x4 split in two halves (the first 16 bits of every pixel, then the second
// 16 bits), tiles in row-major order. The original copies one row of a tile at a time (4 or 8 bytes) with memcpy, or one
// pixel for depth 32; the same copies are made here, in the same order. Depths other than 4, 8, 16 and 32 are an error the
// original reports through RwErrorSet (refused).
void copy_bytes(Cpu& c, u32 to, u32 from, u32 count) {
    for (u32 i = 0; i < count; ++i) c.mem_write8(to + i, c.mem_read8(from + i));
}

bool retile(Cpu& c, u32 tiles, u32 linear, std::int32_t width, std::int32_t height, u32 depth, u32 stride, bool to_tiles,
            u32 link) {
    const auto copy = [&](u32 tile_offset, u32 linear_offset, u32 count) {
        if (to_tiles)
            copy_bytes(c, tiles + tile_offset, linear + linear_offset, count);
        else
            copy_bytes(c, linear + linear_offset, tiles + tile_offset, count);
    };
    switch (depth) {
        case 4u:
        case 8u: {
            const std::int32_t across = static_cast<std::int32_t>((static_cast<u32>(width) + 7u) & ~7u) >> 3;
            for (std::int32_t y = 0; y < height; ++y) {
                const u32 row = static_cast<u32>(y) * stride;
                for (std::int32_t x = 0, tx = 0; x < width; x += 8, ++tx) {
                    const u32 tile = static_cast<u32>(across * (y >> (depth == 4u ? 3 : 2)) + tx);
                    if (depth == 4u)
                        copy(((tile << 6) + ((static_cast<u32>(y) << 3) & 0x38u)) >> 1, row + 4u * static_cast<u32>(tx), 4u);
                    else
                        copy((tile << 5) + ((static_cast<u32>(y) << 3) & 0x18u), row + 8u * static_cast<u32>(tx), 8u);
                }
            }
            return true;
        }
        case 16u: {
            const std::int32_t across = static_cast<std::int32_t>((static_cast<u32>(width) + 3u) & ~3u) >> 2;
            for (std::int32_t y = 0; y < height; ++y) {
                const u32 row = static_cast<u32>(y) * stride;
                for (std::int32_t x = 0, tx = 0; x < width; x += 4, ++tx) {
                    const u32 tile = static_cast<u32>(across * (y >> 2) + tx);
                    copy(((tile << 4) + ((static_cast<u32>(y) << 2) & 0xCu)) << 1, row + 8u * static_cast<u32>(tx), 8u);
                }
            }
            return true;
        }
        case 32u: {
            const std::int32_t across = static_cast<std::int32_t>((static_cast<u32>(width) + 3u) & ~3u) >> 2;
            for (std::int32_t y = 0; y < height; ++y) {
                const u32 row = static_cast<u32>(y) * stride;
                for (std::int32_t x = 0; x < width; ++x) {
                    const u32 tile = static_cast<u32>(across * (y >> 2) + (x >> 2));
                    const u32 offset = (tile << 6) + ((((static_cast<u32>(y) << 2) & 0xCu) + (static_cast<u32>(x) & 3u)) << 1);
                    copy(offset, row + 4u * static_cast<u32>(x), 2u);
                    copy(offset + 0x20u, row + 4u * static_cast<u32>(x) + 2u, 2u);
                }
            }
            return true;
        }
        default:
            return report_error(c, kErrorDepth, link);   // the original reports the depth and returns with nothing moved
    }
}

// sraw by the register's low six bits: a shift of 32 or more leaves the sign.
std::int32_t sraw(std::int32_t value, u32 amount) {
    amount &= 0x3Fu;
    return amount >= 32u ? (value < 0 ? -1 : 0) : (value >> amount);
}

// 0x80496478: the bytes of one level of a raster, rounded up to 32. A raster with the private lock flags 0x02 / 0x04 set
// answers for its whole size; otherwise the level's size is width / height shifted by the level (at least 1).
bool level_bytes(Cpu& c, u32 raster, u32 level, u32& bytes, u32 link) {
    const u32 parent = c.mem_read32(raster + kParent);
    std::int32_t width, height;
    if ((c.mem_read8(raster + kPrivateFlags) & 0x06u) != 0u) {
        width = static_cast<std::int32_t>(c.mem_read32(parent + kWidth));
        height = static_cast<std::int32_t>(c.mem_read32(parent + kHeight));
    } else {
        const std::int32_t w = sraw(static_cast<std::int32_t>(c.mem_read32(parent + kWidth)), level & 0xFFu);
        const std::int32_t h = sraw(static_cast<std::int32_t>(c.mem_read32(parent + kHeight)), level & 0xFFu);
        width = w != 0 ? w : 1;
        height = h != 0 ? h : 1;
    }
    const u32 w = static_cast<u32>(width), h = static_cast<u32>(height);
    u32 size;
    switch (c.mem_read32(raster + kDepth)) {
        case 4u: size = (((w + 7u) & ~7u) * ((h + 7u) & ~7u)) >> 1; break;
        case 8u: size = ((w + 7u) & ~7u) * ((h + 3u) & ~3u); break;
        case 16u: size = (((w + 3u) & ~3u) * ((h + 3u) & ~3u)) << 1; break;
        case 32u: size = (((w + 3u) & ~3u) * ((h + 3u) & ~3u)) << 2; break;
        default:   // the original reports the depth and answers 0 (unrounded)
            bytes = 0u;
            return report_error(c, kErrorDepth, link);
    }
    bytes = (size + 0x1Fu) & ~0x1Fu;
    return true;
}

// 0x80497458: type and format of a new raster. `word` = type (bits 0..2) | flags (0xF8) | format code and bits (0xFF00: the
// code 0x100..0xA00, with 0x4000 / 0x2000 the 4 / 8 bit palette variants). It stores the type and flags into the raster, the
// texture format (ext +0xC), palette format (+0x10), palette flag (+0x14), the raster's depth (+0x14) and the format byte
// (+0x23). The combinations the original answers with an RwErrorSet report (and a null result) are refused here.
// Returns 1 on success, 0 when the original reports an error and answers 0 (reported), -1 when the run stopped.
int raster_format(Cpu& c, u32 raster, u32 word, u32 link) {
    const u32 sda = c.gpr[13];
    const u32 ext = c.mem_read32(raster + kParent) + c.mem_read32(sda + kSdaPluginOffset);
    const u32 type = word & 7u;
    u32 bits = word & 0xFF00u;
    c.mem_write8(raster + kType, static_cast<u8>(type));
    c.mem_write8(raster + kFlags, static_cast<u8>(word & 0xF8u));
    const auto fail = [&](u32 error) { return report_error(c, error, link) ? 0 : -1; };
    const auto depth = [&](u32 value) { c.mem_write32(raster + kDepth, value); };
    const auto set = [&](u32 texture_format, u32 palette_format, u32 palette_flag) {
        c.mem_write32(ext + 0x0Cu, texture_format);
        if (palette_format != 0xFFFFFFFFu) c.mem_write32(ext + 0x10u, palette_format);
        c.mem_write32(ext + 0x14u, palette_flag);
    };
    const bool four = (bits & 0x4000u) != 0u, eight = (bits & 0x2000u) != 0u;
    if (type == 3u || type >= 6u) return fail(kErrorType);
    const u32 code = bits & 0x0F00u;
    if (type == 1u || type == 2u) {
        const u32 flag = c.mem_read8(c.mem_read32(sda + 0x61F4u) + 0x19u);
        if (code == 0u) {
            if (flag != 0u) {
                set(4u, 0xFFFFFFFFu, 0u);
                bits |= 0x200u;
                depth(0x10u);
            } else {
                set(6u, 0xFFFFFFFFu, 0u);
                bits |= 0x600u;
                depth(0x20u);
            }
        } else if (bits != 0x700u || flag == 0u) {
            return fail(kErrorType);
        }
    } else {
        if (type == 5u && (bits & 0x6000u) != 0u) return fail(kErrorFormat);
        if (code == 0u) {
            switch (c.mem_read32(raster + kDepth)) {
                case 0u:
                    if (type == 5u) {
                        set(4u, 0xFFFFFFFFu, 0u);
                        bits |= 0x200u;
                        depth(0x10u);
                    } else {
                        set(5u, 0xFFFFFFFFu, 1u);
                        bits |= 0x100u;
                        depth(four ? 4u : eight ? 8u : 0x10u);
                    }
                    break;
                case 4u:
                    if (four) {
                        set(8u, 2u, 1u);
                        bits |= 0x100u;
                    } else {
                        set(0u, 0xFFFFFFFFu, 0u);
                    }
                    break;
                case 8u:
                    if (eight) {
                        set(9u, 2u, 1u);
                        bits |= 0x100u;
                    } else {
                        set(1u, 0xFFFFFFFFu, 0u);
                    }
                    break;
                case 16u:
                    set(5u, 0xFFFFFFFFu, 1u);
                    bits |= 0x100u;
                    break;
                case 32u:
                    set(6u, 0xFFFFFFFFu, 1u);
                    bits |= 0x500u;
                    break;
                default:
                    return fail(kErrorDepth);
            }
        } else {
            switch (code) {
                case 0x100u:
                case 0x300u:
                case 0x200u: {
                    const u32 palette = code == 0x200u ? 1u : 2u;
                    if (four) {
                        set(8u, palette, code == 0x200u ? 0u : 1u);
                        depth(4u);
                    } else if (eight) {
                        set(9u, palette, code == 0x200u ? 0u : 1u);
                        depth(8u);
                    } else {
                        set(code == 0x200u ? 4u : 5u, 0xFFFFFFFFu, code == 0x200u ? 0u : 1u);
                        depth(0x10u);
                    }
                    break;
                }
                case 0xA00u:
                    if (four) {
                        set(8u, 2u, 0u);
                        depth(4u);
                    } else if (eight) {
                        set(9u, 2u, 0u);
                        depth(8u);
                    } else {
                        set(5u, 0xFFFFFFFFu, 0u);
                        depth(0x10u);
                    }
                    break;
                case 0x500u:
                case 0x600u:
                    if ((bits & 0x6000u) != 0u) return fail(kErrorFormat);
                    set(6u, 0xFFFFFFFFu, code == 0x500u ? 1u : 0u);
                    depth(0x20u);
                    break;
                default:
                    return fail((code == 0x400u || code == 0x700u || code == 0x800u || code == 0x900u) ? kErrorFormat : kErrorType);
            }
        }
    }
    c.mem_write8(raster + kFormat, static_cast<u8>(bits >> 8));
    return 1;
}

// GXInitTlutObj's words (the palette object lives at the platform data's start): format, palette address, entry count.
void init_tlut_object(Cpu& c, u32 object, u32 address, u32 format, u32 entries) {
    const u32 first = (format << 10) & 0xC00u;
    const u32 rotated = (address >> 5) | (address << 27);
    u32 second = c.mem_read32(object + 4u);
    second = (second & ~0x1FFFFFu) | (rotated & 0x1FFFFFu);
    second = (second & 0x00FFFFFFu) | 0x64000000u;
    c.mem_write32(object, first);
    c.mem_write32(object + 4u, second);
    c.mem_write16(object + 8u, static_cast<u16>(entries));
}

// The number of mip levels the format asks for: floor(log2(max(width, height))) + 1 (as a byte) with the mipmap format bit,
// else 1. A negative size never ends in the original's shift loop (the one place this stops the run).
u32 mip_count(Cpu& c, u32 raster, bool& ok) {
    if ((c.mem_read8(raster + kFormat) & kFormatMipmap) == 0u) return 1u;
    const std::int32_t w = static_cast<std::int32_t>(c.mem_read32(raster + kWidth));
    const std::int32_t h = static_cast<std::int32_t>(c.mem_read32(raster + kHeight));
    const std::int32_t largest = w > h ? w : h;
    if (largest < 0) {
        stop(c, "a raster with a negative size (the original's level count loop never ends)");
        ok = false;
        return 0u;
    }
    u32 log = 0xFFFFFFFFu;
    for (u32 v = static_cast<u32>(largest); v != 0u; v >>= 1) ++log;
    return ((log & 0xFFu) + 1u) & 0xFFu;
}

// The tail GcnRasterCreate (0x80497CA8) and GcnRasterSetMipLevels (0x80497A70) share: the pixel block for the levels (the
// parent's level count: its byte +0x32, else the format's), through the engine's allocator, and for a palette raster the
// palette after the levels and its object. Sets r3 = 1, or reports the allocation failure (RwErrorSet) and answers 0.
void allocate_levels(Cpu& c, u32 raster, u32 ext, u32 link) {
    const u32 sda = c.gpr[13];
    bool ok = true;
    const u32 parent = c.mem_read32(raster + kParent);
    const u32 stored = c.mem_read8(parent + c.mem_read32(sda + kSdaPluginOffset) + 0x32u);
    const u32 count = stored != 0xFFu ? stored + 1u : mip_count(c, raster, ok);
    if (!ok) return;
    u32 levels = 0;
    for (u32 i = count; i != 0u;) {   // levels count-1 .. 0
        --i;
        u32 bytes = 0;
        if (!level_bytes(c, raster, i & 0xFFu, bytes, link)) return;
        levels += bytes;
    }
    const u32 engine = c.mem_read32(sda + kSdaEngine);
    const u32 allocator = c.mem_read32(engine + 0x108u);
    const bool palette = (c.mem_read8(raster + kFormat) & 0x60u) != 0u;
    const u32 depth = c.mem_read32(raster + kDepth);
    const u32 entries = (depth & 0x3Fu) >= 32u ? 0u : (1u << (depth & 0x3Fu));   // slw: a shift of 32 or more gives 0
    const u32 size = palette ? levels + ((entries << 1) + 0x1Fu) : levels + 0x1Fu;
    u32 block = 0;
    if (!guest_call(c, allocator, link, {size, 0x30411u}, block)) return;
    c.mem_write32(ext + 0x18u, block);
    if (block == 0u) {
        if (report_error(c, kErrorMemory, link)) c.gpr[3] = 0u;   // the original reports the size and answers 0
        return;
    }
    c.mem_write32(ext + 0x1Cu, (block + 0x1Fu) & ~0x1Fu);
    if (palette) {
        c.mem_write32(ext + 0x20u, c.mem_read32(ext + 0x1Cu) + levels);
        init_tlut_object(c, ext, c.mem_read32(ext + 0x20u), c.mem_read32(ext + 0x10u), entries & 0xFFFFu);
    }
    // (a camera texture's DCInvalidateRange over the block is not made)
    c.gpr[3] = 1u;
}

// 0x80497A70: the level count is set by the caller (r4): byte +0x32 = count - 1, then the same allocation.
void raster_set_levels(Cpu& c) {
    const u32 raster = c.gpr[3];
    const u32 ext = raster + c.mem_read32(c.gpr[13] + kSdaPluginOffset);
    c.mem_write8(ext + 0x32u, static_cast<u8>(c.gpr[4] - 1u));
    allocate_levels(c, raster, ext, kSetLevels + 4u);
}

// 0x80497CA8: create a raster's platform data: the format (above), then, for a raster of type 0, 4 or 5 with a size, the
// pixel block (every level's bytes, 32-aligned, through the engine's allocator; for a palette raster the palette follows
// the levels and its object is initialised). Rasters without a size or of the z-buffer / camera kinds only get the
// "pixels are not the driver's" flag. Allocation failures and the original's error reports are refused.
void raster_create(Cpu& c) {
    const u32 raster = c.gpr[4], word = c.gpr[5];
    const u32 sda = c.gpr[13];
    const u32 ext = raster + c.mem_read32(sda + kSdaPluginOffset);
    const u32 link = kRasterCreate + 4u;
    c.mem_write32(raster + kStride, 0u);
    c.mem_write32(ext + 0x0Cu, 0xFFu);
    c.mem_write32(ext + 0x10u, 0xFFu);
    for (u32 offset = 0x14u; offset <= 0x2Cu; offset += 4u) c.mem_write32(ext + offset, 0u);
    c.mem_write16(ext + 0x30u, c.mem_read16(sda + kSdaTokenRead));
    c.mem_write8(ext + 0x32u, 0xFFu);
    c.mem_write8(ext + 0x33u, 0xFFu);
    const int formatted = raster_format(c, raster, word, link);
    if (formatted <= 0) {
        if (formatted == 0) c.gpr[3] = 0u;
        return;
    }
    const std::int32_t width = static_cast<std::int32_t>(c.mem_read32(raster + kWidth));
    const std::int32_t height = static_cast<std::int32_t>(c.mem_read32(raster + kHeight));
    if (width == 0 || height == 0) {
        c.mem_write8(raster + kFlags, 0x80u);
        c.gpr[3] = 1u;
        return;
    }
    const u32 type = c.mem_read8(raster + kType);
    if (type == 3u || type >= 6u) {
        if (report_error(c, kErrorType, link)) c.gpr[3] = 0u;
        return;
    }
    if (type == 1u || type == 2u) {
        c.mem_write8(raster + kFlags, 0x80u);
        c.gpr[3] = 1u;
        return;
    }
    if ((c.mem_read8(raster + kFlags) & kFlagDontAllocate) != 0u) {
        c.gpr[3] = 1u;
        return;
    }
    bool ok = true;
    const u32 mips = mip_count(c, raster, ok);
    if (!ok) return;
    c.mem_write8(ext + 0x32u, static_cast<u8>(mips - 1u));
    allocate_levels(c, raster, ext, link);
}

// 0x8049A124: fill a rectangle of the current raster (*(r13 + 0x6210)) with a pixel. The raster types:
//  - 0, 4, 5 (normal, texture, camera texture): a CPU fill through the core's lock / unlock (the raster is locked unless it
//    already is: private flag 0x04). A full-raster clear with 0 or -1 is one memset of stride * height bytes; otherwise
//    rows of the depth's pixel (4: a byte of two nibbles, 8: a byte, 16: halfwords, 32: words) at the rectangle.
//  - 1, 2 (z-buffer, camera): the original draws a full quad through GX (ClearQuad 0x80499E58); the lifted build asks its
//    renderer (RasterClearFn). The reference build has no such renderer and keeps the original ClearQuad for its own guest
//    callers; this path of the raster clear stops there, naming it (no recording reaches it).
//  - 3, >= 6: RwErrorSet(0x8000000D) and answers 0. A depth other than 4, 8, 16, 32: RwErrorSet(0x8000000C), nothing filled.
struct Rect {
    std::int32_t x, y, w, h;
};

RasterClearFn g_raster_clear = nullptr;

// 0x80499E58 ClearQuad(device, rect*, colour*, flags): bit 0 clears the colour (rgba bytes at r5; else black), bit 1 the
// depth (to the farthest); neither: nothing. The original sets GX state, draws the quad through the pipe and restores the
// state; what stays is the alpha-test mode set to 1 (the mode setter 0x804987C8, entered as the native owner it is) and
// the picture, which the renderer clears.
void clear_quad(Cpu& c) {
    const u32 rect = c.gpr[4], colour_at = c.gpr[5], flags = c.gpr[6];
    if ((flags & 3u) == 0u) return;
    if (g_raster_clear == nullptr) {
        stop(c, "ClearQuad 0x80499E58: this build has no renderer to clear a raster (RasterClearFn not set)");
        return;
    }
    u32 rgba = 0xFF000000u;
    if ((flags & 1u) != 0u)
        rgba = static_cast<u32>(c.mem_read8(colour_at)) | (static_cast<u32>(c.mem_read8(colour_at + 1u)) << 8) |
               (static_cast<u32>(c.mem_read8(colour_at + 2u)) << 16) | (static_cast<u32>(c.mem_read8(colour_at + 3u)) << 24);
    const u32 saved_r3 = c.gpr[3];
    c.gpr[3] = 1u;
    c.serve_owner(kAlphaMode);   // alpha-test mode 1 (when this build owns the setter; the renderer sets its own either way)
    c.gpr[3] = saved_r3;
    g_raster_clear(c, static_cast<std::int32_t>(c.mem_read32(rect)), static_cast<std::int32_t>(c.mem_read32(rect + 4u)),
                   static_cast<std::int32_t>(c.mem_read32(rect + 8u)), static_cast<std::int32_t>(c.mem_read32(rect + 12u)), rgba,
                   (flags & 1u) != 0u, (flags & 2u) != 0u);
}

void fill_bytes(Cpu& c, u32 at, u8 value, u32 count) {
    for (u32 i = 0; i < count; ++i) c.mem_write8(at + i, value);
}

void raster_clear_rect(Cpu& c, const Rect& rect, u32 pixel, u32 link, u32 rect_at) {
    const u32 sda = c.gpr[13];
    const u32 raster = c.mem_read32(sda + kSdaContext);
    const u32 type = c.mem_read8(raster + kType);
    if (type == 1u || type == 2u) {   // a z-buffer / camera raster: the clear quad over the rectangle at the raster's offset
        const u32 device = c.gpr[3];
        const std::int32_t ox = static_cast<std::int16_t>(c.mem_read16(raster + 0x1Cu));
        const std::int32_t oy = static_cast<std::int16_t>(c.mem_read16(raster + 0x1Eu));
        if (rect_at != 0u) {   // the original moves the caller's rectangle to the raster's place, in the caller's memory
            c.mem_write32(rect_at, static_cast<u32>(rect.x + ox));
            c.mem_write32(rect_at + 4u, static_cast<u32>(rect.y + oy));
        }
        const u32 scratch = c.gpr[1] - 0x40u;   // the rectangle (and the colour) live in a frame held below r1
        const u32 saved_sp = c.gpr[1];
        c.gpr[1] = scratch;
        c.mem_write32(scratch + 0x10u, static_cast<u32>(rect.x + ox));
        c.mem_write32(scratch + 0x14u, static_cast<u32>(rect.y + oy));
        c.mem_write32(scratch + 0x18u, static_cast<u32>(rect.w));
        c.mem_write32(scratch + 0x1Cu, static_cast<u32>(rect.h));
        u32 flags = 2u, colour_at = 0u;
        if (type == 2u) {   // RwRGBAFromPixel(&rgba, pixel, format << 8): the engine's converter (RenderWare core)
            u32 ignored = 0;
            if (!guest_call(c, 0x8048A168u, link, {scratch + 0x20u, pixel, static_cast<u32>(c.mem_read8(raster + kFormat)) << 8}, ignored)) {
                c.gpr[1] = saved_sp;
                return;
            }
            flags = 1u;
            colour_at = scratch + 0x20u;
        }
        c.gpr[3] = device;
        c.gpr[4] = scratch + 0x10u;
        c.gpr[5] = colour_at;
        c.gpr[6] = flags;
        clear_quad(c);
        c.gpr[1] = saved_sp;
        c.gpr[3] = 1u;
        return;
    }
    if (type == 3u || type >= 6u) {
        if (report_error(c, kErrorType, link)) c.gpr[3] = 0u;
        return;
    }
    bool locked = false;
    u32 ignored = 0;
    const auto lock = [&](u32 mode) {
        if ((c.mem_read8(raster + kPrivateFlags) & 0x04u) != 0u) return true;
        locked = true;
        return guest_call(c, kRwRasterLock, link, {raster, 0u, mode}, ignored);
    };
    const auto finish = [&]() {
        if (locked && !guest_call(c, kRwRasterUnlock, link, {raster}, ignored)) return;
        c.gpr[3] = 1u;
    };
    const std::int32_t w = static_cast<std::int32_t>(c.mem_read32(raster + kWidth));
    const std::int32_t h = static_cast<std::int32_t>(c.mem_read32(raster + kHeight));
    if (c.mem_read32(raster + kParent) == raster && static_cast<std::int16_t>(c.mem_read16(raster + 0x1Cu)) == 0 &&
        static_cast<std::int16_t>(c.mem_read16(raster + 0x1Eu)) == 0 && w == rect.w && h == rect.h) {
        if (!lock(9u)) return;
        if (pixel == 0u || pixel == 0xFFFFFFFFu) {
            fill_bytes(c, c.mem_read32(raster + kPixels), static_cast<u8>(pixel), c.mem_read32(raster + kStride) * static_cast<u32>(h));
            finish();
            return;
        }
    } else if (!lock(3u)) {
        return;
    }
    const u32 depth = c.mem_read32(raster + kDepth);
    const u32 stride = c.mem_read32(raster + kStride);
    const u32 pixels = c.mem_read32(raster + kPixels);
    for (std::int32_t y = 0; y < rect.h; ++y) {
        const u32 row = pixels + stride * static_cast<u32>(rect.y + y);
        switch (depth) {
            case 4u: {
                const u8 pair = static_cast<u8>((pixel & 0x0Fu) | ((pixel << 4) & 0xF0u));
                fill_bytes(c, row + static_cast<u32>(rect.x >> 1), pair, static_cast<u32>(rect.w >> 1));
                break;
            }
            case 8u:
                fill_bytes(c, row + static_cast<u32>(rect.x), static_cast<u8>(pixel), static_cast<u32>(rect.w));
                break;
            case 16u:
                for (std::int32_t x = 0; x < rect.w; ++x)
                    c.mem_write16(row + 2u * static_cast<u32>(rect.x) + 2u * static_cast<u32>(x), static_cast<u16>(pixel));
                break;
            case 32u:
                for (std::int32_t x = 0; x < rect.w; ++x)
                    c.mem_write32(row + 4u * static_cast<u32>(rect.x) + 4u * static_cast<u32>(x), pixel);
                break;
            default:
                if (!report_error(c, kErrorDepth, link)) return;
                finish();
                return;
        }
    }
    finish();
}

// ---- GcnNativeTextureRead 0x8049A8DC: a texture from a stream -------------------------------------------------------
//
// RwStreamFindChunk (struct chunk, version 0x35000 .. 0x37002), a 0x58-byte platform block (id 6; filter word at +4, LOD
// values at +8 .. +0x14, the name at +0x18, the mask name at +0x38), a 16-byte raster header (flags, width, height, depth, levels, texture
// format, palette format, palette flag), RwRasterCreate with the flags | 0x80, the platform data, GcnRasterSetMipLevels,
// the palette (when the format has one) and the texels read straight into the raster's blocks, RwTextureCreate, the
// texture's filter / address word, RwTextureSetName and the LOD values. Every failure answers 0 and, like the original,
// frees nothing before the raster exists and nothing but the raster after it. The calls are RenderWare core
// (guest_call); the data-cache flushes and GXInvalidateTexAll write no guest word and are not made. The locals live in
// a frame the owner holds below r1 (the callees' frames start beneath it).
TextureLoadedFn g_texture_loaded = nullptr;

void texture_read(Cpu& c) {
    constexpr u32 kFindChunk = 0x8047DFA0u, kStreamRead = 0x80481D9Cu, kRasterCreateCore = 0x8048AEC4u,
                  kRasterDestroyCore = 0x8048AC2Cu, kTextureCreate = 0x8048DD2Cu, kTextureSetName = 0x8048DEA4u,
                  kTextureSetMaskName = 0x8048DF50u;
    const u32 stream = c.gpr[3], out = c.gpr[4];
    const u32 sda = c.gpr[13];
    const u32 link = kTextureRead + 4u;
    const u32 saved = c.gpr[1];
    const u32 f = saved - 0xC0u;   // the locals: +8 size word, +0xC version, +0x10 length, +0x14 raster header, +0x24 platform block
    c.gpr[1] = f;
    bool ok = true;
    const auto call = [&](u32 target, std::initializer_list<u32> args) -> u32 {
        u32 result = 0;
        if (ok && !guest_call(c, target, link, args, result)) ok = false;
        return result;
    };
    const auto finish = [&](u32 result) {
        c.gpr[1] = saved;
        if (ok) c.gpr[3] = result;
    };
    if (call(kFindChunk, {stream, 1u, f + 0x10u, f + 0xCu}) == 0u) return finish(0u);
    if (!ok) return finish(0u);
    const u32 version = c.mem_read32(f + 0xCu);
    if (version < 0x35000u || version > 0x37002u) return finish(0u);
    if (call(kStreamRead, {stream, f + 0x24u, 0x58u}) != 0x58u) return finish(0u);
    if (c.mem_read32(f + 0x24u) != 6u) return finish(0u);
    if (call(kStreamRead, {stream, f + 0x14u, 0x10u}) != 0x10u) return finish(0u);
    const u32 raster = call(kRasterCreateCore, {c.mem_read16(f + 0x18u), c.mem_read16(f + 0x1Au), c.mem_read8(f + 0x1Cu),
                                                c.mem_read32(f + 0x14u) | 0x80u});
    if (!ok || raster == 0u) return finish(0u);
    const u32 ext = raster + c.mem_read32(sda + kSdaPluginOffset);
    c.mem_write32(ext + 0x0Cu, c.mem_read8(f + 0x1Eu));
    c.mem_write32(ext + 0x10u, c.mem_read8(f + 0x1Fu));
    c.mem_write32(ext + 0x14u, c.mem_read32(f + 0x20u) != 0u ? 1u : 0u);
    if (!ok) return finish(0u);
    c.gpr[3] = raster;   // GcnRasterSetMipLevels(raster, levels): the owner's own body, over the same frame
    c.gpr[4] = c.mem_read8(f + 0x1Du);
    raster_set_levels(c);
    if (c.stopped()) {
        c.gpr[1] = saved;
        return;
    }
    if (c.gpr[3] == 0u) {
        call(kRasterDestroyCore, {raster});
        return finish(0u);
    }
    c.mem_write8(raster + kFlags, static_cast<u8>(c.mem_read8(raster + kFlags) & 0x7Fu));
    if ((c.mem_read8(raster + kFormat) & 0x60u) != 0u) {   // the palette
        const u32 depth = c.mem_read32(raster + kDepth);
        const u32 bytes = ((depth & 0x3Fu) >= 32u ? 0u : (1u << (depth & 0x3Fu))) << 1;
        if (call(kStreamRead, {stream, c.mem_read32(ext + 0x20u), bytes}) != bytes) return finish(0u);
    }
    const u32 mip_bit = c.mem_read8(raster + kFormat) & 0x10u;
    c.mem_write8(raster + kFormat, static_cast<u8>(c.mem_read8(raster + kFormat) & ~mip_bit));
    if (call(kStreamRead, {stream, f + 8u, 4u}) != 4u) return finish(0u);
    const u32 size = c.mem_read32(f + 8u);
    if (call(kStreamRead, {stream, c.mem_read32(ext + 0x1Cu), size}) != size) return finish(0u);
    c.mem_write8(raster + kFormat, static_cast<u8>(c.mem_read8(raster + kFormat) | mip_bit));
    const u32 texture = call(kTextureCreate, {raster});
    if (!ok) return finish(0u);
    if (texture == 0u) {
        call(kRasterDestroyCore, {raster});
        return finish(0u);
    }
    const u32 filter = c.mem_read32(f + 0x28u);
    c.mem_write32(texture + 0x50u, (c.mem_read32(texture + 0x50u) & 0xFFFF0000u) | (filter & 0xFFFFu));
    call(kTextureSetName, {texture, f + 0x3Cu});
    call(kTextureSetMaskName, {texture, f + 0x5Cu});
    if (!ok) return finish(0u);
    if (g_texture_loaded == nullptr) {
        stop(c, "GcnNativeTextureRead: this build has no texture-loaded step (TextureLoadedFn not set)");
        c.gpr[1] = saved;
        return;
    }
    float lod = 0.0f;
    const u32 lod_bits = c.mem_read32(f + 0x38u);
    std::memcpy(&lod, &lod_bits, sizeof(lod));
    g_texture_loaded(c, texture, c.mem_read32(f + 0x30u), c.mem_read32(f + 0x34u), c.mem_read32(f + 0x2Cu), static_cast<double>(lod));
    c.mem_write32(out, texture);
    finish(1u);
}

// 0x80497104: end of a level lock. Types 0, 4 and 5 only (other types: the original's error report, refused). A raster that
// was locked for writing (private flag 0x04) without the "already in tiles" flag (0x20) has its linear pixels converted to
// the driver's tiles (retile, over the two lock pointers +0x24 / +0x28 of the platform
// data), then the original flushed the data cache over the level and invalidated the GX texture cache: neither changes a
// guest word and neither exists here. A raster that is its own parent takes its lock size (+0x28 / +0x2C) back into
// width / height; the linear copy (+0x28) is freed through the engine unless it is flagged tiled; the lock words are
// cleared; a writable mipmapped raster whose level count is still 0 (+0x33) is reset and told to the engine hook
// (kRasterNotify).
void raster_unlock(Cpu& c) {
    const u32 raster = c.gpr[4];
    const u32 sda = c.gpr[13];
    const u32 parent = c.mem_read32(raster + kParent);
    const u32 type = c.mem_read8(raster + kType) & 7u;
    if (!(type == 0u || type == 4u || type == 5u)) {
        if (report_error(c, kErrorUnlock, kRasterUnlock + 4u)) c.gpr[3] = 0u;
        return;
    }
    const u32 ext = parent + c.mem_read32(sda + kSdaPluginOffset);
    constexpr u32 kLockLinear = 0x24u, kLockTiled = 0x28u, kExtLevel = 0x33u;
    const u32 link = kRasterUnlock + 4u;
    u32 ignored = 0;
    if ((c.mem_read8(raster + kPrivateFlags) & 0x04u) != 0u) {
        if ((c.mem_read8(raster + kPrivateFlags) & 0x20u) == 0u) {
            if (!retile(c, c.mem_read32(ext + kLockLinear), c.mem_read32(ext + kLockTiled),
                        static_cast<std::int32_t>(c.mem_read32(parent + kWidth)), static_cast<std::int32_t>(c.mem_read32(parent + kHeight)),
                        c.mem_read32(raster + kDepth), c.mem_read32(raster + kStride), true, link))
                return;
        }
        // DCFlushRange(linear pixels, level size) and GXInvalidateTexAll: no guest word. (The size call has none either.)
    }
    if (parent == raster) {
        c.mem_write32(raster + kWidth, c.mem_read32(raster + 0x28u));
        c.mem_write32(raster + kHeight, c.mem_read32(raster + 0x2Cu));
    }
    if ((c.mem_read8(raster + kPrivateFlags) & 0x20u) == 0u) {
        const u32 engine = c.mem_read32(sda + kSdaEngine);
        if (!guest_call(c, c.mem_read32(engine + kMemoryFree), link, {c.mem_read32(ext + kLockTiled)}, ignored)) return;
        c.mem_write32(ext + kLockTiled, 0u);
    }
    c.mem_write32(raster + kStride, 0u);
    c.mem_write32(raster + kPixels, 0u);
    const bool notify = (c.mem_read8(raster + kPrivateFlags) & 0x04u) != 0u && (c.mem_read8(raster + kFormat) & 0x10u) != 0u &&
                        c.mem_read8(ext + kExtLevel) == 0u;
    c.mem_write8(ext + kExtLevel, 0xFFu);
    c.mem_write8(raster + kPrivateFlags, static_cast<u8>(c.mem_read8(raster + kPrivateFlags) & 0xD9u));
    c.mem_write8(parent + kPrivateFlags, static_cast<u8>(c.mem_read8(parent + kPrivateFlags) & 0xD9u));
    if (notify && !guest_call(c, kRasterNotify, link, {raster, 0u}, ignored)) return;
    c.gpr[3] = 1u;
}

// 0x80496CA4: start a level lock. r3 = where the pixel pointer goes, r4 = raster, r5 = flags (bit 0 write, bit 1 read, bit 3
// "the level is addressed in place": no lock buffer) | level << 8. Types 0, 4 and 5 only; depths 4, 8, 16 and 32 only; a
// sub-raster cannot lock in place (the original's error reports are refused). The lock buffer (a linear copy of the level,
// size = row bytes * the level height) comes from the engine's allocator, the tile conversion and the byte sizes from the
// game's own RenderWare functions (guest_call). The data-cache flush of a camera-texture level is not made. A write lock
// waits for the draw that last used the raster (the draw-sync token, as RasterDestroy).
void raster_lock(Cpu& c, NativeGxSync& sync) {
    const u32 out = c.gpr[3], raster = c.gpr[4], flags = c.gpr[5];
    const u32 sda = c.gpr[13];
    const u32 level = (flags >> 8) & 0xFFu;
    const u32 parent = c.mem_read32(raster + kParent);
    const u32 type = c.mem_read8(raster + kType) & 7u;
    if (!(type == 0u || type == 4u || type == 5u)) {
        if (report_error(c, kErrorLock, kRasterLock + 4u)) c.gpr[3] = 0u;
        return;
    }
    const u32 ext = parent + c.mem_read32(sda + kSdaPluginOffset);
    const u32 link = kRasterLock + 4u;
    const bool in_place = (flags & 8u) != 0u;
    u32 sum = 0u;
    for (u32 i = level; (i & 0xFFu) != 0u; --i) {   // the byte sizes of the levels below
        u32 bytes = 0;
        if (!level_bytes(c, raster, i - 1u, bytes, link)) return;
        sum += bytes;
    }
    const u32 pixels = c.mem_read32(ext + 0x1Cu) + sum;
    const std::int32_t level_width = sraw(static_cast<std::int32_t>(c.mem_read32(parent + kWidth)), level);
    const u32 width = level_width != 0 ? static_cast<u32>(level_width) : 1u;
    const u32 depth = c.mem_read32(raster + kDepth);
    u32 row;   // bytes of one row of the level
    switch (depth) {
        case 4u: row = static_cast<u32>(static_cast<std::int32_t>((width + 7u) & ~7u) >> 1); break;
        case 8u: row = (width + 7u) & ~7u; break;
        case 16u: row = ((width + 3u) << 1) & ~7u; break;
        case 32u: row = ((width + 3u) << 2) & ~15u; break;
        default:   // the original reports the depth and goes on with a row of 0 bytes
            if (!report_error(c, kErrorDepth, link)) return;
            row = 0u;
            break;
    }
    if (!in_place) {
        const std::int32_t level_height = sraw(static_cast<std::int32_t>(c.mem_read32(parent + kHeight)), level);
        const u32 rows = level_height != 0 ? static_cast<u32>(level_height) : 1u;
        const u32 engine = c.mem_read32(sda + kSdaEngine);
        u32 buffer = 0;
        if (!guest_call(c, c.mem_read32(engine + 0x108u), link, {row * rows, 0x30411u}, buffer)) return;
        c.mem_write32(ext + 0x28u, buffer);
        if (buffer == 0u) {
            c.gpr[3] = 0u;
            return;
        }
    }
    if (parent == raster) {
        const u32 full_width = c.mem_read32(raster + kWidth), full_height = c.mem_read32(raster + kHeight);
        const std::int32_t h = sraw(static_cast<std::int32_t>(full_height), level);
        c.mem_write32(raster + 0x28u, full_width);
        c.mem_write32(raster + 0x2Cu, full_height);
        c.mem_write32(raster + kWidth, width);
        c.mem_write32(raster + kHeight, h != 0 ? static_cast<u32>(h) : 1u);
        c.mem_write32(raster + kPixels, in_place ? pixels : c.mem_read32(ext + 0x28u));
    } else {
        if (in_place) {
            if (report_error(c, kErrorLock, link)) c.gpr[3] = 0u;
            return;
        }
        const std::int32_t x = static_cast<std::int16_t>(c.mem_read16(raster + 0x1Cu));
        const std::int32_t y = static_cast<std::int16_t>(c.mem_read16(raster + 0x1Eu));
        std::int32_t shifted;
        switch (depth) {
            case 4u: shifted = x >> 1; break;
            case 8u: shifted = x; break;
            case 16u: shifted = x * 2; break;
            case 32u: shifted = x * 4; break;
            default:   // the original reports the depth and answers 0 (the row was 0 bytes: reported above)
                if (report_error(c, kErrorDepth, link)) c.gpr[3] = 0u;
                return;
        }
        c.mem_write32(raster + kPixels, c.mem_read32(ext + 0x28u) + row * static_cast<u32>(y) + static_cast<u32>(shifted));
    }
    c.mem_write32(ext + 0x24u, pixels);
    c.mem_write32(raster + kStride, row);
    c.mem_write8(ext + 0x33u, static_cast<u8>(level));
    if ((flags & 2u) != 0u) {
        c.mem_write8(raster + kPrivateFlags, static_cast<u8>(c.mem_read8(raster + kPrivateFlags) | 2u));
        c.mem_write8(parent + kPrivateFlags, static_cast<u8>(c.mem_read8(parent + kPrivateFlags) | 2u));
        if (!in_place) {   // the tiles become the linear copy the program reads
            if (!retile(c, c.mem_read32(ext + 0x24u), c.mem_read32(ext + 0x28u),
                        static_cast<std::int32_t>(c.mem_read32(parent + kWidth)), static_cast<std::int32_t>(c.mem_read32(parent + kHeight)),
                        c.mem_read32(raster + kDepth), c.mem_read32(raster + kStride), false, link))
                return;
        }
    }
    if ((flags & 1u) != 0u) {
        c.mem_write8(raster + kPrivateFlags, static_cast<u8>(c.mem_read8(raster + kPrivateFlags) | 4u));
        c.mem_write8(parent + kPrivateFlags, static_cast<u8>(c.mem_read8(parent + kPrivateFlags) | 4u));
        const u16 token = c.mem_read16(ext + kExtToken);
        u16 next = c.mem_read16(sda + kSdaTokenNext);
        if (token == next) {
            sync.set_draw_sync(next);
            next = static_cast<u16>((static_cast<u32>(next) + 1u) % kTokenRing);
            c.mem_write16(sda + kSdaTokenNext, next);
        }
        if (!token_reached(c, sync, token)) {
            stop(c, "GcnRasterLock: the draw-sync token " + std::to_string(token) + " of raster " + hex(raster) +
                        " is not reached (the original waits)");
            return;
        }
    }
    if (in_place) {
        c.mem_write8(raster + kPrivateFlags, static_cast<u8>(c.mem_read8(raster + kPrivateFlags) | 0x20u));
        c.mem_write8(parent + kPrivateFlags, static_cast<u8>(c.mem_read8(parent + kPrivateFlags) | 0x20u));
    }
    c.mem_write32(out, c.mem_read32(raster + kPixels));
    c.gpr[3] = 1u;
}

}  // namespace

bool report_rw_error(Cpu& cpu, u32 code, u32 link) { return report_error(cpu, code, link); }

void set_raster_clear(RasterClearFn clear) noexcept { g_raster_clear = clear; }

void install_lifted(Cpu& cpu, TextureLoadedFn loaded) {
    g_texture_loaded = loaded;
    cpu.install_hle(kTextureRead, [](Cpu& c) {
        texture_read(c);
        return true;
    }, "native RW raster: GcnNativeTextureRead");
    // Only here: the owner needs the renderer, and the reference build's guest camera clear enters ClearQuad directly.
    cpu.install_hle(kClearQuad, [](Cpu& c) {
        clear_quad(c);
        return true;
    }, "native RW raster: GcnRasterClearQuad");
}

const Address* addresses(std::size_t& count) noexcept {
    count = sizeof(kAddresses) / sizeof(kAddresses[0]);
    return kAddresses;
}

void install(Cpu& cpu, NativeGxSync& sync) {
    const auto install_hle = [&cpu](u32 address, const char* name, auto&& serve) {
        cpu.install_hle(address, [serve](Cpu& c) {
            serve(c);
            return true;
        }, std::string("native RW raster: ") + name);
    };
    install_hle(kTextureSetRaster, "GcnTextureSetRaster", [](Cpu& c) {   // texture r3 takes raster r4
        const u32 texture = c.gpr[3];
        c.mem_write32(texture, c.gpr[4]);
        c.mem_write32(texture + c.mem_read32(c.gpr[13] + kSdaTexturePlugin) + 0x20u, 0x01000000u);
        c.gpr[3] = 1u;
    });
    install_hle(kRasterSubRaster, "GcnRasterSubRaster", [](Cpu& c) {     // sub r3 shares the pixels of parent r4
        const u32 sub = c.gpr[3], parent = c.gpr[4];
        c.mem_write32(sub + kStride, c.mem_read32(parent + kStride));
        c.mem_write32(sub + kDepth, c.mem_read32(parent + kDepth));
        c.mem_write8(sub + kType, c.mem_read8(parent + kType));
        c.mem_write8(sub + kFormat, c.mem_read8(parent + kFormat));
        c.mem_write32(sub + kPixels, 0u);
        c.gpr[3] = 1u;
    });
    install_hle(kRasterSetContext, "GcnRasterSetContext", [](Cpu& c) {
        c.mem_write32(c.gpr[13] + kSdaContext, c.gpr[4]);
        c.gpr[3] = 1u;
    });
    install_hle(kRasterGetMipLevels, "GcnRasterGetMipLevels", [](Cpu& c) { get_mip_levels(c); });
    install_hle(kTokenReached, "draw-sync token reached", [&sync](Cpu& c) {
        c.gpr[3] = token_reached(c, sync, static_cast<u16>(c.gpr[3])) ? 1u : 0u;
    });
    install_hle(kRasterDestroy, "GcnRasterDestroy", [&sync](Cpu& c) { raster_destroy(c, sync); });
    install_image_owners(cpu);
    install_size_owners(cpu);
    install_hle(kRasterUnlock, "GcnRasterUnlock", [](Cpu& c) { raster_unlock(c); });
    install_hle(kRasterCreate, "GcnRasterCreate", [](Cpu& c) { raster_create(c); });
    install_hle(kSetLevels, "GcnRasterSetMipLevels", [](Cpu& c) { raster_set_levels(c); });
    install_hle(kRasterFormat, "GcnRasterFormat", [](Cpu& c) {
        const int formatted = raster_format(c, c.gpr[3], c.gpr[4], kRasterFormat + 4u);
        if (formatted >= 0) c.gpr[3] = static_cast<u32>(formatted);
    });
    install_hle(kClearRect, "GcnRasterClearRect", [](Cpu& c) {
        const u32 rect = c.gpr[4];
        raster_clear_rect(c, {static_cast<std::int32_t>(c.mem_read32(rect)), static_cast<std::int32_t>(c.mem_read32(rect + 4u)),
                              static_cast<std::int32_t>(c.mem_read32(rect + 8u)), static_cast<std::int32_t>(c.mem_read32(rect + 12u))},
                          c.gpr[5], kClearRect + 4u, rect);
    });
    install_hle(kClearWrapper, "GcnRasterClear", [](Cpu& c) {   // the whole current raster
        const u32 raster = c.mem_read32(c.gpr[13] + kSdaContext);
        raster_clear_rect(c, {0, 0, static_cast<std::int32_t>(c.mem_read32(raster + kWidth)),
                              static_cast<std::int32_t>(c.mem_read32(raster + kHeight))},
                          c.gpr[5], kClearWrapper + 4u, 0u);
    });
    install_hle(kRenderWrapper, "GcnRasterClearEntry", [](Cpu& c) {   // the caller's rectangle, forwarded
        const u32 rect = c.gpr[4];
        raster_clear_rect(c, {static_cast<std::int32_t>(c.mem_read32(rect)), static_cast<std::int32_t>(c.mem_read32(rect + 4u)),
                              static_cast<std::int32_t>(c.mem_read32(rect + 8u)), static_cast<std::int32_t>(c.mem_read32(rect + 12u))},
                          c.gpr[5], kRenderWrapper + 4u, rect);
    });
    install_hle(kRasterLock, "GcnRasterLock", [&sync](Cpu& c) { raster_lock(c, sync); });
    install_hle(kTileConvert, "GcnRasterTile", [](Cpu& c) {
        retile(c, c.gpr[3], c.gpr[4], static_cast<std::int32_t>(c.gpr[5]), static_cast<std::int32_t>(c.gpr[6]), c.gpr[7], c.gpr[8], true,
               kTileConvert + 4u);
    });
    install_hle(kTileUnconvert, "GcnRasterUntile", [](Cpu& c) {
        retile(c, c.gpr[4], c.gpr[3], static_cast<std::int32_t>(c.gpr[5]), static_cast<std::int32_t>(c.gpr[6]), c.gpr[7], c.gpr[8], false,
               kTileUnconvert + 4u);
    });
    install_hle(kLevelBytes, "GcnRasterLevelBytes", [](Cpu& c) {
        u32 bytes = 0;
        if (level_bytes(c, c.gpr[3], c.gpr[4], bytes, kLevelBytes + 4u)) c.gpr[3] = bytes;
    });
    install_hle(kPaletteLock, "GcnRasterLockPalette", [](Cpu& c) { palette_lock(c); });
    install_hle(kPaletteUnlock, "GcnRasterUnlockPalette", [](Cpu& c) { palette_unlock(c); });
}

}  // namespace gekko::rw_raster
