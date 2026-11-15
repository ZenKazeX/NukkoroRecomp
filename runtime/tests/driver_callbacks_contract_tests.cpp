// liveness-roots: reference-interpreter-only
// The five driver callbacks of gekko/native_driver_callbacks.h (docs/agents/sdk-rwdrv.md) as ORIGINAL words from the pinned DOL in the
// reference interpreter against the native owners, reached the way the game reaches them (the installed owner of the address).
//
//   group B  0x80393634 default texture region callback, 0x80393730 default TLUT region callback: the original runs its own words
//            (including its two callees GXGetTexObjFmt 0x8039822C and GXGetTexObjMipMap 0x80398234, original words here too);
//            inputs are an object whose format word is varied over the whole 32-bit range and whose mipmap byte takes every
//            low-bit pattern, a texture id over the whole signed range, and a random __GXData behind the SDA2 pointer.
//   group C  0x80499A9C / 0x80499AC4 / 0x80499AEC raster render wrappers: the original runs its wrapper words with the core
//            0x80499924 replaced by a recorder; the owner runs through the installed owner table with a recorder owner at the
//            same address.
//
// Compared per case: r3, r1, LR, r14..r31, f14..f31 (both halves), every byte of the arena, the set of words written (the original
// writes only its own frame and the LR word in the caller's frame, all at or above stack top - 0x40 and below stack top + 8; the owner
// writes nothing) and, for the wrappers, what the core was handed (r3, r4, r5 scaled, r6 fast).
//
// Negative controls: the comparison must report a difference in every case of each sabotage of the owner's machine - another format
// word, a flipped mipmap bit, another __GXData pointer, an extra arena word, a clobbered callee-saved register, a moved r1, a damaged
// f31 second half, a changed LR, another r3, an owner served at another address.
//
// usage: driver_callbacks_contract_tests <sys/main.dol>
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_driver_callbacks.h"
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
namespace dc = gekko::driver_callbacks;

constexpr u32 kSda = 0x805EC500u;
constexpr u32 kSda2 = 0x805FA780u;
constexpr u32 kStackTop = 0x817E0000u;
constexpr u32 kFinish = 0x817FF000u;
constexpr u32 kArena = 0x81310000u;
constexpr u32 kArenaSize = 0x600u;
constexpr u32 kGxData = kArena + 0x000u;  // the TLUT records end at +0x388 + 20 * 16 = 0x4C8
constexpr u32 kObject = kArena + 0x500u;

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

struct Core {  // what the raster render core was handed
    bool called = false;
    u32 r3 = 0, r4 = 0, r5 = 0, r6 = 0;
    bool operator==(const Core& o) const { return called == o.called && r3 == o.r3 && r4 == o.r4 && r5 == o.r5 && r6 == o.r6; }
};

struct Outcome {
    bool returned = false;
    std::string stop;
    u32 r1 = 0, r3 = 0, lr = 0;
    u32 saved[18] = {};
    u64 f_saved[18][2] = {};
    std::vector<u32> arena;
    std::set<u32> written;
    Core core;
};

struct Call {
    u32 address = 0;
    u32 seed = 0;
    u32 r3 = 0, r4 = 0;
    u32 format = 0;       // the word at object + 0x14
    u32 mip_byte = 0;     // the byte at object + 0x1F
    u32 core_result = 0;  // what the replaced core returns in r3
};

struct Perturb {  // sabotage of the owner's machine only
    std::function<void(Cpu&)> before;  // after the inputs are planted, before the owner runs
    std::function<void(Cpu&)> after;   // after the owner returned
    u32 owner_address = 0;             // when non-zero the owner of this address is served instead
};

