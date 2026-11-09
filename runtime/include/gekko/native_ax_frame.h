#pragma once

// The AX frame builder __AXNextFrame (0x803850AC), owned natively (docs/agents/audio-ax.md, stage e step 3a).
//
// The original assembles, once per AI frame at the end of __AXOutNewFrame (and twice from AXOutInit), the DSP command list that the
// NEXT frame hands to the AX microcode: a 0x300-byte list in one of two ping-pong slots (guest 0x8058ABC0 + 0x300 * n) built from the
// engine's sda state: the output mode (sda 0x5D38, AXSetMode), whether an aux A / aux B callback is registered (sda 0x5CF0 / 0x5CF4),
// the compressor switch (sda 0x5D34, set by AXInit) and the frame's output buffer. The list is a protocol between two halves of one
// device; the only reader left is the native mixer, which needs exactly the frame settings and the output buffer address. The list is
// therefore NOT written to guest memory: its frame settings go to an AxLists slot (the console's "next list", consumed one frame later;
// that lag is observable through the settings the game changes within a frame, so it is kept). Slot 0 / 1 are chosen by the list pointer
// the guest keeps in sda 0x5D2C, as the original's builder wrote there.
//
// The original also keeps the command list's cost in sda 0x5D30 (the cycles of the commands it emitted); __AXSyncPBs adds it to its
// running total, so it is reproduced exactly ("console DSP budget, preserved for identical output"): 0x1A9 + 0x2E44 + the mode's
// surround command (mode 0: 0x546, mode 1: 0x5E6) + 0xDED per aux / matrix command + 0xBB8 for the compressor + 0x2710 + 2.

#include <array>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <string>

#include "gekko/ax_pb_image.h"
#include "gekko/cpu.h"
#include "gekko/native_ax_sync.h"
#include "gekko/native_ax_voice.h"
#include "gekko/types.h"
#include "native_audio_mixer.h"

namespace gekko {

// The two command-list slots as the native mixer sees them.
struct AxLists {
    static constexpr u32 kBase = 0x8058ABC0u, kStride = 0x300u, kCount = 2u;
    struct List {
        shadow::audio::FrameSettings settings{};
        u32 output = 0u;  // the frame's output buffer (stereo address of command 0x0E)
        u32 head = 0x8058FCA0u;  // the PB chain's first block (command 0x02): the SDK's PB array; not saved (a loaded state restarts at the array)
        bool valid = false;
    };
    std::array<List, kCount> slot{};

    static constexpr bool owns(u32 address) noexcept {
        return address >= kBase && address < kBase + kCount * kStride && (address - kBase) % kStride == 0u;
    }
    static constexpr u32 index_of(u32 address) noexcept { return (address - kBase) / kStride; }
    void clear() noexcept { slot = {}; }
};

}  // namespace gekko

