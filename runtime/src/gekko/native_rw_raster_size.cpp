// The RenderWare GameCube driver's raster size arithmetic (gekko/native_rw_raster.h).
//
//   0x80492B04 floor(log2(r3)) as a signed loop            (-1 for 0)
//   0x80496478 (raster, level) -> the bytes of one mip level: texture-block rounded, 32-byte aligned
//   0x804965A8 (raster)        -> the bytes of every mip level
//   0x8049A57C GcnRasterGetSize(out, &raster) -> the size of the raster's platform data plus its pixels, 1 / 0
//
// Every body is the original's arithmetic over the same guest words. The mip count comes from the GcnRasterGetMipLevels
// owner (0x804966D8, called as an owner); the error report of an unknown raster kind is RenderWare core (guest_call).
#include "gekko/native_guest_call.h"
#include "gekko/native_rw_raster.h"
#include "gekko/ppc_ops.h"

#include <cstdint>
#include <initializer_list>
#include <string>

namespace gekko::rw_raster {

namespace {

constexpr u32 kLog2 = 0x80492B04u, kMipBytes = 0x80496478u, kAllMipsBytes = 0x804965A8u, kGetSize = 0x8049A57Cu;
constexpr u32 kGetMipLevels = 0x804966D8u;   // the owner (native_rw_raster.cpp)
constexpr u32 kErrorFormat = 0x8047EC78u, kErrorSet = 0x8047EBD4u;   // RenderWare core
constexpr u32 kErrBadKind = 0x8000000Cu;
constexpr u32 kSdaPlugin = 0x6200u;

void stop(Cpu& c, const std::string& why) {
    c.request_stop(StopReason::ExecutionFault, "native RW raster size: " + why);
}

// sraw: shift amounts 32..63 (bit 5 of the amount) give the sign fill.
u32 arithmetic_shift(u32 value, u32 amount) {
    const u32 sh = amount & 0x3Fu;
    const std::int32_t v = static_cast<std::int32_t>(value);
    if (sh >= 32u) return v < 0 ? 0xFFFFFFFFu : 0u;
    return static_cast<u32>(v >> sh);
}

// 0x80496478 (raster, level): the original's frame is 0x10 bytes; the error report passes a pointer into it.
bool mip_bytes(Cpu& c, u32 raster, u32 level, u32& bytes) {
    constexpr u32 kLink = kMipBytes + 4u;
    const u32 parent = c.mem_read32(raster);
    u32 width, height;
    if ((c.mem_read8(raster + 0x22u) & 0x06u) != 0u) {
        width = c.mem_read32(parent + 0x0Cu);
        height = c.mem_read32(parent + 0x10u);
    } else {
        const u32 shift = level & 0xFFu;
        const u32 w = arithmetic_shift(c.mem_read32(parent + 0x0Cu), shift);
        const u32 h = arithmetic_shift(c.mem_read32(parent + 0x10u), shift);
        width = w != 0u ? w : 1u;
        height = h != 0u ? h : 1u;
    }
    u32 size;
    switch (c.mem_read32(raster + 0x14u)) {
    case 4:  size = (((width + 7u) & ~7u) * ((height + 7u) & ~7u)) >> 1; break;
    case 8:  size = ((width + 7u) & ~7u) * ((height + 3u) & ~3u); break;
    case 16: size = (((width + 3u) & ~3u) * ((height + 3u) & ~3u)) << 1; break;
    case 32: size = (((width + 3u) & ~3u) * ((height + 3u) & ~3u)) << 2; break;
    default: {
        const u32 sp0 = c.gpr[1], sp = sp0 - 0x10u;
        u32 message = 0, ignored = 0;
        c.mem_write32(sp, sp0);
        c.gpr[1] = sp;
        c.mem_write32(sp + 8u, 1u);
        ppc::set_cr_bit(c, 6u, 0u);   // crclr cr1eq
        const bool ok = guest_call(c, kErrorFormat, kLink, {kErrBadKind}, message) &&
                        (c.mem_write32(sp + 12u, message), guest_call(c, kErrorSet, kLink, {sp + 8u}, ignored));
        c.gpr[1] = sp0;
        bytes = 0u;
        return ok;
    }
    }
    bytes = (size + 0x1Fu) & ~0x1Fu;
    return true;
}

// 0x804965A8 (raster): the sum over mip levels (count - 1 down to 0).
bool all_mip_bytes(Cpu& c, u32 raster, u32& total) {
    const u32 sp0 = c.gpr[1], sp = sp0 - 0x20u;
    c.mem_write32(sp, sp0);
    c.gpr[1] = sp;
    total = 0u;
    c.gpr[3] = sp + 8u;
    c.gpr[4] = raster;
    c.gpr[5] = 0u;
    if (!c.serve_owner(kGetMipLevels) || c.stopped()) {
        c.gpr[1] = sp0;
        if (!c.stopped()) stop(c, "the mip-level owner is not installed");
        return false;
    }
    while (true) {
        const u32 count = c.mem_read32(sp + 8u);
        c.mem_write32(sp + 8u, count - 1u);
        if (count == 0u) break;
        u32 bytes = 0;
        if (!mip_bytes(c, raster, (count - 1u) & 0xFFu, bytes)) {
            c.gpr[1] = sp0;
            return false;
        }
        total += bytes;
    }
    c.gpr[1] = sp0;
    return true;
}

}  // namespace

void install_size_owners(Cpu& cpu) {
    const auto own = [&cpu](u32 address, const char* name, auto&& serve) {
        cpu.install_hle(address, [serve](Cpu& c) {
            serve(c);
            return true;
        }, std::string("native RW raster size: ") + name);
    };
    own(kLog2, "log2 loop", [](Cpu& c) {
        const std::int32_t value = static_cast<std::int32_t>(c.gpr[3]);
        if (value < 0) {   // srawi never reaches zero: the original loops forever
            stop(c, "floor(log2) of a negative value (the original never ends)");
            return;
        }
        u32 bits = 0u;
        for (u32 v = static_cast<u32>(value); v != 0u; v >>= 1) ++bits;
        c.gpr[3] = bits - 1u;
    });
    own(kAllMipsBytes, "all mip levels bytes", [](Cpu& c) {
        u32 total = 0;
        if (all_mip_bytes(c, c.gpr[3], total)) c.gpr[3] = total;
    });
    own(kGetSize, "GcnRasterGetSize", [](Cpu& c) {
        const u32 out = c.gpr[3];
        const u32 raster = c.mem_read32(c.gpr[4]);
        if (raster == 0u) {
            c.mem_write32(out, 0x64u);
            c.gpr[3] = 1u;
            return;
        }
        u32 size = 0x74u;
        if (((c.mem_read8(raster + 0x23u) << 8) & 0x6000u) != 0u) {   // a paletted raster keeps 1 << depth halfword entries
            const u32 depth = c.mem_read32(raster + 0x14u);
            size = 0x74u + (((depth & 0x20u) != 0u ? 0u : (1u << (depth & 31u))) << 1);
        }
        u32 pixels = 0;
        const u32 sp0 = c.gpr[1];   // the original's 0x10-byte frame: the error report below it points into it
        c.mem_write32(sp0 - 0x10u, sp0);
        c.gpr[1] = sp0 - 0x10u;
        const bool ok = all_mip_bytes(c, raster, pixels);
        c.gpr[1] = sp0;
        if (!ok) return;
        c.mem_write32(out, size + 4u + pixels);
        c.gpr[3] = 1u;
    });
}

}  // namespace gekko::rw_raster
