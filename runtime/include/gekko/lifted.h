#pragma once

// The lifted subsystems the translated game still calls (docs/lift-20261006/LIFT_PROGRAMME.md).
//
// Each member is native code that knows nothing of the translated machine (runtime/lift, checked by
// tools/lift/check_purity.py). The generated call-site lowerings reach them through Cpu::lifted(); the host
// feeds and observes them directly. The dependency points one way: scaffolding -> native subsystem.

#include "peripheral/manager_imp_pc.h"

namespace gekko {

struct Lifted {
    // L1: the PC implementation of the engine's peripheral interface (docs/lift-20261006/L1_INPUT.md).
    shadow::peripheral::ManagerImpPc peripheral;
};

}  // namespace gekko
