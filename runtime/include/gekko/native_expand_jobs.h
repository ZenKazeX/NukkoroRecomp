#pragma once

// The game's background PRS expand (TExpandBG) without a GameCube thread - docs/NATIVE_WORKERS.md, batch W1.
// GEKKO_NATIVE_EXPAND_JOBS=1 enables it; =2 adds Phase 1 of docs/NATIVE_STREAMING_REPLACEMENT_PLAN.md: the expand by
// the host decoder (GameMemory::expand_for_native) instead of the game's step, the notice still the game's. Default off
// until the user accepts it; refused together with the NativeJobs bridge (SHADOW_NATIVE_JOBS).
//
// The original (PAL main.dol): fn_8004C770 extracts a compressed ONE member with a callback by constructing a
// TExpandBG (ctor 0x80043E78: src +0x330, dst +0x334, callback +0x338, userdata +0x340, size +0x33C). Its base ctor
// SonicteamUSA::System::Thread::Thread (0x8035A15C, called from 0x80043EA0) writes the worker's fields, allocates a
// 0x1000-byte stack (0x803A12A4) with a 0xCD guard (memset 0x10), creates and resumes a detached priority-16 OSThread
// at +0x10 (entry 0x8035A01C) and registers the worker in the manager's 128-slot table (0x8035A390, manager
// 0x80584964). The worker runs when the main thread yields or sleeps: its step (vtable +0xC, 0x80043D5C) expands
// src -> dst (PRS 0x80043FE0, a native owner) and builds the TExpandNotice Task under group 0 (0x80043DB0); then
// running (+4) = 0, the finish (vtable +0x10, an empty Thread::vf2), done (+6) = 1, OSExitThread. The reaper
// 0x8035A2DC (once per main-loop iteration, before the task walk) deletes done workers; the dtor 0x8035A0C8 frees the
// stack after OSIsThreadTerminated (OSThread state +0x2C8 == 0 or 8) - else OSCancelThread.
// In gameplay a TExpandBG made in iteration N runs and finishes in N; its notice updates in N + 1, where the reaper
// deletes it (W0, chain_report.py).
//
// Native:
//   * the base ctor, entered from TExpandBG's ctor (LR 0x80043EA4) only, does the same fields, the same stack
//     allocation and guard and the same registration - no OSThread: its state word is 0 (terminated), and its first
//     words carry the job's tag and creation stamp (game tick, sequence). OSThread bytes are read by no game code
//     (W0: the poison test). Any other caller (TBGRead) runs the original constructor.
//   * at the reaper's entry (declining: the reaper then runs as before), every tagged worker in the manager's table
//     that is not done is run in creation order exactly as the worker entry runs it: the step through its vtable (the
//     game's own expand and notice), running = 0, the finish, done = 1 - on the game thread, without OSYieldThread.
// The pending jobs are guest memory only (the manager's table, the tags), so savestates need nothing new.
// Timing: a job is published at the next reaper call after its creation - in gameplay the original's frame; while
// loading, at most as late as the original.
//
// GEKKO_NATIVE_EXPAND_JOBS=3 is Phase 2 of docs/NATIVE_STREAMING_REPLACEMENT_PLAN.md: no worker object at all. The
// ONE member extract fn_8004C770 (this, index, dst, callback, userdata) is a native owner:
//   * not open (this +0x2C == 0 and its name byte 0) -> 0; the entry as fn_8004CA50 (this, index) computes it, in
//     C++: index < 2 or index >= count + 2 (signed; count = *(this +0x34) +0x10) -> 0, else *(this +0x34) + 0x34 +
//     index * 0x38; no entry -> 0;
//   * stored (entry +0x34 bit 0 clear): the copy dst <- entry +0x30, entry +0x2C bytes, as a host memmove
//     (GameMemory::move_for_native; memcpy 0x800054F4 when the memory is not direct); a callback gets fn_8004AD84 (the
//     TOneFileNotice, a game Task); -> the size. Compressed without a callback: the host decoder
//     (GameMemory::expand_for_native; the PRS owner 0x80043FE0 when it declines) -> the count.
//   * compressed with a callback: a host job {src, dst, callback, userdata} is queued -> 0. No `new` 0x348, no
//     TExpandBG ctor, no Thread base (stack, OSThread, manager slot), no "Expanding" registration (0x8025D580 in the
//     table 0x8056FFC8: its only reader, 0x8025D488, asks for kind 2; TExpandBG registers kind 1), no reaper delete.
//     The empty bracket calls 0x8035A018 / 0x8035A014 (bare blr) are not made.
// At the reaper's entry the queued jobs are published in submission order: the expand by the host decoder
// (GameMemory::expand_for_native; a decline runs the PRS owner 0x80043FE0 on the game thread), then the game's
// TExpandNotice exactly as 0x80043DB0 builds it - new 0x38, group 0 of 0x80571C6C (0x801EB614), ctor 0x80043F60
// with callback, size, userdata - and the worker field (+0x34) 0: the notice's update only clears that word.
// The queue is host state, saved in the savestate's native expand jobs block (version 28).
// What differs from modes 1 / 2: the heap layout (no 0x348 object, no 0x1000 stack: later allocations move) and the
// registry / manager tables' contents. Reported, never called identical (STREAMING_BEHAVIORAL_CONTRACTS.md G2).
// GEKKO_EXPAND_CENSUS=<file> writes one line per published job in modes 2 and 3 (tick, src, dst, size, callback,
// userdata, FNV-1a of the destination bytes) for the comparison.
//
// GEKKO_NATIVE_EXPAND_JOBS=4 is Phase 3's expand part (docs/NATIVE_STREAMING_REPLACEMENT_PLAN.md): mode 3, and the
// decode no longer waits for the publication point on the game thread when the host decode-ahead misses.
//   * At the extract - unless the decode-ahead already holds that stream decoded (GameMemory::ahead_has_stream) -
//     the job's compressed stream is copied from guest memory (at most the member's output size x 9/8
//     + 0x200: PRS's worst case, a literal per 9 bits, plus the decoder's token reach) and handed to one native worker
//     (a std::jthread, a FIFO under a mutex and a stop-aware condition variable). The worker decodes the copy with the host
//     decoder (prs_decode_ahead); it never reads or writes guest memory.
//   * At the publication point, in the same order as mode 3: the decode-ahead first (as before); then the worker's
//     result, waited for only if it is not done (the wait is measured), and placed only when every stream byte the
//     decode consumed still equals the guest's (GameMemory::expand_for_native's check); else the decode on the game
//     thread as before. All three give the same bytes; only where the work ran differs.
//   * Savestates keep the queue (version 28); the worker's results are not saved. A job loaded from a state has none
//     and is published through the decode-ahead or the game-thread decode.
//
// GEKKO_NATIVE_EXPAND_JOBS=5 is W2 (docs/NATIVE_WORKERS.md, docs/NATIVE_STREAMING_REPLACEMENT_PLAN.md): mode 4, and the
// game's background file read without its TBGRead worker. Needs GEKKO_NATIVE_BG_READ=1 (the read is its inline read).
//   * TBGReadManager::update 0x800453E0 (a Task, r3 = the manager) is a native owner. As the original: nothing while a
//     read is in flight (0x805EF1D8 != 0) or reads are paused (byte 0x805EF1DC); the first non-empty request list of
//     three (0x805742A0); none -> the busy entry off (0x80045370(this, 0)); else on, the file opened on the game
//     thread (0x8047AFA0(name, 1, 0, request)), and without a destination its size rounded up to 32 and allocated
//     (0x80049BB8(size, 2)); then, instead of `new` 0x390 + the TBGRead ctor (Thread base: stack, OSThread, manager
//     slot; the name copy at +0x34C): a queued host job {the request's six words, file} and the in-flight count + 1
//     (the ctor's); the request node popped and freed (0x80049B5C) as the original.
//   * At the reaper's entry, in submission order with the expand jobs, TBGRead's step 0x80045680 without its thread:
//     the load-hold query is skipped (its owner, native_disc_error, always answers 0); the whole file read into the
//     destination by the inline read (NativeRtGcnFs::chunked_read, the read loop 0x80045150 with chunk 0x2000) and
//     its result stored as the request's size word; the completion Task as 0x80045890 builds it - new 0x48, Task
//     ctor 0x8004F014 under the read manager's task (*fn_80037200()), then TFileControlAsycManager's ctor 0x800458F0's
//     writes in C++ (vtable 0x8051E2F0, word 0 from 0x805E4918, flags | 0x100, the six words at +0x28, the file at
//     +0x40, flags | 0x8000) with the worker word +0x44 = 0 (its update only clears it); then the in-flight count - 1
//     (the dtor's, at the reaper's delete).
//   * What differs from mode 4: no 0x390 object and no 0x1000 stack (later allocations move), the manager table, and
//     the Task's +0x44 until its update. The Task is built at the reaper's entry of the next main-loop iteration
//     instead of in the worker's slice before it; nothing between those two points reads it.
// Frames (contract C4: the game copies stack leftovers into the heap): the extract and TBGReadManager::update owners
// write their originals' frames - back chain, saved LR, saved registers - and make their guest calls from them with the
// original return sites and non-volatile registers. Where no object is built (a queued expand, a spawned read) the
// skipped ctor's own frames are not made; its return address in the owner's LR slot is written. The publication ran on
// the workers' stacks in the original: the main thread's stack below the reaper (within the thread's stack bounds,
// up to sp + 8) is restored after it, so it leaves no frames there.
// The census (GEKKO_EXPAND_CENSUS) adds a line per completed read ("R", the main-loop iteration, file, destination,
// size, the request's words 0 / 3 / 4 / 5, FNV-1a of the destination bytes): in modes 2-4 at the original ctor's
// entry (a declining hook), in mode 5 at the native build.

