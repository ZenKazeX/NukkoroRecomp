// liveness-roots: reference-interpreter-only
// Original-DOL contract for the target registry query layer (gekko/native_s1_target2.h): the original instructions run in the reference
// interpreter; the native owner runs on a second machine with the same DOL and the same memory, and when it declines the original body
// runs there too. The guest allocator (operator new 0x803A1380 / delete 0x803A1334) is a bump allocator hooked identically on both
// machines (the real one needs the game's heap): the log of its calls (kind, size or pointer, LR) and its bump pointer are compared, which
// is the allocator-order proof of the contract; the real allocator's own frames are exercised in the game run (state.json, behaviour gate).
// Compared after every call: the registry, its nodes, the targets, the count blocks, the heap region the call allocates in, the whole
// stack window the call tree writes (random stale words included), r1, r2, r3, r13, r14..r31, LR, the bump pointer and the allocator log.
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_s1_target2.h"
#include "gekko/ppc_ops.h"

#include <cstdio>
#include <cstdlib>
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
constexpr u32 kQ1 = 0x81470000u, kQ2 = 0x81471000u, kQData = 0x81472000u, kVecA = 0x81474000u, kVecB = 0x81474040u, kFakeVtable = 0x81475000u;
constexpr u32 kReg = 0x81400000u, kNodes = 0x81401000u, kTargets = 0x81410000u, kCounts = 0x81420000u, kHeap = 0x81500000u;
constexpr u32 kTargetCount = 40u, kCountBlocks = 40u, kMaxNodes = 300u;
constexpr u32 kStackWindow = 0x800u, kHeapCompare = 0x3000u;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Machine {
    GuestMemory memory;
    Cpu cpu{memory};
    u32 bump = kHeap;
    std::vector<std::string> log;
    explicit Machine(const std::vector<u8>& dol) {
        const auto word = [&](unsigned offset) { return load_be32(dol.data() + offset); };
        for (unsigned i = 0; i < 18; ++i) {
            const u32 offset = word(i * 4), address = word(0x48 + i * 4), size = word(0x90 + i * 4);
            require(u64(offset) + size <= dol.size(), "invalid DOL section");
            if (size) memory.write_block(address, dol.data() + offset, size);
        }
        cpu.install_hle(0x803A1380u, [this](Cpu& c) {
            char t[64]; std::snprintf(t, sizeof t, "N %u %08X", c.gpr[3], c.lr); log.emplace_back(t);
            const u32 size = c.gpr[3]; c.gpr[3] = bump; bump += (size + 7u) & ~7u; return true; }, "test new");
        cpu.install_hle(0x803A1334u, [this](Cpu& c) {
            char t[64]; std::snprintf(t, sizeof t, "D %08X %08X", c.gpr[3], c.lr); log.emplace_back(t); return true; }, "test delete");
    }
    void prepare(u32 entry, u32 reg, const u32 saved[18]) {
        cpu.reset();
        cpu.set_msr(cpu.msr() | kMsrFP);
        cpu.pc = entry; cpu.npc = entry + 4; cpu.lr = kReturn; entry_pc = entry;
        cpu.gpr[1] = kStack; cpu.gpr[2] = kR2; cpu.gpr[13] = kR13;
        for (u32 k = 0; k < 18; ++k) cpu.gpr[14 + k] = saved[k];
        cpu.gpr[3] = reg;
    }
    u32 entry_pc = 0;
    void run_original() {
        unsigned steps = 0;
        while (cpu.pc != kReturn && !cpu.stopped() && steps++ < 8000000) cpu.run_steps(1);
        if (!(cpu.pc == kReturn && !cpu.stopped() && cpu.gpr[1] == kStack)) {
            char t[160]; std::snprintf(t, sizeof t, "original call failed return/stack contract: entry %08X pc %08X stopped %d r1 %08X steps %u lr %08X r3 %08X", entry_pc, cpu.pc, cpu.stopped() ? 1 : 0, cpu.gpr[1], steps, cpu.lr, cpu.gpr[3]);
            throw std::runtime_error(t);
        }
    }
};

u32 g_seed = 0x5EED7A26u;
u32 rnd() { g_seed = g_seed * 1664525u + 1013904223u; return g_seed >> 8 ^ (g_seed << 16); }
u32 pick(u32 n) { return rnd() % n; }

