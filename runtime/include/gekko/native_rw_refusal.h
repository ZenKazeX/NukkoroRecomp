#pragma once

// What the RenderWare math owners (native_rw_matrix, native_rw_vector, native_rw_view_cache) do with a call outside their contract:
// the FPU switched off, GQR0 not the float format the originals' psq loads assume, or a matrix operation outside 0..2.
//
// In the single-thread configuration (os_threads::single_thread_configuration(): the lifted build's only one) none of these can
// happen - MSR[FP] is never cleared (cut 3, docs/agents/cri-threads.md) and no emitted instruction or runtime write sets GQR0
// (docs/agents/native-boot.md Step 12) - so reaching one is a loud stop naming the owner, and the call counts as served; the
// lifted build lists these rows as native owners that never decline (extract/native-owners-r2.txt). In the guest-thread
// configuration (the reference build, where the OS's lazy FPU switch clears MSR[FP]) the owner declines and the guest body runs.

#include <string>

#include "gekko/cpu.h"
#include "gekko/native_os_threads.h"

namespace gekko::rw_refusal {

// Returns whether the call was served: true (a stop requested) in the single-thread configuration, false (decline) otherwise.
inline bool refuse(Cpu& c, const char* owner, const char* why) {
    if (!os_threads::single_thread_configuration()) return false;
    c.request_stop(StopReason::ExecutionFault, std::string(owner) + ": " + why + " (the owner's contract excludes it)");
    return true;
}

}  // namespace gekko::rw_refusal
