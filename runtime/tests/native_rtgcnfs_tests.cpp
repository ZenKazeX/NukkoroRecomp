// liveness-roots: reference-interpreter-only
// RenderWare's GameCube file system RtGCNFS (docs/RTGCNFS_SPEC.md, docs/DISC_SYSTEM_MAP.md "Cut 4"): the native owners
// against the ORIGINAL words of the game's DOL in the reference interpreter, over one script of calls through every
// path of open / read / setPosition / sync / abort / close (synchronous; the asynchronous ones must be refused).
//
// The file system object and its RtFile records are laid out in guest memory as RtGCNFSystemInit leaves them (the
// device table, the 0x800-byte cache buffers, the records' file-system pointers). Both machines serve the DVD SDK
// through the native file API (the original called it), memcpy through its owner, and three stand-ins that log:
// OSYieldThread (with the stack pointer it was called at), the file system's drive-error callback (fs+0x5C: its
// arguments) and OSReport (its arguments). ORIGINAL runs RtGCNFS and RenderWare's helpers as original words; NATIVE
// runs NativeRtGcnFs (the helpers stay original words). After every call:
//   * the result, r1, r2, r13-r31, the non-volatile CR fields, LR and PC;
//   * the yields, error-callback calls and reports, in order;
//   * every byte of the file system object, the drive state G (0x2A40(r13)), the destination buffers and the four
//     records - except the DVD command block the original kept in each (+0x40..+0x6B other than the state word
//     +0x4C, and the DVDFileInfo callback +0x78), which the DVD owners wrote and nothing but RtGCNFS read: there the
//     native machine must hold the bytes it started with (checked).
//   * the stack window below the caller [r1 - 0x400, r1 + 8): every frame word the device and its helpers leave there
//     (back chains, LR words, callee-save areas, open's path buffer) - the game copies stack leftovers into heap
//     records, so an owner's frames must be the original's. The OSYieldThread stand-in stores what OSYieldThread's
//     prologue stores (the caller's LR word, its back chain, r31), on both machines, so the LR word a yield leaves in
//     read's / the transfer's frame and the registers at each yield are compared too.
// A third machine runs the same script with the owners, but every read through NativeRtGcnFs::read_sync (Phase 4: the
// read for a native caller, its yields continued at once): it must end each read exactly as the owner machine did -
// the same result, registers, events and bytes - with no yield (its stack is not compared with the owner's: the
// yields it continues at once store no OSYieldThread words).
//
// usage: native_rtgcnfs_tests <sys/main.dol>

#include "gekko/asset_mount.h"
#include "shadow/backends/storage_backends.h"
#include "gekko/cpu.h"
#include "gekko/game_memory.h"
#include "gekko/guest_memory.h"
#include "gekko/native_file_api.h"
#include "gekko/native_rtgcnfs.h"

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

constexpr u32 kOpen = 0x8047B874u, kClose = 0x8047B988u, kRead = 0x8047BC60u, kReadChunk = 0x8047BC64u;
constexpr u32 kReadTransfer = 0x8047BC68u, kSetPosition = 0x8047C0B0u, kSync = 0x8047C1A4u, kAbort = 0x8047C3D4u;
constexpr u32 kGetDeviceName = 0x8047B83Cu;
constexpr u32 kDvdOpen = 0x8037AAA0u, kDvdClose = 0x8037AB68u, kDvdReadAsync = 0x8037AF04u;
constexpr u32 kDvdCallbackReturn = 0x8037CF44u, kDvdBlockStatus = 0x8037D390u, kDvdDriveStatus = 0x8037D3DCu;
constexpr u32 kDvdCancelAsync = 0x8037D4E8u, kMemcpy = 0x800054F4u;
constexpr u32 kYield = 0x80378A08u, kReport = 0x803731CCu, kReadCallback = 0x8047B9C0u, kTrigger = 0x8047AA18u;
// The fixture.
constexpr u32 kErrorCallback = 0x81500000u, kErrorData = 0x81500100u;
constexpr u32 kFs = 0x81400000u, kFiles = 0x81400100u, kRecord = 0x90u, kRecords = 4u;
constexpr u32 kCaches = 0x81410000u, kCacheSize = 0x800u;
constexpr u32 kBuffers = 0x81420000u, kBufferSize = 0x4000u;
constexpr u32 kStrings = 0x81600000u, kEngine = 0x81480000u;
constexpr u32 kFinish = 0x817FF000u, kStack = 0x817E0000u;
constexpr u32 kR13 = 0x805EC500u, kGlobal = kR13 + 0x2A40u;

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
    for (u32 i = 0; i < size; ++i) bytes[i] = static_cast<char>((i * 97u + seed) & 0xFFu);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// Written once per process: every machine mounts the same tree, and rewriting it while an earlier machine has its
