// liveness-roots: reference-interpreter-only
// TExpandBG without a GameCube thread (gekko/native_expand_jobs.h, docs/NATIVE_WORKERS.md W1): the native Thread
// constructor against the ORIGINAL words of SonicteamUSA::System::Thread::Thread (0x8035A15C) in the reference
// interpreter, entered as TExpandBG's constructor enters it (LR 0x80043EA4) and run to that return. Stand-ins record
// the stack allocator (0x803A12A4: returns the next block of an arena), the manager's instance (0x801E24D0) and
// register (0x8035A390: returns a slot), memset (0x8000540C; the stand-in sets the bytes) and the OS's
// OSCreateThread / OSResumeThread (0x80378A44 / 0x80378ECC). Checked, for default and explicit stack sizes:
//   - the worker's fields outside its OSThread (+0x00..+0x0F, +0x328..+0x32B), the stack's 0xCD guard and the guest
//     calls before and after the thread calls are the original's, in order;
//   - the original creates (entry 0x8035A01C, priority 16, detached) and resumes a thread; the native one does neither,
//     and its OSThread state (+0x2C8) is 0, so the dtor's OSIsThreadTerminated (0x80378498) takes it as terminated;
//   - another caller (TBGRead's constructor) is not served: the hook declines.
//
// usage: native_expand_jobs_tests <sys/main.dol>

#include "gekko/cpu.h"
#include "gekko/game_memory.h"
#include "gekko/guest_memory.h"
#include "gekko/native_expand_jobs.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

using namespace gekko;

namespace {

constexpr u32 kThreadCtor = 0x8035A15Cu, kFromExpand = 0x80043EA4u, kFromRead = 0x8004579Cu;
constexpr u32 kAllocate = 0x803A12A4u, kManager = 0x801E24D0u, kRegister = 0x8035A390u;
constexpr u32 kCreate = 0x80378A44u, kResume = 0x80378ECCu, kMemset = 0x8000540Cu;
constexpr u32 kWorker = 0x81500000u, kArena = 0x81600000u, kManagerObject = 0x81700000u;
constexpr u32 kStack = 0x817E0000u, kR13 = 0x805EC500u, kR2 = 0x805FA780u;

int g_failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what.c_str());
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
    std::unique_ptr<NativeExpandJobs> jobs;
    std::vector<std::string> log;
    u32 arena = kArena;

    static void allocate(Cpu& c) {
        g_machine->log.push_back("alloc " + hex(c.gpr[3]));
        c.gpr[3] = g_machine->arena;
        g_machine->arena += 0x2000u;
        c.pc = c.lr;
    }
    static void manager(Cpu& c) {
        g_machine->log.push_back("manager");
        c.gpr[3] = kManagerObject;
        c.pc = c.lr;
    }
    static void reg(Cpu& c) {
        g_machine->log.push_back("register " + hex(c.gpr[3]) + " " + hex(c.gpr[4]));
        c.gpr[3] = 7u;
        c.pc = c.lr;
    }
    static void create(Cpu& c) {
        g_machine->log.push_back("OSCreateThread " + hex(c.gpr[3]) + " " + hex(c.gpr[4]) + " " + hex(c.gpr[5]) + " " +
                                 hex(c.gpr[6]) + " " + hex(c.gpr[7]) + " " + hex(c.gpr[8]) + " " + hex(c.gpr[9]));
        c.gpr[3] = 1u;
        c.pc = c.lr;
    }
    static void memset_(Cpu& c) {
        g_machine->log.push_back("memset " + hex(c.gpr[3]) + " " + hex(c.gpr[4]) + " " + hex(c.gpr[5]));
        for (u32 i = 0; i < c.gpr[5]; ++i) c.mem_write8(c.gpr[3] + i, static_cast<u8>(c.gpr[4]));
        c.pc = c.lr;
    }
    static void resume(Cpu& c) {
        g_machine->log.push_back("OSResumeThread " + hex(c.gpr[3]));
        c.gpr[3] = 1u;
        c.pc = c.lr;
    }

    Machine(const std::vector<u8>& dol, bool native) {
        const auto word = [&](std::size_t offset) { return load_be32(dol.data() + offset); };
        for (u32 i = 0; i < 18u; ++i) {
            const u32 offset = word(i * 4u), address = word(0x48u + i * 4u), size = word(0x90u + i * 4u);
            if (!size) continue;
            require(std::uint64_t(offset) + size <= dol.size(), "invalid DOL extent");
            bus.write_block(address, dol.data() + offset, size);
        }
        require(bus.read32(kThreadCtor) == 0x9421FFF0u && bus.read32(kFromExpand - 4u) == 0x483162BDu,
                "unexpected Thread constructor / TExpandBG call words");
        cpu.reset();
        entries = {{kCreate, 4u, create, "OSCreateThread stand-in"},
                   {kResume, 4u, resume, "OSResumeThread stand-in"},
                   {kManager, 4u, manager, "manager instance stand-in"},
                   {kRegister, 4u, reg, "manager register stand-in"},
                   {kAllocate, 4u, allocate, "allocator stand-in"},
                   {kMemset, 4u, memset_, "memset stand-in"}};
        std::sort(entries.begin(), entries.end(),
                  [](const RecompiledCode::Entry& a, const RecompiledCode::Entry& b) { return a.address < b.address; });
        code.install(entries.data(), entries.size());
        cpu.attach_recompiled(&code);
        if (native) {
            jobs = std::make_unique<NativeExpandJobs>(cpu);
            require(jobs->install(), "the native expand jobs did not install");
        }
        for (u32 i = 0; i < 0x400u; i += 4u) bus.write32(kWorker + i, 0xA5A5A5A5u);  // heap garbage
        for (u32 i = 0; i < 0x4000u; i += 4u) bus.write32(kArena + i, 0x5A5A5A5Au);
        cpu.gpr[2] = kR2;
        cpu.gpr[13] = kR13;
        cpu.set_msr(cpu.msr() | kMsrDR | kMsrIR | kMsrFP);
    }

    bool construct(u32 size, u32 caller) {
        cpu.gpr[3] = kWorker;
        cpu.gpr[4] = size;
        cpu.gpr[1] = kStack;
        cpu.lr = caller;
        cpu.pc = kThreadCtor;
        g_machine = this;
        u64 steps = 0;
        while (cpu.pc != caller && !cpu.stopped() && steps++ < 1'000'000ull) cpu.run_steps(1u);
        g_machine = nullptr;
        return cpu.pc == caller && !cpu.stopped();
    }
};

