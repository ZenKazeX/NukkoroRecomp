// The native file API against the original Nintendo DVD SDK words.
//
// liveness-roots: this test enters no generated code - every SDK address it
// names runs as ORIGINAL words in the reference interpreter - so its literals
// must not keep the replaced SDK bodies in the shipping image:
// liveness-roots: reference-interpreter-only
//
// Two machines load the original DOL. ORIGINAL runs the SDK's own instruction
// words in the reference interpreter, over the namespace published in the
// layout those words walk (test_support::fst_publication, at kFst, with
// the SDK globals pointing at it). NATIVE serves the same calls with the
// production owners (NativeFileApi over AssetSystem / AssetNamespace) and no
// SDK code at all. Each case is one whole call on both, compared on what the
// API's callers observe:
//   * the result in r3;
//   * every byte of the game's memory outside the call's own stack window (the original's
//     frames and its caller's LR save word are its scratch; the owners have
//     none) - so every DVDFileInfo / command block / DVDDir / DVDDirEntry field
//     and every byte a name pointer leads to;
//   * the game-visible guest calls: callbacks with their arguments;
//   * the SDK's warnings (OSReport) against the native miss record, and its
//     asserts (OSPanic) against the native stop.
// The disc's own namespace on the NATIVE side is built from the HOST TREE (the
// production source); synthetic tables cover the corners the disc does not
// have. Intended differences are asserted by value and counted separately -
// never masked: the SDK-internal OSWakeupThread on its thread queue, the SDK's
// private globals, the states the API never produces (in-flight, paused,
// fatal drive) and the SDK's alarms, which the API does not own.

#include "test_scratch.h"
#include "gekko/asset_mount.h"
#include "shadow/backends/storage_backends.h"
#include "support/fst_oracle.h"
#include "gekko/asset_namespace.h"
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_file_api.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using gekko::s32;
using gekko::u32;
using gekko::u64;
using gekko::u8;
namespace assets = gekko::assets;

constexpr u32 kSda = 0x805EC500u;
constexpr u32 kSda2 = 0x805FA780u;
constexpr u32 kStackTop = 0x817E0000u;
constexpr u32 kStackWindow = 0x8000u;
constexpr u32 kFinish = 0x817FF000u;   // sentinel return address (holds `b .`)
constexpr u32 kFst = 0x81200000u;      // where the namespace is published / pooled
constexpr u32 kScratch = 0x81100000u;  // strings, file infos, buffers
constexpr u32 kCallbackSink = 0x8037B300u;

// SDK globals (r13-relative) the ORIGINAL machine needs to walk the image.
constexpr u32 kFstStart = 0x5B44u, kFstStringStart = 0x5B48u, kMaxEntry = 0x5B4Cu;
constexpr u32 kCurrentDir = 0x5B50u, kLongFileNameFlag = 0x5B54u, kThreadQueue = 0x5B58u;
constexpr u32 kFatalErrorFunc = 0x5BB0u, kAutoInvalidation = 0x2734u;

constexpr u32 kOSReport = 0x803731CCu, kOSPanic = 0x8037324Cu;
constexpr u32 kOSSleepThread = 0x803792C4u, kOSWakeupThread = 0x803793B0u;
constexpr u32 kReadAbs = 0x8037CF40u;

constexpr u32 kConvertPath = 0x8037A7ACu, kOpen = 0x8037AAA0u, kClose = 0x8037AB68u;
constexpr u32 kReadAsync = 0x8037AF04u, kReadSync = 0x8037AFF4u;
constexpr u32 kTransferred = 0x8037B294u;
constexpr u32 kBlockStatus = 0x8037D390u, kDriveStatus = 0x8037D3DCu, kAutoInvalidationEntry = 0x8037D488u;
constexpr u32 kCancelAsync = 0x8037D4E8u, kCancel = 0x8037D764u, kDiskId = 0x8037D834u;
constexpr u32 kCheckDisk = 0x8037D83Cu, kTestAlarm = 0x8037DA50u, kAutoFatal = 0x8037DF9Cu;
constexpr u32 kOpenWarning = 0x80562720u;

int g_failures = 0;
int g_cases = 0;
int g_documented = 0;

void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

std::vector<u8> read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::vector<u8>{std::istreambuf_iterator<char>(input), {}};
}

std::string hex(u32 value) {
    char text[16];
    std::snprintf(text, sizeof(text), "%08X", value);
    return text;
}

std::string guest_string(gekko::Cpu& c, u32 address, u32 limit = 512u) {
    std::string text;
    for (u32 i = 0; i < limit; ++i) {
        const u8 b = c.mem_read8(address + i);
        if (b == 0u) break;
        text.push_back(static_cast<char>(b));
    }
    return text;
}

void check(bool ok, const std::string& what) {
    ++g_cases;
    if (ok) return;
    ++g_failures;
    if (g_failures <= 40) std::fprintf(stderr, "MISMATCH %s\n", what.c_str());
}