// files mapped made reads fail at random (a read "could not be delivered" in some runs).
std::filesystem::path mount_root() {
    static const std::filesystem::path root = [] {
        const auto dir = gekko::test_support::scratch_directory("gekko-native-rtgcnfs-tests");
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        write_file(dir / "small.dff", 0x1A3u, 1u);
        write_file(dir / "big.txd", 0x2345u, 2u);
        write_file(dir / "exact.bin", 0x1000u, 3u);
        write_file(dir / "sub" / "deep.one", 0x900u, 4u);
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
    std::unique_ptr<NativeRtGcnFs> device;
    std::vector<std::string> log;
    RecompiledCode code;
    std::vector<RecompiledCode::Entry> entries;
    std::vector<u8> initial_records;

    static void yield(Cpu& c) {
        g_machine->log.push_back("yield sp=" + hex(c.gpr[1]));
        // OSYieldThread 0x80378A08's own stores before it switches: mflr r0; stw r0, 4(r1); stwu r1, -0x10(r1);
        // stw r31, 0xC(r1).
        c.mem_write32(c.gpr[1] + 4u, c.lr);
        c.mem_write32(c.gpr[1] - 0x10u, c.gpr[1]);
        c.mem_write32(c.gpr[1] - 0x4u, c.gpr[31]);
        c.pc = c.lr;
    }
    static void error_callback(Cpu& c) {
        g_machine->log.push_back("errcb " + hex(c.gpr[3]) + " " + hex(c.gpr[4]));
        c.gpr[3] = 1u;
        c.pc = c.lr;
    }
    static void trigger(Cpu& c) {
        g_machine->log.push_back("trigger " + hex(c.gpr[3]));
        c.pc = c.lr;
    }
    // The engine's string slots the open path calls (strlen 0x803ADC08, strcpy 0x803ADB50): stand-ins on BOTH machines
    // (an owner's guest call needs translated code, which this interpreter test has only for stand-ins). strlen leaves
    // r3 = the length, r4 = the NUL's address, r0 = 0 and CR0 EQ as the original's loop does.
    static void str_length(Cpu& c) {
        u32 at = c.gpr[3], n = 0u;
        while (c.mem_read8(at) != 0u) {
            ++at;
            ++n;
        }
        c.gpr[3] = n;
        c.gpr[4] = at;
        c.gpr[0] = 0u;
        c.cr = (c.cr & 0x0FFFFFFFu) | (0x2u << 28) | (((c.xer >> 31) & 1u) << 28);
        c.pc = c.lr;
    }
    static void str_copy(Cpu& c) {
        const u32 to = c.gpr[3];
        u32 from = c.gpr[4], at = to;
        g_machine->log.push_back("strcpy " + hex(to) + " " + hex(from));
        for (;; ++from, ++at) {
            const u8 b = c.mem_read8(from);
            c.mem_write8(at, b);
            if (b == 0u) break;
        }
        c.gpr[3] = to;
        c.pc = c.lr;
    }
    static void report(Cpu& c) {
        g_machine->log.push_back("report " + hex(c.gpr[3]) + " " + hex(c.gpr[4]));
        c.pc = c.lr;
    }
    static void read_callback(Cpu& c) {
        c.mem_write32(c.gpr[4] + 0x2Cu, c.gpr[3]);
        c.pc = c.lr;
    }
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
        // The words the device is, as recovered (docs/RTGCNFS_SPEC.md).
        require(bus.read32(kOpen) == 0x9421FEE0u && bus.read32(kRead) == 0x9421FFC0u &&
                    bus.read32(kReadCallback) == 0x9064002Cu && bus.read32(kReadCallback + 4u) == 0x4E800020u &&
                    bus.read32(kGlobal) == 1u,
                "unexpected RtGCNFS instruction evidence");
        std::string error;
        std::vector<u8> identity(0x20u, 0u);
        require(shadow::backends::mount_host_directory(assets, mount_root(), identity, error), "the fixture's assets did not mount");
        cpu.reset();
        entries = {{kMemcpy, 4u, served<kMemcpy>, "memcpy"},
                   {kDvdOpen, 4u, served<kDvdOpen>, "DVDOpen"},
                   {kDvdClose, 4u, served<kDvdClose>, "DVDClose"},
                   {kDvdReadAsync, 4u, served<kDvdReadAsync>, "DVDReadAsyncPrio"},
                   {kDvdCallbackReturn, 4u, served<kDvdCallbackReturn>, "DVD callback return"},
                   {kDvdBlockStatus, 4u, served<kDvdBlockStatus>, "DVDGetCommandBlockStatus"},
                   {kDvdDriveStatus, 4u, served<kDvdDriveStatus>, "DVDGetDriveStatus"},
                   {kDvdCancelAsync, 4u, served<kDvdCancelAsync>, "DVDCancelAsync"},
                   {kYield, 4u, yield, "OSYieldThread stand-in"},
                   {kReport, 4u, report, "OSReport stand-in"},
                   {0x803ADB50u, 4u, str_copy, "strcpy stand-in"},
                   {0x803ADC08u, 4u, str_length, "strlen stand-in"},
                   {kErrorCallback, 4u, error_callback, "drive-error callback stand-in"},
                   {kTrigger, 4u, trigger, "rtFSTriggerFileOustandingOpCallback stand-in"}};
        if (native) {
            entries.push_back({kOpen, 4u, served<kOpen>, "open"});
            entries.push_back({kClose, 4u, served<kClose>, "close"});
            entries.push_back({kRead, 4u, served<kRead>, "read"});
            entries.push_back({kReadChunk, 4u, served<kReadChunk>, "read chunk"});
            entries.push_back({kReadTransfer, 4u, served<kReadTransfer>, "read transfer"});
            entries.push_back({kSync, 4u, served<kSync>, "sync"});
            entries.push_back({kAbort, 4u, served<kAbort>, "abort"});
        } else {
            // The original's read-complete callback (stw r3,0x2C(r4); blr) is translated code in production.
            entries.push_back({kReadCallback, 8u, read_callback, "read-complete callback"});
        }
        std::sort(entries.begin(), entries.end(),
                  [](const RecompiledCode::Entry& a, const RecompiledCode::Entry& b) { return a.address < b.address; });
        code.install(entries.data(), entries.size());
        cpu.attach_recompiled(&code);
        files.install();
        memory_owners.install();
        if (native) {
            device = std::make_unique<NativeRtGcnFs>(cpu, assets, files);
            device->install();
        }
        // The file system and its records as RtGCNFSystemInit leaves them.
        const u32 table[][2] = {{0x14u, 0x8047B818u}, {0x18u, 0x8047B844u}, {0x1Cu, kGetDeviceName},
                                {0x28u, kOpen}, {0x2Cu, kClose}, {0x30u, kRead}, {0x34u, 0x8047C0A0u},
                                {0x38u, kSetPosition}, {0x3Cu, kSync}, {0x40u, kAbort}, {0x44u, 0x8047A898u},
                                {0x48u, 0x8047C3FCu}, {0x4Cu, 0x8047C43Cu}};
        std::mt19937_64 random(0x47C5ull);
        for (u32 i = 0; i < 0x64u; i += 4u) bus.write32(kFs + i, static_cast<u32>(random()));
        for (const auto& slot : table) bus.write32(kFs + slot[0], slot[1]);
        bus.write32(kFs + 0x04u, kRecords);
        bus.write32(kFs + 0x08u, 2u);
        bus.write32(kFs + 0x10u, 0u);
        bus.write32(kFs + 0x58u, kFiles);
        bus.write32(kFs + 0x5Cu, kErrorCallback);
        bus.write32(kFs + 0x60u, kErrorData);
        // RenderWare's engine: the two string slots RenderWare's path helpers call, as the game installs them
        // (read from a Westopolis savestate: ENG+0xCC strcpy 0x803ADB50, ENG+0xF4 strlen 0x803ADC08).
        for (u32 i = 0; i < 0x200u; i += 4u) bus.write32(kEngine + i, 0u);
        bus.write32(kEngine + 0xCCu, 0x803ADB50u);
        bus.write32(kEngine + 0xF4u, 0x803ADC08u);
        bus.write32(kR13 + 0x615Cu, kEngine);
        std::vector<u8> zero(kRecords * kRecord, 0u);
        bus.write_block(kFiles, zero.data(), static_cast<u32>(zero.size()));
        for (u32 i = 0; i < kRecords; ++i) {
            const u32 f = kFiles + i * kRecord;
            bus.write32(f + 0x20u, 1u);  // CLOSED
            bus.write32(f + 0x38u, kFs);
            bus.write32(f + 0x7Cu, kCaches + i * kCacheSize);
            bus.write32(f + 0x88u, kCacheSize);
        }
        std::vector<u8> fill(kRecords * kCacheSize + 4u * kBufferSize);
        for (auto& v : fill) v = static_cast<u8>(random());
        bus.write_block(kCaches, fill.data(), kRecords * kCacheSize);
        bus.write_block(kBuffers, fill.data() + kRecords * kCacheSize, 4u * kBufferSize);
        initial_records.resize(kRecords * kRecord);
        bus.peek_block(kFiles, initial_records.data(), static_cast<u32>(initial_records.size()));
        for (u32 r = 0; r < 32u; ++r) cpu.gpr[r] = static_cast<u32>(random());
        cpu.gpr[2] = 0x805FA780u;
        cpu.gpr[13] = kR13;
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
        while (cpu.pc != kFinish && !cpu.stopped() && steps++ < 50'000'000ull) cpu.run_steps(1u);
        g_machine = nullptr;
        return cpu.pc == kFinish && !cpu.stopped();
    }
};

