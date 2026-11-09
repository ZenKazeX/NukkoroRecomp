// The reference build's entry into the guest scheduler (native_guest_scheduler.cpp); absent from the lifted build.
#pragma once

namespace gekko {
class NativeCri;
class NativeInterrupts;
}

namespace gekko::guest_scheduler {

// Gives the native interrupt delivery the guest SelectThread / OSLoadContext for a delivery end that needs them (a second
// guest thread exists). Without it the delivery stops loudly.
void install(NativeInterrupts& interrupts);
// The native CRI recognises SelectThread's idle spin (its idle work hook) only where the guest scheduler exists.
void install_cri(NativeCri& cri);
// The OS handler of exception 7 (OSSwitchFPUContext), for the guest-thread configuration's lazy FPU switch.
unsigned fp_unavailable_handler();

}  // namespace gekko::guest_scheduler
