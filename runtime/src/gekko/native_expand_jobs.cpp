// The game's background PRS expand (TExpandBG) without a GameCube thread. Contract: include/gekko/native_expand_jobs.h,
// docs/NATIVE_WORKERS.md (W1).

#include "gekko/native_expand_jobs.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "gekko/cpu.h"
#include "gekko/game_memory.h"
#include "gekko/recomp.h"
#include "gekko/savestate.h"

namespace gekko {

namespace {

constexpr u32 kThreadCtor = 0x8035A15Cu;      // SonicteamUSA::System::Thread::Thread(this, stack size)
constexpr u32 kFromExpandCtor = 0x80043EA4u;  // its return address in TExpandBG::TExpandBG (0x80043E78)
constexpr u32 kReaper = 0x8035A2DCu;          // the manager's per-iteration reaper (r3 = manager)
constexpr u32 kThreadVtable = 0x805608C8u;
constexpr u32 kExpandVtable = 0x8051E23Cu;    // TExpandBG's (its ctor sets it after the base ctor)
constexpr u32 kAllocate = 0x803A12A4u;        // the stack's allocator (operator new)
constexpr u32 kMemset = 0x8000540Cu;
constexpr u32 kManager = 0x801E24D0u;         // the manager's instance (0x80584964)
constexpr u32 kRegister = 0x8035A390u;        // manager register(manager, worker) -> slot

// The worker (SonicteamUSA::System::Thread)
constexpr u32 wSlot = 0x0u, wRunning = 0x4u, wFlag5 = 0x5u, wDone = 0x6u, wPriority = 0x8u, wStack = 0xCu;
constexpr u32 wThread = 0x10u, wVtable = 0x328u;
constexpr u32 wSrc = 0x330u, wDst = 0x334u, wSize = 0x33Cu;  // TExpandBG's fields (ctor 0x80043E78)
constexpr u32 kNoticeBuilder = 0x80043DB0u;  // TExpandBG step's tail: new TExpandNotice under group 0
constexpr u32 kEmptyFinish = 0x8035A0BCu;    // Thread::vf2, TExpandBG's finish: a bare blr
constexpr u32 tState = 0x2C8u;                        // OSThread state (u16): 0 / 8 = terminated
constexpr u32 tTag = 0x0u, tTickHi = 0x4u, tTickLo = 0x8u, tSeq = 0xCu;  // in the unused OSThread words
constexpr u32 kTag = 0x4E584A31u;                     // 'NXJ1'
constexpr u32 kDefaultStack = 0x1000u;
constexpr u32 kSlots = 0x80u;

// Phase 2 (mode 3): the ONE member extract and what its compressed-with-callback branch reaches.
constexpr u32 kExtract = 0x8004C770u;      // fn_8004C770(one, index, dst, callback, userdata)
constexpr u32 kMemcpy = 0x800054F4u;       // memcpy (memmove), when the memory is not direct host memory
constexpr u32 kFileNotice = 0x8004AD84u;   // (callback, size, userdata): the stored member's TOneFileNotice
constexpr u32 kPrs = 0x80043FE0u;          // the PRS expand (src, dst) -> count, when the host decoder declines
constexpr u32 kNew = 0x803A1380u;          // operator new
constexpr u32 kGroups = 0x8001C4F8u;       // static_80571C6C::instance
constexpr u32 kGroup = 0x801EB614u;        // (groups, index) -> the group
constexpr u32 kNoticeCtor = 0x80043F60u;   // TExpandNotice(this, worker, parent, callback, size, userdata)
constexpr u32 kNoticeBytes = 0x38u;

// W2 (mode 5): TBGRead without a worker object.
constexpr u32 kReadUpdate = 0x800453E0u;          // TBGReadManager::update (r3 = the manager)
constexpr u32 kFileLoading = 0x80045370u;         // the manager's "File Loading" busy entry (this, on)
constexpr u32 kOpen = 0x8047AFA0u;                // update's open (name, 1, 0, request) -> the file or 0
constexpr u32 kRwAllocate = 0x80049BB8u;          // the RenderWare allocator (size, 2)
constexpr u32 kRwFree = 0x80049B5Cu;              // ... and its free (the request node)
constexpr u32 kTaskCtor = 0x8004F014u;            // Task::Task(this, parent)
constexpr u32 kReadManagerGetter = 0x80037200u;   // the read manager's holder (built on its first call)
constexpr u32 kReadManagerHolder = 0x805EF064u;   // ... the holder's address once built, else 0
constexpr u32 kFileControlCtor = 0x800458F0u;     // TFileControlAsycManager(this, worker, parent, request, file)
constexpr u32 kFileControlVtable = 0x8051E2F0u, kFileControlWord0 = 0x805E4918u, kFileControlBytes = 0x48u;
constexpr u32 kInFlight = 0x805EF1D8u, kPause = 0x805EF1DCu, kReadLists = 0x805742A0u, kReadChunk = 0x2000u;
constexpr u32 tFlags = 0x4u, tVtable = 0x18u, tRequest = 0x28u, tFile = 0x40u, tWorker = 0x44u;
constexpr u32 oOneOpen = 0x2Cu, oOneHeader = 0x34u, hCount = 0x10u;
constexpr u32 eFirst = 0x34u, eBytes = 0x38u, eStoredSize = 0x2Cu, eData = 0x30u, eFlags = 0x34u;

// fn_8004CA50 One::Entry (a leaf): the member's entry, or 0 for an index outside [2, count + 2) (signed compares).
// The caller has checked that the ONE is open, the function's own first test.
u32 one_entry(Cpu& c, u32 one, u32 index) {
    if (static_cast<s32>(index) < 2) return 0u;
    const u32 header = c.mem_read32(one + oOneHeader);
    const u32 count = c.mem_read32(header + hCount);
    if (static_cast<s32>(index) >= static_cast<s32>(count + 2u)) return 0u;
    return header + eFirst + index * eBytes;
}

// A synchronous guest call returning here.
bool call_guest(Cpu& c, u32 function, u32 r3, u32 r4 = 0u, u32 r5 = 0u, u32 r6 = 0u, u32 r7 = 0u, u32 r8 = 0u) {
    const u32 resume = c.lr;
    const u32 link = c.lr;
    c.gpr[3] = r3;
    c.gpr[4] = r4;
    c.gpr[5] = r5;
    c.gpr[6] = r6;
    c.gpr[7] = r7;
    c.gpr[8] = r8;
    c.gpr[12] = function;
    c.ctr = function;
    recomp::call_indirect(c, function, link);
    if (c.exiting()) c.finish_transfers();
    if (c.exiting() || c.pc != link) {
        c.request_stop(StopReason::ExecutionFault,
                       "native expand jobs: a guest call (0x" + std::to_string(function) + ") did not return");
        return false;
    }
    c.lr = resume;
    return true;
}

// Frames (docs/STREAMING_BEHAVIORAL_CONTRACTS.md C4: the stack is observable - the game copies stack leftovers into
// the heap). The owners of main-thread functions (the extract, TBGReadManager::update) write their originals' frames
// and make their guest calls from them, with the original return sites and the non-volatile registers the original
// holds there, so the callees' frames are the original's too.
using Regs = std::array<u32, 5>;  // r27..r31
Regs regs_of(const Cpu& c) { return {c.gpr[27], c.gpr[28], c.gpr[29], c.gpr[30], c.gpr[31]}; }
u32& reg(Regs& regs, u32 r) { return regs[r - 27u]; }
// stwu r1, -size(r1); mflr r0; stw r0, size + 4(r1). Returns the frame.
u32 prologue(Cpu& c, u32 sp, u32 lr, u32 size) {
    const u32 frame = sp - size;
    c.mem_write32(frame, sp);
    c.mem_write32(sp + 4u, lr);
    return frame;
}
// stmw rN, at(r1).
void save_regs(Cpu& c, u32 at, u32 first, const Regs& regs) {
    for (u32 r = first; r <= 31u; ++r) c.mem_write32(at + (r - first) * 4u, regs[r - 27u]);
}
bool call_guest_from(Cpu& c, u32 frame, u32 site, const Regs& regs, u32 function, u32 r3, u32 r4 = 0u, u32 r5 = 0u,
                     u32 r6 = 0u) {
    const u32 sp = c.gpr[1], link = c.lr, entry_pc = c.pc;
    const Regs saved = regs_of(c);
    c.gpr[1] = frame;
    for (u32 r = 27u; r <= 31u; ++r) c.gpr[r] = regs[r - 27u];
    c.gpr[3] = r3;
    c.gpr[4] = r4;
    c.gpr[5] = r5;
    c.gpr[6] = r6;
    c.gpr[12] = function;
    c.ctr = function;
    recomp::call_indirect(c, function, site);
    if (c.exiting()) c.finish_transfers();
    if (c.exiting() || c.pc != site) {
        c.request_stop(StopReason::ExecutionFault,
                       "native expand jobs: a guest call (0x" + std::to_string(function) + ") did not return");
        return false;
    }
    c.gpr[1] = sp;
    c.lr = link;
    c.pc = entry_pc;  // an owner leaves pc at its entry (the verifier's owner pass then resumes at LR, as dispatch does)
    for (u32 r = 27u; r <= 31u; ++r) c.gpr[r] = saved[r - 27u];
    return true;
}

// The main thread's stack below `sp`, within its own stack (OSThread +0x308, the current thread at 0x800000E4), kept
// across work that the original ran on a worker's stack (the publication), so that work leaves no frames here.
class StackKeep {
public:
    StackKeep(Cpu& c, u32 sp) : c_(c) {
        constexpr u32 kCurrentThread = 0x800000E4u, tStackBase = 0x304u, tStackEnd = 0x308u, kWindow = 0x10000u;
        u32 low = sp - 0x1000u;
        if (const u32 thread = c.mem_read32(kCurrentThread); thread != 0u) {
            const u32 base = c.mem_read32(thread + tStackBase), end = c.mem_read32(thread + tStackEnd);
            if (end < sp && sp <= base) low = std::max(end, sp - kWindow);
        }
        low_ = low & ~3u;
        // Up to sp + 8: the callees also save their return address in the caller's LR slot at sp + 4.
        for (u32 at = low_; at < (sp & ~3u) + 8u; at += 4u) words_.push_back(c.mem_read32(at));
    }
    ~StackKeep() {
        u32 at = low_;
        for (const u32 w : words_) {
            c_.mem_write32(at, w);
            at += 4u;
        }
    }

private:
    Cpu& c_;
    u32 low_ = 0u;
    std::vector<u32> words_;
};

u64 elapsed_ns(std::chrono::steady_clock::time_point started) {
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count());
}

u32 fnv1a(Cpu& c, u32 at, u32 size) {
    u32 h = 0x811C9DC5u;
    for (u32 i = 0; i < size; ++i) h = (h ^ c.mem_read8(at + i)) * 0x01000193u;
    return h;
}

}  // namespace

