#pragma once

// A lowered call site, as the generator emitted it (docs/lift-20261006/LIFT_PROGRAMME.md section 2.1).
//
// extract/lift-sites.txt lists the calls of translated code into lifted subsystems. The generator replaced
// each with a generated function that reads the caller's arguments, calls the native subsystem with values
// and writes the results where the caller expects them (recomp::kLiftSites, sorted by address). Translated
// code calls that function at the site; the dispatcher and the reference interpreter call the same function
// when they reach the site (Cpu::find_lift_site), so both engines agree and the native call runs once.
// This is scaffolding of the translated side: a site disappears when its caller is lifted.

#include "gekko/types.h"

namespace gekko {

class Cpu;

// Sequential sites continue after the call. Prepared sites publish their own
// PC (return, exception entry, or stopped fault). Policy is known at generation
// time: storing it here keeps ordinary lowerings eligible for tail calls.
enum class LiftSiteContinuation : u32 { Sequential, Prepared };

struct LiftSiteRecord {
    using Lower = void (*)(Cpu&);
    u32 address;           // the call instruction
    LiftSiteContinuation continuation;
    Lower lower;          // publishes the register/value results
};

}  // namespace gekko
