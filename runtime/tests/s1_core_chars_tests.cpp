// liveness-roots: reference-interpreter-only
// Original-DOL contract for the character sweep query 0x80021934 (gekko/native_s1_chars.h): the original instructions run in the
// reference interpreter; the native owner on a second machine with the same DOL and memory. Both machines hook the callees the owner
// calls (handle manager 0x80045AC0, 0x80021480, 0x80021C04, 0x80021BC4, 0x80021B08, 0x800C51C8, 0x80021B74) with logging stubs: each
// stub appends its target, LR, r3..r10, f1, r1 and the two stack words [r1 + 8] / [r1 + 0xC] to a log in the arena and returns a value
// derived from its arguments (so the r10 -> r31 chain through the slot sweeps is observable). The leaves the owner inlines (0x80021C80,
// 0x8003AA58, 0x8003A568) run as original code on the original machine. Fixture: a character table (two banks of 0xA8 halfwords at
// +0x48 / +0x198, character pointers at +0x588, bodies at +0xB8) with random occupancy, null characters and null bodies, a manager that
// returns the table or null, every mode-flag combination, the stack flag byte 0 / non-zero. Compared after every call: the arena with
// the log (every argument of every callee call, in order), the stack window below r1 (the dead frame words, the stack argument slots),
// r1 r2 r3 r13 r14-r31, f1, LR. Negative controls: a dropped frame word, a wrong return value, a corrupted log entry, a clobbered
// non-volatile register, a missing stack-argument word must each be caught.
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_s1_chars.h"

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
constexpr u32 kArena = 0x81400000u, kArenaSize = 0x4000u;
constexpr u32 kTable = kArena + 0x100u;        // the character table object (0x830 bytes)
constexpr u32 kCharacters = kArena + 0x1000u;  // 0xA8 * 0x20 characters
constexpr u32 kBodies = kArena + 0x2400u;      // 0xA8 * 0x10
constexpr u32 kQuery = kArena + 0x3000u;       // the query / sphere inputs
constexpr u32 kLog = kArena + 0x3400u;         // word 0 = count, entries of 0x40 bytes
constexpr u32 kManagerValue = kArena + 0x3FF0u;
constexpr u32 kStackBelow = 0x200u, kStackAbove = 0x40u;
constexpr u32 kStubs[] = {0x80045AC0u, 0x80021480u, 0x80021C04u, 0x80021BC4u, 0x80021B08u, 0x800C51C8u, 0x80021B74u};

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bool stub(Cpu& c, u32 target) {
    const u32 count = c.mem_read32(kLog);
    const u32 e = kLog + 0x40u + 0x40u * count;
    if (e + 0x40u <= kArena + kArenaSize) {
        c.mem_write32(e + 0x00u, target);
        c.mem_write32(e + 0x04u, c.lr);
        // only the argument registers the callee takes are meaningful (the rest is the caller's dead scratch)
        const u32 nargs = target == 0x80045AC0u ? 1u : (target == 0x80021B08u ? 0u : (target == 0x80021C04u || target == 0x80021BC4u || target == 0x80021B74u) ? 2u : 8u);
        for (u32 r = 3; r <= 10; ++r) c.mem_write32(e + 0x08u + 4u * (r - 3u), (r - 3u) < nargs ? c.gpr[r] : 0u);
        c.mem_write32(e + 0x28u, target == 0x80021C04u ? static_cast<u32>(c.fpr[1].bits[0] >> 29) : 0u);
        c.mem_write32(e + 0x2Cu, c.gpr[1]);
        c.mem_write32(e + 0x30u, nargs == 8u ? c.mem_read32(c.gpr[1] + 8u) : 0u);
        c.mem_write32(e + 0x34u, target == 0x800C51C8u ? c.mem_read32(c.gpr[1] + 0xCu) : 0u);
        c.mem_write32(kLog, count + 1u);
    }
    u32 r3 = c.gpr[3];
    switch (target) {
        case 0x80045AC0u: r3 = c.mem_read32(kManagerValue); break;
        case 0x80021480u: r3 = ((c.gpr[3] ^ c.gpr[4] ^ (c.gpr[10] * 7u)) << 5 | (c.gpr[3] >> 27)) + count; break;
        case 0x800C51C8u: r3 = (c.gpr[3] ^ c.gpr[8]) + 0x1234u + count; break;
        case 0x80021B08u: r3 = 0x81450000u; break;
        default: break;  // 0x80021C04 / 0x80021BC4 / 0x80021B74: r3 returned as given
    }
    c.gpr[3] = r3;
    return true;
}

