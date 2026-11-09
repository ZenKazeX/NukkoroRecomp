#pragma once

// The game's ONE archive layer in C++ (docs/NATIVE_STREAMING_REPLACEMENT_PLAN.md Phase 4, contract C4 of
// docs/STREAMING_BEHAVIORAL_CONTRACTS.md; the word-by-word specification is the session note spec_ONE_layer.md, copied
// into docs/ONE_LAYER.md). GEKKO_NATIVE_ONE=1 installs the owners; default off until the user accepts it.
//
// The archive object (0x58 bytes, no vtable): +0x00 the path of a synchronous open (byte 0 = 0 when none), +0x2C the
// whole-file buffer of an asynchronous open, +0x30 its size, +0x34 the directory ("One Ver ..."), +0x38 the
// allocation (synchronous) or the buffer + 0xC, +0x3C the data size, +0x40 an allocator object (no caller passes one),
// +0x44 the async-pending byte, +0x50 / +0x54 zeroed by the ctor. Open = +0x2C != 0 or the name byte != 0. The
// directory: the count at +0x10 (byte-swapped by the parse), entries of 0x38 bytes at +0x34 + index * 0x38, slots 0 and
// 1 never visited; an entry's name at +0, size +0x2C, data pointer +0x30 (an offset until the parse), flags +0x34.
//
// Owned (batch 4a), each with the original's results and guest-visible writes:
//   0x8004CA50 Entry(one, index)          a leaf: 0 when not open, index < 2 or index >= count + 2 (signed)
//   0x8004CB20 IsOpen(one)
//   0x8004C938 FindName(one, name)       the query uppercased ('a'..'z' only), the first entry whose name equals it
//   0x8004C878 FindSubstr(one, sub, start) the first entry from `start` whose name contains `sub`
//   0x8004CD38 ParseDirectory(one)       the version, the count's swap, per entry: the 0.50 layout's copy, the swaps of
//                                        +0x2C / +0x30 / +0x34 and +0x30 made absolute
//   0x8004CAC0 Adopt(one, buffer, size)  Close, the fields, the parse
//   0x8004CB48 Close(one)                the buffer freed through the RenderWare free 0x80049B5C (game code: the heap),
//                                        the fields cleared
//   0x8004CFD8 One(one, path, allocator) the ctor's fields; a path opens as Open below
//   0x8004CE1C Open(one, path)            the sync open (batch 4b): Close, the read manager's getter, the path copy and
//                                        RwStreamOpen (0x80045014, game code), the chunk header (0x8047E99C) and the
//                                        body (RwStreamRead 0x80481D9C -> RwFread 0x8047B430 -> the RtGCNFS read) in
//                                        C++, RwMalloc between the category writes (game code), RwStreamClose (game
//                                        code), the name, the fields, the parse. Its reads go through
//                                        NativeRtGcnFs::read_sync (set_sync_read): read's own code with its yields
//                                        continued at once - only when no other thread of the current thread's
//                                        priority is ready (RunQueueBits), where OSYieldThread would return at once.
//                                        Otherwise, and for an allocator object, Open and the ctor with a path decline.
//   0x8004CF90 ~One(one, flags)          Close, then operator delete 0x803A1334 (game code) when (s16) flags > 0
// Declined (the original runs, word for word): an archive with an allocator object (+0x40 != 0: Close / Adopt / dtor
// free through its vtable; no caller passes one), Open (and the ctor with a path) while a thread of the current
// thread's priority is ready or without read_sync, a parse of an archive that is not open (the original reads through
// 0). What Open does not do: OSYieldThread's bookkeeping at its reads' yields (see read_sync).
// Exactness, the stack included: each owner writes its original's frame (back chain, saved LR, saved registers,
// FindName's uppercase copy) and makes its guest calls (the RenderWare free, operator delete, the game's sscanf for the
// version) from that frame, with the original's return site and non-volatile registers, so their frames are the
// original's too; memset's frame is written in C++. The game copies uninitialised stack bytes into heap records
// (fn_8006BEF0), so skipped frames were visible in the heap (the first version of these owners: 18 bytes on stg0100).
// What differs: the volatile registers at the return.
//   0x8004CC94 OpenAsync(one, path, callback, arg, dst, size)  batch 4c: the request block on its frame (+0x1C never
//                                        written), Close, +0x48 / +0x4C, then the read manager's getter and the
//                                        submit 0x8004506C (game code: the exists check and the queued node)
//   0x8004CC28 OnRead(request)           Adopt, then the requester's callback entered as the original's bctrl does
//                                        (OnRead's frame, LR = 0x8004CC6C) through the dispatcher, so the callback
//                                        may yield; the owner at 0x8004CC6C runs the tail (+0x44 = 0, the epilogue).
//                                        A request without a buffer (the original parses through 0) is declined.
//   0x800450C4 the whole-file sync read (manager, path, &size)  batch 4d (the file layer): RwFopen (game code), RwMalloc
//                                        of its length (game code), RwFread in C++ through read_sync (declined, as
//                                        Open, while a thread of the current priority is ready), the size, RwFclose
//                                        (game code); -> the buffer.
//   TOneFileAsync<T>::update (task, arg) batch 4e, the six loaders (RpDMorphAnimation 0x8004AE60, RpWorld 0x8004B3A0,
//                                        RtDict 0x8004B7BC, RpClump 0x8004BBD4, RwTexDictionary 0x8004BFF0, raw
//                                        0x8004C408): one template - state 0 the allocation (retry) and the extract,
//                                        1 the wait, 2 the memory stream, the chunk and the RenderWare reader (game
//                                        code until Phase 5), 3 the wait flag, 4 the end and the requester's callback
//                                        (entered as OnRead's is; an owner at the word after its bctrl runs the tail).
// Still game code: the requesters' callbacks, Extract
// (owned by native_expand_jobs in its modes 3-5).

