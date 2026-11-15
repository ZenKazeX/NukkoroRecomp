// liveness-roots: reference-interpreter-only
// The RenderWare texture bind 0x8049AC88 (docs/agents/axfx-effects.md, Step L) as ORIGINAL words in the reference interpreter against the
// native owner (gekko/native_rw_texbind.h): the same prefilled texture / raster / plugin structures on two machines, then every byte of
// the arena, the bound-table words, the preserved registers (r1, r14..r31, f14..f31 both halves), the owner's frame words and the
// words written outside the arena. Both machines serve the GX texture-object functions natively (the original bind calls them) and the
// three GX loads silent, as the lifted build does; the original also runs the original GcnTextureSetRaster words (0x8049819C).
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_gx_texobj.h"
#include "gekko/native_rw_texbind.h"
#include "support/os_entry.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
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
constexpr u32 kArena = 0x81310000u;
constexpr u32 kArenaSize = 0x600u;
constexpr u32 kTable = 0x8056FC90u;
constexpr u32 kTableSpan = 0x120u;       // the filter / format / bound tables, compared whole
constexpr u32 kPlugin = 0x60u;           // SDA + 0x6218
constexpr u32 kRasterPlugin = 0x10u;     // SDA + 0x6200
constexpr u32 kTex = kArena + 0x000u;
constexpr u32 kRaster = kArena + 0x100u;
constexpr u32 kPixels = kArena + 0x200u;
constexpr u32 kDefaultTex = kArena + 0x300u;
constexpr u32 kRegionBase = kArena + 0x400u;

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
    u32 seed = 0;
    u32 texture = 0;       // r3 (0: the default texture)
    u32 unit = 0;          // r4
    u32 raster_flags = 0;  // the byte at raster + 0x23
    u32 plugin_flags = 0;  // the word at texture + 0x60 + 0x20
    u32 version = 0;       // the word at texture + 0x50
    u32 levels = 0;        // the byte at region + 0x32
    u32 sda_half = 0;      // the halfword at SDA + 0x2AF0
    u64 f31 = 0;
    bool f31_ps1_single = true;
    bool default_texture_planted = false;
};

struct Outcome {
    bool returned = false;
    u32 r1 = 0;
    u32 saved[18] = {};
    u64 f_saved[18][2] = {};
    std::vector<u8> arena;
    std::vector<u8> table;
    u32 frame_words[6] = {};
    std::set<u32> written;
    std::string stop;
};

struct Machine {
    GuestMemory bus;
    Cpu cpu{bus};
    std::set<u32>* trace = nullptr;
    Machine(const std::vector<u8>& dol, bool native) {
        const auto word = [&](std::size_t offset) { return load_be32(dol.data() + offset); };
        for (u32 i = 0; i < 18u; ++i) {
            const u32 offset = word(i * 4u), address = word(0x48u + i * 4u), size = word(0x90u + i * 4u);
            if (!size) continue;
            require(u64(offset) + size <= dol.size(), "invalid DOL extent");
            bus.write_block(address, dol.data() + offset, size);
        }
        bus.write32(kFinish, 0x48000000u);
        cpu.reset();
        gx_texobj::install(cpu);
        gx_texobj::install_lifted(cpu);
        if (native) rw_texbind::install_lifted(cpu);
        bus.set_access_trace([this](u32 address, std::size_t size, bool writing) {
            if (!writing || trace == nullptr) return;
            for (std::size_t i = 0; i < size; ++i) trace->insert((address + static_cast<u32>(i)) & ~3u);
        });
    }

    void plant(const Call& c) {
        Rng rng;
        rng.state ^= static_cast<u64>(c.seed) * 0x2545F4914F6CDD1Dull + 1u;
        for (u32 i = 0; i < kArenaSize; i += 4u) cpu.mem_write32(kArena + i, rng.next());
        // the structures: texture -> raster -> pixels; the plugin offsets live in the SDA
        cpu.mem_write32(kSda + 0x6218u, kPlugin);
        cpu.mem_write32(kSda + 0x6200u, kRasterPlugin);
        cpu.mem_write32(kSda + 0x620Cu, kDefaultTex);
        cpu.mem_write32(kSda + 0x6208u, kRaster);
        cpu.mem_write16(kSda + 0x2AF0u, static_cast<u16>(c.sda_half));
        for (const u32 tex : {kTex, kDefaultTex}) {
            cpu.mem_write32(tex, kRaster);
            cpu.mem_write32(tex + 0x50u, c.version ^ (tex == kDefaultTex ? 0x00010000u : 0u));
            cpu.mem_write32(tex + kPlugin + 0x20u, c.plugin_flags);
        }
        cpu.mem_write32(kRaster, kPixels);
        cpu.mem_write8(kRaster + 0x23u, static_cast<u8>(c.raster_flags));
        cpu.mem_write32(kPixels, kRegionBase);  // *pixels is the region base the bind reads; its plugin region sits at +0x10
        cpu.mem_write8(kRegionBase + kRasterPlugin + 0x32u, static_cast<u8>(c.levels));
    }

