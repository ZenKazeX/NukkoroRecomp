// liveness-roots: reference-interpreter-only
// The GX state API (docs/agents/card-saves.md, graphics.gx cluster gxA): the 26 state setters / matrix loaders of the
// GX library, as ORIGINAL words in the reference interpreter, against the native owners (gekko/native_gx_state.h).
//
// What the originals do: write the library's shadow of the state it was told (fields of __GXData, the structure
// *(r2 + 0xFD8) points to) and send the matching BP / XF commands through the write-gather pipe. The pipe stores are
// dead transport (extract/gx-sites.txt lists every one as `dead`: Flipper has no consumer here), so the only thing a
// call leaves behind is the shadow. This test establishes, for every function and several argument sets each:
//
//   1. the original returns, restores r1, reaches no live pipe site (no token / draw-done / copy / opcode / vertex call)
//      and no unlisted pipe store;
//   2. every guest write the original makes lies inside __GXData (below 0x600 bytes of it) or the stack frame - nothing
//      else: no SDA word, no OS word, no argument buffer;
//   3. none of those writes touches a word the native immediate builder reads (the vertex descriptor and attribute
//      formats, gx + 0x14..0x20 and the VAT rows) - printed as the retired map;
//   4. the native owner serves the same call, returns to the caller, leaves r1 and the saved state alone and writes
//      NOTHING - the shadow is retired: the surviving readers are measured on the replay (the guest watch, report).
//
// Negative controls: a body that writes outside the allowed regions, and a body that writes a vertex-format word, are
// both told apart.
//
// usage: gx_state_contract_tests <sys/main.dol> <extract/gx-sites.txt>
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/gx_sites.h"
#include "gekko/native_gx_state.h"
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
        if (native) gx_state::install(cpu);
        if (native && lifted) gx_state::install_lifted(cpu);
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
        {0x80395D48, "GXSetTexCoordGen2", {args(0, 1, 4, 0x1E, 0, 0x7D), args(7, 0, 1, 0x24, 1, 0x40), args(3, 1, 5, 0x12, 0, 0x7D)}},
        {0x80395FC8, "GXSetNumTexGens", {args(0), args(1), args(8), args(3)}},
        {0x80396B54, "GXSetCullMode", {args(0), args(1), args(2), args(3)}},
        {0x80397A34, "GXLoadLightObjImm", {args(kArgs, 1), args(kArgs + 0x40, 0x80), args(kArgs, 0x04)}},
        {0x80397AB0, "GXSetChanAmbColor", {args(4, kArgs), args(5, kArgs + 4), args(0, kArgs + 8)}},
        {0x80397B98, "GXSetChanMatColor", {args(4, kArgs), args(5, kArgs + 4), args(1, kArgs + 8)}},
        {0x80397C80, "GXSetNumChans", {args(0), args(1), args(2)}},
        {0x80397CBC, "GXSetChanCtrl", {args(4, 0, 0, 1, 0, 0, 1), args(5, 1, 1, 1, 0xFF, 2, 1), args(0, 1, 0, 0, 0x0F, 1, 0)}},
        {0x80399778, "GXSetAlphaCompare", {args(7, 0, 0, 7, 0), args(4, 128, 1, 4, 64), args(1, 255, 2, 6, 0)}},
        {0x80399A0C, "GXSetFog", {floats(0, 100.0, 5000.0, 1.0, 10000.0, 0, 0x26, kArgs), floats(0, 0, 0, 0, 0, 0, 0, kArgs), floats(0, 1.0, 2.0, 0.1, 3.0, 0, 4, kArgs + 8)}},
        {0x80399D44, "GXSetBlendMode", {args(0, 4, 5, 0), args(1, 4, 5, 0), args(3, 0, 1, 15), args(2, 1, 0, 5)}},
        {0x80399D98, "GXSetColorUpdate", {args(0), args(1)}},
        {0x80399DC4, "GXSetAlphaUpdate", {args(0), args(1)}},
        {0x80399DF0, "GXSetZMode", {args(1, 3, 1), args(0, 7, 0), args(1, 0, 1)}},
        {0x80399E24, "GXSetZCompLoc", {args(0), args(1)}},
        {0x80399F58, "GXSetDstAlpha", {args(0, 0), args(1, 0xFF), args(1, 0x40)}},
        {0x8039A0B4, "GXSetProjection", {args(kArgs, 0), args(kArgs, 1)}},
        {0x8039A158, "GXSetProjectionv", {args(kArgs), args(kArgs + 0x20)}},
        {0x8039A22C, "GXLoadPosMtxImm", {args(kArgs, 0), args(kArgs, 9)}},
        {0x8039A27C, "GXLoadNrmMtxImm", {args(kArgs, 0), args(kArgs, 3)}},
        {0x8039A2CC, "GXSetCurrentMtx", {args(0), args(9), args(27)}},
        {0x8039A300, "GXLoadTexMtxImm", {args(kArgs, 30, 0), args(kArgs, 60, 1), args(kArgs, 0x40, 0)}},
        {0x8039A444, "GXSetViewportJitter", {floats(0, 0, 640, 480, 0, 1, 1), floats(10, 20, 320, 240, 0.25, 0.75, 0)}},
        {0x8039A49C, "GXSetViewport", {floats(0, 0, 640, 480, 0, 1), floats(10, 20, 320, 240, 0.25, 0.75)}},
        {0x8039A4E4, "GXSetScissor", {args(0, 0, 640, 480), args(10, 20, 320, 240), args(0, 0, 1, 1)}},
        {0x8039A55C, "GXSetScissorBoxOffset", {args(0, 0), args(0x15, 0x2A), args(0xFFFFFFF0u, 5)}},
        {0x80396004, "GXSetMisc", {args(0, 0), args(1, 0), args(2, 1), args(3, 2), args(4, 1), args(5, 0)}},
        {0x80398828, "TEV / indirect setter 124 B", {args(0, 1, 2, 3), args(1, 0, 3, 2), args(3, 3, 0, 1)}},
        {0x80398E14, "GXSetTevIndirect", {args(0, 0, 0, 0, 0, 0, 0), args(1, 1, 1, 1, 1, 1, 1), args(3, 2, 3, 3, 2, 7, 6)}},
        {0x80398E80, "indirect texture matrix setter 376 B", {args(0, kArgs, 0, 0), args(1, kArgs, 1, 2), args(2, kArgs + 0x20, 3, 1)}},
        {0x80398FF8, "GXSetIndTexCoordScale", {args(0, 0, 0), args(1, 3, 4), args(3, 5, 2), args(2, 1, 1)}},
        {0x8039913C, "GXSetIndTexOrder", {args(0, 0, 0), args(1, 3, 2), args(2, 7, 4), args(3, 1, 1)}},
        {0x80399228, "GXSetNumIndStages", {args(0), args(1), args(4)}},
        {0x8039924C, "GXSetTevDirect", {args(0), args(3), args(7)}},
        {0x803992EC, "GXSetTevOp", {args(0, 0), args(1, 1), args(2, 2), args(3, 3), args(4, 4)}},
        {0x80399378, "GXSetTevColorIn (shape)", {args(0, 15, 15, 15, 15), args(1, 8, 10, 12, 15), args(2, 0, 1, 2, 3)}},
        {0x803993BC, "GXSetTevAlphaIn (shape)", {args(0, 7, 7, 7, 7), args(1, 4, 5, 6, 7), args(2, 0, 1, 2, 3)}},
        {0x80399400, "GXSetTevColorOp (shape)", {args(0, 0, 0, 0, 1, 0), args(1, 1, 1, 1, 0, 3), args(2, 8, 2, 2, 1, 1)}},
        {0x80399468, "GXSetTevAlphaOp (shape)", {args(0, 0, 0, 0, 1, 0), args(1, 1, 1, 1, 0, 3), args(2, 8, 2, 2, 1, 1)}},
        {0x80399530, "TEV setter 100 B", {args(0, 0), args(1, 3), args(2, 1)}},
        {0x80399594, "GXSetTevKColor", {args(0, kArgs), args(1, kArgs + 4), args(3, kArgs + 8)}},
        {0x803995F8, "GXSetTevKColorSel (shape)", {args(0, 0), args(1, 15), args(5, 31)}},
        {0x80399654, "GXSetTevKAlphaSel (shape)", {args(0, 0), args(1, 15), args(5, 31)}},
        {0x80399848, "GXSetTevOrder", {args(0, 0, 0, 4), args(1, 1, 1, 0), args(4, 7, 7, 0xFF), args(2, 0xFF, 0xFF, 0xFF)}},
        {0x803999E4, "GXSetNumTevStages", {args(1), args(2), args(16)}},
        {0x80399294, "empty body", {args(), args(1, 2, 3)}},
        {0x80395D38, "GXInvalidateVtxCache", {args()}},
        // the dirty-state flush (gx+0x5AC set, gx+0 clear) and the pipe call words; every pipe store is dead
        {0x8039A044, "GXCallDisplayList", {args(kArgs, 0x40), args(kArgs, 0x20), with_gx(args(kArgs + 0x20, 0x80), {{0x5AC, 0x3F}}),
                                           with_gx(args(kArgs, 0x20), {{0x5AC, 0x1}, {0x0, 1}})}},
    };
}

