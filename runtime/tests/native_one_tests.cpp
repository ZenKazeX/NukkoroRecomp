// liveness-roots: reference-interpreter-only
// The ONE archive layer in C++ (gekko/native_one.h, Phase 4 batch 4a) against the ORIGINAL words in the reference
// interpreter. Two machines, the same DOL: the original runs every function word for word, including the MSL routines
// it calls (strcmp, strstr, memset); the native one has GEKKO_NATIVE_ONE=1. Stand-ins on both: the RenderWare free
// 0x80049B5C, operator delete 0x803A1334, the synchronous Open 0x8004CE1C and MSL sscanf 0x803AC608 (each logs its
// arguments and the stack pointer it was called with; sscanf parses "One Ver %f" and stores the float - the native
// ParseDirectory calls the game's sscanf as the original does, so it does not depend on what sscanf computes).
// Compared after every call: r3, the non-volatile registers, the archive object (0x58 bytes, pre-filled), the whole
// archive buffer, the 0x400 stack bytes below the caller's stack pointer (pre-filled: the owners write their
// originals' frames), and the logged guest calls in order.
//   - The corpus: every .one file on the disc (files/ next to sys/main.dol) adopted (0x8004CAC0: Close + fields +
//     ParseDirectory) from a buffer holding the whole file; then Entry over the index bounds, IsOpen, FindName (the
//     first, middle and last members' names, a lowercase spelling, an absent name, a 44-character query), FindSubstr
//     (several substrings and starts, an empty one), Close and the dtor.
//   - The ctor with and without a path and with an allocator object, the sync Close, the dtor's flags, a version
//     string other than 0.60 / 0.50 (declined: the original's own words decide).
//
// usage: native_one_tests <sys/main.dol>

#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_one.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
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

using namespace gekko;

namespace {

constexpr u32 kFindSubstr = 0x8004C878u, kFindName = 0x8004C938u, kEntry = 0x8004CA50u, kAdopt = 0x8004CAC0u;
constexpr u32 kIsOpen = 0x8004CB20u, kClose = 0x8004CB48u, kParse = 0x8004CD38u, kOpen = 0x8004CE1Cu;
constexpr u32 kDtor = 0x8004CF90u, kCtor = 0x8004CFD8u;
constexpr u32 kRwFree = 0x80049B5Cu, kDelete = 0x803A1334u, kSscanf = 0x803AC608u;
constexpr u32 kStackBytes = 0x400u;
// Open (batch 4b): the stand-ins' objects. The path copy + RwStreamOpen 0x80045014 returns kStreamObject (a file
// stream: type 2, fp at +0xC) for a path that is the test file's, else 0; fp +0x38 = kFs, whose read slot (+0x30) is
// RtGCNFS's read 0x8047BC60 - a stand-in reading the test file (fp +0 position, +4 length) from kFileData. RwMalloc
// 0x80049BB8 returns kBuffer; RwStreamClose 0x8048229C logs. The read manager's holder is set (no lazy build).
constexpr u32 kStreamOpen = 0x80045014u, kReadSlot = 0x8047BC60u, kRwAllocate = 0x80049BB8u, kStreamClose = 0x8048229Cu;
constexpr u32 kStreamObject = 0x81000300u, kFp = 0x81000400u, kFs = 0x81000500u, kFileData = 0x81100000u;
constexpr u32 kHolderAt = 0x805EF064u, kHolder = 0x81000600u, kCurrentThread = 0x800000E4u, kThread = 0x81000800u;
constexpr u32 kRunQueueBits = 0x805EC500u + 0x5AE0u;
// The read manager's getter 0x80037200 and the RW allocation category's push 0x80049B3C / pop 0x80049B1C: stand-ins on
// both machines too (a native owner's guest calls need translated code, which this interpreter test has only for
// stand-ins); each logs where it was called from, and the category ones write 0x805E4A88 as the originals do.
constexpr u32 kGetter = 0x80037200u, kCategoryPush = 0x80049B3Cu, kCategoryPop = 0x80049B1Cu, kCategory = 0x805E4A88u;
// OpenAsync / OnRead (batch 4c): the submit 0x8004506C (logs the request block it is given; returns the word at
// kSubmitResult) and a requester's callback (at kUserCallback: logs its argument, stack pointer, LR and r31). OnRead's
// request block at kRequest.
constexpr u32 kOpenAsync = 0x8004CC94u, kOnRead = 0x8004CC28u, kSubmit = 0x8004506Cu, kSubmitResult = 0x81000900u;
constexpr u32 kUserCallback = 0x80001300u, kRequest = 0x81000A00u;
// The whole-file read 0x800450C4 (batch 4d): RwFopen 0x800514CC (returns kFp for the test path, else 0) and RwFclose
// 0x8047B3D8 stand-ins (both log where they were called from); the size is stored at kSizeAt.
constexpr u32 kWholeRead = 0x800450C4u, kFileOpen = 0x800514CCu, kFileClose = 0x8047B3D8u, kSizeAt = 0x81000C00u;
// The TOneFileAsync<T> loaders (batch 4e). Stand-ins on both machines: the manager 0x8004B164 (-> kMgr), its count
// 0x8004B13C (-> the word at kCountValue), register 0x8004D564 / unregister 0x8004D424, the extract 0x8004C770 (-> the
// word at kExtractValue), RwStreamOpen 0x804823B8 (-> kMemStream; logs the memory block it is given), RwStreamFindChunk
// 0x8047DFA0 (-> the word at kFindValue), RwTexDictionarySetCurrent 0x8048E114, the six readers and the dictionary's
// after-call 0x802E0BB0 (-> 0x7E5017), each logging its arguments and where it was called from. RwMalloc follows the
// word at kAllocMode (0: always kBuffer; 1: its first call fails; 3: its second call fails).
constexpr u32 kLoaderManager = 0x8004B164u, kLoaderCount = 0x8004B13Cu, kLoaderRegister = 0x8004D564u;
constexpr u32 kLoaderUnregister = 0x8004D424u, kExtractFn = 0x8004C770u, kStreamOpenFn = 0x804823B8u;
constexpr u32 kFindChunk = 0x8047DFA0u, kTexDictSet = 0x8048E114u, kLoaderAfter = 0x802E0BB0u;
constexpr u32 kReaders[6] = {0x80443C74u, 0x80456744u, 0x8047A2D4u, 0x804586E8u, 0x8048500Cu, 0x80001400u};
constexpr u32 kLoaderUpdates[6] = {0x8004AE60u, 0x8004B3A0u, 0x8004B7BCu, 0x8004BBD4u, 0x8004BFF0u, 0x8004C408u};
const char* const kLoaderNames[6] = {"RpDMorphAnimation", "RpWorld", "RtDict", "RpClump", "RwTexDictionary", "raw"};
constexpr u32 kMgr = 0x81000D00u, kCountValue = 0x81000D80u, kExtractValue = 0x81000D84u, kFindValue = 0x81000D88u;
constexpr u32 kAllocMode = 0x81000D8Cu, kMemStream = 0x81000E00u, kTask = 0x81000F00u, kArgOut = 0x81000FF0u;
constexpr u32 kDir = 0x81001000u, kLoaderJobs = 0x805EF234u;
const char* const kTestPath = "files/test/archive.one";
constexpr u32 kStack = 0x817E0000u, kR13 = 0x805EC500u, kR2 = 0x805FA780u, kSentinel = 0x80001200u;
constexpr u32 kOne = 0x81000000u, kString = 0x81000100u, kAllocObject = 0x81000200u, kBuffer = 0x80800000u;
constexpr u32 kObjectBytes = 0x58u;

int g_failures = 0;
u64 g_checks = 0;
void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        if (g_failures <= 60) std::printf("FAIL: %s\n", what.c_str());
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

struct Machine;
Machine* g_machine = nullptr;

struct Machine {
    GuestMemory bus;
    Cpu cpu{bus};
    RecompiledCode code;
    std::vector<RecompiledCode::Entry> entries;
    std::unique_ptr<NativeOne> one;
    std::vector<std::string> log;

