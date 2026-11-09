// liveness-roots: reference-interpreter-only
// The GX texture-object functions (docs/agents/axfx-effects.md, Step J1) as ORIGINAL words in the reference interpreter
// against the native owners (gekko/native_gx_texobj.h): the same arguments and the same prefilled memory on two machines,
// then every byte around the object, the return registers (r3, f1), the preserved registers and the written word set.
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/gx_sites.h"
#include "gekko/native_gx_texobj.h"
#include "support/os_entry.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <sstream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace gekko;

constexpr u32 kSda = 0x805EC500u;
constexpr u32 kSda2 = 0x805FA780u;
constexpr u32 kStackTop = 0x817E0000u;
constexpr u32 kStackFloor = 0x817D0000u;
constexpr u32 kFinish = 0x817FF000u;
constexpr u32 kObj = 0x81310100u;       // the objects and regions live in a prefilled arena
constexpr u32 kArena = 0x81310000u;
constexpr u32 kArenaSize = 0x400u;

int g_failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) {
        ++g_failures;
        if (g_failures <= 60) std::printf("FAIL: %s\n", what.c_str());
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

const std::vector<GxSiteRecord>* g_sites = nullptr;

struct Rng {
    u64 state = 0x9E3779B97F4A7C15ull;
    u32 next() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return static_cast<u32>(state >> 16);
    }
};

struct Call {
    u32 r[11] = {};          // r3..r10 in r[3..10]
    double f[4] = {};        // f1..f3 in f[1..3]
    u32 stack_arg = 0;       // 8(r1)
    u32 seed = 0;            // the prefill of the arena
    std::vector<u32> pre;    // words written at kObj after the prefill (a recorded object)
    std::vector<u32> gx_pre; // words written at gx+0x4D8 (a recorded projection shadow)
};

struct Outcome {
    bool returned = false;
    u32 r3 = 0;
    u64 f1_ps0 = 0, f1_ps1 = 0;
    u32 r1 = 0;
    u32 saved[18] = {};
    std::vector<u8> arena;
    std::set<u32> written;
    std::string stop;
};

