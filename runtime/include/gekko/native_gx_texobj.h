#pragma once

// The GX library's texture-object functions, owned natively (docs/agents/axfx-effects.md, "Step J1").
//
// They are the GX callees of the RenderWare GameCube driver's texture bind (0x8049AC88), which holds 19 of the 22 GX bodies the
// lifted build still compiles. Every one of them produces or reads only the 32-byte GXTexObj (plus the texture-cache region
// object for 0x80398490); nothing is sent through the write-gather pipe (extract/gx-sites.txt lists no site in any of them) and
// none touches __GXData. The driver reads the object back through the getters, so these are reproduced exactly, in both trees.
//
//   0x80397E34 GXInitTexObj(obj, image, width, height, format, wrapS, wrapT, mipmap): zero the object, then the filter / wrap /
//              size / format / image-address words and, for a mipmapped texture, the LOD word; the block-size table (a jump
//              table of six interior labels, 0x80397FB4..0x80398018, which disappear with it) and the 15-bit size in blocks
//              at +0x1C. Float: the mip LOD count is 16 * (31 - clz(max(width, height))) truncated (fctiwz).
//   0x80398080 GXInitTexObjCI: GXInitTexObj, then clear the 'not a colour-index texture' bit (+0x1F bit 1) and store the TLUT
//              name (the ninth argument, on the stack) at +0x18.
//   0x803980C8 GXInitTexObjLOD(obj, minFilter, magFilter, minLOD f1, maxLOD f2, lodBias f3, biasClamp, edgeLOD, aniso): the
//              clamped LOD fields (single-precision multiplies and fctiwz) into the first two object words.
//   0x8039824C GXGetTexObjLODBias (f1), 0x8039828C GetTexObjBiasClamp, 0x80398298 GetTexObjEdgeLOD, 0x803982AC GetTexObjMaxAniso.
//   0x80398490 GXInitTexCacheRegion-shaped initialiser (region r3, r4, r5, r6): the first two words and the halfword at +8 of a texture
//              cache region.
//   0x8039A1E4 GXGetProjectionv(float* p): p[0] = 1.0 when the projection shadow word gx+0x4D8 is non-zero else 0.0, p[1..6] = the six
//              floats at gx+0x4DC. Reads the (retired, never rewritten) shadow, so exact.
//
// Lifted tree only (install_lifted), served silent: 0x803982C0 (the pre-loaded texture load), 0x8039843C (GXLoadTexObj),
// 0x803984C8 (GXLoadTlut) and 0x803987B8 (GXInvalidateTexAll). In the lifted build every pipe site of them is dead
// (extract/gx-sites.txt) and the guest words they write - the register-id top bytes of the texture / TLUT object, the texture
// region record the region callback returns, the __GXData shadow (gx+0x514.., gx+0x534.., gx+0x5AC bit 0, gx+2) - are read by
// no code outside these functions (the getters above read other bits; GXInitTexObj rewrites the object). The region callbacks
// (gx+0x4C8 / 0x4CC) are not called. The legacy tree keeps the original bodies: it draws through the guest pipelines.
//
// Registers a caller may rely on (r1, r14..r31, f14..f31, the stack above r1) are kept; the volatile registers and the FPSCR /
// CR0 status the original leaves behind are dead at every call site (the contract test compares everything else).

#include "gekko/types.h"

#include <cstddef>

namespace gekko {
class Cpu;
}

namespace gekko::gx_texobj {

struct Address {
    u32 address;
    const char* name;
};

constexpr u32 kInitTexObj = 0x80397E34u;
constexpr u32 kInitTexObjCI = 0x80398080u;
constexpr u32 kInitTexObjLod = 0x803980C8u;
constexpr u32 kGetLodBias = 0x8039824Cu;
constexpr u32 kGetBiasClamp = 0x8039828Cu;
constexpr u32 kGetEdgeLod = 0x80398298u;
constexpr u32 kGetMaxAniso = 0x803982ACu;
constexpr u32 kInitTexCacheRegion = 0x80398490u;
constexpr u32 kGetProjectionv = 0x8039A1E4u;
constexpr u32 kLoadTexObjPreLoaded = 0x803982C0u;
constexpr u32 kLoadTexObj = 0x8039843Cu;
constexpr u32 kLoadTlut = 0x803984C8u;
constexpr u32 kInvalidateTexAll = 0x803987B8u;

const Address* addresses(std::size_t& count) noexcept;         // both trees
const Address* lifted_addresses(std::size_t& count) noexcept;  // lifted tree only: the loads, served silent

// The bodies, callable by the contract test.
void serve(Cpu& cpu, u32 address);

void install(Cpu& cpu);
void install_lifted(Cpu& cpu);

struct Owners {
    static const Address* addresses(std::size_t& count) noexcept { return gx_texobj::addresses(count); }
};

}  // namespace gekko::gx_texobj
