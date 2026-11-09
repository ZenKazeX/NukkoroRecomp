// GcnRasterSetImage and the image-to-raster conversions of the RenderWare GameCube driver (gekko/native_rw_raster.h).
//
//   0x80494614 GcnRasterSetImage(raster, image) -> 1 / 0        locks, converts, unlocks
//   0x80493C38 4-bit paletted raster   <- image      0x80493E90 8-bit paletted raster <- image
//   0x804940EC 16-bit raster           <- image      0x80494380 32-bit raster         <- image
//   0x80493B74 (image, depth) -> a temporary paletted raster (colour quantisation through RenderWare core)
//   0x80492B24.. the five palette / pixel converters (RGB5A1, RGB565, RGB4A3, ARGB, AR/GB), native functions below
//
// Every body is the original's arithmetic over the same guest words; RenderWare core (the lock helpers, the raster
// allocator, the quantiser, RwErrorSet) is reached through guest_call, the driver's own functions are native. Each
// body reproduces the original's stack frame (size, back chain) because the error report and the quantiser pass
// pointers into it, and the 16/32-bit bodies index a palette table that lives in it.
#include "gekko/native_guest_call.h"
#include "gekko/native_rw_raster.h"
#include "gekko/ppc_ops.h"

#include <cstdint>
#include <initializer_list>
#include <string>

