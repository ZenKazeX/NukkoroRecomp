// liveness-roots: reference-interpreter-only
// Original-DOL contract for the counted-handle family (gekko/native_s1_shptr.h): the original instructions run in the reference
// interpreter, the owner on a second machine with the same DOL and memory (a call the owner hands back runs the original there too).
// Fixture: four control blocks (vtable with dispose / destroy stubs, use and weak counts from the edge set 0, 1, 2, 3, 5, 0xFFFFFFFF),
// shared_ptr / weak_ptr objects (px, pi) that name a block or are empty, output objects. Every entry of the family is driven with
// random arguments, singly and in random sequences over the same blocks (copy, assign, release, lock interleavings), the state
// carried from call to call. Compared after every call: the whole 4 KiB arena (counts, handles), the stack window below r1 (the
// dead frame words of the whole chain, with the nested return addresses and the saved r28..r31), r1 r2 r3 r13 r14-r31, CR2-CR4, LR.
// Mandatory cases with a negative control each: last strong release (use -> 0, dispose), the weak count reaching zero (destroy), lock
// on an expired block (empty result, no increment), copy of an empty pointer, copy / release interleavings, the dead-stack words.
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_s1_shptr.h"

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
using s1_shptr::Entry;
using s1_shptr::Kind;
constexpr u32 kReturn = 0x817FF000u, kStack = 0x817E0000u, kR2 = 0x805FA780u, kR13 = 0x805EC500u;
constexpr u32 kArena = 0x81400000u, kArenaSize = 0x1000u;
constexpr u32 kVtable = kArena + 0x80u, kStub = kArena + 0xA0u;
constexpr u32 kBlocks = kArena + 0x100u, kShared = kArena + 0x200u, kWeak = kArena + 0x300u, kOut = kArena + 0x400u;
constexpr u32 kStackBelow = 0x300u, kStackAbove = 0x40u;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
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
    void prepare(u32 entry, const u32 args[2], const u32 saved[8]) {
        cpu.reset();
        cpu.set_msr(cpu.msr() | kMsrFP);
        cpu.pc = entry; cpu.npc = entry + 4; cpu.lr = kReturn;
        cpu.gpr[1] = kStack; cpu.gpr[2] = kR2; cpu.gpr[13] = kR13;
        cpu.gpr[3] = args[0]; cpu.gpr[4] = args[1];
        for (u32 k = 0; k < 8; ++k) cpu.gpr[24 + k] = saved[k];
    }
    void run_original() {
        unsigned steps = 0;
        while (cpu.pc != kReturn && !cpu.stopped() && steps++ < 400000) cpu.run_steps(1);
        if (!(cpu.pc == kReturn && !cpu.stopped() && cpu.gpr[1] == kStack))
            std::fprintf(stderr, "original run: pc %08X stopped %d reason %d r1 %08X\n", cpu.pc, cpu.stopped() ? 1 : 0,
                         int(cpu.stop().reason), cpu.gpr[1]);
        require(cpu.pc == kReturn && !cpu.stopped() && cpu.gpr[1] == kStack, "original call failed return/stack contract");
    }
};

u32 g_seed = 0x5EED5A5Au;
u32 rnd() { g_seed = g_seed * 1664525u + 1013904223u; return g_seed >> 8 ^ (g_seed << 16); }
u32 pick(u32 n) { return rnd() % n; }

void fill(Machine& a, Machine& b, u32 at, u32 word) { a.memory.write32(at, word); b.memory.write32(at, word); }

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
    if ((o.cpu.cr & 0x00FFF000u) != (n.cpu.cr & 0x00FFF000u)) { detail = std::string(name) + ": CR2-CR4 differ"; return false; }
    if (o.cpu.lr != n.cpu.lr) { detail = std::string(name) + ": LR differs"; return false; }
    return true;
}

// ---- fixture --------------------------------------------------------------------------------------------------------------------
u32 block(u32 i) { return kBlocks + 0x20u * i; }
u32 shared(u32 i) { return kShared + 0x10u * i; }
u32 weak(u32 i) { return kWeak + 0x10u * i; }
u32 out(u32 i) { return kOut + 0x10u * i; }

u32 count_value() {
    static const u32 v[] = {0u, 1u, 1u, 2u, 2u, 3u, 5u, 0xFFFFFFFFu};
    return v[pick(8)];
}

