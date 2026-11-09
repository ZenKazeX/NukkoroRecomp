// The GX pipe site list against the original GX library words.
//
// liveness-roots: this test enters no generated code - every GX SDK function
// and OS routine it calls runs as ORIGINAL words in the reference interpreter -
// so its literals must not keep any body alive in the shipping image:
// liveness-roots: reference-interpreter-only
//
// docs/native-time-20261005/GX_DATA_PLANE.md. The machine loads the original
// DOL and the generator's own input, extract/gx-sites.txt, as the interpreter's
// site table (Cpu::set_gx_sites), with a recording sink in place of System's
// router. There is no write-gather pipe device: a pipe store the table does
// not hold stops the machine. Checked:
//
//   GXSetDrawSync    the BP 0x48 and BP 0x47 payloads arrive as the two token
//                    calls, the BP opcode bytes and GXFlush's padding vanish;
//   GXBegin          the header arrives as the opcode and count calls (the
//                    dirty flush and the dummy flush strip vanish);
//   GXCopyDisp       one copy trigger, BP 0x52, nothing else;
//   GXLoadPosMtxImm  an XF matrix upload (stwu + psq_st) vanishes entirely;
//   GXSetBlendMode, GXSetTevColorIn, GXFlush: state and padding vanish;
//   mutations        a table without GXSetDrawSync's BP 0x48 site, and a pipe
//                    store from a word the table does not list, both stop with
//                    the unlisted-store report.

#include "gekko/cpu.h"
#include "gekko/gx_sites.h"
#include "gekko/guest_memory.h"
#include "support/os_entry.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using gekko::u16;
using gekko::u32;
using gekko::u64;
using gekko::u8;
using gekko::GxSiteFamily;
using gekko::GxSiteRecord;

constexpr u32 kSda = 0x805EC500u;
constexpr u32 kSda2 = 0x805FA780u;
constexpr u32 kStackTop = 0x817E0000u;
constexpr u32 kFinish = 0x817FF000u;     // sentinel return address (holds `b .`)
constexpr u32 kScratchCode = 0x817FE000u;  // a test-only function (the unlisted-store mutation)
constexpr u32 kOsContext = 0x817F0000u;    // the current OSContext (low memory 0xD4)
constexpr u32 kGxDataSize = 0x600u;
constexpr u32 kSdaGxFirst = 0x5DF8u;
constexpr u32 kSdaGxEnd = 0x5E58u;

constexpr u32 kGxSetDrawSync = 0x80396428u;
constexpr u32 kGxBegin = 0x80396934u;
constexpr u32 kGxCopyDisp = 0x80397444u;
constexpr u32 kGxLoadPosMtxImm = 0x80397A34u;
constexpr u32 kGxSetBlendMode = 0x80399D44u;
constexpr u32 kGxSetTevColorIn = 0x80399378u;
constexpr u32 kGxFlush = 0x80396098u;
constexpr u32 kTokenIrqSite = 0x8039645Cu;
constexpr u32 kTokenSite = 0x80396474u;
constexpr u32 kOpcodeSite = 0x803969DCu;
constexpr u32 kCountSite = 0x803969E0u;
constexpr u32 kCopyTriggerSite = 0x8039755Cu;

int failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL %s\n", what.c_str());
    }
}

void require(bool ok, const std::string& what) {
    if (!ok) throw std::runtime_error(what);
}

std::vector<u8> read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    require(static_cast<bool>(stream), "cannot read " + path.string());
    return std::vector<u8>(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

// extract/gx-sites.txt: "site word family [note]".
std::vector<GxSiteRecord> read_sites(const std::filesystem::path& path) {
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
    require(static_cast<bool>(stream), "cannot read " + path.string());
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
        for (const auto& entry : kFamilies) {
            if (family == entry.first) {
                record.family = entry.second;
                known = true;
            }
        }
        require(known, "unknown family in " + line);
        sites.push_back(record);
    }
    std::sort(sites.begin(), sites.end(),
              [](const GxSiteRecord& a, const GxSiteRecord& b) { return a.address < b.address; });
    return sites;
}

struct RecordingSink final : gekko::GxSiteSink {
    std::vector<std::string> calls;
    void record(const char* what, u32 site, u32 a, u32 b) {
        char line[96];
        std::snprintf(line, sizeof(line), "%s %08X %X %X", what, site, a, b);
        calls.emplace_back(line);
    }
    void draw_sync_token(u32 site, u16 token, bool interrupt) override { record("token", site, token, interrupt); }
    void draw_done(u32 site, u32 value) override { record("draw_done", site, value, 0u); }
    void copy_trigger(u32 site, u32 value) override { record("copy", site, value >> 24, 0u); }
    void primitive_opcode(u32 site, u8 opcode) override { record("opcode", site, opcode, 0u); }
    void primitive_count(u32 site, u16 count) override { record("count", site, count, 0u); }
    void vertex(u32 site, u32 value, u32 bytes) override { record("vertex", site, value, bytes); }
};