struct Machine {
    GuestMemory bus;
    Cpu cpu{bus};
    std::set<u32>* trace = nullptr;
    CountingSink sink;
    u32 gx = 0;
    Machine(const std::vector<u8>& dol, const std::vector<GxSiteRecord>& sites, bool native, bool lifted = false) {
        const auto word = [&](std::size_t offset) { return load_be32(dol.data() + offset); };
        for (u32 i = 0; i < 18u; ++i) {
            const u32 offset = word(i * 4u), address = word(0x48u + i * 4u), size = word(0x90u + i * 4u);
            if (!size) continue;
            require(u64(offset) + size <= dol.size(), "invalid DOL extent");
            bus.write_block(address, dol.data() + offset, size);
        }
        bus.write32(kFinish, 0x48000000u);
        gx = bus.peek32(kSda2 + 0xFD8u);
        cpu.reset();
        cpu.set_gx_sites(sites.data(), sites.size(), &sink);
        if (native) gx_texobj::install(cpu);
        if (native && lifted) gx_texobj::install_lifted(cpu);
        bus.set_access_trace([this](u32 address, std::size_t size, bool writing) {
            if (!writing || trace == nullptr) return;
            for (std::size_t i = 0; i < size; ++i) trace->insert((address + static_cast<u32>(i)) & ~3u);
        });
    }
    Outcome run(u32 entry, const Call& c) {
        Outcome o;
        cpu.clear_stop();
        // identical prefill of the arena
        Rng rng;
        rng.state ^= static_cast<u64>(c.seed) * 0x2545F4914F6CDD1Dull + 1u;
        for (u32 i = 0; i < kArenaSize; i += 4u) cpu.mem_write32(kArena + i, rng.next());
        for (u32 i = 0; i < c.pre.size(); ++i) cpu.mem_write32(kObj + 4u * i, c.pre[i]);
        for (u32 i = 0; i < c.gx_pre.size(); ++i) cpu.mem_write32(gx + 0x4D8u + 4u * i, c.gx_pre[i]);
        cpu.gpr[1] = kStackTop;
        cpu.gpr[2] = kSda2;
        cpu.gpr[13] = kSda;
        for (unsigned r = 14; r < 32; ++r) cpu.gpr[r] = 0x11110000u + r;
        for (unsigned r = 3; r <= 10; ++r) cpu.gpr[r] = c.r[r];
        for (unsigned f = 1; f <= 3; ++f) {
            u64 bits;
            std::memcpy(&bits, &c.f[f], sizeof bits);
            cpu.fpr[f].bits[0] = bits;
            cpu.fpr[f].bits[1] = bits;
        }
        cpu.mem_write32(kStackTop + 8u, c.stack_arg);
        cpu.pc = entry;
        cpu.npc = entry + 4u;
        cpu.lr = kFinish;
        std::set<u32> written;
        trace = &written;
        unsigned steps = 0;
        while (cpu.pc != kFinish && !cpu.stopped() && steps++ < 100000u) cpu.run_steps(1u);
        trace = nullptr;
        o.returned = cpu.pc == kFinish && !cpu.stopped();
        o.stop = cpu.stopped() ? "stopped" : (o.returned ? "" : "did not return");
        o.r3 = cpu.gpr[3];
        o.f1_ps0 = cpu.fpr[1].bits[0];
        o.f1_ps1 = cpu.fpr[1].bits[1];
        o.r1 = cpu.gpr[1];
        for (unsigned r = 14; r < 32; ++r) o.saved[r - 14] = cpu.gpr[r];
        o.arena.resize(kArenaSize);
        for (u32 i = 0; i < kArenaSize; ++i) o.arena[i] = cpu.mem_read8(kArena + i);
        o.written = std::move(written);
        return o;
    }
};

struct Compare {
    bool float_result = false;
    bool int_result = false;
};

// Run the original and the owner; true when everything observable is equal. `why` names the first difference.
bool equal(const std::vector<u8>& dol, u32 entry, const Call& call, const Compare& what, std::string* why,
           const std::function<void(Cpu&)>& after_owner = {}) {
    static Machine original(dol, *g_sites, false), owned(dol, *g_sites, true);  // one pair for the whole run: each call prefills what it reads
    const Outcome a = original.run(entry, call);
    Outcome b;
    if (after_owner) {
        // the negative controls: the owner, then the sabotage
        owned.cpu.clear_stop();
        b = owned.run(entry, call);
        after_owner(owned.cpu);
        b.arena.resize(kArenaSize);
        for (u32 i = 0; i < kArenaSize; ++i) b.arena[i] = owned.cpu.mem_read8(kArena + i);
    } else {
        b = owned.run(entry, call);
    }
    if (!a.returned) {
        *why = "the original did not return: " + a.stop;
        return false;
    }
    if (!b.returned) {
        *why = "the owner did not return: " + b.stop;
        return false;
    }
    if (a.r1 != b.r1) {
        *why = "r1";
        return false;
    }
    for (unsigned i = 0; i < 18; ++i)
        if (a.saved[i] != b.saved[i]) {
            *why = "callee-saved r" + std::to_string(14 + i);
            return false;
        }
    if (what.int_result && a.r3 != b.r3) {
        *why = "r3 " + hex(a.r3) + " vs " + hex(b.r3);
        return false;
    }
    if (what.float_result && (a.f1_ps0 != b.f1_ps0 || a.f1_ps1 != b.f1_ps1)) {
        *why = "f1";
        return false;
    }
    for (u32 i = 0; i < kArenaSize; ++i)
        if (a.arena[i] != b.arena[i]) {
            *why = "arena byte +" + hex(i);
            return false;
        }
    // The original writes only the arena and its own stack frame; the owner only the arena.
    for (const u32 w : a.written)
        if (!((w >= kArena - 4u && w < kArena + kArenaSize) || (w >= kStackFloor && w < kStackTop + 16u))) {
            *why = "the original wrote outside the arena and the stack: " + hex(w);
            return false;
        }
    if (!after_owner)
        for (const u32 w : b.written)
            if (!(w >= kArena - 4u && w < kArena + kArenaSize)) {
                *why = "the owner wrote outside the arena: " + hex(w);
                return false;
            }
    return true;
}

