// liveness-roots: reference-interpreter-only
// TBGRead's chunked background read (docs/STAGE_LOADING.md 9d): the inline owner of 0x80045150 (NativeRtGcnFs with
// GEKKO_NATIVE_BG_READ=1) against the ORIGINAL words of the game's DOL in the reference interpreter - the read loop
// 0x80045150, RwFread 0x8047B430 and RtGCNFS's read 0x8047BC60 with its submit, all original - over files of every
// size class around the 0x2000 chunk and the 32-byte alignment, aligned / unaligned / allocated destinations, other
// chunk sizes, a file read from a nonzero position and a cache left valid by an earlier read.
//
// The fixture is native_rtgcnfs_tests' (the file system and its records as RtGCNFSystemInit leaves them, the DVD SDK
// through the native file API, memcpy through its owner) plus a stand-in for the RenderWare allocator fn_80049BB8
// (logs its arguments, returns the next 32-aligned block of a fixed arena). Files are opened by each machine's own
// open (original words / the RtGCNFS owner: their equality is native_rtgcnfs_tests'). After every call:
//   * the result, r1, r2, r13-r31, the non-volatile CR fields, LR and PC;
//   * the guest calls in order: the allocator, every drive-error callback and report - the same on both;
//   * the yields: the original yields twice per chunk, the owner never (the one intended difference: when the read
//     ends, not what it does) - counted and checked;
//   * the stack window below the caller after every call - the loop's, open's, close's, read's and their callees'
//     frames (the game copies stack leftovers into heap records). The OSYieldThread stand-in writes no frame on
//     either machine; in the game the original's yields also leave OSYieldThread's words (read's / the transfer's
//     LR word, OSYieldThread's own frame below), which the inline loop - its yields continued at once - does not
//     (the user-approved change of GEKKO_NATIVE_BG_READ: OSYieldThread's bookkeeping is what is not done);
//   * every byte of the file system object, the drive state G, the caches, the destinations and the four records
//     (except the DVD command block the original kept in each, which only the original wrote).
//
// usage: native_bg_read_tests <sys/main.dol>

#include "shadow/backends/storage_backends.h"
#include "gekko/cpu.h"
#include "gekko/game_memory.h"
#include "gekko/guest_memory.h"
#include "gekko/native_file_api.h"
#include "gekko/native_rtgcnfs.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
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

constexpr u32 kLoop = 0x80045150u, kFread = 0x8047B430u, kAlloc = 0x80049BB8u;
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
constexpr u32 kBuffers = 0x81000000u, kBuffersSize = 0x200000u;  // destinations and the allocator's arena
constexpr u32 kArena = kBuffers + 0x100000u;
constexpr u32 kSlots = 0x81480400u;  // the loop's `&destination` cells
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

// Sizes around the chunk (0x2000) and the 32-byte alignment, and a large block archive's (Westopolis 110,752).
const u32 kSizes[] = {1u, 31u, 32u, 33u, 0x7FFu, 0x800u, 0x801u, 0x1FE1u, 0x1FFFu, 0x2000u, 0x2001u, 0x201Fu,
                      0x2020u, 0x3FFFu, 0x4000u, 0x4001u, 0x4F3Bu, 0x10000u, 110752u, 0x1B0A5u};

