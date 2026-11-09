// The game's audio platform, served natively. Contract and rationale:
// include/gekko/native_audio.h and docs/native-time-20261005/DESIGN.md.

#include "gekko/native_audio.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "gekko/audio_audit.h"
#include "gekko/cpu.h"
#include "gekko/game_clock.h"
#include "gekko/guest_memory.h"
#include "gekko/profiling.h"

namespace gekko {

namespace {

// ---- the entry points the generator routes here (extract/native-owners.txt)
constexpr u32 kInitAudioSystem = 0x80372260u;     // __OSInitAudioSystem (OSInit)
constexpr u32 kStopAudioSystem = 0x8037241Cu;     // __OSStopAudioSystem (reset paths)
constexpr u32 kAiInitDma = 0x80381E74u;           // AIInitDMA
constexpr u32 kAiStartDma = 0x80381EFCu;          // AIStartDMA
constexpr u32 kAiStopDma = 0x80381F14u;           // AIStopDMA
constexpr u32 kAiSetStreamPlayState = 0x80381F2Cu;
constexpr u32 kAiGetStreamPlayState = 0x80382004u;
constexpr u32 kAiSetDspSampleRate = 0x80382014u;
constexpr u32 kAiGetDspSampleRate = 0x803820F4u;
constexpr u32 kAiSetStreamSampleRate = 0x80382108u;  // __AI_set_stream_sample_rate
constexpr u32 kAiGetStreamSampleRate = 0x803821DCu;
constexpr u32 kAiSetStreamVolLeft = 0x803821ECu;
constexpr u32 kAiGetStreamVolLeft = 0x80382208u;
constexpr u32 kAiSetStreamVolRight = 0x80382218u;
constexpr u32 kAiGetStreamVolRight = 0x80382234u;
constexpr u32 kAiInit = 0x80382244u;
constexpr u32 kDspCheckMailToDsp = 0x8038C274u;
constexpr u32 kDspCheckMailFromDsp = 0x8038C284u;
constexpr u32 kDspReadMailFromDsp = 0x8038C294u;
constexpr u32 kDspSendMailToDsp = 0x8038C2ACu;
constexpr u32 kDspInit = 0x8038C2C0u;
constexpr u32 kDspAddTask = 0x8038C38Cu;
constexpr u32 kDspAssertTask = 0x8038C43Cu;

const NativeAudio::Address kAddresses[] = {
    {kInitAudioSystem, "__OSInitAudioSystem", false},
    {kStopAudioSystem, "__OSStopAudioSystem", false},
    {kAiInitDma, "AIInitDMA", false},
    {kAiStartDma, "AIStartDMA", false},
    {kAiStopDma, "AIStopDMA", false},
    {kAiSetStreamPlayState, "AISetStreamPlayState", false},
    {kAiGetStreamPlayState, "AIGetStreamPlayState", false},
    {kAiSetDspSampleRate, "AISetDSPSampleRate", false},
    {kAiGetDspSampleRate, "AIGetDSPSampleRate", false},
    {kAiSetStreamSampleRate, "__AI_set_stream_sample_rate", false},
    {kAiGetStreamSampleRate, "AIGetStreamSampleRate", false},
    {kAiSetStreamVolLeft, "AISetStreamVolLeft", false},
    {kAiGetStreamVolLeft, "AIGetStreamVolLeft", false},
    {kAiSetStreamVolRight, "AISetStreamVolRight", false},
    {kAiGetStreamVolRight, "AIGetStreamVolRight", false},
    {kAiInit, "AIInit", false},
    {kDspCheckMailToDsp, "DSPCheckMailToDSP", false},
    {kDspCheckMailFromDsp, "DSPCheckMailFromDSP", false},
    {kDspReadMailFromDsp, "DSPReadMailFromDSP", false},
    {kDspSendMailToDsp, "DSPSendMailToDSP", false},
    {kDspInit, "DSPInit", false},
    {kDspAddTask, "DSPAddTask", false},
    {kDspAssertTask, "DSPAssertTask", false},
};

// ---- SDK state, r13-relative (the SDK's own small-data variables) ----------
// ai.c
constexpr u32 kSdaAisCallback = 0x5C60u;   // __AIS_Callback
constexpr u32 kSdaAidCallback = 0x5C64u;   // __AID_Callback
constexpr u32 kSdaCallbackStack = 0x5C68u; // __CallbackStack
constexpr u32 kSdaAiInitFlag = 0x5C70u;    // __AI_init_flag
constexpr u32 kSdaAidActive = 0x5C74u;     // __AID_Active
constexpr u32 kSdaAiBounds = 0x5C78u;      // bound_32KHz, bound_48KHz, min_wait, max_wait, buffer (OSTime)
// dsp.c / dsp_task.c
constexpr u32 kSdaDspInitFlag = 0x5DD0u;    // __DSP_init_flag
constexpr u32 kSdaRudePending = 0x5DD8u;    // __DSP_rude_task_pending
constexpr u32 kSdaRudeTask = 0x5DDCu;       // __DSP_rude_task
constexpr u32 kSdaTmpTask = 0x5DE0u;        // __DSP_tmp_task
constexpr u32 kSdaLastTask = 0x5DE4u;       // __DSP_last_task
constexpr u32 kSdaFirstTask = 0x5DE8u;      // __DSP_first_task
constexpr u32 kSdaCurrTask = 0x5DECu;       // __DSP_curr_task

// DSPTaskInfo (dsp.h).
constexpr u32 kTaskState = 0x00u;      // 0 INIT, 1 RUN, 2 YIELD, 3 DONE
constexpr u32 kTaskPriority = 0x04u;
constexpr u32 kTaskFlags = 0x08u;      // 1 ACTIVE, 2 CANCEL
constexpr u32 kTaskIramMmem = 0x0Cu;
constexpr u32 kTaskIramLength = 0x10u;
constexpr u32 kTaskInitCallback = 0x28u;
constexpr u32 kTaskResumeCallback = 0x2Cu;
constexpr u32 kTaskDoneCallback = 0x30u;
constexpr u32 kTaskRequestCallback = 0x34u;
constexpr u32 kTaskNext = 0x38u;
constexpr u32 kTaskPrev = 0x3Cu;
constexpr u32 kTaskFlagCancel = 0x2u;

// DSP <-> CPU mails of the SDK task protocol (dsp_task.c).
constexpr u32 kMailInit = 0xDCD10000u;
constexpr u32 kMailResume = 0xDCD10001u;
constexpr u32 kMailYield = 0xDCD10002u;
constexpr u32 kMailDone = 0xDCD10003u;
constexpr u32 kMailRequest = 0xDCD10004u;
constexpr u32 kMailSwitch = 0xCDD10001u;
constexpr u32 kMailHalt = 0xCDD10002u;
constexpr u32 kMailContinue = 0xCDD10003u;
// The AX command protocol: 0xBABE | list size, then the list address.
constexpr u32 kAxListHeader = 0xBABE0000u;

// The one microcode the native DSP implements: the AX microcode this game
// links (0x1F00 bytes at 0x80564E60), identified by content.
constexpr u32 kAxMicrocodeLength = 0x1F00u;
constexpr u64 kAxMicrocodeFnv1a = 0x7708BDFA41638A48ull;


std::string hex(u32 value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08X", value);
    return text;
}

void stop(Cpu& c, const std::string& why) {
    c.request_stop(StopReason::ExecutionFault, "native audio: " + why);
}

// One AI DMA block (32 bytes, 8 stereo frames) in game ticks: 8 x 40.5 MHz /
// rate - 10,125 at 32 kHz, 6,750 at 48 kHz, both exact.
u64 ticks_per_block(bool rate_32k) noexcept {
    return 8u * (kGameTicksPerSecond / (rate_32k ? 32000u : 48000u)) +
           (rate_32k ? (8u * (kGameTicksPerSecond % 32000u)) / 32000u
                     : (8u * (kGameTicksPerSecond % 48000u)) / 48000u);
}

}  // namespace

const NativeAudio::Address* NativeAudio::addresses(std::size_t& count) noexcept {
    count = sizeof(kAddresses) / sizeof(kAddresses[0]);
    return kAddresses;
}

void NativeAudio::reset() noexcept {
    dma_start_ = 0;
    dma_control_ = 0;
    pass_start_ = 0;
    pass_blocks_ = 0;
    stream_playing_ = false;
    stream_rate_48k_ = false;
    dsp_rate_32k_ = false;
    volume_left_ = 0;
    volume_right_ = 0;
    task_running_ = false;
    task_ = 0;
    mail_valid_ = false;
    mail_ = 0;
    expect_list_address_ = false;
    list_size_ = 0;
    completions_.clear();
    cpu_.game_clock().cancel(GameEvent::Audio);
}

// ---------------------------------------------------------------------------
// The AI DMA engine
// ---------------------------------------------------------------------------

// One time base: the passes sit on whole multiples of their length in the time base (5 ms, 202,500 ticks, for the
// game's 640-byte buffer at 32 kHz), as the frames sit on multiples of 1/60 s (NativeFrameLimiter, NativeVideo) - a
// pass ends on a frame boundary every 50 ms (three frames), at the same tick, instead of at a phase that depends on
// when the game happened to start its DMA. The first pass of a start is cut short at the next grid point; reloads
// follow on without a gap and so stay on the grid.
void NativeAudio::begin_pass(u64 now, bool on_grid) {
    pass_start_ = dma_start_;
    pass_blocks_ = dma_control_ & 0x7FFFu;
    if (pass_blocks_ == 0u) {
        stop(cpu_, "AI DMA enabled with a zero-length buffer");
        cpu_.game_clock().cancel(GameEvent::Audio);
        return;
    }
    const u64 length = static_cast<u64>(pass_blocks_) * ticks_per_block(dsp_rate_32k_);
    u64 end = now + length;
    if (on_grid) {
        const u64 offset = cpu_.time_base() - cpu_.game_clock().now();
        end = ((now + offset) / length + 1u) * length - offset;
    }
    cpu_.game_clock().schedule(GameEvent::Audio, end);
}

void NativeAudio::on_pass_end(u64 now) {
    GEKKO_PROFILE_ZONE("audio/pass");
    if ((dma_control_ & 0x8000u) == 0u) {
        cpu_.game_clock().cancel(GameEvent::Audio);
        return;
    }
    if (!dsp_rate_32k_) {
        stop(cpu_, "a 48 kHz AI pass has no output path (the game runs its mixer at 32 kHz)");
        cpu_.game_clock().cancel(GameEvent::Audio);
        return;
    }
    const u32 bytes = pass_blocks_ * 32u;
    const hw::HostAudioPointer64 host = memory_.host_pointer(pass_start_, bytes);
    if (host == nullptr) ++stats_.silent_passes;
    audio_audit::event(audio_audit::Lane::Guest, "ai-pass", pass_start_, bytes, stats_.passes, now);
    if (sink_) sink_(host, bytes);
    ++stats_.passes;
    stats_.pass_bytes += bytes;
    // The engine reloads from the programmed buffer and raises AIDINT: the
    // callback programs the buffer of the pass after this one.
    begin_pass(now);
    interrupts_.raise(NativeInterrupts::Source::AiDma);
}

// __AIDHandler: the __AID_Active guard around the registered callback.
bool NativeAudio::ai_handler(Cpu& c, u32 frame, u32 step) {
    const u32 sda = c.gpr[13];
    if (step == 0u) {
        const u32 callback = c.mem_read32(sda + kSdaAidCallback);
        if (callback == 0u || c.mem_read32(sda + kSdaAidActive) != 0u) return false;
        if (c.mem_read32(sda + kSdaCallbackStack) != 0u) {
            // AIInit(stack) asks for __AICallbackStackSwitch; this game passes
            // NULL (fn_801F2E58), and no native path exists for the switch.
            stop(c, "AIInit was given a callback stack; the switch has no native path");
            return false;
        }
        c.mem_write32(sda + kSdaAidActive, 1u);
        ++stats_.ai_callbacks;
        interrupts_.call_guest(c, frame, callback, callback, 0u);
        return true;
    }
    c.mem_write32(sda + kSdaAidActive, 0u);
    return false;
}

void NativeAudio::set_stream_play_state(u32 state) noexcept {
    if ((state & 1u) == (stream_playing_ ? 1u : 0u)) return;
    if (!stream_rate_48k_ && state == 1u) {
        // The 32 kHz start path: the SDK saves both volumes, silences them,
        // synchronises the sample-rate converter and restores them - with left
        // and right exchanged (AISetStreamPlayState 0x80381FC0: VolLeft gets
        // the saved right volume, VolRight the saved left).
        const u8 right = volume_right_;
        const u8 left = volume_left_;
        stream_playing_ = true;
        volume_left_ = right;
        volume_right_ = left;
        return;
    }
    stream_playing_ = (state & 1u) != 0u;
}

// ---------------------------------------------------------------------------
// The DSP DMA interrupt (OS cause DSP_ARAM): native transfer completions
// ---------------------------------------------------------------------------

void NativeAudio::complete_transfer(u32 callback, u32 request) {
    completions_.emplace_back(callback, request);
    interrupts_.raise(NativeInterrupts::Source::DspDma);
}

// The DSP DMA interrupt: each native transfer's completion callback in turn (complete_transfer), one guest call
// per step, in interrupt context as ARQ's service routine ran it.
bool NativeAudio::dsp_dma_handler(Cpu& c, u32 frame, u32 step) {
    (void)step;
    if (completions_.empty()) return false;
    const auto [callback, request] = completions_.front();
    completions_.pop_front();
    ++stats_.transfer_completions;
    interrupts_.call_guest(c, frame, callback, request, 0u);
    return true;
}

// The game constructor's ARInit call (0x801F2EA0, a lowered site): of ARInit's work only what outlives ARAM - the
// DSP DMA interrupt (OS cause DSP_ARAM) unmasked, as ARInit unmasked it, so native transfer completions are taken.
// ARAM's allocator bookkeeping and size words have no reader left (docs/ARAM.md step 5).
void NativeAudio::dma_interrupt_init(Cpu& c) {
    interrupts_.unmask_os_interrupts(c, 0x02000000u);  // DSP_ARAM
}

// ---------------------------------------------------------------------------
// The DSP: the AX microcode's contract and the SDK task manager
// ---------------------------------------------------------------------------

void NativeAudio::post_mail(Cpu& c, u32 mail) {
    if (mail_valid_) {
        stop(c, "the DSP posted " + hex(mail) + " while " + hex(mail_) + " was still unread");
        return;
    }
    mail_ = mail;
    mail_valid_ = true;
    ++stats_.dsp_mails;
    interrupts_.raise(NativeInterrupts::Source::Dsp);
}

// A mail from the CPU, to the running task.
void NativeAudio::receive_mail(Cpu& c, u32 mail) {
    if (!task_running_) {
        stop(c, "mail " + hex(mail) + " to the DSP while no task runs");
        return;
    }
    if (expect_list_address_) {
        expect_list_address_ = false;
        last_command_list_ = mail;
        ++stats_.command_lists;
        // The microcode consumes the list (mixes the frame) and yields.
        if (mixer_) mixer_(mail);
        post_mail(c, kMailYield);
        return;
    }
    if ((mail & 0xFFFF0000u) == kAxListHeader) {
        list_size_ = mail & 0xFFFFu;
        expect_list_address_ = true;
        return;
    }
    stop(c, "mail " + hex(mail) + " is not part of the AX command protocol");
}

// __DSP_insert_task (0x8038CCA4): priority order, first/last/curr kept.
void NativeAudio::insert_task(Cpu& c, u32 task) {
    const u32 sda = c.gpr[13];
    const u32 first = c.mem_read32(sda + kSdaFirstTask);
    if (first == 0u) {
        c.mem_write32(sda + kSdaCurrTask, task);
        c.mem_write32(sda + kSdaLastTask, task);
        c.mem_write32(sda + kSdaFirstTask, task);
        c.mem_write32(task + kTaskPrev, 0u);
        c.mem_write32(task + kTaskNext, 0u);
        return;
    }
    for (u32 t = first; t != 0u; t = c.mem_read32(t + kTaskNext)) {
        if (c.mem_read32(task + kTaskPriority) < c.mem_read32(t + kTaskPriority)) {
            c.mem_write32(task + kTaskPrev, c.mem_read32(t + kTaskPrev));
            c.mem_write32(t + kTaskPrev, task);
            c.mem_write32(task + kTaskNext, t);
            const u32 prev = c.mem_read32(task + kTaskPrev);
            if (prev == 0u) {
                c.mem_write32(sda + kSdaFirstTask, task);
            } else {
                c.mem_write32(prev + kTaskNext, task);
            }
            return;
        }
    }
    const u32 last = c.mem_read32(sda + kSdaLastTask);
    c.mem_write32(last + kTaskNext, task);
    c.mem_write32(task + kTaskNext, 0u);
    c.mem_write32(task + kTaskPrev, c.mem_read32(sda + kSdaLastTask));
    c.mem_write32(sda + kSdaLastTask, task);
}

// __DSP_remove_task (0x8038CD44).
void NativeAudio::remove_task(Cpu& c, u32 task) {
    const u32 sda = c.gpr[13];
    c.mem_write32(task + kTaskFlags, 0u);
    c.mem_write32(task + kTaskState, 3u);
    if (c.mem_read32(sda + kSdaFirstTask) == task) {
        const u32 next = c.mem_read32(task + kTaskNext);
        if (next != 0u) {
            c.mem_write32(sda + kSdaFirstTask, next);
            c.mem_write32(next + kTaskPrev, 0u);
        } else {
            c.mem_write32(sda + kSdaCurrTask, 0u);
            c.mem_write32(sda + kSdaLastTask, 0u);
            c.mem_write32(sda + kSdaFirstTask, 0u);
        }
        return;
    }
    if (c.mem_read32(sda + kSdaLastTask) == task) {
        const u32 prev = c.mem_read32(task + kTaskPrev);
        c.mem_write32(sda + kSdaLastTask, prev);
        c.mem_write32(prev + kTaskNext, 0u);
        c.mem_write32(sda + kSdaCurrTask, c.mem_read32(sda + kSdaFirstTask));
        return;
    }
    const u32 prev = c.mem_read32(task + kTaskPrev);
    const u32 next = c.mem_read32(task + kTaskNext);
    c.mem_write32(sda + kSdaCurrTask, next);
    c.mem_write32(prev + kTaskNext, next);
    c.mem_write32(next + kTaskPrev, prev);
}

// __DSP_boot_task: the DSP loads the task's microcode and starts it. Only the
// AX microcode has a native implementation.
void NativeAudio::boot_task(Cpu& c, u32 task) {
    const u32 code = c.mem_read32(task + kTaskIramMmem);
    const u32 length = c.mem_read32(task + kTaskIramLength);
    u64 hash = 0xcbf29ce484222325ull;
    if (length == kAxMicrocodeLength) {
        for (u32 i = 0; i < length; ++i) {
            hash ^= c.mem_read8(code + i);
            hash *= 0x100000001b3ull;
        }
    }
    if (length != kAxMicrocodeLength || hash != kAxMicrocodeFnv1a) {
        char text[96];
        std::snprintf(text, sizeof(text), "%016llX", static_cast<unsigned long long>(hash));
        stop(c, "DSP task " + hex(task) + " runs microcode " + hex(code) + " (" + hex(length) +
                    " bytes, fnv1a " + text + "); only the AX microcode has a native implementation");
        return;
    }
    task_running_ = true;
    task_ = task;
    expect_list_address_ = false;
    post_mail(c, kMailInit);
}

// __DSPHandler (0x8038C554): one mail from the DSP per interrupt, the task's
// callbacks in the SDK's order. Single-task paths only; a task switch has no
// native path (one task exists: AX).
bool NativeAudio::dsp_handler(Cpu& c, u32 frame, u32 step) {
    const u32 sda = c.gpr[13];
    const u32 curr = c.mem_read32(sda + kSdaCurrTask);
    const auto call = [&](u32 offset) {
        const u32 callback = c.mem_read32(curr + offset);
        if (callback == 0u) return false;
        ++stats_.dsp_callbacks;
        interrupts_.call_guest(c, frame, callback, curr, 0u);
        return true;
    };
    if (step != 0u) {
        // The step after a callback: only DONE has work left (its scratch
        // word 0 says which).
        if (c.mem_read32(NativeInterrupts::scratch(frame, 0u)) == kMailDone) {
            task_running_ = false;  // 0xCDD10002: the DSP halts the task
            c.mem_write32(curr + kTaskState, 3u);
            remove_task(c, curr);
        }
        return false;
    }
    if (!mail_valid_) {
        // The original spins in DSPCheckMailFromDSP for ever.
        stop(c, "DSP interrupt without a mail from the DSP");
        return false;
    }
    u32 mail = mail_;
    mail_valid_ = false;
    if (curr == 0u) {
        stop(c, "DSP mail " + hex(mail) + " with no current task");
        return false;
    }
    if ((c.mem_read32(curr + kTaskFlags) & kTaskFlagCancel) != 0u && mail == kMailYield) mail = kMailDone;
    const bool single = c.mem_read32(curr + kTaskNext) == 0u && curr == c.mem_read32(sda + kSdaFirstTask);
    switch (mail) {
        case kMailInit:
            c.mem_write32(curr + kTaskState, 1u);
            return call(kTaskInitCallback);
        case kMailResume:
            c.mem_write32(curr + kTaskState, 1u);
            return call(kTaskResumeCallback);
        case kMailYield:
            if (c.mem_read32(sda + kSdaRudePending) != 0u) {
                if (c.mem_read32(sda + kSdaRudeTask) != curr) {
                    stop(c, "a DSP task switch (rude task " + hex(c.mem_read32(sda + kSdaRudeTask)) +
                                ") has no native path");
                    return false;
                }
                // kMailContinue: the task resumes where it yielded.
                c.mem_write32(sda + kSdaRudeTask, 0u);
                c.mem_write32(sda + kSdaRudePending, 0u);
                return call(kTaskResumeCallback);
            }
            if (!single) {
                stop(c, "a DSP task switch (" + hex(curr) + " yields to another task) has no native path");
                return false;
            }
            return call(kTaskResumeCallback);  // after kMailContinue
        case kMailDone:
            if (c.mem_read32(sda + kSdaRudePending) != 0u || !single) {
                stop(c, "a DSP task finishing into another task has no native path");
                return false;
            }
            c.mem_write32(NativeInterrupts::scratch(frame, 0u), kMailDone);
            if (call(kTaskDoneCallback)) return true;
            task_running_ = false;
            c.mem_write32(curr + kTaskState, 3u);
            remove_task(c, curr);
            return false;
        case kMailRequest:
            return call(kTaskRequestCallback);
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// The owners
// ---------------------------------------------------------------------------

namespace {
std::atomic<NativeAudio*> g_installed{nullptr};
}  // namespace

void NativeAudio::install() {
    g_installed.store(this, std::memory_order_release);
    Cpu& cpu = cpu_;
    const auto own = [&cpu](u32 address, const char* name, auto&& serve) {
        cpu.install_hle(address, [serve](Cpu& c) {
            serve(c);
            return true;  // an owner never declines
        }, std::string("native audio: ") + name);
    };
    interrupts_.set_handler(NativeInterrupts::Source::AiDma,
                            [this](Cpu& c, u32 frame, u32 step) { return ai_handler(c, frame, step); });
    interrupts_.set_handler(NativeInterrupts::Source::DspDma,
                            [this](Cpu& c, u32 frame, u32 step) { return dsp_dma_handler(c, frame, step); });
    interrupts_.set_handler(NativeInterrupts::Source::Dsp,
                            [this](Cpu& c, u32 frame, u32 step) { return dsp_handler(c, frame, step); });

    // ---- the OS audio bring-up ------------------------------------------------
    // __OSInitAudioSystem saved 128 bytes from a fixed GameCube address below
    // the arena top, loaded and ran the DSP's init microcode in their place,
    // waited on it and restored them. There is no DSP to initialise, and the
    // saved copy never outlived OSInit (its ClearArena zero-fills the arena
    // that held it), so nothing is done.
    own(kInitAudioSystem, "__OSInitAudioSystem", [](Cpu&) {});
    own(kStopAudioSystem, "__OSStopAudioSystem", [this](Cpu&) {
        dma_control_ = static_cast<u16>(dma_control_ & ~0x8000u);
        cpu_.game_clock().cancel(GameEvent::Audio);
        task_running_ = false;
        mail_valid_ = false;
        expect_list_address_ = false;
        interrupts_.clear(NativeInterrupts::Source::AiDma);
        interrupts_.clear(NativeInterrupts::Source::DspDma);
        interrupts_.clear(NativeInterrupts::Source::Dsp);
    });

    // ---- AI: the DMA engine ----------------------------------------------------
    own(kAiInitDma, "AIInitDMA", [this](Cpu& c) {
        dma_start_ = c.gpr[3] & ~0x1Fu;  // the game's buffer address (32-byte aligned)
        dma_control_ = static_cast<u16>((dma_control_ & 0x8000u) | ((c.gpr[4] >> 5) & 0xFFFFu));
    });
    own(kAiStartDma, "AIStartDMA", [this](Cpu& c) {
        if ((dma_control_ & 0x8000u) != 0u) return;
        dma_control_ = static_cast<u16>(dma_control_ | 0x8000u);
        begin_pass(c.game_clock().now(), true);
    });
    own(kAiStopDma, "AIStopDMA", [this](Cpu& c) {
        dma_control_ = static_cast<u16>(dma_control_ & ~0x8000u);
        c.game_clock().cancel(GameEvent::Audio);
    });

    // ---- AI: control and the (sourceless) stream settings ------------------------
    own(kAiSetStreamPlayState, "AISetStreamPlayState",
        [this](Cpu& c) { set_stream_play_state(c.gpr[3]); });
    own(kAiGetStreamPlayState, "AIGetStreamPlayState",
        [this](Cpu& c) { c.gpr[3] = stream_playing_ ? 1u : 0u; });
    own(kAiSetDspSampleRate, "AISetDSPSampleRate", [this](Cpu& c) {
        const u32 rate = c.gpr[3];
        if (rate == (dsp_rate_32k_ ? 0u : 1u)) return;
        // AI_SAMPLERATE_32KHZ = 0, AI_SAMPLERATE_48KHZ = 1; the SDK clears
        // AFR first and sets it again only for 32 kHz (volumes and the stream
        // settings come back as they were).
        dsp_rate_32k_ = rate == 0u;
    });
    own(kAiGetDspSampleRate, "AIGetDSPSampleRate",
        [this](Cpu& c) { c.gpr[3] = dsp_rate_32k_ ? 0u : 1u; });
    own(kAiSetStreamSampleRate, "__AI_set_stream_sample_rate", [this](Cpu& c) {
        const u32 rate = c.gpr[3];
        if (rate == (stream_rate_48k_ ? 1u : 0u)) return;
        const bool playing = stream_playing_;
        const u8 left = volume_left_;
        const u8 right = volume_right_;
        volume_right_ = 0;
        volume_left_ = 0;
        stream_rate_48k_ = (rate & 1u) != 0u;
        set_stream_play_state(playing ? 1u : 0u);
        volume_left_ = left;
        volume_right_ = right;
    });
    own(kAiGetStreamSampleRate, "AIGetStreamSampleRate",
        [this](Cpu& c) { c.gpr[3] = stream_rate_48k_ ? 1u : 0u; });
    own(kAiSetStreamVolLeft, "AISetStreamVolLeft", [this](Cpu& c) { set_stream_volume_left(c.gpr[3]); });
    own(kAiGetStreamVolLeft, "AIGetStreamVolLeft", [this](Cpu& c) { c.gpr[3] = volume_left_; });
    own(kAiSetStreamVolRight, "AISetStreamVolRight", [this](Cpu& c) { set_stream_volume_right(c.gpr[3]); });
    own(kAiGetStreamVolRight, "AIGetStreamVolRight", [this](Cpu& c) { c.gpr[3] = volume_right_; });

    own(kAiInit, "AIInit", [this](Cpu& c) {
        const u32 sda = c.gpr[13];
        if (c.mem_read32(sda + kSdaAiInitFlag) == 1u) return;
        // The OSTime constants __AI_SRC_INIT used (OSNanosecondsToTicks of
        // 31524, 42024, 42000, 63000 and 3000 ns), computed the SDK's way.
        const u32 bus = c.mem_read32(0x800000F8u);
        const u32 per_us8 =
            static_cast<u32>((static_cast<u64>(0x431BDE83u) * (bus >> 2)) >> 32u) >> 15u;
        const u32 nanoseconds[] = {0x7B24u, 0xA428u, 0xA410u, 0xF618u, 0x0BB8u};
        for (u32 i = 0; i < 5u; ++i) {
            const u32 product = per_us8 * nanoseconds[i];
            const u32 ticks = static_cast<u32>((static_cast<u64>(0x10624DD3u) * product) >> 32u) >> 9u;
            c.mem_write32(sda + kSdaAiBounds + 8u * i, 0u);
            c.mem_write32(sda + kSdaAiBounds + 8u * i + 4u, ticks);
        }
        volume_left_ = 0;
        volume_right_ = 0;
        // __AI_set_stream_sample_rate(AI_SAMPLERATE_48KHZ), then
        // AISetDSPSampleRate(AI_SAMPLERATE_32KHZ).
        stream_rate_48k_ = true;
        dsp_rate_32k_ = true;
        c.mem_write32(sda + kSdaAisCallback, 0u);
        c.mem_write32(sda + kSdaAidCallback, 0u);
        c.mem_write32(sda + kSdaCallbackStack, c.gpr[3]);
        // __AIDHandler and __AISHandler are not registered: this layer
        // delivers AIDINT itself and there is no stream interrupt.
        interrupts_.unmask_os_interrupts(c, 0x04000000u);  // DSP_AI
        interrupts_.unmask_os_interrupts(c, 0x00800000u);  // AI_AI
        c.mem_write32(sda + kSdaAiInitFlag, 1u);
    });

    // ---- the DSP -----------------------------------------------------------------
    own(kDspCheckMailToDsp, "DSPCheckMailToDSP", [](Cpu& c) { c.gpr[3] = 0u; });  // consumed at once
    own(kDspCheckMailFromDsp, "DSPCheckMailFromDSP", [this](Cpu& c) { c.gpr[3] = mail_valid_ ? 1u : 0u; });
    own(kDspReadMailFromDsp, "DSPReadMailFromDSP", [this](Cpu& c) {
        c.gpr[3] = mail_;
        mail_valid_ = false;  // reading the mail empties the mailbox; DSPINT stays
    });
    own(kDspSendMailToDsp, "DSPSendMailToDSP", [this](Cpu& c) { receive_mail(c, c.gpr[3]); });
    own(kDspInit, "DSPInit", [this](Cpu& c) {
        const u32 sda = c.gpr[13];
        if (c.mem_read32(sda + kSdaDspInitFlag) == 1u) return;
        // __DSPHandler is not registered: this layer delivers DSPINT itself.
        interrupts_.unmask_os_interrupts(c, 0x01000000u);  // DSP_DSP
        c.mem_write32(sda + kSdaTmpTask, 0u);
        c.mem_write32(sda + kSdaCurrTask, 0u);
        c.mem_write32(sda + kSdaLastTask, 0u);
        c.mem_write32(sda + kSdaFirstTask, 0u);
        c.mem_write32(sda + kSdaDspInitFlag, 1u);
    });
    own(kDspAddTask, "DSPAddTask", [this](Cpu& c) {
        const u32 task = c.gpr[3];
        insert_task(c, task);
        c.mem_write32(task + kTaskState, 0u);
        c.mem_write32(task + kTaskFlags, 1u);
        if (task == c.mem_read32(c.gpr[13] + kSdaFirstTask)) boot_task(c, task);
        c.gpr[3] = task;
    });
    own(kDspAssertTask, "DSPAssertTask", [](Cpu& c) {
        const u32 sda = c.gpr[13];
        const u32 task = c.gpr[3];
        const u32 curr = c.mem_read32(sda + kSdaCurrTask);
        if (curr == task) {
            c.mem_write32(sda + kSdaRudeTask, task);
            c.mem_write32(sda + kSdaRudePending, 1u);
            c.gpr[3] = task;
            return;
        }
        if (c.mem_read32(task + kTaskPriority) < c.mem_read32(curr + kTaskPriority)) {
            // The SDK also interrupts the running DSP (DSPCR PIINT) so it
            // yields early; the native DSP is never mid-frame.
            c.mem_write32(sda + kSdaRudeTask, task);
            c.mem_write32(sda + kSdaRudePending, 1u);
            c.gpr[3] = task;
            return;
        }
        c.gpr[3] = 0u;
    });
}

// ---------------------------------------------------------------------------
// Savestate
// ---------------------------------------------------------------------------

namespace {
void put32(std::vector<u8>& out, u32 value) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<u8>(value >> shift));
}
bool get32(const u8*& cursor, const u8* end, u32& value) {
    if (end - cursor < 4) return false;
    value = (static_cast<u32>(cursor[0]) << 24) | (static_cast<u32>(cursor[1]) << 16) |
            (static_cast<u32>(cursor[2]) << 8) | cursor[3];
    cursor += 4;
    return true;
}
}  // namespace

void NativeAudio::save_state(std::vector<u8>& out) const {
    if (!completions_.empty())
        std::fprintf(stderr, "[savestate] %zu native transfer completion(s) pending: this state is incomplete "
                             "(a store's caller is mid-transfer, which complete_transfer's contract excludes)\n",
                     completions_.size());
    put32(out, dma_start_);
    put32(out, dma_control_);
    put32(out, pass_start_);
    put32(out, pass_blocks_);
    put32(out, (stream_playing_ ? 1u : 0u) | (stream_rate_48k_ ? 2u : 0u) | (dsp_rate_32k_ ? 4u : 0u) |
                   (task_running_ ? 8u : 0u) | (mail_valid_ ? 16u : 0u) | (expect_list_address_ ? 32u : 0u));
    put32(out, static_cast<u32>(volume_left_) | (static_cast<u32>(volume_right_) << 8));
    put32(out, task_);
    put32(out, mail_);
    put32(out, list_size_);
    put32(out, last_command_list_);
}

bool NativeAudio::load_state(const u8*& cursor, const u8* end) {
    u32 control = 0, flags = 0, volumes = 0;
    if (!get32(cursor, end, dma_start_) || !get32(cursor, end, control) ||
        !get32(cursor, end, pass_start_) || !get32(cursor, end, pass_blocks_) ||
        !get32(cursor, end, flags) || !get32(cursor, end, volumes) || !get32(cursor, end, task_) ||
        !get32(cursor, end, mail_) || !get32(cursor, end, list_size_) ||
        !get32(cursor, end, last_command_list_)) {
        return false;
    }
    dma_control_ = static_cast<u16>(control);
    stream_playing_ = (flags & 1u) != 0u;
    stream_rate_48k_ = (flags & 2u) != 0u;
    dsp_rate_32k_ = (flags & 4u) != 0u;
    task_running_ = (flags & 8u) != 0u;
    mail_valid_ = (flags & 16u) != 0u;
    expect_list_address_ = (flags & 32u) != 0u;
    volume_left_ = static_cast<u8>(volumes);
    volume_right_ = static_cast<u8>(volumes >> 8);
    // Native completions are never pending at a save (complete_transfer's contract).
    completions_.clear();
    return true;
}

// ---- the lowered call sites (extract/lift-sites-native.txt; declared by the generator in recomp_table.cpp) --------

namespace scaffold {

// 0x801F2EA0 in the game constructor: ARInit(0x8053F2B0, 4). Its result was never read.
u32 dsp_dma_interrupt_init(Cpu& c, u32 stack, u32 entries) {
    (void)stack;
    (void)entries;
    NativeAudio* audio = g_installed.load(std::memory_order_acquire);
    if (audio == nullptr) {
        c.request_stop(StopReason::ExecutionFault,
                       "lowered site scaffold::dsp_dma_interrupt_init ran without native audio installed");
        return 0u;
    }
    audio->dma_interrupt_init(c);
    return 0u;
}

}  // namespace scaffold

}  // namespace gekko