double widen(float f) { return static_cast<double>(f); }

unsigned g_cases = 0;
void expect(const std::vector<u8>& dol, u32 entry, const Call& call, const Compare& what, const std::string& label) {
    std::string why;
    ++g_cases;
    check(equal(dol, entry, call, what, &why), label + ": " + why);
}

void test_init_tex_obj(const std::vector<u8>& dol) {
    Rng rng;
    const u32 formats[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 0x10, 0x1E, 0x2A, 0xFFFFFFFFu};
    const u32 sizes[] = {0, 1, 2, 3, 4, 7, 8, 9, 16, 31, 32, 33, 64, 100, 128, 255, 256, 512, 640, 1024, 0xFFFF, 0x10001, 0x12345};
    for (const u32 fmt : formats)
        for (const u32 mip : {0u, 1u, 0x100u, 0xFFu})
            for (unsigned i = 0; i < 14; ++i) {
                Call c;
                c.seed = rng.next();
                c.r[3] = kObj + ((i % 7u == 6u) ? 1u : 0u);  // an unaligned object now and then
                c.r[4] = (i % 3u == 0u) ? rng.next() : 0x80123400u + (rng.next() & 0xFFE0u);
                c.r[5] = sizes[rng.next() % (sizeof(sizes) / sizeof(sizes[0]))];
                c.r[6] = sizes[rng.next() % (sizeof(sizes) / sizeof(sizes[0]))];
                c.r[7] = fmt;
                c.r[8] = rng.next() & ((i & 1u) ? 3u : 0xFFu);
                c.r[9] = rng.next() & ((i & 2u) ? 3u : 0xFFu);
                c.r[10] = mip;
                expect(dol, gx_texobj::kInitTexObj, c, {}, "GXInitTexObj fmt=" + hex(fmt) + " mip=" + hex(mip));
                // GXInitTexObjCI with a TLUT name on the stack
                c.stack_arg = rng.next();
                expect(dol, gx_texobj::kInitTexObjCI, c, {}, "GXInitTexObjCI fmt=" + hex(fmt) + " mip=" + hex(mip));
            }
}

void test_init_tex_obj_lod(const std::vector<u8>& dol) {
    Rng rng;
    const float values[] = {-100.0f, -4.5f, -4.0f, -3.99f, -1.0f, -0.1f, 0.0f, 0.03125f, 0.5f, 1.0f, 3.5f, 3.99f, 4.0f, 4.5f, 9.99f,
                            10.0f, 10.5f, 31.9f, 100.0f, 1e30f, -1e30f, -0.0f, 1e-40f, -1e-40f, 2147483648.0f, -2147483904.0f, 8.0e9f, std::numeric_limits<float>::infinity(),
                            -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN(), 1e-30f};
    const unsigned n = sizeof(values) / sizeof(values[0]);
    for (unsigned a = 0; a < n; ++a)
        for (unsigned b = 0; b < n; ++b)
            for (unsigned i = 0; i < 5; ++i) {
                Call c;
                c.seed = rng.next();
                c.r[3] = kObj;
                c.r[4] = i < 6 ? (rng.next() % 8u) : rng.next();  // min filter (a register index into the table)
                c.r[5] = (rng.next() % 4u);                      // mag filter
                c.r[6] = (i & 1u) ? 1u : (rng.next() & 0xFFu);   // bias clamp
                c.r[7] = (i & 2u) ? 0u : (rng.next() & 0x1FFu);  // edge LOD
                c.r[8] = rng.next() & 7u;                        // anisotropy
                c.f[1] = widen(values[a]);
                c.f[2] = widen(values[b]);
                c.f[3] = widen(values[(a * 7u + b * 3u + i) % n]);
                expect(dol, gx_texobj::kInitTexObjLod, c, {}, "GXInitTexObjLOD");
            }
}

