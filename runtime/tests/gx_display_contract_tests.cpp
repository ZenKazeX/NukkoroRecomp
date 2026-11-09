// liveness-roots: reference-interpreter-only
// The GX display-copy and FIFO-setup leaves (docs/agents/axfx-effects.md, Step H) as ORIGINAL words in the reference
// interpreter against the native owners (gekko/native_gx_display.h). Same method as gx_state_contract_tests.
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/gx_sites.h"
#include "gekko/native_gx_display.h"
#include "gekko/native_gx_sync.h"
#include "gekko/native_interrupts.h"
#include "support/os_entry.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace gekko;

constexpr u32 kSda = 0x805EC500u;
constexpr u32 kSda2 = 0x805FA780u;
constexpr u32 kStackTop = 0x817E0000u;
constexpr u32 kStackFloor = 0x817D0000u;   // the frames of the call are below kStackTop, above this
constexpr u32 kFinish = 0x817FF000u;       // sentinel return address (holds `b .`)
constexpr u32 kScratchCode = 0x817FE000u;  // a test-only function (the negative controls)
constexpr u32 kOsContext = 0x817F0000u;
constexpr u32 kGxDataSize = 0x600u;
constexpr u32 kSdaGxFirst = 0x5DF8u;
constexpr u32 kSdaGxEnd = 0x5E58u;
constexpr u32 kGxBegin = 0x80396934u;
constexpr u32 kGxSetDrawSync = 0x80396428u;
constexpr u32 kArgs = 0x81310000u;         // argument buffers (matrices, colours, light objects, projection)

int g_failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) {
        ++g_failures;
        if (g_failures <= 80) std::printf("FAIL: %s\n", what.c_str());
    }
}
void require(bool ok, const std::string& what) {
    if (!ok) throw std::runtime_error(what);
}
std::string hex(u32 v) {
    char b[16];
    std::snprintf(b, sizeof(b), "%08X", v);
    return b;
}

