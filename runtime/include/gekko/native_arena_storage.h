#pragma once

// The bytes of a native arena (the motion clip pool, the GCAX sound banks, CRI's stream ring): host memory owned by
// its component, outside the game's address window (2026-11-21, docs/NATIVE_WINDOWS_MIGRATION.md "the native arenas
// leave the guest window"). A reservation of the arena's size, committed in 16 KiB granules from its start as the
// component's writes reach further, as the guest-window region it replaces was: the committed prefix is what a
// checkpoint and the state report carry, so its granule and its zero fill are part of their content. The reservation
// never moves, so a pointer into the committed prefix stays valid until release. (Defined in sound_arena.cpp.)
//
// The game never sees these bytes or their host address: it holds 32-bit handles (the motion pool's addresses, the
// sound addresses) that only the owning component resolves.

#include "gekko/types.h"

namespace gekko {

class NativeArenaStorage {
public:
    static constexpr u32 kGranule = 0x4000u;  // the commit unit (the guest window's page, which the reports carry)

    NativeArenaStorage() noexcept = default;
    ~NativeArenaStorage() { release(); }
    NativeArenaStorage(const NativeArenaStorage&) = delete;
    NativeArenaStorage& operator=(const NativeArenaStorage&) = delete;

    // `size` bytes reserved (a granule multiple), nothing committed. False on a second reservation or a host refusal.
    bool reserve(u32 size) noexcept;
    // The committed prefix grown to at least `end` bytes (rounded up to the granule; zero-filled). False past the
    // reservation or on a host refusal.
    bool commit_through(u32 end) noexcept;
    void release() noexcept;
    // A checkpoint's image of the arena: reserved at `size`, `committed` bytes (a granule multiple) taken from `image`.
    bool restore(u32 size, u32 committed, const u8* image) noexcept;

    bool present() const noexcept { return bytes_ != nullptr; }
    u32 size() const noexcept { return size_; }
    u32 committed() const noexcept { return committed_; }
    // The committed bytes [offset, offset + count), or nullptr.
    u8* at(u32 offset, u32 count) noexcept {
        return bytes_ != nullptr && offset <= committed_ && count <= committed_ - offset ? bytes_ + offset : nullptr;
    }
    const u8* at(u32 offset, u32 count) const noexcept {
        return bytes_ != nullptr && offset <= committed_ && count <= committed_ - offset ? bytes_ + offset : nullptr;
    }
    const u8* data() const noexcept { return bytes_; }

private:
    u8* bytes_ = nullptr;
    u32 size_ = 0u;
    u32 committed_ = 0u;
};

// An arena as a checkpoint and the state report describe it. FORMAT BOUNDARY (2026-11-21): checkpoint version 34
// carried the arenas as records of the guest window's region list (base, size, committed bytes, kind 4 "native
// arena", name); they are still written as such records, by their components, so version-34 files load both ways
// and the report's labels stay. `label` is the window base the arena had then: a label of the record, not an address
// anything resolves.
struct NativeArenaImage {
    const char* name = nullptr;
    u32 label = 0;
    u32 size = 0;
    u32 committed = 0;
    const u8* bytes = nullptr;
};

}  // namespace gekko
