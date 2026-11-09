// SCAFFOLDING (R2): the Im2D line and triangle emitters (im2d_prims.h) - deleted by R6
#include "im2d_prims.h"

#include "gekko/cpu.h"

#include <algorithm>

namespace gekko::scaffold {

namespace {
u32 draw(Cpu& cpu, Im2dIndexedSink sink, u32 type, const u32* index, u32 index_count) {
    const u32 vertices = cpu.gpr[3];
    u32 count = cpu.gpr[4];
    // The indices go below the stack pointer: the original's own callees used that space.
    const u32 scratch = (cpu.gpr[1] - 0x40u) & ~3u;
    for (u32 i = 0; i < index_count; ++i) {
        cpu.mem_write16(scratch + 2u * i, static_cast<u16>(index[i]));
        count = std::max(count, (index[i] & 0xFFFFu) + 1u);
    }
    (void)sink(cpu, type, vertices, count, scratch, index_count);
    cpu.gpr[3] = 1u;  // every Im2D entry returns 1
    return 1u;
}
}  // namespace

void serve_im2d_line(Cpu& cpu, Im2dIndexedSink sink) {
    const u32 index[2] = {cpu.gpr[5], cpu.gpr[6]};
    draw(cpu, sink, kRwLineList, index, 2u);
}

void serve_im2d_triangle(Cpu& cpu, Im2dIndexedSink sink) {
    const u32 index[3] = {cpu.gpr[5], cpu.gpr[6], cpu.gpr[7]};
    draw(cpu, sink, kRwTriList, index, 3u);
}

}  // namespace gekko::scaffold
