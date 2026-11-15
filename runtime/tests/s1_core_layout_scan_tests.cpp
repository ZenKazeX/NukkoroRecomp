// liveness-roots: reference-interpreter-only
// Original-DOL contract for the layout manager's one-player scan (gekko/native_s1_core.h, 0x800CAC08): the original instructions
// run in the reference interpreter (with the range test 0x800CA970 served by NativeObjectRange, as in the game); the native
// owner runs on a second machine with the same DOL and the same memory, and when it declines the original body runs there too.
// Random layouts: 0x800 slots, a pool of records (flags, saved flags, position, range byte), the manager's mode word and a point.
// Scenario classes: quiet frames (nothing to create or restore: the owner must SERVE), frames with one to three slots that need
// the factory or the respawn paths (the owner must DECLINE and the original must give the same memory), and fully random flags.
// Compared after every call: the manager, the point, the records, every slot, the stack window around the frames, r1, r2, r13,
// r27..r31 and LR. Negative controls: a wrong count, a missing prologue word, a missing spawn-test frame, a stale count after a
// decline and an owner that serves a spawning frame must each be caught by the same comparison.
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_object_range.h"
#include "gekko/native_s1_core.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace gekko;
constexpr u32 kReturn = 0x817FF000u, kStack = 0x817E0000u, kR2 = 0x805FA780u, kR13 = 0x805EC500u;
constexpr u32 kArena = 0x81400000u;          // manager at +0, point +0x600, factory table +0x700, factory objects +0x800
constexpr u32 kPoint = kArena + 0x600u, kTable = kArena + 0x700u, kFactories = kArena + 0x800u, kStub = kArena + 0x900u;
constexpr u32 kRecords = kArena + 0x1000u, kRecordSize = 0x30u, kRecordCount = 96u, kReserved = 90u;  // 90..95 own the injected slots
constexpr u32 kSlots = 0x81410000u, kSlotCount = 0x800u, kSlotSize = 0x20u;
constexpr u32 kArenaCompare = 0x2400u;
constexpr u32 kStackWindow = 0x100u;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Machine {
    GuestMemory memory;
    Cpu cpu{memory};
    NativeObjectRange range{cpu};
    explicit Machine(const std::vector<u8>& dol) {
        const auto word = [&](unsigned offset) { return load_be32(dol.data() + offset); };
        for (unsigned i = 0; i < 18; ++i) {
            const u32 offset = word(i * 4), address = word(0x48 + i * 4), size = word(0x90 + i * 4);
            require(u64(offset) + size <= dol.size(), "invalid DOL section");
            if (size) memory.write_block(address, dol.data() + offset, size);
        }
        range.install();
    }
    void prepare(u32 entry, const u32 saved[5]) {
        cpu.reset();
        cpu.set_msr(cpu.msr() | kMsrFP);
        cpu.pc = entry; cpu.npc = entry + 4; cpu.lr = kReturn;
        cpu.gpr[1] = kStack; cpu.gpr[2] = kR2; cpu.gpr[13] = kR13;
        for (u32 k = 0; k < 5; ++k) cpu.gpr[27 + k] = saved[k];
        cpu.gpr[3] = kArena; cpu.gpr[4] = kPoint;
    }
    void run_original() {
        unsigned steps = 0;
        while (cpu.pc != kReturn && !cpu.stopped() && steps++ < 2000000) cpu.run_steps(1);
        require(cpu.pc == kReturn && !cpu.stopped() && cpu.gpr[1] == kStack, "original call failed return/stack contract");
    }
};

u32 g_seed = 0xC0DE5EEDu;
u32 rnd() { g_seed = g_seed * 1664525u + 1013904223u; return g_seed >> 8 ^ (g_seed << 16); }
u32 pick(u32 n) { return rnd() % n; }
u32 f2w(float f) { u32 w; std::memcpy(&w, &f, 4); return w; }

void fill(Machine& a, Machine& b, u32 at, u32 word) { a.memory.write32(at, word); b.memory.write32(at, word); }
void fill8(Machine& a, Machine& b, u32 at, u32 v) { a.memory.write8(at, static_cast<u8>(v)); b.memory.write8(at, static_cast<u8>(v)); }

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
    for (u32 off = 0; off < kArenaCompare; off += 4u) if (differ(kArena + off, "arena")) return false;
    for (u32 off = 0; off < kSlotCount * kSlotSize; off += 4u) if (differ(kSlots + off, "slots")) return false;
    for (u32 off = 0; off < kStackWindow + 0x20u; off += 4u) if (differ(kStack - kStackWindow + off, "stack")) return false;
    for (u32 r : {1u, 2u, 13u, 27u, 28u, 29u, 30u, 31u})
        if (o.cpu.gpr[r] != n.cpu.gpr[r]) {
            char t[120];
            std::snprintf(t, sizeof t, "%s r%u: original %08X native %08X", name, r, o.cpu.gpr[r], n.cpu.gpr[r]);
            detail = t;
            return false;
        }
    if (o.cpu.lr != n.cpu.lr) { detail = std::string(name) + ": LR differs"; return false; }
    return true;
}

