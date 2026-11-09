// liveness-roots: reference-interpreter-only
// The game's directory scan 0x80044ED4 (docs/DISC_SYSTEM_MAP.md, "Cut 2"): the native owner against the ORIGINAL
// words of the game's DOL in the reference interpreter, on the same guest image, namespace and register file.
//
// The ORIGINAL machine runs the scan and everything it calls as original words: strcpy, strstr and the DVD SDK's
// DVDOpenDir / DVDReadDir, which walk the namespace published in the layout those words read
// (test_support::fst_publication, with the SDK globals pointing at it, as native_file_api_reference_tests does).
// Only DVDOpen / DVDClose (still native owners of the file API), the game's memcpy (its native owner) and the
// game's heap are served, the heap by the same deterministic stand-in on both machines: alloc(size, heap) bumps
// through a block of seeded bytes and logs (size, heap, r1), free(block) logs (block, r1). The NATIVE machine
// publishes the same image and runs the owner. Equal means:
//   * the result, r1, r2, r13-r31, the non-volatile CR fields, LR and PC;
//   * the heap calls: the same sequence, arguments and stack pointer;
//   * every byte of the heap block, the out slot, the published image and the caller's frame at and above r1.
// Bytes below r1 (the callee frames: the original's DVDDir, DVDFileInfo and saved registers) are dead by the ABI
// and not compared.
//
// usage: native_directory_scan_tests <sys/main.dol>

#include "gekko/asset_mount.h"
#include "shadow/backends/storage_backends.h"
#include "gekko/cpu.h"
#include "gekko/game_memory.h"
#include "gekko/guest_memory.h"
#include "gekko/native_directory_scan.h"
#include "gekko/native_file_api.h"
#include "support/fst_oracle.h"

#include <algorithm>
#include <array>
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

constexpr u32 kScan = 0x80044ED4u;
constexpr u32 kHeapAlloc = 0x80049BB8u, kHeapFree = 0x80049B5Cu, kMemcpy = 0x800054F4u;
constexpr u32 kOpen = 0x8037AAA0u, kClose = 0x8037AB68u;
constexpr u32 kOSReport = 0x803731CCu, kOSPanic = 0x8037324Cu;
constexpr u32 kFinish = 0x817FF000u;  // the call's return address: never executed
constexpr u32 kStack = 0x817E0000u;
constexpr u32 kHeapBase = 0x81000000u, kHeapSize = 0x00200000u;
constexpr u32 kOut = 0x81600000u, kPath = 0x81600100u, kFilter = 0x81600200u;
constexpr u32 kFst = 0x81400000u;  // the published namespace the original SDK words walk
constexpr u32 kR13 = 0x805EC500u, kCap = kR13 - 0x7BF0u;
// The SDK globals (r13-relative) the original DVDOpenDir / DVDReadDir read (native_file_api_reference_tests).
constexpr u32 kFstStart = 0x5B44u, kFstStringStart = 0x5B48u, kMaxEntry = 0x5B4Cu, kCurrentDir = 0x5B50u;
constexpr u32 kLongFileNameFlag = 0x5B54u;

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

void write_file(const std::filesystem::path& path, u32 size) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    const std::vector<char> bytes(size, 'x');
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// The fixture tree. sub/a.bin shares its bare name with the root's a.bin (the original resolves a bare name from
// the root, so it reports the root's length), sub/x.sfd has none at the root (-1).
std::filesystem::path mount_root() {
    const auto dir = gekko::test_support::scratch_directory("gekko-native-directory-scan-tests");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    write_file(dir / "E1001.sfd", 0x1234u);
    write_file(dir / "a.bin", 0x40u);
    write_file(dir / "movie.sfd.txt", 0x10u);
    write_file(dir / "Z.TXT", 0x20u);
    write_file(dir / "sub" / "a.bin", 0x99u);
    write_file(dir / "sub" / "x.sfd", 0x77u);
    std::filesystem::create_directories(dir / "zdir");
    return dir;
}