void test_getters(const std::vector<u8>& dol) {
    Rng rng;
    for (unsigned i = 0; i < 400; ++i) {
        Call c;
        c.seed = rng.next();
        c.r[3] = kObj;
        // plant a chosen word at the object so the interesting bit patterns are all hit
        expect(dol, gx_texobj::kGetLodBias, c, {.float_result = true}, "GXGetTexObjLODBias");
        expect(dol, gx_texobj::kGetBiasClamp, c, {.int_result = true}, "GXGetTexObjBiasClamp");
        expect(dol, gx_texobj::kGetEdgeLod, c, {.int_result = true}, "GXGetTexObjEdgeLOD");
        expect(dol, gx_texobj::kGetMaxAniso, c, {.int_result = true}, "GXGetTexObjMaxAniso");
    }
}

void test_region(const std::vector<u8>& dol) {
    Rng rng;
    for (unsigned i = 0; i < 300; ++i) {
        Call c;
        c.seed = rng.next();
        c.r[3] = kObj + 4u * (i % 3u);
        c.r[4] = rng.next();
        c.r[5] = rng.next();
        c.r[6] = rng.next();
        expect(dol, gx_texobj::kInitTexCacheRegion, c, {}, "GX texture cache region initialiser");
    }
}


// GXGetProjectionv: exact against the original words (the shadow words are prefilled with finite floats).
void set_gx_words(Machine& m, u32 seed) {
    Rng rng;
    rng.state ^= static_cast<u64>(seed) * 0x2545F4914F6CDD1Dull + 3u;
    m.cpu.mem_write32(m.gx + 0x4D8u, (seed & 1u) ? 0u : rng.next());
    for (u32 i = 0; i < 6u; ++i) {
        const float f = static_cast<float>(static_cast<s32>(rng.next() % 2001u) - 1000) / 8.0f;
        u32 bits;
        std::memcpy(&bits, &f, sizeof bits);
        m.cpu.mem_write32(m.gx + 0x4DCu + 4u * i, bits);
    }
}

void test_projection(const std::vector<u8>& dol) {
    static Machine original(dol, *g_sites, false), owned(dol, *g_sites, true);
    for (u32 i = 0; i < 64u; ++i) {
        Call c;
        c.seed = i;
        c.r[3] = kObj + 8u * (i % 4u);
        set_gx_words(original, i);
        set_gx_words(owned, i);
        const Outcome a = original.run(gx_texobj::kGetProjectionv, c);
        set_gx_words(owned, i);
        const Outcome b = owned.run(gx_texobj::kGetProjectionv, c);
        ++g_cases;
        check(a.returned && b.returned && a.arena == b.arena && a.r1 == b.r1, "GXGetProjectionv: the destination array differs");
    }
}

