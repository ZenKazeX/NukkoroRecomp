#pragma once

// A MODEL OF THE CONSOLE'S AUDIO DEVICES, cut at the SDK's lowest functions:
// the audio interface's DMA engine and the DSP's mailbox / task manager, plus
// the OS audio bring-up. There is no ARAM (docs/ARAM.md): the samples live in
// native stores and their transfers complete on the DSP DMA interrupt. It is
// not a native audio engine (NATIVE_AUDIT.md C5): the translated Nintendo SDK
// above it (AX, AXFX, MIX), Sega's GCAX driver and CRI keep running on
// it, paced by its AI DMA interrupt. NE-AUDIO replaces this whole stack with a
// native voice engine at the game's audio API boundary.
//
// docs/native-time-20261005/DESIGN.md (stage 1). Every SDK function of those
// three devices that surviving guest code can reach is an HLE owner here
// (extract/native-owners.txt; the generator emits only a thunk). The owners
// keep each device's contract - DMA control words, the 5 ms pass clock, the
// AIDINT / DSPINT completions and the DSP DMA interrupt, the mail protocol - as follows:
//
//   game / CRI / AX (translated guest code)
//      |  AI*, DSP*, __OSInitAudioSystem (native owners)
//   NativeAudio
//      |  audio frames: the AI pass clock      GameClock (GameEvent::Audio)
//      |  the AX microcode's contract          System::run_ax_mix (the mixer)
//      |  the samples: native sound stores      gekko/sound_arena.h (docs/ARAM.md)
//      |  completions: AIDINT, DSP DMA, DSPINT NativeInterrupts (event calls
//      |                                       into the SDK callbacks)
//   AudioRuntime (WASAPI)                      the finished passes, published
//
// THE CONTRACT (what the game observes)
//  * Audio frames: a pass of the programmed DMA buffer (AIInitDMA) begins when
//    AIStartDMA enables it and lasts its length at the DSP sample rate in game
//    time (0x280 bytes = 160 frames = 5 ms = 202,500 ticks at 32 kHz); when it
//    ends, its bytes are published, the next pass latches the programmed
//    buffer, and the AI DMA callback runs in interrupt context (__AIDHandler's
//    order: __AID_Active guard, the callback).
//  * The DSP: the AX task (its microcode identified by content) boots on
//    DSPAddTask and reports INIT; each command list (0xBABExxxx, address)
//    is mixed when its address arrives, then the task yields (DSPINT), and the
//    SDK task manager's handler (__DSPHandler, native) runs the task's
//    callbacks in its order. Any other task or mail stops the machine.
//  * The DSP DMA interrupt (OS cause DSP_ARAM): each native sound store's
//    transfer completion runs its callback there, in interrupt context
//    (complete_transfer).
//  * The SDK's RAM state (callbacks, flags, task list, the OS
//    interrupt mask words) is written where the SDK keeps it.
//
// What is deliberately NOT reproduced: AI/DSP registers, the AI stream
// (DVD audio) sample counter and its interrupt - no stream source exists -,
// the DSP boot ROM and mailbox handshakes, DMA latency, and every spin the SDK made on them.

#include <array>
#include <cstddef>
#include <deque>
#include <functional>
#include <utility>
#include <vector>

#include "gekko/hw/types.h"
#include "gekko/native_interrupts.h"
#include "gekko/types.h"

namespace gekko {

class GuestMemory;
class Cpu;

class NativeAudio {
public:
    NativeAudio(Cpu& cpu, GuestMemory& memory, NativeInterrupts& interrupts) noexcept
        : cpu_(cpu), memory_(memory), interrupts_(interrupts) {}

    // Installs every owner and the three interrupt sources' handlers.
    void install();

    // The AX microcode's work: mix one command list (System::run_ax_mix).
    using Mixer = std::function<void(u32 command_list)>;
    void set_mixer(Mixer mixer) { mixer_ = std::move(mixer); }

    // Where a finished pass goes: the guest's own big-endian S16 stereo bytes
    // (nullptr when the buffer is not host-backed: the pass is still time).
    using PassSink = std::function<void(hw::HostAudioPointer64, hw::AudioByteOffset64)>;
    void set_pass_sink(PassSink sink) { sink_ = std::move(sink); }

    // GameEvent::Audio: the current pass ended at `now`.
    void on_pass_end(u64 now);