std::vector<u8> read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::vector<u8>{std::istreambuf_iterator<char>(input), {}};
}

struct HeapCall {
    u32 function, a0, a1, sp;
    bool operator==(const HeapCall& o) const {
        return function == o.function && a0 == o.a0 && a1 == o.a1 && sp == o.sp;
    }
};

struct Machine;
Machine* g_machine = nullptr;  // the machine whose stand-in is running (one runs at a time)

struct Machine {
    GuestMemory bus;
    Cpu cpu{bus};
    assets::AssetSystem assets;
    NativeFileApi files{cpu, bus, assets};
    GameMemory memory_owners{cpu};
    std::unique_ptr<NativeDirectoryScan> scan;
    std::vector<HeapCall> heap_calls;
    std::vector<u8> published;
    std::vector<std::string> reports;  // the SDK's OSReport warnings (the original machine)
    u32 heap_next = kHeapBase;
    RecompiledCode code;
    std::vector<RecompiledCode::Entry> entries;

    static void alloc(Cpu& c) {
        Machine& m = *g_machine;
        m.heap_calls.push_back({kHeapAlloc, c.gpr[3], c.gpr[4], c.gpr[1]});
        const u32 block = m.heap_next;
        m.heap_next = (m.heap_next + c.gpr[3] + 0x3Fu) & ~0x1Fu;
        c.gpr[3] = block;
        c.pc = c.lr;
    }
    static void report(Cpu& c) {
        g_machine->reports.push_back("OSReport");
        c.pc = c.lr;
    }
    static void panic(Cpu& c) { c.request_stop(StopReason::ExecutionFault, "OSPanic in the original SDK words"); }
    static void release(Cpu& c) {
        Machine& m = *g_machine;
        m.heap_calls.push_back({kHeapFree, c.gpr[3], 0u, c.gpr[1]});
        c.pc = c.lr;
    }
    // Each served entry gets its own thunk: a native call (recomp::call_indirect) enters it without setting pc.
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
        // The words the scan is, as recovered (DISC_SYSTEM_MAP.md cut 2).
        require(bus.read32(kScan) == 0x9421FF80u && bus.read32(0x80044EF4u) == 0x4833623Du &&
                    bus.read32(0x80044FB4u) == 0x4833623Du && bus.read32(kCap) == 0x500u,
                "unexpected directory-scan instruction evidence");
        std::string error;
        std::vector<u8> identity(0x20u, 0u);
        require(shadow::backends::mount_host_directory(assets, mount_root(), identity, error), "the fixture's assets did not mount");
        cpu.reset();
        entries = {{kHeapAlloc, 4u, alloc, "heap alloc stand-in"},
                   {kHeapFree, 4u, release, "heap free stand-in"},
                   {kMemcpy, 4u, served<kMemcpy>, "memcpy"},
                   {kOpen, 4u, served<kOpen>, "DVDOpen"},
                   {kClose, 4u, served<kClose>, "DVDClose"},
                   {kOSReport, 4u, report, "OSReport stand-in"},
                   {kOSPanic, 4u, panic, "OSPanic stand-in"}};
        if (native) entries.push_back({kScan, 4u, served<kScan>, "directory scan"});
        std::sort(entries.begin(), entries.end(),
                  [](const RecompiledCode::Entry& a, const RecompiledCode::Entry& b) { return a.address < b.address; });
        code.install(entries.data(), entries.size());
        cpu.attach_recompiled(&code);
        files.install();
        published = test_support::fst_publication(assets);
        memory_owners.install();
        if (native) {
            scan = std::make_unique<NativeDirectoryScan>(cpu, assets);
            scan->install();
        }
    }

    void put_string(u32 at, const std::string& text) {
        for (std::size_t i = 0; i < text.size(); ++i) bus.write8(at + static_cast<u32>(i), static_cast<u8>(text[i]));
        bus.write8(at + static_cast<u32>(text.size()), 0u);
    }

    // The same seeded image and register file for both machines, then the call to its return.
    bool call(u64 seed, const std::string& path, const std::string* filter, u32 cap) {
        std::mt19937_64 random(seed);
        std::vector<u8> fill(kHeapSize);
        for (auto& v : fill) v = static_cast<u8>(random());
        bus.write_block(kHeapBase, fill.data(), kHeapSize);
        bus.write_block(kFst, published.data(), static_cast<u32>(published.size()));
        const u32 count = assets.names().count();
        bus.write32(kR13 + kFstStart, kFst);
        bus.write32(kR13 + kFstStringStart, kFst + count * 12u);
        bus.write32(kR13 + kMaxEntry, count);
        bus.write32(kR13 + kCurrentDir, 0u);
        bus.write32(kR13 + kLongFileNameFlag, 1u);  // OSInit's final value
        std::vector<u8> frame(0x200u);
        for (auto& v : frame) v = static_cast<u8>(random());
        bus.write_block(kStack - 0x100u, frame.data(), static_cast<u32>(frame.size()));
        bus.write32(kOut, 0xDEADBEEFu);
        bus.write32(kCap, cap);
        put_string(kPath, path);
        if (filter != nullptr) put_string(kFilter, *filter);
        heap_calls.clear();
        reports.clear();
        heap_next = kHeapBase;
        for (u32 r = 0; r < 32u; ++r) cpu.gpr[r] = static_cast<u32>(random());
        cpu.gpr[1] = kStack;
        cpu.gpr[2] = 0x805FA780u;
        cpu.gpr[13] = kR13;
        cpu.gpr[4] = kOut;
        cpu.gpr[5] = kPath;
        cpu.gpr[6] = filter != nullptr ? kFilter : 0u;
        cpu.cr = static_cast<u32>(random());
        cpu.xer = static_cast<u32>(random()) & 0xE000007Fu;
        cpu.ctr = static_cast<u32>(random());
        cpu.set_msr(cpu.msr() | kMsrDR | kMsrIR | kMsrFP);
        cpu.lr = kFinish;
        cpu.pc = kScan;
        g_machine = this;
        u64 steps = 0;
        while (cpu.pc != kFinish && !cpu.stopped() && steps++ < 50'000'000ull) cpu.run_steps(1u);
        g_machine = nullptr;
        return cpu.pc == kFinish && !cpu.stopped();
    }
};