// The lifted loads: the original (with the default region callbacks installed) writes only the object / region records,
// the __GXData shadow and its own stack and sends dead pipe words; the lifted owner writes nothing.
void test_lifted_loads(const std::vector<u8>& dol) {
    struct Load {
        u32 entry;
        const char* name;
        u32 r3, r4, r5;
    };
    const Load loads[] = {{gx_texobj::kLoadTexObj, "GXLoadTexObj", kObj, 0, 0},
                          {gx_texobj::kLoadTexObj, "GXLoadTexObj", kObj, 3, 0},
                          {gx_texobj::kLoadTexObjPreLoaded, "pre-loaded load", kObj, kObj + 0x80, 2},
                          {gx_texobj::kLoadTlut, "GXLoadTlut", kObj, 1, 0},
                          {gx_texobj::kInvalidateTexAll, "GXInvalidateTexAll", 0, 0, 0}};
    std::map<std::string, std::set<u32>> outside;
    for (const Load& load : loads) {
        Machine original(dol, *g_sites, false), owned(dol, *g_sites, true, true);
        for (Machine* m : {&original, &owned}) {
            m->cpu.mem_write32(m->gx + 0x4C8u, 0x80393634u);  // the default texture-region callback
            m->cpu.mem_write32(m->gx + 0x4CCu, 0x80393730u);  // the default TLUT-region callback
        }
        Call c;
        c.seed = 5;
        c.r[3] = load.r3;
        c.r[4] = load.r4;
        c.r[5] = load.r5;
        const Outcome a = original.run(load.entry, c);
        const Outcome b = owned.run(load.entry, c);
        ++g_cases;
        check(a.returned, std::string(load.name) + ": the original returns: " + a.stop);
        check(b.returned && b.written.empty(), std::string(load.name) + ": the lifted owner returns and writes nothing");
        check(b.saved[0] == a.saved[0] && b.r1 == a.r1, std::string(load.name) + ": preserved registers");
        for (const u32 w : a.written) {
            const bool arena = w >= kArena - 4u && w < kArena + kArenaSize;
            const bool stack = w >= kStackFloor && w < kStackTop + 16u;
            const bool gxdata = w >= original.gx && w < original.gx + 0x600u;
            check(arena || stack || gxdata, std::string(load.name) + ": the original wrote outside the object, __GXData and the stack: " + hex(w));
            if (arena) outside[load.name].insert(w - kArena);
            else if (gxdata) outside[load.name].insert(0x10000u + (w - original.gx));
        }
    }
    for (const auto& entry : outside) {
        std::string offsets;
        for (const u32 off : entry.second) {
            char text[16];
            std::snprintf(text, sizeof(text), off >= 0x10000u ? " gx+%X" : " obj+%X", off & 0xFFFFu);
            offsets += text;
        }
        std::printf("    %-20s writes:%s\n", entry.first.c_str(), offsets.c_str());
    }
}


// The recorded corpus (test_data/gx-texobj-corpus/*.txt): the real arguments and the object bytes the legacy tree's game
// passed to these functions (recorded at their entries on Westopolis, Final Haunt and Lethal Highway), replayed against the
// original words and the owners. A line: entry, r4..r10, f1..f3 (bits), the stack word at 8(r1), then the object words
// (8 for the object functions, 4 for the region initialiser, 7 for the projection shadow at gx+0x4D8).
void test_corpus(const std::vector<u8>& dol, const char* directory) {
    unsigned records = 0;
    std::map<u32, unsigned> per_entry;
    std::vector<std::filesystem::path> files;
    for (const auto& item : std::filesystem::directory_iterator(directory))
        if (item.path().extension() == ".txt") files.push_back(item.path());
    std::sort(files.begin(), files.end());
    for (const auto& path : files) {
        std::ifstream stream(path);
        std::string line;
        unsigned in_file = 0;
        while (std::getline(stream, line)) {
            std::istringstream fields(line);
            std::vector<std::string> tokens;
            for (std::string token; fields >> token;) tokens.push_back(token);
            if (tokens.size() < 12u) continue;
            const u32 entry = static_cast<u32>(std::stoul(tokens[0], nullptr, 16));
            Call c;
            c.seed = records;
            c.r[3] = kObj;
            for (unsigned r = 4; r <= 10; ++r) c.r[r] = static_cast<u32>(std::stoul(tokens[r - 3u], nullptr, 16));
            for (unsigned f = 1; f <= 3; ++f) {
                const u64 bits = std::stoull(tokens[7u + f], nullptr, 16);
                std::memcpy(&c.f[f], &bits, sizeof bits);
            }
            c.stack_arg = static_cast<u32>(std::stoul(tokens[11], nullptr, 16));
            std::vector<u32> words;
            for (std::size_t i = 12; i < tokens.size(); ++i) words.push_back(static_cast<u32>(std::stoul(tokens[i], nullptr, 16)));
            Compare what;
            if (entry == gx_texobj::kGetLodBias) what.float_result = true;
            if (entry == gx_texobj::kGetBiasClamp || entry == gx_texobj::kGetEdgeLod || entry == gx_texobj::kGetMaxAniso) what.int_result = true;
            if (entry == gx_texobj::kGetProjectionv) c.gx_pre = words;
            else c.pre = words;
            if (entry == gx_texobj::kGetProjectionv) {
                // the shadow words are prefilled in both machines by the run (c.gx_pre)
            }
            expect(dol, entry, c, what, "corpus " + path.filename().string() + " entry " + hex(entry));
            ++records;
            ++in_file;
            ++per_entry[entry];
        }
        std::printf("    %-24s %u records%s", path.filename().string().c_str(), in_file, "\n");
    }
    for (const auto& entry : per_entry) std::printf("      %s: %u%s", hex(entry.first).c_str(), entry.second, "\n");
    check(records > 0u, "the corpus directory holds no records");
}