NativeExpandJobs::~NativeExpandJobs() {
    if (worker_.joinable()) {
        worker_.request_stop();
        worker_.join();
    }
    if (census_ != nullptr) std::fclose(census_);
}

bool NativeExpandJobs::install() {
    // 1 = W1 (the game's step at the reaper), 2 = Phase 1 (the expand by the host decoder, no guest step),
    // 3 = Phase 2 (no worker object: the extract queues a host job), 4 = Phase 3 (+ the native decode worker),
    // 5 = W2 (+ TBGRead without a worker object).
    // Default (user decision 2026-11-08, S4): mode 5. GEKKO_NATIVE_EXPAND_JOBS=0 selects the original TExpandBG thread.
    const char* env = std::getenv("GEKKO_NATIVE_EXPAND_JOBS");
    if (env == nullptr) env = "5";
    if (env[0] < '1' || env[0] > '5' || env[1] != '\0') return false;
#if defined(SHADOW_RENDER_LIFTED)
    if (env[0] != '5') return false;  // the lifted build prunes the guest loader threads: only the default mode exists
#endif
    phase1_ = env[0] != '1';
    phase2_ = env[0] >= '3';
    phase3_ = env[0] >= '4';
    w2_ = env[0] == '5';
    if (w2_ && !chunked_read_) {
        std::fprintf(stderr, "native expand jobs: mode 5 (W2) needs the inline background read, GEKKO_NATIVE_BG_READ=1\n");
        return false;
    }
    if (const char* bridge = std::getenv("SHADOW_NATIVE_JOBS"); bridge != nullptr && bridge[0] == '1') {
        std::fprintf(stderr, "native expand jobs: not installed together with the NativeJobs bridge\n");
        return false;
    }
    enabled_ = true;
    Cpu& cpu = cpu_;
    if (const char* path = std::getenv("GEKKO_EXPAND_CENSUS"); phase1_ && path != nullptr && path[0] != '\0')
        census_ = std::fopen(path, "w");
    if (phase3_) worker_ = std::jthread([this](std::stop_token stop) { work(stop); });

    // Phase 2 (mode 3): the ONE member extract fn_8004C770, every path; the compressed member with a callback becomes a
    // queued host job instead of a TExpandBG.
    if (phase2_) {
        cpu.install_hle(
            kExtract,
            [this](Cpu& c) {
                const u32 one = c.gpr[3], index = c.gpr[4], dst = c.gpr[5], callback = c.gpr[6], userdata = c.gpr[7];
                const u32 sp = c.gpr[1], link = c.lr;
                ++stats_.extracts;
                u32 result = 0u;
                // The original's frame: 0x20 bytes, r27..r31 saved at +0xC; then r27 = dst, r28 = callback,
                // r29 = userdata, r30 = 0, r31 = the entry.
                Regs regs = regs_of(c);
                const u32 frame = prologue(c, sp, link, 0x20u);
                save_regs(c, frame + 0xCu, 27u, regs);
                reg(regs, 27) = dst;
                reg(regs, 28) = callback;
                reg(regs, 29) = userdata;
                reg(regs, 30) = 0u;
                // Open: a handle (+0x2C) or a name (its first byte).
                if (c.mem_read32(one + oOneOpen) != 0u || c.mem_read8(one) != 0u) {
                    const u32 entry = one_entry(c, one, index);  // a leaf
                    reg(regs, 31) = entry;
                    if (entry != 0u) {
                        const u32 data = c.mem_read32(entry + eData);
                        if ((c.mem_read32(entry + eFlags) & 1u) != 0u) {
                            if (callback != 0u) {
                                Job job{data, dst, callback, userdata, {}};
                                if (phase3_ && game_memory_ != nullptr && game_memory_->ahead_has_stream(c, data)) {
                                    ++stats_.ahead_ready;
                                } else if (phase3_ && game_memory_ != nullptr) {
                                    // Phase 3: the stream's bytes now, decoded by the worker until the publication.
                                    const auto started = std::chrono::steady_clock::now();
                                    auto prepared = std::make_shared<Prepared>();
                                    prepared->size = c.mem_read32(entry + eStoredSize);
                                    const std::size_t bound =
                                        std::size_t{prepared->size} + prepared->size / 8u + 0x200u;
                                    if (game_memory_->snapshot_stream(c, data, bound, prepared->bytes)) {
                                        stats_.snapshot_bytes += prepared->bytes.size();
                                        job.prepared = submit(std::move(prepared));
                                        ++stats_.submitted;
                                    }
                                    const u64 ns = elapsed_ns(started);
                                    stats_.snapshot_ns += ns;
                                    stats_.snapshot_ns_max = std::max(stats_.snapshot_ns_max, ns);
                                }
                                queue_.push_back(std::move(job));
                                ++stats_.queued;
                                // The TExpandBG ctor the original calls here saves its return address in this frame's
                                // LR slot (its frames below are not made: no object is built).
                                c.mem_write32(frame + 4u, 0x8004C80Cu);
                            } else if (const std::optional<u32> count =
                                           game_memory_ != nullptr ? game_memory_->expand_for_native(c, data, dst)
                                                                   : std::nullopt) {
                                result = *count;
                                ++stats_.synchronous_native;
                            } else {
                                if (!call_guest_from(c, frame, 0x8004C828u, regs, kPrs, data, dst)) return true;
                                result = c.gpr[3];
                            }
                        } else {
                            const u32 size = c.mem_read32(entry + eStoredSize);
                            if (game_memory_ != nullptr && game_memory_->move_for_native(c, dst, data, size)) {
                                ++stats_.stored_native;
                            } else if (!call_guest_from(c, frame, 0x8004C840u, regs, kMemcpy, dst, data, size)) {
                                return true;
                            }
                            if (callback != 0u &&
                                !call_guest_from(c, frame, 0x8004C858u, regs, kFileNotice, callback, size, userdata))
                                return true;
                            result = size;
                        }
                    }
                }
                c.gpr[1] = sp;
                c.lr = link;
                c.gpr[3] = result;
                return true;
            },
            "native expand jobs: the ONE extract, compressed members with a callback queued (phase 2)");
    }

    // W2 (mode 5): TBGReadManager::update without a TBGRead.
    if (w2_) {
        cpu.install_hle(
            kReadUpdate,
            [this](Cpu& c) {
                const u32 manager = c.gpr[3], sp = c.gpr[1], link = c.lr;
                ++stats_.read_updates;
                const auto back = [&] {
                    c.gpr[1] = sp;
                    c.lr = link;
                    return true;
                };
                // The original's frame: 0x20 bytes, r29..r31 saved at +0x14.
                Regs regs = regs_of(c);
                const u32 frame = prologue(c, sp, link, 0x20u);
                save_regs(c, frame + 0x14u, 29u, regs);
                if (c.mem_read32(kInFlight) != 0u || c.mem_read8(kPause) != 0u) return back();
                u32 list = 0u;
                while (list < 3u && c.mem_read32(kReadLists + list * 4u) == 0u) ++list;
                reg(regs, 31) = list;
                if (list >= 3u) {
                    if (!call_guest_from(c, frame, 0x8004544Cu, regs, kFileLoading, manager, 0u)) return true;
                    return back();
                }
                if (!call_guest_from(c, frame, 0x80045458u, regs, kFileLoading, manager, 1u)) return true;
                const u32 request = c.mem_read32(kReadLists + list * 4u) + 4u;
                reg(regs, 29) = request;
                if (!call_guest_from(c, frame, 0x80045480u, regs, kOpen, c.mem_read32(request), 1u, 0u, request))
                    return true;
                const u32 file = c.gpr[3];
                reg(regs, 30) = file;
                if (file != 0u) {
                    if (c.mem_read32(request + 4u) == 0u) {
                        const u32 size = (c.mem_read32(file + 4u) + 0x1Fu) & ~0x1Fu;
                        c.mem_write32(request + 8u, size);
                        if (!call_guest_from(c, frame, 0x800454B8u, regs, kRwAllocate, size, 2u)) return true;
                        c.mem_write32(request + 4u, c.gpr[3]);
                    }
                    Job job{0u, 0u, 0u, 0u, {}};
                    job.kind = Kind::Read;
                    for (u32 i = 0; i < 6u; ++i) job.request[i] = c.mem_read32(request + i * 4u);
                    job.file = file;
                    queue_.push_back(std::move(job));
                    c.mem_write32(kInFlight, c.mem_read32(kInFlight) + 1u);  // the TBGRead ctor's
                    // That ctor saves its return address in this frame's LR slot (its frames below are not made).
                    c.mem_write32(frame + 4u, 0x800454DCu);
                    ++stats_.reads_queued;
                }
                reg(regs, 30) = list * 4u;
                reg(regs, 31) = kReadLists;
                if (const u32 head = c.mem_read32(kReadLists + list * 4u); head != 0u) {
                    const u32 next = c.mem_read32(head);
                    reg(regs, 29) = next;
                    if (!call_guest_from(c, frame, 0x80045508u, regs, kRwFree, head)) return true;
                    c.mem_write32(kReadLists + list * 4u, next);
                }
                return back();
            },
            "native expand jobs: TBGReadManager::update, reads queued without a TBGRead (W2)");
    } else if (census_ != nullptr) {
        // The census line of a read the original's worker completed: at its completion Task's ctor (declines).
        cpu.install_hle(
            kFileControlCtor,
            [this](Cpu& c) {
                u32 words[6];
                for (u32 i = 0; i < 6u; ++i) words[i] = c.mem_read32(c.gpr[6] + i * 4u);
                census_read(c, c.gpr[7], words);
                return false;
            },
            "native expand jobs: the read census at TFileControlAsycManager's ctor");
    }

    // The base constructor for TExpandBG only: the original's writes and calls in its order, no OSThread.
    cpu.install_hle(
        kThreadCtor,
        [this](Cpu& c) {
            if (c.lr != kFromExpandCtor) return false;  // TBGRead and any other caller: the original constructor
            const u32 worker = c.gpr[3];
            const u32 size = c.gpr[4] != 0u ? c.gpr[4] : kDefaultStack;
            const u32 sp = c.gpr[1], link = c.lr;
            c.mem_write32(worker + wVtable, kThreadVtable);
            c.mem_write32(worker + wSlot, 0xFFFFFFFFu);
            c.mem_write8(worker + wRunning, 1u);
            c.mem_write8(worker + wFlag5, 0u);
            c.mem_write8(worker + wDone, 0u);
            c.mem_write32(worker + wStack, 0u);
            c.mem_write32(worker + wPriority, 0x10u);
            if (!call_guest(c, kAllocate, size)) return true;
            const u32 stack = c.gpr[3];
            c.mem_write32(worker + wStack, stack);
            if (!call_guest(c, kMemset, stack, 0xCDu, 0x10u)) return true;
            // The thread that is not created: terminated state, the job's tag and creation stamp.
            const u32 thread = worker + wThread;
            const u64 now = c.game_clock().now();
            if (now != stamp_tick_) {
                stamp_tick_ = now;
                stamp_seq_ = 0u;
            }
            c.mem_write16(thread + tState, 0u);
            c.mem_write32(thread + tTag, kTag);
            c.mem_write32(thread + tTickHi, static_cast<u32>(now >> 32));
            c.mem_write32(thread + tTickLo, static_cast<u32>(now));
            c.mem_write32(thread + tSeq, stamp_seq_++);
            if (!call_guest(c, kManager, 0u)) return true;
            const u32 manager = c.gpr[3];
            if (!call_guest(c, kRegister, manager, worker)) return true;
            c.mem_write32(worker + wSlot, c.gpr[3]);
            ++stats_.created;
            c.gpr[1] = sp;
            c.lr = link;
            c.gpr[3] = worker;
            return true;
        },
        "native expand jobs: Thread ctor for TExpandBG without an OSThread");

    // Publication: at the reaper's entry, every tagged worker not done, in creation order, as the entry runs it.
    cpu.install_hle(
        kReaper,
        [this](Cpu& c) {
            ++stats_.reaper_calls;
            if (phase2_) {
                bool done = false;
                if (!queue_.empty()) {
                    // The original ran this work on its workers' stacks: the main thread's stack is kept as it was.
                    const StackKeep keep(c, c.gpr[1]);
                    done = publish_queue(c);
                }
                ++iterations_;
                // Mode 3+ builds no TExpandBG, but a state saved in mode 1/2 can hold tagged workers that have no
                // OSThread: they are published below as mode 2 would, else they never finish and their loader waits
                // forever.
                if (done || !restored_tagged_) return done;
            }
            const u32 manager = c.gpr[3];
            const u32 table = c.mem_read32(manager);
            std::vector<std::tuple<u64, u32, u32>> jobs;  // (tick, sequence, worker)
            for (u32 i = 0; i < kSlots; ++i) {
                const u32 worker = c.mem_read32(table + i * 8u);
                if (c.mem_read32(table + i * 8u + 4u) == 0u || worker == 0u) continue;
                const u32 thread = worker + wThread;
                // A tag alone is not enough: a later worker built over a published one's memory by the original
                // constructor keeps that word (OSCreateThread does not clear it). The tag is cleared at publication,
                // and the worker must be a TExpandBG.
                if (c.mem_read32(thread + tTag) != kTag || c.mem_read32(worker + wVtable) != kExpandVtable ||
                    c.mem_read8(worker + wDone) != 0u)
                    continue;
                const u64 tick = (static_cast<u64>(c.mem_read32(thread + tTickHi)) << 32) | c.mem_read32(thread + tTickLo);
                jobs.emplace_back(tick, c.mem_read32(thread + tSeq), worker);
            }
            if (jobs.empty()) {
                restored_tagged_ = false;  // none left from a mode 1/2 state
                return false;
            }
            std::sort(jobs.begin(), jobs.end());
            const u32 saved[3] = {c.gpr[3], c.gpr[4], c.lr};
            for (const auto& [tick, seq, worker] : jobs) {
                // Phase 1 (mode 2): TExpandBG's step 0x80043D5C natively - its load-hold query is the native owner
                // that never holds; the expand src (+0x330) -> dst (+0x334) by the host decoder (from the stream the
                // host workers decoded ahead, or decoded here); the count into +0x33C; then the game's own notice
                // builder 0x80043DB0 (the completion, game code: the requester's callback). The step returns 0, so
                // the entry's loop ends. A stream the host decoder declines runs the game's step as in mode 1.
                bool native = false;
                if (phase1_ && game_memory_ != nullptr) {
                    const u32 src = c.mem_read32(worker + wSrc), dst = c.mem_read32(worker + wDst);
                    if (const std::optional<u32> size = game_memory_->expand_for_native(c, src, dst)) {
                        c.mem_write32(worker + wSize, *size);
                        if (census_ != nullptr)
                            std::fprintf(census_, "%llu %08X %08X %08X %08X %08X %08X\n",
                                         static_cast<unsigned long long>(c.game_clock().now()), src, dst, *size,
                                         c.mem_read32(worker + 0x338u), c.mem_read32(worker + 0x340u),
                                         fnv1a(c, dst, *size));
                        if (!call_guest(c, kNoticeBuilder, worker)) return true;
                        native = true;
                        ++stats_.expanded_natively;
                    } else {
                        ++stats_.expanded_by_guest;
                    }
                }
                // The worker entry 0x8035A01C: while (step() && running); running = 0; finish(); done = 1.
                bool again = !native;
                while (again && c.mem_read8(worker + wRunning) != 0u) {
                    const u32 vtable = c.mem_read32(worker + wVtable);
                    if (!call_guest(c, c.mem_read32(vtable + 0xCu), worker)) return true;
                    again = (c.gpr[3] & 0xFFu) != 0u;
                }
                c.mem_write8(worker + wRunning, 0u);
                const u32 vtable = c.mem_read32(worker + wVtable);
                const u32 finish = c.mem_read32(vtable + 0x10u);
                if (finish != kEmptyFinish && !call_guest(c, finish, worker)) return true;
                c.mem_write8(worker + wDone, 1u);
                c.mem_write32(worker + wThread + tTag, 0u);
                ++stats_.published;
            }
            c.gpr[3] = saved[0];
            c.gpr[4] = saved[1];
            c.lr = saved[2];
            return false;  // the reaper runs as before and deletes them
        },
        "native expand jobs: publication at the reaper (declines)");
    return true;
}