struct Machine {
    GuestMemory bus;
    Cpu cpu{bus};
    std::set<u32>* trace = nullptr;
    Core* core = nullptr;  // where the recorder owner writes
    u32 core_result = 0;
    explicit Machine(const std::vector<u8>& dol, bool native) {
        const auto word = [&](std::size_t offset) { return load_be32(dol.data() + offset); };
        for (u32 i = 0; i < 18u; ++i) {
            const u32 offset = word(i * 4u), address = word(0x48u + i * 4u), size = word(0x90u + i * 4u);
            if (!size) continue;
            require(u64(offset) + size <= dol.size(), "invalid DOL extent");
            bus.write_block(address, dol.data() + offset, size);
        }
        bus.write32(kFinish, 0x48000000u);
        cpu.reset();
        if (native) {
            dc::install(cpu);
            dc::install_lifted(cpu);
            cpu.install_hle(dc::kRasterRenderCore, [this](Cpu& c) {
                if (core != nullptr) *core = Core{true, c.gpr[3], c.gpr[4], c.gpr[5], c.gpr[6]};
                c.gpr[3] = core_result;
                return true;
            }, "test recorder: raster render core");
        }
        bus.set_access_trace([this](u32 address, std::size_t size, bool writing) {
            if (!writing || trace == nullptr) return;
            for (std::size_t i = 0; i < size; ++i) trace->insert((address + static_cast<u32>(i)) & ~3u);
        });
    }

    Outcome run(const Call& c, bool native, const Perturb& p) {
        Outcome o;
        cpu.clear_stop();
        Rng rng;
        rng.state ^= static_cast<u64>(c.seed) * 0x2545F4914F6CDD1Dull + 1u;
        for (u32 i = 0; i < kArenaSize; i += 4u) cpu.mem_write32(kArena + i, rng.next());
        cpu.mem_write32(kSda2 + 0xFD8u, kGxData);
        cpu.mem_write32(kObject + 0x14u, c.format);
        cpu.mem_write8(kObject + 0x1Fu, static_cast<u8>(c.mip_byte));
        cpu.gpr[1] = kStackTop;
        cpu.gpr[2] = kSda2;
        cpu.gpr[13] = kSda;
        for (unsigned r = 14; r < 32; ++r) cpu.gpr[r] = 0x11110000u + r;
        for (unsigned f = 14; f < 32; ++f) {
            cpu.fpr[f].bits[0] = 0x3FF0000000000000ull + (static_cast<u64>(f) << 32);
            cpu.fpr[f].bits[1] = 0x4000000000000000ull + (static_cast<u64>(f) << 32);
        }
        cpu.gpr[0] = rng.next();
        cpu.gpr[3] = c.r3;
        cpu.gpr[4] = c.r4;
        cpu.gpr[5] = rng.next();
        cpu.gpr[6] = rng.next();
        cpu.lr = kFinish;
        std::set<u32> written;
        core_result = c.core_result;
        core = &o.core;
        if (native && p.before) p.before(cpu);
        trace = &written;
        if (native) {
            const u32 address = p.owner_address != 0u ? p.owner_address : c.address;
            const bool served = cpu.serve_owner(address);
            o.returned = served && !cpu.stopped();
            o.stop = !served ? "no owner" : (cpu.stopped() ? "stopped" : "");
        } else {
            cpu.pc = c.address;
            cpu.npc = c.address + 4u;
            unsigned steps = 0;
            while (cpu.pc != kFinish && !cpu.stopped() && steps++ < 1000u) {
                if (cpu.pc == dc::kRasterRenderCore) {  // the recorder replaces the original core
                    o.core = Core{true, cpu.gpr[3], cpu.gpr[4], cpu.gpr[5], cpu.gpr[6]};
                    cpu.gpr[3] = c.core_result;
                    cpu.pc = cpu.lr;
                    cpu.npc = cpu.pc + 4u;
                    continue;
                }
                cpu.run_steps(1u);
            }
            o.returned = cpu.pc == kFinish && !cpu.stopped();
            o.stop = cpu.stopped() ? "stopped" : (o.returned ? "" : "did not return");
        }
        trace = nullptr;
        core = nullptr;
        if (native && p.after) p.after(cpu);
        o.r1 = cpu.gpr[1];
        o.r3 = cpu.gpr[3];
        o.lr = cpu.lr;
        for (unsigned r = 14; r < 32; ++r) o.saved[r - 14] = cpu.gpr[r];
        for (unsigned f = 14; f < 32; ++f) {
            o.f_saved[f - 14][0] = cpu.fpr[f].bits[0];
            o.f_saved[f - 14][1] = cpu.fpr[f].bits[1];
        }
        o.arena.resize(kArenaSize / 4u);
        for (u32 i = 0; i < kArenaSize / 4u; ++i) o.arena[i] = cpu.mem_read32(kArena + 4u * i);
        o.written = std::move(written);
        return o;
    }
};

