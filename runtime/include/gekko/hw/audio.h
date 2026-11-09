#pragma once

// Host audio contract for the recompiler runtime.
//
// Guest timing is authoritative: the AI DMA engine publishes the blocks it
// reads on retired-instruction cadence. The host never changes pitch, never
// servos the ring level into the pacer, and never waits on the renderer.
// AudioRuntime (gekko/audio_runtime.h) is the implementation.
//
// GuestAudioAddress32 is the original guest-visible DMA/PB/ARAM address.
// HostAudioPointer64 is the native pointer that address resolves to. Host
// sample, DMA, byte, frame and timestamp counters are 64-bit; guest AI/DSP
// registers that the game can read stay 32-bit.

#include "gekko/audio_runtime.h"
#include "gekko/hw/types.h"

namespace gekko::hw {

using AudioHost = AudioRuntime;

// Stall bound, not a latency target. 16,384 stereo frames is ~512 ms of the
// AI source rate. Overflow drops the incoming block whole and is counted.
inline constexpr AudioFrameIndex64 kAudioRingFrames = 16384;

}  // namespace gekko::hw
