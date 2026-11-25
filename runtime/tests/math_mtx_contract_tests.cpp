// liveness-roots: reference-interpreter-only
// Contract for the native owner of the Dolphin SDK's PSMTXConcat 0x8037976C (gekko/native_math_mtx.h). The ORIGINAL words
// (paired-single psq_l / ps_muls0 / ps_madds0/1 / psq_st) run in the reference interpreter from the pinned DOL; the native owner
// runs on an identical machine (same memory, same registers).
//
// Inputs: three data classes per call - "plain" floats in +-[0.25, 4) (the fast path's territory), mostly plain with a few
// exceptional words, and the full exceptional zoo (+-0, subnormals, +-inf, NaNs with payloads, +-1, huge, tiny, around 2^-100,
// random bits); the constant pair at 0x805EEC18 now and then replaced (the original reads it at each call); ab equal to a, to b,
// a equal to b (and to ab), ab overlapping a or b by 4..44 bytes, and a, b or ab overlapping the 0x40-byte frame the original
// writes below SP; FPSCR with and without the non-IEEE bit and with sticky bits set; arbitrary 64-bit patterns in every FPR's
// ps0 / ps1; GQR0 type 0 and now and then a quantised type.
// Compared after each call: every GPR but r6 (the original's scratch: the constant's address), LR, CTR, the whole CR, f14..f31
// (both halves), FPSCR, the constant's two words and every byte of the 0x2000-byte arena (matrices, outputs AND the stack words
// the original writes below SP). NOT compared (volatile by the ABI): r6, f0..f13.
// The fast path's acceptance is counted on a third identical machine.
// Negative controls (the run must FAIL): --control-quirk (the fourth fused step of the last row removed), --control-order
// (one ps_madds1 turned into ps_madds0), --control-frame (stfd f31 moved by 4 bytes).
// usage: math_mtx_contract_tests <sys/main.dol> [--control-...]
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_math_mtx.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace gekko;
using namespace gekko::math_mtx;
constexpr u32 kArena = 0x80700000u, kArenaBytes = 0x2000u, kReturn = 0x80100000u;
constexpr u32 kSp = kArena + 0x1F00u;
constexpr u32 kFrameLow = kSp - 0x40u;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

u32 g_state = 4242u;
u32 rnd() {
    g_state = g_state * 1664525u + 1013904223u;
    return g_state ^ (g_state >> 15);
}
u64 rnd64() { return (u64{rnd()} << 32) | rnd(); }
u32 exceptional_word() {
    static const u32 special[] = {0x00000000u, 0x80000000u, 0x00000001u, 0x80000001u, 0x007FFFFFu, 0x807FFFFFu,
                                  0x00800000u, 0x80800000u, 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0xFFC00000u,
                                  0x7FC12345u, 0xFFD00001u, 0x7F800001u, 0x3F800000u, 0xBF800000u, 0x7F7FFFFFu,
                                  0xFF7FFFFFu, 0x34000000u, 0x4B800000u, 0x1E000000u, 0x0D800000u, 0x7E000000u, 0x00FFFFFFu};
    switch (rnd() % 10u) {
    case 0: return special[rnd() % (sizeof(special) / sizeof(special[0]))];
    case 1: return rnd();
    case 2: return (rnd() & 0x80000000u) | (rnd() & 0x00FFFFFFu);                 // subnormal / tiny
    case 3: return (rnd() & 0x80000000u) | 0x1A000000u | (rnd() & 0x03FFFFFFu);  // around 2^-100
    default: return (rnd() & 0x80000000u) | 0x3E800000u | (rnd() & 0x00FFFFFFu);  // +-[0.25, ~4]
    }
}
u32 plain_word() { return (rnd() & 0x80000000u) | 0x3E800000u | (rnd() & 0x00FFFFFFu); }

