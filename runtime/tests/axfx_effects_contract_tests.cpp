// Original-DOL ownership oracle for the game's ReverbStd, Chorus and Delay set-up (gekko/native_axfx_effects.h).
// liveness-roots: reference-interpreter-only
// No generated reference body: every SDK instruction comes from the pinned DOL.
// The allocator is a guarded test fixture, not a proposed production heap.
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_axfx_effects.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace gekko;
using namespace gekko::axfx_effects;

constexpr u32 kState = 0x81000000u;
constexpr u32 kHeap = 0x81004000u, kHeapLimit = 0x81300000u;
constexpr u32 kReturn = 0x817FF000u, kAlloc = kReturn + 4, kFree = kReturn + 8;
constexpr u32 kStack = 0x817E0000u, kR2 = 0x805FA780u, kR13 = 0x805EC500u;
constexpr u32 kGuard = 0xA5A5A5A5u;
constexpr u32 kObjectBytes = 0x200u;  // the largest object (ReverbStd, 0x154 bytes) plus margin

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
u32 bits(float value) {
    u32 result;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

unsigned g_zero_block_leaks = 0;  // cases where the original leaks zero-byte blocks (see run_original)
struct Allocation { u32 address, bytes; bool live; };
struct Oracle {
    GuestMemory memory;
    Cpu cpu{memory};
    std::vector<Allocation> allocations;
    unsigned alloc_calls = 0, fail_at = 0;
    u32 cursor = kHeap;
    u64 instructions = 0;

    explicit Oracle(const char* path) {
        std::ifstream input(path, std::ios::binary);
        const std::vector<u8> dol{std::istreambuf_iterator<char>(input), {}};
        require(dol.size() >= 256, "missing DOL");
        u64 hash = 0xCBF29CE484222325ull;
        for (u8 byte : dol) hash = (hash ^ byte) * 0x100000001B3ull;
        // SHA-256: fde4fa6f81a60313b710161c196dc51c2260be62251ee02775d5eee06f9d55af.
        require(hash == 0xD2CFB4A1CA897F9Bull, "unexpected original DOL identity");
        const auto word = [&](unsigned offset) { return load_be32(dol.data() + offset); };
        for (unsigned i = 0; i < 18; ++i) {
            const u32 offset = word(i * 4), address = word(0x48 + i * 4), size = word(0x90 + i * 4);
            require(u64(offset) + size <= dol.size(), "invalid DOL section");
            if (size) memory.write_block(address, dol.data() + offset, size);
        }
        reset();
    }
    void reset() {
        allocations.clear(); alloc_calls = fail_at = 0; cursor = kHeap;
        const std::vector<u8> zero(kHeapLimit - kState);
        memory.write_block(kState, zero.data(), zero.size());
        memory.write32(kR13 + 0x27B8, kAlloc);
        memory.write32(kR13 + 0x27BC, kFree);
    }
    void returned() { cpu.pc = cpu.lr; cpu.npc = cpu.pc + 4; }
    // The game's heap (0x8043BDE4) rounds a request up to 32 bytes, so a zero-size request is served like any other
    // while a free block exists: the fixture serves it too.
    void allocate() {
        const u32 bytes = cpu.gpr[3];
        if (++alloc_calls == fail_at) { cpu.gpr[3] = 0; returned(); return; }
        require(bytes < 0x100000u && cursor + bytes + 64 < kHeapLimit, "unbounded allocation");
        const u32 address = cursor + 16;
        allocations.push_back({address, bytes, true});
        for (u32 offset = 0; offset < 16; offset += 4) {
            memory.write32(address - 16 + offset, kGuard);
            memory.write32(address + bytes + offset, kGuard);
        }
        for (u32 offset = 0; offset < bytes; offset += 4) memory.write32(address + offset, 0xCDCDCDCDu);
        cursor = (address + bytes + 31) & ~15u;
        cpu.gpr[3] = address; returned();
    }
    void free() {
        const auto found = std::find_if(allocations.begin(), allocations.end(),
            [&](const Allocation& a) { return a.address == cpu.gpr[3] && a.live; });
        require(found != allocations.end(), "unknown allocation or double free");
        found->live = false; returned();
    }
    void guards() {
        for (const auto& a : allocations) for (u32 offset = 0; offset < 16; offset += 4)
            require(memory.peek32(a.address - 16 + offset) == kGuard &&
                    memory.peek32(a.address + a.bytes + offset) == kGuard, "allocation guard overwritten");
    }
    void call(u32 entry, u32 r3 = kState) {
        cpu.reset(); cpu.pc = entry; cpu.npc = entry + 4; cpu.lr = kReturn;
        cpu.fpscr = 0;
        cpu.gpr[1] = kStack; cpu.gpr[2] = kR2; cpu.gpr[13] = kR13;
        cpu.gpr[3] = r3;
        const u64 tick = cpu.game_clock().now();
        unsigned steps = 0;
        while (cpu.pc != kReturn && !cpu.stopped() && steps++ < 60000000) {
            if (cpu.pc == kAlloc) { allocate(); continue; }
            if (cpu.pc == kFree) { free(); continue; }
            cpu.run_steps(1); ++instructions;
        }
        if (cpu.pc != kReturn || cpu.stopped())
            std::fprintf(stderr, "entry=%08X pc=%08X steps=%u stop=%s\n", entry, cpu.pc, steps,
                         cpu.stop().detail.c_str());
        require(cpu.pc == kReturn && !cpu.stopped() && cpu.gpr[1] == kStack &&
                cpu.game_clock().now() == tick, "original call failed return/stack/time contract");
        guards();
    }
    unsigned live() const {
        return static_cast<unsigned>(std::count_if(allocations.begin(), allocations.end(),
                                                   [](const Allocation& a) { return a.live; }));
    }
    std::vector<u8> snapshot() {
        std::vector<u8> result(kHeapLimit - kState);
        memory.read_block(kState, result.data(), result.size());
        return result;
    }
};

// One effect: its entries, where its parameters sit, which values the game keeps there, and how many lines the
// original allocates for them.
struct Effect {
    const char* name;
    u32 init, shutdown;
    u32 parameters;                       // offset of the first parameter word
    std::vector<u32> live;                // the parameter words of an accepted configuration
    // (low, high) bounds per word for the sweep; floats are bit patterns of the bound values.
    std::vector<std::array<u32, 2>> bounds;
    bool floats;
    std::function<bool(Cpu&)> native_init;
    unsigned lines;                       // allocations of an accepted live configuration
};

// What the originals leave in the game's hands: r3 of init and of shutdown, and whether init built anything.
struct Outcome { u32 init_r3, shutdown_r3; unsigned allocations; bool built; };

Outcome run_original(Oracle& o, const Effect& e, const std::vector<u32>& words) {
    o.reset();
    for (std::size_t i = 0; i < words.size(); ++i) o.memory.write32(kState + e.parameters + u32(i) * 4u, words[i]);
    o.call(e.init);
    Outcome out{o.cpu.gpr[3], 0u, static_cast<unsigned>(o.allocations.size()), false};
    out.built = out.init_r3 != 0u;
    if (!out.built) {
        require(o.live() == 0u, "a refused original init left an allocation");
        return out;
    }
    require(out.init_r3 == 1u, "original init returned neither 0 nor 1");
    require(o.live() == out.allocations, "an accepted original init freed something at once");
    o.call(e.shutdown);
    out.shutdown_r3 = o.cpu.gpr[3];
    // A pre-delay so small that it rounds to zero samples makes ReverbStd request three zero-byte blocks, and its
    // shutdown then skips them (it frees the pre-delay lines only when the sample count is nonzero): the original
    // leaks them. Nothing else may remain allocated.
    const bool zero_blocks = std::all_of(o.allocations.begin(), o.allocations.end(),
                                         [](const Allocation& a) { return !a.live || a.bytes == 0u; });
    require(zero_blocks, "the original shutdown leaked a non-empty allocation");
    if (o.live() != 0u) ++g_zero_block_leaks;
    return out;
}

using Judge = std::function<u32(Oracle&, const Effect&)>;

// Native init + shutdown on a fresh object holding the same words: returns the init r3 and requires that not one
// byte of the object or of the heap region changed, and that the shutdown returns 1 and writes nothing.
u32 run_native(Oracle& o, const Effect& e, const std::vector<u32>& words, const Judge& mutant) {
    o.reset();
    for (std::size_t i = 0; i < words.size(); ++i) o.memory.write32(kState + e.parameters + u32(i) * 4u, words[i]);
    // A sentinel in every byte the object could hold, so a write of any value (even the old one) cannot hide.
    for (u32 offset = 0; offset < kObjectBytes; offset += 4) {
        const bool parameter = offset >= e.parameters && offset < e.parameters + words.size() * 4u;
        if (!parameter) o.memory.write32(kState + offset, 0x5A5A5A5Au + offset);
    }
    const std::vector<u8> before = o.snapshot();
    o.cpu.gpr[3] = kState;
    require(e.native_init(o.cpu), "native init refused a readable object");
    u32 r3 = o.cpu.gpr[3];
    if (mutant) r3 = mutant(o, e);
    o.cpu.gpr[3] = kState;
    serve_shutdown(o.cpu);
    require(o.cpu.gpr[3] == 1u, "native shutdown result differs");
    require(o.snapshot() == before, "a native owner wrote a byte of guest memory");
    return r3;
}

// The sweep: for every parameter word, every bound, one step outside, zero, middle, and (floats) -0, +-inf, NaN, the
// native verdict equals the original's. Returns the number of disagreements (zero for the real owner).
unsigned sweep(Oracle& o, const Effect& e, const Judge& mutant, unsigned& cases, unsigned& accepted) {
    std::vector<std::vector<u32>> inputs{e.live};
    const float inf = std::numeric_limits<float>::infinity(), nan = std::numeric_limits<float>::quiet_NaN();
    for (std::size_t field = 0; field < e.live.size(); ++field) {
        std::vector<u32> values;
        const u32 low = e.bounds[field][0], high = e.bounds[field][1];
        if (e.floats) {
            float l, h;
            std::memcpy(&l, &low, 4); std::memcpy(&h, &high, 4);
            for (float v : {l, std::nextafter(l, -inf), h, std::nextafter(h, inf), -0.0f, 0.0f, (l + h) / 2, inf, -inf,
                            nan, -1.0f, 11.0f, 1e-30f, std::nextafter(0.0f, inf)})
                values.push_back(bits(v));
        } else {
            for (u32 v : {low, low - 1u, high, high + 1u, 0u, (low + high) / 2u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu})
                values.push_back(v);
        }
        for (u32 value : values) {
            auto words = e.live;
            words[field] = value;
            inputs.push_back(words);
        }
    }
    unsigned mismatches = 0;
    for (const auto& words : inputs) {
        const Outcome original = run_original(o, e, words);
        const u32 native = run_native(o, e, words, mutant);
        if (native != original.init_r3) ++mismatches;
        else if (!mutant && original.built) {
            // The shutdown results already compared inside run_native (1); the original's must agree.
            require(original.shutdown_r3 == 1u, "original shutdown did not return 1");
        }
        ++cases; if (original.built) ++accepted;
    }
    return mismatches;
}

std::vector<Effect> effects() {
    const auto f = [](float v) { return bits(v); };
    std::vector<Effect> list;
    // ReverbStd: words in memory order +0x140 coloration, +0x144 mix, +0x148 time, +0x14C damping, +0x150 pre-delay.
    list.push_back({"ReverbStd", kStdInit, kStdShutdown, kStdParametersOffset,
                    {f(0.5f), f(0.5f), f(3.0f), f(0.5f), f(0.1f)},
                    {{{f(0), f(1)}}, {{f(0), f(1)}}, {{f(0.01f), f(10)}}, {{f(0), f(1)}}, {{f(0), f(0.1f)}}}, true,
                    [](Cpu& c) { return serve_std_init(c); }, 15});
    list.push_back({"Chorus", kChorusInit, kChorusShutdown, kChorusParametersOffset, {15, 0, 500},
                    {{{5, 15}}, {{0, 5}}, {{500, 10000}}}, false,
                    [](Cpu& c) { return serve_chorus_init(c); }, 1});
    list.push_back({"Delay", kDelayInit, kDelayShutdown, kDelayParametersOffset,
                    {500, 500, 500, 50, 50, 50, 100, 100, 100},
                    {{{10, 5000}}, {{10, 5000}}, {{10, 5000}}, {{0, 100}}, {{0, 100}}, {{0, 100}},
                     {{0, 100}}, {{0, 100}}, {{0, 100}}}, false,
                    [](Cpu& c) { return serve_delay_init(c); }, 3});
    return list;
}

// Lifecycle of each original: what it allocates, that its shutdown frees all of it, that every allocation failure
// point is propagated as 0 without a leak (the native owners never allocate, so this path is retired: stated here,
// not reproduced), and that the native owners judge the same live object the same way.
void lifecycle(Oracle& o, const Effect& e) {
    const Outcome live = run_original(o, e, e.live);
    require(live.built && live.allocations == e.lines, "live initialization allocated another number of lines");
    for (unsigned failure = 1; failure <= live.allocations; ++failure) {
        o.reset(); o.fail_at = failure;
        for (std::size_t i = 0; i < e.live.size(); ++i) o.memory.write32(kState + e.parameters + u32(i) * 4u, e.live[i]);
        o.call(e.init);
        require(o.cpu.gpr[3] == 0u && o.alloc_calls >= failure && o.live() == 0u,
                "allocation failure was not propagated without a leak");
    }
    std::printf("PASS %s original lifecycle: %u allocations, shutdown frees all, %u allocation-failure points\n",
                e.name, live.allocations, live.allocations);
}

// Both callers discard every result: the six call sites and the one caller of the start-up routine.
void results_are_discarded(Oracle& o) {
    // init: after bl, the next instruction is lwz r3, 0x5FAC(r13) (0x806D5FAC) ...
    for (u32 after : {0x8043F9D4u, 0x8043F9F4u, 0x8043F6C0u, 0x8043F6E0u})
        require(o.memory.peek32(after) == 0x806D5FACu, "a caller of a std/chorus init or shutdown reads r3");
    // ... Delay init is the last call of the start-up routine (0x8043F9A0), whose own caller loads r4, r0 and then
    // overwrites r3 (li r3, 0) before it uses it; Delay shutdown is followed by lwz r0 and later li r3, 0.
    require(o.memory.peek32(0x8043FA04) == 0x80010014u && o.memory.peek32(0x8043FA08) == 0x7C0803A6u &&
            o.memory.peek32(0x8043F978) == 0x48000029u && o.memory.peek32(0x8043F97C) == 0x808D5FACu &&
            o.memory.peek32(0x8043F980) == 0x38000001u && o.memory.peek32(0x8043F984) == 0x38600000u,
            "the start-up routine's caller does not discard r3");
    require(o.memory.peek32(0x8043F6F0) == 0x800D5FACu && o.memory.peek32(0x8043F700) == 0x38600000u,
            "the shutdown routine reads the Delay shutdown's r3");
    // The selector's callers pass the constants 2 and 0: li r4, 2 / li r4, 0 at 0x804402B8 and 0x80440274.
    require(o.memory.peek32(0x804402B8) == 0x38800002u && o.memory.peek32(0x80440274) == 0x38800000u,
            "the selector is called with other modes than 2 and 0");
    std::puts("PASS the results of every init/shutdown are discarded; the selector is only called with modes 2 and 0");
}


// ---- The aux callbacks (ReverbStd 0x803890B0, Chorus 0x8038980C, Delay 0x80389B5C) -------------------------------------
// They are stop owners: unreachable, so nothing is compared with a native effect. The proofs below are the contract.

constexpr u32 kSelector = 0x8043F304u, kRegisterA = 0x80385050u, kRegisterB = 0x8038505Cu;
constexpr u32 kHiCallback = 0x803885A0u;
constexpr u32 kSoundObject = 0x81200000u;  // what r13 + 0x5FAC points at in this fixture
constexpr u32 kSelectorBytes = 0xE4u;      // 0x8043F304 .. 0x8043F3E4

struct Section { u32 address, size; bool text; };
std::vector<Section> g_sections;

void read_sections(const char* path) {
    std::ifstream input(path, std::ios::binary);
    const std::vector<u8> dol{std::istreambuf_iterator<char>(input), {}};
    for (unsigned i = 0; i < 18; ++i) {
        const u32 size = load_be32(dol.data() + 0x90 + i * 4u);
        if (size) g_sections.push_back({load_be32(dol.data() + 0x48 + i * 4u), size, i < 7});
    }
}

// What the original selector registers for (aux, mode), read by running its bytes in the interpreter.
struct Registration { bool called; u32 aux; u32 callback, context; };
Registration run_selector(Oracle& o, u32 aux, u32 mode) {
    o.reset();
    o.memory.write32(kR13 + 0x5FAC, kSoundObject);
    o.cpu.reset(); o.cpu.pc = kSelector; o.cpu.npc = kSelector + 4; o.cpu.lr = kReturn;
    o.cpu.gpr[1] = kStack; o.cpu.gpr[2] = kR2; o.cpu.gpr[13] = kR13; o.cpu.gpr[3] = aux; o.cpu.gpr[4] = mode;
    Registration seen{false, 0, 0, 0};
    unsigned steps = 0;
    while (o.cpu.pc != kReturn && !o.cpu.stopped() && steps++ < 1000) {
        if (o.cpu.pc == kRegisterA || o.cpu.pc == kRegisterB) {
            seen = {true, o.cpu.pc == kRegisterA ? 0u : 1u, o.cpu.gpr[3], o.cpu.gpr[4]};
            o.cpu.pc = o.cpu.lr; o.cpu.npc = o.cpu.pc + 4;
            continue;
        }
        o.cpu.run_steps(1);
    }
    require(o.cpu.pc == kReturn && !o.cpu.stopped(), "the original selector did not return");
    return seen;
}

// Conservative: any instruction that names r4 as its target or base (a store only when it updates rA).
bool touches_r4(u32 word) {
    const u32 op = word >> 26, rt = (word >> 21) & 31u, ra = (word >> 16) & 31u;
    if (op == 10 || op == 11) return false;  // compares
    if (op == 37 || op == 39 || op == 45 || op == 53 || op == 55) return ra == 4u;  // store with update
    if (op == 36 || op == 38 || op == 44 || op == 47 || op == 52 || op == 54) return false;  // plain stores
    return rt == 4u || ra == 4u;
}

struct Hit { u32 site, target; const char* how; };

void scan_references(Oracle& o, const std::vector<u32>& targets, std::vector<Hit>& hits) {
    const auto note = [&](u32 site, u32 value, const char* how) {
        for (u32 t : targets) if (value == t) hits.push_back({site, t, how});
    };
    for (const Section& sec : g_sections) {
        const u32 end = sec.address + sec.size;
        for (u32 at = sec.address; at + 4 <= end; at += 4) {
            const u32 word = o.memory.peek32(at);
            if (!sec.text) { note(at, word, "data word"); continue; }
            const u32 op = word >> 26;
            if (op == 18) {  // b, bl, ba, bla
                const u32 disp = static_cast<u32>(static_cast<s32>((word & 0x03FFFFFCu) << 6) >> 6);
                note(at, (word & 2u) ? disp : at + disp, (word & 1u) ? "bl" : "b");
            } else if (op == 16) {  // conditional branch
                const u32 disp = static_cast<u32>(static_cast<s32>(static_cast<s16>(word & 0xFFFCu)));
                note(at, (word & 2u) ? disp : at + disp, "bc");
            } else if (op == 15) {  // addis rD, rA, hi; a pair with addi / ori of the same register within 8 instructions
                const u32 reg = (word >> 21) & 31u, hi = (word & 0xFFFFu) << 16;
                const bool lis = ((word >> 16) & 31u) == 0u;
                for (u32 look = 1; look <= 8u && at + look * 4u + 4u <= end; ++look) {
                    const u32 next = o.memory.peek32(at + look * 4u);
                    if ((next >> 26) == 14u && ((next >> 16) & 31u) == reg)  // addi rD, reg, lo
                        note(at, hi + static_cast<u32>(static_cast<s32>(static_cast<s16>(next & 0xFFFFu))), "lis/addi");
                    if (lis && (next >> 26) == 24u && ((next >> 21) & 31u) == reg)  // ori rA, reg, lo
                        note(at, hi | (next & 0xFFFFu), "lis/ori");
                }
            }
        }
    }
}

// The rest of the proof (b) + (c): empty when every reference is accounted for, else what is wrong.
std::string reachability_problem(Oracle& o, std::vector<u32>& caller_sites, std::vector<u32>& loaded, std::size_t& reference_count) {
    std::vector<Hit> hits;
    scan_references(o, {kSelector, kRegisterA, kRegisterB, kStdCallback, kChorusCallback, kDelayCallback, kHiCallback}, hits);
    unsigned callers = 0, register_a = 0, register_b = 0, other = 0;
    std::vector<u32> materialized_targets;
    caller_sites.clear(); loaded.clear();
    const auto inside_selector = [](u32 site) { return site >= kSelector && site < kSelector + kSelectorBytes; };
    for (const Hit& h : hits) {
        const bool callback = h.target == kStdCallback || h.target == kChorusCallback || h.target == kDelayCallback ||
                              h.target == kHiCallback;
        if (h.target == kSelector && !std::strcmp(h.how, "bl")) { ++callers; caller_sites.push_back(h.site); }
        else if (h.target == kRegisterA && inside_selector(h.site) && !std::strcmp(h.how, "bl")) ++register_a;
        else if (h.target == kRegisterB && inside_selector(h.site) && !std::strcmp(h.how, "bl")) ++register_b;
        else if (callback && inside_selector(h.site) && !std::strcmp(h.how, "lis/addi")) materialized_targets.push_back(h.target);
        else ++other;
    }
    reference_count = hits.size();
    if (other != 0u) return "something other than the selector names the selector, a register function or a callback";
    // a lis reaches the addi of the next block too (same base register), so a target may be seen twice: compare the set
    std::sort(materialized_targets.begin(), materialized_targets.end());
    materialized_targets.erase(std::unique(materialized_targets.begin(), materialized_targets.end()), materialized_targets.end());
    if (register_a != 1u || register_b != 1u ||
        materialized_targets != std::vector<u32>{kHiCallback, kStdCallback, kChorusCallback, kDelayCallback})
        return "the selector's own references are not the expected ones";
    if (callers != 2u) return "the selector has other callers than the two known sites";
    // each caller's mode is a constant: the last instruction touching r4 before the bl is li r4, 2 / li r4, 0 with no
    // control flow in between
    std::sort(caller_sites.begin(), caller_sites.end());
    for (u32 site : caller_sites) {
        u32 found = 0xFFFFFFFFu;
        for (u32 at = site - 4; at >= site - 64u; at -= 4) {
            const u32 word = o.memory.peek32(at), op = word >> 26;
            if (op == 16 || op == 18 || op == 19) break;
            if (touches_r4(word)) { found = word; break; }
        }
        if (found != 0x38800002u && found != 0x38800000u) return "a caller of the selector does not load its mode as li r4, 2 / 0";
        loaded.push_back(found & 0xFFFFu);
    }
    return {};
}

// The address of the li r4 a caller of the selector ends with.
u32 mode_load_address(Oracle& o, u32 site) {
    for (u32 at = site - 4; at >= site - 64u; at -= 4)
        if (touches_r4(o.memory.peek32(at))) return at;
    return 0u;
}

// The two facts that make modes 1, 3, 4 unreachable, from the bytes of the DOL.
void callbacks_unreachable(Oracle& o) {
    // (a) the selector registers exactly these callbacks for these modes, on aux A (r3 = 0) and aux B (r3 = 1)
    const u32 base = kSoundObject + 0x10000u;
    const struct { u32 mode, callback, context; } modes[] = {
        {1, kStdCallback, base - 0x1FCu}, {2, kHiCallback, base - 0xA8u},
        {3, kChorusCallback, base + 0x138u}, {4, kDelayCallback, base + 0x1D4u}};
    for (const auto& m : modes)
        for (u32 aux : {0u, 1u}) {
            const Registration r = run_selector(o, aux, m.mode);
            require(r.called && r.aux == aux && r.callback == m.callback && r.context == m.context,
                    "the selector registers another callback or context for a mode");
        }
    for (u32 aux : {0u, 1u}) {
        const Registration r = run_selector(o, aux, 0);
        require(r.called && r.callback == 0u, "selector mode 0 does not clear the callback");
    }
    for (u32 mode : {5u, 6u, 0xFFFFFFFFu, 0x80000000u})
        require(!run_selector(o, 0, mode).called, "a selector mode outside 0..4 registers something");

    // (b) + (c)
    std::vector<u32> caller_sites, loaded;
    std::size_t reference_count = 0;
    const std::string problem = reachability_problem(o, caller_sites, loaded, reference_count);
    if (!problem.empty()) std::fprintf(stderr, "reachability: %s\n", problem.c_str());
    require(problem.empty(), "the aux callbacks may be reachable (see the line above)");
    const unsigned callers = static_cast<unsigned>(caller_sites.size());
    // Negative controls: plant, in the DOL image, each way the callbacks could become reachable; each must be reported.
    const u32 spare = 0x80003200u;  // an instruction in the first text section, restored after every control
    const u32 saved = o.memory.peek32(spare);
    std::size_t scratch_count = 0;
    std::vector<u32> sites_copy, loaded_copy;
    const auto bl_to = [&](u32 target) { return 0x48000001u | ((target - spare) & 0x03FFFFFCu); };
    const auto lis = [](u32 reg, u32 value) { return 0x3C000000u | (reg << 21) | (value >> 16); };
    const auto addi = [](u32 rd, u32 ra, u32 value) { return 0x38000000u | (rd << 21) | (ra << 16) | (value & 0xFFFFu); };
    struct Plant { const char* what; std::vector<std::pair<u32, u32>> words; };
    const u32 mode_word_at = mode_load_address(o, caller_sites[0]);
    const Plant plants[] = {
        {"a third caller of the selector", {{spare, bl_to(kSelector)}}},
        {"a direct call of the ReverbStd callback", {{spare, bl_to(kStdCallback)}}},
        {"a direct call of the Chorus callback", {{spare, bl_to(kChorusCallback)}}},
        {"a direct call of AXRegisterAuxACallback", {{spare, bl_to(kRegisterA)}}},
        {"a lis/addi naming the Delay callback", {{spare, lis(5, kDelayCallback + 0x8000u)}, {spare + 4, addi(0, 5, kDelayCallback)}}},
        {"a function pointer to the Chorus callback in a data word", {{0x8056E98Cu, kChorusCallback}}},
        {"a caller loading another mode", {{mode_word_at, 0x38800003u}}},
    };
    for (const Plant& plant : plants) {
        std::vector<std::pair<u32, u32>> before;
        for (const auto& w : plant.words) { before.push_back({w.first, o.memory.peek32(w.first)}); o.memory.write32(w.first, w.second); }
        const std::string found = reachability_problem(o, sites_copy, loaded_copy, scratch_count);
        for (const auto& w : before) o.memory.write32(w.first, w.second);
        if (found.empty()) std::fprintf(stderr, "control not detected: %s\n", plant.what);
        require(!found.empty(), "a negative control (a way into the callbacks planted in the image) was not detected");
    }
    o.memory.write32(spare, saved);
    require(reachability_problem(o, sites_copy, loaded_copy, scratch_count).empty(), "the image was not restored after the controls");
    std::printf("PASS the aux callbacks are unreachable: selector modes 1/3/4 register 0x%08X / 0x%08X / 0x%08X (aux A and B, "
                "contexts checked), mode 0 clears, others nothing; its %u callers (0x%08X mode %u, 0x%08X mode %u) load the mode "
                "as a constant; %zu references to the selector, register functions and callbacks in all sections, all inside the "
                "selector or its two call sites; 7 negative controls (extra caller, direct callback call, register call, lis/addi, "
                "data word, other mode) detected\n",
                kStdCallback, kChorusCallback, kDelayCallback, callers, caller_sites[0], loaded[0], caller_sites[1], loaded[1],
                reference_count);
}

// The owners: entering each stops the run with a detail naming the callback; a mutant that returns silently is caught.
struct Entered { bool stopped; bool names_callback; };
Entered enter(Oracle& o, u32 callback, const std::function<void(Cpu&, u32)>& owner) {
    o.cpu.clear_stop();
    o.cpu.gpr[4] = kSoundObject;
    owner(o.cpu, callback);
    char needle[16];
    std::snprintf(needle, sizeof(needle), "0x%08X", callback);
    return {o.cpu.stopped() && o.cpu.stop().reason == StopReason::ExecutionFault,
            o.cpu.stop().detail.find(needle) != std::string::npos};
}

void callbacks_stop(Oracle& o) {
    for (u32 callback : {kStdCallback, kChorusCallback, kDelayCallback}) {
        const Entered e = enter(o, callback, [](Cpu& c, u32 a) { serve_callback_entered(c, a); });
        require(e.stopped && e.names_callback, "a callback owner did not stop the run naming itself");
        // Negative control: an owner that returns silently (the failure this contract exists to prevent) is detected.
        const Entered silent = enter(o, callback, [](Cpu& c, u32) { c.gpr[3] = 0u; });
        require(!silent.stopped, "the negative control (a silent callback owner) was not detected");
    }
    GuestMemory memory;
    Cpu cpu{memory};
    install(cpu);
    for (u32 callback : {kStdCallback, kChorusCallback, kDelayCallback})
        require(cpu.has_hle(callback), "install did not register a callback owner");
    require(!cpu.has_hle(kHiCallback), "install registered ReverbHi's callback (the reverb owner's, not this file's)");
    o.cpu.clear_stop();
    std::puts("PASS the three callback owners stop the run naming the callback; a silent owner is detected; install registers them");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: axfx_effects_contract_tests main.dol");
        Oracle original(argv[1]);
        read_sections(argv[1]);
        results_are_discarded(original);
        callbacks_unreachable(original);
        callbacks_stop(original);
        unsigned total = 0;
        for (const Effect& e : effects()) {
            lifecycle(original, e);
            unsigned cases = 0, accepted = 0;
            const unsigned mismatches = sweep(original, e, {}, cases, accepted);
            require(mismatches == 0u, "a native verdict differs from the original's");
            require(accepted > 0 && accepted < cases, "the sweep did not reach both verdicts");
            // Negative control: an owner that accepts one value beyond a bound (off by one) must be caught.
            unsigned control_cases = 0, control_accepted = 0;
            const Judge off_by_one = [](Oracle& o2, const Effect& e2) {
                const u32 verdict = o2.cpu.gpr[3];
                if (verdict != 0u) return verdict;
                for (std::size_t i = 0; i < e2.live.size(); ++i) {
                    const u32 word = o2.memory.peek32(kState + e2.parameters + u32(i) * 4u);
                    u32 beyond = e2.bounds[i][1] + 1u;
                    if (e2.floats) {
                        float high;
                        std::memcpy(&high, &e2.bounds[i][1], 4);
                        beyond = bits(std::nextafter(high, std::numeric_limits<float>::infinity()));
                    }
                    if (word == beyond) return 1u;
                }
                return verdict;
            };
            require(sweep(original, e, off_by_one, control_cases, control_accepted) > 0u,
                    "the negative control (accepts one value beyond a bound) was not detected");
            std::printf("PASS %s owned init/shutdown: %u parameter sets (%u accepted), return value equal to the original's, "
                        "no byte written; negative control (off-by-one bound) detected\n", e.name, cases, accepted);
            total += cases;
        }
        std::printf("(the original leaks three zero-byte pre-delay blocks in %u parameter sets: documented quirk)\n",
                    g_zero_block_leaks);
        std::printf("PASS axfx effects contract: %u parameter sets, %llu original instructions\n", total,
                    static_cast<unsigned long long>(original.instructions));
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL axfx effects contract: %s\n", error.what());
        return 1;
    }
}
