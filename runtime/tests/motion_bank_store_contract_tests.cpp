// liveness-roots: reference-interpreter-only
// Contract for the native motion bank store (gekko/native_motion_bank_store.h, SubQ batch B003): the ORIGINAL words of the
// 15 store functions run in the reference interpreter from the pinned DOL on machine O; the native store serves the same
// entries on machine N. Both start from byte-identical memory and registers and get the same call sequence.
//
// Every guest function the store reaches is a recording stand-in on BOTH machines (an HLE hook): the allocator 0x80412184
// (RwCalloc semantics: zeroed blocks; now and then out of memory; freed blocks reused), the free 0x8041214C (scribbles the
// freed block), the pool allocator 0x804223B8 / free 0x80422350 (pool addresses in the native arena; out of memory now and
// then, always above 1 MiB), the handle maker 0x80411710, two pack destructors, the slot binder 0x8041C1C0. Each stand-in
// logs its target, r3..r6, LR, r1 and a hash of r14..r31, and writes a callee frame below r1 (back chain, link word, r20..r31)
// so the stack residue the store leaves is compared too. The clip transfers 0x804222C0 / 0x80422234 are the production
// NativeMotionStore on both machines (owners on O, direct calls on N). The helpers the original calls by address and the
// native store does itself (strlen, strcmp, strcpy, memcpy, memset, fabs, the 3- and 12-word copies) run as the DOL's words
// on O.
//
// Rounds start empty (packs registered through 0x804140A8, which builds pool banks through 0x80414588) or from a registry
// laid out in guest memory (pool and resident banks with loaded nodes, odd use counts, held ids, cursors), which N imports
// (MotionBankStore::import_from_guest, the checkpoint path). Inside a round N also publishes and re-imports its state now
// and then. Operations: register (with an id, auto ids, overlapping ranges, a pack already registered, owning packs of 1 and
// more clips), unregister, drop, acquire, release, data_of, handle_of, frame_range, name_of, id_of_name, track, query,
// next_record, bind; ids inside and outside the ranges, with high bits, 0; probes with negative, NaN and infinite times.
// Compared after every call: r1, r2, r13, r14..r31, LR, FPSCR, r3 for the functions with a result, and the stand-in log;
// at the end of every round (with --every-op after every call): every byte of the packs, the heap, the imported banks, the
// bind objects, the probes and outputs, the small-data window 0x805E0000..0x80600000 (with N's state block published), the
// used arena and 8 KiB of stack below r1.
//
// Negative controls (the run must FAIL): --control-quirk (the original's build frees the header it leaks for an owning pack
// of more than one clip), --control-order (the original's acquire makes a new node the list's head instead of its tail).
// usage: motion_bank_store_contract_tests <sys/main.dol> [--control-...] [--rounds N] [--every-op]
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_motion_bank_store.h"
#include "gekko/native_motion_store.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace gekko;

constexpr u32 kR2 = 0x805FA780u, kR13 = 0x805EC500u;
constexpr u32 kStack = 0x817F0000u, kReturn = 0x81700000u;
constexpr u32 kPacks = 0x81000000u, kPackStride = 0x4000u, kPackCount = 24u;
constexpr u32 kImport = 0x81080000u, kImportStride = 0x2000u;  // import-mode banks (header, table, clips)
constexpr u32 kImportNodes = 0x810E0000u;                      // import-mode nodes, 0x400 apart
constexpr u32 kHeap = 0x81100000u, kHeapEnd = 0x81200000u;
constexpr u32 kObjects = 0x81300000u, kSlots = 0x81310000u;
constexpr u32 kProbes = 0x81340000u, kOut = 0x81341000u, kKindOut = 0x81342000u, kLookupNames = 0x81343000u;
// The pool's handle space: native storage of NativeMotionStore (2026-11-21), written and read here only through
// NativeMotionStore::pool_bytes; any access to it through the window fails the run (Machine::retired).
constexpr u32 kArena = NativeMotionStore::kPoolBase, kImportPool = kArena, kPoolStart = kArena + 0x40000u;
bool in_pool(u32 a) { return a - kArena < NativeMotionStore::kPoolReserve; }
constexpr u32 kSmallData = 0x805E0000u, kSmallDataEnd = 0x80600000u;
constexpr u32 kStateBlock = 0x805F2458u;

constexpr u32 kAlloc = 0x80412184u, kFree = 0x8041214Cu, kPoolAlloc = 0x804223B8u, kPoolFree = 0x80422350u;
constexpr u32 kHandleFor = 0x80411710u, kBindChannels = 0x8041C1C0u, kDestroyA = 0x80300000u, kDestroyB = 0x80300100u;

constexpr u32 kDrop = 0x80412764u, kNext = 0x80412868u, kQuery = 0x80412E3Cu, kHandleOf = 0x8041342Cu, kTrack = 0x8041352Cu;
constexpr u32 kFrameRange = 0x80413644u, kIdOfName = 0x80413788u, kNameOf = 0x80413870u, kDataOf = 0x80413970u;
constexpr u32 kUnregister = 0x80413A24u, kRegister = 0x804140A8u, kRelease = 0x80414290u, kAcquire = 0x80414368u;
constexpr u32 kBuild = 0x80414588u, kBind = 0x8041ADACu;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
std::string hex(u32 v) {
    char b[16];
    std::snprintf(b, sizeof(b), "%08X", v);
    return b;
}

