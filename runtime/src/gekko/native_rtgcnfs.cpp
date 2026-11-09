// RenderWare's GameCube file system RtGCNFS, native over the asset namespace. Contract: include/gekko/native_rtgcnfs.h,
// docs/RTGCNFS_SPEC.md (every function's specification from the disassembly) and docs/DISC_SYSTEM_MAP.md ("Cut 4").

#include "gekko/native_rtgcnfs.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "gekko/asset_mount.h"
#include "gekko/cpu.h"
#include "gekko/native_os_threads.h"
#include "gekko/native_file_api.h"
#include "gekko/recomp.h"

namespace gekko {

namespace {

// ---- the entry points
constexpr u32 kOpen = 0x8047B874u;
constexpr u32 kClose = 0x8047B988u;
constexpr u32 kRead = 0x8047BC60u;
constexpr u32 kReadChunk = 0x8047BC64u;     // continuation: a chunk's yield returned
constexpr u32 kReadTransfer = 0x8047BC68u;  // continuation: a transfer's yield returned
constexpr u32 kSync = 0x8047C1A4u;
constexpr u32 kAbort = 0x8047C3D4u;

const NativeRtGcnFs::Address kAddresses[] = {
    {kOpen, "RtGCNFS open", false},
    {kClose, "RtGCNFS close", false},
    {kRead, "RtGCNFS read", false},
    {kReadChunk, "RtGCNFS read: a chunk's yield returned", true},
    {kReadTransfer, "RtGCNFS read: a transfer's yield returned", true},
    {kSync, "RtGCNFS sync", false},
    {kAbort, "RtGCNFS abort", false},
};

// ---- what they call
constexpr u32 kYield = 0x80378A08u;          // OSYieldThread
constexpr u32 kMemmove = 0x800054F4u;        // memcpy (memmove; a native owner)
constexpr u32 kGetDeviceNameFunction = 0x8047B83Cu;  // RtGCNFS's fsGetDeviceName: the device name is ""
constexpr u32 kDeviceNameOffset = 0x2A44u;           // ... at 0x2A44(r13)

// ---- RtFile (0x90) and RtFileSystem fields (docs/RTGCNFS_SPEC.md section 3)
constexpr u32 fLengthHi = 0x00u, fLengthLo = 0x04u, fPositionHi = 0x08u, fPositionLo = 0x0Cu;
constexpr u32 fInitialHi = 0x10u, fInitialLo = 0x14u, fAsync = 0x18u, fPriority = 0x1Cu, fStatus = 0x20u;
constexpr u32 fError = 0x24u, fFileSystem = 0x38u;
constexpr u32 fResult = 0x6Cu, fToken = 0x70u, fFileLength = 0x74u;
constexpr u32 fCache = 0x7Cu, fCacheValid = 0x80u, fCacheOffset = 0x84u, fCacheSize = 0x88u;
constexpr u32 sStatus = 0x08u, sGetDeviceName = 0x1Cu, sErrorCallback = 0x5Cu, sErrorData = 0x60u;
constexpr u32 kStatusClosed = 1u, kStatusReady = 2u, kStatusError = 4u;
constexpr u32 kNoError = 1u, kDriveNoError = 7u, kFsReady = 2u;
constexpr u32 kGlobalOffset = 0x2A40u;  // G = 0x2A40(r13): the device's drive state (1 = OK)

// The original's frames, byte for byte (the game copies stack leftovers into heap records):
//   read 0x8047BC60, frame F = 0x40: F+0 back chain, F+0x44 (the caller's slot) LR, F+0x1C..F+0x3F r23..r31 of the
//     caller (_savegpr_23 0x803A1EE0 with r11 = F+0x40). Nothing else of F is written on the synchronous paths (the
//     stfd at F+8 is the asynchronous report's). It keeps its state in registers: r23 file, r24 destination,
//     r25 count, r26 retries, r27 position % 32, r28 destination % 32, r29 a cache serve's length, r30 remaining,
//     r31 the jump/string table 0x8056EE40. Its callees store their LR at F+4.
//   its transfer 0x8047B9C8(file, buffer, size, offset, async), frame T = F - 0x20: T+0 back chain, F+4 LR,
//     T+0x14/0x18/0x1C read's r29/r30/r31; it holds r29 file, r30 its file system, r31 the 32-rounded size (after its
//     drive poll: the drive status, 0).
//   open 0x8047B874, frame O = 0x120: O+0 back chain, the caller's LR slot, O+0x110..0x11F r28..r31 of the caller,
//     the path buffer O+8 (0x100); r28 fs, r29 file, r30 flags, r31 the name past its device. Its helpers below it:
//     _rtGetFileNameFromDeviceName 0x8047A6E8 (0x20: r29..r31 at +0x14) and _rtFSBuildFileName 0x8047A76C (0x30:
//     r25..r31 at +0x14 by _savegpr_25).
//   close / sync / abort: 0x10 (close r31 at +0xC; sync r30 at +8, r31 at +0xC; abort nothing more).
constexpr u32 kReadFrame = 0x40u, kReadSave = 0x1Cu, kTransferFrame = 0x20u;
constexpr u32 kOpenFrame = 0x120u, kOpenSave = 0x110u, kPathBuffer = 0x100u, kSmallFrame = 0x10u;
constexpr u32 kNameFrame = 0x20u, kBuildFrame = 0x30u, kBuildSave = 0x14u;
constexpr u32 kReadTable = 0x8056EE40u;  // liveness-roots: value-only
// Return sites (handed to callees as their LR, stored in the frames as the original's are; never entered).
constexpr u32 kChunkYieldSite = 0x8047BE58u;     // liveness-roots: value-only
constexpr u32 kDirectSite = 0x8047BED4u;         // liveness-roots: value-only
constexpr u32 kFillSite = 0x8047C034u;           // liveness-roots: value-only
constexpr u32 kTransferYieldSite = 0x8047BA40u;  // liveness-roots: value-only
constexpr u32 kErrorCallbackSite = 0x8047BBF8u;  // liveness-roots: value-only
constexpr u32 kOpenNameSite = 0x8047B8B4u;       // liveness-roots: value-only
constexpr u32 kOpenBuildSite = 0x8047B8E0u;      // liveness-roots: value-only
constexpr u32 kNameLengthSite = 0x8047A744u;     // liveness-roots: value-only
constexpr u32 kBuildCopySite = 0x8047A7DCu;      // liveness-roots: value-only
// RenderWare's engine (0x615C(r13)): its string slots strcpy (+0xCC) and strlen (+0xF4) the path helpers call.
constexpr u32 kEngineOffset = 0x615Cu, eStrcpy = 0xCCu, eStrlen = 0xF4u;
constexpr u32 kCr0Lt = 0x8u, kCr0Eq = 0x2u;

void stop(Cpu& c, const std::string& why) {
    c.request_stop(StopReason::ExecutionFault, "native RtGCNFS: " + why);
}

// The contract is synchronous files only. No caller in this game can open a file asynchronously (mode bit 0x10, or a
// file system defaulted to async): proven from the DOL by tools/platform/rtfs_async_proof.py (CTest
// platform_rtfs_async_proof). The original's asynchronous branches (OSReport diagnostics, the outstanding-operation
// callback) are therefore not served; an asynchronous open or record is refused by stopping the run, never served
// silently.
void refuse_async(Cpu& c, const char* where) {
    stop(c, std::string("an asynchronous file (") + where +
                "): outside the proven synchronous contract (docs/RTGCNFS_SPEC.md, rtfs_async_proof.py)");
}

s64 read64(Cpu& c, u32 at) {
    return static_cast<s64>((static_cast<u64>(c.mem_read32(at)) << 32) | c.mem_read32(at + 4u));
}
void write64(Cpu& c, u32 at, s64 v) {
    c.mem_write32(at, static_cast<u32>(static_cast<u64>(v) >> 32));
    c.mem_write32(at + 4u, static_cast<u32>(static_cast<u64>(v)));
}
s32 s(u32 v) noexcept { return static_cast<s32>(v); }

bool guest_string(Cpu& c, u32 address, std::string& out) {
    out.clear();
    for (u32 i = 0u; i < 4096u; ++i) {
        const u8 byte = c.mem_read8(address + i);
        if (byte == 0u) return true;
        out.push_back(static_cast<char>(byte));
    }
    return false;
}

// A synchronous guest call returning here (OSReport, memmove, the error callback, the outstanding-op callback).
// `site` is the return address the callee sees (its LR): the original call site's, where callers record it (the
// game-memory journal logs memmove's caller).
bool call_guest_from(Cpu& c, u32 site, u32 function, u32 r3, u32 r4 = 0u, u32 r5 = 0u, u32 r6 = 0u, u32 r7 = 0u) {
    const u32 resume = c.lr;
    const u32 link = site;
    c.gpr[3] = r3;
    c.gpr[4] = r4;
    c.gpr[5] = r5;
    c.gpr[6] = r6;
    c.gpr[7] = r7;
    c.gpr[12] = function;
    c.ctr = function;
    recomp::call_indirect(c, function, link);
    if (c.exiting()) c.finish_transfers();
    if (c.exiting() || c.pc != link) {
        stop(c, "a guest call (0x" + std::to_string(function) + ") did not return");
        return false;
    }
    c.lr = resume;
    return true;
}
// A guest call the original makes from `frame` at `site` (its bl / bctrl return address), with the non-volatile
// registers the owner keeps exactly as the original holds them there (r14..r31 are left as they are); r6 and up are
// not arguments of any callee here and keep what they hold.
bool call_at(Cpu& c, u32 frame, u32 site, u32 function, u32 r3, u32 r4, u32 r5) {
    const u32 resume = c.lr;
    c.gpr[1] = frame;
    c.gpr[3] = r3;
    c.gpr[4] = r4;
    c.gpr[5] = r5;
    c.gpr[12] = function;
    c.ctr = function;
    recomp::call_indirect(c, function, site);
    if (c.exiting()) c.finish_transfers();
    if (c.exiting() || c.pc != site || c.gpr[1] != frame) {
        stop(c, "a guest call (0x" + std::to_string(function) + ") did not return");
        return false;
    }
    c.lr = resume;
    return true;
}
// stwu r1, -size(r1); mflr r0; stw r0, size + 4(r1): the frame.
u32 prologue(Cpu& c, u32 sp, u32 link, u32 size) {
    const u32 frame = sp - size;
    c.mem_write32(frame, sp);
    c.mem_write32(sp + 4u, link);
    return frame;
}
// stmw / _savegpr_N: r(first)..r31 at `at`; lmw / _restgpr_N back.
void save_gprs(Cpu& c, u32 at, u32 first) {
    for (u32 r = first; r <= 31u; ++r) c.mem_write32(at + (r - first) * 4u, c.gpr[r]);
}
void restore_gprs(Cpu& c, u32 at, u32 first) {
    for (u32 r = first; r <= 31u; ++r) c.gpr[r] = c.mem_read32(at + (r - first) * 4u);
}
// cr0 as the original's last compare left it (with XER[SO]), where a thread switch saves the CR.
void set_cr0(Cpu& c, u32 bits) {
    c.cr = (c.cr & 0x0FFFFFFFu) | (bits << 28) | ((c.xer >> 31) << 28);
}
// The return address of read's `bl memmove` (cache serve): handed to memmove as its LR, never entered.
constexpr u32 kMemmoveSite = 0x8047BF74u;  // liveness-roots: value-only

// ---- the game's chunked background read (docs/STAGE_LOADING.md 9d), GEKKO_NATIVE_BG_READ=1
// TBGRead's read loop 0x80045150(manager, file, &destination, -, chunk), called only from TBGRead's step (0x800456C4,
// chunk 0x2000): no destination -> fn_80049BB8(length, 2); then `RwFread(destination, k, 1, file)` (0x8047B430: the
// file system's read slot fs+0x30 with k * 1 bytes, the result divided by k) for k = chunk while more than a chunk
// remains (signed), then once for the rest, whose count is rounded up to 32; it returns the sum.
constexpr u32 kBgReadLoop = 0x80045150u;
constexpr u32 kRwAlloc = 0x80049BB8u;  // the RenderWare allocator (size, hint)
constexpr u32 sRead = 0x30u;           // RtFileSystem: the read slot

}  // namespace

void NativeRtGcnFs::drain(Cpu& c) {
    // Each elided yield: OSYieldThread returned at once (r1 = the frame it was entered with, back at the continuation).
    while (pending_ && !c.stopped()) {
        const Pending p = *pending_;
        pending_.reset();
        c.gpr[1] = p.sp;
        c.lr = p.resume;
        if (p.resume == kReadChunk) read_chunk_(c);
        else read_transfer_(c);
    }
}

std::optional<u32> NativeRtGcnFs::read_sync(Cpu& c, u32 file, u32 destination, u32 count) {
    ++stats_.sync_reads;
    sync_ = true;
    pending_.reset();
    c.gpr[3] = file;
    c.gpr[4] = destination;
    c.gpr[5] = count;
    read_entry_(c);
    // Each yield: OSYieldThread returned at once (r1 = the frame it was entered with, back at the continuation).
    while (pending_ && !c.stopped()) {
        const Pending p = *pending_;
        pending_.reset();
        c.gpr[1] = p.sp;
        c.lr = p.resume;
        if (p.resume == kReadChunk) read_chunk_(c);
        else read_transfer_(c);
    }
    sync_ = false;
    pending_.reset();
    if (c.stopped()) return std::nullopt;
    return c.gpr[3];
}

const NativeRtGcnFs::Address* NativeRtGcnFs::addresses(std::size_t& count) noexcept {
    count = sizeof(kAddresses) / sizeof(kAddresses[0]);
    return kAddresses;
}

void NativeRtGcnFs::install() {
    Cpu& cpu = cpu_;
    const auto own = [&cpu](u32 address, const char* name, auto&& serve) {
        cpu.install_hle(address, [serve](Cpu& c) {
            serve(c);
            return true;  // an owner never declines
        }, std::string("native RtGCNFS: ") + name);
    };

    // ---- read: the original's code around its two yields, in its frames and registers ----------------------
    // Frame-exact (see the layout at the top): read's state lives in r23..r31 as the original allocates them, so it
    // survives each yield in the thread context exactly as the original's does, and every frame word, every guest
    // call's r1 / LR / non-volatile registers and every yield's are the original's. Each yield hands OSYieldThread a
    // continuation entry for its LR in place of the return site (0x8047BE58 / 0x8047BA40); OSYieldThread stores it in
    // the frame's LR word, which the continuation puts back to the original's return site.
    // read's epilogue: _restgpr_23, LR from the caller's slot, r1 back, `value` in r3.
    const auto finish_read = [](Cpu& c, u32 value) {
        const u32 frame = c.gpr[1];
        c.gpr[11] = frame + kReadFrame;
        restore_gprs(c, frame + kReadSave, 23u);
        c.gpr[0] = c.mem_read32(frame + kReadFrame + 4u);
        c.lr = c.gpr[0];
        c.gpr[1] = frame + kReadFrame;
        c.gpr[3] = value;
    };
    // `bl OSYieldThread` from `sp` with cr0 as the original's last compare left it, returning at `resume`.
    const auto yield = [this](Cpu& c, u32 sp, u32 resume, u32 cr0) {
        c.gpr[1] = sp;
        c.lr = resume;
        set_cr0(c, cr0);
        if (sync_ || os_threads::yield_is_noop(c)) {  // no other thread of this priority is ready - continued at once
            ++stats_.yields_elided;
            pending_ = Pending{sp, resume};
            return;
        }
        ++stats_.yields;
        c.leave_to_dispatcher(kYield, FallbackReason::Yield, resume);
    };
    // The loop head 0x8047C054: the next chunk yields first (bl at 0x8047BE54), or the read is done.
    const auto chunk = [yield, finish_read](Cpu& c) {
        if (s(c.gpr[30]) > 0 && s(c.gpr[26]) < 1) {
            yield(c, c.gpr[1], kReadChunk, kCr0Lt);  // cmpwi r26, 1
            return;
        }
        c.gpr[0] = kStatusReady;
        c.mem_write32(c.gpr[23] + fStatus, kStatusReady);  // synchronous (async records are refused at read entry)
        finish_read(c, c.gpr[25] - c.gpr[30]);
    };
    // `bl 0x8047B9C8(file, buffer, size, offset, 0)` from read's frame at `site`: its prologue, the DVD read (the bytes
    // at once, as the read-complete callback left them: the result in +0x6C), then its drive wait's yield.
    const auto transfer = [this, yield](Cpu& c, u32 site, u32 buffer, u32 size, u32 offset) {
        const u32 file = c.gpr[23];
        const u32 frame = prologue(c, c.gpr[1], site, kTransferFrame);
        save_gprs(c, frame + 0x14u, 29u);
        const u32 aligned = (size + 0x1Fu) & ~0x1Fu;
        c.gpr[31] = aligned;
        c.gpr[29] = file;
        c.gpr[30] = c.mem_read32(file + fFileSystem);
        c.gpr[1] = frame;
        if (!files_.device_read(c, c.mem_read32(file + fToken) + offset, buffer, aligned)) {
            stop(c, "a read of " + std::to_string(aligned) + " bytes at " + std::to_string(offset) +
                        " could not be delivered");
            return;
        }
        c.mem_write32(file + fResult, aligned);
        ++stats_.transfers;
        stats_.bytes += aligned;
        yield(c, frame, kReadTransfer, kCr0Eq);  // bl at 0x8047BA3C after cmpwi r0(async), 0
    };

    const auto read_entry = [this, finish_read, chunk](Cpu& c) {
        ++stats_.reads;
        const u32 file = c.gpr[3], destination = c.gpr[4], count = c.gpr[5];
        const u32 caller = c.gpr[1];
        const u32 frame = prologue(c, caller, c.lr, kReadFrame);
        c.gpr[11] = caller;
        save_gprs(c, frame + kReadSave, 23u);  // bl _savegpr_23
        c.gpr[1] = frame;
        c.gpr[23] = file;
        c.gpr[24] = destination;
        c.gpr[25] = count;
        c.gpr[31] = kReadTable;
        c.gpr[26] = 0u;
        c.mem_write32(file + fInitialLo, c.mem_read32(file + fPositionLo));
        c.mem_write32(file + fInitialHi, c.mem_read32(file + fPositionHi));
        if (c.mem_read32(file + fAsync) != 0u) {
            refuse_async(c, "read");
            return;
        }
        if (count == 0u) {
            finish_read(c, 0u);
            return;
        }
        const s64 length = read64(c, file + fLengthHi);
        const s64 position = read64(c, file + fPositionHi);
        if (length < position + static_cast<s64>(count))
            c.gpr[25] = c.mem_read32(file + fLengthLo) - c.mem_read32(file + fPositionLo);
        if (c.gpr[25] == 0u) {
            finish_read(c, 0u);
            return;
        }
        c.gpr[30] = c.gpr[25];
        c.gpr[28] = destination & 31u;
        c.gpr[27] = c.mem_read32(file + fPositionLo) & 31u;
        chunk(c);
    };
    // An owner whose yields were elided continues inline until the read finishes or yields for real.
    const auto draining = [this](auto body) {
        return [this, body](Cpu& c) {
            body(c);
            drain(c);
        };
    };
    own(kRead, "read", draining(read_entry));
    read_entry_ = read_entry;

    // A chunk's yield returned (0x8047BE58): the position checks, then a direct transfer, a cache serve or a fill.
    const auto read_chunk = [transfer, finish_read, chunk](Cpu& c) {
        const u32 frame = c.gpr[1];
        if (c.mem_read32(frame) != frame + kReadFrame) {
            stop(c, "a read resumed without its frame");
            return;
        }
        if (c.mem_read32(frame + 4u) == kReadChunk) c.mem_write32(frame + 4u, kChunkYieldSite);  // OSYieldThread's LR
        const u32 file = c.gpr[23];
        const s64 position = read64(c, file + fPositionHi);
        const s64 length = read64(c, file + fLengthHi);
        if (position < 0 || !(position < length)) {
            finish_read(c, 0u);
            return;
        }
        const u32 remaining = c.gpr[30], position_mod = c.gpr[27], destination_mod = c.gpr[28];
        const u32 destination = c.gpr[24];
        if (destination_mod == 0u && position_mod == 0u && s(remaining) >= 32) {
            transfer(c, kDirectSite, destination, remaining & ~31u, c.mem_read32(file + fPositionLo));
            return;
        }
        if (c.mem_read32(file + fAsync) != 0u) {
            refuse_async(c, "read chunk");
            return;
        }
        const u32 cache = c.mem_read32(file + fCache);
        if (c.mem_read32(file + fCacheValid) == 1u) {
            const u32 offset = c.mem_read32(file + fCacheOffset);
            const s32 available = s(c.mem_read32(file + fCacheSize) - offset);
            c.gpr[29] = s(remaining) < available ? remaining : static_cast<u32>(available);
            const u32 take = c.gpr[29];
            if (!call_at(c, frame, kMemmoveSite, kMemmove, destination, cache + offset, take)) return;
            c.mem_write32(file + fCacheOffset, c.mem_read32(file + fCacheOffset) + take);
            c.gpr[28] = static_cast<u32>(s(destination_mod + take) % 32);
            if (s(c.mem_read32(file + fCacheOffset)) >= s(c.mem_read32(file + fCacheSize))) {
                c.mem_write32(file + fCacheValid, 0u);
                c.mem_write32(file + fCacheOffset, 0u);
            }
            write64(c, file + fPositionHi, read64(c, file + fPositionHi) + static_cast<s64>(s(take)));
            c.gpr[27] = static_cast<u32>(s(position_mod + take) % 32);
            c.gpr[30] = remaining - take;
            c.gpr[24] = destination + take;
            chunk(c);
            return;
        }
        const u32 aligned_end = (c.mem_read32(file + fLengthLo) + 0x1Fu) & ~0x1Fu;
        const u32 aligned_position = c.mem_read32(file + fPositionLo) - position_mod;
        u32 size = c.mem_read32(file + fCacheSize);
        if (s(aligned_end) < s(size + aligned_position)) size = aligned_end - aligned_position;
        transfer(c, kFillSite, cache, size, aligned_position);
    };
    own(kReadChunk, "read chunk", draining(read_chunk));
    read_chunk_ = read_chunk;

    // A transfer's yield returned (0x8047BA40): the drive poll (DVDGetDriveStatus: 0, always ready -> the END case
    // 0x8047BBD4: status 4, the file system's drive-error callback with RenderWare's "no error" 7), the result, the
    // transfer's epilogue, and read applies it at its return site (direct 0x8047BED4 / fill 0x8047C034).
    const auto read_transfer = [chunk](Cpu& c) {
        const u32 frame = c.gpr[1], read_frame = frame + kTransferFrame;
        const u32 site = c.mem_read32(read_frame + 4u);
        if (c.mem_read32(frame) != read_frame || c.mem_read32(read_frame) != read_frame + kReadFrame ||
            (site != kDirectSite && site != kFillSite)) {
            stop(c, "a read transfer resumed without its frames");
            return;
        }
        if (c.mem_read32(frame + 4u) == kReadTransfer) c.mem_write32(frame + 4u, kTransferYieldSite);
        const u32 file = c.gpr[29], fs = c.gpr[30];
        c.gpr[31] = 0u;  // the drive status
        c.mem_write32(file + fStatus, kStatusError);
        const u32 callback = c.mem_read32(fs + sErrorCallback);
        if (callback != 0u &&
            !call_at(c, frame, kErrorCallbackSite, callback, kDriveNoError, c.mem_read32(fs + sErrorData), c.gpr[5]))
            return;
        s32 delivered = s(c.mem_read32(file + fResult));
        if (delivered < 0) delivered = 0;
        c.mem_write32(file + fStatus, kStatusReady);
        c.gpr[0] = c.mem_read32(read_frame + 4u);
        restore_gprs(c, frame + 0x14u, 29u);
        c.lr = site;
        c.gpr[1] = read_frame;
        c.gpr[3] = static_cast<u32>(delivered);
        if (site == kDirectSite) {
            if (delivered > 0) {
                write64(c, file + fPositionHi, read64(c, file + fPositionHi) + delivered);
                c.gpr[30] -= static_cast<u32>(delivered);
                c.gpr[24] += static_cast<u32>(delivered);
                c.gpr[26] = 0u;
                c.mem_write32(file + fCacheValid, 0u);
                c.mem_write32(file + fCacheOffset, 0u);
            } else {
                ++c.gpr[26];
            }
        } else {
            if (delivered > 0) {
                c.mem_write32(file + fCacheOffset, c.gpr[27]);
                c.gpr[26] = 0u;
                c.mem_write32(file + fCacheValid, 1u);
            } else {
                ++c.gpr[26];
            }
        }
        chunk(c);
    };
    own(kReadTransfer, "read transfer", draining(read_transfer));
    read_transfer_ = read_transfer;

    // ---- open, close, sync, abort: their frames as the original's ------------------------------------------
    own(kOpen, "open", [this](Cpu& c) {
        const u32 fs = c.gpr[3], file = c.gpr[4], name = c.gpr[5], flags = c.gpr[6];
        const u32 caller = c.gpr[1];
        const u32 frame = prologue(c, caller, c.lr, kOpenFrame);
        save_gprs(c, frame + kOpenSave, 28u);
        c.gpr[1] = frame;
        c.gpr[30] = flags;
        c.gpr[29] = file;
        c.gpr[28] = fs;
        const auto done = [&c, frame, caller](u32 value) {
            c.gpr[0] = c.mem_read32(caller + 4u);
            restore_gprs(c, frame + kOpenSave, 28u);
            c.lr = c.gpr[0];
            c.gpr[1] = caller;
            c.gpr[3] = value;
        };
        if ((flags & 0xEu) != 0u) return done(4u);  // RTFS_ERROR_WRITEPROTECTED
        if ((flags & 0x10u) != 0u) {  // the original: isAsync = 1, status BUSY
            refuse_async(c, "open");
            return;
        }
        // _rtGetFileNameFromDeviceName(name) from open's frame: the text after the first ':' (the whole name without
        // one), scanning while i < the engine's strlen(name).
        {
            const u32 helper = prologue(c, frame, kOpenNameSite, kNameFrame);
            save_gprs(c, helper + 0x14u, 29u);
            c.gpr[30] = 0u;
            c.gpr[29] = name;
            c.gpr[31] = name;
            for (;;) {
                const u32 engine = c.mem_read32(c.gpr[13] + kEngineOffset);
                if (!call_at(c, helper, kNameLengthSite, c.mem_read32(engine + eStrlen), c.gpr[29], engine, c.gpr[5]))
                    return;
                if (!(s(c.gpr[30]) < s(c.gpr[3]))) break;
                if (c.mem_read8(c.gpr[31]) == static_cast<u8>(':')) {
                    c.gpr[29] = c.gpr[30] + c.gpr[29] + 1u;
                    break;
                }
                ++c.gpr[30];
                ++c.gpr[31];
            }
            c.gpr[3] = c.gpr[29];
            restore_gprs(c, helper + 0x14u, 29u);
            c.gpr[1] = frame;
        }
        c.gpr[31] = c.gpr[3];
        // The file system's device name (fs+0x1C: RtGCNFS's own leaf `addi r3, r13, 0x2A44; blr`: "").
        if (c.mem_read32(fs + sGetDeviceName) != kGetDeviceNameFunction) {
            stop(c, "a file system whose device-name function is not RtGCNFS's");
            return;
        }
        const u32 drive = c.gpr[13] + kDeviceNameOffset;
        // _rtFSBuildFileName(frame + 8, 0x100, drive, path, '/') from open's frame: refused past 0x100 - 1; the drive
        // by the engine's strcpy, then the path with '/' and '\' as '/' (its NUL included).
        u32 built = 0u;
        {
            const u32 helper = prologue(c, frame, kOpenBuildSite, kBuildFrame);
            c.gpr[11] = frame;
            save_gprs(c, helper + kBuildSave, 25u);  // bl _savegpr_25
            const u32 buffer = frame + 8u, path = c.gpr[31];
            c.gpr[27] = drive;
            c.gpr[25] = buffer;
            c.gpr[26] = kPathBuffer;
            c.gpr[28] = path;
            c.gpr[29] = static_cast<u32>('/');
            // bl strlen 0x803ADC08 (a leaf: no frame).
            const auto length = [&c](u32 at, u32& n) {
                for (n = 0u; n < 0x10000u; ++n)
                    if (c.mem_read8(at + n) == 0u) return true;
                stop(c, "a file name without a NUL");
                return false;
            };
            if (!length(drive, c.gpr[30]) || !length(path, c.gpr[31])) return;
            if (s(c.gpr[30] + c.gpr[31]) <= s(kPathBuffer - 1u)) {
                const u32 engine = c.mem_read32(c.gpr[13] + kEngineOffset);
                if (!call_at(c, helper, kBuildCopySite, c.mem_read32(engine + eStrcpy), buffer, drive, engine)) return;
                const u32 to = buffer + c.gpr[30];
                for (u32 i = 0u; i <= c.gpr[31]; ++i) {
                    const u8 byte = c.mem_read8(path + i);
                    c.mem_write8(to + i, byte == '/' || byte == '\\' ? static_cast<u8>(c.gpr[29]) : byte);
                }
                built = buffer;
            }
            c.gpr[11] = frame;
            restore_gprs(c, helper + kBuildSave, 25u);
            c.gpr[1] = frame;
        }
        if (built == 0u) return done(5u);  // RTFS_ERROR_FILENAMETOOLONG
        std::string path;
        if (!guest_string(c, built, path)) {
            stop(c, "a built file name without a NUL");
            return;
        }
        // DVDOpen(frame + 8, file + 0x40): the name resolved in the asset namespace.
        ++stats_.opens;
        const assets::AssetNamespace& names = assets_.names();
        const s32 entry = names.resolve(path);
        if (entry < 0 || names.is_directory(static_cast<u32>(entry)) ||
            names.entry(static_cast<u32>(entry)).size > 0xFFFFFFFFull) {
            ++stats_.open_misses;
            std::printf("RtGCNFS: '%s' was not found under /.\n", path.c_str());
            return done(3u);  // RTFS_ERROR_FILENOTFOUND
        }
        const u32 length = static_cast<u32>(names.entry(static_cast<u32>(entry)).size);
        c.mem_write32(file + fToken, assets_.token_of(static_cast<u32>(entry)));
        c.mem_write32(file + fFileLength, length);
        c.mem_write32(file + fStatus, kStatusReady);
        c.mem_write32(file + fError, kNoError);
        c.mem_write32(file + fPositionLo, 0u);
        c.mem_write32(file + fPositionHi, 0u);
        c.mem_write32(file + fCacheValid, 0u);
        c.mem_write32(file + fCacheOffset, 0u);
        c.mem_write32(file + fLengthLo, length);
        c.mem_write32(file + fLengthHi, 0u);
        c.mem_write32(file + fAsync, 0u);  // synchronous (an asynchronous open was refused above)
        c.mem_write32(file + fStatus, kStatusReady);
        c.mem_write32(file + fPriority, 2u);
        done(1u);  // RTFS_ERROR_NOERROR
    });
    own(kClose, "close", [](Cpu& c) {
        const u32 frame = prologue(c, c.gpr[1], c.lr, kSmallFrame);
        c.mem_write32(frame + 0xCu, c.gpr[31]);
        c.mem_write32(c.gpr[3] + fStatus, kStatusClosed);  // after DVDClose(file + 0x40): nothing left to close
        c.gpr[0] = c.lr;
        c.gpr[3] = 1u;  // the original's r3 was DVDClose's TRUE
    });
    // The drive is always ready (drive status 0) and every transfer complete (block END).
    own(kSync, "sync", [](Cpu& c) {
        const u32 file = c.gpr[3];
        const u32 frame = prologue(c, c.gpr[1], c.lr, kSmallFrame);
        c.mem_write32(frame + 0xCu, c.gpr[31]);
        c.mem_write32(frame + 0x8u, c.gpr[30]);
        const u32 fs = c.mem_read32(file + fFileSystem);
        const u32 global = c.gpr[13] + kGlobalOffset;
        if (!(c.mem_read32(global) == 1u && c.mem_read32(file + fStatus) != kStatusError)) {
            c.mem_write32(fs + sStatus, kFsReady);
            c.mem_write32(global, 1u);
        }
        if (c.mem_read32(file + fAsync) != 0u) {  // the original's outstanding-operation callback tail
            refuse_async(c, "sync");
            return;
        }
        c.mem_write32(file + fStatus, kStatusReady);
        c.gpr[0] = c.lr;
        c.gpr[3] = c.mem_read32(file + fStatus);
    });
    own(kAbort, "abort", [](Cpu& c) {
        prologue(c, c.gpr[1], c.lr, kSmallFrame);
        c.gpr[0] = c.lr;
        c.gpr[3] = 1u;  // DVDCancelAsync: nothing in flight
    });

    // ---- TBGRead's chunked read, served inline (GEKKO_NATIVE_BG_READ=1; docs/STAGE_LOADING.md 9d) -------------
    // The same per-chunk reads, each the synchronous read above step by step - its entry, the direct transfers, the
    // cache fill and serve, every transfer's drive-error callback and status writes, the memmove of a cache serve -
    // in host code without its two yields (the chunk loop's head and the transfer's drive wait). Only when the step
    // ends changes: the original handed the CPU to the game thread twice per chunk, one presented frame each while
    // the loading screen is up. Bytes, file state, guest calls and their order are the original's.
    const char* bg = std::getenv("GEKKO_NATIVE_BG_READ");
    // Default on (user decision 2026-11-08, S4); GEKKO_NATIVE_BG_READ=0 selects the original TBGRead thread.
    if (bg != nullptr && bg[0] != '1') return;
    // The loop 0x80045150 as the original runs it, frame for frame (the game copies stack leftovers into heap records:
    // a native owner's frames must be the original's). Its frame (0x20: LR in the caller's slot, r27..r31 at +0xC);
    // each RwFread 0x8047B430 from it (0x10: r31 at +0xC) entering the file system's read slot - RtGCNFS's read run by
    // read_sync, its own code and frames with each yield continued at once (the user-approved change of
    // GEKKO_NATIVE_BG_READ: the chunk loop no longer hands the CPU back twice per chunk). r3 = the sum; r27..r31 are
    // restored by its lmw.
    struct Loop {
        u32 sp = 0, link = 0, frame = 0;
        std::array<u32, 5> caller{};  // r27..r31 on entry
        std::array<u32, 5> regs{};    // r27..r31 as the loop holds them at a call
    };
    const auto enter_loop = [](Cpu& c, u32 file, u32 at, u32 chunk) {
        Loop l;
        l.sp = c.gpr[1];
        l.link = c.lr;
        for (u32 r = 27u; r <= 31u; ++r) l.caller[r - 27u] = c.gpr[r];
        l.frame = l.sp - 0x20u;
        c.mem_write32(l.frame, l.sp);
        c.mem_write32(l.sp + 4u, l.link);
        for (u32 r = 27u; r <= 31u; ++r) c.mem_write32(l.frame + 0xCu + (r - 27u) * 4u, l.caller[r - 27u]);
        l.regs = l.caller;
        l.regs[0] = file;   // or. r27, r4, r4
        l.regs[1] = at;     // mr r28, r5
        l.regs[4] = chunk;  // mr r31, r7
        return l;
    };
    // A guest call from the loop's frame at `site` (its `bl`'s return address) with the loop's registers.
    const auto call_from_loop = [](Cpu& c, const Loop& l, u32 site, u32 function, u32 r3, u32 r4) {
        const u32 sp = c.gpr[1], link = c.lr;
        std::array<u32, 5> saved{};
        for (u32 r = 27u; r <= 31u; ++r) saved[r - 27u] = c.gpr[r];
        c.gpr[1] = l.frame;
        for (u32 r = 27u; r <= 31u; ++r) c.gpr[r] = l.regs[r - 27u];
        c.lr = site;
        const bool ok = call_guest_from(c, site, function, r3, r4);
        const u32 result = c.gpr[3];
        c.gpr[1] = sp;
        c.lr = link;
        for (u32 r = 27u; r <= 31u; ++r) c.gpr[r] = saved[r - 27u];
        c.gpr[3] = result;
        return ok;
    };
    // RwFread(destination, k, 1, file) from the loop's frame at `site`; false when the run was stopped.
    const auto fread = [this](Cpu& c, const Loop& l, u32 site, u32 file, u32 destination, u32 k, u32& count) {
        const u32 sp = c.gpr[1], link = c.lr;
        std::array<u32, 5> saved{};
        for (u32 r = 27u; r <= 31u; ++r) saved[r - 27u] = c.gpr[r];
        const u32 frame = l.frame - 0x10u;  // stwu r1, -0x10(r1); mflr r0; stw r0, 0x14(r1); stw r31, 0xC(r1)
        c.mem_write32(frame, l.frame);
        c.mem_write32(l.frame + 4u, site);
        c.mem_write32(frame + 0xCu, l.regs[4]);
        c.gpr[1] = frame;
        for (u32 r = 27u; r <= 30u; ++r) c.gpr[r] = l.regs[r - 27u];
        c.gpr[31] = k;  // mr r31, r4
        c.lr = 0x8047B460u;  // the slot's bctrl return
        const u32 fs = c.mem_read32(file + fFileSystem);
        const u32 slot = c.mem_read32(fs + sRead);
        std::optional<u32> bytes;
        if (slot == kRead) {
            bytes = read_sync(c, file, destination, k);
        } else if (call_guest_from(c, 0x8047B460u, slot, file, destination, k)) {  // another file system's own read
            bytes = c.gpr[3];
        }
        c.gpr[1] = sp;
        c.lr = link;
        for (u32 r = 27u; r <= 31u; ++r) c.gpr[r] = saved[r - 27u];
        if (!bytes) return false;
        count = k != 0u ? *bytes / k : 0u;  // divwu (0 for k = 0, where the original's quotient is undefined and x 0)
        return true;
    };
    const auto run_loop = [this, fread](Cpu& c, Loop& l, u32 file, u32 destination, u32 chunk) -> std::optional<u32> {
        ++stats_.background_reads;
        const u32 length = c.mem_read32(file + fLengthLo);
        u32 count = 0u;
        if (chunk == 0u) {  // 0x80045208: r28 is still &destination; r29/r30 the caller's
            if (!fread(c, l, 0x8004521Cu, file, destination, length, count)) return std::nullopt;
            return length * count;
        }
        u32 total = 0u;
        l.regs[2] = length;       // r29 = remaining
        l.regs[3] = 0u;           // r30 = total
        l.regs[1] = destination;  // r28 = the destination
        for (u32 remaining = length; remaining != 0u;) {
            if (static_cast<s32>(remaining) > static_cast<s32>(chunk)) {
                if (!fread(c, l, 0x800451C0u, file, destination, chunk, count)) return std::nullopt;
                total += chunk * count;
                remaining -= chunk;
                destination += chunk;
            } else {
                if (!fread(c, l, 0x800451E8u, file, destination, remaining, count)) return std::nullopt;
                total += ((remaining + 0x1Fu) & ~0x1Fu) * count;
                remaining = 0u;
            }
            l.regs[1] = destination;
            l.regs[2] = remaining;
            l.regs[3] = total;
            ++stats_.background_chunks;
        }
        return total;
    };
    // W2 (GEKKO_NATIVE_EXPAND_JOBS=5): the loop for a queued read at the reaper, its destination already allocated.
    chunked_read_ = [enter_loop, run_loop](Cpu& c, u32 file, u32 start, u32 chunk) -> std::optional<u32> {
        Loop l = enter_loop(c, file, 0u, chunk);
        return run_loop(c, l, file, start, chunk);
    };
    own(kBgReadLoop, "TBGRead's chunked read (yields continued at once)",
        [enter_loop, call_from_loop, run_loop](Cpu& c) {
            const u32 file = c.gpr[4], at = c.gpr[5], chunk = c.gpr[7];
            Loop l = enter_loop(c, file, at, chunk);
            if (file == 0u) {  // beq: li r3, 0
                c.gpr[3] = 0u;
                return;
            }
            if (c.mem_read32(at) == 0u) {
                if (!call_from_loop(c, l, 0x80045188u, kRwAlloc, c.mem_read32(file + fLengthLo), 2u)) return;
                c.mem_write32(at, c.gpr[3]);
            }
            if (const std::optional<u32> total = run_loop(c, l, file, c.mem_read32(at), chunk)) c.gpr[3] = *total;
        });
}

}  // namespace gekko