namespace gekko::ax_frame {

constexpr u32 kNextFrame = 0x803850ACu;
constexpr u32 kAiCallback = 0xAB5A0001u, kInit = 0x8038465Cu, kQuit = 0x803846CCu;
constexpr u32 kSdaAuxA = 0x5CF0u, kSdaAuxB = 0x5CF4u, kSdaListPtr = 0x5D2Cu, kSdaCycles = 0x5D30u, kSdaCompressor = 0x5D34u, kSdaMode = 0x5D38u;

struct Owners {
    struct Address {
        u32 address;
        const char* name;
        bool continuation;
    };
    static const Address* addresses(std::size_t& count) noexcept {
        static constexpr Address kAddresses[] = {{kNextFrame, "__AXNextFrame", false},
                                                 {kInit, "AXInit", false},
                                                 {kQuit, "AXQuit", false}};
        count = sizeof(kAddresses) / sizeof(kAddresses[0]);
        return kAddresses;
    }
};

// __AXNextFrame(r3 = the surround buffer, r4 = the frame's output buffer): the list for the next frame mix and its cycles.
inline void next_frame(Cpu& c, AxLists& lists) {
    const u32 sda = c.gpr[13];
    const u32 pointer = c.mem_read32(sda + kSdaListPtr);
    if (!AxLists::owns(pointer)) {
        c.request_stop(StopReason::ExecutionFault, "native AX frame: the list pointer (sda 0x5D2C) is not one of the two command-list slots");
        return;
    }
    shadow::audio::FrameSettings settings;
    u32 cycles = 0x1A9u + 0x2E44u;
    const u32 mode = c.mem_read32(sda + kSdaMode);
    const bool aux_a = c.mem_read32(sda + kSdaAuxA) != 0u;
    const bool aux_b = c.mem_read32(sda + kSdaAuxB) != 0u;
    if (mode == 0u) {
        settings.mode = shadow::audio::OutputMode::StereoFold;
        cycles += 0x546u;
    } else if (mode == 1u) {
        settings.mode = shadow::audio::OutputMode::OppositeFold;
        cycles += 0x5E6u;
    }
    if (mode == 2u) {
        // Mode 2 sends aux A and the Dolby matrix; aux B is not used.
        if (aux_a) {
            settings.aux_a = true;
            cycles += 0xDEDu;
        }
        settings.mode = shadow::audio::OutputMode::DolbyMatrix;
        cycles += 0xDEDu;
    } else {
        if (aux_a) {
            settings.aux_a = true;
            cycles += 0xDEDu;
        }
        if (aux_b) {
            settings.aux_b = true;
            cycles += 0xDEDu;
        }
    }
    if (c.mem_read32(sda + kSdaCompressor) != 0u) {
        settings.compressor = true;
        settings.compressor_threshold = 0x8000u;
        settings.compressor_release_frames = 10u;
        cycles += 0xBB8u;
    }
    cycles += 0x2710u + 2u;
    auto& list = lists.slot[AxLists::index_of(pointer)];
    list.settings = settings;
    list.output = c.gpr[4];
    list.head = 0x8058FCA0u;
    list.valid = true;
    c.mem_write32(sda + kSdaCycles, cycles);
}

inline void install(Cpu& cpu, AxLists& lists) {
    cpu.install_hle(kNextFrame, [&lists](Cpu& c) {
        next_frame(c, lists);
        return true;  // an owner never declines
    }, std::string("AX frame: __AXNextFrame"));
}

}  // namespace gekko::ax_frame

// ---------------------------------------------------------------------------------------------------------------------------------
// Step 3b: the frame sequencing, AXInit and AXQuit
// ---------------------------------------------------------------------------------------------------------------------------------
//
// The engine of the SDK ran from two interrupts: the AI DMA callback (__AXOutAiCallback 0x80385950, each 5 ms pass) and the DSP task's
// resume callback (__AXOutSPCallback 0x80385A20), which the DSP raised when it had taken a command list; a state word (sda 0x5D48)
// kept the two in step, and the frame (__AXOutNewFrame 0x80385798) ran from whichever came second, with the time between the two as
// the sync's cycle argument. Measured on the game (3,000 frames of Westopolis): the frame always ran from the AI callback with the
// state at 1 and the argument 0 (the DSP resumed before the next pass). There is no DSP: the AI DMA handler (NativeAudio) runs the frame as a step machine, the
// microcode's work (mixing the previous frame's list) is done inline as the original did it, in the order the original
// observed it:
//
//   1. __AXSyncPBs (native_ax_sync.h): the guest's voice parameters reach the DSP side's copy (AxPbImage), the copy-back of the
//      previous frame's state and position reaches the guest.
//   2. the mixer takes the list the previous frame built (AxLists), against the synced image, and writes the output pass.
//   3. __AXServiceCallbackStack: the drop callbacks of voices the sync dropped, then those voices go to the free stack.
//   4. the aux flush (0x80384E14): the aux A / aux B callbacks (the game's reverb and effect sends) see the aux buffers of this frame.
//   5. the frame callback (sda 0x5D58: the game's GCAX update: voices started and stopped, MIXUpdateSettings, ...).
//   6. __AXNextFrame: the list for the next frame.
//   7. the output buffer index advances and the next output pass is programmed (AIInitDMA).
//
// Not reproduced (nothing reads them): the profile record (OSGetTime stamps at 0x80594A50, handed to AXGetProfile's buffer when
// one was registered: the game does not), the depop block (0x80385FB0 / the list's first command), and the cache range operations
// on the aux buffers (no effect on guest memory here).