    // The output rate the passes are produced at (AISetDSPSampleRate).
    u32 dsp_sample_rate() const noexcept { return dsp_rate_32k_ ? 32000u : 48000u; }

    struct Address {
        u32 address;
        const char* name;
        bool continuation;
    };
    static const Address* addresses(std::size_t& count) noexcept;

    struct Stats {
        u64 passes = 0;            // AI passes completed and published
        u64 pass_bytes = 0;
        u64 silent_passes = 0;     // a pass whose buffer was not host-backed
        u64 command_lists = 0;     // AX command lists mixed
        u64 dsp_mails = 0;         // mails the DSP posted to the CPU
        u64 ai_callbacks = 0;
        u64 dsp_callbacks = 0;
        u64 transfer_completions = 0;  // native transfers' callbacks run at the DSP DMA interrupt
    };
    const Stats& stats() const noexcept { return stats_; }
    u32 last_command_list() const noexcept { return last_command_list_; }

    // A transfer a native sound store finished at once (docs/ARAM.md): its completion callback runs at the DSP DMA
    // interrupt (OS cause DSP_ARAM), in interrupt context with r3 = `request`, as ARQ's service routine ran it once
    // the hardware's DMA ended. Queued in order; one delivery runs every queued callback.
    void complete_transfer(u32 callback, u32 request);
    // The game constructor's ARInit site (0x801F2EA0, lowered): the DSP DMA interrupt unmasked.
    void dma_interrupt_init(Cpu& c);

    // Savestate (version 10): the DMA engine, the AI control and stream
    // settings and the DSP's task/mailbox state. Guest RAM holds the rest.
    void save_state(std::vector<u8>& out) const;
    bool load_state(const u8*& cursor, const u8* end);
    void reset() noexcept;

private:
    // A pass begun by AIStartDMA ends on the pass grid of the time base (see begin_pass); a reloaded pass follows
    // the last one without a gap.
    void begin_pass(u64 now, bool on_grid = false);
    void post_mail(Cpu& c, u32 mail);
    void receive_mail(Cpu& c, u32 mail);
    void boot_task(Cpu& c, u32 task);
    void insert_task(Cpu& c, u32 task);
    void remove_task(Cpu& c, u32 task);
    bool dsp_handler(Cpu& c, u32 frame, u32 step);
    bool ai_handler(Cpu& c, u32 frame, u32 step);
    bool dsp_dma_handler(Cpu& c, u32 frame, u32 step);
    // complete_transfer's queue {callback, request}. Never pending across a savestate: the store's caller spins
    // with interrupts enabled right after posting, so the delivery comes at that loop's first back edge, before
    // any wait point.
    std::deque<std::pair<u32, u32>> completions_;
    // The SDK's stream-control helpers, as their owners and the other AI
    // owners call them.
    void set_stream_volume_left(u32 volume) noexcept { volume_left_ = static_cast<u8>(volume); }
    void set_stream_volume_right(u32 volume) noexcept { volume_right_ = static_cast<u8>(volume); }
    void set_stream_play_state(u32 state) noexcept;

    Cpu& cpu_;
    GuestMemory& memory_;
    NativeInterrupts& interrupts_;
    Mixer mixer_;
    PassSink sink_;
    Stats stats_;

    // ---- the AI DMA engine --------------------------------------------------
    u32 dma_start_ = 0;      // programmed buffer (a window address, 32-byte aligned)
    u16 dma_control_ = 0;    // bit 15 enable, bits 0-14 length in 32-byte blocks
    u32 pass_start_ = 0;     // the pass in progress (latched at its start)
    u32 pass_blocks_ = 0;
    // ---- AI control and stream settings (AISet*/AIGet*) ---------------------
    bool stream_playing_ = false;   // AISetStreamPlayState
    bool stream_rate_48k_ = false;  // __AI_set_stream_sample_rate
    bool dsp_rate_32k_ = false;     // AISetDSPSampleRate (32 kHz once AIInit ran)
    u8 volume_left_ = 0;
    u8 volume_right_ = 0;
    // ---- the DSP ---------------------------------------------------------------
    bool task_running_ = false;     // the AX task booted and not done
    u32 task_ = 0;                  // its DSPTaskInfo
    bool mail_valid_ = false;       // a mail from the DSP awaits the CPU
    u32 mail_ = 0;
    bool expect_list_address_ = false;  // the AX protocol: header seen
    u32 list_size_ = 0;
    u32 last_command_list_ = 0;
};

}  // namespace gekko