// An intended difference: asserted by value on both sides, counted apart.
void documented(bool ok, const std::string& what) {
    ++g_documented;
    check(ok, "documented difference broken: " + what);
}

std::vector<std::string>* g_native_calls = nullptr;

// The NATIVE machine's callback sink: translated code (the API only calls back
// into code that has it), recording the call and returning.
void native_sink(gekko::Cpu& c) {
    if (g_native_calls) g_native_calls->push_back("callback r3=" + hex(c.gpr[3]) + " r4=" + hex(c.gpr[4]));
    c.pc = c.lr;
}

// The game's memory a comparison covers: the program image and the OS heap
// region after it - one window range over two regions (offset = address -
// kMemoryBegin).
constexpr u32 kMemoryBegin = gekko::GuestMemory::kImageBase;
constexpr u32 kMemoryEnd = gekko::GuestMemory::kLegacyHeapEnd;
constexpr u32 kMemorySize = kMemoryEnd - kMemoryBegin;

struct Machine {
    gekko::GuestMemory bus;
    gekko::Cpu cpu{bus};
    std::vector<std::string> calls;
    std::vector<u8> image;
    bool ended_inside = false;
    static constexpr std::size_t kPage = 4096u;

    explicit Machine(const std::vector<u8>& dol) {
        const auto word = [&](std::size_t offset) { return gekko::load_be32(dol.data() + offset); };
        for (u32 i = 0; i < 18u; ++i) {
            const u32 offset = word(i * 4u), address = word(0x48u + i * 4u), size = word(0x90u + i * 4u);
            if (!size) continue;
            require(static_cast<u64>(offset) + size <= dol.size(), "invalid DOL extent");
            bus.write_block(address, dol.data() + offset, size);
        }
        bus.write32(kFinish, 0x48000000u);
    }
    void snapshot() { image = bus.window_bytes(kMemoryBegin, kMemoryEnd); }
    // The bytes of one snapshot page as they are now (a page never crosses a region).
    const u8* page_now(std::size_t page) const {
        return bus.peek_pointer(kMemoryBegin + static_cast<u32>(page), kPage);
    }
    void restore(const std::function<void(gekko::Cpu&)>& setup) {
        for (std::size_t page = 0; page < image.size(); page += kPage) {
            if (std::memcmp(page_now(page), image.data() + page, kPage) != 0)
                bus.write_block(kMemoryBegin + static_cast<u32>(page), image.data() + page, kPage);
        }
        ended_inside = false;
        cpu.clear_stop();
        cpu.reset();
        cpu.gpr[1] = kStackTop;
        cpu.gpr[2] = kSda2;
        cpu.gpr[13] = kSda;
        calls.clear();
        setup(cpu);
    }
    // One call, from `entry` to the sentinel return (or a stop), a dispatch at
    // a time: one interpreted instruction, or one owner or fixture hook.
    u32 call(u32 entry, std::initializer_list<u32> args) {
        u32 r = 3u;
        for (const u32 value : args) cpu.gpr[r++] = value;
        cpu.pc = entry;
        cpu.lr = kFinish;
        unsigned steps = 0;
        while (cpu.pc != kFinish && !cpu.stopped() && steps++ < 20000000u) cpu.run_steps(1u);
        if (cpu.stopped()) return cpu.gpr[3];
        require(cpu.pc == kFinish, "call did not return");
        require(ended_inside || cpu.gpr[1] == kStackTop, "call left r1 moved");
        return cpu.gpr[3];
    }
    using Dirty = std::vector<std::pair<std::size_t, std::vector<u8>>>;
    Dirty dirty_pages() const {
        Dirty pages;
        for (std::size_t page = 0; page < image.size(); page += kPage) {
            const u8* now = page_now(page);
            if (std::memcmp(now, image.data() + page, kPage) != 0)
                pages.push_back({page, std::vector<u8>(now, now + kPage)});
        }
        return pages;
    }
};

// Bytes no comparison reads: the stack window (and the caller's LR save word)
// and any listed SDK-private global.
bool excluded(std::size_t offset, const std::vector<u32>& private_words) {
    const u32 low = kStackTop - kStackWindow - kMemoryBegin;
    const u32 high = kStackTop + 8u - kMemoryBegin;
    if (offset >= low && offset < high) return true;
    for (const u32 word : private_words) {
        const u32 at = word - kMemoryBegin;
        if (offset >= at && offset < at + 4u) return true;
    }
    return false;
}