namespace gekko::ax_frame {

constexpr u32 kSdaState = 0x5D48u, kSdaOutIndex = 0x5D40u, kSdaDmaIndex = 0x5D44u, kSdaOutMode = 0x5D70u, kSdaFrameCallback = 0x5D58u;
constexpr u32 kSdaListIndex = 0x5D28u, kSdaCallbackStack = 0x5CE8u, kSdaAiCallback = 0x5C64u;
constexpr u32 kSdaAuxIndexA = 0x5D10u, kSdaAuxIndexB = 0x5D14u, kSdaAuxFlagA = 0x5D18u, kSdaAuxFlagB = 0x5D1Cu, kSdaAuxBuffer = 0x5D20u;
constexpr u32 kSdaAuxCtxA = 0x5CF8u, kSdaAuxCtxB = 0x5CFCu;
constexpr u32 kOutBase = 0x8058B1C0u, kMidBuffer = kOutBase + 0x780u, kAuxBase = 0x80587EC0u, kAuxBBase = 0x80589540u;
constexpr u32 kStackHeads = 0x80587DC0u, kStackTails = 0x80587E40u;
constexpr u32 kAIInitDMA = 0x80381E74u, kAIStartDMA = 0x80381EFCu, kAIStopDMA = 0x80381F14u;
constexpr u32 kOSDisableInterrupts = 0x8037611Cu, kOSRestoreInterrupts = 0x80376144u;

// What the frame needs besides the CPU: the DSP side's parameter copy, the next-frame lists and the mixer.
struct Engine {
    AxPbImage* image = nullptr;
    AxLists* lists = nullptr;
    std::function<void(u32 list)> mix;  // System::run_ax_mix: mixes the list (a slot address) into the output pass
};

namespace detail {

// 0x803848F4: push a voice on the free stack (priority 0).
inline void push_free(Cpu& c, u32 vpb) {
    const u32 head = c.mem_read32(kStackHeads);
    c.mem_write32(vpb, head);
    c.mem_write32(kStackHeads, vpb);
    c.mem_write32(vpb + 0xCu, 0u);
}

// A native AI owner called as its guest function was: r3 / r4 are its arguments, the result is left in r3.
inline bool serve(Cpu& c, u32 address, u32 r3, u32 r4 = 0u) {
    c.gpr[3] = r3;
    c.gpr[4] = r4;
    return c.serve_owner(address);
}

// The words a callback receives as its buffer list (aux A: three or four buffer addresses; aux B: three). They sit in the first bytes
// of the SDK's former PB array in guest memory, which nothing uses any more.
constexpr u32 kAuxArguments = 0x8058FCA0u;

inline void write_buffers(Cpu& c, u32 p, bool fourth) {
    c.mem_write32(kAuxArguments, p);
    c.mem_write32(kAuxArguments + 4u, p + 0x280u);
    c.mem_write32(kAuxArguments + 8u, p + 0x500u);
    if (fourth) c.mem_write32(kAuxArguments + 12u, p + 0x1B80u);
}

}  // namespace detail

// A guest call the frame wants made in interrupt context before it can go on. The callbacks run as the delivery's own guest calls: the
// game's frame callback wakes threads, and the delivery's end reschedules, which a call nested inside native code could not do.
struct Call {
    u32 target = 0, r3 = 0, r4 = 0;
};

// What the frame does next once a requested call has returned; kept in the first of the four scratch words of the delivery (guest
// memory, so a state saved between two calls resumes), the dropped voice being serviced in the second.
enum Phase : u32 {
    kStart = 0,
    kAfterDrop = 1,       // a drop callback returned: its voice is unlinked and freed
    kPopStack = 2,        // __AXServiceCallbackStack: the next dropped voice
    kAuxASend = 3,        // the aux A callback
    kAfterAuxA = 4,
    kAuxBSend = 5,        // the aux B callback
    kAfterAuxB = 6,
    kAfterFrameCallback = 7,
};

// The AI DMA callback __AXOutAiCallback 0x80385950 and the __AXOutNewFrame 0x80385798(0) it runs, as a step machine in the order the
// original ran it (see the header comment). Step 0 starts the frame; every call the frame asks for is made by the caller (in interrupt
// context, MSR[EE] clear, as the original's callbacks were) and its return resumes the frame with the next step. Returns true with
// `call` filled when a call is wanted, false when the frame is finished (or the run was stopped).
inline bool frame_advance(Cpu& c, Engine& e, u32 scratch, u32 step, Call& call) {
    const u32 sda = c.gpr[13];
    u32 phase = step == 0u ? static_cast<u32>(kStart) : c.mem_read32(scratch);
    if (phase == kStart) {
        if (c.mem_read32(sda + kSdaState) != 1u || c.mem_read32(sda + kSdaOutMode) != 0u) {
            c.request_stop(StopReason::ExecutionFault,
                           "native AX frame: the AI callback found the engine in a state the DSP handshake never produced");
            return false;
        }
        c.mem_write32(sda + kSdaState, 0u);
        c.gpr[3] = 0u;  // the cycles the time between the two interrupts added: always 0 (see above)
        ax_sync::sync_pbs(c, *e.image);
        // 0x80385070: the list the previous frame built is mixed now; the builder writes the other slot.
        const u32 index = c.mem_read32(sda + kSdaListIndex) & 1u;
        const u32 next = (index + 1u) & 1u;
        c.mem_write32(sda + kSdaListIndex, next);
        c.mem_write32(sda + 0x5D2Cu, AxLists::kBase + next * AxLists::kStride);
        e.mix(AxLists::kBase + index * AxLists::kStride);
        if (c.stopped()) return false;
        phase = kPopStack;
    } else if (phase == kAfterDrop) {
        const u32 vpb = c.mem_read32(scratch + 4u);
        ax_voice::unlink_from_stack(c, vpb);
        detail::push_free(c, vpb);
        phase = kPopStack;
    }
    if (phase == kPopStack) {  // 0x80384718 __AXServiceCallbackStack
        for (;;) {
            const u32 vpb = c.mem_read32(sda + kSdaCallbackStack);
            if (vpb == 0u) break;
            c.mem_write32(sda + kSdaCallbackStack, c.mem_read32(vpb + 8u));
            if (c.mem_read32(vpb + 0xCu) == 0u) continue;
            if (const u32 callback = c.mem_read32(vpb + 0x10u); callback != 0u) {
                c.mem_write32(scratch, kAfterDrop);
                c.mem_write32(scratch + 4u, vpb);
                call = {callback, vpb, 0u};
                return true;
            }
            ax_voice::unlink_from_stack(c, vpb);
            detail::push_free(c, vpb);
        }
        // 0x80384E14: the aux buffers of this frame, by the current indices.
        const u32 a = c.mem_read32(sda + kSdaAuxIndexA), b = c.mem_read32(sda + kSdaAuxIndexB);
        c.mem_write32(sda + 0x5D00u, kAuxBase + a * 0x780u);
        c.mem_write32(sda + 0x5D04u, kAuxBase + b * 0x780u);
        c.mem_write32(sda + 0x5D08u, kAuxBase + a * 0x780u + 0x1680u);
        c.mem_write32(sda + 0x5D0Cu, kAuxBase + b * 0x780u + 0x1680u);
        phase = kAuxASend;
    }
    const u32 mode = c.mem_read32(sda + 0x5D38u);
    if (phase == kAuxASend) {
        if (const u32 callback = c.mem_read32(sda + 0x5CF0u); callback != 0u) {
            detail::write_buffers(c, kAuxBase + c.mem_read32(sda + kSdaAuxBuffer) * 0x780u, mode == 2u);
            c.mem_write32(scratch, kAfterAuxA);
            call = {callback, detail::kAuxArguments, c.mem_read32(sda + kSdaAuxCtxA)};
            return true;
        }
        phase = kAuxBSend;
    } else if (phase == kAfterAuxA) {
        phase = kAuxBSend;
    }
    if (phase == kAuxBSend) {
        if (const u32 callback = c.mem_read32(sda + 0x5CF4u); callback != 0u && mode != 2u) {
            detail::write_buffers(c, kAuxBase + c.mem_read32(sda + kSdaAuxBuffer) * 0x780u + 0x1680u, false);
            c.mem_write32(scratch, kAfterAuxB);
            call = {callback, detail::kAuxArguments, c.mem_read32(sda + kSdaAuxCtxB)};
            return true;
        }
        phase = kAfterAuxB;
    }
    if (phase == kAfterAuxB) {
        // The aux buffer indices advance, then the game's frame callback.
        const u32 a = c.mem_read32(sda + kSdaAuxIndexA), b = c.mem_read32(sda + kSdaAuxIndexB), k = c.mem_read32(sda + kSdaAuxBuffer);
        c.mem_write32(sda + kSdaAuxIndexA, (a + 1u) % 3u);
        c.mem_write32(sda + kSdaAuxIndexB, (b + 1u) % 3u);
        c.mem_write32(sda + kSdaAuxFlagA, (c.mem_read32(sda + kSdaAuxFlagA) + 1u) & 1u);
        c.mem_write32(sda + kSdaAuxFlagB, (c.mem_read32(sda + kSdaAuxFlagB) + 1u) & 1u);
        c.mem_write32(sda + kSdaAuxBuffer, (k + 1u) % 3u);
        if (const u32 callback = c.mem_read32(sda + kSdaFrameCallback); callback != 0u) {
            c.mem_write32(scratch, kAfterFrameCallback);
            call = {callback, 0u, 0u};
            return true;
        }
    }
    // The frame callback returned (or there is none): the next frame's list, the output buffer index and the next pass.
    const u32 out = c.mem_read32(sda + kSdaOutIndex);
    c.gpr[3] = kMidBuffer;
    c.gpr[4] = kOutBase + 0x280u * out;
    next_frame(c, *e.lists);
    if (c.stopped()) return false;
    const u32 pass = (out + 1u) & 1u;
    c.mem_write32(sda + kSdaOutIndex, pass);
    detail::serve(c, kAIInitDMA, kOutBase + 0x280u * pass, 0x280u);
    c.mem_write32(sda + kSdaState, 1u);  // what the DSP's resume callback set
    return false;
}

// 0x8038478C / 0x80384840: the voice stacks and the callback stack, emptied.
inline void clear_stacks(Cpu& c) {
    c.mem_write32(c.gpr[13] + kSdaCallbackStack, 0u);
    for (u32 i = 0; i < 32u; ++i) {
        c.mem_write32(kStackHeads + i * 4u, 0u);
        c.mem_write32(kStackTails + i * 4u, 0u);
    }
}

inline void zero(Cpu& c, u32 address, u32 bytes) {
    for (u32 i = 0; i < bytes; i += 4u) c.mem_write32(address + i, 0u);
}

// 0x80384680 AXInit: the AX SDK's state, native. The DSP task, its INIT handshake (a spin on sda 0x5D5C) and the version banner of
// the original are not performed.
inline void init(Cpu& c, AxPbImage& image, AxLists& lists) {
    const u32 sda = c.gpr[13];
    clear_stacks(c);
    // 0x80386E88: the budget, the voice records and the DSP side's parameter blocks.
    c.mem_write32(sda + ax_sync::kSdaBudget, static_cast<u32>((u64{c.mem_read32(0x800000F8u)} * 0x51EB851Full) >> 39));
    c.mem_write32(sda + ax_sync::kSdaTotalCost, 0u);
    image.clear();
    zero(c, 0x805989A0u, 0x8B00u);            // the AXVPB array
    for (u32 i = 0; i < AxPbImage::kCount; ++i) {
        const u32 vpb = 0x805989A0u + i * 0x22Cu;
        const u32 pb = AxPbImage::kBase + i * AxPbImage::kStride;
        const u32 blob = 0x8058FCA0u + 0x3D00u + i * 0x40u;
        const u32 ring = AxPbImage::kRingBase + i * AxPbImage::kRingStride;
        c.mem_write32(vpb + 0x18u, i);
        c.mem_write32(vpb + 0x30u, vpb + 0x34u);
        c.mem_write32(vpb + 0x134u, blob);
        c.mem_write32(vpb + 0x1Cu, 0x2000A4u);
        const u32 next = i == AxPbImage::kCount - 1u ? 0u : pb + AxPbImage::kStride;
        c.mem_write16(vpb + 0x138u, static_cast<u16>(next >> 16));
        c.mem_write16(vpb + 0x13Au, static_cast<u16>(next));
        c.mem_write16(vpb + 0x13Cu, static_cast<u16>(pb >> 16));
        c.mem_write16(vpb + 0x13Eu, static_cast<u16>(pb));
        c.mem_write16(vpb + 0x170u, static_cast<u16>(blob >> 16));
        c.mem_write16(vpb + 0x172u, static_cast<u16>(blob));
        c.mem_write16(vpb + 0x186u, static_cast<u16>(ring >> 16));
        c.mem_write16(vpb + 0x188u, static_cast<u16>(ring));
        image.w32(i, 0x0u, next);
        image.w16(i, 0x4u, static_cast<u16>(pb >> 16));
        image.w16(i, 0x6u, static_cast<u16>(pb));
        image.w16(i, 0x38u, static_cast<u16>(blob >> 16));
        image.w16(i, 0x3Au, static_cast<u16>(blob));
        image.w16(i, 0x4Eu, static_cast<u16>(ring >> 16));
        image.w16(i, 0x50u, static_cast<u16>(ring));
        detail::push_free(c, vpb);
    }
    // 0x80384BC8: the aux sends.
    for (const u32 word : {0x5CF0u, 0x5CF4u, 0x5CF8u, 0x5CFCu}) c.mem_write32(sda + word, 0u);
    c.mem_write32(sda + kSdaAuxIndexA, 0u);
    c.mem_write32(sda + kSdaAuxIndexB, 1u);
    c.mem_write32(sda + kSdaAuxFlagA, 0u);
    c.mem_write32(sda + kSdaAuxFlagB, 1u);
    c.mem_write32(sda + kSdaAuxBuffer, 2u);
    zero(c, kAuxBase, 0x780u);
    zero(c, kAuxBBase, 0x780u);
    // 0x8038575C: the output mode, the compressor and the first list slot.
    c.mem_write32(sda + 0x5D38u, 0u);
    c.mem_write32(sda + kSdaListIndex, 0u);
    c.mem_write32(sda + 0x5D2Cu, AxLists::kBase);
    c.mem_write32(sda + kSdaCompressor, 1u);
    // 0x80385B68 __AXOutInit(0): the output buffers, the first list, the AI callback and the first pass.
    c.mem_write32(sda + kSdaOutIndex, 0u);
    c.mem_write32(sda + kSdaDmaIndex, 0u);
    c.mem_write32(sda + kSdaOutMode, 0u);
    c.mem_write32(sda + 0x5D64u, 0u);
    zero(c, kOutBase, 0xA00u);
    lists.clear();
    c.mem_write32(sda + kSdaAiCallback, kAiCallback);  // AIRegisterDMACallback: the engine's token, not a code address (nothing may call it)
    c.gpr[3] = kMidBuffer;
    c.gpr[4] = kOutBase + 0x280u;
    next_frame(c, lists);
    c.mem_write32(sda + kSdaState, 1u);
    c.mem_write32(sda + kSdaFrameCallback, 0u);
    detail::serve(c, kAIInitDMA, kOutBase, 0x280u);
    detail::serve(c, kAIStartDMA, 0u);
}

// 0x803846CC AXQuit.
inline void quit(Cpu& c) {
    const u32 sda = c.gpr[13];
    clear_stacks(c);
    for (const u32 word : {0x5CF0u, 0x5CF4u}) c.mem_write32(sda + word, 0u);  // 0x80384CAC: the aux callbacks
    detail::serve(c, kOSDisableInterrupts, 0u);
    const u32 previous = c.gpr[3];
    c.mem_write32(sda + kSdaFrameCallback, 0u);
    detail::serve(c, kAIStopDMA, 0u);
    detail::serve(c, kOSRestoreInterrupts, previous);
}

inline void install_engine(Cpu& cpu, Engine& engine) {
    cpu.install_hle(kInit, [&engine](Cpu& c) {
        init(c, *engine.image, *engine.lists);
        return true;
    }, std::string("AX frame: AXInit"));
    cpu.install_hle(kQuit, [](Cpu& c) {
        quit(c);
        return true;
    }, std::string("AX frame: AXQuit"));
}

}  // namespace gekko::ax_frame