struct Rng {
    u32 s;
    explicit Rng(u32 seed = 1u) : s(seed * 2654435761u + 0x9E3779B9u) {}
    u32 next() {
        s = s * 1664525u + 1013904223u;
        return (s >> 8) ^ (s << 21);
    }
    u32 below(u32 n) { return n == 0u ? 0u : next() % n; }
};

struct Call {
    u32 target, r3, r4, r5, r6, lr, sp;
    u64 regs;
    bool operator==(const Call&) const = default;
};

// The stand-ins' state: one per machine, the same seed on both, so equal call sequences give equal answers.
struct Stand {
    Rng rng;
    u32 heap_top = kHeap;
    std::map<u32, std::vector<u32>> free_blocks;  // rounded size -> freed blocks (reused last first)
    std::map<u32, u32> live;                      // block -> rounded size
    u32 pool_top = kPoolStart;
    u32 handles = 0;
    bool no_oom = false;  // id_of_name: a pool clip that cannot load gives strcmp a null name (a fault, as in the game)
    std::vector<Call> log;
};

u32 round32(u32 x) { return (x + 31u) & ~31u; }

struct Machine {
    GuestMemory memory;
    Cpu cpu{memory};
    NativeMotionStore pool{cpu, memory};
    std::unique_ptr<MotionBankStore> store;
    Stand stand;
    u32 retired = 0;  // the first window access to the pool's handle space (address | write), 0 while none

    Machine(const std::vector<u8>& dol, bool native) {
        const auto word = [&](std::size_t o) {
            return u32{dol[o]} << 24 | u32{dol[o + 1]} << 16 | u32{dol[o + 2]} << 8 | u32{dol[o + 3]};
        };
        for (u32 i = 0; i < 18u; ++i) {
            const u32 offset = word(i * 4u), address = word(0x48u + i * 4u), size = word(0x90u + i * 4u);
            if (size == 0u) continue;
            require(std::uint64_t(offset) + size <= dol.size(), "invalid DOL extent");
            memory.write_block(address, dol.data() + offset, size);
        }
        require(pool.commit_through(kArena + 0x400000u), "the pool's storage could not be reserved");
        // An access to the pool's handle space through the window (a translated / original dereference of a pool
        // address, or a host one) is a contract failure, never a read of 0.
        memory.set_retired_access_stop(
            [](void* context, u32 address, u32, bool write, u64) {
                Machine& m = *static_cast<Machine*>(context);
                if (m.retired == 0u) m.retired = address | (write ? 1u : 0u);
            },
            this);
        cpu.reset();
        pool.install();  // the transfers 0x804222C0 / 0x80422234 (the original's callees)
        const auto log = [this](Cpu& c, u32 target) {
            u64 regs = 0xCBF29CE484222325ull;
            for (u32 k = 14; k < 32u; ++k) regs = (regs ^ c.gpr[k]) * 0x100000001B3ull;
            // the argument registers the callee takes (the rest are volatile leftovers no callee reads)
            const u32 arity = target == kAlloc ? 2u : target == kBindChannels ? 3u : 1u;
            stand.log.push_back(Call{target, c.gpr[3], arity > 1u ? c.gpr[4] : 0u, arity > 2u ? c.gpr[5] : 0u, 0u, c.lr,
                                     c.gpr[1], regs});
            // a callee's frame: back chain, its link word in the caller's frame, r20..r31
            const u32 sp = c.gpr[1];
            c.mem_write32(sp - 0x40u, sp);
            c.mem_write32(sp + 4u, c.lr);
            for (u32 k = 20; k < 32u; ++k) c.mem_write32(sp - 0x40u + 16u + 4u * (k - 20u), c.gpr[k]);
        };
        cpu.install_hle(kAlloc, [this, log](Cpu& c) {
            log(c, kAlloc);
            const u32 bytes = c.gpr[3] * c.gpr[4];
            u32 block = 0u;
            const bool fails = stand.rng.below(14) == 0u && !stand.no_oom;
            if (!fails && bytes < 0x40000u) {
                const u32 size = round32(bytes);
                auto& reuse = stand.free_blocks[size];
                if (!reuse.empty() && stand.rng.below(2) == 0u) {
                    block = reuse.back();
                    reuse.pop_back();
                } else if (stand.heap_top + size + 0x80u < kHeapEnd) {
                    block = round32(stand.heap_top) + (stand.rng.below(6) == 0u ? 4u * stand.rng.below(8) : 0u);
                    stand.heap_top = block + size + 0x40u;
                }
                if (block != 0u) {
                    stand.live[block] = size;
                    for (u32 i = 0; i < bytes; ++i) c.mem_write8(block + i, 0u);
                }
            }
            c.gpr[3] = block;
            return true;
        }, "test alloc");
        cpu.install_hle(kFree, [this, log](Cpu& c) {
            log(c, kFree);
            const u32 block = c.gpr[3];
            if (auto it = stand.live.find(block); it != stand.live.end()) {
                const u32 size = it->second;
                stand.live.erase(it);
                stand.free_blocks[size].push_back(block);
                for (u32 i = 0; i + 4u <= size && i < 0x40u; i += 4u) c.mem_write32(block + i, 0xDEAD0000u | i);
            }
            c.gpr[3] = stand.rng.next();
            return true;
        }, "test free");
        cpu.install_hle(kPoolAlloc, [this, log](Cpu& c) {
            log(c, kPoolAlloc);
            const u32 bytes = c.gpr[3];
            u32 at = 0u;
            if (stand.rng.below(12) != 0u && bytes < 0x100000u && stand.pool_top + bytes < kArena + 0x3F0000u) {
                at = stand.pool_top;
                stand.pool_top = round32(at + bytes) + 0x20u;
            }
            c.gpr[3] = at;
            return true;
        }, "test pool alloc");
        for (const u32 target : {kPoolFree, kDestroyA, kDestroyB}) {
            cpu.install_hle(target, [this, log, target](Cpu& c) {
                log(c, target);
                c.gpr[3] = stand.rng.next();
                return true;
            }, "test log only");
        }
        cpu.install_hle(kHandleFor, [this, log](Cpu& c) {
            log(c, kHandleFor);
            c.gpr[3] = (c.gpr[3] * 2654435761u) ^ (++stand.handles << 20);
            return true;
        }, "test handle");
        cpu.install_hle(kBindChannels, [this, log](Cpu& c) {
            log(c, kBindChannels);
            c.gpr[3] = stand.rng.below(4) == 0u ? 0u : 1u;
            return true;
        }, "test bind channels");
        if (native) {
            store = std::make_unique<MotionBankStore>(cpu, memory, pool);
            store->install();
        }
    }