// Phase 2 (GEKKO_NATIVE_EXPAND_JOBS=3): the ONE member extract fn_8004C770, original words against the native owner.
// Stand-ins (shared by both sides) for what the extract and the expand step reach: memcpy (0x800054F4, copies), the
// stored member's notice
// (0x8004AD84), the PRS expand (0x80043FE0: writes a pattern derived from src, returns its count), operator new
// (0x803A1380: an arena), TExpandBG's ctor (0x80043E78: the fields only), the step's load-hold query (0x8032D138: 0),
// the task groups (0x8001C4F8) and their getter (0x801EB614), TExpandNotice's ctor (0x80043F60) and the reaper's
// queue call (0x803793B0). Real words: the extract, the member lookup One::Entry 0x8004CA50 (the native computes it in
// C++), TExpandBG's step 0x80043D5C and notice builder 0x80043DB0, the empty brackets 0x8035A018 / 0x8035A014 and the
// reaper 0x8035A2DC. The ONE's directory: +0x34 -> kEntries (count at +0x10 = 3: indices 2, 3, 4), entries at
// kEntries + 0x34 + index * 0x38.
constexpr u32 kExtract = 0x8004C770u, kMemcpy = 0x800054F4u, kFileNotice = 0x8004AD84u;
constexpr u32 kPrs = 0x80043FE0u, kNew = 0x803A1380u, kExpandCtor = 0x80043E78u, kHold = 0x8032D138u;
constexpr u32 kGroups = 0x8001C4F8u, kGroup = 0x801EB614u, kNoticeCtor = 0x80043F60u, kReaperQueue = 0x803793B0u;
constexpr u32 kStep = 0x80043D5Cu, kReaper = 0x8035A2DCu, kSentinel = 0x80001200u;
constexpr u32 kOne = 0x81400000u, kEntries = 0x81410000u, kSource = 0x81420000u, kDest = 0x81430000u;
constexpr u32 kHeap = 0x81480000u, kGroupsObject = 0x81470000u, kParent = 0x81470100u, kManagerTable = 0x81471000u;
constexpr u32 kCallback = 0x80177B30u, kUserdata = 0x81234560u;
constexpr u32 kArchive = 0x81600000u, kArchiveDest = 0x81700000u;  // Phase 3: a real ONE archive's data, the output
constexpr u32 entry_at(u32 index) { return kEntries + 0x34u + index * 0x38u; }
// W2 (GEKKO_NATIVE_EXPAND_JOBS=5): TBGReadManager::update, TBGRead's ctor / step / dtor and the completion Task.
// Stand-ins: the busy entry 0x80045370, the open 0x8047AFA0 (a file object whose +4 is the size), the RenderWare
// allocator 0x80049BB8 and free 0x80049B5C, the Thread base ctor 0x8035A15C / dtor 0x8035A0C8, the name copy
// 0x803ADB50, operator delete 0x803A1334, Task::Task 0x8004F014 (writes +4 = 0x0010 and +8 = the parent) and the read
// loop 0x80045150 (a pattern from the file over the destination, returning the size rounded up to 32, as one chunk does);
// the native's read is the same pattern (set_chunked_read). Real words: the update, TBGRead's ctor 0x80045778, step
// 0x80045680, its tail 0x800456EC / 0x80045890, TFileControlAsycManager's ctor 0x800458F0, the dtor 0x80045714, the
// read manager's getter 0x80037200 and the empty brackets.
constexpr u32 kReadUpdate = 0x800453E0u, kBusy = 0x80045370u, kOpenFile = 0x8047AFA0u, kRwAlloc = 0x80049BB8u;
constexpr u32 kRwFree = 0x80049B5Cu, kThreadCtorAt = 0x8035A15Cu, kThreadDtorAt = 0x8035A0C8u, kNameCopy = 0x803ADB50u;
constexpr u32 kDelete = 0x803A1334u, kTaskCtor = 0x8004F014u, kReadLoop = 0x80045150u;
constexpr u32 kReadStep = 0x80045680u, kReadDtor = 0x80045714u;
constexpr u32 kInFlight = 0x805EF1D8u, kPause = 0x805EF1DCu, kLists = 0x805742A0u, kHolderAt = 0x805EF064u;
constexpr u32 kReadManager = 0x81490000u, kHolder = 0x81490100u, kReadParent = 0x81490200u, kNode = 0x81491000u;
constexpr u32 kFile = 0x81492000u, kRwArena = 0x81500000u;
u32 read_pattern(Cpu& c, u32 file, u32 destination) {
    const u32 size = c.mem_read32(file + 4u);
    for (u32 i = 0; i < size; ++i) c.mem_write8(destination + i, static_cast<u8>((file >> 4) + i * 13u));
    return (size + 0x1Fu) & ~0x1Fu;
}

struct ExtractMachine;
ExtractMachine* g_extract = nullptr;

struct ExtractMachine {
    GuestMemory bus;
    Cpu cpu{bus};
    RecompiledCode code;
    std::vector<RecompiledCode::Entry> entries;
    std::unique_ptr<NativeExpandJobs> jobs;
    std::vector<std::string> log;
    u32 heap = kHeap;
    u32 rw_heap = kRwArena;

