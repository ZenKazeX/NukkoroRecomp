#pragma once

// The game's background loaders as native jobs (docs/NATIVE_THREADS.md, stage A). Not a scheduler.
//
// The game's only own threads are SonicteamUSA::System::Thread workers (TBGRead, TExpandBG): a worker is created,
// runs `while (step() && running) yield(); running = 0; finish(); done = 1;` once and is reaped by the game's manager.
// Here such a worker creates no GameCube thread: OSCreateThread with the worker entry (0x8035A01C) registers a job,
// and the job runs on the game thread itself at the game thread's next OSYieldThread (RenderStart::update, once per
// frame), jobs in creation order, each to completion: step (vtable +0xC) until it returns false, then finish
// (vtable +0x10), then the done byte - the same calls the worker made, without its thread. A yield made inside a
// running job (the RtGCNFS read yields per chunk) returns at once: there is no other thread to run.
// OSResumeThread / OSIsThreadTerminated / OSCancelThread on a job's thread answer from the job. Every other thread
// (CRI's) is left to the guest OS for now (stage B): those calls decline.
//
// Deliberate difference (decided by an A/B replay, not assumed away): a job completes atomically at the first yield
// point after its creation, where the original worker could advance at earlier sleep points and interleaved with
// the game thread at its read's chunk yields; only the completion is observable (a Task on the main task tree).

#include <cstdint>
#include <vector>

#include "gekko/types.h"

namespace gekko {

class Cpu;
class NativeCri;

class NativeJobs {
public:
    explicit NativeJobs(Cpu& cpu) : cpu_(cpu) {}
    void install();
    // CRI's server threads share the thread API owners installed here (native_cri.h): routed first.
    void set_cri(NativeCri* cri) noexcept { cri_ = cri; }

    struct Stats {
        std::uint64_t created = 0, completed = 0, cancelled = 0, runs = 0, yields_inside_jobs = 0, declined = 0;
        // Host wall time of each job on the game thread (step start -> finish return): the stall the bridge causes.
        std::uint64_t job_ns_total = 0, job_ns_max = 0;
        u32 job_max_kind = 0;  // the worker's interface vtable (TBGRead / TExpandBG) of the slowest job
        std::vector<std::uint64_t> job_ns;  // every job, for p99 in the exit report
    };
    const Stats& stats() const noexcept { return stats_; }
    bool enabled() const noexcept { return enabled_; }
    std::size_t pending() const noexcept;

    void save_state(std::vector<u8>& out) const;
    bool load_state(const u8*& cursor, const u8* end);

    // The game's worker entry and the two guest-visible continuation entries (inside SetInterruptMask's extent,
    // listed in extract/native-owners.txt).
    static constexpr u32 kWorkerEntry = 0x8035A01Cu;
    static constexpr u32 kStepReturn = 0x80376218u;
    static constexpr u32 kFinishReturn = 0x8037621Cu;

private:
    enum class State : u8 { Pending = 0, Running = 1, Done = 2 };
    struct Job {
        u32 thread = 0;  // the OSThread address the game passed (its handle); never written here
        u32 worker = 0;  // the Thread object (r5 of OSCreateThread = the entry's argument)
        State state = State::Pending;
    };
    Job* find(u32 thread);
    void run_next(Cpu& c);   // call the next pending job's step, or return from the yield
    void served_step_return(Cpu& c);  // a step returned (r3): step again, or call finish
    void call(Cpu& c, u32 function, u32 r3, u32 link);

    Cpu& cpu_;
    NativeCri* cri_ = nullptr;
    std::vector<Job> jobs_;  // creation order
    bool enabled_ = false;   // SHADOW_NATIVE_JOBS=1 turns the bridge on (diagnosis only; see install())
    bool running_ = false;   // the runner is between a yield's entry and its return
    u32 current_ = 0;        // the running job's thread
    u32 return_lr_ = 0;      // where the yield returns to
    u32 return_sp_ = 0;      // the yield caller's stack pointer
    std::int64_t job_start_ns_ = 0;  // host steady clock at the running job's first step
    Stats stats_;
};

}  // namespace gekko