std::vector<u8> read_file(const char* path) {
    std::ifstream stream(path, std::ios::binary);
    require(static_cast<bool>(stream), std::string("cannot read ") + path);
    return std::vector<u8>(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

std::vector<GxSiteRecord> read_sites(const char* path) {
    static const std::pair<const char*, GxSiteFamily> kFamilies[] = {
        {"dead", GxSiteFamily::Dead},
        {"draw_sync_token", GxSiteFamily::DrawSyncToken},
        {"draw_sync_token_irq", GxSiteFamily::DrawSyncTokenIrq},
        {"draw_done", GxSiteFamily::DrawDone},
        {"copy_trigger", GxSiteFamily::CopyTrigger},
        {"primitive_opcode", GxSiteFamily::PrimitiveOpcode},
        {"primitive_count", GxSiteFamily::PrimitiveCount},
        {"vertex", GxSiteFamily::Vertex},
    };
    std::ifstream stream(path);
    require(static_cast<bool>(stream), std::string("cannot read ") + path);
    std::vector<GxSiteRecord> sites;
    std::string line;
    while (std::getline(stream, line)) {
        std::istringstream fields(line);
        std::string site, word, family;
        if (!(fields >> site) || site[0] == '#') continue;
        require(static_cast<bool>(fields >> word >> family), "malformed site line: " + line);
        GxSiteRecord record{static_cast<u32>(std::stoul(site, nullptr, 16)),
                            static_cast<u32>(std::stoul(word, nullptr, 16)), GxSiteFamily::Dead};
        bool known = false;
        for (const auto& entry : kFamilies)
            if (family == entry.first) {
                record.family = entry.second;
                known = true;
            }
        require(known, "unknown family in " + line);
        sites.push_back(record);
    }
    std::sort(sites.begin(), sites.end(),
              [](const GxSiteRecord& a, const GxSiteRecord& b) { return a.address < b.address; });
    return sites;
}

struct CountingSink final : GxSiteSink {
    unsigned calls = 0;
    std::vector<std::string> log;
    void note(const char* what, u32 a, u32 b) {
        ++calls;
        char text[64];
        std::snprintf(text, sizeof(text), "%s %X %X", what, a, b);
        log.emplace_back(text);
    }
    void draw_sync_token(u32, u16 token, bool irq) override { note("token", token, irq); }
    void draw_done(u32, u32 value) override { note("draw_done", value, 0); }
    void copy_trigger(u32, u32 value) override { note("copy", value, 0); }
    void primitive_opcode(u32, u8 opcode) override { note("opcode", opcode, 0); }
    void primitive_count(u32, u16 count) override { note("count", count, 0); }
    void vertex(u32, u32 value, u32 bytes) override { note("vertex", value, bytes); }
};

struct Call {
    u32 r3 = 0, r4 = 0, r5 = 0, r6 = 0, r7 = 0, r8 = 0, r9 = 0;
    double f[8] = {};
    std::vector<std::pair<u32, u32>> gx_pre;  // (offset in __GXData, word) set before the call: dirty-state paths
};

struct Outcome {
    bool returned = false;
    unsigned live_sink_calls = 0;
    u64 unlisted = 0;
    std::set<u32> written;   // word addresses written (any size)
    u32 r1 = 0;
    std::string stop;
};

struct Machine {
    GuestMemory bus;
    Cpu cpu{bus};
    CountingSink sink;
    u32 gx = 0;
    bool native;
    std::set<u32>* trace = nullptr;

    Machine(const std::vector<u8>& dol, const std::vector<GxSiteRecord>& sites, bool native_owners, bool lifted = false)
        : native(native_owners) {
        const auto word = [&](std::size_t offset) { return load_be32(dol.data() + offset); };
        for (u32 i = 0; i < 18u; ++i) {
            const u32 offset = word(i * 4u), address = word(0x48u + i * 4u), size = word(0x90u + i * 4u);
            if (!size) continue;
            require(u64(offset) + size <= dol.size(), "invalid DOL extent");
            bus.write_block(address, dol.data() + offset, size);
        }
        bus.write32(kFinish, 0x48000000u);
        test_support::install_os_system_call(bus);
        bus.write32(test_support::kOsCurrentContext, kOsContext);
        gx = bus.peek32(kSda2 + 0xFD8u);
        require(gx >= GuestMemory::kImageBase && gx < GuestMemory::kLegacyHeapEnd, "the DOL's __GXData pointer");
        cpu.reset();
        cpu.set_gx_sites(sites.data(), sites.size(), &sink);
        if (native) gx_display::install(cpu);
        if (native && lifted) gx_display::install_lifted(cpu);
        bus.set_access_trace([this](u32 address, std::size_t size, bool writing) {
            if (!writing || trace == nullptr) return;
            for (std::size_t i = 0; i < size; ++i) trace->insert((address + static_cast<u32>(i)) & ~3u);
        });
    }
    void setup() {
        cpu.clear_stop();
        cpu.gpr[1] = kStackTop;
        cpu.gpr[2] = kSda2;
        cpu.gpr[13] = kSda;
        for (u32 a = kSda + kSdaGxFirst; a < kSda + kSdaGxEnd; a += 4u) cpu.mem_write32(a, 0u);
        for (u32 i = 0; i < kGxDataSize; i += 4u) cpu.mem_write32(gx + i, 0u);
        // Argument buffers: matrices / colours / projection / light objects hold recognisable values.
        for (u32 i = 0; i < 0x100u; i += 4u) cpu.mem_write32(kArgs + i, 0x3F800000u + (i << 8));
        sink.calls = 0;
    }
    Outcome run(u32 entry, const Call& c) {
        Outcome o;
        for (const auto& pre : c.gx_pre) cpu.mem_write32(gx + pre.first, pre.second);
        std::set<u32> written;
        trace = &written;
        cpu.gpr[3] = c.r3;
        cpu.gpr[4] = c.r4;
        cpu.gpr[5] = c.r5;
        cpu.gpr[6] = c.r6;
        cpu.gpr[7] = c.r7;
        cpu.gpr[8] = c.r8;
        cpu.gpr[9] = c.r9;
        for (unsigned i = 0; i < 8; ++i) {
            u64 bits;
            std::memcpy(&bits, &c.f[i], sizeof(bits));
            cpu.fpr[1 + i].bits[0] = bits;
            cpu.fpr[1 + i].bits[1] = bits;
        }
        cpu.pc = entry;
        cpu.npc = entry + 4;
        cpu.lr = kFinish;
        unsigned steps = 0;
        while (cpu.pc != kFinish && !cpu.stopped() && steps++ < 2000000u) cpu.run_steps(1u);
        trace = nullptr;
        o.returned = !cpu.stopped() && cpu.pc == kFinish;
        o.r1 = cpu.gpr[1];
        o.live_sink_calls = sink.calls;
        o.unlisted = cpu.unlisted_gx_stores();
        o.written = std::move(written);
        if (cpu.stopped()) o.stop = cpu.stop().detail;
        return o;
    }
};

struct Function {
    u32 entry;
    const char* name;
    std::vector<Call> calls;
};

Call args(u32 r3 = 0, u32 r4 = 0, u32 r5 = 0, u32 r6 = 0, u32 r7 = 0, u32 r8 = 0, u32 r9 = 0) {
    Call c;
    c.r3 = r3;
    c.r4 = r4;
    c.r5 = r5;
    c.r6 = r6;
    c.r7 = r7;
    c.r8 = r8;
    c.r9 = r9;
    return c;
}
Call with_gx(Call c, std::vector<std::pair<u32, u32>> pre) {
    c.gx_pre = std::move(pre);
    return c;
}
Call floats(double a, double b, double c, double d, double e, double f, u32 r3 = 0, u32 r4 = 0) {
    Call call = args(r3, r4);
    call.f[0] = a;
    call.f[1] = b;
    call.f[2] = c;
    call.f[3] = d;
    call.f[4] = e;
    call.f[5] = f;
    return call;
}

std::vector<Function> functions() {
    return {
        {0x80396BD4, "GXSetDispCopySrc", {args(0, 0, 640, 480), args(16, 8, 320, 240), args(0, 0, 1, 1), args(0x3FF, 0x3FF, 0x400, 0x400)}},
        {0x80396CCC, "GXSetDispCopyDst", {args(1280), args(640), args(32), args(0x7FE0)}},
        {0x80396E54, "display copy clamp", {args(0), args(1), args(2), args(3)}},
        {0x80397228, "GXSetCopyFilter", {args(0, 0, 0, 0), args(1, kArgs, 1, kArgs + 0x20), args(1, kArgs + 0x40, 0, kArgs), args(2, kArgs, 1, kArgs + 0x10)}},
        {0x80397430, "display copy gamma", {args(0), args(1), args(2), args(3)}},
        {0x80399E58, "pixel format setter", {args(0, 0), args(1, 0), args(2, 1), args(3, 2), args(4, 3), args(5, 7), args(6, 1)}},
        {0x80399FCC, "BP mask setter", {args(0, 0), args(0xFFFFFF, 1), args(0x123456, 1), args(0x1, 0)}},
        {0x803971B0, "BP 0x4F-0x51 setter", {args(kArgs, 0), args(kArgs + 0x10, 0xFFFFFF), args(kArgs + 0x20, 0x123456), args(kArgs + 0x30, 1)}},
        {0x803992C8, "BP state register setter", {args(), with_gx(args(), {{0x124, 7}}), with_gx(args(), {{0x124, 0xF}}), with_gx(args(), {{0x124, 0x123456}})}},
        // the dirty-state flush with each combination of the dirty bits
        {0x80396098, "GXFlush", {args(), with_gx(args(), {{0x5AC, 0x1}}), with_gx(args(), {{0x5AC, 0x2}}), with_gx(args(), {{0x5AC, 0x4}}),
                                 with_gx(args(), {{0x5AC, 0x8}}), with_gx(args(), {{0x5AC, 0x10}}), with_gx(args(), {{0x5AC, 0x18}}),
                                 with_gx(args(), {{0x5AC, 0x3F}})}},
    };
}

// The vertex description the native immediate builder reads (native_immediate.h): VCD gx+0x14 / +0x18, VAT A / B / C
// gx+0x1C / +0x3C / +0x5C + 4 * format (8 formats each).
bool vertex_format_word(u32 offset) {
    return (offset >= 0x14u && offset < 0x1Cu) || (offset >= 0x1Cu && offset < 0x1Cu + 32u) ||
           (offset >= 0x3Cu && offset < 0x3Cu + 32u) || (offset >= 0x5Cu && offset < 0x5Cu + 32u);
}

struct Judgement {
    bool inside = true;      // every write is in __GXData or the stack
    bool vertex = false;     // a write touches a vertex-format word
};
bool outside_report = false;
Judgement judge(const Outcome& o, u32 gx, std::set<u32>* gx_offsets) {
    Judgement j;
    for (const u32 word : o.written) {
        if (word >= gx && word < gx + kGxDataSize) {
            const u32 offset = word - gx;
            if (gx_offsets) gx_offsets->insert(offset);
            if (vertex_format_word(offset)) j.vertex = true;
        } else if (word >= kStackFloor && word < kStackTop + 8u) {
            // a stack frame of the call (and the caller's LR slot at 4(r1))
        } else {
            j.inside = false;
            if (outside_report) std::printf("    write outside: %s\n", hex(word).c_str());
        }
    }
    return j;
}

void test_functions(const std::vector<u8>& dol, const std::vector<GxSiteRecord>& sites) {
    std::map<std::string, std::set<u32>> map;
    unsigned calls = 0;
    for (const Function& f : functions()) {
        for (const Call& c : f.calls) {
            Machine original(dol, sites, false), owned(dol, sites, true);
            original.setup();
            owned.setup();
            const Outcome a = original.run(f.entry, c);
            const Outcome b = owned.run(f.entry, c);
            ++calls;
            const std::string label = std::string(f.name) + " r3=" + hex(c.r3) + " r4=" + hex(c.r4);
            check(a.returned, label + ": the original returns: " + a.stop);
            check(a.r1 == kStackTop, label + ": the original restores r1");
            check(a.live_sink_calls == 0u && a.unlisted == 0u, label + ": no live pipe site and no unlisted store");
            outside_report = true;
            const Judgement j = judge(a, original.gx, &map[f.name]);
            outside_report = false;
            check(j.inside, label + ": every write is inside __GXData or the stack");
            check(!j.vertex, label + ": no write to a vertex-format word");
            check(b.returned && b.r1 == kStackTop, label + ": the owner returns to the caller with r1 intact");
            check(b.written.empty(), label + ": the owner writes nothing");
            check(b.live_sink_calls == 0u && b.unlisted == 0u, label + ": the owner reaches no pipe site");
        }
    }
    std::printf("  %u calls of %zu functions: the originals write only __GXData shadow words and stack frames\n", calls, map.size());
    for (const auto& entry : map) {
        std::string offsets;
        for (const u32 offset : entry.second) {
            char text[16];
            std::snprintf(text, sizeof(text), " +%X", offset);
            offsets += text;
        }
        std::printf("    %-24s%s\n", entry.first.c_str(), offsets.c_str());
    }
}

// GXInitFifoBase: the one row whose product a caller can read. The original (with GXInitFifoLimits / GXInitFifoPtrs under
// it and the interrupt disable / restore) against the owner on identical machines: the FIFO object's bytes, the
// callee-saved registers, r1, the MSR (interrupts enabled and disabled on entry) and every guest write.
struct FifoCase {
    u32 fifo, base, size;
    bool interrupts;
};

bool fifo_equal(const std::vector<u8>& dol, const std::vector<GxSiteRecord>& sites, const FifoCase& fc, void (*owner)(Cpu&),
                std::string* why) {
    Machine original(dol, sites, false), owned(dol, sites, false);
    for (Machine* m : {&original, &owned}) {
        m->setup();
        for (u32 i = 0; i < 0x40u; i += 4u) m->cpu.mem_write32(fc.fifo + i, 0xA5A50000u + i);
        m->cpu.set_msr(fc.interrupts ? (m->cpu.msr() | kMsrEE) : (m->cpu.msr() & ~kMsrEE));
        for (unsigned r = 14; r < 32; ++r) m->cpu.gpr[r] = 0x11110000u + r;
        m->cpu.gpr[3] = fc.fifo;
        m->cpu.gpr[4] = fc.base;
        m->cpu.gpr[5] = fc.size;
    }
    std::set<u32> written;
    original.trace = &written;
    original.cpu.pc = gekko::gx_display::kGxInitFifoBase;
    original.cpu.npc = original.cpu.pc + 4;
    original.cpu.lr = kFinish;
    unsigned steps = 0;
    while (original.cpu.pc != kFinish && !original.cpu.stopped() && steps++ < 100000u) original.cpu.run_steps(1u);
    original.trace = nullptr;
    if (original.cpu.stopped() || original.cpu.pc != kFinish) {
        *why = "the original did not return";
        return false;
    }
    std::set<u32> owner_written;
    owned.trace = &owner_written;
    owner(owned.cpu);
    owned.trace = nullptr;
    if (owned.cpu.stopped()) {
        *why = "the owner stopped";
        return false;
    }
    for (u32 i = 0; i < 0x40u; i += 4u)
        if (original.cpu.mem_read32(fc.fifo + i) != owned.cpu.mem_read32(fc.fifo + i)) {
            *why = "FIFO word +" + hex(i);
            return false;
        }
    for (unsigned r = 14; r < 32; ++r)
        if (original.cpu.gpr[r] != owned.cpu.gpr[r]) {
            *why = "callee-saved register";
            return false;
        }
    if (original.cpu.gpr[1] != owned.cpu.gpr[1]) {
        *why = "r1";
        return false;
    }
    if (original.cpu.msr() != owned.cpu.msr()) {
        *why = "MSR";
        return false;
    }
    for (const u32 w : owner_written)
        if (w < (fc.fifo & ~3u) || w >= fc.fifo + 0x20u) {
            *why = "the owner wrote outside the FIFO object";
            return false;
        }
    for (const u32 w : written) {
        const bool in_fifo = w >= (fc.fifo & ~3u) && w < fc.fifo + 0x20u;
        const bool in_stack = w >= kStackFloor && w < kStackTop + 8u;
        if (!in_fifo && !in_stack) {
            *why = "the original wrote outside the FIFO object and the stack";
            return false;
        }
    }
    return true;
}

void bad_fifo(Cpu& cpu) {  // negative control: the pointer-count word is left alone
    gx_display::serve_init_fifo_base(cpu);
    cpu.mem_write32(cpu.gpr[3] + 0x1Cu, 0xA5A5001Cu);
}
void bad_fifo_end(Cpu& cpu) {  // negative control: the end pointer without the -4
    gx_display::serve_init_fifo_base(cpu);
    cpu.mem_write32(cpu.gpr[3] + 0x04u, cpu.gpr[4] + cpu.gpr[5]);
}

void test_fifo(const std::vector<u8>& dol, const std::vector<GxSiteRecord>& sites) {
    unsigned cases = 0;
    const FifoCase table[] = {{0x81310000u, 0x81400000u, 0x20000u, true}, {0x81310100u, 0x81500000u, 0x40000u, false},
                              {0x81310000u, 0x80300000u, 0x10000u, true}, {0x81310040u, 0x81600000u, 0x1000u, false},
                              {0x81310000u, 0x81700000u, 0x4000u, true},  {0x81310080u, 0x81800000u, 0x7FFFFFE0u, true},
                              {0x81310000u, 0x81900020u, 0x20u, false}};
    for (const FifoCase& fc : table) {
        std::string why;
        check(fifo_equal(dol, sites, fc, gx_display::serve_init_fifo_base, &why),
              "GXInitFifoBase fifo=" + hex(fc.fifo) + " base=" + hex(fc.base) + " size=" + hex(fc.size) + ": " + why);
        ++cases;
        std::string unused;
        check(!fifo_equal(dol, sites, fc, bad_fifo, &unused), "negative control (count word kept) was not detected");
        check(!fifo_equal(dol, sites, fc, bad_fifo_end, &unused), "negative control (end pointer) was not detected");
    }
    std::printf("  GXInitFifoBase: %u cases against the original (FIFO object, callee-saved registers, MSR, writes), 2 negative controls each\n", cases);
}

// GXCopyDisp: the original emits one copy trigger; the lifted owner refuses.
void test_copy_disp(const std::vector<u8>& dol, const std::vector<GxSiteRecord>& sites) {
    for (const Call& c : {args(0x81400000u, 0u), args(0x81400000u, 1u), with_gx(args(0x81400000u, 1u), {{0x1EC, 0x7F}})}) {
        Machine original(dol, sites, false), owned(dol, sites, true, true);
        original.setup();
        owned.setup();
        const Outcome a = original.run(gekko::gx_display::kGxCopyDisp, c);
        check(a.returned && a.unlisted == 0u && a.live_sink_calls == 1u && original.sink.log.size() == 1u &&
                  original.sink.log[0].rfind("copy", 0) == 0,
              "GXCopyDisp: the original emits exactly one copy trigger: " + a.stop);
        const Outcome b = owned.run(gekko::gx_display::kGxCopyDisp, c);
        check(!b.returned && owned.cpu.stopped() && b.stop.find("GXCopyDisp") != std::string::npos && b.live_sink_calls == 0u &&
                  b.written.empty(),
              "GXCopyDisp: the lifted owner stops naming GXCopyDisp and writes nothing: " + b.stop);
    }
    // Without the lifted owners (the legacy tree) the original body runs.
    Machine legacy(dol, sites, true, false);
    legacy.setup();
    const Outcome c = legacy.run(gekko::gx_display::kGxCopyDisp, args(0x81400000u, 0u));
    check(c.returned && c.live_sink_calls == 1u, "GXCopyDisp: without the lifted owners the original body runs (legacy tree)");
}

void test_negative_controls(const std::vector<u8>& dol, const std::vector<GxSiteRecord>& sites) {
    Machine m(dol, sites, false);
    m.setup();
    Outcome outside;
    outside.written.insert(0x80400000u);
    check(!judge(outside, m.gx, nullptr).inside, "negative control (write outside __GXData) was not detected");
    Outcome vertex;
    vertex.written.insert(m.gx + 0x1Cu);
    check(judge(vertex, m.gx, nullptr).vertex, "negative control (vertex-format word) was not detected");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: gx_display_contract_tests <sys/main.dol> <extract/gx-sites.txt>\n");
        return 2;
    }
    try {
        const std::vector<u8> dol = read_file(argv[1]);
        const std::vector<GxSiteRecord> sites = read_sites(argv[2]);
        test_functions(dol, sites);
        test_fifo(dol, sites);
        test_copy_disp(dol, sites);
        test_negative_controls(dol, sites);
        {
            GuestMemory memory;
            Cpu cpu(memory);
            gx_display::install(cpu);
            gx_display::install_lifted(cpu);
            std::size_t count = 0;
            const gx_display::Address* owners = gx_display::addresses(count);
            for (std::size_t i = 0; i < count; ++i) check(cpu.has_hle(owners[i].address), "an owner is not installed");
            owners = gx_display::lifted_addresses(count);
            for (std::size_t i = 0; i < count; ++i) check(cpu.has_hle(owners[i].address), "a lifted owner is not installed");
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL gx display contract: %s\n", error.what());
        return 1;
    }
    if (g_failures != 0) {
        std::printf("FAIL gx display contract: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("PASS gx display contract\n");
    return 0;
}
