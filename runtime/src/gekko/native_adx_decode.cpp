// The native owner of the CRI ADX stereo 4-bit block decoder 0x803C5B2C (gekko/native_adx_decode.h).

#include "gekko/native_adx_decode.h"

#include "gekko/cpu.h"

namespace gekko::adx_decode {

namespace {
// mullw / add / srawi 12 as the 32-bit original computes them (wrapping), then the clamp to a signed 16-bit sample.
inline u32 predict_sum(s32 c1, s32 h1, s32 c2, s32 h2) noexcept {
    return static_cast<u32>(c1) * static_cast<u32>(h1) + static_cast<u32>(c2) * static_cast<u32>(h2);
}
inline s32 predict(s32 c1, s32 h1, s32 c2, s32 h2) noexcept { return static_cast<s32>(predict_sum(c1, h1, c2, h2)) >> 12; }
inline void set_carry(Cpu& c, bool carry) noexcept { c.xer = carry ? (c.xer | 0x20000000u) : (c.xer & ~0x20000000u); }
// XER[CA] after srawi by `shift`: set when the source is negative and a one bit is shifted out.
inline bool carry_after_srawi(u32 value, u32 shift) noexcept { return (value >> 31) != 0u && (value & ((1u << shift) - 1u)) != 0u; }
inline s32 clamp16(s32 value) noexcept { return value > 0x7FFF ? 0x7FFF : (value < -0x8000 ? -0x8000 : value); }
inline s32 mad(s32 nibble, s32 scale, s32 prediction) noexcept {
    return static_cast<s32>(static_cast<u32>(nibble) * static_cast<u32>(scale) + static_cast<u32>(prediction));
}
}  // namespace

void serve_decode_ste4(Cpu& c) {
    u32 src = c.gpr[3];
    const s32 n = static_cast<s32>(c.gpr[4]);
    const u32 out0 = c.gpr[5], hist0 = c.gpr[6], out1 = c.gpr[7], hist1 = c.gpr[8];
    const s32 coef1 = static_cast<s16>(c.gpr[9]), coef2 = static_cast<s16>(c.gpr[10]);
    const u32 sp = c.gpr[1];
    // The original's frame, written first and unconditionally (stwu r1,-0x40(r1) at 0x803C5B2C, stmw r19,0xc(r1) at
    // 0x803C5B3C): the back chain at SP-0x40 and the entry values of r19..r31 at SP-0x34..SP-1. Dead after the lmw, but
    // it lies in guest RAM, which the state report hashes, so the owner leaves the same bytes (the stmw runs before r19..r31
    // are touched, so the image is the caller's registers).
    c.mem_write32(sp - 0x40u, sp);
    for (u32 i = 19u; i < 32u; ++i) c.mem_write32(sp - 0x34u + 4u * (i - 19u), c.gpr[i]);
    const u32 key_at = c.mem_read32(sp + 8u);
    const s32 key_mult = static_cast<s16>(c.mem_read16(sp + 0xEu));
    const s32 key_add = static_cast<s16>(c.mem_read16(sp + 0x12u));

    s32 nibble_table[16];
    for (u32 i = 0; i < 16u; ++i) nibble_table[i] = static_cast<s32>(c.mem_read32(kNibbleTable + 4u * i));

    const s32 pairs = (n + (n < 0 ? 1 : 0)) >> 1;
    // XER[CA]: the original's last executed srawi decides it (srawi by 1 of the pair count first, then in each pair the
    // channel 1 low-nibble prediction, srawi by 12, is the last one).
    bool carry = carry_after_srawi(static_cast<u32>(n) + (n < 0 ? 1u : 0u), 1u);
    s32 h0a = static_cast<s16>(c.mem_read16(hist0)), h0b = static_cast<s16>(c.mem_read16(hist0 + 2u));
    s32 h1a = static_cast<s16>(c.mem_read16(hist1)), h1b = static_cast<s16>(c.mem_read16(hist1 + 2u));
    s32 done = 0;
    u32 o0 = out0, o1 = out1;

    for (; done < pairs; ++done) {
        const s32 word0 = static_cast<s16>(c.mem_read16(src));
        if (word0 & 0x8000) { c.gpr[3] = static_cast<u32>(done) << 1; if (done != 0) c.ctr = 0; set_carry(c, carry); return; }
        const s32 key0 = static_cast<s16>(c.mem_read16(key_at));
        const s32 scale0 = static_cast<s16>(((word0 ^ key0) & 0x1FFF) + 1);
        c.mem_write16(key_at, static_cast<u16>(static_cast<u32>(key0) * static_cast<u32>(key_mult) + static_cast<u32>(key_add)));
        c.mem_write16(key_at, static_cast<u16>(c.mem_read16(key_at) & 0x7FFFu));

        const s32 word1 = static_cast<s16>(c.mem_read16(src + 0x12u));
        if (word1 & 0x8000) { c.gpr[3] = static_cast<u32>(done) << 1; if (done != 0) c.ctr = 0; set_carry(c, carry); return; }
        const s32 key1 = static_cast<s16>(c.mem_read16(key_at));
        const s32 scale1 = static_cast<s16>(((word1 ^ key1) & 0x1FFF) + 1);
        c.mem_write16(key_at, static_cast<u16>(static_cast<u32>(key1) * static_cast<u32>(key_mult) + static_cast<u32>(key_add)));
        c.mem_write16(key_at, static_cast<u16>(c.mem_read16(key_at) & 0x7FFFu));

        src += 2u;
        for (u32 i = 0; i < 16u; ++i) {
            const s32 byte0 = static_cast<s8>(c.mem_read8(src));
            const s32 byte1 = static_cast<s8>(c.mem_read8(src + 0x12u));
            ++src;
            // ch0 and ch1 high nibbles
            const s32 l_hi = clamp16(mad(byte0 >> 4, scale0, predict(coef1, h0a, coef2, h0b)));
            const s32 r_hi = clamp16(mad(byte1 >> 4, scale1, predict(coef1, h1a, coef2, h1b)));
            c.mem_write16(o0, static_cast<u16>(l_hi));
            c.mem_write16(o1, static_cast<u16>(r_hi));
            // low nibbles: the original indexes the word table by (byte << 2) & 0x3C, i.e. the low nibble
            const s32 low0 = nibble_table[static_cast<u32>(byte0) & 0xFu];
            const s32 low1 = nibble_table[static_cast<u32>(byte1) & 0xFu];
            const s32 l_lo = clamp16(mad(low0, scale0, predict(coef1, l_hi, coef2, h0a)));
            const u32 r_lo_sum = predict_sum(coef1, r_hi, coef2, h1a);
            carry = carry_after_srawi(r_lo_sum, 12u);
            const s32 r_lo = clamp16(mad(low1, scale1, static_cast<s32>(r_lo_sum) >> 12));
            c.mem_write16(o0 + 2u, static_cast<u16>(l_lo));
            c.mem_write16(o1 + 2u, static_cast<u16>(r_lo));
            o0 += 4u;
            o1 += 4u;
            h0a = l_lo; h0b = l_hi;
            h1a = r_lo; h1b = r_hi;
        }
        src += 0x12u;
    }
    c.mem_write16(hist0, static_cast<u16>(h0a));
    c.mem_write16(hist0 + 2u, static_cast<u16>(h0b));
    c.mem_write16(hist1, static_cast<u16>(h1a));
    c.mem_write16(hist1 + 2u, static_cast<u16>(h1b));
    if (done != 0) c.ctr = 0;
    set_carry(c, carry);
    c.gpr[3] = c.gpr[4];
}

void install(Cpu& cpu) {
    cpu.install_hle(kDecodeSte4, [](Cpu& c) { serve_decode_ste4(c); return true; }, "ADX stereo 4-bit decode (native)");
}

}  // namespace gekko::adx_decode
