#pragma once

// Semantic hardware types for the 64-bit host runtime.
//
// Guest addresses stay 32-bit. Host sizes, GPU virtual addresses and resource
// identities are 64-bit. Completions carry both a host fence serial and the
// guest cycle at which the result becomes observable (docs:
// GODFATHER_MODERNIZATION/RECOMPILER_MODERN_HOST_ARCHITECTURE.md).

#include <cstddef>
#include <cstdint>

#include "gekko/types.h"

namespace gekko::hw {

using GuestEffective = u32;
using GuestPhysical = u32;
using GuestCycle = u64;
using HostBytes = u64;
using GpuVirtualAddress = u64;
using FenceSerial = u64;

// Audio: guest-visible addresses and hardware registers stay 32-bit.
// Host sample/frame/byte/DMA accounting is 64-bit so a running counter, ring
// index or WASAPI UINT32 cannot wrap onto the guest clock or starve the
// device callback. Never store a host pointer in GuestAudioAddress32.
using GuestAudioAddress32 = u32;
using HostAudioPointer64 = const u8*;
using SampleCounter64 = u64;
using DmaCounter64 = u64;
using AudioByteOffset64 = u64;
using AudioFrameIndex64 = u64;
using AudioTimestamp64 = u64;

template <typename Tag>
struct Handle {
    u32 index = 0;
    u32 generation = 0;

    constexpr bool valid() const noexcept { return generation != 0u; }
    constexpr u64 pack() const noexcept {
        return (static_cast<u64>(generation) << 32) | index;
    }
    static constexpr Handle from_pack(u64 value) noexcept {
        Handle h;
        h.index = static_cast<u32>(value);
        h.generation = static_cast<u32>(value >> 32);
        return h;
    }
    constexpr bool operator==(const Handle& other) const noexcept {
        return index == other.index && generation == other.generation;
    }
};

struct TextureTag {};
struct BufferTag {};
struct PipelineTag {};
struct FenceTag {};

using TextureHandle = Handle<TextureTag>;
using BufferHandle = Handle<BufferTag>;
using PipelineHandle = Handle<PipelineTag>;
using FenceHandle = Handle<FenceTag>;

// Every deferred host operation returns this. host_serial is ordinary GPU
// bookkeeping. observable_at is a function of guest state at the arming
// instruction plus retired guest cycles; host latency must never leak into it.
struct Completion {
    FenceSerial host_serial = 0;
    GuestCycle observable_at = 0;
};

}  // namespace gekko::hw