// Both machines start every case from byte-identical memory (same DOL, same
// setup writes); compare what each call left behind.
bool same_memory(const Machine& a, const Machine& b, std::string& where,
                 const std::vector<u32>& private_words = {}) {
    const Machine::Dirty da = a.dirty_pages(), db = b.dirty_pages();
    std::size_t ia = 0, ib = 0;
    while (ia < da.size() || ib < db.size()) {
        const std::size_t pa = ia < da.size() ? da[ia].first : SIZE_MAX;
        const std::size_t pb = ib < db.size() ? db[ib].first : SIZE_MAX;
        const std::size_t page = pa < pb ? pa : pb;
        const u8* left = pa == page ? da[ia].second.data() : a.image.data() + page;
        const u8* right = pb == page ? db[ib].second.data() : b.image.data() + page;
        for (std::size_t i = 0; i < Machine::kPage; ++i) {
            if (excluded(page + i, private_words)) continue;
            if (left[i] != right[i]) {
                where = "memory differs at 0x" + hex(static_cast<u32>(kMemoryBegin + page + i));
                return false;
            }
        }
        if (pa == page) ++ia;
        if (pb == page) ++ib;
    }
    return true;
}

void put_string(gekko::Cpu& c, u32 address, const std::string& text) {
    for (std::size_t i = 0; i < text.size(); ++i) c.mem_write8(address + static_cast<u32>(i), static_cast<u8>(text[i]));
    c.mem_write8(address + static_cast<u32>(text.size()), 0u);
}

// ---- the pair of machines over one namespace -----------------------------------

struct Pair {
    Machine original;
    Machine native;
    std::unique_ptr<assets::AssetSystem> files;
    std::unique_ptr<gekko::NativeFileApi> api;
    std::vector<u8> published;  // the image both machines carry at kFst
    gekko::RecompiledCode table;
    gekko::RecompiledCode::Entry sink{kCallbackSink, 4u, native_sink, "callback sink"};

    Pair(const std::vector<u8>& dol, assets::AssetNamespace names, const std::filesystem::path& files_root)
        : original(dol), native(dol) {
        files = std::make_unique<assets::AssetSystem>();
        std::string error;
        require(shadow::backends::mount_host_directory(*files, files_root, std::move(names),
                                                     std::vector<u8>(0x20u, 0x11u), error),
                "the namespace did not mount");
        published = gekko::test_support::fst_publication(*files);

        // ORIGINAL: the SDK's words; the OS calls they make are recorded.
        gekko::Cpu& oc = original.cpu;
        oc.install_hle(kOSReport, [this](gekko::Cpu& c) {
            original.calls.push_back("OSReport fmt=" + hex(c.gpr[3]) + " a=" + guest_string(c, c.gpr[4]) +
                                     " b=" + guest_string(c, c.gpr[5]));
            return true;
        }, "fixture OSReport");
        oc.install_hle(kOSPanic, [this](gekko::Cpu& c) {
            original.calls.push_back("OSPanic line=" + hex(c.gpr[4]));
            c.lr = kFinish;
            original.ended_inside = true;
            return true;
        }, "fixture OSPanic");
        oc.install_hle(kOSWakeupThread, [this](gekko::Cpu& c) {
            original.calls.push_back("OSWakeupThread " + hex(c.gpr[3]));
            return true;
        }, "fixture OSWakeupThread");
        oc.install_hle(kOSSleepThread, [this](gekko::Cpu& c) {
            original.calls.push_back("OSSleepThread " + hex(c.gpr[3]));
            c.lr = kFinish;
            original.ended_inside = true;
            return true;
        }, "fixture OSSleepThread");
        oc.install_hle(kReadAbs, [this](gekko::Cpu& c) {
            original.calls.push_back("read block=" + hex(c.gpr[3]) + " addr=" + hex(c.gpr[4]) + " len=" +
                                     hex(c.gpr[5]) + " token=" + hex(c.gpr[6]) + " cb=" + hex(c.gpr[7]));
            c.gpr[3] = 1u;
            return true;
        }, "fixture DVDReadAbsAsyncPrio");
        oc.install_hle(kCallbackSink, [this](gekko::Cpu& c) {
            original.calls.push_back("callback r3=" + hex(c.gpr[3]) + " r4=" + hex(c.gpr[4]));
            return true;
        }, "fixture callback");

        // NATIVE: the production owners, the sink as translated code.
        table.install(&sink, 1u);
        native.cpu.attach_recompiled(&table);
        api = std::make_unique<gekko::NativeFileApi>(native.cpu, native.bus, *files);
        api->install();

        for (Machine* m : {&original, &native}) {
            m->bus.write_block(kFst, published.data(), published.size());
            m->snapshot();
        }
    }

    // Both machines: the published image, the SDK globals the original walks
    // it through (inert on the native side), then the case's own setup.
    void restore(const std::function<void(gekko::Cpu&)>& setup) {
        const u32 count = files->names().count();
        const auto common = [&](gekko::Cpu& c) {
            c.mem_write32(kSda + kFstStart, kFst);
            c.mem_write32(kSda + kFstStringStart, kFst + count * 12u);
            c.mem_write32(kSda + kMaxEntry, count);
            c.mem_write32(kSda + kCurrentDir, 0u);
            c.mem_write32(kSda + kLongFileNameFlag, 1u);  // OSInit's final value (checked in main)
            setup(c);
        };
        original.restore(common);
        native.restore(common);
        native.cpu.attach_recompiled(&table);
        g_native_calls = &native.calls;
    }
};