    static void ret(Cpu& c) { c.pc = c.lr; }
    // Where a guest call came from: its stack pointer, return site and the non-volatile registers the callee would save.
    static std::string from(const Cpu& c) {
        std::string s = " sp " + hex(c.gpr[1]) + " lr " + hex(c.lr);
        for (u32 r = 27u; r < 32u; ++r) s += " r" + std::to_string(r) + " " + hex(c.gpr[r]);
        return s;
    }
    static void memcpy_(Cpu& c) {
        g_extract->log.push_back("memcpy " + hex(c.gpr[3]) + " " + hex(c.gpr[4]) + " " + hex(c.gpr[5]) + from(c));
        for (u32 i = 0; i < c.gpr[5]; ++i) c.mem_write8(c.gpr[3] + i, c.mem_read8(c.gpr[4] + i));
        ret(c);
    }
    static void file_notice(Cpu& c) {
        g_extract->log.push_back("file notice " + hex(c.gpr[3]) + " " + hex(c.gpr[4]) + " " + hex(c.gpr[5]) + from(c));
        ret(c);
    }
    static void prs(Cpu& c) {
        g_extract->log.push_back("prs " + hex(c.gpr[3]) + " " + hex(c.gpr[4]));
        if (c.gpr[3] >= kArchive && c.gpr[3] < kArchive + 0x100000u) {
            // A real archive member (Phase 3): the host decoder, proven equal to the game's on every disc member
            // (asset_import_tests), from the guest's bytes.
            std::vector<u8> stream(0x20000u);
            for (u32 i = 0; i < stream.size(); ++i) stream[i] = c.mem_read8(c.gpr[3] + i);
            PrsDecoded d;
            require(prs_decode_ahead(stream.data(), stream.size(), 0x100000u, d), "the archive member decodes");
            for (u32 i = 0; i < d.output.size(); ++i) c.mem_write8(c.gpr[4] + i, d.output[i]);
            c.gpr[3] = static_cast<u32>(d.output.size());
            ret(c);
            return;
        }
        const u32 count = 0x40u + (c.gpr[3] & 0xFFu);
        for (u32 i = 0; i < count; ++i) c.mem_write8(c.gpr[4] + i, static_cast<u8>(c.mem_read8(c.gpr[3] + (i & 0xFu)) ^ i));
        c.gpr[3] = count;
        ret(c);
    }
    static void new_(Cpu& c) {
        g_extract->log.push_back("new " + hex(c.gpr[3]) + " = " + hex(g_extract->heap));
        c.gpr[3] = g_extract->heap;
        g_extract->heap += 0x400u;
        ret(c);
    }
    static void expand_ctor(Cpu& c) {
        g_extract->log.push_back("TExpandBG " + hex(c.gpr[4]) + " " + hex(c.gpr[5]) + " " + hex(c.gpr[6]) + " " +
                                 hex(c.gpr[7]));
        const u32 w = c.gpr[3];
        c.mem_write32(c.gpr[1] + 4u, c.lr);  // the real ctor's prologue saves its return address in the caller's frame
        c.mem_write32(w + 0x328u, 0x8051E23Cu);
        c.mem_write32(w + 0x330u, c.gpr[4]);
        c.mem_write32(w + 0x334u, c.gpr[5]);
        c.mem_write32(w + 0x338u, c.gpr[6]);
        c.mem_write32(w + 0x340u, c.gpr[7]);
        c.mem_write32(w + 0x33Cu, 0u);
        ret(c);
    }
    static void hold(Cpu& c) {
        g_extract->log.push_back("hold");
        c.gpr[3] = 0u;
        ret(c);
    }
    static void groups(Cpu& c) {
        g_extract->log.push_back("groups");
        c.gpr[3] = kGroupsObject;
        ret(c);
    }
    static void group(Cpu& c) {
        g_extract->log.push_back("group " + hex(c.gpr[3]) + " " + hex(c.gpr[4]));
        c.gpr[3] = c.mem_read32(c.gpr[3] + c.gpr[4] * 4u + 4u);
        ret(c);
    }
    static void notice_ctor(Cpu& c) {
        frame_writes(c);
        g_extract->log.push_back("notice " + hex(c.gpr[3]) + " worker " + hex(c.gpr[4]) + " " + hex(c.gpr[5]) + " " +
                                 hex(c.gpr[6]) + " " + hex(c.gpr[7]) + " " + hex(c.gpr[8]));
        ret(c);
    }
    static void reaper_queue(Cpu& c) { ret(c); }
    static void busy(Cpu& c) {
        g_extract->log.push_back("busy " + hex(c.gpr[3]) + " " + hex(c.gpr[4] & 0xFFu) + from(c));
        ret(c);
    }
    static void open_file(Cpu& c) {
        g_extract->log.push_back("open " + hex(c.gpr[3]) + " " + hex(c.gpr[4]) + " " + hex(c.gpr[5]) + " " + hex(c.gpr[6]) +
                                 from(c));
        c.gpr[3] = c.mem_read32(kFile + 0x100u) != 0u ? 0u : kFile;  // +0x100 set: the open fails
        ret(c);
    }
    static void rw_alloc(Cpu& c) {
        const u32 size = c.gpr[3];
        g_extract->log.push_back("rw alloc " + hex(size) + " " + hex(c.gpr[4]) + " = " + hex(g_extract->rw_heap) + from(c));
        c.gpr[3] = g_extract->rw_heap;
        g_extract->rw_heap += ((size + 0x1Fu) & ~0x1Fu) + 0x1000u;
        ret(c);
    }
    static void rw_free(Cpu& c) {
        g_extract->log.push_back("rw free " + hex(c.gpr[3]) + from(c));
        ret(c);
    }
    static void thread_ctor(Cpu& c) {
        g_extract->log.push_back("Thread ctor " + hex(c.gpr[4]));
        ret(c);  // r3 = this
    }
    static void thread_dtor(Cpu& c) {
        g_extract->log.push_back("Thread dtor");
        ret(c);
    }
    static void name_copy(Cpu& c) {
        g_extract->log.push_back("name copy");
        ret(c);
    }
    static void delete_(Cpu& c) {
        g_extract->log.push_back("delete");
        ret(c);
    }
    // A real constructor's frame: its return address in the caller's LR slot, words in its own frame below.
    static void frame_writes(Cpu& c) {
        c.mem_write32(c.gpr[1] + 4u, c.lr);
        c.mem_write32(c.gpr[1] - 0x80u, 0x5AFE0000u | (c.lr & 0xFFFFu));
    }
    static void task_ctor(Cpu& c) {
        frame_writes(c);
        g_extract->log.push_back("Task " + hex(c.gpr[3]) + " " + hex(c.gpr[4]));
        c.mem_write16(c.gpr[3] + 4u, 0x0010u);
        c.mem_write32(c.gpr[3] + 8u, c.gpr[4]);
        ret(c);
    }
    static void read_loop(Cpu& c) {
        const u32 file = c.gpr[4], destination = c.mem_read32(c.gpr[5]);
        g_extract->log.push_back("read " + hex(file) + " " + hex(destination));
        c.gpr[3] = read_pattern(c, file, destination);
        ret(c);
    }

    ExtractMachine(const std::vector<u8>& dol, bool native) {
        const auto word = [&](std::size_t offset) { return load_be32(dol.data() + offset); };
        for (u32 i = 0; i < 18u; ++i) {
            const u32 offset = word(i * 4u), address = word(0x48u + i * 4u), size = word(0x90u + i * 4u);
            if (!size) continue;
            require(std::uint64_t(offset) + size <= dol.size(), "invalid DOL extent");
            bus.write_block(address, dol.data() + offset, size);
        }
        require(bus.read32(kExtract) == 0x9421FFE0u && bus.read32(kStep) == 0x9421FFF0u &&
                    bus.read32(0x8004C7E4u) == 0x4830D835u /* bl 0x8035A018 */,
                "unexpected extract / step words");
        cpu.reset();
        entries = {{kMemcpy, 4u, memcpy_, "memcpy"},
                   {kFileNotice, 4u, file_notice, "notice"},  {kPrs, 4u, prs, "prs"},
                   {kNew, 4u, new_, "new"},                   {kExpandCtor, 4u, expand_ctor, "TExpandBG ctor"},
                   {kHold, 4u, hold, "hold"},                 {kGroups, 4u, groups, "groups"},
                   {kGroup, 4u, group, "group"},              {kNoticeCtor, 4u, notice_ctor, "notice ctor"},
                   {kReaperQueue, 4u, reaper_queue, "reaper queue"},
                   {kBusy, 4u, busy, "busy"},                 {kOpenFile, 4u, open_file, "open"},
                   {kRwAlloc, 4u, rw_alloc, "rw alloc"},      {kRwFree, 4u, rw_free, "rw free"},
                   {kThreadCtorAt, 4u, thread_ctor, "Thread ctor"}, {kThreadDtorAt, 4u, thread_dtor, "Thread dtor"},
                   {kNameCopy, 4u, name_copy, "name copy"},   {kDelete, 4u, delete_, "delete"},
                   {kTaskCtor, 4u, task_ctor, "Task ctor"},   {kReadLoop, 4u, read_loop, "read loop"}};
        std::sort(entries.begin(), entries.end(),
                  [](const RecompiledCode::Entry& a, const RecompiledCode::Entry& b) { return a.address < b.address; });
        code.install(entries.data(), entries.size());
        cpu.attach_recompiled(&code);
        if (native) {
            jobs = std::make_unique<NativeExpandJobs>(cpu);
            jobs->set_chunked_read([](Cpu& c, u32 file, u32 destination, u32) -> std::optional<u32> {
                g_extract->log.push_back("read " + hex(file) + " " + hex(destination));
                return read_pattern(c, file, destination);
            });
            require(jobs->install() && jobs->phase2(), "the native expand jobs (mode 3) did not install");
        }
        // Members: index 2 stored (0x30 bytes), 3 compressed, 4 compressed (another source).
        for (u32 i = 0; i < 0x100u; ++i) bus.write8(kSource + i, static_cast<u8>(i * 7u + 3u));
        const u32 members[3][3] = {{0x30u, kSource, 0u}, {0x80u, kSource + 0x10u, 1u}, {0x60u, kSource + 0x24u, 1u}};
        bus.write32(kOne + 0x34u, kEntries);
        bus.write32(kEntries + 0x10u, 3u);
        for (u32 m = 0; m < 3u; ++m) {
            bus.write32(entry_at(m + 2u) + 0x2Cu, members[m][0]);
            bus.write32(entry_at(m + 2u) + 0x30u, members[m][1]);
            bus.write32(entry_at(m + 2u) + 0x34u, members[m][2] | 0x100u);
        }
        bus.write32(kGroupsObject + 4u, kParent);
        for (u32 i = 0; i < 0x400u; i += 4u) bus.write32(kManagerTable + i, 0u);
        bus.write32(kManagerTable + 0x800u, kManagerTable);  // the manager: +0 = its table
        cpu.gpr[2] = kR2;
        cpu.gpr[13] = kR13;
        for (u32 r = 14u; r < 32u; ++r) cpu.gpr[r] = 0x1000u * r + 0x11u;
        cpu.set_msr(cpu.msr() | kMsrDR | kMsrIR | kMsrFP);
    }