// Mode 3's publication, at the reaper's entry: each queued job in submission order as TExpandBG's step would finish
// it - the expand, then the notice as 0x80043DB0 builds it (no worker: its field +0x34 is 0).
bool NativeExpandJobs::publish_queue(Cpu& c) {
    if (queue_.empty()) return false;
    const u32 saved[3] = {c.gpr[3], c.gpr[4], c.lr};
    std::vector<Job> jobs;
    jobs.swap(queue_);  // jobs queued during the publication wait for the next reaper
    std::size_t next = 0;
    // A stopped run (a failed guest call) keeps the job it stopped in and the ones after it queued, ahead of any queued
    // meanwhile: a run that continues, or a state saved after it, still publishes them (and a W2 read's in-flight count
    // is still released).
    const auto stopped = [&] {
        queue_.insert(queue_.begin(), std::make_move_iterator(jobs.begin() + static_cast<std::ptrdiff_t>(next)),
                      std::make_move_iterator(jobs.end()));
        return true;
    };
    for (; next < jobs.size(); ++next) {
        Job& j = jobs[next];
        if (j.kind == Kind::Read) {
            if (!publish_read(c, j)) return stopped();
            continue;
        }
        u32 size = 0u;
        std::optional<u32> expanded;
        if (game_memory_ != nullptr && j.prepared.valid()) {
            // Phase 3: the decode-ahead when it has the stream; else the worker's result (waited for only now).
            const bool ready = j.prepared.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
            if (!ready) expanded = game_memory_->expand_for_native(c, j.src, j.dst, nullptr, false);
            if (!expanded) {
                if (!ready) {
                    const auto started = std::chrono::steady_clock::now();
                    j.prepared.wait();
                    const u64 ns = elapsed_ns(started);
                    ++stats_.waited;
                    stats_.wait_ns += ns;
                    stats_.wait_ns_max = std::max(stats_.wait_ns_max, ns);
                }
                const std::shared_ptr<const Prepared> prepared = j.prepared.get();
                if (!prepared->ok) ++stats_.worker_declined;
                expanded =
                    game_memory_->expand_for_native(c, j.src, j.dst, prepared->ok ? &prepared->decoded : nullptr);
            }
        } else {
            if (phase3_) ++stats_.unprepared;
            if (game_memory_ != nullptr) expanded = game_memory_->expand_for_native(c, j.src, j.dst);
        }
        if (expanded) {
            size = *expanded;
            ++stats_.expanded_natively;
        } else {
            if (!call_guest(c, kPrs, j.src, j.dst)) return stopped();
            size = c.gpr[3];
            ++stats_.expanded_by_owner;
        }
        if (census_ != nullptr)
            std::fprintf(census_, "%llu %08X %08X %08X %08X %08X %08X\n",
                         static_cast<unsigned long long>(c.game_clock().now()), j.src, j.dst, size, j.callback,
                         j.userdata, fnv1a(c, j.dst, size));
        if (!call_guest(c, kNew, kNoticeBytes)) return stopped();
        const u32 notice = c.gpr[3];
        if (notice != 0u) {
            if (!call_guest(c, kGroups, 0u)) return stopped();
            if (!call_guest(c, kGroup, c.gpr[3], 0u)) return stopped();
            if (!call_guest(c, kNoticeCtor, notice, 0u, c.gpr[3], j.callback, size, j.userdata)) return stopped();
        }
        ++stats_.published;
    }
    c.gpr[3] = saved[0];
    c.gpr[4] = saved[1];
    c.lr = saved[2];
    return false;  // the reaper runs as before
}

