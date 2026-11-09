// The native owners of the GameCube driver's colour <-> pixel standard functions (gekko/native_rw_pixel.h).

#include "gekko/native_rw_pixel.h"

#include "gekko/cpu.h"

#include <cstdio>
#include <string>

namespace gekko::rw_pixel {

namespace {

constexpr u32 kFormatMask = 0x0F00u;

bool opaque(const Rgba& c) noexcept { return c.a == 0xFFu; }

}  // namespace

bool rgb_to_pixel(const Rgba& c, u32 raster_format, u32& pixel) noexcept {
    switch (raster_format & kFormatMask) {
    case 0x000u:
    case 0x200u:  // 565
        pixel = ((u32(c.r) << 8) & 0xF800u) | ((u32(c.g) << 3) & 0x07E0u) | (u32(c.b) >> 3);
        return true;
    case 0x100u:
    case 0x300u:  // GX RGB5A3: opaque colours keep five bits a channel, the rest four and a three-bit alpha
        if (opaque(c)) {
            pixel = 0x8000u | ((u32(c.r) << 7) & 0x7C00u) | ((u32(c.g) << 2) & 0x03E0u) | (u32(c.b) >> 3);
        } else {
            pixel = ((u32(c.r) << 4) & 0x0F00u) | ((u32(c.a) << 7) & 0x7000u) | (u32(c.g) & 0x00F0u) | (u32(c.b) >> 4);
        }
        return true;
    case 0xA00u:  // 555
        pixel = 0x8000u | ((u32(c.r) << 7) & 0x7C00u) | ((u32(c.g) << 2) & 0x03E0u) | (u32(c.b) >> 3);
        return true;
    case 0x500u:  // 8888
        pixel = (u32(c.a) << 24) | (u32(c.r) << 16) | (u32(c.g) << 8) | u32(c.b);
        return true;
    case 0x600u:  // 888
        pixel = 0xFF000000u | (u32(c.r) << 16) | (u32(c.g) << 8) | u32(c.b);
        return true;
    default:
        return false;
    }
}

bool pixel_to_rgb(u32 p, u32 raster_format, Rgba& c) noexcept {
    const u32 format = raster_format & kFormatMask;
    switch (format) {
    case 0x000u:
    case 0x200u:
        c = {u8((p >> 8) & 0xF8u), u8((p >> 3) & 0xFCu), u8((p << 3) & 0xF8u), 0xFFu};
        return true;
    case 0x100u:
    case 0x300u:
        if ((p & 0x8000u) != 0u) {
            c = {u8((p >> 7) & 0xF8u), u8((p >> 2) & 0xF8u), u8((p << 3) & 0xF8u), 0xFFu};
        } else {
            // The 1555 branch of the original leaves alpha 0; the 4444 branch recovers the three alpha bits.
            c = {u8((p >> 4) & 0xF0u), u8(p & 0xF0u), u8((p << 4) & 0xF0u),
                 format == 0x100u ? u8(0u) : u8((p >> 7) & 0xE0u)};
        }
        return true;
    case 0xA00u:
        c = {u8((p >> 7) & 0xF8u), u8((p >> 2) & 0xF8u), u8((p << 3) & 0xF8u), 0xFFu};
        return true;
    case 0x500u:
        c = {u8(p >> 16), u8(p >> 8), u8(p), u8(p >> 24)};
        return true;
    case 0x600u:
        c = {u8(p >> 16), u8(p >> 8), u8(p), 0xFFu};
        return true;
    default:
        return false;
    }
}

bool serve_rgb_to_pixel(Cpu& cpu) {
    const u32 out = cpu.gpr[3], colour = cpu.gpr[4], format = cpu.gpr[5];
    const Rgba c{cpu.mem_read8(colour), cpu.mem_read8(colour + 1u), cpu.mem_read8(colour + 2u),
                 cpu.mem_read8(colour + 3u)};
    u32 pixel = 0;
    if (!rgb_to_pixel(c, format, pixel)) return false;
    cpu.mem_write32(out, pixel);
    cpu.gpr[3] = 1u;
    return true;
}

bool serve_pixel_to_rgb(Cpu& cpu) {
    const u32 out = cpu.gpr[3], pixel = cpu.mem_read32(cpu.gpr[4]), format = cpu.gpr[5];
    Rgba c{};
    if (!pixel_to_rgb(pixel, format, c)) return false;
    cpu.mem_write8(out, c.r);
    cpu.mem_write8(out + 1u, c.g);
    cpu.mem_write8(out + 2u, c.b);
    cpu.mem_write8(out + 3u, c.a);
    cpu.gpr[3] = 1u;
    return true;
}

namespace {

// The image scan 0x804947AC. image: width +4, height +8, depth +0xC, stride +0x10, pixels +0x14, palette +0x18.
u32 scan_image(Cpu& cpu, u32 image) {
    const u32 depth = cpu.mem_read32(image + 0x0Cu);
    const s32 width = static_cast<s32>(cpu.mem_read32(image + 0x04u));
    const s32 height = static_cast<s32>(cpu.mem_read32(image + 0x08u));
    const u32 stride = cpu.mem_read32(image + 0x10u);
    u32 row = cpu.mem_read32(image + 0x14u);
    const u32 palette = cpu.mem_read32(image + 0x18u);
    const bool palettised = depth == 4u || depth == 8u;
    bool cutout = false;
    for (s32 y = 0; y < height; ++y, row += stride) {
        for (s32 x = 0; x < width; ++x) {
            const u32 alpha = palettised ? cpu.mem_read8(palette + cpu.mem_read8(row + static_cast<u32>(x)) * 4u + 3u)
                                         : cpu.mem_read8(row + static_cast<u32>(x) * 4u + 3u);
            if (alpha == 0xFFu) continue;
            cutout = true;
            if (alpha <= 0x0Fu) continue;
            if (!palettised) return 0x300u;
            return depth == 4u ? 0x4300u : 0x2300u;
        }
    }
    u32 format = cutout ? 0x100u : 0x200u;
    if (depth == 4u) format |= 0x4000u;
    else if (depth == 8u) format |= 0x2000u;
    return format;
}

// floor(log2(v)) as the original's loop computes it (-1 for 0), then 1 << that, where a shift count is taken modulo 64 and a count of
// 32 or more gives 0 (slw): a width of 0 gives 0. The loop is bounded: a negative width never reaches 0 in the original (it hangs).
s32 round_down_pow2(s32 v) {
    s32 log = -1;
    for (s32 t = v; t != 0 && log < 64; t >>= 1) ++log;
    const u32 shift = static_cast<u32>(log) & 63u;
    return shift >= 32u ? 0 : static_cast<s32>(1u << shift);
}

}  // namespace

bool serve_image_find_raster_format(Cpu& cpu) {
    const u32 raster = cpu.gpr[3], image = cpu.gpr[4], flags = cpu.gpr[5];
    const u32 type = flags & 7u;
    cpu.mem_write8(raster + 0x20u, static_cast<u8>(type));
    cpu.mem_write32(raster + 0x14u, 0u);
    switch (type) {
    case 0u:
    case 4u:
    case 5u: {
        s32 width = static_cast<s32>(cpu.mem_read32(image + 0x04u));
        s32 height = static_cast<s32>(cpu.mem_read32(image + 0x08u));
        if (width > 0x400) width = 0x400;
        if (height > 0x400) height = 0x400;
        if ((flags & 0x8000u) != 0u) {
            width = round_down_pow2(width);
            height = round_down_pow2(height);
        }
        cpu.mem_write32(raster + 0x0Cu, static_cast<u32>(width));
        cpu.mem_write32(raster + 0x10u, static_cast<u32>(height));
        const u32 format = scan_image(cpu, image) | (flags & 0x9000u);
        cpu.mem_write8(raster + 0x23u, static_cast<u8>(format >> 8));
        cpu.gpr[3] = 1u;
        return true;
    }
    case 1u:
    case 2u:
        cpu.mem_write8(raster + 0x23u, 0u);
        cpu.mem_write32(raster + 0x0Cu, cpu.mem_read32(image + 0x04u));
        cpu.mem_write32(raster + 0x10u, cpu.mem_read32(image + 0x08u));
        cpu.gpr[3] = 1u;
        return true;
    default:
        return false;
    }
}

void install(Cpu& cpu) {
    const auto bind = [&](u32 address, bool (*serve)(Cpu&), const char* what) {
        cpu.install_hle(address, [serve, what](Cpu& c) {
            if (!serve(c)) {
                char detail[128];
                std::snprintf(detail, sizeof(detail),
                              "%s: argument %08X is one the original reports as an RwError (luminance, unknown format or raster type)",
                              what, c.gpr[5]);
                c.request_stop(StopReason::ExecutionFault, detail);
            }
            return true;
        }, std::string(what) + " (native)");
    };
    bind(kRgbToPixel, serve_rgb_to_pixel, "RGBTOPIXEL");
    bind(kPixelToRgb, serve_pixel_to_rgb, "PIXELTORGB");
    bind(kImageFindRasterFormat, serve_image_find_raster_format, "IMAGEFINDRASTERFORMAT");
}

}  // namespace gekko::rw_pixel