// ---- a random layout -------------------------------------------------------------------------------------------------------------
enum class Kind { Live, DormantOut, DormantHeld, DormantAnyNoType, Inert, RespawnQuiet, RunsRestricted, DormantIn, Restore40, RestoreB,
                  RespawnOut, Random };

void write_record(Machine& o, Machine& n, u32 record, u32 flags, u32 saved, float x, float y, float z, u32 range, u32 group) {
    fill(o, n, record + 0x00u, f2w(x));
    fill(o, n, record + 0x04u, f2w(y));
    fill(o, n, record + 0x08u, f2w(z));
    fill(o, n, record + 0x18u, flags);
    fill(o, n, record + 0x1Cu, saved);
    fill8(o, n, record + 0x22u, group);
    fill8(o, n, record + 0x23u, range);
}

const float kPx = 120.0f, kPy = -40.0f, kPz = 700.0f;

// A record of the given kind. `range` is the authored range byte; near/far is relative to the point.
void make_record(Machine& o, Machine& n, u32 record, Kind kind) {
    const u32 range = pick(32);
    const float reach = 100.0f * float(range);
    const float far_off = reach + 5.0f + float(pick(900)), near_off = reach * (float(pick(1000)) / 1100.0f);
    const auto at = [&](float d, float& x, float& y, float& z) {
        const float a = float(pick(360)) * 0.017453292f;
        x = kPx + d * std::cos(a); y = kPy; z = kPz + d * std::sin(a);
    };
    float x, y, z;
    u32 flags = 0, saved = rnd();
    const u32 noise = rnd() & 0x7F9F0000u & ~0x20000u & ~0x800u & ~0x80000u & ~0x10000u & ~0x800000u;  // bits that do not matter
    switch (kind) {
        case Kind::Live: flags = 2u | (noise & ~1u); at(pick(2) ? far_off : near_off, x, y, z); break;
        case Kind::DormantOut: flags = 1u | (noise & ~2u & ~0x40u); at(far_off, x, y, z); break;
        case Kind::DormantHeld: flags = 1u | 0x20000u | (noise & ~2u); at(pick(2) ? far_off : near_off, x, y, z); break;
        case Kind::DormantAnyNoType: flags = 1u | 0x200000u | (pick(2) ? 0x800u : 0u) | (noise & ~2u); at(pick(2) ? far_off : near_off, x, y, z); break;
        case Kind::Inert: flags = noise & ~3u & ~0x40u; saved &= ~1u; at(near_off, x, y, z); break;
        case Kind::RespawnQuiet: flags = (noise & ~3u & ~0x40u); saved |= 1u; if (pick(2)) { flags |= 0x10000u; at(near_off, x, y, z); } else at(far_off, x, y, z); break;
        case Kind::RunsRestricted: flags = 0x8000u | (pick(2) ? 2u : 1u) | (noise & ~3u); at(far_off, x, y, z); break;
        case Kind::DormantIn: flags = 1u | (pick(3) == 0 ? 0x800u : 0u) | (noise & ~2u & ~0x20000u); at(near_off, x, y, z); break;
        case Kind::Restore40: flags = 0x40u | (pick(2) ? 0x208u : 0u) | (noise & ~3u); saved |= 1u; at(far_off, x, y, z); break;
        case Kind::RestoreB: flags = 0x800000u | (noise & ~3u & ~0x40u); saved |= 1u; at(far_off, x, y, z); break;
        case Kind::RespawnOut: flags = 0x10000u | (noise & ~3u & ~0x40u); saved |= 1u; at(far_off, x, y, z); break;
        case Kind::Random: flags = rnd() & (pick(2) ? 0x8FFFFu : 0xFFFFFFFFu); x = float(int(pick(4000)) - 2000); y = float(int(pick(400)) - 200); z = float(int(pick(4000)) - 2000); break;
    }
    write_record(o, n, record, flags, saved, x, y, z, range, pick(256));
}

enum class Scenario { Quiet, Injected, Random };