// W2: TBGRead's step for a queued read, then its delete's in-flight decrement. False when the run stopped.
bool NativeExpandJobs::publish_read(Cpu& c, Job& j) {
    // The step 0x80045680: the load-hold query is skipped (its owner answers 0). The read: the destination is the
    // request's (allocated by the update); the loop allocates only when it is 0, as 0x80045150 does.
    if (j.file != 0u) {
        if (j.request[1] == 0u) {
            if (!call_guest(c, kRwAllocate, c.mem_read32(j.file + 4u), 2u)) return false;
            j.request[1] = c.gpr[3];
        }
        const auto started = std::chrono::steady_clock::now();
        const std::optional<u32> total = chunked_read_(c, j.file, j.request[1], kReadChunk);
        const u64 ns = elapsed_ns(started);
        stats_.read_ns += ns;
        stats_.read_ns_max = std::max(stats_.read_ns_max, ns);
        if (!total) return false;
        j.request[2] = *total;
        stats_.read_bytes += *total;
    }
    census_read(c, j.file, j.request.data());
    // The completion Task, as 0x80045890 builds it.
    if (!call_guest(c, kNew, kFileControlBytes)) return false;
    if (const u32 task = c.gpr[3]; task != 0u) {
        u32 holder = c.mem_read32(kReadManagerHolder);
        if (holder == 0u) {
            if (!call_guest(c, kReadManagerGetter, 0u)) return false;
            holder = c.gpr[3];
        }
        if (!call_guest(c, kTaskCtor, task, c.mem_read32(holder))) return false;
        c.mem_write32(task + tVtable, kFileControlVtable);
        c.mem_write32(task + tWorker, 0u);
        c.mem_write32(task, c.mem_read32(kFileControlWord0));
        c.mem_write16(task + tFlags, static_cast<u16>(c.mem_read16(task + tFlags) | 0x100u));
        for (u32 i = 0; i < 6u; ++i) c.mem_write32(task + tRequest + i * 4u, j.request[i]);
        c.mem_write32(task + tFile, j.file);
        c.mem_write32(task + tWorker, 0u);  // no worker: the update only clears this word
        c.mem_write16(task + tFlags, static_cast<u16>(c.mem_read16(task + tFlags) | 0x8000u));
    }
    c.mem_write32(kInFlight, c.mem_read32(kInFlight) - 1u);  // the TBGRead dtor's, at the reaper's delete
    ++stats_.reads_published;
    ++stats_.published;
    return true;
}

