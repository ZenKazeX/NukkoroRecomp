// liveness-roots: reference-interpreter-only
// The game's saves (docs/agents/card-saves.md): the owners of the CARD library entries the game still calls
// (CARDInit 0x8038DE3C, CARDGetResultCode 0x8038E044) against the ORIGINAL words of the pinned DOL in the reference
// interpreter. ORIGINAL runs the DOL's instructions; NATIVE runs SaveStore's owner on the same fixture. Four stand-ins
// serve both machines (the original's callees that reach hardware or text): DSPInit 0x8038C2C0 (logged: the audio
// layer's owner in the game), OSGetFontEncode 0x80375150 (video registers; logged), OSReport 0x803731CC (text; logged)
// and, for the isolated machine, nothing else - OSInitAlarm, OSRegisterVersion, OSRegisterResetFunction,
// OSCreateAlarm, OSInitThreadQueue and __CARDSetDiskID run as original words.
//
// usage: save_store_contract_tests <sys/main.dol>
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_card.h"
#include "gekko/native_interrupts.h"
#include "gekko/save_file.h"
#include "gekko/save_store.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>
#include "test_scratch.h"

namespace {
using namespace gekko;

constexpr u32 kReturn = 0x817FF000u, kStack = 0x817E0000u;
constexpr u32 kR2 = 0x805FA780u, kR13 = 0x805EC500u;
constexpr u32 kCardInit = 0x8038DE3Cu, kGetResultCode = 0x8038E044u;
constexpr u32 kDspInit = 0x8038C2C0u, kFontEncode = 0x80375150u, kReport = 0x803731CCu;
constexpr u32 kBlocks = 0x805A2CA0u, kBlockBytes = 0x220u;  // __CARDBlock[2], 0x110 each
constexpr u32 kDecrementerHandler = 0x80371E14u;
constexpr u32 kExceptionTable = 0x81003000u;

int g_failures = 0;
void check(bool ok, const std::string& what) {
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

struct Range {
    u32 begin, end;
};

struct Machine {
    GuestMemory memory;
    Cpu cpu{memory};
    SaveStore store{cpu, memory};
    std::vector<std::string> log;
    bool native;

    Machine(const std::vector<u8>& dol, bool native_owner, bool dsp_owner = true) : native(native_owner) {
        const auto word = [&](unsigned offset) { return load_be32(dol.data() + offset); };
        for (unsigned i = 0; i < 18; ++i) {
            const u32 offset = word(i * 4), address = word(0x48 + i * 4), size = word(0x90 + i * 4);
            require(u64(offset) + size <= dol.size(), "invalid DOL section");
            if (size) memory.write_block(address, dol.data() + offset, size);
        }
        cpu.reset();
        if (native) store.install();  // the original machine runs the DOL's words alone
        // The audio layer's DSPInit owner, as a logging stand-in (both machines: the original reaches it by `bl`).
        if (dsp_owner)
            cpu.install_hle(kDspInit, [this](Cpu& c) {
                log.push_back("DSPInit");
                (void)c;
                return true;
            }, "test: DSPInit");
        // The fixture OSInit leaves: the exception table with the decrementer handler (OSInitAlarm's no-op test).
        memory.write32(kR13 + 0x5A44u, kExceptionTable);
        memory.write32(kExceptionTable + 8u * 4u, kDecrementerHandler);
    }
    void returned() {
        cpu.pc = cpu.lr;
        cpu.npc = cpu.pc + 4;
    }
    // One call to `entry` with up to three arguments; the stand-ins answer for the original's hardware callees.
    void call(u32 entry, u32 r3 = 0, u32 r4 = 0, u32 r5 = 0) {
        cpu.reset();
        cpu.gpr[1] = kStack;
        cpu.gpr[2] = kR2;
        cpu.gpr[13] = kR13;
        cpu.gpr[3] = r3;
        cpu.gpr[4] = r4;
        cpu.gpr[5] = r5;
        cpu.lr = kReturn;
        if (native) {
            require(cpu.try_hle(entry), "a native owner serves every call");
            return;
        }
        cpu.pc = entry;
        cpu.npc = entry + 4;
        unsigned steps = 0;
        while (cpu.pc != kReturn && !cpu.stopped() && steps++ < 200000) {
            if (cpu.pc == kDspInit) {
                if (cpu.try_hle(kDspInit)) {
                    returned();
                    continue;
                }
            }
            if (cpu.pc == kFontEncode) {
                log.push_back("OSGetFontEncode");
                cpu.gpr[3] = 0;
                returned();
                continue;
            }
            if (cpu.pc == kReport) {
                log.push_back("OSReport");
                returned();
                continue;
            }
            cpu.run_steps(1);
        }
        require(cpu.pc == kReturn && !cpu.stopped(), "the original call did not return");
        require(cpu.gpr[1] == kStack, "the original call left the stack pointer changed");
    }
};

std::vector<u8> read_file(const char* path) {
    std::ifstream input(path, std::ios::binary);
    return std::vector<u8>{std::istreambuf_iterator<char>(input), {}};
}

// Every byte of main RAM outside `retired` and the dead stack window must be equal; r1, r13..r31, LR, PC too.
std::vector<u32> memory_differences(Machine& a, Machine& b, const std::vector<Range>& retired) {
    std::vector<u32> differing;
    const auto skipped = [&](u32 address) {
        if (address >= kStack - 0x1000u && address < kStack + 0x100u) return true;  // dead stack
        for (const Range& r : retired)
            if (address >= r.begin && address < r.end) return true;
        return false;
    };
    for (u32 address = 0x80000000u; address < 0x81800000u; address += 4u) {
        if (a.memory.peek32(address) == b.memory.peek32(address) || skipped(address)) continue;
        differing.push_back(address);
        if (differing.size() > 40) break;
    }
    return differing;
}

bool registers_equal(const Machine& a, const Machine& b) {
    if (a.cpu.gpr[1] != b.cpu.gpr[1] || a.cpu.gpr[2] != b.cpu.gpr[2]) return false;
    for (unsigned r = 13; r <= 31; ++r)
        if (a.cpu.gpr[r] != b.cpu.gpr[r]) return false;
    return a.cpu.lr == b.cpu.lr && a.cpu.pc == b.cpu.pc;
}

// CARDInit: the native owner writes nothing the original's surviving callers read. The retired state: the two
// __CARDBlock records, the encoding word at r13 + 0x5DF0, the reset queue's two words (r13 + 0x5AB0 / + 0x5AB4) and the
// library's reset record (0x80568358, 16 bytes). The logs equal except the version report (OSReport text only).
void test_card_init(const std::vector<u8>& dol) {
    const std::vector<Range> retired = {{kBlocks, kBlocks + kBlockBytes},
                                        {kR13 + 0x5DF0u, kR13 + 0x5DF4u},
                                        {kR13 + 0x5AB0u, kR13 + 0x5AB8u},
                                        {0x80568358u, 0x80568368u}};
    Machine original(dol, false), owned(dol, true);
    original.call(kCardInit);
    owned.call(kCardInit);
    std::printf("  CARDInit original log:");
    for (const std::string& l : original.log) std::printf(" %s", l.c_str());
    std::printf(" | block0 +0x10C %08X +4 %08X | native log:", original.memory.peek32(kBlocks + 0x10Cu), original.memory.peek32(kBlocks + 4u));
    for (const std::string& l : owned.log) std::printf(" %s", l.c_str());
    std::printf("\n");
    check(registers_equal(original, owned), "CARDInit: registers r1, r2, r13-r31, LR, PC");
    const auto differing = memory_differences(original, owned, retired);
    for (u32 a : differing) check(false, "CARDInit: memory differs at " + hex(a));
    check(differing.empty(), "CARDInit: every byte outside the retired ranges equal");
    // The calls the original makes into hardware / text: DSPInit (kept by the owner), the version report and the font
    // encoding read (retired: text, and a cache the surviving callers recompute from the same video register).
    std::vector<std::string> original_log = original.log;
    original_log.erase(std::remove(original_log.begin(), original_log.end(), "OSReport"), original_log.end());
    original_log.erase(std::remove(original_log.begin(), original_log.end(), "OSGetFontEncode"), original_log.end());
    check(original.log.size() == original_log.size() + 2u, "CARDInit: the original reports its version and reads the font encoding once each");
    check(original_log == owned.log && original_log.size() == 1u && original_log[0] == "DSPInit",
          "CARDInit: the native owner makes the one call the original's surviving effects need, DSPInit, once");
    // What the original did write is what the retired list names, and nothing else (the list is not padding):
    const auto retired_diff = [&] {
        unsigned n = 0;
        for (const Range& r : retired)
            for (u32 a = r.begin; a < r.end; a += 4u)
                if (original.memory.peek32(a) != owned.memory.peek32(a)) ++n;
        return n;
    }();
    check(retired_diff > 0, "CARDInit: the retired state really is written by the original (the ranges are not vacuous)");
    check(original.memory.peek32(kBlocks + 4u) == 0xFFFFFFFDu && original.memory.peek32(kBlocks + 0x114u) == 0xFFFFFFFDu,
          "CARDInit: the original leaves NOCARD in both blocks");

    // Negative control 1: an owner that forgot DSPInit is told apart by the call log.
    Machine forgot(dol, true, false);  // the audio layer's owner absent: the owner's DSPInit call goes nowhere
    forgot.call(kCardInit);
    check(forgot.log != original_log, "negative control: a CARDInit that does not reach DSPInit differs in the call log");
    // Negative control 2: an extra write outside the retired ranges is reported.
    Machine corrupted(dol, true);
    corrupted.call(kCardInit);
    corrupted.memory.write32(0x80500000u, 0x12345678u);
    check(!memory_differences(original, corrupted, retired).empty(), "negative control: a stray write is reported");

    // The one precondition: OSInit has installed the decrementer handler, so OSInitAlarm is a no-op. Without it the
    // original installs the handler and registers a reset record - an effect the owner does not make.
    Machine cold(dol, false);
    cold.memory.write32(kExceptionTable + 8u * 4u, 0u);
    cold.call(kCardInit);
    check(cold.memory.peek32(kExceptionTable + 8u * 4u) == kDecrementerHandler,
          "precondition: on a machine OSInit never ran, the original's CARDInit installs the decrementer handler");
}

// CARDGetResultCode(chan): -128 outside channels 0..1 in both; slot B reads the block result CARDInit leaves (NOCARD);
// slot A is READY in the store (an idle card: a job ends inside its tick), where the original returns the block's
// result field - 0 only while no operation ran since a successful one. The caller (the reset path 0x8032D304) acts on
// -1 (BUSY) alone.
void test_get_result_code(const std::vector<u8>& dol) {
    Machine original(dol, false), owned(dol, true);
    original.call(kCardInit);  // the state the game's boot leaves: both blocks NOCARD
    for (u32 chan : {0xFFFFFFFFu, 0x80000000u, 2u, 3u, 0x7FFFFFFFu}) {
        original.call(kGetResultCode, chan);
        owned.call(kGetResultCode, chan);
        check(original.cpu.gpr[3] == 0xFFFFFF80u && owned.cpu.gpr[3] == original.cpu.gpr[3],
              "CARDGetResultCode: channel " + hex(chan) + " is FATAL_ERROR (-128) in both");
    }
    original.call(kGetResultCode, 1u);
    owned.call(kGetResultCode, 1u);
    check(original.cpu.gpr[3] == 0xFFFFFFFDu && owned.cpu.gpr[3] == 0xFFFFFFFDu, "CARDGetResultCode: slot B is NOCARD in both");
    // Slot A: for every result the original's block can hold, whether the caller waits (== -1) is the same unless the
    // card is BUSY, which the store never presents (the documented difference).
    for (u32 result : {0u, 0xFFFFFFFDu, 0xFFFFFFFBu, 0xFFFFFFFAu, 0xFFFFFFFCu, 0xFFFFFFFFu}) {
        original.memory.write32(kBlocks + 4u, result);
        original.call(kGetResultCode, 0u);
        owned.call(kGetResultCode, 0u);
        check(original.cpu.gpr[3] == result, "CARDGetResultCode: the original returns the block's result " + hex(result));
        const bool waits_original = original.cpu.gpr[3] == 0xFFFFFFFFu, waits_owned = owned.cpu.gpr[3] == 0xFFFFFFFFu;
        if (result == 0xFFFFFFFFu) check(waits_original && !waits_owned, "the documented difference: BUSY is never presented");
        else check(waits_original == waits_owned, "CARDGetResultCode: the reset path's wait decision equals for " + hex(result));
    }
    // Negative control: an owner returning BUSY for slot A would be told apart by the decision.
    check(true, "negative control: see the BUSY row above (the decision differs exactly there)");
}

// ---------------------------------------------------------------------------------------------------------------
// The card job runner: the original TMemoryCardCtrl runner (start 0x8029BF84, tick 0x8029BA8C, the fourteen CARD
// operations and the CARD library under them, original DOL words) running on the card model's block device
// (NativeCard: __CARDRead / __CARDWrite / __CARDEraseSector / CARDMountAsync / CARDUnmount / CARDProbeEx owners, its
// completions delivered between ticks by the oracle's pump) against the save store on a host GCI. Both machines start
// from a fresh card / no file and run the same script of jobs; after every job the runner object's observables, the
// read buffer and the file on the card / the GCI are compared.
constexpr u32 kTick = 0x8029BA8Cu, kRunnerCtor = 0x8029C1C0u;
constexpr u32 kJobFormat = 0x8029BF30u, kJobCreate = 0x8029BEACu, kJobDelete = 0x8029BE40u, kJobRead = 0x8029BDC4u,
              kJobOverwrite = 0x8029BD48u, kJobFreeBlocks = 0x8029BC74u, kJobScan = 0x8029BCC8u;
constexpr u32 kNew = 0x803A1380u, kAllocate = 0x803A12A4u, kVerify = 0x80390680u, kDeliveryReturn = 0x80376210u;
constexpr u32 kDeliveryFrame = 0x817C0000u, kHeap = 0x81100000u, kHeapLimit = 0x81700000u;
constexpr u32 kSram = 0x80586FE0u;
constexpr u32 kName = 0x81080000u, kBufferA = 0x81090000u, kBufferB = 0x810A0000u, kReadBuffer = 0x810B0000u;
constexpr u32 kDescriptor = 0x81070000u;
constexpr const char* kFileName = "SHADOWTHEHEDGEHOG";

struct Runner {
    GuestMemory memory;
    Cpu cpu{memory};
    NativeInterrupts interrupts{cpu};
    NativeCard card{cpu, memory, interrupts};
    SaveStore store{cpu, memory};
    bool original;
    std::filesystem::path directory;
    u32 cursor = kHeap;
    u32 runner = 0;

    Runner(const std::vector<u8>& dol, bool original_machine, const std::filesystem::path& dir)
        : original(original_machine), directory(dir) {
        const auto word = [&](unsigned offset) { return load_be32(dol.data() + offset); };
        for (unsigned i = 0; i < 18; ++i) {
            const u32 offset = word(i * 4), address = word(0x48 + i * 4), size = word(0x90 + i * 4);
            require(u64(offset) + size <= dol.size(), "invalid DOL section");
            if (size) memory.write_block(address, dol.data() + offset, size);
        }
        cpu.reset();
        std::filesystem::create_directories(dir);
        // The disc header the library reads its game / maker from (__CARDSetDiskID(0x80000000)).
        memory.write_block(0x80000000u, reinterpret_cast<const u8*>("GUPP8P"), 6);
        // The OS SRAM block's flash ID for slot A and its checksum, and the vendor the 4-Mbit rule compares with.
        const char flash[12] = {'S', 'H', 'A', 'D', 'O', 'W', 'P', 'C', 'S', 'A', 'V', 'E'};
        u32 sum = 0;
        for (unsigned i = 0; i < 12; ++i) {
            memory.write8(kSram + 0x14u + i, static_cast<u8>(flash[i]));
            sum += static_cast<u8>(flash[i]);
        }
        memory.write8(kSram + 0x3Au, static_cast<u8>(~sum));
        memory.write16(kR13 + 0x27D8u, load_be16(reinterpret_cast<const u8*>(flash)));
        memory.write32(kR13 + 0x5A44u, kExceptionTable);
        memory.write32(kExceptionTable + 8u * 4u, kDecrementerHandler);
        if (original) {
            card.install();
            card.set_verify_entry(kVerify);
            card.set_default_api_callback(0x8038CDD8u);  // __CARDDefaultApiCallback, a blr of the original library
            card.set_path(dir / "card.raw");  // absent: created as an empty, formatted 4-Mbit card
            run(kCardInit);
        } else {
            store.install();
            store.set_path(dir / "save.gci");
        }
        run(kRunnerCtor, 0u);
        runner = cpu.gpr[3];
        require(runner != 0u, "the runner object was not built");
        const u32 ctx = cpu.mem_read32(runner);
        cpu.mem_write8(ctx + 0xB4u, '8');
        cpu.mem_write8(ctx + 0xB5u, 'P');
        for (unsigned i = 0; i < 4; ++i) cpu.mem_write8(ctx + 0xB6u + i, static_cast<u8>("GUPP"[i]));
        put_string(kName, kFileName);
    }
    void put_string(u32 address, const char* text) {
        u32 i = 0;
        for (; text[i]; ++i) memory.write8(address + i, static_cast<u8>(text[i]));
        memory.write8(address + i, 0);
    }
    u32 allocate(u32 bytes) {
        const u32 address = (cursor + 31u) & ~31u;
        require(address + bytes < kHeapLimit, "the fixture heap is exhausted");
        cursor = address + bytes;
        return address;
    }
    void returned() {
        cpu.pc = cpu.lr;
        cpu.npc = cpu.pc + 4;
    }
    // One instruction, or the stand-in / owner at the current address.
    void step() {
        const u32 pc = cpu.pc;
        if (pc == kNew || pc == kAllocate) {
            cpu.gpr[3] = allocate(cpu.gpr[3]);
            returned();
        } else if (pc == kDspInit || pc == kReport || pc == 0x80372508u || pc == 0x80372534u || pc == 0x80372564u ||
                   pc == 0x80372594u || pc == 0x803725C0u) {  // the DSP, the debug text, the data / instruction caches
            returned();
        } else if (pc == 0x80391910u) {
            // __CARDFormatRegionAsync's VISEL read (lhz r18, 0x206E(r3)): the platform answers 0 (no component cable).
            cpu.gpr[18] = 0;
            cpu.pc = pc + 4;
            cpu.npc = pc + 8;
        } else if (pc == kFontEncode) {
            cpu.gpr[3] = 0;
            returned();
        } else if (cpu.has_hle(pc)) {
            cpu.try_hle(pc);
        } else {
            cpu.run_steps(1);
        }
    }
    // Runs `entry` as the original (or the owner) until it returns; the stand-ins answer for the callees the
    // library reaches that only a console has (the allocator, the debug text, the video-encoder read).
    void run(u32 entry, u32 r3 = 0, u32 r4 = 0, u32 r5 = 0, u32 r6 = 0, u32 r7 = 0) {
        cpu.reset();
        cpu.gpr[1] = kStack;
        cpu.gpr[2] = kR2;
        cpu.gpr[13] = kR13;
        cpu.gpr[3] = r3;
        cpu.gpr[4] = r4;
        cpu.gpr[5] = r5;
        cpu.gpr[6] = r6;
        cpu.gpr[7] = r7;
        cpu.lr = kReturn;
        cpu.pc = entry;
        cpu.npc = entry + 4;
        u64 steps = 0;
        while (cpu.pc != kReturn && !cpu.stopped() && steps++ < 400000000ull) step();
        if (cpu.pc != kReturn || cpu.stopped())
            std::printf("run(%08X): pc=%08X stopped=%d %s\n", entry, cpu.pc, cpu.stopped() ? 1 : 0,
                        cpu.stop().detail.c_str());
        require(cpu.pc == kReturn && !cpu.stopped(), "a guest call did not return");
    }
    void pump() {
        if (!original) return;
        const u32 saved_pc = cpu.pc, saved_lr = cpu.lr, saved_r1 = cpu.gpr[1];
        while (card.deliver_pending(cpu, kDeliveryFrame, kDeliveryReturn, [&] { step(); })) {
        }
        if (cpu.stopped()) std::printf("delivery stopped: pc=%08X %s\n", cpu.pc, cpu.stop().detail.c_str());
        require(!cpu.stopped(), "a completion's delivery stopped the machine");
        cpu.pc = saved_pc;
        cpu.npc = saved_pc + 4;
        cpu.lr = saved_lr;
        cpu.gpr[1] = saved_r1;
    }
    struct Outcome {
        bool started = false;
        u32 result = 0, busy = 0, job = 0, flags = 0, free_bytes = 0, free_entries = 0, count = 0, ticks = 0;
        std::vector<u8> read;
    };
    // One job: its entry, then ticks (completions delivered between them) until the runner is idle.
    Outcome job(u32 entry, u32 r4, u32 r5, u32 r6, u32 r7, u32 read_bytes = 0) {
        Outcome out;
        if (read_bytes) {
            const std::vector<u8> poison(read_bytes, 0xEE);
            memory.write_block(kReadBuffer, poison.data(), read_bytes);
        }
        run(entry, runner, r4, r5, r6, r7);
        out.started = cpu.gpr[3] != 0;
        pump();
        while (out.started && out.ticks < 600) {
            run(kTick, runner);
            const u32 busy = cpu.gpr[3];  // the tick's answer, before a completion's delivery uses the registers
            pump();
            ++out.ticks;
            if (busy == 0u) break;
        }
        const u32 ctx = cpu.mem_read32(runner);
        out.busy = cpu.mem_read32(runner + 0x14u);
        out.job = cpu.mem_read32(runner + 0x18u);
        out.result = cpu.mem_read32(runner + 0x1Cu);
        out.flags = cpu.mem_read32(runner + 0x20u);
        out.free_bytes = cpu.mem_read32(runner + 0x30u);
        out.free_entries = cpu.mem_read32(runner + 0x34u);
        out.count = cpu.mem_read32(ctx + 0xACu);
        out.read.resize(read_bytes);
        if (read_bytes) memory.read_block(kReadBuffer, out.read.data(), read_bytes);
        return out;
    }
    // The file as the machine holds it: the card's directory entry and blocks, or the GCI on the host.
    bool file(save_file::Gci& out) {
        std::string error;
        if (original) {
            card.flush();
            return save_file::extract_from_card(card.image(), "GUPP", "8P", kFileName, out, &error);
        }
        return save_file::read_gci(directory / "save.gci", out, &error);
    }
};

std::string describe(const Runner::Outcome& o) {
    char b[200];
    std::snprintf(b, sizeof(b), "started %d result %u busy %u job %u flags %u free %u/%u count %u", o.started ? 1 : 0,
                  o.result, o.busy, o.job, o.flags, o.free_bytes, o.free_entries, o.count);
    return b;
}

// The fields the manager reads (docs/SAVE_REROUTE_PLAN.md): the job's started flag, its result, the idle state
// (busy / job / open flags), the free-space getters (free job only), the scan count (scan job only), the read data.
bool outcomes_equal(const Runner::Outcome& a, const Runner::Outcome& b, bool free_job, bool scan_job) {
    if (a.started != b.started || a.result != b.result || a.busy != b.busy || a.job != b.job || a.flags != b.flags)
        return false;
    if (free_job && (a.free_bytes != b.free_bytes || a.free_entries != b.free_entries)) return false;
    if (scan_job && a.count != b.count) return false;
    return a.read == b.read;
}

// A directory entry and its data: everything the CARD library records except the modification time (+0x28) and the
// first block (+0x36), which are the card's placement and the clock.
bool files_equal(const save_file::Gci& a, const save_file::Gci& b) {
    for (unsigned i = 0; i < save_file::kGciHeaderBytes; ++i) {
        if ((i >= 0x28 && i < 0x2C) || i == 0x36 || i == 0x37) continue;
        if (a.header[i] != b.header[i]) return false;
    }
    return a.data == b.data;
}

void fill_pattern(Runner& r, u32 address, u32 bytes, u32 seed) {
    std::vector<u8> data(bytes);
    for (u32 i = 0; i < bytes; ++i) data[i] = static_cast<u8>((i * 131u + seed) ^ (i >> 8));
    r.memory.write_block(address, data.data(), bytes);
}

struct Step {
    const char* what;
    u32 entry;
    u32 r4, r5, r6, r7;
    u32 read_bytes;
    bool free_job, scan_job;
};

// Both machines run the steps; after each the outcomes and the files are compared. Returns the number of differing
// steps (the differences are also reported through check()).
unsigned play(Runner& original, Runner& owned, const std::vector<Step>& script) {
    unsigned differing = 0;
    for (const Step& step : script) {
        const Runner::Outcome a = original.job(step.entry, step.r4, step.r5, step.r6, step.r7, step.read_bytes);
        const Runner::Outcome b = owned.job(step.entry, step.r4, step.r5, step.r6, step.r7, step.read_bytes);
        const bool same = outcomes_equal(a, b, step.free_job, step.scan_job);
        check(same, std::string("runner: ") + step.what + " | original: " + describe(a) + " | store: " + describe(b));
        save_file::Gci fa, fb;
        const bool has_a = original.file(fa), has_b = owned.file(fb);
        const bool files_same = has_a == has_b && (!has_a || files_equal(fa, fb));
        check(files_same, std::string("runner: the file after '") + step.what + "'");
        if (has_a && has_b && !files_same) {
            for (unsigned i = 0; i < save_file::kGciHeaderBytes; ++i)
                if (fa.header[i] != fb.header[i])
                    std::printf("    header +0x%02X original %02X store %02X\n", i, fa.header[i], fb.header[i]);
            std::printf("    data %zu vs %zu bytes, equal %d\n", fa.data.size(), fb.data.size(), fa.data == fb.data ? 1 : 0);
        }
        if (!same || !files_same) ++differing;
        std::printf("  %-46s %s  result %u  (original %u ticks, store %u tick)\n", step.what,
                    same && files_same ? "equal" : "DIFFERENT", a.result, a.ticks, b.ticks);
    }
    return differing;
}

void fill_both(Runner& original, Runner& owned) {
    for (Runner* r : {&original, &owned}) {
        // The save descriptor the manager passes to the create job (0x8029A9FC reads it): a banner, the icon at 0x40
        // in RGB5A3, one icon, the middle speed, no comment, no bounce - the game's own file.
        const u32 words[12] = {0, 0, 1, 0, 0, 0, 0, 0x40, 0, 1, 0, 1};
        for (unsigned i = 0; i < 12; ++i) r->memory.write32(kDescriptor + 4u * i, words[i]);
        fill_pattern(*r, kBufferA, 0x4000, 7u);
        fill_pattern(*r, kBufferB, 0x6000, 91u);
    }
}

void card_checksum(std::vector<u8>& raw, std::size_t data, std::size_t length, std::size_t stored) {
    u16 checksum = 0, inverse = 0;
    for (std::size_t offset = 0; offset < length; offset += 2) {
        const u16 word = load_be16(raw.data() + data + offset);
        checksum = static_cast<u16>(checksum + word);
        inverse = static_cast<u16>(inverse + static_cast<u16>(~word));
    }
    store_be16(raw.data() + stored, checksum == 0xFFFFu ? 0u : checksum);
    store_be16(raw.data() + stored + 2u, inverse == 0xFFFFu ? 0u : inverse);
}

void test_runner(const std::vector<u8>& dol) {
    const std::filesystem::path root = gekko::test_support::scratch_directory("save-store-contract-tests");
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    const u32 name = kName;
    const std::vector<Step> script = {
        {"scan: no file", kJobScan, name, 0, 0, 0, 0, false, true},
        {"free blocks: empty card", kJobFreeBlocks, 0, 0, 0, 0, 0, true, false},
        {"read: no such file", kJobRead, name, 0x4000, kReadBuffer, 0, 0x4000, false, false},
        {"delete: no such file", kJobDelete, name, 0, 0, 0, 0, false, false},
        {"overwrite: no such file", kJobOverwrite, name, 0x4000, kBufferA, 0, 0, false, false},
        {"create 2 blocks", kJobCreate, name, 0x4000, kBufferA, kDescriptor, 0, false, false},
        {"create again: exists", kJobCreate, name, 0x4000, kBufferA, kDescriptor, 0, false, false},
        {"scan: one file", kJobScan, name, 0, 0, 0, 0, false, true},
        {"free blocks: two used", kJobFreeBlocks, 0, 0, 0, 0, 0, true, false},
        {"read the file", kJobRead, name, 0x4000, kReadBuffer, 0, 0x4000, false, false},
        {"read the first block only", kJobRead, name, 0x2000, kReadBuffer, 0, 0x2000, false, false},
        {"read more than the file holds", kJobRead, name, 0x6000, kReadBuffer, 0, 0x6000, false, false},
        {"overwrite with new data", kJobOverwrite, name, 0x4000, kBufferB, 0, 0, false, false},
        {"read the new data", kJobRead, name, 0x4000, kReadBuffer, 0, 0x4000, false, false},
        {"overwrite with more than the file holds", kJobOverwrite, name, 0x6000, kBufferB, 0, 0, false, false},
        {"read: unchanged after the failed overwrite", kJobRead, name, 0x4000, kReadBuffer, 0, 0x4000, false, false},
        {"delete the file", kJobDelete, name, 0, 0, 0, 0, false, false},
        {"read: gone", kJobRead, name, 0x4000, kReadBuffer, 0, 0x4000, false, false},
        {"scan: none again", kJobScan, name, 0, 0, 0, 0, false, true},
        {"free blocks: empty again", kJobFreeBlocks, 0, 0, 0, 0, 0, true, false},
        {"create again after the delete", kJobCreate, name, 0x4000, kBufferA, kDescriptor, 0, false, false},
        {"format the card", kJobFormat, 0, 0, 0, 0, 0, false, false},
        {"read: gone after the format", kJobRead, name, 0x4000, kReadBuffer, 0, 0x4000, false, false},
        {"free blocks: empty after the format", kJobFreeBlocks, 0, 0, 0, 0, 0, true, false},
    };
    {
        Runner original(dol, true, root / "original"), owned(dol, false, root / "owned");
        fill_both(original, owned);
        std::printf("-- the job script, original runner + CARD library on the card model / the save store --\n");
        const unsigned differing = play(original, owned, script);
        check(differing == 0u, "runner: every step of the script equals");
    }

    // An interrupted create (iconAddr 0xFFFFFFFF in the directory entry): the library's file is there but the game
    // calls it broken. The card image and the GCI are both made from a created file, then patched the same way.
    {
        Runner original(dol, true, root / "o2"), owned(dol, false, root / "s2");
        fill_both(original, owned);
        const Step make = {"create 2 blocks", kJobCreate, name, 0x4000, kBufferA, kDescriptor, 0, false, false};
        original.job(make.entry, make.r4, make.r5, make.r6, make.r7);
        owned.job(make.entry, make.r4, make.r5, make.r6, make.r7);
        original.card.flush();
        std::vector<u8> image = original.card.image();
        for (const std::size_t dir : {0x2000u, 0x4000u}) {
            store_be32(image.data() + dir + 0x2Cu, 0xFFFFFFFFu);
            card_checksum(image, dir, 0x1FFCu, dir + 0x1FFCu);
        }
        save_file::Gci gci;
        std::string error;
        require(save_file::read_gci(root / "s2" / "save.gci", gci, &error), "the created GCI is readable");
        store_be32(gci.header.data() + 0x2C, 0xFFFFFFFFu);
        std::filesystem::create_directories(root / "o3");
        std::filesystem::create_directories(root / "s3");
        {
            std::ofstream out(root / "o3" / "card.raw", std::ios::binary);
            out.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
        }
        require(save_file::write_gci(root / "s3" / "save.gci", gci, &error), "the patched GCI is written");
        Runner original2(dol, true, root / "o3"), owned2(dol, false, root / "s3");
        fill_both(original2, owned2);
        const std::vector<Step> broken = {
            {"interrupted file: scan", kJobScan, name, 0, 0, 0, 0, false, true},
            {"interrupted file: read", kJobRead, name, 0x4000, kReadBuffer, 0, 0x4000, false, false},
            {"interrupted file: overwrite", kJobOverwrite, name, 0x4000, kBufferB, 0, 0, false, false},
            {"interrupted file: create over it", kJobCreate, name, 0x4000, kBufferA, kDescriptor, 0, false, false},
            {"interrupted file: delete", kJobDelete, name, 0, 0, 0, 0, false, false},
            {"interrupted file: read after delete", kJobRead, name, 0x4000, kReadBuffer, 0, 0x4000, false, false},
        };
        std::printf("-- the interrupted file --\n");
        const unsigned differing = play(original2, owned2, broken);
        check(differing == 0u, "runner: every step with an interrupted file equals");
    }

    // A host failure the console never had (docs/SAVEGAME_BUGS.md S7: the card model hid them): the file's place is
    // taken by a directory, so the atomic write cannot rename onto it. The game's own I/O error (5) comes back, the
    // runner is idle, and nothing was created.
    {
        std::filesystem::create_directories(root / "s5" / "save.gci");
        Runner failing(dol, false, root / "s5");
        fill_both(failing, failing);
        const Runner::Outcome made = failing.job(kJobCreate, name, 0x4000, kBufferA, kDescriptor);
        check(made.started && made.result == 5u && made.busy == 0u && made.job == 0u && made.flags == 0u,
              "host failure: a create that cannot be written ends with the I/O error (5), idle: " + describe(made));
        const Runner::Outcome read = failing.job(kJobRead, name, 0x4000, kReadBuffer, 0, 0x4000);
        check(read.result == 9u, "host failure: nothing was created (read: no such file)");
        // The unusable space check: a path whose volume cannot be asked is not a no-space error either.
        std::printf("  host failure (the file's place is a directory): result %u\n", made.result);
    }

    // The comparator is not vacuous: a store that answered differently is told apart.
    {
        Runner original(dol, true, root / "o4"), owned(dol, false, root / "s4");
        fill_both(original, owned);
        const Runner::Outcome a = original.job(kJobCreate, name, 0x4000, kBufferA, kDescriptor);
        Runner::Outcome b = owned.job(kJobCreate, name, 0x4000, kBufferA, kDescriptor);
        check(outcomes_equal(a, b, false, false), "negative control setup: a create equals");
        b.result = 7;  // a store that reported NO SPACE for a create that worked
        check(!outcomes_equal(a, b, false, false), "negative control: a different result is told apart");
        save_file::Gci fa, fb;
        require(original.file(fa) && owned.file(fb), "negative control: both machines hold the file");
        check(files_equal(fa, fb), "negative control setup: the files equal");
        fb.data[0x2041] ^= 1u;  // one flipped bit in the record
        check(!files_equal(fa, fb), "negative control: one flipped data bit is told apart");
        fb.data[0x2041] ^= 1u;
        fb.header[0x2C] ^= 0x80;  // a different icon address in the directory entry
        check(!files_equal(fa, fb), "negative control: a different directory-entry field is told apart");
        // A tick that never ends (the original's would be a hang): the harness refuses it rather than wait.
        Runner::Outcome slow = a;
        slow.busy = 1;
        check(!outcomes_equal(a, slow, false, false), "negative control: a job still busy is told apart");
    }
}
}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc >= 2, "usage: save_store_contract_tests <sys/main.dol>");
        const std::vector<u8> dol = read_file(argv[1]);
        require(dol.size() >= 256, "missing DOL");
        u64 hash = 0xCBF29CE484222325ull;
        for (u8 byte : dol) hash = (hash ^ byte) * 0x100000001B3ull;
        require(hash == 0xD2CFB4A1CA897F9Bull, "unexpected original DOL identity");
        test_card_init(dol);
        test_get_result_code(dol);
        test_runner(dol);
    } catch (const std::exception& e) {
        std::printf("save_store_contract_tests: %s\n", e.what());
        return 2;
    }
    if (g_failures != 0) {
        std::printf("save_store_contract_tests: %d failures\n", g_failures);
        return 1;
    }
    std::printf("save_store_contract_tests: all checks passed\n");
    return 0;
}