    void prepare(const u32* nonvolatile) {
        cpu.gpr[1] = kStack;
        cpu.gpr[2] = kR2;
        cpu.gpr[13] = kR13;
        for (u32 k = 14; k < 32u; ++k) cpu.gpr[k] = nonvolatile[k - 14u];
        cpu.set_msr(cpu.msr() | kMsrDR | kMsrIR | kMsrFP);
    }
    u32 call(u32 entry, std::initializer_list<u32> args) {
        u32 r = 3;
        for (const u32 a : args) cpu.gpr[r++] = a;
        cpu.lr = kReturn;
        cpu.pc = entry;
        cpu.npc = entry + 4u;
        for (unsigned long long steps = 0; cpu.pc != kReturn && !cpu.stopped(); ++steps) {
            require(steps < 20'000'000ull, "a call to " + hex(entry) + " did not return");
            cpu.run_steps(1u);
        }
        require(!cpu.stopped(), "the machine stopped in " + hex(entry) + ": " + cpu.stop().detail);
        require(retired == 0u, "a window access to the pool's handle space at " + hex(retired & ~1u) +
                                   ((retired & 1u) != 0u ? " (store)" : " (load)") + " in " + hex(entry));
        return cpu.gpr[3];
    }

    // The test's own reads and writes: a pool address through the pool's native API, anything else through memory.
    u8 byte_at(u32 a) {
        if (!in_pool(a)) return memory.read8(a);
        const u8* p = pool.pool_bytes(a, 1u);
        require(p != nullptr, "a pool byte outside the committed pool at " + hex(a));
        return *p;
    }
    void put(u32 a, u32 v, u32 width) {
        for (u32 i = 0; i < width; ++i) {
            const u8 b = static_cast<u8>(v >> (8u * (width - 1u - i)));
            if (!in_pool(a + i)) {
                memory.write8(a + i, b);
                continue;
            }
            u8* p = pool.pool_bytes(a + i, 1u);
            require(p != nullptr, "a pool byte outside the committed pool at " + hex(a + i));
            *p = b;
        }
    }
    u32 word_at(u32 a) {
        return u32{byte_at(a)} << 24 | u32{byte_at(a + 1u)} << 16 | u32{byte_at(a + 2u)} << 8 | byte_at(a + 3u);
    }

    u64 hash_range(u32 at, u32 bytes) {
        std::vector<u8> block(bytes);
        if (in_pool(at)) {
            const u8* p = pool.pool_bytes(at, bytes);
            require(p != nullptr || bytes == 0u, "a pool range outside the committed pool at " + hex(at));
            if (bytes != 0u) std::memcpy(block.data(), p, bytes);
        } else {
            memory.read_block(at, block.data(), block.size());
        }
        u64 h = 0xCBF29CE484222325ull ^ at;
        for (u8 b : block) h = (h ^ b) * 0x100000001B3ull;
        return h;
    }
};

// ---- the worlds -----------------------------------------------------------------------------------------------------
// Both machines get every byte; `put` writes on both.
struct Pair {
    Machine& o;
    Machine& n;
    void w8(u32 a, u32 v) { o.put(a, v, 1u); n.put(a, v, 1u); }
    void w16(u32 a, u32 v) { o.put(a, v, 2u); n.put(a, v, 2u); }
    void w32(u32 a, u32 v) { o.put(a, v, 4u); n.put(a, v, 4u); }
    void string(u32 a, const char* s) {
        for (u32 i = 0;; ++i) {
            w8(a + i, static_cast<u8>(s[i]));
            if (s[i] == 0) break;
        }
    }
    void clear(u32 a, u32 bytes) {
        for (u32 i = 0; i < bytes; i += 4u) w32(a + i, 0u);
    }
};

const char* const kNames[] = {"run", "walk", "idle", "jump_a", "jump_b", "spin_dash_long_name", "x", "", "run"};

