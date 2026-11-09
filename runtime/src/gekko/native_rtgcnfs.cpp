// RenderWare's GameCube file system RtGCNFS, native over the asset namespace. Contract: include/gekko/native_rtgcnfs.h,
// docs/RTGCNFS_SPEC.md (every function's specification from the disassembly) and docs/DISC_SYSTEM_MAP.md ("Cut 4").

#include "gekko/native_rtgcnfs.h"

#include <cstdio>
#include <string>

#include "gekko/asset_mount.h"
#include "gekko/cpu.h"
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
constexpr u32 kReport = 0x803731CCu;         // OSReport
constexpr u32 kMemmove = 0x800054F4u;        // memcpy (memmove; a native owner)
constexpr u32 kGetDeviceNameFunction = 0x8047B83Cu;  // RtGCNFS's fsGetDeviceName: the device name is ""
constexpr u32 kDeviceNameOffset = 0x2A44u;           // ... at 0x2A44(r13)
constexpr u32 kTriggerCallback = 0x8047AA18u;  // rtFSTriggerFileOustandingOpCallback(file)

// ---- RtFile (0x90) and RtFileSystem fields (docs/RTGCNFS_SPEC.md section 3)
constexpr u32 fLengthHi = 0x00u, fLengthLo = 0x04u, fPositionHi = 0x08u, fPositionLo = 0x0Cu;
constexpr u32 fInitialHi = 0x10u, fInitialLo = 0x14u, fAsync = 0x18u, fPriority = 0x1Cu, fStatus = 0x20u;
constexpr u32 fError = 0x24u, fOutstandingOp = 0x28u, fOutstandingSize = 0x2Cu, fFileSystem = 0x38u;
constexpr u32 fResult = 0x6Cu, fToken = 0x70u, fFileLength = 0x74u;
constexpr u32 fCache = 0x7Cu, fCacheValid = 0x80u, fCacheOffset = 0x84u, fCacheSize = 0x88u;
constexpr u32 sStatus = 0x08u, sGetDeviceName = 0x1Cu, sErrorCallback = 0x5Cu, sErrorData = 0x60u;
constexpr u32 kStatusClosed = 1u, kStatusReady = 2u, kStatusBusy = 3u, kStatusError = 4u;
constexpr u32 kNoError = 1u, kOpRead = 5u, kDriveNoError = 7u, kFsReady = 2u;
constexpr u32 kGlobalOffset = 0x2A40u;  // G = 0x2A40(r13): the device's drive state (1 = OK)

// The original's frames: read 0x40 (the chunk yield runs at it), a transfer below it 0x20, open 0x120.
constexpr u32 kReadFrame = 0x40u, kTransferFrame = 0x20u, kOpenFrame = 0x120u, kPathBuffer = 0x100u;
// The read's state, kept in its frame.
constexpr u32 kTag = 0x52544653u;  // 'RTFS'
constexpr u32 rTag = 0x08u, rFile = 0x0Cu, rDestination = 0x10u, rCount = 0x14u, rRemaining = 0x18u;
constexpr u32 rPositionMod = 0x1Cu, rDestinationMod = 0x20u, rRetries = 0x24u, rMode = 0x28u, rLink = 0x2Cu;
constexpr u32 kModeDirect = 1u, kModeFill = 2u;

// OSReport formats of the asynchronous branch (dead in this game: nothing opens a file asynchronously).
constexpr u32 kReportUnaligned = 0x8056EE74u, kReportBuffer = 0x8056EE94u, kReportPosition = 0x8056EEBCu;
constexpr u32 kReportSize = 0x8056EEE8u, kReportCache = 0x8056EF14u;

