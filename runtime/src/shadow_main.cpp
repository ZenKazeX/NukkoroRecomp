#include "gekko/cpu_diag.h"
#include "gekko/thread_name.h"  // ES-T1
#include "gekko/self_sampler.h"
#include "gekko/display.h"
#include "gekko/display_settings.h"
#include "gekko/native_options_display.h"
#include "gekko/render_geometry.h"
#include "gekko/ui_test.h"
#include "gekko/host_window.h"
#include "gekko/audio_runtime.h"
#if !defined(SHADOW_RENDER_LIFTED)  // G5 (HW_REPORT T.1): the legacy renderer is not in the lifted build
#include "gekko/native_renderer.h"
#else
#include "gekko/native_gpu.h"  // G5: NativeGpu::Stats only (the telemetry row's legacy columns: zero here)
#endif
#include "gekko/native_assets.h"
#include "gekko/texture_overrides.h"
#include "native_memory.h"
#if !defined(SHADOW_RENDER_LIFTED)  // G5
#include "gekko/rw_capture.h"
#endif
#include "gekko/audio_audit.h"
#include "gekko/host_stats.h"
#include "gekko/host_jobs.h"
#include "gekko/host_timing.h"
#include "gekko/profiling.h"
#include "gekko/recomp.h"
#include "gekko/savestate.h"
#include "gekko/dev_link.h"
#include "gekko/guest_memory.h"
#include "gekko/input_tape.h"
#include "gekko/asset_trace.h"
#include "gekko/measure_probes.h"
#include "gekko/system.h"
#include "gekko/xinput_pad.h"
#if defined(SHADOW_RENDER_LIFTED)
#include "gekko/lifted_render.h"
#if defined(SHADOW_RENDER_LIFTED)
#include "render/display_modes.h"
#endif
#endif
#include "embedded_game_data.h"
#include "native_bootstrap.h"

static_assert(shadow::kProgramImageBase == gekko::GuestMemory::kImageBase &&
                  shadow::kProgramImageEnd == gekko::GuestMemory::kImageEnd,
              "the bootstrap's program image is the game window's image region");

static_assert(shadow::kProgramImageBase == gekko::GuestMemory::kImageBase &&
                  shadow::kProgramImageEnd == gekko::GuestMemory::kImageEnd,
              "the bootstrap's program image is the game window's image region");
#include "native_pc_boot_contract.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <memory>
#include <map>
#include <set>
#include <utility>
#include <vector>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#ifndef QUOTA_LIMITS_HARDWS_MIN_ENABLE
#define QUOTA_LIMITS_HARDWS_MIN_ENABLE 0x00000001
#endif
#pragma comment(lib, "psapi.lib")
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#endif

namespace {

struct GameThreadPerformancePolicy {
    bool applied = false;  // the policy ran on the thread that executes translated code (windowed or headless)
    bool priority_applied = false;
    bool priority_boosts_enabled = false;
    bool execution_speed_throttling_disabled = false;
    // This process's own execution-speed throttling (EcoQoS) opted out: a property of this process only; no system
    // setting is read or changed.
    bool process_throttling_disabled = false;
    int effective_priority = 0;
};

// Where the game thread actually ran (R8, ES 2026-10-20): sampled once per host service (every 5 ms of game time)
// with GetCurrentProcessorNumberEx and classified by the CPU set's EfficiencyClass. On a hybrid CPU the performance
// cores have the highest class; on a non-hybrid CPU every core has class 0 and every sample counts as performance.
// Measurement only: nothing is pinned and nothing reads it back.
class GameThreadPlacement {
public:
    GameThreadPlacement() {
#ifdef _WIN32
        ULONG length = 0;
        GetSystemCpuSetInformation(nullptr, 0, &length, GetCurrentProcess(), 0);
        if (length == 0u) return;
        std::vector<unsigned char> buffer(length);
        auto* first = reinterpret_cast<SYSTEM_CPU_SET_INFORMATION*>(buffer.data());
        if (!GetSystemCpuSetInformation(first, length, &length, GetCurrentProcess(), 0)) return;
        unsigned char top = 0;
        for (ULONG offset = 0; offset + sizeof(DWORD) * 2u <= length;) {
            const auto* info = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(buffer.data() + offset);
            if (info->Size == 0u) break;
            if (info->Type == CpuSetInformation) {
                const unsigned group = info->CpuSet.Group;
                const unsigned index = info->CpuSet.LogicalProcessorIndex;
                if (group < kGroups && index < 64u) {
                    efficiency_[group][index] = info->CpuSet.EfficiencyClass;
                    known_[group] |= (1ull << index);
                    top = (std::max)(top, static_cast<unsigned char>(info->CpuSet.EfficiencyClass));
                }
            }
            offset += info->Size;
        }
        top_class_ = top;
        available_ = true;
#endif
    }
    // The game thread, once per host service.
    void sample() noexcept {
#ifdef _WIN32
        if (!available_) return;
        PROCESSOR_NUMBER number{};
        GetCurrentProcessorNumberEx(&number);
        ++samples_;
        if (number.Group < kGroups && number.Number < 64u && ((known_[number.Group] >> number.Number) & 1ull) != 0u) {
            if (efficiency_[number.Group][number.Number] == top_class_) ++performance_;
            seen_[number.Group] |= (1ull << number.Number);
        } else {
            ++unknown_;
        }
#endif
    }
    bool available() const noexcept { return available_; }
    std::uint64_t samples() const noexcept { return samples_; }
    std::uint64_t performance_samples() const noexcept { return performance_; }
    std::uint64_t unknown_samples() const noexcept { return unknown_; }
    double performance_percent() const noexcept {
        return samples_ == 0u ? 0.0 : 100.0 * static_cast<double>(performance_) / static_cast<double>(samples_);
    }
    bool hybrid() const noexcept { return top_class_ != 0u; }
    // The logical processors of group 0 the game thread was seen on.
    std::uint64_t seen_mask() const noexcept { return seen_[0]; }

private:
    static constexpr unsigned kGroups = 4;
    unsigned char efficiency_[kGroups][64]{};
    std::uint64_t known_[kGroups]{};
    std::uint64_t seen_[kGroups]{};
    unsigned char top_class_ = 0;
    bool available_ = false;
    std::uint64_t samples_ = 0, performance_ = 0, unknown_ = 0;
};

// Game time is the GameClock (docs/native-time-20261005/DESIGN.md): host speed changes only WHEN things happen in
// wall time, never what the game computes. Host performance is
// a different concern. The translated guest stream is necessarily serial, so
// make that one latency-sensitive thread explicit to the Windows scheduler and
// opt it out of execution-speed (EcoQoS) throttling. This is deliberately not
// REALTIME/HIGH priority and does not pin an affinity mask; the UI, audio device
// and worker pool must still run, and the scheduler remains free to choose the
// best performance core on hybrid CPUs.
//
// R8 (ES 2026-10-20): applied on BOTH paths - the windowed game thread and the headless main thread that executes
// translated code - so headless timings run under the same policy (before, headless reported thread_priority 0 and
// throttling on, and Windows placed it on E-cores at some times and not others). It also opts this process out of
// execution-speed throttling: a property of our own process, not a system setting.
GameThreadPerformancePolicy apply_game_thread_performance_policy() noexcept {
    GameThreadPerformancePolicy result;
    result.applied = true;
#ifdef _WIN32
#if defined(PROCESS_POWER_THROTTLING_CURRENT_VERSION) && defined(PROCESS_POWER_THROTTLING_EXECUTION_SPEED)
    {
        PROCESS_POWER_THROTTLING_STATE process_power{};
        process_power.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        process_power.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
        process_power.StateMask = 0u;
        result.process_throttling_disabled =
            SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &process_power,
                                  sizeof(process_power)) != FALSE;
    }
#endif
    HANDLE thread = GetCurrentThread();
    result.priority_applied =
        SetThreadPriority(thread, THREAD_PRIORITY_ABOVE_NORMAL) != FALSE;
    // FALSE means dynamic priority boosts are enabled.
    result.priority_boosts_enabled = SetThreadPriorityBoost(thread, FALSE) != FALSE;

#if defined(THREAD_POWER_THROTTLING_CURRENT_VERSION) && \
    defined(THREAD_POWER_THROTTLING_EXECUTION_SPEED)
    THREAD_POWER_THROTTLING_STATE power{};
    power.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
    power.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    power.StateMask = 0u;
    result.execution_speed_throttling_disabled =
        SetThreadInformation(thread, ThreadPowerThrottling, &power, sizeof(power)) != FALSE;
#endif
    result.effective_priority = GetThreadPriority(thread);
#endif
    return result;
}

template <std::size_t N>
std::string histogram_json(const gekko::Histogram<N>& histogram) {
    char text[320];
    std::snprintf(text, sizeof(text),
                  "{\"count\": %llu, \"mean_ms\": %.4f, \"p50_ms\": %.4f, \"p95_ms\": %.4f, "
                  "\"p99_ms\": %.4f, \"p999_ms\": %.4f, \"max_ms\": %.4f}",
                  static_cast<unsigned long long>(histogram.count), histogram.mean_ns() / 1.0e6,
                  histogram.percentile_ms(0.50), histogram.percentile_ms(0.95),
                  histogram.percentile_ms(0.99), histogram.percentile_ms(0.999),
                  static_cast<double>(histogram.max) / 1.0e6);
    return text;
}

template <std::size_t N>
std::string histogram_text(const gekko::Histogram<N>& histogram) {
    char text[224];
    std::snprintf(text, sizeof(text),
                  "%llu samples, mean %.3f, p50 %.3f, p95 %.3f, p99 %.3f, p99.9 %.3f, max %.3f ms",
                  static_cast<unsigned long long>(histogram.count), histogram.mean_ns() / 1.0e6,
                  histogram.percentile_ms(0.50), histogram.percentile_ms(0.95),
                  histogram.percentile_ms(0.99), histogram.percentile_ms(0.999),
                  static_cast<double>(histogram.max) / 1.0e6);
    return text;
}

// ---------------------------------------------------------------------------
// Host tuning switches.
//
// Each name below turns one host-side change from the 2026-09 resource pass on
// or off. They all default ON (except the two marked otherwise), so the shipped
// binary is the tuned one; --tune <name>=0 restores the previous behaviour for
// that change alone. Nothing here touches guest state: every target is a host
// implementation detail behind a setter whose owner argued its guest
// invisibility in code.
enum class TuneScope { System, Window };

struct TuneEntry {
    const char* name;
    int id;
    TuneScope scope;
    const char* what;
};

constexpr TuneEntry kTuneEntries[] = {
    // (The GX FIFO decode switches 2-5 and 44 went with the decoder: phase 4,
    // docs/native-time-20261005/GX_DATA_PLANE.md.)
    // Audio production (guest thread)
    {"ax-sample-resampling", 27, TuneScope::System, "resample ADPCM in sample units, not nibbles"},
    {"ax-adpcm-cursor", 28, TuneScope::System, "continuous per-voice ADPCM decode cursor"},
    // Storage. (The IPL-chip decode switch 45 went with the emulated external
    // interface: stage 5, docs/native-time-20261005/STAGE5_EXI.md.)
    {"persist-debounce", 29, TuneScope::System, "debounce memory-card image writes"},
    // Host jobs
    {"job-notify-one", 35, TuneScope::System, "wake one worker per queued chunk"},
    // Presenter / window
    {"narrow-queue-lock", 38, TuneScope::Window, "hold the present queue lock only across execute"},
    {"deferred-pin-release", 39, TuneScope::Window,
     "release the XFB pin when the blit retires (default on)"},
    {"keep-foreground", 40, TuneScope::Window, "restore and refocus the window while iconic (default off)"},
};

const TuneEntry* find_tune_entry(const std::string& name) noexcept {
    for (const TuneEntry& entry : kTuneEntries) {
        if (name == entry.name) return &entry;
    }
    return nullptr;
}

// Applies every tuning entry that belongs to this scope. Called once per scope
// as the objects come into existence; an entry whose scope never materialises
// (a window switch in a headless run) is reported by the caller, not silently
// dropped.
void apply_host_tuning(const std::vector<std::pair<std::string, bool>>& tuning, TuneScope scope,
                       gekko::System* system,
                       gekko::HostWindow* window, std::set<std::string>* applied) {
    for (const auto& item : tuning) {
        const TuneEntry* entry = find_tune_entry(item.first);
        if (entry == nullptr || entry->scope != scope) continue;
        const bool on = item.second;
        switch (entry->id) {
            case 27: system->set_ax_sample_resampling(on); break;
            case 28: system->set_ax_adpcm_cursor(on); break;
            case 29: system->card().set_persist_debounce(on); break;
            case 35: gekko::set_host_job_notify_one(on); break;
            case 38: window->set_plume_narrow_queue_lock(on); break;
            case 39: window->set_plume_deferred_pin_release(on); break;
            case 40: window->set_keep_foreground(on); break;
            default: continue;
        }
        if (applied != nullptr) applied->insert(item.first);
    }
}

// CPU time charged to the CALLING thread. The guest thread's own CPU time is
// the only way to tell whether renderer.guest_blocked accounts for everything
// the guest waits on: guest_blocked is subtracted from wall to get "execution
// time", so any wait NOT counted there is invisible - it hides inside the
// execution figure and inflates it. With this,
//
//     hidden_wait = (guest run wall - guest_blocked) - guest thread CPU
//
// and a hidden_wait near zero is proof the blocking taxonomy is complete,
// rather than an assumption that it is.
double thread_cpu_seconds_now() noexcept {
#ifdef _WIN32
    FILETIME created{}, exited{}, kernel{}, user{};
    if (GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user) == FALSE) {
        return 0.0;
    }
    ULARGE_INTEGER kernel_ticks{};
    kernel_ticks.LowPart = kernel.dwLowDateTime;
    kernel_ticks.HighPart = kernel.dwHighDateTime;
    ULARGE_INTEGER user_ticks{};
    user_ticks.LowPart = user.dwLowDateTime;
    user_ticks.HighPart = user.dwHighDateTime;
    return static_cast<double>(kernel_ticks.QuadPart + user_ticks.QuadPart) / 1.0e7;
#else
    return 0.0;
#endif
}

// Set by the thread that actually executes translated code, around its
// run() call only - so setup, disc mount and shutdown are excluded and the
// window is exactly the one guest_blocked accrues in. Negative means "not
// measured on this path" so a zero can never be mistaken for a measurement.
double g_guest_run_wall_seconds = -1.0;
double g_guest_run_cpu_seconds = -1.0;

double process_cpu_seconds_now() noexcept {
#ifdef _WIN32
    FILETIME created{}, exited{}, kernel{}, user{};
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) == FALSE) {
        return 0.0;
    }
    ULARGE_INTEGER kernel_ticks{};
    kernel_ticks.LowPart = kernel.dwLowDateTime;
    kernel_ticks.HighPart = kernel.dwHighDateTime;
    ULARGE_INTEGER user_ticks{};
    user_ticks.LowPart = user.dwLowDateTime;
    user_ticks.HighPart = user.dwHighDateTime;
    return static_cast<double>(kernel_ticks.QuadPart + user_ticks.QuadPart) / 1.0e7;
#else
    const std::clock_t ticks = std::clock();
    return ticks == static_cast<std::clock_t>(-1)
               ? 0.0
               : static_cast<double>(ticks) / static_cast<double>(CLOCKS_PER_SEC);
#endif
}

// Every thread of this process at one moment (OPTIMIZATION_PLAN A2): its OS description, the CPU cycles it has used
// (QueryThreadCycleTime: cycles charged to the thread, kernel and user, at the TSC rate) and its kernel / user time.
// Taken at the start and the end of the measured window; the report gives each thread's change over the window (a
// thread created inside it counts from zero). A thread that ended inside the window is not seen.
struct ThreadSample {
    unsigned long tid = 0;
    std::string name;
    unsigned long long cycles = 0;
    double kernel_seconds = 0.0, user_seconds = 0.0;
};

std::vector<ThreadSample> sample_threads() {
    std::vector<ThreadSample> threads;
#ifdef _WIN32
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return threads;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    const DWORD pid = GetCurrentProcessId();
    for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != pid) continue;
        const HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
        if (thread == nullptr) continue;
        ThreadSample sample;
        sample.tid = entry.th32ThreadID;
        ULONG64 cycles = 0;
        if (QueryThreadCycleTime(thread, &cycles)) sample.cycles = cycles;
        FILETIME created{}, exited{}, kernel{}, user{};
        if (GetThreadTimes(thread, &created, &exited, &kernel, &user)) {
            const auto seconds = [](const FILETIME& t) {
                return static_cast<double>((static_cast<unsigned long long>(t.dwHighDateTime) << 32) |
                                           t.dwLowDateTime) / 1.0e7;
            };
            sample.kernel_seconds = seconds(kernel);
            sample.user_seconds = seconds(user);
        }
        PWSTR description = nullptr;
        if (SUCCEEDED(GetThreadDescription(thread, &description)) && description != nullptr) {
            for (const wchar_t* c = description; *c != L'\0'; ++c)
                sample.name.push_back(*c < 0x80 && *c >= 0x20 && *c != L'"' && *c != L'\\' ? static_cast<char>(*c) : '?');
            LocalFree(description);
        }
        CloseHandle(thread);
        threads.push_back(std::move(sample));
    }
    CloseHandle(snapshot);
#endif
    return threads;
}

// The window's per-thread table as a JSON array, busiest first.
std::string thread_window_json(const std::vector<ThreadSample>& start, const std::vector<ThreadSample>& end,
                               double wall_seconds) {
    struct Row {
        const ThreadSample* thread;
        unsigned long long cycles;
        double kernel, user;
    };
    std::vector<Row> rows;
    for (const ThreadSample& e : end) {
        Row row{&e, e.cycles, e.kernel_seconds, e.user_seconds};
        for (const ThreadSample& s : start) {
            if (s.tid != e.tid || s.cycles > e.cycles) continue;
            row.cycles -= s.cycles;
            row.kernel -= s.kernel_seconds;
            row.user -= s.user_seconds;
            break;
        }
        rows.push_back(row);
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.cycles > b.cycles; });
    std::ostringstream json;
    json.setf(std::ios::fixed);
    json.precision(4);
    json << "[";
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const Row& r = rows[i];
        json << (i == 0 ? "" : ",") << "\n      {\"tid\": " << r.thread->tid << ", \"name\": \"" << r.thread->name
             << "\", \"cycles\": " << r.cycles << ", \"kernel_seconds\": " << r.kernel
             << ", \"user_seconds\": " << r.user << ", \"cpu_share_of_wall\": "
             << (wall_seconds > 0.0 ? (r.kernel + r.user) / wall_seconds : 0.0) << "}";
    }
    json << "\n    ]";
    return json.str();
}

// Pacing sleeps to the game clock's wall deadlines. That must not
// force a process-wide 1 ms multimedia timer: that keeps every core out of
// deep idle and is why a 13900K sits at 70 C while the game thread is mostly
// asleep. A high-resolution waitable timer is local to this wait.
struct RecoveredClockSleeper {
    RecoveredClockSleeper() {
#ifdef _WIN32
        timer_ = CreateWaitableTimerExW(nullptr, nullptr,
                                        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                        TIMER_ALL_ACCESS);
        if (timer_ == nullptr) {
            timer_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        }
#endif
    }
    ~RecoveredClockSleeper() {
#ifdef _WIN32
        if (timer_ != nullptr) {
            CloseHandle(timer_);
        }
#endif
    }
    void sleep_until(std::chrono::steady_clock::time_point deadline) {
#ifdef _WIN32
        if (timer_ != nullptr) {
            for (;;) {
                const auto now = std::chrono::steady_clock::now();
                if (now >= deadline) {
                    return;
                }
                const auto ns =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now)
                        .count();
                LARGE_INTEGER due;
                due.QuadPart = -((ns + 99) / 100);
                if (due.QuadPart >= 0) {
                    due.QuadPart = -1;
                }
                if (!SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) {
                    break;
                }
                WaitForSingleObject(timer_, INFINITE);
            }
        }
#endif
        std::this_thread::sleep_until(deadline);
    }

private:
#ifdef _WIN32
    HANDLE timer_ = nullptr;
#endif
};

struct Options {
    std::optional<std::filesystem::path> game_data;
    std::optional<std::filesystem::path> memory_card;
    bool boot = true;
    bool trace = false;
    bool profile = false;
    // Forensic resource lineage (--asset-trace). Off unless a path is given.
    std::optional<std::filesystem::path> asset_trace;
    std::string resource_lineage;  // substring filter over the resolved path
    bool divide_intrinsics = true;
    // The Metrowerks __dynamic_cast served natively (gekko/dynamic_cast_intrinsic.h); false runs the guest body (A/B).
    bool dynamic_cast_intrinsic = true;
    // RwMatrixMultiply served natively (gekko/matrix_multiply_intrinsic.h); false runs the guest body (A/B).
    bool matrix_multiply_intrinsic = true;
    bool verify_matrix_multiply = false;  // each native call also run by the guest body and compared
    // The CharaColli bounding tests served natively where they reject (gekko/chara_colli_intrinsic.h); false runs
    // the guest bodies (A/B).
    bool chara_colli_intrinsic = true;
    bool verify_chara_colli = false;  // each test also run by the guest body and compared
    bool chara_colli_loops = true;  // the two query loops too (false: the tests only)
    bool chara_colli_manage = true;  // the manager pass's vf 0x7C updates too
    bool trace_stale_contacts = false;  // diagnostic: reads of stale contact bytes
    bool chara_colli_pass = false;  // the manager's pair pass itself too (opt-in: not exact yet)
    bool chara_colli_bone_memo = true;  // GeneralBone runs reused (false: every update computed)
    bool chara_colli_bone_native = true;  // GeneralBone updates computed natively (false: by the guest)
    // Host FPR snapshot across exceptions. Off: the hardware does not do it and
    // it was measured to change nothing in 1,141,282 restores.
    bool fpr_shadow = false;
    bool window = true;
    bool xinput = true;
    bool pc_save_text = true;
    unsigned long console_type = 0x10000006ul;
    // The TV format the IPL leaves behind. This disc is PAL (GUPP8P) and "pal"
    // is the faithful setting: the guest derives its format from the VI display
    // configuration register, not from the IPL word alone (see
    // System::apply_boot_configuration), and with a PAL VI handed over it does
    // what the original does - loads palsel/PalSel*.txd and renders the 50/60 Hz
    // selection screen as the first thing on screen.
    //
    // This is the PAL disc GUPP8P, so PAL is the faithful setting and is the
    // default: it is what brings up the original 50/60 Hz selection screen.
    // "ntsc" hands the guest a VI with the display off so the SDK cold-starts
    // NTSC itself, which is what the recorded byte-identity hashes and every
    // recorded 640x480 2D/widescreen baseline were measured against.
    bool pal_boot = true;
    // Game milliseconds this run may cover (0 = no limit). Checked at every
    // host service (5 ms of game time), so the run ends at the first service
    // at or past it.
    unsigned long long budget_ms = 0ull;
    // Wall milliseconds the guest may execute without reaching a game-clock
    // wait point before the run stops with its location (0 = off).
    unsigned long wait_watchdog_ms = 0ul;
    std::optional<std::filesystem::path> symbols;
    std::vector<std::pair<unsigned long, unsigned long>> dumps;
    std::vector<unsigned long> watches;
    // Developer navigation: boot straight into a Select Mode stage (see
    // docs/tools/BOOT_STAGE.md) and list every stage the guest table knows.
    std::optional<gekko::u32> boot_stage;
    std::optional<gekko::u32> boot_event;
    bool boot_story = false;
    bool boot_flashback = false;
    bool list_stages = false;
    std::optional<std::filesystem::path> native_dump;
    gekko::u32 native_dump_first = 1;
    gekko::u32 native_dump_last = 3;
    bool native_validate_textures = false;
    // Guest-texel footprint reconstruction for resident EFB copies; see
    // native_shaders.hlsl sample_map_footprint. --no-effect-footprint disables it.
    bool effect_footprint = true;
    // Host-native asset manager (ONE/PRS/TXD loading straight from files/).
    // On with --native-renderer; --no-native-assets restores the pure
    // guest-captured texture supply for A/B runs.
    bool native_assets = true;
    // The native gameplay scheduler walks the guest's task tree in native code
    // (task bodies stay translated guest code). --legacy-scheduler runs the
    // guest's own traversal instead, for A/B comparison only.
#if defined(SHADOW_REFERENCE_RUNNER)
    bool legacy_scheduler = false;
#else
    static constexpr bool legacy_scheduler = false;
#endif
    gekko::u32 native_frames_in_flight = 2;
    bool native_gpu = true;
    gekko::u32 native_output_width = 640;
    gekko::u32 native_output_height = 480;
    bool native_output_explicit = false;
    // The lifted renderer's framing (docs/boundary-20261007/DESIGN.md): square pixels, or the game camera's own
    // horizontal extent (--framing game).
    bool game_framing = false;
    std::optional<std::filesystem::path> native_screenshot;
    gekko::u32 native_screenshot_frame = 0;
    gekko::u32 native_screenshot_stride = 0;
    gekko::u32 native_screenshot_count = 1;
    std::optional<std::filesystem::path> native_copy_dump;
    gekko::u32 native_copy_dump_frame = 0;
    // Where to write the bytes of a display list the geometry converter
    // refused. A refused list makes an object invisible for the whole session.
    std::optional<std::filesystem::path> geometry_failure_dump;
    unsigned long context_hook = 0;
    // Development only. Observe the recovered CRI Sofdec movie pipeline at the
    // guest's own call boundaries, and optionally stop the host from forcing
    // MWS_PLY_STAT_PLAYEND so the recovered player's real behaviour can be
    // seen. Neither changes a single guest instruction.
    // Host audio playback (AudioRuntime on the default WASAPI endpoint). Off by
    // default in a headless run: a measurement run should not open the
    // machine's sound device; the producer still runs and counts.
    std::optional<bool> audio;
    // Development: write the raw bytes the AI DMA engine hands out, so what the
    // guest is actually producing can be inspected rather than guessed at.
    // Big-endian signed 16-bit stereo at 32 kHz.
    std::optional<std::filesystem::path> audio_dump;
    std::optional<std::filesystem::path> movie_trace;
    // Development: write the decoded movie picture as <path>_NN.ppm, taken at
    // RwRasterUnlock where the converted pixels are still linear.
    std::optional<std::filesystem::path> movie_frame_dump;
    // Forcing MWS_PLY_STAT_PLAYEND compensated for a frozen Sofdec clock. The
    // AX stand-in advances that clock, so the movies now finish on their own.
    // Default off - it is a forgery, and it cuts a real movie short.
    std::optional<bool> sofdec_playend_stub;
    // Development only. Observe the PAL 50/60 Hz selection screen's own load
    // path. Observation only; installs no behaviour.
    bool ui_probe = false;
    // Development-only UI state driver (gekko/ui_test.h). Latches scripted
    // controller samples into the peripheral layer so the fade, loading, menu and HUD
    // states can be reached and looped without a human at the controller.
    std::optional<std::string> ui_test_mode;
    std::optional<std::filesystem::path> ui_test_script;
    // The before / after comparison workstation (tools/comparator, docs/comparator/README.md): --dev-link connects
    // to it (127.0.0.1:<port>), --dev-link-name is this run's role there, --start-paused waits for its "resume"
    // before the first instruction.
    std::optional<std::string> dev_link;
    std::string dev_link_name;
    bool start_paused = false;
    // Input tapes (gekko/input_tape.h): record every latched controller sample of the run, or latch them from a tape.
    std::optional<std::filesystem::path> input_tape_record;
    std::optional<std::filesystem::path> input_tape_play;
    // Ends the run at the traversal boundary after the host service at this absolute game tick - the boundary a
    // checkpoint was saved at (a tape's "stop" line). With --save-state it writes that checkpoint's savestate.
    gekko::u64 stop_at_tick = 0;
    // A JSON state report at the end of the run, with these guest ranges (address:length, comma separated).
    std::optional<std::filesystem::path> state_report;
    std::vector<std::string> report_peeks;
    // One-button comparison snapshots (docs/comparator/README.md "One-button snapshots"): the window's snapshot key
    // (F1..F12, a letter or a digit, or "none"; default F5) saves this exact moment - the savestate, the input tape
    // that led here, a state report and the next frame - into a new folder under --snapshot-dir (default snapshots/
    // beside the executable) and tells the workstation.
    std::string snapshot_key = "F5";
    // Replay markers (docs/tools/REPLAY_MARKERS.md): the key that marks a moment, and where the markers go (default:
    // beside the input tape being played or recorded, <tape>.markers.jsonl).
    std::string marker_key = "F8";
    std::string range_key = "F9";  // alternately opens and closes a marked range
    std::optional<std::filesystem::path> marker_file;
    std::optional<std::filesystem::path> snapshot_dir;
    // Headless replays for the workstation: savestates at the boundaries after these game ticks (the run goes on),
    // the first frame finished at or after a game tick written to a file, and the run ended once it is written.
    std::vector<std::pair<gekko::u64, std::filesystem::path>> save_state_at_ticks;
    std::optional<std::pair<gekko::u64, std::filesystem::path>> capture_at_tick;
    bool stop_after_capture = false;
    std::optional<std::filesystem::path> performance_log;
    std::optional<std::filesystem::path> telemetry_log;
    bool recompiled = true;
    bool debug_history = false;
    bool verify_recompiled = false;
    bool fallback_audit = false;
    // STEP-0 measurement batch (2026-09-23, findings-2026-09-23/step0-measurement).
    // All default off and observational: --measure <r3,r4,r5probes|all>
    // selects the probes, --measure-out names the JSON file they are written
    // to at exit, --measure-probes configures the r5probes entry/sampler set.
    std::set<std::string> measure;
    std::optional<std::filesystem::path> measure_out;
    std::string measure_probes_spec = "sets=r5";
    // Host tuning switches (2026-09-02). Every performance or correctness
    // change from the resource-architecture pass ships enabled and can be
    // turned off individually with --tune <name>=0, so a byte-identity failure
    // is bisected on ONE binary instead of across two builds. Parsed as a list
    // so the option surface does not grow a flag per change.
    std::vector<std::pair<std::string, bool>> tuning;
    bool visibility_probe = false;
    bool fov_probe = false;
    // World-atomic culling against the canonical camera. Default ON: with it off
    // props are culled to the guest 63.2501 deg cone while the display shows a
    // wider one, so they pop out at the screen edges. It defers at unity
    // coverage, so 4:3 keeps the original arithmetic exactly.
    bool native_visibility = true;
    // The layout objects' draw distance (docs/DRAW_DISTANCE.md): the native
    // far-object proxies, render only - the game's range stays the original's.
    // 1 = none (the original); above 1, to that many times each entry's
    // authored range; 8 = wherever the object still covers a pixel.
    float draw_distance = 8.0f;
    bool draw_distance_explicit = false;  // given on the command line: display.cfg's draw_distance does not apply
    // The effect layer switches (docs/EFFECTS_INVENTORY.md "Layer switches"): layers off, a soloed layer, a solo
    // without the world. A viewing tool: the game runs the same, only what is drawn changes. F6 in the window too.
    std::string effects_off;
    std::string effects_solo;
    bool effects_isolate = false;
    bool fps_counter = false;  // --fps-counter: presented FPS over the picture (Num * in the window)
    bool fps_counter_explicit = false;  // given on the command line: display.cfg's fps_counter does not apply
    // The fade-in of models that appear inside the view (gekko/appear_fade.h); 0 turns it off.
    gekko::u32 fade_in_ms = 0;
    // The native health bar IS the health-bar layer; --legacy-health-bar runs
    // the guest's own implementation instead, for A/B only.
#if defined(SHADOW_REFERENCE_RUNNER)
    bool legacy_health_bar = false;
#else
    static constexpr bool legacy_health_bar = false;
#endif
    bool present_plume = true;
    bool native_xfb_present = true;
    bool verify_plume = false;
    // Machine-readable session report, written at exit with every counter the
    // text report prints. Default on; --json-report overrides the path and
    // --no-json-report disables it.
    std::optional<std::filesystem::path> json_report;
    bool json_report_enabled = true;
    std::optional<int> window_width;
    std::optional<int> window_height;
    std::optional<bool> fullscreen;
    std::optional<int> render_width;
    std::optional<int> render_height;
    // Integer supersampling of the native EFB. This is deliberately separate
    // from render_width/render_height, which describe presentation/layout.
    unsigned internal_scale = 1u;
    // Default: derive the scale from the output height. See the resolution
    // comment at the set_internal_scale call site.
    bool internal_scale_auto = true;
    bool logical_pixel_snap = true;
    std::optional<gekko::AspectMode> aspect;
    std::optional<float> custom_aspect;
    std::optional<float> hud_scale;
    std::optional<float> hud_inset;
    std::optional<std::filesystem::path> display_cfg;
    std::optional<std::filesystem::path> pad_cfg;
    // The prompts' controller buttons (gekko/button_glyphs.h); unset: pad.cfg's button_style (default auto).
    std::optional<shadow::peripheral::ButtonStyle> button_style;
    // Development savestate (gekko/savestate.h). --state-file overrides the
    // slot path the N/M hotkeys use; --load-state restores it before the first
    // frame so a rendering change can be tested from a fixed gameplay position
    // without a human driving the menus.
    std::optional<std::filesystem::path> state_file;
    std::optional<std::filesystem::path> save_state_file;
    std::optional<std::filesystem::path> native_boot_contract_directory;
    bool load_state_at_start = false;
    bool save_state_at_exit = false;
    // On by default: measured on the Westopolis savestate at 1920x1080 it took
    // whole-process CPU from 3.19 cores to 1.36, per-present CPU over the run
    // from 1237 ms to 268 ms, and FPS from 55.70 to 60.16 - the guest's own
    // field rate. The CPU resampler stays as the fallback and is still what
    // --verify-plume checks.
    bool present_gpu_blit = true;
    bool present_area_resolve = true;
    bool vsync = true;
    bool vsync_explicit = false;  // given on the command line: display.cfg's vsync does not apply
    // Independent presentation cadence. It never changes how many guest cycles
    // execute. Zero means unlocked (or physical-refresh paced when vsync is on).
    double render_rate = 60.0;
    bool render_rate_explicit = false;  // given on the command line: display.cfg's render_rate does not apply
    // Ceiling on pacing recovery as a multiple of real time; negative means
    // "leave FixedSimulationClock::Config's default alone". Exposed so the
    // limiter can be swept and so an A/B against the old sprint-at-host-speed
    // behaviour (--catchup-rate off) stays available.
    double catchup_rate = -1.0;
    // Host-hitch forgiveness threshold in ms (host_timing.h). Negative leaves
    // the pacer default (off); 100 is the measured candidate; 0 = off.
    double hitch_forgiveness_ms = -1.0;
    // Cross-fading two XFB generations is not console behaviour: the VI
    // repeats a late XFB and never blends. It also latched on permanently for a
    // 50 Hz source at the 60 Hz default (engage < 0.90 x render rate) and after
    // any dip below 54 Hz at 60 Hz, presenting ghosted frames one generation
    // late. Opt-in since 2026-09-16 (--frame-interpolation).
    bool frame_interpolation = false;
    bool frame_interpolation_explicit = false;  // given on the command line: display.cfg's key does not apply
    // Present completed guest frames in arrival order when two land inside one
    // present interval, instead of always the newest (which skipped one and
    // repeated the next). UI thread only; --no-present-fifo is the A/B arm.
    bool present_fifo = true;
    std::string present_dump;
    // Presented-frame ordinal for --present-dump. The blur burst on a
    // --load-state route is over by the default 30.
    int present_dump_frame = 30;
    unsigned job_workers = 0;
};

std::filesystem::path executable_directory() {
#ifdef _WIN32
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length == buffer.size()) {
        throw std::runtime_error("GetModuleFileNameW failed");
    }
    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path();
#else
    return std::filesystem::current_path();
#endif
}

double game_seconds(const gekko::System& system) {
    return static_cast<double>(const_cast<gekko::System&>(system).game_ticks()) /
           static_cast<double>(gekko::kGameTicksPerSecond);
}

// ---- state reports and the development link (docs/comparator/README.md) --------------------------------------------

std::string utf8_path(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

std::filesystem::path path_from_utf8(const std::string& text) {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    return std::filesystem::u8path(text);
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}

std::filesystem::path executable_path() {
#ifdef _WIN32
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    buffer.resize(length);
    return std::filesystem::path(buffer);
#else
    return std::filesystem::path();
#endif
}

// A guest range for a state report or a peek: `address`, or a pointer path from it - read the 32-bit word at the
// address, add the next offset, and so on ("0x8056FF1C>0x4>0x50:64": the camera holder's RwCamera, its frame, the
// frame's matrix). `key` is the text it was asked with.
struct GuestRange {
    gekko::u32 address = 0;
    std::vector<gekko::u32> chain;
    gekko::u32 length = 0;
    std::string key;
};

// "0x8057E808:4,0x805E1898:2496,0x8056FF1C>0x4>0x50:64" (hex or decimal).
bool parse_guest_ranges(const std::string& text, std::vector<GuestRange>& out, std::string* error) {
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t comma = text.find(',', start);
        if (comma == std::string::npos) comma = text.size();
        std::string item = text.substr(start, comma - start);
        start = comma + 1;
        item.erase(0, item.find_first_not_of(" \t"));
        if (item.empty()) continue;
        item.erase(item.find_last_not_of(" \t") + 1);
        const std::size_t colon = item.find(':');
        if (colon == std::string::npos) {
            if (error != nullptr) *error = "expected address:length, got '" + item + "'";
            return false;
        }
        GuestRange range;
        range.key = item;
        const std::string path = item.substr(0, colon);
        std::size_t at = 0;
        bool first = true;
        while (at <= path.size()) {
            std::size_t next = path.find('>', at);
            if (next == std::string::npos) next = path.size();
            const unsigned long long value = std::strtoull(path.substr(at, next - at).c_str(), nullptr, 0);
            if (value > 0xFFFFFFFFull) {
                if (error != nullptr) *error = "bad guest range '" + item + "'";
                return false;
            }
            if (first) range.address = static_cast<gekko::u32>(value);
            else range.chain.push_back(static_cast<gekko::u32>(value));
            first = false;
            at = next + 1;
        }
        const unsigned long long length = std::strtoull(item.substr(colon + 1).c_str(), nullptr, 0);
        if (length == 0ull || length > (1ull << 20) || range.chain.size() > 8u) {
            if (error != nullptr) *error = "bad guest range '" + item + "'";
            return false;
        }
        range.length = static_cast<gekko::u32>(length);
        out.push_back(std::move(range));
    }
    return true;
}

// The bytes a range names now, or nullptr (a pointer on the way or the range itself not mapped).
const gekko::u8* resolve_guest_range(gekko::System& system, const GuestRange& range, gekko::u32* resolved) {
    gekko::u32 address = range.address;
    for (const gekko::u32 offset : range.chain) {
        const gekko::u8* word = system.guest_memory().peek(gekko::GcAddr(address), 4u);
        if (word == nullptr) return nullptr;
        address = ((static_cast<gekko::u32>(word[0]) << 24) | (static_cast<gekko::u32>(word[1]) << 16) |
                   (static_cast<gekko::u32>(word[2]) << 8) | static_cast<gekko::u32>(word[3])) +
                  offset;
    }
    if (resolved != nullptr) *resolved = address;
    return system.guest_memory().peek(gekko::GcAddr(address), range.length);
}

std::string hex_bytes(const gekko::u8* data, std::size_t size) {
    static const char digits[] = "0123456789abcdef";
    std::string out(size * 2u, '0');
    for (std::size_t i = 0; i < size; ++i) {
        out[2u * i] = digits[data[i] >> 4];
        out[2u * i + 1u] = digits[data[i] & 0xFu];
    }
    return out;
}

// A 64-bit digest of one block of the window (change detection between two runs, not security).
gekko::u64 block_digest(const gekko::u8* data, std::size_t size) {
    gekko::u64 h = 0x9E3779B97F4A7C15ull ^ static_cast<gekko::u64>(size);
    std::size_t i = 0;
    for (; i + 8u <= size; i += 8u) {
        gekko::u64 word;
        std::memcpy(&word, data + i, sizeof(word));
        h ^= word;
        h *= 0x100000001B3ull;
        h ^= h >> 29;
    }
    for (; i < size; ++i) {
        h ^= data[i];
        h *= 0x100000001B3ull;
    }
    return h ^ (h >> 32);
}

// The state report (docs/comparator/README.md "State reports"): the run's clocks, the renderer kind, the guest ranges
// asked for, and a digest of the game's address window in 64 KiB blocks, read as a savestate writes it (the
// renderer's own words as 0). At a boundary only.
std::string state_report_json(gekko::System& system, gekko::u64 render_frames, gekko::u64 boundary_tick,
                              const std::vector<GuestRange>& ranges, bool memory_digest) {
    gekko::dev::Json report;
    const gekko::RendererStateKind kind = gekko::renderer_state_kind(system);
    report.field("format", "shadow-state-report 1")
        .field("game_ticks", system.game_ticks())
        .field("ticks_per_second", gekko::kGameTicksPerSecond)
        .field("game_seconds", game_seconds(system))
        .field("vi_frames", static_cast<gekko::u64>(system.video().frames()))
        .field("render_frames", render_frames)
        .field("boundary_tick", boundary_tick)
        .field("savestate_version", gekko::save_state_version())
        .field("renderer_kind", static_cast<gekko::u32>(kind))
        .field("renderer", gekko::renderer_state_kind_name(kind));
    std::string peeks = "{";
    std::string digest;
    gekko::with_renderer_words_cleared(system, [&] {
        for (const GuestRange& range : ranges) {
            const gekko::u8* bytes = resolve_guest_range(system, range, nullptr);
            if (peeks.size() > 1u) peeks += ",";
            peeks += gekko::dev::quote(range.key) + ":" +
                     (bytes != nullptr ? gekko::dev::quote(hex_bytes(bytes, range.length)) : std::string("null"));
        }
        if (!memory_digest) return;
        constexpr std::size_t kBlock = 64u << 10;
        digest = "{\"block_bytes\":" + std::to_string(kBlock) + ",\"regions\":[";
        bool first_region = true;
        for (const gekko::GuestMemory::Region& region : system.guest_memory().regions()) {
            if (!first_region) digest += ",";
            first_region = false;
            digest += "{\"name\":" + gekko::dev::quote(region.name) + ",\"base\":" + std::to_string(region.base) +
                      ",\"committed\":" + std::to_string(region.committed) + ",\"blocks\":[";
            const gekko::u8* bytes = region.committed != 0u
                                         ? system.guest_memory().peek(gekko::GcAddr(region.base), region.committed)
                                         : nullptr;
            for (std::size_t offset = 0; bytes != nullptr && offset < region.committed; offset += kBlock) {
                const std::size_t size = (std::min)(kBlock, static_cast<std::size_t>(region.committed) - offset);
                char hash[24];
                std::snprintf(hash, sizeof(hash), "\"%016llx\"",
                              static_cast<unsigned long long>(block_digest(bytes + offset, size)));
                if (offset != 0u) digest += ",";
                digest += hash;
            }
            digest += "]}";
        }
        digest += "]}";
    });
    peeks += "}";
    report.raw("peeks", peeks);
    if (memory_digest) report.raw("memory", digest);
    return report.str();
}

// The Windows virtual key a --snapshot-key names: F1..F24, one letter or digit; 0 for "none"; -1 when unknown.
int snapshot_virtual_key(const std::string& name) {
    std::string key;
    for (const char c : name) key += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (key.empty() || key == "NONE" || key == "OFF") return 0;
    if (key.size() == 1u && ((key[0] >= 'A' && key[0] <= 'Z') || (key[0] >= '0' && key[0] <= '9'))) return key[0];
    if (key.size() >= 2u && key[0] == 'F') {
        char* end = nullptr;
        const long n = std::strtol(key.c_str() + 1, &end, 10);
        if (end != nullptr && *end == '\0' && n >= 1 && n <= 24) return 0x70 + static_cast<int>(n) - 1;  // VK_F1..
    }
    return -1;
}

void print_usage() {
    std::cout << "Usage: ShadowTheHedgehog.exe [options]\n"
                 "\n"
                 "  --game-data <directory>   Root holding sys/ and files/ from the original disc\n"
                 "  --memory-card <file>      Attach a real .gci or writable .raw as card A\n"
                 "                            (.gci writes persist in a separate .pc.raw card;\n"
                 "                            a missing .raw is created empty. Default: the\n"
                 "                            GCI under GC/EUR/Card A, else\n"
                 "                            save-data/ShadowTheHedgehog.pc.raw)\n"
                 "  --no-boot                 Mount and load only; do not execute game code\n"
                 "  --budget-ms <ms>          Game milliseconds this run may cover\n"
                 "  --wait-watchdog-ms <ms>   Stop with the guest location when the guest runs\n"
                 "                            this long (wall) without reaching a wait point\n"
                 "  --symbols <file.csv>      addr_hex,name symbol table for diagnostics\n"
                 "  --headless                Run without opening a window\n"
                 "  --no-logical-pixel-snap   A/B: stop converting orthographic pixel-centre\n"
                 "                            geometry to logical pixel edges at scale > 1. This\n"
                 "                            is NOT the pre-2026-08-31 behaviour - the old vertex\n"
                 "                            snap and max(px,0.5) sample clamp are gone. No effect\n"
                 "                            at --internal-scale 1\n"
                 "  --present-dump-frame <n>  Presented-frame ordinal --present-dump captures\n"
                 "                            (default 30). The blur burst on a --load-state\n"
                 "                            route is over well before that\n"

                 "  --width <pixels>          Window / presentation width (default 1280)\n"
                 "  --height <pixels>         Window / presentation height (default 720)\n"
                 "  --fullscreen              Borderless fullscreen on the primary monitor\n"
                 "  --windowed                Force a window (overrides display.cfg)\n"
                 "  --render-width <pixels>   Internal layout width (0 = follow window)\n"
                 "  --render-height <pixels>  Internal layout height (0 = follow window)\n"
                 "  --renderer native         The native Direct3D 12 renderer, the only one\n"
                 "                            (--native-renderer is the same)\n"
                 "  --internal-scale <1..6|auto>  Internal render scale for WHICHEVER backend\n"
                 "                            is selected: an integer, isotropic multiplier of\n"
                 "                            the guest's 640x528 EFB. 1x=640x528, 3x=1080p,\n"
                 "                            4x=1440p, 6x=4K-class supersampling. The display\n"
                 "                            aspect is applied at present time, not here.\n"
                 "                            auto (default) = the smallest scale that does not\n"
                 "                            magnify the guest raster in the fitted output\n"
                 "  --aspect <mode>           Auto / 4:3 / 16:9 / 16:10 / 21:9 / 32:9 / custom\n"
                 "  --draw-distance <scale>   Object draw distance: 1 (the original) to 8 (default: to a pixel)\n"
                 "  --effects-off <a,b,...>   Effect layers not drawn (glare, lens-flare, motion-blur, fog, ...;\n"
                 "                            --list-effect-layers); Num / (or F6) in the window: the layer list\n"
                 "  --effects-solo <layer>    Draw only this effect layer (the other effects off)\n"
                 "  --effects-isolate         With --effects-solo: without the world (the effect alone)\n"
                 "  --list-effect-layers      Print the effect layer names and exit\n"
                 "  --fps-counter             Presented FPS and frame times, top right (Num * toggles it)\n"
                 "  --fade-in-ms <ms>         Fade-in of models appearing in view, 0 (off, the default) to 5000\n"
                 "  --aspect-ratio <w/h>      Custom aspect, e.g. 2.37\n"
                 "  --hud-scale <factor>      HUD element scale, independent of aspect\n"
                 "  --hud-inset <pixels>      Horizontal/vertical HUD safe-zone inset\n"
                 "  --state-file <file>       Savestate slot used by the N (save) and\n"
                 "                            M (load) hotkeys. Development tool; default\n"
                 "                            save-data/slot0.gekkostate\n"
                 "  F8 (in game)              Marker (--marker-key): prints [marker] with the game\n"
                 "                            tick, appends it to the marker file and records it in a\n"
                 "                            GEKKO_AUDIO_AUDIT trace\n"
                 "  F9 (in game)              Range (--range-key): the first press marks a range's start,\n"
                 "                            the next its end (replay or loop it: replay_markers.py)\n"
                 "  --load-state              Restore --state-file before the first frame\n"
                 "  --save-state              Write a savestate when the run ends\n"
                 "  --save-state-file <file>  Override only the end-of-run savestate output\n"
                 "  --dev-link 127.0.0.1:<port>  Connect to the before / after comparison\n"
                 "                            workstation (tools/comparator); --dev-link-name <role>\n"
                 "                            names this run there, --start-paused waits for its\n"
                 "                            resume before the first instruction\n"
                 "  --marker-key <key>        Marks this moment of a replay (F1-F24, a letter, a digit,\n"
                 "                            none; default F8): game tick, frames and tape sample\n"
                 "                            appended to --marker-file (default <tape>.markers.jsonl)\n"
                 "  --range-key <key>         Opens / closes a marked range (as --marker-key; default F9)\n"
                 "  --snapshot-key <key>      The window's comparison snapshot key (F1-F12, a letter\n"
                 "                            or a digit, none; default F5): saves this moment into a\n"
                 "                            new folder under --snapshot-dir <dir> (default:\n"
                 "                            snapshots/ beside the executable)\n"
                 "  --input-tape-record <file>  Record every latched controller sample (game time)\n"
                 "  --input-tape-play <file>  Latch the controller samples of a recorded tape\n"
                 "  --stop-at-tick <ticks>    End the run at the traversal boundary after the host\n"
                 "                            service at this game tick (a checkpoint's boundary)\n"
                 "  --state-report <file>     Write a JSON state report at the end of the run;\n"
                 "                            --report-peek <addr:len,...> adds guest ranges\n"
                 "  --save-state-at-tick <ticks>:<file>  Also save at the boundary after that game\n"
                 "                            tick and run on (repeatable; headless)\n"
                 "  --capture-at-tick <ticks>:<file>  Write the first frame finished at or after\n"
                 "                            that game tick (headless; <file>.capture.json names it);\n"
                 "                            --stop-after-capture ends the run once it is written\n"
                 "  --native-boot-contract-dir <new-dir>\n"
                 "                            Observe cold-boot CPU/device/memory fragments;\n"
                 "                            diagnostic only, incomplete and not resumable\n"
                 "  --display-cfg <file>      Key=value display config (default display.cfg)\n"
                 "  --pad-cfg <file>          Controller mapping (default pad.cfg beside the\n"
                 "                            executable)\n"
                 "  --button-style <style>    Buttons the prompts show: auto (the connected\n"
                 "                            controller), original (the disc's), xbox,\n"
                 "                            playstation, switch. Overrides pad.cfg's\n"
                 "                            button_style\n"
                 "  --fpr-shadow              Restore a host snapshot of all 32 FPRs across\n"
                 "                            exceptions. OFF by default: Gekko does not\n"
                 "                            preserve FPRs across an exception, and the\n"
                 "                            restore changed 0 of 1,141,282 values measured.\n"
                 "  --compiler-intrinsics     Also replace the tick wrapper and time-base\n"
                 "                            scaling helpers (0x80051BBC 0x80051414) with\n"
                 "                            host arithmetic. OFF by default: they read the\n"
                 "                            time base mid-call and are not exact.\n"
                 "  --no-divide-intrinsics    A/B: run the recovered 64-bit divide loops\n"
                 "                            (0x803A1F54 0x803A2040) instead of one native\n"
                 "                            64-bit division. Default ON: register, stack and\n"
                 "                            instruction-count exact, and declined whenever an\n"
                 "                            event would fall inside the call\n"
                 "  --no-dynamic-cast-intrinsic  A/B: run the guest __dynamic_cast (0x803A1AFC) instead of\n"
                 "                            the native RTTI walk\n"
                 "  --no-matrix-multiply-intrinsic  A/B: run the guest RwMatrixMultiply (0x8047FC08)\n"
                 "                            instead of the native body\n"
                 "  --verify-matrix-multiply  Run every native RwMatrixMultiply call through the\n"
                 "                            guest body too and compare all state (slow)\n"
                 "  --no-chara-colli-intrinsic  A/B: run the guest CharaColli bounding tests instead of\n"
                 "                            the native reject\n"
                 "  --verify-chara-colli      Run every CharaColli bounding test through the guest body\n"
                 "                            too and compare (slow)\n"
                 "  --no-chara-colli-loops    A/B: run the guest CharaColli query loops (the tests stay\n"
                 "                            native)\n"
                 "  --trace-stale-contacts    diagnostic: count guest reads of the contact bytes the narrow\n"
                 "                            phase copied from stale stack (slow)\n"
                 "  --chara-colli-pass        experimental: the CharaColli manager's pair pass natively (its\n"
                 "                            stale-stack contact bytes differ: docs/CPU_FINAL_HAUNT.md)\n"
                 "  --no-chara-colli-manage   A/B: the CharaColli manager pass's vf 0x7C updates in the\n"
                 "                            guest\n"
                 "  --chara-colli-bone-guest  A/B: GeneralBone updates not reused run by the guest\n"
                 "                            instead of the native update\n"
                 "  --asset-trace <file>      Write a FILE->READ->GUEST MEMORY->PARSER\n"
                 "                            resource-lineage trace as JSONL\n"
                 "  --file-origin-trace <f>   Alias for --asset-trace\n"
                 "  --resource-lineage <sub>  Restrict --asset-trace to paths containing\n"
                 "                            <sub> (e.g. stg0202)\n"
                 "  --no-xinput               Disable the host XInput bridge\n"
                 "  --original-save-text      Show the game's own memory-card messages instead\n"
                 "                            of the PC wording of the save screen\n"
                 "  --perf-log <file.csv>     Log every visible presentation interval and CPU phase\n"
                 "  --telemetry <file.csv>    Log resource residency once a second: what grows as\n"
                 "                            a level is traversed\n"
                 "  --present-plume           Present through plume/Direct3D 12 (default)\n"
                 "  --native-xfb-present / --no-native-xfb-present\n"
                 "                            Persistent GPU NativeXFB presentation (default on)\n"
#if defined(SHADOW_REFERENCE_RUNNER)
                 "  --legacy-scheduler        A/B only: the guest's own task traversal instead of\n"
                 "                            the native gameplay scheduler (default: native)\n"
#endif
                 "  --tune <name>=0|1         Turn one host tuning switch off or on; repeatable.\n"
                 "                            --tune-list prints every name. All default on.\n"
                 "  --movie-trace <file.csv>  Development: observe the recovered Sofdec\n"
                 "                            movie pipeline (frames produced, picture\n"
                 "                            conversions, Im2D blits). Guest code is\n"
                 "                            unchanged; every hook only reads\n"
                 "  --ax-mixer / --no-ax-mixer  Stand in for the DSP: advance AX voice\n"
                 "                            positions and mix the voices into the audio\n"
                 "                            interface buffer. Default on when windowed, off\n"
                 "                            when headless (it writes guest memory, which the\n"
                 "                            byte-identity routes must not see). Without it\n"
                 "                            there is no sound and no movie plays past its\n"
                 "                            first frame\n"
                 "  --measure <items>         STEP-0 measurement, observation only (default\n"
                 "                            off): comma list of r3, r4, r5probes or all,\n"
                 "                            or gx-inputs (digests of what the GX stream's\n"
                 "                            consumers receive; not part of all).\n"
                 "                            r3 also enables --fallback-audit.\n"
                 "                            Not for performance runs\n"
                 "  --measure-out <file>      Where --measure writes its JSON (default\n"
                 "                            measure.json in the working directory)\n"
                 "  --measure-probes <spec>   r5probes configuration, e.g.\n"
                 "                            \"sets=r5,r4;pb=200,220,240;label=x\"\n"
                 "  --audio / --no-audio      Host audio playback through WASAPI (default: on\n"
                 "                            when windowed, off when headless; the producer\n"
                 "                            counts either way)\n"
                 "  --boot-stage <stgNNNN>    DEVELOPER NAVIGATION: cold-boot straight into a stage\n"
                 "                            through the game's own Select Mode hand-off (no pad\n"
                 "                            input, memory card not read, PAL keeps its 60 Hz\n"
                 "                            preselect). Not combinable with --load-state or\n"
                 "                            --ui-test <mode>; --ui-test-script plays its pad\n"
                 "                            input into the stage (wait out the hand-off first)\n"
                 "  --boot-event <eventNNNN>  DEVELOPER NAVIGATION: the same hand-off into\n"
                 "                            StageSequence::PlayEvent - one in-engine event\n"
                 "                            (files/event/eventNNNN*) played by recovered code\n"
                 "  --boot-flashback          DEVELOPER NAVIGATION: the same hand-off into\n"
                 "                            StageSequence::PlayFlashback\n"
                 "  --boot-story              DEVELOPER NAVIGATION: the same hand-off into\n"
                 "                            StageSequence::PlayStory as a new game\n"
                 "  --list-displays           Print the displays (desktop size, refresh, supported\n"
                 "                            modes) and the resolution presets as JSON, then exit\n"
                 "  --list-stages             Print every stage in the guest stage table and\n"
                 "                            whether --boot-stage can open it, then exit\n"
                 "  --tv-format <pal|ntsc>    TV format the IPL leaves behind. Default pal:\n"
                 "                            what this PAL disc expects, and what brings up\n"
                 "                            the original 50/60 Hz selection screen. Use ntsc\n"
                 "                            to reproduce the recorded 4:3 byte-identity\n"
                 "                            hashes and the 640x480 2D baselines\n"
                 "  --sofdec-stub             Force MWS_PLY_STAT_PLAYEND after 50 VI frames.\n"
                 "                            A forgery, kept only to A/B against the old\n"
                 "                            behaviour; it cuts real movies short. Off by\n"
                 "                            default - movies now end on their own\n"
                 "  --ui-test <mode>          Development UI driver: publish scripted pad\n"
                 "                            input so a UI state can be reached and looped\n"
                 "                            without a controller. Modes: boot, fade,\n"
                 "                            loading, select, hud\n"
                 "  --ui-test-script <file>   Same driver from a script file (overrides\n"
                 "                            --ui-test). See gekko/ui_test.h for the format\n"
                 "  --verify-plume            Read the presented back buffer back and compare it\n"
                 "                            with the pixels handed to the presenter\n"
                 "  --vsync / --no-vsync      Synchronize DX12 presentation to display refresh\n"
                 "                            (default on; off permits tearing when supported)\n"
                 "  --render-rate <hz|unlocked>  Independent presentation rate (default 60).\n"
                 "                            Supports 30/60/120/144/165/240 or any positive Hz\n"
                 "  --frame-interpolation / --no-frame-interpolation\n"
                 "                            Cross-fade completed XFB snapshots when the source\n"
                 "                            runs below 90% of the render rate (default OFF:\n"
                 "                            the console repeats a late frame, it never blends)\n"
                 "  --no-present-fifo         A/B: when two guest frames complete inside one\n"
                 "                            present interval, show only the newest (skips one,\n"
                 "                            repeats the next). Default shows them in order, at\n"
                 "                            most one interval late and never more than 200 ms\n"
                 "  --trace                   Unsupported; rejected without starting execution\n"
                 "  --profile                 Report the hottest guest code at exit\n"
                 "  --no-recomp               Reference runner only: interpret original PPC\n"
                 "  --debug-history           Enable the postmortem instruction ring\n"
                 "  --no-debug-history        Explicitly disable postmortem history\n"
                 "  --verify-recomp           Reference runner only: compare native and PPC\n"
                 "                            registers, memory and timing\n"
                 "\n"
                 "Shipping layout:\n"
                 "  ShadowTheHedgehog.exe\n"
                 "  files/...\n"
                 "Development builds without an embedded system image also require sys/.\n";
}

gekko::DolProgram to_gekko_program(const shadow::DolImage& image) {
    gekko::DolProgram program;
    program.bytes = image.bytes;
    program.bss_address = image.bss_address;
    program.bss_size = image.bss_size;
    program.entry_point = image.entry_point;
    for (const auto& section : image.sections) {
        program.sections.push_back(
            gekko::DolSectionInfo{section.file_offset, section.address, section.size, section.executable});
    }
    return program;
}

std::size_t load_symbols(gekko::System& system, const std::filesystem::path& path) {
    std::ifstream stream(path);
    if (!stream) {
        return 0;
    }
    std::string line;
    std::size_t count = 0;
    std::getline(stream, line);  // header
    while (std::getline(stream, line)) {
        const auto comma = line.find(',');
        if (comma == std::string::npos) {
            continue;
        }
        const std::string address_text = line.substr(0, comma);
        std::string name = line.substr(comma + 1);
        if (!name.empty() && name.front() == '"') {
            const auto closing = name.find('"', 1);
            name = closing == std::string::npos ? name.substr(1) : name.substr(1, closing - 1);
        } else {
            const auto next = name.find(',');
            if (next != std::string::npos) {
                name = name.substr(0, next);
            }
        }
        try {
            const unsigned long address = std::stoul(address_text, nullptr, 16);
            system.add_symbol(static_cast<gekko::u32>(address), name);
            ++count;
        } catch (const std::exception&) {
            continue;
        }
    }
    return count;
}


// display.cfg: --display-cfg, else beside the executable. Read at startup and written by the display settings' save().
std::filesystem::path display_cfg_path(const Options& options) {
    return options.display_cfg ? *options.display_cfg : executable_directory() / "display.cfg";
}

gekko::DisplayConfig resolve_display_config(const Options& options) {
    gekko::DisplayConfig config;
    config.window_width = 1280;
    config.window_height = 720;
    config.aspect = gekko::AspectMode::Auto;

    const std::filesystem::path cfg_path = display_cfg_path(options);
    if (std::filesystem::exists(cfg_path)) {
        std::ifstream stream(cfg_path);
        std::ostringstream text;
        text << stream.rdbuf();
        std::string error;
        if (!gekko::parse_display_file(text.str(), config, &error)) {
            throw std::runtime_error(error.empty() ? "failed to parse display.cfg" : error);
        }
        std::cout << "Display config: " << cfg_path.string() << "\n";
    }

    // Host input binding, same shape as the display config: a key=value file
    // beside the executable, absent by default. It binds host buttons to the
    // recovered PAD's bits ONLY - the +-100 stick range, the guest's own 8000
    // deadzone and the analog A/B 0-or-255 encoding are data dependencies of
    // the game's movement and camera code, not preferences, so they are not
    // configurable here.
    //
    // --pad-cfg exists for the same reason as --display-cfg: a build that runs
    // from its own tree (the instrumented profiling build) would otherwise read
    // pad.cfg from beside ITSELF and silently play with the default bindings.
    {
        const std::filesystem::path pad_path =
            options.pad_cfg ? *options.pad_cfg : executable_directory() / "pad.cfg";
        if (options.pad_cfg && !std::filesystem::exists(pad_path)) {
            // Asked for by name and absent: say so rather than quietly falling
            // back to the defaults the caller was trying to replace.
            std::cerr << "WARNING: pad config not found, using default bindings: "
                      << pad_path.string() << "\n";
        }
        if (std::filesystem::exists(pad_path)) {
            std::ifstream stream(pad_path);
            std::ostringstream text;
            text << stream.rdbuf();
            shadow::peripheral::GamepadBinding binding;
            std::string error;
            if (!shadow::peripheral::parse_gamepad_binding(text.str(), binding, &error)) {
                throw std::runtime_error(pad_path.string() + ": " +
                                         (error.empty() ? "failed to parse" : error));
            }
            gekko::set_gamepad_binding(binding);
            std::cout << "Pad mapping: " << pad_path.string() << "\n";
        }
    }

    if (options.window_width) {
        config.window_width = *options.window_width;
    }
    if (options.window_height) {
        config.window_height = *options.window_height;
    }
    if (options.fullscreen) {
        config.fullscreen = *options.fullscreen;
    }
    if (options.render_width) {
        config.render_width = *options.render_width;
    }
    if (options.render_height) {
        config.render_height = *options.render_height;
    }
    if (options.aspect) {
        config.aspect = *options.aspect;
    }
    if (options.custom_aspect) {
        config.custom_aspect = *options.custom_aspect;
    }
    if (options.hud_scale) {
        config.hud_scale = *options.hud_scale;
    }
    if (options.hud_inset) {
        config.hud_inset = *options.hud_inset;
    }
    if (config.window_width < 160) {
        config.window_width = 160;
    }
    if (config.window_height < 120) {
        config.window_height = 120;
    }
    return config;
}

int run(Options options) {
    if (options.native_boot_contract_directory &&
        (!options.boot || options.load_state_at_start || !options.recompiled)) {
        std::cerr << "ERROR: --native-boot-contract-dir requires a translated cold boot"
                     " without --load-state.\n";
        return 2;
    }
    const bool reference_execution = gekko::reference_execution_available();
    if (!reference_execution && (!options.recompiled || options.verify_recompiled || options.trace)) {
        std::cerr << "ERROR: --no-recomp and --verify-recomp are reference-only options; --trace is unsupported. "
                     "The native executable cannot interpret PPC. Use ShadowTheHedgehogReference for validation.\n";
        return 2;
    }
    if (options.trace) {
        std::cerr << "ERROR: per-instruction tracing is not implemented by the reference runner.\n";
        return 2;
    }
    if (!reference_execution && options.boot && gekko::recomp::kFunctionCount == 0u) {
        std::cerr << "ERROR: no generated game code is linked; build with SHADOW_RECOMP_DOL. "
                     "PPC interpretation is forbidden.\n";
        return 2;
    }
    std::cout << (reference_execution ? "Shadow the Hedgehog reference validation runtime\n"
                                      : "Shadow the Hedgehog native preservation runtime\n");
    shadow::EmbeddedGameData embedded = shadow::load_embedded_game_data();
    const bool embedded_system_image = embedded.complete();
    const auto root = embedded_system_image
                          ? shadow::find_extracted_game_assets(
                                options.game_data, executable_directory(),
                                std::filesystem::current_path())
                          : shadow::find_extracted_game_data(
                                options.game_data, executable_directory(),
                                std::filesystem::current_path());
    if (!root) {
        std::cerr << "ERROR: Original game assets were not found.\n";
        print_usage();
        return 2;
    }

    // The three blobs the boot needs. They are program and metadata, not a
    // filesystem: the title block the drive-identity probe returns, the BI2
    // block the OS reads, and the executable itself. There is no disc image
    // behind them and no directory table: the file tree is the native
    // namespace built from files/ (System::mount_assets), and the native boot
    // runs no disc loader.
    struct SystemImage {
        std::vector<gekko::u8> boot;
        std::vector<gekko::u8> bi2;
        std::vector<gekko::u8> dol;
    } image;
    if (embedded_system_image) {
        image.boot = std::move(embedded.boot);
        image.bi2 = std::move(embedded.bi2);
        image.dol = std::move(embedded.dol);
    } else {
        const auto slurp = [](const std::filesystem::path& path) {
            std::vector<gekko::u8> bytes;
            std::ifstream in(path, std::ios::binary);
            if (!in) return bytes;
            in.seekg(0, std::ios::end);
            const auto size = in.tellg();
            if (size > 0) {
                bytes.resize(static_cast<std::size_t>(size));
                in.seekg(0, std::ios::beg);
                in.read(reinterpret_cast<char*>(bytes.data()), size);
            }
            return bytes;
        };
        image.boot = slurp(*root / "sys" / "boot.bin");
        image.bi2 = slurp(*root / "sys" / "bi2.bin");
        image.dol = slurp(*root / "sys" / "main.dol");
    }

    const auto mount = [&] {
        GEKKO_PROFILE_ZONE("boot/identify");
        return shadow::identify_game_data(*root, image.boot);
    }();
    if (mount.disc.game_id != "GUPP8P") {
        std::cerr << "ERROR: This runtime profile requires the PAL GUPP8P disc; found "
                  << mount.disc.game_id << ".\n";
        return 3;
    }

    // ES-W4 (2026-10-20): moved, not copied. image.dol is not read again, and parse_dol takes its bytes by value, so
    // the copy kept a second 5.51 MiB DOL for the whole run (W1 census: two 5.51 MiB process-heap blocks).
    auto dol = [&] {
        GEKKO_PROFILE_ZONE("boot/dol-parse");
        return shadow::parse_dol(std::move(image.dol));
    }();

    // Guard original words and reserve a new output directory before installing
    // any observer. This owner exists only for an explicitly requested capture.
    std::unique_ptr<shadow::pc_bootstrap::ContractObserver> boot_contract_observer;
#if !defined(GEKKO_SHIPPING)  // ES: the boot-contract capture is a verification tool, not part of the user's exe
    if (options.native_boot_contract_directory) {
        boot_contract_observer = std::make_unique<shadow::pc_bootstrap::ContractObserver>(
            *options.native_boot_contract_directory, dol);
    }
#endif

    std::size_t text_sections = 0;
    for (const auto& section : dol.sections) {
        if (section.executable) {
            ++text_sections;
        }
    }

    std::cout << "Game data: " << mount.root.string() << "\n"
              << "System image: "
              << (embedded_system_image ? "embedded in executable" : "external sys/") << "\n"
              << "Title: " << mount.disc.game_id << "  " << mount.disc.title << "\n"
              << "DOL loaded: " << dol.bytes.size() << " bytes, " << text_sections << " text + "
              << (dol.sections.size() - text_sections) << " data sections\n"
              << "Game memory: native address window, program image 0x" << std::hex << std::uppercase
              << gekko::GuestMemory::kImageBase << "-0x" << gekko::GuestMemory::kImageEnd << std::dec
              << std::nouppercase << "; entry point 0x"
              << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << dol.entry_point
              << std::dec << std::nouppercase << "\n";

    if (!options.boot) {
        // The same namespace System would mount, built and reported without
        // booting: proves the files/ tree is a mountable asset tree.
        gekko::assets::AssetNamespace names;
        std::string error;
        if (!gekko::assets::AssetNamespace::from_directory(mount.root / "files", names, error)) {
            std::cerr << "ERROR: could not build the asset namespace: " << error << "\n";
            return 4;
        }
        std::cout << "Asset namespace: " << names.file_count() << " files, "
                  << names.directory_count() << " directories\n";
        std::cout << "M0-M4 READY: native host, data discovery, asset namespace, and DOL load succeeded.\n";
        return 0;
    }

    gekko::System system;
    const gekko::DisplayConfig display = resolve_display_config(options);
    // A render size Direct3D 12 cannot hold as one texture (D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION) is refused here
    // instead of failing at target creation.
    if (display.render_width > 16384u || display.render_height > 16384u) {
        std::cerr << "ERROR: render size " << display.render_width << "x" << display.render_height
                  << " exceeds Direct3D 12's 16384-pixel texture limit.\n";
        return 2;
    }
    // display.cfg's presentation keys, under the command line's flags (the command line > display.cfg > the default).
    if (display.render_rate && !options.render_rate_explicit) options.render_rate = *display.render_rate;
    if (display.vsync && !options.vsync_explicit) options.vsync = *display.vsync;
    if (display.frame_interpolation && !options.frame_interpolation_explicit)
        options.frame_interpolation = *display.frame_interpolation;
    if (display.draw_distance && !options.draw_distance_explicit) options.draw_distance = *display.draw_distance;
    if (display.fps_counter && !options.fps_counter_explicit) options.fps_counter = *display.fps_counter;
    // The display settings while the game runs (gekko/display_settings.h): the ones in effect now, and the file the
    // Options menu saves them to. Its owners pick changes up in the UI loop and at the host services below.
    {
        gekko::DisplaySettings settings;
        settings.fullscreen = display.fullscreen;
        settings.window_width = display.window_width;
        settings.window_height = display.window_height;
        settings.render_width = display.render_width;
        settings.render_height = display.render_height;
        settings.render_rate = options.render_rate;
        settings.vsync = options.vsync;
        settings.frame_interpolation = options.frame_interpolation;
        settings.draw_distance = options.draw_distance;
        settings.fps_counter = options.fps_counter;
        gekko::display_settings().initialize(settings, display_cfg_path(options));
#if defined(SHADOW_RENDER_LIFTED)
        // The Options menu's Display page offers the launcher's resolution presets (render/display_modes.h).
        std::vector<gekko::options_display::Preset> presets;
        for (const shadow::render::ResolutionPreset& p : shadow::render::resolution_presets())
            presets.push_back({p.width, p.height, p.family});
        gekko::options_display::set_resolution_presets(
            std::move(presets), [](std::uint32_t w, std::uint32_t h) { return shadow::render::aspect_family(w, h).name; });
#endif
    }
    system.display().configure(display);
    // Internal scale. ONE control, and it means the same thing to both
    // backends: the integer, isotropic multiplier between the guest's 640x528
    // EFB and the physical render targets. It is deliberately not a renderer
    // selector any more - selecting the legacy raster is what --renderer is
    // for - because while it was one the native renderer above 1x could not be
    // reached from a gate and so was never gated at all.
    //
    // `auto` is the default because the alternative default - 1 - renders the
    // whole game at the console's 640x528 and lets the presenter magnify it,
    // which is exactly the pixelation the effect and glare passes show most
    // (their output is a full-screen composite, so every stair-step in it is a
    // stair-step of the console raster grid).
    //
    // Chosen from both axes of the fitted CONTENT rectangle. The raster stays
    // anamorphic 640x480 and the display aspect is applied at PRESENT time, so
    // at 1920x1080 the horizontal sampling ratio is 3x even though the vertical
    // ratio is only 2.25x; taking height alone used to select 2x and then
    // magnify every resident surface horizontally. Integer and ceiling: a
    // non-integer factor makes the EFB copy resolve back to guest dimensions
    // inexact, and the game performs hundreds of texture copies per frame whose
    // UVs depend on that being exact.
    //
    // The rectangle `auto` measures is the one the frame will be PRESENTED
    // into: the window when there is one, otherwise --native-output, otherwise
    // the console's own 640x480 - which is why every deterministic
    // byte-identity route, all of which run headless without --native-output,
    // still resolves to exactly 1.
    const int presentation_width =
        options.native_output_explicit ? static_cast<int>(options.native_output_width)
        : options.window               ? display.window_width
                                       : static_cast<int>(options.native_output_width);
    const int presentation_height =
        options.native_output_explicit ? static_cast<int>(options.native_output_height)
        : options.window               ? display.window_height
                                       : static_cast<int>(options.native_output_height);
    const gekko::DisplayLayout presentation_layout =
        gekko::make_display_layout(display, presentation_width, presentation_height);
    unsigned internal_scale = options.internal_scale;
    if (options.internal_scale_auto) {
        internal_scale =
            gekko::recommended_internal_scale(presentation_layout, gekko::kMaxInternalScale);
    }
    std::cout << "Display: " << gekko::format_display_config(display) << "\n";
    // Both backends now derive their EFB from the same integer, so this line
    // describes whichever one is running. It used to be printed from the legacy
    // object unconditionally, which reported a 1920x1584 3x EFB for a native
    // renderer that was actually running 1920x1110 with two independent
    // fractional factors - exactly the kind of claim this project treats as a
    // hypothesis, so it should not be the runtime making it.
#if defined(SHADOW_RENDER_LIFTED)
    // The lifted build's renderer (R2) has no EFB scale: it renders at --render-width / --render-height (display.cfg
    // render_width / render_height), else at the window's size (G10, HW_REPORT N.2); the size it really uses is
    // printed by the renderer ("[lifted-render] render size").
    std::cout << "Render size (R2): "
              << (display.render_width > 0 && display.render_height > 0
                      ? std::to_string(display.render_width) + "x" + std::to_string(display.render_height) +
                            " asked: the 3D scene is rasterized at exactly this size (docs/RESOLUTION_AUDIT.md)"
                      : std::string("the window's size"))
              << "; --internal-scale and the internal EFB apply to the legacy renderer only\n";
#else
    {
        const gekko::EfbGeometry efb_geometry{gekko::RenderScale{internal_scale}};
        std::cout << "Internal EFB: " << efb_geometry.native_width() << 'x'
                  << efb_geometry.native_height() << " (" << efb_geometry.scale.factor
                  << "x isotropic; guest copies remain 640x528 units"
                  << (options.internal_scale_auto ? ", chosen automatically from the fitted output"
                                                  : ", set explicitly")
                  << ")\n";
    }
#endif
    if (display.hud_scale != 1.0f || display.hud_inset != 0.0f) {
        // These two are exposed in display.cfg but are NOT validated, and they
        // are not inert: either of them makes layout_is_native() false even at
        // 4:3 output, which turns the whole 2D layout path on - the scissor
        // remap and the tiled-strip continuation included - and so gives up the
        // byte-identical 4:3 guarantee. They also make the vertical affine
        // live, and the vertical anchor arithmetic is against a 480-tall canvas
        // while the bounds it is applied to are EFB rows 0..527.
        std::cout << "WARNING: hud_scale/hud_inset are experimental. They disable the"
                  << " byte-identical 4:3 path and enable untested vertical anchoring."
                  << "\n";
    }
    std::optional<std::filesystem::path> memory_card = options.memory_card;
    if (!memory_card) {
        const std::filesystem::path completed_save =
            mount.root / "GC" / "EUR" / "Card A" /
            "8P-GUPP-SHADOWTHEHEDGEHOG.gci";
        if (std::filesystem::exists(completed_save)) {
            memory_card = completed_save;
        } else {
            // A first run without an imported save: the game's own card,
            // created empty on first use, so it can create and keep its save.
            memory_card = executable_directory() / "save-data" / "ShadowTheHedgehog.pc.raw";
        }
    }
    if (memory_card) {
        std::filesystem::path working_card;
        std::string extension = memory_card->extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char byte) {
                           return static_cast<char>(std::tolower(byte));
        });
        if (extension == ".gci") {
            working_card = executable_directory() / "save-data" /
                           (memory_card->stem().string() + ".pc.raw");
        }
        system.set_memory_card_path(*memory_card, working_card);
        std::cout << "Memory card A source: " << memory_card->string() << "\n"
                  << "Memory card A working image: "
                  << system.memory_card_backing_path().string() << "\n";
        if (!system.memory_card_sram_path().empty()) {
            std::cout << "Memory card A SRAM identity: "
                      << system.memory_card_sram_path().string() << "\n";
        } else {
            std::cout << "Memory card A SRAM identity: derived from card header\n";
        }
    }
    if (options.symbols) {
        const std::size_t count = load_symbols(system, *options.symbols);
        std::cout << "Symbols: " << count << " loaded from " << options.symbols->string() << "\n";
    }

    gekko::BootConfiguration boot;
    boot.disc_header = image.boot;
    boot.disc_header.resize(0x20);
    boot.bi2 = image.bi2;
    boot.pal = options.pal_boot;
    boot.console_type = static_cast<gekko::u32>(options.console_type);

    // Mount the assets: the native namespace is built from the host tree
    // itself (no table is read), with one AssetId and guest token per file.
    {
        std::vector<gekko::u8> identity = image.boot;
        identity.resize(0x20);
        std::string error;
        system.set_pc_save_text(options.pc_save_text);
        {
            const shadow::peripheral::GamepadBinding& binding = gekko::gamepad_binding();
            system.button_glyphs().configure(options.button_style.value_or(binding.button_style), binding);
            // The sharp sheets go to the renderer, keyed by the patched texture's content: the lifted renderer's
            // import asks gekko/texture_overrides.h; the capture renderer's asset manager (when it runs) serves the
            // same sheet by the hash RwCapture computes from the guest raster.
            system.button_glyphs().set_sharp_sink([](gekko::button_glyphs::ButtonGlyphs::SharpSheet&& sheet) {
                auto replacement = std::make_shared<gekko::texture_overrides::Override>();
                replacement->width = sheet.sharp_width;
                replacement->height = sheet.sharp_height;
                replacement->gx_rgba8 =
                    gekko::texture_overrides::encode_gx_rgba8(sheet.rgba8.data(), sheet.sharp_width, sheet.sharp_height);
                gekko::texture_overrides::Registry::instance().put(
                    gekko::texture_overrides::content_key(sheet.texels, sheet.texel_bytes, sheet.palette,
                                                          sheet.palette_bytes),
                    std::move(replacement));
                gekko::NativeAssetManager* manager = gekko::NativeAssetManager::active();
                if (manager == nullptr) return true;
                auto asset = std::make_shared<gekko::NativeAssetManager::TextureAsset>();
                asset->width = sheet.sharp_width;
                asset->height = sheet.sharp_height;
                asset->gx_format = sheet.gx_format;
                asset->rgba8.assign(sheet.rgba8.begin(), sheet.rgba8.end());
                manager->pin_texture(gekko::NativeAssetManager::texture_hash(
                                         sheet.gx_format, sheet.tlut_format, sheet.depth, sheet.width, sheet.height,
                                         sheet.texels, sheet.texel_bytes, sheet.palette, sheet.palette_bytes),
                                     std::move(asset));
                return true;
            });
        }
        if (!system.mount_assets(*root / "files", std::move(identity), error)) {
            std::cerr << "ERROR: could not mount game assets: " << error << "\n";
            return 4;
        }
        const gekko::assets::MountReport& report = system.assets().report();
        std::cout << "Assets: " << report.files << " files, " << report.directories
                  << " directories, "
                  << static_cast<double>(report.declared_bytes) / (1024.0 * 1024.0)
                  << " MiB across " << static_cast<double>(report.address_span) / (1024.0 * 1024.0)
                  << " MiB of token space (" << report.table_bytes / 1024 << " KiB of tables)\n";
        std::cout << "Save text: " << (options.pc_save_text ? "PC wording (English)" : "original memory-card text")
                  << "; Options Display tile: tile art and help line in 6 languages; "
                  << system.assets().replaced_count() << " file(s) replaced\n";
        {
            const gekko::button_glyphs::ButtonGlyphs::Report& buttons = system.button_glyphs().report();
            std::cout << "Buttons: "
                      << shadow::peripheral::button_style_name(
                             options.button_style.value_or(gekko::gamepad_binding().button_style))
                      << " -> " << gekko::button_glyphs::family_name(buttons.family) << "; " << buttons.files
                      << " file(s), " << buttons.sheets << " sheet(s) patched as read";
            if (buttons.mismatched != 0u) std::cout << ", " << buttons.mismatched << " file(s) not this disc's";
            std::cout << "\n";
        }
        for (const std::string& problem : system.assets().replacement_errors()) {
            std::cerr << "WARNING: native content not applied: " << problem << "\n";
        }
    }

    // No DVD drive exists in this port and nothing pretends to be one: every
    // entry point of the file API the game links against is a native owner
    // serving the mounted assets (path -> namespace entry -> token -> one copy
    // out of a mapped host file), completing before it returns.
    system.enable_file_api();
    std::cout << "Files: native file API (namespace lookup, mapped host files; no DVD SDK, "
                 "no DI device, no command queue)\n";
    if (!options.legacy_scheduler) {
        // The game's frame is one traversal of one task tree. From here the
        // ordering, the pause policy, the removal protocol and the recursion
        // are native code walking the guest's own task objects; each task's
        // execute and destructor are still its translated guest code, called
        // through the dispatcher so an interrupt stays deliverable between two
        // tasks exactly where it was.
        system.enable_gameplay_scheduler();
        std::cout << "Gameplay: native scheduler (native task traversal; "
                     "translated task bodies)\n";
        if (options.measure.count("r3") != 0u) {
            // STEP-0 R3: observational only (no hooks, no guest writes). Video
            // fields advance on the guest thread (System::advance_game_time).
            system.gameplay_scheduler()->enable_fp_probe(
                [&system]() -> gekko::u64 { return system.video().frames(); });
            std::cout << "Measure: r3 scheduler FP probe enabled (observational)\n";
        }
    }
    GEKKO_PROFILE_ONLY({
        // TRACE IDENTITY. A capture with no identity cannot be compared with
        // another one, and comparing two runs is the whole reason to capture.
        // This records what the run WAS - renderer, scale, stage, budget - so a
        // trace read next week still says which configuration produced it.
        std::ostringstream identity;
        identity << "GekkoForge native renderer, internal scale " << options.internal_scale << ", "
                 << (options.window ? "windowed" : "headless");
        if (options.boot_stage) {
            identity << ", stage 0x" << std::hex << *options.boot_stage << std::dec;
        }
        if (options.load_state_at_start) identity << ", from savestate";
        // The default is 0, i.e. unlimited; only a real limit is worth recording.
        if (options.budget_ms != 0ull) identity << ", budget " << options.budget_ms << " ms";
        const std::string text = identity.str();
        GEKKO_PROFILE_APP_INFO(text.c_str(), text.size());
    })
    {
        GEKKO_PROFILE_ZONE("boot/load-dol");
        system.load_dol(to_gekko_program(dol));
        // ES-W4: the program image now lives in guest memory; nothing reads the DOL file bytes after this (the boot
        // contract observer checked them in its constructor and keeps no reference). Free the 5.51 MiB copy.
        std::vector<std::uint8_t>().swap(dol.bytes);
    }
    system.apply_boot_configuration(boot);
    if (options.list_stages || options.boot_stage) {
        // The guest's own tables in the program image: 59 stage records of
        // 0x50 bytes {+0 id, +4 name pointer, +8 flags} at 0x804C5AE8, and the
        // three Select Mode grids of record indices (-1 = empty) that its
        // confirm (0x802C48C8..0x802C495C) launches through PlayOnce.
        constexpr gekko::u32 kTable = 0x804C5AE8u;
        constexpr gekko::u32 kRecords = 59u;
        auto& memory = system.guest_memory();
        std::vector<bool> bootable(kRecords, false);
        const struct {
            gekko::u32 address;
            gekko::u32 slots;
        } grids[] = {{0x80500760u, 30u}, {0x805007D8u, 30u}, {0x80500850u, 10u}};
        for (const auto& grid : grids) {
            for (gekko::u32 slot = 0; slot < grid.slots; ++slot) {
                const gekko::u32 record = memory.read32(grid.address + slot * 4u);
                if (record < kRecords) bootable[record] = true;
            }
        }
        const auto record_name = [&](gekko::u32 record) {
            gekko::u32 name = memory.read32(kTable + record * 0x50u + 4u);
            std::string text;
            for (gekko::u32 i = 0; name != 0u && i < 40u; ++i) {
                const char c = static_cast<char>(memory.read8(name + i));
                if (c == 0) break;
                text.push_back(c);
            }
            return text;
        };
        bool found_bootable = false;
        if (options.list_stages) {
            std::cout << "Stages in the guest table (bootable = launchable by --boot-stage):\n";
        }
        for (gekko::u32 record = 0; record < kRecords; ++record) {
            const gekko::u32 id = memory.read32(kTable + record * 0x50u);
            if (options.list_stages) {
                char row[128];
                std::snprintf(row, sizeof(row), "  stg%04u  %-22s %s\n", id,
                              record_name(record).c_str(),
                              bootable[record] ? "bootable"
                                               : "not bootable (practice/2P/event/last story)");
                std::cout << row;
            }
            if (options.boot_stage && id == *options.boot_stage && bootable[record]) {
                found_bootable = true;
            }
        }
        if (options.list_stages) return 0;
        if (!found_bootable) {
            char stage_name[16];
            std::snprintf(stage_name, sizeof(stage_name), "stg%04u", static_cast<unsigned>(*options.boot_stage));
            std::cerr << "ERROR: --boot-stage " << stage_name
                      << " is not a Select Mode stage; run --list-stages" << std::endl;
            return 2;
        }
        // A script's pad input is allowed: the hand-off presses nothing, so a script that waits it out drives the
        // stage itself (pause, play). The built-in --ui-test modes navigate the front end and would fight it.
        if (options.load_state_at_start || (options.ui_test_mode && !options.ui_test_script)) {
            std::cerr << "ERROR: --boot-stage needs a cold boot without --load-state or --ui-test <mode>"
                      << std::endl;
            return 2;
        }
        system.set_boot_stage(*options.boot_stage);
    }
    if (options.boot_event) {
        if (options.boot_stage || options.load_state_at_start || options.ui_test_mode || options.ui_test_script) {
            std::cerr << "ERROR: --boot-event needs a cold boot without --boot-stage, --load-state or --ui-test"
                      << std::endl;
            return 2;
        }
        // The event's files must exist in the game data (files/event/eventNNNN*).
        char prefix[16];
        std::snprintf(prefix, sizeof(prefix), "event%04u", static_cast<unsigned>(*options.boot_event));
        bool present = false;
        std::error_code listing_error;
        for (const auto& entry :
             std::filesystem::directory_iterator(*root / "files" / "event", listing_error)) {
            if (entry.path().filename().string().rfind(prefix, 0) == 0) present = true;
        }
        if (!present) {
            std::cerr << "ERROR: --boot-event " << prefix << ": no files/event/" << prefix
                      << "* in the game data" << std::endl;
            return 2;
        }
        system.set_boot_event(*options.boot_event);
    }
    if (options.boot_story) {
        if (options.boot_stage || options.boot_event || options.load_state_at_start || options.ui_test_mode ||
            options.ui_test_script) {
            std::cerr << "ERROR: --boot-story needs a cold boot without --boot-stage, --boot-event, "
                         "--load-state or --ui-test"
                      << std::endl;
            return 2;
        }
        system.set_boot_story();
    }
    if (options.boot_flashback) {
        if (options.boot_stage || options.boot_event || options.boot_story || options.load_state_at_start ||
            options.ui_test_mode || options.ui_test_script) {
            std::cerr << "ERROR: --boot-flashback needs a cold boot without another --boot-*, "
                         "--load-state or --ui-test"
                      << std::endl;
            return 2;
        }
        system.set_boot_flashback();
    }
    auto contexts = std::make_shared<std::set<gekko::u32>>();
    if (options.context_hook != 0) {
        system.cpu().install_hle(
            static_cast<gekko::u32>(options.context_hook),
            [contexts](gekko::Cpu& cpu) {
                if (cpu.gpr[3] >= 0x80000000u && contexts->size() < 64) {
                    contexts->insert(cpu.gpr[3]);
                }
                return false;
            },
            "context");
    }
    auto watch_hits = std::make_shared<std::map<gekko::u32, int>>();
    // Diagnostics: SHADOW_WATCH_LIMIT=<n> prints the first n hits of each --watch (default 8), each with the game
    // tick it happened at - e.g. a whole allocation sequence to diff between two builds.
    long long watch_limit = 8;
    if (const char* limit = std::getenv("SHADOW_WATCH_LIMIT")) watch_limit = std::strtoll(limit, nullptr, 10);
    // SHADOW_WATCH_STACK=<n>: each printed hit also carries the guest back chain (n frames).
    unsigned watch_stack = 0;
    if (const char* stack = std::getenv("SHADOW_WATCH_STACK"))
        watch_stack = static_cast<unsigned>(std::strtoul(stack, nullptr, 10));
    for (const unsigned long address : options.watches) {
        const gekko::u32 target = static_cast<gekko::u32>(address);
        system.cpu().install_hle(
            target,
            [target, watch_hits, watch_limit, watch_stack](gekko::Cpu& cpu) {
                int& hits = (*watch_hits)[target];
                ++hits;
                if (hits <= watch_limit) {
                    char row[256];
                    std::snprintf(row, sizeof(row),
                                  "[watch] 0x%08X lr=0x%08X r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X t=%llu",
                                  target, cpu.lr, cpu.gpr[3], cpu.gpr[4], cpu.gpr[5],
                                  cpu.gpr[6], cpu.gpr[7],
                                  static_cast<unsigned long long>(cpu.game_clock().now()));
                    std::cout << row;
                    if (watch_stack != 0u) std::cout << " | " << cpu.guest_location(watch_stack);
                    std::cout << "\n";
                }
                return false;
            },
            "watch");
    }

    // ---- forensic resource lineage (--asset-trace) -------------------------
    //
    // Observe-only probes on the guest's own asset pipeline. Every handler
    // returns false, so the recompiled body still executes: these record what
    // the game did, they never do it for the game. The addresses are the guest
    // functions identified by their own string literals in the original DOL
    // (extract/functions.csv `strings` column), not by anything the host knows
    // about this game's data:
    //
    //   0x80177868  "stg%04d/stg%04d_dat.one"  builds the stage archive name
    //   0x80169294  "stg%04d/stg%04d_fx.one"
    //   0x80169248  "stg%04d/stg%04d_gdt.one"
    //   0x8003AB90  "%s/%s_texCM.one"
    //   0x8004CFD8  ONE open (callee of the loaders above)
    //   0x8004CE1C  ONE archive open/parse
    //   0x8004CD38  "One Ver %f"   header/version parse
    //   0x80177964  "PATH.PTP"     archive member lookup
    //   0x80043E78  "Expanding"    member decompression
    std::unique_ptr<gekko::AssetTracer> asset_tracer;
    if (options.asset_trace) {
        asset_tracer = gekko::AssetTracer::create(options.asset_trace->string(),
                                                  options.resource_lineage);
        if (!asset_tracer) {
            std::cerr << "cannot open asset trace " << options.asset_trace->string() << "\n";
            return 1;
        }
        gekko::AssetTracer::set_active(asset_tracer.get());
        asset_tracer->note("session",
                           "asset trace armed; probes are observe-only (handler returns false)");
        struct Probe {
            gekko::u32 address;
            const char* role;
        };
        static constexpr Probe kProbes[] = {
            {0x80177868u, "stage_dat_one_loader"}, {0x80169294u, "stage_fx_one_loader"},
            {0x80169248u, "stage_gdt_one_loader"}, {0x8003AB90u, "stage_texcm_one_loader"},
            {0x8004CFD8u, "one_open"},             {0x8004CE1Cu, "one_archive_parse"},
            {0x80177964u, "one_member_lookup"},    {0x80043E78u, "one_member_expand"},
        };
        for (const Probe& probe : kProbes) {
            gekko::AssetTracer* tracer = asset_tracer.get();
            const char* role = probe.role;
            const gekko::u32 address = probe.address;
            system.cpu().install_hle(
                address,
                [tracer, role, address](gekko::Cpu& cpu) {
                    tracer->guest_fn(cpu, role, address);
                    return false;
                },
                std::string("asset-trace ") + probe.role);
        }
        // The header parser reads the archive header out of guest memory at
        // *(r3 + 0x34). Dumping those bytes here is what closes the loop: they
        // are compared against the bytes on disk, so a one-byte edit to the
        // asset is visible in the trace and nowhere else.
        gekko::AssetTracer* tracer = asset_tracer.get();
        system.cpu().install_hle(
            0x8004CD38u,
            [tracer](gekko::Cpu& cpu) {
                tracer->guest_fn(cpu, "one_header_version_parse", 0x8004CD38u);
                const gekko::u32 header = cpu.read_guest_u32(cpu.gpr[3] + 0x34u);
                if (cpu.guest_memory().mapped(gekko::GcAddr(header), 32u)) {
                    tracer->guest_memory(cpu, "one_header_bytes_at_file+0x0C", header, 32u);
                }
                return false;
            },
            "asset-trace one_header_version_parse");
        std::cout << "Asset trace: " << options.asset_trace->string();
        if (!options.resource_lineage.empty()) {
            std::cout << " (filtered to paths containing \"" << options.resource_lineage << "\")";
        }
        std::cout << "\n";
    }

    // The GX pipe sites and the platform sites the generator lowered
    // (extract/gx-sites.txt, extract/platform-sites.txt): the interpreter
    // applies the same tables, so they are installed whether or not
    // recompiled code runs.
    system.install_gx_sites(gekko::recomp::kGxSites, gekko::recomp::kGxSiteCount);
    system.install_platform_sites(gekko::recomp::kPlatformSites, gekko::recomp::kPlatformSiteCount);
    system.install_lift_sites(gekko::recomp::kLiftSites, gekko::recomp::kLiftSiteCount);
    gekko::RecompiledCode recompiled_code;
    if (options.recompiled && gekko::recomp::kFunctionCount != 0) {
        recompiled_code.install(gekko::recomp::kFunctions, gekko::recomp::kFunctionCount);
        recompiled_code.install_ranges(gekko::recomp::kRanges, gekko::recomp::kRangeCount);
        system.cpu().attach_recompiled(&recompiled_code);
        std::cout << "Recompiled code: " << gekko::recomp::kFunctionCount
                  << " guest entry points over " << gekko::recomp::kRangeCount
                  << " translated address ranges, running as native x86-64" "\n";
    } else if (gekko::recomp::kFunctionCount == 0) {
        std::cout << "Recompiled code: none in this build; interpreting everything" "\n";
    } else {
        std::cout << "Recompiled code: disabled by --no-recomp" "\n";
    }
    // The ordered list of assets a session touched costs a string per first
    // touch, so it is built only for the two diagnostics that read it. The
    // COUNT is always maintained - it is one bit and one increment.
    system.assets().set_track_touched(options.asset_trace.has_value());
    system.cpu().set_record_fallbacks(options.fallback_audit);
    if (options.profile) gekko::cpu_diag().dispatch_timing = true;
    // The game's rumble requests (its Options -> Vibration setting decides
    // whether it makes any) reach the peripheral layer; mirror them to the
    // physical controller. Fires on the game thread at a CHANGE only, and
    // XInputSetState is thread-safe. Not for a headless run or a tape replay:
    // nobody holds the controller, and a run stopped mid-rumble leaves the
    // motors on (XInput keeps the last state after the process ends).
    if (options.xinput && options.window && !options.input_tape_play) {
        system.peripheral().set_rumble_listener([](std::size_t port, bool on) {
            gekko::set_xinput_rumble(static_cast<unsigned>(port), on);
        });
    }
    // Host tuning: applied here for everything that exists once System (and,
    // for the rasterizer entries, the DX12 backend) is up. Window and audio
    // scopes are applied where those objects are created.
    std::set<std::string> tuning_applied;
    std::set<std::string> tuning_scopes_seen;
    // Both of these have to be decided before System::start(), which is where
    // the HLE table is built.
    if (options.movie_trace || options.movie_frame_dump) {
        system.enable_movie_trace();
    }
    if (options.visibility_probe) {
        system.enable_visibility_probe();
    }
    if (options.native_visibility) {
        system.enable_native_visibility();
    }
    system.enable_native_health_bar(!options.legacy_health_bar);
    if (options.fov_probe) {
        system.enable_fov_probe();
    }
    if (options.movie_frame_dump) {
        system.set_movie_frame_dump(options.movie_frame_dump->generic_string());
    }
    if (options.ui_probe) {
        system.enable_ui_probe();
    }
    const bool audio_requested = options.audio.value_or(options.window);

    // Playback and capture independently consume the same guest-clocked DMA
    // samples. Capturing must not bypass opening the playback device.
    apply_host_tuning(options.tuning, TuneScope::System, &system,
                      nullptr, &tuning_applied);

    // Audio: every finished AI pass (NativeAudio, 5 ms of game time at 32 kHz)
    // is published into AudioRuntime (lock-free SPSC ring; WASAPI event-driven
    // consumer with a fixed-ratio resampler from the native 32,000 Hz). Game
    // time is the only clock: nothing here paces, servos or waits on the game,
    // and the renderer/presenter are not involved. Without playback the same
    // producer runs in counting mode, so every run reports the produced stream.
    gekko::AudioRuntime audio_runtime;
    std::ofstream audio_dump_stream;
    if (options.audio_dump) {
        audio_dump_stream.open(*options.audio_dump, std::ios::binary);
        if (audio_dump_stream) {
            std::cout << "[audio-dump] writing raw big-endian S16 stereo 32 kHz to "
                      << options.audio_dump->string() << "\n";
        } else {
            std::cerr << "[audio-dump] could not open " << options.audio_dump->string() << "\n";
        }
    }
    {
        gekko::AudioRuntime::Config audio_config;
        audio_config.mode =
            audio_requested ? gekko::AudioRuntime::Mode::Wasapi : gekko::AudioRuntime::Mode::Null;
        // ES-W7: start the endpoint open on the consumer thread and create the
        // render device meanwhile; wait_open() below finishes the handshake.
        audio_config.defer_handshake = true;
        audio_runtime.open(audio_config);
    }
    const bool capture_ready = audio_dump_stream.is_open() && audio_dump_stream.good();
    system.audio().set_pass_sink(
        [&audio_runtime, &audio_dump_stream, capture_enabled = capture_ready]
        (gekko::hw::HostAudioPointer64 bytes, gekko::hw::AudioByteOffset64 size) mutable {
            static constexpr gekko::u8 kSilentBlock[32] = {};
            if (bytes == nullptr) {
                // A pass whose buffer is not host-backed: the DAC still
                // consumed its time, so the timeline carries silence.
                audio_runtime.publish_silence(size / 4u);
            } else {
                audio_runtime.publish_block(bytes, size);
            }
            if (capture_enabled) {
                audio_dump_stream.write(
                    reinterpret_cast<const char*>(bytes != nullptr ? bytes : kSilentBlock),
                    static_cast<std::streamsize>(bytes != nullptr ? size : sizeof(kSilentBlock)));
                if (!audio_dump_stream) {
                    capture_enabled = false;
                    std::cerr << "[audio-dump] write failed; capture stopped\n";
                }
            }
        });
    // OFF unless explicitly asked for. This is the one place the runtime ever
    // lied to the game: it wrote MWS_PLY_STAT_PLAYEND into the player's own
    // handle whether or not the movie had finished, because with a frozen
    // Sofdec clock the event script would otherwise have waited for ever. The
    // clock runs now, so movies end through the player's own logic and the
    // forgery would only cut them off after 50 VI frames. It survives solely as
    // an A/B switch against the old behaviour.
    system.set_divide_intrinsics(options.divide_intrinsics);
    system.set_dynamic_cast_intrinsic(options.dynamic_cast_intrinsic);
    system.set_matrix_multiply_intrinsic(options.matrix_multiply_intrinsic);
    system.set_verify_matrix_multiply(options.verify_matrix_multiply);
    system.set_chara_colli_intrinsic(options.chara_colli_intrinsic);
    system.set_verify_chara_colli(options.verify_chara_colli);
    system.set_chara_colli_loops(options.chara_colli_loops);
    system.set_chara_colli_manage(options.chara_colli_manage);
    system.set_chara_colli_pass(options.chara_colli_pass);
    system.set_trace_stale_contacts(options.trace_stale_contacts);
    system.set_chara_colli_bone_native(options.chara_colli_bone_native);
    system.set_chara_colli_bone_memo(options.chara_colli_bone_memo);
    system.cpu().set_interrupt_fpr_shadow(options.fpr_shadow);
    system.set_sofdec_playend_stub(options.sofdec_playend_stub.value_or(false));
    gekko::UiTestScript ui_test_script;
    bool ui_test_active = false;
    if (options.ui_test_script || options.ui_test_mode) {
        std::string ui_error;
        bool ok = false;
        if (options.ui_test_script) {
            std::ifstream stream(*options.ui_test_script);
            if (!stream) {
                std::cerr << "ERROR: could not open " << options.ui_test_script->string() << "\n";
                return 2;
            }
            std::ostringstream text;
            text << stream.rdbuf();
            ok = gekko::UiTestScript::parse(text.str(), ui_test_script, &ui_error);
        } else {
            ok = gekko::UiTestScript::builtin(*options.ui_test_mode, ui_test_script, &ui_error);
        }
        if (!ok) {
            std::cerr << "ERROR: " << ui_error << "\n";
            return 2;
        }
        ui_test_active = true;
        std::cout << "[ui-test] " << ui_test_script.step_count() << " steps, "
                  << ui_test_script.total_frames() << " VI fields"
                  << (ui_test_script.looping() ? ", looping" : "") << "\n";
    }

    if (options.verify_recompiled) {
        // Per-call segments are ~17 instructions on the boot route, so a 400M
        // oracle budget produces ~23M segments; 2M capped coverage at 8.6%.
        system.cpu().set_verify_recompiled(true, 16, 50000000ull);
        std::cout << "Verifying every recompiled call against the interpreter\n";
    }
    system.cpu().set_history_depth(options.debug_history ? 4096u : 0u);
    system.cpu().set_instruction_trace(options.trace);
#if !defined(SHADOW_RENDER_LIFTED)  // G5: the legacy renderer and its configuration
    gekko::NativeRenderer native_renderer;
#endif
    std::unique_ptr<gekko::NativeAssetManager> native_assets;
#if !defined(SHADOW_RENDER_LIFTED)  // G5
    gekko::NativeRendererConfig native_config;
    native_config.frames_in_flight = options.native_frames_in_flight;
    native_config.gpu = options.native_gpu;
    // Windowed, the EFB backing follows the configured window size unless
    // --native-output names one; headless defaults to the console's 640x480.
    const bool explicit_output = options.native_output_explicit;
    gekko::u32 native_width = options.native_output_width;
    gekko::u32 native_height = options.native_output_height;
    if (explicit_output && options.window) {
        // Under the resolution architecture --native-output names the
        // PRESENTATION rectangle, and a window already is one. Honouring
        // both froze the renderer's Hor+ aspect at the --native-output
        // rect while the presenter kept fitting to the window, so the
        // frustum was widened for one aspect and letterboxed into another.
        std::cout << "NOTE: --native-output is ignored while a window is open; the window's "
                     "client area is the presentation rectangle. Use --internal-scale to "
                     "choose the render resolution.\n";
    }
    if (options.window && display.window_width > 0 && display.window_height > 0) {
        // A first guess; the window loop reports the real client area (and
        // every later resize) through NativeGpu::request_output_size.
        const gekko::DisplayLayout guess = gekko::make_display_layout(
            display, display.window_width, display.window_height);
        native_width = static_cast<gekko::u32>(guess.output_width);
        native_height = static_cast<gekko::u32>(guess.output_height);
    }
    native_config.gpu_settings.output_width = native_width;
    native_config.gpu_settings.output_height = native_height;
    // The display configuration's aspect mode, HUD scale and inset apply as
    // they do to the legacy path; the EFB backing is the output itself.
    gekko::DisplayLayout native_layout = gekko::make_display_layout(
        display, static_cast<int>(native_width), static_cast<int>(native_height));
    native_layout.output_width = static_cast<int>(native_width);
    native_layout.output_height = static_cast<int>(native_height);
    native_config.gpu_settings.hud_layout = native_layout;
    native_config.gpu_settings.content_aspect = gekko::render_content_aspect(native_layout);
    // 0 keeps the renderer's own `auto`, so a window resize re-derives the
    // scale from the new rectangle; a non-zero value pins what the user
    // asked for across every resize.
    native_config.gpu_settings.internal_scale =
        options.internal_scale_auto ? 0u : static_cast<gekko::u32>(internal_scale);
    native_config.gpu_settings.effect_footprint = options.effect_footprint;
    // One control for both backends: --no-logical-pixel-snap turns off the
    // raster-border snap here exactly as it does in the legacy raster.
    native_config.gpu_settings.logical_pixel_snap = options.logical_pixel_snap;
    if (options.native_screenshot) {
        native_config.gpu_settings.screenshot_path = *options.native_screenshot;
        native_config.gpu_settings.screenshot_frame = options.native_screenshot_frame;
        native_config.gpu_settings.screenshot_stride = options.native_screenshot_stride;
        native_config.gpu_settings.screenshot_count = options.native_screenshot_count;
    }
    if (options.native_copy_dump) {
        std::filesystem::create_directories(*options.native_copy_dump);
        native_config.gpu_settings.copy_dump_directory = *options.native_copy_dump;
        native_config.gpu_settings.copy_dump_frame = options.native_copy_dump_frame;
    }
    if (options.geometry_failure_dump) {
        std::filesystem::create_directories(*options.geometry_failure_dump);
        native_config.gpu_settings.geometry_failure_dump = *options.geometry_failure_dump;
    }
    if (options.native_dump) {
        native_config.dump_directory = *options.native_dump;
        native_config.dump_first_frame = options.native_dump_first;
        native_config.dump_last_frame = options.native_dump_last;
    }
#endif  // G5
#if defined(SHADOW_RENDER_LIFTED)
    // R2e (docs/lift-20261006/R2_DRIVER.md 4.3): the generated code lowers the RenderWare driver's draw-facing
    // entries into the scaffolding, which describes frames to the native renderer; RwCapture, the old renderer and
    // its presenter are not started. The scaffolding must be installed before any guest instruction runs.
    if (gekko::recomp::kFunctionCount == 0u) {
        std::cerr << "ERROR: the lifted renderer needs the recompiled game\n";
        return 4;
    }
    gekko::LiftedRender lifted_render;
    {
        gekko::LiftedRenderOptions lifted_options;
        lifted_options.vsync = options.vsync;
        lifted_options.collect_timing = options.telemetry_log.has_value() || options.performance_log.has_value();
        lifted_options.game_framing = options.game_framing;
        // G10: the render size (--render-width / --render-height, display.cfg), aspect-locked by the renderer.
        lifted_options.render_width = display.render_width > 0 ? static_cast<std::uint32_t>(display.render_width) : 0u;
        lifted_options.render_height =
            display.render_height > 0 ? static_cast<std::uint32_t>(display.render_height) : 0u;
        std::string lifted_error;
        if (!lifted_render.start(lifted_options, &lifted_error)) {
            std::cerr << "ERROR: native renderer: " << lifted_error << std::endl;
            return 4;
        }
        // The 2D identity hooks' announcements lay the game's 2D out for the real output (R2_DRIVER.md 4.8).
        system.set_hud_listener(lifted_render.hud_listener());
        lifted_render.set_draw_distance(options.draw_distance);
        {
            std::string layer_error;
            if (!lifted_render.set_effect_layers(options.effects_off, options.effects_solo, options.effects_isolate,
                                                 &layer_error)) {
                std::cerr << "ERROR: " << layer_error << " (layers: " << gekko::LiftedRender::effect_layer_names()
                          << ")\n";
                std::exit(2);
            }
            lifted_render.set_fps_counter(options.fps_counter);
            if (!options.effects_off.empty() || !options.effects_solo.empty())
                std::cout << "[effect-layers] " << lifted_render.effect_layers() << '\n';
        }
        lifted_render.set_fade_in_ticks(static_cast<gekko::u64>(options.fade_in_ms) *
                                        (gekko::kGameTicksPerSecond / 1000u));
        // The lifted build's native owners (extract/native-owners-r2.txt: the lock-on mark's draw).
        lifted_render.install_owners(system.cpu());
        // The cull cone is the displayed cone (R2_DRIVER.md 4.12): the renderer draws each view's width from the
        // game's vertical view and the view's aspect, so the visibility test and the canonical camera do too.
        system.display().set_width_follows_height(true);
        system.display().set_game_framing(options.game_framing);
        // The lifted renderer's savestate block (gekko/savestate.h, version 19): the R2 driver's state RenderWare
        // observes, its handle words saved as 0, and after a load the driver's resources and the views' histories
        // dropped.
        {
            gekko::RendererStateHooks hooks;
            hooks.kind = gekko::RendererStateKind::Lifted;
            hooks.save = [&lifted_render](std::vector<gekko::u8>& out) { lifted_render.save_state(out); };
            hooks.load = [&lifted_render](const gekko::u8*& cursor, const gekko::u8* end) {
                return lifted_render.load_state(cursor, end);
            };
            hooks.loaded = [&lifted_render]() { lifted_render.on_state_loaded(); };
            hooks.own_words = [&lifted_render](std::vector<gekko::u32>& out) { lifted_render.own_words(out); };
            gekko::set_renderer_state_hooks(std::move(hooks));
        }
        if (options.native_screenshot) {
            gekko::LiftedCapture capture;
            capture.path = options.native_screenshot->string();
            capture.first_frame = options.native_screenshot_frame;
            capture.stride = options.native_screenshot_stride;
            capture.count = options.native_screenshot_count;
            // --native-output WxH sizes the captures (e.g. 4:3 for a comparison where Hor+ is the identity).
            if (options.native_output_explicit) {
                capture.width = options.native_output_width;
                capture.height = options.native_output_height;
            }
            lifted_render.request_capture(capture);
        }
        if (!options.window)
            lifted_render.start_headless();
    }
    std::cout << "Native renderer: the lowered RenderWare driver draws through the R2 renderer\n";
#else
    if (!native_renderer.start(native_config)) {
        std::cerr << "ERROR: native renderer: " << native_renderer.last_error() << std::endl;
        return 4;
    }
    gekko::RwCapture& capture = system.enable_native_capture();
    capture.set_queue(&native_renderer.queue());
    capture.set_validate_texture_content(options.native_validate_textures);
    // STEP-0 measurement R4/COHERE/HASH-TOKEN (default off). Enabled before
    // system.start(): RwCapture::install() (called LAST by System::start)
    // then chains the DCFlushRange/DCStoreRange observers after every
    // production hook.
    if (options.measure.count("gx-inputs") != 0u) {
        capture.enable_gx_input_digest();
        system.gx_sync().enable_input_digest();
        std::cout << "Measure: gx-inputs digests enabled (observational)\n";
    }
    if (options.measure.count("r4") != 0u) {
        gekko::RwCapture::CoherenceProbeConfig r4_config;
        capture.enable_coherence_probe(r4_config);
        std::cout << "Measure: r4 geometry coherence probe enabled (observational)\n";
    }
    std::cout << "Native renderer: RenderWare capture on, " << options.native_frames_in_flight
              << " frame(s) in flight\n";
#endif
    {
        // ES-W7: the audio handshake started before the renderer; finish it now, before
        // any guest code runs (nothing has been published yet). Same outcomes as before.
        const bool live = audio_runtime.wait_open();
        const gekko::AudioRuntime::Report opened = audio_runtime.report();
        if (audio_requested && live) {
            std::cout << "Host audio: " << opened.device_name << " (" << opened.device_format
                      << ", " << opened.device_rate << " Hz, " << opened.device_channels
                      << " ch, period " << opened.device_period_frames << " frames / "
                      << opened.device_period_ms << " ms"
                      << (opened.low_latency_period ? ", IAudioClient3" : "")
                      << (opened.mmcss ? ", MMCSS" : "")
                      << (opened.open_pending_at_return ? ", still opening" : "")
                      << "); guest source " << opened.source_rate_hz << " Hz, resampler "
                      << opened.resampler_up << "/" << opened.resampler_down << " x "
                      << opened.resampler_taps << " taps\n";
        } else if (audio_requested) {
            std::cerr << "Host audio unavailable: " << opened.last_error << "\n";
        }
    }
#if defined(SHADOW_RENDER_LIFTED)
    // ES-M1 (2026-10-20): the native asset manager is the legacy capture renderer's texture cache. Its only reader is
    // the RenderWare capture layer (rw_capture.cpp, enabled only in the #else branch above); the lifted renderer
    // imports its own resources. Built here it parsed and decoded every texture file the game read (fed by
    // native_file_api's notify_file) on 31 threads with zero lookups (123 of 123 lifted runs). Not constructed:
    // NativeAssetManager::active() stays null, so the feed calls are no-ops.
    if (options.native_assets) {
        std::cout << "Native asset manager: not started (the lifted renderer imports its own resources; "
                     "nothing in this build reads the cache)\n";
    }
#else
    if (options.native_assets) {
        // The manager loads the guest's OWN .one/.txd files from files/
        // and decodes their rasters with the guest's decoder; the capture
        // layer serves them only on a content-hash hit, so a miss is never
        // a rendering change (see docs/NATIVE_ASSET_MANAGER_2026-09-20.md).
        native_assets = std::make_unique<gekko::NativeAssetManager>(*root, 0u, options.job_workers);
        native_assets->start();
        gekko::NativeAssetManager::set_active(native_assets.get());
    }
#endif
    system.start();
    // The generated code has no original body for a native owner: every one
    // must be installed before recompiled code runs.
    if (options.recompiled && gekko::recomp::kFunctionCount != 0) {
        for (std::size_t index = 0; index < gekko::recomp::kNativeOwnerCount; ++index) {
            if (!system.cpu().has_hle(gekko::recomp::kNativeOwners[index])) {
                std::cerr << "ERROR: native owner 0x" << std::hex << gekko::recomp::kNativeOwners[index]
                          << std::dec << " is not installed; its original body is not in this image\n";
                return 6;
            }
        }
    }
    // Chained after the production hook owners, before any guest instruction.
    // Otherwise System::start can replace an observer's entry silently.
#if !defined(GEKKO_SHIPPING)
    if (boot_contract_observer) boot_contract_observer->install(system);
#endif

    // STEP-0 measurement (r5probes): installed AFTER System::start() so no
    // production install_hle can silently replace a probe (audit R8). Each
    // probe is a chained observer: it runs, then the production handler (if
    // any) decides the call exactly as before.
    std::unique_ptr<gekko::MeasureProbes> measure_probes;
    if (options.measure.count("r5probes") != 0u) {
        gekko::MeasureProbes::Config probe_config;
        std::string probe_error;
        if (!gekko::MeasureProbes::parse_spec(options.measure_probes_spec, probe_config,
                                              probe_error)) {
            std::cerr << "ERROR: --measure-probes: " << probe_error << "\n";
            return 2;
        }
        measure_probes = std::make_unique<gekko::MeasureProbes>(system, std::move(probe_config));
        measure_probes->install();
        gekko::MeasureProbes::set_active(measure_probes.get());
        std::cout << "Measure: r5probes installed (" << options.measure_probes_spec << ")\n";
    }
    // Every item's JSON, collected at exit and written to --measure-out.
    std::vector<std::pair<std::string, std::string>> measure_results;

    // Development savestate slot. Kept beside the memory card under the game
    // data directory so a slot travels with the disc it was taken from.
    const std::filesystem::path state_slot =
        options.state_file ? *options.state_file
                           : (executable_directory() / "save-data" / "slot0.gekkostate");
    if (options.load_state_at_start) {
        // Before the first instruction retires, so no guest function is on the
        // native stack and nothing has been submitted to the renderer yet.
        std::string state_error;
        if (!gekko::load_state(system, state_slot, &state_error)) {
            std::cerr << "ERROR: " << state_error << "\n";
            return 5;
        }
        std::cout << "Restored savestate " << state_slot.string() << " at "
                  << game_seconds(system) << " s of game time\n";
    }

    // ---- the comparison workstation's controls (docs/comparator/README.md) ----------------------------------------
    // The frames the game has handed to its renderer (the lifted driver's frames; the capture's presents).
    const auto render_frames = [&]() -> gekko::u64 {
#if defined(SHADOW_RENDER_LIFTED)
        return lifted_render.next_frame();
#else
        const gekko::RwCapture* capture_layer = system.native_capture();
        return capture_layer != nullptr ? capture_layer->report().presents : 0u;
#endif
    };
    struct DevCapture {
        gekko::u64 written = 0;
        gekko::u64 frame = 0;
        std::string path;
        gekko::u64 game_ticks = 0;  // the captured frame's game tick
    };
    const auto dev_capture_result = [&]() -> DevCapture {
#if defined(SHADOW_RENDER_LIFTED)
        const gekko::LiftedRender::CaptureResult result = lifted_render.capture_result();
        return DevCapture{result.written, result.frame, result.path, result.game_ticks};
#else
        if (gekko::NativeGpu* gpu = native_renderer.gpu()) {
            const gekko::NativeGpu::ScreenshotResult result = gpu->screenshot_result();
            return DevCapture{result.written, result.frame, utf8_path(result.path), result.game_ticks};
        }
        return DevCapture{};
#endif
    };
    // Captures the `ahead`-th frame the game finishes from now (1: the next); returns the file it will be written to
    // (the lifted renderer writes PNG at width x height, the legacy renderer its EFB as a binary PPM).
    const auto dev_arm_capture = [&](const std::filesystem::path& path, gekko::u64 ahead, gekko::u32 width,
                                     gekko::u32 height) -> std::filesystem::path {
        const gekko::u64 n = ahead == 0u ? 1u : ahead;
#if defined(SHADOW_RENDER_LIFTED)
        std::filesystem::path png = path;
        png.replace_extension(".png");
        lifted_render.arm_capture(png.string(), lifted_render.next_frame() + n - 1u, width, height);
        return png;
#else
        (void)width;
        (void)height;
        std::filesystem::path ppm = path;
        ppm.replace_extension(".ppm");
        if (gekko::NativeGpu* gpu = native_renderer.gpu()) {
            gpu->request_screenshot(ppm, static_cast<gekko::u32>(render_frames() + n));
        }
        return ppm;
#endif
    };
    // ... the first frame the game finishes at or after `game_tick` (lockstep: the clock both builds share).
    const auto dev_arm_capture_at_tick = [&](const std::filesystem::path& path, gekko::u64 game_tick,
                                             gekko::u32 width, gekko::u32 height) -> std::filesystem::path {
#if defined(SHADOW_RENDER_LIFTED)
        std::filesystem::path png = path;
        png.replace_extension(".png");
        lifted_render.arm_capture_at_tick(png.string(), game_tick, width, height);
        return png;
#else
        (void)width;
        (void)height;
        std::filesystem::path ppm = path;
        ppm.replace_extension(".ppm");
        if (gekko::NativeGpu* gpu = native_renderer.gpu()) gpu->request_screenshot_at_tick(ppm, game_tick);
        return ppm;
#endif
    };
    std::vector<GuestRange> report_ranges;
    for (const std::string& text : options.report_peeks) {
        std::string range_error;
        if (!parse_guest_ranges(text, report_ranges, &range_error)) {
            std::cerr << "ERROR: --report-peek: " << range_error << "\n";
            return 2;
        }
    }
    // Input tapes: playback latches the tape's samples where the run would latch its own; a recording starts here, at
    // the run's starting state (after --load-state).
    gekko::InputTape input_tape;
    if (options.input_tape_play) {
        if (ui_test_active) {
            std::cerr << "ERROR: --input-tape-play and --ui-test(-script) both latch the controller\n";
            return 2;
        }
        std::string tape_error;
        if (!input_tape.load(*options.input_tape_play, &tape_error)) {
            std::cerr << "ERROR: " << tape_error << "\n";
            return 2;
        }
        if (input_tape.start_tick() != system.game_ticks()) {
            std::cerr << "WARNING: the input tape starts at game tick " << input_tape.start_tick()
                      << " but this run starts at " << system.game_ticks()
                      << " - it was not recorded from this starting state\n";
        }
        std::cout << "[input-tape] playing " << options.input_tape_play->string() << ": " << input_tape.size()
                  << " samples from tick " << input_tape.start_tick() << "\n";
    }
    gekko::InputTape recording_tape;
    gekko::InputTape* const tape_recorder = options.input_tape_record ? &recording_tape : nullptr;
    // Replay markers (docs/tools/REPLAY_MARKERS.md): each press of the marker key appends one line - where the game
    // is (tick, frames) and where the tape is (the next sample) - so the moment can be replayed and inspected again.
    const std::filesystem::path marker_path = options.marker_file ? *options.marker_file
                                              : options.input_tape_play
                                                  ? std::filesystem::path(options.input_tape_play->string() + ".markers.jsonl")
                                              : options.input_tape_record
                                                  ? std::filesystem::path(options.input_tape_record->string() + ".markers.jsonl")
                                                  : std::filesystem::path();
    const std::string marker_session = [] {
        const std::time_t now = std::time(nullptr);
        char text[32];
        std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%S", std::localtime(&now));
        return std::string(text);
    }();
    gekko::u64 marker_ordinal = 0;
    gekko::u64 range_ordinal = 0;
    bool range_open = false;
    // `kind`: "point" (the marker key) or "range-start" / "range-end" (the range key's alternate presses).
    // `point`: where the user pointed (a click or the mouse at the marker key), a fraction of the window from the top left.
    const auto record_marker = [&](gekko::u64 game_ticks, double wall_s, const char* kind = "point",
                                   const double* point = nullptr) {
        ++marker_ordinal;
        const char* mode = input_tape.playing() ? "play" : tape_recorder != nullptr ? "record" : "live";
        gekko::dev::Json line;
        line.field("marker", marker_ordinal)
            .field("session", marker_session)
            .field("kind", kind);
        if (std::strcmp(kind, "point") != 0)
            line.field("range", range_ordinal);
        line.field("mode", mode)
            .field("game_ticks", game_ticks)
            .field("game_seconds", static_cast<double>(game_ticks) / static_cast<double>(gekko::kGameTicksPerSecond))
            .field("render_frame", render_frames())
            .field("vi_frame", static_cast<gekko::u64>(system.video().frames()))
            .field("wall_seconds", wall_s);
        if (point != nullptr)
            line.field("x", point[0]).field("y", point[1]);
        if (options.input_tape_play) {
            line.field("tape", utf8_path(*options.input_tape_play))
                .field("tape_sample", static_cast<gekko::u64>(input_tape.cursor()))
                .field("tape_samples", static_cast<gekko::u64>(input_tape.size()));
        } else if (options.input_tape_record) {
            line.field("tape", utf8_path(*options.input_tape_record));
        }
        std::fprintf(stderr, "[marker] #%llu %s at game tick %llu (%.3f s), render frame %llu (%s)\n",
                     static_cast<unsigned long long>(marker_ordinal), kind, static_cast<unsigned long long>(game_ticks),
                     static_cast<double>(game_ticks) / static_cast<double>(gekko::kGameTicksPerSecond),
                     static_cast<unsigned long long>(render_frames()),
                     std::strcmp(kind, "point") == 0 ? options.marker_key.c_str() : options.range_key.c_str());
        if (marker_path.empty())
            return;
        std::ofstream out(marker_path, std::ios::binary | std::ios::app);
        out << line.str() << "\n";
        std::cout << "[marker] #" << marker_ordinal << " -> " << marker_path.string() << "\n";
    };
    if (tape_recorder != nullptr) {
        tape_recorder->begin_recording(system.game_ticks(), static_cast<int>(display.window_width),
                                       static_cast<int>(display.window_height));
        std::cout << "[input-tape] recording to " << options.input_tape_record->string() << " from tick "
                  << system.game_ticks() << "\n";
    }
#if defined(SHADOW_RENDER_LIFTED)
    // The game-visible output (HW_REPORT J, F1-F3). The game's state depends on the output's aspect: the R2 driver's
    // screen tests (the widescreen HUD clamp, rw_driver projection.cpp visible_screen) and the native frustum
    // (System::display().aspect()) both widen with it. One value feeds both, set before the game runs and changed
    // only at host services - a window's resize is latched at the next one, like a controller sample - and a tape
    // records it (output records) and replays it, so a replay sees the output the recording saw at the same ticks,
    // windowed or headless. Before the game runs: the window's client area (windowed), the captures' explicit size
    // (--native-output, headless), else the configured display (--width / --height / display.cfg).
    int game_output_w = 0;
    int game_output_h = 0;
    unsigned game_output_changes = 0;
    std::atomic<std::uint64_t> pending_game_output{0};  // the UI thread's newest client size: (w << 32) | h
    bool game_output_mismatch_noted = false;
    // (Headless captures with an explicit --native-output keep defining the output, as before.)
    bool exact_render_size = display.render_width > 0 && display.render_height > 0 &&
                             !(options.native_screenshot && options.native_output_explicit);
    const auto set_game_output = [&](int w, int h, gekko::u64 tick, const char* source) {
        if (w <= 0 || h <= 0 || (w == game_output_w && h == game_output_h)) return;
        game_output_w = w;
        game_output_h = h;
        system.display().resize(w, h);
        lifted_render.set_output_aspect(static_cast<gekko::u32>(w), static_cast<gekko::u32>(h));
        if (tape_recorder != nullptr) tape_recorder->note_output(tick, w, h);
        if (++game_output_changes <= 16u) {
            std::cout << "[output] the game's output is " << w << "x" << h << " from game tick " << tick << " ("
                      << source << ")\n";
        }
    };
    const auto start_game_output = [&](int w, int h, const char* source) {
        const gekko::u64 tick = system.game_ticks();
        if (input_tape.playing() && input_tape.has_outputs()) {
            int tw = 0, th = 0;
            if (input_tape.output_at(tick, &tw, &th)) {
                set_game_output(tw, th, tick, "the input tape");
                return;
            }
        } else if (input_tape.playing()) {
            std::cerr << "WARNING: the input tape has no output records (version 1): the game's output is this run's "
                      << w << "x" << h << " (" << source << "); the replay matches the recording only at the "
                      << "recording's aspect (its display line: " << input_tape.width() << "x" << input_tape.height()
                      << ")\n";
        }
        // An exact render size (both a width and a height asked): the game's output IS that size, so the 3D scene is
        // rasterized at exactly the requested pixels with the camera, culling and HUD at its aspect; a window of
        // another shape shows it fitted with bars (the present scale). docs/RESOLUTION_AUDIT.md.
        if (exact_render_size) {
            set_game_output(static_cast<int>(display.render_width), static_cast<int>(display.render_height), tick,
                            "the requested render size");
            return;
        }
        set_game_output(w, h, tick, source);
    };
    const auto latch_game_output = [&](gekko::u64 tick) {
        const std::uint64_t pending = pending_game_output.load(std::memory_order_acquire);
        const int pw = static_cast<int>(pending >> 32);
        const int ph = static_cast<int>(pending & 0xffffffffu);
        // The display settings' game group (gekko/display_settings.h), here where the driver reads it: the draw
        // distance, and the render size - an exact one is the game's output from this tick, like a latched resize (a
        // tape's output records still win); none follows the window again through the latch below.
        {
            gekko::DisplaySettings settings;
            if (gekko::display_settings().take_changes(gekko::kDisplayGameChange, &settings) != 0u) {
                lifted_render.set_draw_distance(settings.draw_distance);
                lifted_render.set_render_size(static_cast<gekko::u32>(settings.render_width),
                                              static_cast<gekko::u32>(settings.render_height));
                exact_render_size = settings.render_width > 0 && settings.render_height > 0 &&
                                    !(options.native_screenshot && options.native_output_explicit);
                if (exact_render_size && !(input_tape.playing() && input_tape.has_outputs()))
                    set_game_output(settings.render_width, settings.render_height, tick, "the display settings");
            }
        }
        if (input_tape.playing() && input_tape.has_outputs()) {
            int tw = 0, th = 0;
            if (input_tape.output_at(tick, &tw, &th)) set_game_output(tw, th, tick, "the input tape");
            if (pending != 0u && !game_output_mismatch_noted && game_output_w > 0 &&
                static_cast<long long>(pw) * game_output_h != static_cast<long long>(ph) * game_output_w) {
                game_output_mismatch_noted = true;
                std::cerr << "WARNING: the window is " << pw << "x" << ph << " but the input tape's output is "
                          << game_output_w << "x" << game_output_h
                          << ": the game follows the tape (the picture's culling may not match the window)\n";
            }
            return;
        }
        // With an exact render size a resize only changes how the picture is fitted into the window.
        if (pending != 0u && !exact_render_size) set_game_output(pw, ph, tick, "window resize");
    };
    // Headless: the captures' explicit size defines the output, else the configured display.
    const int headless_output_w = options.native_screenshot && options.native_output_explicit
                                      ? static_cast<int>(options.native_output_width)
                                      : (display.window_width > 0 ? static_cast<int>(display.window_width) : 640);
    const int headless_output_h = options.native_screenshot && options.native_output_explicit
                                      ? static_cast<int>(options.native_output_height)
                                      : (display.window_height > 0 ? static_cast<int>(display.window_height) : 480);
    const char* const headless_output_source =
        options.native_screenshot && options.native_output_explicit ? "the captures' --native-output"
                                                                    : "the configured display";
#endif
    // --stop-at-tick: the boundary after the host service at that tick ends the run (headless).
    bool stop_at_requested = false;
    gekko::u64 stop_at_request_tick = 0;
    // The development link (windowed runs): connected now, served by the game thread between traversals.
    gekko::dev::Link dev_link;
    if (options.dev_link) {
        gekko::dev::Json hello;
        const gekko::RendererStateKind kind = gekko::renderer_state_kind(system);
        hello.field("name", options.dev_link_name)
            .field("pid", static_cast<gekko::u64>(GetCurrentProcessId()))
            .field("exe", utf8_path(executable_path()))
#if defined(SHADOW_RENDER_LIFTED)
            .field("flavour", "lifted")
            .field("capture_format", "png")
#else
            .field("flavour", "native")
            .field("capture_format", "ppm")
#endif
#if defined(SHADOW_REFERENCE_RUNNER)
            .field("reference", true)
#else
            .field("reference", false)
#endif
            .field("savestate_version", gekko::save_state_version())
            .field("renderer_kind", static_cast<gekko::u32>(kind))
            .field("renderer", gekko::renderer_state_kind_name(kind))
            .field("game_ticks", system.game_ticks())
            .field("ticks_per_second", gekko::kGameTicksPerSecond)
            .field("windowed", options.window)
            .field("snapshot_key", options.snapshot_key)
            .field("paused", options.start_paused)
            .field("tape_recording", tape_recorder != nullptr)
            .field("tape_playing", input_tape.playing());
        std::string link_error;
        if (!dev_link.start(*options.dev_link, hello, &link_error)) {
            std::cerr << "ERROR: " << link_error << "\n";
            return 2;
        }
        std::cout << "[dev-link] connected to " << *options.dev_link << " as '" << options.dev_link_name << "'\n";
        if (!options.window) std::cout << "[dev-link] headless runs do not serve requests (windowed only)\n";
    }

    if (options.wait_watchdog_ms != 0ul) {
        system.set_wait_watchdog(static_cast<gekko::u32>(options.wait_watchdog_ms));
        std::cout << "Wait-point watchdog: " << options.wait_watchdog_ms << " ms of wall time\n";
    }
    std::cout << "\nExecuting the original PAL game image from its entry point...\n";
    // Game time: where this session started and where the budget ends it.
    const gekko::u64 initial_game_ticks = system.game_ticks();
    const gekko::u64 budget_end_ticks =
        options.budget_ms == 0ull
            ? 0ull
            : initial_game_ticks + options.budget_ms * (gekko::kGameTicksPerSecond / 1000u);
    // Set by the host service that ends the run at the budget. A budget is not
    // a stop: the report keeps saying "still running", as it always has.
    bool budget_reached = false;
    // The budget ends the run at the first point at or after it where a
    // savestate can be written: between gameplay traversals (a traversal that
    // dispatched a task keeps part of the frame's walk in host state, and
    // save_state refuses it). Under the native retrace the clock advances
    // inside traversals, so the stop is requested for the next traversal
    // boundary (System::request_stop_between_traversals); a traversal still
    // open a game second past the budget ends the run anyway. Deterministic:
    // decided in game time.
    const auto budget_due = [&system, &budget_reached, budget_end_ticks](gekko::u64 game_ticks) {
        if (budget_end_ticks == 0u || game_ticks < budget_end_ticks) return false;
        budget_reached = true;
        if (game_ticks >= budget_end_ticks + gekko::kGameTicksPerSecond) return true;
        system.request_stop_between_traversals(gekko::StopReason::HostRequest, "budget reached");
        return false;
    };
    GameThreadPerformancePolicy game_thread_policy;
    // Where the thread executing translated code ran, sampled at each host service (R8; both paths).
    GameThreadPlacement game_thread_placement;
    // Filled by the windowed presentation summary so the JSON session report
    // can include it after those locals go out of scope.
    struct PresentationSummary {
        bool available = false;
        std::size_t intervals = 0;
        double average_fps = 0.0;
        double minimum_fps = 0.0;
        double one_percent_low = 0.0;
        double target_fps = 0.0;
        bool interpolation = false;
        gekko::u64 blend_presents = 0;
        gekko::u64 interpolation_toggles = 0;
        gekko::u64 native_previous_misses = 0;
        gekko::u64 native_acquire_misses = 0;
        bool present_fifo = false;
        gekko::u64 fifo_deferred_presents = 0;
        gekko::u64 fifo_lag_drops = 0;
        gekko::u64 fifo_fallbacks = 0;
        gekko::u64 rendered_frames = 0;
        gekko::u64 dropped_render_deadlines = 0;
        double execute_s = 0.0;
        double submit_s = 0.0;
        double retire_s = 0.0;
        double sleep_s = 0.0;
        double present_s = 0.0;
        gekko::u64 game_thread_stalls = 0;
        double longest_game_stall_ms = 0.0;
        gekko::u64 frame_production_stalls = 0;
        double longest_frame_stall_ms = 0.0;
        gekko::u64 no_gx_spells = 0;
        double longest_no_gx_ms = 0.0;
        double p50_ms = 0.0;
        double p95_ms = 0.0;
        double p99_ms = 0.0;
        double p999_ms = 0.0;
        double max_ms = 0.0;
    } presentation_summary;
    const auto execution_start = std::chrono::steady_clock::now();
    const double process_cpu_start = process_cpu_seconds_now();
    const std::vector<ThreadSample> threads_start = sample_threads();
    // The fixed simulation clock maps game time to real time. It does not
    // manufacture time or change any deadline: the host may finish one quantum
    // quickly, then this host-only clock lets it become idle.
    gekko::FixedSimulationClock::Config simulation_config;
    // One pacing quantum per System::kPacingTicks: every frame boundary of a 50 or 60 Hz game is a pacing boundary.
    simulation_config.pacing_hz = gekko::System::kPacingHz;
    if (options.catchup_rate >= 0.0) {
        simulation_config.maximum_catchup_rate = options.catchup_rate;
    }
    if (options.hitch_forgiveness_ms >= 0.0) {
        simulation_config.hitch_forgiveness_threshold = std::chrono::nanoseconds(
            std::llround(options.hitch_forgiveness_ms * 1000000.0));
    }
    gekko::FixedSimulationClock simulation_clock(simulation_config);
    simulation_clock.reset(system.game_ticks(), execution_start);
    gekko::u64 pacing_resyncs = 0;
    gekko::u64 pacing_debt_clamps = 0;
    gekko::u64 pacing_boundaries = 0;
    gekko::u64 pacing_skipped_ticks = 0;
    double pacing_maximum_debt_ms = 0.0;
    // Mirrors of the windowed pacer's headroom split, hoisted so the exit
    // report can see them. Written once, after the game thread has joined.
    gekko::u64 pacing_paced_services = 0;
    gekko::u64 pacing_unpaced_services = 0;
    // 2026-09 mission distributions. Guest-thread written inside the host
    // service, read after the game thread joins; the presentation ones are
    // UI-thread written and read after the loop. See host_stats.h.
    gekko::Histogram<256> quantum_hist{250000};     // 0.25 ms buckets, 64 ms range

    // ---- frame-tail decomposition ------------------------------------------
    //
    // The average frame is fine and has been for a long time: ~59.95 FPS with
    // the GPU at a quarter of its budget and half the game thread asleep. What
    // is NOT fine is the tail - p95 ~21.9 ms, p99 ~26.4 ms, max ~30.4 ms - and
    // nothing in the existing counters can say where a 26 ms quantum went,
    // because every one of them is a whole-session total.
    //
    // So each 60 Hz quantum is decomposed into the four things the game thread
    // can actually be doing, and the long ones are kept individually. The
    // residual is printed too: an unattributed remainder is the interesting
    // number, not an embarrassing one, because it is the part no existing
    // counter explains.
    //
    // It also records the quantum that FOLLOWS a long one. If the runtime is
    // oscillating between lateness and catch-up rather than holding cadence,
    // the frame after a long frame is systematically short, and that is a
    // different defect from "some frames are expensive" with a different fix.
    // A locally defined class cannot carry static data members, so the two
    // thresholds live beside it.
    constexpr gekko::u64 kFrameTailLongNs = 20000000u;  // 20 ms
    constexpr std::size_t kFrameTailKeep = 16;
    struct FrameTail {

        struct Sample {
            gekko::u64 wall_ns = 0;
            gekko::u64 guest_ns = 0;    // executing translated guest code
            gekko::u64 service_ns = 0;  // host service bodies
            gekko::u64 sleep_ns = 0;    // pacer sleep
            gekko::u64 overshoot_ns = 0;// slept past the deadline it asked for
            gekko::u64 instructions = 0;
            gekko::u32 ticks = 0;
            gekko::u64 next_wall_ns = 0;  // the quantum that followed
        };

        // Accumulated across the ticks of the quantum in progress.
        gekko::u64 guest_ns = 0, service_ns = 0, sleep_ns = 0, overshoot_ns = 0;
        gekko::u64 instructions = 0;
        gekko::u32 ticks = 0;

        gekko::u64 quanta = 0, long_quanta = 0;
        gekko::u64 wall_total = 0;
        // Totals over the long quanta only.
        gekko::u64 l_guest = 0, l_service = 0, l_sleep = 0, l_overshoot = 0, l_wall = 0;
        // The quantum after a long one.
        gekko::u64 after_long_total = 0, after_long_count = 0, after_long_short = 0;
        bool previous_was_long = false;

        std::vector<Sample> longest;

        void tick(gekko::u64 guest, gekko::u64 service, gekko::u64 slept,
                  gekko::u64 overshoot, gekko::u64 advanced_ticks) {
            guest_ns += guest;
            service_ns += service;
            sleep_ns += slept;
            overshoot_ns += overshoot;
            instructions += advanced_ticks;
            ++ticks;
        }

        void boundary(gekko::u64 wall_ns) {
            ++quanta;
            wall_total += wall_ns;
            if (previous_was_long) {
                after_long_total += wall_ns;
                ++after_long_count;
                // "Short" against the 16.666 ms cadence, i.e. the runtime
                // giving back time it borrowed.
                if (wall_ns < 16000000u) ++after_long_short;
            }
            const bool is_long = wall_ns >= kFrameTailLongNs;
            if (is_long) {
                ++long_quanta;
                l_wall += wall_ns;
                l_guest += guest_ns;
                l_service += service_ns;
                l_sleep += sleep_ns;
                l_overshoot += overshoot_ns;
                Sample sample;
                sample.wall_ns = wall_ns;
                sample.guest_ns = guest_ns;
                sample.service_ns = service_ns;
                sample.sleep_ns = sleep_ns;
                sample.overshoot_ns = overshoot_ns;
                sample.instructions = instructions;
                sample.ticks = ticks;
                longest.push_back(sample);
                std::sort(longest.begin(), longest.end(),
                          [](const Sample& a, const Sample& b) { return a.wall_ns > b.wall_ns; });
                if (longest.size() > kFrameTailKeep) longest.resize(kFrameTailKeep);
            } else if (previous_was_long && !longest.empty()) {
                // Attach the follower to the most recent long sample we kept.
                for (Sample& sample : longest) {
                    if (sample.next_wall_ns == 0u) { sample.next_wall_ns = wall_ns; break; }
                }
            }
            previous_was_long = is_long;
            guest_ns = service_ns = sleep_ns = overshoot_ns = 0;
            instructions = 0;
            ticks = 0;
        }
    };
    FrameTail frame_tail;
    gekko::u64 frame_tail_last_total = 0;
    gekko::Histogram<512> service_cost_hist{1000};  // 1 us buckets, 512 us range
    gekko::u64 service_backpressure_ns_total = 0;
    gekko::Histogram<256> unique_present_hist{250000};
    gekko::u64 unique_presents = 0;
    gekko::u64 repeat_presents = 0;
    // Arrival-order presentation (--present-fifo). deferred: ticks that showed
    // the older frame of a same-interval pair; lag_drops: ticks that skipped one
    // frame to end a lag held for kFifoMaxLagTicks; fallbacks: the older
    // generation could not be acquired, so the newest was shown.
    gekko::u64 fifo_deferred_presents = 0;
    gekko::u64 fifo_lag_drops = 0;
    gekko::u64 fifo_fallbacks = 0;
    gekko::u64 present_gaps = 0;
    // 2026-09-02 presenter-loop counters (D06-F3/F4/F5). UI-thread written,
    // read after the loop has ended; none of them is on a guest path.
    gekko::u64 blend_presents = 0;                 // presents that cross-faded two generations
    gekko::u64 native_acquire_misses = 0;          // the current generation's slot was gone
    gekko::u64 native_previous_misses = 0;         // gen-1 gone: presented unblended instead
    gekko::u64 interpolation_toggles = 0;          // hysteresis engage/release edges
    gekko::PlumePresenter::PresentStats presenter_stats{};
    bool native_presenter_failed = false;
    std::string presenter_error;
    if (options.window) {
        GEKKO_PROFILE_THREAD("present");
        gekko::HostWindow window;
#if defined(SHADOW_RENDER_LIFTED)
        // The native renderer owns the window's swap chain itself.
        window.set_plume_presentation(false);
#else
        window.set_plume_presentation(options.present_plume);
#endif
#if !defined(SHADOW_RENDER_LIFTED)  // G5
        gekko::NativeGpu* const native_gpu = native_renderer.gpu();
        if (native_gpu != nullptr) {
            // The native renderer's device; the presenter records on its own
            // queue, so no queue mutex is shared with the renderer thread.
            window.set_plume_external_context(native_gpu->device(), native_gpu->present_queue(),
                                              nullptr);
        }
#endif
        window.set_plume_verify(options.verify_plume);
        window.set_plume_gpu_blit(options.present_gpu_blit);
        apply_host_tuning(options.tuning, TuneScope::Window, &system,
                          &window, &tuning_applied);
        window.set_plume_area_resolve(options.present_area_resolve);
        window.set_vsync(options.vsync);
        window.set_plume_present_dump(options.present_dump, options.present_dump_frame);
        window.set_xinput_enabled(options.xinput);
        window.set_snapshot_key(snapshot_virtual_key(options.snapshot_key));
        window.set_marker_key(snapshot_virtual_key(options.marker_key));
        window.set_range_key(snapshot_virtual_key(options.range_key));
        // Replaying a tape, a click on the picture marks "the bug is here" (docs/tools/REPLAY_MARKERS.md).
        window.set_click_marks(options.input_tape_play.has_value());
#if defined(SHADOW_RENDER_LIFTED)
        // An exact render size: the window takes the picture's shape, so it shows it without bars.
        if (exact_render_size)
            gekko::HostWindow::set_content_aspect(static_cast<double>(display.render_width) / display.render_height);
#endif
        if (!window.create(display.window_width, display.window_height, "Shadow the Hedgehog",
                           display.fullscreen)) {
            std::cerr << "WARNING: could not open a window; running headless.\n";
#if defined(SHADOW_RENDER_LIFTED)
            start_game_output(headless_output_w, headless_output_h, headless_output_source);
            lifted_render.start_headless();
#endif
            system.set_host_service([&](gekko::u64 game_ticks) {
                if (budget_due(game_ticks)) {
                    budget_reached = true;
                    system.cpu().request_stop(gekko::StopReason::HostRequest, "budget reached");
                }
            });
            system.run();
            if (budget_reached) system.cpu().clear_stop();
            system.set_host_service({});
        } else {
            {
                int client_w = 0;
                int client_h = 0;
                window.client_size(client_w, client_h);
#if defined(SHADOW_RENDER_LIFTED)
                if (client_w > 0 && client_h > 0) {
                    start_game_output(client_w, client_h, "the window's client area");
                } else {
                    start_game_output(headless_output_w, headless_output_h, "the configured display");
                }
#endif
                if (client_w > 0 && client_h > 0) {
#if !defined(SHADOW_RENDER_LIFTED)
                    system.display().resize(client_w, client_h);
#endif
#if !defined(SHADOW_RENDER_LIFTED)  // G5
                    if (native_gpu != nullptr) {
                        const gekko::DisplayLayout layout =
                            gekko::make_display_layout(display, client_w, client_h);
                        native_gpu->request_output_size(
                            static_cast<gekko::u32>(layout.output_width),
                            static_cast<gekko::u32>(layout.output_height), layout,
                            options.internal_scale_auto
                                ? 0u
                                : static_cast<gekko::u32>(internal_scale));
                    }
#endif
                }
            }
#if defined(SHADOW_RENDER_LIFTED)
            std::cout << "Presentation: the native renderer's Direct3D 12 swap chain, vsync "
                      << (options.vsync ? "on" : "off") << "\n";
#else
            if (options.present_plume) {
                if (window.plume_active()) {
                    std::cout << "Presentation: plume, Direct3D 12, native renderer device"
                              << ", vsync " << (options.vsync ? "on" : "off")
                              << " on completed XFBs\n";
                } else {
                    std::cout << "Presentation: plume unavailable, falling back to GDI\n";
                }
            }
#endif
            // D06-F7: the presenter keeps the rasterizer's native XFB pin
            // until the flight slot that read it has retired. Default ON
            // (8-slot ring). The hook is the only route back to the renderer.
#if !defined(SHADOW_RENDER_LIFTED)  // G5
            if (native_gpu != nullptr) {
                window.set_plume_native_release_hook(
                    [](void* context, gekko::u32 slot, gekko::u64 generation) {
                        static_cast<gekko::NativeGpu*>(context)->release_output(slot, generation);
                    },
                    native_gpu);
            }
#endif
            // ---------------------------------------------------------------
            // Two threads, and no instruction slice.
            //
            // The game thread enters Cpu::run once and stays inside translated
            // native code. The host's periodic work runs at GameEvent::Frame,
            // every 5 ms of game time (System::kHostServiceTicks): the game
            // clock only moves at a wait point, so every service runs with all
            // guest state committed and the guest waiting.
            //
            // The UI thread owns the HWND, its message queue and the swap
            // chain - all three are thread-affine - and presents whatever
            // completed frame the game thread has published. It never touches
            // System, Cpu, Bus or the renderer's mutable state.
            // ---------------------------------------------------------------
            constexpr auto kHostStallThreshold = std::chrono::milliseconds(20);
            // Two frames missed at 60 Hz. This only opens a dry spell for
            // accounting; nothing is reported at this threshold.
            constexpr auto kFrameStallThreshold = std::chrono::milliseconds(33);
            // A dry spell's portions are reported only past this. At the
            // observed 14-54 FPS an isolated one-frame gap is 20-70 ms of
            // ordinary pacing, not a defect - 311 of the 498 frame-production
            // reports in the 2026-08-26 Westopolis session were exactly that.
            // 100 ms is at least three consecutive missed fields.
            constexpr auto kFrameProductionStallThreshold = std::chrono::milliseconds(100);
            RecoveredClockSleeper recovered_clock_sleep;

            std::atomic<bool> stop_requested{false};
            std::atomic<bool> game_done{false};
            // Savestate hotkeys are detected on the UI thread but must act on
            // the game thread: only the host-service boundary has the guest at
            // a command boundary with no guest frame on the native stack.
            std::atomic<bool> savestate_save_requested{false};
            std::atomic<bool> savestate_load_requested{false};
            // The development link (docs/comparator/README.md): a request waiting (set by its reader thread), the run
            // paused, the frame count a run_frames request pauses at (0: none), the host-service tick the pending
            // boundary was first asked for at and whether one is asked for.
            std::atomic<bool> dev_boundary{false};
            std::atomic<bool> dev_paused{options.start_paused && dev_link.connected()};
            std::atomic<gekko::u64> dev_run_target{0};
            std::atomic<gekko::u64> dev_run_target_ticks{0};  // run_ticks: pause once game time reaches it
            std::atomic<bool> dev_capture_only{false};         // capture {pause: false}: answer once written
            std::atomic<gekko::u64> dev_capture_only_baseline{0};
            std::atomic<gekko::u64> dev_request_tick{0};
            std::atomic<bool> dev_stop_pending{false};
            // One-button comparison snapshots (docs/comparator/README.md "One-button snapshots"): presses of the
            // snapshot key not served yet (the UI thread counts them), the boundary they asked for (the host-service
            // tick that first asked: a replay asks at the same one), and the next frame the snapshot waits for.
            std::atomic<unsigned> snapshot_requested{0};
            std::atomic<bool> snapshot_stop_pending{false};
            std::atomic<gekko::u64> snapshot_request_tick{0};
            std::atomic<gekko::u64> snapshot_press_tick{0};
            std::mutex snapshot_mutex;
            struct SnapshotReference {
                bool armed = false;
                gekko::u64 baseline = 0;
                gekko::u64 boundary_tick = 0;
                std::string id;
                std::filesystem::path folder;
            } snapshot_reference;
            dev_link.set_on_request(
                [](void* flag) { static_cast<std::atomic<bool>*>(flag)->store(true, std::memory_order_release); },
                &dev_boundary);
            if (dev_link.pending()) dev_boundary.store(true, std::memory_order_release);
            std::exception_ptr game_exception;
            // Game ticks, published by the game thread for the UI thread's logs.
            std::atomic<gekko::u64> retired_published{0};

            std::mutex pad_mutex;
            // Port 0 is the merged keyboard + XInput-0 pad; ports 1-3 are pure
            // XInput controllers 1-3, giving the game's own 2P Battle menu a
            // real second controller. Disconnected ports publish an empty
            // (unconnected) sample, which the game reads as an absent port.
            std::array<shadow::peripheral::ControllerSample, 4> pads_published{};
            pads_published[0].connected = true;

            // Written by the game thread, read by the UI thread for the frame
            // log and the summary. Plain counters would be legal to cache in a
            // register across the UI loop.
            std::atomic<gekko::u64> execute_ns{0};   // translated code + renderer, on the game thread
            std::atomic<gekko::u64> submit_ns{0};
            std::atomic<gekko::u64> retire_ns{0};
            std::atomic<gekko::u64> sleep_ns{0};
            std::atomic<gekko::u64> service_calls{0};
            std::atomic<gekko::u64> guest_stalls{0};
            std::atomic<gekko::u64> longest_guest_stall_ns{0};
            // Pacer headroom as a RATE, not a lifetime total. sleep_ns alone
            // cannot answer "did the pacer ever sleep during the phase whose
            // frame rate I am trying to fix?", because a session is bimodal:
            // loads and menus run ahead of the recovered clock and sleep, while
            // in-stage the guest is behind and the sleep branch is never taken.
            // A lifetime sum of the two reads as "27% headroom" when the phase
            // that sets the frame rate had none. These two are emitted per
            // telemetry row so the split is visible per second.
            std::atomic<gekko::u64> paced_services{0};    // pacer slept: guest was AHEAD
            std::atomic<gekko::u64> unpaced_services{0};  // no sleep: guest was AT or BEHIND

            // ---- interval telemetry ---------------------------------------
            // One row per wall second, sampled on the game thread. Its purpose
            // is to answer a single question: what grows as the player travels?
            // Every counter that could accumulate without bound is here, so a
            // progressive slowdown shows up as a column that never stops rising
            // rather than as a frame-rate impression.
            std::ofstream telemetry_log;
            if (options.telemetry_log) {
                const std::filesystem::path parent = options.telemetry_log->parent_path();
                if (!parent.empty()) std::filesystem::create_directories(parent);
                telemetry_log.open(*options.telemetry_log, std::ios::binary);
                if (telemetry_log) {
                    // `renderer` says which renderer produced the row. Every
                    // column from `triangles` to `batch_pixel` is a LEGACY
                    // Flipper counter and reads zero under the native renderer,
                    // which is the default - so a row that does not say which
                    // renderer it came from cannot be read at all. The native
                    // figures are the nat_* block at the end.
#if defined(SHADOW_RENDER_LIFTED)
                    telemetry_log << "schema,wall_s,interval_s,renderer,fps,frames,attempts,game_ms,game_speed,produced_per_game_s,"
                                     "packets,dropped,pending,render_present_ms,packet_wait_ms,capture_ms,gpu_frame_ms,"
                                     "ram_working_mib,ram_private_mib,draws_last,textures_live,meshes_live,"
                                     "skipped_draws,resource_failures,paced,unpaced,interp_instr,host_stalls\n";
#else
                    telemetry_log
                        << "wall_s,renderer,fps,cpu_frame_ms,gpu_frame_ms,game_speed,"
                           "xfb_per_game_s,paced,unpaced,"
                           "frames,game_ms,"
                           "interp_entries,interp_instr,host_stalls,"
                           "ram_working_mib,ram_private_mib,"
                           "nat_draws,nat_triangles,nat_textures,nat_texture_mib,"
                           "nat_geometry,nat_upload_mib,nat_vram_mib,"
                           "nat_missing_geometry,nat_rejections\n";
#endif
                } else {
                    std::cerr << "WARNING: could not open telemetry log "
                              << options.telemetry_log->string() << "\n";
                }
            }

            std::ofstream performance_log;
            if (options.performance_log) {
                const std::filesystem::path parent = options.performance_log->parent_path();
                if (!parent.empty()) std::filesystem::create_directories(parent);
                performance_log.open(*options.performance_log, std::ios::binary);
                if (performance_log) {
#if defined(SHADOW_RENDER_LIFTED)
                    performance_log << "schema,wall_seconds,present_index,packet_tick,game_ticks,success,"
                                       "render_return_interval_ms,packet_wait_ms,capture_ms,render_present_ms\n";
#else
                    performance_log
                        << "wall_seconds,xfb_generation,xfb_delta,vi_frame,game_ticks,"
                           "frame_ms,execute_ms,submit_ms,retire_ms,sleep_ms,present_ms\n";
#endif
                } else {
                    std::cerr << "WARNING: could not open performance log "
                              << options.performance_log->string() << "\n";
                }
            }

            const auto elapsed_ns = [](std::chrono::steady_clock::time_point begin,
                                       std::chrono::steady_clock::time_point end) {
                return static_cast<gekko::u64>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
            };

            // Every stall is reported with the state that identifies what was
            // actually running, not just how long it took. Cpu::pc is published
            // by translated code at every loop back edge, which is exactly where
            // a machine that is spending tens of milliseconds somewhere will be;
            // it is stale only for straight-line code, which cannot stall.
            const auto report_stall = [&](const char* phase, double duration_ms,
                                          gekko::u64 advanced_ticks, gekko::u64 interpreted,
                                          double submit_ms, double retire_ms, double gpu_wait_ms,
                                          double io_ms, const char* reason) {
                const gekko::Cpu& cpu = system.cpu();
                std::fprintf(stderr,
                             "[host-stall] %s %.3f ms\n"
                             "  guest_pc          %s\n"
                             "  guest_lr          %s\n"
                             "  native_execution  %.3f ms while game time advanced %.3f ms\n"
                             "  interpreter       %llu instructions\n"
                             "  render_submit     %.3f ms\n"
                             "  render_retire     %.3f ms\n"
                             "  gpu_wait          %.3f ms\n"
                             "  io_wait           %.3f ms\n"
                             "  last_file         %s\n"
                             "  native_generation %llu\n"
                             "  reason            %s\n",
                             phase, duration_ms,
                             system.describe_address(cpu.pc).c_str(),
                             system.describe_address(cpu.lr).c_str(),
                             duration_ms - submit_ms - retire_ms,
                             static_cast<double>(advanced_ticks) * 1000.0 /
                                 static_cast<double>(gekko::kGameTicksPerSecond),
                             static_cast<unsigned long long>(interpreted), submit_ms, retire_ms,
                             gpu_wait_ms, io_ms,
                             system.assets().last_path().empty()
                                 ? "-"
                                 : system.assets().last_path().c_str(),
                             static_cast<unsigned long long>(
#if !defined(SHADOW_RENDER_LIFTED)  // G5
                                 native_gpu != nullptr ? native_gpu->output_generation() : 0u),
#else
                                 0u),  // G5: the lifted build has no legacy GPU output
#endif
                             reason);
                std::fflush(stderr);
            };

            // ---- the host's periodic work, on the game thread --------------
            auto last_service_end = std::chrono::steady_clock::now();
            auto last_boundary_wall = last_service_end;
            bool have_boundary = false;
            auto last_telemetry = last_service_end;
#if defined(SHADOW_RENDER_LIFTED)
            gekko::LiftedRender::Telemetry previous_lifted_telemetry;
#else
            gekko::u64 telemetry_frames = 0;
            gekko::u64 telemetry_execute_ns = 0;
            gekko::u64 telemetry_gpu_ns = 0;
#endif
            gekko::u64 telemetry_ticks = system.game_ticks();
            gekko::u64 telemetry_paced = 0;
            gekko::u64 telemetry_unpaced = 0;
            gekko::u64 last_service_instructions = system.game_ticks();
            gekko::u64 last_interpreted = 0;
            gekko::u64 last_asset_read_ns = 0;
            gekko::u64 last_card_persists = 0;
            gekko::u64 last_card_persist_ns = 0;
            // Real-time pacing at game time `game_ticks`, after the host work that began at `service_start`. It runs
            // at every pacing tick (System::kPacingTicks, a divisor of every frame period: docs/FRAME_PACING.md
            // "Pacing on the frame grid") - from the full host service when one falls on the tick, else from the
            // pacing-only service - so each frame boundary is slept at exactly, before the frame it begins is built.
            bool have_frame_boundary = false;
            gekko::u64 last_frame_index = 0;
            // Wall time the pacing-only ticks slept since the last host service. They do not move last_service_end,
            // so the service's guest-execution window (its stall check and execute_ns) is last_service_end to its
            // end minus this: the same guest execution as with no pacing-only tick in between.
            gekko::u64 paced_between_ns = 0;
            const auto pace = [&](gekko::u64 game_ticks, std::chrono::steady_clock::time_point service_start,
                                  bool service) {
                const auto after_work = std::chrono::steady_clock::now();
                if (service) {
                    // The two components this tick contributed: time the guest
                    // spent executing before entering the service, and the
                    // service body itself.
                    frame_tail.tick(elapsed_ns(last_service_end, service_start) - paced_between_ns,
                                    elapsed_ns(service_start, after_work), 0u, 0u,
                                    game_ticks - frame_tail_last_total);
                    frame_tail_last_total = game_ticks;
                    paced_between_ns = 0;
                }
                const auto pacing = simulation_clock.observe(game_ticks, after_work);
                // The telemetry stays per 60 Hz quantum: wall time between consecutive 1/60 s boundaries as
                // observed on this thread, sleep included - exactly 16.67 ms when paced, longer when the host fell
                // behind. This is the guest-speed distribution the baseline had to reconstruct offline from
                // --perf-log.
                const gekko::u64 frame_index = game_ticks / (gekko::kGameTicksPerSecond / 60u);
                if (pacing.boundary && (!have_frame_boundary || frame_index != last_frame_index)) {
                    if (have_boundary) {
                        const auto quantum_ns = elapsed_ns(last_boundary_wall, after_work);
                        quantum_hist.add(quantum_ns);
                        frame_tail.boundary(quantum_ns);
                        // Frame-time lane for the save/stall correlation reducer
                        // (tools/save_trace_report.py): wall ns of this 60 Hz quantum.
                        gekko::audio_audit::event(gekko::audio_audit::Lane::Guest, "quantum",
                                                  static_cast<gekko::u64>(quantum_ns),
                                                  static_cast<gekko::u64>(pacing.debt.count()));
                    }
                    last_boundary_wall = after_work;
                    have_boundary = true;
                    have_frame_boundary = true;
                    last_frame_index = frame_index;
                }
                if (pacing.boundary && pacing.should_sleep) {
                    paced_services.fetch_add(1, std::memory_order_relaxed);
                    {
                        gekko::audio_audit::Scope audit_sleep("pacer", "pacer");
                        recovered_clock_sleep.sleep_until(pacing.deadline);
                    }
                    const auto woke = std::chrono::steady_clock::now();
                    simulation_clock.note_wake(woke);
                    // Overshoot is what the scheduler cost us beyond the
                    // deadline: sleeping 16 ms when 16 was asked is free,
                    // waking 4 ms late is a frame.
                    const gekko::u64 slept_ns = elapsed_ns(after_work, woke);
                    const gekko::u64 late_ns = woke > pacing.deadline ? elapsed_ns(pacing.deadline, woke) : 0u;
                    frame_tail.tick(0u, 0u, slept_ns, late_ns, 0u);
                    sleep_ns.fetch_add(slept_ns, std::memory_order_relaxed);
                    if (service) {
                        last_service_end = woke;
                    } else {
                        paced_between_ns += slept_ns;
                    }
                } else {
                    if (pacing.boundary) {
                        unpaced_services.fetch_add(1, std::memory_order_relaxed);
                    }
                    if (service) last_service_end = after_work;
                }
            };
            system.set_host_pacing([&](gekko::u64 game_ticks) {
                if (gekko::MeasureProbes* probes = gekko::MeasureProbes::active()) probes->on_host_service();
                pace(game_ticks, std::chrono::steady_clock::now(), false);
            });
            system.set_host_service(
                [&](gekko::u64 game_ticks) {
                    const auto service_start = std::chrono::steady_clock::now();
                    if (gekko::MeasureProbes* probes = gekko::MeasureProbes::active()) probes->on_host_service();
                    game_thread_placement.sample();
                    // Whole-body cost of one host service, so the audit can
                    // tell service work apart from native guest execution.
                    gekko::audio_audit::Scope service("host-service", "host-service");
                    // Game time: the service runs at this event's time.
                    const gekko::u64 total = game_ticks;
                    const gekko::u64 paced_cycles = game_ticks;
                    retired_published.store(total, std::memory_order_relaxed);

                    if (stop_requested.load(std::memory_order_acquire)) {
                        system.cpu().request_stop(gekko::StopReason::HostRequest,
                                                  "host requested shutdown");
                        return;
                    }
                    if (budget_due(game_ticks)) {
                        budget_reached = true;
                        system.cpu().request_stop(gekko::StopReason::HostRequest, "budget reached");
                        return;
                    }
                    // --stop-at-tick in a window too (a looped range ends here): the first service at or after it.
                    if (options.stop_at_tick != 0u && !stop_at_requested && game_ticks >= options.stop_at_tick) {
                        stop_at_requested = true;
                        stop_at_request_tick = game_ticks;
                        budget_reached = true;
                        system.cpu().request_stop(gekko::StopReason::HostRequest, "stop-at-tick");
                        return;
                    }

                    // Input. The samples are latched here, at this point of
                    // game time, rather than published straight into the
                    // peripheral layer from the UI thread: a run replays
                    // exactly (docs/lift-20261006/L1_INPUT.md).
                    {
                        std::array<shadow::peripheral::ControllerSample, 4> pads;
                        {
                            std::lock_guard<std::mutex> guard(pad_mutex);
                            pads = pads_published;
                        }
                        // A tape being played replaces the live samples (gekko/input_tape.h); a recording keeps
                        // what was latched.
                        if (input_tape.playing()) input_tape.samples_at(game_ticks, pads);
                        for (std::size_t port = 0; port < pads.size(); ++port) {
                            system.peripheral().latch(port, pads[port]);
                        }
                        if (tape_recorder != nullptr) tape_recorder->observe(game_ticks, pads);
                    }
#if defined(SHADOW_RENDER_LIFTED)
                    // The game-visible output: a resize since the last host service takes effect here.
                    latch_game_output(game_ticks);
#endif

                    // The snapshot key (docs/comparator/README.md "One-button snapshots"): the boundary after this
                    // host service is the snapshot's moment; this service's tick names it (a replay asks here).
                    if (snapshot_requested.load(std::memory_order_acquire) != 0u &&
                        !snapshot_stop_pending.exchange(true, std::memory_order_acq_rel)) {
                        snapshot_request_tick.store(game_ticks, std::memory_order_release);
                    }

                    // The development link: a request waiting, or the frames a run asked for handed over - stop at
                    // the next traversal boundary, where the game thread answers. The first host service that asks
                    // names the boundary (a checkpoint's "stop" tick: a replay asks at the same one).
                    if (dev_link.connected()) {
                        const gekko::u64 target = dev_run_target.load(std::memory_order_acquire);
                        if (target != 0u && render_frames() >= target) {
                            dev_boundary.store(true, std::memory_order_release);
                        }
                        const gekko::u64 tick_target = dev_run_target_ticks.load(std::memory_order_acquire);
                        if (tick_target != 0u && game_ticks >= tick_target) {
                            dev_boundary.store(true, std::memory_order_release);
                        }
                        if (dev_capture_only.load(std::memory_order_acquire) &&
                            dev_capture_result().written >
                                dev_capture_only_baseline.load(std::memory_order_acquire)) {
                            dev_boundary.store(true, std::memory_order_release);
                        }
                        if (dev_boundary.load(std::memory_order_acquire)) {
                            if (!dev_stop_pending.exchange(true, std::memory_order_acq_rel)) {
                                dev_request_tick.store(game_ticks, std::memory_order_release);
                            }
                            system.request_stop_between_traversals(gekko::StopReason::HostRequest,
                                                                   "development link");
                            return;
                        }
                    }
                    if (snapshot_stop_pending.load(std::memory_order_acquire)) {
                        system.request_stop_between_traversals(gekko::StopReason::HostRequest, "snapshot");
                        return;
                    }

                    // The legacy Flipper produces frames only when it owns
                    // them. Under the native renderer the frame comes from
                    // RwCapture/SegmentQueue, and servicing the legacy object
                    // here only published no-op guard and source-read sets.
                    // The native renderer's frames come from RwCapture and its
                    // own queue; the service has no renderer work to submit.
                    const auto submit_start = std::chrono::steady_clock::now();
                    const auto submit_end = submit_start;
                    const auto retire_end = submit_start;

                    // Savestate hotkeys (N save / M load). This callback runs
                    // INSIDE translated guest code (at a wait point) -
                    // System::set_host_service is explicit that it "must not
                    // touch architected state" - so neither the save nor the
                    // load may happen here: restoring registers with guest
                    // frames still on the native stack unwinds into a machine
                    // that no longer exists. Both are deferred to the game
                    // thread, which performs them after run() has returned.
                    if (savestate_save_requested.load(std::memory_order_acquire) ||
                        savestate_load_requested.load(std::memory_order_acquire)) {
                        system.request_stop_between_traversals(gekko::StopReason::HostRequest,
                                                               "savestate boundary");
                        return;
                    }

                    const gekko::u64 this_submit = elapsed_ns(submit_start, submit_end);
                    const gekko::u64 this_retire = elapsed_ns(submit_end, retire_end);
                    submit_ns.fetch_add(this_submit, std::memory_order_relaxed);
                    retire_ns.fetch_add(this_retire, std::memory_order_relaxed);
                    // Guest execution since the last service: the pacing-only ticks between (pace) leave
                    // last_service_end alone and add the time they slept to paced_between_ns.
                    execute_ns.fetch_add(elapsed_ns(last_service_end, retire_end) - paced_between_ns,
                                         std::memory_order_relaxed);
                    service_calls.fetch_add(1, std::memory_order_relaxed);
                    {
                        const gekko::u64 tick_ns = this_submit + this_retire;
                        service_cost_hist.add(tick_ns);
                    }

                    // Stall accounting. The window is from the end of the last
                    // service - after any pacing sleep - to the end of this one,
                    // so it contains only guest execution and renderer work.
                    const gekko::u64 window_ns = elapsed_ns(last_service_end, retire_end) - paced_between_ns;
                    if (window_ns >= static_cast<gekko::u64>(
                                         std::chrono::duration_cast<std::chrono::nanoseconds>(
                                             kHostStallThreshold)
                                             .count())) {
                        // Wall time inside asset reads this window. The old
                        // bucket counted BYTES the guest thread read from disk
                        // and passed them where the reporter prints
                        // milliseconds. A read is now a memcpy out of a mapped
                        // page, so the cost that matters is the copy plus the
                        // first-touch page fault - which a byte count could
                        // never see and a clock can.
                        const gekko::u64 asset_read_ns = system.assets().stats().read_ns;
                        const gekko::u64 interpreted =
                            system.cpu().interpreted_instructions() - last_interpreted;
                        const double submit_ms = static_cast<double>(this_submit) / 1.0e6;
                        const double retire_ms = static_cast<double>(this_retire) / 1.0e6;
                        // Native GPU fence waits happen on the renderer thread,
                        // never inside this guest window.
                        const double gpu_wait_ms = 0.0;
                        const double duration_ms = static_cast<double>(window_ns) / 1.0e6;
                        const double asset_ms =
                            static_cast<double>(asset_read_ns - last_asset_read_ns) / 1.0e6;
                        // A host memory-card persist runs on the game thread -
                        // inside a native card owner, or the debounce's tick in
                        // a clock advance - so without this bucket a save
                        // window reads as "native-execution" (2026-09-05: 13
                        // windows of 123-167 ms).
                        const gekko::u64 card_persists_now = system.card().stats().persists;
                        const double card_ms =
                            static_cast<double>(system.card().stats().persist_ns -
                                                last_card_persist_ns) / 1.0e6;
                        const char* reason =
                            card_ms > duration_ms * 0.5                   ? "memory-card-persist"
                            : asset_ms > duration_ms * 0.5                ? "asset-read"
                            : interpreted != 0u                         ? "interpreter"
                                                                          : "native-execution";
                        report_stall("game-thread", duration_ms,
                                     total - last_service_instructions, interpreted, submit_ms,
                                     retire_ms, gpu_wait_ms,
                                     asset_ms, reason);
                        {
                            // The state the stall hunt asks for beyond pc/lr: the
                            // branch/loop registers, TB, DEC, the interrupt lines,
                            // and the host card writes this window paid for.
                            const gekko::Cpu& cpu = system.cpu();
                            std::fprintf(stderr,
                                         "  card_persist      %llu host writes, %.3f ms in this window "
                                         "(%llu card writes so far, tick wired %s)\n"
                                         "  registers         ctr=%08X cr=%08X xer=%08X msr=%08X "
                                         "tb=%llu dec=%08X ext_int=%u native_int=%u\n",
                                         static_cast<unsigned long long>(card_persists_now -
                                                                         last_card_persists),
                                         card_ms,
                                         static_cast<unsigned long long>(
                                             system.card().stats().writes),
                                         system.card().persist_tick_wired() ? "yes" : "NO",
                                         cpu.ctr, cpu.cr, cpu.xer, cpu.msr(),
                                         static_cast<unsigned long long>(cpu.time_base()),
                                         cpu.decrementer(), cpu.external_interrupt() ? 1u : 0u,
                                         cpu.native_interrupt() ? 1u : 0u);
                            std::fflush(stderr);
                        }
                        guest_stalls.fetch_add(1, std::memory_order_relaxed);
                        gekko::u64 longest =
                            longest_guest_stall_ns.load(std::memory_order_relaxed);
                        while (window_ns > longest &&
                               !longest_guest_stall_ns.compare_exchange_weak(
                                   longest, window_ns, std::memory_order_relaxed)) {
                        }
                    }
                    // Every window, like the counters below: io_wait is this window's reads only (ES 2026-10-19).
                    last_asset_read_ns = system.assets().stats().read_ns;
                    last_interpreted = system.cpu().interpreted_instructions();
                    last_service_instructions = total;
                    last_card_persists = system.card().stats().persists;
                    last_card_persist_ns = system.card().stats().persist_ns;

                    // Renderer-specific interval telemetry. Frame/timing deltas
                    // describe this window; resource counters retain their own
                    // documented live or cumulative scope.
                    if (telemetry_log && retire_end - last_telemetry >= std::chrono::seconds(1)) {
#if defined(SHADOW_RENDER_LIFTED)
                        const auto observed = lifted_render.telemetry();
                        const auto& previous = previous_lifted_telemetry;
                        const double seconds = std::chrono::duration<double>(retire_end - last_telemetry).count();
                        const double game_seconds = static_cast<double>(total - telemetry_ticks) / gekko::kGameTicksPerSecond;
                        const auto frames = observed.presents - previous.presents;
                        const auto attempts = observed.attempts - previous.attempts;
                        PROCESS_MEMORY_COUNTERS_EX memory{};
                        memory.cb = sizeof(memory);
                        const bool memory_available = GetProcessMemoryInfo(GetCurrentProcess(),
                            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)) != FALSE;
                        telemetry_log << std::fixed << std::setprecision(6) << "1,"
                            << std::chrono::duration<double>(retire_end - execution_start).count() << ',' << seconds << ",lifted,"
                            << frames / seconds << ',' << frames << ',' << attempts << ',' << game_seconds * 1000.0
                            << ',' << game_seconds / seconds << ',';
                        if (game_seconds > 0.0) telemetry_log << (observed.packets - previous.packets) / game_seconds;
                        telemetry_log << ',' << observed.packets << ',' << observed.dropped << ',' << observed.pending << ',';
                        if (attempts != 0u)
                            telemetry_log << (observed.render_present_ns - previous.render_present_ns) / (1.0e6 * attempts);
                        telemetry_log << ',' << (observed.packet_wait_ns - previous.packet_wait_ns) / 1.0e6
                            << ',' << (observed.capture_ns - previous.capture_ns) / 1.0e6
                            << ",,";  // GPU execution is not measured by this observer.
                        if (memory_available) telemetry_log << memory.WorkingSetSize / (1024.0 * 1024.0);
                        telemetry_log << ',';
                        if (memory_available) telemetry_log << memory.PrivateUsage / (1024.0 * 1024.0);
                        telemetry_log << ',' << observed.draws_last << ',' << observed.textures << ',' << observed.meshes
                            << ',' << observed.skipped_draws << ',' << observed.resource_failures
                            << ',' << (paced_services.load(std::memory_order_relaxed) - telemetry_paced)
                            << ',' << (unpaced_services.load(std::memory_order_relaxed) - telemetry_unpaced)
                            << ',' << system.cpu().interpreted_instructions()
                            << ',' << guest_stalls.load(std::memory_order_relaxed) << '\n';
                        telemetry_log.flush();
                        previous_lifted_telemetry = observed;
                        last_telemetry = retire_end;
                        telemetry_ticks = total;
                        telemetry_paced = paced_services.load(std::memory_order_relaxed);
                        telemetry_unpaced = unpaced_services.load(std::memory_order_relaxed);
#else
                        // Legacy native renderer counters belong only to this backend.
                        const gekko::NativeGpu::Stats native_stats =
                            native_gpu != nullptr ? native_gpu->stats()
                                                  : gekko::NativeGpu::Stats{};
                        const bool native_active = native_gpu != nullptr;
                        const gekko::u64 frames = native_stats.frames;
                        const gekko::u64 gpu_execution_ns = native_stats.gpu_frame_ns;
                        const double seconds =
                            std::chrono::duration<double>(retire_end - last_telemetry).count();
                        const gekko::u64 frame_delta = frames - telemetry_frames;
                        const gekko::u64 tick_delta = total - telemetry_ticks;
                        // Game seconds this row covered: speed is game time over
                        // wall time (1.0 = real time), and the XFB rate per
                        // game second separates "the game issued fewer copies"
                        // from "the host ran slow".
                        const double guest_seconds = static_cast<double>(tick_delta) /
                                                     static_cast<double>(gekko::kGameTicksPerSecond);
                        const auto mib = [](gekko::u64 bytes) {
                            return static_cast<double>(bytes) / (1024.0 * 1024.0);
                        };
                        const auto ms = [](gekko::u64 ns) {
                            return static_cast<double>(ns) / 1.0e6;
                        };
#ifdef _WIN32
                        PROCESS_MEMORY_COUNTERS_EX memory{};
                        memory.cb = sizeof(memory);
                        GetProcessMemoryInfo(GetCurrentProcess(),
                                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
                                             sizeof(memory));
#endif
                        telemetry_log
                            << std::fixed << std::setprecision(3)
                            << std::chrono::duration<double>(retire_end - execution_start).count()
                            << ',' << (native_active ? "native" : "legacy")
                            << ',' << (seconds > 0.0 ? frame_delta / seconds : 0.0)
                            << ',' << (frame_delta ? ms(execute_ns.load(std::memory_order_relaxed) -
                                                       telemetry_execute_ns) / frame_delta : 0.0)
                            << ',' << (frame_delta ? ms(gpu_execution_ns -
                                                       telemetry_gpu_ns) / frame_delta : 0.0)
                            << ',' << (seconds > 0.0 ? guest_seconds / seconds : 0.0)
                            << ',' << (guest_seconds > 0.0 ? frame_delta / guest_seconds : 0.0)
                            << ',' << (paced_services.load(std::memory_order_relaxed) -
                                       telemetry_paced)
                            << ',' << (unpaced_services.load(std::memory_order_relaxed) -
                                       telemetry_unpaced)
                            << ',' << frame_delta << ',' << guest_seconds * 1000.0
                            << ',' << system.cpu().interpreter_strands().size()
                            << ',' << system.cpu().interpreted_instructions()
                            << ',' << guest_stalls.load(std::memory_order_relaxed)
#ifdef _WIN32
                            << ',' << mib(memory.WorkingSetSize) << ',' << mib(memory.PrivateUsage)
#else
                            << ",0,0"
#endif
                            // Native renderer, under its own names. Cumulative
                            // where the counter is cumulative; no legacy column
                            // is reused for a different quantity.
                            << ',' << native_stats.draws << ',' << native_stats.triangles
                            << ',' << native_stats.textures_live
                            << ',' << mib(native_stats.texture_bytes)
                            << ',' << native_stats.geometry_live
                            << ',' << mib(native_stats.upload_bytes)
                            << ',' << mib(native_stats.vram_usage_bytes)
                            << ',' << native_stats.missing_geometry
                            << ',' << native_stats.rejections_posted
                            << '\n';
                        telemetry_log.flush();
                        last_telemetry = retire_end;
                        telemetry_frames = frames;
                        telemetry_ticks = total;
                        telemetry_execute_ns = execute_ns.load(std::memory_order_relaxed);
                        telemetry_gpu_ns = gpu_execution_ns;
                        telemetry_paced = paced_services.load(std::memory_order_relaxed);
                        telemetry_unpaced = unpaced_services.load(std::memory_order_relaxed);
#endif
                    }

                    // Host-only fixed-quantum pacing: the game clock maps to
                    // wall time here. Most service calls do no timing work; only
                    // a crossed simulation quantum can sleep, so an efficient
                    // host runs a batch and then becomes idle instead of polling
                    // its lead continuously.
                    // Diagnostic only, inert unless set:
                    // GEKKO_DEBUG_HOST_STALL=<ms>@<seconds> blocks this thread ONCE,
                    // <seconds> after the first paced service, to reproduce a host
                    // hitch (a stage load) deterministically for pacer/audio A/Bs.
#if !defined(GEKKO_SHIPPING)  // ES: the hitch injector is a measurement tool, not part of the user's exe
                    {
                        struct DebugStall { long ms = 0; double at = 0.0; };
                        static const DebugStall debug_stall = [] {
                            DebugStall d;
                            if (const char* v = std::getenv("GEKKO_DEBUG_HOST_STALL")) {
                                d.ms = std::strtol(v, nullptr, 10);
                                if (const char* at = std::strchr(v, '@')) d.at = std::strtod(at + 1, nullptr);
                            }
                            return d;
                        }();
                        static const auto debug_stall_origin = std::chrono::steady_clock::now();
                        static bool debug_stall_done = false;
                        if (debug_stall.ms > 0 && !debug_stall_done &&
                            std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                          debug_stall_origin).count() >= debug_stall.at) {
                            debug_stall_done = true;
                            std::cerr << "[debug-host-stall] blocking the game thread for " << debug_stall.ms << " ms\n";
                            std::this_thread::sleep_for(std::chrono::milliseconds(debug_stall.ms));
                        }
                    }
#endif
                    pace(paced_cycles, service_start, true);
                });

            // ---- the game thread -------------------------------------------
            // Re-anchor the pacing clock HERE, not at execution_start.
            //
            // execution_start is taken before the window exists. Everything
            // between the two - opening the window, creating the DXGI swap
            // chain, bringing up the D3D12 device and the presentation path,
            // the shader cache - is host startup during which the guest has
            // run NOTHING. Anchored at execution_start, all of it lands in
            // the pacer's absolute ladder as debt the guest never incurred, and
            // the guest then repays it by running fast. Measured on the boot
            // route: 0.5-0.9 s of debt regardless of the disc cache, which is
            // the whole of the burst that the catch-up limiter later has to
            // suppress.
            //
            // This discards no guest time. The guest has executed nothing yet,
            // so there is nothing to discard: it is the same re-anchor that
            // already happens after a savestate load, for the same reason.
            // execution_start itself is deliberately NOT moved - every wall
            // clock column in the telemetry and the perf log is relative to it.
            {
                const auto guest_start = std::chrono::steady_clock::now();
                const double host_startup_seconds =
                    std::chrono::duration<double>(guest_start - execution_start).count();
                simulation_clock.reset(system.game_ticks(), guest_start);
                last_service_end = guest_start;
                paced_between_ns = 0;
                std::cout << "Host startup before the first guest instruction: "
                          << host_startup_seconds << " s (pacing anchored here)\n";
            }
            // ---- the development link, served on the game thread (docs/comparator/README.md) ----------------
            // Every request is answered after run() returned: the guest is at a traversal boundary with no guest
            // frame on the native stack (the savestate hotkeys' boundary). While paused, the thread waits here.
            gekko::u64 dev_run_id = 0;  // the run_frames / run_ticks / capture request waiting for its frames
            bool dev_run_waiting = false;
            gekko::u64 dev_capture_only_id = 0;
            std::string dev_capture_only_path;
            bool dev_run_capture = false;
            gekko::u64 dev_capture_baseline = 0;
            std::string dev_run_capture_path;
            const auto dev_status = [&]() {
                gekko::dev::Json body;
                body.field("paused", dev_paused.load(std::memory_order_acquire))
                    .field("game_ticks", system.game_ticks())
                    .field("game_seconds", game_seconds(system))
                    .field("vi_frames", static_cast<gekko::u64>(system.video().frames()))
                    .field("render_frames", render_frames())
                    .field("boundary_tick", dev_request_tick.load(std::memory_order_acquire))
                    .field("tape_recording", tape_recorder != nullptr && tape_recorder->recording())
                    .field("tape_playing", input_tape.playing())
                    .field("tape_late_entries", input_tape.late_entries())
                    .field("traversals", system.gameplay_scheduler() != nullptr
                                             ? system.gameplay_scheduler()->stats().traversals
                                             : gekko::u64{0});
#if defined(SHADOW_RENDER_LIFTED)
                body.field("frame_open", lifted_render.frame_open());
#endif
                return body;
            };
            const auto dev_reanchor = [&]() {
                // A pause is wall time the game did not run: the pacing ladder restarts here (as after a load).
                const auto now = std::chrono::steady_clock::now();
                simulation_clock.reset(system.game_ticks(), now);
                last_service_instructions = system.game_ticks();
                last_service_end = now;
                paced_between_ns = 0;
                audio_runtime.reset_timeline();
            };
            const auto dev_client_size = [&](gekko::u32& width, gekko::u32& height) {
                int w = 0, h = 0;
                window.client_size(w, h);
                width = static_cast<gekko::u32>((std::max)(w, 1));
                height = static_cast<gekko::u32>((std::max)(h, 1));
            };
            // ---- one-button comparison snapshots (docs/comparator/README.md "One-button snapshots") ---------
            // At the boundary the snapshot key asked for: this build's own savestate, the input tape from the run's
            // start (when it records one), a state report and - written by the renderer once it finishes it - the
            // next frame, into a new folder; the workstation is told. The game does not describe the moment: the
            // workstation reads what it is (stage, player, camera, objects) out of the savestate.
            const std::filesystem::path snapshot_root =
                options.snapshot_dir ? *options.snapshot_dir : (executable_directory() / "snapshots");
            const auto take_snapshot = [&]() {
                snapshot_requested.store(0u, std::memory_order_release);
                const auto started = std::chrono::steady_clock::now();
                const gekko::u64 ticks = system.game_ticks();
                const gekko::u64 asked_at = snapshot_request_tick.load(std::memory_order_acquire);
                const std::time_t now_time = std::time(nullptr);
                std::tm now_tm{};
                localtime_s(&now_tm, &now_time);
                char stamp[32];
                char created[32];
                std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &now_tm);
                std::strftime(created, sizeof(created), "%Y-%m-%dT%H:%M:%S", &now_tm);
                const std::string id = std::string("snap-") + stamp + "-" + std::to_string(ticks);
                const std::filesystem::path folder = snapshot_root / id;
                std::error_code made;
                std::filesystem::create_directories(folder, made);
                std::cout << "[Snapshot] Capturing comparison state at game tick " << ticks << " ("
                          << game_seconds(system) << " s of game time; boundary asked at tick " << asked_at
                          << ")...\n";
                std::string error;
                const bool saved = gekko::save_state(system, folder / "state.gekkostate", &error);
                const gekko::RendererStateKind kind = gekko::renderer_state_kind(system);
                gekko::dev::Json manifest;
                manifest.field("format", "shadow-snapshot 1")
                    .field("id", id)
                    .field("created", created)
                    .field("session", options.dev_link_name)
#if defined(SHADOW_RENDER_LIFTED)
                    .field("flavour", "lifted")
#else
                    .field("flavour", "native")
#endif
                    .field("exe", utf8_path(executable_path()))
                    .field("savestate_version", gekko::save_state_version())
                    .field("renderer_kind", static_cast<gekko::u32>(kind))
                    .field("renderer", gekko::renderer_state_kind_name(kind))
                    .field("game_ticks", ticks)
                    .field("game_seconds", game_seconds(system))
                    .field("boundary_request_tick", asked_at)
                    .field("press_tick", snapshot_press_tick.load(std::memory_order_acquire))
                    .field("vi_frames", static_cast<gekko::u64>(system.video().frames()))
                    .field("render_frames", render_frames())
                    .field("state", saved ? "state.gekkostate" : "");
                if (!saved) {
                    std::cerr << "[Snapshot] the savestate failed: " << error << "\n";
                    manifest.field("error", error);
                }
                if (saved && tape_recorder != nullptr) {
                    if (tape_recorder->write(folder / "input.tape", asked_at, ticks, &error)) {
                        manifest.field("tape", "input.tape").field("tape_start_tick", tape_recorder->start_tick());
                    } else {
                        manifest.null_field("tape").field("tape_error", error);
                    }
                } else {
                    manifest.null_field("tape").field("tape_error", "this run records no input tape");
                }
                {
                    std::ofstream out(folder / "report.json", std::ios::binary | std::ios::trunc);
                    out << state_report_json(system, render_frames(), asked_at, report_ranges, true) << "\n";
                }
                manifest.field("report", "report.json");
                // The reference frame: the first one finished after this boundary (both renderers stamp it with the
                // same game tick; a replay of the snapshot reaches it from the savestate). Not while a workstation
                // capture holds the renderer's capture slot.
                gekko::u32 width = 0, height = 0;
                dev_client_size(width, height);
                manifest.field("width", width).field("height", height);
#if !defined(SHADOW_RENDER_LIFTED)
                if (gekko::NativeGpu* gpu = native_renderer.gpu()) {
                    manifest.field("internal_scale", gpu->stats().internal_scale);
                }
#endif
                bool armed = false;
                if (saved && !dev_run_capture && !dev_capture_only.load(std::memory_order_acquire)) {
                    std::lock_guard<std::mutex> guard(snapshot_mutex);
                    snapshot_reference.baseline = dev_capture_result().written;
                    const std::filesystem::path reference =
                        dev_arm_capture_at_tick(folder / "reference", ticks + 1u, width, height);
                    snapshot_reference.armed = true;
                    snapshot_reference.boundary_tick = ticks;
                    snapshot_reference.id = id;
                    snapshot_reference.folder = folder;
                    manifest.field("reference", utf8_path(reference.filename())).field("reference_after_tick", ticks);
                    armed = true;
                } else {
                    manifest.null_field("reference");
                }
                {
                    std::ofstream out(folder / "snapshot.json", std::ios::binary | std::ios::trunc);
                    out << manifest.str() << "\n";
                }
                const double ms =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
                std::cout << "[Snapshot] Saved " << folder.string() << " (state" << (tape_recorder != nullptr ? ", input tape" : "")
                          << ", report) in " << ms << " ms" << (armed ? "; the reference frame follows" : "") << "\n";
                gekko::dev::Json event;
                event.field("ok", saved).field("folder", utf8_path(folder)).raw("manifest", manifest.str());
                dev_link.event("snapshot", event);
                snapshot_stop_pending.store(false, std::memory_order_release);
                dev_reanchor();  // the save's wall time is not game time
            };
            // The reference frame of the last snapshot, once its renderer has written it (the UI thread polls).
            const auto poll_snapshot_reference = [&]() {
                std::lock_guard<std::mutex> guard(snapshot_mutex);
                if (!snapshot_reference.armed) return;
                const DevCapture result = dev_capture_result();
                if (result.written <= snapshot_reference.baseline) return;
                snapshot_reference.armed = false;
                gekko::dev::Json reference;
                reference.field("format", "shadow-snapshot-reference 1")
                    .field("id", snapshot_reference.id)
                    .field("path", result.path)
                    .field("frame", result.frame)
                    .field("frame_game_ticks", result.game_ticks)
                    .field("boundary_tick", snapshot_reference.boundary_tick);
                {
                    std::ofstream out(snapshot_reference.folder / "reference.json", std::ios::binary | std::ios::trunc);
                    out << reference.str() << "\n";
                }
                std::cout << "[Snapshot] Reference frame " << result.frame << " (game tick " << result.game_ticks
                          << ") -> " << result.path << "\n";
                gekko::dev::Json event;
                event.field("folder", utf8_path(snapshot_reference.folder)).raw("reference", reference.str());
                dev_link.event("snapshot_reference", event);
            };
            const auto dev_handle = [&](const gekko::dev::Request& r) {
                const std::string& cmd = r.cmd;
                if (cmd == "hello" || cmd == "status") {
                    dev_link.reply(r.id, true, dev_status());
                } else if (cmd == "pause") {
                    dev_paused.store(true, std::memory_order_release);
                    dev_link.reply(r.id, true, dev_status());
                } else if (cmd == "resume") {
                    dev_paused.store(false, std::memory_order_release);
                    dev_link.reply(r.id, true, dev_status());
                } else if (cmd == "run_ticks") {
                    if (dev_run_waiting) {
                        dev_link.reply(r.id, false, gekko::dev::Json(), "a run is already in progress");
                        return;
                    }
                    const gekko::u64 ticks = (std::max<gekko::u64>)(1u, r.unsigned_number("ticks", 1));
                    dev_run_capture = false;
                    const std::string capture = r.text("path");
                    if (!capture.empty()) {
                        // The first frame finished at or after capture_tick (default: the run's last frame period).
                        gekko::u32 width = 0, height = 0;
                        dev_client_size(width, height);
                        width = static_cast<gekko::u32>(r.unsigned_number("width", width));
                        height = static_cast<gekko::u32>(r.unsigned_number("height", height));
                        const gekko::u64 at = r.unsigned_number("capture_tick", system.game_ticks() + ticks);
                        dev_capture_baseline = dev_capture_result().written;
                        dev_run_capture_path =
                            utf8_path(dev_arm_capture_at_tick(path_from_utf8(capture), at, width, height));
                        dev_run_capture = true;
                    }
                    dev_run_id = r.id;
                    dev_run_waiting = true;
                    dev_run_target_ticks.store(system.game_ticks() + ticks, std::memory_order_release);
                    dev_paused.store(false, std::memory_order_release);  // answered at the boundary after it
                } else if (cmd == "capture" && r.has("pause") && !r.flag("pause")) {
                    // The next frame, captured while the game keeps running (a thumbnail).
                    const std::string capture = r.text("path");
                    if (capture.empty() || dev_capture_only.load(std::memory_order_acquire)) {
                        dev_link.reply(r.id, false, gekko::dev::Json(),
                                       capture.empty() ? "capture needs a path" : "a capture is pending");
                        return;
                    }
                    gekko::u32 width = 0, height = 0;
                    dev_client_size(width, height);
                    width = static_cast<gekko::u32>(r.unsigned_number("width", width));
                    height = static_cast<gekko::u32>(r.unsigned_number("height", height));
                    dev_capture_only_baseline.store(dev_capture_result().written, std::memory_order_release);
                    dev_capture_only_path = utf8_path(dev_arm_capture(path_from_utf8(capture), 1u, width, height));
                    dev_capture_only_id = r.id;
                    dev_capture_only.store(true, std::memory_order_release);
                } else if (cmd == "run_frames" || cmd == "capture") {
                    if (dev_run_waiting) {
                        dev_link.reply(r.id, false, gekko::dev::Json(), "a run is already in progress");
                        return;
                    }
                    const gekko::u64 frames =
                        cmd == "capture" ? 1u : (std::max<gekko::u64>)(1u, r.unsigned_number("frames", 1));
                    dev_run_capture = false;
                    const std::string capture = r.text("path");
                    if (!capture.empty()) {
                        gekko::u32 width = 0, height = 0;
                        dev_client_size(width, height);
                        width = static_cast<gekko::u32>(r.unsigned_number("width", width));
                        height = static_cast<gekko::u32>(r.unsigned_number("height", height));
                        dev_capture_baseline = dev_capture_result().written;
                        dev_run_capture_path =
                            utf8_path(dev_arm_capture(path_from_utf8(capture), frames, width, height));
                        dev_run_capture = true;
                    }
                    dev_run_id = r.id;
                    dev_run_waiting = true;
                    dev_run_target.store(render_frames() + frames, std::memory_order_release);
                    dev_paused.store(false, std::memory_order_release);  // answered once the frames are done
                } else if (cmd == "save") {
                    const std::filesystem::path path = path_from_utf8(r.text("path"));
                    std::string error;
                    if (path.empty()) {
                        dev_link.reply(r.id, false, gekko::dev::Json(), "save needs a path");
                        return;
                    }
                    if (!gekko::save_state(system, path, &error)) {
                        dev_link.reply(r.id, false, gekko::dev::Json(), error);
                        return;
                    }
                    std::cout << "[dev-link] saved " << path.string() << " at " << game_seconds(system)
                              << " s of game time\n";
                    gekko::dev::Json body = dev_status();
                    body.field("path", utf8_path(path))
                        .field("stop_tick", dev_request_tick.load(std::memory_order_acquire));
                    const std::string tape = r.text("tape");
                    if (!tape.empty()) {
                        if (tape_recorder == nullptr) {
                            body.null_field("tape").field("tape_error", "this run records no input tape");
                        } else if (!tape_recorder->write(path_from_utf8(tape),
                                                         dev_request_tick.load(std::memory_order_acquire),
                                                         system.game_ticks(), &error)) {
                            body.null_field("tape").field("tape_error", error);
                        } else {
                            body.field("tape", tape).field("tape_start_tick", tape_recorder->start_tick());
                        }
                    }
                    const std::string report = r.text("report");
                    if (!report.empty()) {
                        std::vector<GuestRange> ranges = report_ranges;
                        std::string range_error;
                        parse_guest_ranges(r.text("peeks"), ranges, &range_error);
                        std::ofstream out(path_from_utf8(report), std::ios::binary | std::ios::trunc);
                        out << state_report_json(system, render_frames(),
                                                 dev_request_tick.load(std::memory_order_acquire), ranges, true)
                            << "\n";
                        body.field("report", report);
                    }
                    dev_link.reply(r.id, true, body);
                } else if (cmd == "load") {
                    const std::filesystem::path path = path_from_utf8(r.text("path"));
                    std::string error;
                    if (!gekko::load_state(system, path, &error)) {
                        dev_link.reply(r.id, false, gekko::dev::Json(), error);
                        // A load refused at its header changed nothing; one that failed later left the machine
                        // inconsistent (savestate.h): stop rather than run garbage.
                        if (error.find("inconsistent") != std::string::npos) {
                            stop_requested.store(true, std::memory_order_release);
                        }
                        return;
                    }
                    std::cout << "[dev-link] restored " << path.string() << " at " << game_seconds(system)
                              << " s of game time\n";
                    dev_reanchor();
                    if (input_tape.playing()) input_tape.stop_playing();
                    if (tape_recorder != nullptr) {
                        gekko::u32 width = 0, height = 0;
                        dev_client_size(width, height);
                        tape_recorder->begin_recording(system.game_ticks(), static_cast<int>(width),
                                                       static_cast<int>(height));
                    }
                    if (r.has("pause")) dev_paused.store(r.flag("pause"), std::memory_order_release);
                    gekko::dev::Json body = dev_status();
                    body.field("path", utf8_path(path));
                    dev_link.reply(r.id, true, body);
                } else if (cmd == "peek") {
                    // SCAFFOLDING (M1): reads the game's address window, which exists while the game's code is
                    // translated - deleted with the window. The workstation, not the game, knows what is there.
                    std::vector<GuestRange> ranges;
                    std::string range_error;
                    if (!parse_guest_ranges(r.text("range"), ranges, &range_error) || ranges.size() != 1u) {
                        dev_link.reply(r.id, false, gekko::dev::Json(),
                                       range_error.empty() ? "peek needs one range (address:length)" : range_error);
                        return;
                    }
                    gekko::u32 resolved = 0;
                    const gekko::u8* bytes = resolve_guest_range(system, ranges[0], &resolved);
                    if (bytes == nullptr) {
                        dev_link.reply(r.id, false, gekko::dev::Json(), "the range is not mapped");
                        return;
                    }
                    gekko::dev::Json body;
                    body.field("address", resolved).field("hex", hex_bytes(bytes, ranges[0].length));
                    dev_link.reply(r.id, true, body);
                } else if (cmd == "report") {
                    std::vector<GuestRange> ranges = report_ranges;
                    std::string range_error;
                    if (!parse_guest_ranges(r.text("peeks"), ranges, &range_error)) {
                        dev_link.reply(r.id, false, gekko::dev::Json(), range_error);
                        return;
                    }
                    gekko::dev::Json body;
                    body.raw("report", state_report_json(system, render_frames(),
                                                         dev_request_tick.load(std::memory_order_acquire), ranges,
                                                         r.flag("memory", true)));
                    dev_link.reply(r.id, true, body);
                } else if (cmd == "tape_play") {
                    std::string error;
                    if (!input_tape.load(path_from_utf8(r.text("path")), &error)) {
                        dev_link.reply(r.id, false, gekko::dev::Json(), error);
                        return;
                    }
                    gekko::dev::Json body = dev_status();
                    body.field("tape_samples", static_cast<gekko::u64>(input_tape.size()))
                        .field("tape_start_tick", input_tape.start_tick());
                    dev_link.reply(r.id, true, body);
                } else if (cmd == "tape_stop") {
                    input_tape.stop_playing();
                    dev_link.reply(r.id, true, dev_status());
                } else if (cmd == "snapshot") {
                    // The snapshot key's action, asked by the workstation (Create Snapshot): taken here, at the
                    // boundary this request is served at (the one the last request asked for); the "snapshot" event
                    // follows the reply.
                    snapshot_request_tick.store(dev_request_tick.load(std::memory_order_acquire),
                                                std::memory_order_release);
                    snapshot_press_tick.store(system.game_ticks(), std::memory_order_release);
                    dev_link.reply(r.id, true, dev_status());
                    take_snapshot();
                } else if (cmd == "restart_stage") {
                    // The pause menu's Restart (0x801E4064): StageAction manager request 7 (RestartAction), forced
                    // - 0x801762D0(mgr, 7, 1). Same gate as the menu (0x801E3298): one player, a life left. The
                    // stage restarts from its start; the action takes the life. docs/tools/STAGE_CONTROL.md.
                    gekko::Cpu& cpu = system.cpu();
                    // Both through the game's own small-data caches: the manager (0x80074598 stores it at
                    // r13+0x2EDC) and the stage record (0x8007993C, r13+0x2BC0); player 0's lives are the record's
                    // +0x748 (0x80178F10 player 0 = +0x728, +0x20).
                    constexpr gekko::u32 kPlayAction = 3u, kRestartAction = 7u;
                    const gekko::u32 manager = cpu.mem_read32(cpu.gpr[13] + 0x2EDCu);
                    const gekko::u32 record = cpu.mem_read32(cpu.gpr[13] + 0x2BC0u);
                    const gekko::u32 kLives = record + 0x748u;
                    const char* refused = nullptr;
                    if (manager == 0u || record == 0u) {
                        refused = "no stage is running (the StageAction manager does not exist yet)";
                    } else if (cpu.mem_read32(manager + 0x8u) != kPlayAction) {
                        refused = "the stage is not in play (restart only from the Play action)";
                    } else if (cpu.mem_read8(manager + 0x17u + kRestartAction) != 0u) {
                        refused = "the stage refuses restart requests right now";
                    } else if (cpu.mem_read8(0x8057E760u + 0xB2u) != 0u) {
                        refused = "two-player stages have no Restart";
                    } else if (static_cast<gekko::s32>(cpu.mem_read32(kLives)) <= 0) {
                        refused = "no life left (the pause menu hides Restart too)";
                    }
                    if (refused != nullptr) {
                        dev_link.reply(r.id, false, gekko::dev::Json(), refused);
                        return;
                    }
                    cpu.mem_write32(manager + 0xCu, kRestartAction);
                    std::cout << "[dev-link] stage restart requested at " << game_seconds(system)
                              << " s of game time\n";
                    gekko::dev::Json body = dev_status();
                    body.field("lives", static_cast<gekko::u64>(cpu.mem_read32(kLives)));
                    dev_link.reply(r.id, true, body);
                } else if (cmd == "quit") {
                    dev_link.reply(r.id, true, dev_status());
                    stop_requested.store(true, std::memory_order_release);
                    system.cpu().request_stop_from_host(gekko::StopReason::HostRequest, "development link quit");
                } else {
                    dev_link.reply(r.id, false, gekko::dev::Json(), "unknown command '" + cmd + "'");
                }
            };
            const auto dev_serve = [&]() {
                dev_stop_pending.store(false, std::memory_order_release);
                bool waited = false;
                for (;;) {
                    gekko::dev::Request request;
                    if (dev_link.take(&request)) {
                        dev_handle(request);
                        continue;
                    }
                    if (dev_capture_only.load(std::memory_order_acquire) &&
                        dev_capture_result().written > dev_capture_only_baseline.load(std::memory_order_acquire)) {
                        const DevCapture result = dev_capture_result();
                        dev_capture_only.store(false, std::memory_order_release);
                        gekko::dev::Json body = dev_status();
                        body.field("capture", result.path).field("capture_frame", result.frame).field("captured", true);
                        dev_link.reply(dev_capture_only_id, true, body);
                        continue;
                    }
                    const gekko::u64 tick_target = dev_run_target_ticks.load(std::memory_order_acquire);
                    if (dev_run_waiting && tick_target != 0u && system.game_ticks() >= tick_target) {
                        dev_run_target_ticks.store(0u, std::memory_order_release);
                        dev_paused.store(true, std::memory_order_release);
                        gekko::dev::Json body;
                        bool captured = true;
                        if (dev_run_capture) {
                            for (int wait = 0; wait < 1000 && dev_capture_result().written <= dev_capture_baseline;
                                 ++wait) {
                                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                            }
                            const DevCapture result = dev_capture_result();
                            captured = result.written > dev_capture_baseline;
                            body = dev_status();
                            body.field("capture", captured ? result.path : dev_run_capture_path)
                                .field("capture_frame", result.frame)
                                .field("capture_game_ticks", result.game_ticks)
                                .field("captured", captured);
                        } else {
                            body = dev_status();
                        }
                        dev_link.reply(dev_run_id, captured, body, captured ? std::string() : "the capture was not written");
                        dev_run_waiting = false;
                        dev_run_capture = false;
                        waited = true;
                        continue;
                    }
                    const gekko::u64 target = dev_run_target.load(std::memory_order_acquire);
                    if (dev_run_waiting && target != 0u && render_frames() >= target) {
                        // The frames are handed over: pause here and answer (with the capture, once its renderer
                        // has written it - the render thread draws the frame the game just finished).
                        dev_run_target.store(0u, std::memory_order_release);
                        dev_paused.store(true, std::memory_order_release);
                        gekko::dev::Json body;
                        bool captured = true;
                        if (dev_run_capture) {
                            for (int wait = 0; wait < 1000 && dev_capture_result().written <= dev_capture_baseline;
                                 ++wait) {
                                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                            }
                            const DevCapture result = dev_capture_result();
                            captured = result.written > dev_capture_baseline;
                            body = dev_status();
                            body.field("capture", captured ? result.path : dev_run_capture_path)
                                .field("capture_frame", result.frame)
                                .field("captured", captured);
                        } else {
                            body = dev_status();
                        }
                        dev_link.reply(dev_run_id, captured, body, captured ? std::string() : "the capture was not written");
                        dev_run_waiting = false;
                        dev_run_capture = false;
                        waited = true;
                        continue;
                    }
                    if (!dev_paused.load(std::memory_order_acquire) || stop_requested.load(std::memory_order_acquire)) {
                        break;
                    }
                    if (!dev_link.connected()) {
                        // The workstation went away: play on.
                        dev_paused.store(false, std::memory_order_release);
                        break;
                    }
                    waited = true;
                    if (dev_link.wait(&request, std::chrono::milliseconds(50))) dev_handle(request);
                }
                if (waited) dev_reanchor();
            };
            std::thread game([&]() {
                GEKKO_PROFILE_THREAD("game");
                gekko::name_this_thread("game");  // ES-T1
                gekko::SelfSampler self_sampler;  // GEKKO_SELF_SAMPLE (diagnostic; off unless set)
                try {
                    game_thread_policy = apply_game_thread_performance_policy();
                    // --start-paused: the workstation's "resume" (or a run) comes before the first instruction.
                    if (dev_paused.load(std::memory_order_acquire)) dev_serve();
                    // Started HERE, on the thread that executes translated code,
                    // so the renderer's owner checks can tell the two apart.
                    // Savestates are serviced BETWEEN runs, never inside
                    // the host service. Only here has translated code fully
                    // unwound, so cpu.pc is a real resume point and no guest
                    // frame is left on the native stack for a restore to
                    // return into. The host service asks for the boundary by
                    // calling request_stop; this loop provides it and resumes.
                    for (;;) {
                        system.run();
                        retired_published.store(system.game_ticks(), std::memory_order_relaxed);

                        const bool want_save = savestate_save_requested.exchange(
                            false, std::memory_order_acq_rel);
                        const bool want_load = savestate_load_requested.exchange(
                            false, std::memory_order_acq_rel);
                        const bool want_dev = dev_boundary.exchange(false, std::memory_order_acq_rel) ||
                                              dev_paused.load(std::memory_order_acquire) || dev_link.pending();
                        const bool want_snapshot = snapshot_stop_pending.load(std::memory_order_acquire);
                        if ((!want_save && !want_load && !want_dev && !want_snapshot) || budget_reached ||
                            stop_requested.load(std::memory_order_acquire)) {
                            break;
                        }
                        // First: the moment the key asked for, before any N / M of the same boundary.
                        if (want_snapshot) take_snapshot();

                        std::string state_error;
                        if (want_save) {
                            if (gekko::save_state(system, state_slot, &state_error)) {
                                std::cout << "[savestate] wrote " << state_slot.string() << " at "
                                          << game_seconds(system) << " s of game time\n";
                            } else {
                                std::cerr << "[savestate] " << state_error << "\n";
                            }
                        }
                        bool load_failed = false;
                        if (want_load) {
                            if (gekko::load_state(system, state_slot, &state_error)) {
                                // A restored game timeline has no relationship
                                // to this session's wall anchor.
                                const auto restored_at = std::chrono::steady_clock::now();
                                simulation_clock.reset(system.game_ticks(), restored_at);
                                last_service_instructions = system.game_ticks();
                                last_service_end = restored_at;
                                paced_between_ns = 0;
                                audio_runtime.reset_timeline();
                                std::cout << "[savestate] restored " << state_slot.string()
                                          << " at " << game_seconds(system)
                                          << " s of game time\n";
                                // The run's starting state is replaced: a recorded tape starts again here, and the
                                // workstation learns that this run no longer continues its checkpoint.
                                if (input_tape.playing()) input_tape.stop_playing();
                                if (tape_recorder != nullptr) {
                                    int client_w = 0, client_h = 0;
                                    window.client_size(client_w, client_h);
                                    tape_recorder->begin_recording(system.game_ticks(), client_w, client_h);
                                }
                                gekko::dev::Json loaded;
                                loaded.field("source", "hotkey")
                                    .field("path", utf8_path(state_slot))
                                    .field("game_ticks", system.game_ticks());
                                dev_link.event("loaded", loaded);
                            } else {
                                std::cerr << "[savestate] " << state_error << "\n";
                                // load_state only mutates after the header
                                // validates, but once it has started there is no
                                // way back. Resuming a half-restored machine
                                // would produce garbage that looks like a
                                // rendering bug, so stop instead.
                                load_failed = true;
                            }
                        }
                        std::cout.flush();
                        if (load_failed) {
                            stop_requested.store(true, std::memory_order_release);
                            break;
                        }
                        if (want_dev) dev_serve();
                        if (stop_requested.load(std::memory_order_acquire)) break;

                        // Clear unconditionally: a failed save or load must not
                        // leave the machine stopped on the savestate request.
                        system.cpu().clear_stop();
                    }
                    if (budget_reached) system.cpu().clear_stop();
                } catch (...) {
                    game_exception = std::current_exception();
                }
                audio_runtime.finish_production();  // AU-T1: the game thread produced its last block; device silence from here is the run's tail
                game_done.store(true, std::memory_order_release);
            });

            // ---- the UI thread ---------------------------------------------
            // The render clock consumes immutable completed XFB snapshots. It
            // has no route back into Cpu/System, so changing this rate cannot
            // change gameplay speed. Two source frames are retained for causal
            // one-frame-delayed presentation interpolation.
            gekko::RenderClock render_clock(options.render_rate);
            render_clock.reset(std::chrono::steady_clock::now());
            bool frame_interpolation = options.frame_interpolation;  // the display settings change it live
            const auto presentation_start = std::chrono::steady_clock::now();
            gekko::u64 native_current_generation = 0;
            gekko::u64 native_previous_generation = 0;
            auto native_transition_start = presentation_start;
            auto last_native_arrival = presentation_start;
            std::chrono::steady_clock::duration native_source_period =
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(1.0 / 60.0));
            bool have_native_source_period = false;
            // The UI thread's wait between presents: a high-resolution timer
            // for the computed render deadline, owned by the presentation loop.
            RecoveredClockSleeper present_wait;
            // D06-F4 (2026-09-02): hysteresis on the cross-fade decision. The
            // old single test - engage whenever the render rate exceeded the
            // source rate by 5 %, i.e. below 0.952x - sat inside the measured
            // windowed speed band (mean 0.998, p5 0.84-0.97), so the blend and
            // the one source frame of display latency it costs by construction
            // switched on and off several times a run. Engage only once the
            // source has clearly fallen behind, release only once it has
            // clearly caught up; between the two the previous decision stands.
            constexpr double kInterpolationEngageRatio = 0.90;
            constexpr double kInterpolationReleaseRatio = 0.97;
            bool interpolation_engaged = false;
            std::vector<double> presentation_intervals_ms;
            std::optional<std::chrono::steady_clock::time_point> last_present_time;
            gekko::u64 last_presented_generation = 0;
            std::optional<std::chrono::steady_clock::time_point> last_unique_present_time;
            // Arrival time of the most recent native generations, and the time
            // of the previous present tick, for arrival-order presentation.
            // MEASURED 2026-09-16 (Gun Fortress, 62 s): the guest completes
            // ~60 frames/s but in bands every ~16.7 s (the 59.94 Hz VI field
            // against the 60 Hz pacer quantum) emits them in pairs ~10 ms apart,
            // which the pacer's burst lands inside one present interval; taking
            // the newest then skipped ~600 frames and repeated ~600 per run.
            std::array<std::pair<gekko::u64, std::chrono::steady_clock::time_point>, 8>
                native_arrivals{};
            std::size_t native_arrival_cursor = 0;
            std::optional<std::chrono::steady_clock::time_point> last_tick_time;
            unsigned fifo_lag_ticks = 0;
            constexpr unsigned kFifoMaxLagTicks = 12;  // 200 ms at 60 Hz
            gekko::u64 present_ns = 0;
            gekko::u64 timed_xfb = 0;
            gekko::u64 frame_stalls = 0;   // GX active, yet no XFB completed
            double longest_frame_stall_ms = 0.0;
            gekko::u64 no_gx_spells = 0;   // the game issued no GX work - not a stall
            double longest_no_gx_ms = 0.0;
            auto last_frame_arrival = std::chrono::steady_clock::now();
            // A dry spell is one continuous run of polls with no completed XFB.
            // Each poll interval is attributed to a GX-active portion (the game
            // submitted copies, so frames should have completed - a genuine
            // frame-production stall) or a no-GX portion (the game is not
            // drawing: a load or a transition, which is not a stall). One
            // report per spell, when it ends - the old per-poll report turned a
            // single 5.84 s loading window into 177 stall lines.
            bool spell_open = false;
            std::chrono::steady_clock::duration spell_gx_active{};
            std::chrono::steady_clock::duration spell_no_gx{};
            auto spell_last_poll = last_frame_arrival;
            const auto close_spell = [&](const char* note) {
                if (!spell_open) {
                    return;
                }
                spell_open = false;
                const double gx_ms =
                    std::chrono::duration<double, std::milli>(spell_gx_active).count();
                const double dry_ms =
                    std::chrono::duration<double, std::milli>(spell_no_gx).count();
                const auto instruction = static_cast<unsigned long long>(
                    retired_published.load(std::memory_order_relaxed));
                // The shipping executable has no GX-activity signal on this
                // thread (the legacy raster's copy counter was its only one),
                // so a dry spell is reported as what it measurably is: time
                // with no newly completed native frame.
                (void)gx_ms;
                const auto generation = static_cast<unsigned long long>(native_current_generation);
                if (spell_no_gx >= kFrameProductionStallThreshold) {
                    ++no_gx_spells;
                    longest_no_gx_ms = (std::max)(longest_no_gx_ms, dry_ms);
                    std::fprintf(stderr,
                                 "[frame-pacing] no completed native frame for %.3f ms "
                                 "(generation %llu, guest instruction %llu%s)\n",
                                 dry_ms, generation, instruction, note);
                }
                std::fflush(stderr);
            };
            const auto account_spell_interval =
                [&](std::chrono::steady_clock::time_point now) {
                    if (!spell_open) {
                        return;
                    }
                    // A poll gap far beyond the 2 ms cadence means this
                    // thread was not polling at all - a title-bar drag holds
                    // pump() inside the Win32 modal move/size loop for its
                    // whole duration. One counter comparison cannot say how
                    // such a gap was really split, so at most this much of it
                    // is classified and the rest is discarded as unobserved.
                    // Otherwise one stray texture copy inside a 3 s drag
                    // would be reported as a 3 s frame-production stall.
                    constexpr auto kMaxAttributedPollInterval =
                        std::chrono::milliseconds(50);
                    auto interval = now - spell_last_poll;
                    if (interval > kMaxAttributedPollInterval) {
                        interval = kMaxAttributedPollInterval;
                    }
                    spell_no_gx += interval;
                    spell_last_poll = now;
                };
            const auto presenter_failed = [&]() {
                if (!window.plume_failed()) return false;
                native_presenter_failed = true;
                stop_requested.store(true, std::memory_order_release);
                try {
                    presenter_error = window.plume_error();
                    std::cerr << "ERROR: native presenter: " << presenter_error << '\n';
                } catch (...) {
                    std::fputs("ERROR: native presenter diagnostic snapshot failed\n", stderr);
                }
                return true;
            };
            // The host service serves stop_requested at the next game-clock
            // event; a guest that is not reaching a wait point would never get
            // there, so the close is also posted to the core directly (the one
            // cross-thread stop, served at the guest's next dispatch). Once.
            bool shutdown_posted = false;
#if defined(SHADOW_RENDER_LIFTED)
            // ---- the UI thread of the lifted renderer (R2_DRIVER.md 4.2) ----
            // The window's messages, the controller samples, the size, and the newest frame the game finished
            // (the renderer records and presents it on its own swap chain). Nothing here touches the guest.
            {
                int client_w = 0;
                int client_h = 0;
                window.client_size(client_w, client_h);
                std::string attach_error;
                if (!lifted_render.attach(window.native_handle(), static_cast<gekko::u32>((std::max)(client_w, 1)),
                                          static_cast<gekko::u32>((std::max)(client_h, 1)), &attach_error)) {
                    std::cerr << "ERROR: native renderer: " << attach_error << '\n';
                    native_presenter_failed = true;
                    presenter_error = attach_error;
                    stop_requested.store(true, std::memory_order_release);
                    shutdown_posted = true;
                    system.cpu().request_stop_from_host(gekko::StopReason::HostRequest, "renderer failed");
                }
            }
            while (!game_done.load(std::memory_order_acquire)) {
                GEKKO_PROFILE_ZONE("ui/iteration");  // the lifted present loop: messages, input, one present
                if (!window.pump() || window.escape_pressed()) {
                    stop_requested.store(true, std::memory_order_release);
                    if (!shutdown_posted) {
                        shutdown_posted = true;
                        system.cpu().request_stop_from_host(gekko::StopReason::HostRequest,
                                                            "host requested shutdown");
                    }
                }
                if (window.take_save_state_request()) {
                    savestate_save_requested.store(true, std::memory_order_release);
                }
                if (window.take_load_state_request()) {
                    savestate_load_requested.store(true, std::memory_order_release);
                }
                for (unsigned requests = window.take_snapshot_requests(); requests != 0u; --requests) {
                    if (snapshot_requested.fetch_add(1u, std::memory_order_acq_rel) == 0u) {
                        snapshot_press_tick.store(retired_published.load(std::memory_order_relaxed),
                                                  std::memory_order_release);
                        std::cout << "[Snapshot] " << options.snapshot_key << " pressed\n";
                    }
                }
                poll_snapshot_reference();
                // The marker key (default F8): a replay marker, so the moment can be replayed and inspected again.
                for (unsigned marks = window.take_user_marks(); marks != 0u; --marks) {
                    const double t = std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - execution_start).count();
                    double point[2];
                    const bool pointed = window.mark_point(point[0], point[1]);
                    record_marker(retired_published.load(std::memory_order_relaxed), t, "point",
                                  pointed ? point : nullptr);
                    gekko::audio_audit::user_mark(marker_ordinal);
                }
                for (unsigned marks = window.take_range_marks(); marks != 0u; --marks) {
                    const double t = std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - execution_start).count();
                    if (!range_open)
                        ++range_ordinal;
                    range_open = !range_open;
                    record_marker(retired_published.load(std::memory_order_relaxed), t,
                                  range_open ? "range-start" : "range-end");
                }
                // The effect layer keys (F6 the list; gekko/effect_layer_keys.h): what is drawn, from the next frame.
                lifted_render.effect_layer_keys(window.take_effect_keys());
                // The display settings' UI groups (gekko/display_settings.h), from this iteration: the window's mode
                // and size (the resize below takes the new client area to the swap chain), V-Sync from the next
                // present, the render clock's rate and frame blending, the FPS counter.
                {
                    gekko::DisplaySettings settings;
                    const unsigned changes = gekko::display_settings().take_changes(
                        gekko::kDisplayWindowChange | gekko::kDisplayPresentChange | gekko::kDisplayOverlayChange,
                        &settings);
                    if ((changes & gekko::kDisplayWindowChange) != 0u) {
                        const bool exact = settings.render_width > 0 && settings.render_height > 0;
                        gekko::HostWindow::set_content_aspect(
                            exact ? static_cast<double>(settings.render_width) / settings.render_height : 0.0);
                        window.set_mode(settings.fullscreen, settings.window_width, settings.window_height);
                    }
                    if ((changes & gekko::kDisplayPresentChange) != 0u) {
                        lifted_render.set_vsync(settings.vsync);
                        render_clock.set_target_hz(settings.render_rate);
                        render_clock.reset(std::chrono::steady_clock::now());
                        frame_interpolation = settings.frame_interpolation;
                    }
                    if ((changes & gekko::kDisplayOverlayChange) != 0u)
                        lifted_render.set_fps_counter(settings.fps_counter);
                }
                {
                    GEKKO_PROFILE_ZONE("ui/resize");
                    int client_w = 0;
                    int client_h = 0;
                    window.client_size(client_w, client_h);
                    if (client_w > 0 && client_h > 0) {
                        // The game's output follows at the next host service (latch_game_output).
                        pending_game_output.store((static_cast<std::uint64_t>(client_w) << 32) |
                                                      static_cast<std::uint32_t>(client_h),
                                                  std::memory_order_release);
                        std::string resize_error;
                        if (!lifted_render.resize(static_cast<gekko::u32>(client_w), static_cast<gekko::u32>(client_h),
                                                  &resize_error)) {
                            std::cerr << "ERROR: native renderer: " << resize_error << '\n';
                        }
                    }
                }
                {
                    std::array<shadow::peripheral::ControllerSample, 4> sampled{};
                    sampled[0] = window.controller();
                    if (options.xinput) {
                        for (unsigned port = 1; port < 4; ++port) {
                            sampled[port] = gekko::poll_xinput_controller(port);
                        }
                    }
                    std::lock_guard<std::mutex> guard(pad_mutex);
                    pads_published = sampled;
                }
                std::string render_error;
                gekko::LiftedRender::PresentationSample sample;
                const bool render_ok = lifted_render.present_newest(std::chrono::milliseconds(8), &render_error,
                                                                    performance_log ? &sample : nullptr);
                if (performance_log && sample.attempted) {
                    performance_log << std::fixed << std::setprecision(6) << "1,"
                        << std::chrono::duration<double>(sample.finished_at - execution_start).count()
                        << ',' << sample.index << ',' << sample.packet_tick << ',' << sample.game_ticks
                        << ',' << (sample.success ? 1 : 0) << ',';
                    if (sample.has_interval) performance_log << sample.interval_ms;
                    performance_log << ',' << sample.packet_wait_ns / 1.0e6 << ',' << sample.capture_ns / 1.0e6
                        << ',' << sample.render_present_ns / 1.0e6 << '\n';
                }
                if (!render_ok) {
                    std::cerr << "ERROR: native renderer: " << render_error << '\n';
                    native_presenter_failed = true;
                    presenter_error = render_error;
                    stop_requested.store(true, std::memory_order_release);
                    if (!shutdown_posted) {
                        shutdown_posted = true;
                        system.cpu().request_stop_from_host(gekko::StopReason::HostRequest, "renderer failed");
                    }
                    break;
                }
            }
#else
            while (!game_done.load(std::memory_order_acquire)) {
                if (!window.pump() || window.escape_pressed()) {
                    stop_requested.store(true, std::memory_order_release);
                    if (!shutdown_posted) {
                        shutdown_posted = true;
                        system.cpu().request_stop_from_host(gekko::StopReason::HostRequest,
                                                            "host requested shutdown");
                    }
                }
                if (window.take_save_state_request()) {
                    savestate_save_requested.store(true, std::memory_order_release);
                }
                if (window.take_load_state_request()) {
                    savestate_load_requested.store(true, std::memory_order_release);
                }
                for (unsigned requests = window.take_snapshot_requests(); requests != 0u; --requests) {
                    if (snapshot_requested.fetch_add(1u, std::memory_order_acq_rel) == 0u) {
                        snapshot_press_tick.store(retired_published.load(std::memory_order_relaxed),
                                                  std::memory_order_release);
                        std::cout << "[Snapshot] " << options.snapshot_key << " pressed\n";
                    }
                }
                poll_snapshot_reference();
                for (unsigned marks = window.take_user_marks(); marks != 0u; --marks) {
                    const double t = std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - execution_start).count();
                    double point[2];
                    const bool pointed = window.mark_point(point[0], point[1]);
                    record_marker(retired_published.load(std::memory_order_relaxed), t, "point",
                                  pointed ? point : nullptr);
                    gekko::audio_audit::user_mark(marker_ordinal);
                }
                for (unsigned marks = window.take_range_marks(); marks != 0u; --marks) {
                    const double t = std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - execution_start).count();
                    if (!range_open)
                        ++range_ordinal;
                    range_open = !range_open;
                    record_marker(retired_published.load(std::memory_order_relaxed), t,
                                  range_open ? "range-start" : "range-end");
                }
                // The display settings (gekko/display_settings.h): the window's mode and size, the render clock's rate
                // and frame blending, from this iteration.
                {
                    gekko::DisplaySettings settings;
                    const unsigned changes = gekko::display_settings().take_changes(
                        gekko::kDisplayWindowChange | gekko::kDisplayPresentChange, &settings);
                    if ((changes & gekko::kDisplayWindowChange) != 0u)
                        window.set_mode(settings.fullscreen, settings.window_width, settings.window_height);
                    if ((changes & gekko::kDisplayPresentChange) != 0u) {
                        render_clock.set_target_hz(settings.render_rate);
                        render_clock.reset(std::chrono::steady_clock::now());
                        frame_interpolation = settings.frame_interpolation;
                    }
                }
                {
                    int client_w = 0;
                    int client_h = 0;
                    window.client_size(client_w, client_h);
                    if (client_w > 0 && client_h > 0) {
                        system.display().resize(client_w, client_h);
                        if (native_gpu != nullptr) {
                            const gekko::DisplayLayout layout =
                                gekko::make_display_layout(display, client_w, client_h);
                            native_gpu->request_output_size(
                                static_cast<gekko::u32>(layout.output_width),
                                static_cast<gekko::u32>(layout.output_height), layout,
                                options.internal_scale_auto
                                    ? 0u
                                    : static_cast<gekko::u32>(internal_scale));
                        }
                    }
                }
                {
                    std::array<shadow::peripheral::ControllerSample, 4> sampled{};
                    sampled[0] = window.controller();
                    if (options.xinput) {
                        for (unsigned port = 1; port < 4; ++port) {
                            sampled[port] = gekko::poll_xinput_controller(port);
                        }
                    }
                    std::lock_guard<std::mutex> guard(pad_mutex);
                    pads_published = sampled;
                }

                // D06-F7: hand back any native XFB pin whose flight slot has
                // already retired. Non-blocking, three integer comparisons
                // when the deferred release is off, and it must run every
                // iteration: the rasterizer has three native slots and the
                // guest holds two, so a pin kept until the flight slot is
                // recycled three presents later would starve reserve.
                window.plume_retire_completed_presents();
                if (presenter_failed()) break;

                if (options.native_xfb_present || native_gpu != nullptr) {
                    const gekko::u64 generation = native_gpu->output_generation();
                    if (generation > native_current_generation) {
                        const auto arrival = std::chrono::steady_clock::now();
                        if (native_current_generation != 0u && arrival > last_native_arrival) {
                            const gekko::u64 delta = generation - native_current_generation;
                            const double seconds =
                                std::chrono::duration<double>(arrival - last_native_arrival).count() /
                                static_cast<double>(delta);
                            if (seconds >= 0.004 && seconds <= 0.100) {
                                const double old_seconds =
                                    std::chrono::duration<double>(native_source_period).count();
                                const double filtered = have_native_source_period
                                                            ? old_seconds * 0.8 + seconds * 0.2
                                                            : seconds;
                                native_source_period =
                                    std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                        std::chrono::duration<double>(filtered));
                                have_native_source_period = true;
                            }
                        }
                        for (gekko::u64 arrived =
                                 (std::max)(native_current_generation + 1u,
                                            generation > 7u ? generation - 7u : 1u);
                             arrived <= generation; ++arrived) {
                            // Native renderer: the frame "arrives" when the guest
                            // produced it. Keying the same-interval pair test on
                            // when the render thread published it instead turned
                            // publish jitter into false pairs and lag drops.
                            auto produced = arrival;
                            gekko::u64 produced_ns = 0;
                            if (native_gpu != nullptr &&
                                native_gpu->output_produced_ns(arrived, produced_ns)) {
                                produced = std::chrono::steady_clock::time_point(
                                    std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                        std::chrono::nanoseconds(produced_ns)));
                            }
                            native_arrivals[native_arrival_cursor++ % native_arrivals.size()] = {
                                arrived, produced};
                        }
                        native_previous_generation = generation > 1u ? generation - 1u : 0u;
                        native_current_generation = generation;
                        native_transition_start = arrival;
                        last_native_arrival = arrival;
                        // D06-F3 (2026-09-02): on the shipped path this IS the
                        // frame arrival. The CPU publish slot below never
                        // advances when native XFB presentation is on
                        // (renderer.cpp stores an owed entry without
                        // resolving), so the dry-spell accounting hung off it
                        // could never arm and neither [host-stall]
                        // frame-production nor [frame-pacing] no GX work could
                        // ever fire windowed. Same spell bookkeeping, fed from
                        // the generation that actually moves.
                        account_spell_interval(arrival);
                        close_spell("");
                        last_frame_arrival = arrival;
                    }
                }

                {
                    // Only once the game has actually started producing frames.
                    const auto now = std::chrono::steady_clock::now();
                    if (native_current_generation != 0u) {
                        if (!spell_open && now - last_frame_arrival >= kFrameStallThreshold) {
                            spell_open = true;
                            spell_gx_active = {};
                            spell_no_gx = {};
                            spell_last_poll = last_frame_arrival;
                        }
                        account_spell_interval(now);
                    }
                }

                const auto render_now = std::chrono::steady_clock::now();
                const bool have_native_frame = native_current_generation != 0u;
                if (have_native_frame && render_clock.due(render_now) &&
                    render_clock.consume(render_now)) {
                    const auto active_source_period = native_source_period;
                    const double source_seconds = (std::max)(
                        1.0e-6, std::chrono::duration<double>(active_source_period).count());
                    const double source_hz = 1.0 / source_seconds;
                    // D06-F4: hysteresis, not a single threshold. An unlocked
                    // render clock (render_rate <= 0) presents as fast as it
                    // can, so the source is always behind it and the blend is
                    // always wanted - the ratio is meaningless there.
                    bool render_is_faster = true;
                    if (render_clock.target_hz() > 0.0) {
                        const double source_ratio = source_hz / render_clock.target_hz();
                        const bool engage = interpolation_engaged
                                                ? source_ratio < kInterpolationReleaseRatio
                                                : source_ratio < kInterpolationEngageRatio;
                        if (engage != interpolation_engaged) {
                            ++interpolation_toggles;
                            interpolation_engaged = engage;
                        }
                        render_is_faster = interpolation_engaged;
                    }
                    double native_blend_alpha = 1.0;
                    bool interpolation_requested = false;
                    constexpr bool have_cpu_pair = false;
                    const bool have_native_pair =
                        have_native_frame && native_previous_generation != 0u;
                    if (frame_interpolation && render_is_faster &&
                        (have_native_pair || have_cpu_pair)) {
                        const auto transition_start = native_transition_start;
                        const double alpha = std::clamp(
                            std::chrono::duration<double>(render_now - transition_start).count() /
                                source_seconds,
                            0.0, 1.0);
                        if (alpha <= 0.0) {
                            native_blend_alpha = 0.0;
                            interpolation_requested = true;
                        } else if (alpha < 1.0) {
                            native_blend_alpha = alpha;
                            interpolation_requested = true;
                        }
                    }

                    const auto present_start = std::chrono::steady_clock::now();
                    // The frame was rendered for render_content_aspect(layout),
                    // which is Hor+ clamped at 4:3 and never narrower. Fitting
                    // it to layout.aspect instead squeezed the image whenever
                    // the window was narrower than 4:3, where it should have
                    // been pillarboxed. Identical at 4:3 and every wider
                    // target, which is every default configuration.
                    const float content_aspect = gekko::render_content_aspect(
                        system.display().layout());
                    bool native_presented = false;
                    gekko::u64 presented_generation = 0;
                    // Arrival-order presentation for GPU-produced frames (the
                    // native renderer and the legacy native XFB). Exactly two
                    // unshown frames whose newer one arrived after the previous
                    // tick are a same-interval pair: show the older now and the
                    // newer next tick, instead of skipping one and repeating
                    // the next. Anything else (one new frame, a backlog of
                    // three or more, cross-fade requested, a lag held for
                    // kFifoMaxLagTicks) presents the newest. Host presentation only.
                    gekko::u64 present_target = native_current_generation;
                    if (native_gpu != nullptr || options.native_xfb_present) {
                        if (options.present_fifo && !interpolation_requested &&
                            last_presented_generation != 0u &&
                            native_current_generation == last_presented_generation + 2u) {
                            bool newer_arrived_this_interval = false;
                            for (const auto& entry : native_arrivals) {
                                if (entry.first == native_current_generation && last_tick_time &&
                                    entry.second > *last_tick_time) {
                                    newer_arrived_this_interval = true;
                                }
                            }
                            if (newer_arrived_this_interval) {
                                if (fifo_lag_ticks < kFifoMaxLagTicks) {
                                    present_target = last_presented_generation + 1u;
                                    ++fifo_deferred_presents;
                                } else {
                                    ++fifo_lag_drops;
                                }
                            }
                        }
                        fifo_lag_ticks =
                            present_target < native_current_generation ? fifo_lag_ticks + 1u : 0u;
                        last_tick_time = render_now;
                    }
                    if (native_gpu != nullptr) {
                        // Native renderer: present the arrival-order target. The
                        // presenter GPU-waits on the frame's ready semaphore on its
                        // own queue and hands the pin back when its blit retires.
                        gekko::NativeGpu::OutputFrame output;
                        bool acquired = native_gpu->acquire_output(present_target, output);
                        if (!acquired && present_target != native_current_generation) {
                            ++fifo_fallbacks;
                            fifo_lag_ticks = 0;
                            acquired = native_gpu->acquire_output(output);
                        } else if (!acquired) {
                            acquired = native_gpu->acquire_output(output);
                        }
                        if (acquired) {
                            const bool presenter_owns_pins = window.plume_pins_adopted();
                            gekko::PlumePresenter::NativePin source_pin;
                            source_pin.slot = output.slot;
                            source_pin.generation = output.generation;
                            source_pin.valid = true;
                            source_pin.ready_semaphore = output.ready_semaphore;
                            gekko::PlumePresenter::NativePin previous_pin;
                            native_presented = window.present_native(
                                output.texture, nullptr, 1.0f, static_cast<int>(output.width),
                                static_cast<int>(output.height), content_aspect, &source_pin,
                                &previous_pin);
                            if (native_presented) presented_generation = output.generation;
                            if (!presenter_owns_pins) {
                                native_gpu->release_output(output.slot, output.generation);
                            }
                        } else {
                            ++native_acquire_misses;
                        }
                    }
                    if (presenter_failed()) break;
                    const auto presented_at = std::chrono::steady_clock::now();
                    GEKKO_PROFILE_FRAME("Present");
                    present_ns += elapsed_ns(present_start, presented_at);

                    double frame_ms = 0.0;
                    if (last_present_time) {
                        frame_ms = std::chrono::duration<double, std::milli>(
                                       presented_at - *last_present_time)
                                       .count();
                        presentation_intervals_ms.push_back(frame_ms);
                        if (frame_ms > 33.4) {
                            ++present_gaps;
                            std::fprintf(stderr,
                                         "[present-gap] %.1f ms between presents at generation "
                                         "%llu\n",
                                         frame_ms,
                                         static_cast<unsigned long long>(presented_generation));
                        }
                    }
                    // Unique versus repeated presents. The interval vector above
                    // is the RE-PRESENT cadence (display-locked); the unique
                    // cadence is the guest's frame rate as actually delivered.
                    if (presented_generation != last_presented_generation) {
                        ++unique_presents;
                        if (last_unique_present_time) {
                            unique_present_hist.add(
                                elapsed_ns(*last_unique_present_time, presented_at));
                        }
                        last_unique_present_time = presented_at;
                        last_presented_generation = presented_generation;
                    } else {
                        ++repeat_presents;
                    }
                    // D06-F3: the xfb_generation / xfb_delta columns read the
                    // generation that actually advances. On the shipped
                    // windowed path that is the native one; the CPU publish
                    // slot stays at 0, so every row used to print 0 and 1.
                    const gekko::u64 logged_generation = native_current_generation;
                    if (performance_log) {
                        performance_log
                            << std::fixed << std::setprecision(6)
                            << std::chrono::duration<double>(presented_at - execution_start).count()
                            << ',' << logged_generation << ','
                            << (timed_xfb == 0u ? 1u : logged_generation - timed_xfb) << ','
                            << system.video().frames() << ','
                            << retired_published.load(std::memory_order_relaxed) << ',' << frame_ms
                            << ','
                            << static_cast<double>(execute_ns.load(std::memory_order_relaxed)) /
                                   1.0e6
                            << ','
                            << static_cast<double>(submit_ns.load(std::memory_order_relaxed)) /
                                   1.0e6
                            << ','
                            << static_cast<double>(retire_ns.load(std::memory_order_relaxed)) /
                                   1.0e6
                            << ','
                            << static_cast<double>(sleep_ns.load(std::memory_order_relaxed)) /
                                   1.0e6
                            << ',' << static_cast<double>(present_ns) / 1.0e6 << '\n';
                    }
                    timed_xfb = logged_generation;
                    last_present_time = presented_at;
                }

                // Sleep on either useful event: a new source frame or the next
                // render deadline. The 16 ms ceiling is solely for the Win32
                // message pump; there is no fixed 2/4 ms polling loop anymore.
                auto wait_duration = std::chrono::duration_cast<
                    std::chrono::steady_clock::duration>(std::chrono::milliseconds(16));
                const auto until_render =
                    render_clock.time_until_deadline(std::chrono::steady_clock::now());
                const bool have_presentation_frame = native_current_generation != 0u;
                if (have_presentation_frame && !render_clock.unlocked()) {
                    wait_duration = (std::min)(wait_duration, until_render);
                } else if (have_presentation_frame && render_clock.unlocked()) {
                    wait_duration = std::chrono::steady_clock::duration::zero();
                }
                if (wait_duration > std::chrono::steady_clock::duration::zero()) {
                    present_wait.sleep_until(std::chrono::steady_clock::now() + wait_duration);
                }
            }
#endif
            // A spell still open here was cut off by shutdown, not resolved -
            // the 2026-08-26 session ended inside a 5.84 s loading window and
            // the truncation was only provable from the telemetry.
            account_spell_interval(std::chrono::steady_clock::now());
            close_spell(", truncated by shutdown");
            // D06-F5 (2026-09-02): the presenter's scan-out counters, hoisted
            // for the exit report. The presenting thread is this one and its
            // loop has ended, so this is the last read and it needs no lock.
            game.join();
#if defined(SHADOW_RENDER_LIFTED)
            // The game thread is done with the scaffolding: the swap chain goes before its window.
            lifted_render.stop();
#endif
            window.destroy();
            presenter_stats = window.plume_present_stats();
            if (window.plume_failed()) {
                native_presenter_failed = true;
                if (presenter_error.empty()) {
                    try {
                        presenter_error = window.plume_error();
                        std::cerr << "ERROR: native presenter: " << presenter_error << '\n';
                    } catch (...) {
                        std::fputs("ERROR: native presenter diagnostic snapshot failed\n", stderr);
                    }
                }
            }
            // Every port's motor stops with the session (ports 1-3 are XInput
            // controllers 1-3).
            for (unsigned port = 0; port < 4u; ++port) gekko::set_xinput_rumble(port, false);
            // Hoist the pacer's headroom split out of this scope for the exit
            // report. Safe here and only here: the writer has been joined.
            pacing_paced_services = paced_services.load(std::memory_order_relaxed);
            pacing_unpaced_services = unpaced_services.load(std::memory_order_relaxed);
            pacing_resyncs = simulation_clock.stats().rebases;
            pacing_debt_clamps = simulation_clock.stats().debt_clamps;
            pacing_boundaries = simulation_clock.stats().boundaries;
            pacing_skipped_ticks = simulation_clock.stats().skipped_ticks;
            pacing_maximum_debt_ms =
                static_cast<double>(simulation_clock.stats().maximum_observed_debt.count()) /
                1.0e6;
            presentation_summary.target_fps = render_clock.target_hz();
            presentation_summary.interpolation = frame_interpolation;
            presentation_summary.blend_presents = blend_presents;
            presentation_summary.interpolation_toggles = interpolation_toggles;
            presentation_summary.native_previous_misses = native_previous_misses;
            presentation_summary.native_acquire_misses = native_acquire_misses;
            presentation_summary.present_fifo = options.present_fifo;
            presentation_summary.fifo_deferred_presents = fifo_deferred_presents;
            presentation_summary.fifo_lag_drops = fifo_lag_drops;
            presentation_summary.fifo_fallbacks = fifo_fallbacks;
            presentation_summary.rendered_frames = render_clock.stats().frames;
            presentation_summary.dropped_render_deadlines =
                render_clock.stats().dropped_deadlines;
            system.set_host_service({});
            system.set_host_pacing({});
            if (game_exception) {
                // The machine state that explains the failure, before the
                // exception unwinds the session (the report and JSON are only
                // written on a clean exit). Never let it mask the real error.
                try {
                    std::fprintf(stderr, "[abort-diagnosis]\n%s\n%s",
                                 system.stop_report().c_str(), system.progress_report().c_str());
                    std::fflush(stderr);
                } catch (...) {
                }
                std::rethrow_exception(game_exception);
            }


            const double execution_seconds_for_report =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - execution_start)
                    .count();
            if (!presentation_intervals_ms.empty()) {
                std::vector<double> sorted = presentation_intervals_ms;
                std::sort(sorted.begin(), sorted.end());
                double total_ms = 0.0;
                for (double value : sorted) total_ms += value;
                const std::size_t worst_count = (std::max)(
                    static_cast<std::size_t>(1),
                    static_cast<std::size_t>(static_cast<double>(sorted.size()) * 0.01));
                double worst_sum = 0.0;
                for (std::size_t index = sorted.size() - worst_count; index < sorted.size();
                     ++index) {
                    worst_sum += sorted[index];
                }
                const double average_fps =
                    total_ms > 0.0 ? static_cast<double>(sorted.size()) * 1000.0 / total_ms : 0.0;
                const double minimum_fps = sorted.back() > 0.0 ? 1000.0 / sorted.back() : 0.0;
                const double one_percent_low =
                    worst_sum > 0.0 ? 1000.0 / (worst_sum / static_cast<double>(worst_count))
                                    : 0.0;
                presentation_summary.available = true;
                presentation_summary.intervals = sorted.size();
                {
                    const auto at = [&](double p) {
                        const std::size_t index = static_cast<std::size_t>(
                            p * static_cast<double>(sorted.size() - 1u));
                        return sorted[index];
                    };
                    presentation_summary.p50_ms = at(0.50);
                    presentation_summary.p95_ms = at(0.95);
                    presentation_summary.p99_ms = at(0.99);
                    presentation_summary.p999_ms = at(0.999);
                    presentation_summary.max_ms = sorted.back();
                }
                presentation_summary.average_fps = average_fps;
                presentation_summary.minimum_fps = minimum_fps;
                presentation_summary.one_percent_low = one_percent_low;
                presentation_summary.execute_s =
                    static_cast<double>(execute_ns.load()) / 1.0e9;
                presentation_summary.submit_s =
                    static_cast<double>(submit_ns.load()) / 1.0e9;
                presentation_summary.retire_s =
                    static_cast<double>(retire_ns.load()) / 1.0e9;
                presentation_summary.sleep_s =
                    static_cast<double>(sleep_ns.load()) / 1.0e9;
                presentation_summary.present_s = static_cast<double>(present_ns) / 1.0e9;
                presentation_summary.game_thread_stalls = guest_stalls.load();
                presentation_summary.longest_game_stall_ms =
                    static_cast<double>(longest_guest_stall_ns.load()) / 1.0e6;
                presentation_summary.frame_production_stalls = frame_stalls;
                presentation_summary.longest_frame_stall_ms = longest_frame_stall_ms;
                presentation_summary.no_gx_spells = no_gx_spells;
                presentation_summary.longest_no_gx_ms = longest_no_gx_ms;
                std::cout << "Host presentation timing: " << sorted.size()
                          << " intervals, average " << average_fps << " FPS, minimum "
                          << minimum_fps << " FPS, 1% low " << one_percent_low << " FPS\n"
                          << "  present intervals: p50 " << presentation_summary.p50_ms
                          << " ms, p95 " << presentation_summary.p95_ms << " ms, p99 "
                          << presentation_summary.p99_ms << " ms, p99.9 "
                          << presentation_summary.p999_ms << " ms, max "
                          << presentation_summary.max_ms << " ms; " << present_gaps
                          << " gaps over 33.4 ms\n"
                          << "  unique frames: " << unique_presents << " presented ("
                          << (execution_seconds_for_report > 0.0
                                  ? static_cast<double>(unique_presents) /
                                        execution_seconds_for_report
                                  : 0.0)
                          << "/s), " << repeat_presents << " re-presents; unique interval "
                          << histogram_text(unique_present_hist) << "\n"
                          << "  frame interpolation: "
                          << (options.frame_interpolation ? "on" : "off") << ", "
                          << blend_presents << " cross-faded presents, "
                          << interpolation_toggles << " engage/release edges, "
                          << native_previous_misses << " previous-generation misses, "
                          << native_acquire_misses << " native acquire misses\n"
                          << "  arrival-order presentation: "
                          << (options.present_fifo ? "on" : "off") << ", "
                          << fifo_deferred_presents << " deferred, " << fifo_lag_drops
                          << " lag drops, " << fifo_fallbacks << " fallbacks\n"
                          << "  game thread: execute "
                          << static_cast<double>(execute_ns.load()) / 1.0e9 << " s, submit "
                          << static_cast<double>(submit_ns.load()) / 1.0e9 << " s, XFB retire "
                          << static_cast<double>(retire_ns.load()) / 1.0e9
                          << " s, recovered-clock sleep "
                          << static_cast<double>(sleep_ns.load()) / 1.0e9 << " s over "
                          << service_calls.load() << " host services\n"
                          // The headroom split. A large sleep total with a
                          // small paced fraction means the sleep was banked in
                          // loads and menus, not in the phase that sets the
                          // frame rate - the two cannot be told apart from the
                          // lifetime sleep total alone.
                          << "  fixed simulation clock: " << paced_services.load()
                          << " quanta slept (guest ahead), " << unpaced_services.load()
                          << " quanta ran on (guest at or behind real time, "
                          << (paced_services.load() + unpaced_services.load() != 0
                                  ? 100.0 * static_cast<double>(unpaced_services.load()) /
                                        static_cast<double>(paced_services.load() +
                                                            unpaced_services.load())
                                  : 0.0)
                          << "%); render clock "
                          << (options.render_rate <= 0.0 ? "unlocked" :
                                  std::to_string(options.render_rate) + " Hz")
                          << (options.frame_interpolation ? ", interpolation on\n" :
                                                                  ", interpolation off\n")
                          << "  UI thread: present " << static_cast<double>(present_ns) / 1.0e9
                          << " s\n"
                          << "  stalls: " << guest_stalls.load() << " on the game thread (longest "
                          << static_cast<double>(longest_guest_stall_ns.load()) / 1.0e6
                          << " ms), " << frame_stalls
                          << " frame-production with GX active (longest "
                          << longest_frame_stall_ms << " ms), " << no_gx_spells
                          << " no-GX-work spells (longest " << longest_no_gx_ms << " ms)\n";
            }
            if (options.present_plume) {
                const auto report = window.plume_verify_report();
                std::cout << "Presentation transport: " << report.native_xfb_frames
                          << " native GPU XFB frames, " << report.cpu_upload_frames
                          << " CPU-upload frames\n";
            }
            if (options.verify_plume) {
                const auto report = window.plume_verify_report();
                char row[256];
                std::snprintf(row, sizeof(row),
                              "Presented frames verified against the back buffer: %llu, "
                              "%llu differed, %llu pixels differed\n",
                              static_cast<unsigned long long>(report.frames),
                              static_cast<unsigned long long>(report.mismatched_frames),
                              static_cast<unsigned long long>(report.mismatched_pixels));
                std::cout << row;
                if (report.skipped != 0) {
                    std::snprintf(row, sizeof(row),
                                  "  WARNING: %llu presents were NOT verified (staging was not "
                                  "1:1, so the oracle does not cover them). This run is "
                                  "unverified.\n",
                                  static_cast<unsigned long long>(report.skipped));
                    std::cout << row;
                }
                if (report.mismatched_pixels != 0) {
                    std::snprintf(row, sizeof(row),
                                  "  first difference at %u,%u: expected %08X, read back %08X\n",
                                  report.first_bad_x, report.first_bad_y,
                                  report.first_bad_expected, report.first_bad_actual);
                    std::cout << row;
                }
            }
        }
    } else {
        // The headless host service (every 5 ms of game time): the budget, the
        // development UI driver, the observation-only diagnostics. The UI
        // driver latches ordinary controller samples into the peripheral layer
        // exactly where the windowed loop latches the keyboard and XInput
        // pads, and paces itself on VI fields so it follows the game's own
        // frame rate. The callbacks capture state declared at THIS scope. Declared inside the branch, the callback
        // dereferenced a dead frame (segfault with the 2026-09-05 ledger; the
        // older SHADOW_STALL_WATCH state had the same latent defect).
        // OBSERVATION-ONLY stall watchdog, inert unless SHADOW_STALL_WATCH
        // is set. The host service runs inside translated guest code, so
        // this reads guest state and never writes any of it.
        //
        // It answers the one question a counter cannot: when GX submission
        // stops while game time keeps advancing, WHERE is the guest? It
        // samples cpu.pc/lr and the volatile registers every service once no
        // new primitive has been submitted for a threshold of game time
        // (SHADOW_STALL_WATCH=<game ms>), which is exactly the freeze signature.
        struct StallWatch {
            bool enabled = false;
            gekko::u64 threshold = 0;
            gekko::u64 last_progress = 0;
            gekko::u64 last_triangles = 0;
            gekko::u64 reports = 0;
        } stall_watch;
        if (const char* watch = std::getenv("SHADOW_STALL_WATCH")) {
            stall_watch.enabled = true;
            stall_watch.threshold =
                std::strtoull(watch, nullptr, 0) * (gekko::kGameTicksPerSecond / 1000u);
            if (stall_watch.threshold == 0ull) stall_watch.threshold = gekko::kGameTicksPerSecond / 10u;
        }
        // Forward-progress ledger: SHADOW_PROGRESS_LEDGER=<game ms step> prints
        // one row per step with what the guest DID (new call targets, XFB
        // copies, VI fields, interrupts, context switches, distinct sampled pc
        // / state, stack range) next to what the host spent (wall, this
        // thread's CPU). Sampled at the host service.
        struct ProgressLedger {
            bool enabled = false;
            gekko::u64 step = 0;
            gekko::u64 next = 0;
            gekko::u64 rows = 0;
            std::set<gekko::u32> pc_buckets;
            std::set<gekko::u64> states;
            gekko::u32 r1_min = 0xffffffffu, r1_max = 0;
            gekko::u64 last_ticks = 0;
            gekko::u64 last_xfb = 0, last_fields = 0, last_ext = 0;
            gekko::u64 last_sc = 0, last_rfi = 0, last_yield = 0, last_switch = 0;
            gekko::u64 last_entries = 0, last_exits = 0;
            std::chrono::steady_clock::time_point last_wall;
            gekko::u64 last_cpu_100ns = 0;
        } ledger;
        if (const char* step = std::getenv("SHADOW_PROGRESS_LEDGER")) {
            ledger.enabled = true;
            ledger.step = std::strtoull(step, nullptr, 0) * (gekko::kGameTicksPerSecond / 1000u);
            if (ledger.step == 0ull) ledger.step = gekko::kGameTicksPerSecond;
            ledger.next = system.game_ticks() + ledger.step;
            ledger.last_ticks = system.game_ticks();
            ledger.last_wall = std::chrono::steady_clock::now();
            std::printf("[progress] step %llu game ticks; columns: game_ms "
                        "| new_call_targets xfb_copies vi_fields ext_int sc rfi yield "
                        "ctx_switch entries exits | pc_buckets states r1_range | wall_ms cpu_ms\n",
                        static_cast<unsigned long long>(ledger.step));
        }
        // Headless replays for the workstation (docs/comparator/README.md "Recreating a frame"):
        // --save-state-at-tick saves at the boundary after each tick and runs on; --capture-at-tick writes the first
        // frame finished at or after its tick, armed before the first instruction so the renderer draws every frame
        // that feeds it (a view's history); --stop-after-capture ends the run at the boundary after it is written.
        std::size_t next_mid_save = 0;
        bool mid_save_pending = false;
        gekko::u64 mid_save_request_tick = 0;
        bool capture_armed = false;
        gekko::u64 capture_baseline = 0;
        std::filesystem::path capture_expected;
        if (options.capture_at_tick) {
            capture_baseline = dev_capture_result().written;
            capture_expected = dev_arm_capture_at_tick(options.capture_at_tick->second, options.capture_at_tick->first,
                                                       static_cast<gekko::u32>(display.window_width),
                                                       static_cast<gekko::u32>(display.window_height));
            capture_armed = true;
            std::cout << "[capture-at-tick] the first frame finished at or after game tick "
                      << options.capture_at_tick->first << " -> " << capture_expected.string() << "\n";
        }
        struct UiTestDriver {
            gekko::System* system = nullptr;
            gekko::UiTestScript* script = nullptr;
            gekko::u32 last_frame = 0xffffffffu;
            gekko::u64 published = 0;
        } ui_driver;
        ui_driver.system = &system;
        ui_driver.script = &ui_test_script;
#if defined(SHADOW_RENDER_LIFTED)
        start_game_output(headless_output_w, headless_output_h, headless_output_source);
#endif
        system.set_host_service(
                [&](gekko::u64 game_ticks) {
                    if (budget_due(game_ticks)) {
                        budget_reached = true;
                        system.cpu().request_stop(gekko::StopReason::HostRequest, "budget reached");
                        return;
                    }
                    if (gekko::MeasureProbes* probes = gekko::MeasureProbes::active()) probes->on_host_service();
                    game_thread_placement.sample();
                    if (ui_test_active) {
                        const gekko::u32 frame = static_cast<gekko::u32>(ui_driver.system->video().frames());
                        const std::string* entered = nullptr;
                        const shadow::peripheral::ControllerSample sample =
                            ui_driver.script->sample_for_frame(frame, &entered);
                        if (entered != nullptr) {
                            std::cout << "[ui-test] VI " << frame << ": " << *entered << "\n";
                            std::cout.flush();
                        }
                        if (frame != ui_driver.last_frame) {
                            ui_driver.last_frame = frame;
                            ++ui_driver.published;
                        }
                        ui_driver.system->peripheral().latch(0, sample);
                    }
                    // Input tapes (gekko/input_tape.h): a tape being played latches its samples here, where the
                    // windowed run latched them; a recording keeps what was latched.
                    if (input_tape.playing() || tape_recorder != nullptr) {
                        gekko::InputTape::Samples pads{};
                        for (std::size_t port = 0; port < pads.size(); ++port) {
                            pads[port] = system.peripheral().sample(port);
                        }
                        if (input_tape.playing()) {
                            input_tape.samples_at(game_ticks, pads);
                            for (std::size_t port = 0; port < pads.size(); ++port) {
                                system.peripheral().latch(port, pads[port]);
                            }
                        }
                        if (tape_recorder != nullptr) tape_recorder->observe(game_ticks, pads);
                    }
#if defined(SHADOW_RENDER_LIFTED)
                    latch_game_output(game_ticks);
#endif
                    // --stop-at-tick: the first host service at or after the tick asks for the boundary (as the
                    // windowed run's development link did when the checkpoint was made), and the run ends there.
                    if (options.stop_at_tick != 0u && !stop_at_requested && game_ticks >= options.stop_at_tick) {
                        stop_at_requested = true;
                        stop_at_request_tick = game_ticks;
                        budget_reached = true;
                        system.request_stop_between_traversals(gekko::StopReason::HostRequest, "stop-at-tick");
                    }
                    // --save-state-at-tick: the first host service at or after each tick asks for the boundary.
                    if (!mid_save_pending && next_mid_save < options.save_state_at_ticks.size() &&
                        game_ticks >= options.save_state_at_ticks[next_mid_save].first) {
                        mid_save_pending = true;
                        mid_save_request_tick = game_ticks;
                        system.request_stop_between_traversals(gekko::StopReason::HostRequest, "save-state-at-tick");
                    }
                    // --stop-after-capture: the frame is written - the run ends at the next boundary.
                    if (options.stop_after_capture && capture_armed && !budget_reached &&
                        dev_capture_result().written > capture_baseline) {
                        budget_reached = true;
                        system.request_stop_between_traversals(gekko::StopReason::HostRequest, "capture written");
                    }
                    if (ledger.enabled) {
                        const gekko::Cpu& cpu = system.cpu();
                        ledger.pc_buckets.insert(cpu.pc >> 5);
                        ledger.states.insert((static_cast<gekko::u64>(cpu.pc) << 32) ^
                                             (static_cast<gekko::u64>(cpu.lr) << 8) ^
                                             cpu.gpr[1]);
                        ledger.r1_min = (std::min)(ledger.r1_min, cpu.gpr[1]);
                        ledger.r1_max = (std::max)(ledger.r1_max, cpu.gpr[1]);
                        if (game_ticks >= ledger.next) {
                            // Frames the guest published to the native renderer.
#if !defined(SHADOW_RENDER_LIFTED)  // G5
                            const gekko::u64 xfb = system.native_capture() != nullptr
                                                       ? system.native_capture()->report().presents
                                                       : 0u;
#else
                            const gekko::u64 xfb = 0u;  // G5: no legacy capture in the lifted build
#endif
                            const gekko::u64 fields = system.video().frames();
                            const gekko::CpuDiagCounters& diag = gekko::cpu_diag();
                            const gekko::u64 ext = diag.exceptions_by_vector[5];
                            const gekko::u64 sc = cpu.fallback_total(gekko::FallbackReason::SystemCall);
                            const gekko::u64 rfi =
                                cpu.fallback_total(gekko::FallbackReason::ReturnFromInterrupt);
                            const gekko::u64 yield = cpu.fallback_total(gekko::FallbackReason::Yield);
                            const gekko::u64 sw = diag.fpr_restore_skipped_context_switch;
                            const auto now = std::chrono::steady_clock::now();
                            gekko::u64 cpu_100ns = 0;
#ifdef _WIN32
                            {
                                FILETIME c, e, k, u;
                                if (GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u) != FALSE) {
                                    const auto ft = [](const FILETIME& f) {
                                        return (static_cast<gekko::u64>(f.dwHighDateTime) << 32) |
                                               f.dwLowDateTime;
                                    };
                                    cpu_100ns = ft(k) + ft(u);
                                }
                            }
#endif
                            std::printf("[progress] %10.3f | %5llu %5llu %5llu %5llu %5llu %5llu %5llu %6llu %6llu | %6zu %6zu %08X-%08X | %8.2f %8.2f\n",
                                        static_cast<double>(game_ticks - ledger.last_ticks) * 1000.0 /
                                            static_cast<double>(gekko::kGameTicksPerSecond),
                                        static_cast<unsigned long long>(xfb - ledger.last_xfb),
                                        static_cast<unsigned long long>(fields - ledger.last_fields),
                                        static_cast<unsigned long long>(ext - ledger.last_ext),
                                        static_cast<unsigned long long>(sc - ledger.last_sc),
                                        static_cast<unsigned long long>(rfi - ledger.last_rfi),
                                        static_cast<unsigned long long>(yield - ledger.last_yield),
                                        static_cast<unsigned long long>(sw - ledger.last_switch),
                                        static_cast<unsigned long long>(cpu.recompiled_calls - ledger.last_entries),
                                        static_cast<unsigned long long>(cpu.interpreter_exits - ledger.last_exits),
                                        ledger.pc_buckets.size(), ledger.states.size(),
                                        ledger.r1_min, ledger.r1_max,
                                        std::chrono::duration<double, std::milli>(now - ledger.last_wall).count(),
                                        static_cast<double>(cpu_100ns - ledger.last_cpu_100ns) / 1.0e4);
                            std::fflush(stdout);
                            ledger.last_ticks = game_ticks;
                            ledger.last_xfb = xfb;
                            ledger.last_fields = fields;
                            ledger.last_ext = ext;
                            ledger.last_sc = sc;
                            ledger.last_rfi = rfi;
                            ledger.last_yield = yield;
                            ledger.last_switch = sw;
                            ledger.last_entries = cpu.recompiled_calls;
                            ledger.last_exits = cpu.interpreter_exits;
                            ledger.last_wall = now;
                            ledger.last_cpu_100ns = cpu_100ns;
                            ledger.pc_buckets.clear();
                            ledger.states.clear();
                            ledger.r1_min = 0xffffffffu;
                            ledger.r1_max = 0;
                            ledger.next += ledger.step;
                            ++ledger.rows;
                        }
                    }
                    if (!stall_watch.enabled) return;
                    // Is MSR[FP] a live bit in this port, or is it stuck off?
                    // The answer decides whether the guest is relying on lazy FPU
                    // context switching or whether the bit is simply never
                    // restored. Sampled once per service tick.
                    {
                        static gekko::u64 fp_on = 0, fp_off = 0;
                        static int last = -1;
                        static gekko::u64 transitions = 0;
                        const int now = (system.cpu().msr() & 0x2000u) != 0u ? 1 : 0;
                        if (now != 0) ++fp_on; else ++fp_off;
                        if (last != -1 && now != last) {
                            ++transitions;
                            if (transitions <= 6ull) {
                                std::fprintf(stderr,
                                             "[msr-fp] transition #%llu -> FP=%d at ticks=%llu "
                                             "pc=0x%08X (on=%llu off=%llu)\n",
                                             static_cast<unsigned long long>(transitions), now,
                                             static_cast<unsigned long long>(game_ticks),
                                             system.cpu().pc,
                                             static_cast<unsigned long long>(fp_on),
                                             static_cast<unsigned long long>(fp_off));
                                std::fflush(stderr);
                            }
                        }
                        last = now;
                        if (((fp_on + fp_off) % 20000ull) == 0ull) {
                            std::fprintf(stderr, "[msr-fp] ticks on=%llu off=%llu transitions=%llu\n",
                                         static_cast<unsigned long long>(fp_on),
                                         static_cast<unsigned long long>(fp_off),
                                         static_cast<unsigned long long>(transitions));
                            std::fflush(stderr);
                        }
                    }
                    const gekko::u64 retired_now = game_ticks;
                    // Draw progress: what the guest submitted to the renderer
                    // that is running.
                    gekko::u64 triangles = 0;
#if !defined(SHADOW_RENDER_LIFTED)  // G5
                    if (const gekko::RwCapture* native = system.native_capture()) {
                        const gekko::RwCapture::Report drawn = native->report();
                        triangles = drawn.immediate_draws + drawn.mesh_draws;
                    }
#endif
                    if (triangles != stall_watch.last_triangles) {
                        stall_watch.last_triangles = triangles;
                        stall_watch.last_progress = retired_now;
                        return;
                    }
                    // Only a stall AFTER the stage has started drawing is the
                    // signature we are after; before the first triangle there is
                    // nothing to have stalled.
                    if (triangles == 0ull) {
                        stall_watch.last_progress = retired_now;
                        return;
                    }
                    if (retired_now - stall_watch.last_progress < stall_watch.threshold) return;
                    if (stall_watch.reports >= 24ull) return;
                    ++stall_watch.reports;
                    const gekko::Cpu& cpu = system.cpu();
                    std::fprintf(stderr,
                                 "[stall] #%llu ticks=%llu quiet_for=%llu tri=%llu "
                                 "pc=0x%08X lr=0x%08X ctr=0x%08X cr=0x%08X msr=0x%08X "
                                 "fpr_ctx_switches=%llu\n",
                                 static_cast<unsigned long long>(stall_watch.reports),
                                 static_cast<unsigned long long>(retired_now),
                                 static_cast<unsigned long long>(retired_now -
                                                                 stall_watch.last_progress),
                                 static_cast<unsigned long long>(triangles), cpu.pc, cpu.lr,
                                 cpu.ctr, cpu.cr, cpu.msr(),
                                 static_cast<unsigned long long>(
                                     cpu.interrupt_fpr_context_switches()));
                    for (int base = 0; base < 32; base += 8) {
                        std::fprintf(stderr, "        r%-2d", base);
                        for (int i = 0; i < 8; ++i) {
                            std::fprintf(stderr, " %08X", cpu.gpr[base + i]);
                        }
                        std::fprintf(stderr, "\n");
                    }
                    // The wrap loop at 0x804349F8 reads its operand from r4+0x18
                    // and its constants from f1..f4. Print both as bit patterns,
                    // because the interesting failures here are exact float
                    // values (a huge finite magnitude, a zero step) rather than
                    // NaN or Inf.
                    std::fprintf(stderr, "        f0..f4 ps0:");
                    for (int i = 0; i < 5; ++i) {
                        double value = 0.0;
                        std::memcpy(&value, &cpu.fpr[i].bits[0], sizeof(value));
                        std::fprintf(stderr, " f%d=%.9g(%016llX)", i, value,
                                     static_cast<unsigned long long>(cpu.fpr[i].bits[0]));
                    }
                    std::fprintf(stderr, "\n");
                    {
                        gekko::Cpu& mutable_cpu = system.cpu();
                        const gekko::u32 rot_bits = mutable_cpu.mem_read32(cpu.gpr[4] + 0x18u);
                        float rot = 0.0f;
                        std::memcpy(&rot, &rot_bits, sizeof(rot));
                        std::fprintf(stderr,
                                     "        node=0x%08X rotation=%.9g (0x%08X) "
                                     "next=0x%08X\n",
                                     cpu.gpr[4], static_cast<double>(rot), rot_bits,
                                     mutable_cpu.mem_read32(cpu.gpr[4] + 0x68u));
                    }
                    std::fflush(stderr);
                });
        GEKKO_PROFILE_THREAD("game");
        gekko::name_this_thread("game (headless main)");  // ES-T1
        gekko::SelfSampler self_sampler;  // GEKKO_SELF_SAMPLE (diagnostic; off unless set)
        // R8 (ES 2026-10-20): the same game-thread policy as the windowed game thread, on the thread that executes
        // translated code here, so headless timings are taken under the policy the shipped configuration runs with.
        game_thread_policy = apply_game_thread_performance_policy();
        // This thread is the one that executes translated code on the headless
        // path, so its own CPU time bounds how much of the run was execution
        // and how much was waiting. Two syscalls per run, outside any loop.
        const double guest_cpu_start = thread_cpu_seconds_now();
        const auto guest_wall_start = std::chrono::steady_clock::now();
        for (;;) {
            system.run();
            // A --save-state-at-tick boundary: its savestate, then on (as the windowed N key's boundary).
            bool saved_mid = false;
            if (mid_save_pending) {
                mid_save_pending = false;
                const std::pair<gekko::u64, std::filesystem::path>& want = options.save_state_at_ticks[next_mid_save++];
                std::string mid_error;
                if (gekko::save_state(system, want.second, &mid_error)) {
                    std::cout << "[save-state-at-tick] wrote " << want.second.string() << " at game tick "
                              << system.game_ticks() << " (asked at " << mid_save_request_tick << " for "
                              << want.first << ")\n";
                    saved_mid = true;
                } else {
                    std::cerr << "[save-state-at-tick] " << mid_error << "\n";
                }
            }
            if (!saved_mid || budget_reached) break;
            system.cpu().clear_stop();
        }
        if (budget_reached) system.cpu().clear_stop();
        g_guest_run_wall_seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - guest_wall_start)
                .count();
        g_guest_run_cpu_seconds = thread_cpu_seconds_now() - guest_cpu_start;
        system.set_host_service({});
    }
    audio_runtime.finish_production();  // AU-T1: guest execution has ended; later device silence is the run's tail
    if (gekko::PrsAhead* ahead = system.prs_ahead()) {
        const gekko::PrsAhead::Stats s = ahead->stats();
        std::cout << "[prs-ahead] archives " << s.archives << " members queued " << s.queued << " decoded "
                  << s.decoded << " (not qualified " << s.unqualified << ") taken " << s.taken << " not ready "
                  << s.not_ready << " dropped " << s.dropped << " key matched but refused " << s.refused
                  << "; expand calls served " << system.game_memory().prs_ahead_served() << " refused "
                  << system.game_memory().prs_ahead_refused() << "\n";
    }
    if (gekko::NativeFileApi* files = system.file_api()) {
        const gekko::NativeFileApi::Stats stats = files->stats();
        std::cout << "[file-api] opens " << stats.opens << " (missed " << stats.open_misses
                  << ", directories " << stats.open_directories << ") closes " << stats.closes
                  << " cancels " << stats.cancels << " reads " << stats.reads << " ("
                  << stats.read_bytes << " B, " << stats.identity_reads << " identity) callbacks "
                  << stats.callbacks << " status " << stats.status_queries << " failed "
                  << stats.reads_failed << " refused "
                  << stats.reads_refused
                  // Both MUST be zero. A non-zero count is proof that some guest
                  // code derives a read token from something other than an
                  // opened file - which the injective token space turns from
                  // silent aliasing into this number.
                  << " unmapped " << stats.reads_unmapped
                  << " out-of-range " << stats.reads_out_of_range << " anomalies " << stats.anomalies
                  << " read-ms " << static_cast<double>(stats.read_ns) / 1.0e6 << " worst-ms "
                  << static_cast<double>(stats.read_ns_max) / 1.0e6 << "\n";
    }
    if (options.measure.count("r3") != 0u) {
        gekko::GameplayScheduler* fp_scheduler = system.gameplay_scheduler();
        if (fp_scheduler != nullptr && fp_scheduler->fp_probe_enabled()) {
            std::cout << fp_scheduler->fp_probe_summary();
            measure_results.emplace_back("r3", fp_scheduler->fp_probe_json());
        } else {
            measure_results.emplace_back("r3", "{\"item\":\"r3\",\"installed\":false,\"scheduler\":\"legacy\",\"reason\":\"--legacy-scheduler: native scheduler not constructed\",\"compare_with\":\"r13 FP-unavailable (vector 0x800) exceptions keyed by SRR0 at 0x8004ECB8,0x8004ED24,0x8004ED80,0x8004ED94,0x8004EDA8,0x8004EDD8 (fn_8004ECAC_body guards)\"}");
        }
    }
    if (gekko::GameplayScheduler* scheduler = system.gameplay_scheduler()) {
        const gekko::GameplayScheduler::Stats stats = scheduler->stats();
        std::cout << "[gameplay-scheduler] traversals " << stats.traversals << " levels "
                  << stats.levels << " (peak " << stats.level_peak << ", nested "
                  << stats.nested_peak << ") visits " << stats.nodes_visited << " executes "
                  << stats.executes << " instrumented " << stats.instrumented << " destroys "
                  << stats.destroys << " marks " << stats.marks << " paused-skips "
                  << stats.paused_skips << " yields " << stats.yields << " faults "
                  << stats.faults
                  << " traversal-storage " << stats.traversal_storage_created << " created "
                  << stats.traversal_storage_reused << " reused (peak "
                  << stats.traversal_storage_peak << ")\n";
    }
    bool native_renderer_failed = false;
    std::string native_renderer_error;
#if !defined(SHADOW_RENDER_LIFTED)  // G5
    native_renderer.stop();
    native_renderer_failed = native_renderer.failed();
    try { native_renderer_error = native_renderer.last_error(); }
    catch (...) {
        native_renderer_failed = true;
        std::fputs("ERROR: native renderer diagnostic snapshot failed\n", stderr);
    }
#endif
    if (!native_renderer_error.empty())
        std::cerr << "ERROR: native renderer: " << native_renderer_error << '\n';
    if (system.gx_sync().input_digest_enabled()) {
        const auto& sync = system.gx_sync().stats();
        char digest[32];
        std::snprintf(digest, sizeof(digest), "0x%016llX",
                      static_cast<unsigned long long>(system.gx_sync().input_digest()));
        std::cout << "[gx-inputs] tokens " << sync.tokens << " (" << sync.token_interrupts
                  << " with an interrupt request) digest " << digest << " draw-done " << sync.draw_dones << "\n";
    }
#if !defined(SHADOW_RENDER_LIFTED)  // G5
    if (system.native_capture() != nullptr) {
        std::cout << system.native_capture()->report_text();
        if (system.native_capture()->coherence_probe_enabled()) {
            std::cout << system.native_capture()->coherence_summary();
            measure_results.emplace_back("r4", system.native_capture()->coherence_json());
        }
    }
#endif
#if defined(SHADOW_RENDER_LIFTED)
    lifted_render.stop();
    std::cout << lifted_render.report_text();
#else
    std::cout << native_renderer.report_text();
#endif
    if (native_assets != nullptr) {
        native_assets->stop();
        gekko::NativeAssetManager::set_active(nullptr);
        const gekko::NativeAssetManager::Stats stats = native_assets->stats();
        std::cout << "[native-assets] files notified " << stats.files_notified << " parsed "
                  << stats.files_parsed << " ones " << stats.ones_parsed << " txds "
                  << stats.txds_parsed << " textures " << stats.textures_registered
                  << " failed " << stats.textures_failed << " lookups " << stats.lookups
                  << " hits " << stats.lookups_hit << " evictions " << stats.evictions
                  << " resident " << stats.resident_bytes << " B budget " << stats.cache_budget_bytes << " B pinned "
                  << stats.pinned_textures << " pinned hits " << stats.pinned_hits << "\n";
        std::cout << "[native-execution] workers " << stats.execution_workers << " graphs "
                  << stats.execution_graphs << " tasks " << stats.execution_tasks
                  << " worker tasks " << stats.execution_worker_tasks << " failures "
                  << stats.execution_failures << "\n";
    }
    {
        const gekko::button_glyphs::ButtonGlyphs::Report& buttons = system.button_glyphs().report();
        std::cout << "[buttons] " << gekko::button_glyphs::family_name(buttons.family) << " patched reads "
                  << buttons.patched_reads << " sharp sheets served " << buttons.sharp_served << "\n";
    }
    std::cout << shadow::memory::system().report();
    {
        // docs/memory-architecture-20261006: the game's address window, region by region.
        const gekko::GuestMemory& memory = system.guest_memory();
        std::cout << "[guest-memory] " << memory.regions().size() << " regions, " << memory.committed_bytes()
                  << " bytes committed of " << memory.reserved_bytes() << " reserved\n";
        for (const gekko::GuestMemory::Region& region : memory.regions()) {
            char row[192];
            std::snprintf(row, sizeof(row), "[guest-memory]   0x%08X..0x%08X %-24s %s, %u bytes committed\n",
                          region.base, static_cast<unsigned>(region.base + region.size), region.name.c_str(),
                          gekko::GuestMemory::kind_name(region.kind), region.committed);
            std::cout << row;
        }
    }
    {
        const auto ax = system.ax_mixer_report();
        if (ax.enabled) {
            std::cout << "AX voice advance: " << ax.frames << " frames, " << ax.voice_advances
                      << " voice advances, " << ax.loops << " loop wraps, " << ax.finished
                      << " one-shots finished, " << ax.skipped_insane
                      << " skipped on an out-of-range window\n";
            std::cout << "AX mix: " << ax.lists << " command lists (" << ax.lists_unparsed
                      << " without an output buffer), " << ax.mixed_frames
                      << " frames written from " << ax.mixed_voices << " voice-frames, "
                      << ax.adpcm_voices << " ADPCM source-frames decoded, peak "
                      << ax.peak << "\n";
            std::cout << "AX muted source history: " << ax.muted_adpcm_voices
                      << " ADPCM voice-frames, " << ax.muted_adpcm_samples_decoded
                      << " source samples decoded (main output accumulation skipped)\n";
            // Both of these run inline on the game thread, so their cost comes
            // straight out of the frame budget. Reported per AX frame as well
            // as in total, because "should audio move to a worker?" is a
            // question about the per-frame figure against a 16.7 ms budget.
            const double advance_ms = static_cast<double>(ax.advance_ns) / 1.0e6;
            const double mix_ms = static_cast<double>(ax.mix_ns) / 1.0e6;
            const double ax_frames = ax.frames != 0 ? static_cast<double>(ax.frames) : 1.0;
            std::cout << "AX cost on the game thread: advance " << advance_ms << " ms, mix "
                      << mix_ms << " ms, total " << (advance_ms + mix_ms) << " ms ("
                      << ((advance_ms + mix_ms) / ax_frames)
                      << " ms per 5 ms AX frame)\n";
            const double effect_ms = static_cast<double>(ax.effect_ns) / 1.0e6;
            std::cout << "AX room effect: " << ax.effect_scheduled << " frames sent/returned, callback "
                      << ax.effect_processed << " processed, " << ax.effect_bypassed << " bypassed, "
                      << ax.effect_idle << " idle; " << effect_ms << " ms on the game thread ("
                      << (effect_ms / ax_frames) << " ms per AX frame)\n";
        }
        {
            const gekko::sound_tables::Report& tables = system.sound_table_report();
            std::cout << "Sound tables (native lookup): " << tables.lookups << " lookups, " << tables.found
                      << " found, " << tables.missing << " missing, " << tables.refused_disabled << " disabled, "
                      << tables.refused_limit << " at their instance limit; data errata applied "
                      << tables.errata_applied << "\n";
            const gekko::se_manager::Report& se = system.se_manager_report();
            const gekko::gcax_midi::Report& midi = system.midi_report();
            std::cout << "SE manager (native): " << se.requests << " requests, " << se.started << " started, "
                      << se.kept_alive << " kept alive, " << se.gated << " gated, " << se.culled << " culled, "
                      << se.no_channel << " without a channel, " << se.rings << " rings; " << se.frames << " frames, "
                      << se.channel_updates << " channel updates, " << se.expired << " expired, " << se.retriggered
                      << " retriggered, " << se.unsupported_port << " unsupported-port messages; MIDI pushed "
                      << midi.pushed << ", dropped " << midi.dropped << ", refused " << midi.refused << "\n";
        }
        if (measure_probes) {
            std::cout << measure_probes->summary();
            measure_results.emplace_back("r5probes", measure_probes->json());
            gekko::MeasureProbes::set_active(nullptr);
        }
        const gekko::AudioRuntime::Report audio = audio_runtime.report();
        {
            const double source_ms = audio.source_rate_hz > 0.0 ? 1000.0 / audio.source_rate_hz : 0.0;
            std::cout << "Audio runtime: " << (audio.mode == gekko::AudioRuntime::Mode::Wasapi
                                                   ? "WASAPI"
                                                   : audio.mode == gekko::AudioRuntime::Mode::Manual
                                                         ? "manual"
                                                         : "counting only")
                      << "; producer " << audio.blocks << " AI DMA blocks, " << audio.produced_frames
                      << " frames (" << audio.silence_frames << " explicit silence), "
                      << audio.dropped_frames << " dropped in " << audio.dropped_events
                      << " events, " << audio.reset_epochs << " timeline resets\n";
            if (audio.mode == gekko::AudioRuntime::Mode::Wasapi) {
                std::cout << "  device: " << audio.device_name << " (" << audio.device_format << ", "
                          << audio.device_rate << " Hz, " << audio.device_channels << " ch, period "
                          << audio.device_period_frames << " frames / " << audio.device_period_ms
                          << " ms, buffer " << audio.device_buffer_frames << " frames"
                          << (audio.low_latency_period ? ", IAudioClient3" : "")
                          << (audio.mmcss ? ", MMCSS" : "") << "), open " << (audio.device_open ? "yes" : "no")
                          << ", starts " << audio.starts << ", reopens " << audio.reopens
                          << ", invalidations " << audio.device_invalidations << ", default changes "
                          << audio.default_device_changes << ", open failures " << audio.open_failures
                          << ", render errors " << audio.render_errors << "\n";
                std::cout << "  consumer: " << audio.consumed_frames << " source frames consumed, "
                          << audio.output_frames << " device frames out; underruns "
                          << audio.underrun_events << " events / " << audio.underrun_frames
                          << " frames; prime silence " << audio.prime_silence_frames
                          << " frames; discarded (reset " << audio.reset_dropped_frames
                          << ", device gap " << audio.device_gap_discarded_frames << "); clipped "
                          << audio.clipped_samples << " samples\n";
                std::cout << "  timing: " << audio.wakes << " wakes, " << audio.late_wakes
                          << " late, " << audio.wake_timeouts << " timeouts, longest interval "
                          << static_cast<double>(audio.max_wake_interval_us) / 1000.0
                          << " ms, longest render " << static_cast<double>(audio.max_render_us) / 1000.0
                          << " ms, Start() " << static_cast<double>(audio.device_start_us) / 1000.0
                          << " ms\n";
                gekko::u64 samples = 0;
                for (const gekko::u64 count : audio.fill_histogram) samples += count;
                if (samples != 0u) {
                    const auto fill_percentile = [&](double p) {
                        const gekko::u64 rank = static_cast<gekko::u64>(p * static_cast<double>(samples - 1u));
                        gekko::u64 seen = 0;
                        for (std::size_t bucket = 0; bucket < audio.fill_histogram.size(); ++bucket) {
                            seen += audio.fill_histogram[bucket];
                            if (seen > rank) {
                                return (static_cast<double>(bucket) + 0.5) *
                                       gekko::AudioRuntime::Report::kFillBucketMs;
                            }
                        }
                        return static_cast<double>(audio.fill_histogram.size()) *
                               gekko::AudioRuntime::Report::kFillBucketMs;
                    };
                    std::cout << "  queue (source time at each wake): p5 " << fill_percentile(0.05)
                              << " ms, p50 " << fill_percentile(0.50) << " ms, p95 "
                              << fill_percentile(0.95) << " ms, max "
                              << static_cast<double>(audio.ring_max_fill_frames) * source_ms
                              << " ms of " << static_cast<double>(audio.ring_capacity_frames) * source_ms
                              << " ms capacity; start fill "
                              << static_cast<double>(audio.start_fill_frames) * source_ms << " ms\n";
                }
            }
            if (!audio.last_error.empty()) {
                std::cout << "  audio error: " << audio.last_error << "\n";
            }
        }
    }
    if (options.movie_trace) {
        std::ofstream trace(*options.movie_trace);
        trace << "game_ticks,vi_frame,site,name,a,b,c,d\n";
        for (const auto& event : system.movie_trace_events()) {
            const char* name = "?";
            switch (event.site) {
                case 0x80036D68u: name = "movie-open"; break;
                case 0x80036B0Cu: name = "movie-tick"; break;
                case 0x800367A8u: name = "frame-ready"; break;
                case 0x804096D0u: name = "convert"; break;
                case 0x80036570u: name = "blit"; break;
                case 0x804086D8u: name = "get-stat"; break;
                case 0x80403E3Cu: name = "get-frame"; break;
                case 0x803FEDE8u: name = "module"; break;
                case 0x80378ECCu: name = "os-resume-thread"; break;
                case 0x80408D80u: name = "sfd-exec-dec"; break;
                case 0x80380138u: name = "vi-get-tv-format"; break;
                case 0x8030717Cu: name = "palsel-gate"; break;
                case 0x80306B48u: name = "palsel-loader"; break;
                case 0x8047B1C0u: name = "rw-fopen"; break;
                case 0x8037E824u: name = "vi-init"; break;
                default: break;
            }
            trace << event.game_ticks << ',' << event.vi_frame << ",0x" << std::hex
                  << event.site << std::dec << ',' << name << ",0x" << std::hex << event.a
                  << ",0x" << event.b << ",0x" << event.c << ",0x" << event.d << std::dec
                  << "\n";
        }
        std::cout << system.movie_trace_report();
        std::cout << "[movie-trace] " << system.movie_trace_events().size()
                  << " events written to " << options.movie_trace->string() << "\n";
    }


    // Savestate at the end of the run. run() has returned, so the guest is at
    // a command boundary with nothing on the native stack, and the flush above
    // has drained the GX pipe - the same preconditions the N hotkey has at the
    // host-service boundary. Pairing this with --budget-ms gives a
    // reproducible slot that --load-state can restore headlessly.
    if (options.save_state_at_exit) {
        const std::filesystem::path& save_slot =
            options.save_state_file ? *options.save_state_file : state_slot;
        std::string state_error;
        if (gekko::save_state(system, save_slot, &state_error)) {
            std::cout << "[savestate] wrote " << save_slot.string() << " at "
                      << game_seconds(system) << " s of game time\n";
        } else {
            // A run that stopped inside guest work (a watchdog stop, a fault)
            // cannot be saved; the reason it stopped must not be lost with it.
            std::cerr << "[savestate] " << state_error << "\n";
            std::cout << "Game time advanced " << game_seconds(system) << " s.\n\n"
                      << system.stop_report();
            return 5;
        }
    }
    if (options.stop_at_tick != 0u) {
        std::cout << "[stop-at-tick] " << (stop_at_requested ? "asked" : "NOT reached") << " at tick "
                  << stop_at_request_tick << " (wanted " << options.stop_at_tick << "); ended at tick "
                  << system.game_ticks() << "\n";
    }
    if (input_tape.playing() || options.input_tape_play) {
        std::cout << "[input-tape] played " << (input_tape.exhausted() ? "to the end" : "partly")
                  << ", entries applied late " << input_tape.late_entries() << "\n";
    }
    if (tape_recorder != nullptr) {
        std::string tape_error;
        if (tape_recorder->write(*options.input_tape_record, stop_at_request_tick,
                                 options.save_state_at_exit ? system.game_ticks() : 0u, &tape_error)) {
            std::cout << "[input-tape] wrote " << options.input_tape_record->string() << " (" << tape_recorder->size()
                      << " samples)\n";
        } else {
            std::cerr << "[input-tape] " << tape_error << "\n";
        }
    }
    if (options.state_report) {
        std::ofstream out(*options.state_report, std::ios::binary | std::ios::trunc);
        out << state_report_json(system, render_frames(), stop_at_request_tick, report_ranges, true) << "\n";
        std::cout << "[state-report] wrote " << options.state_report->string() << "\n";
    }
    if (options.capture_at_tick && !options.window) {
        // The capture's own record beside it: which frame it is and its game tick (the workstation keys on it).
        const DevCapture result = dev_capture_result();
        const bool written = result.written > 0u && !result.path.empty();
        std::filesystem::path record = options.capture_at_tick->second;
        record.replace_extension(".capture.json");
        gekko::dev::Json capture_record;
        capture_record.field("format", "shadow-capture 1")
            .field("written", written)
            .field("path", written ? result.path : std::string())
            .field("frame", result.frame)
            .field("frame_game_ticks", result.game_ticks)
            .field("wanted_tick", options.capture_at_tick->first)
            .field("ended_at_tick", system.game_ticks());
        std::ofstream out(record, std::ios::binary | std::ios::trunc);
        out << capture_record.str() << "\n";
        if (written) {
            std::cout << "[capture-at-tick] wrote " << result.path << " (frame " << result.frame << ", game tick "
                      << result.game_ticks << ")\n";
        } else {
            std::cerr << "[capture-at-tick] NOT written (the first frame at or after game tick "
                      << options.capture_at_tick->first << " was not finished by tick " << system.game_ticks() << ")\n";
        }
    }
    const double execution_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - execution_start)
            .count();
    const double process_cpu_end = process_cpu_seconds_now();
    const std::string threads_json = thread_window_json(threads_start, sample_threads(), execution_seconds);
    const double process_cpu_seconds =
        process_cpu_end >= process_cpu_start ? process_cpu_end - process_cpu_start : 0.0;
    double memory_working_set_mib = 0.0;
    double memory_peak_working_set_mib = 0.0;
    double memory_private_mib = 0.0;
    double memory_peak_private_mib = 0.0;
#ifdef _WIN32
    {
        PROCESS_MEMORY_COUNTERS_EX memory{};
        memory.cb = sizeof(memory);
        if (GetProcessMemoryInfo(GetCurrentProcess(),
                                 reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
                                 sizeof(memory)) != FALSE) {
            memory_working_set_mib = static_cast<double>(memory.WorkingSetSize) / (1024.0 * 1024.0);
            memory_peak_working_set_mib =
                static_cast<double>(memory.PeakWorkingSetSize) / (1024.0 * 1024.0);
            memory_private_mib = static_cast<double>(memory.PrivateUsage) / (1024.0 * 1024.0);
            memory_peak_private_mib =
                static_cast<double>(memory.PeakPagefileUsage) / (1024.0 * 1024.0);
        }
    }
#endif
    std::cout << "Process memory: working set " << memory_working_set_mib << " MiB (peak "
              << memory_peak_working_set_mib << "), private " << memory_private_mib
              << " MiB (peak " << memory_peak_private_mib << ")\n";
    {
        // Storage, in one line. There is no residency budget, no warming plan,
        // no eviction, no I/O engine and no profile to report on: a read is a
        // decode and a memcpy out of a mapped page. What is worth knowing is
        // how many reads happened, what they cost, and whether the decode ever
        // failed - the last two MUST be zero.
        const gekko::assets::AssetSystem::Stats reads = system.assets().stats();
        const gekko::assets::MountReport& mounted = system.assets().report();
        const gekko::assets::FileProvider::Stats files = system.assets().files().stats();
        char row[512];
        std::snprintf(row, sizeof(row),
                      "Assets: %llu reads, %.2f MiB, %.3f ms total / %.3f ms worst; %llu of %u "
                      "files mapped (%.1f MiB); %llu unmapped, %llu out of range, %llu failed\n",
                      static_cast<unsigned long long>(reads.reads),
                      static_cast<double>(reads.bytes) / (1024.0 * 1024.0),
                      static_cast<double>(reads.read_ns) / 1.0e6,
                      static_cast<double>(reads.read_ns_max) / 1.0e6,
                      static_cast<unsigned long long>(files.mapped), mounted.files,
                      static_cast<double>(files.mapped_bytes) / (1024.0 * 1024.0),
                      static_cast<unsigned long long>(reads.unmapped),
                      static_cast<unsigned long long>(reads.out_of_range),
                      static_cast<unsigned long long>(reads.failed));
        std::cout << row;
    }
    {
        // The native audio platform (NativeAudio): passes published, AX lists
        // mixed, DSP mails, interrupt deliveries.
        const gekko::NativeAudio::Stats& audio = system.audio().stats();
        {
            const gekko::NativeMotionStore::Stats& motion = system.motion_store().stats();
            std::printf("Motion store (native, no ARAM): %llu pools, %llu uploads (%.2f MiB), %llu fetches (%.2f MiB), "
                        "arena committed %.2f MiB, %llu interrupt points\n",
                        static_cast<unsigned long long>(motion.pools), static_cast<unsigned long long>(motion.uploads),
                        static_cast<double>(motion.upload_bytes) / (1024.0 * 1024.0),
                        static_cast<unsigned long long>(motion.fetches),
                        static_cast<double>(motion.fetch_bytes) / (1024.0 * 1024.0),
                        static_cast<double>(motion.committed) / (1024.0 * 1024.0),
                        static_cast<unsigned long long>(motion.interrupt_points));
            const gekko::NativeSoundBanks::Stats& banks = system.sound_banks().stats();
            std::printf("Sound banks (native, no ARAM): %llu reserves, %llu uploads (%.2f MiB), arena reserved %.2f MiB, "
                        "committed %.2f MiB, %llu interrupt points\n",
                        static_cast<unsigned long long>(banks.reserves), static_cast<unsigned long long>(banks.uploads),
                        static_cast<double>(banks.upload_bytes) / (1024.0 * 1024.0),
                        static_cast<double>(banks.reserved) / (1024.0 * 1024.0),
                        static_cast<double>(banks.committed) / (1024.0 * 1024.0),
                        static_cast<unsigned long long>(banks.interrupt_points));
            const gekko::NativeStreamRing::Stats& ring = system.stream_ring().stats();
            std::printf("Stream ring (native, no ARAM): %llu reserves, %llu releases, %llu transfers (%.2f MiB), "
                        "arena committed %.2f MiB; %llu completions at the DSP DMA interrupt\n",
                        static_cast<unsigned long long>(ring.reserves), static_cast<unsigned long long>(ring.releases),
                        static_cast<unsigned long long>(ring.posts),
                        static_cast<double>(ring.post_bytes) / (1024.0 * 1024.0),
                        static_cast<double>(ring.committed) / (1024.0 * 1024.0),
                        static_cast<unsigned long long>(system.audio().stats().transfer_completions));
        }
        const gekko::NativeInterrupts::Stats& interrupts = system.interrupts().stats();
        const gekko::NativeFrameLimiter::Stats& limiter = system.frame_limiter().stats();
        char row[512];
        std::snprintf(row, sizeof(row),
                      "Native audio: %llu passes (%llu silent, %.2f MiB), %llu AX command lists, "
                      "%llu DSP mails; callbacks AI %llu, DSP DMA %llu, DSP %llu\n",
                      static_cast<unsigned long long>(audio.passes),
                      static_cast<unsigned long long>(audio.silent_passes),
                      static_cast<double>(audio.pass_bytes) / (1024.0 * 1024.0),
                      static_cast<unsigned long long>(audio.command_lists),
                      static_cast<unsigned long long>(audio.dsp_mails),
                      static_cast<unsigned long long>(audio.ai_callbacks),
                      static_cast<unsigned long long>(audio.transfer_completions),
                      static_cast<unsigned long long>(audio.dsp_callbacks));
        std::cout << row;
        using Source = gekko::NativeInterrupts::Source;
        const auto delivered = [&interrupts](Source source) {
            return static_cast<unsigned long long>(
                interrupts.delivered[gekko::NativeInterrupts::slot(source)]);
        };
        std::snprintf(row, sizeof(row),
                      "Native interrupts: delivered VI %llu, AI %llu, ARAM %llu, DSP %llu, PE finish %llu, "
                      "CP breakpoint %llu; %llu guest calls, %llu reschedules, %llu OS mask applications\n",
                      delivered(Source::Vi), delivered(Source::AiDma), delivered(Source::DspDma),
                      delivered(Source::Dsp), delivered(Source::PeFinish), delivered(Source::GxBreakpoint),
                      static_cast<unsigned long long>(interrupts.guest_calls),
                      static_cast<unsigned long long>(interrupts.reschedules),
                      static_cast<unsigned long long>(interrupts.mask_calls));
        std::cout << row;
        const gekko::CpuDiagCounters& poll = gekko::cpu_diag();
        std::snprintf(row, sizeof(row),
                      "Fixed-point waits: %llu progress back edges, %llu watches (%llu ended by real "
                      "work), %llu fixed points\n",
                      static_cast<unsigned long long>(poll.poll_points),
                      static_cast<unsigned long long>(poll.poll_watches),
                      static_cast<unsigned long long>(system.guest_memory().watch_aborts()),
                      static_cast<unsigned long long>(poll.poll_fixed_points));
        std::cout << row;
        std::snprintf(row, sizeof(row),
                      "Game time: %.6f s; frame limiter %llu frames (%llu waited %.3f s, %llu yields, "
                      "%llu movie refresh waits, %llu bypassed, %llu already due, %llu held from zero "
                      "time, interval %u, %llu paced at one period); "
                      "%llu idle waits; %llu host services\n",
                      game_seconds(system), static_cast<unsigned long long>(limiter.frames),
                      static_cast<unsigned long long>(limiter.waits),
                      static_cast<double>(limiter.waited_ticks) /
                          static_cast<double>(gekko::kGameTicksPerSecond),
                      static_cast<unsigned long long>(limiter.yields),
                      static_cast<unsigned long long>(limiter.movie_waits),
                      static_cast<unsigned long long>(limiter.bypassed),
                      static_cast<unsigned long long>(limiter.immediate),
                      static_cast<unsigned long long>(limiter.zero_time), limiter.last_interval,
                      static_cast<unsigned long long>(limiter.paced_at_one_period),
                      static_cast<unsigned long long>(system.idle_waits()),
                      static_cast<unsigned long long>(system.host_services()));
        std::cout << row;
    }
    if (!options.tuning.empty()) {
        std::cout << "Host tuning:";
        for (const auto& item : options.tuning) {
            std::cout << ' ' << item.first << '=' << (item.second ? '1' : '0');
            if (tuning_applied.find(item.first) == tuning_applied.end()) {
                std::cout << "(not applied)";
            }
        }
        std::cout << "\n";
        for (const auto& item : options.tuning) {
            if (tuning_applied.find(item.first) == tuning_applied.end()) {
                std::cerr << "WARNING: --tune " << item.first
                          << " never reached its target in this configuration "
                             "(missing window, audio device or DX12 backend); it had NO effect.\n";
            }
        }
    }
    const gekko::HostJobReport jobs = gekko::host_job_report();
    if (options.window) {
        std::cout << "Fixed simulation pacing: " << pacing_boundaries << " boundaries, "
                  << pacing_paced_services << " efficient waits, "
                  << pacing_unpaced_services << " behind/no-wait, "
                  << pacing_resyncs << " debt rebases, " << pacing_debt_clamps
                  << " hysteretic debt clamps, " << pacing_skipped_ticks
                  << " skipped boundaries, " << simulation_clock.stats().hitches_forgiven
                  << " host hitches forgiven ("
                  << std::chrono::duration<double, std::milli>(
                         simulation_clock.stats().hitch_time_forgiven).count()
                  << " ms), maximum debt " << pacing_maximum_debt_ms
                  << " ms (60 Hz quanta, "
                  << (simulation_clock.config().maximum_catchup_rate > 1.0
                          ? "rate-limited catch-up"
                          : "absolute-deadline catch-up")
                  << ")\n";
        std::cout << "  guest 60 Hz quanta (wall between boundaries): "
                  << histogram_text(quantum_hist) << "; "
                  << quantum_hist.count_at_or_above(17000000u) << " quanta >= 17 ms, "
                  << quantum_hist.count_at_or_above(20000000u) << " >= 20 ms\n";
        // ---- where the long quanta actually went ---------------------------
        if (frame_tail.quanta != 0u) {
            const auto ms = [](gekko::u64 ns) { return static_cast<double>(ns) / 1.0e6; };
            const auto mean_ms = [&](gekko::u64 total, gekko::u64 n) {
                return n == 0u ? 0.0 : ms(total) / static_cast<double>(n);
            };
            const gekko::u64 n = frame_tail.long_quanta;
            char row[900];
            const double accounted = mean_ms(frame_tail.l_guest, n) +
                                     mean_ms(frame_tail.l_service, n) +
                                     mean_ms(frame_tail.l_sleep, n);
            std::snprintf(
                row, sizeof(row),
                "[frame-tail] %llu quanta, %llu long (>= 20 ms), mean %.3f ms overall\n"
                "  long-quantum mean %.3f ms = guest %.3f + service %.3f + sleep %.3f"
                " (unattributed %.3f), sleep overshoot %.3f ms\n"
                "  after a long quantum: mean %.3f ms over %llu samples, %llu of them < 16 ms\n",
                static_cast<unsigned long long>(frame_tail.quanta),
                static_cast<unsigned long long>(n),
                mean_ms(frame_tail.wall_total, frame_tail.quanta),
                mean_ms(frame_tail.l_wall, n), mean_ms(frame_tail.l_guest, n),
                mean_ms(frame_tail.l_service, n), mean_ms(frame_tail.l_sleep, n),
                mean_ms(frame_tail.l_wall, n) - accounted, mean_ms(frame_tail.l_overshoot, n),
                mean_ms(frame_tail.after_long_total, frame_tail.after_long_count),
                static_cast<unsigned long long>(frame_tail.after_long_count),
                static_cast<unsigned long long>(frame_tail.after_long_short));
            std::cout << row;
            for (const auto& sample : frame_tail.longest) {
                std::snprintf(row, sizeof(row),
                              "  %8.3f ms | guest %7.3f service %7.3f sleep %7.3f over %6.3f |"
                              " %5u ticks %9llu instr | next %7.3f ms\n",
                              ms(sample.wall_ns), ms(sample.guest_ns), ms(sample.service_ns),
                              ms(sample.sleep_ns), ms(sample.overshoot_ns), sample.ticks,
                              static_cast<unsigned long long>(sample.instructions),
                              ms(sample.next_wall_ns));
                std::cout << row;
            }
        }
        std::cout << "  host service per tick (CPU, backpressure excluded): "
                  << histogram_text(service_cost_hist) << "; backpressure inside ticks "
                  << static_cast<double>(service_backpressure_ns_total) / 1.0e6 << " ms\n";
    }
    if (game_thread_policy.applied) {
        std::cout << "Game-thread scheduling: priority "
                  << game_thread_policy.effective_priority
                  << (game_thread_policy.priority_applied ? " (above normal)" : " (unchanged)")
                  << ", dynamic boosts "
                  << (game_thread_policy.priority_boosts_enabled ? "enabled" : "unchanged")
                  << ", execution-speed throttling "
                  << (game_thread_policy.execution_speed_throttling_disabled ? "disabled" :
                                                                                "unchanged")
                  << ", process throttling "
                  << (game_thread_policy.process_throttling_disabled ? "disabled" : "unchanged")
                  << (options.window ? " (windowed)" : " (headless)") << "\n";
    }
    if (game_thread_placement.available()) {
        char mask[24];
        std::snprintf(mask, sizeof(mask), "0x%016llX",
                      static_cast<unsigned long long>(game_thread_placement.seen_mask()));
        std::cout << "Game-thread placement: " << game_thread_placement.samples() << " host-service samples, "
                  << std::fixed << std::setprecision(1) << game_thread_placement.performance_percent()
                  << std::defaultfloat << " % on "
                  << (game_thread_placement.hybrid() ? "performance cores" : "a non-hybrid CPU (all one class)")
                  << ", logical processors seen " << mask
                  << (game_thread_placement.unknown_samples() != 0u ? " (some samples unclassified)" : "") << "\n";
    }
    if (options.visibility_probe) {
        const auto& probe = system.visibility_probe();
        char row[512];
        std::snprintf(
            row, sizeof(row),
            "Guest frustum probe: %llu sphere tests (%llu layout ok, %llu unrecognised)\n"
            "  guest FOV: %.3f deg horizontal, %.3f deg vertical; host widened to %.3f deg horizontal\n"
            "  viewWindow=(%.6f,%.6f) [x range %.6f..%.6f] camera=0x%08X\n"
            "  -> guest hFOV %.4f deg, vFOV %.4f deg, tan-aspect %.6f\n"
            "  rejected by guest %llu, rejected by widened frustum %llu\n"
            "  LOST TO THE GUEST CULL: %llu (%.2f%% of all tests) - visible to the host, never submitted\n",
            static_cast<unsigned long long>(probe.tests),
            static_cast<unsigned long long>(probe.layout_ok),
            static_cast<unsigned long long>(probe.layout_bad),
            probe.guest_half_hfov_deg * 2.0, probe.guest_half_vfov_deg * 2.0,
            probe.widened_half_hfov_deg * 2.0,
            probe.view_window_x, probe.view_window_y,
            probe.view_window_x_min > 1e8 ? 0.0 : probe.view_window_x_min,
            probe.view_window_x_max, probe.camera_address,
            2.0 * std::atan(probe.view_window_x) * 180.0 / 3.14159265358979323846,
            2.0 * std::atan(probe.view_window_y) * 180.0 / 3.14159265358979323846,
            probe.view_window_y > 1e-9 ? probe.view_window_x / probe.view_window_y : 0.0,
            static_cast<unsigned long long>(probe.rejected_guest),
            static_cast<unsigned long long>(probe.rejected_widened),
            static_cast<unsigned long long>(probe.lost),
            probe.tests != 0u ? 100.0 * static_cast<double>(probe.lost) /
                                    static_cast<double>(probe.tests)
                              : 0.0);
        std::cout << row;

        // Production culler vs the independent clip-space reference. The
        // production culler is the guest own plane test; the reference
        // transforms each sphere conservative world-space AABB into the
        // DISPLAYED camera view space and rejects only when all eight corners
        // are outside the same clip plane. The first number is the popping
        // defect, measured per prop rather than inferred.
        static const char* const kPlaneNames[7] = {"left",  "right", "bottom", "top",
                                                   "near",  "far",   "none"};
        char detail[640];
        std::snprintf(
            detail, sizeof(detail),
            "  near/far in use: %.3f / %.3f\n"
            "  PRODUCTION REJECTED WHILE STILL ON SCREEN: %llu  (reference says visible)\n"
            "    by production plane: left %llu, right %llu, bottom %llu, top %llu, near %llu, far %llu\n"
            "    worst case: sphere 0x%08X still had %.2f world units of horizontal room\n"
            "  reference rejected while production kept it: %llu (must be 0)\n",
            probe.near_plane, probe.far_plane,
            static_cast<unsigned long long>(probe.reference_visible_production_rejected),
            static_cast<unsigned long long>(probe.disagree_by_plane[0]),
            static_cast<unsigned long long>(probe.disagree_by_plane[1]),
            static_cast<unsigned long long>(probe.disagree_by_plane[2]),
            static_cast<unsigned long long>(probe.disagree_by_plane[3]),
            static_cast<unsigned long long>(probe.disagree_by_plane[4]),
            static_cast<unsigned long long>(probe.disagree_by_plane[5]),
            probe.worst_disagreement_sphere, probe.worst_disagreement_margin,
            static_cast<unsigned long long>(probe.reference_rejected_production_visible));
        std::cout << detail;

        const auto& records = system.visibility_disagreements();
        if (!records.empty()) {
            std::cout << "  first " << records.size()
                      << " disagreeing props (sphere, world centre, radius, rejecting plane, "
                         "production margin, horizontal room left, view xyz):\n";
            for (const auto& r : records) {
                char line[320];
                std::snprintf(line, sizeof(line),
                              "    0x%08X  c=(%10.2f,%10.2f,%10.2f) r=%8.2f  %-6s  over=%9.2f  "
                              "room=%8.2f  view=(%9.2f,%9.2f,%9.2f)\n",
                              r.sphere_address, r.centre[0], r.centre[1], r.centre[2], r.radius,
                              kPlaneNames[r.rejecting_plane < 7u ? r.rejecting_plane : 6u],
                              r.production_margin, r.reference_slack, r.view_x, r.view_y,
                              r.view_z);
                std::cout << line;
            }
        }
    }
    {
        const auto& range = system.object_range().stats();
        char row[512];
        std::snprintf(row, sizeof(row),
                      "Object range: %llu spawns of %llu spawn tests, %llu leaves of %llu leave "
                      "tests, %llu always in range, %llu out-of-range tests, %llu radius reads\n",
                      static_cast<unsigned long long>(range.spawns),
                      static_cast<unsigned long long>(range.spawn_tests),
                      static_cast<unsigned long long>(range.leaves),
                      static_cast<unsigned long long>(range.leave_tests),
                      static_cast<unsigned long long>(range.always_in_range),
                      static_cast<unsigned long long>(range.out_of_range_tests),
                      static_cast<unsigned long long>(range.radius_reads));
        std::cout << row;
    }
    if (options.native_visibility) {
        const auto& nv = system.native_visibility_stats();
        char row[512];
        std::snprintf(row, sizeof(row),
                      "Native visibility: %llu decided by the native frustum "
                      "(%llu inside, %llu boundary, %llu outside), %llu camera rebuilds\n"
                      "  ADMITTED that the original would have rejected: %llu\n"
                      "  deferred: %llu at unity widening (4:3 reference), "
                      "%llu unrecognised layout, %llu display not yet configured\n",
                      static_cast<unsigned long long>(nv.decided),
                      static_cast<unsigned long long>(nv.inside),
                      static_cast<unsigned long long>(nv.boundary),
                      static_cast<unsigned long long>(nv.outside),
                      static_cast<unsigned long long>(nv.rebuilds),
                      static_cast<unsigned long long>(nv.admitted),
                      static_cast<unsigned long long>(nv.deferred_unity),
                      static_cast<unsigned long long>(nv.deferred_layout),
                      static_cast<unsigned long long>(nv.deferred_unconfigured));
        std::cout << row;
        const auto& fb = system.frustum_build_stats();
        std::snprintf(row, sizeof(row),
                      "  camera frustums built from the displayed picture: %llu (left to RenderWare: %llu without a "
                      "canonical camera, %llu with a view offset)\n",
                      static_cast<unsigned long long>(fb.built),
                      static_cast<unsigned long long>(fb.declined_camera),
                      static_cast<unsigned long long>(fb.declined_offset));
        std::cout << row;
    }
    {
        // The health bar ledger. Every outcome is attributed AND printed: the
        // probe this replaces had two observers classifying the same call into
        // one shared ledger, so its buckets summed to twice its call count and
        // every published per-cause figure was 2x. One classifier, one ledger,
        // and the unattributed residual is printed so it cannot hide again.
        using Reason = gekko::health_bar::Rejection;
        const auto& h = system.health_bar_stats();
        const auto reason = [&h](Reason r) {
            return static_cast<unsigned long long>(h.rejected[static_cast<std::size_t>(r)]);
        };
        unsigned long long attributed = static_cast<unsigned long long>(h.drawn);
        for (const auto n : h.rejected) attributed += static_cast<unsigned long long>(n);
        char row[768];
        std::snprintf(row, sizeof(row),
                      "Health bar (%s): %llu considered, %llu drawn, %llu quads\n"
                      "  camera: %llu updates, %llu refused, %llu declines\n"
                      "  rejected: hud %llu, disabled %llu, authored-min %llu, "
                      "authored-max %llu, near %llu, far %llu, behind %llu, "
                      "off-screen %llu, unattributed %llu\n",
                      options.legacy_health_bar ? "legacy guest path" : "native",
                      static_cast<unsigned long long>(h.considered),
                      static_cast<unsigned long long>(h.drawn),
                      static_cast<unsigned long long>(h.quads),
                      static_cast<unsigned long long>(h.camera_updates),
                      static_cast<unsigned long long>(h.camera_refused),
                      static_cast<unsigned long long>(system.health_bar_camera_declines()),
                      reason(Reason::HudHidden), reason(Reason::Disabled),
                      reason(Reason::AuthoredMinDepth), reason(Reason::AuthoredMaxDepth),
                      reason(Reason::CameraNear), reason(Reason::CameraFar),
                      reason(Reason::BehindCamera),
                      reason(Reason::OffScreenLeft) + reason(Reason::OffScreenRight) +
                          reason(Reason::OffScreenBottom) + reason(Reason::OffScreenTop),
                      static_cast<unsigned long long>(h.considered) - attributed);
        std::cout << row;
    }
    {
        // The Options menu's Display tile (gekko/native_options_display.h).
        const auto& d = system.options_display().report();
        std::cout << "Options Display tile: " << d.setups << " screens, " << d.row_draws << " page draws, " << d.moves
                  << " moves, " << d.changes << " changes, " << d.applies << " applied (" << d.apply_failures
                  << " refused), " << d.saves << " saved (" << d.save_failures << " failed), " << d.font_failures
                  << " font failures";
        if (!system.options_display().last_error().empty())
            std::cout << "; last error: " << system.options_display().last_error();
        std::cout << "\n";
    }
    if (options.fov_probe) {
        const auto& fp = system.fov_probe();
        char row[600];
        std::snprintf(row, sizeof(row),
                      "Camera FOV chain: %llu pair reads, %llu scale calls\n"
                      "  camera-mode object 0x%08X: fovX %.4f deg (range %.4f..%.4f), fovY %.4f deg\n"
                      "  scale index: last %d; taken -1 x%llu / 0 x%llu / other x%llu\n",
                      static_cast<unsigned long long>(fp.pair_reads),
                      static_cast<unsigned long long>(fp.scale_calls),
                      fp.mode_object, fp.fov_x,
                      fp.fov_x_min > 1e8 ? 0.0 : fp.fov_x_min,
                      fp.fov_x_max < -1e8 ? 0.0 : fp.fov_x_max, fp.fov_y,
                      fp.scale_index,
                      static_cast<unsigned long long>(fp.scale_index_negative),
                      static_cast<unsigned long long>(fp.scale_index_zero),
                      static_cast<unsigned long long>(fp.scale_index_other));
        std::cout << row;
    }
    {
        {
            // Memory-card host persistence. "tick wired" false would mean the
            // debounce is inert and every card write rewrites the image.
            const gekko::NativeCard::Stats& card = system.card().stats();
            char persist[320];
            std::snprintf(persist, sizeof(persist),
                          "    memory card: %llu writes, %llu erases; host "
                          "persists %llu (%llu absorbed by the debounce), %.1f ms total, "
                          "%.1f ms max; tick wired %s; %llu failures\n",
                          static_cast<unsigned long long>(card.writes),
                          static_cast<unsigned long long>(card.erases),
                          static_cast<unsigned long long>(card.persists),
                          static_cast<unsigned long long>(card.persists_deferred),
                          static_cast<double>(card.persist_ns) / 1.0e6,
                          static_cast<double>(card.persist_ns_max) / 1.0e6,
                          system.card().persist_tick_wired() ? "yes" : "NO",
                          static_cast<unsigned long long>(card.persist_failures));
            std::cout << persist;
            const gekko::NativeIpl::Stats& ipl = system.ipl().stats();
            std::snprintf(persist, sizeof(persist),
                          "    IPL services: %llu debug-UART lines, %llu SRAM persists, "
                          "%llu unchanged SRAM commits skipped, %llu failed\n",
                          static_cast<unsigned long long>(ipl.uart_lines),
                          static_cast<unsigned long long>(ipl.sram_persists),
                          static_cast<unsigned long long>(ipl.sram_unchanged),
                          static_cast<unsigned long long>(ipl.sram_failures));
            std::cout << persist;
        }
        {
            const gekko::HostPageStats& stats = gekko::host_page_stats();
            char cost[192];
            std::snprintf(cost, sizeof(cost),
                          "      page table: %llu installs (GuestMemory maps pages itself), %llu re-selections "
                          "%.1f ms\n",
                          static_cast<unsigned long long>(stats.installs),
                          static_cast<unsigned long long>(stats.invalidations),
                          static_cast<double>(stats.invalidation_ns) / 1.0e6);
            std::cout << cost;
        }
    }
    std::cout << "Host jobs: " << jobs.logical_workers << " logical workers, "
              << jobs.parallel_groups << " joined groups, " << jobs.worker_chunks
              << " worker chunks + " << jobs.caller_chunks << " caller chunks, "
              << jobs.items << " independent items\n";
    // Every barrier attributed to the caller that opened it, ranked by how long
    // that caller sat in the join. host_parallel_for is fork/join: the opening
    // thread runs the first chunk itself and then blocks until the rest land,
    // so this is the serialization the shape imposes, not merely bookkeeping.
    {
        struct SiteRow {
            const char* name;
            gekko::JobSiteReport data;
        };
        std::vector<SiteRow> rows;
        for (unsigned index = 0; index < static_cast<unsigned>(gekko::JobSite::Count); ++index) {
            if (jobs.sites[index].groups == 0u) continue;
            rows.push_back(
                {gekko::job_site_name(static_cast<gekko::JobSite>(index)), jobs.sites[index]});
        }
        std::sort(rows.begin(), rows.end(),
                  [](const SiteRow& a, const SiteRow& b) { return a.data.wait_ns > b.data.wait_ns; });
        if (!rows.empty()) {
            std::cout << "  join barriers by caller (total blocked, groups, avg, longest):\n";
            for (const SiteRow& row : rows) {
                const double total_ms = static_cast<double>(row.data.wait_ns) / 1.0e6;
                const double avg_us = row.data.groups == 0u
                                          ? 0.0
                                          : static_cast<double>(row.data.wait_ns) /
                                                static_cast<double>(row.data.groups) / 1.0e3;
                // CPU-01: the FORK half of the barrier. wait_ns alone hides a
                // group whose set-up costs more than the parallelism it buys -
                // exactly the case a fan-out threshold has to be tuned against.
                const double fork_ms = static_cast<double>(row.data.fork_ns) / 1.0e6;
                const double fork_avg_us = row.data.groups == 0u
                                               ? 0.0
                                               : static_cast<double>(row.data.fork_ns) /
                                                     static_cast<double>(row.data.groups) / 1.0e3;
                const double items_per_group =
                    row.data.groups == 0u ? 0.0
                                          : static_cast<double>(row.data.items) /
                                                static_cast<double>(row.data.groups);
                const double chunks_per_group =
                    row.data.groups == 0u ? 0.0
                                          : static_cast<double>(row.data.chunks) /
                                                static_cast<double>(row.data.groups);
                std::cout << "    " << row.name << ": " << total_ms << " ms over "
                          << row.data.groups << " groups (" << avg_us << " us avg, "
                          << (static_cast<double>(row.data.longest_wait_ns) / 1.0e6)
                          << " ms longest), " << row.data.items << " items in "
                          << row.data.chunks << " chunks\n";
                std::cout << "      fork " << fork_ms << " ms (" << fork_avg_us
                          << " us avg, " << (static_cast<double>(row.data.longest_fork_ns) / 1.0e6)
                          << " ms longest); " << items_per_group << " items/group, "
                          << chunks_per_group << " chunks/group; fork+join "
                          << (fork_ms + total_ms) << " ms\n";
            }
            std::cout << "    total caller-blocked time: "
                      << (static_cast<double>(jobs.total_wait_ns) / 1.0e6) << " ms\n";
            // CPU-01: fork/wake economics. A futile wake is a worker that woke
            // and found nothing to do - pure host scheduling loss. notify_calls
            // says which wake arm ran (== queued chunks with notify_one on,
            // == groups with it off).
            const double total_fork_ms = static_cast<double>(jobs.total_fork_ns) / 1.0e6;
            const double futile_share =
                jobs.worker_wakes == 0u
                    ? 0.0
                    : 100.0 * static_cast<double>(jobs.futile_wakes) /
                          static_cast<double>(jobs.worker_wakes);
            std::cout << "    fork/wake economics: total fork " << total_fork_ms
                      << " ms, fork+join " << (total_fork_ms + static_cast<double>(jobs.total_wait_ns) / 1.0e6)
                      << " ms over " << jobs.parallel_groups << " groups; "
                      << jobs.worker_chunks << " worker chunks + " << jobs.caller_chunks
                      << " caller chunks\n";
            std::cout << "      wakes: " << jobs.worker_wakes << " worker wakes, "
                      << jobs.futile_wakes << " futile (" << futile_share << "%), "
                      << jobs.notify_calls << " notify calls ("
                      << (jobs.notify_one ? "notify_one" : "notify_all") << " arm, "
                      << jobs.logical_workers << " workers); job queue capacity "
                      << jobs.queue_capacity << ", peak " << jobs.queue_peak << "\n";
        }
    }
    std::cout << "Game time advanced " << static_cast<double>(system.game_ticks() - initial_game_ticks) /
                                                 static_cast<double>(gekko::kGameTicksPerSecond)
              << " s.\n\n" << system.stop_report();

    if (system.cpu().recompiled_calls != 0) {
        // Dispatcher round trips - sc, rfi, yield, trap - interpret nothing
        // unless the interpreter line says so.
        char row[352];
        std::snprintf(row, sizeof(row),
                      "Native execution: %llu entries, %llu dispatcher round trips (sc/rfi/yield/trap, "
                      "nothing interpreted unless the interpreter line says so)\n",
                      static_cast<unsigned long long>(system.cpu().recompiled_calls),
                      static_cast<unsigned long long>(system.cpu().interpreter_exits));
        std::cout << row;
        // F1c finding D1 / audit R12. jump_indirect and resume_elsewhere
        // either publish a native target (Cpu::transfer_to; before R12 this
        // was a host CALL that could keep a frame per guest loop lap) or leave
        // to the dispatcher. Same events and counts as before R12; the label
        // changed because no transfer keeps a host frame any more.
        const gekko::u64 transfer_calls = system.cpu().native_transfer_calls;
        const gekko::u64 transfer_leaves = system.cpu().native_transfer_leaves;
        char transfer_row[256];
        std::snprintf(transfer_row, sizeof(transfer_row),
                      "Native transfers: %llu published to the waiting frame (jump_indirect/"
                      "resume_elsewhere), %llu left to the dispatcher\n",
                      static_cast<unsigned long long>(transfer_calls),
                      static_cast<unsigned long long>(transfer_leaves));
        std::cout << transfer_row;
    }

    {
        {
            const gekko::CpuDiagCounters& diag = gekko::cpu_diag();
            std::printf("Counter identity: interpreted %llu; entries %llu = dispatcher exits %llu + "
                        "normal returns %llu\n",
                        static_cast<unsigned long long>(system.cpu().interpreted_instructions()),
                        static_cast<unsigned long long>(system.cpu().recompiled_calls),
                        static_cast<unsigned long long>(system.cpu().interpreter_exits),
                        static_cast<unsigned long long>(diag.native_normal_returns));
            if (diag.dispatch_timing) {
                const double run_ms = static_cast<double>(diag.run_ns) / 1.0e6;
                const double native_ms = static_cast<double>(diag.native_ns) / 1.0e6;
                std::printf("Dispatcher time: Cpu::run %.3f ms = native call trees %.3f ms + "
                            "dispatcher %.3f ms (of which interrupt delivery %.3f ms) over %llu "
                            "iterations, %llu timed native calls (%.1f ns dispatcher per iteration)\n",
                            run_ms, native_ms, run_ms - native_ms,
                            static_cast<double>(diag.interrupt_raise_ns) / 1.0e6,
                            static_cast<unsigned long long>(diag.dispatch_iterations),
                            static_cast<unsigned long long>(diag.native_calls_timed),
                            diag.dispatch_iterations != 0u
                                ? static_cast<double>(diag.run_ns - diag.native_ns) /
                                      static_cast<double>(diag.dispatch_iterations)
                                : 0.0);
            }
            {
                // INTERPRETER ERADICATION AUDIT (docs/INTERPRETER_ERADICATION_AUDIT.md)
                const unsigned long long total =
                    gekko::g_non_native_ppc_executions.load(std::memory_order_relaxed);
                std::printf("TOTAL_NON_NATIVE_PPC_EXECUTIONS: %llu\n", total);
                if (total != 0ull) {
                    std::printf("  non-native origins (pc, instruction, lr, cause, count, first@interpreted, thread):\n");
                    for (const auto& origin : diag.non_native_origins) {
                        if (origin.count == 0u) continue;
                        std::printf("    %s  0x%08X  lr=0x%08X  cause=%u  x%llu  @%llu  t=%016llx\n",
                                    system.describe_address(origin.pc).c_str(), origin.instruction,
                                    origin.lr, origin.cause,
                                    static_cast<unsigned long long>(origin.count),
                                    static_cast<unsigned long long>(origin.first_interpreted),
                                    static_cast<unsigned long long>(origin.thread_hash));
                    }
                    if (diag.non_native_origins_overflow != 0u) {
                        std::printf("    ... %llu executions with no free origin slot\n",
                                    static_cast<unsigned long long>(diag.non_native_origins_overflow));
                    }
                }
                {
                    // Cpu::try_hle filter occupancy (1024 direct-mapped slots keyed by
                    // address >> 2, cpu.h kHleSlotCount): a slot holding two hooks sends
                    // EVERY call into that slot, hooked or not, through try_hle_slow.
                    std::map<gekko::u32, std::vector<gekko::u32>> slots;
                    for (const auto& entry : system.cpu().hle_entries()) {
                        slots[(entry.address >> 2) & 1023u].push_back(entry.address);
                    }
                    std::size_t shared = 0;
                    std::string members;
                    for (const auto& [slot, addresses] : slots) {
                        if (addresses.size() < 2u) continue;
                        ++shared;
                        char text[48];
                        for (const gekko::u32 address : addresses) {
                            std::snprintf(text, sizeof(text), " %08X", address);
                            members += text;
                        }
                        members += " |";
                    }
                    std::printf("HLE filter: %zu of %zu hooked slots shared%s%s\n", shared, slots.size(),
                                shared != 0u ? ":" : "", members.c_str());
                }
                std::printf("HLE hooks installed: %zu; hits this run:\n", system.cpu().hle_entries().size());
                for (const auto& entry : system.cpu().hle_entries()) {
                    if (entry.hits == 0u) continue;
                    std::printf("    0x%08X  x%llu  %s\n", entry.address,
                                static_cast<unsigned long long>(entry.hits), entry.name.c_str());
                }
            }
        }
    }

    {
        char row[224];
        std::snprintf(row, sizeof(row),
                      "Dispatch blocked: interrupt due %llu, HLE hook %llu, no native code %llu\n",
                      static_cast<unsigned long long>(system.cpu().dispatch_blocked_interrupt),
                      static_cast<unsigned long long>(system.cpu().dispatch_blocked_hle),
                      static_cast<unsigned long long>(system.cpu().dispatch_no_native_code));
        std::cout << row;
    }
    {
        char row[288];
        std::snprintf(row, sizeof(row),
                      "Native boundary transitions: not-recompiled %llu, sc %llu, rfi %llu, "
                      "trap %llu, unexpected-return %llu, past-function %llu, yield %llu, "
                      "verify-cut %llu\n",
                      static_cast<unsigned long long>(
                          system.cpu().fallback_total(gekko::FallbackReason::NotRecompiled)),
                      static_cast<unsigned long long>(
                          system.cpu().fallback_total(gekko::FallbackReason::SystemCall)),
                      static_cast<unsigned long long>(
                          system.cpu().fallback_total(gekko::FallbackReason::ReturnFromInterrupt)),
                      static_cast<unsigned long long>(
                          system.cpu().fallback_total(gekko::FallbackReason::Trap)),
                      static_cast<unsigned long long>(
                          system.cpu().fallback_total(gekko::FallbackReason::UnexpectedReturn)),
                      static_cast<unsigned long long>(
                          system.cpu().fallback_total(gekko::FallbackReason::EndOfFunction)),
                      static_cast<unsigned long long>(
                          system.cpu().fallback_total(gekko::FallbackReason::Yield)),
                      static_cast<unsigned long long>(
                          system.cpu().fallback_total(gekko::FallbackReason::VerifyCut)));
        std::cout << row;
    }
    {
        char row[256];
        std::snprintf(row, sizeof(row),
                      "Native dispatch: %llu at declared entry points, %llu entered mid-function "
                      "through the translated-range table\n",
                      static_cast<unsigned long long>(system.cpu().recompiled_calls -
                                                      system.cpu().dispatch_entered_mid_function),
                      static_cast<unsigned long long>(
                          system.cpu().dispatch_entered_mid_function));
        std::cout << row;
    }
    if (const gekko::RecompiledCode* table = system.cpu().recompiled()) {
        // The dense span's committed memory is the regression this line guards:
        // one stray entry far from the code region once grew it to 4.06 GiB.
        char row[224];
        std::snprintf(row, sizeof(row),
                      "Dispatch tables: %.1f MiB committed (dense span 0x%08X-0x%08X)\n",
                      static_cast<double>(table->table_bytes()) / (1024.0 * 1024.0),
                      table->low(), table->high());
        std::cout << row;
    }
    {
        // Interpreter cost, attributed to the guest address that stranded
        // execution there. A translation that is missing but cheap is noise; a
        // translation that is missing and expensive is the next thing to fix.
        const auto strands = system.cpu().interpreter_strands();
        const gekko::u64 interpreted = system.cpu().interpreted_instructions();
        const double interpreted_ms =
            static_cast<double>(system.cpu().interpreted_ns()) / 1.0e6;
        char row[256];
        // The headline the architecture is judged on: how many times the machine
        // had to leave translated code and execute a guest instruction itself.
        std::snprintf(row, sizeof(row),
                      "INTERPRETER ENTRIES: %llu  (%llu guest instructions, %.3f ms)\n",
                      static_cast<unsigned long long>(strands.size() == 0 ? 0u : [&] {
                          gekko::u64 total = 0;
                          for (const auto& strand : strands) total += strand.entries;
                          return total;
                      }()),
                      static_cast<unsigned long long>(interpreted), interpreted_ms);
        std::cout << row;
        std::snprintf(row, sizeof(row),
                      "Interpreter cost: %llu instructions in %.2f ms "
                      "(%.1f%% of execution) across %zu distinct strand origins\n",
                      static_cast<unsigned long long>(interpreted),
                      interpreted_ms,
                      execution_seconds > 0.0
                          ? 100.0 * (interpreted_ms / 1000.0) / execution_seconds
                          : 0.0,
                      strands.size());
        std::cout << row;
        const std::size_t shown = (std::min)(strands.size(), static_cast<std::size_t>(12));
        for (std::size_t index = 0; index < shown; ++index) {
            const auto& strand = strands[index];
            std::snprintf(row, sizeof(row),
                          "  %10llu instr  %8.2f ms  %7llu entries  longest %7llu  ",
                          static_cast<unsigned long long>(strand.instructions),
                          static_cast<double>(strand.host_ns) / 1.0e6,
                          static_cast<unsigned long long>(strand.entries),
                          static_cast<unsigned long long>(strand.longest_instructions));
            std::cout << row << system.describe_address(strand.address) << "  from "
                      << system.describe_address(strand.last_link) << "\n";
        }
    }
    if (options.fallback_audit) {
        const auto sites = system.cpu().fallback_sites();
        std::cout << "\nInterpreter fallbacks: " << sites.size()
                  << " distinct sites, in the order the boot path hits them:\n"
                  << "  guest target            caller        count        reason\n";
        std::size_t shown = 0;
        for (const auto& site : sites) {
            if (site.reason == gekko::FallbackReason::Yield) {
                continue;  // expected: a stop request or interrupt delivery
            }
            char row[288];
            std::snprintf(row, sizeof(row), "  %-22s  0x%08X  %11llu  %s",
                          system.describe_address(site.target).c_str(), site.caller,
                          static_cast<unsigned long long>(site.count),
                          gekko::fallback_reason_name(site.reason));
            std::cout << row << "\n";
            if (++shown >= 400) {
                std::cout << "  ... " << (sites.size() - shown) << " more\n";
                break;
            }
        }
        {
            // The Yield sites too, by count. A yield is a loop back edge (or
            // mtmsr, or a wait point) that saw an interrupt due or a stop.
            std::vector<gekko::Cpu::FallbackSite> yields;
            for (const auto& site : sites) {
                if (site.reason == gekko::FallbackReason::Yield) yields.push_back(site);
            }
            std::sort(yields.begin(), yields.end(),
                      [](const auto& a, const auto& b) { return a.count > b.count; });
            // 2026-09-23 (STEP 0, audit R13): the whole table, not the top 24.
            // The rows that matter for R13 - a resume target equal to its own
            // yielding site inside an SDK mfmsr..mtmsr window - are rare by
            // count and were below the old cut-off.
            std::cout << "\nYield sites: " << yields.size() << " distinct, all by count:\n"
                      << "  resume target           yielding site   count\n";
            for (const auto& site : yields) {
                char row[224];
                std::snprintf(row, sizeof(row), "  %-22s  0x%08X  %11llu",
                              system.describe_address(site.target).c_str(), site.caller,
                              static_cast<unsigned long long>(site.count));
                std::cout << row << "\n";
            }
        }
    }

    if (options.verify_recompiled) {
        const auto& report = system.cpu().verify_report();
        char row[352];
        // The segment counts alone once hid a collapsed oracle; the instruction
        // coverage share is the number a verify run is judged on.
        std::snprintf(row, sizeof(row),
                      "Verification: %llu call segments matched, %llu mismatched, %llu skipped "
                      "(hardware access), %llu skipped (did not return); %llu instructions "
                      "replayed and verified\n",
                      static_cast<unsigned long long>(report.verified),
                      static_cast<unsigned long long>(report.mismatches),
                      static_cast<unsigned long long>(report.skipped_device),
                      static_cast<unsigned long long>(report.skipped_exit),
                      static_cast<unsigned long long>(report.verified_instructions));
        std::cout << row;
        std::cout << "  journal peaks: native " << report.native_journal_peak_entries
                  << " entries, interpreter " << report.interpreter_journal_peak_entries
                  << " entries, " << report.unique_native_bytes_peak
                  << " unique native bytes; retained bounded scratch "
                  << static_cast<double>(report.journal_scratch_peak_bytes) /
                         (1024.0 * 1024.0)
                  << " MiB\n";
        for (const std::string& message : report.mismatch_messages) {
            std::cout << "  MISMATCH " << message << "\n";
        }
        for (const std::string& message : report.messages) {
            std::cout << "  " << message << "\n";
        }
    }

    std::cout << "\nMachine progress:\n" << system.progress_report();

    for (const auto& entry : *watch_hits) {
        std::cout << "[watch] 0x" << std::hex << entry.first << std::dec
                  << " total hits " << entry.second << "\n";
    }
    if (!contexts->empty()) {
        std::cout << "\nOS thread contexts (resume address, link register):\n";
        for (const gekko::u32 context : *contexts) {
            const gekko::u32 base = context;
            const gekko::u32 srr0 = system.guest_memory().read32(base + 0x198u);
            const gekko::u32 link = system.guest_memory().read32(base + 0x84u);
            const gekko::u32 stack = system.guest_memory().read32(base + 0x04u);
            std::cout << "  context 0x" << std::hex << context << std::dec
                      << "  pc " << system.describe_address(srr0)
                      << "  lr " << system.describe_address(link)
                      << "  sp 0x" << std::hex << stack << std::dec << "\n";
            // Walk the linked EABI stack frames of this thread.
            // PowerPC EABI stores a return address in the caller frame, so the
            // link for the frame at `frame` lives at *(frame) + 4.
            gekko::u32 frame = stack;
            for (int depth = 0; depth < 14 && system.guest_memory().mapped(gekko::GcAddr(frame), 4u); ++depth) {
                const gekko::u32 parent = system.guest_memory().peek32(frame);
                if (parent <= frame || !system.guest_memory().mapped(gekko::GcAddr(parent), 8u)) {
                    break;
                }
                const gekko::u32 ret = system.guest_memory().peek32(parent + 4u);
                if (ret >= 0x80003000u && ret < 0x80600000u) {
                    std::cout << "      " << system.describe_address(ret) << "\n";
                }
                frame = parent;
            }
        }
    }


    for (const auto& request : options.dumps) {
        char header[96];
        std::snprintf(header, sizeof(header), "\nGuest memory 0x%08lX (%lu bytes):\n",
                      request.first, request.second);
        std::cout << header;
        for (unsigned long offset = 0; offset < request.second; offset += 16) {
            char row[160];
            int written = std::snprintf(row, sizeof(row), "  %08lX:", request.first + offset);
            for (unsigned long index = 0; index < 16 && offset + index < request.second; ++index) {
                const gekko::u8 value =
                    system.guest_memory().peek8(static_cast<gekko::u32>(request.first + offset + index));
                written += std::snprintf(row + written, sizeof(row) - written, " %02X", value);
            }
            std::cout << row << "\n";
        }
    }

    // Closing the window is a normal exit. It now goes through request_stop so
    // the game thread leaves translated code at a clean guest boundary, which
    // means "stopped" alone no longer means "something went wrong".
    const gekko::StopReason reason = system.cpu().stop().reason;
    const bool failed = native_renderer_failed || native_presenter_failed || !native_renderer_error.empty() || !presenter_error.empty() ||
                        (reason != gekko::StopReason::Running &&
                         reason != gekko::StopReason::HostRequest &&
                         reason != gekko::StopReason::HleExit);

    // STEP-0 measurement output (default off). One JSON object: the run's
    // identity plus one member per requested item. Every probe ran on the
    // guest thread, which has stopped, so nothing here races it.
    if (!options.measure.empty()) {
        for (const std::string& item : options.measure) {
            const bool present = std::any_of(measure_results.begin(), measure_results.end(),
                                             [&item](const auto& entry) { return entry.first == item; });
            if (!present) {
                measure_results.emplace_back(
                    item, "{\"enabled\":false,\"reason\":\"item requested but produced no result on this run\"}");
            }
        }
        const std::filesystem::path measure_path =
            options.measure_out.value_or(std::filesystem::path("measure.json"));
        std::ofstream measure_file(measure_path, std::ios::binary | std::ios::trunc);
        measure_file << "{\"run\":{\"game_ticks\":" << system.game_ticks()
                     << ",\"vi_frames\":" << system.video().frames()
                     << ",\"stop_reason\":" << static_cast<int>(reason)
                     << ",\"failed\":" << (failed ? "true" : "false")
                     << ",\"native_renderer\":true"
                     << ",\"legacy_scheduler\":" << (options.legacy_scheduler ? "true" : "false")
                     << ",\"windowed\":" << (options.window ? "true" : "false") << "}";
        for (const auto& [item, json] : measure_results) {
            measure_file << ",\"" << item << "\":" << json;
        }
        measure_file << "}\n";
        std::cout << "Measure: wrote " << measure_results.size() << " item(s) to "
                  << measure_path.string() << (measure_file.good() ? "" : " (WRITE FAILED)") << "\n";
    }

    // Machine-readable session report: every counter the text report prints,
    // in one JSON document, written on every run unless --no-json-report.
    if (options.json_report_enabled) {
        const std::filesystem::path report_path =
            options.json_report.value_or(std::filesystem::path("session-report.json"));
        char time_text[32] = "unknown";
        const std::time_t now_time = std::time(nullptr);
        std::tm now_tm{};
#ifdef _WIN32
        localtime_s(&now_tm, &now_time);
#else
        localtime_r(&now_time, &now_tm);
#endif
        std::strftime(time_text, sizeof(time_text), "%Y-%m-%d %H:%M:%S", &now_tm);

        const gekko::AudioRuntime::Report audio_report = audio_runtime.report();
        const auto& verify = system.cpu().verify_report();
        const gekko::HostJobReport jobs = gekko::host_job_report();
        const gekko::u64 interpreted = system.cpu().interpreted_instructions();
        const double game_seconds_advanced =
            static_cast<double>(system.game_ticks() - initial_game_ticks) /
            static_cast<double>(gekko::kGameTicksPerSecond);
        const double game_speed =
            execution_seconds > 0.0 ? game_seconds_advanced / execution_seconds : 0.0;
        const auto fallback = [&](gekko::FallbackReason value) {
            return static_cast<unsigned long long>(system.cpu().fallback_total(value));
        };

        // Gekko conformance census (docs/cpu-renovation/
        // GEKKO_ISA_CONFORMANCE_2026-09-03.md). Counters only: every standing
        // hash must be unchanged while these are present, and that invariance
        // is the acceptance test for the instrumentation itself.
        std::string gekko_conformance_json;
        {
            static const char* const kVectorNames[16] = {
                nullptr, "system_reset", "machine_check", "dsi", "isi",
                "external_interrupt", "alignment", "program", "fp_unavailable",
                "decrementer", nullptr, nullptr, "system_call", "trace", nullptr,
                "performance_monitor"};
            const gekko::CpuDiagCounters& diag = gekko::cpu_diag();
            std::ostringstream conf;
            conf << "  \"gekko_conformance\": {\"exceptions_total\": "
                 << diag.exceptions_total
                 << ", \"dec_edge_total\": " << diag.dec_edge_total
                 << ", \"dec_edge_suppressed_ee_clear\": "
                 << diag.dec_edge_suppressed_ee_clear
                 << ", \"dec_pending_taken\": " << diag.dec_pending_taken
                 << ", \"dec_pending_dropped_ee_clear\": "
                 << diag.dec_pending_dropped_ee_clear
                 << ", \"fpr_restore_performed\": " << diag.fpr_restore_performed
                 << ", \"fpr_restore_changed\": " << diag.fpr_restore_changed
                 << ", \"fpr_restore_changed_registers\": "
                 << diag.fpr_restore_changed_registers
                 << ", \"fpr_restore_skipped_context_switch\": "
                 << diag.fpr_restore_skipped_context_switch
                 << ", \"by_vector\": {";
            bool first_vector = true;
            for (std::size_t slot = 0; slot < 16u; ++slot) {
                if (kVectorNames[slot] == nullptr) continue;
                if (!first_vector) conf << ", ";
                first_vector = false;
                conf << "\"" << kVectorNames[slot] << "\": "
                     << diag.exceptions_by_vector[slot];
            }
            conf << "}},\n";
            gekko_conformance_json = conf.str();
        }
        const auto numeric_array_json = [](const auto& values) {
            std::ostringstream array;
            array << '[';
            const char* separator = "";
            for (const auto value : values) {
                array << separator << value;
                separator = ",";
            }
            array << ']';
            return array.str();
        };
#if !defined(SHADOW_RENDER_LIFTED)  // G5
        const auto& immediate_observed = system.immediate().stats();
        std::ostringstream graphics_observations;
        graphics_observations
            << "  \"graphics_observations\": {\"scope\": \"current_implementation_counters_not_hardware_conformance\""
            << ", \"immediate_primitives\": " << immediate_observed.primitives
            << ", \"immediate_vertices\": " << immediate_observed.vertices
            << ", \"immediate_payload_bytes\": " << immediate_observed.payload_bytes
            << ", \"immediate_empty_primitives\": " << immediate_observed.empty
            << ", \"unlisted_gx_pipe_stores\": " << system.cpu().unlisted_gx_stores()
            << "},\n";
#else
        // G5: the lifted build has no GX immediate path (HW_REPORT T); its counters are not reported.
        std::ostringstream graphics_observations;
        graphics_observations
            << "  \"graphics_observations\": {\"scope\": \"current_implementation_counters_not_hardware_conformance\""
            << ", \"unlisted_gx_pipe_stores\": " << system.cpu().unlisted_gx_stores()
            << "},\n";
#endif
        // The resolution state in one place (docs/RESOLUTION_AUDIT.md): the physical render size (0 x 0: the
        // window's size), its family by its actual ratio, the logical game raster, the game-visible output, the
        // window mode and the primary display's native mode and refresh.
        std::string resolution_json;
#if defined(SHADOW_RENDER_LIFTED)
        {
            const std::uint32_t rw = lifted_render.render_width(), rh = lifted_render.render_height();
            const shadow::render::AspectFamily family = shadow::render::aspect_family(rw, rh);
            std::string display_error;
            const std::vector<shadow::render::DisplayOutput> outputs =
                shadow::render::enumerate_displays(&display_error);
            const shadow::render::DisplayOutput* monitor = nullptr;
            for (const shadow::render::DisplayOutput& o : outputs)
                if (monitor == nullptr || o.primary) monitor = &o;
            std::ostringstream r;
            r << "  \"resolution\": {\"render_width\": " << rw << ", \"render_height\": " << rh
              << ", \"aspect\": " << (rh != 0u ? static_cast<double>(rw) / rh : 0.0) << ", \"family\": \""
              << family.name << "\", \"family_exact\": " << (family.exact ? "true" : "false")
              << ", \"asked\": \"" << display.render_width << "x" << display.render_height
              << "\", \"logical_raster\": \"640x480 (PAL 640x528) game canvas units\", \"game_output\": \""
              << game_output_w << "x" << game_output_h << "\", \"window_mode\": \""
              << (!options.window ? "headless" : display.fullscreen ? "borderless-fullscreen" : "windowed")
              << "\", \"window\": \"" << display.window_width << "x" << display.window_height << "\"";
            if (monitor != nullptr)
                r << ", \"monitor\": \"" << monitor->monitor << "\", \"monitor_device\": \"" << monitor->device
                  << "\", \"monitor_native\": \"" << monitor->desktop_width << "x" << monitor->desktop_height
                  << "\", \"monitor_refresh_hz\": " << monitor->desktop_refresh_hz;
            r << "},\n";
            resolution_json = r.str();
        }
#endif
        std::ostringstream json;
        json.setf(std::ios::fixed);
        json.precision(6);
        json << "{\n"
             << "  \"schema\": 1,\n"
             << "  \"timestamp\": \"" << time_text << "\",\n"
             << "  \"stop\": {\"reason\": " << static_cast<int>(reason)
             << ", \"failed\": " << (failed ? "true" : "false") << "},\n"
             << "  \"run\": {\"budget_ms\": " << options.budget_ms
             << ", \"window\": " << (options.window ? "true" : "false")
             << ", \"native_renderer\": true"
             << ", \"native_renderer_failed\": " << (native_renderer_failed ? "true" : "false")
             << ", \"native_presenter_failed\": " << (native_presenter_failed ? "true" : "false")
             << ", \"verify_recomp\": " << (options.verify_recompiled ? "true" : "false")
             << ", \"reference_execution\": " << (reference_execution ? "true" : "false")
              << "},\n"
              << "  \"capture\": {\"native_screenshot_requested\": "
              << (options.native_screenshot.has_value() ? "true" : "false") << "},\n"
              << graphics_observations.str()
              << "  \"host\": {\"wall_seconds\": " << execution_seconds
              << ", \"process_cpu_seconds\": " << process_cpu_seconds
              << ", \"average_logical_cores\": "
              << (execution_seconds > 0.0 ? process_cpu_seconds / execution_seconds : 0.0)
              // Guest-thread accounting, headless path only (-1 = not measured
              // on this path). guest_run_wall minus guest_blocked is the time
              // the guest is BELIEVED to have been executing; guest_run_cpu is
              // the time it demonstrably was. The difference is waiting that
              // the guest_blocked taxonomy does not name.
              << ", \"guest_run_wall_seconds\": " << g_guest_run_wall_seconds
              << ", \"guest_run_cpu_seconds\": " << g_guest_run_cpu_seconds
              << ",\n    \"threads\": " << threads_json
              << "},\n"
              << gekko_conformance_json
              << "  \"guest\": {\"game_ticks\": " << system.game_ticks()
              << ", \"initial_game_ticks\": " << initial_game_ticks
              << ", \"game_seconds\": " << game_seconds_advanced
              << ", \"execution_seconds\": " << execution_seconds
             << ", \"game_speed\": " << game_speed
             << ", \"idle_waits\": " << system.idle_waits()
             << ", \"poll_fixed_points\": " << gekko::cpu_diag().poll_fixed_points
             << ", \"poll_points\": " << gekko::cpu_diag().poll_points
             << ", \"host_services\": " << system.host_services()
             << ", \"limiter_frames\": " << system.frame_limiter().stats().frames
             << ", \"native_entries\": " << system.cpu().recompiled_calls
             << ", \"interpreted_instructions\": " << interpreted
             << ", \"interpreter_ms\": "
             << static_cast<double>(system.cpu().interpreted_ns()) / 1.0e6
             << ", \"interpreter_strand_origins\": "
             << system.cpu().interpreter_strands().size()
             << ", \"dispatch_blocked_interrupt\": " << system.cpu().dispatch_blocked_interrupt
             << ", \"dispatch_blocked_hle\": " << system.cpu().dispatch_blocked_hle
             << ", \"dispatch_no_native_code\": " << system.cpu().dispatch_no_native_code
             << ", \"dispatch_mid_function\": " << system.cpu().dispatch_entered_mid_function
             << ", \"dispatch_table_bytes\": "
             << (system.cpu().recompiled() != nullptr
                     ? system.cpu().recompiled()->table_bytes()
                     : 0u)
             << ",\n    \"boundaries\": {\"not_recompiled\": "
             << fallback(gekko::FallbackReason::NotRecompiled)
             << ", \"sc\": " << fallback(gekko::FallbackReason::SystemCall)
             << ", \"rfi\": " << fallback(gekko::FallbackReason::ReturnFromInterrupt)
             << ", \"trap\": " << fallback(gekko::FallbackReason::Trap)
             << ", \"unexpected_return\": " << fallback(gekko::FallbackReason::UnexpectedReturn)
             << ", \"past_function\": " << fallback(gekko::FallbackReason::EndOfFunction)
             << ", \"yield\": " << fallback(gekko::FallbackReason::Yield)
              << ", \"verify_cut\": " << fallback(gekko::FallbackReason::VerifyCut) << "}},\n"
              << "  \"pacing\": {\"game_clock_hz\": " << gekko::kGameTicksPerSecond
              << ", \"fixed_quantum_hz\": 60"
              << ", \"maximum_catchup_rate\": "
              << simulation_clock.config().maximum_catchup_rate
              << ", \"game_seconds\": " << game_seconds_advanced
              << ", \"resyncs\": " << pacing_resyncs
              << ", \"debt_clamps\": " << pacing_debt_clamps
              << ", \"boundaries\": " << pacing_boundaries
              << ", \"skipped_boundaries\": " << pacing_skipped_ticks
              << ", \"maximum_debt_ms\": " << pacing_maximum_debt_ms
              << ", \"hitches_forgiven\": " << simulation_clock.stats().hitches_forgiven
              << ", \"hitch_ms_forgiven\": "
              << std::chrono::duration<double, std::milli>(
                     simulation_clock.stats().hitch_time_forgiven).count()
              // Headroom as a split, not a lifetime sleep total: unpaced
              // services are the ones where the guest was at or behind real
              // time and the pacer had nothing to give back.
              << ", \"paced_services\": " << pacing_paced_services
              << ", \"unpaced_services\": " << pacing_unpaced_services
              << ", \"thread_priority\": " << game_thread_policy.effective_priority
              << ", \"above_normal\": "
              << (game_thread_policy.priority_applied ? "true" : "false")
              << ", \"dynamic_boosts\": "
              << (game_thread_policy.priority_boosts_enabled ? "true" : "false")
              << ", \"execution_speed_throttling_disabled\": "
              << (game_thread_policy.execution_speed_throttling_disabled ? "true" : "false")
              << ", \"game_thread_policy_applied\": " << (game_thread_policy.applied ? "true" : "false")
              << ", \"process_throttling_disabled\": "
              << (game_thread_policy.process_throttling_disabled ? "true" : "false")
              << ", \"placement_samples\": " << game_thread_placement.samples()
              << ", \"placement_performance_samples\": " << game_thread_placement.performance_samples()
              << ", \"placement_hybrid\": " << (game_thread_placement.hybrid() ? "true" : "false")
              << ", \"placement_seen_mask\": " << game_thread_placement.seen_mask()
              << "},\n"
              << "  \"verification\": {\"enabled\": "
             << (options.verify_recompiled ? "true" : "false")
             << ", \"segments_matched\": " << verify.verified
             << ", \"mismatched\": " << verify.mismatches
             << ", \"skipped_hardware\": " << verify.skipped_device
             << ", \"skipped_not_returned\": " << verify.skipped_exit
             << ", \"verified_instructions\": " << verify.verified_instructions
             << ", \"native_journal_peak_entries\": "
             << verify.native_journal_peak_entries
             << ", \"interpreter_journal_peak_entries\": "
             << verify.interpreter_journal_peak_entries
             << ", \"unique_native_bytes_peak\": " << verify.unique_native_bytes_peak
             << ", \"journal_scratch_peak_mib\": "
             << static_cast<double>(verify.journal_scratch_peak_bytes) / (1024.0 * 1024.0)
             << "},\n"
#if defined(SHADOW_RENDER_LIFTED)
             << "  \"renderer\": {\"backend\": \"lifted\"},\n"
             << resolution_json
#else
             << "  \"renderer\": {\"backend\": \"native\""
             << ", \"internal_scale\": " << internal_scale
             << ", \"internal_scale_auto\": " << (options.internal_scale_auto ? "true" : "false")
             << "},\n"
#endif
#if defined(SHADOW_RENDER_LIFTED)
             << "  \"presentation\": " << lifted_render.presentation_json() << ",\n"
#else
             << "  \"presentation\": {\"available\": "
             << (presentation_summary.available ? "true" : "false")
             << ", \"intervals\": " << presentation_summary.intervals
              << ", \"average_fps\": " << presentation_summary.average_fps
              << ", \"minimum_fps\": " << presentation_summary.minimum_fps
              << ", \"one_percent_low_fps\": " << presentation_summary.one_percent_low
              << ", \"target_fps\": " << presentation_summary.target_fps
              << ", \"interpolation\": "
              << (presentation_summary.interpolation ? "true" : "false")
              << ", \"blend_presents\": " << presentation_summary.blend_presents
              << ", \"interpolation_toggles\": " << presentation_summary.interpolation_toggles
              << ", \"native_previous_misses\": " << presentation_summary.native_previous_misses
              << ", \"native_acquire_misses\": " << presentation_summary.native_acquire_misses
              << ", \"present_fifo\": " << (presentation_summary.present_fifo ? "true" : "false")
              << ", \"fifo_deferred_presents\": " << presentation_summary.fifo_deferred_presents
              << ", \"fifo_lag_drops\": " << presentation_summary.fifo_lag_drops
              << ", \"fifo_fallbacks\": " << presentation_summary.fifo_fallbacks
              << ", \"rendered_frames\": " << presentation_summary.rendered_frames
              << ", \"dropped_render_deadlines\": "
              << presentation_summary.dropped_render_deadlines
             << ",\n    \"game_thread\": {\"execute_s\": " << presentation_summary.execute_s
             << ", \"submit_s\": " << presentation_summary.submit_s
             << ", \"retire_s\": " << presentation_summary.retire_s
             << ", \"sleep_s\": " << presentation_summary.sleep_s
             << "},\n    \"ui_present_s\": " << presentation_summary.present_s
             << ",\n    \"stalls\": {\"game_thread\": "
             << presentation_summary.game_thread_stalls
             << ", \"longest_game_ms\": " << presentation_summary.longest_game_stall_ms
             << ", \"frame_production\": " << presentation_summary.frame_production_stalls
             << ", \"longest_frame_ms\": " << presentation_summary.longest_frame_stall_ms
              << ", \"no_gx_spells\": " << presentation_summary.no_gx_spells
              << ", \"longest_no_gx_ms\": " << presentation_summary.longest_no_gx_ms
              << "}},\n"
#endif
              << "  \"audio\": {\"active\": "
              << (audio_report.device_open ? "true" : "false")
              << ", \"mode\": " << static_cast<int>(audio_report.mode)
              << ", \"blocks\": " << audio_report.blocks
              << ", \"produced_samples\": " << audio_report.produced_frames
              << ", \"produced_seconds\": "
              << (audio_report.source_rate_hz > 0.0
                      ? static_cast<double>(audio_report.produced_frames) / audio_report.source_rate_hz
                      : 0.0)
              << ", \"silence_frames\": " << audio_report.silence_frames
              << ", \"dropped_frames\": " << audio_report.dropped_frames
              << ", \"dropped_events\": " << audio_report.dropped_events
              << ", \"timeline_resets\": " << audio_report.reset_epochs
              << ", \"consumed_frames\": " << audio_report.consumed_frames
              << ", \"output_frames\": " << audio_report.output_frames
              << ", \"underrun_events\": " << audio_report.underrun_events
              << ", \"underrun_frames\": " << audio_report.underrun_frames
              << ", \"prime_silence_frames\": " << audio_report.prime_silence_frames
              << ", \"reset_dropped_frames\": " << audio_report.reset_dropped_frames
              << ", \"device_gap_discarded_frames\": " << audio_report.device_gap_discarded_frames
              << ", \"clipped_samples\": " << audio_report.clipped_samples
              << ", \"ring_capacity_frames\": " << audio_report.ring_capacity_frames
              << ", \"ring_max_fill_frames\": " << audio_report.ring_max_fill_frames
              << ", \"start_fill_frames\": " << audio_report.start_fill_frames
              << ", \"starts\": " << audio_report.starts
              << ", \"reopens\": " << audio_report.reopens
              << ", \"device_invalidations\": " << audio_report.device_invalidations
              << ", \"open_failures\": " << audio_report.open_failures
              << ", \"render_errors\": " << audio_report.render_errors
              << ", \"wakes\": " << audio_report.wakes
              << ", \"late_wakes\": " << audio_report.late_wakes
              << ", \"max_wake_interval_us\": " << audio_report.max_wake_interval_us
              << ", \"max_render_us\": " << audio_report.max_render_us
              << ", \"device_rate\": " << audio_report.device_rate
              << ", \"device_period_ms\": " << audio_report.device_period_ms << "},\n"
              << "  \"host_jobs\": {\"workers\": " << jobs.logical_workers
             << ", \"groups\": " << jobs.parallel_groups
             << ", \"worker_chunks\": " << jobs.worker_chunks
             << ", \"items\": " << jobs.items
             << ", \"hardware_threads\": " << jobs.hardware_threads
             << ", \"requested_workers\": " << jobs.requested_workers
             << ", \"queue_slot_limit\": " << (jobs.logical_workers - 1u)
             << ", \"queue_capacity\": " << jobs.queue_capacity
             << ", \"queue_peak\": " << jobs.queue_peak
             << ", \"caller_help_chunks\": " << jobs.caller_help_chunks << "}"
             ;
        {
            const gekko::HostPageStats& stats = gekko::host_page_stats();
            json << ",\n  \"host_stats\": {\"schema\": 3"
                 << ", \"memory\": {\"working_set_mib\": " << memory_working_set_mib
                 << ", \"peak_working_set_mib\": " << memory_peak_working_set_mib
                 << ", \"private_mib\": " << memory_private_mib
                 << ", \"peak_private_mib\": " << memory_peak_private_mib << "}"
                 << ", \"quantum\": " << histogram_json(quantum_hist)
                 << ", \"quanta_over_17ms\": " << quantum_hist.count_at_or_above(17000000u)
                 << ", \"service_cost\": " << histogram_json(service_cost_hist)
#if defined(SHADOW_RENDER_LIFTED)
                 << ", \"presentation\": null"  // the active owner reports at the top level
#else
                 << ", \"presentation\": {\"p50_ms\": " << presentation_summary.p50_ms
                 << ", \"p95_ms\": " << presentation_summary.p95_ms
                 << ", \"p99_ms\": " << presentation_summary.p99_ms
                 << ", \"p999_ms\": " << presentation_summary.p999_ms
                 << ", \"max_ms\": " << presentation_summary.max_ms
                 << ", \"unique_presents\": " << unique_presents
                 << ", \"repeat_presents\": " << repeat_presents
                 << ", \"gaps_over_33ms\": " << present_gaps
                 << ", \"unique_interval\": " << histogram_json(unique_present_hist) << "}"
#endif
                 << ", \"memory_card\": {\"reads\": " << system.card().stats().reads
                 << ", \"writes\": " << system.card().stats().writes
                 << ", \"erases\": " << system.card().stats().erases
                 << ", \"overwritten_bytes\": " << system.card().stats().overwritten_bytes
                 << ", \"host_persists\": " << system.card().stats().persists
                 << ", \"host_persists_deferred\": " << system.card().stats().persists_deferred
                 << ", \"host_persist_ms\": " << static_cast<double>(system.card().stats().persist_ns) / 1.0e6
                 << ", \"host_persist_max_ms\": " << static_cast<double>(system.card().stats().persist_ns_max) / 1.0e6
                 << ", \"tick_wired\": " << (system.card().persist_tick_wired() ? "true" : "false")
                 << ", \"failures\": " << system.card().stats().persist_failures << "}"
                 << ", \"host_pages\": {\"installs\": " << stats.installs
                 << ", \"invalidations\": " << stats.invalidations
                 << ", \"invalidation_ms\": " << static_cast<double>(stats.invalidation_ns) / 1.0e6
                 << ", \"bus_invalidation_requests\": " << system.guest_memory().translation_invalidations() << "}"
                 // R12: every published LK=0 transfer run by Cpu::finish_transfers.
                 << ", \"native_transfers_driven\": " << system.cpu().native_transfers_driven
                 << ", \"audio\": {\"underrun_events\": " << audio_report.underrun_events
                 << ", \"underrun_frames\": " << audio_report.underrun_frames
                 << ", \"device_period_frames\": " << audio_report.device_period_frames
                 << ", \"queue_histogram_5ms\": [";
            for (std::size_t bucket = 0; bucket < audio_report.fill_histogram.size(); ++bucket) {
                if (bucket != 0u) json << ',';
                json << audio_report.fill_histogram[bucket];
            }
            json << "]}}";
        }
        {
            const gekko::assets::AssetSystem::Stats reads = system.assets().stats();
            const gekko::assets::FileProvider::Stats files = system.assets().files().stats();
            json << ",\n  \"assets\": {\"reads\": " << reads.reads
                 << ", \"bytes\": " << reads.bytes
                 << ", \"read_ns\": " << reads.read_ns
                 << ", \"read_ns_max\": " << reads.read_ns_max
                 << ", \"files\": " << system.assets().report().files
                 << ", \"mapped\": " << files.mapped
                 << ", \"mapped_bytes\": " << files.mapped_bytes
                 << ", \"map_failures\": " << files.failures
                 << ", \"address_span\": " << system.assets().report().address_span
                 << ", \"unmapped\": " << reads.unmapped
                 << ", \"out_of_range\": " << reads.out_of_range
                 << ", \"failed\": " << reads.failed << "}";
        }
        json << ",\n  \"native_memory\": " << shadow::memory::system().report_json()
             << ",\n  \"guest_memory\": {\"regions\": " << system.guest_memory().regions().size()
             << ", \"committed_bytes\": " << system.guest_memory().committed_bytes()
             << ", \"reserved_bytes\": " << system.guest_memory().reserved_bytes() << "}";
        json << "\n}\n";

        std::ofstream out(report_path, std::ios::binary | std::ios::trunc);
        if (out) {
            const std::string text = json.str();
            out.write(text.data(), static_cast<std::streamsize>(text.size()));
            std::cout << "Session report written: " << report_path.string() << "\n";
        } else {
            std::cerr << "WARNING: could not write session report "
                      << report_path.string() << "\n";
        }
    }

    return failed ? 5 : 0;
}

// Kept out of parse_options, and folded into ONE else-if branch there: that
// argument chain is at MSVC's block-nesting limit (C1061), so neither an
// inline ladder nor a second branch compiles.

// The native renderer is the only renderer; it needs Direct3D 12.
void require_native_renderer() {
#if !defined(SHADOW_RENDER_LIFTED)  // G5: the lifted build's renderer checks its own device (LiftedRender::start)
    if (!gekko::NativeGpu::available()) {
        throw std::runtime_error("The renderer requires native Direct3D 12; there is no other renderer");
    }
#endif
}

Options parse_options(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument(argv[index]);
#if !defined(SHADOW_REFERENCE_RUNNER)
        // Refuse oracle controls before constructing runtime/device/file owners.
        // These controls cannot silently change the production backend.
        const char* reference_only[] = {
            "--legacy-scheduler", "--legacy-health-bar", "--native-no-gpu", "--verify-plume",
            "--native-xfb-present", "--no-native-xfb-present",
        };
        for (const char* control : reference_only) {
            if (argument == control) {
                std::cerr << "ERROR: " << argument
                          << " is reference-only; use ShadowTheHedgehogReference\n";
                std::exit(2);
            }
        }
#endif
#if defined(GEKKO_SHIPPING)
        // SHADOW_SHIPPING (ES 2026-10-20): development / verification-only controls do not exist in the user's
        // exe. Refused before any owner is constructed, like the reference-only controls above. The user's own
        // tooling (input tapes, savestates, --json-report, --boot-stage, --memory-card, --budget-ms) stays.
        const char* development_only[] = {
            "--dev-link", "--dev-link-name", "--start-paused", "--ui-test", "--ui-test-script",
            "--asset-trace", "--file-origin-trace", "--resource-lineage", "--native-boot-contract-dir",
            "--measure", "--measure-out", "--measure-probes", "--visibility-probe", "--fov-probe", "--ui-probe",
        };
        for (const char* control : development_only) {
            if (argument == control) {
                std::cerr << "ERROR: " << argument
                          << " is a development control; it is not part of this build (SHADOW_SHIPPING)\n";
                std::exit(2);
            }
        }
#endif

        // The comparison workstation's controls (docs/comparator/README.md), before the chain for the same C1061
        // reason as the options below.
        if (argument == "--dev-link" && index + 1 < argc) {
            options.dev_link = std::string(argv[++index]);
            continue;
        }
        if (argument == "--dev-link-name" && index + 1 < argc) {
            options.dev_link_name = argv[++index];
            continue;
        }
        if (argument == "--start-paused") {
            options.start_paused = true;
            continue;
        }
        if (argument == "--input-tape-record" && index + 1 < argc) {
            options.input_tape_record = std::filesystem::path(argv[++index]);
            continue;
        }
        if (argument == "--input-tape-play" && index + 1 < argc) {
            options.input_tape_play = std::filesystem::path(argv[++index]);
            continue;
        }
        if (argument == "--stop-at-tick" && index + 1 < argc) {
            options.stop_at_tick = std::strtoull(argv[++index], nullptr, 10);
            continue;
        }
        if (argument == "--state-report" && index + 1 < argc) {
            options.state_report = std::filesystem::path(argv[++index]);
            continue;
        }
        if (argument == "--report-peek" && index + 1 < argc) {
            options.report_peeks.push_back(argv[++index]);
            continue;
        }
        if (argument == "--marker-key" && index + 1 < argc) {
            options.marker_key = argv[++index];
            if (snapshot_virtual_key(options.marker_key) < 0) {
                throw std::invalid_argument("--marker-key wants F1..F24, a letter, a digit or none, got '" +
                                            options.marker_key + "'");
            }
            continue;
        }
        if (argument == "--range-key" && index + 1 < argc) {
            options.range_key = argv[++index];
            if (snapshot_virtual_key(options.range_key) < 0) {
                throw std::invalid_argument("--range-key wants F1..F24, a letter, a digit or none, got '" +
                                            options.range_key + "'");
            }
            continue;
        }
        if (argument == "--marker-file" && index + 1 < argc) {
            options.marker_file = std::filesystem::path(argv[++index]);
            continue;
        }
        if (argument == "--snapshot-key" && index + 1 < argc) {
            options.snapshot_key = argv[++index];
            if (snapshot_virtual_key(options.snapshot_key) < 0) {
                throw std::invalid_argument("--snapshot-key wants F1..F24, a letter, a digit or none, got '" +
                                            options.snapshot_key + "'");
            }
            continue;
        }
        if (argument == "--snapshot-dir" && index + 1 < argc) {
            options.snapshot_dir = std::filesystem::path(argv[++index]);
            continue;
        }
        if ((argument == "--save-state-at-tick" || argument == "--capture-at-tick") && index + 1 < argc) {
            // <game tick>:<file> - the tick is digits, so the first ':' ends it (a drive letter's comes later).
            const std::string spec = argv[++index];
            const std::size_t colon = spec.find(':');
            char* end = nullptr;
            const gekko::u64 tick = std::strtoull(spec.c_str(), &end, 10);
            if (colon == std::string::npos || colon + 1u >= spec.size() || end != spec.c_str() + colon || tick == 0u) {
                throw std::invalid_argument(argument + " wants <game tick>:<file>, got '" + spec + "'");
            }
            const std::filesystem::path file(spec.substr(colon + 1u));
            if (argument == "--save-state-at-tick") {
                options.save_state_at_ticks.emplace_back(tick, file);
                std::sort(options.save_state_at_ticks.begin(), options.save_state_at_ticks.end());
            } else {
                options.capture_at_tick = std::make_pair(tick, file);
            }
            continue;
        }
        if (argument == "--stop-after-capture") {
            options.stop_after_capture = true;
            continue;
        }

        // Handled before the chain, not inside it: parse_options' else-if
        // ladder is at MSVC's block-nesting limit (C1061) and one more
        // branch does not compile.
        // Handled before the chain for the same C1061 reason as the options
        // below it: the else-if ladder is at MSVC's block-nesting limit.
        if (argument == "--legacy-health-bar" || argument == "--native-health-bar") {
#if defined(SHADOW_REFERENCE_RUNNER)
            options.legacy_health_bar = argument == "--legacy-health-bar";
#endif
            continue;
        }
        if (argument == "--effects-off" && index + 1 < argc) {
            options.effects_off = argv[++index];
            continue;
        }
        if (argument == "--effects-solo" && index + 1 < argc) {
            options.effects_solo = argv[++index];
            continue;
        }
        if (argument == "--fps-counter") {
            options.fps_counter = true;
            options.fps_counter_explicit = true;
            continue;
        }
        if (argument == "--effects-isolate") {
            options.effects_isolate = true;
            continue;
        }
        if (argument == "--draw-distance" && index + 1 < argc) {
            // Pre-chain for the same C1061 reason as the options below it.
            char* end = nullptr;
            const float value = std::strtof(argv[++index], &end);
            if (end == nullptr || *end != '\0' || !(value >= 1.0f && value <= 8.0f)) {
                std::cerr << "ERROR: --draw-distance expects a scale from 1 to 8 (1 = the original game)\n";
                std::exit(2);
            }
            options.draw_distance = value;
            options.draw_distance_explicit = true;
            continue;
        }
        if (argument == "--fade-in-ms" && index + 1 < argc) {
            // Pre-chain for the same C1061 reason as the options below it.
            char* end = nullptr;
            const unsigned long value = std::strtoul(argv[++index], &end, 10);
            if (end == nullptr || *end != '\0' || value > 5000ul) {
                std::cerr << "ERROR: --fade-in-ms expects 0 (off) to 5000\n";
                std::exit(2);
            }
            options.fade_in_ms = static_cast<gekko::u32>(value);
            continue;
        }
        if (argument == "--native-visibility" || argument == "--no-native-visibility") {
            // Pre-chain for the same C1061 reason as the options below it.
            options.native_visibility = argument == "--native-visibility";
            continue;
        }
        if (argument == "--fpr-shadow") {
            options.fpr_shadow = true;
            continue;
        }
        if (argument == "--no-fpr-shadow") {
            options.fpr_shadow = false;
            continue;
        }
        if (argument == "--compiler-intrinsics" || argument == "--no-compiler-intrinsics") {
            std::cerr << "ERROR: " << argument << " is gone: the tick/time-base intrinsics served a "
                         "spin the native frame limiter no longer makes\n";
            std::exit(2);
        }
        if (argument == "--renderer" && index + 1 < argc) {
            // The native renderer is the only renderer; naming it is accepted.
            if (std::string(argv[++index]) != "native") {
                std::cerr << "ERROR: --renderer expects native (the only renderer)\n";
                std::exit(2);
            }
            continue;
        }
        if (argument == "--divide-intrinsics") {
            options.divide_intrinsics = true;
            continue;
        }
        if (argument == "--no-divide-intrinsics") {
            options.divide_intrinsics = false;
            continue;
        }
        if (argument == "--no-dynamic-cast-intrinsic") {
            options.dynamic_cast_intrinsic = false;
            continue;
        }
        if (argument == "--no-matrix-multiply-intrinsic") {
            options.matrix_multiply_intrinsic = false;
            continue;
        }
        if (argument == "--verify-matrix-multiply") {
            options.verify_matrix_multiply = true;
            continue;
        }
        if (argument == "--no-chara-colli-intrinsic") {
            options.chara_colli_intrinsic = false;
            continue;
        }
        if (argument == "--chara-colli-bone-no-memo") {
            options.chara_colli_bone_memo = false;
            continue;
        }
        if (argument == "--chara-colli-bone-guest") {
            options.chara_colli_bone_native = false;
            continue;
        }
        if (argument == "--trace-stale-contacts") {
            options.trace_stale_contacts = true;
            continue;
        }
        if (argument == "--chara-colli-pass") {
            options.chara_colli_pass = true;
            continue;
        }
        if (argument == "--no-chara-colli-manage") {
            options.chara_colli_manage = false;
            continue;
        }
        if (argument == "--no-chara-colli-loops") {
            options.chara_colli_loops = false;
            continue;
        }
        if (argument == "--verify-chara-colli") {
            options.verify_chara_colli = true;
            continue;
        }
        if (argument == "--asset-trace" || argument == "--file-origin-trace") {
            if (index + 1 < argc) {
                options.asset_trace = std::filesystem::path(argv[++index]);
            }
            continue;
        }
        if (argument == "--resource-lineage") {
            if (index + 1 < argc) {
                options.resource_lineage = argv[++index];
            }
            continue;
        }
        if (argument == "--list-stages") {
            options.list_stages = true;
            continue;
        }
        if (argument == "--native-renderer") {
            // The native renderer is the only renderer; naming it is accepted.
            continue;
        }
        if (argument == "--native-dump" && index + 1 < argc) {
            options.native_dump = std::filesystem::path(argv[++index]);
            continue;
        }
        if (argument == "--native-dump-frames" && index + 1 < argc) {
            const std::string value = argv[++index];
            const std::size_t colon = value.find(':');
            options.native_dump_first = static_cast<gekko::u32>(std::stoul(value.substr(0, colon)));
            options.native_dump_last = colon == std::string::npos
                                           ? options.native_dump_first
                                           : static_cast<gekko::u32>(std::stoul(value.substr(colon + 1)));
            continue;
        }
        if (argument == "--native-no-gpu") {
            options.native_gpu = false;
            continue;
        }
        if (argument == "--framing" && index + 1 < argc) {
            const std::string value = argv[++index];
            if (value != "square" && value != "game") {
                std::cerr << "ERROR: --framing expects square or game" << std::endl;
                std::exit(2);
            }
            options.game_framing = value == "game";
            continue;
        }
        if (argument == "--native-output" && index + 1 < argc) {
            const std::string value = argv[++index];
            const std::size_t x = value.find('x');
            if (x == std::string::npos) {
                std::cerr << "ERROR: --native-output expects WIDTHxHEIGHT" << std::endl;
                std::exit(2);
            }
            options.native_output_width = static_cast<gekko::u32>(std::stoul(value.substr(0, x)));
            options.native_output_height = static_cast<gekko::u32>(std::stoul(value.substr(x + 1)));
            options.native_output_explicit = true;
            continue;
        }
        if (argument == "--native-screenshot" && index + 1 < argc) {
            options.native_screenshot = std::filesystem::path(argv[++index]);
            continue;
        }
        if (argument == "--native-screenshot-frame" && index + 1 < argc) {
            options.native_screenshot_frame = static_cast<gekko::u32>(std::stoul(argv[++index]));
            continue;
        }
        if (argument == "--native-screenshot-stride" && index + 1 < argc) {
            options.native_screenshot_stride = static_cast<gekko::u32>(std::stoul(argv[++index]));
            continue;
        }
        if (argument == "--native-screenshot-count" && index + 1 < argc) {
            options.native_screenshot_count = static_cast<gekko::u32>(std::stoul(argv[++index]));
            continue;
        }
        if (argument == "--dump-failed-geometry" && index + 1 < argc) {
            options.geometry_failure_dump = std::filesystem::path(argv[++index]);
            continue;
        }
        if (argument == "--native-copy-dump" && index + 2 < argc) {
            options.native_copy_dump = std::filesystem::path(argv[++index]);
            options.native_copy_dump_frame = static_cast<gekko::u32>(std::stoul(argv[++index]));
            continue;
        }
        if (argument == "--native-validate-textures") {
            options.native_validate_textures = true;
            continue;
        }
        if (argument == "--no-effect-footprint") {
            // A/B only: point-sample resident EFB copies instead of
            // reconstructing the guest-texel footprint (the glare ladder).
            options.effect_footprint = false;
            continue;
        }
        if (argument == "--legacy-scheduler") {
            // A/B comparison only: the guest's own task traversal instead of
            // the native scheduler. Default is the native scheduler.
#if defined(SHADOW_REFERENCE_RUNNER)
            options.legacy_scheduler = true;
#endif
            continue;
        }
        if (argument == "--no-native-assets") {
            options.native_assets = false;
            continue;
        }
        if (argument == "--native-frames-in-flight" && index + 1 < argc) {
            options.native_frames_in_flight = static_cast<gekko::u32>(std::stoul(argv[++index]));
            continue;
        }
        if (argument == "--boot-stage") {
            std::string value = index + 1 < argc ? argv[++index] : std::string();
            if (value.rfind("stg", 0) == 0) value = value.substr(3);
            const bool digits = !value.empty() && value.size() <= 4 &&
                                std::all_of(value.begin(), value.end(),
                                            [](char c) { return c >= '0' && c <= '9'; });
            if (!digits) {
                std::cerr << "ERROR: --boot-stage expects stgNNNN or NNNN (decimal)"
                          << std::endl;
                std::exit(2);
            }
            options.boot_stage = static_cast<gekko::u32>(std::stoul(value, nullptr, 10));
            continue;
        }
        if (argument == "--boot-story") {
            options.boot_story = true;
            continue;
        }
        if (argument == "--boot-flashback") {
            options.boot_flashback = true;
            continue;
        }
        if (argument == "--boot-event") {
            std::string value = index + 1 < argc ? argv[++index] : std::string();
            if (value.rfind("event", 0) == 0) value = value.substr(5);
            const bool digits = !value.empty() && value.size() <= 4 &&
                                std::all_of(value.begin(), value.end(),
                                            [](char c) { return c >= '0' && c <= '9'; });
            if (!digits) {
                std::cerr << "ERROR: --boot-event expects eventNNNN or NNNN (decimal)" << std::endl;
                std::exit(2);
            }
            options.boot_event = static_cast<gekko::u32>(std::stoul(value, nullptr, 10));
            continue;
        }
        if (argument == "--present-fifo") {
            options.present_fifo = true;
            continue;
        }
        if (argument == "--no-present-fifo") {
            options.present_fifo = false;
            continue;
        }
        if (argument == "--hitch-forgiveness-ms" && index + 1 < argc) {
            options.hitch_forgiveness_ms = std::strtod(argv[++index], nullptr);
            if (!std::isfinite(options.hitch_forgiveness_ms) ||
                options.hitch_forgiveness_ms < 0.0) {
                std::cerr << "--hitch-forgiveness-ms needs a non-negative number (0 = off)\n";
                std::exit(1);
            }
            continue;
        }
        if (argument == "--present-dump-frame" && index + 1 < argc) {
            options.present_dump_frame = std::stoi(argv[++index]);
            continue;
        }
        // Host tuning switches. Handled here rather than in the else-if chain
        // below: that chain is already at MSVC's block-nesting limit, and a
        // standalone match with continue adds no nesting to it.
        if (argument == "--tune" && index + 1 < argc) {
            std::string spec = argv[++index];
            bool value = true;
            const std::size_t equals = spec.find('=');
            if (equals != std::string::npos) {
                const std::string text = spec.substr(equals + 1);
                spec = spec.substr(0, equals);
                if (text == "0" || text == "off" || text == "false") {
                    value = false;
                } else if (text != "1" && text != "on" && text != "true") {
                    std::cerr << "ERROR: --tune " << spec << '=' << text
                              << " expects 0 or 1.\n";
                    std::exit(2);
                }
            }
            if (find_tune_entry(spec) == nullptr) {
                std::cerr << "ERROR: unknown --tune name '" << spec
                          << "'. Use --tune-list to see them.\n";
                std::exit(2);
            }
            options.tuning.emplace_back(spec, value);
            continue;
        }
        if (argument == "--tune-list") {
            std::cout << "Host tuning switches (--tune <name>=0|1; all default 1 unless noted):\n";
            for (const TuneEntry& entry : kTuneEntries) {
                char row[160];
                std::snprintf(row, sizeof(row), "  %-28s %s\n", entry.name, entry.what);
                std::cout << row;
            }
            std::exit(0);
        }
#if defined(SHADOW_RENDER_LIFTED)
        if (argument == "--list-displays") {
            // The displays (DXGI outputs, desktop size and refresh, supported modes) and the resolution presets, as
            // JSON, for the launcher's display settings (render/display_modes.h).
            std::string error;
            const std::vector<shadow::render::DisplayOutput> outputs = shadow::render::enumerate_displays(&error);
            std::cout << shadow::render::displays_json(outputs, error);
            std::exit(outputs.empty() ? 1 : 0);
        }
        if (argument == "--list-effect-layers") {
            // The effect layer names (render/effect_layers.h), comma separated, for the launcher.
            std::cout << gekko::LiftedRender::effect_layer_names() << '\n';
            std::exit(0);
        }
#endif
        if (argument == "--help" || argument == "-h") {
            print_usage();
            std::exit(0);
        } else if (argument == "--no-boot") {
            options.boot = false;
        } else if (argument == "--trace") {
            options.trace = true;
        } else if (argument == "--console-type" && index + 1 < argc) {
            const std::string kind(argv[++index]);
            options.console_type = (kind == "retail") ? 0x00000003ul : 0x10000006ul;
        } else if (argument == "--no-xinput") {
            options.xinput = false;
        } else if (argument == "--original-save-text") {
            options.pc_save_text = false;
        } else if (argument == "--headless") {
            options.window = false;
        } else if (argument == "--profile") {
            options.profile = true;
        } else if (argument == "--no-recomp") {
            options.recompiled = false;
        } else if (argument == "--no-debug-history") {
            options.debug_history = false;
        } else if (argument == "--debug-history") {
            options.debug_history = true;
        } else if (argument == "--verify-recomp") {
            options.verify_recompiled = true;
        } else if (argument == "--fallback-audit") {
            options.fallback_audit = true;
        } else if (argument == "--visibility-probe") {
            options.visibility_probe = true;
        } else if (argument == "--fov-probe") {
            options.fov_probe = true;
        } else if (argument == "--present-plume") {
            options.present_plume = true;
        } else if (argument == "--native-xfb-present") {
            options.native_xfb_present = true;
            options.present_plume = true;
        } else if (argument == "--no-native-xfb-present") {
            options.native_xfb_present = false;
        } else if (argument == "--no-present-area-resolve") {
            options.present_area_resolve = false;
        } else if (argument == "--verify-plume") {
            options.present_plume = true;
            options.verify_plume = true;
        } else if (argument == "--vsync") {
            options.vsync = true;
            options.vsync_explicit = true;
        } else if (argument == "--no-vsync") {
            options.vsync = false;
            options.vsync_explicit = true;
        } else if (argument == "--render-rate" && index + 1 < argc) {
            const std::string rate = argv[++index];
            options.render_rate_explicit = true;
            if (rate == "unlocked") {
                options.render_rate = 0.0;
            } else {
                options.render_rate = std::strtod(rate.c_str(), nullptr);
                if (!std::isfinite(options.render_rate) || options.render_rate <= 0.0 ||
                    options.render_rate > 1000.0) {
                    std::cerr << "ERROR: --render-rate expects unlocked or a value in (0,1000]\n";
                    std::exit(1);
                }
            }
        } else if (argument == "--catchup-rate" && index + 1 < argc) {
            const std::string rate = argv[++index];
            if (rate == "off" || rate == "unlimited") {
                options.catchup_rate = 0.0;
            } else {
                options.catchup_rate = std::strtod(rate.c_str(), nullptr);
                if (!std::isfinite(options.catchup_rate) || options.catchup_rate < 0.0 ||
                    options.catchup_rate > 100.0) {
                    std::cerr << "ERROR: --catchup-rate expects off or a value in [0,100]\n";
                    std::exit(1);
                }
            }
        } else if (argument == "--frame-interpolation") {
            options.frame_interpolation = true;
            options.frame_interpolation_explicit = true;
        } else if (argument == "--no-frame-interpolation") {
            options.frame_interpolation = false;
            options.frame_interpolation_explicit = true;
        } else if (argument == "--game-data" && index + 1 < argc) {
            options.game_data = std::filesystem::path(argv[++index]);
        } else if (argument == "--memory-card" && index + 1 < argc) {
            options.memory_card = std::filesystem::path(argv[++index]);
        } else if (argument == "--service-interval" || argument == "--tick-profile") {
            std::cerr << "ERROR: " << argument << " is gone: there is no instruction clock; the host "
                         "service runs every 5 ms of game time, headless or not\n";
            std::exit(2);
        } else if (argument == "--movie-trace" && index + 1 < argc) {
            options.movie_trace = std::filesystem::path(argv[++index]);
        } else if (argument == "--no-sofdec-stub") {
            options.sofdec_playend_stub = false;
        } else if (argument == "--sofdec-stub") {
            options.sofdec_playend_stub = true;
        } else if (argument == "--ui-probe") {
            options.ui_probe = true;
        } else if (argument == "--measure" && index + 1 < argc) {
            const std::string list = argv[++index];
            std::size_t begin = 0;
            while (begin <= list.size()) {
                const std::size_t end = (std::min)(list.find(',', begin), list.size());
                const std::string key = list.substr(begin, end - begin);
                if (key == "all") {
                    options.measure.insert({"r3", "r4", "r5probes"});
                } else if (key == "r3" || key == "r4" || key == "r5probes" || key == "gx-inputs") {
                    options.measure.insert(key);
                } else if (key == "r13") {
                    std::cerr << "ERROR: --measure r13 is gone: its exception windows were "
                                 "measured in retired instructions, which no longer exist\n";
                    std::exit(2);
                } else if (key == "r7") {
                    std::cerr << "ERROR: --measure r7 is gone: it measured the emulated serial "
                                 "interface's poll grid, which no longer exists (controller samples are "
                                 "latched every 5 ms of game time)\n";
                    std::exit(2);
                } else if (!key.empty()) {
                    std::cerr << "ERROR: --measure: unknown item '" << key
                              << "' (known: r3, r4, r5probes, gx-inputs, all)\n";
                    std::exit(2);
                }
                begin = end + 1;
            }
            if (options.measure.count("r3") != 0u) {
                // r3's comparison reads the exception histogram and the Yield
                // table needs the fallback map.
                options.fallback_audit = true;
            }
        } else if (argument == "--measure-out" && index + 1 < argc) {
            options.measure_out = std::filesystem::path(argv[++index]);
        } else if (argument == "--measure-probes" && index + 1 < argc) {
            options.measure_probes_spec = argv[++index];
        } else if (argument == "--ax-mixer") {
            // Accepted: the AX contract (voice advance and mix) always runs.
        } else if (argument == "--no-ax-mixer") {
            std::cerr << "ERROR: --no-ax-mixer is gone: the native DSP runs the AX contract on every "
                         "command list (the voices move whether or not a device plays them)\n";
            std::exit(2);
        } else if (argument == "--audio") {
            options.audio = true;
        } else if (argument == "--no-audio") {
            options.audio = false;
        } else if (argument == "--movie-frame-dump" && index + 1 < argc) {
            options.movie_frame_dump = std::filesystem::path(argv[++index]);
        } else if (argument == "--audio-dump" && index + 1 < argc) {
            options.audio_dump = std::filesystem::path(argv[++index]);
        } else if (argument == "--tv-format" && index + 1 < argc) {
            const std::string format = argv[++index];
            if (format == "pal") {
                options.pal_boot = true;
            } else if (format == "ntsc") {
                options.pal_boot = false;
            } else {
                std::cerr << "ERROR: --tv-format expects pal or ntsc\n";
                print_usage();
                std::exit(1);
            }
        } else if (argument == "--ui-test" && index + 1 < argc) {
            options.ui_test_mode = argv[++index];
        } else if (argument == "--ui-test-script" && index + 1 < argc) {
            options.ui_test_script = std::filesystem::path(argv[++index]);
        } else if (argument == "--telemetry" && index + 1 < argc) {
            options.telemetry_log = argv[++index];
        } else if (argument == "--perf-log" && index + 1 < argc) {
            options.performance_log = std::filesystem::path(argv[++index]);
        } else if (argument == "--json-report" && index + 1 < argc) {
            options.json_report = std::filesystem::path(argv[++index]);
        } else if (argument == "--no-json-report") {
            options.json_report_enabled = false;
        } else if (argument == "--contexts" && index + 1 < argc) {
            options.context_hook = std::stoul(argv[++index], nullptr, 0);
        } else if (argument == "--watch" && index + 1 < argc) {
            options.watches.push_back(std::stoul(argv[++index], nullptr, 0));
        } else if (argument == "--dump" && index + 2 < argc) {
            const unsigned long address = std::stoul(argv[++index], nullptr, 0);
            const unsigned long length = std::stoul(argv[++index], nullptr, 0);
            options.dumps.emplace_back(address, length);
        } else if (argument == "--symbols" && index + 1 < argc) {
            options.symbols = std::filesystem::path(argv[++index]);
        } else if (argument == "--budget") {
            std::cerr << "ERROR: --budget counted retired instructions, which no longer exist; "
                         "use --budget-ms <game milliseconds>\n";
            std::exit(2);
        } else if (argument == "--budget-ms" && index + 1 < argc) {
            options.budget_ms = std::stoull(argv[++index]);
        } else if (argument == "--wait-watchdog-ms" && index + 1 < argc) {
            options.wait_watchdog_ms = std::stoul(argv[++index]);
        } else if ((argument == "--width" || argument == "--window-width") &&
                   index + 1 < argc) {
            options.window_width = std::stoi(argv[++index]);
        } else if ((argument == "--height" || argument == "--window-height") &&
                   index + 1 < argc) {
            options.window_height = std::stoi(argv[++index]);
        } else if (argument == "--fullscreen") {
            options.fullscreen = true;
        } else if (argument == "--windowed") {
            options.fullscreen = false;
        } else if (argument == "--render-width" && index + 1 < argc) {
            options.render_width = std::stoi(argv[++index]);
        } else if (argument == "--render-height" && index + 1 < argc) {
            options.render_height = std::stoi(argv[++index]);
        } else if (argument == "--no-logical-pixel-snap") {
            options.logical_pixel_snap = false;
        } else if (argument == "--internal-scale" && index + 1 < argc) {
            const std::string scale_value = argv[++index];
            if (scale_value == "auto") {
                options.internal_scale_auto = true;
            } else {
                options.internal_scale_auto = false;
                options.internal_scale = static_cast<unsigned>(std::stoul(scale_value));
                if (options.internal_scale == 0u ||
                    options.internal_scale > gekko::kMaxInternalScale) {
                    std::cerr << "ERROR: --internal-scale must be 1.."
                              << gekko::kMaxInternalScale << " or \"auto\"\n";
                    std::exit(1);
                }
            }
        } else if (argument == "--aspect" && index + 1 < argc) {
            bool ok = false;
            options.aspect = gekko::parse_aspect_mode(argv[++index], &ok);
            if (!ok) {
                std::cerr << "ERROR: unknown aspect mode\n";
                print_usage();
                std::exit(1);
            }
        } else if (argument == "--aspect-ratio" && index + 1 < argc) {
            options.custom_aspect = std::stof(argv[++index]);
            if (!options.aspect) {
                options.aspect = gekko::AspectMode::Custom;
            }
        } else if (argument == "--hud-scale" && index + 1 < argc) {
            options.hud_scale = std::stof(argv[++index]);
        } else if ((argument == "--hud-inset" || argument == "--hud-safe-zone") &&
                   index + 1 < argc) {
            options.hud_inset = std::stof(argv[++index]);
        } else if (argument == "--state-file" && index + 1 < argc) {
            options.state_file = std::filesystem::path(argv[++index]);
        } else if (argument == "--load-state") {
            options.load_state_at_start = true;
        } else if (argument == "--native-boot-contract-dir") {
            if (index + 1 >= argc)
                throw std::invalid_argument("--native-boot-contract-dir requires a new directory");
            options.native_boot_contract_directory = std::filesystem::path(argv[++index]);
        } else if (argument == "--save-state") {
            options.save_state_at_exit = true;
        } else if (argument == "--save-state-file" && index + 1 < argc) {
            options.save_state_file = std::filesystem::path(argv[++index]);
            options.save_state_at_exit = true;
        } else if (argument == "--job-workers" && index + 1 < argc) {
            options.job_workers = static_cast<unsigned>(std::stoul(argv[++index]));
        } else if (argument == "--present-dump" && index + 1 < argc) {
            options.present_dump = argv[++index];
        } else if (argument == "--present-gpu-blit") {
            options.present_gpu_blit = true;
            options.present_plume = true;
        } else if (argument == "--no-present-gpu-blit") {
            options.present_gpu_blit = false;
        } else if (argument == "--display-cfg" && index + 1 < argc) {
            options.display_cfg = std::filesystem::path(argv[++index]);
        } else if (argument == "--pad-cfg" && index + 1 < argc) {
            options.pad_cfg = std::filesystem::path(argv[++index]);
        } else if (argument == "--button-style" && index + 1 < argc) {
            shadow::peripheral::ButtonStyle style{};
            if (!shadow::peripheral::parse_button_style(argv[++index], style)) {
                std::cerr << "ERROR: --button-style takes auto, original, xbox, playstation or switch\n";
                std::exit(1);
            }
            options.button_style = style;
        } else {
            std::cerr << "ERROR: Unknown or incomplete argument: " << argument << "\n";
            print_usage();
            std::exit(1);
        }
    }
    require_native_renderer();
    return options;
}

}  // namespace

int main(int argc, char** argv) {
    gekko::name_this_thread("main-ui");  // ES-T1: the window / present loop runs here in windowed runs
    // stdout is FULLY buffered whenever it is not a console - a pipe or a file -
    // and MSVC has no line buffering (_IOLBF behaves as full buffering). A
    // launcher reading the pipe therefore saw none of the startup report until
    // 4 KiB had accumulated or the process exited, and lost it entirely if the
    // run was killed. Unbuffered costs nothing measurable: the runtime writes a
    // startup banner and an exit report, not per frame. A console was already
    // flushed per write, so only pipes and files change.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    try {
        const Options options = parse_options(argc, argv);
        // Before anything can touch the job system: the pool is a singleton
        // built on first use and its size is fixed at construction.
        gekko::set_host_job_worker_target(options.job_workers);
        return run(options);
    } catch (const std::exception& error) {
        std::cerr << "ERROR: " << error.what() << "\n";
        return 1;
    }
}
