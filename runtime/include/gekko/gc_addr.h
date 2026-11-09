#pragma once

// A GameCube address as host code sees it (docs/memory-architecture-20261006).
//
// The translated game is 32-bit PowerPC code: every structure it builds holds
// 32-bit pointers, and those pointers keep their GameCube form (cached view
// 0x80000000 + offset, uncached view 0xC0000000 + offset) because the game
// computes with them. A GcAddr is that value and nothing else: it is not a host
// pointer, it cannot be dereferenced, and it never becomes one by arithmetic.
// The only way from a GcAddr to bytes is GuestMemory (gekko/guest_memory.h),
// which resolves it against what the native memory system actually maps and
// refuses everything else.

#include "gekko/types.h"

namespace gekko {

class GcAddr {
public:
    constexpr GcAddr() noexcept = default;
    constexpr explicit GcAddr(u32 value) noexcept : value_(value) {}

    constexpr u32 value() const noexcept { return value_; }
    constexpr bool null() const noexcept { return value_ == 0u; }

    // Offsetting stays in the guest's own 32-bit arithmetic (it wraps exactly
    // as the guest's address arithmetic does).
    constexpr GcAddr operator+(u32 offset) const noexcept { return GcAddr(value_ + offset); }
    constexpr GcAddr operator-(u32 offset) const noexcept { return GcAddr(value_ - offset); }
    constexpr u32 operator-(GcAddr other) const noexcept { return value_ - other.value_; }

    friend constexpr bool operator==(GcAddr a, GcAddr b) noexcept { return a.value_ == b.value_; }
    friend constexpr bool operator!=(GcAddr a, GcAddr b) noexcept { return a.value_ != b.value_; }
    friend constexpr bool operator<(GcAddr a, GcAddr b) noexcept { return a.value_ < b.value_; }

private:
    u32 value_ = 0u;
};

}  // namespace gekko