void stop(Cpu& c, const std::string& why) {
    c.request_stop(StopReason::ExecutionFault, "native RtGCNFS: " + why);
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
bool call_guest(Cpu& c, u32 function, u32 r3, u32 r4 = 0u, u32 r5 = 0u, u32 r6 = 0u, u32 r7 = 0u) {
    return call_guest_from(c, c.lr, function, r3, r4, r5, r6, r7);
}
// The return address of read's `bl memmove` (cache serve): handed to memmove as its LR, never entered.
constexpr u32 kMemmoveSite = 0x8047BF74u;  // liveness-roots: value-only

}  // namespace

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

    // ---- read: the state machine around the two yields ------------------------------------------------------
    // Return from read to its caller with `value`.
    const auto finish_read = [](Cpu& c, u32 frame, u32 value) {
        c.lr = c.mem_read32(frame + rLink);
        c.gpr[1] = frame + kReadFrame;
        c.gpr[3] = value;
    };
    // Hand the CPU to the game's OSYieldThread from `sp`, resuming at `resume`.
    const auto yield = [this](Cpu& c, u32 sp, u32 resume) {
        ++stats_.yields;
        c.gpr[1] = sp;
        c.lr = resume;
        c.leave_to_dispatcher(kYield, FallbackReason::Yield, resume);
    };
    // One transfer: the bytes at once (as the DVD read-complete callback left them: the result in +0x6C).
    const auto transfer = [this](Cpu& c, u32 file, u32 buffer, u32 length, u32 offset) {
        const u32 aligned = (length + 0x1Fu) & ~0x1Fu;
        if (!files_.device_read(c, c.mem_read32(file + fToken) + offset, buffer, aligned)) {
            stop(c, "a read of " + std::to_string(aligned) + " bytes at " + std::to_string(offset) +
                        " could not be delivered");
            return false;
        }
        c.mem_write32(file + fResult, aligned);
        ++stats_.transfers;
        stats_.bytes += aligned;
        return true;
    };
    // The loop head of the synchronous read: the next chunk yields first, or the read is done.
    const auto chunk = [yield, finish_read](Cpu& c, u32 frame) {
        const u32 file = c.mem_read32(frame + rFile);
        if (s(c.mem_read32(frame + rRemaining)) > 0 && s(c.mem_read32(frame + rRetries)) < 1) {
            yield(c, frame, kReadChunk);
            return;
        }
        c.mem_write32(file + fStatus, c.mem_read32(file + fAsync) == 0u ? kStatusReady : kStatusBusy);
        finish_read(c, frame, c.mem_read32(frame + rCount) - c.mem_read32(frame + rRemaining));
    };
    const auto frame_of = [](Cpu& c, u32 sp, u32& frame) {
        frame = sp;
        if (c.mem_read32(frame + rTag) != kTag) {
            stop(c, "a read resumed without its frame");
            return false;
        }
        return true;
    };

    own(kRead, "read", [this, transfer, finish_read, chunk](Cpu& c) {
        ++stats_.reads;
        const u32 file = c.gpr[3];
        u32 destination = c.gpr[4];
        u32 count = c.gpr[5];
        const u32 caller = c.gpr[1], frame = caller - kReadFrame;
        c.mem_write32(frame, caller);
        c.mem_write32(caller + 4u, c.lr);
        c.mem_write32(frame + rLink, c.lr);
        c.gpr[1] = frame;
        c.mem_write32(file + fInitialLo, c.mem_read32(file + fPositionLo));
        c.mem_write32(file + fInitialHi, c.mem_read32(file + fPositionHi));
        const bool async = c.mem_read32(file + fAsync) != 0u;
        if (async) c.mem_write32(file + fOutstandingOp, kOpRead);
        if (count == 0u) {
            finish_read(c, frame, 0u);
            return;
        }
        const s64 length = read64(c, file + fLengthHi);
        s64 position = read64(c, file + fPositionHi);
        // r4 after the original's 64-bit compare: the length's high word biased by 0x80000000, or the clamped count
        // (the first report below passes it along, as a format-only OSReport's unused argument).
        u32 compare_residue = c.mem_read32(file + fLengthHi) ^ 0x80000000u;
        if (length < position + static_cast<s64>(count)) {
            count = c.mem_read32(file + fLengthLo) - c.mem_read32(file + fPositionLo);
            compare_residue = count;
            if (async) count = (count + 0x1Fu) & ~0x1Fu;
        }
        if (count == 0u) {
            finish_read(c, frame, 0u);
            return;
        }
        u32 remaining = count;
        const u32 destination_mod = destination & 31u, position_mod = c.mem_read32(file + fPositionLo) & 31u;
        if (async) {  // dead in this game, kept exact
            const s32 size_mod = s(count) % 32;
            if (destination_mod != 0u || position_mod != 0u || size_mod != 0) {
                if (!call_guest(c, kReport, kReportUnaligned, compare_residue)) return;
                if (destination_mod != 0u && !call_guest(c, kReport, kReportBuffer, destination)) return;
                if (position_mod != 0u) {
                    write64(c, frame + 8u, position);  // the original passes the address of a stack copy
                    if (!call_guest(c, kReport, kReportPosition, frame + 8u)) return;
                }
                if (size_mod != 0 && !call_guest(c, kReport, kReportSize, remaining)) return;
                finish_read(c, frame, 0u);
                return;
            }
            s32 retries = 0;
            while (s(remaining) > 0 && retries < 1) {
                if (!transfer(c, file, destination, remaining, c.mem_read32(file + fPositionLo))) return;
                c.mem_write32(file + fStatus, kStatusBusy);
                c.mem_write32(c.gpr[13] + kGlobalOffset, 1u);
                const s32 delivered = s((remaining + 0x1Fu) & ~0x1Fu);
                if (delivered > 0) {
                    position = read64(c, file + fPositionHi) + delivered;
                    write64(c, file + fPositionHi, position);
                    remaining = 0u;
                    retries = 0;
                    c.mem_write32(file + fCacheValid, 0u);
                    c.mem_write32(file + fCacheOffset, 0u);
                } else {
                    ++retries;
                }
            }
            finish_read(c, frame, s(remaining) > 0 ? 0u : count);
            return;
        }
        c.mem_write32(frame + rTag, kTag);
        c.mem_write32(frame + rFile, file);
        c.mem_write32(frame + rDestination, destination);
        c.mem_write32(frame + rCount, count);
        c.mem_write32(frame + rRemaining, remaining);
        c.mem_write32(frame + rPositionMod, position_mod);
        c.mem_write32(frame + rDestinationMod, destination_mod);
        c.mem_write32(frame + rRetries, 0u);
        c.mem_write32(frame + rMode, 0u);
        chunk(c, frame);
    });

    // A chunk's yield returned: the position checks, then a direct transfer, a cache serve or a cache fill.
    own(kReadChunk, "read chunk", [this, transfer, yield, finish_read, chunk, frame_of](Cpu& c) {
        u32 frame = 0u;
        if (!frame_of(c, c.gpr[1], frame)) return;
        const u32 file = c.mem_read32(frame + rFile);
        const s64 position = read64(c, file + fPositionHi);
        const s64 length = read64(c, file + fLengthHi);
        if (position < 0 || !(position < length)) {
            finish_read(c, frame, 0u);
            return;
        }
        const u32 remaining = c.mem_read32(frame + rRemaining);
        const u32 position_mod = c.mem_read32(frame + rPositionMod);
        const u32 destination_mod = c.mem_read32(frame + rDestinationMod);
        const u32 destination = c.mem_read32(frame + rDestination);
        u32 buffer = 0u, size = 0u, offset = 0u;
        if (destination_mod == 0u && position_mod == 0u && s(remaining) >= 32) {
            c.mem_write32(frame + rMode, kModeDirect);
            buffer = destination;
            size = remaining & ~31u;
            offset = c.mem_read32(file + fPositionLo);
        } else if (c.mem_read32(file + fAsync) != 0u) {
            if (!call_guest(c, kReport, kReportCache)) return;
            finish_read(c, frame, 0u);
            return;
        } else if (c.mem_read32(file + fCacheValid) == 1u) {
            const s32 available = s(c.mem_read32(file + fCacheSize) - c.mem_read32(file + fCacheOffset));
            const u32 take = static_cast<u32>(s(remaining) < available ? s(remaining) : available);
            if (!call_guest_from(c, kMemmoveSite, kMemmove, destination,
                                 c.mem_read32(file + fCache) + c.mem_read32(file + fCacheOffset), take))
                return;
            c.mem_write32(file + fCacheOffset, c.mem_read32(file + fCacheOffset) + take);
            c.mem_write32(frame + rDestinationMod, static_cast<u32>(s(destination_mod + take) % 32));
            if (s(c.mem_read32(file + fCacheOffset)) >= s(c.mem_read32(file + fCacheSize))) {
                c.mem_write32(file + fCacheValid, 0u);
                c.mem_write32(file + fCacheOffset, 0u);
            }
            write64(c, file + fPositionHi, position + static_cast<s64>(take));
            c.mem_write32(frame + rPositionMod, static_cast<u32>(s(position_mod + take) % 32));
            c.mem_write32(frame + rRemaining, remaining - take);
            c.mem_write32(frame + rDestination, destination + take);
            chunk(c, frame);
            return;
        } else {
            const u32 aligned_end = (c.mem_read32(file + fLengthLo) + 0x1Fu) & ~0x1Fu;
            const u32 aligned_position = c.mem_read32(file + fPositionLo) - position_mod;
            size = c.mem_read32(file + fCacheSize);
            if (s(aligned_end) < s(aligned_position + size)) size = aligned_end - aligned_position;
            c.mem_write32(frame + rMode, kModeFill);
            buffer = c.mem_read32(file + fCache);
            offset = aligned_position;
        }
        if (!transfer(c, file, buffer, size, offset)) return;
        // The transfer waited on the drive: one yield from its own frame below read's.
        const u32 below = frame - kTransferFrame;
        c.mem_write32(below, frame);
        yield(c, below, kReadTransfer);
    });

    // A transfer's yield returned: the drive reported no error (state 7 to the file system's callback), the
    // transfer is complete, and read applies it.
    own(kReadTransfer, "read transfer", [finish_read, chunk, frame_of](Cpu& c) {
        u32 frame = 0u;
        if (!frame_of(c, c.gpr[1] + kTransferFrame, frame)) return;
        const u32 file = c.mem_read32(frame + rFile);
        const u32 fs = c.mem_read32(file + fFileSystem);
        c.mem_write32(file + fStatus, kStatusError);  // the poll's END case, overwritten below as in the original
        const u32 callback = c.mem_read32(fs + sErrorCallback);
        if (callback != 0u && !call_guest(c, callback, kDriveNoError, c.mem_read32(fs + sErrorData))) return;
        c.gpr[1] = frame;
        s32 delivered = s(c.mem_read32(file + fResult));
        if (delivered < 0) delivered = 0;
        c.mem_write32(file + fStatus, kStatusReady);
        s32 retries = s(c.mem_read32(frame + rRetries));
        if (c.mem_read32(frame + rMode) == kModeDirect) {
            if (delivered > 0) {
                write64(c, file + fPositionHi, read64(c, file + fPositionHi) + delivered);
                c.mem_write32(frame + rRemaining, c.mem_read32(frame + rRemaining) - static_cast<u32>(delivered));
                c.mem_write32(frame + rDestination, c.mem_read32(frame + rDestination) + static_cast<u32>(delivered));
                retries = 0;
                c.mem_write32(file + fCacheValid, 0u);
                c.mem_write32(file + fCacheOffset, 0u);
            } else {
                ++retries;
            }
        } else {
            if (delivered > 0) {
                c.mem_write32(file + fCacheOffset, c.mem_read32(frame + rPositionMod));
                c.mem_write32(file + fCacheValid, 1u);
                retries = 0;
            } else {
                ++retries;
            }
        }
        c.mem_write32(frame + rRetries, static_cast<u32>(retries));
        chunk(c, frame);
        (void)finish_read;
    });

    // ---- open, close, sync, abort ----------------------------------------------------------------------------
    own(kOpen, "open", [this](Cpu& c) {
        const u32 fs = c.gpr[3], file = c.gpr[4], name = c.gpr[5], flags = c.gpr[6];
        const u32 caller = c.gpr[1], frame = caller - kOpenFrame, link = c.lr;
        const auto done = [&](u32 value) {
            c.gpr[1] = caller;
            c.lr = link;
            c.gpr[3] = value;
        };
        c.mem_write32(frame, caller);
        c.mem_write32(caller + 4u, link);
        c.gpr[1] = frame;
        if ((flags & 0xEu) != 0u) return done(4u);  // RTFS_ERROR_WRITEPROTECTED
        // _rtGetFileNameFromDeviceName: the text after the first ':' (the whole name without one).
        std::string given;
        if (!guest_string(c, name, given)) {
            stop(c, "a file name without a NUL");
            return;
        }
        const std::size_t colon = given.find(':');
        const std::string path_part = colon == std::string::npos ? given : given.substr(colon + 1u);
        // The file system's device name (RtGCNFS's own fsGetDeviceName: "", at 0x2A44(r13)).
        if (c.mem_read32(fs + sGetDeviceName) != kGetDeviceNameFunction) {
            stop(c, "a file system whose device-name function is not RtGCNFS's");
            return;
        }
        std::string drive;
        if (!guest_string(c, c.gpr[13] + kDeviceNameOffset, drive)) {
            stop(c, "a device name without a NUL");
            return;
        }
        // _rtFSBuildFileName(buf, 0x100, drive, path, '/'): refused past 0x100 - 1; '/' and '' become '/'.
        if (drive.size() + path_part.size() > kPathBuffer - 1u) return done(5u);  // RTFS_ERROR_FILENAMETOOLONG
        std::string path = drive + path_part;
        for (std::size_t i = drive.size(); i < path.size(); ++i)
            if (path[i] == '\\' || path[i] == '/') path[i] = '/';
        for (std::size_t i = 0; i < path.size(); ++i) c.mem_write8(frame + 8u + static_cast<u32>(i), static_cast<u8>(path[i]));
        c.mem_write8(frame + 8u + static_cast<u32>(path.size()), 0u);
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
        if ((flags & 0x10u) != 0u) {
            c.mem_write32(file + fAsync, 1u);
            c.mem_write32(file + fStatus, kStatusBusy);
        } else {
            c.mem_write32(file + fAsync, 0u);
            c.mem_write32(file + fStatus, kStatusReady);
        }
        c.mem_write32(file + fPriority, 2u);
        done(1u);  // RTFS_ERROR_NOERROR
    });
    own(kClose, "close", [](Cpu& c) {
        c.mem_write32(c.gpr[3] + fStatus, kStatusClosed);
        c.gpr[3] = 1u;  // the original's r3 was DVDClose's TRUE
    });
    // The drive is always ready (drive status 0) and every transfer complete (block END).
    own(kSync, "sync", [](Cpu& c) {
        const u32 file = c.gpr[3];
        const u32 fs = c.mem_read32(file + fFileSystem);
        const u32 global = c.gpr[13] + kGlobalOffset;
        if (!(c.mem_read32(global) == 1u && c.mem_read32(file + fStatus) != kStatusError)) {
            c.mem_write32(fs + sStatus, kFsReady);
            c.mem_write32(global, 1u);
        }
        c.mem_write32(file + fStatus, kStatusReady);
        if (c.mem_read32(file + fAsync) != 0u && c.mem_read32(file + fStatus) == kStatusReady) {
            if (c.mem_read32(file + fOutstandingOp) == kOpRead) {
                c.mem_write32(file + fOutstandingSize, c.mem_read32(file + fPositionLo) - c.mem_read32(file + fInitialLo));
            }
            if (!call_guest(c, kTriggerCallback, file)) return;
        }
        c.gpr[3] = c.mem_read32(file + fStatus);
    });
    own(kAbort, "abort", [](Cpu& c) { c.gpr[3] = 1u; });
}

}  // namespace gekko