u32 float_word(Rng& g) {
    static const u32 special[] = {0x00000000u, 0x80000000u, 0x3F800000u, 0xBF800000u, 0x7FC00000u, 0x7F800001u,
                                  0xFF800000u, 0x7F800000u, 0x41F00000u, 0x42700000u, 0x00000001u, 0x3727C5ACu};
    switch (g.below(6)) {
        case 0: return special[g.below(sizeof(special) / sizeof(special[0]))];
        case 1: return (g.next() & 0x80000000u) | (0x3C000000u + (g.next() & 0x03FFFFFFu));  // small
        default: {
            const float f = static_cast<float>(g.below(4000)) / 50.0f - 20.0f;  // -20 .. 60
            return float_to_bits(f);
        }
    }
}

// An event chain at `at` (inside [at, limit)): records of kind 1..3 with mode bits, a length at least the header the
// kind's mode needs, times; then the 0xFFFF end. Returns the bytes used.
u32 make_events(Pair& p, Rng& g, u32 at, u32 limit) {
    u32 q = at;
    const u32 records = g.below(6);
    for (u32 k = 0; k < records; ++k) {
        const u32 mode = g.below(3);  // 0 plain, 1 0x8000, 2 0x4000
        const u32 kind = (1u + g.below(3)) | (mode == 1u ? 0x8000u : mode == 2u ? 0x4000u : 0u);
        const u32 header = mode == 1u ? 12u : mode == 2u ? 8u : 4u;
        const u32 length = header + 4u * g.below(4);
        if (q + length + 8u > limit) break;
        p.w16(q, kind);
        p.w16(q + 2u, length);
        for (u32 i = 4u; i < length; i += 4u) p.w32(q + i, i <= 8u ? float_word(g) : g.next());
        q += length;
    }
    p.w16(q, 1u);
    p.w16(q + 2u, 0xFFFFu);
    return q + 4u - at;
}
// A clip data block at `at`: +4 handle, +8 its byte count, +0x10 / +0x12 frames, +0x34 a chain of track records.
u32 make_data(Pair& p, Rng& g, u32 at) {
    static const u32 frames[] = {0u, 1u, 30u, 0xFFFFu, 0x8000u, 0x7FFFu, 12345u};
    const u32 bytes = 0x60u + 4u * g.below(0x40);
    p.w32(at + 4u, g.next());
    p.w32(at + 8u, bytes);
    p.w16(at + 0x10u, frames[g.below(7)]);
    p.w16(at + 0x12u, frames[g.below(7)]);
    u32 t = at + 0x34u;
    const u32 tracks = 1u + g.below(3);
    for (u32 k = 0; k < tracks; ++k) {
        const u32 step = 8u + 4u * g.below(3);
        p.w32(t, step);
        p.w8(t + 7u, k + 1u < tracks ? (g.below(2) == 0u ? 1u : 0x80u) : 0u);
        t += step;
    }
    return bytes;
}

// A loaded pack (pack i of the round): header, table, names, clip data and events inside its 0x4000 bytes.
void make_pack(Pair& p, Rng& g, u32 i, u32 first, u32 count, u32 owns) {
    const u32 pack = kPacks + kPackStride * i;
    p.clear(pack, kPackStride);
    p.w16(pack + 0x0u, first);
    p.w16(pack + 0x2u, count);
    p.w8(pack + 0x4u, owns);
    p.w8(pack + 0x5u, g.below(2));
    p.w8(pack + 0x6u, 0u);
    p.w8(pack + 0x7u, g.below(256));
    static const u32 destroy[] = {0u, kDestroyA, kDestroyB};
    p.w32(pack + 0x8u, destroy[g.below(3)]);
    p.w32(pack + 0xCu, 0u);
    const u32 table = pack + 0x40u;
    p.w32(pack + 0x10u, table);
    u32 name_at = pack + 0x200u, data_at = pack + 0x600u, event_at = pack + 0x2E00u;
    // Every clip has a name and data: a null one makes the original read address 0 (strcmp, the build's data size, the
    // track cursor), a fault in the game as here.
    for (u32 k = 0; k < count && k < 16u; ++k) {
        const u32 e = table + 12u * k;
        p.string(name_at, kNames[g.below(9)]);
        p.w32(e, name_at);
        name_at += 0x20u + 4u * g.below(2) + (g.below(5) == 0u ? 1u : 0u);
        p.w32(e + 4u, data_at);
        data_at += make_data(p, g, data_at) + 0x10u + 4u * g.below(3);
        if (g.below(3) == 0u) p.w32(e + 8u, 0u);
        else {
            p.w32(e + 8u, event_at);
            event_at += make_events(p, g, event_at, pack + kPackStride - 0x10u) + 4u * g.below(3);
        }
    }
}