#include <functional>
#include <optional>

#include "gekko/types.h"

namespace gekko {

class Cpu;

class NativeOne final {
public:
    explicit NativeOne(Cpu& cpu) noexcept : cpu_(cpu) {}
    // Installs the owners when GEKKO_NATIVE_ONE=1. Returns whether it is enabled.
    bool install();
    bool enabled() const noexcept { return enabled_; }
    // Open's reads (NativeRtGcnFs::read_sync): (file, destination, count) entered as the file system's read slot.
    using SyncRead = std::function<std::optional<u32>(Cpu&, u32 file, u32 destination, u32 count)>;
    void set_sync_read(SyncRead read) { sync_read_ = std::move(read); }

    struct Stats {
        u64 entry = 0, is_open = 0, find_name = 0, find_substr = 0, parse = 0, adopt = 0, close = 0, ctor = 0, dtor = 0;
        u64 open = 0, open_bytes = 0;   // sync opens served, and the bytes they read
        u64 open_thread_ready = 0;      // ... declined: a thread of the current priority was ready
        u64 open_async = 0, on_read = 0, on_read_tail = 0;  // OpenAsync; OnRead; its tail after the callback
        u64 whole_reads = 0, whole_bytes = 0, whole_thread_ready = 0;  // 0x800450C4; its bytes; declined (thread ready)
        u64 loader_updates = 0, loader_callbacks = 0, loader_tails = 0;  // TOneFileAsync<T>::update (batch 4e)
        u64 swap16 = 0, swap32 = 0, swap_format = 0;  // the byte-swap leaves 0x80043D0C / 0x80043D34 / 0x80043C80
        u64 declined = 0;  // calls handed to the original words
    };
    const Stats& stats() const noexcept { return stats_; }

private:
    Cpu& cpu_;
    bool enabled_ = false;
    Stats stats_;
    SyncRead sync_read_;
};

}  // namespace gekko