// ---- the comparisons -----------------------------------------------------------

void compare_path(Pair& p, const std::string& path) {
    p.restore([&](gekko::Cpu& c) { put_string(c, kScratch, path); });
    const s32 original = static_cast<s32>(p.original.call(kConvertPath, {kScratch}));
    const s32 native = p.files->names().resolve(path);
    check(p.original.calls.empty() && native == original,
          "resolve(\"" + path + "\") original " + std::to_string(original) + " native " + std::to_string(native));
}

void compare_open(Pair& p, const std::string& path) {
    constexpr u32 kInfo = kScratch + 0x400u;
    const auto setup = [&](gekko::Cpu& c) {
        put_string(c, kScratch, path);
        for (u32 i = 0; i < 0x40u; i += 4u) c.mem_write32(kInfo + i, 0xA5A5A5A5u);
    };
    p.restore(setup);
    const std::size_t misses_before = p.api->missed_paths().size();
    const u64 miss_count_before = p.api->stats().open_misses;
    const u32 original = p.original.call(kOpen, {kScratch, kInfo});
    const u32 native = p.native.call(kOpen, {kScratch, kInfo});
    std::string where;
    bool ok = native == original && same_memory(p.original, p.native, where) && p.native.calls.empty();
    const bool missed = p.api->stats().open_misses == miss_count_before + 1u;
    if (missed) {
        // The SDK's warning, with the current directory it prints ("/": the
        // API's current directory is always the root).
        const std::string expected = "OSReport fmt=" + hex(kOpenWarning) + " a=" + path + " b=/";
        ok = ok && p.original.calls == std::vector<std::string>{expected} &&
             (misses_before >= 64u || p.api->missed_paths().back() == path);
    } else {
        ok = ok && p.original.calls.empty();
    }
    check(ok, "DVDOpen(\"" + path + "\") original " + std::to_string(original) + " native " +
                  std::to_string(native) + " " + where);
}

void compare_status(Pair& p, s32 state) {
    constexpr u32 kBlock = kScratch + 0x400u;
    const auto setup = [&](gekko::Cpu& c) { c.mem_write32(kBlock + 0x0Cu, static_cast<u32>(state)); };
    p.restore(setup);
    const u32 original = p.original.call(kBlockStatus, {kBlock});
    const u32 native = p.native.call(kBlockStatus, {kBlock});
    check(native == original, "DVDGetCommandBlockStatus(state=" + std::to_string(state) + ")");
    // The drive, in the only state the port has: no fatal error, no pause,
    // nothing executing (the SDK globals that would say otherwise have no
    // writer left in the image).
    p.restore(setup);
    check(p.native.call(kDriveStatus, {}) == p.original.call(kDriveStatus, {}), "DVDGetDriveStatus");
}

void compare_transferred(Pair& p, s32 state) {
    constexpr u32 kBlock = kScratch + 0x400u;
    const auto setup = [&](gekko::Cpu& c) {
        c.mem_write32(kBlock + 0x0Cu, static_cast<u32>(state));
        c.mem_write32(kBlock + 0x20u, 0x00012340u);
        c.mem_write32(kBlock + 0x1Cu, 0x00000800u);
    };
    p.restore(setup);
    const u32 native = p.native.call(kTransferred, {kBlock});
    const bool answered = state == -1 || state == 0 || (state >= 3 && state <= 7) || state == 10 || state == 11;
    if (state == 1) {
        // BUSY reads the drive's DILENGTH: never in flight natively.
        documented(native == 0x00012340u, "DVDGetTransferredSize(BUSY) is the final count");
        return;
    }
    p.restore(setup);
    const u32 original = p.original.call(kTransferred, {kBlock});
    if (answered) {
        check(native == original, "DVDGetTransferredSize(state=" + std::to_string(state) + ")");
    } else {
        // WAITING (0) and the states the SDK answers with the block pointer:
        // never produced; the API reports the block's final count.
        documented(native == 0x00012340u && original != native,
                   "DVDGetTransferredSize(state=" + std::to_string(state) + ")");
    }
}

