// CRI's "DVD" file device (gcCi), native over the asset namespace. Contract: include/gekko/native_cri_file_device.h
// and docs/DISC_SYSTEM_MAP.md ("Cut 3": the disassembly every entry follows, and the evidence).

#include "gekko/native_cri_file_device.h"

#include <cstdio>
#include <string>

#include "gekko/asset_mount.h"
#include "gekko/cpu.h"
#include "gekko/native_file_api.h"
#include "gekko/recomp.h"

namespace gekko {

namespace {

// ---- the entry points (the interface table 0x80569FC8 holds all but GetInterface)
constexpr u32 kGetInterface = 0x803CEA9Cu;  // returns the table (cvFs calls it through its device record)
constexpr u32 kExecServer = 0x803CE924u;
constexpr u32 kEntryErrFunc = 0x803CE910u;
constexpr u32 kGetFileSize = 0x803CE7ACu;
constexpr u32 kOpen = 0x803CE59Cu;
constexpr u32 kClose = 0x803CE3D8u;
constexpr u32 kSeek = 0x803CE30Cu;
constexpr u32 kTell = 0x803CE2ACu;
constexpr u32 kReqRd = 0x803CDE84u;
constexpr u32 kStopTr = 0x803CDCE4u;
constexpr u32 kGetStat = 0x803CDC80u;
constexpr u32 kGetSctLen = 0x803CDC20u;
constexpr u32 kSetSctLen = 0x803CDB3Cu;
constexpr u32 kGetNumTr = 0x803CDADCu;

const NativeCriFileDevice::Address kAddresses[] = {
    {kGetNumTr, "gcCi GetNumTr", false},     {kSetSctLen, "gcCi SetSctLen", false},
    {kGetSctLen, "gcCi GetSctLen", false},   {kGetStat, "gcCi GetStat", false},
    {kStopTr, "gcCi StopTr", false},         {kReqRd, "gcCi ReqRd", false},
    {kTell, "gcCi Tell", false},             {kSeek, "gcCi Seek", false},
    {kClose, "gcCi Close", false},           {kOpen, "gcCi Open", false},
    {kGetFileSize, "gcCi GetFileSize", false}, {kEntryErrFunc, "gcCi EntryErrFunc", false},
    {kExecServer, "gcCi ExecServer", false}, {kGetInterface, "gcCi_GetInterface", false},
};

// ---- the device's state (guest memory, the original layout)
constexpr u32 kTable = 0x80569FC8u;
constexpr u32 kGlobal = 0x805C1BB8u;
constexpr u32 kLastStatus = kGlobal + 0x00u;
constexpr u32 kDeviceState = kGlobal + 0x04u;  // byte
constexpr u32 kCancelling = kGlobal + 0x08u;
constexpr u32 kErrorObject = kGlobal + 0x0Cu;
constexpr u32 kErrorFunction = kGlobal + 0x10u;
constexpr u32 kPool = kGlobal + 0x14u;
constexpr u32 kHandles = 40u;
constexpr u32 kHandleSize = 0x64u;
constexpr u32 kPrefix = 0x805C2B70u;
constexpr u32 kPrefixSize = 0x100u;
constexpr u32 kSyncReadFlag = 0x8056A030u;  // set by the ADX init (0x803CEB08): DVDReadPrio instead of the async read

// One handle.
constexpr u32 hUsed = 0x00u, hState = 0x02u, hBuffer = 0x08u, hStatus = 0x0Cu, hSectorLength = 0x10u;
constexpr u32 hLength = 0x14u, hSectors = 0x18u, hPosition = 0x1Cu, hTransferred = 0x20u, hRequested = 0x24u;
constexpr u32 hToken = 0x58u, hFileLength = 0x5Cu;
constexpr u8 kIdle = 0u, kDone = 1u, kReading = 2u;
constexpr s32 kStatusEnd = 0, kStatusCanceled = 10;

// The original's error messages, 0x80518A40 + n (the reporter gets their address).
constexpr u32 kMessages = 0x80518A40u;
constexpr u32 mNullHandle = 0x2Cu, mSetSctLenHandle = 0x44u, mSctLenAlign = 0x5Cu, mGetSctLenHandle = 0x78u;
constexpr u32 mCancelFailed = 0x90u, mCancelTimeout = 0xACu, mNegative = 0xCCu, mNullBuffer = 0xECu;
constexpr u32 mOpenName = 0x110u, mOpenMode = 0x134u, mOpenFull = 0x158u, mOpenFailed = 0x188u;
constexpr u32 mSizeName = 0x1ACu, mSizeOpenFailed = 0x1D8u;

constexpr u32 kPathBuffer = 0x104u;  // the original's stack buffer for prefix + name
constexpr u32 kMaxGuestString = 4096u;
constexpr u32 kDefaultSectorLength = 0x800u;

void stop(Cpu& c, const std::string& why) {
    c.request_stop(StopReason::ExecutionFault, "native CRI file device: " + why);
}

bool guest_string(Cpu& c, u32 address, std::string& out) {
    out.clear();
    for (u32 i = 0u; i < kMaxGuestString; ++i) {
        const u8 byte = c.mem_read8(address + i);
        if (byte == 0u) return true;
        out.push_back(static_cast<char>(byte));
    }
    return false;
}

// A guest call returning here; the owner's own caller (LR) is left as it was.
bool call_guest(Cpu& c, u32 function, u32 r3, u32 r4, u32 r5) {
    const u32 link = c.lr;
    c.gpr[3] = r3;
    c.gpr[4] = r4;
    c.gpr[5] = r5;
    c.gpr[12] = function;
    c.ctr = function;
    recomp::call_indirect(c, function, link);
    if (c.exiting()) c.finish_transfers();
    if (c.exiting() || c.pc != link) {
        stop(c, "the error reporter did not return");
        return false;
    }
    c.lr = link;
    return true;
}

s32 s(u32 v) noexcept { return static_cast<s32>(v); }

}  // namespace

const NativeCriFileDevice::Address* NativeCriFileDevice::addresses(std::size_t& count) noexcept {
    count = sizeof(kAddresses) / sizeof(kAddresses[0]);
    return kAddresses;
}

void NativeCriFileDevice::install() {
    Cpu& cpu = cpu_;
    const auto own = [&cpu](u32 address, const char* name, auto&& serve) {
        cpu.install_hle(address, [serve](Cpu& c) {
            serve(c);
            return true;  // an owner never declines
        }, std::string("native CRI file device: ") + name);
    };
    // The registered reporter, as the original called it: (object, message, handle or 0).
    const auto report = [this](Cpu& c, u32 message, u32 argument) {
        const u32 function = c.mem_read32(kErrorFunction);
        if (function == 0u) return true;
        ++stats_.errors;
        return call_guest(c, function, c.mem_read32(kErrorObject), kMessages + message, argument);
    };
    // prefix + name with '\' turned into '/', the path the original opened. False after stopping.
    const auto path_of = [](Cpu& c, u32 name, std::string& path) {
        std::string prefix, tail;
        if (!guest_string(c, kPrefix, prefix) || !guest_string(c, name, tail)) {
            stop(c, "a path without a NUL");
            return false;
        }
        path = prefix + tail;
        if (path.size() + 1u > kPathBuffer) {
            stop(c, "'" + path + "' overruns the original's 0x104-byte path buffer");
            return false;
        }
        for (char& ch : path)
            if (ch == '\\') ch = '/';
        return true;
    };
    const auto file_of = [this](const std::string& path, u32& token, u32& length) {
        const assets::AssetNamespace& names = assets_.names();
        const s32 entry = names.resolve(path);
        if (entry < 0 || names.is_directory(static_cast<u32>(entry))) return false;
        const u64 size = names.entry(static_cast<u32>(entry)).size;
        if (size > 0xFFFFFFFFull) return false;
        token = assets_.token_of(static_cast<u32>(entry));
        length = static_cast<u32>(size);
        return true;
    };
    // The server's completion of one reading handle (the read itself was delivered when it was requested).
    const auto complete = [this](Cpu& c, u32 h) {
        c.mem_write32(h + hStatus, static_cast<u32>(kStatusEnd));
        c.mem_write32(kLastStatus, static_cast<u32>(kStatusEnd));
        const u32 sector = c.mem_read32(h + hSectorLength);
        const u32 bytes = c.mem_read32(h + hRequested) * sector;
        c.mem_write32(h + hTransferred, bytes);
        c.mem_write32(h + hPosition, c.mem_read32(h + hPosition) + c.mem_read32(h + hRequested));
        const s32 end = s(c.mem_read32(h + hPosition) * sector);
        const s32 length = s(c.mem_read32(h + hLength));
        if (end > length) {
            const u32 excess = static_cast<u32>(end - length);
            const u32 from = c.mem_read32(h + hBuffer) + bytes - excess;
            for (u32 i = 0u; i < excess; ++i) c.mem_write8(from + i, 0u);
        }
        c.mem_write8(h + hState, kDone);
        c.mem_write8(kDeviceState, kDone);
        ++stats_.completions;
    };
    // StopTr's and Close's cancel: nothing is in flight, so only the bookkeeping of a stopped transfer remains.
    // A handle whose last status is not END / CANCELED was in error, which this device never leaves.
    const auto cancel = [this, report](Cpu& c, u32 h) {
        const u8 state = c.mem_read8(h + hState);
        if (state == kDone || state == kIdle) return true;
        ++stats_.stops;
        c.mem_write32(kCancelling, 0u);
        const s32 status = s(c.mem_read32(h + hStatus));
        if (status != kStatusEnd && status != kStatusCanceled) {
            stop(c, "a stop of a handle in error (the original spun two seconds and reported a timeout)");
            (void)report;
            return false;
        }
        c.mem_write8(h + hState, kIdle);
        c.mem_write8(kDeviceState, kIdle);
        return true;
    };

    own(kGetInterface, "GetInterface", [](Cpu& c) {
        for (u32 i = 0u; i < kPrefixSize; ++i) c.mem_write8(kPrefix + i, 0u);
        c.mem_write32(kErrorFunction, 0u);
        c.mem_write32(kErrorObject, 0u);
        for (u32 i = 0u; i < 12u; i += 4u) c.mem_write32(kGlobal + i, 0u);
        c.gpr[3] = kTable;
    });
    own(kEntryErrFunc, "EntryErrFunc", [](Cpu& c) {
        c.mem_write32(kErrorFunction, c.gpr[3]);
        c.mem_write32(kErrorObject, c.gpr[4]);
    });
    own(kGetFileSize, "GetFileSize", [report, path_of, file_of](Cpu& c) {
        const u32 name = c.gpr[3];
        if (name == 0u) {
            if (report(c, mSizeName, 0u)) c.gpr[3] = 0u;
            return;
        }
        std::string path;
        if (!path_of(c, name, path)) return;
        u32 token = 0u, length = 0u;
        if (!file_of(path, token, length)) {
            if (report(c, mSizeOpenFailed, 0u)) c.gpr[3] = 0u;
            return;
        }
        c.gpr[3] = (length & 0x80000000u) != 0u ? 0x7FFFFFFFu : length;
    });
    own(kOpen, "Open", [this, report, path_of, file_of](Cpu& c) {
        const u32 name = c.gpr[3], mode = c.gpr[5];
        if (name == 0u) {
            if (report(c, mOpenName, 0u)) c.gpr[3] = 0u;
            return;
        }
        if (mode != 0u) {
            if (report(c, mOpenMode, 0u)) c.gpr[3] = 0u;
            return;
        }
        u32 h = 0u;
        for (u32 i = 0u; i < kHandles; ++i) {
            if (c.mem_read8(kPool + i * kHandleSize + hUsed) == 0u) {
                h = kPool + i * kHandleSize;
                break;
            }
        }
        if (h == 0u) {
            if (report(c, mOpenFull, 0u)) c.gpr[3] = 0u;
            return;
        }
        std::string path;
        if (!path_of(c, name, path)) return;
        ++stats_.opens;
        u32 token = 0u, length = 0u;
        if (!file_of(path, token, length)) {
            ++stats_.open_misses;
            std::printf("CRI file device: '%s' was not found under /.\n", path.c_str());
            if (!report(c, mOpenFailed, 0u)) return;
            for (u32 i = 0u; i < kHandleSize; i += 4u) c.mem_write32(h + i, 0u);
            c.gpr[3] = 0u;
            return;
        }
        c.mem_write32(h + hToken, token);
        c.mem_write32(h + hFileLength, length);
        c.mem_write32(h + hSectorLength, kDefaultSectorLength);
        const u32 clamped = (length & 0x80000000u) != 0u ? 0x7FFFFFFFu : length;
        c.mem_write32(h + hLength, clamped);
        c.mem_write32(h + hSectors, static_cast<u32>(s(kDefaultSectorLength + clamped - 1u) / s(kDefaultSectorLength)));
        c.mem_write32(h + hPosition, 0u);
        c.mem_write32(h + hBuffer, 0u);
        c.mem_write32(h + hRequested, 0u);
        c.mem_write32(h + hTransferred, 0u);
        c.mem_write8(h + hState, kIdle);
        c.mem_write8(h + hUsed, 1u);
        c.gpr[3] = h;
    });
    own(kClose, "Close", [cancel](Cpu& c) {
        const u32 h = c.gpr[3];
        if (h == 0u) return;
        if (!cancel(c, h)) return;
        for (u32 i = 0u; i < kHandleSize; i += 4u) c.mem_write32(h + i, 0u);
    });
    own(kSeek, "Seek", [report](Cpu& c) {
        const u32 h = c.gpr[3], whence = c.gpr[5];
        const s32 offset = s(c.gpr[4]);
        if (h == 0u) {
            if (report(c, mNullHandle, 0u)) c.gpr[3] = 0u;
            return;
        }
        s32 position = s(c.mem_read32(h + hPosition));
        const s32 sectors = s(c.mem_read32(h + hSectors));
        if (whence == 0u) position = offset;
        else if (whence == 2u) position = sectors + offset;
        else if (whence == 1u) position = position + offset;
        if (position > sectors) position = sectors;
        if (position <= 0) position = 0;
        c.mem_write32(h + hPosition, static_cast<u32>(position));
        c.gpr[3] = static_cast<u32>(position);
    });
    own(kTell, "Tell", [report](Cpu& c) {
        const u32 h = c.gpr[3];
        if (h == 0u) {
            if (report(c, mNullHandle, 0u)) c.gpr[3] = 0u;
            return;
        }
        c.gpr[3] = c.mem_read32(h + hPosition);
    });
    own(kReqRd, "ReqRd", [this, report](Cpu& c) {
        const u32 h = c.gpr[3], buffer = c.gpr[5];
        const s32 sectors = s(c.gpr[4]);
        if (h == 0u) {
            if (report(c, mNullHandle, 0u)) c.gpr[3] = 0u;
            return;
        }
        if (sectors < 0) {
            if (report(c, mNegative, h)) c.gpr[3] = 0u;
            return;
        }
        if (buffer == 0u) {
            if (report(c, mNullBuffer, h)) c.gpr[3] = 0u;
            return;
        }
        const u8 state = c.mem_read8(h + hState);
        const s32 status = s(c.mem_read32(h + hStatus));
        if ((state != kDone && state != kIdle) || (status != kStatusEnd && status != kStatusCanceled)) {
            c.gpr[3] = 0u;
            return;
        }
        // One transfer at a time across the device.
        for (u32 i = 0u; i < kHandles; ++i) {
            const u32 other = kPool + i * kHandleSize;
            if (c.mem_read8(other + hUsed) == 1u && c.mem_read8(other + hState) == kReading) {
                c.gpr[3] = 0u;
                return;
            }
        }
        if (sectors == 0) {
            c.mem_write8(h + hState, kDone);
            c.mem_write8(kDeviceState, kDone);
            c.gpr[3] = 0u;
            return;
        }
        c.mem_write32(h + hTransferred, 0u);
        c.mem_write32(h + hBuffer, buffer);
        c.mem_write32(h + hRequested, static_cast<u32>(sectors));
        // (The original's server pass here finds no reading handle: the loop above refused one.)
        const u32 sector = c.mem_read32(h + hSectorLength);
        const s32 offset = s(c.mem_read32(h + hPosition) * sector);
        s32 length = s(static_cast<u32>(sectors) * sector);
        const s32 size = s(c.mem_read32(h + hLength));
        if (offset + length > size) {
            length = size - offset;
            if (length < 0) {
                c.mem_write8(h + hState, kDone);
                c.mem_write8(kDeviceState, kDone);
                c.gpr[3] = static_cast<u32>(sectors);
                return;
            }
        }
        const u32 aligned = (static_cast<u32>(length) + 31u) & ~31u;
        if (c.mem_read32(kSyncReadFlag) != 0u && aligned == 0u) {
            c.gpr[3] = 0u;  // DVDReadPrio of nothing returned 0 bytes, which the original took as a failure
            return;
        }
        if (!files_.device_read(c, c.mem_read32(h + hToken) + static_cast<u32>(offset), buffer, aligned)) {
            std::string why = "a read of " + std::to_string(aligned) + " bytes at " + std::to_string(offset) +
                              " could not be delivered";
            stop(c, why);
            return;
        }
        ++stats_.requests;
        stats_.bytes += aligned;
        c.mem_write8(h + hState, kReading);
        c.mem_write8(kDeviceState, kReading);
        c.gpr[3] = c.mem_read32(h + hRequested);
    });
    own(kStopTr, "StopTr", [report, cancel](Cpu& c) {
        const u32 h = c.gpr[3];
        if (h == 0u) {
            report(c, mNullHandle, 0u);
            return;
        }
        cancel(c, h);
    });
    own(kGetStat, "GetStat", [report](Cpu& c) {
        const u32 h = c.gpr[3];
        if (h == 0u) {
            if (report(c, mNullHandle, 0u)) c.gpr[3] = 0u;
            return;
        }
        c.gpr[3] = static_cast<u32>(static_cast<s32>(static_cast<s8>(c.mem_read8(h + hState))));
    });
    own(kGetSctLen, "GetSctLen", [report](Cpu& c) {
        const u32 h = c.gpr[3];
        if (h == 0u) {
            if (report(c, mGetSctLenHandle, 0u)) c.gpr[3] = 0u;
            return;
        }
        c.gpr[3] = c.mem_read32(h + hSectorLength);
    });
    own(kSetSctLen, "SetSctLen", [report](Cpu& c) {
        const u32 h = c.gpr[3], next = c.gpr[4];
        if (h == 0u) {
            report(c, mSetSctLenHandle, 0u);
            return;
        }
        const s32 previous = s(c.mem_read32(h + hSectorLength));
        if (previous % 32 != 0) {  // the original tests the CURRENT length
            report(c, mSctLenAlign, 0u);
            return;
        }
        if (s(next) == 0) {
            stop(c, "SetSctLen(0): the original divides by it");
            return;
        }
        const s32 bytes = s(c.mem_read32(h + hPosition)) * previous;
        c.mem_write32(h + hSectorLength, next);
        c.mem_write32(h + hSectors, static_cast<u32>(s(next + c.mem_read32(h + hLength) - 1u) / s(next)));
        c.mem_write32(h + hPosition, static_cast<u32>(bytes / s(next)));
        c.mem_write32(h + hTransferred, c.mem_read32(h + hRequested) * next);
    });
    own(kGetNumTr, "GetNumTr", [report](Cpu& c) {
        const u32 h = c.gpr[3];
        if (h == 0u) {
            if (report(c, mNullHandle, 0u)) c.gpr[3] = 0u;
            return;
        }
        c.gpr[3] = c.mem_read32(h + hTransferred);
    });
    own(kExecServer, "ExecServer", [complete](Cpu& c) {
        for (u32 i = 0u; i < kHandles; ++i) {
            const u32 h = kPool + i * kHandleSize;
            if (c.mem_read8(h + hUsed) == 1u && c.mem_read8(h + hState) == kReading) complete(c, h);
        }
    });
}

}  // namespace gekko