    static void ret(Cpu& c) { c.pc = c.lr; }
    // Where a call came from: its stack pointer, return site and r28..r31 (what its frame would save).
    static std::string from(const Cpu& c) {
        std::string s = " sp " + hex(c.gpr[1]) + " lr " + hex(c.lr);
        for (u32 r = 28u; r < 32u; ++r) s += " r" + std::to_string(r) + " " + hex(c.gpr[r]);
        return s;
    }
    static void rw_free(Cpu& c) {
        g_machine->log.push_back("free " + hex(c.gpr[3]) + from(c));
        ret(c);
    }
    static void delete_(Cpu& c) {
        g_machine->log.push_back("delete " + hex(c.gpr[3]) + from(c));
        ret(c);
    }
    static void stream_open(Cpu& c) {
        std::string path;
        for (u32 i = 0; i < 0x80u; ++i) {
            const u8 b = c.mem_read8(c.gpr[4] + i);
            if (b == 0u) break;
            path.push_back(static_cast<char>(b));
        }
        g_machine->log.push_back("stream open " + hex(c.gpr[3]) + " " + path + from(c));
        c.gpr[3] = path == kTestPath ? kStreamObject : 0u;
        ret(c);
    }
    // The test file's read (the stand-in for RtGCNFS's, and the native side's read_sync): r3 fp, r4 buffer, r5 count.
    static u32 file_read(Cpu& c, u32 fp, u32 buffer, u32 count) {
        g_machine->log.push_back("read " + hex(fp) + " " + hex(buffer) + " " + hex(count) + from(c));
        const u32 position = c.mem_read32(fp), length = c.mem_read32(fp + 4u);
        const u32 n = std::min(count, length - position);
        for (u32 i = 0; i < n; ++i) c.mem_write8(buffer + i, c.mem_read8(kFileData + position + i));
        c.mem_write32(fp, position + n);
        return n;
    }
    static void read_slot(Cpu& c) {
        c.gpr[3] = file_read(c, c.gpr[3], c.gpr[4], c.gpr[5]);
        ret(c);
    }
    u32 allocations = 0;
    static void rw_allocate(Cpu& c) {
        g_machine->log.push_back("allocate " + hex(c.gpr[3]) + " " + hex(c.gpr[4]) + from(c));
        const u32 mode = c.mem_read32(kAllocMode), n = ++g_machine->allocations;
        c.gpr[3] = (mode == 1u && n == 1u) || (mode == 3u && n == 2u) ? 0u : kBuffer;
        ret(c);
    }
    // A call logged with its arguments (the registers beyond its arity are no argument: their values are left over).
    static void logged(Cpu& c, const char* what, u32 arity, u32 result) {
        std::string s = what;
        for (u32 r = 3u; r < 3u + arity; ++r) s += " " + hex(c.gpr[r]);
        g_machine->log.push_back(s + from(c));
        c.gpr[3] = result;
        ret(c);
    }
    static void loader_manager(Cpu& c) { logged(c, "manager", 0u, kMgr); }
    static void loader_count(Cpu& c) { logged(c, "count", 1u, c.mem_read32(kCountValue)); }
    static void loader_register(Cpu& c) { logged(c, "register", 4u, 1u); }
    static void loader_unregister(Cpu& c) { logged(c, "unregister", 2u, 1u); }
    static void extract_fn(Cpu& c) { logged(c, "extract", 5u, c.mem_read32(kExtractValue)); }
    static void mem_stream_open(Cpu& c) {
        g_machine->log.push_back("memory stream " + hex(c.mem_read32(c.gpr[5])) + " " + hex(c.mem_read32(c.gpr[5] + 4u)));
        logged(c, "stream open", 3u, kMemStream);
    }
    static void find_chunk(Cpu& c) { logged(c, "find chunk", 4u, c.mem_read32(kFindValue)); }
    static void tex_dict_set(Cpu& c) { logged(c, "texdict", 1u, c.gpr[3]); }
    static void reader(Cpu& c) { logged(c, "reader", 1u, 0x7E5017u); }
    static void reader2(Cpu& c) { logged(c, "reader", 2u, 0x7E5017u); }  // RtDict's: (dictionary, stream)
    static void loader_after(Cpu& c) { logged(c, "after", 1u, 0u); }
    static void getter(Cpu& c) {
        g_machine->log.push_back("getter" + from(c));
        c.gpr[3] = kHolder;
        ret(c);
    }
    static void category_push(Cpu& c) {
        g_machine->log.push_back("category " + hex(c.gpr[3]) + from(c));
        c.mem_write32(kCategory, c.gpr[3]);
        ret(c);
    }
    static void category_pop(Cpu& c) {
        g_machine->log.push_back("category pop" + from(c));
        c.mem_write32(kCategory, 0x20u);
        ret(c);
    }
    static void submit(Cpu& c) {
        std::string s = "submit " + hex(c.gpr[3]) + " " + hex(c.gpr[4]) + " " + hex(c.gpr[5]) + " [";
        for (u32 i = 0; i < 6u; ++i) s += " " + hex(c.mem_read32(c.gpr[4] + i * 4u));
        g_machine->log.push_back(s + " ]" + from(c));
        c.gpr[3] = c.mem_read32(kSubmitResult);
        ret(c);
    }
    static void file_open(Cpu& c) {
        std::string path;
        for (u32 i = 0; i < 0x80u; ++i) {
            const u8 b = c.mem_read8(c.gpr[3] + i);
            if (b == 0u) break;
            path.push_back(static_cast<char>(b));
        }
        g_machine->log.push_back("fopen " + path + " " + hex(c.gpr[4]) + from(c));
        c.gpr[3] = path == kTestPath ? kFp : 0u;
        ret(c);
    }
    static void file_close(Cpu& c) {
        g_machine->log.push_back("fclose " + hex(c.gpr[3]) + from(c));
        c.gpr[3] = 1u;
        ret(c);
    }
    static void user_callback(Cpu& c) {
        g_machine->log.push_back("callback " + hex(c.gpr[3]) + from(c));
        ret(c);
    }
    static void stream_close(Cpu& c) {
        g_machine->log.push_back("stream close " + hex(c.gpr[3]) + " " + hex(c.gpr[4]) + from(c));
        c.gpr[3] = 1u;
        ret(c);
    }
    static void sscanf_(Cpu& c) {
        g_machine->log.push_back("sscanf " + hex(c.gpr[3]) + " " + hex(c.gpr[4]) + " " + hex(c.gpr[5]) + " sp " +
                                 hex(c.gpr[1]) + " lr " + hex(c.lr) + " r29 " + hex(c.gpr[29]) + " r30 " +
                                 hex(c.gpr[30]) + " r31 " + hex(c.gpr[31]) + " cr " + hex(c.cr & 0x02000000u));
        std::string text;
        for (u32 i = 0; i < 0x20u; ++i) {
            const u8 b = c.mem_read8(c.gpr[3] + i);
            if (b == 0u) break;
            text.push_back(static_cast<char>(b));
        }
        const float v = text.rfind("One Ver ", 0) == 0 ? std::strtof(text.c_str() + 8, nullptr) : 1.0f;
        u32 bits;
        std::memcpy(&bits, &v, sizeof bits);
        c.mem_write32(c.gpr[5], bits);
        c.gpr[3] = 1u;
        ret(c);
    }

