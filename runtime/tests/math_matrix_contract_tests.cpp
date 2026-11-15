// liveness-roots: reference-interpreter-only
// Contract for the game's 3x4 matrix / vector library owners in gekko/native_math_affine.h: point 0x8040C6CC, rotate vector
// 0x8040C668, translate by 0x8040C5EC, translate by floats 0x8040C57C, scale columns 0x8040C7E0, scale by floats 0x8040C770,
// squared length 0x8040DF7C, concat 0x8040C94C, invert in place 0x8040CD24 and invert 0x8040CF48. The ORIGINAL words run in the
// reference interpreter from the pinned DOL; the native owner runs on an identical machine (same memory, same registers).
//
// Inputs: three data classes per call - "plain" floats in +-[0.25, 4) (the fast path's territory), mostly plain with a few
// exceptional words, and the full exceptional zoo (+-0, subnormals, +-inf, NaNs with payloads, +-1, huge, tiny, random bits);
// singular matrices (a zero row / two equal rows: the determinant equals the zero constant), a 0.0 constant of -0.0 or another
// value and a changed 1.0 constant (both read from guest memory at each call), outputs equal to or inside an input (full and
// partial overlap), FPSCR with and without the non-IEEE bit and with sticky bits set, arbitrary 64-bit patterns in every FPR's
// ps0 / ps1 (so the callee-saved save / restore of the framed routines is exercised on doubles that are not singles, denormal
// singles and signalling NaNs), GQR0 type 0 and now and then a quantised type, f1..f3 arguments that are not singles.
// Compared after each call: r1, r2, r3, r13..r31, LR, CTR, the whole CR, f14..f31 (both halves) and f1 (squared length), FPSCR,
// and every byte of the 0x2000-byte arena (matrices, vectors, outputs AND the stack words the framed routines write below SP).
// NOT compared (volatile / dead by the ABI): r0, r4..r12, cr1.., f0..f13.
// The fast path's acceptance is counted per entry by running the fast function on a third identical machine.
// Negative controls (the run must FAIL): --control-concat --control-invert --control-copy --control-point --control-rotate
// --control-length --control-translate-floats --control-scale-floats --control-frame --control-ps1.
// usage: math_matrix_contract_tests <sys/main.dol> [--control-...]
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_math_affine.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace gekko;
using namespace gekko::math_affine;
constexpr u32 kArena = 0x80700000u, kArenaBytes = 0x2000u, kReturn = 0x80100000u;
constexpr u32 kSp = kArena + 0x1F00u;
constexpr u32 kR2 = kArena + 0x800u - 0x1638u;  // r2 + 0x1638 = the 1.0 constant cell, + 0x163C the 0.0 constant cell

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

u32 g_state = 777u;
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
    case 2: return (rnd() & 0x80000000u) | (rnd() & 0x00FFFFFFu);  // subnormal / tiny
    case 3: return (rnd() & 0x80000000u) | 0x1A000000u | (rnd() & 0x03FFFFFFu);  // around 2^-100
    default: return (rnd() & 0x80000000u) | 0x3E800000u | (rnd() & 0x00FFFFFFu);  // +-[0.25, ~4]
    }
}
u32 plain_word() { return (rnd() & 0x80000000u) | 0x3E800000u | (rnd() & 0x00FFFFFFu); }

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

