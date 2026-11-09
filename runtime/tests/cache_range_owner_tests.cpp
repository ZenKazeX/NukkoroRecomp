// DCInvalidateRange (0x80372508) and DCFlushRangeNoSync (0x80372594) are owned natively (game_memory.cpp,
// serve_cache_range_loop): the per-line cache operation has nothing to act on here, and the owner leaves the registers
// the loop leaves. This runs the ORIGINAL words in the reference interpreter for many (address, length) pairs and
// compares every register the routine can touch - r3, r4, r5, CTR, CR, XER, LR, the rest of the GPRs - with the
// owner's result from the same starting state. Memory is not compared: neither the original (dcbi / dcbf, no stores)
// nor the owner writes any.
// usage: cache_range_owner_tests <sys/main.dol>

#include "gekko/cpu.h"
#include "gekko/game_memory.h"
#include "gekko/guest_memory.h"

#include <array>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

using namespace gekko;

namespace {

constexpr u32 kDcInvalidateRange = 0x80372508u, kDcFlushRangeNoSync = 0x80372594u;
constexpr u32 kSentinel = 0x80001200u;

int g_failures = 0;
unsigned long long g_checks = 0;
void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok && ++g_failures <= 40) std::printf("FAIL: %s\n", what.c_str());
}
std::string hex(u32 v) {
    char b[16];
    std::snprintf(b, sizeof(b), "%08X", v);
    return b;
}

struct Machine {
    GuestMemory bus;
    Cpu cpu{bus};
    explicit Machine(const std::vector<u8>& dol) {
        const auto word = [&](std::size_t o) {
            return u32{dol[o]} << 24 | u32{dol[o + 1]} << 16 | u32{dol[o + 2]} << 8 | u32{dol[o + 3]};
        };
        for (u32 i = 0; i < 18u; ++i) {
            const u32 offset = word(i * 4u), address = word(0x48u + i * 4u), size = word(0x90u + i * 4u);
            if (size == 0u) continue;
            if (std::uint64_t(offset) + size > dol.size()) throw std::runtime_error("invalid DOL extent");
            bus.write_block(address, dol.data() + offset, size);
        }
        // The words the owner stands for (cmplwi r4,0 opens both; the loops differ only in dcbi / dcbf).
        if (bus.read32(kDcInvalidateRange) != 0x28040000u || bus.read32(kDcFlushRangeNoSync) != 0x28040000u)
            throw std::runtime_error("unexpected DCInvalidateRange / DCFlushRangeNoSync words");
        cpu.reset();
        cpu.set_msr(cpu.msr() | kMsrDR | kMsrIR | kMsrFP);
    }
};

struct Regs {
    std::array<u32, 32> gpr;
    u32 ctr, cr, xer, lr;
};
Regs regs_of(const Cpu& c) {
    Regs r{};
    for (u32 i = 0; i < 32u; ++i) r.gpr[i] = c.gpr[i];
    r.ctr = c.ctr;
    r.cr = c.cr;
    r.xer = c.xer;
    r.lr = c.lr;
    return r;
}
void seed(Cpu& c, std::mt19937& rng, u32 r3, u32 r4, u32 ctr, u32 cr, u32 xer) {
    for (u32 i = 0; i < 32u; ++i) c.gpr[i] = rng();
    c.gpr[3] = r3;
    c.gpr[4] = r4;
    c.ctr = ctr;
    c.cr = cr;
    c.xer = xer;
    c.lr = kSentinel;
}