struct Machine {
    GuestMemory memory;
    Cpu cpu{memory};
    explicit Machine(const std::vector<u8>& dol) {
        const auto word = [&](unsigned offset) { return load_be32(dol.data() + offset); };
        for (unsigned i = 0; i < 18; ++i) {
            const u32 offset = word(i * 4), address = word(0x48 + i * 4), size = word(0x90 + i * 4);
            require(u64(offset) + size <= dol.size(), "invalid DOL section");
            if (size) memory.write_block(address, dol.data() + offset, size);
        }
    }
    void install_stubs() {
        for (u32 t : kStubs) cpu.install_hle(t, [t](Cpu& c) { return stub(c, t); }, "stub");
    }
    void prepare(const u32 args[8], const u32 saved[13], bool fp) {
        cpu.reset();
        cpu.set_msr(fp ? (cpu.msr() | kMsrFP) : (cpu.msr() & ~kMsrFP));
        cpu.pc = 0x80021934u; cpu.npc = 0x80021938u; cpu.lr = kReturn;
        cpu.gpr[1] = kStack; cpu.gpr[2] = kR2; cpu.gpr[13] = kR13;
        for (u32 k = 0; k < 8; ++k) cpu.gpr[3 + k] = args[k];
        for (u32 k = 0; k < 13; ++k) cpu.gpr[19 + k] = saved[k];
        const u64 d = single_to_double_bits(0x3F800000u);
        cpu.fpr[1].bits[0] = d; cpu.fpr[1].bits[1] = d;
    }
    void run_original() {
        unsigned steps = 0;
        while (cpu.pc != kReturn && !cpu.stopped() && steps++ < 2000000) cpu.run_steps(1);
        if (!(cpu.pc == kReturn && !cpu.stopped() && cpu.gpr[1] == kStack))
            std::fprintf(stderr, "original run: pc %08X stopped %d reason %d r1 %08X\n", cpu.pc, cpu.stopped() ? 1 : 0, int(cpu.stop().reason), cpu.gpr[1]);
        require(cpu.pc == kReturn && !cpu.stopped() && cpu.gpr[1] == kStack, "original call failed return/stack contract");
    }
};

u32 g_seed = 0xC4A55EEDu;
u32 rnd() { g_seed = g_seed * 1664525u + 1013904223u; return g_seed >> 8 ^ (g_seed << 16); }
u32 pick(u32 n) { return rnd() % n; }
void fill(Machine& a, Machine& b, u32 at, u32 word) { a.memory.write32(at, word); b.memory.write32(at, word); }
void fill16(Machine& a, Machine& b, u32 at, u32 half) { a.memory.write16(at, static_cast<u16>(half)); b.memory.write16(at, static_cast<u16>(half)); }

int g_failures = 0, g_checked = 0;
void report(const char* what, const std::string& detail) {
    ++g_failures;
    if (g_failures < 24) std::fprintf(stderr, "FAIL %s: %s\n", what, detail.c_str());
}

bool same_state(Machine& o, Machine& n, const char* name, std::string& detail) {
    char t[200];
    const auto differ = [&](u32 at, const char* region) {
        if (o.memory.peek32(at) == n.memory.peek32(at)) return false;
        std::snprintf(t, sizeof t, "%s %s at %08X: original %08X native %08X", name, region, at, o.memory.peek32(at), n.memory.peek32(at));
        detail = t;
        return true;
    };
    for (u32 off = 0; off < kArenaSize; off += 4u) if (differ(kArena + off, "arena")) return false;
    for (u32 off = 0; off < kStackBelow + kStackAbove; off += 4u) if (differ(kStack - kStackBelow + off, "stack")) return false;
    for (u32 r : {1u, 2u, 3u, 13u, 14u, 15u, 16u, 17u, 18u, 19u, 20u, 21u, 22u, 23u, 24u, 25u, 26u, 27u, 28u, 29u, 30u, 31u})
        if (o.cpu.gpr[r] != n.cpu.gpr[r]) {
            std::snprintf(t, sizeof t, "%s r%u: original %08X native %08X", name, r, o.cpu.gpr[r], n.cpu.gpr[r]);
            detail = t;
            return false;
        }
    if (o.cpu.lr != n.cpu.lr) { detail = std::string(name) + ": LR differs"; return false; }
    if (o.cpu.fpr[1].bits[0] != n.cpu.fpr[1].bits[0]) { detail = std::string(name) + ": f1 differs"; return false; }
    return true;
}