void fill(Machine& a, Machine& b, u32 at, u32 word) { a.memory.write32(at, word); b.memory.write32(at, word); }

int g_failures = 0, g_checked = 0;
void report(const char* what, const std::string& detail) {
    ++g_failures;
    if (g_failures < 24) std::fprintf(stderr, "FAIL %s: %s\n", what, detail.c_str());
}

bool same_state(Machine& o, Machine& n, const char* name, std::string& detail) {
    const auto differ = [&](u32 at, const char* region) {
        if (o.memory.peek32(at) == n.memory.peek32(at)) return false;
        char t[200];
        std::snprintf(t, sizeof t, "%s %s at %08X: original %08X native %08X", name, region, at, o.memory.peek32(at), n.memory.peek32(at));
        detail = t;
        return true;
    };
    for (u32 off = 0; off < 0x40u; off += 4u) if (differ(kReg + off, "registry")) return false;
    for (u32 off = 0; off < kMaxNodes * 0x10u; off += 4u) if (differ(kNodes + off, "nodes")) return false;
    for (u32 off = 0; off < kTargetCount * 0x40u; off += 4u) if (differ(kTargets + off, "targets")) return false;
    for (u32 off = 0; off < kCountBlocks * 0x10u; off += 4u) if (differ(kCounts + off, "counts")) return false;
    for (u32 off = 0; off < kHeapCompare; off += 4u) if (differ(kHeap + off, "heap")) return false;
    for (u32 off = 0; off < 0x40u; off += 4u) if (differ(kQ1 + off, "query 1") || differ(kQ2 + off, "query 2")) return false;
    for (u32 off = 0; off < 0x1100u; off += 4u) if (differ(kQData + off, "query data")) return false;
    for (u32 off = 0; off < 0x80u; off += 4u) if (differ(kVecA + off, "vectors")) return false;
    for (u32 off = 0; off < kStackWindow + 0x40u; off += 4u) if (differ(kStack - kStackWindow + off, "stack")) return false;
    for (u32 f = 14u; f <= 31u; ++f)
        if (o.cpu.fpr[f].bits[0] != n.cpu.fpr[f].bits[0] || o.cpu.fpr[f].bits[1] != n.cpu.fpr[f].bits[1]) {
            char t[160];
            std::snprintf(t, sizeof t, "%s f%u: original %016llX/%016llX native %016llX/%016llX", name, f, (unsigned long long)o.cpu.fpr[f].bits[0], (unsigned long long)o.cpu.fpr[f].bits[1], (unsigned long long)n.cpu.fpr[f].bits[0], (unsigned long long)n.cpu.fpr[f].bits[1]);
            detail = t;
            return false;
        }
    for (u32 r : {1u, 2u, 3u, 13u, 14u, 15u, 16u, 17u, 18u, 19u, 20u, 21u, 22u, 23u, 24u, 25u, 26u, 27u, 28u, 29u, 30u, 31u})
        if (o.cpu.gpr[r] != n.cpu.gpr[r]) {
            char t[120];
            std::snprintf(t, sizeof t, "%s r%u: original %08X native %08X", name, r, o.cpu.gpr[r], n.cpu.gpr[r]);
            detail = t;
            return false;
        }
    if (o.cpu.lr != n.cpu.lr) { detail = std::string(name) + ": LR differs"; return false; }
    if (o.bump != n.bump) { detail = std::string(name) + ": bump pointer differs"; return false; }
    if (o.log != n.log) { detail = std::string(name) + ": allocator log differs"; return false; }
    return true;
}

// After a caught mismatch of a negative control the two machines hold different words in the compared regions: copy the original's over the native's so
// the next call starts from equal states (otherwise stale differences are counted as catches).
void resync(Machine& o, Machine& n) {
    const auto copy = [&](u32 at, u32 bytes) { for (u32 off = 0; off < bytes; off += 4u) n.memory.write32(at + off, o.memory.peek32(at + off)); };
    copy(kReg, 0x40u); copy(kNodes, kMaxNodes * 0x10u); copy(kTargets, kTargetCount * 0x40u); copy(kCounts, kCountBlocks * 0x10u); copy(kHeap, kHeapCompare);
    copy(kQ1, 0x40u); copy(kQ2, 0x40u); copy(kQData, 0x1100u); copy(kVecA, 0x80u); copy(kStack - kStackWindow, kStackWindow + 0x40u);
}