// Builds one random manager/point/slots/records on both machines; returns whether the frame needs the original's other paths.
void build(Machine& o, Machine& n, Scenario scenario, bool& restricted) {
    for (u32 off = 0; off < kArenaCompare; off += 4u) fill(o, n, kArena + off, 0xA5A50000u | (off & 0xFFFFu));  // distinct sentinels
    const bool table = pick(16) != 0;
    restricted = pick(5) < 2;
    fill(o, n, kArena + 0x08u, rnd());                       // stale count
    fill(o, n, kArena + 0x14u, table ? kTable : 0u);
    fill(o, n, kArena + 0x18u, kSlots);
    fill(o, n, kArena + 0x428u, (rnd() & ~1u) | (restricted ? 1u : 0u));
    fill(o, n, kPoint + 0u, f2w(kPx)); fill(o, n, kPoint + 4u, f2w(kPy)); fill(o, n, kPoint + 8u, f2w(kPz));
    for (u32 f = 0; f < 4; ++f) {  // the factories: object +0xC is the create function (a blr)
        fill(o, n, kTable + 4u * f, kFactories + 0x20u * f);
        fill(o, n, kFactories + 0x20u * f + 0xCu, kStub);
    }
    fill(o, n, kStub, 0x4E800020u);  // blr
    static const Kind quiet_kinds[] = {Kind::Live, Kind::DormantOut, Kind::DormantHeld, Kind::DormantAnyNoType, Kind::Inert,
                                       Kind::RespawnQuiet, Kind::RunsRestricted};
    for (u32 i = 0; i < kReserved; ++i) {
        const Kind kind = scenario == Scenario::Random ? Kind::Random : quiet_kinds[pick(sizeof quiet_kinds / sizeof quiet_kinds[0])];
        make_record(o, n, kRecords + kRecordSize * i, kind);
    }
    for (u32 i = 0; i < kSlotCount; ++i) {
        const u32 slot = kSlots + kSlotSize * i;
        const u32 record = kRecords + kRecordSize * pick(kReserved);
        fill(o, n, slot + 0x00u, record);
        fill(o, n, slot + 0x04u, rnd()); fill(o, n, slot + 0x08u, rnd()); fill(o, n, slot + 0x0Cu, rnd());
        // about a fifth of the slots have no type (a slot property)
        fill(o, n, slot + 0x10u, pick(5) == 0 ? 0xFFFFFFFFu : pick(4));
        fill(o, n, slot + 0x14u, rnd()); fill(o, n, slot + 0x18u, rnd()); fill(o, n, slot + 0x1Cu, rnd());
    }
    // The "any" records (marker 0x200000) are dormant and may be in range: a Quiet frame gives their slots no type.
    if (scenario == Scenario::Quiet) {
        for (u32 i = 0; i < kSlotCount; ++i) {
            const u32 slot = kSlots + kSlotSize * i;
            if ((o.memory.peek32(o.memory.peek32(slot) + 0x18u) & 0x200000u) != 0u) fill(o, n, slot + 0x10u, 0xFFFFFFFFu);
        }
    } else if (scenario == Scenario::Injected) {
        const u32 count = 1 + pick(3);
        static const Kind eventful[] = {Kind::DormantIn, Kind::Restore40, Kind::RestoreB, Kind::RespawnOut};
        for (u32 k = 0; k < count; ++k) {
            const u32 record = kRecords + kRecordSize * (kReserved + k);
            const Kind kind = eventful[pick(4)];
            make_record(o, n, record, kind);
            const u32 slot = kSlots + kSlotSize * pick(kSlotCount);
            fill(o, n, slot, record);
            fill(o, n, slot + 0x10u, kind == Kind::DormantIn ? pick(4) : (pick(2) ? pick(4) : 0xFFFFFFFFu));
        }
    }
}

