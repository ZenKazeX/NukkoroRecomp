#pragma once

// A native store of sound samples that the AX voices address (docs/ARAM.md): the GCAX banks and CRI's stream ring.
// The game sees the store at its own sound addresses: its code keeps building voice addresses as base + offset from
// the base handed back where it used to allocate ARAM. Those addresses are 32-bit handles, not guest addresses: the
// store's bytes are native storage owned here (NativeArenaStorage, outside the game's address window since
// 2026-11-21), and only native code resolves a sound address - the AX voice decode reads a voice whose addresses lie
// in a store straight from that store (source()), the copies into it go through copy_in.
//
// The store is a window of the sound address space whose size is a power of two and whose start is a multiple of
// it, so a mask turns a sound byte address into an offset. A store the game allocates now (reserve) sits at its
// default base, past the 16 MiB of ARAM. A store adopted from a savestate made before ARAM was retired (adopt) sits
// where that state's ARAM block was, so the state's own addresses stay valid. Granules commit as bytes land in them.

#include <string>
#include <vector>

#include "gekko/native_arena_storage.h"
#include "gekko/types.h"

namespace gekko {

class GuestMemory;

class SoundArena {
public:
    // `record_label`: the window base the store had while it was a region of the game's address window; now only the
    // label of its checkpoint record and its state-report entry (NativeArenaImage).
    SoundArena(GuestMemory& memory, u32 default_base, u32 span, u32 record_label, const char* name) noexcept
        : memory_(memory), default_base_(default_base), span_(span), record_label_(record_label), name_(name),
          base_(default_base), held_(span) {}

    // Where a voice's samples are: the store's bytes, the mask that turns a sound byte address into an offset, and
    // the first sound byte address past the committed bytes. Null while nothing is reserved.
    struct Source {
        const u8* bytes = nullptr;
        u32 mask = 0;
        u32 end = 0;
    };
    Source source() const noexcept;
    // The store's sound addresses: a voice starting here belongs to this store.
    bool holds(u32 sound_byte_address) const noexcept {
        return sound_byte_address >= base_ && sound_byte_address - base_ < held_;
    }

    u32 sound_base() const noexcept { return base_; }
    u32 reserved() const noexcept { return reserve_; }
    u32 requested() const noexcept { return requested_; }
    u32 committed() const noexcept { return storage_.committed(); }

    // The game's allocation of `bytes` at the default base: the storage reserved, nothing committed. False with `why`
    // set on a request the span cannot hold, a second reserve, or a host refusal.
    bool reserve(u32 bytes, std::string& why);
    // A savestate's ARAM block [base, base + bytes) taken over as this store (docs/ARAM.md "Old savestates"): the
    // smallest aligned window holding it, reserved; `image` (the block's bytes) written into it.
    bool adopt(u32 base, u32 bytes, const u8* image, std::string& why);
    // A savestate is being restored: discard drops the storage and the placement (the file's records restore what it
    // names); forget drops the placement only; resume takes storage the state's record restored at the default base
    // (a state saved before version 24 by a build that had the store) back as reserved.
    void discard() noexcept;
    void forget() noexcept;
    bool resume(u32 requested, std::string& why);
    // The whole reserve committed now (a store whose reader may run ahead of its writer: the bytes not yet written
    // read as zero, as never-written ARAM did).
    bool commit_reserve(std::string& why);
    // The game's release of it: the storage freed.
    bool release(std::string& why);
    // `bytes` of the game's memory at `from` copied to the sound address `to`, committing as needed.
    bool copy_in(u32 to, u32 from, u32 bytes, std::string& why);

    // Savestate (version 24): the store's placement, after the audio block.
    void save_state(std::vector<u8>& out) const;
    bool load_state(const u8*& cursor, const u8* end, std::string& why);
    // The store's bytes for a checkpoint record and the state report (NativeArenaImage; nothing while no storage is
    // reserved), and their restore from such a record.
    bool image(NativeArenaImage& out) const noexcept;
    u32 record_label() const noexcept { return record_label_; }
    bool restore_image(u32 size, u32 committed, const u8* bytes, std::string& why);

private:
    bool reserve_window(u32 window, u32 size, std::string& why);
    bool commit_through(u32 offset_end, std::string& why);

    GuestMemory& memory_;  // the game's memory, which copy_in reads (never the store's bytes)
    u32 default_base_;
    u32 span_;
    u32 record_label_;
    const char* name_;
    NativeArenaStorage storage_;
    u32 base_;           // the first sound address the game uses
    u32 held_;           // the sound addresses from base_ this store answers for
    u32 window_ = 0;     // the window's first sound address (a multiple of reserve_)
    u32 reserve_ = 0;    // the window's size, a power of two; 0 while nothing is reserved
    u32 requested_ = 0;  // the game's request (what its release gets back)
};

}  // namespace gekko