std::filesystem::path mount_root() {
    const auto dir = gekko::test_support::scratch_directory("gekko-native-bg-read-tests");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    for (std::size_t i = 0; i < std::size(kSizes); ++i) {
        std::ofstream out(dir / ("f" + std::to_string(i) + ".one"), std::ios::binary | std::ios::trunc);
        std::vector<char> bytes(kSizes[i]);
        for (u32 j = 0; j < kSizes[i]; ++j) bytes[j] = static_cast<char>((j * 131u + i * 7u + (j >> 9)) & 0xFFu);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    return dir;
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
    u32 yields = 0;
    u32 arena = kArena;
    RecompiledCode code;
    std::vector<RecompiledCode::Entry> entries;
    std::vector<u8> initial_records;

    static void yield(Cpu& c) {
        ++g_machine->yields;
        c.pc = c.lr;
    }
    static void error_callback(Cpu& c) {
        g_machine->log.push_back("errcb " + hex(c.gpr[3]) + " " + hex(c.gpr[4]));
        c.gpr[3] = 1u;
        c.pc = c.lr;
    }
    static void allocate(Cpu& c) {
        g_machine->log.push_back("alloc " + hex(c.gpr[3]) + " " + hex(c.gpr[4]));
        const u32 at = g_machine->arena;
        g_machine->arena += (c.gpr[3] + 0x3Fu) & ~0x1Fu;
        c.gpr[3] = at;
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

    Machine(const std::vector<u8>& dol, const std::filesystem::path& root, bool native) {
        const auto word = [&](std::size_t offset) { return load_be32(dol.data() + offset); };
        for (u32 i = 0; i < 18u; ++i) {
            const u32 offset = word(i * 4u), address = word(0x48u + i * 4u), size = word(0x90u + i * 4u);
            if (!size) continue;
            require(std::uint64_t(offset) + size <= dol.size(), "invalid DOL extent");
            bus.write_block(address, dol.data() + offset, size);
        }
        // The words the loop, RwFread and the device are, as recovered (docs/STAGE_LOADING.md 9d,
        // docs/RTGCNFS_SPEC.md): stwu r1,-0x20(r1) / li r7,0x2000 at the one caller / stwu r1,-0x10(r1).
        require(bus.read32(kLoop) == 0x9421FFE0u && bus.read32(0x800456C0u) == 0x38E02000u &&
                    bus.read32(kFread) == 0x9421FFF0u && bus.read32(kRead) == 0x9421FFC0u &&
                    bus.read32(kGlobal) == 1u,
                "unexpected instruction evidence");
        std::string error;
        std::vector<u8> identity(0x20u, 0u);
        require(shadow::backends::mount_host_directory(assets, root, identity, error), "the fixture's assets did not mount");
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
                   {kAlloc, 4u, allocate, "RenderWare allocator stand-in"},
                   {kErrorCallback, 4u, error_callback, "drive-error callback stand-in"},
                   {kTrigger, 4u, trigger, "rtFSTriggerFileOustandingOpCallback stand-in"}};
        if (native) {
            entries.push_back({kLoop, 4u, served<kLoop>, "TBGRead's chunked read"});
            entries.push_back({kOpen, 4u, served<kOpen>, "open"});
            entries.push_back({kClose, 4u, served<kClose>, "close"});
            entries.push_back({kRead, 4u, served<kRead>, "read"});
            entries.push_back({kReadChunk, 4u, served<kReadChunk>, "read chunk"});
            entries.push_back({kReadTransfer, 4u, served<kReadTransfer>, "read transfer"});
            entries.push_back({kSync, 4u, served<kSync>, "sync"});
            entries.push_back({kAbort, 4u, served<kAbort>, "abort"});
        } else {
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
        const u32 table[][2] = {{0x14u, 0x8047B818u}, {0x18u, 0x8047B844u}, {0x1Cu, kGetDeviceName},
                                {0x28u, kOpen}, {0x2Cu, kClose}, {0x30u, kRead}, {0x34u, 0x8047C0A0u},
                                {0x38u, kSetPosition}, {0x3Cu, kSync}, {0x40u, kAbort}, {0x44u, 0x8047A898u},
                                {0x48u, 0x8047C3FCu}, {0x4Cu, 0x8047C43Cu}};
        std::mt19937_64 random(0xB64Dull);
        for (u32 i = 0; i < 0x64u; i += 4u) bus.write32(kFs + i, static_cast<u32>(random()));
        for (const auto& slot : table) bus.write32(kFs + slot[0], slot[1]);
        bus.write32(kFs + 0x04u, kRecords);
        bus.write32(kFs + 0x08u, 2u);
        bus.write32(kFs + 0x10u, 0u);
        bus.write32(kFs + 0x58u, kFiles);
        bus.write32(kFs + 0x5Cu, kErrorCallback);
        bus.write32(kFs + 0x60u, kErrorData);
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
        std::vector<u8> fill(kRecords * kCacheSize);
        for (auto& v : fill) v = static_cast<u8>(random());
        bus.write_block(kCaches, fill.data(), kRecords * kCacheSize);
        std::vector<u8> pattern(kBuffersSize);
        for (u32 i = 0; i < kBuffersSize; ++i) pattern[i] = static_cast<u8>(i * 29u + 0x5Au);
        bus.write_block(kBuffers, pattern.data(), kBuffersSize);
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
        while (cpu.pc != kFinish && !cpu.stopped() && steps++ < 200'000'000ull) cpu.run_steps(1u);
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
        check(false, where + ": guest call " + std::to_string(k) + " original '" +
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
    same(kBuffers, kBuffersSize, "the destinations");
    same(kSlots, 0x100u, "the destination cells");
    // The stack below the caller: the loop's, RwFread's, read's and the transfer's frames are the original's (the game
    // copies stack leftovers into heap records). The OSYieldThread stand-in writes no frame on either machine.
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
        std::printf("usage: native_bg_read_tests <sys/main.dol>\n");
        return 2;
    }
#if defined(_WIN32)
    _putenv_s("GEKKO_NATIVE_BG_READ", "1");
#else
    setenv("GEKKO_NATIVE_BG_READ", "1", 1);
#endif
    try {
        const std::vector<u8> dol = read_file(argv[1]);
        require(dol.size() > 0x100u, "cannot read the DOL");
        const auto root = mount_root();
        Machine original(dol, root, false), owner(dol, root, true);
        int steps = 0;
        u32 original_yields = 0, owner_yields = 0, reads = 0;
        u64 bytes = 0;
        const auto both = [&](const std::string& what, u32 entry, std::initializer_list<u32> args) {
            const u32 ya = original.yields, yb = owner.yields;
            const bool a = original.call(entry, args);
            const bool b = owner.call(entry, args);
            check(a, what + ": the original did not return (" + original.cpu.stop().detail + ")");
            check(b, what + ": the owner did not return (" + owner.cpu.stop().detail + ")");
            if (a && b) compare(original, owner, what, true);
            if (entry == kLoop) {
                original_yields += original.yields - ya;
                owner_yields += owner.yields - yb;
                check(owner.yields == yb, what + ": the owner yielded");
                ++reads;
                bytes += original.cpu.gpr[3];
            }
            ++steps;
            return original.cpu.gpr[3];
        };
        const auto text = [&](u32 slot, const std::string& s) {
            original.put_string(kStrings + slot * 0x40u, s);
            owner.put_string(kStrings + slot * 0x40u, s);
            return kStrings + slot * 0x40u;
        };
        const auto cell = [&](u32 slot, u32 destination) {
            original.bus.write32(kSlots + slot * 4u, destination);
            owner.bus.write32(kSlots + slot * 4u, destination);
            return kSlots + slot * 4u;
        };
        const auto record = [](u32 i) { return kFiles + i * kRecord; };
        u32 slot = 0, dest = kBuffers;
        // Every size, read from position 0 by the game's chunk (0x2000) into an aligned destination, an unaligned
        // one, and none (allocated).
        for (std::size_t i = 0; i < std::size(kSizes); ++i) {
            const u32 name = text(static_cast<u32>(i), "dvd:f" + std::to_string(i) + ".one");
            const std::string size = " (" + std::to_string(kSizes[i]) + " bytes)";
            const u32 f = record(static_cast<u32>(i % kRecords));
            for (const int kind : {0, 1, 2}) {
                both("open" + size, kOpen, {kFs, f, name, 1u});
                const u32 target = kind == 0 ? dest : kind == 1 ? dest + 3u : 0u;
                both(std::string(kind == 0 ? "aligned" : kind == 1 ? "unaligned" : "allocated") + size, kLoop,
                     {0x81470000u, f, cell(slot++, target), 0u, 0x2000u});
                dest += (kSizes[i] + 0x5Fu) & ~0x1Fu;
                if (dest > kArena - 0x30000u) dest = kBuffers;
                both("close" + size, kClose, {f});
            }
        }
        // Other chunk sizes (0 = one read of the whole file), a nonzero start position, and a cache left valid.
        const u32 big = text(40u, "dvd:f18.one");  // 110,752 bytes
        for (const u32 chunk : {0u, 0x100u, 0x800u, 0x1000u, 0x3000u, 0x7FFFFFFFu}) {
            both("open for chunk " + hex(chunk), kOpen, {kFs, record(0), big, 1u});
            both("chunk " + hex(chunk), kLoop, {0x81470000u, record(0), cell(slot++, kBuffers + 0x40u), 0u, chunk});
            both("close for chunk " + hex(chunk), kClose, {record(0)});
        }
        both("open (position 0x1234)", kOpen, {kFs, record(1), big, 1u});
        both("setPosition BEGIN 0x1234", kSetPosition, {record(1), 0x1234u, 1u});
        both("chunked read from 0x1234 (clamped at the end)", kLoop,
             {0x81470000u, record(1), cell(slot++, kBuffers + 0x80u), 0u, 0x2000u});
        both("close (position 0x1234)", kClose, {record(1)});
        both("open (cache)", kOpen, {kFs, record(2), big, 1u});
        both("read 7 bytes (fills the cache)", kRead, {record(2), kBuffers + 0x20003u, 7u});
        both("chunked read with a valid cache", kLoop,
             {0x81470000u, record(2), cell(slot++, kBuffers + 0x20003u + 7u), 0u, 0x2000u});
        both("close (cache)", kClose, {record(2)});
        both("no file", kLoop, {0x81470000u, 0u, cell(slot++, kBuffers), 0u, 0x2000u});
        check(original_yields > 0u && owner_yields == 0u, "yields: the original must yield, the owner must not");
        const auto& stats = owner.device->stats();
        check(stats.background_reads == reads - 1u, "the owner's background-read count");
        std::printf("native background read: %d calls against the original in the interpreter (%u chunked reads, "
                    "%llu bytes, %llu chunks; the original yielded %u times, the owner 0), %d failure(s)\n",
                    steps, reads, static_cast<unsigned long long>(bytes),
                    static_cast<unsigned long long>(stats.background_chunks), original_yields, g_failures);
    } catch (const std::exception& error) {
        std::printf("ERROR: %s\n", error.what());
        return 1;
    }
    return g_failures == 0 ? 0 : 1;
}
