// liveness-roots: reference-interpreter-only
// Contract for RwRasterRender's core (0x80499924, scaffold/rw_driver/raster_render.h). The ORIGINAL runs in the reference
// interpreter from the pinned DOL with its three callees replaced by recorders - the draw body 0x80499394, the error
// formatter 0x8047EC78 and RwErrorSet 0x8047EBD4 - and the native core runs on the same machine with recording sinks.
// Compared per call: which of the three happened (nothing / draw / error and at which of the three error sites - the
// original's site is the return address its formatter call comes back to), the draw's arguments (raster, the rect the
// body is handed - four words as the original copies them to its frame - the scaled and fast flags), the error record
// ({1, message} with the formatter's arguments 2 and 0x8056FB9C) and the result r3 (1, or 0 after an error).
// Grid: current target type T x source type S (0..8 and 255) x scaled x fast, with random rect and raster size words.
// usage: raster_render_contract_tests <sys/main.dol>
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "rw_driver/raster_render.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace gekko;
using namespace gekko::scaffold;
constexpr u32 kR13 = 0x805EC500u, kReturn = 0x80100000u;
constexpr u32 kDraw = 0x80499394u, kFormat = 0x8047EC78u, kErrorSet = 0x8047EBD4u;
constexpr u32 kTarget = 0x80700000u, kRaster = 0x80701000u, kRect = 0x80702000u;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct Record {
    int kind = 0;  // 0 nothing, 1 draw, 2 error
    u32 raster = 0, rect[4] = {}, scaled = 0, fast = 0;
    u32 site_link = 0, format_arg0 = 0, format_arg1 = 0, record_flag = 0, record_message = 0;
    u32 result = 0;
    bool operator==(const Record& o) const {
        return kind == o.kind && raster == o.raster && rect[0] == o.rect[0] && rect[1] == o.rect[1] && rect[2] == o.rect[2] &&
               rect[3] == o.rect[3] && scaled == o.scaled && fast == o.fast && site_link == o.site_link &&
               format_arg0 == o.format_arg0 && format_arg1 == o.format_arg1 && record_flag == o.record_flag &&
               record_message == o.record_message && result == o.result;
    }
};

constexpr u32 kSiteLinks[3] = {0x804999E0u, 0x80499A44u, 0x80499A74u};  // the formatter call's return address per site

// the native sinks write into this
Record g_native;
void native_quad(Cpu& c, u32 raster, const u32 rect[4], bool scaled, bool fast) {
    g_native.kind = 1;
    g_native.raster = raster;
    for (int i = 0; i < 4; ++i) g_native.rect[i] = rect[i];
    g_native.scaled = scaled;
    g_native.fast = fast;
    (void)c;
}
bool native_error(Cpu&, RasterErrorSite site) {
    g_native.kind = 2;
    g_native.site_link = kSiteLinks[static_cast<unsigned>(site)];
    g_native.format_arg0 = 2u;
    g_native.format_arg1 = 0x8056FB9Cu;
    g_native.record_flag = 1u;
    g_native.record_message = 0x1234ABCDu;  // the formatter stub's answer
    return true;
}

