#pragma once

#include <utility>

namespace gekko::native {

// The owning fault/status is already retained without allocating. Wake blocked
// peers before best-effort text; an out-of-memory diagnostic cannot hide failure.
template<class Publish, class Diagnose>
void publishFailureBeforeDiagnostics(Publish&& publish, Diagnose&& diagnose) {
    std::forward<Publish>(publish)();
    try { std::forward<Diagnose>(diagnose)(); }
    catch (...) {} // Diagnostic failure never replaces the actual owning fault.
}

} // namespace gekko::native