enum class Scenario { Live, Eventful, CopyCase, DtorCase, FlagCase, SameCase, FpCase, FpForeign, FpAllForeign };

// One random registry: N nodes in a ring, holders {px, pn} naming targets and count blocks.
void build(Machine& o, Machine& n, Scenario scenario) {
    for (u32 off = 0; off < 0x40u; off += 4u) fill(o, n, kReg + off, 0xA5A50000u | off);
    for (u32 off = 0; off < kMaxNodes * 0x10u; off += 4u) fill(o, n, kNodes + off, 0xB6B60000u | (off & 0xFFFFu));
    for (u32 off = 0; off < kHeapCompare; off += 4u) fill(o, n, kHeap + off, 0xD7D70000u | (off & 0xFFFFu));
    for (u32 off = 0; off < kStackWindow + 0x40u; off += 4u) fill(o, n, kStack - kStackWindow + off, rnd());
    for (u32 t = 0; t < kTargetCount; ++t) {
        for (u32 off = 0; off < 0x40u; off += 4u) fill(o, n, kTargets + t * 0x40u + off, rnd());
        const u32 shape = pick(3);
        fill(o, n, kTargets + t * 0x40u + 0x30u, shape == 0 ? (1u << pick(32)) : shape == 1 ? (rnd() | 1u) : (rnd() & ~1u));
    }
    for (u32 b = 0; b < kCountBlocks; ++b) {
        fill(o, n, kCounts + b * 0x10u + 0u, 0x80500000u + 4u * b);
        fill(o, n, kCounts + b * 0x10u + 4u, 1u + pick(5));
        fill(o, n, kCounts + b * 0x10u + 8u, 1u + pick(3));
        fill(o, n, kCounts + b * 0x10u + 0xCu, rnd());
    }
    u32 count = pick(10) == 0 ? pick(200) : pick(24);
    if (pick(12) == 0) count = 0;
    const u32 sentinel = kReg + 0xCu;
    fill(o, n, kReg + 8u, count);
    u32 prev = sentinel;
    for (u32 i = 0; i < count; ++i) {
        const u32 node = kNodes + 0x10u * i;
        u32 px = kTargets + 0x40u * pick(kTargetCount), pn = kCounts + 0x10u * pick(kCountBlocks);
        if (scenario == Scenario::Eventful && pick(count / 3 + 2) == 0) {
            switch (pick(3)) {
                case 0: fill(o, n, pn + 4u, 0u); break;                       // expired
                case 1: pn = 0u; break;                                       // null pn
                default: fill(o, n, pn + 4u, 0xFFFFFF80u + pick(0x70)); break; // huge count
            }
        }
        fill(o, n, node + 0u, prev);
        fill(o, n, node + 8u, px);
        fill(o, n, node + 0xCu, pn);
        if (prev == sentinel) fill(o, n, sentinel + 4u, node); else fill(o, n, prev + 4u, node);
        prev = node;
    }
    if (count == 0) { fill(o, n, sentinel + 0u, sentinel); fill(o, n, sentinel + 4u, sentinel); }
    else { fill(o, n, prev + 4u, sentinel); fill(o, n, sentinel + 0u, prev); }
}

// A source query (vtable, capacity, size, data, logical end) with random entries, and a destination object with stale words.
void build_query(Machine& o, Machine& n, u32 q, u32 count, u32 data_at) {
    fill(o, n, q + 0u, 0x8053ED04u);
    fill(o, n, q + 4u, count + pick(4));
    fill(o, n, q + 8u, count);
    const u32 data = count != 0u || pick(2) != 0u ? data_at : 0u;
    fill(o, n, q + 0xCu, data);
    fill(o, n, q + 0x10u, data == 0u ? 0u : data_at + 4u * count);
    for (u32 i = 0; i < count; ++i) fill(o, n, data_at + 4u * i, 0x81410000u + 8u * pick(200));
}


float rfloat(float scale) {
    const u32 r = pick(8);
    if (r == 0) return static_cast<float>(static_cast<int>(pick(21)) - 10);          // ties are likely
    return (static_cast<float>(pick(20001)) - 10000.0f) * scale / 10000.0f;
}
u32 fbits(float f) { u32 w; std::memcpy(&w, &f, 4); return w; }