struct Machine {
    GuestMemory memory;
    Cpu cpu{memory};
    explicit Machine(const std::vector<u8>& dol) {
        const auto word = [&](std::size_t o) {
            return u32{dol[o]} << 24 | u32{dol[o + 1]} << 16 | u32{dol[o + 2]} << 8 | u32{dol[o + 3]};
        };
        for (u32 i = 0; i < 18u; ++i) {
            const u32 offset = word(i * 4u), address = word(0x48u + i * 4u), size = word(0x90u + i * 4u);
            if (size == 0u) continue;
            require(std::uint64_t(offset) + size <= dol.size(), "invalid DOL extent");
            memory.write_block(address, dol.data() + offset, size);
        }
        cpu.reset();
    }
    void setup(u32 t, u32 s, bool scaled, bool fast, u32 seed) {
        cpu.reset();
        u32 x = seed * 2654435761u + 7u;
        const auto next = [&] { return x = x * 1664525u + 1013904223u; };
        for (u32 i = 0; i < 32; ++i) cpu.gpr[i] = next();
        cpu.gpr[1] = 0x817E0000u;
        cpu.gpr[13] = kR13;
        cpu.lr = kReturn;
        memory.write32(kR13 + kRasterTargetWord, kTarget);
        memory.write8(kTarget + kRasterTypeByte, static_cast<u8>(t));
        memory.write8(kRaster + kRasterTypeByte, static_cast<u8>(s));
        memory.write32(kRaster + 0x0Cu, 8u + (seed % 5u) * 16u);
        memory.write32(kRaster + 0x10u, 4u + (seed % 3u) * 28u);
        for (u32 i = 0; i < 4; ++i) memory.write32(kRect + 4u * i, next() & 0x3FFu);
        cpu.gpr[3] = kRaster;
        cpu.gpr[4] = kRect;
        cpu.gpr[5] = scaled ? 1u + (next() & 3u) : 0u;
        cpu.gpr[6] = fast ? 1u + (next() & 3u) : 0u;
    }
};
}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: raster_render_contract_tests main.dol");
        std::ifstream input(argv[1], std::ios::binary);
        const std::vector<u8> dol{std::istreambuf_iterator<char>(input), {}};
        require(dol.size() >= 256, "missing DOL");
        u64 hash = 0xCBF29CE484222325ull;
        for (u8 byte : dol) hash = (hash ^ byte) * 0x100000001B3ull;
        require(hash == 0xD2CFB4A1CA897F9Bull, "unexpected original DOL identity");
        Machine o(dol), n(dol);
        unsigned runs = 0, draws = 0, errors = 0, nothing = 0;
        const u32 targets[] = {0, 1, 2, 3, 4, 5, 6, 7, 255};
        const u32 sources[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 255};
        for (u32 t : targets)
            for (u32 s : sources)
                for (int flags = 0; flags < 4; ++flags)
                    for (u32 seed = 1; seed <= 3; ++seed) {
                        const bool scaled = (flags & 1) != 0, fast = (flags & 2) != 0;
                        o.setup(t, s, scaled, fast, seed);
                        n.setup(t, s, scaled, fast, seed);
                        Cpu& c = o.cpu;
                        Record original;
                        c.pc = kRasterRenderCore;
                        c.npc = kRasterRenderCore + 4u;
                        for (unsigned steps = 0; steps < 400 && c.pc != kReturn && !c.stopped(); ++steps) {
                            if (c.pc == kDraw) {
                                require(original.kind == 0, "two outcomes in one call");
                                original.kind = 1;
                                original.raster = c.gpr[3];
                                for (u32 i = 0; i < 4; ++i) original.rect[i] = o.memory.read32(c.gpr[4] + 4u * i);
                                original.scaled = c.gpr[5] != 0u;
                                original.fast = c.gpr[6] != 0u;
                                c.gpr[3] = 0x77u;  // the body's return value is not read by the core
                                c.pc = c.lr;
                                c.npc = c.pc + 4u;
                                continue;
                            }
                            if (c.pc == kFormat) {
                                require(original.kind == 0, "two outcomes in one call");
                                original.kind = 2;
                                original.site_link = c.lr;
                                original.format_arg0 = c.gpr[3];
                                original.format_arg1 = c.gpr[4];
                                c.gpr[3] = 0x1234ABCDu;
                                c.pc = c.lr;
                                c.npc = c.pc + 4u;
                                continue;
                            }
                            if (c.pc == kErrorSet) {
                                original.record_flag = o.memory.read32(c.gpr[3]);
                                original.record_message = o.memory.read32(c.gpr[3] + 4u);
                                c.gpr[3] = 0x55u;
                                c.pc = c.lr;
                                c.npc = c.pc + 4u;
                                continue;
                            }
                            c.run_steps(1u);
                        }
                        require(c.pc == kReturn, "the original did not return");
                        original.result = c.gpr[3];
                        g_native = Record{};
                        serve_raster_render(n.cpu, native_quad, native_error);
                        g_native.result = n.cpu.gpr[3];
                        const std::string at = " at T=" + std::to_string(t) + " S=" + std::to_string(s) +
                                               " scaled=" + std::to_string(scaled) + " fast=" + std::to_string(fast);
                        require(original == g_native, "native core differs from the original" + at + " (original kind " +
                                                          std::to_string(original.kind) + " result " +
                                                          std::to_string(original.result) + ", native kind " +
                                                          std::to_string(g_native.kind) + " result " +
                                                          std::to_string(g_native.result) + ")");
                        draws += original.kind == 1;
                        errors += original.kind == 2;
                        nothing += original.kind == 0;
                        ++runs;
                    }
        require(draws > 0 && errors > 0 && nothing > 0, "the grid missed an outcome");
        std::printf("PASS raster render core: %u calls (%u draws, %u errors at three sites, %u silent) equal to the original: "
                    "outcome, error site, rect handed to the draw, flags, error record, r3\n",
                    runs, draws, errors, nothing);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL raster render contract: %s\n", error.what());
        return 1;
    }
}
