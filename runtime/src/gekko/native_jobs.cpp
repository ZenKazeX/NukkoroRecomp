// The game's background loaders as native jobs. Contract and rationale: include/gekko/native_jobs.h,
// docs/NATIVE_THREADS.md (stage A).

#include "gekko/native_jobs.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string>

#include "gekko/cpu.h"
#include "gekko/native_cri.h"
#include "gekko/savestate.h"

namespace gekko {

namespace {

// The thread API calls a worker's life goes through (PAL DOL).
constexpr u32 kCreateThread = 0x80378A44u;   // OSCreateThread(thread, func, param, stack, size, prio, attr)
constexpr u32 kResumeThread = 0x80378ECCu;   // OSResumeThread(thread) -> previous suspend count
constexpr u32 kIsTerminated = 0x80378498u;   // OSIsThreadTerminated(thread) (Thread::dtor's test)
constexpr u32 kCancelThread = 0x80378D10u;   // OSCancelThread(thread)
constexpr u32 kYieldThread = 0x80378A08u;    // OSYieldThread()

// SonicteamUSA::System::Thread (ctor 0x8035A15C, entry 0x8035A01C): the fields the entry uses.
constexpr u32 kRunning = 0x4u;   // byte: keep stepping
constexpr u32 kDone = 0x6u;      // byte: finished (the manager 0x8035A2DC reaps it)
constexpr u32 kVtable = 0x328u;  // vtable pointer of the worker's interface
constexpr u32 kStepSlot = 0xCu;
constexpr u32 kFinishSlot = 0x10u;

constexpr u32 kRunnerFrame = 0x40u;  // below the yield caller's frame: the jobs' stack frames start here

void stop(Cpu& c, const std::string& why) { c.request_stop(StopReason::ExecutionFault, "native jobs: " + why); }

}  // namespace

NativeJobs::Job* NativeJobs::find(u32 thread) {
    for (Job& j : jobs_)
        if (j.thread == thread) return &j;
    return nullptr;
}

std::size_t NativeJobs::pending() const noexcept {
    return static_cast<std::size_t>(
        std::count_if(jobs_.begin(), jobs_.end(), [](const Job& j) { return j.state == State::Pending; }));
}

void NativeJobs::call(Cpu& c, u32 function, u32 r3, u32 link) {
    c.gpr[3] = r3;
    c.gpr[12] = function;
    c.ctr = function;
    c.lr = link;
    c.leave_to_dispatcher(function, FallbackReason::Yield, link);
}

// The worker entry's body for the next pending job: skipped when the object is null or already done (the entry's
// two early exits); the step only while the running byte is set; then running = 0, finish, done = 1.
void NativeJobs::run_next(Cpu& c) {
    for (Job& j : jobs_) {
        if (j.state != State::Pending) continue;
        if (j.worker == 0u || c.mem_read8(j.worker + kDone) != 0u) {
            j.state = State::Done;  // the original entry exits at once
            continue;
        }
        j.state = State::Running;
        current_ = j.thread;
        job_start_ns_ = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (c.mem_read8(j.worker + kRunning) == 0u) {
            c.gpr[3] = 0u;  // as if a step had returned false
            served_step_return(c);
            return;
        }
        call(c, c.mem_read32(c.mem_read32(j.worker + kVtable) + kStepSlot), j.worker, kStepReturn);
        return;
    }
    // No job left: return from the yield that started the runner.
    running_ = false;
    current_ = 0u;
    c.gpr[1] = return_sp_;
    c.lr = return_lr_;
}

void NativeJobs::served_step_return(Cpu& c) {
    Job* j = find(current_);
    if (j == nullptr || j->state != State::Running) {
        stop(c, "a step returned with no running job");
        return;
    }
    if ((c.gpr[3] & 0xFFu) != 0u && c.mem_read8(j->worker + kRunning) != 0u) {
        // The original yields here and steps again; with no other thread to run, step again at once.
        call(c, c.mem_read32(c.mem_read32(j->worker + kVtable) + kStepSlot), j->worker, kStepReturn);
        return;
    }
    c.mem_write8(j->worker + kRunning, 0u);
    call(c, c.mem_read32(c.mem_read32(j->worker + kVtable) + kFinishSlot), j->worker, kFinishReturn);
}

void NativeJobs::install() {
    Cpu& cpu = cpu_;
    // OFF by default (2026-11-02 A/B, docs/NATIVE_THREADS.md): with the bridge on, the Westopolis replay created one
    // job that never ran - during the stage loading screen the game thread waits instead of passing its yield point,
    // so the load never completed (the loading screen stayed up). SHADOW_NATIVE_JOBS=1 enables it for diagnosis only.
    // The continuation entries stay installed (extract/native-owners.txt lists them, startup requires an owner).
    const char* env = std::getenv("SHADOW_NATIVE_JOBS");
    enabled_ = env != nullptr && env[0] == '1';
    cpu.install_hle(
        kCreateThread,
        [this](Cpu& c) {
            if (cri_ != nullptr && cri_->create_thread(c)) return true;
            if (!enabled_ || c.gpr[4] != kWorkerEntry) {
                ++stats_.declined;
                return false;  // not a game worker (CRI's): the guest OS creates it (stage B)
            }
            if (Job* existing = find(c.gpr[3])) {
                if (existing->state == State::Running) {
                    stop(c, "a worker's thread object was reused while its job runs");
                    return true;
                }
                *existing = Job{c.gpr[3], c.gpr[5], State::Pending};
            } else {
                jobs_.push_back(Job{c.gpr[3], c.gpr[5], State::Pending});
            }
            // Created suspended, as the original; OSResumeThread releases it. Pending until then is harmless: the
            // ctor always resumes right after creating (0x8035A1FC).
            ++stats_.created;
            c.gpr[3] = 1u;  // TRUE
            return true;
        },
        "native threads: OSCreateThread (CRI servers, game workers)");
    cpu.install_hle(
        kResumeThread,
        [this](Cpu& c) {
            if (cri_ != nullptr && cri_->resume_thread(c)) return true;
            Job* j = find(c.gpr[3]);
            if (j == nullptr) return false;
            c.gpr[3] = j->state == State::Pending ? 1u : 0u;  // the previous suspend count
            return true;
        },
        "native threads: OSResumeThread (CRI servers, game workers)");
    cpu.install_hle(
        kIsTerminated,
        [this](Cpu& c) {
            Job* j = find(c.gpr[3]);
            if (j == nullptr) return false;
            const bool done = j->state == State::Done;
            c.gpr[3] = done ? 1u : 0u;
            if (done) jobs_.erase(jobs_.begin() + (j - jobs_.data()));  // the dtor's test: the object goes now
            return true;
        },
        "native jobs: OSIsThreadTerminated (game workers)");
    cpu.install_hle(
        kCancelThread,
        [this](Cpu& c) {
            Job* j = find(c.gpr[3]);
            if (j == nullptr) return false;
            if (j->state == State::Running) {
                stop(c, "a running job was cancelled");
                return true;
            }
            jobs_.erase(jobs_.begin() + (j - jobs_.data()));
            ++stats_.cancelled;
            return true;
        },
        "native jobs: OSCancelThread (game workers)");
    cpu.install_hle(
        kYieldThread,
        [this](Cpu& c) {
            if (running_) {
                // A yield inside a job (the RtGCNFS read's per-chunk yield): nothing else to run.
                ++stats_.yields_inside_jobs;
                return true;
            }
            if (pending() == 0u) return false;  // the guest yield for any other thread (stage B)
            running_ = true;
            ++stats_.runs;
            return_lr_ = c.lr;
            return_sp_ = c.gpr[1];
            c.gpr[1] = return_sp_ - kRunnerFrame;
            c.mem_write32(c.gpr[1], return_sp_);  // back chain
            run_next(c);
            return true;
        },
        "native jobs: OSYieldThread (runs the game workers' jobs)");
    cpu.install_hle(
        kStepReturn,
        [this](Cpu& c) {
            served_step_return(c);
            return true;
        },
        "native jobs: step return");
    cpu.install_hle(
        kFinishReturn,
        [this](Cpu& c) {
            Job* j = find(current_);
            if (j == nullptr || j->state != State::Running) {
                stop(c, "a finish returned with no running job");
                return true;
            }
            c.mem_write8(j->worker + kDone, 1u);
            j->state = State::Done;
            ++stats_.completed;
            const std::uint64_t ns = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
                    .count() - job_start_ns_);
            stats_.job_ns_total += ns;
            stats_.job_ns.push_back(ns);
            if (ns > stats_.job_ns_max) {
                stats_.job_ns_max = ns;
                stats_.job_max_kind = c.mem_read32(j->worker + kVtable);
            }
            run_next(c);
            return true;
        },
        "native jobs: finish return");
}