void compare_cancel(Pair& p, s32 state) {
    constexpr u32 kBlock = kScratch + 0x400u;
    const bool finished = state == -1 || state == 0 || state == 10;
    const bool in_flight = (state >= 1 && state <= 7) || state == 11;
    for (const u32 callback : {0u, kCallbackSink}) {
        const auto setup = [&](gekko::Cpu& c) { c.mem_write32(kBlock + 0x0Cu, static_cast<u32>(state)); };
        p.restore(setup);
        const u32 native = p.native.call(kCancelAsync, {kBlock, callback});
        if (in_flight) {
            // The SDK drives its low-level layer here; the API never leaves a
            // block in flight, so it completes the cancel at once.
            const std::vector<std::string> expected =
                callback != 0u ? std::vector<std::string>{"callback r3=00000000 r4=" + hex(kBlock)}
                               : std::vector<std::string>{};
            documented(native == 1u && p.native.calls == expected,
                       "DVDCancelAsync(state=" + std::to_string(state) + ") completes at once");
            continue;
        }
        const u32 original = p.original.call(kCancelAsync, {kBlock, callback});
        std::string where;
        check(original == 1u && native == original && p.native.calls == p.original.calls &&
                  same_memory(p.original, p.native, where),
              "DVDCancelAsync(state=" + std::to_string(state) + ", cb=" + hex(callback) + ") " + where);
    }
    if (!finished) return;
    // DVDCancel and DVDClose on a finished block: the same results; the SDK's
    // only other effect is OSWakeupThread on its own thread queue (which
    // nothing ever sleeps on - no request is ever in flight).
    for (const u32 entry : {kCancel, kClose}) {
        p.restore([&](gekko::Cpu& c) { c.mem_write32(kBlock + 0x0Cu, static_cast<u32>(state)); });
        const u32 original = p.original.call(entry, {kBlock});
        const u32 native = p.native.call(entry, {kBlock});
        std::string where;
        check(native == original && same_memory(p.original, p.native, where),
              std::string(entry == kCancel ? "DVDCancel" : "DVDClose") + "(state=" + std::to_string(state) + ") " + where);
        documented(p.native.calls.empty() &&
                       p.original.calls == std::vector<std::string>{"OSWakeupThread " + hex(kSda + kThreadQueue)},
                   "cancel wakes the SDK's thread queue only in the original");
    }
}

// A sequence of calls whose results depend on the flag the previous call left.
void compare_flag_sequence(Pair& p, u32 entry, u32 slot, const std::vector<u32>& flags, const char* name) {
    p.restore([](gekko::Cpu&) {});
    std::vector<u32> original, native;
    for (const u32 flag : flags) {
        original.push_back(p.original.call(entry, {flag}));
        native.push_back(p.native.call(entry, {flag}));
    }
    std::string where;
    check(native == original && same_memory(p.original, p.native, where, {kSda + slot}),
          std::string(name) + " sequence " + where);
    const u32 untouched = gekko::load_be32(p.native.image.data() + (kSda + slot - kMemoryBegin));
    documented(p.native.cpu.mem_read32(kSda + slot) == untouched,
               std::string(name) + " keeps its flag natively, not in the SDK's global");
}

// The read entries: their argument assert and the request they issue.
void compare_read(Pair& p, bool synchronous, u32 file_length, u32 length, u32 offset) {
    constexpr u32 kInfo = kScratch + 0x400u;
    const auto setup = [&](gekko::Cpu& c) {
        c.mem_write32(kInfo + 0x30u, 0x60010000u);
        c.mem_write32(kInfo + 0x34u, file_length);
        c.mem_write32(kInfo + 0x0Cu, 0u);
        c.mem_write32(kInfo + 0x20u, length);
    };
    p.restore(setup);
    if (synchronous) p.original.call(kReadSync, {kInfo, 0x80900000u, length, offset, 2u});
    else p.original.call(kReadAsync, {kInfo, 0x80900000u, length, offset, 0u, 2u});
    const bool original_panicked = !p.original.calls.empty() && p.original.calls.front().rfind("OSPanic", 0) == 0;
    const u64 refused_before = p.api->stats().reads_refused;
    if (synchronous) p.native.call(kReadSync, {kInfo, 0x80900000u, length, offset, 2u});
    else p.native.call(kReadAsync, {kInfo, 0x80900000u, length, offset, 0u, 2u});
    const bool native_stopped = p.native.cpu.stopped();
    bool ok = native_stopped == original_panicked;
    if (ok && !original_panicked && p.api->stats().reads_refused != refused_before) {
        // A destination the guest cannot own (past the game's memory): the API refuses it
        // as FATAL; the SDK would have handed it to the drive's DMA.
        documented(p.native.cpu.mem_read32(kInfo + 0x0Cu) == 0xFFFFFFFFu &&
                       !p.original.calls.empty() && p.original.calls.front().rfind("read ", 0) == 0,
                   "a destination beyond guest RAM completes FATAL");
        return;
    }
    if (ok && !original_panicked) {
        // The request the original forwards to its raw read is the request the
        // native read served: the same block, destination, length and token.
        gekko::Cpu& n = p.native.cpu;
        const std::string served = "read block=" + hex(kInfo) + " addr=80900000 len=" + hex(length) + " token=" +
                                   hex(n.mem_read32(kInfo + 0x10u)) + " cb=" +
                                   hex(synchronous ? 0x8037B10Cu : 0x8037AFC4u);
        ok = !p.original.calls.empty() && p.original.calls.front() == served &&
             n.mem_read32(kInfo + 0x14u) == length && n.mem_read32(kInfo + 0x18u) == 0x80900000u &&
             n.mem_read32(kInfo + 0x10u) == 0x60010000u + offset;
        if (!synchronous) ok = ok && n.gpr[3] == 1u && n.mem_read32(kInfo + 0x38u) == 0u;
    }
    check(ok, std::string(synchronous ? "DVDReadPrio" : "DVDReadAsyncPrio") + " (file=" + std::to_string(file_length) +
                  ", len=" + std::to_string(length) + ", off=" + std::to_string(offset) + ")");
}