#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <vector>

#include "gekko/game_memory.h"
#include "gekko/types.h"

namespace gekko {

class Cpu;
class GameMemory;

class NativeExpandJobs final {
public:
    explicit NativeExpandJobs(Cpu& cpu) noexcept : cpu_(cpu) {}
    ~NativeExpandJobs();
    // Installs the hooks when GEKKO_NATIVE_EXPAND_JOBS=1, 2, 3 or 4. Returns whether it is enabled.
    bool install();
    bool enabled() const noexcept { return enabled_; }
    bool phase1() const noexcept { return phase1_; }
    bool phase2() const noexcept { return phase2_; }
    bool phase3() const noexcept { return phase3_; }
    bool w2() const noexcept { return w2_; }
    std::size_t pending() const noexcept { return queue_.size(); }

    // The queue of mode 3 (empty otherwise). Loading a non-empty queue into a run without mode 3 fails, and one with a
    // read job into a run without mode 5. `version`: the savestate's (28: expand jobs only, 29: jobs with a kind).
    void save_state(std::vector<u8>& out) const;
    bool load_state(const u8*& cursor, const u8* end, u32 version);
    // Phase 1 (GEKKO_NATIVE_EXPAND_JOBS=2) expands through GameMemory::expand_for_native.
    void set_game_memory(GameMemory* memory) noexcept { game_memory_ = memory; }
    // W2's read (NativeRtGcnFs::chunked_read): (file, destination, chunk) -> the read loop's result, or nothing when the
    // run stopped. Set before install(); mode 5 is refused without it.
    using ChunkedRead = std::function<std::optional<u32>(Cpu&, u32 file, u32 destination, u32 chunk)>;
    void set_chunked_read(ChunkedRead read) { chunked_read_ = std::move(read); }

