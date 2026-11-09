// The reference build's way into the guest scheduler, for the modes that create guest threads (SHADOW_NATIVE_CRI=0 and the
// three loader switches =0, docs/agents/cri-threads.md "Cut 2b"). Not compiled into the lifted (shipping) build
// (SHADOW_LIFTED_UNCOMPILED_SOURCES): there the delivery's end stops loudly instead, and the generator does not keep
// SelectThread / OSLoadContext alive for it.

#include "gekko/native_guest_scheduler.h"

#include "gekko/cpu.h"
#include "gekko/native_cri.h"
#include "gekko/native_interrupts.h"

namespace gekko::guest_scheduler {
namespace {
constexpr u32 kSelectThread = 0x803787B0u;   // SelectThread(yield)
constexpr u32 kOSLoadContext = 0x80372C68u;  // OSLoadContext(context)
constexpr u32 kRescheduleReturn = 0x80376214u;  // native interrupt: reschedule return
}  // namespace

unsigned fp_unavailable_handler() { return 0x80373100u; }

void install_cri(NativeCri& cri) {
    cri.set_idle_wait_loop(0x803788FCu);  // SelectThread's idle loop head (extract/wait-points.txt)
}

void install(NativeInterrupts& interrupts) {
    interrupts.set_guest_scheduler(
        [](Cpu& c, bool yield) {
            c.gpr[3] = yield ? 1u : 0u;
            c.lr = kRescheduleReturn;
            c.leave_to_dispatcher(kSelectThread, FallbackReason::Yield, kRescheduleReturn);
        },
        [](Cpu& c, u32 context) {
            c.gpr[3] = context;
            c.leave_to_dispatcher(kOSLoadContext, FallbackReason::Yield, kRescheduleReturn);
        });
}

}  // namespace gekko::guest_scheduler
