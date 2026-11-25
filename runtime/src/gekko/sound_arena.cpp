// A native store of sound samples (include/gekko/sound_arena.h, docs/ARAM.md), and the native arenas' storage
// (include/gekko/native_arena_storage.h).

#include "gekko/sound_arena.h"

#include <cstring>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include "gekko/guest_memory.h"

namespace gekko {

// ---- NativeArenaStorage ------------------------------------------------------------------------------------------

bool NativeArenaStorage::reserve(u32 size) noexcept {
    if (bytes_ != nullptr || size == 0u || (size % kGranule) != 0u) return false;
#if defined(_WIN32)
    bytes_ = static_cast<u8*>(VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS));
#else
    void* p = mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    bytes_ = p == MAP_FAILED ? nullptr : static_cast<u8*>(p);
#endif
    if (bytes_ == nullptr) return false;
    size_ = size;
    committed_ = 0u;
    return true;
}

bool NativeArenaStorage::commit_through(u32 end) noexcept {
    if (bytes_ == nullptr) return false;
    const u64 needed = (u64{end} + kGranule - 1u) & ~u64{kGranule - 1u};
    if (needed > size_) return false;
    if (needed <= committed_) return true;
#if defined(_WIN32)
    if (VirtualAlloc(bytes_ + committed_, static_cast<SIZE_T>(needed - committed_), MEM_COMMIT, PAGE_READWRITE) !=
        bytes_ + committed_)
        return false;
#else
    if (mprotect(bytes_ + committed_, static_cast<std::size_t>(needed - committed_), PROT_READ | PROT_WRITE) != 0)
        return false;
#endif
    committed_ = static_cast<u32>(needed);
    return true;
}

void NativeArenaStorage::release() noexcept {
    if (bytes_ == nullptr) return;
#if defined(_WIN32)
    VirtualFree(bytes_, 0, MEM_RELEASE);
#else
    munmap(bytes_, size_);
#endif
    bytes_ = nullptr;
    size_ = 0u;
    committed_ = 0u;
}

// In place when the storage has the record's size and commits no more than it (a pointer a reader holds stays valid,
// as a restored guest region's did); otherwise reserved again at the record's size, so bytes committed past the saved
// prefix do not survive the restore.
bool NativeArenaStorage::restore(u32 size, u32 committed, const u8* image) noexcept {
    if (committed > size || (committed % kGranule) != 0u) return false;
    if (bytes_ == nullptr || size_ != size || committed_ > committed) {
        release();
        if (!reserve(size)) return false;
    }
    if (!commit_through(committed)) {
        release();
        return false;
    }
    if (committed != 0u) std::memcpy(bytes_, image, committed);
    return true;
}

// ---- SoundArena ------------------------------------------------------------------------------------------------------

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
    if (reserve_ == 0u || !storage_.present())
        return {};
    return {storage_.data(), reserve_ - 1u, window_ + storage_.committed()};
}

bool SoundArena::reserve_window(u32 window, u32 size, std::string& why) {
    if (reserve_ != 0u || storage_.present()) {
        why = "reserved twice without a release";
        return false;
    }
    if (!storage_.reserve(size)) {
        why = "the store's storage could not be reserved";
        return false;
    }
    window_ = window;
    reserve_ = size;
    return true;
}

bool SoundArena::commit_through(u32 offset_end, std::string& why) {
    if (offset_end > reserve_ || !storage_.commit_through(offset_end)) {
        const u32 needed = (offset_end + NativeArenaStorage::kGranule - 1u) & ~(NativeArenaStorage::kGranule - 1u);
        why = "the store could not commit " + std::to_string(needed) + " bytes";
        return false;
    }
    return true;
}

bool SoundArena::reserve(u32 bytes, std::string& why) {
    u32 size = NativeArenaStorage::kGranule;
    while (size < bytes && size < span_)
        size <<= 1;
    if (bytes == 0u || size < bytes || (default_base_ % size) != 0u) {
        why = "a request of " + std::to_string(bytes) + " bytes does not fit the sound address span";
        return false;
    }
    if (!reserve_window(default_base_, size, why))
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
    u32 size = NativeArenaStorage::kGranule;
    while (size < 0x80000000u && (size < bytes || (base & ~(size - 1u)) != ((base + bytes - 1u) & ~(size - 1u))))
        size <<= 1;
    const u32 window = base & ~(size - 1u);
    if (!reserve_window(window, size, why))
        return false;
    base_ = base;
    held_ = bytes;
    requested_ = bytes;
    if (!commit_through(base - window + bytes, why))
        return false;
    std::memcpy(storage_.at(base - window, bytes), image, bytes);
    return true;
}

void SoundArena::discard() noexcept {
    storage_.release();
    forget();
}

void SoundArena::forget() noexcept {
    base_ = default_base_;
    held_ = span_;
    window_ = 0u;
    reserve_ = 0u;
    requested_ = 0u;
}

bool SoundArena::resume(u32 requested, std::string& why) {
    if (!storage_.present() || (default_base_ % storage_.size()) != 0u) {
        why = std::string(name_) + ": no stored bytes to resume";
        return false;
    }
    base_ = default_base_;
    held_ = span_;
    window_ = default_base_;
    reserve_ = storage_.size();
    requested_ = requested;
    return true;
}

bool SoundArena::commit_reserve(std::string& why) {
    if (reserve_ == 0u) {
        why = "the store could not commit its reserve";
        return false;
    }
    return commit_through(reserve_, why);
}

bool SoundArena::release(std::string& why) {
    if (reserve_ == 0u) {
        why = "released while nothing is reserved";
        return false;
    }
    discard();
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
        std::memcpy(storage_.at(to - window_, bytes), data, bytes);
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
    if (reserve != 0u && (!storage_.present() || storage_.size() != reserve)) {
        why = std::string(name_) + ": the state's stored bytes are missing or have another size";
        return false;
    }
    if (reserve == 0u && storage_.present()) {
        why = std::string(name_) + ": the state stores bytes it does not reserve";
        return false;
    }
    base_ = base;
    held_ = held;
    window_ = window;
    reserve_ = reserve;
    requested_ = requested;
    return true;
}

bool SoundArena::image(NativeArenaImage& out) const noexcept {
    if (!storage_.present())
        return false;
    out = NativeArenaImage{name_, record_label_, storage_.size(), storage_.committed(), storage_.data()};
    return true;
}

bool SoundArena::restore_image(u32 size, u32 committed, const u8* bytes, std::string& why) {
    if (!storage_.restore(size, committed, bytes)) {
        why = std::string(name_) + ": the record's bytes could not be restored";
        return false;
    }
    return true;
}

}  // namespace gekko