    struct Stats {
        u64 created = 0;    // TExpandBG workers constructed without a thread
        u64 published = 0;  // ... run at a reaper call
        u64 reaper_calls = 0;
        u64 expanded_natively = 0;  // Phase 1: the expand by the host decoder, no guest step
        u64 expanded_by_guest = 0;  // Phase 1: declined by the host decoder, the game's step ran
        u64 extracts = 0;           // Phase 2: fn_8004C770 calls served
        u64 queued = 0;             // ... compressed with a callback: host jobs, no worker
        u64 expanded_by_owner = 0;  // ... published through the PRS owner (the host decoder declined)
        u64 submitted = 0;          // Phase 3: streams copied at the extract and decoded by the native worker
        u64 ahead_ready = 0;        // ... not copied: the decode-ahead already held the stream
        u64 snapshot_bytes = 0, snapshot_ns = 0, snapshot_ns_max = 0;  // ... the copies (game thread)
        u64 worker_declined = 0;    // ... streams the worker's decoder did not accept
        u64 waited = 0, wait_ns = 0, wait_ns_max = 0;  // publications that waited for the worker
        u64 unprepared = 0;         // published without a worker result (decode-ahead ready, or loaded from a state)
        u64 stored_native = 0;      // stored members copied by the host memmove (the rest: memcpy 0x800054F4)
        u64 synchronous_native = 0; // compressed members without a callback expanded by the host decoder
        u64 read_updates = 0;       // W2: TBGReadManager::update calls served
        u64 reads_queued = 0;       // ... reads queued (no TBGRead)
        u64 reads_published = 0;    // ... read and completed at the reaper's entry
        u64 read_bytes = 0;         // ... the read loop's results summed
        u64 read_ns = 0, read_ns_max = 0;  // ... the reads' game-thread time (the inline read at publication)
    };
    const Stats& stats() const noexcept { return stats_; }

private:
    bool publish_queue(Cpu& c);

