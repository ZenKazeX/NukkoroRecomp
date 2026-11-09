// The world-sector vertex-format decoders, native. Contract: include/gekko/native_rw_world_decode.h.
#include "gekko/native_rw_world_decode.h"

namespace gekko::rw_world_decode {
namespace {

// The shared tail of 0x8046798C and 0x80467AA4: `base` is the component count, `field` the multiplier selector.
u32 scaled(u32 base, u32 field) {
    if (field == 4u) return base << 2;
    if (field > 4u) return 0u;
    if (field >= 2u) return base << 1;
    return base;   // 0 and 1: the count itself (bgelr)
}

u32 components_a(Cpu& c, u32 descriptor) {
    const u32 word = c.mem_read32(descriptor + 4u);
    return scaled((word & 1u) != 0u ? 3u : 2u, (word >> 1) & 7u);
}

u32 components_b(Cpu& c, u32 descriptor) {
    switch ((c.mem_read32(descriptor + 4u) >> 10) & 7u) {
        case 1: return 3u;
        case 3: return 6u;
        case 4: return 12u;
        default: return 0u;
    }
}

u32 components_c(Cpu& c, u32 descriptor, u32 index) {
    const u32 shift = ((index << 2) & 0x3FCu) + 14u;   // srw: a shift of 32..63 gives 0, the amount is taken mod 64
    const u32 word = c.mem_read32(descriptor + 4u);
    const u32 field = ((shift & 0x3Fu) >= 32u ? 0u : word >> (shift & 0x3Fu)) & 7u;
    static constexpr u32 kAnswer[8] = {2u, 3u, 4u, 2u, 3u, 4u, 0u, 0u};
    return kAnswer[field];
}

u32 components_d(Cpu& c, u32 descriptor, u32 index) {
    const u32 selector = index & 0xFFu;
    if (selector > 7u) return 0u;
    // (word offset, bit of the 2-way selector, shift of the 3-bit multiplier field) per selector
    struct Entry { u32 offset, bit, field; };
    static constexpr Entry kEntries[8] = {{4u, 21u, 22u}, {8u, 0u, 1u},  {8u, 9u, 10u},  {8u, 18u, 19u},
                                          {8u, 27u, 28u}, {0xCu, 5u, 6u}, {0xCu, 14u, 15u}, {0xCu, 23u, 24u}};
    const Entry& e = kEntries[selector];
    const u32 word = c.mem_read32(descriptor + e.offset);
    const u32 base = ((word >> e.bit) & 1u) != 0u ? 2u : 1u;
    return scaled(base, (word >> e.field) & 7u);
}

}  // namespace

void install(Cpu& cpu) {
    cpu.install_hle(kComponentsA, [](Cpu& c) {
        c.gpr[3] = components_a(c, c.gpr[3]);
        return true;
    }, "world-sector vertex-format decoder 0x8046798C (rwR4 owner)");
    cpu.install_hle(kComponentsB, [](Cpu& c) {
        c.gpr[3] = components_b(c, c.gpr[3]);
        return true;
    }, "world-sector vertex-format decoder 0x804679F0 (rwR4 owner)");
    cpu.install_hle(kComponentsC, [](Cpu& c) {
        c.gpr[3] = components_c(c, c.gpr[3], c.gpr[4]);
        return true;
    }, "world-sector vertex-format decoder 0x80467A3C (rwR4 owner)");
    cpu.install_hle(kComponentsD, [](Cpu& c) {
        c.gpr[3] = components_d(c, c.gpr[3], c.gpr[4]);
        return true;
    }, "world-sector vertex-format decoder 0x80467AA4 (rwR4 owner)");
}

}  // namespace gekko::rw_world_decode
