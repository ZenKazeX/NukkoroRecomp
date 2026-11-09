// CRI's stream joints (SJMEM, SJRBF, SJ_SplitChunk) as native C++20. Contract and evidence: include/gekko/native_sj.h,
// docs/CRI_SERVERS.md section 4b. Each function below follows its original's order of guest reads and writes.

#include "gekko/native_sj.h"

#include <cstring>
#include <string>

#include "gekko/cpu.h"
#include "gekko/native_svm.h"

namespace gekko {

namespace {

// SJCRS
constexpr u32 kCrsLock = 0x803C9E30u, kCrsUnlock = 0x803C9DE4u;  // translated (ADX calls them too)
constexpr u32 kCrsFinish = 0x803C9E7Cu, kCrsInit = 0x803C9EACu;
constexpr u32 kCrsDepth = 0x805BBD78u, kCrsUsers = 0x805BBD80u;

// SJMEM
constexpr u32 kMemGetBufSize = 0x803C9EDCu, kMemIsGetChunk = 0x803C9F88u, kMemUngetChunk = 0x803CA09Cu,
              kMemPutChunk = 0x803CA230u, kMemGetChunk = 0x803CA334u, kMemGetNumData = 0x803CA468u,
              kMemReset = 0x803CA55Cu, kMemEntryErrFunc = 0x803CA608u, kMemGetUuid = 0x803CA6C4u,
              kMemDestroy = 0x803CA770u, kMemCreate = 0x803CA824u, kMemFinish = 0x803CA9F0u, kMemInit = 0x803CAA44u,
              kMemError = 0x803CAAA4u;
constexpr u32 kMemPool = 0x805BBD8Cu, kMemUsers = 0x805BBD88u, kMemTable = 0x80518238u;
constexpr u32 kMemSize = 0x24u, kMemCount = 32u;

// SJRBF
constexpr u32 kRbfGetCount = 0x803CAACCu, kRbfGetXtrSize = 0x803CABA0u, kRbfGetBufSize = 0x803CAC4Cu,
              kRbfGetBufPtr = 0x803CACF8u, kRbfIsGetChunk = 0x803CADA4u, kRbfUngetBody = 0x803CAF08u,
              kRbfUngetChunk = 0x803CB120u, kRbfPutBody = 0x803CB178u, kRbfPutChunk = 0x803CB320u,
              kRbfGetBody = 0x803CB378u, kRbfGetChunk = 0x803CB580u, kRbfGetNumData = 0x803CB5E8u,
              kRbfReset = 0x803CB6DCu, kRbfEntryErrFunc = 0x803CB7A0u, kRbfGetUuid = 0x803CB85Cu,
              kRbfDestroy = 0x803CB908u, kRbfCreate = 0x803CB9BCu, kRbfFinish = 0x803CBB90u, kRbfInit = 0x803CBBE4u,
              kRbfError = 0x803CBC44u;
constexpr u32 kRbfPool = 0x805BC214u, kRbfUsers = 0x805BC210u, kRbfTable = 0x805183A0u;
constexpr u32 kRbfSize = 0x40u, kRbfCount = 256u;

constexpr u32 kSplitChunk = 0x803CBF34u, kSjError = 0x803CBF98u;

// The two halves of each message in the module's table (the original's strcpy / strcat offsets): null handle,
// invalid handle, illegal buffer size.
struct Messages {
    u32 null_first, null_second, invalid_first, invalid_second;
};
constexpr u32 kMemNull = 0x1Cu, kMemInvalid = 0x48u;   // SJMEM's second halves
constexpr u32 kRbfNull = 0x48u, kRbfInvalid = 0x74u, kRbfBadSize = 0x130u;

// The guest-call frame (0x40 bytes; +4 is the callee's LR save word).
constexpr u32 kFrameSize = 0x40u;
constexpr u32 kTag = 0x4E534A20u;  // 'NSJ '
constexpr u32 kFrameTag = 0x08u, kFramePhase = 0x0Cu, kFramePost = 0x10u, kFrameFn = 0x14u, kFrameObj = 0x18u,
              kFrameLr = 0x1Cu, kFrameArgs = 0x20u, kFrameResult = 0x2Cu;
enum Phase : u32 { kLock = 0, kCallback, kAfter, kUnlock, kDone };

constexpr s32 kCodeBadParameter = -3;

const NativeSj::Address kAddresses[] = {
    {kCrsFinish, "SJCRS finish", false},
    {kCrsInit, "SJCRS init", false},
    {kMemGetBufSize, "SJMEM_GetBufSize", false},
    {kMemIsGetChunk, "SJMEM_IsGetChunk", false},
    {kMemUngetChunk, "SJMEM_UngetChunk", false},
    {kMemPutChunk, "SJMEM_PutChunk", false},
    {kMemGetChunk, "SJMEM_GetChunk", false},
    {kMemGetNumData, "SJMEM_GetNumData", false},
    {kMemReset, "SJMEM_Reset", false},
    {kMemEntryErrFunc, "SJMEM_EntryErrFunc", false},
    {kMemGetUuid, "SJMEM_GetUuid", false},
    {kMemDestroy, "SJMEM_Destroy", false},
    {kMemCreate, "SJMEM_Create", false},
    {kMemFinish, "SJMEM_Finish", false},
    {NativeSj::kReturn, "native SJ: a guest call returned", true},
    {kMemInit, "SJMEM_Init", false},
    {kMemError, "SJMEM_Error", false},
    {kRbfGetCount, "SJRBF transfer counter", false},
    {kRbfGetXtrSize, "SJRBF_GetXtrSize", false},
    {kRbfGetBufSize, "SJRBF_GetBufSize", false},
    {kRbfGetBufPtr, "SJRBF_GetBufPtr", false},
    {kRbfIsGetChunk, "SJRBF_IsGetChunk", false},
    {kRbfUngetBody, "SJRBF_UngetChunk body", false},
    {kRbfUngetChunk, "SJRBF_UngetChunk", false},
    {kRbfPutBody, "SJRBF_PutChunk body", false},
    {kRbfPutChunk, "SJRBF_PutChunk", false},
    {kRbfGetBody, "SJRBF_GetChunk body", false},
    {kRbfGetChunk, "SJRBF_GetChunk", false},
    {kRbfGetNumData, "SJRBF_GetNumData", false},
    {kRbfReset, "SJRBF_Reset", false},
    {kRbfEntryErrFunc, "SJRBF_EntryErrFunc", false},
    {kRbfGetUuid, "SJRBF_GetUuid", false},
    {kRbfDestroy, "SJRBF_Destroy", false},
    {kRbfCreate, "SJRBF_Create", false},
    {kRbfFinish, "SJRBF_Finish", false},
    {kRbfInit, "SJRBF_Init", false},
    {kRbfError, "SJRBF_Error", false},
    {kSplitChunk, "SJ_SplitChunk", false},
    {kSjError, "SJ error report", false},
};

void stop(Cpu& c, const std::string& why) { c.request_stop(StopReason::ExecutionFault, "native SJ: " + why); }

s32 sw(u32 v) { return static_cast<s32>(v); }
s32 smin(s32 a, s32 b) { return b < a ? b : a; }
// divw / mullw / subf: the remainder of a signed division truncated toward zero (the original's modulo)
u32 remainder(u32 a, u32 b) {
    const s64 q = static_cast<s64>(sw(a)) / static_cast<s64>(sw(b));
    return a - static_cast<u32>(q) * b;
}

std::string cstring(Cpu& c, u32 at) {
    std::string text;
    for (u32 i = 0; i < 0x100u; ++i) {
        const u8 byte = c.mem_read8(at + i);
        if (byte == 0u) break;
        text.push_back(static_cast<char>(byte));
    }
    return text;
}

void fill_zero(Cpu& c, u32 at, u32 n) {
    if (u8* p = c.guest_memory().span(GcAddr(at), n)) {
        std::memset(p, 0, n);
        return;
    }
    for (u32 i = 0; i < n; ++i) c.mem_write8(at + i, 0u);
}

// SJCRS_Init / SJCRS_Finish: a user count; the first user and the last clear the lock depth.
void crs_init(Cpu& c) {
    const u32 users = c.mem_read32(kCrsUsers) + 1u;
    c.mem_write32(kCrsUsers, users);
    if (users == 1u) c.mem_write32(kCrsDepth, 0u);
}
void crs_finish(Cpu& c) {
    const u32 users = c.mem_read32(kCrsUsers) - 1u;
    c.mem_write32(kCrsUsers, users);
    if (users == 0u) c.mem_write32(kCrsDepth, 0u);
}

}  // namespace

const NativeSj::Address* NativeSj::addresses(std::size_t& count) noexcept {
    count = std::size(kAddresses);
    return kAddresses;
}

void NativeSj::report(Cpu& c, u32 table, u32 first, u32 second) {
    ++stats_.errors;
    svm_.error_report(cstring(c, table + first) + cstring(c, table + second));
}

void NativeSj::move(Cpu& c, u32 to, u32 from, s32 n) {
    if (n <= 0) return;
    const u32 k = static_cast<u32>(n);
    stats_.bytes_mirrored += k;
    u8* const dst = c.guest_memory().span(GcAddr(to), k);
    u8* const src = c.guest_memory().span(GcAddr(from), k);
    if (dst != nullptr && src != nullptr) {
        std::memmove(dst, src, k);
        return;
    }
    // the C library's byte-wise memmove: forward unless the source lies below the destination
    if (from < to)
        for (u32 i = k; i-- > 0u;) c.mem_write8(to + i, c.mem_read8(from + i));
    else
        for (u32 i = 0; i < k; ++i) c.mem_write8(to + i, c.mem_read8(from + i));
}

// ---- the chunk operations (owners and direct callers) ---------------------------------------------------------------

// A handle check: null or unused handles are reported; the call then does nothing more.
bool NativeSj::valid(Cpu& c, u32 sj, bool ring, u32 null_first, u32 invalid_first) {
    const u32 table = ring ? kRbfTable : kMemTable;
    if (sj == 0u) {
        report(c, table, null_first, ring ? kRbfNull : kMemNull);
        return false;
    }
    if (c.mem_read32(sj + 4u) == 0u) {
        report(c, table, invalid_first, ring ? kRbfInvalid : kMemInvalid);
        return false;
    }
    return true;
}

NativeSj::Fault NativeSj::mem_get(Cpu& c, u32 sj, u32 lane, u32 bytes, Chunk& ck) {
    ++stats_.gets;
    if (!valid(c, sj, false, 0xC8u, 0xD4u)) return Fault::None;
    if (lane == 1u) {
        ck.length = static_cast<u32>(smin(sw(c.mem_read32(sj + 0x0Cu)), sw(bytes)));
        ck.data = c.mem_read32(sj + 0x14u) + c.mem_read32(sj + 0x10u);
        c.mem_write32(sj + 0x10u, c.mem_read32(sj + 0x10u) + ck.length);
        c.mem_write32(sj + 0x0Cu, c.mem_read32(sj + 0x0Cu) - ck.length);
        return Fault::None;
    }
    ck = {};
    return lane == 0u ? Fault::None : Fault::Callback;
}

NativeSj::Fault NativeSj::mem_put(Cpu& c, u32 sj, u32 lane, Chunk& ck) {
    ++stats_.puts;
    if (!valid(c, sj, false, 0xB0u, 0xBCu)) return Fault::None;
    if (sw(ck.length) <= 0 || ck.data == 0u) return Fault::None;
    if (lane == 0u || lane == 1u) return Fault::None;  // a memory joint takes nothing back
    ck = {};
    return Fault::Callback;
}

NativeSj::Fault NativeSj::mem_unget(Cpu& c, u32 sj, u32 lane, Chunk& ck) {
    ++stats_.ungets;
    if (!valid(c, sj, false, 0x98u, 0xA4u)) return Fault::None;
    if (sw(ck.length) <= 0 || ck.data == 0u) return Fault::None;
    if (lane == 0u) return Fault::Callback;
    if (lane != 1u) {
        ck = {};
        return Fault::Callback;
    }
    const s32 back = sw(c.mem_read32(sj + 0x10u)) - sw(ck.length);
    const u32 offset = back > 0 ? static_cast<u32>(back) : 0u;
    c.mem_write32(sj + 0x10u, offset);
    const s32 data = sw(c.mem_read32(sj + 0x0Cu) + ck.length);
    c.mem_write32(sj + 0x0Cu, static_cast<u32>(smin(data, sw(c.mem_read32(sj + 0x18u)))));
    return offset != ck.data - c.mem_read32(sj + 0x14u) ? Fault::Callback : Fault::None;
}

namespace {
// available bytes of a ring lane: its count, bounded by the run to the ring's end plus the mirror
s32 run_of(Cpu& c, u32 sj, u32 count, u32 position) {
    const s32 run = sw(c.mem_read32(sj + 0x24u) + (c.mem_read32(sj + 0x20u) - c.mem_read32(sj + position)));
    return smin(sw(c.mem_read32(sj + count)), run);
}
}  // namespace

NativeSj::Fault NativeSj::rbf_get(Cpu& c, u32 sj, u32 lane, u32 bytes, Chunk& ck) {
    ++stats_.gets;
    if (!valid(c, sj, true, 0x160u, 0x16Cu)) return Fault::None;
    if (c.mem_read32(sj + 0x20u) == 0u) {
        report(c, kRbfTable, 0x178u, kRbfBadSize);
        return Fault::None;
    }
    if (lane != 0u && lane != 1u) {
        ck = {};
        return Fault::Callback;
    }
    const u32 count = lane == 0u ? 0x10u : 0x0Cu, position = lane == 0u ? 0x14u : 0x18u,
              got = lane == 0u ? 0x28u : 0x30u;
    ck.length = static_cast<u32>(smin(run_of(c, sj, count, position), sw(bytes)));
    ck.data = c.mem_read32(sj + 0x1Cu) + c.mem_read32(sj + position);
    c.mem_write32(sj + position, remainder(c.mem_read32(sj + position) + ck.length, c.mem_read32(sj + 0x20u)));
    c.mem_write32(sj + count, c.mem_read32(sj + count) - ck.length);
    c.mem_write32(sj + got, c.mem_read32(sj + got) + ck.length);
    return Fault::None;
}

NativeSj::Fault NativeSj::rbf_put(Cpu& c, u32 sj, u32 lane, Chunk& ck) {
    ++stats_.puts;
    if (!valid(c, sj, true, 0x148u, 0x154u)) return Fault::None;
    const s32 length = sw(ck.length);
    if (length <= 0 || ck.data == 0u) return Fault::None;
    if (lane == 1u) {
        // the part written at the ring's start goes to the mirror after its end
        const u32 base = c.mem_read32(sj + 0x1Cu);
        const s32 offset = sw(ck.data - base), extra = sw(c.mem_read32(sj + 0x24u));
        if (offset < extra)
            move(c, c.mem_read32(sj + 0x20u) + static_cast<u32>(offset) + base, ck.data, smin(length, extra - offset));
        // the part written into the mirror goes to the ring's start
        const s32 end = length + sw(ck.data - base), size = sw(c.mem_read32(sj + 0x20u));
        if (end > size) {
            const s32 over = smin(length, end - size);
            move(c, base, base + static_cast<u32>(end - over), over);
        }
        c.mem_write32(sj + 0x0Cu, c.mem_read32(sj + 0x0Cu) + ck.length);
        c.mem_write32(sj + 0x34u, c.mem_read32(sj + 0x34u) + ck.length);
        return Fault::None;
    }
    if (lane == 0u) {
        c.mem_write32(sj + 0x10u, c.mem_read32(sj + 0x10u) + ck.length);
        c.mem_write32(sj + 0x2Cu, c.mem_read32(sj + 0x2Cu) + ck.length);
        return Fault::None;
    }
    ck = {};
    return Fault::Callback;
}

NativeSj::Fault NativeSj::rbf_unget(Cpu& c, u32 sj, u32 lane, Chunk& ck) {
    ++stats_.ungets;
    if (!valid(c, sj, true, 0x10Cu, 0x118u)) return Fault::None;
    const u32 size = c.mem_read32(sj + 0x20u);
    if (size == 0u) {
        report(c, kRbfTable, 0x124u, kRbfBadSize);
        return Fault::None;
    }
    if (sw(ck.length) <= 0 || ck.data == 0u) return Fault::None;
    if (lane != 0u && lane != 1u) {
        ck = {};
        return Fault::Callback;
    }
    const u32 count = lane == 0u ? 0x10u : 0x0Cu, position = lane == 0u ? 0x14u : 0x18u,
              got = lane == 0u ? 0x28u : 0x30u;
    // stepping the lane's position back by the chunk must land on the chunk's start
    const u32 back = remainder(c.mem_read32(sj + position) + size - ck.length, size);
    const u32 at = remainder(ck.data - c.mem_read32(sj + 0x1Cu), size);
    if (back != at) return Fault::CallbackThenSubtract;  // the original then takes the length off `got` too
    c.mem_write32(sj + position, back);
    c.mem_write32(sj + count, c.mem_read32(sj + count) + ck.length);
    c.mem_write32(sj + got, c.mem_read32(sj + got) - ck.length);
    return Fault::None;
}

void NativeSj::split_chunk(const Chunk& ck, u32 bytes, Chunk& first, Chunk& rest) {
    ++stats_.splits;
    const Chunk whole = ck;  // the callers split a chunk into itself
    first = whole;
    rest.length = first.length;
    if (sw(first.length) > sw(bytes)) first.length = bytes;
    rest.length -= first.length;
    rest.data = rest.length == 0u ? 0u : first.data + first.length;
}

// A fault reached by a direct call: the default callbacks report as the original does; a guest callback cannot run
// from native code.
bool NativeSj::direct_fault(Cpu& c, u32 sj, Fault fault, u32 counter, u32 length) {
    if (fault == Fault::None) return true;
    const bool ring = c.mem_read32(sj) == kRbfVtable;
    const u32 errfn = c.mem_read32(sj + (ring ? 0x38u : 0x1Cu));
    if (errfn != 0u && errfn != kMemError && errfn != kRbfError) return false;
    if (errfn != 0u) {
        ++stats_.errors;
        svm_.error_report(errfn == kMemError ? "SJMEM Error" : "SJRBF Error");
    }
    if (fault == Fault::CallbackThenSubtract) c.mem_write32(sj + counter, c.mem_read32(sj + counter) - length);
    return true;
}

bool NativeSj::get_chunk(Cpu& c, u32 sj, u32 lane, u32 bytes, Chunk& ck) {
    const u32 vtable = c.mem_read32(sj);
    if (vtable == kRbfVtable) return direct_fault(c, sj, rbf_get(c, sj, lane, bytes, ck), 0u, 0u);
    if (vtable == kMemVtable) return direct_fault(c, sj, mem_get(c, sj, lane, bytes, ck), 0u, 0u);
    return false;
}

bool NativeSj::unget_chunk(Cpu& c, u32 sj, u32 lane, const Chunk& ck) {
    Chunk copy = ck;
    const u32 vtable = c.mem_read32(sj);
    if (vtable == kRbfVtable)
        return direct_fault(c, sj, rbf_unget(c, sj, lane, copy), lane == 0u ? 0x28u : 0x30u, ck.length);
    if (vtable == kMemVtable) return direct_fault(c, sj, mem_unget(c, sj, lane, copy), 0u, 0u);
    return false;
}

bool NativeSj::put_chunk(Cpu& c, u32 sj, u32 lane, const Chunk& ck) {
    Chunk copy = ck;
    const u32 vtable = c.mem_read32(sj);
    if (vtable == kRbfVtable) return direct_fault(c, sj, rbf_put(c, sj, lane, copy), 0u, 0u);
    if (vtable == kMemVtable) return direct_fault(c, sj, mem_put(c, sj, lane, copy), 0u, 0u);
    return false;
}

void NativeSj::apply(Cpu& c, Post post, u32 a0, u32 a1, u32 a2, u32& result) {
    switch (post) {
        case Post::None:
            break;
        case Post::SubtractChunk:  // a0 = the counter's address, a1 = the chunk
            c.mem_write32(a0, c.mem_read32(a0) - c.mem_read32(a1 + 4u));
            break;
        case Post::StoreCount:  // a0 = the count's destination, a1 = the count, a2 = the bytes asked for
            c.mem_write32(a0, a1);
            result = a1 == a2 ? 1u : 0u;
            break;
    }
}

bool NativeSj::callback(Cpu& c, u32 errfn, u32 errobj, Post post, u32 a0, u32 a1, u32 a2, u32 result) {
    if (errfn == kMemError || errfn == kRbfError || errfn == 0u) {
        if (errfn != 0u) {
            ++stats_.errors;
            svm_.error_report(errfn == kMemError ? "SJMEM Error" : "SJRBF Error");
        }
        apply(c, post, a0, a1, a2, result);
        c.gpr[3] = result;
        return true;
    }
    // A guest callback, inside the guest SJCRS lock as in the original.
    const u32 caller_sp = c.gpr[1];
    const u32 frame = caller_sp - kFrameSize;
    if ((caller_sp & 7u) != 0u || !c.guest_memory().mapped(GcAddr(frame), kFrameSize)) {
        stop(c, "the caller's stack cannot hold a frame (r1 " + std::to_string(caller_sp) + ")");
        return false;
    }
    ++stats_.guest_callbacks;
    c.mem_write32(frame, caller_sp);
    c.mem_write32(frame + kFrameTag, kTag);
    c.mem_write32(frame + kFramePost, static_cast<u32>(post));
    c.mem_write32(frame + kFrameFn, errfn);
    c.mem_write32(frame + kFrameObj, errobj);
    c.mem_write32(frame + kFrameLr, c.lr);
    c.mem_write32(frame + kFrameArgs + 0u, a0);
    c.mem_write32(frame + kFrameArgs + 4u, a1);
    c.mem_write32(frame + kFrameArgs + 8u, a2);
    c.mem_write32(frame + kFrameResult, result);
    c.mem_write32(frame + kFramePhase, kLock);
    c.gpr[1] = frame;
    step(c, frame);
    return false;
}

void NativeSj::call(Cpu& c, u32 frame, u32 phase, u32 function, u32 r3, u32 r4) {
    c.mem_write32(frame + kFramePhase, phase);
    c.gpr[1] = frame;
    c.gpr[3] = r3;
    c.gpr[4] = r4;
    c.gpr[12] = function;
    c.ctr = function;
    c.lr = kReturn;
    c.leave_to_dispatcher(function, FallbackReason::Yield, kReturn);
}

void NativeSj::resume(Cpu& c) {
    const u32 frame = c.gpr[1];
    if ((frame & 3u) != 0u || !c.guest_memory().mapped(GcAddr(frame), kFrameSize) ||
        c.mem_read32(frame + kFrameTag) != kTag) {
        stop(c, "a guest call returned without its frame (r1 " + std::to_string(frame) + ")");
        return;
    }
    step(c, frame);
}

void NativeSj::step(Cpu& c, u32 frame) {
    switch (c.mem_read32(frame + kFramePhase)) {
        case kLock:
            call(c, frame, kCallback, kCrsLock, 0u, 0u);
            return;
        case kCallback:
            call(c, frame, kAfter, c.mem_read32(frame + kFrameFn), c.mem_read32(frame + kFrameObj),
                 static_cast<u32>(kCodeBadParameter));
            return;
        case kAfter: {
            u32 result = c.mem_read32(frame + kFrameResult);
            apply(c, static_cast<Post>(c.mem_read32(frame + kFramePost)), c.mem_read32(frame + kFrameArgs),
                  c.mem_read32(frame + kFrameArgs + 4u), c.mem_read32(frame + kFrameArgs + 8u), result);
            c.mem_write32(frame + kFrameResult, result);
            call(c, frame, kDone, kCrsUnlock, 0u, 0u);
            return;
        }
        case kDone:
            c.mem_write32(frame + kFrameTag, 0u);
            c.gpr[3] = c.mem_read32(frame + kFrameResult);
            c.lr = c.mem_read32(frame + kFrameLr);
            c.gpr[1] = c.mem_read32(frame);
            return;
        default:
            stop(c, "a frame in an unknown phase");
            return;
    }
}

void NativeSj::install() {
    Cpu& cpu = cpu_;
    const auto own = [&cpu](u32 address, const char* name, auto&& serve) {
        cpu.install_hle(
            address,
            [serve](Cpu& c) {
                serve(c);
                return true;
            },
            std::string("native SJ: ") + name);
    };
    const auto mem_ok = [this](Cpu& c, u32 sj, u32 null_first, u32 invalid_first) {
        return valid(c, sj, false, null_first, invalid_first);
    };
    const auto rbf_ok = [this](Cpu& c, u32 sj, u32 null_first, u32 invalid_first) {
        return valid(c, sj, true, null_first, invalid_first);
    };
    // the field getters: (handle) -> word at offset, 0 for a bad handle
    const auto getter = [&own](u32 address, const char* name, auto ok, u32 null_first, u32 invalid_first,
                               u32 offset) {
        own(address, name, [ok, null_first, invalid_first, offset](Cpu& c) {
            const u32 sj = c.gpr[3];
            c.gpr[3] = ok(c, sj, null_first, invalid_first) ? c.mem_read32(sj + offset) : 0u;
        });
    };
    const auto pool_create = [](Cpu& c, u32 pool, u32 size, u32 count) -> u32 {
        for (u32 i = 0; i < count; ++i)
            if (c.mem_read32(pool + i * size + 4u) == 0u) return pool + i * size;
        return 0u;
    };
    // A chunk operation called by guest code: the chunk at a guest address, read before and written back after
    // (the original writes it in place; a chunk never overlaps its joint), then the error callback if reached - for
    // SJRBF UngetChunk's mismatch followed by the length taken off the lane's transfer counter.
    const auto chunk_owner = [this](Cpu& c, u32 sj, u32 ck, bool ring, Fault fault, const Chunk& after) {
        c.mem_write32(ck, after.data);
        c.mem_write32(ck + 4u, after.length);
        if (fault == Fault::None) return;
        const u32 errfn = c.mem_read32(sj + (ring ? 0x38u : 0x1Cu));
        const u32 errobj = c.mem_read32(sj + (ring ? 0x3Cu : 0x20u));
        if (fault == Fault::Callback) {
            callback(c, errfn, errobj, Post::None, 0u, 0u, 0u, c.gpr[3]);
        } else {
            const u32 counter = c.gpr[4] == 0u ? 0x28u : 0x30u;  // only lanes 0 / 1 reach the mismatch
            callback(c, errfn, errobj, Post::SubtractChunk, sj + counter, ck, 0u, c.gpr[3]);
        }
    };
    const auto read_chunk = [](Cpu& c, u32 ck) { return Chunk{c.mem_read32(ck), c.mem_read32(ck + 4u)}; };

    // ---- SJCRS user count ----------------------------------------------------------------------------------
    own(kCrsInit, "SJCRS init", [](Cpu& c) { crs_init(c); });
    own(kCrsFinish, "SJCRS finish", [](Cpu& c) { crs_finish(c); });

    // ---- SJMEM ---------------------------------------------------------------------------------------------
    own(kMemInit, "SJMEM_Init", [](Cpu& c) {
        crs_init(c);
        if (c.mem_read32(kMemUsers) == 0u) fill_zero(c, kMemPool, kMemSize * kMemCount);
        c.mem_write32(kMemUsers, c.mem_read32(kMemUsers) + 1u);
    });
    own(kMemFinish, "SJMEM_Finish", [](Cpu& c) {
        const u32 users = c.mem_read32(kMemUsers) - 1u;
        c.mem_write32(kMemUsers, users);
        if (users == 0u) fill_zero(c, kMemPool, kMemSize * kMemCount);
        crs_finish(c);
    });
    own(kMemError, "SJMEM_Error", [this](Cpu&) {
        ++stats_.errors;
        svm_.error_report("SJMEM Error");
    });
    own(kMemCreate, "SJMEM_Create", [this, pool_create](Cpu& c) {
        const u32 buffer = c.gpr[3], size = c.gpr[4];
        const u32 sj = pool_create(c, kMemPool, kMemSize, kMemCount);
        if (sj != 0u) {
            ++stats_.creates;
            c.mem_write32(sj + 4u, 1u);
            c.mem_write32(sj + 0u, kMemVtable);
            c.mem_write32(sj + 0x14u, buffer);
            c.mem_write32(sj + 0x18u, size);
            c.mem_write32(sj + 0x08u, kMemTable);  // its UUID heads the module's table
            c.mem_write32(sj + 0x1Cu, kMemError);
            c.mem_write32(sj + 0x20u, sj);
            c.mem_write32(sj + 0x0Cu, c.mem_read32(sj + 0x18u));  // Reset
            c.mem_write32(sj + 0x10u, 0u);
        }
        c.gpr[3] = sj;
    });
    own(kMemDestroy, "SJMEM_Destroy", [this, mem_ok](Cpu& c) {
        const u32 sj = c.gpr[3];
        if (!mem_ok(c, sj, 0x140u, 0x14Cu)) return;
        ++stats_.destroys;
        fill_zero(c, sj, kMemSize);
    });
    getter(kMemGetUuid, "SJMEM_GetUuid", mem_ok, 0x128u, 0x134u, 0x08u);
    getter(kMemGetBufSize, "SJMEM_GetBufSize", mem_ok, 0x10u, 0x3Cu, 0x18u);
    own(kMemReset, "SJMEM_Reset", [mem_ok](Cpu& c) {
        const u32 sj = c.gpr[3];
        if (!mem_ok(c, sj, 0xF8u, 0x104u)) return;
        c.mem_write32(sj + 0x0Cu, c.mem_read32(sj + 0x18u));
        c.mem_write32(sj + 0x10u, 0u);
    });
    own(kMemEntryErrFunc, "SJMEM_EntryErrFunc", [mem_ok](Cpu& c) {
        const u32 sj = c.gpr[3];
        if (!mem_ok(c, sj, 0x110u, 0x11Cu)) return;
        c.mem_write32(sj + 0x1Cu, c.gpr[4]);
        c.mem_write32(sj + 0x20u, c.gpr[5]);
    });
    own(kMemGetNumData, "SJMEM_GetNumData", [this, mem_ok](Cpu& c) {
        const u32 sj = c.gpr[3], lane = c.gpr[4];
        ++stats_.queries;
        if (!mem_ok(c, sj, 0xE0u, 0xECu)) {
            c.gpr[3] = 0u;
            return;
        }
        if (lane == 1u) {
            c.gpr[3] = c.mem_read32(sj + 0x0Cu);
        } else if (lane == 0u) {
            c.gpr[3] = 0u;
        } else {
            callback(c, c.mem_read32(sj + 0x1Cu), c.mem_read32(sj + 0x20u), Post::None, 0u, 0u, 0u, 0u);
        }
    });
    own(kMemIsGetChunk, "SJMEM_IsGetChunk", [this, mem_ok](Cpu& c) {
        const u32 sj = c.gpr[3], lane = c.gpr[4], n = c.gpr[5], rbyte = c.gpr[6];
        ++stats_.queries;
        if (!mem_ok(c, sj, 0x80u, 0x8Cu)) {
            c.gpr[3] = 0u;
            return;
        }
        u32 r = 0u;
        if (lane == 1u) {
            r = static_cast<u32>(smin(sw(c.mem_read32(sj + 0x0Cu)), sw(n)));
        } else if (lane != 0u) {
            callback(c, c.mem_read32(sj + 0x1Cu), c.mem_read32(sj + 0x20u), Post::StoreCount, rbyte, 0u, n, 0u);
            return;
        }
        u32 result = 0u;
        apply(c, Post::StoreCount, rbyte, r, n, result);
        c.gpr[3] = result;
    });
    own(kMemGetChunk, "SJMEM_GetChunk", [this, chunk_owner, read_chunk](Cpu& c) {
        const u32 sj = c.gpr[3], ck = c.gpr[6];
        Chunk chunk = read_chunk(c, ck);
        const Fault fault = mem_get(c, sj, c.gpr[4], c.gpr[5], chunk);
        chunk_owner(c, sj, ck, false, fault, chunk);
    });
    own(kMemPutChunk, "SJMEM_PutChunk", [this, chunk_owner, read_chunk](Cpu& c) {
        const u32 sj = c.gpr[3], ck = c.gpr[5];
        Chunk chunk = read_chunk(c, ck);
        const Fault fault = mem_put(c, sj, c.gpr[4], chunk);
        chunk_owner(c, sj, ck, false, fault, chunk);
    });
    own(kMemUngetChunk, "SJMEM_UngetChunk", [this, chunk_owner, read_chunk](Cpu& c) {
        const u32 sj = c.gpr[3], ck = c.gpr[5];
        Chunk chunk = read_chunk(c, ck);
        const Fault fault = mem_unget(c, sj, c.gpr[4], chunk);
        chunk_owner(c, sj, ck, false, fault, chunk);
    });

    // ---- SJRBF ---------------------------------------------------------------------------------------------
    own(kRbfInit, "SJRBF_Init", [](Cpu& c) {
        crs_init(c);
        if (c.mem_read32(kRbfUsers) == 0u) fill_zero(c, kRbfPool, kRbfSize * kRbfCount);
        c.mem_write32(kRbfUsers, c.mem_read32(kRbfUsers) + 1u);
    });
    own(kRbfFinish, "SJRBF_Finish", [](Cpu& c) {
        const u32 users = c.mem_read32(kRbfUsers) - 1u;
        c.mem_write32(kRbfUsers, users);
        if (users == 0u) fill_zero(c, kRbfPool, kRbfSize * kRbfCount);
        crs_finish(c);
    });
    own(kRbfError, "SJRBF_Error", [this](Cpu&) {
        ++stats_.errors;
        svm_.error_report("SJRBF Error");
    });
    const auto rbf_reset = [](Cpu& c, u32 sj) {
        c.mem_write32(sj + 0x0Cu, 0u);
        c.mem_write32(sj + 0x10u, c.mem_read32(sj + 0x20u));
        for (const u32 field : {0x14u, 0x18u, 0x28u, 0x2Cu, 0x30u, 0x34u}) c.mem_write32(sj + field, 0u);
    };
    own(kRbfCreate, "SJRBF_Create", [this, pool_create, rbf_reset](Cpu& c) {
        const u32 buffer = c.gpr[3], size = c.gpr[4], extra = c.gpr[5];
        const u32 sj = pool_create(c, kRbfPool, kRbfSize, kRbfCount);
        if (sj != 0u) {
            ++stats_.creates;
            c.mem_write32(sj + 4u, 1u);
            c.mem_write32(sj + 0u, kRbfVtable);
            c.mem_write32(sj + 0x1Cu, buffer);
            c.mem_write32(sj + 0x20u, size);
            c.mem_write32(sj + 0x24u, extra);
            c.mem_write32(sj + 0x08u, kRbfTable + 0x2Cu);  // its UUID in the module's table
            c.mem_write32(sj + 0x38u, kRbfError);
            c.mem_write32(sj + 0x3Cu, sj);
            rbf_reset(c, sj);
        }
        c.gpr[3] = sj;
    });
    own(kRbfDestroy, "SJRBF_Destroy", [this, rbf_ok](Cpu& c) {
        const u32 sj = c.gpr[3];
        if (!rbf_ok(c, sj, 0x1E4u, 0x1F0u)) return;
        ++stats_.destroys;
        fill_zero(c, sj, kRbfSize);
    });
    own(kRbfReset, "SJRBF_Reset", [rbf_ok, rbf_reset](Cpu& c) {
        const u32 sj = c.gpr[3];
        if (rbf_ok(c, sj, 0x19Cu, 0x1A8u)) rbf_reset(c, sj);
    });
    own(kRbfEntryErrFunc, "SJRBF_EntryErrFunc", [rbf_ok](Cpu& c) {
        const u32 sj = c.gpr[3];
        if (!rbf_ok(c, sj, 0x1B4u, 0x1C0u)) return;
        c.mem_write32(sj + 0x38u, c.gpr[4]);
        c.mem_write32(sj + 0x3Cu, c.gpr[5]);
    });
    getter(kRbfGetUuid, "SJRBF_GetUuid", rbf_ok, 0x1CCu, 0x1D8u, 0x08u);
    getter(kRbfGetBufPtr, "SJRBF_GetBufPtr", rbf_ok, 0xDCu, 0xE8u, 0x1Cu);
    getter(kRbfGetBufSize, "SJRBF_GetBufSize", rbf_ok, 0xC4u, 0xD0u, 0x20u);
    getter(kRbfGetXtrSize, "SJRBF_GetXtrSize", rbf_ok, 0xACu, 0xB8u, 0x24u);
    own(kRbfGetCount, "SJRBF transfer counter", [rbf_ok](Cpu& c) {
        // (handle, lane, 0 = got / 1 = put): +0x28 + lane * 8 + kind * 4, unchecked as in the original
        const u32 sj = c.gpr[3];
        c.gpr[3] = rbf_ok(c, sj, 0x3Cu, 0x68u) ? c.mem_read32(sj + (c.gpr[4] << 3) + (c.gpr[5] << 2) + 0x28u) : 0u;
    });
    own(kRbfGetNumData, "SJRBF_GetNumData", [this, rbf_ok](Cpu& c) {
        const u32 sj = c.gpr[3], lane = c.gpr[4];
        ++stats_.queries;
        if (!rbf_ok(c, sj, 0x184u, 0x190u)) {
            c.gpr[3] = 0u;
            return;
        }
        if (lane == 1u) {
            c.gpr[3] = c.mem_read32(sj + 0x0Cu);
        } else if (lane == 0u) {
            c.gpr[3] = c.mem_read32(sj + 0x10u);
        } else {
            callback(c, c.mem_read32(sj + 0x38u), c.mem_read32(sj + 0x3Cu), Post::None, 0u, 0u, 0u, 0u);
        }
    });
    own(kRbfIsGetChunk, "SJRBF_IsGetChunk", [this, rbf_ok](Cpu& c) {
        const u32 sj = c.gpr[3], lane = c.gpr[4], n = c.gpr[5], rbyte = c.gpr[6];
        ++stats_.queries;
        if (!rbf_ok(c, sj, 0xF4u, 0x100u)) {
            c.gpr[3] = 0u;
            return;
        }
        if (lane != 0u && lane != 1u) {
            callback(c, c.mem_read32(sj + 0x38u), c.mem_read32(sj + 0x3Cu), Post::StoreCount, rbyte, 0u, n, 0u);
            return;
        }
        const u32 r = static_cast<u32>(smin(run_of(c, sj, lane == 0u ? 0x10u : 0x0Cu, lane == 0u ? 0x14u : 0x18u),
                                            sw(n)));
        u32 result = 0u;
        apply(c, Post::StoreCount, rbyte, r, n, result);
        c.gpr[3] = result;
    });
    const auto rbf_get_owner = [this, chunk_owner, read_chunk](Cpu& c) {
        const u32 sj = c.gpr[3], ck = c.gpr[6];
        Chunk chunk = read_chunk(c, ck);
        const Fault fault = rbf_get(c, sj, c.gpr[4], c.gpr[5], chunk);
        chunk_owner(c, sj, ck, true, fault, chunk);
    };
    own(kRbfGetChunk, "SJRBF_GetChunk", rbf_get_owner);
    own(kRbfGetBody, "SJRBF_GetChunk body", rbf_get_owner);
    const auto rbf_put_owner = [this, chunk_owner, read_chunk](Cpu& c) {
        const u32 sj = c.gpr[3], ck = c.gpr[5];
        Chunk chunk = read_chunk(c, ck);
        const Fault fault = rbf_put(c, sj, c.gpr[4], chunk);
        chunk_owner(c, sj, ck, true, fault, chunk);
    };
    own(kRbfPutChunk, "SJRBF_PutChunk", rbf_put_owner);
    own(kRbfPutBody, "SJRBF_PutChunk body", rbf_put_owner);
    const auto rbf_unget_owner = [this, chunk_owner, read_chunk](Cpu& c) {
        const u32 sj = c.gpr[3], ck = c.gpr[5];
        Chunk chunk = read_chunk(c, ck);
        const Fault fault = rbf_unget(c, sj, c.gpr[4], chunk);
        chunk_owner(c, sj, ck, true, fault, chunk);
    };
    own(kRbfUngetChunk, "SJRBF_UngetChunk", rbf_unget_owner);
    own(kRbfUngetBody, "SJRBF_UngetChunk body", rbf_unget_owner);

    // ---- SJ_SplitChunk (chunk, n, first, rest) and the error forwarder ------------------------------------
    // In place, as the original: a caller may name one chunk as both halves (split_chunk serves native callers).
    own(kSplitChunk, "SJ_SplitChunk", [this](Cpu& c) {
        const u32 ck = c.gpr[3], n = c.gpr[4], first = c.gpr[5], rest = c.gpr[6];
        ++stats_.splits;
        const u32 data = c.mem_read32(ck), length = c.mem_read32(ck + 4u);
        c.mem_write32(first, data);
        c.mem_write32(first + 4u, length);
        c.mem_write32(rest + 4u, c.mem_read32(first + 4u));
        if (sw(c.mem_read32(first + 4u)) > sw(n)) c.mem_write32(first + 4u, n);
        c.mem_write32(rest + 4u, c.mem_read32(rest + 4u) - c.mem_read32(first + 4u));
        if (c.mem_read32(rest + 4u) == 0u)
            c.mem_write32(rest, 0u);
        else
            c.mem_write32(rest, c.mem_read32(first) + c.mem_read32(first + 4u));
    });
    own(kSjError, "SJ error report", [this](Cpu& c) {
        ++stats_.errors;
        svm_.error_report(cstring(c, c.gpr[3]));
    });
    own(kReturn, "a guest call returned", [this](Cpu& c) { resume(c); });
}

}  // namespace gekko
