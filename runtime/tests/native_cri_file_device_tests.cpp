// liveness-roots: reference-interpreter-only
// CRI's "DVD" file device gcCi (docs/DISC_SYSTEM_MAP.md, "Cut 3"): the native owners against the ORIGINAL words of
// the game's DOL in the reference interpreter, over one script of calls that reaches every entry and every path
// (errors, end-of-file clipping and zero fill, the one-transfer rule, cancel, sector-length changes, a full pool,
// the synchronous-read mode).
//
// ORIGINAL runs gcCi as original words, with its strcpy / strcat / strlen / memset / DCInvalidateRange / OSGetTime,
// over the native file API's DVD owners (what it called in production until this cut) and GameMemory's owners
// (memcpy, DCStoreRange). NATIVE runs NativeCriFileDevice over the same namespace. Both register the same error
// reporter stand-in, which logs (object, message, handle). After every call:
//   * the result (r3, for the entries that return one), r1, r2, r13-r31, the non-volatile CR fields, LR and PC;
//   * every byte of the device's global block, its prefix and every transfer buffer;
//   * every byte of the 40 handles, except the DVDFileInfo the original kept in each (+0x28..+0x57, +0x60..+0x63),
//     which the DVD owners wrote and nothing but gcCi read: there the native machine must hold the bytes it held
//     before the script (it never writes them) - checked;
//   * the error reports, in order.
// Below r1 (the callee frames) is dead by the ABI and not compared.
//
// usage: native_cri_file_device_tests <sys/main.dol>

#include "gekko/asset_mount.h"
#include "shadow/backends/storage_backends.h"
#include "gekko/cpu.h"
#include "gekko/game_memory.h"
#include "gekko/guest_memory.h"
#include "gekko/native_cri_file_device.h"
#include "gekko/native_file_api.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>
#include "test_scratch.h"

using namespace gekko;

namespace {

// gcCi
constexpr u32 kGetInterface = 0x803CEA9Cu, kExecServer = 0x803CE924u, kEntryErrFunc = 0x803CE910u;
constexpr u32 kGetFileSize = 0x803CE7ACu, kOpen = 0x803CE59Cu, kClose = 0x803CE3D8u, kSeek = 0x803CE30Cu;
constexpr u32 kTell = 0x803CE2ACu, kReqRd = 0x803CDE84u, kStopTr = 0x803CDCE4u, kGetStat = 0x803CDC80u;
constexpr u32 kGetSctLen = 0x803CDC20u, kSetSctLen = 0x803CDB3Cu, kGetNumTr = 0x803CDADCu;
constexpr u32 kGlobal = 0x805C1BB8u, kPool = 0x805C1BCCu, kHandleSize = 0x64u, kHandles = 40u;
constexpr u32 kPrefix = 0x805C2B70u, kSyncReadFlag = 0x8056A030u, kTable = 0x80569FC8u;
// What the original calls, served on both machines.
constexpr u32 kDvdOpen = 0x8037AAA0u, kDvdClose = 0x8037AB68u, kDvdReadAsync = 0x8037AF04u;
constexpr u32 kDvdReadSync = 0x8037AFF4u, kDvdTransferred = 0x8037B294u, kDvdCallbackReturn = 0x8037CF44u;
constexpr u32 kDvdBlockStatus = 0x8037D390u, kDvdDriveStatus = 0x8037D3DCu, kDvdCancel = 0x8037D764u;
constexpr u32 kMemcpy = 0x800054F4u, kDcStore = 0x80372564u;
constexpr u32 kReadCallback = 0x803CDAD8u;  // gcCi's async-read callback: a lone blr (translated code in production)
// The fixture.
constexpr u32 kReporter = 0x81500000u, kReporterObject = 0x81500100u;
constexpr u32 kFinish = 0x817FF000u, kStack = 0x817E0000u;
constexpr u32 kStrings = 0x81600000u, kBuffers = 0x81610000u, kBufferSize = 0x8000u;

int g_failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) {
        ++g_failures;
        if (g_failures <= 40) std::printf("FAIL: %s\n", what.c_str());
    }
}
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
std::string hex(u32 v) {
    char b[16];
    std::snprintf(b, sizeof(b), "%08X", v);
    return b;
}