    void open(u32 handle, u8 name) {
        bus.write32(kOne + 0x2Cu, handle);
        bus.write8(kOne, name);
    }
    bool run(u32 pc) {
        cpu.gpr[1] = kStack;
        cpu.lr = kSentinel;
        cpu.pc = pc;
        g_extract = this;
        u64 steps = 0;
        while (cpu.pc != kSentinel && !cpu.stopped() && steps++ < 1'000'000ull) cpu.run_steps(1u);
        g_extract = nullptr;
        return cpu.pc == kSentinel && !cpu.stopped();
    }
    // The stack below the callers' stack pointer, pre-filled the same way on both machines before a compared call.
    void fill_stack() {
        for (u32 i = 0; i < 0x400u; i += 4u) bus.write32(kStack - 0x400u + i, 0xDEAD0000u | i);
        for (u32 i = 0; i < 0x10u; i += 4u) bus.write32(kStack + i, 0xBEEF0000u | i);
    }
    bool extract(u32 index, u32 callback) {
        cpu.gpr[3] = kOne;
        cpu.gpr[4] = index;
        cpu.gpr[5] = kDest;
        cpu.gpr[6] = callback;
        cpu.gpr[7] = kUserdata;
        return run(kExtract);
    }
};

// The stack bytes [low, high) equal on both machines.
bool same_stack(ExtractMachine& a, ExtractMachine& b, u32 low, u32 high) {
    for (u32 at = low; at < high; at += 4u)
        if (a.bus.read32(at) != b.bus.read32(at)) return false;
    return true;
}
constexpr u32 kWindowLow = kStack - 0x400u, kWindowHigh = kStack + 0x10u;