// The async read's completion: the original forwards cbForReadAsync, which
// calls fi->callback(result, fi); the native read calls fi->callback itself.
void compare_read_completion(Pair& p) {
    constexpr u32 kInfo = kScratch + 0x400u;
    constexpr u32 kForward = 0x8037AFC4u;  // cbForReadAsync
    for (const u32 result : {0x40u, 0xFFFFFFFFu}) {
        p.restore([&](gekko::Cpu& c) { c.mem_write32(kInfo + 0x38u, kCallbackSink); });
        p.original.call(kForward, {result, kInfo});
        check(p.original.calls == std::vector<std::string>{"callback r3=" + hex(result) + " r4=" + hex(kInfo)},
              "cbForReadAsync forwards (result, fi) to fi->callback");
    }
    p.restore([](gekko::Cpu&) {});
    p.original.call(kForward, {0x40u, kInfo});
    check(p.original.calls.empty(), "cbForReadAsync with no callback calls nothing");
}

// ---- tables ----------------------------------------------------------------------

// A tree in the layout the original walks: entry 0 the root, names in entry
// order with the first child's at offset 0 (the disc's own layout).
struct Table {
    struct Node {
        std::string name;
        bool dir = false;
        std::vector<Node> children;
        u32 length = 0;
    };
    static std::vector<u8> build(const Node& root) {
        std::vector<std::pair<const Node*, u32>> flat;
        std::vector<u32> next;
        std::function<void(const Node&, u32)> walk = [&](const Node& node, u32 parent) {
            const u32 index = static_cast<u32>(flat.size());
            flat.push_back({&node, parent});
            next.push_back(0u);
            for (const Node& child : node.children) walk(child, index);
            next[index] = static_cast<u32>(flat.size());
        };
        walk(root, 0u);
        std::vector<u8> strings;
        std::vector<u8> bytes(flat.size() * 12u);
        for (std::size_t i = 0; i < flat.size(); ++i) {
            const Node& node = *flat[i].first;
            u32 w0, w1, w2;
            if (i == 0) {
                w0 = 0x01000000u;
                w1 = 0u;
                w2 = static_cast<u32>(flat.size());
            } else {
                const u32 name_offset = static_cast<u32>(strings.size());
                strings.insert(strings.end(), node.name.begin(), node.name.end());
                strings.push_back(0u);
                w0 = (node.dir ? 0x01000000u : 0u) | name_offset;
                w1 = node.dir ? flat[i].second : 0x60000000u;
                w2 = node.dir ? next[i] : node.length;
            }
            gekko::store_be32(bytes.data() + i * 12u, w0);
            gekko::store_be32(bytes.data() + i * 12u + 4u, w1);
            gekko::store_be32(bytes.data() + i * 12u + 8u, w2);
        }
        bytes.insert(bytes.end(), strings.begin(), strings.end());
        return bytes;
    }
};
using Node = Table::Node;
Node file(const std::string& name, u32 length = 100u) { return Node{name, false, {}, length}; }
Node dir(const std::string& name, std::vector<Node> children) { return Node{name, true, std::move(children), 0u}; }

std::vector<std::vector<u8>> synthetic_tables() {
    std::vector<std::vector<u8>> out;
    out.push_back(Table::build(dir("", {
        file("readme.txt"), file("ReadMe.TXT"), dir("Data", {file("a.bin"), file("A.BIN"), dir("Sub", {file("x"), dir("deep", {file("y.dat")})}), dir("empty", {})}),
        dir("data2", {file("..."), file(".hidden"), file("dot.in.name.bin"), file("space name.bin")}),
        file("x"), dir("x2", {dir("x", {file("x")})}),
        file(std::string("hi\x80\xFF") + "byte"), file("Z"), file("z"), dir("stg0604", {file("stg0604.one"), file("STG0604.ONE")}),
        file("averyveryverylongfilename.extension"), file("A.B.C"), file("ab.c d"),
    })));
    out.push_back(Table::build(dir("", {})));
    out.push_back(Table::build(dir("", {dir("a", {dir("b", {dir("c", {file("d")})})}), dir("b", {file("a")})})));
    return out;
}

