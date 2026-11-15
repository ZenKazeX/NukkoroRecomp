#include "gekko/native_vi_wait.h"

#include <string>

#include "gekko/cpu.h"

namespace gekko::vi_wait {
namespace {

constexpr u32 kOSDisableInterrupts = 0x8037611Cu, kOSRestoreInterrupts = 0x80376144u, kOSSleepThread = 0x803792C4u;
constexpr u32 kSdaRetraceCount = 0x5BCCu, kSdaRetraceQueue = 0x5BD4u;
constexpr u32 kFrame = 16u;

bool done(Cpu& c) { return c.stopped() || c.stop_requested(); }

// 0x8037ED10: lwz r0,0x14(r1) ; lwz r31,12(r1) ; lwz r30,8(r1) ; addi r1,r1,16 ; mtlr r0 ; blr
void epilogue(Cpu& c) {
    const u32 sp = c.gpr[1];
    c.gpr[0] = c.mem_read32(sp + kFrame + 4u);
    c.gpr[31] = c.mem_read32(sp + 12u);
    c.gpr[30] = c.mem_read32(sp + 8u);
    c.gpr[1] = sp + kFrame;
    c.lr = c.gpr[0];
}

bool sleep_handed_back(Cpu& c) {
    c.request_stop(StopReason::ExecutionFault,
                   "VIWaitForRetrace: OSSleepThread handed control back to the dispatcher (the native wait serves callers with interrupts disabled)");
    return true;
}

// 0x8037ECFC to the end: OSSleepThread has returned; r30 is the retrace count read at 0x8037ECEC, r31 the restore argument.
// Also the row a run resumes through when a stop ended the sleep: the sleep hands back at its own entry with LR here
// (os_threads::serve_sleep_thread), so the wait goes on with the frame, r30 and r31 it had (RC-kernel-1).
void after_sleep(Cpu& c) {
    for (;;) {
        c.gpr[0] = c.mem_read32(c.gpr[13] + kSdaRetraceCount);
        const u32 a = c.gpr[30], b = c.gpr[0];
        c.set_cr_field(0, (a < b ? 8u : a > b ? 4u : 2u) | ((c.xer & kXerSO) != 0u ? 1u : 0u));   // cmplw r30,r0 (0x7C1E0040: xo 32)
        if (a != b) break;
        c.gpr[3] = c.gpr[13] + kSdaRetraceQueue;
        c.lr = kAfterSleep;
        if (!c.serve_owner(kOSSleepThread) || done(c)) return;   // a stop: the sleep left a resumable entry (pc, r3, LR)
        if (c.exiting()) { (void)sleep_handed_back(c); return; }
    }
    c.gpr[3] = c.gpr[31];
    c.lr = kAfterRestore;
    if (!c.serve_owner(kOSRestoreInterrupts) || done(c)) return;
    if (c.exiting()) return;   // an interrupt the restore made deliverable is taken first; 0x8037ED10 runs after it
    epilogue(c);
}

// 0x8037ECEC to the first sleep: r3 is OSDisableInterrupts's result, r1 the frame.
void after_disable(Cpu& c) {
    c.gpr[30] = c.mem_read32(c.gpr[13] + kSdaRetraceCount);
    c.gpr[31] = c.gpr[3];
    c.gpr[3] = c.gpr[13] + kSdaRetraceQueue;
    c.lr = kAfterSleep;
    if (!c.serve_owner(kOSSleepThread) || done(c)) return;   // a stop: as in after_sleep
    if (c.exiting()) { (void)sleep_handed_back(c); return; }
    after_sleep(c);
}

bool wait_for_retrace(Cpu& c) {
    const u32 sp0 = c.gpr[1], sp = sp0 - kFrame;
    c.gpr[0] = c.lr;
    c.mem_write32(sp0 + 4u, c.lr);
    c.mem_write32(sp, sp0);
    c.mem_write32(sp + 12u, c.gpr[31]);
    c.mem_write32(sp + 8u, c.gpr[30]);
    c.gpr[1] = sp;
    c.lr = kAfterDisable;
    if (!c.serve_owner(kOSDisableInterrupts) || done(c)) return true;
    if (c.exiting()) return true;   // taken at the gate's entry; the gate runs again and returns to 0x8037ECEC
    after_disable(c);
    return true;
}

}  // namespace

void install(Cpu& cpu) {
    cpu.install_hle(kWaitForRetrace, [](Cpu& c) { return wait_for_retrace(c); }, "VIWaitForRetrace (native)");
    cpu.install_hle(kAfterDisable, [](Cpu& c) { after_disable(c); return true; }, "VIWaitForRetrace after OSDisableInterrupts");
    cpu.install_hle(kAfterSleep, [](Cpu& c) { after_sleep(c); return true; }, "VIWaitForRetrace after OSSleepThread");
    cpu.install_hle(kAfterRestore, [](Cpu& c) { epilogue(c); return true; }, "VIWaitForRetrace after OSRestoreInterrupts");
}

}  // namespace gekko::vi_wait
