// The GX texture-object functions as native owners (gekko/native_gx_texobj.h).

#include "gekko/native_gx_texobj.h"

#include "gekko/cpu.h"

#include <bit>
#include <cmath>
#include <cstring>
#include <string>

namespace gekko::gx_texobj {

namespace {

constexpr Address kAddresses[] = {
    {kInitTexObj, "GXInitTexObj"},
    {kInitTexObjCI, "GXInitTexObjCI"},
    {kInitTexObjLod, "GXInitTexObjLOD"},
    {kGetLodBias, "GXGetTexObjLODBias"},
    {kGetBiasClamp, "GXGetTexObjBiasClamp"},
    {kGetEdgeLod, "GXGetTexObjEdgeLOD"},
    {kGetMaxAniso, "GXGetTexObjMaxAniso"},
    {kInitTexCacheRegion, "GXInitTexCacheRegion"},
    {kGetProjectionv, "GXGetProjectionv"},
};

constexpr Address kLiftedAddresses[] = {
    {kLoadTexObjPreLoaded, "GX texture load (pre-loaded)"},
    {kLoadTexObj, "GXLoadTexObj"},
    {kLoadTlut, "GXLoadTlut"},
    {kInvalidateTexAll, "GXInvalidateTexAll"},
};

constexpr u32 kSda2GxData = 0xFD8u;  // r2-relative pointer to __GXData

// The small-data byte table at r13 + 0x2830 (0x805EED30, 00 04 01 05 02 06): GX texture filter -> the hardware field.
constexpr u32 kSdaFilterField = 0x2830u;

// Single-precision constants of the SDA2 pool (r2 + 0x1040 ...).
constexpr float kLodScale = 16.0f;       // 0x1040
constexpr float kBiasMin = -4.0f;        // 0x1050
constexpr float kBiasLimit = 4.0f;       // 0x1054
constexpr float kBiasMax = 3.99000000953674316f;  // 0x1058 (0x407F5C29)
constexpr float kBiasScale = 32.0f;      // 0x105C
constexpr float kLodMin = 0.0f;          // 0x1060
constexpr float kLodMax = 10.0f;         // 0x1064
constexpr float kBiasUnit = 0.03125f;    // 0x1068

u32 rotl(u32 value, unsigned amount) noexcept { return amount == 0u ? value : (value << amount) | (value >> (32u - amount)); }
// rlwimi rA, rS, sh, mb, me with the mask given: (rotl(rS, sh) & mask) | (rA & ~mask)
u32 insert(u32 target, u32 source, unsigned shift, u32 mask) noexcept { return (rotl(source, shift) & mask) | (target & ~mask); }

double fpr_double(const Cpu& cpu, unsigned n) noexcept {
    double value;
    std::memcpy(&value, &cpu.fpr[n].bits[0], sizeof value);
    return value;
}
void set_fpr_single(Cpu& cpu, unsigned n, float value) noexcept {
    const double widened = static_cast<double>(value);
    u64 bits;
    std::memcpy(&bits, &widened, sizeof bits);
    cpu.fpr[n].bits[0] = bits;
    cpu.fpr[n].bits[1] = bits;
}
// fctiwz as the reference interpreter does it (FPSCR[NI] clear): NaN and negative overflow give 0x80000000, positive overflow 0x7FFFFFFF.
s32 fctiwz(double value) noexcept {
    if (std::isnan(value)) return static_cast<s32>(0x80000000u);
    if (value > 2147483647.0) return 2147483647;
    if (value < -2147483648.0) return static_cast<s32>(0x80000000u);
    return static_cast<s32>(std::trunc(value));
}
float fmuls(float a, double b) noexcept { return static_cast<float>(static_cast<double>(a) * b); }

// lfs / fcmpu / fmr: clamp as the original does, including its handling of NaN (a NaN falls through every branch untouched).
double clamp_bias(double value) noexcept {
    if (value < static_cast<double>(kBiasMin)) return kBiasMin;
    if (value >= static_cast<double>(kBiasLimit)) return kBiasMax;
    return value;
}
double clamp_lod(double value) noexcept {
    if (value < static_cast<double>(kLodMin)) return kLodMin;
    if (value > static_cast<double>(kLodMax)) return kLodMax;
    return value;
}

void zero_object(Cpu& cpu, u32 obj) {
    if ((obj & 3u) == 0u) {
        for (u32 i = 0; i < 0x20u; i += 4u) cpu.mem_write32(obj + i, 0u);
    } else {
        for (u32 i = 0; i < 0x20u; ++i) cpu.mem_write8(obj + i, 0u);
    }
}

void init_tex_obj(Cpu& cpu, u32 obj, u32 image, u32 width, u32 height, u32 format, u32 wrap_s, u32 wrap_t, u32 mipmap) {
    zero_object(cpu, obj);
    u32 w0 = 0, w1 = 0, w2 = 0, w3 = 0;
    u8 flags = 0;
    w0 = insert(w0, wrap_s, 0, 0x00000003u);
    w0 = insert(w0, wrap_t, 2, 0x0000000Cu);
    w0 = insert(w0, 1u, 4, 0x00000010u);
    const u32 w16 = width & 0xFFFFu, h16 = height & 0xFFFFu;
    if ((mipmap & 0xFFu) != 0u) {
        flags |= 1u;
        w0 = insert(w0, (format - 8u) <= 2u ? 5u : 6u, 5, 0x000000E0u);
        const unsigned lead = static_cast<unsigned>(std::countl_zero(w16 > h16 ? w16 : h16));
        const u32 count = 31u - lead;  // may wrap (both sizes zero): the original converts the unsigned word
        const float scaled = fmuls(kLodScale, static_cast<double>(static_cast<float>(static_cast<double>(count))));
        w1 = insert(w1, static_cast<u32>(fctiwz(scaled)), 8, 0x0000FF00u);
    } else {
        w0 = insert(w0, 4u, 5, 0x000000E0u);
    }
    w2 = insert(w2, w16 - 1u, 0, 0x000003FFu);
    w2 = insert(w2, h16 - 1u, 10, 0x000FFC00u);
    w2 = insert(w2, format, 20, 0x00F00000u);
    w3 = insert(w3, image, 27, 0x001FFFFFu);

    // The block-size table, indexed by the low four bits of the format (the six interior labels of the original).
    struct Blocks {
        u8 flag;
        u8 x_shift;
        u8 y_shift;
    };
    static constexpr Blocks kBlocks[16] = {
        {1, 3, 3}, {2, 3, 2}, {2, 3, 2}, {2, 2, 2}, {2, 2, 2}, {2, 2, 2}, {3, 2, 2}, {2, 2, 2},
        {1, 3, 3}, {2, 3, 2}, {2, 2, 2}, {2, 2, 2}, {2, 2, 2}, {2, 2, 2}, {0, 3, 3}, {2, 2, 2},
    };
    const Blocks& block = kBlocks[format & 0xFu];
    const u32 xs = block.x_shift, ys = block.y_shift;
    const s32 across = (static_cast<s32>(w16 + ((1u << xs) - 1u))) >> xs;
    const s32 down = (static_cast<s32>(h16 + ((1u << ys) - 1u))) >> ys;
    const u32 blocks = static_cast<u32>(across * down) & 0x7FFFu;

    cpu.mem_write32(obj + 0x00u, w0);
    cpu.mem_write32(obj + 0x04u, w1);
    cpu.mem_write32(obj + 0x08u, w2);
    cpu.mem_write32(obj + 0x0Cu, w3);
    cpu.mem_write32(obj + 0x14u, format);
    cpu.mem_write16(obj + 0x1Cu, static_cast<u16>(blocks));
    cpu.mem_write8(obj + 0x1Eu, block.flag);
    cpu.mem_write8(obj + 0x1Fu, static_cast<u8>(flags | 2u));
}

}  // namespace

const Address* addresses(std::size_t& count) noexcept {
    count = sizeof(kAddresses) / sizeof(kAddresses[0]);
    return kAddresses;
}

const Address* lifted_addresses(std::size_t& count) noexcept {
    count = sizeof(kLiftedAddresses) / sizeof(kLiftedAddresses[0]);
    return kLiftedAddresses;
}

void serve(Cpu& cpu, u32 address) {
    const u32 r3 = cpu.gpr[3], r4 = cpu.gpr[4], r5 = cpu.gpr[5], r6 = cpu.gpr[6], r7 = cpu.gpr[7], r8 = cpu.gpr[8];
    switch (address) {
    case kInitTexObj:
        init_tex_obj(cpu, r3, r4, r5, r6, r7, r8, cpu.gpr[9], cpu.gpr[10]);
        break;
    case kInitTexObjCI: {
        const u32 tlut = cpu.mem_read32(cpu.gpr[1] + 8u);  // the ninth argument: the first word of the caller's parameter area
        init_tex_obj(cpu, r3, r4, r5, r6, r7, r8, cpu.gpr[9], cpu.gpr[10]);
        cpu.mem_write8(r3 + 0x1Fu, static_cast<u8>(cpu.mem_read8(r3 + 0x1Fu) & ~2u));
        cpu.mem_write32(r3 + 0x18u, tlut);
        break;
    }
    case kInitTexObjLod: {
        // r3 obj, r4 min filter, r5 mag filter, f1 min LOD, f2 max LOD, f3 LOD bias, r6 bias clamp, r7 edge LOD, r8 anisotropy
        const double bias = clamp_bias(fpr_double(cpu, 3));
        u32 w0 = cpu.mem_read32(r3);
        const float scaled_bias = fmuls(kBiasScale, bias);
        w0 = insert(w0, static_cast<u32>(fctiwz(static_cast<double>(scaled_bias))), 9, 0x0001FE00u);
        w0 = insert(w0, r5 == 1u ? 1u : 0u, 4, 0x00000010u);
        w0 = insert(w0, cpu.mem_read8(cpu.gpr[13] + kSdaFilterField + r4), 5, 0x000000E0u);
        w0 = insert(w0, (r7 & 0xFFu) != 0u ? 0u : 1u, 8, 0x00000100u);
        w0 = insert(w0, 0u, 17, 0x00020000u);
        w0 = insert(w0, 0u, 18, 0x00040000u);
        w0 = insert(w0, r8, 19, 0x00180000u);
        w0 = insert(w0, r6, 21, 0x00200000u);
        cpu.mem_write32(r3, w0);
        const double min_lod = clamp_lod(fpr_double(cpu, 1));
        const s32 min_scaled = fctiwz(static_cast<double>(fmuls(kLodScale, min_lod)));
        const double max_lod = clamp_lod(fpr_double(cpu, 2));
        u32 w1 = cpu.mem_read32(r3 + 4u);
        w1 = insert(w1, static_cast<u32>(min_scaled), 0, 0x000000FFu);
        cpu.mem_write32(r3 + 4u, w1);
        w1 = cpu.mem_read32(r3 + 4u);
        w1 = insert(w1, static_cast<u32>(fctiwz(static_cast<double>(fmuls(kLodScale, max_lod)))), 8, 0x0000FF00u);
        cpu.mem_write32(r3 + 4u, w1);
        break;
    }
    case kGetLodBias: {
        const s32 field = static_cast<s8>((cpu.mem_read32(r3) >> 9) & 0xFFu);
        set_fpr_single(cpu, 1, fmuls(static_cast<float>(field), kBiasUnit));
        break;
    }
    case kGetBiasClamp:
        cpu.gpr[3] = (cpu.mem_read32(r3) >> 21) & 1u;
        break;
    case kGetEdgeLod:
        cpu.gpr[3] = ((cpu.mem_read32(r3) >> 8) & 1u) == 0u ? 1u : 0u;
        break;
    case kGetMaxAniso:
        cpu.gpr[3] = (cpu.mem_read32(r3) >> 19) & 3u;
        break;
    case kInitTexCacheRegion: {
        u32 w0 = 0;
        w0 = insert(w0, r5, 10, 0x00000C00u);
        cpu.mem_write32(r3, w0);
        u32 w1 = cpu.mem_read32(r3 + 4u);
        w1 = insert(w1, r4, 27, 0x001FFFFFu);
        cpu.mem_write32(r3 + 4u, w1);
        w1 = cpu.mem_read32(r3 + 4u);
        w1 = insert(w1, 0x64u, 24, 0xFF000000u);
        cpu.mem_write32(r3 + 4u, w1);
        cpu.mem_write16(r3 + 8u, static_cast<u16>(r6));
        break;
    }
    case kGetProjectionv: {
        const u32 gx = cpu.mem_read32(cpu.gpr[2] + kSda2GxData);
        cpu.mem_write32(r3, cpu.mem_read32(gx + 0x4D8u) != 0u ? 0x3F800000u : 0u);
        for (u32 i = 0; i < 6u; ++i) cpu.mem_write32(r3 + 4u + 4u * i, cpu.mem_read32(gx + 0x4DCu + 4u * i));
        break;
    }
    default:
        break;
    }
}

void install(Cpu& cpu) {
    for (const Address& entry : kAddresses) {
        const u32 address = entry.address;
        cpu.install_hle(address, [address](Cpu& c) {
            serve(c, address);
            return true;
        }, std::string("native GX texture object: ") + entry.name);
    }
}

}  // namespace gekko::gx_texobj

namespace gekko::gx_texobj {

void install_lifted(Cpu& cpu) {
    for (const Address& entry : kLiftedAddresses) {
        cpu.install_hle(entry.address, [](Cpu&) { return true; }, std::string("native GX texture load: ") + entry.name);
    }
}

}  // namespace gekko::gx_texobj
