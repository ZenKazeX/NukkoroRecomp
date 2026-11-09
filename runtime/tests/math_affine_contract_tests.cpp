// liveness-roots: reference-interpreter-only
// Contract for the 3x4 matrix leaves 0x8040C6CC (point), 0x8040C5EC (translate by) and 0x8040C7E0 (scale columns), owned in
// gekko/native_math_affine.h. The ORIGINAL words run in the reference interpreter from the pinned DOL; the native owner runs
// on an identical machine. Fuzz: random float words mixed with the exceptional classes (+-0, subnormals, +-inf, NaNs with
// different payloads, +-1, large and tiny magnitudes), the output aliasing the matrix or the point (full and partial
// overlap), FPSCR with and without the non-IEEE (flush) bit and with sticky bits already set.
// Compared after each call: every general register, LR, CTR, CR, f8..f31 (the original's f0..f7 are its scratch and are the
// only floating registers not compared), FPSCR, and every byte of the 0x1000-byte arena (matrix, vectors, outputs).
// usage: math_affine_contract_tests <sys/main.dol>
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_math_affine.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace gekko;
using namespace gekko::math_affine;
constexpr u32 kR13 = 0x805EC500u, kArena = 0x80700000u, kReturn = 0x80100000u;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

u32 g_state = 12345u;
u32 rnd() {
    g_state = g_state * 1664525u + 1013904223u;
    return g_state ^ (g_state >> 15);
}
u32 float_word() {
    static const u32 special[] = {0x00000000u, 0x80000000u, 0x00000001u, 0x80000001u, 0x007FFFFFu, 0x807FFFFFu,
                                  0x00800000u, 0x80800000u, 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0xFFC00000u,
                                  0x7FC12345u, 0xFFD00001u, 0x7F800001u, 0x3F800000u, 0xBF800000u, 0x7F7FFFFFu,
                                  0xFF7FFFFFu, 0x34000000u, 0x4B800000u};
    switch (rnd() % 6u) {
    case 0: return special[rnd() % (sizeof(special) / sizeof(special[0]))];
    case 1: return rnd();
    case 2: return 0x3F000000u + (rnd() & 0x00FFFFFFu);        // 0.5 .. ~4
    case 3: return (rnd() & 0x80000000u) | (0x00000000u + (rnd() & 0x00FFFFFFu));  // subnormal / tiny
    case 4: return (rnd() & 0x80000000u) | 0x3F800000u | (rnd() & 0x007FFFFFu);    // +-[1, 2)
    default: return (rnd() & 0x80000000u) | (rnd() & 0x7FFFFFFFu);
    }
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
        std::vector<u8> block(0x1000);
        memory.read_block(kArena, block.data(), block.size());
        for (u8 b : block) h = (h ^ b) * 0x100000001B3ull;
        return h;
    }
};