void check_phase2(const std::vector<u8>& dol) {
    struct Case {
        const char* name;
        u32 handle;
        u8 name_byte;
        u32 index, callback;
    };
    // Every path that does not create a worker: the same words, calls, bytes and result.
    // The lookup's bounds (One::Entry): below 2, negative, count + 2 and beyond -> no member.
    for (const Case& k : {Case{"not open", 0u, 0u, 3u, kCallback}, Case{"open by name, no member", 0u, 0x41u, 99u, kCallback},
                          Case{"index 1", 0x55u, 0u, 1u, kCallback}, Case{"index -1", 0x55u, 0u, 0xFFFFFFFFu, kCallback},
                          Case{"index count + 2", 0x55u, 0u, 5u, kCallback}, Case{"index 0x80000000", 0x55u, 0u, 0x80000000u, 0u},
                          Case{"stored, callback", 0x55u, 0u, 2u, kCallback}, Case{"stored, no callback", 0x55u, 0u, 2u, 0u},
                          Case{"compressed, no callback", 0x55u, 0u, 3u, 0u}}) {
        ExtractMachine original(dol, false), native(dol, true);
        original.open(k.handle, k.name_byte);
        native.open(k.handle, k.name_byte);
        original.fill_stack();
        native.fill_stack();
        const bool a = original.extract(k.index, k.callback), b = native.extract(k.index, k.callback);
        const std::string what = std::string("extract, ") + k.name;
        check(same_stack(original, native, kWindowLow, kWindowHigh), what + ": the stack (the frames)");
        check(a && b, what + ": both returned");
        check(original.cpu.gpr[3] == native.cpu.gpr[3], what + ": r3 original " + hex(original.cpu.gpr[3]) + " native " +
                                                            hex(native.cpu.gpr[3]));
        for (u32 r : {1u, 2u, 13u}) check(original.cpu.gpr[r] == native.cpu.gpr[r], what + ": r" + std::to_string(r));
        for (u32 r = 14u; r < 32u; ++r) check(original.cpu.gpr[r] == native.cpu.gpr[r], what + ": r" + std::to_string(r));
        check(original.log == native.log, what + ": the guest calls");
        for (u32 i = 0; i < 0x200u; i += 4u)
            check(original.bus.read32(kDest + i) == native.bus.read32(kDest + i), what + ": the destination bytes");
        check(native.jobs->stats().extracts == 1u && native.jobs->pending() == 0u, what + ": served, nothing queued");
    }

    // Compressed with a callback: two members, so the order is checked. The original makes a TExpandBG per member;
    // its step (0x80043D5C, as the worker runs it) expands and builds the notice. The native queues two host jobs and
    // publishes both at the reaper's entry.
    ExtractMachine original(dol, false), native(dol, true);
    original.open(0x55u, 0u);
    native.open(0x55u, 0u);
    std::vector<u32> workers;
    for (const u32 index : {3u, 4u}) {
        const std::size_t before = original.log.size();
        original.fill_stack();
        native.fill_stack();
        check(original.extract(index, kCallback) && original.cpu.gpr[3] == 0u, "original extract with a callback");
        for (std::size_t i = before; i < original.log.size(); ++i)
            if (original.log[i].rfind("new 00000348", 0) == 0) workers.push_back(original.heap - 0x400u);
        check(native.extract(index, kCallback) && native.cpu.gpr[3] == 0u, "native extract with a callback returns 0");
        for (u32 r = 14u; r < 32u; ++r) check(original.cpu.gpr[r] == native.cpu.gpr[r], "queued extract: r" + std::to_string(r));
        // The extract's own frame (the original then builds the TExpandBG below it: those frames are not made).
        check(same_stack(original, native, kStack - 0x20u, kStack + 0x8u), "queued extract: its frame");
    }
    check(workers.size() == 2u, "the original made two 0x348 workers");
    check(native.jobs->pending() == 2u && native.jobs->stats().queued == 2u, "the native queued two jobs");
    const std::vector<std::string> native_submit = native.log;
    check(native_submit.empty(), "the native extract makes no guest call (no lookup, allocation, TExpandBG or thread)");
    // The original's publication: each worker's step in creation order.
    const std::size_t original_steps = original.log.size();
    for (const u32 w : workers) {
        original.cpu.gpr[3] = w;
        check(original.run(kStep) && original.cpu.gpr[3] == 0u, "the original step returns 0");
    }
    std::vector<std::string> expected(original.log.begin() + static_cast<std::ptrdiff_t>(original_steps), original.log.end());
    expected.erase(std::remove(expected.begin(), expected.end(), std::string("hold")), expected.end());
    // The notice's worker field: the worker in the original, 0 in the native (its update only clears it).
    for (std::string& e : expected) {
        for (const u32 w : workers) {
            const std::string from = "worker " + hex(w);
            if (const std::size_t at = e.find(from); at != std::string::npos) e.replace(at, from.size(), "worker 00000000");
        }
    }
    // The native publication: the reaper's entry (the hook publishes, then the original reaper runs on an empty table).
    const std::size_t native_before = native.log.size();
    native.cpu.gpr[3] = kManagerTable + 0x800u;
    check(native.run(kReaper), "the native reaper returned");
    std::vector<std::string> published(native.log.begin() + static_cast<std::ptrdiff_t>(native_before), native.log.end());
    // The heap: without the workers the notices land elsewhere (contracts G2). Each side's allocations are named by
    // their order ("A0", "A1", ...): the notice must be built in the block new just returned.
    const auto by_order = [](std::vector<std::string>& log) {
        std::vector<std::string> blocks;
        for (std::string& e : log) {
            if (const std::size_t at = e.find(" = "); e.rfind("new ", 0) == 0 && at != std::string::npos) {
                blocks.push_back(e.substr(at + 3));
                e = e.substr(0, at) + " = A" + std::to_string(blocks.size() - 1u);
            }
            for (std::size_t b = 0; b < blocks.size(); ++b) {
                const std::string from = "notice " + blocks[b];
                if (e.rfind(from, 0) == 0) e.replace(0, from.size(), "notice A" + std::to_string(b));
            }
        }
    };
    by_order(expected);
    by_order(published);
    check(published == expected, "the published calls equal the original steps' (prs, new 0x38, groups, group, notice)");
    if (published != expected) {
        for (const auto& e : expected) std::printf("  original: %s\n", e.c_str());
        for (const auto& e : published) std::printf("  native:   %s\n", e.c_str());
    }
    check(native.jobs->pending() == 0u && native.jobs->stats().published == 2u &&
              native.jobs->stats().expanded_by_owner == 2u,
          "both jobs published through the PRS owner (no GameMemory in this test)");
    for (u32 i = 0; i < 0x200u; i += 4u)
        check(original.bus.read32(kDest + i) == native.bus.read32(kDest + i), "phase 2: the destination bytes");
    // The worker's size field equals what the native passed to the notice.
    for (const u32 w : workers) check(original.bus.read32(w + 0x33Cu) != 0u, "the original's size field is written");
    // A savestate of the queue round-trips.
    {
        ExtractMachine again(dol, true);
        again.open(0x55u, 0u);
        check(again.extract(3u, kCallback) && again.jobs->pending() == 1u, "a queued job to save");
        std::vector<u8> blob;
        again.jobs->save_state(blob);
        ExtractMachine restored(dol, true);
        const u8* cursor = blob.data();
        check(restored.jobs->load_state(cursor, blob.data() + blob.size(), 30u) && restored.jobs->pending() == 1u &&
                  cursor == blob.data() + blob.size(),
              "the queue's savestate block round-trips");
    }
    // Negative control: a native that published the jobs in reverse order would be caught.
    {
        std::vector<std::string> reversed = published;
        if (reversed.size() >= 10u) std::rotate(reversed.begin(), reversed.begin() + 5, reversed.end());
        check(reversed != expected, "control: reordered publication differs");
    }
}

std::vector<u8> read_file(const char* path);