void write_file(const std::filesystem::path& path, u32 size, u32 seed) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    std::vector<char> bytes(size);
    for (u32 i = 0; i < size; ++i) bytes[i] = static_cast<char>((i * 131u + seed) & 0xFFu);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// Written once. Both machines mount this folder; rewriting it for the second one deleted the first one's files, and
// on a loaded machine a deleted file can still be unreadable when the first machine reads it.
std::filesystem::path mount_root() {
    static const std::filesystem::path root = [] {
        const auto dir = gekko::test_support::scratch_directory("gekko-native-cri-file-device-tests");
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        write_file(dir / "a.adx", 0x1234u, 1u);   // not a whole number of sectors
        write_file(dir / "b.sfd", 0x1000u, 2u);   // exactly two
        write_file(dir / "big.bin", 0x5000u, 3u);
        write_file(dir / "sub" / "c.bin", 0x900u, 4u);
        return dir;
    }();
    return root;
}

std::vector<u8> read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::vector<u8>{std::istreambuf_iterator<char>(input), {}};
}

struct Machine;
Machine* g_machine = nullptr;

struct Machine {
    GuestMemory bus;
    Cpu cpu{bus};
    assets::AssetSystem assets;
    NativeFileApi files{cpu, bus, assets};
    GameMemory memory_owners{cpu};
    std::unique_ptr<NativeCriFileDevice> device;
    std::vector<std::string> reports;
    RecompiledCode code;
    std::vector<RecompiledCode::Entry> entries;
    std::vector<u8> initial_pool;

    static void reporter(Cpu& c) {
        g_machine->reports.push_back(hex(c.gpr[3]) + " " + hex(c.gpr[4]) + " " + hex(c.gpr[5]));
        c.pc = c.lr;
    }
    static void returns(Cpu& c) { c.pc = c.lr; }
    template <u32 Address>
    static void served(Cpu& c) {
        c.try_hle(Address);
    }