bool in_dvd_block(u32 offset) {
    return (offset >= 0x40u && offset < 0x6Cu && !(offset >= 0x4Cu && offset < 0x50u)) ||
           (offset >= 0x78u && offset < 0x7Cu);
}

void compare(Machine& a, Machine& b, const std::string& where, bool stack) {
    check(a.cpu.gpr[3] == b.cpu.gpr[3], where + ": r3 original " + hex(a.cpu.gpr[3]) + " owner " + hex(b.cpu.gpr[3]));
    for (const u32 r : {1u, 2u, 13u}) check(a.cpu.gpr[r] == b.cpu.gpr[r], where + ": r" + std::to_string(r));
    for (u32 r = 14u; r < 32u; ++r) {
        check(a.cpu.gpr[r] == b.cpu.gpr[r],
              where + ": r" + std::to_string(r) + " original " + hex(a.cpu.gpr[r]) + " owner " + hex(b.cpu.gpr[r]));
    }
    check((a.cpu.cr & 0x00FFF000u) == (b.cpu.cr & 0x00FFF000u), where + ": the non-volatile CR fields");
    check(a.cpu.lr == b.cpu.lr && a.cpu.pc == b.cpu.pc, where + ": lr / pc");
    if (a.log != b.log) {
        std::size_t k = 0;
        while (k < a.log.size() && k < b.log.size() && a.log[k] == b.log[k]) ++k;
        check(false, where + ": event " + std::to_string(k) + " original '" +
                         (k < a.log.size() ? a.log[k] : std::string("-")) + "' owner '" +
                         (k < b.log.size() ? b.log[k] : std::string("-")) + "'");
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
    same(kFs, 0x64u, "the file system object");
    same(kGlobal, 4u, "the drive state G");
    same(kCaches, kRecords * kCacheSize, "the caches");
    same(kBuffers, 4u * kBufferSize, "the buffers");
    // The stack window, except the native DVD API's completion trampoline: a 0x40-byte frame tagged 'NFCB' at +48 that
    // the fixture's DVDReadAsyncPrio stand-in pushes to run the read-complete callback. The owner reads without the DVD
    // API, and in the game that callback runs from the DVD interrupt - neither side's bytes there are the original's.
    const auto same_stack = [&](u32 address, u32 size) {
        std::vector<u8> x(size), y(size);
        a.bus.peek_block(address, x.data(), size);
        b.bus.peek_block(address, y.data(), size);
        const auto tagged = [](const std::vector<u8>& v, u32 at) {
            // ... or, when OSYieldThread's stand-in later wrote over the tag (r1 - 0x10 at the transfer's yield),
            // by the return address it saved at +52: the transfer's `bl DVDReadAsyncPrio` 0x8047BA08.
            if (at + 56u > v.size()) return false;
            const bool tag = v[at + 48u] == 'N' && v[at + 49u] == 'F' && v[at + 50u] == 'C' && v[at + 51u] == 'B';
            const bool link = v[at + 52u] == 0x80u && v[at + 53u] == 0x47u && v[at + 54u] == 0xBAu && v[at + 55u] == 0x08u;
            return tag || link;
        };
        for (u32 at = 0; at + 4u <= size; at += 4u) {
            if (!tagged(x, at) && !tagged(y, at)) continue;
            // Masked here, and the original's bytes copied to the owner, so the frame's leftovers (its return value
            // at +56 outlives the tag once later frames overwrite +48/+52) never show up in a later comparison.
            const u32 n = std::min<u32>(64u, size - at);
            b.bus.write_block(address + at, x.data() + at, n);
            for (u32 k = at; k < at + n; ++k) x[k] = y[k] = 0u;
        }
        for (u32 i = 0; i < size; ++i) {
            if (x[i] != y[i]) {
                check(false, where + ": the stack window differs first at " + hex(address + i) + " (original " +
                                 std::to_string(x[i]) + " owner " + std::to_string(y[i]) + ")");
                return;
            }
        }
    };
    if (stack) same_stack(kStack - 0x400u, 0x408u);
    std::vector<u8> x(kRecords * kRecord), y(kRecords * kRecord);
    a.bus.peek_block(kFiles, x.data(), static_cast<u32>(x.size()));
    b.bus.peek_block(kFiles, y.data(), static_cast<u32>(y.size()));
    for (u32 i = 0; i < x.size(); ++i) {
        const u32 offset = i % kRecord;
        if (in_dvd_block(offset)) {
            if (y[i] != b.initial_records[i]) {
                check(false, where + ": the owner wrote the old DVD block at " + hex(kFiles + i));
                break;
            }
            continue;
        }
        if (x[i] != y[i]) {
            check(false, where + ": record " + std::to_string(i / kRecord) + " +0x" + hex(offset).substr(6) +
                             " original " + std::to_string(x[i]) + " owner " + std::to_string(y[i]));
            break;
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::printf("usage: native_rtgcnfs_tests <sys/main.dol>\n");
        return 2;
    }
    try {
        const std::vector<u8> dol = read_file(argv[1]);
        require(dol.size() > 0x100u, "cannot read the DOL");
        Machine original(dol, false), owner(dol, true), sync(dol, true);
        int steps = 0, sync_reads = 0;
        const auto both = [&](const std::string& what, u32 entry, std::initializer_list<u32> args) {
            const bool a = original.call(entry, args);
            const bool b = owner.call(entry, args);
            check(a, what + ": the original did not return (" + original.cpu.stop().detail + ")");
            check(b, what + ": the owner did not return (" + owner.cpu.stop().detail + ")");
            if (a && b) compare(original, owner, what, true);
            // The read_sync machine: reads entered as the read slot is (r1 = the caller's, LR = its return site).
            bool s = false;
            if (entry == kRead) {
                std::vector<u32> v(args);
                sync.cpu.gpr[1] = kStack;
                sync.cpu.lr = kFinish;
                g_machine = &sync;
                const std::optional<u32> got = sync.device->read_sync(sync.cpu, v[0], v[1], v[2]);
                g_machine = nullptr;
                sync.cpu.pc = sync.cpu.lr;
                s = got.has_value() && sync.cpu.pc == kFinish && !sync.cpu.stopped();
                ++sync_reads;
            } else {
                s = sync.call(entry, args);
            }
            check(s, what + ": the read_sync machine did not return (" + sync.cpu.stop().detail + ")");
            if (b && s) {
                std::vector<std::string> owner_log = owner.log;
                owner.log.erase(std::remove_if(owner.log.begin(), owner.log.end(),
                                               [](const std::string& e) { return e.rfind("yield ", 0) == 0; }),
                                owner.log.end());
                check(std::none_of(sync.log.begin(), sync.log.end(),
                                   [](const std::string& e) { return e.rfind("yield ", 0) == 0; }),
                      what + ": read_sync yielded");
                compare(owner, sync, what + " (read_sync against the owner)", false);
                owner.log = owner_log;
            }
            ++steps;
            return original.cpu.gpr[3];
        };
        const auto text = [&](u32 slot, const std::string& s) {
            original.put_string(kStrings + slot * 0x100u, s);
            owner.put_string(kStrings + slot * 0x100u, s);
            sync.put_string(kStrings + slot * 0x100u, s);
            return kStrings + slot * 0x100u;
        };
        const auto file = [](u32 i) { return kFiles + i * kRecord; };
        const u32 small = text(0, "dvd:small.dff"), big = text(1, "dvd:big.txd"), exact = text(2, "exact.bin");
        const u32 deep = text(3, "dvd:sub\\deep.one"), missing = text(4, "dvd:missing.bin");
        const u32 directory = text(5, "dvd:sub"), longname = text(6, "dvd:" + std::string(300u, 'x'));
        const u32 b0 = kBuffers, b1 = kBuffers + kBufferSize + 3u, b2 = kBuffers + 2u * kBufferSize;
        const u32 b3 = kBuffers + 3u * kBufferSize + 0x20u;

        both("open for writing", kOpen, {kFs, file(0), small, 2u});
        both("open a name too long", kOpen, {kFs, file(0), longname, 1u});
        both("open missing", kOpen, {kFs, file(0), missing, 1u});
        both("open a directory", kOpen, {kFs, file(0), directory, 1u});
        both("open small.dff", kOpen, {kFs, file(0), small, 1u});
        both("open big.txd", kOpen, {kFs, file(1), big, 1u});
        both("open exact.bin (no device prefix)", kOpen, {kFs, file(2), exact, 1u});
        both("open sub\\deep.one", kOpen, {kFs, file(3), deep, 1u});
        both("read 0 bytes", kRead, {file(0), b0, 0u});
        both("read small: 7 unaligned bytes (cache fill + serve)", kRead, {file(0), b1, 7u});
        both("read small: 40 more from the cache", kRead, {file(0), b1 + 7u, 40u});
        both("read small: past the end (clamped)", kRead, {file(0), b0, 0x1000u});
        both("read small at the end", kRead, {file(0), b0, 16u});
        both("read big: 0x1000 aligned (direct)", kRead, {file(1), b2, 0x1000u});
        both("read big: 0x900 aligned + tail", kRead, {file(1), b2 + 0x1000u, 0x930u});
        both("read big: the rest, unaligned destination", kRead, {file(1), b1 + 0x40u, 0x2000u});
        both("setPosition big BEGIN 0x10", kSetPosition, {file(1), 0x10u, 1u});
        both("read big: 0x30 from 0x10 (cache)", kRead, {file(1), b3, 0x30u});
        both("setPosition big CURRENT +0x100 (inside the cache)", kSetPosition, {file(1), 0x100u, 2u});
        both("read big: 0x20 from the cache", kRead, {file(1), b3, 0x20u});
        both("setPosition big CURRENT -0x200 (out of the cache)", kSetPosition, {file(1), 0xFFFFFE00u, 2u});
        both("setPosition big END -0x80", kSetPosition, {file(1), 0xFFFFFF80u, 3u});
        both("read big: the last 0x80 (cache straddle)", kRead, {file(1), b3, 0x100u});
        both("read exact: whole, aligned", kRead, {file(2), b0, 0x1000u});
        both("read exact: at the end", kRead, {file(2), b0, 0x20u});
        both("read deep: 0x123", kRead, {file(3), b2 + 5u, 0x123u});
        both("sync small", kSync, {file(0), 0u});
        both("abort small", kAbort, {file(0)});
        for (u32 i = 0; i < kRecords; ++i) both("close", kClose, {file(i)});
        // The asynchronous paths are outside the owner's contract: no caller in this game can open a file
        // asynchronously (tools/platform/rtfs_async_proof.py, CTest platform_rtfs_async_proof, proves it from the
        // DOL). The owner must refuse each by stopping the run (its own refusal, nothing served), on a fresh machine.
        const auto refused = [&](const std::string& what, u32 entry, std::initializer_list<u32> args, bool async_record) {
            Machine m(dol, true);
            m.put_string(kStrings, "dvd:small.dff");
            if (async_record) m.bus.write32(file(0) + 0x18u, 1u);  // isAsync
            const bool returned = m.call(entry, args);
            check(!returned && m.cpu.stopped() &&
                      m.cpu.stop().detail.find("an asynchronous file") != std::string::npos,
                  what + ": the owner must refuse it (" + m.cpu.stop().detail + ")");
            ++steps;
        };
        refused("open async (mode 0x11)", kOpen, {kFs, file(0), kStrings, 0x11u}, false);
        refused("read an async record", kRead, {file(0), b1, 7u}, true);
        refused("sync an async record", kSync, {file(0), 0u}, true);
        std::printf("native RtGCNFS: %d calls against the original in the interpreter (%d reads also through read_sync), "
                    "%d failure(s)\n", steps, sync_reads, g_failures);
    } catch (const std::exception& error) {
        std::printf("ERROR: %s\n", error.what());
        return 1;
    }
    return g_failures == 0 ? 0 : 1;
}
