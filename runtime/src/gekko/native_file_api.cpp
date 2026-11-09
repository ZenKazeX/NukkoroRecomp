// The game's file API, served natively. Contract and rationale:
// include/gekko/native_file_api.h, docs/native-asset-pipeline-20261005/README.md.

#include "gekko/native_file_api.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

#include "gekko/asset_trace.h"
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_assets.h"
#include "gekko/profiling.h"
#include "gekko/recomp.h"

namespace gekko {

namespace {

// ---- the entry points the generator routes here (extract/native-owners.txt)
//
// Each is the start of the SDK function the game, RenderWare, CRI, the OS or
// a data word still names; nothing below them survives in the image.
constexpr u32 kGetDriveConfig = 0x80371768u;   // __OSGetDIConfig (EXI's devkit test)
constexpr u32 kOpen = 0x8037AAA0u;             // DVDOpen
constexpr u32 kClose = 0x8037AB68u;            // DVDClose
constexpr u32 kReadAsync = 0x8037AF04u;        // DVDReadAsyncPrio
constexpr u32 kReadSync = 0x8037AFF4u;         // DVDReadPrio
constexpr u32 kTransferred = 0x8037B294u;      // DVDGetTransferredSize
constexpr u32 kNoOpCallback = 0x8037B300u;     // the SDK's default no-op callback (a data word names it)
constexpr u32 kInit = 0x8037B304u;             // DVDInit
constexpr u32 kReadAbs = 0x8037CF40u;          // DVDReadAbsAsyncPrio
constexpr u32 kReadDiskId = 0x8037D0ECu;       // DVDReadDiskID
constexpr u32 kCancelStream = 0x8037D1C0u;     // DVDCancelStreamAsync (console reboot only)
constexpr u32 kInquiry = 0x8037D27Cu;          // DVDInquiryAsync
constexpr u32 kBlockStatus = 0x8037D390u;      // DVDGetCommandBlockStatus
constexpr u32 kDriveStatus = 0x8037D3DCu;      // DVDGetDriveStatus
constexpr u32 kAutoInvalidation = 0x8037D488u; // DVDSetAutoInvalidation
constexpr u32 kResume = 0x8037D498u;           // DVDResume
constexpr u32 kCancelAsync = 0x8037D4E8u;      // DVDCancelAsync
constexpr u32 kCancel = 0x8037D764u;           // DVDCancel
constexpr u32 kDiskId = 0x8037D834u;           // DVDGetCurrentDiskID
constexpr u32 kCheckDisk = 0x8037D83Cu;        // DVDCheckDisk
constexpr u32 kPrepareReset = 0x8037D934u;     // __DVDPrepareResetAsync (console reboot only)
constexpr u32 kTestAlarm = 0x8037DA50u;        // __DVDTestAlarm
constexpr u32 kAutoFatal = 0x8037DF9Cu;        // DVDSetAutoFatalMessaging

// Where every callback the API makes returns: an entry inside DVDReadAbsAsyncPrio's
// row, so the generator gives it a thunk of its own.
constexpr u32 kCallbackReturn = 0x8037CF44u;
constexpr u32 kCallbackFrameTag = 0x4E464342u;  // 'NFCB'
constexpr u32 kCallbackFrameSize = 64u;

// Guest structure layouts the API defines (CONTRACTS.md): the command block,
// and DVDFileInfo = command block + startAddr/length/callback.
constexpr u32 kBlockCommand = 0x08u;
constexpr u32 kBlockState = 0x0Cu;
constexpr u32 kBlockOffset = 0x10u;
constexpr u32 kBlockLength = 0x14u;
constexpr u32 kBlockAddress = 0x18u;
constexpr u32 kBlockCurrentTransfer = 0x1Cu;
constexpr u32 kBlockTransferred = 0x20u;
constexpr u32 kBlockCallback = 0x28u;
constexpr u32 kFileStart = 0x30u;
constexpr u32 kFileLength = 0x34u;
constexpr u32 kFileCallback = 0x38u;
constexpr u32 kBlockSize = 0x30u;

constexpr s32 kStateFatal = -1;
constexpr s32 kStateEnd = 0;
constexpr s32 kStateCoverClosed = 3;
constexpr s32 kStateCanceled = 10;

constexpr u32 kCommandRead = 1u;
constexpr u32 kCommandReadDiskId = 5u;
constexpr u32 kCommandInquiry = 14u;

constexpr u32 kDiskIdAddress = 0x80000000u;  // the title block the boot leaves at the base of MEM1
constexpr u32 kMaxGuestPath = 4096u;
constexpr std::size_t kMissedPathsKept = 64u;

const NativeFileApi::Address kAddresses[] = {
    {kGetDriveConfig, "__OSGetDIConfig", false},
    {kOpen, "DVDOpen", false},
    {kClose, "DVDClose", false},
    {kReadAsync, "DVDReadAsyncPrio", false},
    {kReadSync, "DVDReadPrio", false},
    {kTransferred, "DVDGetTransferredSize", false},
    {kNoOpCallback, "DVD default no-op callback", false},
    {kInit, "DVDInit", false},
    {kReadAbs, "DVDReadAbsAsyncPrio", false},
    {kReadDiskId, "DVDReadDiskID", false},
    {kCancelStream, "DVDCancelStreamAsync", false},
    {kInquiry, "DVDInquiryAsync", false},
    {kBlockStatus, "DVDGetCommandBlockStatus", false},
    {kDriveStatus, "DVDGetDriveStatus", false},
    {kAutoInvalidation, "DVDSetAutoInvalidation", false},
    {kResume, "DVDResume", false},
    {kCancelAsync, "DVDCancelAsync", false},
    {kCancel, "DVDCancel", false},
    {kDiskId, "DVDGetCurrentDiskID", false},
    {kCheckDisk, "DVDCheckDisk", false},
    {kPrepareReset, "__DVDPrepareResetAsync", false},
    {kTestAlarm, "__DVDTestAlarm", false},
    {kAutoFatal, "DVDSetAutoFatalMessaging", false},
    {kCallbackReturn, "file API callback return", true},
};

// A guest span that is one contiguous run of the game's mapped memory (a
// zero-length span still needs its first byte mapped).
bool ram_span(GuestMemory& memory, u32 address, u32 size) {
    if (address == 0u) return false;
    return memory.mapped(GcAddr(address), size == 0u ? 1u : size);
}

// A callback the API may enter: translated code at a word-aligned address, and
// room on the guest stack for the continuation frame.
bool callback_ready(Cpu& c, GuestMemory& memory, u32 callback) {
    if (callback == 0u) return true;
    return (callback & 3u) == 0u && c.recompiled() != nullptr &&
           (c.recompiled()->lookup(callback) != nullptr ||
            c.recompiled()->find_range(callback) != nullptr) &&
           c.gpr[1] >= kCallbackFrameSize &&
           ram_span(memory, c.gpr[1] - kCallbackFrameSize, kCallbackFrameSize);
}

// callback(result, block) on the game's behalf. The caller's LR and the value
// the entry point returns ride in a tagged frame on the guest stack, so a
// savestate taken inside the callback resumes; the callback returns to
// kCallbackReturn, whose owner pops the frame.
void call_back(Cpu& c, u32 callback, u32 result, u32 block, u32 return_value) {
    const u32 stack = c.gpr[1];
    const u32 frame = stack - kCallbackFrameSize;
    c.mem_write32(frame, stack);
    c.mem_write32(frame + 48u, kCallbackFrameTag);
    c.mem_write32(frame + 52u, c.lr);
    c.mem_write32(frame + 56u, return_value);
    c.gpr[1] = frame;
    c.gpr[3] = result;
    c.gpr[4] = block;
    c.lr = kCallbackReturn;
    c.leave_to_dispatcher(callback, FallbackReason::Yield, kCallbackReturn);
}

bool callback_return(Cpu& c) {
    GEKKO_PROFILE_MESSAGE_LITERAL("file/callback-return");
    if (c.mem_read32(c.gpr[1] + 48u) != kCallbackFrameTag) {
        c.request_stop(StopReason::ExecutionFault, "file API callback returned without its frame");
        return true;
    }
    c.lr = c.mem_read32(c.gpr[1] + 52u);
    c.gpr[3] = c.mem_read32(c.gpr[1] + 56u);
    c.gpr[1] += kCallbackFrameSize;
    return true;
}

// The guest's path string, up to its NUL. False when there is none within
// kMaxGuestPath bytes (the SDK would walk on through whatever followed).
bool guest_path(Cpu& c, u32 address, std::string& out) {
    out.clear();
    for (u32 i = 0u; i < kMaxGuestPath; ++i) {
        const u8 byte = c.mem_read8(address + i);
        if (byte == 0u) return true;
        out.push_back(static_cast<char>(byte));
    }
    return false;
}

std::string hex(u32 value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08X", value);
    return text;
}

void stop(Cpu& c, const char* entry, const std::string& why) {
    c.request_stop(StopReason::ExecutionFault, std::string("file API ") + entry + ": " + why);
}

// The read entry points' own argument check (the SDK's dvdfs.c asserts at
// 0x2EE/0x2F4 and 0x334/0x33A, which OSPanic): an offset inside the file and
// an end below its length + 32. False after stopping the machine.
bool read_in_file(Cpu& c, const char* entry, u32 info, u32 length, u32 offset) {
    const u32 size = c.mem_read32(info + kFileLength);
    const u32 end = offset + length;
    if (static_cast<s32>(offset) < 0 || offset > size || static_cast<s32>(end) < 0 ||
        !(end < size + 0x20u)) {
        stop(c, entry, "read of " + hex(length) + " bytes at " + hex(offset) + " outside a file of " +
                           hex(size) + " bytes (the SDK asserts this and halts)");
        return false;
    }
    return true;
}

}  // namespace

const NativeFileApi::Address* NativeFileApi::addresses(std::size_t& count) noexcept {
    count = sizeof(kAddresses) / sizeof(kAddresses[0]);
    return kAddresses;
}

void NativeFileApi::count_read_time(u64 ns) noexcept {
    stats_.read_ns += ns;
    if (ns > stats_.read_ns_max) stats_.read_ns_max = ns;
}

NativeFileApi::ReadOutcome NativeFileApi::read(Cpu& c, u32 block, u32 address, u32 length, u32 token,
                                               u32 callback, u32 command, Source source,
                                               u32 return_value) {
    GEKKO_PROFILE_ZONE("file/read");
    GEKKO_PROFILE_VALUE(length);
    const auto started = std::chrono::steady_clock::now();
    ++stats_.reads;
    ReadOutcome outcome;
    const bool block_ok = ram_span(memory_, block, kBlockSize);
    const bool destination_ok = block_ok && ram_span(memory_, address, length);
    if (!block_ok || !destination_ok || !callback_ready(c, memory_, callback)) {
        // A malformed request completes FATAL, where the caller can see it;
        // the callback (when it can be entered) is told -1.
        ++stats_.reads_refused;
        GEKKO_PROFILE_MESSAGE_LITERAL("file/read-refused");
        outcome.refused = true;
        if (block_ok) {
            c.mem_write32(block + kBlockState, static_cast<u32>(kStateFatal));
            c.mem_write32(block + kBlockTransferred, 0u);
            if (callback != 0u && callback_ready(c, memory_, callback)) {
                ++stats_.callbacks;
                call_back(c, callback, 0xFFFFFFFFu, block, return_value);
                outcome.called_back = true;
            }
        }
        return outcome;
    }

    bool ok = false;
    std::string resolved;
    u64 relative = 0;
    if (source == Source::Identity) {
        // The title block: served directly, it is not an asset.
        ++stats_.identity_reads;
        const std::vector<u8>& identity = assets_.identity_block();
        memory_.write_in_place(address, length, [&](u8* destination) {
            std::fill_n(destination, length, u8{0});
            const std::size_t available = std::min<std::size_t>(identity.size(), length);
            if (available != 0u) std::copy_n(identity.begin(), available, destination);
        });
        resolved = "<title>";
        ok = !identity.empty();
    } else {
        ok = copy_asset(token, address, length, resolved, relative);
    }
    if (!ok) {
        ++stats_.reads_failed;
        GEKKO_PROFILE_MESSAGE_LITERAL("file/read-failed");
    } else {
        stats_.read_bytes += length;
    }
    // The block's completion fields, as its callers read them.
    c.mem_write32(block + kBlockCommand, command);
    c.mem_write32(block + kBlockOffset, token);
    c.mem_write32(block + kBlockLength, length);
    c.mem_write32(block + kBlockAddress, address);
    c.mem_write32(block + kBlockCurrentTransfer, ok ? length : 0u);
    c.mem_write32(block + kBlockCallback, callback);
    if (AssetTracer* tracer = AssetTracer::active()) {
        tracer->file_read(c.pc & ~3u, c.lr & ~3u, AssetTracer::call_chain_of(&c), token, length, address,
                          resolved, relative, ok ? memory_.peek_pointer(address, length) : nullptr,
                          ok);
    }
    if (NativeAssetManager* manager = NativeAssetManager::active();
        ok && length != 0u && source == Source::Asset && manager != nullptr) {
        manager->notify_file(resolved);
    }
    c.mem_write32(block + kBlockState, static_cast<u32>(ok ? kStateEnd : kStateFatal));
    c.mem_write32(block + kBlockTransferred, ok ? length : 0u);
    count_read_time(static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now() - started)
                                         .count()));
    outcome.result = ok ? static_cast<s32>(length) : -1;
    if (callback != 0u) {
        ++stats_.callbacks;
        call_back(c, callback, static_cast<u32>(outcome.result), block, return_value);
        outcome.called_back = true;
    }
    return outcome;
}