void NativeExpandJobs::census_read(Cpu& c, u32 file, const u32* words) {
    if (census_ == nullptr) return;
    // The hash covers the file's bytes (its length, +4), not the rounding to 32 after them: those bytes are whatever the
    // heap held there.
    const u32 length = file != 0u ? std::min(c.mem_read32(file + 4u), words[2]) : 0u;
    std::fprintf(census_, "R %llu %08X %08X %08X %08X %08X %08X %08X %08X\n",
                 static_cast<unsigned long long>(iterations_), file, words[1], words[2], words[0], words[3], words[4],
                 words[5], fnv1a(c, words[1], length));
}

// Phase 3: the worker decodes the copies in submission order; it touches no guest memory.
NativeExpandJobs::PreparedResult NativeExpandJobs::submit(std::shared_ptr<Prepared> prepared) {
    std::packaged_task<std::shared_ptr<const Prepared>()> task([prepared = std::move(prepared)]() {
        // The member's size first; larger hints only for a stream whose output runs past it. A failure here (an
        // allocation) only declines the job: the game thread then decodes it, instead of the exception being rethrown
        // through translated frames at the publication.
        try {
            for (const u32 hint : {prepared->size, 0x10000u, 0x100000u, 0x800000u}) {
                if (hint < prepared->size) continue;
                PrsDecoded d;
                bool hint_short = false;
                if (!prs_decode_ahead(prepared->bytes.data(), prepared->bytes.size(), hint, d, &hint_short)) {
                    if (!hint_short) break;  // an input end or a copy from before the output: no hint helps
                    continue;
                }
                prepared->decoded = std::move(d);  // its stream pointer stays in prepared->bytes
                prepared->ok = true;
                break;
            }
        } catch (const std::exception&) {
            prepared->decoded = PrsDecoded{};
            prepared->ok = false;
        }
        return std::shared_ptr<const Prepared>(prepared);
    });
    PreparedResult result = task.get_future().share();
    {
        std::lock_guard<std::mutex> lock(work_mutex_);
        work_queue_.push_back(std::move(task));
    }
    work_wake_.notify_one();
    return result;
}