    Machine(const std::vector<u8>& dol, bool native) {
        const auto word = [&](std::size_t offset) {
            const u8* p = dol.data() + offset;
            return u32{p[0]} << 24 | u32{p[1]} << 16 | u32{p[2]} << 8 | u32{p[3]};
        };
        for (u32 i = 0; i < 18u; ++i) {
            const u32 offset = word(i * 4u), address = word(0x48u + i * 4u), size = word(0x90u + i * 4u);
            if (!size) continue;
            require(std::uint64_t(offset) + size <= dol.size(), "invalid DOL extent");
            bus.write_block(address, dol.data() + offset, size);
        }
        require(bus.read32(kParse) == 0x9421FFE0u && bus.read32(kFindName) == 0x9421FFB0u, "unexpected ONE words");
        cpu.reset();
        entries = {{kRwFree, 4u, rw_free, "rw free"}, {kDelete, 4u, delete_, "delete"}, {kSscanf, 4u, sscanf_, "sscanf"},
                   {kStreamOpen, 4u, stream_open, "stream open"}, {kReadSlot, 4u, read_slot, "read slot"},
                   {kRwAllocate, 4u, rw_allocate, "allocate"}, {kStreamClose, 4u, stream_close, "stream close"},
                   {kGetter, 4u, getter, "getter"}, {kCategoryPush, 4u, category_push, "category push"},
                   {kCategoryPop, 4u, category_pop, "category pop"}, {kSubmit, 4u, submit, "submit"},
                   {kUserCallback, 4u, user_callback, "callback"}, {kFileOpen, 4u, file_open, "fopen"},
                   {kFileClose, 4u, file_close, "fclose"}, {kLoaderManager, 4u, loader_manager, "manager"},
                   {kLoaderCount, 4u, loader_count, "count"}, {kLoaderRegister, 4u, loader_register, "register"},
                   {kLoaderUnregister, 4u, loader_unregister, "unregister"}, {kExtractFn, 4u, extract_fn, "extract"},
                   {kStreamOpenFn, 4u, mem_stream_open, "stream open"}, {kFindChunk, 4u, find_chunk, "find chunk"},
                   {kTexDictSet, 4u, tex_dict_set, "texdict"}, {kLoaderAfter, 4u, loader_after, "after"}};
        for (u32 i = 0; i < 5u; ++i) entries.push_back({kReaders[i], 4u, i == 2u ? reader2 : reader, "reader"});
        std::sort(entries.begin(), entries.end(),
                  [](const RecompiledCode::Entry& a, const RecompiledCode::Entry& b) { return a.address < b.address; });
        code.install(entries.data(), entries.size());
        cpu.attach_recompiled(&code);
        if (native) {
            one = std::make_unique<NativeOne>(cpu);
            one->set_sync_read([](Cpu& c, u32 fp, u32 buffer, u32 count) -> std::optional<u32> {
                return file_read(c, fp, buffer, count);
            });
            require(one->install(), "GEKKO_NATIVE_ONE did not install");
        }
        cpu.gpr[2] = kR2;
        cpu.gpr[13] = kR13;
        for (u32 r = 14u; r < 32u; ++r) cpu.gpr[r] = 0x1000u * r + 0x11u;
        cpu.set_msr(cpu.msr() | kMsrDR | kMsrIR | kMsrFP);
        // Open's world: the stream, its file system, the read manager's holder, the current thread (priority 16) and
        // an empty run queue.
        bus.write32(kStreamObject, 2u);
        bus.write32(kStreamObject + 0xCu, kFp);
        bus.write32(kFp + 0x38u, kFs);
        bus.write32(kFs + 0x30u, kReadSlot);
        bus.write32(kHolderAt, kHolder);
        bus.write32(kHolder, 0x81000700u);
        bus.write32(kCurrentThread, kThread);
        bus.write32(kThread + 0x2D0u, 16u);
        bus.write32(kRunQueueBits, 0u);
    }
    bool call(u32 pc, u32 r3, u32 r4 = 0u, u32 r5 = 0u, u32 r6 = 0u, u32 r7 = 0u, u32 r8 = 0u) {
        cpu.gpr[1] = kStack;
        cpu.gpr[3] = r3;
        cpu.gpr[4] = r4;
        cpu.gpr[5] = r5;
        cpu.gpr[6] = r6;
        cpu.gpr[7] = r7;
        cpu.gpr[8] = r8;
        cpu.lr = kSentinel;
        cpu.pc = pc;
        g_machine = this;
        u64 steps = 0;
        while (cpu.pc != kSentinel && !cpu.stopped() && steps++ < 50'000'000ull) cpu.run_steps(1u);
        g_machine = nullptr;
        return cpu.pc == kSentinel && !cpu.stopped();
    }
    void put_string(u32 at, const std::string& s) {
        for (std::size_t i = 0; i < s.size(); ++i) bus.write8(at + static_cast<u32>(i), static_cast<u8>(s[i]));
        bus.write8(at + static_cast<u32>(s.size()), 0u);
    }
};

struct Pair {
    Machine original, native;
    Pair(const std::vector<u8>& dol) : original(dol, false), native(dol, true) {}
    template <typename F>
    void both(F&& f) {
        f(original);
        f(native);
    }
    // One call on both machines, then everything compared.
    void compare(const std::string& what, u32 pc, u32 r3, u32 r4, u32 r5, u32 buffer_bytes) {
        compare6(what, pc, {r3, r4, r5, 0u, 0u, 0u}, buffer_bytes);
    }
    void compare6(const std::string& what, u32 pc, std::array<u32, 6> args, u32 buffer_bytes) {
        const u32 r3 = args[0];
        original.allocations = native.allocations = 0u;
        original.log.clear();
        native.log.clear();
        both([](Machine& m) {
            for (u32 i = 0; i < kStackBytes; i += 4u) m.bus.write32(kStack - kStackBytes + i, 0xDEAD0000u | i);
            for (u32 i = 0; i < 0x10u; i += 4u) m.bus.write32(kStack + i, 0xBEEF0000u | i);
        });
        const bool a = original.call(pc, args[0], args[1], args[2], args[3], args[4], args[5]);
        const bool b = native.call(pc, args[0], args[1], args[2], args[3], args[4], args[5]);
        check(a && b, what + ": both returned");
        check(original.cpu.gpr[3] == native.cpu.gpr[3] || pc == kParse,
              what + ": r3 original " + hex(original.cpu.gpr[3]) + " native " + hex(native.cpu.gpr[3]));
        bool regs = original.cpu.gpr[1] == native.cpu.gpr[1] && original.cpu.gpr[2] == native.cpu.gpr[2] &&
                    original.cpu.gpr[13] == native.cpu.gpr[13];
        for (u32 r = 14u; r < 32u; ++r) regs &= original.cpu.gpr[r] == native.cpu.gpr[r];
        check(regs, what + ": the non-volatile registers");
        // The call's contract (as the in-game strict oracle holds an owner to it): r4 and f1 too.
        check(original.cpu.gpr[4] == native.cpu.gpr[4],
              what + ": r4 original " + hex(original.cpu.gpr[4]) + " native " + hex(native.cpu.gpr[4]));
        check(original.cpu.fpr[1].bits[0] == native.cpu.fpr[1].bits[0] &&
                  original.cpu.fpr[1].bits[1] == native.cpu.fpr[1].bits[1],
              what + ": f1");
        bool object = true;
        for (u32 i = 0; i < kObjectBytes; ++i) object &= original.bus.read8(kOne + i) == native.bus.read8(kOne + i);
        check(object, what + ": the archive object");
        bool buffer = true;
        for (u32 i = 0; i < buffer_bytes; i += 4u) buffer &= original.bus.read32(kBuffer + i) == native.bus.read32(kBuffer + i);
        check(buffer, what + ": the archive buffer");
        bool stack = true;
        for (u32 i = 0; i < kStackBytes + 0x10u; i += 4u)
            stack &= original.bus.read32(kStack - kStackBytes + i) == native.bus.read32(kStack - kStackBytes + i);
        check(stack, what + ": the stack below the caller (the frames)");
        if (original.log != native.log) {
            std::size_t k = 0;
            while (k < original.log.size() && k < native.log.size() && original.log[k] == native.log[k]) ++k;
            check(false, what + ": the guest calls (first difference, call " + std::to_string(k) + ": original '" +
                             (k < original.log.size() ? original.log[k] : std::string("-")) + "' native '" +
                             (k < native.log.size() ? native.log[k] : std::string("-")) + "')");
        } else {
            check(true, what + ": the guest calls");
        }
    }
};

std::vector<u8> read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::vector<u8>{std::istreambuf_iterator<char>(input), {}};
}