bool NativeFileApi::copy_asset(u32 token, u32 address, u32 length, std::string& resolved, u64& relative) {
    // A zero-length read delivers nothing and succeeds: the SDK's own bounds check admits it (offset <= length,
    // end < length + 32) and the drive completes it. (Until 2026-10-29 it reached GuestMemory::write_in_place,
    // which resolves no span of size 0, and completed FATAL - a defect no recorded tape had reached.)
    if (length == 0u) {
        resolved.clear();
        relative = 0u;
        return true;
    }
    bool ok = false;
    assets::AssetSystem::Located located;
    switch (assets_.locate(token, length, located)) {
        case assets::AssetSystem::ReadStatus::Ok: {
            u64 got = 0u;
            memory_.write_in_place(address, length, [&](u8* destination) {
                ok = assets_.copy(located, destination, got);
            });
            break;
        }
        case assets::AssetSystem::ReadStatus::Unmapped:
            ++stats_.reads_unmapped;
            break;
        case assets::AssetSystem::ReadStatus::OutOfRange:
            ++stats_.reads_out_of_range;
            break;
        case assets::AssetSystem::ReadStatus::Failed:
            break;
    }
    resolved = assets_.last_path();
    relative = assets_.last_offset();
    if (ok && read_observer_) read_observer_(located.id, resolved, relative, address, length);
    return ok;
}