u64 single_as_double(u32 word) {
    const u32 sign = word >> 31, exp = (word >> 23) & 0xFFu, frac = word & 0x7FFFFFu;
    if (exp == 0xFFu) return (u64{sign} << 63) | (0x7FFull << 52) | (u64{frac} << 29);
    if (exp == 0u) {
        float f;
        std::memcpy(&f, &word, 4);
        double d = static_cast<double>(f);
        u64 bits;
        std::memcpy(&bits, &d, 8);
        return bits;
    }
    return (u64{sign} << 63) | (u64{exp - 127u + 1023u} << 52) | (u64{frac} << 29);
}

struct Machine {
    GuestMemory memory;
    Cpu cpu{memory};
    explicit Machine(const std::vector<u8>& dol) {
        const auto word = [&](std::size_t o) {
            return u32{dol[o]} << 24 | u32{dol[o + 1]} << 16 | u32{dol[o + 2]} << 8 | u32{dol[o + 3]};
        };
        for (u32 i = 0; i < 18u; ++i) {
            const u32 offset = word(i * 4u), address = word(0x48u + i * 4u), size = word(0x90u + i * 4u);
            if (size == 0u) continue;
            require(std::uint64_t(offset) + size <= dol.size(), "invalid DOL extent");
            memory.write_block(address, dol.data() + offset, size);
        }
        cpu.reset();
    }
    u64 hash() {
        u64 h = 0xCBF29CE484222325ull;
        std::vector<u8> block(kArenaBytes);
        memory.read_block(kArena, block.data(), block.size());
        for (u8 b : block) h = (h ^ b) * 0x100000001B3ull;
        return h;
    }
};
}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc >= 2, "usage: math_mtx_contract_tests main.dol [--control-...]");
        const std::string control = argc >= 3 ? argv[2] : "";
        std::ifstream input(argv[1], std::ios::binary);
        const std::vector<u8> dol{std::istreambuf_iterator<char>(input), {}};
        require(dol.size() >= 256, "missing DOL");
        u64 hash = 0xCBF29CE484222325ull;
        for (u8 byte : dol) hash = (hash ^ byte) * 0x100000001B3ull;
        require(hash == 0xD2CFB4A1CA897F9Bull, "unexpected original DOL identity");
        Machine o(dol), n(dol), p(dol);
        // stwu r1,-0x40(r1); the last word blr; the constant pair (0.0f, 1.0f)
        require(o.memory.read32(kConcat) == 0x9421FFC0u && o.memory.read32(kConcat + 0xC8u) == 0x4E800020u,
                "unexpected routine words");
        require(o.memory.read32(kUnit01) == 0x00000000u && o.memory.read32(kUnit01 + 4u) == 0x3F800000u,
                "unexpected constant pair at 0x805EEC18");
        if (control == "--control-quirk") {  // ps_madds1 f0, f31, f5, f0 (the last row's fourth step) -> nop
            const u32 at = 0x80379820u;
            require((o.memory.read32(at) >> 26) == 4u && ((o.memory.read32(at) >> 1) & 0x1Fu) == 15u, "control-quirk word");
            o.memory.write32(at, 0x60000000u);
        } else if (control == "--control-order") {  // ps_madds1 f12, f8, f0, f12 -> ps_madds0 (a[0][0] instead of a[0][1])
            const u32 at = 0x803797B4u, w = o.memory.read32(at);
            require((w >> 26) == 4u && ((w >> 1) & 0x1Fu) == 15u, "control-order word");
            o.memory.write32(at, (w & ~0x3Eu) | (14u << 1));
        } else if (control == "--control-frame") {  // stfd f31, 0x28(r1) -> 0x2C(r1)
            const u32 at = 0x8037978Cu, w = o.memory.read32(at);
            require((w >> 26) == 54u && (w & 0xFFFFu) == 0x28u, "control-frame word");
            o.memory.write32(at, (w & 0xFFFF0000u) | 0x2Cu);
        } else {
            require(control.empty(), "unknown control " + control);
        }

        const u32 a_at = kArena + 0x100u, b_at = kArena + 0x200u, out_at = kArena + 0x300u;
        constexpr unsigned kCalls = 60000u;
        unsigned fast = 0, aliased = 0, frame_overlap = 0, constant_changed = 0, nonplain_gqr = 0, nonfinite = 0;
        for (unsigned iteration = 0; iteration < kCalls; ++iteration) {
            // 0 none, 1 ab == a, 2 ab == b, 3 a == b, 4 a == b == ab, 5 ab inside a, 6 ab overlapping b from below,
            // 7 ab overlapping the frame, 8 a or b overlapping the frame, 9 none
            const u32 mode = iteration % 10u;
            u32 r3 = a_at, r4 = b_at, r5 = out_at;
            if (mode == 1) r5 = a_at;
            else if (mode == 2) r5 = b_at;
            else if (mode == 3) r4 = a_at;
            else if (mode == 4) r4 = r5 = a_at;
            else if (mode == 5) r5 = a_at + 4u * (1u + rnd() % 11u);
            else if (mode == 6) r5 = b_at - 4u * (1u + rnd() % 11u);
            else if (mode == 7) r5 = kFrameLow - 0x2Cu + 4u * (rnd() % 26u);
            else if (mode == 8) {
                const u32 at = kFrameLow - 0x2Cu + 4u * (rnd() % 26u);
                if (rnd() % 2u) r3 = at;
                else r4 = at;
            }
            if (mode >= 1u && mode <= 6u) ++aliased;
            if (mode == 7u || mode == 8u) ++frame_overlap;
            std::vector<u32> arena(kArenaBytes / 4u);
            for (u32& w : arena) w = rnd();
            const u32 data_class = rnd() % 8u;  // 0..4 plain, 5 mostly plain, 6..7 exceptional zoo
            const auto element = [&]() -> u32 {
                if (data_class < 5u) return plain_word();
                if (data_class == 5u) return rnd() % 12u == 0u ? exceptional_word() : plain_word();
                return exceptional_word();
            };
            for (u32 i = 0; i < 12u; ++i) {
                arena[(0x100u + 4u * i) / 4u] = element();
                arena[(0x200u + 4u * i) / 4u] = element();
                arena[(r3 - kArena + 4u * i) / 4u] = element();  // a / b placed in the frame area get data too
                arena[(r4 - kArena + 4u * i) / 4u] = element();
            }
            u32 unit[2] = {0x00000000u, 0x3F800000u};
            if (rnd() % 10u == 0u) {
                ++constant_changed;
                unit[rnd() % 2u] = rnd() % 2u ? exceptional_word() : plain_word();
            }
            const u32 fpscr = (rnd() & 1u ? 0x4u : 0u) | (rnd() & 1u ? 0xF8000000u & rnd() : 0u);
            u32 gqr0 = 0u;
            if (rnd() % 10u == 0u) {
                gqr0 = (4u + rnd() % 4u) | ((4u + rnd() % 4u) << 16) | ((rnd() & 0x3Fu) << 8) | ((rnd() & 0x3Fu) << 24);
                ++nonplain_gqr;
            } else if (rnd() % 10u == 0u) {
                gqr0 = (rnd() % 4u) | ((rnd() % 4u) << 16) | ((rnd() & 0x3Fu) << 8) | ((rnd() & 0x3Fu) << 24);  // reserved float types
            }
            u32 gpr_seed[32];
            u64 fpr_seed[32][2];
            {
                u32 x = rnd();
                for (u32 i = 0; i < 32u; ++i) {
                    x = x * 1664525u + 1013904223u;
                    gpr_seed[i] = x;
                    fpr_seed[i][0] = rnd() % 4u == 0u ? rnd64() : single_as_double(exceptional_word());
                    fpr_seed[i][1] = rnd() % 3u == 0u ? rnd64() : single_as_double(exceptional_word());
                }
            }
            for (Machine* m : {&o, &n, &p}) {
                Cpu& c = m->cpu;
                c.reset();
                for (u32 i = 0; i < kArenaBytes / 4u; ++i) m->memory.write32(kArena + 4u * i, arena[i]);
                m->memory.write32(kUnit01, unit[0]);
                m->memory.write32(kUnit01 + 4u, unit[1]);
                for (u32 i = 0; i < 32u; ++i) {
                    c.gpr[i] = gpr_seed[i];
                    c.fpr[i].bits[0] = fpr_seed[i][0];
                    c.fpr[i].bits[1] = fpr_seed[i][1];
                }
                c.gpr[1] = kSp;
                c.gpr[3] = r3; c.gpr[4] = r4; c.gpr[5] = r5;
                c.lr = kReturn;
                c.ctr = 0x4242u;
                c.cr = 0x31415926u;
                c.fpscr = fpscr;
                c.spr[SPR_GQR0] = gqr0;
            }
            if (detail::concat_fast(p.cpu, r3, r4, r5)) ++fast;
            Cpu& c = o.cpu;
            c.pc = kConcat;
            c.npc = kConcat + 4u;
            for (unsigned steps = 0; steps < 200 && c.pc != kReturn && !c.stopped(); ++steps) c.run_steps(1u);
            require(c.pc == kReturn, "the original did not return");
            Cpu& d = n.cpu;
            serve_concat(d);
            const std::string at = " iteration " + std::to_string(iteration) + " mode " + std::to_string(mode) + " class " +
                                   std::to_string(data_class) + " gqr0 " + std::to_string(gqr0);
            for (u32 i = 0; i < 32u; ++i)
                if (i != 6u) require(c.gpr[i] == d.gpr[i], "r" + std::to_string(i) + " differs" + at);
            require(c.lr == d.lr, "LR differs" + at);
            require(c.ctr == d.ctr, "CTR differs" + at);
            require(c.cr == d.cr, "CR differs" + at);
            for (u32 i = 14; i < 32u; ++i)
                require(c.fpr[i].bits[0] == d.fpr[i].bits[0] && c.fpr[i].bits[1] == d.fpr[i].bits[1],
                        "f" + std::to_string(i) + " differs (ps0 " + std::to_string(c.fpr[i].bits[0]) + " vs " +
                            std::to_string(d.fpr[i].bits[0]) + ", ps1 " + std::to_string(c.fpr[i].bits[1]) + " vs " +
                            std::to_string(d.fpr[i].bits[1]) + ")" + at);
            require(c.fpscr == d.fpscr, "FPSCR differs: original " + std::to_string(c.fpscr) + " native " + std::to_string(d.fpscr) + at);
            require(o.memory.read32(kUnit01) == n.memory.read32(kUnit01) &&
                        o.memory.read32(kUnit01 + 4u) == n.memory.read32(kUnit01 + 4u),
                    "the constant pair differs" + at);
            require(o.hash() == n.hash(), "memory differs" + at);
            for (u32 i = 0; i < 12u; ++i) nonfinite += ((o.memory.read32(r5 + 4u * i) >> 23) & 0xFFu) == 0xFFu;
        }
        std::printf("PASS PSMTXConcat 0x8037976C: %u calls; every GPR but r6, LR, CTR, CR, f14-f31 (both halves), FPSCR, the "
                    "constant pair and the whole 0x2000-byte arena (including the stack frame) equal to the original; %u with "
                    "a, b and ab aliased, %u overlapping the frame, %u with the constant changed, %u GQR0 quantised, %u "
                    "non-finite result words; fast path accepted %u (%.0f %%)\n",
                    kCalls, aliased, frame_overlap, constant_changed, nonplain_gqr, nonfinite, fast, 100.0 * fast / kCalls);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL PSMTXConcat contract: %s\n", error.what());
        return 1;
    }
}
