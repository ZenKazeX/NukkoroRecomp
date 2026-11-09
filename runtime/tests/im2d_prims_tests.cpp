// liveness-roots: reference-interpreter-only
// Contract for the Im2D line / triangle emitters 0x80491D0C / 0x80491B08 (scaffold/rw_driver/im2d_prims.h).
// Two kinds of evidence, stated as such:
//  1. The ORIGINAL's decoded semantics, read from the pinned DOL: vertex k is `vertices + k * 0x18`; the line emits
//     vertex r5 then r6 as GX_LINES (0xA8, 2 vertices), the triangle r5, r6, r7 as GX_TRIANGLES (0x90, 3 vertices); both
//     validate nothing and return 1. The words below are those instructions; any other order or constant fails here.
//  2. The owner's behaviour on a recording sink standing for GcnIm2DRenderIndexedPrimitive: the primitive type
//     (RenderWare 1 = line list, 3 = triangle list), the vertex array, the vertex count handed over (max(numVertices,
//     highest index + 1)), the indices read back from guest memory at the call (v1, v2 [, v3] in that order) and r3 == 1.
// The pixels are the immediate path's own (rwim_tests and the Westopolis replay cover it): this test does not draw.
// usage: im2d_prims_tests <sys/main.dol>
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "rw_driver/im2d_prims.h"
#include "rwim/immediate.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace gekko;
using namespace gekko::scaffold;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct Call {
    u32 type = 0, vertices = 0, count = 0, index_count = 0;
    std::vector<u32> indices;
    unsigned calls = 0;
};
Call g_call;