// Phase 3 (GEKKO_NATIVE_EXPAND_JOBS=4): two real compressed members of files/character/amy.one (AMY_HAND_L01.DFF,
// AMY_HAND_L02.DFF) through the extract and the publication, against the original's extract + step, with a GameMemory
// on the native side so its worker decodes the copies. Checked: the destination bytes and the notices' sizes equal
// the original's; both jobs were copied, decoded by the worker and placed from its result. Control: one stream byte
// changed between the extract and the publication (on both sides): the worker's copy must be refused and the bytes
// must still equal the original's decode of the changed stream.
void check_phase3(const std::vector<u8>& dol, const std::string& archive_path) {
    const std::vector<u8> archive = read_file(archive_path.c_str());
    require(archive.size() > 0x1000u && std::string(reinterpret_cast<const char*>(archive.data()) + 0xC, 7) == "One Ver",
            "cannot read files/character/amy.one");
    const u8* const data = archive.data() + 0xCu;  // the directory base (the open's +0x34)
    const auto le32 = [](const u8* p) { return u32{p[0]} | u32{p[1]} << 8 | u32{p[2]} << 16 | u32{p[3]} << 24; };
    std::vector<u8> clean;  // member 1's output without the change
    for (const bool corrupt : {false, true}) {
        const std::string what = corrupt ? "phase 3, a stream changed after the extract" : "phase 3";
        ExtractMachine original(dol, false), native(dol, true);
        GameMemory memory(native.cpu);
        native.jobs->set_game_memory(&memory);
        require(native.jobs->phase3(), "mode 4 installed");
        for (ExtractMachine* m : {&original, &native}) {
            m->bus.write_block(kArchive, data, static_cast<u32>(archive.size() - 0xCu));
            for (u32 index = 3u; index <= 4u; ++index) {  // the archive's members 3 and 4, relocated
                const u8* e = data + 0x34u + index * 0x38u;
                m->bus.write32(entry_at(index) + 0x2Cu, le32(e + 0x2Cu));
                m->bus.write32(entry_at(index) + 0x30u, kArchive + le32(e + 0x30u));
                m->bus.write32(entry_at(index) + 0x34u, le32(e + 0x34u));
            }
            m->open(0x55u, 0u);
        }
        const u32 sizes[2] = {le32(data + 0x34u + 3u * 0x38u + 0x2Cu), le32(data + 0x34u + 4u * 0x38u + 0x2Cu)};
        const u32 dests[2] = {kArchiveDest, kArchiveDest + 0x8000u};
        std::vector<u32> workers;
        for (u32 member = 1u; member <= 2u; ++member) {
            original.cpu.gpr[3] = kOne;
            original.cpu.gpr[4] = member + 2u;
            original.cpu.gpr[5] = dests[member - 1u];
            original.cpu.gpr[6] = kCallback;
            original.cpu.gpr[7] = kUserdata;
            const std::size_t before = original.log.size();
            check(original.run(kExtract), what + ": original extract");
            for (std::size_t i = before; i < original.log.size(); ++i)
                if (original.log[i].rfind("new 00000348", 0) == 0) workers.push_back(original.heap - 0x400u);
            native.cpu.gpr[3] = kOne;
            native.cpu.gpr[4] = member + 2u;
            native.cpu.gpr[5] = dests[member - 1u];
            native.cpu.gpr[6] = kCallback;
            native.cpu.gpr[7] = kUserdata;
            check(native.run(kExtract) && native.cpu.gpr[3] == 0u, what + ": native extract returns 0");
        }
        check(workers.size() == 2u, what + ": the original made two workers");
        check(native.jobs->stats().submitted == 2u && native.jobs->stats().snapshot_bytes >= sizes[0] / 2u,
              what + ": both streams copied and submitted to the worker");
        if (corrupt) {
            // A literal byte late in the first member's stream: the stream still decodes, to other bytes.
            const u32 at = kArchive + le32(data + 0x34u + 3u * 0x38u + 0x30u) + 0x400u;
            for (ExtractMachine* m : {&original, &native}) m->bus.write8(at, static_cast<u8>(m->bus.read8(at) ^ 0x5Au));
        }
        for (const u32 w : workers) {
            original.cpu.gpr[3] = w;
            check(original.run(kStep), what + ": the original step");
        }
        native.cpu.gpr[3] = kManagerTable + 0x800u;
        check(native.run(kReaper), what + ": the native publication");
        for (u32 k = 0; k < 2u; ++k) {
            bool same = true;
            for (u32 i = 0; i < sizes[k] + 0x40u; ++i) same &= original.bus.read8(dests[k] + i) == native.bus.read8(dests[k] + i);
            check(same, what + ": member " + std::to_string(k + 1u) + "'s destination bytes");
        }
        std::vector<u8> first(sizes[0]);
        for (u32 i = 0; i < sizes[0]; ++i) first[i] = native.bus.read8(dests[0] + i);
        if (!corrupt) clean = first;
        else check(first != clean, what + ": the change reached the output (the control is live)");
        // The notices: the same callback, size and userdata in the same order (the size from each side's decode).
        std::vector<std::string> a, b;
        for (const auto& e : original.log) if (e.rfind("notice ", 0) == 0) a.push_back(e.substr(e.find(" worker ") + 17u));
        for (const auto& e : native.log) if (e.rfind("notice ", 0) == 0) b.push_back(e.substr(e.find(" worker ") + 17u));
        check(a.size() == 2u && a == b, what + ": the notices' arguments");
        if (!corrupt) {
            check(a.size() == 2u && a[0].find(hex(sizes[0])) != std::string::npos, what + ": the notice carries the size");
            check(memory.native_expand_stats().served_prepared == 2u && native.jobs->stats().worker_declined == 0u,
                  what + ": both placed from the worker's result");
        } else {
            check(memory.native_expand_stats().served_prepared == 1u, what + ": the changed stream's copy was refused");
            check(memory.native_expand_stats().decoded_here + native.jobs->stats().expanded_by_owner == 1u,
                  what + ": the changed stream decoded at the publication");
        }
        check(native.jobs->pending() == 0u && native.jobs->stats().published == 2u, what + ": both published");
    }

    // The synchronous paths with a GameMemory (C++ only, no guest call): a stored member (the archive's member 3's
    // bytes, flagged stored) by the host memmove, and member 4 without a callback by the host decoder - against the
    // original's memcpy and PRS expand. The test's PRS stand-in is the host decoder (proven equal to the game's on every
    // disc member, asset_import_tests); this checks the extract's paths, sizes and bytes.
    for (const bool stored : {true, false}) {
        const std::string what = stored ? "synchronous, a stored member" : "synchronous, a compressed member";
        ExtractMachine original(dol, false), native(dol, true);
        GameMemory memory(native.cpu);
        native.jobs->set_game_memory(&memory);
        const u32 index = stored ? 3u : 4u;
        for (ExtractMachine* m : {&original, &native}) {
            m->bus.write_block(kArchive, data, static_cast<u32>(archive.size() - 0xCu));
            const u8* e = data + 0x34u + index * 0x38u;
            m->bus.write32(entry_at(index) + 0x2Cu, stored ? 0x1234u : le32(e + 0x2Cu));
            m->bus.write32(entry_at(index) + 0x30u, kArchive + le32(e + 0x30u));
            m->bus.write32(entry_at(index) + 0x34u, stored ? 0u : le32(e + 0x34u));
            m->open(0x55u, 0u);
            m->cpu.gpr[3] = kOne;
            m->cpu.gpr[4] = index;
            m->cpu.gpr[5] = kArchiveDest;
            m->cpu.gpr[6] = 0u;
            m->cpu.gpr[7] = kUserdata;
            check(m->run(kExtract), what + ": the extract returned");
        }
        check(original.cpu.gpr[3] == native.cpu.gpr[3] && native.cpu.gpr[3] != 0u,
              what + ": r3 original " + hex(original.cpu.gpr[3]) + " native " + hex(native.cpu.gpr[3]));
        for (u32 r = 14u; r < 32u; ++r) check(original.cpu.gpr[r] == native.cpu.gpr[r], what + ": r" + std::to_string(r));
        bool same = true;
        for (u32 i = 0; i < original.cpu.gpr[3] + 0x40u; ++i)
            same &= original.bus.read8(kArchiveDest + i) == native.bus.read8(kArchiveDest + i);
        check(same, what + ": the destination bytes");
        check(original.log.size() == 1u && native.log.empty(),
              what + ": the original made one guest call, the native none");
        check(stored ? native.jobs->stats().stored_native == 1u : native.jobs->stats().synchronous_native == 1u,
              what + ": served natively");
    }
}

