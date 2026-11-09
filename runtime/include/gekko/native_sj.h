#pragma once

// CRI's stream joints (SJ) as native C++20: the memory joint SJMEM, the ring-buffer joint SJRBF, SJ_SplitChunk and
// the SJCRS init / finish counters. docs/CRI_SERVERS.md section 4b; reconstruction step 2a (the rna output pulls PCM
// through SJRBF joints, step 2).
//
// The original: 38 functions 0x803C9E7C..0x803CBF98 (identified by their own error codes E2004090201..52 and the
// interface order of CRI's SJ vtable: Destroy +0x0C, GetUuid +0x10, Reset +0x14, GetChunk +0x18, UngetChunk +0x1C,
// PutChunk +0x20, GetNumData +0x24, IsGetChunk +0x28, EntryErrFunc +0x2C). A joint has two lanes: 0 = free space,
// 1 = data; a chunk is {data pointer, byte count} in guest memory.
//   SJMEM (vtable 0x80569D40; pool 0x805BBD8C, 32 x 0x24, use count 0x805BBD88): a fixed buffer of data. Fields
//     +4 used, +8 uuid, +0xC data left, +0x10 read offset, +0x14 buffer, +0x18 size, +0x1C / +0x20 error callback.
//     Lane 0 is always empty; GetChunk(1) hands out from +0x10, UngetChunk(1) steps back (an unget that does not end
//     where the read offset lands is reported), PutChunk accepts both lanes and does nothing.
//   SJRBF (vtable 0x80569D70; pool 0x805BC214, 256 x 0x40, use count 0x805BC210): a ring of +0x20 bytes at +0x1C with
//     +0x24 extra bytes after its end that mirror the ring's head, so a chunk may run past the end contiguously. Fields
//     +0xC data count, +0x10 free count, +0x14 free position, +0x18 data position, +0x28 / +0x2C free got / put,
//     +0x30 / +0x34 data got / put, +0x38 / +0x3C error callback. PutChunk(1) copies the part written into the mirror
//     back to the ring's start and the part written at the ring's start into the mirror.
//   SJ_SplitChunk (0x803CBF34): {first n bytes, the rest} of a chunk.
//
// The joints stay in their guest pools: a handle is a guest address whose vtable word the translated users (ADX,
// Sofdec, the stream controller) read and call through, until those users are native. No state is kept here, so
// savestates need nothing new.
//
// Lock: every original function runs under SJCRS_Lock (0x803C9E30: OSDisableInterrupts with a nesting count). A
// native function cannot be interrupted by guest code and the lock / unlock pair leaves no trace, so it is not taken -
// except around a guest error callback, which the original runs inside the lock: there the guest SJCRS_Lock /
// SJCRS_Unlock are called around it. SJCRS_Lock / Unlock themselves stay translated: ADX's own wrappers
// (0x803C90D4 / 0x803C90F4, 0x803CEB80 / 0x803CEBA0) call them too.
//
// Error callbacks: a joint's default callback is its module's error function (0x803CAAA4 / 0x803CBC44), which reports
// "SJMEM Error" / "SJRBF Error" through the SVM's error report - native here. Any other callback (EntryErrFunc) is
// guest code: called through the continuation entry kReturn with this module's frame on the guest stack.

#include <cstddef>
#include <string>

#include "gekko/types.h"

namespace gekko {

class Cpu;
class NativeSvm;

class NativeSj {
public:
    NativeSj(Cpu& cpu, NativeSvm& svm) : cpu_(cpu), svm_(svm) {}
    void install();

    struct Stats {
        u64 creates = 0, destroys = 0, gets = 0, puts = 0, ungets = 0, splits = 0, queries = 0, errors = 0,
            guest_callbacks = 0, bytes_mirrored = 0;
    };
    const Stats& stats() const noexcept { return stats_; }

    struct Address {
        u32 address;
        const char* name;
        bool continuation;
    };
    static const Address* addresses(std::size_t& count) noexcept;

    static constexpr u32 kReturn = 0x803CA9F4u;  // inside SJMEM_Finish's pruned extent
    static constexpr u32 kMemVtable = 0x80569D40u, kRbfVtable = 0x80569D70u;

    // Direct calls for native callers (the rna output, docs/CRI_SERVERS.md 4c): a joint's GetChunk / UngetChunk /
    // PutChunk on a chunk held natively, dispatched on the joint's vtable word as the guest's virtual call is.
    // False when the call cannot complete natively: the joint is neither SJMEM nor SJRBF, or the call reached the
    // joint's error callback and that callback is guest code. Errors with the default callbacks are reported as the
    // original reports them and the call completes.
    struct Chunk {
        u32 data = 0, length = 0;
    };
    bool get_chunk(Cpu& c, u32 sj, u32 lane, u32 bytes, Chunk& ck);
    bool unget_chunk(Cpu& c, u32 sj, u32 lane, const Chunk& ck);
    bool put_chunk(Cpu& c, u32 sj, u32 lane, const Chunk& ck);
    void split_chunk(const Chunk& ck, u32 bytes, Chunk& first, Chunk& rest);

private:
    // The chunk operations. Fault: whether the original reached the joint's error callback at the end of the call,
    // and whether it then took the chunk's length off a transfer counter (SJRBF UngetChunk's mismatch).
    enum class Fault : u32 { None = 0, Callback, CallbackThenSubtract };
    bool valid(Cpu& c, u32 sj, bool ring, u32 null_first, u32 invalid_first);
    Fault mem_get(Cpu& c, u32 sj, u32 lane, u32 bytes, Chunk& ck);
    Fault mem_put(Cpu& c, u32 sj, u32 lane, Chunk& ck);
    Fault mem_unget(Cpu& c, u32 sj, u32 lane, Chunk& ck);
    Fault rbf_get(Cpu& c, u32 sj, u32 lane, u32 bytes, Chunk& ck);
    Fault rbf_put(Cpu& c, u32 sj, u32 lane, Chunk& ck);
    Fault rbf_unget(Cpu& c, u32 sj, u32 lane, Chunk& ck);
    bool direct_fault(Cpu& c, u32 sj, Fault fault, u32 counter, u32 length);

    // A reported error: the original's message (two strings of the module's table, strcpy + strcat) goes to the
    // SVM's error report.
    void report(Cpu& c, u32 table, u32 first, u32 second);
    // The joint's error callback with code -3, then `post`; returns false when a guest call was started (the owner
    // finishes from the continuation).
    enum class Post : u32 { None = 0, SubtractChunk, StoreCount };
    bool callback(Cpu& c, u32 errfn, u32 errobj, Post post, u32 a0, u32 a1, u32 a2, u32 result);
    void apply(Cpu& c, Post post, u32 a0, u32 a1, u32 a2, u32& result);
    void resume(Cpu& c);
    void step(Cpu& c, u32 frame);
    void call(Cpu& c, u32 frame, u32 phase, u32 function, u32 r3, u32 r4);

    // SJRBF's PutChunk(1) copies (memmove within guest memory)
    void move(Cpu& c, u32 to, u32 from, s32 n);

    Cpu& cpu_;
    NativeSvm& svm_;
    Stats stats_;
};

}  // namespace gekko