    Outcome run(const Call& c, const std::function<void(Cpu&)>& after = {}, const std::function<void(Cpu&)>& undo = {}) {
        Outcome o;
        cpu.clear_stop();
        plant(c);
        cpu.gpr[1] = kStackTop;
        cpu.gpr[2] = kSda2;
        cpu.gpr[13] = kSda;
        for (unsigned r = 14; r < 32; ++r) cpu.gpr[r] = 0x11110000u + r;
        for (unsigned f = 14; f < 32; ++f) {
            cpu.fpr[f].bits[0] = 0x3FF0000000000000ull + (static_cast<u64>(f) << 32);
            cpu.fpr[f].bits[1] = 0x4000000000000000ull + (static_cast<u64>(f) << 32);
        }
        cpu.fpr[31].bits[0] = c.f31;
        cpu.fpr[31].bits[1] = c.f31_ps1_single ? 0x3FF8000000000000ull : 0x3FF0000000000001ull;  // 1.5 / 1 + 2^-52 (not a single)
        cpu.gpr[3] = c.texture == 0u ? 0u : c.texture;
        cpu.gpr[4] = c.unit;
        cpu.pc = rw_texbind::kTextureBind;
        cpu.npc = rw_texbind::kTextureBind + 4u;
        cpu.lr = kFinish;
        std::set<u32> written;
        trace = &written;
        unsigned steps = 0;
        while (cpu.pc != kFinish && !cpu.stopped() && steps++ < 100000u) cpu.run_steps(1u);
        trace = nullptr;
        if (after) after(cpu);
        o.returned = cpu.pc == kFinish && !cpu.stopped();
        o.stop = cpu.stopped() ? "stopped" : (o.returned ? "" : "did not return");
        o.r1 = cpu.gpr[1];
        for (unsigned r = 14; r < 32; ++r) o.saved[r - 14] = cpu.gpr[r];
        for (unsigned f = 14; f < 32; ++f) {
            o.f_saved[f - 14][0] = cpu.fpr[f].bits[0];
            o.f_saved[f - 14][1] = cpu.fpr[f].bits[1];
        }
        o.arena.resize(kArenaSize);
        for (u32 i = 0; i < kArenaSize; ++i) o.arena[i] = cpu.mem_read8(kArena + i);
        o.table.resize(kTableSpan);
        for (u32 i = 0; i < kTableSpan; ++i) o.table[i] = cpu.mem_read8(kTable + i);
        const u32 frame = kStackTop - 0x60u;
        const u32 slots[6] = {frame + 8u, frame + 0x50u, frame + 0x54u, frame + 0x58u, frame + 0x5Cu, 0u};
        for (unsigned i = 0; i < 5; ++i) o.frame_words[i] = cpu.mem_read32(slots[i]);
        o.written = std::move(written);
        if (undo) undo(cpu);  // the machines persist between cases: a sabotage is taken back so it cannot leak into the next case
        return o;
    }
};