// W2: a read request through TBGReadManager::update and its completion - the original's TBGRead (ctor, step as its
// thread runs it, the reaper's delete) against the native queue and its publication at the reaper's entry.
void check_w2(const std::vector<u8>& dol) {
    struct Case {
        const char* name;
        u32 in_flight;
        u8 pause;
        bool request, destination, open_fails;
    };
    // The update's every exit: a read in flight, paused, no request, a failed open; and a read with / without a
    // destination of its own.
    for (const Case& k : {Case{"a read in flight", 1u, 0u, true, false, false}, Case{"paused", 0u, 1u, true, false, false},
                          Case{"no request", 0u, 0u, false, false, false}, Case{"the open fails", 0u, 0u, true, false, true},
                          Case{"a read, its destination allocated", 0u, 0u, true, false, false},
                          Case{"a read into the requester's destination", 0u, 0u, true, true, false}}) {
        const std::string what = std::string("W2, ") + k.name;
        ExtractMachine original(dol, false), native(dol, true);
        require(native.jobs->w2(), "mode 5 installed");
        for (ExtractMachine* m : {&original, &native}) {
            m->bus.write32(kInFlight, k.in_flight);
            m->bus.write8(kPause, k.pause);
            for (u32 i = 0; i < 3u; ++i) m->bus.write32(kLists + i * 4u, 0u);
            m->bus.write32(kHolderAt, kHolder);
            m->bus.write32(kHolder, kReadParent);
            m->bus.write32(kFile + 4u, 0x1234u);
            m->bus.write32(kFile + 0x100u, k.open_fails ? 1u : 0u);
            if (k.request) {
                // A node in list 1 (list 0 empty): next, the six words (name, destination, size, -, callback, -).
                const u32 words[7] = {0u, kNode + 0x1Cu, k.destination ? 0x81600000u : 0u, 0x777u, 0xAAAA0003u,
                                      0x8004CC28u, 0xAAAA0005u};
                for (u32 i = 0; i < 7u; ++i) m->bus.write32(kNode + i * 4u, words[i]);
                m->bus.write32(kLists + 4u, kNode);
            }
        }
        const auto update = [](ExtractMachine& m) {
            m.cpu.gpr[3] = kReadManager;
            return m.run(kReadUpdate);
        };
        original.fill_stack();
        native.fill_stack();
        check(update(original) && update(native), what + ": the updates returned");
        const bool spawns = k.in_flight == 0u && k.pause == 0u && k.request && !k.open_fails;
        // Every exit's stack equals the original's; a spawn's own frame does (the original builds a TBGRead below it).
        check(spawns ? same_stack(original, native, kStack - 0x20u, kStack + 0x8u)
                     : same_stack(original, native, kWindowLow, kWindowHigh),
              what + ": the stack (the frames)");
        for (u32 r = 14u; r < 32u; ++r) check(original.cpu.gpr[r] == native.cpu.gpr[r], what + ": r" + std::to_string(r));
        check(original.bus.read32(kInFlight) == native.bus.read32(kInFlight), what + ": the in-flight count after the update");
        for (u32 i = 0; i < 3u; ++i)
            check(original.bus.read32(kLists + i * 4u) == native.bus.read32(kLists + i * 4u), what + ": the request lists");
        for (u32 i = 0; i < 7u; ++i)
            check(original.bus.read32(kNode + i * 4u) == native.bus.read32(kNode + i * 4u), what + ": the request's words");
        // The original's worker: new 0x390 (the TBGRead), the Thread base, the name copy.
        u32 worker = 0u;
        std::vector<std::string> expected;
        for (const std::string& e : original.log) {
            if (e.rfind("new 00000390 = ", 0) == 0) {
                worker = static_cast<u32>(std::stoul(e.substr(15), nullptr, 16));
                continue;
            }
            if (e.rfind("Thread ctor", 0) == 0 || e == "name copy") continue;
            expected.push_back(e);
        }
        check(native.log == expected, what + ": the update's guest calls are the original's without the TBGRead");
        if (native.log != expected) {
            for (const auto& e : original.log) std::printf("  original: %s\n", e.c_str());
            for (const auto& e : native.log) std::printf("  native:   %s\n", e.c_str());
        }
        const bool reads = k.in_flight == 0u && k.pause == 0u && k.request && !k.open_fails;
        check((worker != 0u) == reads && native.jobs->pending() == (reads ? 1u : 0u), what + ": a read queued exactly when the original made a TBGRead");
        if (!reads) continue;
        check(native.bus.read32(kInFlight) == 1u, what + ": the in-flight count is 1");
        // The completion: the original's step (as its thread runs it) and the reaper's delete; the native publication.
        const std::size_t original_before = original.log.size(), native_before = native.log.size();
        original.cpu.gpr[3] = worker;
        check(original.run(kReadStep) && original.cpu.gpr[3] == 0u, what + ": the original step returns 0");
        check(original.bus.read32(kInFlight) == 1u, what + ": the original's count stays 1 until the delete");
        original.cpu.gpr[3] = worker;
        original.cpu.gpr[4] = 1u;
        check(original.run(kReadDtor), what + ": the original's delete");
        // The publication leaves the main thread's stack as a reaper call with nothing queued leaves it (the original
        // ran this work on the TBGRead's own stack).
        ExtractMachine idle(dol, true);
        native.fill_stack();
        idle.fill_stack();
        native.cpu.gpr[3] = kManagerTable + 0x800u;
        idle.cpu.gpr[3] = kManagerTable + 0x800u;
        check(native.run(kReaper), what + ": the native publication");
        check(idle.run(kReaper), what + ": the reaper with nothing queued");
        check(same_stack(native, idle, kWindowLow, kWindowHigh), what + ": the publication left the main stack unchanged");
        check(original.bus.read32(kInFlight) == 0u && native.bus.read32(kInFlight) == 0u, what + ": the in-flight count is 0");
        std::vector<std::string> a(original.log.begin() + static_cast<std::ptrdiff_t>(original_before), original.log.end());
        std::vector<std::string> b(native.log.begin() + static_cast<std::ptrdiff_t>(native_before), native.log.end());
        u32 task_a = 0u, task_b = 0u;
        std::vector<std::string> a_kept;
        for (const std::string& e : a) {
            if (e == "hold" || e == "Thread dtor" || e == "delete") continue;
            if (e.rfind("new 00000048 = ", 0) == 0) {
                task_a = static_cast<u32>(std::stoul(e.substr(15), nullptr, 16));
                a_kept.push_back("new 00000048");
                continue;
            }
            a_kept.push_back(e.rfind("Task ", 0) == 0 ? e.substr(0, 5) + e.substr(14) : e);
        }
        std::vector<std::string> b_kept;
        for (const std::string& e : b) {
            if (e.rfind("new 00000048 = ", 0) == 0) {
                task_b = static_cast<u32>(std::stoul(e.substr(15), nullptr, 16));
                b_kept.push_back("new 00000048");
                continue;
            }
            b_kept.push_back(e.rfind("Task ", 0) == 0 ? e.substr(0, 5) + e.substr(14) : e);
        }
        check(a_kept == b_kept, what + ": the completion's guest calls (read, new 0x48, Task under the read manager)");
        if (a_kept != b_kept) {
            for (const auto& e : a) std::printf("  original: %s\n", e.c_str());
            for (const auto& e : b) std::printf("  native:   %s\n", e.c_str());
        }
        check(task_a != 0u && task_b != 0u, what + ": both built the Task");
        // The Task: every byte of its 0x48 equal but the worker word +0x44 (the original's TBGRead, the native's 0).
        for (u32 at = 0; at < 0x48u; at += 4u) {
            const u32 x = original.bus.read32(task_a + at), y = native.bus.read32(task_b + at);
            if (at == 0x44u) {
                check(x == worker && y == 0u, what + ": the Task's worker word (original the TBGRead, native 0)");
                continue;
            }
            check(x == y, what + ": Task +0x" + hex(at).substr(6) + " original " + hex(x) + " native " + hex(y));
        }
        const u32 destination = original.bus.read32(task_a + 0x2Cu);
        bool same = destination != 0u && destination == native.bus.read32(task_b + 0x2Cu);
        for (u32 i = 0; i < 0x1240u; ++i) same &= original.bus.read8(destination + i) == native.bus.read8(destination + i);
        check(same, what + ": the destination bytes");
        check(native.jobs->stats().reads_published == 1u && native.jobs->pending() == 0u, what + ": published");
    }
    // The queue's savestate block with a read job: back into mode 5; refused by mode 4.
    {
        ExtractMachine m(dol, true);
        m.bus.write32(kInFlight, 0u);
        m.bus.write8(kPause, 0u);
        for (u32 i = 0; i < 3u; ++i) m.bus.write32(kLists + i * 4u, 0u);
        m.bus.write32(kFile + 4u, 0x40u);
        m.bus.write32(kFile + 0x100u, 0u);
        const u32 words[7] = {0u, kNode + 0x1Cu, 0u, 0u, 0u, 0x8004CC28u, 0u};
        for (u32 i = 0; i < 7u; ++i) m.bus.write32(kNode + i * 4u, words[i]);
        m.bus.write32(kLists, kNode);
        m.cpu.gpr[3] = kReadManager;
        check(m.run(kReadUpdate) && m.jobs->pending() == 1u, "W2 savestate: a queued read");
        std::vector<u8> blob;
        m.jobs->save_state(blob);
        ExtractMachine restored(dol, true);
        const u8* cursor = blob.data();
        check(restored.jobs->load_state(cursor, blob.data() + blob.size(), 30u) && restored.jobs->pending() == 1u &&
                  cursor == blob.data() + blob.size(),
              "W2 savestate: the read job round-trips");
#if defined(_WIN32)
        _putenv_s("GEKKO_NATIVE_EXPAND_JOBS", "4");
#else
        setenv("GEKKO_NATIVE_EXPAND_JOBS", "4", 1);
#endif
        ExtractMachine mode4(dol, true);
        cursor = blob.data();
        check(!mode4.jobs->load_state(cursor, blob.data() + blob.size(), 30u), "W2 savestate: refused without mode 5");
    }
}