void NativeJobs::save_state(std::vector<u8>& out) const {
    state::put<u32>(out, static_cast<u32>(jobs_.size()));
    for (const Job& j : jobs_) {
        state::put<u32>(out, j.thread);
        state::put<u32>(out, j.worker);
        state::put<u8>(out, static_cast<u8>(j.state));
    }
    state::put<u8>(out, running_ ? 1u : 0u);
    state::put<u32>(out, current_);
    state::put<u32>(out, return_lr_);
    state::put<u32>(out, return_sp_);
}

bool NativeJobs::load_state(const u8*& cursor, const u8* end) {
    u32 count = 0;
    if (!state::get<u32>(cursor, end, count) || count > 0x10000u) return false;
    std::vector<Job> jobs(count);
    for (Job& j : jobs) {
        u8 s = 0;
        if (!state::get<u32>(cursor, end, j.thread) || !state::get<u32>(cursor, end, j.worker) ||
            !state::get<u8>(cursor, end, s) || s > 2u)
            return false;
        j.state = static_cast<State>(s);
    }
    u8 running = 0;
    if (!state::get<u8>(cursor, end, running) || !state::get<u32>(cursor, end, current_) ||
        !state::get<u32>(cursor, end, return_lr_) || !state::get<u32>(cursor, end, return_sp_))
        return false;
    // Jobs saved by the bridge run only with it (its yield hook would otherwise run them in a process without it).
    if ((running != 0u || !jobs.empty()) && !enabled_) {
        std::fprintf(stderr, "native jobs: the state has bridge jobs; it needs SHADOW_NATIVE_JOBS=1\n");
        return false;
    }
    jobs_ = std::move(jobs);
    running_ = running != 0u;
    return true;
}

}  // namespace gekko