struct Case {
    u32 entry, r3, r4, r5, fpscr;
    std::vector<u32> arena;  // 0x400 words
    u32 gpr_seed;
};
}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: math_affine_contract_tests main.dol");
        std::ifstream input(argv[1], std::ios::binary);
        const std::vector<u8> dol{std::istreambuf_iterator<char>(input), {}};
        require(dol.size() >= 256, "missing DOL");
        u64 hash = 0xCBF29CE484222325ull;
        for (u8 byte : dol) hash = (hash ^ byte) * 0x100000001B3ull;
        require(hash == 0xD2CFB4A1CA897F9Bull, "unexpected original DOL identity");
        Machine o(dol), n(dol);
        require(o.memory.read32(kPoint) == 0xC0A50004u && o.memory.read32(kTranslateBy) == 0xC0030004u &&
                    o.memory.read32(kScaleColumns) == 0xC0030000u,
                "unexpected routine words");
        unsigned runs = 0, aliased = 0, nonfinite = 0;
        const u32 m_at = kArena + 0x100u, v_at = kArena + 0x200u, out_at = kArena + 0x300u;
        for (u32 entry : {kPoint, kTranslateBy, kScaleColumns})
            for (u32 iteration = 0; iteration < 6000u; ++iteration) {
                // overlap choice: 0 none, 1 out == m, 2 out == v, 3 out inside m (offset 4), 4 v inside m, 5 out inside v
                const u32 mode = iteration % 6u;
                u32 r3, r4, r5;
                if (entry == kPoint) {
                    r3 = out_at; r4 = m_at; r5 = v_at;
                    if (mode == 1) r3 = m_at;
                    else if (mode == 2) r3 = v_at;
                    else if (mode == 3) r3 = m_at + 4u * (1u + rnd() % 8u);
                    else if (mode == 4) r5 = m_at + 4u * (rnd() % 10u);
                    else if (mode == 5) r3 = v_at + 4u * (1u + rnd() % 2u);
                } else {
                    r3 = m_at; r4 = v_at; r5 = 0;
                    if (mode == 2 || mode == 4) r4 = m_at + 4u * (rnd() % 10u);
                    else if (mode == 3 || mode == 5) r4 = m_at + 0x10u * (rnd() % 2u) + 4u * (rnd() % 3u);
                }
                if (mode != 0) ++aliased;
                std::vector<u32> arena(0x400u);
                for (u32& w : arena) w = rnd();
                for (u32 i = 0; i < 0x40u; ++i) arena[(0x100u + 4u * i) / 4u] = float_word();
                const u32 fpscr = (rnd() & 1u ? 0x4u : 0u) | (rnd() & 1u ? 0xF8000000u & rnd() : 0u);
                const u32 seed = rnd();
                for (Machine* m : {&o, &n}) {
                    Cpu& c = m->cpu;
                    c.reset();
                    for (u32 i = 0; i < 0x400u; ++i) m->memory.write32(kArena + 4u * i, arena[i]);
                    u32 x = seed;
                    for (u32 i = 0; i < 32u; ++i) {
                        x = x * 1664525u + 1013904223u;
                        c.gpr[i] = x;
                        c.fpr[i].bits[0] = (u64{x} << 32) | (x * 2654435761u);
                        c.fpr[i].bits[1] = (u64{x ^ 0xA5A5A5A5u} << 32) | x;
                    }
                    c.gpr[1] = 0x817E0000u;
                    c.gpr[2] = 0x805F0000u;
                    c.gpr[13] = kR13;
                    c.gpr[3] = r3; c.gpr[4] = r4; c.gpr[5] = r5;
                    c.lr = kReturn;
                    c.fpscr = fpscr;
                }
                Cpu& c = o.cpu;
                c.pc = entry;
                c.npc = entry + 4u;
                for (unsigned steps = 0; steps < 200 && c.pc != kReturn && !c.stopped(); ++steps) c.run_steps(1u);
                require(c.pc == kReturn, "the original did not return");
                Cpu& d = n.cpu;
                if (entry == kPoint) serve_point(d);
                else if (entry == kTranslateBy) serve_translate_by(d);
                else serve_scale_columns(d);
                const std::string at = " entry " + std::to_string(entry) + " iteration " + std::to_string(iteration) +
                                       " mode " + std::to_string(mode);
                for (u32 i = 0; i < 32u; ++i) require(c.gpr[i] == d.gpr[i], "r" + std::to_string(i) + " differs" + at);
                require(c.lr == d.lr && c.ctr == d.ctr && c.cr == d.cr, "LR / CTR / CR differ" + at);
                for (u32 i = 8; i < 32u; ++i)
                    require(c.fpr[i].bits[0] == d.fpr[i].bits[0] && c.fpr[i].bits[1] == d.fpr[i].bits[1],
                            "f" + std::to_string(i) + " differs" + at);
                require(c.fpscr == d.fpscr, "FPSCR differs: original " + std::to_string(c.fpscr) + " native " +
                                                std::to_string(d.fpscr) + at);
                require(o.hash() == n.hash(), "memory differs" + at);
                for (u32 i = 0; i < 12u; ++i) {
                    const u32 w = o.memory.read32(m_at + 4u * i);
                    nonfinite += ((w >> 23) & 0xFFu) == 0xFFu;
                }
                ++runs;
            }
        std::printf("PASS math affine: %u calls (0x8040C6CC, C5EC, C7E0; %u with the output or vector overlapping the matrix, "
                    "%u non-finite words seen in results): every GPR, LR/CTR/CR, f8-f31, FPSCR and the whole arena equal to "
                    "the original\n",
                    runs, aliased, nonfinite);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL math affine contract: %s\n", error.what());
        return 1;
    }
}