std::string text(const char* what, u32 site, u32 a, u32 b) {
    char line[96];
    std::snprintf(line, sizeof(line), "%s %08X %X %X", what, site, a, b);
    return line;
}

struct Machine {
    gekko::GuestMemory bus;
    gekko::Cpu cpu{bus};
    RecordingSink sink;
    u32 gx = 0;

    Machine(const std::vector<u8>& dol, const std::vector<GxSiteRecord>& sites) {
        const auto word = [&](std::size_t offset) { return gekko::load_be32(dol.data() + offset); };
        for (u32 i = 0; i < 18u; ++i) {
            const u32 offset = word(i * 4u), address = word(0x48u + i * 4u), size = word(0x90u + i * 4u);
            if (!size) continue;
            require(static_cast<u64>(offset) + size <= dol.size(), "invalid DOL extent");
            bus.write_block(address, dol.data() + offset, size);
        }
        bus.write32(kFinish, 0x48000000u);
        // PPCSync is an `sc`: the OS's system-call vector as __OSExceptionInit /
        // __OSInitSystemCall leave it (the native OS entry performs its head,
        // which only toggles HID0's sync bits), and a current OSContext for the
        // entry to find.
        test_support::install_os_system_call(bus);
        bus.write32(test_support::kOsCurrentContext, kOsContext);
        gx = bus.peek32(kSda2 + 0xFD8u);
        require(gx >= gekko::GuestMemory::kImageBase && gx < gekko::GuestMemory::kLegacyHeapEnd,
                "the DOL's __GXData pointer");
        cpu.reset();
        cpu.set_gx_sites(sites.data(), sites.size(), &sink);
    }
    void setup() {
        cpu.clear_stop();
        cpu.gpr[1] = kStackTop;
        cpu.gpr[2] = kSda2;
        cpu.gpr[13] = kSda;
        for (u32 a = kSda + kSdaGxFirst; a < kSda + kSdaGxEnd; a += 4u) cpu.mem_write32(a, 0u);
        for (u32 i = 0; i < kGxDataSize; i += 4u) cpu.mem_write32(gx + i, 0u);
        sink.calls.clear();
    }
    // Runs the original words of `entry` until it returns; false if it stopped.
    bool call(u32 entry, u32 r3 = 0u, u32 r4 = 0u, u32 r5 = 0u, u32 r6 = 0u, u32 r7 = 0u) {
        cpu.gpr[3] = r3;
        cpu.gpr[4] = r4;
        cpu.gpr[5] = r5;
        cpu.gpr[6] = r6;
        cpu.gpr[7] = r7;
        cpu.pc = entry;
        cpu.lr = kFinish;
        unsigned steps = 0;
        while (cpu.pc != kFinish && !cpu.stopped() && steps++ < 2000000u) cpu.run_steps(1u);
        return !cpu.stopped() && cpu.pc == kFinish && cpu.gpr[1] == kStackTop;
    }
};

void case_draw_sync(const std::vector<u8>& dol, const std::vector<GxSiteRecord>& sites) {
    Machine m(dol, sites);
    m.setup();
    const bool returned = m.call(kGxSetDrawSync, 0x1234u);
    check(returned, "GXSetDrawSync returned: " + m.cpu.stop().detail);
    check(m.sink.calls == std::vector<std::string>({text("token", kTokenIrqSite, 0x1234u, 1u),
                                                   text("token", kTokenSite, 0x1234u, 0u)}),
          "GXSetDrawSync: the BP 0x48 then the BP 0x47 token, nothing else");
    check(m.cpu.unlisted_gx_stores() == 0u, "GXSetDrawSync: no unlisted pipe store");
}

void case_begin(const std::vector<u8>& dol, const std::vector<GxSiteRecord>& sites) {
    Machine m(dol, sites);
    m.setup();
    // __GXData zeroed: no dirty state, and vNumNot == 0 makes GXBegin send the
    // dummy flush strip first (__GXSendFlushPrim) - its pipe stores are dead.
    const bool returned = m.call(kGxBegin, 0x80u /* quads */, 0u /* format */, 4u);
    check(returned, "GXBegin returned: " + m.cpu.stop().detail);
    check(m.sink.calls == std::vector<std::string>({text("opcode", kOpcodeSite, 0x80u, 0u),
                                                   text("count", kCountSite, 4u, 0u)}),
          "GXBegin: the header byte and the vertex count, nothing else");
    check(m.cpu.unlisted_gx_stores() == 0u, "GXBegin: no unlisted pipe store");
}