bool equal(const std::vector<u8>& dol, const Call& call, std::string* why, const std::function<void(Cpu&)>& after_owner = {},
           const std::function<void(Cpu&)>& undo = {}) {
    static Machine original(dol, false), owned(dol, true);  // one pair for the whole run: each call plants everything it reads
    const Outcome a = original.run(call);
    const Outcome b = owned.run(call, after_owner, undo);
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
    for (unsigned i = 0; i < 18; ++i) {
        if (a.saved[i] != b.saved[i]) {
            *why = "callee-saved r" + std::to_string(14 + i);
            return false;
        }
        if (a.f_saved[i][0] != b.f_saved[i][0] || a.f_saved[i][1] != b.f_saved[i][1]) {
            *why = "callee-saved f" + std::to_string(14 + i);
            return false;
        }
    }
    for (u32 i = 0; i < kArenaSize; ++i)
        if (a.arena[i] != b.arena[i]) {
            *why = "arena byte +" + hex(i);
            return false;
        }
    for (u32 i = 0; i < kTableSpan; ++i)
        if (a.table[i] != b.table[i]) {
            *why = "table byte +" + hex(i);
            return false;
        }
    for (unsigned i = 0; i < 5; ++i)
        if (a.frame_words[i] != b.frame_words[i]) {
            *why = "frame word " + std::to_string(i);
            return false;
        }
    const auto inside = [](u32 w) {
        return (w >= kArena - 4u && w < kArena + kArenaSize) || (w >= kTable + 0x4Cu - 4u && w < kTable + kTableSpan);
    };
    for (const u32 w : a.written)
        if (!(inside(w) || (w >= kStackFloor && w < kStackTop + 16u))) {
            *why = "the original wrote outside the arena, the tables and the stack: " + hex(w);
            return false;
        }
    if (!after_owner)
        for (const u32 w : b.written)
            if (!(inside(w) || (w >= kStackTop - 0x60u && w < kStackTop - 0x60u + 0x60u))) {
                *why = "the owner wrote outside the arena, the tables and its frame: " + hex(w);
                return false;
            }
    return true;
}

unsigned g_cases = 0;
unsigned g_paths[8] = {};
void expect(const std::vector<u8>& dol, const Call& call, const std::string& label) {
    std::string why;
    ++g_cases;
    check(equal(dol, call, &why), label + ": " + why);
}

Call random_call(Rng& rng, unsigned i) {
    Call c;
    c.seed = rng.next();
    const u32 raster_bytes[] = {0x00u, 0x20u, 0x40u, 0x60u, 0x80u, 0xA0u, 0xC0u, 0xE0u, 0x1Fu, 0x3Fu};
    c.raster_flags = (i % 5u == 4u) ? (rng.next() & 0xFFu) : raster_bytes[rng.next() % 10u];
    // plugin flags: bit 0x01000000 (new), 0x02000000 (preloaded), the low halfword (version)
    c.plugin_flags = (rng.next() & 0xFCFCFFFFu) | ((rng.next() & 1u) ? 0x01000000u : 0u) | ((rng.next() & 1u) ? 0x02000000u : 0u);
    c.version = (rng.next() & 0xFFFF0000u) | ((rng.next() % 3u == 0u) ? (c.plugin_flags & 0xFFFFu) : (rng.next() & 0xFFFFu));
    if (rng.next() % 2u == 0u) c.version = (c.version & 0xFFFF0000u) | (c.plugin_flags & 0xFFFFu);  // the same version about half the time
    c.levels = (rng.next() % 4u == 0u) ? 0u : (rng.next() & 0xFFu);
    c.sda_half = rng.next() & 0xFFFFu;
    c.unit = (rng.next() % 5u == 0u) ? (rng.next() & 0xFu) : (rng.next() & 7u);
    c.texture = (rng.next() % 5u == 0u) ? 0u : kTex;
    c.f31 = (static_cast<u64>(rng.next()) << 32) | rng.next();
    c.f31_ps1_single = rng.next() % 2u == 0u;
    return c;
}

void test_random(const std::vector<u8>& dol) {
    Rng rng;
    for (unsigned i = 0; i < 6000; ++i) expect(dol, random_call(rng, i), "bind random #" + std::to_string(i));
}

// Every path of the function, on purpose (the random cases reach them only by chance): the colour-index and plain texture, a new one,
// an unchanged one, a changed version, a preloaded one, the default texture (r3 = 0), with the TLUT name from the object or the unit.
void test_paths(const std::vector<u8>& dol) {
    Rng rng;
    for (const u32 raster_byte : {0x00u, 0x20u, 0x40u, 0x60u})
        for (const u32 new_flag : {0u, 0x01000000u})
            for (const u32 preloaded : {0u, 0x02000000u})
                for (const u32 same : {0u, 1u})
                    for (const u32 texture : {kTex, 0u})
                        for (const u32 data_equals_unit : {0u, 1u})
                            for (unsigned i = 0; i < 3; ++i) {
                                Call c = random_call(rng, i);
                                c.raster_flags = raster_byte;
                                c.plugin_flags = (c.plugin_flags & ~0x03000000u) | new_flag | preloaded;
                                c.version = (c.version & 0xFFFF0000u) | (same ? (c.plugin_flags & 0xFFFFu) : ((c.plugin_flags + 1u) & 0xFFFFu));
                                c.texture = texture;
                                c.unit = rng.next() & 7u;
                                (void)data_equals_unit;
                                expect(dol, c, "bind path raster=" + hex(raster_byte) + " new=" + hex(new_flag) + " pre=" + hex(preloaded) +
                                                    " same=" + std::to_string(same) + " tex=" + hex(texture));
                            }
}