    Machine(const std::vector<u8>& dol, bool native) {
        const auto word = [&](std::size_t offset) { return load_be32(dol.data() + offset); };
        for (u32 i = 0; i < 18u; ++i) {
            const u32 offset = word(i * 4u), address = word(0x48u + i * 4u), size = word(0x90u + i * 4u);
            if (!size) continue;
            require(std::uint64_t(offset) + size <= dol.size(), "invalid DOL extent");
            bus.write_block(address, dol.data() + offset, size);
        }
        // The words the device is, as recovered (DISC_SYSTEM_MAP.md cut 3).
        require(bus.read32(kTable) == kExecServer && bus.read32(kTable + 0x10u) == kOpen &&
                    bus.read32(kReadCallback) == 0x4E800020u &&
                    bus.read32(kGetInterface) == 0x9421FFF0u && bus.read32(kReqRd) == 0x9421FFD0u,
                "unexpected gcCi instruction evidence");
        std::string error;
        std::vector<u8> identity(0x20u, 0u);
        require(shadow::backends::mount_host_directory(assets, mount_root(), identity, error), "the fixture's assets did not mount");
        cpu.reset();
        entries = {{kMemcpy, 4u, served<kMemcpy>, "memcpy"},
                   {kDcStore, 4u, served<kDcStore>, "DCStoreRange"},
                   {kDvdOpen, 4u, served<kDvdOpen>, "DVDOpen"},
                   {kDvdClose, 4u, served<kDvdClose>, "DVDClose"},
                   {kDvdReadAsync, 4u, served<kDvdReadAsync>, "DVDReadAsyncPrio"},
                   {kDvdReadSync, 4u, served<kDvdReadSync>, "DVDReadPrio"},
                   {kDvdTransferred, 4u, served<kDvdTransferred>, "DVDGetTransferredSize"},
                   {kDvdCallbackReturn, 4u, served<kDvdCallbackReturn>, "DVD callback return"},
                   {kDvdBlockStatus, 4u, served<kDvdBlockStatus>, "DVDGetCommandBlockStatus"},
                   {kDvdDriveStatus, 4u, served<kDvdDriveStatus>, "DVDGetDriveStatus"},
                   {kDvdCancel, 4u, served<kDvdCancel>, "DVDCancel"},
                   {kReadCallback, 4u, returns, "gcCi read callback (blr)"},
                   {kReporter, 4u, reporter, "error reporter stand-in"}};
        if (native) {
            for (const u32 a : {kGetInterface, kExecServer, kEntryErrFunc, kGetFileSize, kOpen, kClose, kSeek, kTell,
                                kReqRd, kStopTr, kGetStat, kGetSctLen, kSetSctLen, kGetNumTr}) {
                RecompiledCode::Fn fn = nullptr;
                switch (a) {
                    case kGetInterface: fn = served<kGetInterface>; break;
                    case kExecServer: fn = served<kExecServer>; break;
                    case kEntryErrFunc: fn = served<kEntryErrFunc>; break;
                    case kGetFileSize: fn = served<kGetFileSize>; break;
                    case kOpen: fn = served<kOpen>; break;
                    case kClose: fn = served<kClose>; break;
                    case kSeek: fn = served<kSeek>; break;
                    case kTell: fn = served<kTell>; break;
                    case kReqRd: fn = served<kReqRd>; break;
                    case kStopTr: fn = served<kStopTr>; break;
                    case kGetStat: fn = served<kGetStat>; break;
                    case kGetSctLen: fn = served<kGetSctLen>; break;
                    case kSetSctLen: fn = served<kSetSctLen>; break;
                    case kGetNumTr: fn = served<kGetNumTr>; break;
                }
                entries.push_back({a, 4u, fn, "gcCi"});
            }
        }
        std::sort(entries.begin(), entries.end(),
                  [](const RecompiledCode::Entry& a, const RecompiledCode::Entry& b) { return a.address < b.address; });
        code.install(entries.data(), entries.size());
        cpu.attach_recompiled(&code);
        files.install();
        memory_owners.install();
        if (native) {
            device = std::make_unique<NativeCriFileDevice>(cpu, assets, files);
            device->install();
        }
        // The same seeded image for the state and the buffers on both machines.
        std::mt19937_64 random(0xC21C1ull);
        std::vector<u8> fill(kHandles * kHandleSize + 0x14u);
        for (auto& v : fill) v = static_cast<u8>(random());
        // The handles start as the game's bss leaves them (zero); the global block and buffers seeded.
        std::vector<u8> zero(kHandles * kHandleSize, 0u);
        bus.write_block(kGlobal, fill.data(), 0x14u);
        bus.write_block(kPool, zero.data(), static_cast<u32>(zero.size()));
        std::vector<u8> buffers(4u * kBufferSize);
        for (auto& v : buffers) v = static_cast<u8>(random());
        bus.write_block(kBuffers, buffers.data(), static_cast<u32>(buffers.size()));
        initial_pool = zero;
        for (u32 r = 0; r < 32u; ++r) cpu.gpr[r] = static_cast<u32>(random());
        cpu.gpr[1] = kStack;
        cpu.gpr[2] = 0x805FA780u;
        cpu.gpr[13] = 0x805EC500u;
        cpu.cr = static_cast<u32>(random());
        cpu.set_msr(cpu.msr() | kMsrDR | kMsrIR | kMsrFP);
    }

