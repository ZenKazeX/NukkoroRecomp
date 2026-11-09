// liveness-roots: reference-interpreter-only
// Original-DOL contract for the owned 3-vector helpers (gekko/native_vec_math.h): every instruction comes from the pinned
// DOL and runs in the reference interpreter; the native owner runs on a second machine with the same memory. Inputs: every
// float class (zeros, denormals, the 2^-126 boundary, infinities, quiet and signalling NaNs with payloads, huge and tiny
// normals, random bit patterns) in both FPSCR[NI] modes, with destination / source overlaps. Compared: the arena bytes, f1
// where the function returns it, and the FPSCR control byte. Negative controls: a wrong native (an unfused dot, the wrong NI
// mode) must be caught by the same comparison. Fixture addresses are outside the DOL's text.
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_vec_math.h"
#include "gekko/ppc_ops.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace gekko;
constexpr u32 kReturn = 0x817FF000u, kStack = 0x817E0000u, kR2 = 0x805FA780u, kR13 = 0x805EC500u;
constexpr u32 kArena = 0x81400000u, kArenaSize = 0x100u;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Machine {
    GuestMemory memory;
    Cpu cpu{memory};
    explicit Machine(const std::vector<u8>& dol, bool original) {
        if (!original) return;
        const auto word = [&](unsigned offset) { return load_be32(dol.data() + offset); };
        for (unsigned i = 0; i < 18; ++i) {
            const u32 offset = word(i * 4), address = word(0x48 + i * 4), size = word(0x90 + i * 4);
            require(u64(offset) + size <= dol.size(), "invalid DOL section");
            if (size) memory.write_block(address, dol.data() + offset, size);
        }
    }
    void prepare(u32 entry, bool ni) {
        cpu.reset();
        cpu.set_msr(cpu.msr() | kMsrFP);
        cpu.fpscr = ni ? kFpscrNI : 0u;
        cpu.pc = entry; cpu.npc = entry + 4; cpu.lr = kReturn;
        cpu.gpr[1] = kStack; cpu.gpr[2] = kR2; cpu.gpr[13] = kR13;
    }
    void run_original() {
        unsigned steps = 0;
        while (cpu.pc != kReturn && !cpu.stopped() && steps++ < 100000) cpu.run_steps(1);
        require(cpu.pc == kReturn && !cpu.stopped() && cpu.gpr[1] == kStack, "original call failed return/stack contract");
    }
};

u32 g_seed = 0xC0FFEE11u;
u32 rnd() { g_seed = g_seed * 1664525u + 1013904223u; return g_seed >> 8 ^ (g_seed << 16); }
u32 pick(u32 n) { return rnd() % n; }

u32 float_word() {
    switch (pick(14)) {
        case 0: return 0u;
        case 1: return 0x80000000u;
        case 2: return (pick(2) << 31) | (1u + pick(0x7FFFFFu));                       // denormal
        case 3: return (pick(2) << 31) | 0x00800000u | pick(4);                         // just above the boundary
        case 4: return (pick(2) << 31) | 0x7F800000u;                                   // infinity
        case 5: return (pick(2) << 31) | 0x7F800000u | (1u + pick(0x7FFFFFu));          // NaN, any payload
        case 6: return (pick(2) << 31) | 0x7F7FFFFFu - pick(4);                         // huge
        case 7: return (pick(2) << 31) | ((0x3F800000u + (pick(9) << 23)) & 0x7FFFFFFFu) | pick(2);  // around 1..256
        case 8: return (pick(2) << 31) | ((0x20u + pick(0x20u)) << 23) | pick(0x800000u);  // tiny normals
        case 9: return (pick(2) << 31) | ((0x60u + pick(0x20u)) << 23) | pick(0x800000u);  // sqrt-ish range
        default: return rnd();
    }
}
u64 scalar_bits() {  // Scale's f1 is a float argument: always a value a single holds
    return single_to_double_bits(float_word());
}

void fill(Machine& a, Machine& b, u32 at, u32 word) {
    a.memory.write32(at, word);
    b.memory.write32(at, word);
}

struct Case {
    const char* name;
    u32 entry;
    void (*serve)(Cpu&) noexcept;
    bool has_r5, returns_f1, takes_f1;
    bool in_place;  // r3 aliases r4 sometimes
};

int g_failures = 0, g_checked = 0;
void report(const char* what, const std::string& detail) {
    ++g_failures;
    if (g_failures < 24) std::fprintf(stderr, "FAIL %s: %s\n", what, detail.c_str());
}