// The owner against deliberately damaged results: each comparison must notice.
void test_negative_controls(const std::vector<u8>& dol) {
    Rng rng;
    struct Control {
        const char* name;
        std::function<void(Cpu&)> sabotage;
        std::function<void(Cpu&)> undo;  // the same flip again for the words that outlive a case
    };
    const Control controls[] = {
        {"texobj word 0", [](Cpu& c) { c.mem_write8(kTex + kPlugin + 1u, static_cast<u8>(c.mem_read8(kTex + kPlugin + 1u) ^ 1u)); }, [](Cpu& c) { c.mem_write8(kTex + kPlugin + 1u, static_cast<u8>(c.mem_read8(kTex + kPlugin + 1u) ^ 1u)); }},
        {"texobj image address", [](Cpu& c) { c.mem_write8(kTex + kPlugin + 0x1Du, static_cast<u8>(c.mem_read8(kTex + kPlugin + 0x1Du) ^ 0x10u)); }, [](Cpu& c) { c.mem_write8(kTex + kPlugin + 0x1Du, static_cast<u8>(c.mem_read8(kTex + kPlugin + 0x1Du) ^ 0x10u)); }},
        {"plugin flags", [](Cpu& c) { c.mem_write8(kTex + kPlugin + 0x20u, static_cast<u8>(c.mem_read8(kTex + kPlugin + 0x20u) ^ 1u)); }, [](Cpu& c) { c.mem_write8(kTex + kPlugin + 0x20u, static_cast<u8>(c.mem_read8(kTex + kPlugin + 0x20u) ^ 1u)); }},
        {"bound table", [](Cpu& c) { c.mem_write32(kTable + 0x4Cu, c.mem_read32(kTable + 0x4Cu) ^ 4u); }, [](Cpu& c) { c.mem_write32(kTable + 0x4Cu, c.mem_read32(kTable + 0x4Cu) ^ 4u); }},
        {"version halfword", [](Cpu& c) { c.mem_write8(kRegionBase + kRasterPlugin + 0x31u, static_cast<u8>(c.mem_read8(kRegionBase + kRasterPlugin + 0x31u) ^ 1u)); }, [](Cpu& c) { c.mem_write8(kRegionBase + kRasterPlugin + 0x31u, static_cast<u8>(c.mem_read8(kRegionBase + kRasterPlugin + 0x31u) ^ 1u)); }},
        {"callee-saved r20", [](Cpu& c) { c.gpr[20] ^= 1u; }},
        {"f31 second half", [](Cpu& c) { c.fpr[31].bits[1] ^= 1ull << 40; }},
        {"stack pointer", [](Cpu& c) { c.gpr[1] += 16u; }},
    };
    for (const Control& control : controls) {
        unsigned noticed = 0, tried = 0;
        for (unsigned i = 0; i < 40; ++i) {
            Call c = random_call(rng, i);
            c.texture = kTex;
            c.plugin_flags |= 0x01000000u;  // the object is rebuilt, so the plugin words are written and read back
            std::string why;
            ++tried;
            if (!equal(dol, c, &why, control.sabotage, control.undo)) ++noticed;
        }
        check(noticed == tried, std::string("negative control not caught every time: ") + control.name + " " + std::to_string(noticed) + "/" + std::to_string(tried));
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: rw_texbind_contract_tests <sys/main.dol>\n");
        return 2;
    }
    try {
        const std::vector<u8> dol = read_file(argv[1]);
        test_random(dol);
        std::printf("  random: %u cases against the original words\n", g_cases);
        test_paths(dol);
        std::printf("  + every path: %u cases in all\n", g_cases);
        test_negative_controls(dol);
        {
            GuestMemory memory;
            Cpu cpu(memory);
            rw_texbind::install_lifted(cpu);
            check(cpu.has_hle(rw_texbind::kTextureBind), "the owner is not installed");
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL rw texbind contract: %s\n", error.what());
        return 1;
    }
    if (g_failures != 0) {
        std::printf("FAIL rw texbind contract: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("PASS rw texbind contract\n");
    return 0;
}