// The vertex-path rows owned only in the lifted build (gx_state::install_lifted), run with the lifted owners installed.
// 0x8039530C / 0x80395790 / 0x80395CF4 are unnamed: their shapes are a switch on an attribute, a switch on an attribute
// with a format row, and a CP array-base load (r3 attribute, r4 base, r5 stride); the argument sets cover the switch's
// cases and its out-of-range side.
std::vector<Function> lifted_functions() {
    return {
        {0x80395758, "GXClearVtxDesc", {args(), with_gx(args(), {{0x14, 0xFFFFFFFFu}, {0x18, 0x7FFF}, {0x5AC, 0x2}})}},
        {0x8039530C, "vertex descriptor setter", {args(0, 0), args(0, 1), args(9, 2), args(10, 3), args(0x18, 1), args(0x19, 1), args(0x1A, 2), args(0x30, 0)}},
        {0x80395790, "vertex attribute format setter", {args(0, 9, 1, 4, 0), args(1, 10, 1, 5, 3), args(7, 0x19, 0, 0, 7), args(7, 0x1A, 1, 1, 1), args(2, 9, 1, 0, 0)}},
        {0x80395CF4, "vertex array setter", {args(9, kArgs, 12), args(0x0A, kArgs + 0x40, 0x20), args(0x19, 0x81234560u, 0xFF), args(0x1A, kArgs, 4), args(0x0B, 0, 0)}},
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
    std::vector<Function> all = functions();
    std::set<u32> lifted_entries;
    for (const Function& f : lifted_functions()) {
        all.push_back(f);
        lifted_entries.insert(f.entry);
    }
    for (const Function& f : all) {
        for (const Call& c : f.calls) {
            Machine original(dol, sites, false), owned(dol, sites, true, lifted_entries.count(f.entry) != 0u);
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
            // The common owners write no vertex-format word (the legacy immediate builder reads them). The lifted-only
            // vertex setters are the ones that do - that is why they are owned only where that builder is not compiled.
            if (lifted_entries.count(f.entry) == 0u) check(!j.vertex, label + ": no write to a vertex-format word");
            check(b.returned && b.r1 == kStackTop, label + ": the owner returns to the caller with r1 intact");
            check(b.written.empty(), label + ": the owner writes nothing");
            check(b.live_sink_calls == 0u && b.unlisted == 0u, label + ": the owner reaches no pipe site");
        }
    }
    std::printf("  %u calls of %zu functions: the originals write only __GXData shadow words and stack frames\n", calls,
                map.size());
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

// GXBegin: the original emits its header (opcode = type | format) and the vertex count as live sites; in the lifted
// build both are dead sites that stop the machine, so the lifted owner refuses the call with GXBegin's own message.
void test_begin(const std::vector<u8>& dol, const std::vector<GxSiteRecord>& sites) {
    for (const Call& c : {args(0x80u, 0u, 4u), args(0x90u, 3u, 3u), args(0xA0u, 7u, 65535u)}) {
        Machine original(dol, sites, false), owned(dol, sites, true, true);
        original.setup();
        owned.setup();
        const Outcome a = original.run(kGxBegin, c);
        check(a.returned && a.unlisted == 0u, "GXBegin: the original returns: " + a.stop);
        check(a.live_sink_calls == 2u && original.sink.log.size() == 2u && original.sink.log[0].rfind("opcode", 0) == 0 &&
                  original.sink.log[1].rfind("count", 0) == 0,
              "GXBegin: the original emits the header opcode and the vertex count, nothing else");
        const Judgement j = judge(a, original.gx, nullptr);
        check(j.inside && !j.vertex, "GXBegin: the original writes only __GXData shadow words");
        const Outcome b = owned.run(kGxBegin, c);
        check(!b.returned && owned.cpu.stopped() && b.stop.find("GXBegin") != std::string::npos && b.live_sink_calls == 0u &&
                  b.written.empty(),
              "GXBegin: the lifted owner stops the machine naming GXBegin and writes nothing: " + b.stop);
    }
    // Negative control: without the lifted owners (the legacy tree) the original body runs.
    Machine legacy(dol, sites, true, false);
    legacy.setup();
    const Outcome c = legacy.run(kGxBegin, args(0x80u, 0u, 4u));
    check(c.returned && c.live_sink_calls == 2u, "GXBegin: without the lifted owners the original body runs (legacy tree)");
}

// GXSetDrawSync: the original's token completions (BP 0x48 with the interrupt request, then BP 0x47) against NativeGxSync's
// owner, with what the guest sees afterwards: the token GXReadDrawSync returns, the counts, the callee-saved registers
// and the MSR.
struct SyncMachine {
    GuestMemory bus;
    Cpu cpu{bus};
    NativeInterrupts interrupts{cpu};
    NativeGxSync sync{cpu, interrupts};
    struct Router final : GxSiteSink {
        NativeGxSync* sync = nullptr;
        std::vector<std::string> log;
        void draw_sync_token(u32, u16 token, bool irq) override {
            char text[48];
            std::snprintf(text, sizeof(text), "token %X %d", token, irq ? 1 : 0);
            log.emplace_back(text);
            sync->on_draw_sync_token(token, irq);
        }
        void draw_done(u32, u32) override { log.emplace_back("draw_done"); }
        void copy_trigger(u32, u32) override { log.emplace_back("copy"); }
        void primitive_opcode(u32, u8) override { log.emplace_back("opcode"); }
        void primitive_count(u32, u16) override { log.emplace_back("count"); }
        void vertex(u32, u32, u32) override { log.emplace_back("vertex"); }
    } router;
    u32 gx = 0;
    SyncMachine(const std::vector<u8>& dol, const std::vector<GxSiteRecord>& sites, bool native) {
        router.sync = &sync;
        const auto word = [&](std::size_t offset) { return load_be32(dol.data() + offset); };
        for (u32 i = 0; i < 18u; ++i) {
            const u32 offset = word(i * 4u), address = word(0x48u + i * 4u), size = word(0x90u + i * 4u);
            if (!size) continue;
            bus.write_block(address, dol.data() + offset, size);
        }
        bus.write32(kFinish, 0x48000000u);
        test_support::install_os_system_call(bus);
        bus.write32(test_support::kOsCurrentContext, kOsContext);
        gx = bus.peek32(kSda2 + 0xFD8u);
        cpu.reset();
        cpu.set_gx_sites(sites.data(), sites.size(), &router);
        if (native) sync.install();
        cpu.set_msr(cpu.msr() | kMsrEE | kMsrFP);
    }
    bool run(u32 token, u32 dirty) {
        cpu.clear_stop();
        cpu.gpr[1] = kStackTop;
        cpu.gpr[2] = kSda2;
        cpu.gpr[13] = kSda;
        for (u32 i = 0; i < kGxDataSize; i += 4u) cpu.mem_write32(gx + i, 0u);
        cpu.mem_write32(gx + 0x5ACu, dirty);
        cpu.gpr[3] = token;
        cpu.pc = kGxSetDrawSync;
        cpu.lr = kFinish;
        unsigned steps = 0;
        while (cpu.pc != kFinish && !cpu.stopped() && steps++ < 2000000u) cpu.run_steps(1u);
        return !cpu.stopped() && cpu.pc == kFinish && cpu.gpr[1] == kStackTop;
    }
};

void test_draw_sync(const std::vector<u8>& dol, const std::vector<GxSiteRecord>& sites) {
    unsigned cases = 0;
    for (const u32 token : {0x0000u, 0x0001u, 0x1234u, 0xFFFFu, 0x10001u}) {
        for (const u32 dirty : {0u, 0x3Fu}) {
            SyncMachine original(dol, sites, false), owned(dol, sites, true);
            const bool a = original.run(token, dirty);
            const bool b = owned.run(token, dirty);
            ++cases;
            const std::string label = "GXSetDrawSync token " + hex(token) + (dirty ? " dirty" : "");
            check(a && b, label + ": both return");
            check(original.router.log.size() == 2u && original.router.log[0].back() == '1' && original.router.log[1].back() == '0',
                  label + ": the original completes the interrupt-requesting token, then the plain one");
            check(original.sync.token() == owned.sync.token() && original.sync.token() == static_cast<u16>(token),
                  label + ": the token GXReadDrawSync returns is the same: " + hex(original.sync.token()) + " / " +
                      hex(owned.sync.token()));
            check(original.sync.stats().tokens == owned.sync.stats().tokens &&
                      original.sync.stats().token_interrupts == owned.sync.stats().token_interrupts,
                  label + ": the same token and token-interrupt counts");
            for (u32 r = 14; r < 32u; ++r)
                check(original.cpu.gpr[r] == owned.cpu.gpr[r], label + ": r" + std::to_string(r) + " (callee-saved) equal");
            check(original.cpu.gpr[1] == owned.cpu.gpr[1] && original.cpu.lr == owned.cpu.lr, label + ": r1 and LR equal");
            check(original.cpu.msr() == owned.cpu.msr(), label + ": MSR equal (the interrupt mask / restore pair nets out)");
        }
    }
    std::printf("  GXSetDrawSync: %u cases, token / counts / callee-saved registers / MSR equal\n", cases);
    // Negative control: a different token is told apart.
    SyncMachine a(dol, sites, false), b(dol, sites, true);
    a.run(0x1111u, 0u);
    b.run(0x2222u, 0u);
    check(a.sync.token() != b.sync.token(), "negative control: different tokens are told apart");
}


// GXInit(base, size) -> the library's FIFO object, the original words on a bare machine against NativeGxSync's owner.
// Both machines start from the state the OS leaves before the first GXInit: every OS interrupt masked (global mask
// 0xFFFFFFE0). The memory the call leaves is compared word by word over the union of the two write sets; the only words
// allowed to differ are the ones the original wrote and nothing reads here (RETIRED below), and each is named.
struct InitMachine {
    GuestMemory bus;
    Cpu cpu{bus};
    NativeInterrupts interrupts{cpu};
    NativeGxSync sync{cpu, interrupts};
    CountingSink sink;
    std::set<u32> written;
    u32 gx = 0;
    InitMachine(const std::vector<u8>& dol, const std::vector<GxSiteRecord>& sites, bool native) {
        const auto word = [&](std::size_t offset) { return load_be32(dol.data() + offset); };
        for (u32 i = 0; i < 18u; ++i) {
            const u32 offset = word(i * 4u), address = word(0x48u + i * 4u), size = word(0x90u + i * 4u);
            if (!size) continue;
            bus.write_block(address, dol.data() + offset, size);
        }
        bus.write32(kFinish, 0x48000000u);
        test_support::install_os_system_call(bus);
        bus.write32(test_support::kOsCurrentContext, kOsContext);
        gx = bus.peek32(kSda2 + 0xFD8u);
        cpu.reset();
        cpu.set_gx_sites(sites.data(), sites.size(), &sink);
        if (native) {
            sync.install();
            gx_state::install(cpu);
        }
        cpu.set_msr(cpu.msr() | kMsrFP);
        bus.set_access_trace([this](u32 address, std::size_t size, bool writing) {
            if (!writing) return;
            for (std::size_t i = 0; i < size; ++i) written.insert((address + static_cast<u32>(i)) & ~3u);
        });
    }
    bool run(u32 base, u32 size) {
        cpu.clear_stop();
        cpu.gpr[1] = kStackTop;
        cpu.gpr[2] = kSda2;
        cpu.gpr[13] = kSda;
        for (u32 a = 0x80000000u + 0xC0u; a < 0x80000000u + 0xD0u; a += 4u) cpu.mem_write32(a, 0u);
        cpu.mem_write32(0x800000C4u, 0xFFFFFFE0u);  // the OS global interrupt mask: everything masked
        cpu.mem_write32(0x800000C8u, 0u);
        for (u32 a = kSda + kSdaGxFirst; a < kSda + kSdaGxEnd; a += 4u) cpu.mem_write32(a, 0u);
        for (u32 i = 0; i < kGxDataSize; i += 4u) cpu.mem_write32(gx + i, 0u);
        for (u32 i = 0; i < 0x40u; i += 4u) cpu.mem_write32(0x805A2EE0u + i, 0u);
        written.clear();
        cpu.gpr[3] = base;
        cpu.gpr[4] = size;
        cpu.pc = 0x80393A88u;
        cpu.npc = cpu.pc + 4;
        cpu.lr = kFinish;
        unsigned steps = 0;
        while (cpu.pc != kFinish && !cpu.stopped() && steps++ < 4000000u) cpu.run_steps(1u);
        return !cpu.stopped() && cpu.pc == kFinish && cpu.gpr[1] == kStackTop;
    }
};

struct Retired {
    u32 begin, end;
    const char* what;
};

void test_gxinit(const std::vector<u8>& dol, const std::vector<GxSiteRecord>& sites) {
    // Words the original GXInit (with the OS's OSRegisterVersion / OSReport and the library's own defaults under it)
    // writes that no surviving code reads: the version registration and its printed text (the dead UART path:
    // the printf state words and buffer), the OS reset record GXInit registers (nothing walks the queue: OSResetSystem
    // ends the program), the hardware register pointers and the library's one-time flag.
    const u32 gx0 = 0x805A2F60u;
    const std::vector<Retired> retired = {
        {0x80568748u, 0x80568760u, "GXInit's OS reset record and queue links"},
        {0x80568F20u, 0x80568F60u, "the printf stream state (OSReport's UART text, dead)"},
        {0x805A6430u, 0x805A6700u, "the version string, its OSRegisterVersion node and the printf buffer"},
        {0x805F1FB0u, 0x805F1FB8u, "OSRegisterVersion's list head"},
        {0x805F22F8u, 0x805F2310u, "the hardware register pointers (__piReg, __cpReg, __peReg, __memReg)"},
        {0x805F231Cu, 0x805F2320u, "GXInit's one-time flag"},
        {0x805F2380u, 0x805F23F0u, "printf's SDA statics"},
        {0x817D0000u, 0x817E0010u, "stack"},
    };
    unsigned cases = 0, words_compared = 0, words_retired = 0;
    const std::pair<u32, u32> arguments[] = {{0x81400000u, 0x40000u}, {0x81000000u, 0x20000u}, {0x81234560u & ~0x1Fu, 0x100000u}};
    for (const auto& argument : arguments) {
        InitMachine original(dol, sites, false), owned(dol, sites, true);
        const bool a = original.run(argument.first, argument.second);
        const bool b = owned.run(argument.first, argument.second);
        ++cases;
        const std::string label = "GXInit(" + hex(argument.first) + ", " + hex(argument.second) + ")";
        check(a && b, label + ": both return: " + original.cpu.stop().detail + " / " + owned.cpu.stop().detail);
        check(original.cpu.gpr[3] == 0x805A2EE0u && owned.cpu.gpr[3] == 0x805A2EE0u,
              label + ": both return the library's FIFO object " + hex(original.cpu.gpr[3]) + " / " + hex(owned.cpu.gpr[3]));
        check(original.sink.calls == 0u && owned.sink.calls == 0u && original.cpu.unlisted_gx_stores() == 0u,
              label + ": no live pipe site");
        for (u32 r = 14; r < 32u; ++r)
            check(original.cpu.gpr[r] == owned.cpu.gpr[r], label + ": r" + std::to_string(r) + " (callee-saved) equal");
        check(original.cpu.gpr[1] == owned.cpu.gpr[1] && original.cpu.lr == owned.cpu.lr, label + ": r1 and LR equal");
        std::set<u32> all = original.written;
        all.insert(owned.written.begin(), owned.written.end());
        for (const u32 word : all) {
            const Retired* hit = nullptr;
            for (const Retired& r : retired)
                if (word >= r.begin && word < r.end) hit = &r;
            if (hit != nullptr) {
                ++words_retired;
                continue;
            }
            ++words_compared;
            const u32 x = original.cpu.mem_read32(word), y = owned.cpu.mem_read32(word);
            check(x == y, label + ": word " + hex(word) + " original " + hex(x) + " owner " + hex(y));
        }
        // __GXData is compared whole (it is no longer retired): the guest GX code that survives - GXLoadTexObj and
        // GXLoadTlut under the RenderWare driver's texture bind - calls the region callbacks the original installs.
        check(owned.cpu.mem_read32(gx0 + 0x4C8u) == 0x80393634u && owned.cpu.mem_read32(gx0 + 0x4CCu) == 0x80393730u,
              label + ": the texture and TLUT region callbacks are installed");
        // The surviving state spelled out: the FIFO object and the SDA words the owners read.
        const u32 base = argument.first, size = argument.second;
        const u32 fifo[8] = {base, base + size - 4u, size, size - 0x4000u, size >> 1, base, base, 0u};
        for (u32 i = 0; i < 8u; ++i)
            check(owned.cpu.mem_read32(0x805A2EE0u + 4u * i) == fifo[i], label + ": FIFO object word " + std::to_string(i));
        check(owned.cpu.mem_read32(kSda + 0x5E20u) == 0x805A2EE0u && owned.cpu.mem_read32(kSda + 0x5E24u) == 0x805A2EE0u,
              label + ": the CPU and GP FIFO pointers name the object");
        check(owned.sync.stats().fifo_binds == 2u && owned.sync.stats().inits == 1u, label + ": one bind each, one init");
    }
    // The strict oracle's failure, reproduced: GXLoadTlut (0x803984C8) calls the TLUT region callback through
    // __GXData+0x4CC; the owner's GXInit left it null and the call jumped to address 0 ("unimplemented instruction" at
    // PC 0x4, LR 0x803984F4). After the owner's GXInit the original GXLoadTlut and GXLoadTexObj bodies must run to their
    // return, and the callback word must name the library's default.
    for (const u32 entry : {0x803984C8u, 0x8039843Cu}) {
        InitMachine original(dol, sites, false), owned(dol, sites, true);
        original.run(0x81400000u, 0x40000u);
        owned.run(0x81400000u, 0x40000u);
        const char* name = entry == 0x803984C8u ? "GXLoadTlut" : "GXLoadTexObj";
        u32 outcome[2] = {0u, 0u};
        InitMachine* machines[2] = {&original, &owned};
        for (int i = 0; i < 2; ++i) {
            Cpu& c = machines[i]->cpu;
            for (u32 k = 0; k < 0x40u; k += 4u) c.mem_write32(0x81100000u + k, 0u);
            c.mem_write32(0x81100000u + 4u, 0x00100000u);   // the object's words: a small TLUT / texture
            c.clear_stop();
            c.gpr[1] = kStackTop;
            c.gpr[3] = 0x81100000u;
            c.gpr[4] = 0u;
            c.pc = entry;
            c.npc = entry + 4;
            c.lr = kFinish;
            unsigned steps = 0;
            while (c.pc != kFinish && !c.stopped() && steps++ < 200000u) c.run_steps(1u);
            outcome[i] = (!c.stopped() && c.pc == kFinish) ? 1u : 0u;
        }
        check(outcome[0] == 1u && outcome[1] == 1u,
              std::string(name) + " after GXInit runs to its return in the original and with the owner: " +
                  owned.cpu.stop().detail);
    }
    {   // negative control: with the callback word null (the old owner), GXLoadTlut does not return
        InitMachine owned(dol, sites, true);
        owned.run(0x81400000u, 0x40000u);
        Cpu& c = owned.cpu;
        c.mem_write32(0x805A2F60u + 0x4CCu, 0u);
        c.clear_stop();
        c.gpr[1] = kStackTop;
        c.gpr[3] = 0x81100000u;
        c.gpr[4] = 0u;
        c.pc = 0x803984C8u;
        c.npc = c.pc + 4;
        c.lr = kFinish;
        unsigned steps = 0;
        while (c.pc != kFinish && !c.stopped() && steps++ < 200000u) c.run_steps(1u);
        check(c.stopped() || c.pc != kFinish, "negative control: a null TLUT region callback is told apart (GXLoadTlut does not return)");
    }
    std::printf("  GXInit: %u argument sets; %u surviving words equal, %u retired words not compared\n", cases, words_compared,
                words_retired);
    // Negative control: an owner that skips GXSetGPFifo leaves the GP pointer / link state different - told apart.
    InitMachine original(dol, sites, false), broken(dol, sites, true);
    broken.cpu.install_hle(0x80394D04u, [](Cpu&) { return true; }, "test: broken GXSetGPFifo");
    original.run(0x81400000u, 0x40000u);
    broken.run(0x81400000u, 0x40000u);
    unsigned differing = 0;
    std::set<u32> all = original.written;
    all.insert(broken.written.begin(), broken.written.end());
    for (const u32 word : all) {
        bool skip = false;
        for (const Retired& r : retired) skip |= word >= r.begin && word < r.end;
        if (!skip && original.cpu.mem_read32(word) != broken.cpu.mem_read32(word)) ++differing;
    }
    check(differing > 0u, "negative control: an owner that skips GXSetGPFifo is told apart (" + std::to_string(differing) + " words)");
}

// GXDrawDone: the original's live draw-done site (BP 0x45 = 2) is where it sleeps on the finish queue; no guest code calls
// it. The owner stops the machine naming it.
void test_draw_done(const std::vector<u8>& dol, const std::vector<GxSiteRecord>& sites) {
    bool listed = false;
    for (const GxSiteRecord& site : sites) listed |= site.address == 0x80396510u && site.family == GxSiteFamily::DrawDone;
    check(listed, "GXDrawDone: its BP 0x45 store (0x80396510) is a listed draw_done site");
    Machine m(dol, sites, true);
    NativeInterrupts interrupts(m.cpu);
    NativeGxSync sync(m.cpu, interrupts);
    sync.install();
    m.setup();
    const Outcome o = m.run(0x803964E8u, args());
    check(!o.returned && o.stop.find("GXDrawDone") != std::string::npos && o.live_sink_calls == 0u,
          "GXDrawDone: the owner stops the machine naming it: " + o.stop);
}

// The three bodies that are not silent (gxB's structural-match finds): two getters and the breakpoint-callback swap,
// original words against the owners on equal machines. Compared: r3, every guest write (address and value), MSR, r1, LR.
void test_small_owners(const std::vector<u8>& dol, const std::vector<GxSiteRecord>& sites) {
    struct Case {
        u32 entry;
        const char* name;
        Call call;
    };
    const Case cases[] = {
        {0x803952FCu, "GXGetCPUFifo", args()},
        {0x803982B8u, "GXGetTexObjData shape", args(kArgs)},
        {0x803982B8u, "GXGetTexObjData shape (other object)", args(kArgs + 0x40)},
        {0x8039822Cu, "GXGetTexObjFmt shape", args(kArgs)},
        {0x8039822Cu, "GXGetTexObjFmt shape (other object)", args(kArgs + 0x40)},
        {0x80398234u, "GXGetTexObjMipMap (flags 0xFF)", args(kArgs)},
        {0x80398234u, "GXGetTexObjMipMap (flags 0xFE)", args(kArgs + 0x40)},
        {0x80398234u, "GXGetTexObjMipMap (flags 0x01)", args(kArgs + 0x80)},
        {0x80398234u, "GXGetTexObjMipMap (flags 0x00)", args(kArgs + 0xC0)},
        {0x80394F3Cu, "GXSetBreakPtCallback (a callback)", args(0x80123450u)},
        {0x80394F3Cu, "GXSetBreakPtCallback (clear)", args(0u)},
    };
    unsigned compared = 0;
    for (const Case& test : cases) {
        for (const u32 ee : {0u, kMsrEE}) {
            Machine original(dol, sites, false), owned(dol, sites, true);
            for (Machine* m : {&original, &owned}) {
                m->setup();
                m->cpu.set_msr(m->cpu.msr() | ee | kMsrFP);
                m->cpu.mem_write32(kSda + 0x5E20u, 0x805A2EE0u);   // CPUFifo
                m->cpu.mem_write32(kSda + 0x5E34u, 0x80777770u);   // BreakPointCB: a previous callback
                m->cpu.mem_write32(kArgs + 0x18u, 0xDEAD0018u);
                m->cpu.mem_write32(kArgs + 0x58u, 0xDEAD0058u);
                m->cpu.mem_write32(kArgs + 0x14u, 0xDEAD0014u);
                m->cpu.mem_write32(kArgs + 0x54u, 0xDEAD0054u);
                m->cpu.mem_write8(kArgs + 0x1Fu, 0xFFu);
                m->cpu.mem_write8(kArgs + 0x5Fu, 0xFEu);
                m->cpu.mem_write8(kArgs + 0x9Fu, 0x01u);
                m->cpu.mem_write8(kArgs + 0xDFu, 0x00u);
                m->cpu.gpr[0] = 0x0C0DE000u;
            }
            const Outcome a = original.run(test.entry, test.call);
            const Outcome b = owned.run(test.entry, test.call);
            const std::string label = std::string(test.name) + (ee ? " EE=1" : " EE=0");
            check(a.returned && b.returned, label + ": both return");
            check(original.cpu.gpr[3] == owned.cpu.gpr[3], label + ": r3 equal: " + hex(original.cpu.gpr[3]) + " / " + hex(owned.cpu.gpr[3]));
            check(original.cpu.msr() == owned.cpu.msr(), label + ": MSR equal");
            if (test.entry == 0x80398234u)   // the mip-map flag getter leaves its cntlzw result in r0
                check(original.cpu.gpr[0] == owned.cpu.gpr[0], label + ": r0 equal: " + hex(original.cpu.gpr[0]) + " / " + hex(owned.cpu.gpr[0]));
            check(a.r1 == b.r1 && a.r1 == kStackTop, label + ": r1 restored");
            std::set<u32> all;
            for (const u32 w : a.written)
                if (!(w >= kStackFloor && w < kStackTop + 8u)) all.insert(w);
            for (const u32 w : b.written)
                if (!(w >= kStackFloor && w < kStackTop + 8u)) all.insert(w);
            for (const u32 w : all)
                check(original.cpu.mem_read32(w) == owned.cpu.mem_read32(w),
                      label + ": word " + hex(w) + " equal: " + hex(original.cpu.mem_read32(w)) + " / " + hex(owned.cpu.mem_read32(w)));
            ++compared;
        }
    }
    // Negative control: the swap's result is the PREVIOUS callback; an owner that returned the new one is told apart.
    Machine original(dol, sites, false);
    original.setup();
    original.cpu.mem_write32(kSda + 0x5E34u, 0x80777770u);
    (void)original.run(0x80394F3Cu, args(0x80123450u));
    check(original.cpu.gpr[3] == 0x80777770u && original.cpu.mem_read32(kSda + 0x5E34u) == 0x80123450u,
          "GXSetBreakPtCallback: the original returns the previous callback and stores the new one (negative control basis)");
    check(original.cpu.gpr[3] != 0x80123450u, "negative control: returning the new callback would be told apart");
    std::printf("  small owners: %u cases (getters and the callback swap) equal\n", compared);
}

// The display-copy scale pair: GXSetDispCopyYScale 0x803970E4 (returns the number of XFB lines, keeps two shadow words)
// and GXGetYScaleFactor 0x80396EAC (a search that answers a float). The original words against the owners over a grid:
// r3 / f1, every guest write outside the stack (address and value), the callee-saved registers.
double as_float_double(double v) { return static_cast<double>(static_cast<float>(v)); }

void compare_scale(const char* what, Machine& original, Machine& owned, const Outcome& a, const Outcome& b, bool float_result,
                   const std::string& label, unsigned& compared) {
    check(a.returned && b.returned, label + ": both return " + a.stop + b.stop);
    if (float_result)
        check(original.cpu.fpr[1].bits[0] == owned.cpu.fpr[1].bits[0], label + ": f1 equal");
    else
        check(original.cpu.gpr[3] == owned.cpu.gpr[3],
              label + ": r3 equal: " + hex(original.cpu.gpr[3]) + " / " + hex(owned.cpu.gpr[3]));
    for (u32 r = 14; r < 32u; ++r) check(original.cpu.gpr[r] == owned.cpu.gpr[r], label + ": r" + std::to_string(r) + " equal");
    for (u32 r = 14; r < 32u; ++r) check(original.cpu.fpr[r].bits[0] == owned.cpu.fpr[r].bits[0], label + ": f" + std::to_string(r) + " equal");
    check(a.r1 == b.r1 && original.cpu.lr == owned.cpu.lr, label + ": r1 and LR equal");
    std::set<u32> all;
    for (const u32 w : a.written)
        if (!(w >= kStackFloor && w < kStackTop + 8u) && !(w >= 0xCC000000u)) all.insert(w);
    for (const u32 w : b.written)
        if (!(w >= kStackFloor && w < kStackTop + 8u) && !(w >= 0xCC000000u)) all.insert(w);
    for (const u32 w : all)
        check(original.cpu.mem_read32(w) == owned.cpu.mem_read32(w),
              label + ": word " + hex(w) + " equal: " + hex(original.cpu.mem_read32(w)) + " / " + hex(owned.cpu.mem_read32(w)));
    check(original.sink.calls == 0u && owned.sink.calls == 0u && a.unlisted == 0u, label + ": no live pipe site");
    (void)what;
    ++compared;
}

void test_scale_functions(const std::vector<u8>& dol, const std::vector<GxSiteRecord>& sites) {
    unsigned compared = 0;
    // GXSetDispCopyYScale: vscale x copy-size words (height - 1 in bits 10..19).
    const double scales[] = {1.0, 0.5, 2.0, 0.75, 1.0833333, 1.5, 3.0, 0.1, 0.9, 1.0001, 1.9, 0.6, 0.55, 16.0, 100.0, 255.0};
    const u32 heights[] = {1u, 240u, 448u, 480u, 528u, 574u, 1024u, 7u};
    for (const double v : scales) {
        for (const u32 h : heights) {
            for (const u32 low : {0u, 0x3FFu}) {
                Machine original(dol, sites, false), owned(dol, sites, true);
                const u32 size = ((h - 1u) << 10) | low | 0x4A000000u;
                for (Machine* m : {&original, &owned}) {
                    m->setup();
                    m->cpu.mem_write32(m->gx + 0x1E4u, size);
                    m->cpu.mem_write32(m->gx + 0x1ECu, 0x12345000u | (h & 1u));
                    m->cpu.mem_write16(m->gx + 2u, 0xFFFFu);
                }
                const Call call = floats(as_float_double(v), 0, 0, 0, 0, 0);
                const Outcome a = original.run(0x803970E4u, call), b = owned.run(0x803970E4u, call);
                compare_scale("yscale", original, owned, a, b, false,
                              "GXSetDispCopyYScale(" + std::to_string(v) + ") height " + std::to_string(h) + " low " + hex(low), compared);
            }
        }
    }
    // The special values: a scale that quantises to 0 (vscale > 256: divwu by zero), NaN, infinity, negative.
    for (const double v : {512.0, 1e30, -1.0, 0.0}) {
        Machine original(dol, sites, false), owned(dol, sites, true);
        for (Machine* m : {&original, &owned}) {
            m->setup();
            m->cpu.mem_write32(m->gx + 0x1E4u, (447u << 10) | 0x4A000000u);
        }
        const Call call = floats(as_float_double(v), 0, 0, 0, 0, 0);
        const Outcome a = original.run(0x803970E4u, call), b = owned.run(0x803970E4u, call);
        compare_scale("yscale-special", original, owned, a, b, false, "GXSetDispCopyYScale(" + std::to_string(v) + ") special", compared);
    }
    // GXGetYScaleFactor(efbHeight, xfbHeight).
    const std::pair<u32, u32> pairs[] = {{480, 480}, {448, 480}, {480, 448}, {528, 480}, {480, 574}, {240, 480}, {480, 240}, {574, 480},
                                         {448, 448}, {600, 576}, {240, 240}, {1, 1}, {1024, 480}, {448, 524}};
    // (Targets the search cannot reach - an efb height of 2 against 480 lines - never end in the original either.)
    for (const auto& pair : pairs) {
        Machine original(dol, sites, false), owned(dol, sites, true);
        for (Machine* m : {&original, &owned}) m->setup();
        const Call call = args(pair.first, pair.second);
        const Outcome a = original.run(0x80396EACu, call), b = owned.run(0x80396EACu, call);
        compare_scale("factor", original, owned, a, b, true,
                      "GXGetYScaleFactor(" + std::to_string(pair.first) + ", " + std::to_string(pair.second) + ")", compared);
    }
    std::printf("  display-copy scale pair: %u cases (r3 / f1, shadow writes, callee-saved registers) equal\n", compared);
    // Negative control: a different efb height gives a different factor / line count.
    Machine x(dol, sites, false), y(dol, sites, false);
    x.setup();
    y.setup();
    (void)x.run(0x80396EACu, args(480, 480));
    (void)y.run(0x80396EACu, args(448, 480));
    check(x.cpu.fpr[1].bits[0] != y.cpu.fpr[1].bits[0], "negative control: different heights give different factors");
}

void test_negative_controls(const std::vector<u8>& dol, const std::vector<GxSiteRecord>& sites) {
    // A body that writes outside the allowed regions (an SDA word), and one that writes a vertex-format word.
    Machine m(dol, sites, false);
    m.setup();
    {
        const u32 code[] = {0x38600001u /* li r3,1 */, 0x90610000u | 0u /* stw r3,0(r1): in the stack, fine */,
                            0x906D5E10u /* stw r3,0x5E10(r13): an SDA word */, 0x4E800020u};
        for (u32 i = 0; i < 4u; ++i) m.bus.write32(kScratchCode + 4u * i, code[i]);
        const Outcome o = m.run(kScratchCode, Call{});
        check(o.returned, "negative control: the scratch body returns");
        check(!judge(o, m.gx, nullptr).inside, "negative control: a write to an SDA word is told apart");
    }
    {
        // lis r4,<gx hi>; ori r4,r4,<gx lo>; stw r3,0x18(r4)  (VCD high)
        const u32 gx = m.gx;
        const u32 code[] = {0x3C800000u | (gx >> 16), 0x60840000u | (gx & 0xFFFFu), 0x90640018u, 0x4E800020u};
        for (u32 i = 0; i < 4u; ++i) m.bus.write32(kScratchCode + 4u * i, code[i]);
        const Outcome o = m.run(kScratchCode, Call{});
        check(o.returned, "negative control: the vertex-word body returns");
        check(judge(o, m.gx, nullptr).vertex, "negative control: a write to a vertex-format word is told apart");
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc >= 3, "usage: gx_state_contract_tests <sys/main.dol> <gx-sites.txt>");
        const std::vector<u8> dol = read_file(argv[1]);
        require(dol.size() >= 256, "missing DOL");
        u64 hash = 0xCBF29CE484222325ull;
        for (u8 byte : dol) hash = (hash ^ byte) * 0x100000001B3ull;
        require(hash == 0xD2CFB4A1CA897F9Bull, "unexpected original DOL identity");
        const std::vector<GxSiteRecord> sites = read_sites(argv[2]);
        require(sites.size() > 1000u, "the GX site list");
        std::size_t count = 0;
        const auto* owners = gx_state::addresses(count);
        // Three owners are not silent (the getters and the callback swap): test_small_owners covers them.
        const std::set<u32> small = {0x803952FCu, 0x803982B8u, 0x8039822Cu, 0x80398234u, 0x80394F3Cu, 0x803970E4u, 0x80396EACu};
        require(count == functions().size() + small.size(), "the owner list and the test's function list differ");
        for (std::size_t i = 0; i < count; ++i) {
            bool found = small.count(owners[i].address) != 0u;
            for (const Function& f : functions()) found |= f.entry == owners[i].address;
            require(found, "owner " + hex(owners[i].address) + " has no test");
        }
        std::size_t lifted_count = 0;
        const auto* lifted_owners = gx_state::lifted_addresses(lifted_count);
        require(lifted_count == lifted_functions().size() + 1u, "the lifted owner list and the test's list differ");
        for (std::size_t i = 0; i < lifted_count; ++i) {
            bool found = lifted_owners[i].address == kGxBegin;
            for (const Function& f : lifted_functions()) found |= f.entry == lifted_owners[i].address;
            require(found, "lifted owner " + hex(lifted_owners[i].address) + " has no test");
        }
        test_functions(dol, sites);
        test_begin(dol, sites);
        test_draw_sync(dol, sites);
        test_gxinit(dol, sites);
        test_draw_done(dol, sites);
        test_small_owners(dol, sites);
        test_scale_functions(dol, sites);
        test_negative_controls(dol, sites);
    } catch (const std::exception& e) {
        std::printf("gx_state_contract_tests: %s\n", e.what());
        return 2;
    }
    if (g_failures != 0) {
        std::printf("gx_state_contract_tests: %d failures\n", g_failures);
        return 1;
    }
    std::printf("gx_state_contract_tests: all checks passed\n");
    return 0;
}