// The byte-swap leaves 0x80043D0C / 0x80043D34 / 0x80043C80 (owned with the layer): every register they write (r0,
// r3-r9, CTR, CR0) and the swapped buffer, over random data.
void swaps(Pair& p) {
    constexpr u32 kSwap16 = 0x80043D0Cu, kSwap32 = 0x80043D34u, kSwapFormat = 0x80043C80u;
    constexpr u32 kFormat = kString;
    std::mt19937 random(0x5A17u);
    const auto fill = [&](u32 bytes) {
        std::vector<u8> data(bytes);
        for (u8& b : data) b = static_cast<u8>(random());
        p.both([&](Machine& m) { m.bus.write_block(kBuffer, data.data(), bytes); });
    };
    const auto volatile_regs = [&](const std::string& what) {
        bool same = p.original.cpu.ctr == p.native.cpu.ctr &&
                    (p.original.cpu.cr >> 28) == (p.native.cpu.cr >> 28);
        for (const u32 r : {0u, 5u, 6u, 7u, 8u, 9u}) same &= p.original.cpu.gpr[r] == p.native.cpu.gpr[r];
        check(same, what + ": r0, r5-r9, CTR and CR0");
    };
    // Sizes around the unit, odd ones, 0, negative (blelr), one large run.
    for (const s32 bytes : {0, -1, -8, 1, 2, 3, 4, 5, 6, 7, 8, 31, 33, 0x101, 0x1000, 0x10003}) {
        for (const u32 pc : {kSwap16, kSwap32}) {
            const u32 span = bytes > 0 ? static_cast<u32>(bytes) + 8u : 16u;
            fill(span);
            const std::string what = std::string(pc == kSwap16 ? "swap16 " : "swap32 ") + std::to_string(bytes);
            p.both([](Machine& m) { m.cpu.ctr = 0x1234u; m.cpu.gpr[0] = 0x77u; });
            p.compare6(what, pc, {kBuffer + (bytes == 5 ? 1u : 0u), static_cast<u32>(bytes), 0x55u, 0x66u, 0x77u, 0x88u},
                       span);
            volatile_regs(what);
        }
    }
    // The format swap: the game's two strings in the DOL, then every character class (out of range, high bit set).
    std::vector<std::string> formats;
    for (const u32 at : {0x804ACB08u, 0x805F4184u}) {
        std::string s;
        for (u32 i = 0; i < 64u; ++i) {
            const u8 b = p.original.bus.read8(at + i);
            if (b == 0u) break;
            s.push_back(static_cast<char>(b));
        }
        formats.push_back(s);
    }
    formats.push_back("cCsSfFiIxXyAzBw");
    formats.push_back(std::string("ci") + static_cast<char>(0x80) + "s" + static_cast<char>(0xFF) + "c" +
                      static_cast<char>(0x42) + "x" + static_cast<char>(0x79) + "f");
    formats.push_back("");
    for (const std::string& format : formats) {
        for (const s32 repeat : {0, -2, 1, 3, 16}) {
            for (const u32 offset : {0u, 1u, 3u}) {
                fill(0x2000u);
                p.both([&](Machine& m) {
                    m.put_string(kFormat, format);
                    m.cpu.ctr = 0x4321u;
                    m.cpu.gpr[0] = 0x99u;
                });
                const std::string what = "swap format '" + format + "' x" + std::to_string(repeat) + " +" +
                                         std::to_string(offset);
                p.compare6(what, kSwapFormat, {kBuffer + offset, kFormat, static_cast<u32>(repeat), 0xAAu, 0xBBu, 0xCCu},
                           0x2000u);
                volatile_regs(what);
            }
        }
    }
}