// The negative controls: a deliberately wrong owner must be detected.
void test_negative_controls(const std::vector<u8>& dol) {
    Call c;
    c.seed = 7;
    c.r[3] = kObj;
    c.r[4] = 0x80123400u;
    c.r[5] = 64;
    c.r[6] = 64;
    c.r[7] = 5;
    c.r[10] = 1;
    std::string unused;
    check(!equal(dol, gx_texobj::kInitTexObj, c, {}, &unused, [](Cpu& cpu) { cpu.mem_write32(kObj + 4u, cpu.mem_read32(kObj + 4u) ^ 0x100u); }),
          "negative control (LOD word) was not detected");
    check(!equal(dol, gx_texobj::kInitTexObj, c, {}, &unused, [](Cpu& cpu) { cpu.mem_write8(kObj + 0x1Fu, 0); }),
          "negative control (flags) was not detected");
    c.stack_arg = 0x1234;
    check(!equal(dol, gx_texobj::kInitTexObjCI, c, {}, &unused, [](Cpu& cpu) { cpu.mem_write32(kObj + 0x18u, 0); }),
          "negative control (TLUT name) was not detected");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: gx_texobj_contract_tests <sys/main.dol> <extract/gx-sites.txt>\n");
        return 2;
    }
    try {
        const std::vector<u8> dol = read_file(argv[1]);
        const std::vector<GxSiteRecord> sites = read_sites(argv[2]);
        g_sites = &sites;
        test_init_tex_obj(dol);
        std::printf("  GXInitTexObj / GXInitTexObjCI: %u cases so far\n", g_cases);
        test_init_tex_obj_lod(dol);
        std::printf("  + GXInitTexObjLOD: %u cases so far\n", g_cases);
        test_getters(dol);
        test_region(dol);
        std::printf("  + getters and region initialiser: %u cases against the original words\n", g_cases);
        test_projection(dol);
        test_lifted_loads(dol);
        test_negative_controls(dol);
        if (argc > 3) {
            test_corpus(dol, argv[3]);
            std::printf("  + recorded corpus replayed: %u cases in all%s", g_cases, "\n");
        }
        {
            GuestMemory memory;
            Cpu cpu(memory);
            gx_texobj::install(cpu);
            gx_texobj::install_lifted(cpu);
            std::size_t count = 0;
            const gx_texobj::Address* owners = gx_texobj::addresses(count);
            for (std::size_t i = 0; i < count; ++i) check(cpu.has_hle(owners[i].address), "an owner is not installed");
            owners = gx_texobj::lifted_addresses(count);
            for (std::size_t i = 0; i < count; ++i) check(cpu.has_hle(owners[i].address), "a lifted owner is not installed");
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL gx texobj contract: %s\n", error.what());
        return 1;
    }
    if (g_failures != 0) {
        std::printf("FAIL gx texobj contract: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("PASS gx texobj contract\n");
    return 0;
}
