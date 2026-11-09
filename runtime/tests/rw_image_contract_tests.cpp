// Original-DOL contract for the owned RenderWare raster -> image standard function (RwImageGetRaster 0x80493A40).
// liveness-roots: reference-interpreter-only
// No generated reference body: every SDK instruction comes from the pinned DOL, run in the reference interpreter. A stub oracle (the
// four RenderWare lock / unlock functions are replaced by recording stubs in BOTH runs, so the comparison is of the conversion and
// of the calls the function makes, not of the lock machinery behind them), evidence (a) of docs/agents/gx-rw-plan.md.
// For every legal pair of image depth (4, 8, 32) and raster depth (4, 8, 16, 32), every raster pixel format the converters accept
// (0x100 0x200 0x300 0x500 0x600 0xA00) and several sizes (odd widths, strided rows) the original function is run on a seeded
// scene and the owner on the same scene. Compared: r3, the image (pixels and palette), every byte of the scene area and the order
// of lock / unlock calls with their arguments. The depth pairs and formats the original reports as an RwError are the owner's
// refusal: an ExecutionFault with nothing written (checked below).
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_rw_image.h"

#include <array>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace gekko;
constexpr u32 kReturn = 0x817FF000u, kStack = 0x817E0000u, kR2 = 0x805FA780u, kR13 = 0x805EC500u;
constexpr u32 kRaster = 0x81710000u, kImage = 0x81711000u, kSrc = 0x81712000u, kSrcPalette = 0x81713000u, kDst = 0x81714000u,
              kDstPalette = 0x81715800u, kAreaEnd = 0x81716000u;
constexpr u32 kLock = 0x8048AFC0u, kLockPalette = 0x8048ACE4u, kUnlockPalette = 0x8048ABD8u, kUnlock = 0x8048AB90u;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Call {
    u32 target, a, b;
    bool operator==(const Call& o) const { return target == o.target && a == o.a && b == o.b; }
};

struct Scene {
    u32 image_depth, raster_depth, format_byte, width, height;
    u8 private_flags;
    u32 src_stride, dst_stride;
    u32 seed;
};