std::vector<u8> read_file(const char* path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<u8>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Runs `frames` frames of a scenario with `owner`; returns the number served. With catch_only, mismatches are counted, not failures.
struct Tally { unsigned served = 0, declined = 0, caught = 0; };
Tally run_class(Machine& o, Machine& n, const char* label, Scenario scenario, unsigned frames,
                const std::function<bool(Cpu&)>& owner, bool catch_only) {
    Tally t;
    for (unsigned i = 0; i < frames; ++i) {
        bool restricted;
        build(o, n, scenario, restricted);
        u32 saved[5];
        for (u32 k = 0; k < 5; ++k) saved[k] = rnd();
        o.prepare(s1_core::kLayoutScan, saved);
        n.prepare(s1_core::kLayoutScan, saved);
        o.run_original();
        const bool served = owner(n.cpu);
        if (served) n.cpu.pc = n.cpu.lr; else n.run_original();
        served ? ++t.served : ++t.declined;
        ++g_checked;
        std::string detail;
        if (!same_state(o, n, label, detail)) {
            if (catch_only) ++t.caught; else report(label, detail);
        }
    }
    return t;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: s1_core_layout_scan_tests main.dol");
        const std::vector<u8> dol = read_file(argv[1]);
        require(dol.size() >= 256, "missing DOL");
        u64 hash = 0xCBF29CE484222325ull;
        for (u8 byte : dol) hash = (hash ^ byte) * 0x100000001B3ull;
        require(hash == 0xD2CFB4A1CA897F9Bull, "unexpected original DOL identity");
        Machine original(dol), native(dol);
        s1_core::set_enabled(true);  // the owner is off by default in the library
        const auto real = [](Cpu& c) { return s1_core::serve_layout_scan(c); };

        const Tally quiet = run_class(original, native, "quiet", Scenario::Quiet, 2500, real, false);
        std::printf("quiet frames: %u served, %u declined\n", quiet.served, quiet.declined);
        require(quiet.served * 10u >= 2500u * 9u, "quiet frames were declined too often");
        const Tally injected = run_class(original, native, "injected", Scenario::Injected, 2500, real, false);
        std::printf("injected frames: %u served, %u declined\n", injected.served, injected.declined);
        require(injected.declined > 0, "no injected frame was declined");
        const Tally random = run_class(original, native, "random", Scenario::Random, 1500, real, false);
        std::printf("random frames: %u served, %u declined\n", random.served, random.declined);

        // Directed: a null record is the original's data fault, never served.
        {
            bool restricted;
            build(original, native, Scenario::Quiet, restricted);
            fill(original, native, kSlots + kSlotSize * 17u, 0u);
            u32 saved[5] = {1, 2, 3, 4, 5};
            native.prepare(s1_core::kLayoutScan, saved);
            native.memory.write32(kStack - 0x20u, 0u);
            require(!s1_core::serve_layout_scan(native.cpu), "a null record was served");
            require(native.memory.peek32(kStack - 0x20u) == 0u, "a declined call wrote its frame");
        }

        // Negative controls: the same comparison must catch a wrong owner.
        unsigned c_count = 0, c_prologue = 0, c_spawnframe = 0, c_serve_spawn = 0, c_reg = 0;
        c_count = run_class(original, native, "ctl count", Scenario::Quiet, 300, [](Cpu& c) {
            const bool ok = s1_core::serve_layout_scan(c);
            if (ok && c.mem_read32(c.gpr[3] + 0x14u) != 0u) c.mem_write32(c.gpr[3] + 8u, c.mem_read32(c.gpr[3] + 8u) + 1u);
            return ok; }, true).caught;
        require(c_count > 0, "negative control: a wrong live count was not caught");
        c_prologue = run_class(original, native, "ctl prologue", Scenario::Quiet, 300, [](Cpu& c) {
            const bool ok = s1_core::serve_layout_scan(c);
            if (ok) c.mem_write32(c.gpr[1] - 0x14u, 0u);
            return ok; }, true).caught;
        require(c_prologue > 0, "negative control: a missing prologue word was not caught");
        c_spawnframe = run_class(original, native, "ctl spawn frame", Scenario::Quiet, 300, [](Cpu& c) {
            const bool ok = s1_core::serve_layout_scan(c);
            if (ok) c.mem_write32(c.gpr[1] - 0x1Cu, 0u);
            return ok; }, true).caught;
        require(c_spawnframe > 0, "negative control: a missing spawn-test frame word was not caught");
        c_serve_spawn = run_class(original, native, "ctl serve spawn", Scenario::Injected, 600, [](Cpu& c) {
            const bool ok = s1_core::serve_layout_scan(c);
            return ok ? true : true; }, true).caught;
        require(c_serve_spawn > 0, "negative control: serving a spawning frame was not caught");
        c_reg = run_class(original, native, "ctl register", Scenario::Quiet, 100, [](Cpu& c) {
            const bool ok = s1_core::serve_layout_scan(c);
            if (ok) c.gpr[28] += 1u;
            return ok; }, true).caught;
        require(c_reg > 0, "negative control: a clobbered non-volatile register was not caught");

        if (g_failures != 0) {
            std::fprintf(stderr, "FAIL s1 core layout scan contract: %d mismatches of %d frames\n", g_failures, g_checked);
            return 1;
        }
        std::printf("PASS s1 core layout scan contract: %d frames compared (controls caught %u + %u + %u + %u + %u)\n", g_checked,
                    c_count, c_prologue, c_spawnframe, c_serve_spawn, c_reg);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL s1 core layout scan contract: %s\n", error.what());
        return 1;
    }
}
