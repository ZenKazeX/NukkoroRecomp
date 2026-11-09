// Indirect-call trace - OBSERVATION ONLY, for validating the static call graph and receiver typing of
// tools/dolrecon (callgraph.py, receivers.py) against what the original game really calls.
//
// SHADOW_INDIRECT_TRACE=<file>: every linked indirect call (bctrl and the generator's unproven linked calls in
// recompiled code, bcctrl in the interpreter) is recorded as (site = the call instruction, target), counted; the
// distinct pairs are written to <file> at exit as "site target count" lines (hex, hex, decimal). Unset: nothing is
// recorded (one cached flag test per call). It changes no guest state and no timing the guest can observe beyond
// host time.
#pragma once

#include <cstdint>

namespace gekko::indirect_call_trace {

namespace detail {
// Set once, before main (SHADOW_INDIRECT_TRACE is read during static initialisation); never written again.
extern const bool g_enabled;
}  // namespace detail

// A plain load: called on every linked indirect call, so it must not be an out-of-line call with a guarded static.
inline bool enabled() noexcept { return detail::g_enabled; }
void record(std::uint32_t site, std::uint32_t target);

}  // namespace gekko::indirect_call_trace