bool NativeFileApi::device_read(Cpu& c, u32 token, u32 address, u32 length) {
    GEKKO_PROFILE_ZONE("file/device-read");
    const auto started = std::chrono::steady_clock::now();
    ++stats_.reads;
    if (!ram_span(memory_, address, length)) {
        ++stats_.reads_refused;
        return false;
    }
    std::string resolved;
    u64 relative = 0;
    const bool ok = copy_asset(token, address, length, resolved, relative);
    if (!ok) {
        ++stats_.reads_failed;
    } else {
        stats_.read_bytes += length;
    }
    if (AssetTracer* tracer = AssetTracer::active()) {
        tracer->file_read(c.pc & ~3u, c.lr & ~3u, AssetTracer::call_chain_of(&c), token, length, address,
                          resolved, relative, ok ? memory_.peek_pointer(address, length) : nullptr, ok);
    }
    if (NativeAssetManager* manager = NativeAssetManager::active(); ok && length != 0u && manager != nullptr) {
        manager->notify_file(resolved);
    }
    count_read_time(static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now() - started)
                                         .count()));
    return ok;
}

void NativeFileApi::install() {
    Cpu& cpu = cpu_;
    const auto own = [&cpu](u32 address, const char* name, auto&& serve) {
        cpu.install_hle(address, [serve](Cpu& c) {
            serve(c);
            return true;  // an owner never declines
        }, std::string("file API ") + name);
    };
    const assets::AssetNamespace& names = assets_.names();

    own(kCallbackReturn, "callback return", [](Cpu& c) { callback_return(c); });

    // ---- configuration the OS and the boot ask for ---------------------------
    // The DI configuration register, read only by EXI's development-hardware
    // test (0x803B4124: "== 0xFF"): retail.
    own(kGetDriveConfig, "__OSGetDIConfig", [](Cpu& c) { c.gpr[3] = 0u; });
    // Nothing to initialise: no queue, no drive, no interrupt handler.
    own(kInit, "DVDInit", [](Cpu&) {});
    own(kNoOpCallback, "default no-op callback", [](Cpu&) {});
    own(kAutoFatal, "DVDSetAutoFatalMessaging", [this](Cpu& c) {
        const bool previous = auto_fatal_messaging_;
        auto_fatal_messaging_ = c.gpr[3] != 0u;
        c.gpr[3] = previous ? 1u : 0u;
    });
    own(kAutoInvalidation, "DVDSetAutoInvalidation", [this](Cpu& c) {
        const bool previous = auto_invalidation_;
        auto_invalidation_ = c.gpr[3] != 0u;
        c.gpr[3] = previous ? 1u : 0u;
    });
    own(kDiskId, "DVDGetCurrentDiskID", [](Cpu& c) { c.gpr[3] = kDiskIdAddress; });

    // ---- the namespace ------------------------------------------------------
    own(kOpen, "DVDOpen", [this, &names](Cpu& c) {
        GEKKO_PROFILE_ZONE("file/open");
        ++stats_.opens;
        const u32 info = c.gpr[4];
        std::string path;
        const bool terminated = guest_path(c, c.gpr[3], path);
        const s32 entry = terminated ? names.resolve(path) : assets::AssetNamespace::kNotFound;
        if (entry < 0) {
            ++stats_.open_misses;
            if (missed_.size() < kMissedPathsKept) missed_.push_back(path);
            std::printf("File API: DVDOpen(): file '%s' was not found under /.\n", path.c_str());
            if (AssetTracer* tracer = AssetTracer::active())
                tracer->note("file-open-miss", path);
            c.gpr[3] = 0u;
            return;
        }
        const u32 index = static_cast<u32>(entry);
        if (names.is_directory(index)) {
            ++stats_.open_directories;
            c.gpr[3] = 0u;
            return;
        }
        const u64 size = names.entry(index).size;
        if (size > 0xFFFFFFFFull) {
            stop(c, "DVDOpen", "'" + path + "' is larger than DVDFileInfo.length can describe");
            return;
        }
        c.mem_write32(info + kFileStart, assets_.token_of(index));
        c.mem_write32(info + kFileLength, static_cast<u32>(size));
        c.mem_write32(info + kFileCallback, 0u);
        c.mem_write32(info + kBlockState, static_cast<u32>(kStateEnd));
        c.gpr[3] = 1u;
    });
    // ---- reads ----------------------------------------------------------------
    own(kReadAsync, "DVDReadAsyncPrio", [this](Cpu& c) {
        const u32 info = c.gpr[3], address = c.gpr[4], length = c.gpr[5], offset = c.gpr[6];
        const u32 callback = c.gpr[7];
        if (!read_in_file(c, "DVDReadAsyncPrio", info, length, offset)) return;
        c.mem_write32(info + kFileCallback, callback);
        // TRUE whatever happened: the result reaches the caller through the
        // block and the callback.
        const ReadOutcome outcome = read(c, info, address, length, c.mem_read32(info + kFileStart) + offset,
                                         callback, kCommandRead, Source::Asset, 1u);
        if (!outcome.called_back) c.gpr[3] = 1u;
    });
    own(kReadSync, "DVDReadPrio", [this](Cpu& c) {
        const u32 info = c.gpr[3], address = c.gpr[4], length = c.gpr[5], offset = c.gpr[6];
        if (!read_in_file(c, "DVDReadPrio", info, length, offset)) return;
        const ReadOutcome outcome = read(c, info, address, length, c.mem_read32(info + kFileStart) + offset,
                                         0u, kCommandRead, Source::Asset, 0u);
        c.gpr[3] = static_cast<u32>(outcome.result);  // bytes transferred, or -1
    });
    own(kReadAbs, "DVDReadAbsAsyncPrio", [this](Cpu& c) {
        const u32 block = c.gpr[3], address = c.gpr[4], length = c.gpr[5], token = c.gpr[6];
        const u32 callback = c.gpr[7];
        if (block == 0u || address == 0u) {
            ++stats_.reads_refused;
            if (block != 0u && ram_span(memory_, block, kBlockSize)) {
                c.mem_write32(block + kBlockState, static_cast<u32>(kStateFatal));
                c.mem_write32(block + kBlockTransferred, 0u);
            }
            c.gpr[3] = 0u;
            return;
        }
        // Issued (1) unless malformed (0); a callback returns 1 itself.
        const ReadOutcome outcome =
            read(c, block, address, length, token, callback, kCommandRead, Source::Asset, 1u);
        if (!outcome.called_back) c.gpr[3] = outcome.refused ? 0u : 1u;
    });
    own(kReadDiskId, "DVDReadDiskID", [this](Cpu& c) {
        const ReadOutcome outcome = read(c, c.gpr[3], c.gpr[4], 0x20u, 0u, c.gpr[5], kCommandReadDiskId,
                                         Source::Identity, 1u);
        if (!outcome.called_back) c.gpr[3] = outcome.refused ? 0u : 1u;
    });
    own(kInquiry, "DVDInquiryAsync", [this](Cpu& c) {
        const ReadOutcome outcome = read(c, c.gpr[3], c.gpr[4], 0x20u, 0u, c.gpr[5], kCommandInquiry,
                                         Source::Identity, 1u);
        if (!outcome.called_back) c.gpr[3] = outcome.refused ? 0u : 1u;
    });
    own(kTransferred, "DVDGetTransferredSize", [this](Cpu& c) {
        const u32 block = c.gpr[3];
        const s32 state = static_cast<s32>(c.mem_read32(block + kBlockState));
        if (state != kStateEnd && state != kStateFatal && state != kStateCanceled) ++stats_.anomalies;
        // Nothing is ever in flight: every block holds its final count.
        c.gpr[3] = c.mem_read32(block + kBlockTransferred);
    });

    // ---- status ---------------------------------------------------------------
    own(kBlockStatus, "DVDGetCommandBlockStatus", [this](Cpu& c) {
        ++stats_.status_queries;
        const s32 state = static_cast<s32>(c.mem_read32(c.gpr[3] + kBlockState));
        c.gpr[3] = static_cast<u32>(state == kStateCoverClosed ? 1 : state);
    });
    // No drive: never busy, never paused, never in error.
    own(kDriveStatus, "DVDGetDriveStatus", [this](Cpu& c) {
        ++stats_.status_queries;
        c.gpr[3] = 0u;
    });
    own(kCheckDisk, "DVDCheckDisk", [](Cpu& c) { c.gpr[3] = 1u; });
    own(kResume, "DVDResume", [](Cpu&) {});
    // The API owns no alarms, so no alarm is one of its own.
    own(kTestAlarm, "__DVDTestAlarm", [](Cpu& c) { c.gpr[3] = 0u; });

    // ---- close and cancel ---------------------------------------------------------
    // Nothing is in flight once an entry point returns, so a cancel completes
    // at once. A block in a state the API never produces is counted.
    const auto terminal = [](s32 state) {
        return state == kStateFatal || state == kStateEnd || state == kStateCanceled;
    };
    own(kClose, "DVDClose", [this](Cpu& c) {
        ++stats_.closes;
        c.gpr[3] = 1u;
    });
    own(kCancel, "DVDCancel", [this, terminal](Cpu& c) {
        ++stats_.cancels;
        if (!terminal(static_cast<s32>(c.mem_read32(c.gpr[3] + kBlockState)))) ++stats_.anomalies;
        c.gpr[3] = 0u;
    });
    own(kCancelAsync, "DVDCancelAsync", [this, terminal](Cpu& c) {
        ++stats_.cancels;
        const u32 block = c.gpr[3], callback = c.gpr[4];
        const s32 state = static_cast<s32>(c.mem_read32(block + kBlockState));
        if (!terminal(state)) ++stats_.anomalies;
        // The SDK calls a cancel callback with (0, block) for a finished block;
        // a block the API never left in flight (1..7, 11) is finished too.
        // States 8, 9 and anything outside -1..11 get no callback, as there.
        const bool finished = terminal(state) || (state >= 1 && state <= 7) || state == 11;
        if (callback != 0u && finished) {
            if (!callback_ready(c, memory_, callback)) {
                stop(c, "DVDCancelAsync", "callback " + hex(callback) + " has no translated code");
                return;
            }
            ++stats_.callbacks;
            call_back(c, callback, 0u, block, 1u);
            return;
        }
        c.gpr[3] = 1u;
    });

    // ---- the console reboot path ----------------------------------------------------
    // __OSBootDol's disc boot (reached only from OSResetSystem(0)) loads the
    // apploader from a disc this port does not have. Stop where it starts.
    own(kPrepareReset, "__DVDPrepareResetAsync", [](Cpu& c) {
        stop(c, "__DVDPrepareResetAsync", "console reboot from disc (__OSBootDol) is not supported");
    });
    own(kCancelStream, "DVDCancelStreamAsync", [](Cpu& c) {
        stop(c, "DVDCancelStreamAsync", "console reboot from disc (__OSBootDol) is not supported");
    });
}

}  // namespace gekko
