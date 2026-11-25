#pragma once

// The motion clips without ARAM: no ARAM pool, no ARQ, no DMA, no wait (docs/ARAM.md, retirement step 1).
//
// What the game does (main.dol, PAL): a motion pack is loaded into the game's heap and registered by 0x804140A8;
// the bank builder 0x80414588 reserves the pack's clip span in a 6 MiB ARAM pool (created by 0x804225B0 with
// ARAlloc, allocated from by 0x804223B8), uploads it (0x804222C0: an ARQ request, then a wait on [r13+0x5FA0]),
// frees the pack and keeps one record of pool addresses per clip (bank byte +6 = 1). Each first play of a clip
// (acquire 0x80414368) allocates a node in the heap and copies the clip's name, data and events back from the pool
// (0x80422234: three ARQ requests, the game thread waiting on each); the node is freed when no motion slot holds the
// clip any more (0x80414290 and its inline copies) and copied again on the next play. The pool is destroyed with
// the stage (0x80422550: its nodes freed, then ARFree).
//
// Owned here: the pool's create and destroy, the two transfers and the pool's bytes. The pool's bytes are native
// storage owned here (NativeArenaStorage; until 2026-11-21 a region of the guest window at 0x90000000): the upload and
// the fetch are plain copies into and out of it; no ARAM block is allocated. The game keeps the pool's addresses -
// the base word [r13+0x5F98] = kPoolBase and every address 0x804223B8 hands out from it - as 32-bit handles: it only
// carries them and passes them back to the transfers, and only native code resolves one (pool_bytes); a guest access
// to one stops the run (nothing is mapped there). Everything else - the builder, acquire, release, the pool's allocator
// and its heap nodes, the per-clip nodes - stays the game's own code (or the native MotionBankStore), so the heap sees
// exactly the original's allocations.
//
// Why not resident banks (the game's other kind, no nodes at all): measured on the Westopolis tape, a Black Arms
// soldier's animation then differs from the original for 271 frames, while the clip bytes are identical and this
// design matches the original exactly on both tapes. The original's animation depends on the per-clip nodes'
// lifetime - most likely the shared current-events pointer ([r13+0x5F70], set by 0x80412E3C) outliving the node it
// points into. Removing the per-clip copy without changing the game needs that reversed first (docs/ARAM.md).

#include <cstddef>
#include <string>

#include "gekko/native_arena_storage.h"
#include "gekko/types.h"

namespace gekko {

class Cpu;
class GuestMemory;

class NativeMotionStore {
public:
    NativeMotionStore(Cpu& cpu, GuestMemory& memory) noexcept : cpu_(cpu), memory_(memory) {}

    void install();

    struct Address {
        u32 address;
        const char* name;
    };
    static const Address* addresses(std::size_t& count) noexcept;

    // The pool's handle space: the base the game keeps (the original's ARAlloc result; a value unused by the console's
    // map) and the handles' extent. Pool byte p of the storage is handle kPoolBase + p.
    static constexpr u32 kPoolBase = 0x90000000u;
    static constexpr u32 kPoolReserve = 0x04000000u;  // 64 MiB reserved; granules commit as the pool uses them
    // The label of the pool's checkpoint record and state-report entry (NativeArenaImage): the window base it had.
    static constexpr u32 kRecordLabel = kPoolBase;
    // A savestate made before ARAM was retired keeps its pool at ARAM addresses (below kAramEnd); those addresses are
    // served from the pool at the same offset (docs/ARAM.md "Old savestates").
    static constexpr u32 kAramEnd = 0x01000000u;
    static u32 pool_address(u32 address) noexcept { return address < kAramEnd ? kPoolBase + address : address; }
    // Such a state's pool [base, base + bytes) (ARAM's bytes in `image`) taken over.
    bool adopt_pool(u32 base, u32 bytes, const u8* image, std::string& why);

    // The committed pool bytes a handle range names (pool_address applied), or nullptr: the one way to the bytes.
    const u8* pool_bytes(u32 address, u32 bytes) const noexcept;
    u8* pool_bytes(u32 address, u32 bytes) noexcept;
    // The pool's storage reserved if it is not, and committed through the handle `end` (exclusive). False outside the
    // handle space or on a host refusal.
    bool commit_through(u32 end) noexcept;
    // A savestate is being restored: the storage dropped (the file's record restores what it names).
    void discard() noexcept { storage_.release(); }
    // The pool's bytes for a checkpoint record and the state report (nothing before the first pool exists), and their
    // restore from such a record.
    bool image(NativeArenaImage& out) const noexcept;
    bool restore_image(u32 size, u32 committed, const u8* bytes, std::string& why);

    struct Stats {
        u64 pools = 0;            // pools created (once per stage)
        u64 uploads = 0;          // clip spans copied into the pool
        u64 upload_bytes = 0;
        u64 fetches = 0;          // parts copied out of the pool into a clip's node
        u64 fetch_bytes = 0;
        u32 committed = 0;        // pool bytes committed (the pool's high-water mark, granular)
        u64 interrupt_points = 0; // transfers that ended where the original's wait took a due interrupt
    };
    const Stats& stats() const noexcept { return stats_; }

    // The two transfers for a native caller (the motion bank store, native_motion_bank_store.h): the copy of
    // 0x804222C0 (pool `to` <- the game's `from`) and of 0x80422234 (the game's `to` <- pool `from`) without the guest
    // call and without taking interrupts (a native caller cannot hand back in the middle of its work; it counts the
    // point instead). False: the run is stopped (a game address outside mapped memory, a handle outside the pool).
    bool upload_span(Cpu& c, u32 to, u32 from, u32 bytes);
    bool fetch_span(Cpu& c, u32 to, u32 from, u32 bytes);

private:
    void pool_create(Cpu& c);
    void pool_destroy(Cpu& c);
    void upload(Cpu& c);
    void fetch(Cpu& c);
    void take_interrupts(Cpu& c);
    bool ensure_storage(Cpu& c);
    bool call_guest(Cpu& c, u32 function, u32 r3, u32 r4);
    void stop(Cpu& c, const std::string& why);

    Cpu& cpu_;
    GuestMemory& memory_;  // the game's memory (the transfers' other side; never the pool's bytes)
    NativeArenaStorage storage_;
    Stats stats_;
    bool verify_ = false;  // SHADOW_MOTION_VERIFY=1: each upload's hash (compare with the ARAM census's)
};

}  // namespace gekko