u32 recording_sink(Cpu& cpu, u32 type, u32 vertices, u32 count, u32 indices, u32 index_count) {
    g_call = Call{};
    g_call.type = type;
    g_call.vertices = vertices;
    g_call.count = count;
    g_call.index_count = index_count;
    for (u32 i = 0; i < index_count; ++i) g_call.indices.push_back(cpu.mem_read16(indices + 2u * i));
    ++g_call.calls;
    return 0xDEADBEEFu;  // the owner must return 1 whatever the sink says
}
}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: im2d_prims_tests main.dol");
        std::ifstream input(argv[1], std::ios::binary);
        const std::vector<u8> dol{std::istreambuf_iterator<char>(input), {}};
        require(dol.size() >= 256, "missing DOL");
        u64 hash = 0xCBF29CE484222325ull;
        for (u8 byte : dol) hash = (hash ^ byte) * 0x100000001B3ull;
        require(hash == 0xD2CFB4A1CA897F9Bull, "unexpected original DOL identity");
        GuestMemory memory;
        Cpu cpu{memory};
        const auto word = [&](std::size_t o) {
            return u32{dol[o]} << 24 | u32{dol[o + 1]} << 16 | u32{dol[o + 2]} << 8 | u32{dol[o + 3]};
        };
        for (u32 i = 0; i < 18u; ++i) {
            const u32 offset = word(i * 4u), address = word(0x48u + i * 4u), size = word(0x90u + i * 4u);
            if (size != 0u) memory.write_block(address, dol.data() + offset, size);
        }
        // 1. the original's decoded semantics
        const struct { u32 address, expect; const char* what; } decoded[] = {
            {0x80491D14, 0x1C850018u, "line: r4 = r5 * 0x18"},
            {0x80491D20, 0x1C060018u, "line: r0 = r6 * 0x18"},
            {0x80491D28, 0x7FC32214u, "line: r30 = vertices + v1"},
            {0x80491D2C, 0x7FE30214u, "line: r31 = vertices + v2"},
            {0x80491D34, 0x386000A8u, "line: GX_LINES"},
            {0x80491D3C, 0x38A00002u, "line: 2 vertices"},
            {0x80491D60, 0xC01E0000u, "line: first vertex stored is v1 (r30)"},
            {0x80491DA8, 0xC01F0000u, "line: second vertex stored is v2 (r31)"},
            {0x80491E68, 0x38600001u, "line: returns 1"},
            {0x80491B10, 0x1C860018u, "triangle: r4 = r6 * 0x18"},
            {0x80491B1C, 0x1C050018u, "triangle: r0 = r5 * 0x18"},
            {0x80491B20, 0x7FE32214u, "triangle: r31 = vertices + v2"},
            {0x80491B2C, 0x7FA30214u, "triangle: r29 = vertices + v1"},
            {0x80491B30, 0x1C070018u, "triangle: r0 = r7 * 0x18"},
            {0x80491B34, 0x7FC30214u, "triangle: r30 = vertices + v3"},
            {0x80491B3C, 0x38600090u, "triangle: GX_TRIANGLES"},
            {0x80491B44, 0x38A00003u, "triangle: 3 vertices"},
            {0x80491B68, 0xC01D0000u, "triangle: first vertex stored is v1 (r29)"},
            {0x80491BB0, 0xC01F0000u, "triangle: second vertex stored is v2 (r31)"},
            {0x80491BF8, 0xC01E0000u, "triangle: third vertex stored is v3 (r30)"},
        };
        for (const auto& d : decoded)
            require(memory.read32(d.address) == d.expect, std::string("original differs from the decoded rule: ") + d.what);

        // 2. the owner on a recording sink
        unsigned runs = 0;
        for (u32 vertices : {0x80700000u, 0x80712340u})
            for (u32 count : {0u, 1u, 2u, 3u, 7u, 100u})
                for (u32 a : {0u, 1u, 2u, 6u})
                    for (u32 b : {0u, 1u, 3u, 9u})
                        for (u32 c3 : {0u, 2u, 8u, 150u}) {
                            for (bool triangle : {false, true}) {
                                cpu.reset();
                                cpu.gpr[1] = 0x817E0000u;
                                cpu.gpr[3] = vertices;
                                cpu.gpr[4] = count;
                                cpu.gpr[5] = a;
                                cpu.gpr[6] = b;
                                cpu.gpr[7] = c3;
                                g_call = Call{};
                                if (triangle) serve_im2d_triangle(cpu, recording_sink);
                                else serve_im2d_line(cpu, recording_sink);
                                require(g_call.calls == 1u, "exactly one draw per emitter call");
                                require(cpu.gpr[3] == 1u, "the emitter returns 1");
                                require(g_call.type == (triangle ? kRwTriList : kRwLineList), "primitive type");
                                require(g_call.vertices == vertices, "vertex array");
                                const std::vector<u32> want = triangle ? std::vector<u32>{a, b, c3} : std::vector<u32>{a, b};
                                require(g_call.index_count == want.size() && g_call.indices == want, "index order");
                                u32 highest = 0;
                                for (u32 v : want) highest = v > highest ? v : highest;
                                const u32 want_count = count > highest + 1u ? count : highest + 1u;
                                require(g_call.count == want_count, "vertex count handed over");
                                // The geometry the immediate path draws from these arguments (rwim: the same topology and
                                // index resolution the native renderer's Im2D draw uses) is the original's FIFO order: the
                                // primitive is a line list / triangle list and its vertices are, in order, the array
                                // entries v1, v2 [, v3] - vertex k at vertices + k * 0x18.
                                shadow::render::Topology topology = shadow::render::Topology::Triangles;
                                bool fan = true;
                                require(shadow::rwim::topology_of(g_call.type, &topology, &fan) && !fan, "type has no topology");
                                require(topology == (triangle ? shadow::render::Topology::Triangles : shadow::render::Topology::Lines),
                                        "native topology differs from the GX primitive");
                                std::vector<std::uint16_t> host;
                                for (u32 v : g_call.indices) host.push_back(static_cast<std::uint16_t>(v));
                                std::vector<std::uint32_t> order;
                                require(shadow::rwim::list_order(host.data(), g_call.index_count, g_call.count, fan, &order),
                                        "the immediate path refuses the indices");
                                require(order.size() == want.size(), "drawn vertex count");
                                for (std::size_t k = 0; k < order.size(); ++k)
                                    require(g_call.vertices + order[k] * kIm2DVertexBytes == vertices + want[k] * kIm2DVertexBytes,
                                            "drawn vertex address k=" + std::to_string(k));
                                ++runs;
                            }
                        }
        std::printf("PASS im2d prims: %zu original instructions match the decoded rule; %u emitter calls: line list / triangle "
                    "list, vertex array, count max(numVertices, highest index + 1), indices v1 v2 [v3] in order, r3 = 1\n",
                    sizeof(decoded) / sizeof(decoded[0]), runs);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL im2d prims: %s\n", error.what());
        return 1;
    }
}