struct Oracle {
    GuestMemory memory;
    Cpu cpu{memory};
    std::vector<Call> calls;
    explicit Oracle(const char* path) {
        std::ifstream input(path, std::ios::binary);
        const std::vector<u8> dol{std::istreambuf_iterator<char>(input), {}};
        require(dol.size() >= 256, "missing DOL");
        u64 hash = 0xCBF29CE484222325ull;
        for (u8 byte : dol) hash = (hash ^ byte) * 0x100000001B3ull;
        require(hash == 0xD2CFB4A1CA897F9Bull, "unexpected original DOL identity");
        const auto word = [&](unsigned offset) { return load_be32(dol.data() + offset); };
        for (unsigned i = 0; i < 18; ++i) {
            const u32 offset = word(i * 4), address = word(0x48 + i * 4), size = word(0x90 + i * 4);
            require(u64(offset) + size <= dol.size(), "invalid DOL section");
            if (size) memory.write_block(address, dol.data() + offset, size);
        }
    }
    void stubs() {
        const auto stub = [this](u32 target) {
            cpu.install_hle(target, [this, target](Cpu& c) {
                // Only the arguments the original sets: (raster, level, mode) / (raster, mode); the unlocks take the raster alone.
                const u32 second = target == kLock ? c.gpr[4] * 16u + c.gpr[5] : target == kLockPalette ? c.gpr[4] : 0u;
                calls.push_back({target, c.gpr[3], second});
                c.gpr[3] = 1u;
                return true;
            }, "recording stub");
        };
        stub(kLock);
        stub(kLockPalette);
        stub(kUnlockPalette);
        stub(kUnlock);
    }
    static u32 next(u32& s) { s = s * 1664525u + 1013904223u; return s >> 8; }
    // Seeded scene: the area is filled with a sentinel, then the raster, image and source data are written.
    void build(const Scene& s) {
        std::vector<u8> area(kAreaEnd - kRaster);
        for (u32 i = 0; i < area.size(); ++i) area[i] = static_cast<u8>(0xC3 ^ i ^ (i >> 8));
        memory.write_block(kRaster, area.data(), area.size());
        u32 seed = s.seed;
        for (u32 i = 0; i < 0x400; i += 1) memory.write8(kSrc + i, static_cast<u8>(next(seed)));
        for (u32 i = 0; i < 0x1000 / 4 && kSrc + 0x400 + i * 4 < kSrcPalette; ++i) memory.write32(kSrc + 0x400 + i * 4, next(seed) * 2654435761u);
        for (u32 i = 0; i < 0x200; ++i) memory.write8(kSrcPalette + i, static_cast<u8>(next(seed)));
        memory.write32(kRaster + 0x04, kSrc);
        memory.write32(kRaster + 0x08, kSrcPalette);
        memory.write32(kRaster + 0x0C, s.width);
        memory.write32(kRaster + 0x10, s.height);
        memory.write32(kRaster + 0x14, s.raster_depth);
        memory.write32(kRaster + 0x18, s.src_stride);
        memory.write8(kRaster + 0x22, s.private_flags);
        memory.write8(kRaster + 0x23, static_cast<u8>(s.format_byte));
        memory.write32(kImage + 0x04, s.width);
        memory.write32(kImage + 0x08, s.height);
        memory.write32(kImage + 0x0C, s.image_depth);
        memory.write32(kImage + 0x10, s.dst_stride);
        memory.write32(kImage + 0x14, kDst);
        memory.write32(kImage + 0x18, kDstPalette);
        calls.clear();
        cpu.reset();
        stubs();
        cpu.gpr[1] = kStack; cpu.gpr[2] = kR2; cpu.gpr[13] = kR13;
        cpu.gpr[3] = kImage; cpu.gpr[4] = kRaster;
        cpu.lr = kReturn;
    }
    void run_original() {
        cpu.pc = kImageGetRaster(); cpu.npc = cpu.pc + 4;
        unsigned steps = 0;
        while (cpu.pc != kReturn && !cpu.stopped() && steps++ < 3000000) cpu.run_steps(1);
        require(cpu.pc == kReturn && !cpu.stopped() && cpu.gpr[1] == kStack, "original call failed return/stack contract");
    }
    static u32 kImageGetRaster() { return rw_image::kImageGetRaster; }
    std::vector<u8> area() {
        std::vector<u8> bytes(kAreaEnd - kRaster);
        memory.read_block(kRaster, bytes.data(), bytes.size());
        return bytes;
    }
};

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: rw_image_contract_tests main.dol");
        Oracle o(argv[1]);
        unsigned scenes = 0, refused = 0, lock_scenes = 0;
        const u32 formats[] = {0x100u, 0x200u, 0x300u, 0x500u, 0x600u, 0xA00u};
        const u32 sizes[][2] = {{1, 1}, {2, 2}, {3, 2}, {7, 3}, {8, 4}, {13, 2}};
        for (u32 image_depth : {4u, 8u, 32u}) {
            for (u32 raster_depth : {4u, 8u, 16u, 32u}) {
                const bool legal = image_depth == 32u || raster_depth == 4u || (image_depth == 8u && raster_depth == 8u);
                for (u32 format : formats) {
                    for (const auto& size : sizes) {
                        for (u32 flags_case = 0; flags_case < 3; ++flags_case) {
                            // 0: already locked, palette locked (no calls); 1: unlocked raster; 2: unlocked raster and palette.
                            const bool palettised = raster_depth <= 8u;
                            u32 format_byte = format >> 8;
                            if (palettised) format_byte |= raster_depth == 4u ? 0x40u : 0x20u;
                            const u8 flags = flags_case == 0 ? 0x0Au : flags_case == 1 ? 0x08u : 0x00u;
                            Scene s{image_depth, raster_depth, format_byte, size[0], size[1], flags, 0x40u + (size[0] & 3u) * 4u, 0x120u,
                                    0x9E3779B9u + scenes * 7919u};
                            if (!legal) {
                                if (flags_case != 0) continue;
                                o.build(s);
                                const auto before = o.area();
                                require(!rw_image::serve_image_get_raster(o.cpu), "the owner converted a pair the original rejects");
                                require(o.cpu.stopped() && o.cpu.stop().reason == StopReason::ExecutionFault, "the refusal was not an ExecutionFault");
                                require(o.area() == before, "a refused call wrote");
                                ++refused;
                                continue;
                            }
                            o.build(s);
                            o.run_original();
                            const u32 original_r3 = o.cpu.gpr[3];
                            const auto original = o.area();
                            const auto original_calls = o.calls;
                            o.build(s);
                            require(rw_image::serve_image_get_raster(o.cpu), "the owner refused a legal scene");
                            require(o.cpu.gpr[3] == original_r3 && original_r3 == 1u, "result register differs");
                            require(o.area() == original, "converted bytes differ from the original's");
                            require(o.calls == original_calls, "lock / unlock calls differ from the original's");
                            require(o.cpu.gpr[1] == kStack && o.cpu.lr == kReturn, "the owner changed r1 or lr");
                            if (flags_case != 0) ++lock_scenes;
                            ++scenes;
                        }
                    }
                }
            }
        }
        // Formats the converters reject (luminance 0x400 and the values nobody lists), image depths other than 4, 8, 32.
        for (u32 bad : {0x000u, 0x400u, 0x700u, 0x800u, 0x900u, 0xB00u}) {
            Scene s{32u, 16u, bad >> 8, 2u, 2u, 0x0Au, 0x40u, 0x80u, 77u};
            o.build(s);
            const auto before = o.area();
            require(!rw_image::serve_image_get_raster(o.cpu) && o.cpu.stopped(), "the owner took a raster format the original rejects");
            require(o.area() == before, "a refused format wrote");
            ++refused;
        }
        for (u32 depth : {0u, 1u, 16u, 24u, 64u}) {
            Scene s{depth, 16u, 0x02u, 2u, 2u, 0x0Au, 0x40u, 0x80u, 99u};
            o.build(s);
            const auto before = o.area();
            require(!rw_image::serve_image_get_raster(o.cpu) && o.cpu.stopped(), "the owner took an image depth the original rejects");
            require(o.area() == before, "a refused depth wrote");
            ++refused;
        }
        std::printf("PASS RwImageGetRaster contract: %u scenes (image depth 4/8/32 x raster depth 4/8/16/32 x 6 formats x 6 sizes, "
                    "%u with lock/unlock calls) equal to the interpreted original in r3, image, scene bytes and call order; %u refusals\n",
                    scenes, lock_scenes, refused);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL rw_image_contract_tests: %s\n", e.what());
        return 1;
    }
}
