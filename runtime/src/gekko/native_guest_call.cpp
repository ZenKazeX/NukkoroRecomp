// guest_call (native_guest_call.h).
#include "gekko/native_guest_call.h"

#include "gekko/recomp.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace gekko {

bool guest_call(Cpu& c, u32 target, u32 link, std::initializer_list<u32> args, u32& result,
                u64 step_bound) {
    if (args.size() > 8) {
        c.request_stop(StopReason::ExecutionFault, "guest_call: more than eight register arguments");
        return false;
    }
    // Only what the return restores is saved: r1, r2, r13 and the non-volatile r14-r31 (the volatile r0, r3-r12 are the callee's
    // to clobber and the caller's to have spilled; r3 is the result).
    const u32 saved_r1 = c.gpr[1], saved_r2 = c.gpr[2], saved_r13 = c.gpr[13];
    u32 saved_nonvolatile[18];
    std::memcpy(saved_nonvolatile, &c.gpr[14], sizeof(saved_nonvolatile));
    const u32 saved_lr = c.lr, saved_ctr = c.ctr, saved_cr = c.cr, saved_pc = c.pc, saved_npc = c.npc;

    u32 arg_register = 3;
    for (u32 a : args) c.gpr[arg_register++] = a;
    // A frame for the callee to save its link register into (r1 + 4 of the frame it was called from); 16 bytes keep the
    // stack aligned and the back chain a valid word.
    const u32 frame = (saved_r1 - 0x10u) & ~0xFu;
    c.mem_write32(frame, saved_r1);
    c.gpr[1] = frame;
    c.gpr[12] = target;
    c.ctr = target;
    c.lr = link;

    bool returned = false;
#if defined(SHADOW_CPU_REFERENCE_BUILD)
    c.pc = target;
    c.npc = target + 4u;
    u64 steps = 0;
    while (c.pc != link && !c.exiting()) {  // exiting(): stopped, or the callee left to the dispatcher (a yield)
        if (steps++ >= step_bound) {
            c.request_stop(StopReason::ExecutionFault, "guest_call: the nested call to " + std::to_string(target) +
                                                           " did not return within the step bound");
            return false;
        }
        c.run_steps(1);
    }
    returned = c.pc == link && !c.exiting();
#else
    (void)step_bound;
    const RecompiledCode* table = c.recompiled();
    const RecompiledCode::Fn native = table != nullptr ? table->lookup(target) : nullptr;
    if (native == nullptr) {
        c.request_stop(StopReason::ExecutionFault, "guest_call: no translated code for the nested call target");
        return false;
    }
    c.rearm_native_entry();
    c.enter_guest_call();
    native(c);
    if (c.exiting()) c.finish_transfers();
    // The generated code returns to its caller with pc at the target for some call forms (mtlr + blrl, tail jumps: "a callee
    // returned somewhere other than the link"): the dispatcher continues natively from pc. So does this, until the callee has
    // returned to `link`; pc without translated code, a stop or an exit that needs the dispatcher is the failure.
    while (!c.exiting() && c.pc != link) {
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
        if (!c.stopped())
            std::fprintf(stderr,
                         "[guest_call] 0x%08X did not return to 0x%08X: pc 0x%08X lr 0x%08X exit word 0x%02X stop requested %d "
                         "stopped %d needs_dispatch %d\n",
                         target, link, c.pc, c.lr, c.exit_state(), c.stop_requested() ? 1 : 0, c.stopped() ? 1 : 0,
                         c.needs_dispatch() ? 1 : 0);
        if (!c.stopped())
            c.request_stop(StopReason::ExecutionFault, "guest_call: a nested guest call did not return to its link");
        return false;
    }
    result = c.gpr[3];
    std::memcpy(&c.gpr[14], saved_nonvolatile, sizeof(saved_nonvolatile));
    c.gpr[1] = saved_r1;
    c.gpr[2] = saved_r2;
    c.gpr[13] = saved_r13;
    c.lr = saved_lr;
    c.ctr = saved_ctr;
    c.cr = saved_cr;
    c.pc = saved_pc;
    c.npc = saved_npc;
    return true;
}

void guest_continue(Cpu& c, u32 entry, u32 return_pc, u32 return_sp) {
#if defined(SHADOW_CPU_REFERENCE_BUILD)
    c.pc = entry;
    c.npc = entry + 4u;
    u64 steps = 0;
    while (!(c.pc == return_pc && c.gpr[1] == return_sp) && !c.exiting() && !c.stopped()) {
        if (steps++ >= kGuestCallStepBound) {
            c.request_stop(StopReason::ExecutionFault, "guest_continue: the function did not return within the step bound");
            return;
        }
        c.run_steps(1);
    }
    if ((c.exiting() || c.stopped()) && !c.stopped())
        c.request_stop(StopReason::ExecutionFault, "guest_continue: the continued function left to the dispatcher in the reference build");
#else
    (void)return_pc;
    (void)return_sp;
    const RecompiledCode* table = c.recompiled();
    const RecompiledCode::Range* range = table != nullptr ? table->find_range(entry) : nullptr;
    if (range == nullptr) {
        c.request_stop(StopReason::ExecutionFault, "guest_continue: no translated code for the entry");
        return;
    }
    c.pc = entry;
    c.npc = entry + 4u;
    range->at(c, entry);
#endif
}

}  // namespace gekko