// Target objects for the floating-point entries: a vtable word (a foreign one for some when `foreign`), a position, a direction, a limit, flags.
void build_targets(Machine& o, Machine& n, bool foreign, bool all) {
    float va[3];
    for (u32 k = 0; k < 3u; ++k) va[k] = rfloat(60.0f);
    for (u32 off = 0; off < 0x40u; off += 4u) fill(o, n, kFakeVtable + off, 0x800091D0u);  // a fake vtable: every slot is a plain 3-float copy
    for (u32 t = 0; t < kTargetCount; ++t) {
        const u32 base = kTargets + t * 0x40u;
        const bool other = foreign && (all || pick(5) == 0);
        fill(o, n, base + 0u, other ? kFakeVtable : 0x8053ECB0u);
        fill(o, n, base + 4u, rnd());
        const u32 mode = pick(20);   // on the centre exactly (zero length), almost on it (below the normalise epsilon), or anywhere
        for (u32 k = 0; k < 3u; ++k) {
            float v = rfloat(60.0f);
            if (mode == 0u) std::memcpy(&v, &va[k], 4);
            else if (mode == 1u) { std::memcpy(&v, &va[k], 4); v += 1e-7f; }
            fill(o, n, base + 8u + 4u * k, fbits(v));
        }
        for (u32 k = 0; k < 3u; ++k) fill(o, n, base + 0x14u + 4u * k, fbits(rfloat(1.0f)));
        fill(o, n, base + 0x24u, fbits(rfloat(1.0f)));
        fill(o, n, base + 0x30u, (!all && pick(4) == 0) ? (rnd() & ~2u) : (rnd() | 2u));
    }
    for (u32 k = 0; k < 3u; ++k) { fill(o, n, kVecA + 4u * k, fbits(va[k])); fill(o, n, kVecB + 4u * k, fbits(rfloat(1.0f))); }
}