void case_copy(const std::vector<u8>& dol, const std::vector<GxSiteRecord>& sites) {
    Machine m(dol, sites);
    m.setup();
    const bool returned = m.call(kGxCopyDisp, 0x81300000u, 1u /* clear */);
    check(returned, "GXCopyDisp returned: " + m.cpu.stop().detail);
    check(m.sink.calls == std::vector<std::string>({text("copy", kCopyTriggerSite, 0x52u, 0u)}),
          "GXCopyDisp: one BP 0x52 copy trigger, nothing else");
    check(m.cpu.unlisted_gx_stores() == 0u, "GXCopyDisp: no unlisted pipe store");
}

void case_silent(const std::vector<u8>& dol, const std::vector<GxSiteRecord>& sites) {
    struct Silent {
        const char* name;
        u32 entry;
        u32 r3, r4, r5, r6, r7;
    };
    const Silent silent[] = {
        {"GXLoadPosMtxImm", kGxLoadPosMtxImm, 0x81310000u, 0u, 0u, 0u, 0u},
        {"GXSetBlendMode", kGxSetBlendMode, 1u, 4u, 5u, 0u, 0u},
        {"GXSetTevColorIn", kGxSetTevColorIn, 0u, 15u, 8u, 10u, 15u},
        {"GXFlush", kGxFlush, 0u, 0u, 0u, 0u, 0u},
    };
    for (const Silent& s : silent) {
        Machine m(dol, sites);
        m.setup();
        for (u32 i = 0; i < 0x40u; i += 4u) m.cpu.mem_write32(0x81310000u + i, 0x3F800000u);  // a matrix of 1.0f
        const bool returned = m.call(s.entry, s.r3, s.r4, s.r5, s.r6, s.r7);
        check(returned, std::string(s.name) + " returned: " + m.cpu.stop().detail);
        check(m.sink.calls.empty() && m.cpu.unlisted_gx_stores() == 0u,
              std::string(s.name) + ": every pipe store is dead transport");
    }
}

void case_mutations(const std::vector<u8>& dol, const std::vector<GxSiteRecord>& sites) {
    {
        std::vector<GxSiteRecord> mutated;
        std::copy_if(sites.begin(), sites.end(), std::back_inserter(mutated),
                     [](const GxSiteRecord& r) { return r.address != kTokenIrqSite; });
        require(mutated.size() + 1u == sites.size(), "the BP 0x48 site is listed");
        Machine m(dol, mutated);
        m.setup();
        const bool returned = m.call(kGxSetDrawSync, 0x0042u);
        check(!returned && m.cpu.unlisted_gx_stores() == 1u &&
                  m.cpu.stop().detail.find("GX site list") != std::string::npos,
              "a list without the BP 0x48 site stops GXSetDrawSync at its store: " + m.cpu.stop().detail);
    }
    {
        Machine m(dol, sites);
        m.setup();
        const u32 words[] = {0x3C60CC01u /* lis r3,0xCC01 */, 0x90038000u /* stw r0,-0x8000(r3) */,
                             0x4E800020u /* blr */};
        for (u32 i = 0; i < 3u; ++i) m.bus.write32(kScratchCode + 4u * i, words[i]);
        const bool returned = m.call(kScratchCode);
        check(!returned && m.cpu.unlisted_gx_stores() == 1u && m.sink.calls.empty(),
              "a pipe store from an unlisted word stops: " + m.cpu.stop().detail);
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc >= 3, "usage: native_gx_sites_reference_tests <main.dol> <gx-sites.txt>");
        const std::vector<u8> dol = read_file(argv[1]);
        require(dol.size() >= 256u, "missing original DOL");
        const std::vector<GxSiteRecord> sites = read_sites(argv[2]);
        require(sites.size() > 1000u, "the GX site list");
        case_draw_sync(dol, sites);
        case_begin(dol, sites);
        case_copy(dol, sites);
        case_silent(dol, sites);
        case_mutations(dol, sites);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "ERROR %s\n", error.what());
        return 2;
    }
    if (failures != 0) {
        std::fprintf(stderr, "native GX sites reference tests: %d failures\n", failures);
        return 1;
    }
    std::printf("native GX sites reference tests: all passed\n");
    return 0;
}