// A registry laid out in guest memory (import mode), as a checkpoint would hold it.
void make_registry(Pair& p, Rng& g) {
    const u32 banks = 1u + g.below(4);
    u32 start = 1u + g.below(6);
    u32 previous = 0u, head = 0u;
    u32 node_at = kImportNodes;
    for (u32 b = 0; b < banks; ++b) {
        const u32 bank = kImport + kImportStride * b;
        p.clear(bank, kImportStride);
        const u32 count = 1u + g.below(8);
        const bool pooled = g.below(2) == 0u;
        p.w16(bank + 0x0u, start);
        p.w16(bank + 0x2u, count);
        p.w8(bank + 0x4u, 0u);
        p.w8(bank + 0x6u, pooled ? 1u : 0u);
        p.w32(bank + 0x8u, pooled ? kFree : (g.below(2) == 0u ? kDestroyA : 0u));
        if (pooled) {
            const u32 pool = kImportPool + 0x10000u * b;
            p.w32(bank + 0x14u, pool);
            for (u32 k = 0; k < count; ++k) {
                const u32 r = bank + 0x1Cu + 24u * k, base = pool + 0x800u * k;
                static const u32 skew[] = {0u, 1u, 31u, 16u, 4u};
                const u32 name = base + skew[g.below(5)], data = base + 0x40u + skew[g.below(5)];
                p.string(name, kNames[g.below(9)]);
                p.w32(r + 0x0u, name);
                p.w32(r + 0x4u, 0x20u);
                const u32 bytes = make_data(p, g, data);
                p.w32(r + 0x8u, data);
                p.w32(r + 0xCu, bytes);
                if (g.below(3) != 0u) {
                    const u32 events = base + 0x400u + skew[g.below(5)];
                    p.w32(r + 0x10u, events);
                    p.w32(r + 0x14u, make_events(p, g, events, base + 0x7F0u));
                } else {
                    p.w32(r + 0x10u, 0u);
                    p.w32(r + 0x14u, 0u);
                }
            }
            // loaded nodes (copies of their clips), now and then odd use counts or an id outside the range
            u32 last = 0u;
            const u32 loaded = g.below(4);
            for (u32 k = 0; k < loaded; ++k) {
                const u32 n = node_at;
                node_at += 0x400u;
                p.clear(n, 0x400u);
                const u32 id = start + g.below(count + (g.below(6) == 0u ? 1u : 0u));
                static const u32 uses[] = {1u, 1u, 2u, 3u, 0u, 0xFFFFu, 0x7FFFu, 0x8000u};
                p.w16(n + 0x0u, id);
                p.w16(n + 0x2u, uses[g.below(8)]);
                p.string(n + 0x20u, kNames[g.below(9)]);
                p.w32(n + 0x4u, n + 0x20u);
                make_data(p, g, n + 0x40u);
                p.w32(n + 0x8u, n + 0x40u);
                if (g.below(3) != 0u) {
                    make_events(p, g, n + 0x280u, n + 0x3F0u);
                    p.w32(n + 0xCu, n + 0x280u);
                }
                p.w32(n + 0x10u, 0u);
                if (last != 0u) p.w32(last + 0x10u, n);
                else p.w32(bank + 0x18u, n);
                last = n;
            }
        } else {
            const u32 table = bank + 0x40u;
            p.w32(bank + 0x10u, table);
            u32 at = bank + 0x200u;
            for (u32 k = 0; k < count; ++k) {
                p.string(at, kNames[g.below(9)]);
                p.w32(table + 12u * k, at);
                at += 0x20u;
                p.w32(table + 12u * k + 4u, at);
                at += make_data(p, g, at) + 0x10u;
                if (g.below(2) == 0u) {
                    p.w32(table + 12u * k + 8u, at);
                    at += make_events(p, g, at, bank + kImportStride - 0x10u) + 0x10u;
                }
            }
        }
        if (previous != 0u) p.w32(previous + 0xCu, bank);
        else head = bank;
        previous = bank;
        start += g.below(5) == 0u ? g.below(count + 1u) : count + g.below(4);  // overlapping ranges now and then
    }
    p.w32(kStateBlock + 0x0u, head);
    p.w32(kStateBlock + 0x4u, 0u);
    for (u32 k = 0; k < 4u; ++k) p.w16(kStateBlock + 0x8u + 2u * k, g.below(3) == 0u ? 0u : 1u + g.below(20));
    p.w32(kStateBlock + 0x10u, 0u);
    p.w8(kStateBlock + 0x14u, g.below(2));
    p.w32(kStateBlock + 0x18u, 0u);
    p.w32(kStateBlock + 0x1Cu, 0u);
}