struct Entry {
    u32 address;
    const char* name;
};
constexpr Entry kEntries[] = {{kPoint, "point"},
                              {kRotateVector, "rotate vector"},
                              {kTranslateBy, "translate by"},
                              {kTranslateFloats, "translate by floats"},
                              {kScaleColumns, "scale columns"},
                              {kScaleFloats, "scale by floats"},
                              {kSquaredLength, "squared length"},
                              {kConcat, "concat"},
                              {kInvertInPlace, "invert in place"},
                              {kInvert, "invert"}};
}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc >= 2, "usage: math_matrix_contract_tests main.dol [--control-...]");
        const std::string control = argc >= 3 ? argv[2] : "";
        std::ifstream input(argv[1], std::ios::binary);
        const std::vector<u8> dol{std::istreambuf_iterator<char>(input), {}};
        require(dol.size() >= 256, "missing DOL");
        u64 hash = 0xCBF29CE484222325ull;
        for (u8 byte : dol) hash = (hash ^ byte) * 0x100000001B3ull;
        require(hash == 0xD2CFB4A1CA897F9Bull, "unexpected original DOL identity");
        Machine o(dol), n(dol), p(dol);
        require(o.memory.read32(kConcat) == 0x9421FF60u && o.memory.read32(kInvertInPlace) == 0x9421FF50u &&
                    o.memory.read32(kInvert) == 0x9421FF40u && o.memory.read32(kPoint) == 0xC0A50004u &&
                    o.memory.read32(kSquaredLength) == 0xC0230000u,
                "unexpected routine words");
        if (control == "--control-concat") o.memory.write32(0x8040CA3Cu, 0xEF38C828u);       // fadds f25 -> fsubs f25
        else if (control == "--control-invert") o.memory.write32(0x8040CE68u, 0xFDA06890u);  // fneg f13,f13 -> fmr f13,f13
        else if (control == "--control-copy") o.memory.write32(0x8040D000u, 0x4182000Cu);    // bne -> beq
        else if (control == "--control-point") o.memory.write32(0x8040C720u, 0xEC010028u);   // fadds f0,f1,f0 -> fsubs
        else if (control == "--control-rotate") o.memory.write32(0x8040C6A4u, 0xEC0101F8u);  // fmadds f0 -> fmsubs
        else if (control == "--control-length") o.memory.write32(0x8040DF94u, 0xEC010028u);  // fadds f0,f1,f0 -> fsubs
        else if (control == "--control-translate-floats") o.memory.write32(0x8040C598u, 0xEC060028u);  // fadds -> fsubs
        else if (control == "--control-scale-floats") o.memory.write32(0x8040C774u, 0xEC00082Au);      // fmuls f0,f0,f1 -> fadds
        else if (control == "--control-frame") o.memory.write32(0x8040C950u, 0xDBE10094u);   // stfd f31,0x90(r1) -> 0x94(r1)
        else if (control == "--control-ps1") o.memory.write32(0x8040CAC4u, 0x60000000u);     // psq_l f31 -> nop (ps1 stays unrounded)
        else require(control.empty(), "unknown control " + control);

        const u32 m_at = kArena + 0x100u, b_at = kArena + 0x200u, out_at = kArena + 0x300u, v_at = kArena + 0x400u;
        unsigned runs[10] = {}, fast[10] = {}, aliased = 0, singular = 0, zero_det_paths = 0, nonplain_gqr = 0, nonsingle_args = 0;
        unsigned total = 0;
        for (unsigned e = 0; e < 10u; ++e) {
            const u32 entry = kEntries[e].address;
            for (u32 iteration = 0; iteration < 6000u; ++iteration) {
                // overlap: 0 none, 1 out == m, 2 out == b/p, 3 out inside m (offset), 4 b/p inside m, 5 out inside b/p
                const u32 mode = iteration % 6u;
                u32 r3 = out_at, r4 = m_at, r5 = b_at;
                const bool in_place = entry == kInvertInPlace || entry == kTranslateBy || entry == kScaleColumns ||
                                      entry == kTranslateFloats || entry == kScaleFloats;
                if (entry == kInvertInPlace || entry == kTranslateFloats || entry == kScaleFloats) {
                    r3 = m_at; r4 = 0; r5 = 0;
                } else if (entry == kSquaredLength) {
                    r3 = mode == 0 ? v_at : m_at + 4u * (rnd() % 10u); r4 = 0; r5 = 0;
                } else if (entry == kTranslateBy || entry == kScaleColumns) {
                    r3 = m_at; r4 = v_at;
                    if (mode == 3 || mode == 4) r4 = m_at + 4u * (rnd() % 12u);
                    else if (mode == 5) r4 = m_at + 0x2Cu;
                } else if (entry == kInvert) {
                    r5 = 0;
                    if (mode == 1 || mode == 2) r3 = m_at;
                    else if (mode == 3 || mode == 5) r3 = m_at + 4u * (1u + rnd() % 10u);
                    else if (mode == 4) r4 = out_at + 4u * (rnd() % 12u);
                } else {  // point, rotate vector (p is 3 words), concat
                    if (mode == 1) r3 = m_at;
                    else if (mode == 2) r3 = b_at;
                    else if (mode == 3) r3 = m_at + 4u * (1u + rnd() % 10u);
                    else if (mode == 4) r5 = m_at + 4u * (rnd() % 12u);
                    else if (mode == 5) r3 = b_at + 4u * (1u + rnd() % 10u);
                }
                (void)in_place;
                if (mode != 0) ++aliased;
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
                    arena[(0x400u + 4u * i) / 4u] = element();
                }
                // singular source matrix: a zero row or two equal rows (the determinant then is exactly 0)
                if ((entry == kInvert || entry == kInvertInPlace) && rnd() % 5u == 0u) {
                    ++singular;
                    const u32 row = rnd() % 3u, other = (row + 1u + rnd() % 2u) % 3u;
                    for (u32 col = 0; col < 4u; ++col)
                        arena[(0x100u + 0x10u * row + 4u * col) / 4u] = (rnd() % 2u) ? 0u : arena[(0x100u + 0x10u * other + 4u * col) / 4u];
                    if (rnd() % 2u) for (u32 col = 0; col < 3u; ++col) arena[(0x100u + 0x10u * row + 4u * col) / 4u] = 0u;
                }
                // constants: 1.0 and the zero constant (+0.0 or -0.0, sometimes a different value)
                arena[0x800u / 4u] = rnd() % 8u == 0u ? exceptional_word() : 0x3F800000u;
                arena[0x804u / 4u] = rnd() % 4u == 0u ? 0x80000000u : (rnd() % 16u == 0u ? exceptional_word() : 0u);
                const u32 fpscr = (rnd() & 1u ? 0x4u : 0u) | (rnd() & 1u ? 0xF8000000u & rnd() : 0u);
                const u32 seed = rnd();
                u32 gqr0 = 0u;
                if (rnd() % 10u == 0u) {
                    gqr0 = (4u + rnd() % 4u) | ((4u + rnd() % 4u) << 16);
                    ++nonplain_gqr;
                }
                u64 farg[3];
                for (u64& a : farg) {
                    a = single_as_double(element());
                    if (rnd() % 12u == 0u) { a = rnd64(); ++nonsingle_args; }
                }
                u32 gpr_seed[32];
                u64 fpr_seed[32][2];
                {
                    u32 x = seed;
                    for (u32 i = 0; i < 32u; ++i) {
                        x = x * 1664525u + 1013904223u;
                        gpr_seed[i] = x;
                        fpr_seed[i][0] = (u64{x} << 32) | (x * 2654435761u);
                        fpr_seed[i][1] = fpr_seed[i][0] ^ (rnd() % 3u == 0u ? 0x0123456789ABCDEFull : 0ull);
                        if (i % 5u == 0u) fpr_seed[i][1] = single_as_double(exceptional_word());
                    }
                }
                for (Machine* m : {&o, &n, &p}) {
                    Cpu& c = m->cpu;
                    c.reset();
                    for (u32 i = 0; i < kArenaBytes / 4u; ++i) m->memory.write32(kArena + 4u * i, arena[i]);
                    for (u32 i = 0; i < 32u; ++i) {
                        c.gpr[i] = gpr_seed[i];
                        c.fpr[i].bits[0] = fpr_seed[i][0];
                        c.fpr[i].bits[1] = fpr_seed[i][1];
                    }
                    c.fpr[1].bits[0] = farg[0];
                    c.fpr[2].bits[0] = farg[1];
                    c.fpr[3].bits[0] = farg[2];
                    c.gpr[1] = kSp;
                    c.gpr[2] = kR2;
                    c.gpr[13] = 0x805EC500u;
                    c.gpr[3] = r3; c.gpr[4] = r4; c.gpr[5] = r5;
                    c.lr = kReturn;
                    c.ctr = 0x4242u;
                    c.cr = 0x31415926u;
                    c.fpscr = fpscr;
                    c.spr[SPR_GQR0] = gqr0;
                }
                // the fast path's acceptance on the third machine (identical state, not compared)
                {
                    Cpu& c = p.cpu;
                    bool taken = false;
                    float f1, f2, f3;
                    const auto single = [](u64 bits, float& out) {
                        const double d = bits_to_double(bits);
                        out = static_cast<float>(d);
                        return static_cast<double>(out) == d;
                    };
                    switch (entry) {
                    case kPoint: taken = detail::point_fast(c, r3, r4, r5); break;
                    case kRotateVector: taken = detail::rotate_vector_fast(c, r3, r4, r5); break;
                    case kTranslateBy: taken = detail::translate_by_fast(c, r3, bits_to_float(c.mem_read32(r4)), bits_to_float(c.mem_read32(r4 + 4u)), bits_to_float(c.mem_read32(r4 + 8u))); break;
                    case kScaleColumns: taken = detail::scale_columns_fast(c, r3, bits_to_float(c.mem_read32(r4)), bits_to_float(c.mem_read32(r4 + 4u)), bits_to_float(c.mem_read32(r4 + 8u))); break;
                    case kTranslateFloats: taken = single(farg[0], f1) && single(farg[1], f2) && single(farg[2], f3) && detail::translate_by_fast(c, r3, f1, f2, f3); break;
                    case kScaleFloats: taken = single(farg[0], f1) && single(farg[1], f2) && single(farg[2], f3) && detail::scale_columns_fast(c, r3, f1, f2, f3); break;
                    case kSquaredLength: { u64 r; taken = detail::squared_length_fast(c, r3, r); break; }
                    case kConcat: taken = detail::concat_fast(c, r3, r4, r5); break;
                    case kInvertInPlace: taken = detail::invert_in_place_fast(c, kR2, r3); break;
                    default: taken = detail::invert_fast(c, kR2, r3, r4); break;
                    }
                    if (taken) ++fast[e];
                }
                Cpu& c = o.cpu;
                c.pc = entry;
                c.npc = entry + 4u;
                for (unsigned steps = 0; steps < 2000 && c.pc != kReturn && !c.stopped(); ++steps) c.run_steps(1u);
                require(c.pc == kReturn, std::string("the original did not return: ") + kEntries[e].name);
                Cpu& d = n.cpu;
                switch (entry) {
                case kPoint: serve_point(d); break;
                case kRotateVector: serve_rotate_vector(d); break;
                case kTranslateBy: serve_translate_by(d); break;
                case kTranslateFloats: serve_translate_floats(d); break;
                case kScaleColumns: serve_scale_columns(d); break;
                case kScaleFloats: serve_scale_floats(d); break;
                case kSquaredLength: serve_squared_length(d); break;
                case kConcat: serve_concat(d); break;
                case kInvertInPlace: serve_invert_in_place(d); break;
                default: serve_invert(d); break;
                }
                const std::string at = std::string(" entry ") + kEntries[e].name + " iteration " + std::to_string(iteration) +
                                       " mode " + std::to_string(mode) + " class " + std::to_string(data_class);
                require(c.gpr[1] == d.gpr[1] && c.gpr[2] == d.gpr[2] && c.gpr[3] == d.gpr[3], "r1 / r2 / r3 differ" + at);
                for (u32 i = 13; i < 32u; ++i) require(c.gpr[i] == d.gpr[i], "r" + std::to_string(i) + " differs" + at);
                require(c.lr == d.lr, "LR differs" + at);
                require(c.ctr == d.ctr, "CTR differs" + at);
                require(c.cr == d.cr, "CR differs: original " + std::to_string(c.cr) + " native " + std::to_string(d.cr) + at);
                for (u32 i = 14; i < 32u; ++i)
                    require(c.fpr[i].bits[0] == d.fpr[i].bits[0] && c.fpr[i].bits[1] == d.fpr[i].bits[1],
                            "f" + std::to_string(i) + " differs (ps0 " + std::to_string(c.fpr[i].bits[0]) + " vs " + std::to_string(d.fpr[i].bits[0]) +
                                ", ps1 " + std::to_string(c.fpr[i].bits[1]) + " vs " + std::to_string(d.fpr[i].bits[1]) + ")" + at);
                if (entry == kSquaredLength)
                    require(c.fpr[1].bits[0] == d.fpr[1].bits[0] && c.fpr[1].bits[1] == d.fpr[1].bits[1],
                            "f1 differs: " + std::to_string(c.fpr[1].bits[0]) + " / " + std::to_string(c.fpr[1].bits[1]) + " vs " +
                                std::to_string(d.fpr[1].bits[0]) + " / " + std::to_string(d.fpr[1].bits[1]) + at);
                require(c.fpscr == d.fpscr, "FPSCR differs: original " + std::to_string(c.fpscr) + " native " + std::to_string(d.fpscr) + at);
                require(o.hash() == n.hash(), "memory differs" + at);
                if ((entry == kInvert || entry == kInvertInPlace) && (c.cr & 0x20000000u) != 0u) ++zero_det_paths;
                ++runs[e];
                ++total;
            }
        }
        std::printf("PASS math matrix library: %u calls over 10 entries; r1, r2, r3, r13-r31, LR, CTR, CR, f14-f31, f1 (length), FPSCR and the whole 0x2000-byte arena "
                    "(including the framed routines' stack words) equal to the original; %u overlapped inputs, %u singular matrices, %u singular-determinant paths, "
                    "%u GQR0 quantised, %u non-single float arguments\n", total, aliased, singular, zero_det_paths, nonplain_gqr, nonsingle_args);
        for (unsigned e = 0; e < 10u; ++e)
            std::printf("  %-22s %5u calls, fast path accepted %5u (%.0f %%)\n", kEntries[e].name, runs[e], fast[e], 100.0 * fast[e] / runs[e]);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL math matrix library contract: %s\n", error.what());
        return 1;
    }
}