bool equal(const std::vector<u8>& dol, const Call& call, std::string* why, const Perturb& p = {}) {
    static Machine original(dol, false), owned(dol, true);
    const bool wrapper = call.address != dc::kTexRegion && call.address != dc::kTlutRegion;
    const Outcome a = original.run(call, false, {});
    const Outcome b = owned.run(call, true, p);
    if (!a.returned) {
        *why = "the original did not return: " + a.stop;
        return false;
    }
    if (!b.returned) {
        *why = "the owner did not return: " + b.stop;
        return false;
    }
    if (a.r3 != b.r3) {
        *why = "r3 " + hex(a.r3) + " vs " + hex(b.r3);
        return false;
    }
    if (a.r1 != b.r1 || a.lr != b.lr) {
        *why = "r1 / lr";
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
    if (wrapper) {
        if (!a.core.called) {
            *why = "the original never reached the core";
            return false;
        }
        if (!(a.core == b.core)) {
            *why = "what the core was handed: original (" + hex(a.core.r3) + " " + hex(a.core.r4) + " " + hex(a.core.r5) + " " +
                   hex(a.core.r6) + ") owner (" + std::to_string(b.core.called) + " " + hex(b.core.r3) + " " + hex(b.core.r4) + " " +
                   hex(b.core.r5) + " " + hex(b.core.r6) + ")";
            return false;
        }
    }
    for (u32 i = 0; i < kArenaSize / 4u; ++i)
        if (a.arena[i] != b.arena[i]) {
            *why = "arena word +" + hex(4u * i);
            return false;
        }
    for (const u32 w : a.written)
        if (!(w >= kStackTop - 0x40u && w < kStackTop + 8u)) {
            *why = "the original wrote outside its frame and the caller's LR word: " + hex(w);
            return false;
        }
    if (!b.written.empty()) {
        *why = "the owner wrote " + hex(*b.written.begin());
        return false;
    }
    return true;
}

unsigned g_cases = 0;
void expect(const std::vector<u8>& dol, const Call& call, const std::string& label) {
    std::string why;
    ++g_cases;
    check(equal(dol, call, &why), label + ": " + why);
    if (g_cases % 1000u == 0u) {
        std::printf("  ... %u cases\n", g_cases);
        std::fflush(stdout);
    }
}

struct Control {
    const char* name;
    Perturb perturb;
    bool tex, tlut, wrapper;
    std::function<void(Call&, Rng&)> shape;  // makes the sabotage observable in every case
};

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: driver_callbacks_contract_tests main.dol");
        const std::vector<u8> dol = read_file(argv[1]);
        require(dol.size() >= 256, "missing DOL");
        u64 hash = 0xCBF29CE484222325ull;
        for (u8 byte : dol) hash = (hash ^ byte) * 0x100000001B3ull;
        require(hash == 0xD2CFB4A1CA897F9Bull, "unexpected original DOL identity");

        Rng rng;
        // ---- group B: the texture region callback, a grid over the interesting values and the 32-bit range at random
        const std::vector<u32> formats = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 0x20, 0x80000000u, 0xFFFFFFFFu,
                                          0xFFFFFFFEu, 0x00000106u, 0x80000006u, 0x7FFFFFFFu};
        const std::vector<u32> ids = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 15, 16, 17, 23, 24, 31, 32, 40, 0xFFFFFFFFu, 0xFFFFFFF8u,
                                      0xFFFFFFF7u, 0xFFFFFFEFu, 0xFFFFFFE9u, 0x80000000u, 0x7FFFFFFFu, 0x7FFFFFF8u, 0x80000001u};
        const u32 mips[] = {0x00u, 0x01u, 0xFEu, 0xFFu};
        unsigned tex_cases = 0, tlut_cases = 0, wrapper_cases = 0;
        for (u32 f : formats)
            for (u32 id : ids)
                for (u32 m : mips) {
                    Call c;
                    c.address = dc::kTexRegion;
                    c.seed = rng.next();
                    c.r3 = kObject;
                    c.r4 = id;
                    c.format = f;
                    c.mip_byte = m;
                    expect(dol, c, "tex region format " + hex(f) + " id " + hex(id) + " mip " + hex(m));
                    ++tex_cases;
                }
        for (unsigned i = 0; i < 3000; ++i) {
            Call c;
            c.address = dc::kTexRegion;
            c.seed = rng.next();
            c.r3 = kObject;
            c.r4 = rng.next();
            c.format = (i % 3 == 0) ? rng.next() : rng.next() % 20u;
            c.mip_byte = rng.next() & 0xFFu;
            expect(dol, c, "tex region random " + std::to_string(i));
            ++tex_cases;
        }
        for (u32 id : ids) {
            Call c;
            c.address = dc::kTlutRegion;
            c.seed = rng.next();
            c.r3 = id;
            expect(dol, c, "tlut region id " + hex(id));
            ++tlut_cases;
        }
        for (u32 id = 0; id < 64; ++id) {
            Call c;
            c.address = dc::kTlutRegion;
            c.seed = rng.next();
            c.r3 = id;
            expect(dol, c, "tlut region id " + std::to_string(id));
            ++tlut_cases;
        }
        for (unsigned i = 0; i < 1000; ++i) {
            Call c;
            c.address = dc::kTlutRegion;
            c.seed = rng.next();
            c.r3 = (i % 2) ? rng.next() : rng.next() % 40u;
            expect(dol, c, "tlut region random " + std::to_string(i));
            ++tlut_cases;
        }
        // ---- group C: the three wrappers with random raster / rect words and random core results
        const u32 wrappers[] = {dc::kRasterRenderScaled0Fast1, dc::kRasterRenderScaled0Fast0, dc::kRasterRenderScaled1Fast1};
        for (u32 address : wrappers)
            for (unsigned i = 0; i < 800; ++i) {
                Call c;
                c.address = address;
                c.seed = rng.next();
                c.r3 = rng.next();
                c.r4 = rng.next();
                c.core_result = (i % 4 == 0) ? 0u : (i % 4 == 1 ? 1u : rng.next());
                expect(dol, c, "wrapper " + hex(address) + " " + std::to_string(i));
                ++wrapper_cases;
            }

        // ---- negative controls
        const auto tex_shape = [](Call& c, Rng&) { c.format = 6u; c.mip_byte = (c.mip_byte & ~1u) | (c.seed & 1u); };
        const auto tlut_id = [](Call& c, Rng& r) { c.r3 = r.next() % 20u; };
        std::vector<Control> controls;
        controls.push_back({"owner sees another format word (6 -> 7)",
                            {[](Cpu& c) { c.mem_write32(kObject + 0x14u, 7u); }, {}, 0}, true, false, false, tex_shape});
        controls.push_back({"owner sees the mipmap bit flipped",
                            {[](Cpu& c) { c.mem_write8(kObject + 0x1Fu, c.mem_read8(kObject + 0x1Fu) ^ 1u); }, {}, 0}, true, false, false,
                            tex_shape});
        controls.push_back({"owner sees another __GXData pointer",
                            {[](Cpu& c) { c.mem_write32(kSda2 + 0xFD8u, kGxData + 0x10u); }, {}, 0}, true, true, false,
                            [&](Call& c, Rng& r) {
                                c.format = r.next() % 12u;
                                c.r3 = c.address == dc::kTlutRegion ? r.next() % 20u : kObject;
                            }});
        controls.push_back({"owner writes an extra arena word",
                            {{}, [](Cpu& c) { c.mem_write32(kArena + 0x40u, c.mem_read32(kArena + 0x40u) + 1u); }, 0}, true, true, true, {}});
        controls.push_back({"owner clobbers r20", {{}, [](Cpu& c) { c.gpr[20] ^= 1u; }, 0}, true, true, true, {}});
        controls.push_back({"owner moves r1", {{}, [](Cpu& c) { c.gpr[1] -= 0x10u; }, 0}, true, true, true, {}});
        controls.push_back({"owner damages f31's second half", {{}, [](Cpu& c) { c.fpr[31].bits[1] ^= 1u; }, 0}, true, true, true, {}});
        controls.push_back({"owner changes LR", {{}, [](Cpu& c) { c.lr += 4u; }, 0}, true, true, true, {}});
        controls.push_back({"owner returns another r3", {{}, [](Cpu& c) { c.gpr[3] ^= 0x10u; }, 0}, true, true, true, {}});
        unsigned total_caught = 0, total_cases = 0;
        const auto run_control = [&](const Control& control, u32 address, const char* group) {
            unsigned caught = 0;
            for (unsigned i = 0; i < 40; ++i) {
                Call c;
                c.address = address;
                c.seed = rng.next();
                c.r3 = rng.next();
                c.r4 = rng.next();
                c.format = rng.next() % 12u;
                c.mip_byte = rng.next() & 0xFFu;
                c.core_result = rng.next();
                if (address == dc::kTexRegion) c.r3 = kObject;
                if (address == dc::kTlutRegion) c.r3 = rng.next() % 20u;
                if (control.shape) control.shape(c, rng);
                std::string why;
                if (!equal(dol, c, &why, control.perturb)) ++caught;
            }
            std::printf("  negative control %-42s %-8s caught %u / 40\n", control.name, group, caught);
            check(caught == 40u, std::string("negative control not caught every time: ") + control.name + " (" + group + ")");
            total_caught += caught;
            total_cases += 40u;
        };
        for (const Control& control : controls) {
            if (control.tex) run_control(control, dc::kTexRegion, "tex");
            if (control.tlut) run_control(control, dc::kTlutRegion, "tlut");
            if (control.wrapper) run_control(control, dc::kRasterRenderScaled0Fast1, "wrapper");
        }
        // an owner served at another address of its group hands other results / flags
        const struct { u32 call, serve; const char* group; } wrong[] = {
            {dc::kRasterRenderScaled0Fast1, dc::kRasterRenderScaled0Fast0, "wrapper"},
            {dc::kRasterRenderScaled0Fast0, dc::kRasterRenderScaled1Fast1, "wrapper"},
            {dc::kRasterRenderScaled1Fast1, dc::kRasterRenderScaled0Fast1, "wrapper"},
            {dc::kTexRegion, dc::kTlutRegion, "tex"},
        };
        for (const auto& w : wrong) {
            Control control{"owner served at another address", {{}, {}, w.serve}, false, false, false, {}};
            run_control(control, w.call, w.group);
        }
        require(g_cases > 0, "no cases ran");
        if (g_failures != 0) {
            std::printf("FAIL driver callbacks contract: %d failures in %u cases\n", g_failures, g_cases);
            return 1;
        }
        std::printf("PASS driver callbacks: %u cases equal to the original words (tex region %u, TLUT region %u, wrappers %u): "
                    "r3, r1, LR, r14..r31, f14..f31, arena words, written words, what the core was handed; "
                    "%u / %u negative-control cases caught\n",
                    g_cases, tex_cases, tlut_cases, wrapper_cases, total_caught, total_cases);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL driver callbacks contract: %s\n", error.what());
        return 1;
    }
}