void prefill(Pair& p) {
    p.both([](Machine& m) {
        for (u32 i = 0; i < kObjectBytes; ++i) m.bus.write8(kOne + i, static_cast<u8>(0xA5u ^ i));
    });
}

// One archive through the layer on both machines.
u64 g_archives = 0, g_legacy = 0, g_members = 0;
void archive(Pair& p, const std::string& name, const std::vector<u8>& file) {
    const u32 bytes = (static_cast<u32>(file.size()) + 0x1Fu) & ~0x1Fu;
    p.both([&](Machine& m) {
        m.bus.write_block(kBuffer, file.data(), static_cast<u32>(file.size()));
        for (u32 i = static_cast<u32>(file.size()); i < bytes; ++i) m.bus.write8(kBuffer + i, 0xCDu);
    });
    prefill(p);
    p.both([](Machine& m) { m.bus.write32(kOne + 0x2Cu, 0u), m.bus.write8(kOne, 0u), m.bus.write32(kOne + 0x40u, 0u); });
    p.compare(name + ": Adopt", kAdopt, kOne, kBuffer, bytes, bytes);
    ++g_archives;
    g_legacy += std::string(reinterpret_cast<const char*>(file.data()) + 0xC, 12) == "One Ver 0.50";
    const u32 count = p.original.bus.read32(kBuffer + 0xCu + 0x10u);
    g_members += count;
    for (const u32 index : {0xFFFFFFFFu, 0u, 1u, 2u, 3u, count, count + 1u, count + 2u, count + 3u, 0x80000000u})
        p.compare(name + ": Entry " + hex(index), kEntry, kOne, index, 0u, 0u);
    p.compare(name + ": IsOpen", kIsOpen, kOne, 0u, 0u, 0u);
    // FindName: the first, middle and last members, a lowercase spelling, absent, too long.
    std::vector<std::string> queries;
    for (const u32 index : {2u, 2u + count / 2u, count + 1u}) {
        if (count == 0u) break;
        std::string member;
        for (u32 k = 0; k < 0x2Cu; ++k) {
            const u8 b = p.original.bus.read8(kBuffer + 0xCu + 0x34u + index * 0x38u + k);
            if (b == 0u) break;
            member.push_back(static_cast<char>(b));
        }
        queries.push_back(member);
        std::string lower = member;
        for (char& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        queries.push_back(lower);
    }
    queries.push_back("NOT_IN_ANY.ARCHIVE");
    queries.push_back(std::string(43, 'A'));
    queries.push_back(std::string(44, 'b'));  // the original's copy is then unterminated: strcmp reads on into the frame
    for (const std::string& q : queries) {
        p.both([&](Machine& m) { m.put_string(kString, q); });
        p.compare(name + ": FindName " + q, kFindName, kOne, kString, 0u, 0u);
    }
    p.compare(name + ": FindName (null)", kFindName, kOne, 0u, 0u, 0u);
    for (const char* sub : {"COLI", "TXD", ".DFF", "_TEX", "ZZQ", ""}) {
        for (const u32 start : {0u, 2u, 5u}) {
            p.both([&](Machine& m) { m.put_string(kString, sub); });
            p.compare(name + ": FindSubstr '" + sub + "' from " + std::to_string(start), kFindSubstr, kOne, kString, start, 0u);
        }
    }
    p.compare(name + ": dtor (flags 1)", kDtor, kOne, 1u, 0u, bytes);
    // The sync open of the same file: the chunk header, RwMalloc, the body read, the name, the parse.
    p.both([&](Machine& m) {
        m.bus.write_block(kFileData, file.data(), static_cast<u32>(file.size()));
        m.bus.write32(kFp, 0u);
        m.bus.write32(kFp + 4u, static_cast<u32>(file.size()));
        for (u32 i = 0; i < bytes; i += 4u) m.bus.write32(kBuffer + i, 0xCDCDCDCDu);
        m.put_string(kString, kTestPath);
        m.bus.write32(kOne + 0x2Cu, 0u);
        m.bus.write8(kOne, 0u);
        m.bus.write8(kOne + 0x44u, 0u);
        m.bus.write32(kOne + 0x40u, 0u);
    });
    p.compare(name + ": Open", kOpen, kOne, kString, 0u, bytes);
    p.compare(name + ": Open again (already loaded)", kOpen, kOne, kString, 0u, bytes);
    p.compare(name + ": dtor after Open", kDtor, kOne, 1u, 0u, bytes);
    // OnRead of the same file read into kBuffer: Adopt, then the requester's callback, then the tail.
    for (const bool with_callback : {true, false}) {
        p.both([&](Machine& m) {
            m.bus.write_block(kBuffer, file.data(), static_cast<u32>(file.size()));
            for (u32 i = static_cast<u32>(file.size()); i < bytes; ++i) m.bus.write8(kBuffer + i, 0xCDu);
            m.bus.write32(kOne + 0x2Cu, 0u);
            m.bus.write8(kOne, 0u);
            m.bus.write8(kOne + 0x44u, 1u);
            m.bus.write32(kOne + 0x40u, 0u);
            m.bus.write32(kOne + 0x48u, with_callback ? kUserCallback : 0u);
            m.bus.write32(kOne + 0x4Cu, 0x81234560u);
            const u32 words[6] = {0x81000B00u, kBuffer, bytes, kOne, kOnRead, 0x5A5A5A5Au};
            for (u32 i = 0; i < 6u; ++i) m.bus.write32(kRequest + i * 4u, words[i]);
        });
        p.compare(name + (with_callback ? ": OnRead" : ": OnRead without a callback"), kOnRead, kRequest, 0u, 0u, bytes);
        p.compare(name + ": dtor after OnRead", kDtor, kOne, 1u, 0u, bytes);
    }
    // The whole-file read of the same file: the buffer, the stored size.
    p.both([&](Machine& m) {
        m.bus.write32(kFp, 0u);
        m.bus.write32(kFp + 4u, static_cast<u32>(file.size()));
        for (u32 i = 0; i < bytes; i += 4u) m.bus.write32(kBuffer + i, 0xCDCDCDCDu);
        m.bus.write32(kSizeAt, 0xFFFFFFFFu);
        m.put_string(kString, kTestPath);
    });
    p.compare(name + ": whole-file read", kWholeRead, 0x81000700u, kString, kSizeAt, bytes);
    check(p.original.bus.read32(kSizeAt) == p.native.bus.read32(kSizeAt) &&
              p.native.bus.read32(kSizeAt) == static_cast<u32>(file.size()),
          name + ": the whole-file read's stored size");
    p.compare(name + ": Close (closed)", kClose, kOne, 0u, 0u, 0u);
    // The sync open's parse: ParseDirectory called directly on an archive opened by name (+0x34 = +0x38 = the data).
    p.both([&](Machine& m) {
        m.bus.write_block(kBuffer, file.data(), static_cast<u32>(file.size()));
        m.bus.write8(kOne, 'f');
        m.bus.write32(kOne + 0x2Cu, 0u);
        m.bus.write32(kOne + 0x34u, kBuffer + 0xCu);
        m.bus.write32(kOne + 0x38u, kBuffer + 0xCu);
    });
    p.compare(name + ": ParseDirectory (sync)", kParse, kOne, 0u, 0u, bytes);
    p.compare(name + ": Close (sync)", kClose, kOne, 0u, 0u, 0u);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::printf("usage: native_one_tests <sys/main.dol>\n");
        return 2;
    }
#if defined(_WIN32)
    _putenv_s("GEKKO_NATIVE_ONE", "1");
#else
    setenv("GEKKO_NATIVE_ONE", "1", 1);
#endif
    try {
        const std::vector<u8> dol = read_file(argv[1]);
        require(dol.size() > 0x100u, "cannot read the DOL");
        Pair p(dol);
        swaps(p);

        // The ctor: no path, a path (it opens: a file that is not there), an allocator object.
        for (const u32 path : {0u, kString}) {
            for (const u32 allocator : {0u, kAllocObject}) {
                prefill(p);
                p.both([](Machine& m) { m.put_string(kString, "files/stg0100/stg0100_dat.one"); });
                p.compare("ctor path " + hex(path) + " allocator " + hex(allocator), kCtor, kOne, path, allocator, 0u);
            }
        }
        // The sync Close: a name and an allocation at +0x38; then closed again; then the dtor's flags.
        for (const u32 flags : {0u, 1u, 0x8000u}) {
            prefill(p);
            p.both([](Machine& m) {
                m.bus.write8(kOne, 'f');
                m.bus.write32(kOne + 0x2Cu, 0u);
                m.bus.write32(kOne + 0x38u, 0x80900000u);
                m.bus.write32(kOne + 0x40u, 0u);
            });
            p.compare("dtor of a sync archive, flags " + hex(flags), kDtor, kOne, flags, 0u, 0u);
        }
        prefill(p);
        p.both([](Machine& m) {
            m.bus.write8(kOne, 'f');
            m.bus.write32(kOne + 0x2Cu, 0u);
            m.bus.write32(kOne + 0x40u, 0u);
        });
        p.compare("Close of a sync archive", kClose, kOne, 0u, 0u, 0u);
        p.compare("dtor of a null archive", kDtor, 0u, 1u, 0u, 0u);
        // An allocator object: Close and the dtor are declined; the original frees through its vtable +0x18 (the free
        // stand-in here).
        prefill(p);
        p.both([](Machine& m) {
            m.bus.write8(kOne, 'f');
            m.bus.write32(kOne + 0x2Cu, 0u);
            m.bus.write32(kOne + 0x40u, kAllocObject);
            m.bus.write32(kAllocObject, kAllocObject + 0x10u);
            m.bus.write32(kAllocObject + 0x10u + 0x18u, kRwFree);
        });
        p.compare("Close with an allocator object (declined)", kClose, kOne, 0u, 0u, 0u);

        // Open's other exits: pending, no path on a closed / an open archive, a file that is not there, and a thread
        // of the same priority ready (declined: the original's reads would yield to it).
        {
            const auto closed = [](Machine& m) {
                for (u32 i = 0; i < kObjectBytes; ++i) m.bus.write8(kOne + i, static_cast<u8>(0xA5u ^ i));
                m.bus.write32(kOne + 0x2Cu, 0u);
                m.bus.write8(kOne, 0u);
                m.bus.write8(kOne + 0x44u, 0u);
                m.bus.write32(kOne + 0x40u, 0u);
                m.put_string(kString, "files/missing.one");
            };
            p.both(closed);
            p.both([](Machine& m) { m.bus.write8(kOne + 0x44u, 1u); });
            p.compare("Open while pending", kOpen, kOne, kString, 0u, 0u);
            p.both(closed);
            p.compare("Open without a path, closed", kOpen, kOne, 0u, 0u, 0u);
            p.both(closed);
            p.both([](Machine& m) { m.bus.write8(kOne, 'f'); m.bus.write32(kOne + 0x38u, 0x80900000u); });
            p.compare("Open without a path, open", kOpen, kOne, 0u, 0u, 0u);
            p.both(closed);
            p.compare("Open of a file that is not there", kOpen, kOne, kString, 0u, 0u);
            p.both(closed);
            p.both([](Machine& m) { m.bus.write32(kRunQueueBits, 1u << 15); });
            p.compare("Open with a thread of the same priority ready (declined)", kOpen, kOne, kString, 0u, 0u);
            p.both([](Machine& m) { m.bus.write32(kRunQueueBits, 0u); });
        }

        // The whole-file read: a file that is not there (no size stored), and a ready thread (declined).
        p.both([](Machine& m) {
            m.bus.write32(kSizeAt, 0x12345678u);
            m.put_string(kString, "files/missing.bin");
        });
        p.compare("whole-file read of a file that is not there", kWholeRead, 0x81000700u, kString, kSizeAt, 0u);
        check(p.native.bus.read32(kSizeAt) == 0x12345678u, "whole-file read of a missing file stores no size");
        p.both([](Machine& m) { m.bus.write32(kRunQueueBits, 1u << 15); });
        p.compare("whole-file read with a thread of the same priority ready (declined)", kWholeRead, 0x81000700u, kString,
                  kSizeAt, 0u);
        p.both([](Machine& m) { m.bus.write32(kRunQueueBits, 0u); });

        // The loaders: every variant through its states (async and sync paths, a failed extract, the job limit, the
        // allocation retries, the wait flag), each update compared with the task, the counters and the result word.
        for (u32 v = 0; v < 6u; ++v) {
            const std::string name = std::string("loader ") + kLoaderNames[v];
            struct Scenario {
                const char* name;
                u32 callback, extract, find, alloc_mode, count, arg;
            };
            for (const Scenario& k :
                 {Scenario{"async, callback", kUserCallback, 0u, 1u, 0u, 3u, kArgOut},
                  Scenario{"sync, chunk not found", 0u, 0x777u, 0u, 1u, 3u, 0u},
                  Scenario{"sync, failed extract", 0u, 0u, 1u, 3u, 3u, kArgOut},
                  Scenario{"at the job limit", kUserCallback, 0u, 1u, 0u, 16u, 0u}}) {
                const std::string what = name + ", " + k.name;
                p.both([&](Machine& m) {
                    for (u32 i = 0; i < 0x60u; ++i) m.bus.write8(kTask + i, static_cast<u8>(0x3Cu ^ i));
                    m.bus.write16(kTask + 0x4u, 0x0100u);  // the task's flags: not done
                    m.bus.write32(kTask + 0x28u, 0u);
                    m.bus.write32(kTask + 0x2Cu, k.callback);
                    m.bus.write32(kTask + 0x30u, 0x81234560u);
                    m.bus.write32(kTask + 0x34u, 2u);
                    m.bus.write32(kTask + 0x40u, kOne);
                    m.bus.write32(kTask + 0x4Cu, 0u);
                    m.bus.write32(kTask + 0x54u, 0x81009000u);
                    m.bus.write8(kTask + 0x58u, 0u);
                    for (u32 i = 0; i < kObjectBytes; ++i) m.bus.write8(kOne + i, static_cast<u8>(0xA5u ^ i));
                    m.bus.write32(kOne + 0x2Cu, kDir - 0xCu);
                    m.bus.write32(kOne + 0x34u, kDir);
                    m.bus.write32(kOne + 0x54u, 5u);
                    m.bus.write32(kDir + 0x10u, 3u);
                    m.bus.write32(kDir + 0x34u + 2u * 0x38u + 0x2Cu, 0x1234u);
                    m.bus.write32(kLoaderJobs, 7u);
                    m.bus.write32(kArgOut, 0xFFFFFFFFu);
                    m.bus.write32(kCountValue, k.count);
                    m.bus.write32(kExtractValue, k.extract);
                    m.bus.write32(kFindValue, k.find);
                    m.bus.write32(kAllocMode, k.alloc_mode);
                });
                for (u32 step = 0; step < 6u; ++step) {
                    const u32 state = p.original.bus.read32(kTask + 0x28u);
                    p.compare6(what + ": update in state " + std::to_string(state), kLoaderUpdates[v],
                               {kTask, k.arg, 0u, 0u, 0u, 0u}, 0u);
                    bool same = true;
                    for (u32 i = 0; i < 0x60u; i += 4u) same &= p.original.bus.read32(kTask + i) == p.native.bus.read32(kTask + i);
                    check(same, what + ": the task after state " + std::to_string(state));
                    check(p.original.bus.read32(kLoaderJobs) == p.native.bus.read32(kLoaderJobs) &&
                              p.original.bus.read32(kArgOut) == p.native.bus.read32(kArgOut),
                          what + ": the jobs count and the result word after state " + std::to_string(state));
                    if ((p.original.bus.read16(kTask + 4u) & 1u) != 0u || k.count >= 16u) break;
                    // The async extract's done callback (game code, not under test): the pending byte cleared and
                    // the size given.
                    if (p.original.bus.read32(kTask + 0x28u) == 1u && p.original.bus.read8(kTask + 0x50u) != 0u)
                        p.both([](Machine& m) {
                            m.bus.write8(kTask + 0x50u, 0u);
                            m.bus.write32(kTask + 0x48u, 0x1234u);
                        });
                }
            }
            // State 3: the wait flag.
            for (const u8 wait : {0u, 1u}) {
                p.both([&](Machine& m) {
                    m.bus.write32(kTask + 0x28u, 3u);
                    m.bus.write8(kTask + 0x58u, wait);
                });
                p.compare6(name + ": state 3, wait flag " + std::to_string(wait), kLoaderUpdates[v], {kTask, 0u, 0u, 0u, 0u, 0u}, 0u);
                check(p.original.bus.read32(kTask + 0x28u) == p.native.bus.read32(kTask + 0x28u), name + ": state 3's result");
            }
        }

        // OpenAsync: pending; accepted on a closed and on an open archive (Close frees first); refused by the submit.
        {
            const auto setup = [](u8 pending, bool open, u32 accept) {
                return [=](Machine& m) {
                    for (u32 i = 0; i < kObjectBytes; ++i) m.bus.write8(kOne + i, static_cast<u8>(0xA5u ^ i));
                    m.bus.write32(kOne + 0x2Cu, 0u);
                    m.bus.write8(kOne, open ? 'f' : 0u);
                    m.bus.write32(kOne + 0x38u, 0x80900000u);
                    m.bus.write8(kOne + 0x44u, pending);
                    m.bus.write32(kOne + 0x40u, 0u);
                    m.bus.write32(kSubmitResult, accept);
                    m.put_string(kString, "files/stg0100/stg0100_dat.one");
                };
            };
            const std::array<u32, 6> args{kOne, kString, kUserCallback, 0x81234560u, 0u, 0u};
            const std::array<u32, 6> with_dst{kOne, kString, kUserCallback, 0x81234560u, 0x80A00000u, 0x1240u};
            p.both(setup(1u, false, 1u));
            p.compare6("OpenAsync while pending", kOpenAsync, args, 0u);
            p.both(setup(0u, false, 1u));
            p.compare6("OpenAsync of a closed archive, accepted", kOpenAsync, args, 0u);
            p.both(setup(0u, true, 1u));
            p.compare6("OpenAsync of an open archive, accepted", kOpenAsync, with_dst, 0u);
            p.both(setup(0u, false, 0x100u));  // only the low byte counts
            p.compare6("OpenAsync refused by the submit", kOpenAsync, args, 0u);
        }

        // A version string the disc does not hold: the game's sscanf decides (the stand-in: 0.55, the 0.50 layout).
        {
            std::vector<u8> file(0x200u, 0u);
            const char* v = "One Ver 0.55";
            std::copy(v, v + 12, file.begin() + 0xC);
            file[0xC + 0x10] = 1u;  // one member (little-endian count)
            const char* member = "X.BIN";
            std::copy(member, member + 5, file.begin() + 0xC + 0x34 + 2 * 0x38);
            archive(p, "version 0.55", file);
        }

        // The corpus: every archive on the disc.
        std::filesystem::path files = std::filesystem::path(argv[1]).parent_path().parent_path() / "files";
        std::vector<std::filesystem::path> archives;
        for (const auto& e : std::filesystem::recursive_directory_iterator(files)) {
            if (!e.is_regular_file()) continue;
            std::string ext = e.path().extension().string();
            for (char& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (ext == ".one") archives.push_back(e.path());
        }
        std::sort(archives.begin(), archives.end());
        require(!archives.empty(), "no .one files next to the DOL");
        for (const auto& path : archives) {
            const std::vector<u8> file = read_file(path);
            if (file.size() < 0x40u || file.size() > 0x00700000u) {
                check(false, path.string() + ": an archive outside the test's buffer");
                continue;
            }
            archive(p, std::filesystem::relative(path, files).string(), file);
        }
        const NativeOne::Stats& st = p.native.one->stats();
        check(st.open >= 1244u && st.open_thread_ready == 1u, "Open served natively for every archive, declined once");
        check(st.on_read >= 2u * 1244u && st.on_read_tail >= 1244u && st.open_async == 4u,
              "OnRead (with and without a callback) and OpenAsync served natively");
        check(st.whole_reads >= 1245u && st.whole_thread_ready == 1u, "the whole-file read served natively, declined once");
        check(st.loader_updates > 6u * 10u && st.loader_callbacks == 6u && st.loader_tails == 6u,
              "the loaders' updates served natively; one callback and tail per variant (updates " +
                  std::to_string(st.loader_updates) + ", callbacks " + std::to_string(st.loader_callbacks) + ", tails " +
                  std::to_string(st.loader_tails) + ")");
        std::printf("native ONE layer: %llu archives (%llu of version 0.50), %llu members; native calls: Entry %llu, "
                    "IsOpen %llu, FindName %llu, FindSubstr %llu, ParseDirectory %llu, Adopt %llu, Close %llu, ctor "
                    "%llu, dtor %llu; declined %llu\n",
                    static_cast<unsigned long long>(g_archives), static_cast<unsigned long long>(g_legacy),
                    static_cast<unsigned long long>(g_members), static_cast<unsigned long long>(st.entry),
                    static_cast<unsigned long long>(st.is_open), static_cast<unsigned long long>(st.find_name),
                    static_cast<unsigned long long>(st.find_substr), static_cast<unsigned long long>(st.parse),
                    static_cast<unsigned long long>(st.adopt), static_cast<unsigned long long>(st.close),
                    static_cast<unsigned long long>(st.ctor), static_cast<unsigned long long>(st.dtor),
                    static_cast<unsigned long long>(st.declined));
    } catch (const std::exception& error) {
        std::printf("ERROR: %s\n", error.what());
        return 1;
    }
    std::printf("native ONE layer: %llu checks, %d failure(s)\n", static_cast<unsigned long long>(g_checks), g_failures);
    return g_failures == 0 ? 0 : 1;
}