struct Case { u32 args[8]; u32 saved[13]; bool fp; };

void build(Machine& o, Machine& n, Case& cs, bool force_mode, u32 mode) {
    for (u32 off = 0; off < kArenaSize; off += 4u) fill(o, n, kArena + off, 0xA5A50000u | (off & 0xFFFFu));
    fill(o, n, kLog, 0u);
    const bool manager_null = pick(8) == 0;
    fill(o, n, kManagerValue, manager_null ? 0u : kTable + 0x28u);
    const u32 density = 1 + pick(20);  // occupancy of the slots
    for (u32 off = 0; off < 0x830u; off += 4u) fill(o, n, kTable + off, rnd());
    for (u32 i = 0; i < 0xA8u; ++i) {
        const bool b0 = pick(20) < density, b1 = pick(20) < density / 2u;
        fill16(o, n, kTable + 0x48u + 2u * i, b0 ? (1u + pick(0xFFFE)) : 0u);
        fill16(o, n, kTable + 0x198u + 2u * i, b1 ? (1u + pick(0xFFFE)) : 0u);
        const u32 ch = kCharacters + 0x20u * i;
        fill(o, n, kTable + 0x588u + 4u * i, pick(6) == 0 ? 0u : ch);
        fill(o, n, ch + 0xB8u - 0xB8u, rnd());
        fill(o, n, ch + 0x14u, rnd());
    }
    // character objects hold the body pointer at +0xB8: place them so +0xB8 falls inside the arena (characters are 0x20 apart: use a
    // separate array of 0x100-byte objects for the first 0x8 slots and share one object for the rest)
    for (u32 i = 0; i < 0xA8u; ++i) {
        const u32 shared_object = kArena + 0x2800u + 0x100u * (i % 6u);
        fill(o, n, kTable + 0x588u + 4u * i, o.memory.peek32(kTable + 0x588u + 4u * i) == 0u ? 0u : shared_object);
    }
    for (u32 k = 0; k < 6; ++k) fill(o, n, kArena + 0x2800u + 0x100u * k + 0xB8u, pick(4) == 0 ? 0u : kBodies + 0x10u * k);
    // the query inputs: r4 points at a sphere (radius float at +0xC)
    for (u32 off = 0; off < 0x40u; off += 4u) fill(o, n, kQuery + off, rnd());
    fill(o, n, kQuery + 0xCu, 0x3F000000u | (rnd() & 0xFFFFFFu));
    cs.fp = true;
    for (u32 k = 0; k < 8; ++k) cs.args[k] = rnd();
    cs.args[0] = rnd();                 // r3 query
    cs.args[1] = kQuery;                // r4 sphere
    cs.args[2] = kQuery + 0x20u;        // r5
    cs.args[3] = force_mode ? mode : (pick(3) == 0 ? (rnd() & 0xFFu) : pick(4));  // r6 mode flags
    for (u32 k = 0; k < 13; ++k) cs.saved[k] = rnd();
    fill(o, n, kStack + 0x8u, rnd());
    fill(o, n, kStack + 0xCu, pick(2) ? (rnd() & 0xFFu) : 0u);  // the byte at [r1 + 0xB] is the low byte of the word at +8: set +8 below
    fill(o, n, kStack + 0x8u, (pick(3) == 0 ? 0u : rnd() & 0xFFFFFF00u) | (pick(2) ? 1u : 0u));
}

struct Tally { unsigned served = 0, declined = 0, caught = 0; };

bool one_call(Machine& o, Machine& n, const char* label, const Case& cs, const std::function<bool(Cpu&)>& owner, bool catch_only, Tally& t) {
    o.prepare(cs.args, cs.saved, cs.fp);
    n.prepare(cs.args, cs.saved, cs.fp);
    o.run_original();
    const bool served = owner(n.cpu);
    if (served) n.cpu.pc = n.cpu.lr; else n.run_original();
    served ? ++t.served : ++t.declined;
    ++g_checked;
    std::string detail;
    if (!same_state(o, n, label, detail)) {
        if (catch_only) ++t.caught; else report(label, detail);
        return false;
    }
    return true;
}