// Runs `cases` random calls of one entry on both machines. `native` is what is compared with the original.
void run_case(Machine& o, Machine& n, const Case& c, unsigned calls, const std::function<void(Cpu&)>& native,
              bool expect_clean, unsigned* mismatches) {
    for (unsigned i = 0; i < calls; ++i) {
        const bool ni = (i & 1u) != 0u;
        // operand vectors: three 3-vectors in the arena at distinct or overlapping offsets
        const u32 slot[4] = {0u, 4u, 8u, 12u};
        u32 dst = kArena + 0x20u, a = kArena + 0x40u, b = kArena + 0x60u;
        switch (pick(5)) {
            case 0: dst = a; break;                                  // destination = first source
            case 1: dst = b; break;
            case 2: dst = a + slot[1 + pick(3)]; break;               // partial overlap
            case 3: a = b + slot[pick(4)]; break;                      // sources overlapping
            default: break;
        }
        for (u32 off = 0; off < kArenaSize; off += 4u) fill(o, n, kArena + off, off < 0x20u ? 0u : float_word());
        const u64 f1 = c.takes_f1 ? scalar_bits() : (u64(rnd()) << 32 | rnd());
        o.prepare(c.entry, ni);
        n.prepare(c.entry, ni);
        const bool is_dot = c.name[0] == 'D', is_length = c.name[0] == 'L';
        for (Machine* m : {&o, &n}) {
            if (is_dot) { m->cpu.gpr[3] = a; m->cpu.gpr[4] = b; }
            else if (is_length) { m->cpu.gpr[3] = a; }
            else { m->cpu.gpr[3] = dst; m->cpu.gpr[4] = a; if (c.has_r5) m->cpu.gpr[5] = b; }
            m->cpu.fpr[1].bits[0] = f1;
        }
        o.run_original();
        native(n.cpu);
        bool same = true;
        std::string detail;
        for (u32 off = 0; off < kArenaSize && same; off += 4u)
            if (o.memory.peek32(kArena + off) != n.memory.peek32(kArena + off)) {
                same = false;
                char t[160];
                std::snprintf(t, sizeof t, "%s ni=%d at +0x%02X: original %08X native %08X (dst %08X a %08X b %08X f1 %016llX)",
                              c.name, ni, off, o.memory.peek32(kArena + off), n.memory.peek32(kArena + off), dst, a, b,
                              static_cast<unsigned long long>(f1));
                detail = t;
            }
        if (same && c.returns_f1 && o.cpu.fpr[1].bits[0] != n.cpu.fpr[1].bits[0]) {
            same = false;
            char t[160];
            std::snprintf(t, sizeof t, "%s ni=%d f1: original %016llX native %016llX", c.name, ni,
                          static_cast<unsigned long long>(o.cpu.fpr[1].bits[0]),
                          static_cast<unsigned long long>(n.cpu.fpr[1].bits[0]));
            detail = t;
        }
        if (same && (o.cpu.fpscr & 0xFFu) != (n.cpu.fpscr & 0xFFu)) {
            same = false;
            detail = std::string(c.name) + ": FPSCR control byte differs";
        }
        ++g_checked;
        if (!same) {
            if (mismatches) ++*mismatches;
            if (expect_clean) report(c.name, detail);
        }
    }
}

std::vector<u8> read_file(const char* path) {
    std::ifstream input(path, std::ios::binary);
    return std::vector<u8>{std::istreambuf_iterator<char>(input), {}};
}
}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: vec_math_contract_tests main.dol");
        const std::vector<u8> dol = read_file(argv[1]);
        require(dol.size() >= 256, "missing DOL");
        u64 hash = 0xCBF29CE484222325ull;
        for (u8 byte : dol) hash = (hash ^ byte) * 0x100000001B3ull;
        require(hash == 0xD2CFB4A1CA897F9Bull, "unexpected original DOL identity");
        Machine original(dol, true), native(dol, false);
        native.memory.write32(0, 0);

        const Case cases[] = {
            {"Sub", vec_math::kSub, vec_math::serve_sub, true, false, false, false},
            {"Add", vec_math::kAdd, vec_math::serve_add, true, false, false, false},
            {"Scale", vec_math::kScale, vec_math::serve_scale, false, false, true, false},
            {"SubFrom", vec_math::kSubFrom, vec_math::serve_sub_from, false, false, false, true},
            {"AddTo", vec_math::kAddTo, vec_math::serve_add_to, false, false, false, true},
            {"Dot", vec_math::kDot, vec_math::serve_dot, false, true, false, false},
            {"Cross", vec_math::kCross, vec_math::serve_cross, true, false, false, false},
            {"Length", vec_math::kLength, vec_math::serve_length, false, true, false, false},
        };
        constexpr unsigned kCalls = 150000;
        for (const Case& c : cases) run_case(original, native, c, kCalls, c.serve, true, nullptr);

        // Negative controls: the same comparison must catch a wrong native.
        unsigned caught = 0;
        const Case& dot = cases[5];
        run_case(original, native, dot, 20000, [](Cpu& cpu) {  // unfused dot: x*x' + y*y' + z*z' rounded three times
            vec_math::serve_dot(cpu);
            double r[3];
            for (unsigned i = 0; i < 3; ++i) {
                float x, y;
                u32 xa = cpu.mem_read32(cpu.gpr[3] + 4u * i), ya = cpu.mem_read32(cpu.gpr[4] + 4u * i);
                std::memcpy(&x, &xa, 4); std::memcpy(&y, &ya, 4);
                r[i] = double(x) * double(y);
            }
            const float w = float(float(r[1]) + float(r[0])) + float(r[2]);
            u32 wb; std::memcpy(&wb, &w, 4);
            cpu.fpr[1].bits[0] = single_to_double_bits(wb);
        }, false, &caught);
        require(caught > 0, "negative control: an unfused dot product was not caught");
        unsigned caught_ni = 0;
        run_case(original, native, cases[0], 20000, [](Cpu& cpu) {  // the wrong NI mode
            cpu.fpscr ^= kFpscrNI;
            vec_math::serve_sub(cpu);
        }, false, &caught_ni);
        require(caught_ni > 0, "negative control: the wrong FPSCR[NI] mode was not caught");

        if (g_failures != 0) {
            std::fprintf(stderr, "FAIL vec math contract: %d mismatches of %d calls\n", g_failures, g_checked);
            return 1;
        }
        std::printf("PASS vec math contract: %d calls compared (controls caught %u + %u)\n", g_checked, caught, caught_ni);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL vec math contract: %s\n", error.what());
        return 1;
    }
}
