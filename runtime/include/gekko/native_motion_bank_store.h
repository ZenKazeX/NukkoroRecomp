#pragma once

// The game's motion store as a native registry (SubQ batch B003; design: recovered/game/motion/README.md).
//
// What the game keeps: a list of clip BANKS in registration order (a loaded motion pack, or the "pool bank" 0x80414588
// builds from one), each owning the clip ids [first, first + count); for a pool bank a cache of the clips now in use,
// one heap NODE per clip holding a copy of its name, data and events taken out of the native clip pool; a held clip per
// record query; two event cursors, a track cursor and the last event record found. The state block of the original
// (r13 + 0x5F58 .. 0x5F74 = 0x805F2458 .. 0x805F2477) held the list head and those words.
//
// Owned here, natively: the bank list (std::vector in list order), each bank's fields, its pool records and its node
// list with the use counts, the four held ids, the three cursors, the last-found word and the track flag. Every lookup
// is a walk of these native structures; no guest word of the registry is ever read back. The 15 functions of the
// store are native owners (extract/native-owners.txt, MotionBankStore::addresses()):
//   0x804140A8 register  0x80414588 build  0x80413A24 unregister  0x80412764 drop loaded clips
//   0x80414368 acquire   0x80414290 release
//   0x80413970 data_of   0x8041342C handle_of  0x80413644 frame_range  0x80413870 name_of  0x80413788 id_of_name
//   0x8041352C track     0x80412E3C query  0x80412868 next_record      0x8041ADAC bind (a client: a motion slot's clip)
// and the 12 symbol-map rows inside those bodies (unreachable `b` words), owned as stops so no original word is translated.
// Behaviour and quirks are those of the readable reconstruction (recovered/game/motion/store/*.h, clip_records/*.h),
// proved against the original words by runtime/tests/motion_bank_store_contract_tests.cpp: the first bank whose range
// holds an id decides; name_of keeps a use per call on a pool clip; id_of_name keeps every pool clip it passes; removal
// frees nodes whatever their uses; build leaks its header for an owning pack whose count is not 1; nodes, bank headers
// and records are allocated from the game's heap in the original's order and sizes.
//
// What stays in guest memory, and why:
//  - The clip bytes: resident packs (read in place) and each node's copy (the heap block, same allocator, sizes and
//    order). The translated consumers read names, data, events and track records through guest pointers, and the event
//    cursor's reads of a node after its last release (docs/ARAM.md) depend on that block's address and lifetime.
//  - The clip data's +4 handle, the name buffer 0x805E2260, the query defaults 0x805E2360 / 0x805E2370, the callers'
//    output buffers, a slot's words (bind): all are read by translated code.
//  - SCAFFOLDING (B003): the bytes the original writes into its heap blocks (a bank header's fields and links, the
//    pool records, a node's id / uses / pointers / link) are still written there, write-only: the blocks are game-heap
//    memory that the state report digests and that freed-node reads (the event cursors) can see, so the heap must stay
//    byte-identical. Nothing reads them back. Deleted by the step that moves the clip consumers (0x80415xxx-0x8041Cxxx)
//    native and the node copies out of the guest heap.
//  - SCAFFOLDING (B003): the stack frames of the original functions (back chain, saved registers, the link word and
//    frame_range's conversion words) and the register image a guest callee sees: the state report digests the stack's
//    deepest words. Deleted with the callers' conversion (the frames then no longer exist).
// The state block 0x805F2458 .. 0x805F2477 is NOT kept in guest memory while the game runs: publish_to_guest() writes it
// for a checkpoint or a state report (the guest image's terms), import_from_guest() rebuilds the registry from a
// restored image.
//
// Native readers: far_objects (runtime/scaffold/rw_driver) walks the banks through MotionBankStore::of(cpu) and reads a
// pool clip through pool().pool_bytes.

#include <cstddef>
#include <vector>

#include "gekko/types.h"

namespace gekko {

class Cpu;
class GuestMemory;
class NativeMotionStore;

class MotionBankStore {
public:
    MotionBankStore(Cpu& cpu, GuestMemory& memory, NativeMotionStore& pool) noexcept;
    ~MotionBankStore();
    MotionBankStore(const MotionBankStore&) = delete;
    MotionBankStore& operator=(const MotionBankStore&) = delete;

    void install();

    struct Address {
        u32 address;
        const char* name;
    };
    static const Address* addresses(std::size_t& count) noexcept;

    // The store serving `cpu` (the one installed on it), or nullptr.
    static const MotionBankStore* of(const Cpu& cpu) noexcept;

    // A pool bank's clip record (absolute pool addresses after the build; words the build leaves unwritten are what
    // the allocation held).
    struct Record {
        u32 name = 0, name_bytes = 0, data = 0, data_bytes = 0, events = 0, events_bytes = 0;
    };
    // A loaded clip of a pool bank (its node: a game-heap block).
    struct Node {
        u32 block = 0;          // the node's guest address
        u16 id = 0;
        s16 uses = 0;
        u32 name = 0, data = 0, events = 0;  // guest addresses into the block
    };
    struct Bank {
        u32 header = 0;         // the bank's guest address (the pack, or the pool bank's header)
        u16 first = 0, count = 0;
        u8 owns = 0, byte5 = 0, pooled = 0, byte7 = 0;
        u32 destroy = 0;
        u32 table = 0;          // the clip table (resident banks; read in place)
        u32 pool = 0;           // the pool block (pool banks)
        std::vector<Record> records;
        std::vector<Node> nodes;   // node-list order (a new node goes to the tail)
    };
    const std::vector<Bank>& banks() const noexcept { return banks_; }
    // The clip pool the pool banks' records address (their addresses are its handles: NativeMotionStore::pool_bytes).
    const NativeMotionStore& pool() const noexcept { return pool_; }

    // The state block 0x805F2458 .. 0x805F2477 into guest memory (a checkpoint, a state report).
    void publish_to_guest();
    // The registry rebuilt from guest memory (a restored checkpoint, whatever version wrote it). False: the guest list
    // is not a list (the run is not stopped; the caller decides).
    bool import_from_guest();

    struct Stats {
        u64 registered = 0;        // register calls that registered a bank
        u64 built = 0;             // pool banks built
        u64 unregistered = 0;
        u64 acquires = 0;
        u64 loads = 0;             // nodes made (clips copied out of the pool)
        u64 load_failures = 0;     // allocations that returned 0
        u64 frees = 0;             // nodes freed (release at 0, unregister, drop)
        u64 queries = 0;           // 0x80412E3C and 0x80412868
        u64 deferred_interrupts = 0;  // transfers after which an interrupt was deliverable (taken after the owner)
    };
    const Stats& stats() const noexcept { return stats_; }

    // The native body behind each owner (r3.. as the original takes them; the result is the original's r3). Public for
    // the contract test; the game reaches them only through the owner entries.
    struct Impl;

private:
    Cpu& cpu_;
    GuestMemory& memory_;
    NativeMotionStore& pool_;
    std::vector<Bank> banks_;
    u32 last_found_ = 0;       // 0x805F245C
    u16 held_[4] = {};         // 0x805F2460 .. 0x805F2466
    u32 track_ = 0;            // 0x805F2468
    u8 track_ready_ = 0;       // 0x805F246C (an s8 flag)
    u32 events_a_ = 0;         // 0x805F2470 (query)
    u32 events_b_ = 0;         // 0x805F2474 (next_record)
    Stats stats_;
};

}  // namespace gekko