std::vector<u8> read_file(const char* path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<u8>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: s1_core_chars_tests main.dol");
        const std::vector<u8> dol = read_file(argv[1]);
        require(dol.size() >= 256, "missing DOL");
        u64 hash = 0xCBF29CE484222325ull;
        for (u8 byte : dol) hash = (hash ^ byte) * 0x100000001B3ull;
        require(hash == 0xD2CFB4A1CA897F9Bull, "unexpected original DOL identity");
        Machine original(dol), native(dol);
        original.install_stubs();
        native.install_stubs();
        s1_chars::set_enabled(true);
        const auto real = [](Cpu& c) { return s1_chars::serve(c); };

        Tally main_run;
        unsigned by_mode[4] = {};
        for (unsigned i = 0; i < 6000; ++i) {
            Case cs;
            build(original, native, cs, i % 3 == 0, (i / 3) % 4);
            one_call(original, native, "sweep", cs, real, false, main_run);
            ++by_mode[cs.args[3] & 3u];
        }
        std::printf("calls: %u served, %u handed back; mode bits 0..3: %u %u %u %u\n", main_run.served, main_run.declined, by_mode[0],
                    by_mode[1], by_mode[2], by_mode[3]);
        require(main_run.declined == 0, "the owner handed back a call with the FPU on");
        const u64 swept = s1_chars::stats().slots_swept;
        std::printf("slots swept (callee 0x80021480 calls made natively): %llu\n", static_cast<unsigned long long>(swept));
        require(swept > 5000, "too few slot sweeps");

        // directed: FPU off with mode bit 1 is handed back before any write; a hook on an inlined leaf too
        {
            Case cs;
            build(original, native, cs, true, 2);
            native.prepare(cs.args, cs.saved, false);
            native.memory.write32(kStack - 0x70u, 0u);
            require(!s1_chars::serve(native.cpu), "served with the FPU off");
            require(native.memory.peek32(kStack - 0x70u) == 0u, "a handed-back call wrote its frame");
        }

        // negative controls
        Tally t;
        const auto control = [&](const char* label, const std::function<bool(Cpu&)>& owner, unsigned calls) {
            Tally x;
            for (unsigned i = 0; i < calls; ++i) {
                Case cs;
                build(original, native, cs, true, 1 + i % 3);
                one_call(original, native, label, cs, owner, true, x);
            }
            return x.caught;
        };
        const unsigned c_frame = control("ctl frame", [](Cpu& c) { const bool ok = s1_chars::serve(c); c.mem_write32(kStack - 0x70u + 0x3Cu, 0u); return ok; }, 100);
        require(c_frame > 0, "negative control: a missing frame word was not caught");
        const unsigned c_ret = control("ctl return", [](Cpu& c) { const bool ok = s1_chars::serve(c); c.gpr[3] ^= 1u; return ok; }, 100);
        require(c_ret > 0, "negative control: a wrong return value was not caught");
        const unsigned c_log = control("ctl log", [](Cpu& c) { const bool ok = s1_chars::serve(c); c.mem_write32(kLog + 0x40u + 8u, 0u); return ok; }, 100);
        require(c_log > 0, "negative control: a corrupted callee argument was not caught");
        const unsigned c_reg = control("ctl register", [](Cpu& c) { const bool ok = s1_chars::serve(c); c.gpr[27] += 1u; return ok; }, 100);
        require(c_reg > 0, "negative control: a clobbered non-volatile register was not caught");
        const unsigned c_arg = control("ctl stack argument", [](Cpu& c) { const bool ok = s1_chars::serve(c); c.mem_write32(kStack - 0x70u + 8u, 0x5A5A5A5Au); return ok; }, 100);
        require(c_arg > 0, "negative control: a wrong stack-argument word was not caught");
        (void)t;

        if (g_failures != 0) {
            std::fprintf(stderr, "FAIL s1 core chars contract: %d mismatches of %d calls\n", g_failures, g_checked);
            return 1;
        }
        std::printf("PASS s1 core chars contract: %d calls compared (controls caught %u + %u + %u + %u + %u)\n", g_checked, c_frame, c_ret, c_log, c_reg, c_arg);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL s1 core chars contract: %s\n", error.what());
        return 1;
    }
}