std::vector<u8> read_file(const char* path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<u8>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

int g_note = 0;                       // the guard control's classification of the call just served
unsigned g_cat_total[4] = {}, g_cat_caught[4] = {};
unsigned g_e_total[4] = {}, g_e_caught[4] = {};
int g_note_entry = 0;
bool g_tally_cats = false;
struct Tally { unsigned served = 0, declined = 0, caught = 0, shrunk = 0, partial = 0; unsigned per_entry[4] = {}; };
using Owner = std::function<bool(Machine&, u32)>;
Tally run_class(Machine& o, Machine& n, const char* label, Scenario scenario, unsigned calls, const Owner& owner, bool catch_only) {
    Tally t;
    for (unsigned i = 0; i < calls; ++i) {
        u32 entry = 0x801E7C40u, r3 = kReg, r4 = 0u, r5 = 0u;
        bool fp_entry = false;
        if (scenario == Scenario::CopyCase || scenario == Scenario::DtorCase) {
            build(o, n, Scenario::Live);   // fresh stale words everywhere
            const u32 count = pick(6) == 0 ? 0u : pick(60);
            build_query(o, n, kQ1, count, kQData);
            for (u32 off = 0; off < 0x40u; off += 4u) fill(o, n, kQ2 + off, 0xE8E80000u | off);
            if (scenario == Scenario::CopyCase) { entry = 0x801E8A68u; r3 = kQ2; r4 = kQ1; }
            else {
                entry = 0x801E8974u; r3 = pick(8) == 0 ? 0u : kQ1;
                static const u32 flags[] = {0xFFFFFFFFu, 0u, 1u, 0x10001u, 0xFFFF0000u, 2u};
                r4 = flags[pick(6)];
            }
        } else if (scenario == Scenario::SameCase || scenario == Scenario::FlagCase || scenario == Scenario::FpCase || scenario == Scenario::FpForeign || scenario == Scenario::FpAllForeign) {
            build(o, n, Scenario::Live);
            for (u32 i = 0; i < 60u; ++i) {   // 60 valid holders in the node area, a few expired
                const u32 node = kNodes + 0x10u * i;
                fill(o, n, node + 8u, kTargets + 0x40u * pick(kTargetCount));
                const u32 pn = kCounts + 0x10u * pick(kCountBlocks);
                fill(o, n, node + 0xCu, pn);
                if (pick(80) == 0) fill(o, n, pn + 4u, 0u);
            }
            // a query over the registry's holders: entries = the node holders (node + 8), vector in kQData
            const u32 count = pick(5) == 0 ? 0u : pick(40);
            fill(o, n, kQ1 + 0u, 0x8053ED04u); fill(o, n, kQ1 + 4u, count + 2u); fill(o, n, kQ1 + 8u, count);
            fill(o, n, kQ1 + 0xCu, kQData); fill(o, n, kQ1 + 0x10u, kQData + 4u * count);
            for (u32 i = 0; i < count; ++i) fill(o, n, kQData + 4u * i, kNodes + 0x10u * pick(60) + 8u);
            r3 = kQ1;
            if (scenario == Scenario::FpCase || scenario == Scenario::FpForeign || scenario == Scenario::FpAllForeign) {
                build_targets(o, n, scenario != Scenario::FpCase, scenario == Scenario::FpAllForeign);
                if (scenario == Scenario::FpAllForeign) {   // 1-3 entries, every target foreign with class bit 1: the view cone evaluates all of them
                    const u32 c2 = 1u + pick(3);
                    fill(o, n, kQ1 + 4u, c2 + 2u); fill(o, n, kQ1 + 8u, c2); fill(o, n, kQ1 + 0x10u, kQData + 4u * c2);
                }
                for (u32 b = 0; b < kCountBlocks; ++b) fill(o, n, kCounts + b * 0x10u + 4u, 1u + pick(5));   // the original dereferences the locked target: no expired holder
                static const u32 entries[] = {0x801E82C4u, 0x801E84C0u, 0x801E8528u, 0x801E879Cu};
                entry = entries[pick(4)];
                fp_entry = true;
                for (u32 off = 0; off < 0x40u; off += 4u) fill(o, n, kQ2 + off, 0xE8E80000u | off);
                if (entry == 0x801E879Cu) { r3 = kQ2; r4 = kQ1; r5 = kVecA; }
                else if (entry == 0x801E82C4u) { r4 = kVecB; r5 = kVecA; }
                else r4 = kVecA;
            } else if (scenario == Scenario::FlagCase) {
                entry = pick(2) != 0 ? 0x801E83D0u : 0x801E8448u;
                static const u32 masks[] = {0u, 0xFFFFFFFFu, 1u, 2u, 0x2000u, 0x800u, 0x80000u, 0x1800u};
                r4 = pick(3) == 0 ? rnd() : masks[pick(8)];
            } else {   // the same-target filter: the handle is a copy of one of the holders (so entries match), sometimes stale
                entry = 0x801E8158u;
                const u32 src = kNodes + 0x10u * pick(60) + 8u;
                fill(o, n, kQ2 + 0u, o.memory.peek32(src));
                fill(o, n, kQ2 + 4u, pick(40) == 0 ? 0u : o.memory.peek32(src + 4u));
                r4 = kQ2;
            }
        } else {
            build(o, n, scenario);
        }
        o.bump = n.bump = kHeap + 0x40u * pick(8); o.log.clear(); n.log.clear();
        u32 saved[18];
        for (u32 k = 0; k < 18; ++k) saved[k] = rnd();
        o.prepare(entry, r3, saved);
        n.prepare(entry, r3, saved);
        const u32 end_before = fp_entry ? o.memory.peek32(kQ1 + 0x10u) : 0u, begin_q = fp_entry ? o.memory.peek32(kQ1 + 0xCu) : 0u;
        o.cpu.gpr[4] = n.cpu.gpr[4] = r4;
        o.cpu.gpr[5] = n.cpu.gpr[5] = r5;
        if (fp_entry) {
            for (u32 f = 14u; f <= 31u; ++f) {
                const u64 a = (static_cast<u64>(rnd()) << 32) | rnd(), b = pick(3) == 0 ? ((static_cast<u64>(rnd()) << 32) | rnd()) : single_to_double_bits(fbits(rfloat(5.0f)));
                o.cpu.fpr[f].bits[0] = n.cpu.fpr[f].bits[0] = a; o.cpu.fpr[f].bits[1] = n.cpu.fpr[f].bits[1] = b;
            }
            const u64 radius = single_to_double_bits(fbits(pick(6) == 0 ? 0.0f : std::fabs(rfloat(90.0f))));
            o.cpu.fpr[1].bits[0] = n.cpu.fpr[1].bits[0] = radius;
            o.cpu.spr[SPR_GQR0] = n.cpu.spr[SPR_GQR0] = 0u;
        }
        o.run_original();
        const bool served = owner(n, entry);
        if (served) n.cpu.pc = n.cpu.lr; else n.run_original();
        served ? ++t.served : ++t.declined;
        if (fp_entry && served) {
            const u32 end_after = o.memory.peek32(kQ1 + 0x10u);
            if (end_after < end_before) { ++t.shrunk; if (end_after > begin_q) ++t.partial; }
            ++t.per_entry[entry == 0x801E82C4u ? 0 : entry == 0x801E84C0u ? 1 : entry == 0x801E8528u ? 2 : 3];
        }
        ++g_checked;
        std::string detail;
        if (g_tally_cats) { ++g_cat_total[g_note]; if (g_note == 3) ++g_e_total[g_note_entry]; }
        if (!same_state(o, n, label, detail)) {
            if (catch_only) { resync(o, n); ++t.caught; if (g_tally_cats) { ++g_cat_caught[g_note]; if (g_note == 3) ++g_e_caught[g_note_entry]; } } else report(label, detail);
        }
    }
    return t;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: s1_target2_contract_tests main.dol");
        const std::vector<u8> dol = read_file(argv[1]);
        require(dol.size() >= 256, "missing DOL");
        u64 hash = 0xCBF29CE484222325ull;
        for (u8 byte : dol) hash = (hash ^ byte) * 0x100000001B3ull;
        require(hash == 0xD2CFB4A1CA897F9Bull, "unexpected original DOL identity");
        Machine original(dol), native(dol);
        s1_target2::set_enabled(true);
        const Owner real = [](Machine& m, u32 entry) {
            return s1_target2::serve(m.cpu, entry == 0x801E7C40u ? s1_target2::Entry::Factory : entry == 0x801E8A68u ? s1_target2::Entry::CopyCtor : entry == 0x801E83D0u ? s1_target2::Entry::FlagA : entry == 0x801E8448u ? s1_target2::Entry::FlagB : entry == 0x801E8158u ? s1_target2::Entry::SameTarget : entry == 0x801E82C4u ? s1_target2::Entry::Sphere : entry == 0x801E84C0u ? s1_target2::Entry::ViewCone : entry == 0x801E8528u ? s1_target2::Entry::Range : entry == 0x801E879Cu ? s1_target2::Entry::Nearest : s1_target2::Entry::Dtor);
        };

        const Tally live = run_class(original, native, "live", Scenario::Live, 1500, real, false);
        std::printf("factory live: %u served, %u declined\n", live.served, live.declined);
        require(live.served * 100u >= 1500u * 99u, "live registries were declined too often");
        const Tally eventful = run_class(original, native, "eventful", Scenario::Eventful, 1500, real, false);
        std::printf("factory eventful: %u served, %u declined\n", eventful.served, eventful.declined);
        require(eventful.declined > 0, "no eventful registry was declined");

        const Tally copy = run_class(original, native, "copy ctor", Scenario::CopyCase, 1500, real, false);
        std::printf("copy ctor: %u served, %u declined%c", copy.served, copy.declined, 10);
        require(copy.served * 100u >= 1500u * 99u, "copy ctor calls were declined too often");
        const Tally dtor = run_class(original, native, "dtor", Scenario::DtorCase, 1500, real, false);
        std::printf("dtor: %u served, %u declined%c", dtor.served, dtor.declined, 10);
        require(dtor.served * 100u >= 1500u * 99u, "dtor calls were declined too often");

        const Tally flag = run_class(original, native, "flag filters", Scenario::FlagCase, 3000, real, false);
        std::printf("flag filters: %u served, %u declined%c", flag.served, flag.declined, 10);
        require(flag.served * 100u >= 3000u * 60u, "flag filter calls were declined too often");

        const Tally same = run_class(original, native, "same-target", Scenario::SameCase, 3000, real, false);
        std::printf("same-target filter: %u served, %u declined%c", same.served, same.declined, 10);
        require(same.served * 100u >= 3000u * 60u, "same-target calls were declined too often");

        const Tally fp = run_class(original, native, "query entries", Scenario::FpCase, 4000, real, false);
        std::printf("floating-point query entries: %u served, %u declined; sphere %u view cone %u range %u nearest %u; the query shrank in %u (a partial survivor set in %u)%c", fp.served, fp.declined, fp.per_entry[0], fp.per_entry[1], fp.per_entry[2], fp.per_entry[3], fp.shrunk, fp.partial, 10);
        require(fp.served * 100u >= 4000u * 50u, "floating-point query calls were declined too often");
        const Tally foreign = run_class(original, native, "foreign targets", Scenario::FpForeign, 2000, real, false);
        std::printf("foreign targets (guard on): %u served, %u declined%c", foreign.served, foreign.declined, 10);
        require(foreign.declined > 0, "a query with a foreign target was never handed back");

        // Negative controls: the same comparison must catch a wrong owner.
        unsigned c_stack = 0, c_alloc = 0, c_reg = 0, c_end = 0, c_log = 0, c_survivor = 0;
        c_stack = run_class(original, native, "ctl deep stack", Scenario::Live, 200, [&](Machine& m, u32 e) {
            const bool ok = real(m, e); if (ok) m.memory.write32(m.cpu.gpr[1] - 0x100u, 0u); return ok; }, true).caught;
        require(c_stack > 0, "negative control: a missing deep stack word was not caught");
        c_alloc = run_class(original, native, "ctl alloc", Scenario::Live, 200, [&](Machine& m, u32 e) {
            const bool ok = real(m, e); if (ok) m.bump += 8u; return ok; }, true).caught;
        require(c_alloc > 0, "negative control: an extra allocation was not caught");
        c_reg = run_class(original, native, "ctl register", Scenario::Live, 100, [&](Machine& m, u32 e) {
            const bool ok = real(m, e); if (ok) m.cpu.gpr[29] += 1u; return ok; }, true).caught;
        require(c_reg > 0, "negative control: a clobbered non-volatile register was not caught");
        c_end = run_class(original, native, "ctl end", Scenario::Live, 200, [&](Machine& m, u32 e) {
            const bool ok = real(m, e); if (ok) { const u32 q = m.cpu.gpr[3]; m.memory.write32(q + 0x10u, m.memory.peek32(q + 0x10u) + 4u); } return ok; }, true).caught;
        require(c_end > 0, "negative control: a wrong logical end was not caught");
        c_log = run_class(original, native, "ctl log", Scenario::Live, 100, [&](Machine& m, u32 e) {
            const bool ok = real(m, e); if (ok) m.log.emplace_back("N 0 00000000"); return ok; }, true).caught;
        require(c_log > 0, "negative control: a wrong allocator log was not caught");
        c_survivor = run_class(original, native, "ctl survivor", Scenario::Live, 200, [&](Machine& m, u32 e) {
            const bool ok = real(m, e); if (ok) { const u32 q = m.cpu.gpr[3]; const u32 data = m.memory.peek32(q + 0xCu); if (data != 0u) m.memory.write32(data, m.memory.peek32(data) ^ 0x10u); } return ok; }, true).caught;
        require(c_survivor > 0, "negative control: a wrong survivor was not caught");

        s1_target2::set_guard_enabled(false);   // the vtable-word guard removed: a foreign target's slot 2 is then not the Target's
        g_tally_cats = true;
        const unsigned c_guard = run_class(original, native, "ctl guard", Scenario::FpForeign, 600, [&](Machine& m, u32 e) {
            // 1 = no foreign target among the entries, 2 = foreign targets present but never evaluated (a view cone skips targets without class bit 1), 3 = a foreign target is evaluated
            int note = 1;
            const u32 b = m.memory.peek32(kQ1 + 0xCu), en = m.memory.peek32(kQ1 + 0x10u);
            for (u32 at = b; at < en; at += 4u) {
                const u32 px = m.memory.peek32(m.memory.peek32(at));
                if (m.memory.peek32(px) != kFakeVtable) continue;
                if (note < 2) note = 2;
                if (e != 0x801E84C0u || (m.memory.peek32(px + 0x30u) & 2u) != 0u) note = 3;
            }
            const bool ok = real(m, e);
            g_note = ok ? note : 0;
            g_note_entry = e == 0x801E82C4u ? 0 : e == 0x801E84C0u ? 1 : e == 0x801E8528u ? 2 : 3;
            return ok; }, true).caught;
        g_tally_cats = false;
        s1_target2::set_guard_enabled(true);
        require(c_guard > 0, "negative control: serving a foreign target without the vtable guard was not caught");
        std::printf("guard control: %u of 600 caught; handed back for another reason %u (caught %u); served with no foreign target in the query %u (caught %u); "
                    "foreign targets present but never evaluated %u (caught %u)\n",
                    c_guard, g_cat_total[0], g_cat_caught[0], g_cat_total[1], g_cat_caught[1], g_cat_total[2], g_cat_caught[2], g_cat_total[3], g_cat_caught[3]);
        std::printf("foreign target evaluated, by entry: sphere %u/%u caught, view cone %u/%u, range %u/%u, nearest %u/%u\n", g_e_caught[0], g_e_total[0], g_e_caught[1], g_e_total[1], g_e_caught[2], g_e_total[2], g_e_caught[3], g_e_total[3]);
        // Where the missing guard must show (every entry of a 1-3 entry query is a foreign target the view cone also evaluates): every served call must differ.
        std::memset(g_cat_total, 0, sizeof g_cat_total); std::memset(g_cat_caught, 0, sizeof g_cat_caught); std::memset(g_e_total, 0, sizeof g_e_total); std::memset(g_e_caught, 0, sizeof g_e_caught);
        s1_target2::set_guard_enabled(false);
        g_tally_cats = true;
        const unsigned c_forced = run_class(original, native, "ctl guard forced", Scenario::FpAllForeign, 400, [&](Machine& m, u32 e) {
            const bool ok = real(m, e); g_note = ok ? 3 : 0; g_note_entry = e == 0x801E82C4u ? 0 : e == 0x801E84C0u ? 1 : e == 0x801E8528u ? 2 : 3; return ok; }, true).caught;
        g_tally_cats = false;
        s1_target2::set_guard_enabled(true);
        std::printf("guard control, foreign-only queries (1-3 foreign entries): %u of %u served calls caught (sphere %u/%u, view cone %u/%u, range %u/%u, nearest %u/%u)%c", c_forced, g_cat_total[3], g_e_caught[0], g_e_total[0], g_e_caught[1], g_e_total[1], g_e_caught[2], g_e_total[2], g_e_caught[3], g_e_total[3], 10);
        require(c_forced > 0 && c_forced == g_cat_total[3], "negative control: a served foreign-only query without the vtable guard was not caught");
        require(g_cat_caught[0] + g_cat_caught[1] + g_cat_caught[2] == 0, "negative control: a call that cannot see a foreign target differed");
        const unsigned c_fp = run_class(original, native, "ctl fp stack", Scenario::FpCase, 200, [&](Machine& m, u32 e) {
            const bool ok = real(m, e); if (ok) m.memory.write32(m.cpu.gpr[1] - 0x40u, m.memory.peek32(m.cpu.gpr[1] - 0x40u) ^ 1u); return ok; }, true).caught;
        require(c_fp > 0, "negative control: a wrong dead stack word of a floating-point entry was not caught");
        const unsigned c_fpr = run_class(original, native, "ctl fp register", Scenario::FpCase, 200, [&](Machine& m, u32 e) {
            const bool ok = real(m, e); if (ok) m.cpu.fpr[31].bits[1] ^= 1ull; return ok; }, true).caught;
        require(c_fpr > 0, "negative control: a wrong callee-saved FPR was not caught");
        const unsigned c_rng = run_class(original, native, "ctl fp result", Scenario::FpCase, 200, [&](Machine& m, u32 e) {
            const bool ok = real(m, e); if (ok) { m.memory.write32(kQ1 + 0x10u, m.memory.peek32(kQ1 + 0x10u) - 4u); } return ok; }, true).caught;
        require(c_rng > 0, "negative control: a wrong logical end of a floating-point entry was not caught");

        if (g_failures != 0) {
            std::fprintf(stderr, "FAIL s1 target2 contract: %d mismatches of %d calls\n", g_failures, g_checked);
            return 1;
        }
        std::printf("PASS s1 target2 contract: %d calls compared (controls caught %u + %u + %u + %u + %u + %u; floating point %u + %u + %u + %u)\n", g_checked,
                    c_stack, c_alloc, c_reg, c_end, c_log, c_survivor, c_guard, c_fp, c_fpr, c_rng);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL s1 target2 contract: %s\n", error.what());
        return 1;
    }
}