void NativeExpandJobs::work(std::stop_token stop) {
    for (;;) {
        std::packaged_task<std::shared_ptr<const Prepared>()> task;
        {
            std::unique_lock<std::mutex> lock(work_mutex_);
            if (!work_wake_.wait(lock, stop, [this] { return !work_queue_.empty(); })) return;  // stopping, nothing left
            if (stop.stop_requested()) return;  // shutting down: queued decodes are not waited for
            task = std::move(work_queue_.front());
            work_queue_.pop_front();
        }
        task();
    }
}

void NativeExpandJobs::save_state(std::vector<u8>& out) const {
    if (!queue_.empty()) std::fprintf(stderr, "native expand jobs: saved with %zu queued job(s)\n", queue_.size());
    state::put<u32>(out, static_cast<u32>(queue_.size()));
    // Version 30: the mode 1/2 creation stamp, so jobs created after a load at the saved tick keep creation order.
    state::put<u64>(out, stamp_tick_);
    state::put<u32>(out, stamp_seq_);
    for (const Job& j : queue_) {
        state::put<u32>(out, static_cast<u32>(j.kind));
        if (j.kind == Kind::Read) {
            for (const u32 w : j.request) state::put<u32>(out, w);
            state::put<u32>(out, j.file);
            continue;
        }
        state::put<u32>(out, j.src);
        state::put<u32>(out, j.dst);
        state::put<u32>(out, j.callback);
        state::put<u32>(out, j.userdata);
    }
}

