// The native owners of the AX MIX table's heap wrappers (gekko/native_ax_mix_alloc.h).

#include "gekko/native_ax_mix_alloc.h"

#include "gekko/cpu.h"
#include "gekko/native_os_heap.h"

namespace gekko::ax_mix_alloc {

namespace {
// mr r4, r3; lwz r3, 0x26E0(r13): the argument moves to r4, the current heap id becomes r3.
void arrange(Cpu& cpu) {
    cpu.gpr[4] = cpu.gpr[3];
    cpu.gpr[3] = cpu.mem_read32(cpu.gpr[13] + os_heap::kCurrentHeap);
}
}  // namespace

bool serve_alloc(Cpu& cpu) {
    arrange(cpu);
    return os_heap::serve_alloc_from_heap(cpu);
}

bool serve_free(Cpu& cpu) {
    arrange(cpu);
    return os_heap::serve_free_to_heap(cpu);
}

void install(Cpu& cpu) {
    cpu.install_hle(kMixAlloc, [](Cpu& c) {
        if (!serve_alloc(c))
            c.request_stop(StopReason::ExecutionFault, "AX MIX alloc: a heap list walk does not end (cyclic list)");
        return true;
    }, "AX MIX alloc wrapper (native)");
    cpu.install_hle(kMixFree, [](Cpu& c) {
        if (!serve_free(c))
            c.request_stop(StopReason::ExecutionFault, "AX MIX free: a heap list walk does not end (cyclic list)");
        return true;
    }, "AX MIX free wrapper (native)");
}

}  // namespace gekko::ax_mix_alloc