std::vector<std::string> edge_paths() {
    return {"", "/", ".", "..", "./", "../", "...", "./.", "/.", "/..", "//", "///readme.txt", "/readme.txt",
            "readme.txt/", "README.TXT", "Readme.Txt", "readme.tx", "readme.txtx", "readme", "Data", "Data/",
            "data/", "DATA/a.bin", "Data/./a.bin", "Data/../readme.txt", "Data/Sub/../a.bin", "Data/Sub/deep/y.dat",
            "Data/Sub/deep/../../Sub/x", "Data//a.bin", "Data/empty/", "Data/empty/x", "data2/...", "data2/.hidden",
            "data2/dot.in.name.bin", "data2/space name.bin", "x", "x/", "x2/x/x", "x2/x", "X2/X/X", "../readme.txt",
            "./readme.txt", "./Data/a.bin", "nope", "nope/", "Data/nope", "Data/Sub/nope/x",
            std::string("hi\x80\xFF") + "byte", std::string("HI\x80\xFF") + "BYTE", std::string("hi\x80\xFE") + "byte",
            "z", "Z", "stg0604/stg0604.one", "STG0604/stg0604.ONE", "averyveryverylongfilename.extension",
            "A.B.C", "ab.c d", "a/b/c/d", "a/b/c/d/", "b/a", "a/b/../b/c/../../b/c/d", ".x", "x/.", "x/..",
            "Data/Sub/deep/.", "Data/Sub/deep/..", "Data/Sub/deep/../.."};
}

// Every path a namespace names, each directory with and without '/'.
std::vector<std::string> namespace_paths(const assets::AssetNamespace& names) {
    std::vector<std::string> out;
    for (u32 i = 1; i < names.count(); ++i) {
        out.push_back(names.path(i));
        if (names.is_directory(i)) out.push_back(names.path(i) + "/");
    }
    return out;
}

std::string swap_case(std::string text) {
    for (char& ch : text) {
        if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
        else if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    return text;
}

// A deterministic adversarial corpus over a namespace's own names: mixed case,
// '.', '..', repeated and trailing separators, missing names, prefixes.
std::vector<std::string> adversarial_paths(const assets::AssetNamespace& names, u32 count) {
    std::vector<std::string> pool;
    for (u32 i = 1; i < names.count(); ++i) pool.emplace_back(names.name(i));
    pool.insert(pool.end(), {".", "..", "", "nope", "x"});
    std::vector<std::string> out;
    u64 state = 0x9E3779B97F4A7C15ull;
    const auto next = [&state]() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    };
    for (u32 n = 0; n < count; ++n) {
        std::string path = (next() & 3u) == 0u ? "/" : "";
        const u32 parts = 1u + static_cast<u32>(next() % 4u);
        for (u32 k = 0; k < parts; ++k) {
            std::string part = pool[static_cast<std::size_t>(next() % pool.size())];
            if ((next() & 3u) == 0u) part = swap_case(part);
            if ((next() & 7u) == 0u && !part.empty()) part.pop_back();
            path += part;
            if (k + 1u < parts || (next() & 3u) == 0u) path += (next() & 7u) == 0u ? "//" : "/";
        }
        out.push_back(path);
    }
    return out;
}