bool NativeExpandJobs::load_state(const u8*& cursor, const u8* end, u32 version) {
    u32 count = 0;
    if (!state::get<u32>(cursor, end, count) || count > 0x10000u) return false;
    u64 stamp_tick = 0u;
    u32 stamp_seq = 0u;
    if (version >= 30u && (!state::get<u64>(cursor, end, stamp_tick) || !state::get<u32>(cursor, end, stamp_seq)))
        return false;
    if (count != 0u && !phase2_) {
        std::fprintf(stderr, "native expand jobs: the state has %u queued jobs; it needs GEKKO_NATIVE_EXPAND_JOBS=3\n",
                     count);
        return false;
    }
    std::vector<Job> jobs(count);
    for (Job& j : jobs) {
        u32 kind = 0u;
        if (version >= 29u && !state::get<u32>(cursor, end, kind)) return false;
        if (kind == static_cast<u32>(Kind::Read)) {
            if (!w2_) {
                std::fprintf(stderr, "native expand jobs: the state has a queued read; it needs GEKKO_NATIVE_EXPAND_JOBS=5\n");
                return false;
            }
            j.kind = Kind::Read;
            for (u32& w : j.request)
                if (!state::get<u32>(cursor, end, w)) return false;
            if (!state::get<u32>(cursor, end, j.file)) return false;
            continue;
        }
        if (kind != static_cast<u32>(Kind::Expand)) return false;
        if (!state::get<u32>(cursor, end, j.src) || !state::get<u32>(cursor, end, j.dst) ||
            !state::get<u32>(cursor, end, j.callback) || !state::get<u32>(cursor, end, j.userdata))
            return false;
    }
    queue_ = std::move(jobs);
    stamp_tick_ = stamp_tick;
    stamp_seq_ = stamp_seq;
    restored_tagged_ = true;  // the restored memory may hold tagged mode 1/2 workers: the next reaper looks
    // Decodes queued for the discarded timeline: nobody waits for them any more; drop the ones not started.
    {
        std::lock_guard<std::mutex> lock(work_mutex_);
        work_queue_.clear();
    }
    return true;
}

}  // namespace gekko
