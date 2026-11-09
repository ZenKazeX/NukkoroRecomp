// Original DOL execution is an oracle in this test executable only.
// The native implementation imports none of these reference dependencies.
// liveness-roots: this test enters no generated code - the original words it
// compares with run in the reference interpreter - so its literals must not
// keep the replaced bodies in the shipping image:
// liveness-roots: reference-interpreter-only
#include "native_billboard.h"
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace gekko;
using namespace shadow::render;
namespace {
void require(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
u32 bits(float v) { u32 word; std::memcpy(&word, &v, 4); return word; }
float val(u32 word) { float v; std::memcpy(&v, &word, 4); return v; }
// Frozen pre-consolidation renderer layout, oracle only. The production has
// one native definition; this fixture detects byte/offset/default drift.
struct OriginalRendererVertex {
    float position[3], normal[3];
    std::uint8_t color0[4], color1[4];
    float uv[2][2];
};
static_assert(sizeof(OriginalRendererVertex) == 48);
struct Output : MmioDevice {
    std::vector<u32> words;
    const char* name() const override { return "reference-output-recorder"; }
    void write32(u32, u32 word) override { words.push_back(word); }
};
struct Oracle {
    GuestMemory bus;
    Cpu cpu{bus};
    Output output;
    BillboardCurve curve;
    explicit Oracle(const char* path) {
        std::ifstream file(path, std::ios::binary);
        std::vector<u8> dol{std::istreambuf_iterator<char>(file), {}};
        require(dol.size() >= 256, "missing original DOL");
        const auto be = [&](std::size_t off) {
            require(off + 4 <= dol.size(), "DOL header truncated");
            return u32(dol[off]) << 24 | u32(dol[off+1]) << 16 | u32(dol[off+2]) << 8 | dol[off+3];
        };
        for (u32 i = 0; i < 18; ++i) {
            const u32 offset = be(i*4), address = be(0x48+i*4), size = be(0x90+i*4);
            if (!size) continue;
            require(u64(offset) + size <= dol.size() && address >= 0x80000000u,
                    "invalid DOL section");
            auto* destination = bus.host_pointer(address, size);
            require(destination != nullptr, "DOL section exceeds oracle memory");
            std::memcpy(destination, dol.data()+offset, size);
        }
        require(bus.read32(0x8044c260) == 0x9421ff18u && bus.read32(0x8044c6a0) == 0x4e800020u,
                "unexpected callback words");
        require(bus.read32(0x805fc118) == 0xcc008000u, "unexpected original output pointer");
        const auto scalar = [&](u32 address) { return val(bus.read32(address)); };
        for (u32 i = 0; i < 2; ++i) {
            curve.period[i] = scalar(0x8051ce08u+i*4);
            curve.offset[i] = scalar(0x8051ce10u+i*4);
            curve.angle_multiplier[i] = scalar(0x8051ce18u+i*4);
        }
        curve.scale = scalar(0x8051ce20u);
        curve.half_extent = scalar(0x805fc110u);
        bus.write32(0x805eee28u, 0x805fc110u); // constructor-published math inputs
        bus.write32(0x805eee2cu, 0x8051ce08u);
        output.words.reserve(36);
        bus.register_mmio(0xCC008000u, 0x100u, &output);
    }
    BillboardCorners run(const Billboard& p, const BillboardBasis& basis, const BillboardAttributes* attr = nullptr,
                         u32 selection = 0xffffffffu) {
        cpu.reset();
        cpu.spr[SPR_HID2] = kHid2PSE | kHid2LSQE;
        cpu.fpscr = kFpscrNI;
        cpu.set_msr(kMsrFP | kMsrIR | kMsrDR);
        cpu.select_memory_window();
        cpu.set_entry(0x8044c260u); cpu.lr = 0x800f1000u;
        cpu.gpr[1] = 0x800f0000u; cpu.gpr[2] = 0x805fa780u; cpu.gpr[13] = 0x805ec500u;
        cpu.gpr[3] = 1; cpu.gpr[4] = 0; cpu.gpr[5] = 0x80070000u;
        if (attr) {
            if (attr->normal_present) cpu.gpr[4] |= 0x01000000u;
            if (attr->color_present) cpu.gpr[4] |= 0x00040000u;
            if (attr->uv_present) cpu.gpr[4] |= 0x00800000u;
            if (selection != 0xffffffffu) cpu.gpr[4] = selection;
            bus.write32(0x80070014u, 0x80071080u); bus.write32(0x80070018u, 12);
            bus.write32(0x80070024u, 0x800710a0u); bus.write32(0x80070028u, 4);
            bus.write32(0x8007002cu, 0x800710a0u); bus.write32(0x80070030u, 16);
            bus.write32(0x8007003cu, 0x80071100u); bus.write32(0x80070040u, 16);
            bus.write32(0x80070044u, 0x80071120u); bus.write32(0x80070048u, 32);
            for (u32 axis = 0; axis < 3; ++axis) bus.write32(0x80071080u+axis*4, bits(attr->normal[axis]));
            for (u32 corner = 0; corner < 4; ++corner) {
                const auto& color = attr->colors[corner];
                const u32 packed = u32(color[0])<<24 | u32(color[1])<<16 | u32(color[2])<<8 | color[3];
                bus.write32(0x80070080u+corner*4, packed);
                bus.write32(0x800710a0u+corner*4, packed);
                // The reference's UV record order is rotated relative to its
                // output. The native API receives actual output-corner values.
                for (u32 axis = 0; axis < 2; ++axis)
                    bus.write32(0x80070090u+corner*8+axis*4, bits(attr->uv[(corner+1)%4][axis]));
                for (u32 axis = 0; axis < 2; ++axis)
                    bus.write32(0x80071120u+corner*8+axis*4, bits(attr->uv[(corner+1)%4][axis]));
            }
            for (u32 axis = 0; axis < 2; ++axis) {
                bus.write32(0x80071100u+axis*4, bits(attr->uv[1][axis]));
                bus.write32(0x80071108u+axis*4, bits(attr->uv[3][axis]));
                if ((cpu.gpr[4] & 0x00300000u) == 0 && (cpu.gpr[4] & 0x00400000u)) {
                    bus.write32(0x80070090u+axis*4, bits(attr->uv[1][axis]));
                    bus.write32(0x80070098u+axis*4, bits(attr->uv[3][axis]));
                }
            }
        }
        bus.write32(0x80070004u, 0x80071000u); bus.write32(0x80070008u, 12);
        bus.write32(0x8007001cu, 0x80071020u); bus.write32(0x80070020u, 8);
        bus.write32(0x80070034u, 0x80071030u); bus.write32(0x80070038u, 4);
        bus.write32(0x8007004cu, 0x80071040u);
        for (u32 i = 0; i < 2; ++i) {
            bus.write32(0x8051ce08u+i*4, bits(curve.period[i]));
            bus.write32(0x8051ce10u+i*4, bits(curve.offset[i]));
            bus.write32(0x8051ce18u+i*4, bits(curve.angle_multiplier[i]));
        }
        bus.write32(0x8051ce20u, bits(curve.scale));
        bus.write32(0x805fc110u, bits(curve.half_extent));
        for (u32 i = 0; i < 3; ++i) {
            bus.write32(0x80071000u+i*4, bits(p.center[i]));
            bus.write32(0x80071040u+i*4, bits(basis.right[i]));
            bus.write32(0x8007104cu+i*4, bits(basis.up[i]));
        }
        for (u32 i = 0; i < 2; ++i) bus.write32(0x80071020u+i*4, bits(p.size[i]));
        bus.write32(0x80071030u, bits(p.rotation));
        output.words.clear();
        unsigned steps = 0;
        while (!cpu.stopped() && cpu.pc != cpu.lr && steps++ < 1000) cpu.step();
        const std::size_t stride = 3 + (cpu.gpr[4] & 0x01000000u ? 3 : 0) +
            (cpu.gpr[4] & 0x00070000u ? 1 : 0) + (cpu.gpr[4] & 0x00f00000u ? 2 : 0);
        require(!cpu.stopped() && cpu.pc == 0x800f1000u && output.words.size() == stride*4,
                "original callback failed or output incomplete");
        require(bus.diagnostics().unmapped_reads == 0 && bus.diagnostics().unmapped_writes == 0,
                "oracle encountered an unmapped access");
        BillboardCorners result;
        for (std::size_t v = 0; v < 4; ++v)
            for (std::size_t axis = 0; axis < 3; ++axis) result[v][axis] = val(output.words[v*stride+axis]);
        return result;
    }
};
}
int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: native_billboard_tests main.dol");
        auto oracle = std::make_unique<Oracle>(argv[1]);
        u32 rng = 0x7eb921u;
        const auto random = [&]() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; };
        const auto finite = [&]() {
            const u32 r = random();
            return val((r & 0x807fffffu) | ((r >> 24) % 254 + 1) << 23);
        };
        unsigned cases = 0;
        const auto compare = [&](const Billboard& p, const BillboardBasis& b) {
            const auto expected = oracle->run(p, b);
            const auto actual = expand_billboard(p, b, oracle->curve);
            for (std::size_t v = 0; v < 4; ++v) for (std::size_t a = 0; a < 3; ++a) {
                if (bits(expected[v][a]) != bits(actual[v][a])) {
                    std::cerr << "case " << cases << " corner " << v << " axis " << a
                              << " expected " << std::hex << bits(expected[v][a]) << " actual " << bits(actual[v][a]) << '\n';
                    throw std::runtime_error("native corner differs from original words");
                }
            }
            ++cases;
        };
        for (u32 i = 0; i < 5000; ++i) {
            Billboard p; BillboardBasis b;
            for (unsigned a = 0; a < 3; ++a) { p.center[a] = finite(); b.right[a] = finite(); b.up[a] = finite(); }
            p.size = {finite(), finite()}; p.rotation = finite();
            compare(p, b);
        }
        const auto authored = oracle->curve;
        const auto moderate = [&]() { return static_cast<float>(static_cast<int>(random()%200000u)-100000)/1024.0f; };
        for (u32 i = 0; i < 10000; ++i) {
            Billboard p; BillboardBasis b;
            for (unsigned a = 0; a < 3; ++a) { p.center[a] = moderate(); b.right[a] = moderate(); b.up[a] = moderate(); }
            p.size = {moderate(), moderate()}; p.rotation = moderate()/32.0f;
            compare(p, b);
        }
        // Curve/basis globals are explicit inputs, never assumed immutable.
        for (u32 i = 0; i < 1000; ++i) {
            for (auto* pair : {&oracle->curve.period, &oracle->curve.offset, &oracle->curve.angle_multiplier})
                for (auto& lane : *pair) lane = moderate();
            oracle->curve.scale = moderate(); oracle->curve.half_extent = moderate();
            compare(Billboard{{moderate(),moderate(),moderate()}, {moderate(),moderate()}, moderate()},
                    BillboardBasis{{moderate(),moderate(),moderate()}, {moderate(),moderate(),moderate()}});
        }
        oracle->curve = authored;
        // Exceptional values, signed zero, subnormal boundaries and authored cardinal angles.
        const u32 special[] = {0, 0x80000000u, 1, 0x007fffffu, 0x00800000u, 0x80800000u,
            0x7f800000u, 0xff800000u, 0x7fc12345u, 0x7f812345u, 0xff812345u,
            0x3fc90fdbu, 0xbfc90fdbu, 0x40490fdbu, 0xc0490fdbu, 0x3f800000u};
        for (u32 field = 0; field < 12; ++field) for (u32 pattern : special) {
            Billboard p{{4,-2,1}, {2,3}, 0}; BillboardBasis b{{1,0,0}, {0,-1,0}};
            if (field < 3) p.center[field] = val(pattern);
            else if (field < 5) p.size[field-3] = val(pattern);
            else if (field == 5) p.rotation = val(pattern);
            else if (field < 9) b.right[field-6] = val(pattern);
            else b.up[field-9] = val(pattern);
            compare(p, b);
        }
        for (u32 field = 0; field < 8; ++field) for (u32 pattern : special) {
            oracle->curve = authored;
            if (field < 2) oracle->curve.period[field] = val(pattern);
            else if (field < 4) oracle->curve.offset[field-2] = val(pattern);
            else if (field < 6) oracle->curve.angle_multiplier[field-4] = val(pattern);
            else if (field == 6) oracle->curve.scale = val(pattern);
            else oracle->curve.half_extent = val(pattern);
            compare(Billboard{{4,-2,1}, {2,3}, 0.1f}, BillboardBasis{{1,0,0}, {0,-1,0}});
        }
        oracle->curve = authored;
        const auto compare_vertices = [&](const Billboard& p, const BillboardBasis& b,
                                          const BillboardAttributes& selected) {
            std::array<RenderVertex,5> vertices{};
            vertices[4].position[0] = 12345;
            require(emit_billboards(&p, &selected, 1, b, authored, vertices.data(), vertices.size()), "native vertex batch rejected");
            std::size_t word = 0;
            for (std::size_t corner = 0; corner < 4; ++corner) {
                const auto& v = vertices[corner];
                const auto first_word = word;
                for (float lane : v.position) require(bits(lane) == oracle->output.words[word++], "position output differs");
                for (float lane : v.normal) {
                    if (selected.normal_present) require(bits(lane) == oracle->output.words[word++], "normal output differs");
                    else require(bits(lane) == 0, "absent normal not initialized");
                }
                if (selected.color_present) {
                    const u32 packed = u32(v.color0[0])<<24 | u32(v.color0[1])<<16 | u32(v.color0[2])<<8 | v.color0[3];
                    require(packed == oracle->output.words[word++], "color output differs");
                } else for (auto channel : v.color0) require(channel == 255, "absent color not canonical white");
                for (auto channel : v.color1) require(channel == 0, "unused color1 not initialized");
                for (float lane : v.uv[1]) require(bits(lane) == 0, "unused UV1 not initialized");
                for (float lane : v.uv[0]) {
                    if (selected.uv_present) require(bits(lane) == oracle->output.words[word++], "UV output differs");
                    else require(bits(lane) == 0, "absent UV not initialized");
                }
                // Reconstruct exactly the old capture's native packet defaults
                // and direct float/RGBA fields from original output words.
                OriginalRendererVertex expected{};
                std::memset(expected.color0, 255, sizeof(expected.color0));
                auto at = first_word;
                for (float& lane : expected.position) lane = val(oracle->output.words[at++]);
                if (selected.normal_present)
                    for (float& lane : expected.normal) lane = val(oracle->output.words[at++]);
                if (selected.color_present) {
                    const auto packed = oracle->output.words[at++];
                    for (unsigned channel = 0; channel < 4; ++channel)
                        expected.color0[channel] = static_cast<std::uint8_t>(packed >> (24 - channel*8));
                }
                if (selected.uv_present)
                    for (float& lane : expected.uv[0]) lane = val(oracle->output.words[at++]);
                require(at == word && std::memcmp(&expected, &v, sizeof(expected)) == 0,
                        "complete canonical renderer vertex bytes differ");
            }
            require(word == oracle->output.words.size(), "unexamined reference output");
            require(vertices[4].position[0] == 12345, "batch overwrote unused capacity");
        };
        unsigned attribute_cases = 0;
        for (unsigned mask = 0; mask < 8; ++mask) for (unsigned sample = 0; sample < 64; ++sample) {
            const Billboard p{{moderate(),moderate(),moderate()}, {moderate(),moderate()}, moderate()/32};
            const BillboardBasis b{{1,0,0}, {0,-1,0}};
            BillboardAttributes a;
            a.normal_present = (mask & 1) != 0;
            a.color_present = (mask & 2) != 0;
            a.uv_present = (mask & 4) != 0;
            for (auto& lane : a.normal) lane = sample < 16 ? val(special[sample]) : moderate();
            for (auto& color : a.colors) for (auto& channel : color) channel = static_cast<std::uint8_t>(random());
            for (auto& uv : a.uv) for (auto& lane : uv) lane = sample < 16 ? val(special[sample]) : moderate();
            oracle->run(p, b, &a);
            compare_vertices(p, b, a);
            ++attribute_cases;
        }
        unsigned selection_cases = 0;
        // Exhaust the original normal/color/UV selection bits, including conflicts.
        // Only this oracle fixture has original flags/records; the native producer
        // receives actual selected values, never a machine/console descriptor.
        for (unsigned normal = 0; normal < 2; ++normal)
            for (unsigned colors = 0; colors < 8; ++colors)
                for (unsigned uv = 0; uv < 16; ++uv)
                    for (unsigned sample = 0; sample < 16; ++sample) {
                        const Billboard p{{4,-2,val(special[sample])}, {2,3}, 0.1f};
                        const BillboardBasis basis{{1,0,0}, {0,-1,0}};
                        BillboardAttributes source;
                        source.normal = {val(special[sample]),0.25f,-0.75f};
                        for (auto& color : source.colors) for (auto& channel : color)
                            channel = static_cast<std::uint8_t>(random());
                        for (auto& point : source.uv) for (auto& lane : point)
                            lane = sample < 12 ? val(special[sample]) : moderate();
                        auto selected = source;
                        selected.normal_present = normal != 0;
                        selected.color_present = colors != 0;
                        selected.uv_present = uv != 0;
                        if (colors & 1) selected.colors.fill(source.colors[0]);
                        const bool rectangle = (uv & 1) || (!(uv & 2) && (uv & 4));
                        if (rectangle) {
                            const auto lower = source.uv[1], upper = source.uv[3];
                            selected.uv = {{{upper[0],lower[1]},lower,{lower[0],upper[1]},upper}};
                        }
                        if (uv & 1) selected.normal = p.center;
                        const u32 flags = normal*0x01000000u | colors*0x00010000u | uv*0x00100000u;
                        oracle->run(p, basis, &source, flags);
                        compare_vertices(p, basis, selected);
                        ++selection_cases;
                    }
        const BillboardBasis b{{1,0,0}, {0,-1,0}};
        std::array<Billboard,2> particles{{{{4,-2,1},{2,3},0}, {{-1,5,8},{3,2},0.2f}}};
        std::array<BillboardAttributes,2> attrs{};
        std::array<RenderVertex,8> vertices{};
        require(emit_billboards(particles.data(), attrs.data(), 2, b, authored, vertices.data(), vertices.size()), "two-particle batch rejected");
        for (std::size_t i = 0; i < particles.size(); ++i) {
            const auto expected = oracle->run(particles[i], b);
            for (std::size_t c = 0; c < 4; ++c) for (std::size_t axis = 0; axis < 3; ++axis)
                require(bits(vertices[i*4+c].position[axis]) == bits(expected[c][axis]), "batch order differs");
        }
        const auto saved = vertices;
        const auto rejected = [&](const Billboard* p, const BillboardAttributes* a, std::size_t n,
                                  RenderVertex* out, std::size_t capacity) {
            require(!emit_billboards(p, a, n, b, authored, out, capacity), "invalid batch accepted");
            require(std::memcmp(vertices.data(), saved.data(), sizeof(vertices)) == 0, "invalid batch partially wrote output");
        };
        rejected(nullptr, attrs.data(), 1, vertices.data(), 8);
        rejected(particles.data(), nullptr, 1, vertices.data(), 8);
        rejected(particles.data(), attrs.data(), 1, nullptr, 8);
        rejected(particles.data(), attrs.data(), 1, vertices.data(), 3);
        rejected(particles.data(), attrs.data(), std::numeric_limits<std::size_t>::max(), vertices.data(), 8);
        rejected(particles.data(), attrs.data(), 1,
                 reinterpret_cast<RenderVertex*>(std::numeric_limits<std::uintptr_t>::max()-3), 4);
        rejected(reinterpret_cast<const Billboard*>(vertices.data()), attrs.data(), 1, vertices.data(), 8);
        rejected(particles.data(), reinterpret_cast<const BillboardAttributes*>(vertices.data()), 1, vertices.data(), 8);
        // Aliases are rejected before reading any source or publishing any vertex.
        require(!emit_billboards(particles.data(), attrs.data(), 1, b, authored,
                reinterpret_cast<RenderVertex*>(const_cast<BillboardBasis*>(&b)), 4), "basis alias accepted");
        require(!emit_billboards(particles.data(), attrs.data(), 1, b, authored,
                reinterpret_cast<RenderVertex*>(const_cast<BillboardCurve*>(&authored)), 4), "curve alias accepted");
        require(emit_billboards(nullptr, nullptr, 0, b, authored, nullptr, 0), "empty batch requires storage");
        std::cout << "native_billboard_tests: " << cases << " original-body cases, " << cases*12
                  << " coordinates bit-identical; " << attribute_cases
                  << " complete attribute cases; " << selection_cases
                  << " original selection cases; complete 48-byte renderer vertices identical; "
                  << "batch/overflow/alias bounds passed; no shipping producer/context/publication proof\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