void run_namespace(const std::vector<u8>& dol, assets::AssetNamespace names, const std::filesystem::path& root,
                   bool disc) {
    const assets::AssetNamespace copy = names;
    Pair pair(dol, std::move(names), root);
    std::vector<std::string> paths = namespace_paths(copy);
    if (!disc) {
        const std::vector<std::string> edges = edge_paths();
        paths.insert(paths.end(), edges.begin(), edges.end());
    }
    std::size_t index = 0;
    for (const std::string& path : paths) {
        compare_path(pair, path);
        compare_path(pair, swap_case(path));
        compare_path(pair, "/" + path);
        if (!disc || (index % 7u) == 0u) compare_open(pair, path);
        ++index;
    }
    for (const std::string& path : adversarial_paths(copy, disc ? 4000u : 1500u)) compare_path(pair, path);
    for (const char* missing : {"nope.bin", "stg0604/nope.one", "STG0604", "stg0604/", "a/b/c/d/e"})
        compare_open(pair, missing);
    std::printf("%s namespace: %zu paths, %u entries\n", disc ? "disc (host tree)" : "synthetic", paths.size(),
                copy.count());
}

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc >= 2, "usage: native_file_api_reference_tests <main.dol>");
        const std::filesystem::path dol_path = argv[1];
        const std::vector<u8> dol = read_file(dol_path);
        require(dol.size() >= 256u, "missing original DOL");
        const std::filesystem::path game = dol_path.parent_path().parent_path();

        // Evidence that these are the words this test claims to execute, and
        // the two facts the native API relies on.
        {
            Machine m(dol);
            require(m.bus.read32(kConvertPath) == 0x7C0802A6u &&
                        m.bus.read32(0x8037A9C0u) == 0x4802E6B5u &&  // bl tolower
                        m.bus.read32(0x8037AB34u) == 0x909F0030u &&  // stw startAddr
                        m.bus.read32(0x8037AFB0u) == 0x38600001u,    // li r3,1
                    "unexpected DVD SDK instruction evidence");
            // OSInit's last word on __DVDLongFileNameFlag: li r0,1 / stw r0,0x5B54(r13)
            // on the path every branch rejoins (0x80370F68): the 8.3 mode is
            // unreachable once OSInit has run, and no file API call precedes it.
            require(m.bus.read32(0x80370F68u) == 0x38000001u &&
                        m.bus.read32(0x80370F70u) == 0x900D5B54u,
                    "OSInit no longer sets the long-file-name flag unconditionally");
            // tolower's table (MSL __lower_map) is exactly the namespace's fold.
            for (u32 b = 0; b < 256u; ++b) {
                const u8 lowered = m.bus.read8(0x80569278u + b);
                check(lowered == assets::AssetNamespace::fold(static_cast<u8>(b)),
                      "__lower_map[" + std::to_string(b) + "] differs from AssetNamespace::fold");
            }
        }

        // --quick skips the namespace sweeps (development iteration only;
        // CTest always runs the whole test).
        const bool quick = argc >= 3 && std::string(argv[2]) == "--quick";
        std::string error;
        const std::filesystem::path scratch = gekko::test_support::scratch_directory("gekko-file-api-reference");
        std::filesystem::create_directories(scratch);
        if (!quick) {
            // The disc: the NATIVE namespace from the host tree, published for
            // the original words exactly as the boot published it before
            // the native namespace (the oracle; production publishes none).
            assets::AssetNamespace disc;
            require(shadow::backends::namespace_from_host_directory(game / "files", disc, error), error.c_str());
            run_namespace(dol, std::move(disc), game / "files", true);
            for (const std::vector<u8>& table : synthetic_tables()) {
                assets::AssetNamespace names;
                require(gekko::test_support::namespace_from_fst(table, names, error), error.c_str());
                run_namespace(dol, std::move(names), scratch, false);
            }
        }

        // Status, cancel, configuration and reads over one small namespace.
        {
            assets::AssetNamespace names;
            require(gekko::test_support::namespace_from_fst(synthetic_tables().front(), names, error),
                    error.c_str());
            Pair pair(dol, std::move(names), scratch);
            for (s32 state = -3; state <= 13; ++state) {
                compare_status(pair, state);
                compare_transferred(pair, state);
                compare_cancel(pair, state);
            }
            compare_status(pair, 0x7FFFFFFF);
            compare_cancel(pair, 0x7FFFFFFF);
            compare_cancel(pair, static_cast<s32>(0x80000000u));
            compare_flag_sequence(pair, kAutoFatal, kFatalErrorFunc, {1u, 0u, 7u, 0u, 1u, 1u}, "DVDSetAutoFatalMessaging");
            compare_flag_sequence(pair, kAutoInvalidationEntry, kAutoInvalidation, {0u, 1u, 0u, 0u, 5u},
                                  "DVDSetAutoInvalidation");
            pair.restore([](gekko::Cpu&) {});
            check(pair.native.call(kDiskId, {}) == pair.original.call(kDiskId, {}), "DVDGetCurrentDiskID");
            pair.restore([](gekko::Cpu&) {});
            check(pair.native.call(kCheckDisk, {}) == pair.original.call(kCheckDisk, {}), "DVDCheckDisk");
            for (const u32 alarm : {0u, 0x81000000u, 0x80587B70u, 0x80587AC0u, 0x80587A98u}) {
                pair.restore([](gekko::Cpu&) {});
                const u32 original = pair.original.call(kTestAlarm, {alarm});
                const u32 native = pair.native.call(kTestAlarm, {alarm});
                // The SDK's own three alarm objects (dvd.c 0x80587B70, dvdlow.c
                // 0x80587AC0 / 0x80587A98); every other alarm is no one's here.
                if (alarm == 0x80587B70u || alarm == 0x80587AC0u || alarm == 0x80587A98u) {
                    documented(native == 0u && original == 1u,
                               "__DVDTestAlarm(" + hex(alarm) + "): the API owns no alarms (original " +
                                   hex(original) + ", native " + hex(native) + ")");
                } else {
                    check(native == original, "__DVDTestAlarm(" + hex(alarm) + ")");
                }
            }
            compare_read_completion(pair);
            for (const bool synchronous : {false, true}) {
                for (const u32 file_length : {0u, 1u, 31u, 32u, 2048u, 0x7FFFFFE0u, 0xFFFFFFF0u}) {
                    for (const u32 length : {0u, 1u, 32u, 0x800u, 0x7FFFFFFFu, 0x80000000u}) {
                        for (const u32 offset : {0u, 1u, 31u, 32u, 2048u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu})
                            compare_read(pair, synchronous, file_length, length, offset);
                    }
                }
            }
        }

        std::error_code ec;
        std::filesystem::remove_all(scratch, ec);
        std::printf("native file API reference: %d cases, %d mismatches, %d documented differences\n", g_cases,
                    g_failures, g_documented);
        return g_failures == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAILED: %s\n", error.what());
        return 1;
    }
}