void one(Machine& m, std::mt19937& rng, u32 entry, u32 r3, u32 r4, bool resume = false, u32 resume_ctr = 0u) {
    const u32 random_ctr = rng();
    const u32 ctr = resume ? resume_ctr : random_ctr;
    const u32 cr = rng(), xer = rng() & 0xE000007Fu;  // XER: SO / OV / CA and the byte count
    const std::mt19937 fork = rng;
    // The original words, interpreted.
    std::mt19937 a = fork;
    seed(m.cpu, a, r3, r4, ctr, cr, xer);
    m.cpu.pc = entry;
    unsigned long long steps = 0;
    while (m.cpu.pc != kSentinel && !m.cpu.stopped() && steps++ < 200'000'000ull) m.cpu.run_steps(1u);
    if (m.cpu.pc != kSentinel) throw std::runtime_error("call to " + hex(entry) + " did not return");
    const Regs original = regs_of(m.cpu);
    // The owner, from the same state.
    std::mt19937 b = fork;
    seed(m.cpu, b, r3, r4, ctr, cr, xer);
    if (resume)
        serve_cache_range_loop_resume(m.cpu);
    else
        serve_cache_range_loop(m.cpu);
    const Regs owned = regs_of(m.cpu);
    const std::string at = hex(entry) + "(" + hex(r3) + ", " + hex(r4) + (resume ? ", ctr " + hex(ctr) : "") + ")";
    for (u32 i = 0; i < 32u; ++i)
        check(original.gpr[i] == owned.gpr[i],
              at + " r" + std::to_string(i) + " " + hex(original.gpr[i]) + " vs " + hex(owned.gpr[i]));
    check(original.ctr == owned.ctr, at + " ctr " + hex(original.ctr) + " vs " + hex(owned.ctr));
    check(original.cr == owned.cr, at + " cr " + hex(original.cr) + " vs " + hex(owned.cr));
    check(original.xer == owned.xer, at + " xer " + hex(original.xer) + " vs " + hex(owned.xer));
    check(original.lr == owned.lr, at + " lr " + hex(original.lr) + " vs " + hex(owned.lr));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: cache_range_owner_tests <sys/main.dol>\n");
        return 2;
    }
    std::ifstream file(argv[1], std::ios::binary);
    const std::vector<u8> dol((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (dol.size() < 0x100u) {
        std::printf("cannot read %s\n", argv[1]);
        return 2;
    }
    try {
        Machine m(dol);
        std::mt19937 rng(20261104u);
        for (const u32 entry : {kDcInvalidateRange, kDcFlushRangeNoSync}) {
            // Edges: no length, one byte at every alignment, exact line multiples, a line boundary crossed by one.
            for (u32 offset = 0; offset < 32u; ++offset) {
                for (const u32 n : {0u, 1u, 2u, 31u, 32u, 33u, 63u, 64u, 65u, 0x100u, 0x1000u, 0x1001u})
                    one(m, rng, entry, 0x80400000u + offset, n);
            }
            // Random ranges up to 1 MiB anywhere in the address space (no memory is touched).
            for (int i = 0; i < 400; ++i) one(m, rng, entry, rng(), rng() % 0x100000u);
            // Lengths whose line count is large, and starts near the top of the address space (r3 wraps).
            for (const u32 n : {0x00800000u, 0x01000001u}) one(m, rng, entry, 0xFFFFFFF7u, n);
            for (const u32 start : {0xFFFFFFE0u, 0xFFFFFFFFu, 0x00000001u}) one(m, rng, entry, start, 0x2000u);
            // Entered at the loop head (a resumption mid-loop): CTR lines left.
            const u32 head = entry == kDcInvalidateRange ? 0x80372524u : 0x803725B0u;
            for (const u32 left : {1u, 2u, 3u, 31u, 32u, 1000u, 0x8000u})
                for (int i = 0; i < 4; ++i) one(m, rng, head, rng(), rng(), true, left);
            for (int i = 0; i < 200; ++i) one(m, rng, head, rng(), rng(), true, 1u + rng() % 0x10000u);
            // A length near 2^32 is not run here: up to 2^32 interpreted iterations. The owner's formula covers it
            // (serve_cache_range_loop: the wrapped count of 0 advances r3 by 2^37 = 0 mod 2^32).
        }
    } catch (const std::exception& e) {
        std::printf("FAIL: %s\n", e.what());
        return 1;
    }
    std::printf("cache_range_owner_tests: %llu checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