void build(Machine& o, Machine& n) {
    for (u32 off = 0; off < kArenaSize; off += 4u) fill(o, n, kArena + off, 0xA5A50000u | (off & 0xFFFFu));
    fill(o, n, kStub, 0x4E800020u);  // blr: the dispose / destroy virtuals
    fill(o, n, kVtable + 0xCu, kStub);
    fill(o, n, kVtable + 0x10u, kStub);
    for (u32 i = 0; i < 4; ++i) {
        fill(o, n, block(i) + 0u, kVtable);
        fill(o, n, block(i) + 4u, count_value());
        fill(o, n, block(i) + 8u, count_value());
    }
    for (u32 i = 0; i < 8; ++i) {
        fill(o, n, shared(i), 0x81500000u + 0x40u * i);  // px
        fill(o, n, shared(i) + 4u, pick(5) == 0 ? 0u : block(pick(4)));
        fill(o, n, weak(i), 0x81500000u + 0x40u * i);
        fill(o, n, weak(i) + 4u, pick(5) == 0 ? 0u : block(pick(4)));
    }
}

u32 any_flag() {
    static const u32 v[] = {0xFFFFFFFFu, 0u, 0xFFFF8000u, 0x00008000u, 0xFFFE0000u};  // s16 values <= 0
    return v[pick(5)];
}

struct Call { u32 args[2]; };
Call make_args(Kind kind) {
    Call c{};
    const u32 i = pick(8), j = pick(8);
    switch (kind) {
        case Kind::Release: case Kind::AddRefLock: c.args[0] = block(pick(4)); break;
        case Kind::ScDtor: c.args[0] = pick(8) == 0 ? 0u : shared(i) + 4u; c.args[1] = any_flag(); break;
        case Kind::ScCopy: case Kind::Assign: c.args[0] = shared(i) + 4u; c.args[1] = shared(j) + 4u; break;
        case Kind::WeakCopy: c.args[0] = weak(i) + 4u; c.args[1] = weak(j) + 4u; break;
        case Kind::WeakDtor: c.args[0] = pick(8) == 0 ? 0u : weak(i) + 4u; c.args[1] = any_flag(); break;
        case Kind::ScFromWeak: c.args[0] = shared(i) + 4u; c.args[1] = weak(j) + 4u; break;
        case Kind::Lock: c.args[0] = out(pick(4)); c.args[1] = weak(j); break;
        case Kind::DtorWrap: c.args[0] = pick(8) == 0 ? 0u : shared(i); c.args[1] = any_flag(); break;
        case Kind::CopyWrap: case Kind::AssignWrap: c.args[0] = shared(i); c.args[1] = shared(j); break;
        case Kind::kCount: break;
    }
    return c;
}

struct Tally { unsigned served = 0, declined = 0, caught = 0; };