namespace gekko::rw_raster {

namespace {

constexpr u32 kSdaPlugin = 0x6200u;   // the raster plugin's offset inside a raster
constexpr u32 kSdata2Quantise = 0x1CA0u;   // r2 + this: the single the quantiser set-up takes in f1

// Raster / image fields (RwRaster at +0x00 parent, +0x04 pixels, +0x08 palette, +0x0C width, +0x10 height, +0x14 depth,
// +0x18 stride, +0x23 format; an image: +0x04 width, +0x08 height, +0x0C depth, +0x10 stride, +0x14 pixels, +0x18 palette).
constexpr u32 kRaster = 0x00u, kRasterPixels = 0x04u, kRasterPalette = 0x08u, kRasterWidth = 0x0Cu, kRasterHeight = 0x10u,
              kRasterDepth = 0x14u, kRasterStride = 0x18u, kRasterFormat = 0x23u;
constexpr u32 kImageWidth = 0x04u, kImageHeight = 0x08u, kImageDepth = 0x0Cu, kImageStride = 0x10u, kImagePixels = 0x14u,
              kImagePalette = 0x18u;

constexpr u32 kPaletteLocked = 0x33u;   // the platform data's byte: non-zero = the palette is not rewritten

// RenderWare core entries (not platform code in the manifest): called, never reimplemented.
constexpr u32 kRasterCreate = 0x80488D58u;    // (width, height, depth) -> raster / 0
constexpr u32 kRasterBegin = 0x80488E68u;     // (raster)
constexpr u32 kRasterFree = 0x80488E00u;      // (raster)
constexpr u32 kQuantInit = 0x80490F28u;       // (context) -> non-zero
constexpr u32 kQuantSetup = 0x8048EF00u;      // (context, image) with f1 = the sdata2 single
constexpr u32 kQuantPalette = 0x80490620u;    // (palette, entries, context)
constexpr u32 kQuantConvert = 0x80490B00u;    // (pixels, stride, width, 0, context, image)
constexpr u32 kQuantEnd = 0x80491448u;        // (context): native below (the plan counts it as a driver row)
constexpr u32 kQuantTreeFree = 0x80491124u;   // (context, node, depth): native below
constexpr u32 kAllocatorFree = 0x80480D2Cu;   // (allocator): RenderWare core
constexpr u32 kErrorFormat = 0x8047EC78u;     // (code[, string]) -> message
constexpr u32 kErrorSet = 0x8047EBD4u;        // (&{1, message})
constexpr u32 kLockRaster = 0x8048AFC0u;      // (raster, 0, 5) -> non-zero on success
constexpr u32 kLockPixels = 0x8048ACE4u;      // (raster, 5)
constexpr u32 kUnlockPixels = 0x8048ABD8u;    // (raster)
constexpr u32 kUnlockRaster = 0x8048AB90u;    // (raster)

// The error codes and the message string the bodies report.
constexpr u32 kErrNoConverter = 0x8000000Du;   // an unknown raster format
constexpr u32 kErrBadDepth = 0x80000008u;      // an unknown image depth
constexpr u32 kErrBadKind = 0x8000000Cu;       // SetImage: an unknown raster kind
constexpr u32 kErrFormat400 = 2u;              // code 2 with the string below (a raster format the driver cannot convert to)
constexpr u32 kErrFormat400String = 0x8056F188u;

void stop(Cpu& c, const std::string& why) {
    c.request_stop(StopReason::ExecutionFault, "native RW raster image: " + why);
}

// ---- the five converters: a pointer to 4 bytes (r, g, b, a) -> the target pixel -------------------------------
u32 to_rgb5a1(Cpu& c, u32 p) {   // 0x80492B24: 0x8000 | r5 << 10 | g5 << 5 | b5
    const u32 r = c.mem_read8(p), g = c.mem_read8(p + 1u), b = c.mem_read8(p + 2u);
    return ((r << 7) & 0x7C00u) | 0x8000u | ((g << 2) & 0x03E0u) | ((b >> 3) & 0x001Fu);
}
u32 to_rgb565(Cpu& c, u32 p) {   // 0x80492B44
    const u32 r = c.mem_read8(p), g = c.mem_read8(p + 1u), b = c.mem_read8(p + 2u);
    return ((r << 8) & 0xF800u) | ((g << 3) & 0x07E0u) | ((b >> 3) & 0x001Fu);
}
u32 to_rgb4a3(Cpu& c, u32 p) {   // 0x80492B64: an opaque pixel (a == 0xFF) is RGB5A1, otherwise 3-bit alpha and 4-bit colours
    const u32 a = c.mem_read8(p + 3u);
    if (a == 0xFFu) return to_rgb5a1(c, p);
    const u32 r = c.mem_read8(p), g = c.mem_read8(p + 1u), b = c.mem_read8(p + 2u);
    return ((r << 4) & 0x0F00u) | ((a << 7) & 0x7000u) | (g & 0x00F0u) | ((b >> 4) & 0x000Fu);
}
u32 to_argb(Cpu& c, u32 p) {     // 0x80492BB4: 0xFF000000 | r << 16 | g << 8 | b
    const u32 r = c.mem_read8(p), g = c.mem_read8(p + 1u), b = c.mem_read8(p + 2u);
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}
u32 to_argb_alpha(Cpu& c, u32 p) {   // 0x80492BD8: a << 24 | r << 16 | g << 8 | b
    const u32 r = c.mem_read8(p), g = c.mem_read8(p + 1u), b = c.mem_read8(p + 2u), a = c.mem_read8(p + 3u);
    return (a << 24) | (r << 16) | (g << 8) | b;
}
using Convert = u32 (*)(Cpu&, u32);

// ---- a body's frame ----------------------------------------------------------------------------------------------
struct Frame {
    Cpu& c;
    u32 sp0, sp;
    Frame(Cpu& cpu, u32 size) : c(cpu), sp0(cpu.gpr[1]), sp(cpu.gpr[1] - size) {
        c.mem_write32(sp, sp0);
        c.gpr[1] = sp;
    }
    bool leave(bool ok) {
        c.gpr[1] = sp0;
        return ok;
    }
};

bool call(Cpu& c, u32 target, u32 link, std::initializer_list<u32> args, u32& result) {
    return guest_call(c, target, link, args, result);
}

// RwErrorSet of {1, the formatter's message}: the struct sits at frame + slot.
bool report(Cpu& c, const Frame& f, u32 slot, u32 link, std::initializer_list<u32> format_args) {
    u32 message = 0, ignored = 0;
    c.mem_write32(f.sp + slot, 1u);
    ppc::set_cr_bit(c, 6u, 0u);   // crclr cr1eq: no vector arguments
    if (!call(c, kErrorFormat, link, format_args, message)) return false;
    c.mem_write32(f.sp + slot + 4u, message);
    return call(c, kErrorSet, link, {f.sp + slot}, ignored);
}

// The converter a raster format selects (cFormat << 8 & 0xF00). Format 0x400 reports a code-2 error; any other
// unknown format reports 0x8000000D; both leave no converter (the original then calls address 0 if it needs one).
bool select_converter(Cpu& c, const Frame& f, u32 raster, u32 link, Convert& out) {
    out = nullptr;
    switch ((c.mem_read8(raster + kRasterFormat) << 8) & 0xF00u) {
    case 0xA00: out = to_rgb5a1; return true;
    case 0x200: out = to_rgb565; return true;
    case 0x100:
    case 0x300: out = to_rgb4a3; return true;
    case 0x600: out = to_argb; return true;
    case 0x500: out = to_argb_alpha; return true;
    case 0x400: return report(c, f, 0x08u, link, {kErrFormat400, kErrFormat400String});
    default: return report(c, f, 0x10u, link, {kErrNoConverter});
    }
}

u32 convert_pixel(Cpu& c, Convert fn, u32 pointer, bool& ok) {
    if (fn == nullptr) {
        stop(c, "the original calls address 0 as the colour converter (no converter for this raster format)");
        ok = false;
        return 0;
    }
    return fn(c, pointer);
}

// 0x80491124 (context, node, depth): frees a tree of nodes with 16 inline children at +0x1C, children first (in order), then the node,
// through the engine free function ([[r13 + 0x615C] + 0x11C](context[0x4004], block)). The original unrolls eight levels of this recursion
// per call; the order of the frees is the observable.
bool free_tree(Cpu& c, u32 context, u32 node, std::int32_t depth) {
    constexpr u32 kLink = kQuantTreeFree + 4u;
    if (node == 0u) return true;
    if (depth > 0) {
        for (u32 k = 0; k < 16u; ++k)
            if (!free_tree(c, context, c.mem_read32(node + 0x1Cu + 4u * k), depth - 1)) return false;
    }
    u32 ignored = 0;
    return call(c, c.mem_read32(c.mem_read32(c.gpr[13] + 0x615Cu) + 0x11Cu), kLink, {c.mem_read32(context + 0x4004u), node}, ignored);
}

// 0x80491448 (context): frees the quantiser's node tree and the allocator.
bool quant_end(Cpu& c, u32 context) {
    constexpr u32 kLink = kQuantEnd + 4u;
    Frame f(c, 0x10u);
    const u32 node = c.mem_read32(context + 0x4000u);
    if (!free_tree(c, context, node, static_cast<std::int32_t>(c.mem_read32(c.gpr[13] + 0x2AD0u)))) return f.leave(false);
    c.mem_write32(context + 0x4000u, 0u);
    u32 ignored = 0;
    return f.leave(call(c, kAllocatorFree, kLink, {c.mem_read32(context + 0x4004u)}, ignored));
}

// 0x80493B74 (image, depth): a temporary paletted RwImage of the source's size, filled by the colour quantiser. The
// callees are RenderWare core (RwImageCreate, RwImageAllocatePixels, the quantiser context set-up and run); the 0x4000-
// byte context lives in the body's own frame, as in the original. Returns false when the machine stopped; `out` is the
// temporary image or 0 (a failed allocation or set-up, which the original reports by returning 0 and leaking).
bool quantise(Cpu& c, u32 image, u32 depth, u32& out) {
    constexpr u32 kQuantiseBody = 0x80493B74u;
    constexpr u32 kLink = kQuantiseBody + 4u;
    Frame f(c, 0x4020u);
    u32 r = 0, ignored = 0;
    out = 0;
    if (!call(c, kRasterCreate, kLink, {c.mem_read32(image + kImageWidth), c.mem_read32(image + kImageHeight), depth}, r)) return f.leave(false);
    if (r == 0u) return f.leave(true);
    const u32 temp = r;
    const u32 context = f.sp + 8u;
    if (!call(c, kRasterBegin, kLink, {temp}, ignored)) return f.leave(false);
    if (!call(c, kQuantInit, kLink, {context}, r)) return f.leave(false);
    if (r == 0u) return f.leave(true);
    c.fpr[1].bits[0] = single_to_double_bits(c.mem_read32(c.gpr[2] + kSdata2Quantise));
    if (!call(c, kQuantSetup, kLink, {context, image}, ignored)) return f.leave(false);
    const u32 entries = (depth & 0x20u) != 0u ? 0u : (1u << (depth & 31u));
    if (!call(c, kQuantPalette, kLink, {c.mem_read32(temp + kImagePalette), entries, context}, ignored)) return f.leave(false);
    if (!call(c, kQuantConvert, kLink,
              {c.mem_read32(temp + kImagePixels), c.mem_read32(temp + kImageStride), c.mem_read32(temp + kImageDepth), 0u, context, image},
              ignored))
        return f.leave(false);
    {
        const u32 sp = c.gpr[1];   // quant_end allocates its own frame below this one
        (void)sp;
        if (!quant_end(c, context)) return f.leave(false);
    }
    out = temp;
    return f.leave(true);
}

// A source of 8 / 32 bits goes through a temporary image of the raster's depth: build it, convert from it (the same
// body, one frame down), free it.
bool via_quantised(Cpu& c, bool (*convert)(Cpu&, u32, u32), u32 raster, u32 image, u32 link) {
    u32 temp = 0, ignored = 0;
    if (!quantise(c, image, c.mem_read32(raster + kRasterDepth), temp)) return false;
    if (temp == 0u) return true;
    if (!convert(c, raster, temp)) return false;
    return call(c, kRasterFree, link, {temp}, ignored);
}

bool report_bad_depth(Cpu& c, const Frame& f, u32 link) { return report(c, f, 0x18u, link, {kErrBadDepth}); }

// The loop the direct and 8-bit bodies share: rows of `raster width` pixels, source rows by the image's stride.
template <typename PixelFn>
bool for_each_pixel(Cpu& c, u32 raster, u32 image, PixelFn&& pixel) {
    const std::int32_t height = static_cast<std::int32_t>(c.mem_read32(raster + kRasterHeight));
    for (std::int32_t y = 0; y < height; ++y) {
        const u32 src = c.mem_read32(image + kImagePixels) + c.mem_read32(image + kImageStride) * static_cast<u32>(y);
        const u32 dst = c.mem_read32(raster + kRasterPixels) + c.mem_read32(raster + kRasterStride) * static_cast<u32>(y);
        for (std::int32_t x = 0; x < static_cast<std::int32_t>(c.mem_read32(raster + kRasterWidth)); ++x)
            if (!pixel(src, dst, static_cast<u32>(x))) return false;
    }
    return true;
}

// 0x80493C38: a 4-bit paletted raster.
bool to_paletted4(Cpu& c, u32 raster, u32 image) {
    constexpr u32 kBody = 0x80493C38u, kLink = kBody + 4u;
    Frame f(c, 0x40u);
    const u32 ext = c.mem_read32(raster + kRaster) + c.mem_read32(c.gpr[13] + kSdaPlugin);
    Convert fn = nullptr;
    if (!select_converter(c, f, raster, kLink, fn)) return f.leave(false);
    const u32 depth = c.mem_read32(image + kImageDepth);
    if (depth == 8u || depth == 0x20u) return f.leave(via_quantised(c, to_paletted4, raster, image, kLink));
    if (depth != 4u) return f.leave(report_bad_depth(c, f, kLink));
    const std::int32_t height = static_cast<std::int32_t>(c.mem_read32(raster + kRasterHeight));
    for (std::int32_t y = 0; y < height; ++y) {
        u32 src = c.mem_read32(image + kImagePixels) + c.mem_read32(image + kImageStride) * static_cast<u32>(y);
        u32 dst = c.mem_read32(raster + kRasterPixels) + c.mem_read32(raster + kRasterStride) * static_cast<u32>(y);
        for (std::int32_t x = 0; x < static_cast<std::int32_t>(c.mem_read32(raster + kRasterWidth)); x += 2) {
            const u32 low = c.mem_read8(src + 1u), high = c.mem_read8(src);
            c.mem_write8(dst, static_cast<u8>((low & 0x0Fu) | ((high << 4) & 0xF0u)));
            src += 2u;
            dst += 1u;
        }
    }
    if (c.mem_read8(ext + kPaletteLocked) == 0u) {
        u32 palette = c.mem_read32(raster + kRasterPalette);
        for (u32 i = 0; i < 0x10u; ++i, palette += 2u) {
            bool ok = true;
            const u32 pixel = convert_pixel(c, fn, c.mem_read32(image + kImagePalette) + 4u * i, ok);
            if (!ok) return f.leave(false);
            c.mem_write16(palette, static_cast<u16>(pixel));
        }
    }
    return f.leave(true);
}

// 0x80493E90: an 8-bit paletted raster.
bool to_paletted8(Cpu& c, u32 raster, u32 image) {
    constexpr u32 kBody = 0x80493E90u, kLink = kBody + 4u;
    Frame f(c, 0x40u);
    Convert fn = nullptr;
    if (!select_converter(c, f, raster, kLink, fn)) return f.leave(false);
    const u32 depth = c.mem_read32(image + kImageDepth);
    if (depth == 0x20u) return f.leave(via_quantised(c, to_paletted8, raster, image, kLink));
    if (depth != 4u && depth != 8u) return f.leave(report_bad_depth(c, f, kLink));
    const u32 ext = c.mem_read32(raster + kRaster) + c.mem_read32(c.gpr[13] + kSdaPlugin);
    if (!for_each_pixel(c, raster, image, [&](u32 src, u32 dst, u32 x) {
            c.mem_write8(dst + x, c.mem_read8(src + x));
            return true;
        }))
        return f.leave(false);
    if (c.mem_read8(ext + kPaletteLocked) == 0u) {
        u32 palette = c.mem_read32(raster + kRasterPalette);
        const std::int32_t entries = static_cast<std::int32_t>(1u << (depth & 31u));
        for (std::int32_t i = 0; i < entries; ++i, palette += 2u) {
            bool ok = true;
            const u32 pixel = convert_pixel(c, fn, c.mem_read32(image + kImagePalette) + 4u * static_cast<u32>(i), ok);
            if (!ok) return f.leave(false);
            c.mem_write16(palette, static_cast<u16>(pixel));
        }
    }
    return f.leave(true);
}

// 0x804940EC / 0x80494380: a 16 / 32-bit raster. A paletted source (4 / 8 bits, one byte per pixel) goes through a
// table of 1 << depth converted palette entries kept in the frame at +0x20; a 32-bit source converts per pixel.
template <bool Wide>
bool to_direct(Cpu& c, u32 raster, u32 image, u32 body, u32 frame_size) {
    const u32 link = body + 4u;
    Frame f(c, frame_size);
    Convert fn = nullptr;
    if (!select_converter(c, f, raster, link, fn)) return f.leave(false);
    const u32 depth = c.mem_read32(image + kImageDepth);
    const u32 unit = Wide ? 4u : 2u;
    const auto store = [&](u32 address, u32 value) {
        if (Wide) c.mem_write32(address, value);
        else c.mem_write16(address, static_cast<u16>(value));
    };
    if (depth == 4u || depth == 8u) {
        const u32 table = f.sp + 0x20u;
        const std::int32_t entries = static_cast<std::int32_t>(1u << (depth & 31u));
        for (std::int32_t i = 0; i < entries; ++i) {
            bool ok = true;
            const u32 pixel = convert_pixel(c, fn, c.mem_read32(image + kImagePalette) + 4u * static_cast<u32>(i), ok);
            if (!ok) return f.leave(false);
            store(table + unit * static_cast<u32>(i), pixel);
        }
        return f.leave(for_each_pixel(c, raster, image, [&](u32 src, u32 dst, u32 x) {
            const u32 entry = table + unit * c.mem_read8(src + x);
            store(dst + unit * x, Wide ? c.mem_read32(entry) : c.mem_read16(entry));
            return true;
        }));
    }
    if (depth == 0x20u) {
        return f.leave(for_each_pixel(c, raster, image, [&](u32 src, u32 dst, u32 x) {
            bool ok = true;
            const u32 pixel = convert_pixel(c, fn, src + 4u * x, ok);
            if (!ok) return false;
            store(dst + unit * x, pixel);
            return true;
        }));
    }
    return f.leave(report_bad_depth(c, f, link));
}

bool to_direct16(Cpu& c, u32 raster, u32 image) { return to_direct<false>(c, raster, image, 0x804940ECu, 0x240u); }
bool to_direct32(Cpu& c, u32 raster, u32 image) { return to_direct<true>(c, raster, image, 0x80494380u, 0x440u); }

// 0x80494614 GcnRasterSetImage(raster, image).
bool set_image(Cpu& c, u32& result) {
    constexpr u32 kBody = 0x80494614u, kLink = kBody + 4u;
    constexpr u32 kFlags = 0x22u, kFormat = 0x23u, kKind = 0x14u;
    const u32 raster = c.gpr[3], image = c.gpr[4];
    const u32 flags = c.mem_read8(raster + kFlags), format = c.mem_read8(raster + kFormat);
    const bool locked = (flags & 0x04u) != 0u;
    const u32 stage = (format << 8) & 0x6000u;
    bool own_lock = false;
    Frame f(c, 0x30u);
    u32 r = 0, ignored = 0;
    result = 0;
    if (!locked) {
        if (!call(c, kLockRaster, kLink, {raster, 0u, 5u}, r)) return f.leave(false);
        if (r == 0u) return f.leave(true);
    }
    if (stage != 0u) {
        own_lock = (c.mem_read8(raster + kFlags) & 0x10u) != 0u;
        if (!own_lock) {
            if (!call(c, kLockPixels, kLink, {raster, 5u}, r)) return f.leave(false);
            if (r == 0u) return f.leave(true);
        }
    }
    bool ok = true;
    switch (c.mem_read32(raster + kKind)) {
    case 4:  ok = to_paletted4(c, raster, image); break;
    case 8:  ok = to_paletted8(c, raster, image); break;
    case 16: ok = to_direct16(c, raster, image); break;
    case 24: ok = report(c, f, 0x10u, kLink, {kErrBadKind}); break;
    case 32: ok = to_direct32(c, raster, image); break;
    default: ok = report(c, f, 0x08u, kLink, {kErrBadKind}); break;
    }
    if (!ok) return f.leave(false);
    if (stage != 0u && !own_lock && !call(c, kUnlockPixels, kLink, {raster}, ignored)) return f.leave(false);
    if (!locked && !call(c, kUnlockRaster, kLink, {raster}, ignored)) return f.leave(false);
    result = 1u;
    return f.leave(true);
}

}  // namespace

void install_image_owners(Cpu& cpu) {
    const auto own = [&cpu](u32 address, const char* name, auto&& serve) {
        cpu.install_hle(address, [serve](Cpu& c) {
            serve(c);
            return true;
        }, std::string("native RW raster image: ") + name);
    };
    own(0x80494614u, "GcnRasterSetImage", [](Cpu& c) {
        u32 result = 0;
        if (set_image(c, result)) c.gpr[3] = result;
    });
    own(0x80493C38u, "image to 4-bit paletted raster", [](Cpu& c) { to_paletted4(c, c.gpr[3], c.gpr[4]); });
    own(0x80493E90u, "image to 8-bit paletted raster", [](Cpu& c) { to_paletted8(c, c.gpr[3], c.gpr[4]); });
    own(0x804940ECu, "image to 16-bit raster", [](Cpu& c) { to_direct16(c, c.gpr[3], c.gpr[4]); });
    own(0x80494380u, "image to 32-bit raster", [](Cpu& c) { to_direct32(c, c.gpr[3], c.gpr[4]); });
    own(kQuantEnd, "quantiser end", [](Cpu& c) { quant_end(c, c.gpr[3]); });
    own(kQuantTreeFree, "quantiser tree free", [](Cpu& c) {
        const u32 sp0 = c.gpr[1];   // the original keeps a 0x80-byte frame under the engine free calls
        c.mem_write32(sp0 - 0x80u, sp0);
        c.gpr[1] = sp0 - 0x80u;
        const bool ok = free_tree(c, c.gpr[3], c.gpr[4], static_cast<std::int32_t>(c.gpr[5]));
        c.gpr[1] = sp0;
        if (ok) c.gpr[3] = 1u;
    });
    own(0x80493B74u, "image quantiser", [](Cpu& c) {
        u32 temp = 0;
        if (quantise(c, c.gpr[3], c.gpr[4], temp)) c.gpr[3] = temp;
    });
}

}  // namespace gekko::rw_raster