    void put_string(u32 at, const std::string& text) {
        for (std::size_t i = 0; i < text.size(); ++i) bus.write8(at + static_cast<u32>(i), static_cast<u8>(text[i]));
        bus.write8(at + static_cast<u32>(text.size()), 0u);
    }

    bool call(u32 entry, std::initializer_list<u32> args) {
        u32 r = 3u;
        for (const u32 v : args) cpu.gpr[r++] = v;
        cpu.gpr[1] = kStack;
        cpu.lr = kFinish;
        cpu.pc = entry;
        g_machine = this;
        u64 steps = 0;
        while (cpu.pc != kFinish && !cpu.stopped() && steps++ < 20'000'000ull) cpu.run_steps(1u);
        g_machine = nullptr;
        return cpu.pc == kFinish && !cpu.stopped();
    }
};

bool in_file_info(u32 offset_in_handle) {
    return (offset_in_handle >= 0x28u && offset_in_handle < 0x58u) ||
           (offset_in_handle >= 0x60u && offset_in_handle < 0x64u);
}

void compare(Machine& a, Machine& b, const std::string& where, bool result) {
    if (result) {
        check(a.cpu.gpr[3] == b.cpu.gpr[3], where + ": r3 original " + hex(a.cpu.gpr[3]) + " owner " + hex(b.cpu.gpr[3]));
    }
    for (const u32 r : {1u, 2u, 13u}) check(a.cpu.gpr[r] == b.cpu.gpr[r], where + ": r" + std::to_string(r));
    for (u32 r = 14u; r < 32u; ++r) check(a.cpu.gpr[r] == b.cpu.gpr[r], where + ": r" + std::to_string(r));
    check((a.cpu.cr & 0x00FFF000u) == (b.cpu.cr & 0x00FFF000u), where + ": the non-volatile CR fields");
    check(a.cpu.lr == b.cpu.lr && a.cpu.pc == b.cpu.pc, where + ": lr / pc");
    if (a.reports != b.reports) {
        std::size_t k = 0;
        while (k < a.reports.size() && k < b.reports.size() && a.reports[k] == b.reports[k]) ++k;
        check(false, where + ": error report " + std::to_string(k) + " original '" +
                         (k < a.reports.size() ? a.reports[k] : std::string("-")) + "' owner '" +
                         (k < b.reports.size() ? b.reports[k] : std::string("-")) + "'");
    }
    const auto same = [&](u32 address, u32 size, const char* what) {
        std::vector<u8> x(size), y(size);
        a.bus.peek_block(address, x.data(), size);
        b.bus.peek_block(address, y.data(), size);
        for (u32 i = 0; i < size; ++i) {
            if (x[i] != y[i]) {
                check(false, where + ": " + what + " differs first at " + hex(address + i) + " (original " +
                                 std::to_string(x[i]) + " owner " + std::to_string(y[i]) + ")");
                return;
            }
        }
    };
    same(kGlobal, 0x14u, "the global block");
    same(kPrefix, 0x100u, "the prefix");
    same(kBuffers, 4u * kBufferSize, "the buffers");
    std::vector<u8> x(kHandles * kHandleSize), y(kHandles * kHandleSize);
    a.bus.peek_block(kPool, x.data(), static_cast<u32>(x.size()));
    b.bus.peek_block(kPool, y.data(), static_cast<u32>(y.size()));
    for (u32 i = 0; i < x.size(); ++i) {
        const u32 offset = i % kHandleSize;
        if (in_file_info(offset)) {
            if (y[i] != b.initial_pool[i] && y[i] != x[i]) {
                check(false, where + ": the owner wrote the old DVDFileInfo at " + hex(kPool + i));
                break;
            }
            continue;
        }
        if (x[i] != y[i]) {
            check(false, where + ": handle " + std::to_string(i / kHandleSize) + " +0x" + hex(offset).substr(6) +
                             " original " + std::to_string(x[i]) + " owner " + std::to_string(y[i]));
            break;
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::printf("usage: native_cri_file_device_tests <sys/main.dol>\n");
        return 2;
    }
    try {
        const std::vector<u8> dol = read_file(argv[1]);
        require(dol.size() > 0x100u, "cannot read the DOL");
        Machine original(dol, false), owner(dol, true);
        int steps = 0;
        // One call on both machines, compared. `result`: the entry returns a value in r3.
        const auto both = [&](const char* what, u32 entry, std::initializer_list<u32> args, bool result) {
            const bool a = original.call(entry, args);
            const bool b = owner.call(entry, args);
            check(a, std::string(what) + ": the original did not return (" + original.cpu.stop().detail + ")");
            check(b, std::string(what) + ": the owner did not return (" + owner.cpu.stop().detail + ")");
            if (a && b) compare(original, owner, what, result);
            ++steps;
            return original.cpu.gpr[3];
        };
        const auto text = [&](u32 slot, const std::string& s) {
            original.put_string(kStrings + slot * 0x40u, s);
            owner.put_string(kStrings + slot * 0x40u, s);
            return kStrings + slot * 0x40u;
        };
        const u32 buf0 = kBuffers, buf1 = kBuffers + kBufferSize, buf2 = kBuffers + 2u * kBufferSize;
        const u32 buf3 = kBuffers + 3u * kBufferSize;

        both("GetInterface", kGetInterface, {}, true);
        both("Tell(0) with no reporter", kTell, {0u}, true);
        both("EntryErrFunc", kEntryErrFunc, {kReporter, kReporterObject}, false);
        const u32 a = text(0, "a.adx"), b = text(1, "b.sfd"), missing = text(2, "missing.bin");
        const u32 sub = text(3, "sub"), c = text(4, "sub\\c.bin"), big = text(5, "big.bin");
        both("Open(NULL)", kOpen, {0u, 0u, 0u}, true);
        both("Open(mode 1)", kOpen, {a, 0u, 1u}, true);
        both("Open(missing)", kOpen, {missing, 0u, 0u}, true);
        both("Open(a directory)", kOpen, {sub, 0u, 0u}, true);
        both("GetFileSize(NULL)", kGetFileSize, {0u}, true);
        both("GetFileSize(missing)", kGetFileSize, {missing}, true);
        both("GetFileSize(a.adx)", kGetFileSize, {a}, true);
        both("GetFileSize(sub\\c.bin)", kGetFileSize, {c}, true);
        const u32 ha = both("Open(a.adx)", kOpen, {a, 0u, 0u}, true);
        const u32 hb = both("Open(b.sfd)", kOpen, {b, 0u, 0u}, true);
        const u32 hc = both("Open(sub\\c.bin)", kOpen, {c, 0u, 0u}, true);
        check(ha != 0u && hb != 0u && hc != 0u, "the three opens returned handles");
        for (const u32 e : {kTell, kGetStat, kGetSctLen, kGetNumTr}) both("a field of a.adx", e, {ha}, true);
        for (const u32 e : {kTell, kGetStat, kGetSctLen, kGetNumTr}) both("a field of NULL", e, {0u}, true);
        const struct {
            s32 offset;
            u32 whence;
        } seeks[] = {{1, 0u}, {1, 1u}, {-1, 2u}, {100, 0u}, {-5, 1u}, {2, 7u}, {0, 0u}};
        for (const auto& s : seeks) both("Seek", kSeek, {ha, static_cast<u32>(s.offset), s.whence}, true);
        both("Seek(NULL)", kSeek, {0u, 1u, 0u}, true);
        both("ReqRd(NULL)", kReqRd, {0u, 1u, buf0}, true);
        both("ReqRd(-1 sectors)", kReqRd, {ha, 0xFFFFFFFFu, buf0}, true);
        both("ReqRd(no buffer)", kReqRd, {ha, 1u, 0u}, true);
        both("ReqRd(0 sectors)", kReqRd, {ha, 0u, buf0}, true);
        both("ReqRd(a, 1)", kReqRd, {ha, 1u, buf0}, true);
        both("ReqRd(b, 1) while a reads", kReqRd, {hb, 1u, buf1}, true);
        both("ReqRd(a) again while it reads", kReqRd, {ha, 1u, buf0}, true);
        both("GetStat(a) reading", kGetStat, {ha}, true);
        both("ExecServer", kExecServer, {}, false);
        both("GetNumTr(a)", kGetNumTr, {ha}, true);
        both("ReqRd(a, 5): clipped at the end", kReqRd, {ha, 5u, buf0}, true);
        both("ExecServer: zero fill", kExecServer, {}, false);
        both("ReqRd(a) past the end", kReqRd, {ha, 1u, buf0}, true);
        both("ReqRd(b, 2): exactly to the end", kReqRd, {hb, 2u, buf1}, true);
        both("ExecServer", kExecServer, {}, false);
        both("ReqRd(b, 1) at the end: 0 bytes", kReqRd, {hb, 1u, buf1}, true);
        both("ExecServer: a whole sector of zeros", kExecServer, {}, false);
        both("StopTr(a) idle", kStopTr, {ha}, false);
        both("StopTr(NULL)", kStopTr, {0u}, false);
        both("ReqRd(c, 1)", kReqRd, {hc, 1u, buf2}, true);
        both("StopTr(c) reading", kStopTr, {hc}, false);
        both("ExecServer after the stop", kExecServer, {}, false);
        both("GetStat(c)", kGetStat, {hc}, true);
        both("SetSctLen(a, 0x400)", kSetSctLen, {ha, 0x400u}, false);
        for (const u32 e : {kGetSctLen, kTell, kGetNumTr}) both("after SetSctLen", e, {ha}, true);
        both("SetSctLen(a, 0x401)", kSetSctLen, {ha, 0x401u}, false);
        both("SetSctLen(a, 0x800): the current length is unaligned", kSetSctLen, {ha, 0x800u}, false);
        both("SetSctLen(NULL)", kSetSctLen, {0u, 0x800u}, false);
        both("ReqRd(c, 1) then Close without a server", kReqRd, {hc, 1u, buf2}, true);
        both("Close(c) reading", kClose, {hc}, false);
        both("Close(a)", kClose, {ha}, false);
        both("Close(NULL)", kClose, {0u}, false);
        both("Close(b)", kClose, {hb}, false);
        // A full pool.
        std::vector<u32> handles;
        for (u32 i = 0; i < kHandles; ++i) handles.push_back(both("Open to fill the pool", kOpen, {big, 0u, 0u}, true));
        both("Open(a full pool)", kOpen, {a, 0u, 0u}, true);
        for (const u32 h : handles) both("Close to empty the pool", kClose, {h}, false);
        // The synchronous mode (the flag the ADX init sets).
        original.bus.write32(kSyncReadFlag, 1u);
        owner.bus.write32(kSyncReadFlag, 1u);
        const u32 hs = both("Open(b.sfd) in sync mode", kOpen, {b, 0u, 0u}, true);
        both("Seek to the end", kSeek, {hs, 2u, 0u}, true);
        both("ReqRd at the end, sync: 0 bytes is a failure", kReqRd, {hs, 1u, buf3}, true);
        both("Seek to 0", kSeek, {hs, 0u, 0u}, true);
        both("ReqRd(b, 1), sync", kReqRd, {hs, 1u, buf3}, true);
        both("ExecServer", kExecServer, {}, false);
        both("Close(b)", kClose, {hs}, false);
        std::printf("native CRI file device: %d calls against the original in the interpreter, %d failure(s)\n",
                    steps, g_failures);
    } catch (const std::exception& error) {
        std::printf("ERROR: %s\n", error.what());
        return 1;
    }
    return g_failures == 0 ? 0 : 1;
}
