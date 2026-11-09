#pragma once

#include "plume_fence_retirement.h"
#include "gekko/native_failure_publication.h"
#include <utility>

namespace gekko::render {

// A terminal presentation fault belongs to the native owner even if its text
// cannot allocate. Preserve the first typed outcome; diagnostics are best effort.
class PresentationFailure {
public:
    bool failed() const noexcept { return failed_; }
    plume::RenderFenceWait outcome() const noexcept { return outcome_; }
    template<class Diagnose>
    void publish(plume::RenderFenceWait outcome, Diagnose&& diagnose) {
        if (outcome == plume::RenderFenceWait::SIGNALED || failed_) return;
        native::publishFailureBeforeDiagnostics([&] {
            outcome_ = outcome;
            failed_ = true;
        }, [&] { std::forward<Diagnose>(diagnose)(outcome_); });
    }
private:
    bool failed_ = false;
    plume::RenderFenceWait outcome_ = plume::RenderFenceWait::SIGNALED;
};

// The presentation queue owns both its slot resources and the source leases
// until actual fence completion. An event wake, error, or timeout is no release.
template<class Release>
bool observePresentationRetirement(bool& in_flight, plume::RenderFenceWait outcome,
                                  Release&& release) {
    if (!in_flight) return true;
    if (outcome != plume::RenderFenceWait::SIGNALED) return false;
    std::forward<Release>(release)();
    in_flight = false;
    return true;
}

inline bool canDisposePresentation(bool in_flight, plume::RenderFenceWait outcome) noexcept {
    return !in_flight || outcome == plume::RenderFenceWait::DEVICE_LOST;
}

template<class Release>
void disposeRemovedPresentation(bool& in_flight, plume::RenderFenceWait outcome, Release&& release) {
    if (!canDisposePresentation(in_flight, outcome))
        throw plume::FenceRetirementError(outcome, "native presentation teardown");
    std::forward<Release>(release)();
    in_flight = false;
}

} // namespace gekko::render
