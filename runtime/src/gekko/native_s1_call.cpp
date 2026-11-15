// A call from native code to a guest function as the translated code makes it (no frame is built, nothing is saved):
// the caller has set LR to its return address and the argument registers. Compiled into both runtime variants.
#include "gekko/native_s1_chars.h"

#include "gekko/recomp.h"

namespace gekko::s1_chars {



bool call_guest(Cpu& c, u32 target) {
#if defined(SHADOW_CPU_REFERENCE_BUILD)
    const u32 link = c.lr;
    c.pc = target;
    c.npc = target + 4u;
    u64 steps = 0;
    while (c.pc != link && !c.exiting()) {
        if (steps++ >= 4000000ull) {
            c.request_stop(StopReason::ExecutionFault, "s1 chars: a nested call did not return");
            return false;
        }
        c.run_steps(1);
    }
    return true;
#else
    const RecompiledCode* table = c.recompiled();
    const RecompiledCode::Fn native = table != nullptr ? table->lookup(target) : nullptr;
    if (native == nullptr) {
        c.request_stop(StopReason::ExecutionFault, "s1 chars: no translated code for a callee");
        return false;
    }
    native(c);
    return true;
#endif
}

}  // namespace gekko::s1_chars