void make_probes(Pair& p, Rng& g) {
    for (u32 k = 0; k < 16u; ++k) {
        const u32 at = kProbes + 0x20u * k;
        for (u32 i = 0; i < 16u; i += 4u) p.w32(at + i, float_word(g));
    }
    p.clear(kOut, 0x1000u);
    p.clear(kKindOut, 0x100u);
    for (u32 k = 0; k < 9u; ++k) p.string(kLookupNames + 0x40u * k, kNames[k]);
}
void make_objects(Pair& p, Rng& g) {
    for (u32 o = 0; o < 4u; ++o) {
        const u32 object = kObjects + 0x200u * o, slots = kSlots + 0x800u * o;
        p.clear(object, 0x200u);
        p.clear(slots, 0x800u);
        p.w32(object + 0x10u, slots);
        for (u32 i = 0x90u; i < 0x110u; i += 4u) p.w32(object + i, g.next());
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc >= 2, "usage: motion_bank_store_contract_tests main.dol [--control-...] [--rounds N] [--every-op]");
        std::string control;
        unsigned rounds = 12000;
        bool every_op = false;
        for (int i = 2; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--rounds" && i + 1 < argc) rounds = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10));
            else if (a == "--every-op") every_op = true;
            else control = a;
        }
        std::ifstream input(argv[1], std::ios::binary);
        const std::vector<u8> dol{std::istreambuf_iterator<char>(input), {}};
        require(dol.size() >= 256, "missing DOL");
        u64 identity = 0xCBF29CE484222325ull;
        for (u8 byte : dol) identity = (identity ^ byte) * 0x100000001B3ull;
        require(identity == 0xD2CFB4A1CA897F9Bull, "unexpected original DOL identity");
        Machine o(dol, false), n(dol, true);
        Pair p{o, n};
        // The store's first words (each framed member's stwu), a pin of the routines the contract is about.
        require(o.memory.read32(kAcquire) == 0x9421FFC0u && o.memory.read32(kBuild) == 0x9421FFD0u &&
                    o.memory.read32(kQuery) == 0x9421FFD0u && o.memory.read32(kBind) == 0x9421FFE0u,
                "unexpected store words");
        if (control == "--control-quirk") {
            // 0x804145DC `mr r3, r26` (return the pack, the header leaks) -> `b 0x804146CC` (free the header first)
            require(o.memory.read32(0x804145DCu) == 0x7F43D378u, "control-quirk word");
            o.memory.write32(0x804145DCu, 0x48000000u | ((0x804146CCu - 0x804145DCu) & 0x03FFFFFCu));
        } else if (control == "--control-order") {
            // 0x80414554 `beq 0x80414560` -> `b 0x80414560`: a new node always becomes the head of its bank's list
            require((o.memory.read32(0x80414554u) >> 26) == 16u, "control-order word");
            o.memory.write32(0x80414554u, 0x48000000u | ((0x80414560u - 0x80414554u) & 0x03FFFFFCu));
        } else {
            require(control.empty(), "unknown control " + control);
        }

        unsigned long long calls = 0, memory_checks = 0, imports = 0, registered = 0, built = 0, refused = 0;
        unsigned long long allocations = 0, frees = 0, query_hits = 0, import_rounds = 0;
        std::map<u32, unsigned long long> per_entry;
        Rng g(77u);
        u32 heap_used = kHeap, pool_used = kPoolStart;
        for (unsigned round = 0; round < rounds; ++round) {
            const u32 seed = 1000u + round;
            heap_used = std::max({heap_used, o.stand.heap_top, n.stand.heap_top});
            pool_used = std::max({pool_used, o.stand.pool_top, n.stand.pool_top});
            o.stand = Stand{Rng(seed)};
            n.stand = Stand{Rng(seed)};
            // fresh memory where the round works
            p.clear(kPacks, kPackStride * kPackCount);
            p.clear(kImport, 0x60000u);
            p.clear(kImportNodes, 0x20000u);
            p.clear(kHeap, heap_used - kHeap + 0x40u);
            p.clear(kObjects, 0x50000u);
            p.clear(kStack - 0x2000u, 0x2100u);
            p.clear(kStateBlock, 0x20u);
            make_probes(p, g);
            make_objects(p, g);
            const bool import_mode = g.below(4) == 0u;
            if (import_mode) {
                make_registry(p, g);
                ++import_rounds;
            }
            require(n.store->import_from_guest(), "the native store refused the round's registry");
            // packs: the round's loader output
            std::vector<u32> pack_state(kPackCount, 0u);  // 0 loaded, 1 registered, 2 disposed (built into a pool bank)
            for (u32 i = 0; i < kPackCount; ++i) {
                const u32 count = g.below(10) == 0u ? 0u : 1u + g.below(12);
                const u32 first = g.below(2) == 0u ? 1u + g.below(100) : 0u;
                const u32 owns = g.below(6) == 0u ? 1u : 0u;
                make_pack(p, g, i, first, owns != 0u && g.below(2) == 0u ? 1u : count, owns);
            }
            u32 regs[18];
            for (u32& r : regs) r = g.next();
            const unsigned ops = 20u + g.below(40);
            for (unsigned op = 0; op < ops; ++op) {
                const auto& banks = n.store->banks();
                // an id: one inside a registered range, or anything small, now and then with high bits or 0
                const auto any_id = [&]() -> u32 {
                    u32 id = g.below(120);
                    if (!banks.empty() && g.below(4) != 0u) {
                        const auto& b = banks[g.below(static_cast<u32>(banks.size()))];
                        id = b.first + g.below(b.count + 1u);
                    }
                    if (g.below(6) == 0u) id |= g.next() & 0xFFFF0000u;
                    if (g.below(30) == 0u) id = 0u;
                    return id;
                };
                // A cursor the original may walk again (id 0) only while it points into memory that still holds the chain
                // it pointed into: a pack, an imported bank or node, or a node of the registry (a freed and reused node
                // can hold a zero length, which loops in the original and here alike).
                const auto walkable = [&](u32 cursor) {
                    if (cursor == 0u) return true;
                    if (cursor >= kPacks && cursor < kPacks + kPackStride * kPackCount) return true;
                    if (cursor >= kImport && cursor < kImportNodes + 0x20000u) return true;
                    for (const auto& b : banks)
                        for (const auto& node : b.nodes) {
                            if (node.events == 0u || o.stand.live.count(node.block) == 0u) continue;
                            for (u32 r = node.events, steps = 0; steps < 64u; ++steps) {  // a record of its chain
                                if (r == cursor) return true;
                                const u32 length = o.memory.read16(r + 2u);
                                if (length == 0xFFFFu || length == 0u) break;
                                r += length;
                            }
                        }
                    return false;
                };
                std::vector<std::pair<u32, std::vector<u32>>> sequence;  // calls of this operation
                bool no_oom = false;
                const u32 kind = g.below(100);
                if (kind < 12u) {
                    u32 i = g.below(kPackCount);
                    if (pack_state[i] == 2u) i = g.below(kPackCount);
                    if (pack_state[i] != 2u) sequence.push_back({kRegister, {kPacks + kPackStride * i}});
                } else if (kind < 16u) {
                    const u32 key = (!banks.empty() && g.below(4) != 0u)
                                        ? banks[g.below(static_cast<u32>(banks.size()))].first | (g.below(5) == 0u ? 0x30000u : 0u)
                                        : any_id();
                    sequence.push_back({kUnregister, {key}});
                } else if (kind < 17u) {
                    sequence.push_back({kDrop, {}});
                } else if (kind < 35u) {
                    sequence.push_back({kAcquire, {any_id()}});
                } else if (kind < 48u) {
                    sequence.push_back({kRelease, {any_id()}});
                } else if (kind < 52u) {
                    sequence.push_back({kDataOf, {any_id()}});
                } else if (kind < 56u) {
                    sequence.push_back({kHandleOf, {any_id()}});
                } else if (kind < 61u) {
                    sequence.push_back({kFrameRange, {g.below(3) == 0u ? 0x805E2360u : kOut + 0x100u + 0x10u * g.below(8), any_id()}});
                } else if (kind < 64u) {
                    sequence.push_back({kNameOf, {any_id()}});
                } else if (kind < 67u) {
                    no_oom = true;
                    sequence.push_back({kIdOfName, {g.below(10) == 0u ? 0u : g.below(3) == 0u ? 0x805E2260u : kLookupNames + 0x40u * g.below(9)}});
                } else if (kind < 73u) {
                    // a clip's cursor, then the next records of it
                    sequence.push_back({kTrack, {any_id() | 1u}});
                    for (u32 k = g.below(5); k > 0u; --k) sequence.push_back({kTrack, {0u}});
                } else if (kind < 95u) {
                    const bool second = kind >= 84u;
                    n.store->publish_to_guest();
                    u32 id = g.below(3) == 0u ? 0u : any_id();
                    if ((id & 0xFFFFu) == 0u && !walkable(n.memory.read32(second ? 0x805F2474u : 0x805F2470u))) id = any_id() | 1u;
                    const u32 mode = g.below(4) < 2u ? 0u : 1u + g.below(2);
                    const u32 flags = (1u + g.below(3)) | (mode == 1u ? 0x8000u : mode == 2u ? 0x4000u : 0u);
                    const u32 probe = g.below(8) == 0u ? 0u : kProbes + 0x20u * g.below(16);
                    const u32 out = kOut + 0x40u * g.below(4);
                    sequence.push_back({second ? kNext : kQuery,
                                        {id, second ? kKindOut + 4u * g.below(4) : flags, probe,
                                         second ? out : (g.below(5) == 0u ? 0u : out), g.below(2)}});
                } else {
                    const u32 o_index = g.below(4), index = g.below(4);
                    const u32 object = kObjects + 0x200u * o_index;
                    const u32 slot = kSlots + 0x800u * o_index + 304u * index;
                    p.w16(slot + 0x110u, any_id());
                    if (g.below(3) == 0u) p.w32(slot + 0x90u, g.next());
                    p.w32(slot + 0x94u, g.next() & 0x1806u);
                    sequence.push_back({kBind, {object, index}});
                }
                for (u32 k = 0; k < 18u; ++k)
                    if (g.below(4) == 0u) regs[k] = g.next();
                for (const auto& [entry, args] : sequence) {
                    o.prepare(regs);
                    n.prepare(regs);
                    o.cpu.fpscr = n.cpu.fpscr = g.below(2) == 0u ? 0u : (g.next() & 0xFFFFF0FBu);
                    o.stand.no_oom = n.stand.no_oom = no_oom;
                    const std::size_t log_o = o.stand.log.size(), log_n = n.stand.log.size();
                    const auto run = [&](Machine& m) {
                        switch (args.size()) {
                            case 0: return m.call(entry, {});
                            case 1: return m.call(entry, {args[0]});
                            case 2: return m.call(entry, {args[0], args[1]});
                            default: return m.call(entry, {args[0], args[1], args[2], args[3], args[4]});
                        }
                    };
                    const u32 ro = run(o);
                    const u32 rn = run(n);
                    ++calls;
                    ++per_entry[entry];
                    const bool has_result = entry != kUnregister && entry != kDrop && entry != kAcquire && entry != kRelease &&
                                            entry != kBind;
                    const std::string at = " (round " + std::to_string(round) + " op " + std::to_string(op) + ", " + hex(entry) +
                                           (args.empty() ? "" : "(" + hex(args[0]) + (args.size() > 1u ? ", " + hex(args[1]) : "") + ")") +
                                           (import_mode ? ", imported registry" : "") + ")";
                    if (has_result) require(ro == rn, "r3 differs: original " + hex(ro) + " native " + hex(rn) + at);
                    for (u32 k : {1u, 2u, 13u}) require(o.cpu.gpr[k] == n.cpu.gpr[k], "r" + std::to_string(k) + " differs" + at);
                    for (u32 k = 14; k < 32u; ++k)
                        require(o.cpu.gpr[k] == n.cpu.gpr[k], "r" + std::to_string(k) + " differs: original " + hex(o.cpu.gpr[k]) +
                                                                  " native " + hex(n.cpu.gpr[k]) + at);
                    require(o.cpu.lr == n.cpu.lr, "LR differs" + at);
                    require(o.cpu.fpscr == n.cpu.fpscr, "FPSCR differs: original " + hex(o.cpu.fpscr) + " native " + hex(n.cpu.fpscr) + at);
                    require(o.stand.log.size() - log_o == n.stand.log.size() - log_n,
                            "the calls differ in number: original " + std::to_string(o.stand.log.size() - log_o) + " native " +
                                std::to_string(n.stand.log.size() - log_n) + at);
                    for (std::size_t k = 0; k < o.stand.log.size() - log_o; ++k) {
                        const Call& a = o.stand.log[log_o + k];
                        const Call& b = n.stand.log[log_n + k];
                        if (a == b) continue;
                        char text[320];
                        std::snprintf(text, sizeof(text),
                                      "call %zu differs: original %08X(%X, %X, %X, %X) lr %08X sp %08X regs %016llX; native %08X(%X, "
                                      "%X, %X, %X) lr %08X sp %08X regs %016llX",
                                      k, a.target, a.r3, a.r4, a.r5, a.r6, a.lr, a.sp, static_cast<unsigned long long>(a.regs), b.target,
                                      b.r3, b.r4, b.r5, b.r6, b.lr, b.sp, static_cast<unsigned long long>(b.regs));
                        require(false, text + at);
                    }
                    for (std::size_t k = log_o; k < o.stand.log.size(); ++k) {
                        if (o.stand.log[k].target == kAlloc) ++allocations;
                        if (o.stand.log[k].target == kFree) ++frees;
                    }
                    if (entry == kRegister) {
                        if (ro != 0u) ++registered;
                        else ++refused;
                        const u32 i = (args[0] - kPacks) / kPackStride;
                        bool listed = false;
                        for (const auto& b : n.store->banks()) listed |= b.header == args[0];
                        if (ro != 0u) {
                            pack_state[i] = listed ? 1u : 2u;
                            if (!listed) ++built;
                        }
                    }
                    if (entry == kUnregister)
                        for (u32 i = 0; i < kPackCount; ++i)
                            if (pack_state[i] == 1u) {
                                bool listed = false;
                                for (const auto& b : n.store->banks()) listed |= b.header == kPacks + kPackStride * i;
                                // destroyed by the unregister, as in the game (its link word still names its successor)
                                if (!listed) pack_state[i] = 2u;
                            }
                    if ((entry == kQuery || entry == kNext) && ro != 0u) ++query_hits;
                    // the checkpoint path: N publishes its state block and rebuilds itself from guest memory
                    if (g.below(40) == 0u) {
                        n.store->publish_to_guest();
                        require(n.store->import_from_guest(), "re-import refused" + at);
                        ++imports;
                    }
                    if (every_op || op + 1u == ops) {
                        n.store->publish_to_guest();
                        ++memory_checks;
                        const u32 heap_top = std::max(o.stand.heap_top, n.stand.heap_top);
                        const u32 pool_top = std::max(o.stand.pool_top, n.stand.pool_top);
                        const struct {
                            u32 at, bytes;
                            const char* what;
                        } ranges[] = {
                            {kPacks, kPackStride * kPackCount, "packs"},
                            {kImport, 0x60000u, "imported banks"},
                            {kImportNodes, 0x20000u, "imported nodes"},
                            {kHeap, heap_top - kHeap + 0x40u, "heap"},
                            {kObjects, 0x50000u, "bind objects, probes and outputs"},
                            {kSmallData, kSmallDataEnd - kSmallData, "small data (state block, name buffer, defaults)"},
                            {kArena, pool_top - kArena, "pool arena"},
                            {kStack - 0x2000u, 0x2000u, "stack below r1"},
                        };
                        for (const auto& r : ranges) {
                            if (o.hash_range(r.at, r.bytes) == n.hash_range(r.at, r.bytes)) continue;
                            for (u32 a = r.at; a < r.at + r.bytes; a += 4u) {
                                const u32 x = o.word_at(a), y = n.word_at(a);
                                if (x != y)
                                    require(false, std::string("memory differs in ") + r.what + " at " + hex(a) + ": original " + hex(x) +
                                                       " native " + hex(y) + at);
                            }
                        }
                    }
                }
            }
        }
        const MotionBankStore::Stats& s = n.store->stats();
        std::printf("PASS motion bank store (B003): %u rounds (%llu from an imported registry), %llu calls, %llu memory "
                    "comparisons (every byte of the packs, heap, imported banks and nodes, objects, probes, outputs, small data, "
                    "used arena and 8 KiB of stack), %llu re-imports; registered %llu (pool banks built %llu), refused %llu; "
                    "allocator calls %llu, frees %llu; native store: nodes made %llu, allocation failures %llu, acquires %llu; "
                    "query / next hits %llu.\nCalls per entry:",
                    rounds, import_rounds, calls, memory_checks, imports, registered, built, refused, allocations, frees,
                    static_cast<unsigned long long>(s.loads), static_cast<unsigned long long>(s.load_failures),
                    static_cast<unsigned long long>(s.acquires), query_hits);
        for (const auto& [address, count] : per_entry) std::printf(" %08X x%llu", address, count);
        std::printf("\n");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL motion bank store contract: %s\n", error.what());
        return 1;
    }
}