// One call on both machines: the original on `o`, the owner (or the original after a hand-back) on `n`.
bool one_call(Machine& o, Machine& n, const char* label, const Entry& e, const Call& call, const std::function<bool(Cpu&, const Entry&)>& owner,
              bool catch_only, Tally& t) {
    u32 saved[8];
    for (u32 k = 0; k < 8; ++k) saved[k] = rnd();
    o.prepare(e.address, call.args, saved);
    n.prepare(e.address, call.args, saved);
    o.run_original();
    const bool served = owner(n.cpu, e);
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

const char* kind_name(Kind k) {
    static const char* const names[] = {"release", "sc_dtor", "sc_copy", "weak_copy", "assign", "weak_dtor", "sc_from_weak",
                                        "add_ref_lock", "lock", "dtor_wrap", "copy_wrap", "assign_wrap"};
    return names[static_cast<unsigned>(k)];
}

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: s1_core_shptr_tests main.dol");
        const std::vector<u8> dol = read_file(argv[1]);
        require(dol.size() >= 256, "missing DOL");
        u64 hash = 0xCBF29CE484222325ull;
        for (u8 byte : dol) hash = (hash ^ byte) * 0x100000001B3ull;
        require(hash == 0xD2CFB4A1CA897F9Bull, "unexpected original DOL identity");
        Machine original(dol), native(dol), scratch(dol);
        s1_shptr::set_enabled(true);
        s1_shptr::install(scratch.cpu);  // fills the entry table (the scratch machine is not run)
        unsigned count = 0;
        const Entry* entries = s1_shptr::entries(count);
        require(count > 30, "too few entries");
        const auto real = [](Cpu& c, const Entry& e) { return s1_shptr::serve(c, e); };

        // 1. every entry, single calls on fresh fixtures
        Tally per_kind[static_cast<unsigned>(Kind::kCount)];
        for (unsigned round = 0; round < 400; ++round) {
            for (unsigned k = 0; k < count; ++k) {
                build(original, native);
                one_call(original, native, "single", entries[k], make_args(entries[k].kind), real, false, per_kind[static_cast<unsigned>(entries[k].kind)]);
            }
        }
        for (unsigned k = 0; k < static_cast<unsigned>(Kind::kCount); ++k)
            std::printf("kind %-13s %u served, %u handed back\n", kind_name(static_cast<Kind>(k)), per_kind[k].served, per_kind[k].declined);
        for (unsigned k = 0; k < static_cast<unsigned>(Kind::kCount); ++k)
            require(per_kind[k].served > 0, "a kind was never served");

        // 2. random sequences over the same blocks: copy / assign / release / lock interleavings, state carried between calls
        Tally seq;
        for (unsigned round = 0; round < 3000; ++round) {
            build(original, native);
            for (unsigned step = 0; step < 12; ++step) {
                const Entry& e = entries[pick(count)];
                one_call(original, native, "sequence", e, make_args(e.kind), real, false, seq);
            }
        }
        std::printf("sequences: %u served, %u handed back\n", seq.served, seq.declined);

        // 3. directed: operator delete (flag > 0) and a foreign hook on a chain member are handed back before any write
        {
            build(original, native);
            u32 args[2] = {shared(0), 1u};
            u32 saved[8] = {1, 2, 3, 4, 5, 6, 7, 8};
            for (unsigned k = 0; k < count; ++k) {
                if (entries[k].kind != Kind::DtorWrap) continue;
                native.prepare(entries[k].address, args, saved);
                native.memory.write32(kStack - 0x10u, 0u);
                require(!s1_shptr::serve(native.cpu, entries[k]), "served an operator delete");
                require(native.memory.peek32(kStack - 0x10u) == 0u, "a handed-back call wrote its frame");
                break;
            }
            s1_shptr::reset_chain_cache();
            native.cpu.install_hle(0x80009408u, [](Cpu&) { return false; }, "watch");  // a --watch on the shared_count dtor
            for (unsigned k = 0; k < count; ++k) {
                if (entries[k].kind != Kind::DtorWrap) continue;
                build(original, native);
                u32 a2[2] = {shared(0), 0xFFFFFFFFu};
                native.prepare(entries[k].address, a2, saved);
                require(!s1_shptr::serve(native.cpu, entries[k]), "served a chain whose member has a foreign hook");
                break;
            }
            s1_shptr::reset_chain_cache();
        }
        // restore a machine without the foreign hook for the controls
        Machine original2(dol), native2(dol);
        s1_shptr::reset_chain_cache();

        // 4. negative controls, one per mandatory case, each on its own scenario
        const auto find = [&](Kind kind) {
            for (unsigned k = 0; k < count; ++k) if (entries[k].kind == kind) return &entries[k];
            return static_cast<const Entry*>(nullptr);
        };
        const auto run_control = [&](const char* label, Kind kind, unsigned calls, const std::function<void(Machine&, Machine&, Call&)>& setup,
                                     const std::function<bool(Cpu&, const Entry&)>& owner) {
            Tally t;
            for (unsigned i = 0; i < calls; ++i) {
                build(original2, native2);
                Call call = make_args(kind);
                setup(original2, native2, call);
                one_call(original2, native2, label, *find(kind), call, owner, true, t);
            }
            return t.caught;
        };
        // 4a. last strong release: the owner pretends to serve the dispose case (it must hand back)
        const unsigned c_last = run_control("ctl last release", Kind::Release, 200, [](Machine& a, Machine& b, Call& c) {
            fill(a, b, c.args[0] + 4u, 1u);  // use 1 -> 0
        }, [](Cpu& c, const Entry& e) { s1_shptr::serve(c, e); return true; });
        require(c_last > 0, "negative control: serving the last strong release was not caught");
        // 4b. weak count reaching zero
        const unsigned c_weak = run_control("ctl weak zero", Kind::WeakDtor, 200, [](Machine& a, Machine& b, Call& c) {
            const u32 pi = a.memory.peek32(c.args[0] != 0u ? c.args[0] : weak(0) + 4u);
            if (c.args[0] == 0u) c.args[0] = weak(0) + 4u;
            if (pi != 0u) fill(a, b, pi + 8u, 1u);
            else { fill(a, b, weak(0) + 4u, block(0)); fill(a, b, block(0) + 8u, 1u); }
            c.args[1] = 0xFFFFFFFFu;
        }, [](Cpu& c, const Entry& e) { s1_shptr::serve(c, e); return true; });
        require(c_weak > 0, "negative control: serving the weak count reaching zero was not caught");
        // 4c. lock on an expired block: empty result, no increment (the owner increments anyway)
        const unsigned c_lock = run_control("ctl expired lock", Kind::Lock, 200, [](Machine& a, Machine& b, Call& c) {
            const u32 w = c.args[1];
            u32 pi = a.memory.peek32(w + 4u);
            if (pi == 0u) { pi = block(1); fill(a, b, w + 4u, pi); }
            fill(a, b, pi + 4u, 0u);  // use 0: expired
        }, [](Cpu& c, const Entry& e) {
            const bool ok = s1_shptr::serve(c, e);
            const u32 pi = c.mem_read32(c.gpr[4] + 4u);
            if (ok && pi != 0u) c.mem_write32(pi + 4u, c.mem_read32(pi + 4u) + 1u);
            return ok; });
        require(c_lock > 0, "negative control: an incrementing lock of an expired pointer was not caught");
        // 4d. copy of an empty pointer: the owner touches the (absent) block
        const unsigned c_empty = run_control("ctl empty copy", Kind::CopyWrap, 200, [](Machine& a, Machine& b, Call& c) {
            fill(a, b, c.args[1] + 4u, 0u);
        }, [](Cpu& c, const Entry& e) {
            const bool ok = s1_shptr::serve(c, e);
            c.mem_write32(kBlocks + 4u, c.mem_read32(kBlocks + 4u) + 1u);  // a count bumped although the pointer was empty
            return ok; });
        require(c_empty > 0, "negative control: a count touched by the copy of an empty pointer was not caught");
        // 4e. copy then release interleavings: a copy that loses its increment
        const unsigned c_inter = run_control("ctl interleave", Kind::CopyWrap, 200, [](Machine& a, Machine& b, Call& c) {
            fill(a, b, c.args[1] + 4u, block(2));
            fill(a, b, block(2) + 4u, 3u);
        }, [](Cpu& c, const Entry& e) {
            const bool ok = s1_shptr::serve(c, e);
            c.mem_write32(kBlocks + 0x40u + 4u, c.mem_read32(kBlocks + 0x40u + 4u) - 1u);  // the copy's use++ undone
            return ok; });
        require(c_inter > 0, "negative control: a lost increment was not caught");
        // 4f. the dead-stack words of the chain
        const unsigned c_frame = run_control("ctl frame", Kind::DtorWrap, 200, [](Machine& a, Machine& b, Call& c) {
            c.args[0] = shared(0);
            c.args[1] = 0xFFFFFFFFu;
            fill(a, b, shared(0) + 4u, block(0));
            fill(a, b, block(0) + 4u, 3u);
        }, [](Cpu& c, const Entry& e) {
            const bool ok = s1_shptr::serve(c, e);
            c.mem_write32(kStack - 0x20u + 0x14u, 0u);  // the nested return-address word of the shared_count dtor frame
            return ok; });
        require(c_frame > 0, "negative control: a missing dead-stack word was not caught");

        if (g_failures != 0) {
            std::fprintf(stderr, "FAIL s1 core shptr contract: %d mismatches of %d calls\n", g_failures, g_checked);
            return 1;
        }
        std::printf("PASS s1 core shptr contract: %d calls compared (controls caught %u + %u + %u + %u + %u + %u)\n", g_checked, c_last,
                    c_weak, c_lock, c_empty, c_inter, c_frame);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL s1 core shptr contract: %s\n", error.what());
        return 1;
    }
}