void compare(Machine& a, Machine& b, const std::string& where) {
    check(a.cpu.gpr[3] == b.cpu.gpr[3], where + ": count original " + std::to_string(a.cpu.gpr[3]) + " owner " +
                                            std::to_string(b.cpu.gpr[3]));
    for (const u32 r : {1u, 2u, 13u}) check(a.cpu.gpr[r] == b.cpu.gpr[r], where + ": r" + std::to_string(r));
    for (u32 r = 14u; r < 32u; ++r) {
        check(a.cpu.gpr[r] == b.cpu.gpr[r],
              where + ": r" + std::to_string(r) + " original " + hex(a.cpu.gpr[r]) + " owner " + hex(b.cpu.gpr[r]));
    }
    check((a.cpu.cr & 0x00FFF000u) == (b.cpu.cr & 0x00FFF000u), where + ": the non-volatile CR fields");
    check(a.cpu.lr == b.cpu.lr && a.cpu.pc == b.cpu.pc, where + ": lr / pc");
    check(a.heap_calls == b.heap_calls, where + ": the heap calls (" + std::to_string(a.heap_calls.size()) + " vs " +
                                            std::to_string(b.heap_calls.size()) + ")");
    const auto same = [&](u32 address, u32 size, const char* what) {
        std::vector<u8> x(size), y(size);
        a.bus.peek_block(address, x.data(), size);
        b.bus.peek_block(address, y.data(), size);
        for (u32 i = 0; i < size; ++i) {
            if (x[i] != y[i]) {
                check(false, where + ": " + what + " differs first at " + hex(address + i));
                return;
            }
        }
    };
    same(kHeapBase, kHeapSize, "the heap");
    same(kOut, 4u, "the out slot");
    same(kStack, 0x100u, "the caller's frame");
    same(kFst, static_cast<u32>(a.published.size()), "the published namespace");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::printf("usage: native_directory_scan_tests <sys/main.dol>\n");
        return 2;
    }
    try {
        const std::vector<u8> dol = read_file(argv[1]);
        require(dol.size() > 0x100u, "cannot read the DOL");
        const std::string sfd = ".sfd", none_match = "nothing-has-this";
        struct Case {
            const char* path;
            const std::string* filter;
            u32 expected;
        };
        const std::vector<Case> cases{
            {".", nullptr, 6u},        // E1001.sfd a.bin movie.sfd.txt sub Z.TXT zdir
            {".", &sfd, 2u},           // E1001.sfd movie.sfd.txt
            {"/", nullptr, 6u},        // the root by its other name
            {"sub", nullptr, 2u},      // a.bin (the root's length), x.sfd (-1)
            {"zdir", nullptr, 0u},     // empty
            {".", &none_match, 0u},    // nothing matches
            {"missing", nullptr, 0u},  // not there: *out = 0
            {"a.bin", nullptr, 0u},    // a file, not a directory
        };
        int index = 0;
        for (const Case& c : cases) {
            Machine original(dol, false), owner(dol, true);
            const u64 seed = 0xD15C0000ull + static_cast<u64>(index++);
            const std::string where = std::string("scan '") + c.path + "'" + (c.filter ? " filter '" + *c.filter + "'" : "");
            const bool a = original.call(seed, c.path, c.filter, 0x500u);
            const bool b = owner.call(seed, c.path, c.filter, 0x500u);
            check(a, where + ": the original did not return (" + original.cpu.stop().detail + ")");
            check(b, where + ": the owner did not return (" + owner.cpu.stop().detail + ")");
            if (a && b) compare(original, owner, where);
            check(original.cpu.gpr[3] == c.expected, where + ": count " + std::to_string(original.cpu.gpr[3]) +
                                                         ", expected " + std::to_string(c.expected));
            check(owner.scan->stats().scans == 1u, where + ": the owner served the call");
        }
        // The disc's quirk, pinned: sub/a.bin reports the root's a.bin length, sub/x.sfd -1.
        {
            Machine owner(dol, true);
            require(owner.call(0x5EEDull, "sub", nullptr, 0x500u), "the sub scan did not return");
            const u32 records = owner.bus.read32(kOut);
            check(owner.bus.read32(records + 0x100u) == 0x40u && owner.bus.read32(records + 0x104u) == 2u,
                  "sub/a.bin carries the root a.bin's length (the bare name resolves from the root)");
            check(owner.bus.read32(records + 0x108u + 0x100u) == 0xFFFFFFFFu, "sub/x.sfd has no root namesake: -1");
        }
        // More matching entries than the game allocates: refused, naming it (the original overran its block).
        {
            Machine owner(dol, true);
            check(!owner.call(0xCA9ull, ".", nullptr, 2u) && owner.cpu.stopped() &&
                      owner.cpu.stop().detail.find("more than the 2 records") != std::string::npos,
                  "a directory larger than the capacity is refused: " + owner.cpu.stop().detail);
        }
        std::printf("native directory scan: %zu cases against the original in the interpreter, %d failure(s)\n",
                    cases.size(), g_failures);
    } catch (const std::exception& error) {
        std::printf("ERROR: %s\n", error.what());
        return 1;
    }
    return g_failures == 0 ? 0 : 1;
}
