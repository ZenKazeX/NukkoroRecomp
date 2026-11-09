// Shared shipping presentation policy: failed retirement retains source leases.
#include "gekko/render/presentation_retirement.h"

#include <array>
#include <cstdio>
#include <limits>
#include <new>

namespace {
int failures = 0;
void check(bool pass, const char* text, int line) {
    if (!pass) { ++failures; std::printf("FAIL line %d: %s\n", line, text); }
}
#define CHECK(x) check(bool(x), #x, __LINE__)
using plume::RenderFenceWait;
using namespace gekko::render;

struct Flight {
    bool in_flight = true;
    unsigned leases = 2; // current and previous output can both be GPU sources
    unsigned releases = 0;
    void release() { releases += leases; leases = 0; }
};

void failed_wait(RenderFenceWait outcome) {
    Flight flight;
    unsigned maps_or_resets = 0;
    const auto result = plume::waitForRetirement([&](std::uint64_t) { return outcome; });
    if (observePresentationRetirement(flight.in_flight, result.outcome,
                                     [&] { flight.release(); })) ++maps_or_resets;
    CHECK(flight.in_flight);
    CHECK(flight.leases == 2);
    CHECK(flight.releases == 0);
    CHECK(maps_or_resets == 0);
    CHECK(!canDisposePresentation(flight.in_flight, outcome));
    bool rejected = false;
    try {
        disposeRemovedPresentation(flight.in_flight, outcome, [&] { flight.release(); });
    } catch (const plume::FenceRetirementError& error) {
        CHECK(error.outcome() == outcome);
        rejected = true;
    }
    CHECK(rejected);
    CHECK(flight.in_flight && flight.leases == 2 && flight.releases == 0);
}
}

int main() {
    for (const auto first : {RenderFenceWait::TIMEOUT, RenderFenceWait::WAIT_ERROR,
                             RenderFenceWait::DEVICE_LOST}) {
        bool aborted = false;
        bool exact_typed_fault = false;
        try {
            const plume::FenceRetirementError error(first, "native GRAPHICS slot");
            gekko::native::publishFailureBeforeDiagnostics([&] { aborted = true; }, [&] {
                CHECK(aborted);
                throw std::bad_alloc();
            });
            throw error;
        } catch (const plume::FenceRetirementError& error) {
            exact_typed_fault = error.outcome() == first;
        } catch (...) {} // An allocation/other fault replacing the typed outcome fails below.
        CHECK(aborted && exact_typed_fault);
        PresentationFailure failure;
        Flight flight;
        unsigned diagnoses = 0;
        CHECK(!failure.failed() && failure.outcome() == RenderFenceWait::SIGNALED);
        failure.publish(first, [&](RenderFenceWait observed) {
            ++diagnoses;
            CHECK(failure.failed() && failure.outcome() == first && observed == first);
            CHECK(flight.in_flight && flight.leases == 2 && flight.releases == 0);
            throw std::bad_alloc();
        });
        CHECK(failure.failed() && failure.outcome() == first && diagnoses == 1);
        // A different later failure cannot replace the exact first fault or retry diagnostics.
        const auto later = first == RenderFenceWait::DEVICE_LOST ? RenderFenceWait::WAIT_ERROR :
                                                                  RenderFenceWait::DEVICE_LOST;
        failure.publish(later, [&](RenderFenceWait) { ++diagnoses; });
        CHECK(failure.outcome() == first && diagnoses == 1);
        CHECK(flight.in_flight && flight.leases == 2 && flight.releases == 0);
        // HostWindow uses this same publication primitive before its best-effort snapshot.
        bool host_failed = false;
        gekko::native::publishFailureBeforeDiagnostics([&] { host_failed = failure.failed(); }, [&] {
            CHECK(host_failed);
            throw std::bad_alloc();
        });
        CHECK(host_failed);
    }
    {
        PresentationFailure healthy;
        unsigned diagnoses = 0;
        healthy.publish(RenderFenceWait::SIGNALED, [&](RenderFenceWait) { ++diagnoses; });
        CHECK(!healthy.failed() && healthy.outcome() == RenderFenceWait::SIGNALED && diagnoses == 0);
    }
    failed_wait(RenderFenceWait::TIMEOUT);
    failed_wait(RenderFenceWait::WAIT_ERROR);

    Flight completed;
    CHECK(observePresentationRetirement(completed.in_flight, RenderFenceWait::SIGNALED,
                                       [&] { completed.release(); }));
    CHECK(!completed.in_flight && completed.leases == 0 && completed.releases == 2);
    CHECK(observePresentationRetirement(completed.in_flight, RenderFenceWait::SIGNALED,
                                       [&] { completed.release(); }));
    CHECK(completed.releases == 2); // repeated polls must never release twice
    CHECK(canDisposePresentation(completed.in_flight, RenderFenceWait::TIMEOUT));

    Flight removed;
    const auto sentinel = (std::numeric_limits<std::uint64_t>::max)();
    const auto loss = plume::classifyD3D12Fence(sentinel, 3, false);
    CHECK(loss == RenderFenceWait::DEVICE_LOST);
    CHECK(!observePresentationRetirement(removed.in_flight, loss, [&] { removed.release(); }));
    CHECK(removed.in_flight && removed.leases == 2 && removed.releases == 0);
    CHECK(canDisposePresentation(removed.in_flight, loss));
    disposeRemovedPresentation(removed.in_flight, loss, [&] { removed.release(); });
    CHECK(!removed.in_flight && removed.leases == 0 && removed.releases == 2);

    // Same source can have independent leases on two submitted presentation
    // slots. Completing one must not release the other slot's ownership.
    std::array<Flight, 3> graph;
    CHECK(observePresentationRetirement(graph[0].in_flight, RenderFenceWait::SIGNALED,
                                       [&] { graph[0].release(); }));
    CHECK(graph[1].in_flight && graph[1].leases == 2 && graph[2].leases == 2);
    const std::array outcomes{RenderFenceWait::SIGNALED, RenderFenceWait::DEVICE_LOST,
                             RenderFenceWait::WAIT_ERROR};
    bool graph_can_destroy = true;
    for (unsigned i = 0; i < graph.size(); ++i)
        graph_can_destroy &= canDisposePresentation(graph[i].in_flight, outcomes[i]);
    unsigned graph_destroys = 0;
    if (graph_can_destroy) ++graph_destroys;
    CHECK(graph_destroys == 0); // no dependency or lease freed before whole-graph preflight
    CHECK(graph[1].leases == 2 && graph[2].leases == 2);

    std::printf("native presentation retirement tests: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
