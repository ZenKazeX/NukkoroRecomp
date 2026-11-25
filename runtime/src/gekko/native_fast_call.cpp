// The fast owners' call out (gekko/native_fast_policy.h): compiled once per runtime build, like native_guest_call.cpp
// (production: the translated code; reference build: the interpreter, nested).
#include "gekko/native_fast_policy.h"
namespace gekko::fast_policy {

bool call_out(Cpu& c, u32 target, u32 link) {
    const u32 saved_pc = c.pc, saved_npc = c.npc;
    bool returned = false;
#if defined(SHADOW_CPU_REFERENCE_BUILD)
    c.pc = target;
    c.npc = target + 4u;
    u64 steps = 0;
    while (c.pc != link && !c.exiting()) {
        if (steps++ >= kGuestCallStepBound) {
            c.request_stop(StopReason::ExecutionFault, "fast owner: a call out did not return within the step bound");
            return false;
        }
        c.run_steps(1);
    }
    returned = c.pc == link && !c.exiting();
#else
    const RecompiledCode* table = c.recompiled();
    const RecompiledCode::Fn native = table != nullptr ? table->lookup(target) : nullptr;
    if (native == nullptr) {
        c.request_stop(StopReason::ExecutionFault, "fast owner: no translated code for a call out");
        return false;
    }
    c.rearm_native_entry();
    c.enter_guest_call();
    native(c);
    if (c.exiting()) c.finish_transfers();
    while (!c.exiting() && c.pc != link) {  // call forms that return through pc (see guest_call)
        const RecompiledCode::Fn next = table->lookup(c.pc);
        if (next == nullptr) break;
        c.rearm_native_entry();
        next(c);
        if (c.exiting()) c.finish_transfers();
    }
    c.leave_guest_call();
    returned = !c.exiting() && c.pc == link;
#endif
    if (!returned) {
        if (!c.stopped()) c.request_stop(StopReason::ExecutionFault, "fast owner: a call out did not return to its link");
        return false;
    }
    c.pc = saved_pc;
    c.npc = saved_npc;
    return true;
}

}  // namespace gekko::fast_policy