std::vector<u8> read_file(const char* path) {
    std::ifstream input(path, std::ios::binary);
    return std::vector<u8>{std::istreambuf_iterator<char>(input), {}};
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::printf("usage: native_expand_jobs_tests <sys/main.dol>\n");
        return 2;
    }
#if defined(_WIN32)
    _putenv_s("GEKKO_NATIVE_EXPAND_JOBS", "1");
#else
    setenv("GEKKO_NATIVE_EXPAND_JOBS", "1", 1);
#endif
    try {
        const std::vector<u8> dol = read_file(argv[1]);
        require(dol.size() > 0x100u, "cannot read the DOL");
        for (const u32 size : {0u, 0x800u, 0x3000u}) {
            Machine original(dol, false), native(dol, true);
            const bool a = original.construct(size, kFromExpand), b = native.construct(size, kFromExpand);
            const std::string what = "stack size " + hex(size);
            check(a && b, what + ": both constructors returned (original: " + original.cpu.stop().detail +
                              "; native: " + native.cpu.stop().detail + "; native pc " + hex(native.cpu.pc) + ")");
            check(original.cpu.gpr[3] == kWorker && native.cpu.gpr[3] == kWorker, what + ": r3 = the worker");
            for (const u32 r : {1u, 2u, 13u}) check(original.cpu.gpr[r] == native.cpu.gpr[r], what + ": r" + std::to_string(r));
            for (u32 r = 14u; r < 32u; ++r) check(original.cpu.gpr[r] == native.cpu.gpr[r], what + ": r" + std::to_string(r));
            // The guest calls: the original's minus the two thread calls.
            std::vector<std::string> expected;
            bool created = false, resumed = false;
            for (const auto& e : original.log) {
                if (e.rfind("OSCreateThread", 0) == 0) {
                    created = e.find(" 8035A01C ") != std::string::npos && e.find(" 00000010 00000001") != std::string::npos;
                    continue;
                }
                if (e.rfind("OSResumeThread", 0) == 0) {
                    resumed = e == "OSResumeThread " + hex(kWorker + 0x10u);
                    continue;
                }
                expected.push_back(e);
            }
            check(created && resumed, what + ": the original creates (entry, priority 16, detached) and resumes");
            check(native.log == expected, what + ": the native guest calls are the original's without the thread");
            for (const auto& e : native.log) check(e.rfind("OS", 0) != 0, what + ": the native made a thread call");
            // The fields outside the OSThread, and the stack's guard.
            for (const u32 at : {0x0u, 0x4u, 0x8u, 0xCu, 0x328u}) {
                check(original.bus.read32(kWorker + at) == native.bus.read32(kWorker + at),
                      what + ": worker +0x" + hex(at).substr(5) + " original " + hex(original.bus.read32(kWorker + at)) +
                          " native " + hex(native.bus.read32(kWorker + at)));
            }
            const u32 stack = original.bus.read32(kWorker + 0xCu);
            for (u32 i = 0; i < 0x20u; i += 4u)
                check(original.bus.read32(stack + i) == native.bus.read32(stack + i), what + ": the stack's first bytes");
            const u16 state = static_cast<u16>(native.bus.read32(kWorker + 0x10u + 0x2C8u) >> 16);
            check(state == 0u, what + ": the native OSThread state is terminated (0)");
            check(native.jobs->stats().created == 1u, what + ": created count");
        }
        {
            Machine native(dol, true);
            const bool b = native.construct(0u, kFromRead);
            bool thread = false;
            for (const auto& e : native.log) thread |= e.rfind("OSCreateThread", 0) == 0;
            check(b && thread && native.jobs->stats().created == 0u, "TBGRead's constructor is not served");
        }
#if defined(_WIN32)
        _putenv_s("GEKKO_NATIVE_EXPAND_JOBS", "3");
#else
        setenv("GEKKO_NATIVE_EXPAND_JOBS", "3", 1);
#endif
        check_phase2(dol);
#if defined(_WIN32)
        _putenv_s("GEKKO_NATIVE_EXPAND_JOBS", "4");
#else
        setenv("GEKKO_NATIVE_EXPAND_JOBS", "4", 1);
#endif
        std::string archive = argv[1];
        archive = archive.substr(0, archive.find_last_of("/\\")) + "/../files/character/amy.one";
        check_phase3(dol, archive);
#if defined(_WIN32)
        _putenv_s("GEKKO_NATIVE_EXPAND_JOBS", "5");
#else
        setenv("GEKKO_NATIVE_EXPAND_JOBS", "5", 1);
#endif
        check_w2(dol);
    } catch (const std::exception& error) {
        std::printf("ERROR: %s\n", error.what());
        return 1;
    }
    std::printf("native expand jobs: %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
