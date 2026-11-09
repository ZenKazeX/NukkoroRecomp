// A native store of sound samples (include/gekko/sound_arena.h, docs/ARAM.md).

#include "gekko/sound_arena.h"

#include <cstring>
#include <string>

#include "gekko/guest_memory.h"

namespace gekko {

namespace {

void put32(std::vector<u8>& out, u32 value) {
    for (int shift = 24; shift >= 0; shift -= 8)
        out.push_back(static_cast<u8>(value >> shift));
}

bool get32(const u8*& cursor, const u8* end, u32& value) {
    if (end - cursor < 4) return false;
    value = (static_cast<u32>(cursor[0]) << 24) | (static_cast<u32>(cursor[1]) << 16) |
            (static_cast<u32>(cursor[2]) << 8) | cursor[3];
    cursor += 4;
    return true;
}

}  // namespace

SoundArena::Source SoundArena::source() const noexcept {
    const GuestMemory::Region* region = reserve_ != 0u ? memory_.region_at(arena_base_) : nullptr;
    if (region == nullptr)
        return {};
    return {region->host, reserve_ - 1u, window_ + region->committed};
}

u32 SoundArena::committed() const noexcept {
    const GuestMemory::Region* region = reserve_ != 0u ? memory_.region_at(arena_base_) : nullptr;
    return region != nullptr ? region->committed : 0u;
}

bool SoundArena::map_window(u32 window, u32 size, std::string& why) {
    if (reserve_ != 0u) {
        why = "reserved twice without a release";
        return false;
    }
    if (!memory_.map(arena_base_, size, 0u, GuestMemory::RegionKind::Arena, name_)) {
        why = "the arena could not be mapped";
        return false;
    }
    window_ = window;
    reserve_ = size;
    return true;
}

bool SoundArena::commit_through(u32 offset_end, std::string& why) {
    const GuestMemory::Region* region = memory_.region_at(arena_base_);
    const u32 needed = (offset_end + GuestMemory::kGranule - 1u) & ~(GuestMemory::kGranule - 1u);
    if (region == nullptr || needed > reserve_ ||
        (region->committed < needed && !memory_.commit(arena_base_, needed))) {
        why = "the arena could not commit " + std::to_string(needed) + " bytes";
        return false;
    }
    return true;
}

bool SoundArena::reserve(u32 bytes, std::string& why) {
    u32 size = GuestMemory::kGranule;
    while (size < bytes && size < span_)
        size <<= 1;
    if (bytes == 0u || size < bytes || (default_base_ % size) != 0u) {
        why = "a request of " + std::to_string(bytes) + " bytes does not fit the sound address span";
        return false;
    }
    if (!map_window(default_base_, size, why))
        return false;
    base_ = default_base_;
    held_ = span_;
    requested_ = bytes;
    return true;
}

bool SoundArena::adopt(u32 base, u32 bytes, const u8* image, std::string& why) {
    if (bytes == 0u) {
        why = "an empty block to adopt";
        return false;
    }
    // The smallest power of two whose aligned window holds the whole block.
    u32 size = GuestMemory::kGranule;
    while (size < 0x80000000u && (size < bytes || (base & ~(size - 1u)) != ((base + bytes - 1u) & ~(size - 1u))))
        size <<= 1;
    const u32 window = base & ~(size - 1u);
    if (!map_window(window, size, why))
        return false;
    base_ = base;
    held_ = bytes;
    requested_ = bytes;
    if (!commit_through(base - window + bytes, why))
        return false;
    std::memcpy(memory_.region_at(arena_base_)->host + (base - window), image, bytes);
    return true;
}

void SoundArena::forget() noexcept {
    base_ = default_base_;
    held_ = span_;
    window_ = 0u;
    reserve_ = 0u;
    requested_ = 0u;
}

bool SoundArena::resume(u32 requested, std::string& why) {
    const GuestMemory::Region* region = memory_.region_at(arena_base_);
    if (region == nullptr || region->base != arena_base_ || (default_base_ % region->size) != 0u) {
        why = std::string(name_) + ": no region to resume";
        return false;
    }
    base_ = default_base_;
    held_ = span_;
    window_ = default_base_;
    reserve_ = region->size;
    requested_ = requested;
    return true;
}

bool SoundArena::commit_reserve(std::string& why) {
    if (reserve_ == 0u) {
        why = "the arena could not commit its reserve";
        return false;
    }
    return commit_through(reserve_, why);
}

bool SoundArena::release(std::string& why) {
    if (reserve_ == 0u) {
        why = "released while nothing is reserved";
        return false;
    }
    if (!memory_.unmap(arena_base_)) {
        why = "the arena could not be unmapped";
        return false;
    }
    forget();
    return true;
}

bool SoundArena::copy_in(u32 to, u32 from, u32 bytes, std::string& why) {
    if (reserve_ == 0u || to < base_ || bytes > held_ || to - base_ > held_ - bytes) {
        why = "a copy outside the store";
        return false;
    }
    if (!commit_through(to - window_ + bytes, why))
        return false;
    const u8* data = memory_.span(GcAddr(from), bytes);
    if (bytes != 0u && data == nullptr) {
        why = "a copy from outside mapped memory";
        return false;
    }
    if (bytes != 0u)
        std::memcpy(memory_.region_at(arena_base_)->host + (to - window_), data, bytes);
    return true;
}

void SoundArena::save_state(std::vector<u8>& out) const {
    put32(out, base_);
    put32(out, held_);
    put32(out, window_);
    put32(out, reserve_);
    put32(out, requested_);
}

bool SoundArena::load_state(const u8*& cursor, const u8* end, std::string& why) {
    u32 base = 0, held = 0, window = 0, reserve = 0, requested = 0;
    if (!get32(cursor, end, base) || !get32(cursor, end, held) || !get32(cursor, end, window) ||
        !get32(cursor, end, reserve) || !get32(cursor, end, requested)) {
        why = std::string(name_) + ": truncated";
        return false;
    }
    const GuestMemory::Region* region = memory_.region_at(arena_base_);
    if (reserve != 0u && (region == nullptr || region->base != arena_base_ || region->size != reserve)) {
        why = std::string(name_) + ": the state's region is missing or has another size";
        return false;
    }
    if (reserve == 0u && region != nullptr) {
        why = std::string(name_) + ": the state maps a region it does not reserve";
        return false;
    }
    base_ = base;
    held_ = held;
    window_ = window;
    reserve_ = reserve;
    requested_ = requested;
    return true;
}

}  // namespace gekko
