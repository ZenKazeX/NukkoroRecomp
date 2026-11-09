// Original-DOL contract for the owned RenderWare colour <-> pixel standard functions (RGBTOPIXEL 0x80492BFC, PIXELTORGB
// 0x80492F10).
// liveness-roots: reference-interpreter-only
// No generated reference body: every SDK instruction comes from the pinned DOL, run in the reference interpreter. Compared
// per call: the result register (r3 = 1), the stored pixel / colour bytes, and that no other guest byte of the scratch area
// changed. The error formats (0x400 and the values the original does not list) are not run: the original reports an RwError
// through the engine's error machinery (uninitialised in this harness) and stores an uninitialised stack word, so the owner's
// contract there is to refuse (serve_* returns false, nothing written), checked below.
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_rw_pixel.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace {
using namespace gekko;
using namespace gekko::rw_pixel;
constexpr u32 kReturn = 0x817FF000u, kStack = 0x817E0000u, kR2 = 0x805FA780u, kR13 = 0x805EC500u;
constexpr u32 kOut = 0x81700000u, kIn = 0x81700100u;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Oracle {
    GuestMemory memory;
    Cpu cpu{memory};
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
    // Arguments r3 / r4 / r5; the scratch area holds a sentinel fill and `input` at kIn.
    void prepare(u32 r3, u32 r4, u32 r5, const std::array<u8, 4>& input) {
        std::array<u8, 0x200> fill{};
        for (u32 i = 0; i < fill.size(); ++i) fill[i] = static_cast<u8>(0xC3 ^ i);
        memory.write_block(kOut, fill.data(), fill.size());
        memory.write_block(kIn, input.data(), input.size());
        cpu.reset();
        cpu.gpr[1] = kStack; cpu.gpr[2] = kR2; cpu.gpr[13] = kR13;
        cpu.gpr[3] = r3; cpu.gpr[4] = r4; cpu.gpr[5] = r5;
        cpu.lr = kReturn;
    }
    void run_original(u32 entry) {
        cpu.pc = entry; cpu.npc = entry + 4;
        unsigned steps = 0;
        while (cpu.pc != kReturn && !cpu.stopped() && steps++ < 3000000) cpu.run_steps(1);
        require(cpu.pc == kReturn && !cpu.stopped() && cpu.gpr[1] == kStack, "original call failed return/stack contract");
    }
    std::array<u8, 0x200> area() {
        std::array<u8, 0x200> bytes{};
        memory.read_block(kOut, bytes.data(), bytes.size());
        return bytes;
    }
};

bool valid_format(u32 format) {
    switch (format & 0x0F00u) {
    case 0x000u: case 0x100u: case 0x200u: case 0x300u: case 0x500u: case 0x600u: case 0xA00u: return true;
    default: return false;
    }
}

// The channel values worth distinguishing: every bit position, the rounding edges of 5 / 6 / 4 / 3 bit fields, 0xFF (opaque).
const std::array<u8, 20> kChannels = {0x00, 0x01, 0x03, 0x07, 0x08, 0x0F, 0x10, 0x1F, 0x20, 0x3F,
                                      0x40, 0x7F, 0x80, 0x81, 0xAA, 0xC0, 0xE0, 0xF8, 0xFE, 0xFF};

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: rw_pixel_contract_tests main.dol");
        Oracle o(argv[1]);
        unsigned runs = 0, refused = 0;
        // Stray flag bits in the raster format outside the 0x0F00 field must not matter.
        const u32 extras[] = {0x0000u, 0x00FFu, 0x1000u, 0x8000u, 0x4000u, 0xF0FFu};
        for (u32 base = 0; base <= 0xF00u; base += 0x100u) {
            for (u32 extra : extras) {
                const u32 format = base | extra;
                if (!valid_format(format)) {
                    // The refusal contract: nothing written, no result.
                    o.prepare(kOut, kIn, format, {1, 2, 3, 4});
                    const auto before = o.area();
                    require(!serve_rgb_to_pixel(o.cpu), "RGBTOPIXEL accepted a format the original rejects");
                    require(o.area() == before, "a refused RGBTOPIXEL wrote");
                    o.prepare(kOut, kIn, format, {1, 2, 3, 4});
                    require(!serve_pixel_to_rgb(o.cpu), "PIXELTORGB accepted a format the original rejects");
                    require(o.area() == before, "a refused PIXELTORGB wrote");
                    ++refused;
                    continue;
                }
                // RGBTOPIXEL over a channel grid.
                for (u8 r : kChannels)
                    for (u8 g : kChannels)
                        for (u8 b : {u8(0x00), u8(0x08), u8(0x7F), u8(0xF8), u8(0xFF)})
                            for (u8 a : {u8(0x00), u8(0x20), u8(0xE0), u8(0xFE), u8(0xFF)}) {
                                const std::array<u8, 4> colour = {r, g, b, a};
                                o.prepare(kOut, kIn, format, colour);
                                o.run_original(kRgbToPixel);
                                const u32 original_r3 = o.cpu.gpr[3];
                                const auto original = o.area();
                                o.prepare(kOut, kIn, format, colour);
                                require(serve_rgb_to_pixel(o.cpu), "RGBTOPIXEL refused a valid format");
                                require(o.cpu.gpr[3] == original_r3, "RGBTOPIXEL result register differs");
                                require(o.area() == original, "RGBTOPIXEL stored bytes differ");
                                ++runs;
                            }
                // PIXELTORGB over pixel words: every single bit, bit pairs of the field edges, and a fixed pseudo-random set.
                std::vector<u32> pixels = {0u, 0xFFFFFFFFu, 0x8000u, 0x7FFFu, 0xFFFFu, 0x00FF00FFu, 0xFF00FF00u};
                for (u32 bit = 0; bit < 32; ++bit) pixels.push_back(1u << bit);
                for (u32 bit = 0; bit < 31; ++bit) pixels.push_back(3u << bit);
                u32 seed = 0x12345678u;
                for (unsigned i = 0; i < 400; ++i) {
                    seed = seed * 1664525u + 1013904223u;
                    pixels.push_back(seed);
                }
                for (u32 pixel : pixels) {
                    const std::array<u8, 4> bytes = {u8(pixel >> 24), u8(pixel >> 16), u8(pixel >> 8), u8(pixel)};
                    o.prepare(kOut, kIn, format, bytes);
                    o.run_original(kPixelToRgb);
                    const u32 original_r3 = o.cpu.gpr[3];
                    const auto original = o.area();
                    o.prepare(kOut, kIn, format, bytes);
                    require(serve_pixel_to_rgb(o.cpu), "PIXELTORGB refused a valid format");
                    require(o.cpu.gpr[3] == original_r3, "PIXELTORGB result register differs");
                    require(o.area() == original, "PIXELTORGB stored bytes differ");
                    ++runs;
                }
            }
        }
        // Negative control: the comparison catches a wrong mask (the 565 green field one bit wide).
        {
            unsigned mismatches = 0;
            for (u8 g : kChannels) {
                const std::array<u8, 4> colour = {0x10, g, 0x10, 0xFF};
                o.prepare(kOut, kIn, 0x200u, colour);
                o.run_original(kRgbToPixel);
                const u32 original = o.cpu.mem_read32(kOut);
                u32 mutated = 0;
                const Rgba c{0x10, g, 0x10, 0xFF};
                (void)rgb_to_pixel(c, 0x200u, mutated);
                mutated ^= (mutated & 0x0020u);  // one green bit lost
                if (mutated != original) ++mismatches;
            }
            require(mismatches > 0, "the negative control is not caught");
        }

        // ---- IMAGEFINDRASTERFORMAT (standard function 9) ----
        {
            constexpr u32 kImage = 0x81710000u, kPixels = 0x81711000u, kPalette = 0x81780000u, kRaster = 0x81790000u;
            u32 seed = 0xC0FFEE11u;
            const auto next = [&seed]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
            unsigned image_runs = 0, image_refused = 0, seen_opaque = 0, seen_cutout = 0, seen_translucent = 0;
            const u32 depths[] = {4u, 8u, 16u, 32u};
            const u32 widths[] = {1u, 2u, 3u, 5u, 17u, 0x3FFu, 0x400u, 0x401u};
            const u32 heights[] = {1u, 2u, 3u, 7u};
            const u32 flag_extras[] = {0x0000u, 0x8000u, 0x1000u, 0x9000u, 0x0010u, 0xFF00u};
            for (u32 depth : depths)
                for (u32 width : widths)
                    for (u32 height : heights)
                        for (unsigned kind = 0; kind < 6; ++kind) {
                            const bool palettised = depth == 4u || depth == 8u;
                            const u32 stride = palettised ? width + (next() & 3u) : width * 4u + (next() & 3u);
                            // alpha population: 0 all opaque, 1 cut-out only (0..15 and 255), 2 one mid alpha at the last pixel,
                            // 3 one mid alpha first, 4 random, 5 the 0x0F / 0x10 edge
                            const auto alpha_for = [&](u32 index) -> u8 {
                                switch (kind) {
                                case 0: return 0xFF;
                                case 1: return (index % 3u) == 0u ? static_cast<u8>(index % 16u) : static_cast<u8>(0xFF);
                                case 2: return index == width * height - 1u ? static_cast<u8>(0x80) : static_cast<u8>(0xFF);
                                case 3: return index == 0u ? static_cast<u8>(0x7F) : static_cast<u8>(0xFF);
                                case 4: return static_cast<u8>(next());
                                default: return (index & 1u) ? static_cast<u8>(0x0F) : static_cast<u8>(0x10);
                                }
                            };
                            std::vector<u8> pixels(stride * height + 16u, 0);
                            std::vector<u8> palette(256u * 4u, 0);
                            if (palettised) {
                                for (u32 i = 0; i < 256u; ++i) {
                                    palette[i * 4 + 0] = static_cast<u8>(next());
                                    palette[i * 4 + 1] = static_cast<u8>(next());
                                    palette[i * 4 + 2] = static_cast<u8>(next());
                                    palette[i * 4 + 3] = alpha_for(i);
                                }
                                for (u32 y = 0; y < height; ++y)
                                    for (u32 x = 0; x < width; ++x)
                                        pixels[y * stride + x] = static_cast<u8>((y * width + x) & (depth == 4u ? 15u : 255u));
                            } else {
                                for (u32 y = 0; y < height; ++y)
                                    for (u32 x = 0; x < width; ++x) {
                                        u8* p = &pixels[y * stride + x * 4u];
                                        p[0] = static_cast<u8>(next());
                                        p[1] = static_cast<u8>(next());
                                        p[2] = static_cast<u8>(next());
                                        p[3] = alpha_for(y * width + x);
                                    }
                            }
                            for (u32 type = 0; type < 8u; ++type)
                                for (u32 extra : flag_extras) {
                                    if (type != 0u && type != 4u && type != 5u && extra != 0u && extra != 0x9000u) continue;
                                    const u32 flags = type | extra;
                                    const auto setup = [&]() {
                                        o.cpu.reset();
                                        o.cpu.gpr[1] = kStack; o.cpu.gpr[2] = kR2; o.cpu.gpr[13] = kR13; o.cpu.lr = kReturn;
                                        o.memory.write32(kImage + 4u, width);
                                        o.memory.write32(kImage + 8u, height);
                                        o.memory.write32(kImage + 0xCu, depth);
                                        o.memory.write32(kImage + 0x10u, stride);
                                        o.memory.write32(kImage + 0x14u, kPixels);
                                        o.memory.write32(kImage + 0x18u, kPalette);
                                        o.memory.write_block(kPixels, pixels.data(), pixels.size());
                                        o.memory.write_block(kPalette, palette.data(), palette.size());
                                        std::array<u8, 0x40> fill{};
                                        for (u32 i = 0; i < fill.size(); ++i) fill[i] = static_cast<u8>(0x5A ^ i);
                                        o.memory.write_block(kRaster, fill.data(), fill.size());
                                        o.cpu.gpr[3] = kRaster; o.cpu.gpr[4] = kImage; o.cpu.gpr[5] = flags;
                                    };
                                    const auto raster_bytes = [&]() {
                                        std::array<u8, 0x40> b{};
                                        o.memory.read_block(kRaster, b.data(), b.size());
                                        return b;
                                    };
                                    if (type == 3u || type >= 6u) {
                                        // The original raises an RwError; the owner refuses, having written what the original wrote first.
                                        setup();
                                        require(!serve_image_find_raster_format(o.cpu), "IMAGEFINDRASTERFORMAT accepted an error raster type");
                                        const auto b = raster_bytes();
                                        require(b[0x20] == type && load_be32(&b[0x14]) == 0u, "a refused call lost the type / depth stores");
                                        ++image_refused;
                                        continue;
                                    }
                                    setup();
                                    o.run_original(kImageFindRasterFormat);
                                    const u32 original_r3 = o.cpu.gpr[3];
                                    const auto original = raster_bytes();
                                    setup();
                                    require(serve_image_find_raster_format(o.cpu), "IMAGEFINDRASTERFORMAT refused a valid type");
                                    require(o.cpu.gpr[3] == original_r3, "IMAGEFINDRASTERFORMAT result register differs");
                                    require(raster_bytes() == original, "IMAGEFINDRASTERFORMAT raster bytes differ");
                                    ++image_runs;
                                    if (type == 0u) {
                                        const u32 scan = (original[0x23] << 8) & 0x0F00u;
                                        if (scan == 0x200u) ++seen_opaque;
                                        else if (scan == 0x100u) ++seen_cutout;
                                        else if (scan == 0x300u) ++seen_translucent;
                                    }
                                }
                        }
            // Coverage control: the sweep reaches every answer of the scan, so a wrong alpha rule cannot pass unseen.
            require(seen_opaque > 0 && seen_cutout > 0 && seen_translucent > 0, "the image sweep misses an answer of the scan");
            std::printf("PASS rw image find format: %u calls equal the original (result register, every raster byte); %u error raster "
                        "types refused; answers seen: %u opaque, %u cut-out, %u translucent\n",
                        image_runs, image_refused, seen_opaque, seen_cutout, seen_translucent);
        }
        std::printf("PASS rw pixel: %u calls equal the original (result, every stored byte); %u error formats refused "
                    "without writing; the negative control is caught\n", runs, refused);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL rw pixel contract: %s\n", error.what());
        return 1;
    }
}
