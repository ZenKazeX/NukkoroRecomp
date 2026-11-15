// The RenderWare texture bind 0x8049AC88 as a native owner (gekko/native_rw_texbind.h).

#include "gekko/native_rw_texbind.h"

#include "gekko/cpu.h"
#include "gekko/native_gx_texobj.h"
#include "gekko/ppc_ops.h"

#include <bit>
#include <cstring>

namespace gekko::rw_texbind {

namespace {

constexpr u32 kTable = 0x8056FC90u;  // r30: the filter table (+0), the object-format words (+0x38), the bound table (+0x4C)
constexpr u32 kSdaRasterPlugin = 0x6200u;
constexpr u32 kSdaDefaultRaster = 0x6208u;
constexpr u32 kSdaDefaultTexture = 0x620Cu;
constexpr u32 kSdaTexturePlugin = 0x6218u;
constexpr u32 kSdaVersionHalf = 0x2AF0u;
constexpr u32 kSda2Float = 0x1D18u;  // the single LOD constant (r2 + 0x1D18)
constexpr u32 kSda2Magic = 0x1D20u;  // the int-to-float double (r2 + 0x1D20)

// PowerPC mask of bits mb..me (bit 0 = most significant), wrapping when mb > me.
constexpr u32 mask(unsigned mb, unsigned me) noexcept {
    const u32 head = 0xFFFFFFFFu >> mb;
    const u32 tail = 0xFFFFFFFFu << (31u - me);
    return mb <= me ? (head & tail) : (head | tail);
}
constexpr u32 rotl(u32 v, unsigned n) noexcept { return n == 0u ? v : (v << n) | (v >> (32u - n)); }
constexpr u32 rlwinm(u32 v, unsigned sh, unsigned mb, unsigned me) noexcept { return rotl(v, sh) & mask(mb, me); }

double fpr_double(const Cpu& cpu, unsigned n) noexcept {
    double value;
    std::memcpy(&value, &cpu.fpr[n].bits[0], sizeof value);
    return value;
}
void set_fpr_pair(Cpu& cpu, unsigned n, double value) noexcept {
    u64 bits;
    std::memcpy(&bits, &value, sizeof bits);
    cpu.fpr[n].bits[0] = bits;
    cpu.fpr[n].bits[1] = bits;
}

void call(Cpu& c, u32 address) { gx_texobj::serve(c, address); }

struct Kept {  // what the GXGetTexObj* getters return for the object about to be replaced
    u32 aniso = 0, edge = 0, clamp = 0;
    double bias = 0.0;
};

Kept read_kept(Cpu& c, u32 obj) {
    Kept k;
    c.gpr[3] = obj;
    call(c, gx_texobj::kGetMaxAniso);
    k.aniso = c.gpr[3];
    c.gpr[3] = obj;
    call(c, gx_texobj::kGetEdgeLod);
    k.edge = c.gpr[3] & 0xFFu;
    c.gpr[3] = obj;
    call(c, gx_texobj::kGetBiasClamp);
    k.clamp = c.gpr[3] & 0xFFu;
    c.gpr[3] = obj;
    call(c, gx_texobj::kGetLodBias);
    k.bias = fpr_double(c, 1);
    return k;
}

// The five inlined copies of the object initialisation. tlut_else is the TLUT name used when the object's TLUT flag is clear (the unit
// in the colour-index texture's own paths, 0 in the other copies); keep the previous LOD parameters (null: the defaults 1, 1, 0 and
// bias = the constant).
void build(Cpu& c, u32 frame, u32 r31, u32 tlut_else, const Kept* keep) {
    const u32 r13 = c.gpr[13], r2 = c.gpr[2];
    const u32 raster = c.mem_read32(r31);
    const u32 r25 = r31 + c.mem_read32(r13 + kSdaTexturePlugin);
    const u32 b = c.mem_read8(raster + 0x23u);
    const u32 r26 = c.mem_read32(raster) + c.mem_read32(r13 + kSdaRasterPlugin);
    const bool indexed = rlwinm(b, 8, 0x11, 0x12) != 0u;
    const u32 r21 = rlwinm(b, 8, 0x10, 0x17);
    const u32 table = kTable + 0x38u;
    u32 filter_lo = 0, filter_hi = 0;
    if (indexed) {
        const u32 tlut = (c.mem_read32(r25 + 0x20u) & mask(6, 6)) != 0u ? c.mem_read32(r25 + 0x18u) : tlut_else;
        const u32 r4 = c.mem_read32(r31 + 0x50u);
        const u32 r10 = rlwinm(r21, 0x11, 0x1F, 0x1F);
        const u32 r8 = c.mem_read32(table + rlwinm(r4, 0x1A, 0x1A, 0x1D));
        const u32 r9i = rlwinm(r4, 0x16, 0x1A, 0x1D);
        c.mem_write32(frame + 8u, tlut);
        const u32 r5 = c.mem_read32(raster + 0xCu) & 0xFFFFu;
        const u32 r6 = c.mem_read32(raster + 0x10u) & 0xFFFFu;
        c.gpr[3] = r25;
        c.gpr[4] = c.mem_read32(r26 + 0x1Cu);
        c.gpr[5] = r5;
        c.gpr[6] = r6;
        c.gpr[7] = c.mem_read32(r26 + 0xCu);
        c.gpr[8] = r8;
        c.gpr[9] = c.mem_read32(table + r9i);
        c.gpr[10] = r10;
        call(c, gx_texobj::kInitTexObjCI);
        const u32 selector = c.mem_read32(r31 + 0x50u) & 0xFFu;
        if (selector == 6u || selector == 5u) {
            filter_lo = c.mem_read32(kTable + 0x20u);
            filter_hi = c.mem_read32(kTable + 0x24u);
        } else {
            filter_lo = c.mem_read32(kTable + (selector << 3));
            filter_hi = c.mem_read32(kTable + (selector << 3) + 4u);
        }
    } else {
        const u32 r0 = c.mem_read32(r31 + 0x50u);
        const u32 r10 = rlwinm(r21, 0x11, 0x1F, 0x1F);
        const u32 r5i = rlwinm(r0, 0x1A, 0x1A, 0x1D);
        const u32 r0i = rlwinm(r0, 0x16, 0x1A, 0x1D);
        const u32 r8 = c.mem_read32(table + r5i);
        const u32 r5 = c.mem_read32(raster + 0xCu) & 0xFFFFu;
        const u32 r11 = c.mem_read32(raster + 0x10u);
        const u32 r9 = c.mem_read32(table + r0i);
        c.gpr[3] = r25;
        c.gpr[4] = c.mem_read32(r26 + 0x1Cu);
        c.gpr[5] = r5;
        c.gpr[6] = r11 & 0xFFFFu;
        c.gpr[7] = c.mem_read32(r26 + 0xCu);
        c.gpr[8] = r8;
        c.gpr[9] = r9;
        c.gpr[10] = r10;
        call(c, gx_texobj::kInitTexObj);
        const u32 offset = rlwinm(c.mem_read32(r31 + 0x50u), 3, 0x15, 0x1C);
        filter_lo = c.mem_read32(kTable + offset);
        filter_hi = c.mem_read32(kTable + offset + 4u);
    }
    // GXInitTexObjLOD(obj, filter lo, filter hi, f1 = constant, f2 = (float)levels, f3 = bias, clamp, edge, aniso)
    const u32 levels = c.mem_read8(r26 + 0x32u);
    const u64 magic_bits = (static_cast<u64>(c.mem_read32(r2 + kSda2Magic)) << 32) | c.mem_read32(r2 + kSda2Magic + 4u);
    double magic;
    std::memcpy(&magic, &magic_bits, sizeof magic);
    const u64 f0_bits = (static_cast<u64>(0x43300000u) << 32) | levels;
    double f0;
    std::memcpy(&f0, &f0_bits, sizeof f0);
    const float f1 = std::bit_cast<float>(c.mem_read32(r2 + kSda2Float));
    const float f2 = static_cast<float>(f0 - magic);  // fsubs: both operands are exact, so the single rounding is the only one
    c.gpr[3] = r25;
    c.gpr[4] = filter_lo;
    c.gpr[5] = filter_hi;
    set_fpr_pair(c, 1, static_cast<double>(f1));
    set_fpr_pair(c, 2, static_cast<double>(f2));
    if (keep != nullptr) {
        set_fpr_pair(c, 3, keep->bias);
        c.gpr[6] = keep->clamp;
        c.gpr[7] = keep->edge;
        c.gpr[8] = keep->aniso;
    } else {
        set_fpr_pair(c, 3, static_cast<double>(f1));
        c.gpr[6] = 1u;
        c.gpr[7] = 1u;
        c.gpr[8] = 0u;
    }
    call(c, gx_texobj::kInitTexObjLod);
    const u32 flags = c.mem_read32(r25 + 0x20u);
    u32 word = c.mem_read32(r31 + 0x50u);
    word = (flags & mask(0, 15)) | (word & ~mask(0, 15));  // rlwimi r0, r3, 0, 0, 15
    word &= mask(8, 6);                                    // rlwinm r0, r0, 0, 8, 6: clears the 'new' flag
    c.mem_write32(r25 + 0x20u, word);
}

}  // namespace

void bind(Cpu& c) {
    const u32 r13 = c.gpr[13];
    const u32 entry_sp = c.gpr[1];
    const u32 frame = entry_sp - 0x60u;  // stwu r1, -0x60(r1)
    // stfd f31, 0x50(r1); psq_st f31, 88(r1), 0, 0
    c.mem_write64(frame + 0x50u, c.fpr[31].bits[0]);
    ppc::psq_store(c, 31u, frame + 0x58u, false, 0u);
    c.gpr[1] = frame;

    const u32 r28 = c.gpr[4];
    u32 r31 = c.gpr[3];
    if (r31 == 0u) {  // the default texture takes the default raster: GcnTextureSetRaster(texture, raster)
        r31 = c.mem_read32(r13 + kSdaDefaultTexture);
        c.mem_write32(r31, c.mem_read32(r13 + kSdaDefaultRaster));
        c.mem_write32(r31 + c.mem_read32(r13 + kSdaTexturePlugin) + 0x20u, 0x01000000u);
    }
    const u32 raster = c.mem_read32(r31);
    const u32 r29 = r31 + c.mem_read32(r13 + kSdaTexturePlugin);
    const u32 r27 = c.mem_read32(raster) + c.mem_read32(r13 + kSdaRasterPlugin);
    c.mem_write16(r27 + 0x30u, static_cast<u16>(c.mem_read16(r13 + kSdaVersionHalf)));
    const u32 b = c.mem_read8(raster + 0x23u);
    const bool indexed = rlwinm(b, 8, 0x11, 0x12) != 0u;
    const u32 flags = c.mem_read32(r29 + 0x20u);
    const bool same_version = (c.mem_read32(r31 + 0x50u) & 0xFFFFu) == (flags & 0xFFFFu);
    if (indexed) {
        if ((flags & mask(7, 7)) != 0u) {
            build(c, frame, r31, r28, nullptr);
        } else if (!same_version) {
            const Kept keep = read_kept(c, r29);
            build(c, frame, r31, r28, &keep);
        } else if ((flags & mask(6, 6)) == 0u && r28 != c.mem_read32(r29 + 0x18u)) {  // GXGetTexObjData
            const Kept keep = read_kept(c, r29);
            build(c, frame, r31, r28, &keep);
        }
        // 0x8049B1D0: GXLoadTlut when the TLUT flag is clear - served silent in the lifted build
    } else if ((flags & mask(7, 7)) != 0u) {
        build(c, frame, r31, 0u, nullptr);
    } else if (!same_version) {
        const Kept keep = read_kept(c, r29);
        build(c, frame, r31, 0u, &keep);
    }
    // 0x8049B510: GXLoadTexObj / the pre-loaded load - served silent in the lifted build
    c.mem_write32(kTable + 0x4Cu + (r28 << 2), r31);
    // psq_l f31, 88(r1), 0, 0; lfd f31, 0x50(r1)
    ppc::psq_load(c, 31u, frame + 0x58u, false, 0u);
    c.fpr[31].bits[0] = c.mem_read64(frame + 0x50u);
    c.gpr[1] = entry_sp;
    c.gpr[3] = kTable + 0x4Cu;  // the original's r3 on return (addi r3, r30, 0x4c): dead at every call site
}

void install_lifted(Cpu& cpu) {
    cpu.install_hle(kTextureBind, [](Cpu& c) {
        bind(c);
        return true;
    }, "native RW texture bind");
}

}  // namespace gekko::rw_texbind
