#pragma once

// A MODEL OF THE CONSOLE'S AUDIO INTERFACE, cut at the SDK's lowest functions: the audio interface's DMA engine and the OS audio
// bring-up. There is no ARAM (docs/ARAM.md): the samples live in native stores and their transfers complete on the DSP DMA
// interrupt. There is no DSP either: the AX microcode's work (mixing a frame) is done natively inside the AX frame
// (gekko/native_ax_frame.h, System::run_ax_mix), which this layer runs from its AI DMA interrupt as a step machine. The translated
// Nintendo SDK above it (AXFX, MIX), Sega's GCAX driver and CRI keep running on it, paced by the AI DMA interrupt.
//
// docs/native-time-20261005/DESIGN.md (stage 1). Every SDK function of the AI library and the OS audio bring-up that surviving guest
// code can reach is an HLE owner here (extract/native-owners.txt; the generator emits only a thunk). The owners keep the device's
// contract - DMA control words, the 5 ms pass clock, the AIDINT and the DSP DMA interrupt - as follows:
//
//   game / CRI / AX (translated guest code)
//      |  AI*, __OSInitAudioSystem (native owners)
//   NativeAudio
//      |  audio frames: the AI pass clock      GameClock (GameEvent::Audio)
//      |  the AX frame (when the SDK's AI DMA callback is the AX engine's token): gekko/native_ax_frame.h
//      |  the samples: native sound stores      gekko/sound_arena.h (docs/ARAM.md)
//      |  completions: AIDINT, DSP DMA          NativeInterrupts (event calls into the SDK callbacks)
//   AudioRuntime (WASAPI)                      the finished passes, published
//
// THE CONTRACT (what the game observes)
//  * Audio frames: a pass of the programmed DMA buffer (AIInitDMA) begins when AIStartDMA enables it and lasts its length at the DSP
//    sample rate in game time (0x280 bytes = 160 frames = 5 ms = 202,500 ticks at 32 kHz); when it ends, its bytes are published,
//    the next pass latches the programmed buffer, and the AI DMA callback runs in interrupt context (__AIDHandler's order:
//    __AID_Active guard, the callback).
//  * The DSP DMA interrupt (OS cause DSP_ARAM): each native sound store's transfer completion runs its callback there, in interrupt
//    context (complete_transfer).
//  * The SDK's RAM state (callbacks, flags, the OS interrupt mask words) is written where the SDK keeps it.
//
// What is deliberately NOT reproduced: AI registers, the AI stream (DVD audio) sample counter and its interrupt - no stream source
// exists -, DMA latency, and every spin the SDK made on them.

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

    // The mixer counted one AX frame's list (System::run_ax_mix): the report's list counter and last list.
    void note_command_list(u32 list) noexcept {
        ++stats_.command_lists;
        last_command_list_ = list;
    }

    // The AX frame (gekko/native_ax_frame.h, audio-ax stage e step 3b): when the SDK's AI DMA callback is `callback` the handler runs
    // the native frame instead of calling it. `advance` is the frame's step machine: it returns true with the guest call it wants made
    // next (in interrupt context, as the original's callbacks were), false when the frame is finished.
    using AxFrame = std::function<bool(Cpu&, u32 scratch, u32 step, u32& target, u32& r3, u32& r4)>;
    void set_ax_frame(u32 callback, AxFrame advance) {
        ax_frame_callback_ = callback;
        ax_frame_ = std::move(advance);
    }

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
        u64 command_lists = 0;     // AX frames mixed
        u64 ai_callbacks = 0;
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

    // Savestate (version 10): the DMA engine and the AI control and stream settings (the words of the former DSP task / mailbox
    // model stay as zeros). Guest RAM holds the rest.
    void save_state(std::vector<u8>& out) const;
    bool load_state(const u8*& cursor, const u8* end);
    void reset() noexcept;

private:
    // A pass begun by AIStartDMA ends on the pass grid of the time base (see begin_pass); a reloaded pass follows
    // the last one without a gap.
    void begin_pass(u64 now, bool on_grid = false);
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
    AxFrame ax_frame_;
    u32 ax_frame_callback_ = 0u;
    bool ax_frame_step(Cpu& c, u32 frame, u32 step);
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
    u32 last_command_list_ = 0;
};

}  // namespace gekko