    Cpu& cpu_;
    bool enabled_ = false;
    bool phase1_ = false;
    bool phase2_ = false;
    bool phase3_ = false;
    bool w2_ = false;
    GameMemory* game_memory_ = nullptr;
    ChunkedRead chunked_read_;
    u64 iterations_ = 0;  // reaper calls (main-loop iterations), for the census
    // Phase 3: a copy of a job's stream and what the worker decoded from it.
    struct Prepared {
        std::vector<u8> bytes;
        u32 size = 0;  // the member's output size (the decoder's hint)
        PrsDecoded decoded;
        bool ok = false;
    };
    using PreparedResult = std::shared_future<std::shared_ptr<const Prepared>>;
    enum class Kind : u32 { Expand = 0, Read = 1 };
    struct Job {
        u32 src, dst, callback, userdata;  // an expand job
        PreparedResult prepared;           // Phase 3; not saved
        Kind kind = Kind::Expand;
        std::array<u32, 6> request{};      // a read job (W2): the request's six words, as the TBGRead ctor copies them
        u32 file = 0;
    };
    bool publish_read(Cpu& c, Job& j);
    void census_read(Cpu& c, u32 file, const u32* words);
    PreparedResult submit(std::shared_ptr<Prepared> prepared);
    void work(std::stop_token stop);
    std::vector<Job> queue_;
    std::FILE* census_ = nullptr;
    Stats stats_;
    u64 stamp_tick_ = 0;  // the game tick of the last creation, and the sequence within it
    u32 stamp_seq_ = 0;
    bool restored_tagged_ = false;  // after a load: tagged mode 1/2 workers may be waiting (modes 3+ publish them)
    // Phase 3's worker: started by install() in mode 4; declared last, so it is asked to stop and joined first.
    std::mutex work_mutex_;
    std::condition_variable_any work_wake_;
    std::deque<std::packaged_task<std::shared_ptr<const Prepared>()>> work_queue_;
    std::jthread worker_;
};

}  // namespace gekko
