// Deterministic owning policy tests; no graphics device or emulated hardware.
#include "plume_fence_retirement.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace {
bool deny_allocations = false;
std::size_t denied_allocations = 0;
}

// This hook is confined to this header-only test executable. It never replaces
// a shipping allocator or injects a fault into a real GPU device.
void* operator new(std::size_t bytes) {
    if (deny_allocations) { ++denied_allocations; throw std::bad_alloc(); }
    if (void* result = std::malloc(bytes == 0 ? 1 : bytes)) return result;
    throw std::bad_alloc();
}
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

namespace {
int failures = 0;
void check(bool pass, const char* text, int line) {
    if (!pass) { ++failures; std::printf("FAIL line %d: %s\n", line, text); }
}
#define CHECK(x) check(bool(x), #x, __LINE__)
using plume::RenderFenceWait;

struct DenyAllocations {
    DenyAllocations() noexcept { denied_allocations = 0; deny_allocations = true; }
    ~DenyAllocations() { deny_allocations = false; }
};

void allocation_independent_errors() {
    using plume::FenceRetirementError;
    static_assert(std::is_nothrow_constructible_v<FenceRetirementError, RenderFenceWait, const char*>);
    static_assert(std::is_nothrow_copy_constructible_v<FenceRetirementError>);
    static_assert(std::is_nothrow_copy_assignable_v<FenceRetirementError>);
    static_assert(FenceRetirementError::kOwnerTextBytes == sizeof("native startup texture upload") - 1u);
    bool old_failed = false;
    {
        DenyAllocations denial;
        try {
            // Exact former allocating diagnostic mechanism; it must exercise the hook.
            const std::runtime_error old(std::string("native startup texture upload") +
                                         ": fence retirement failed (deadline expired)");
            (void)old;
        } catch (const std::bad_alloc&) { old_failed = true; }
    }
    CHECK(old_failed && denied_allocations != 0);
    constexpr const char* owners[] = {
        "native startup texture upload", "native GRAPHICS slot", "native COPY batch",
        "native presentation slot", "native presentation teardown",
        "test owner", "test renderer", "split guard"
    };
    constexpr RenderFenceWait outcomes[] = {
        RenderFenceWait::TIMEOUT, RenderFenceWait::WAIT_ERROR, RenderFenceWait::DEVICE_LOST
    };
    for (const char* owner : owners) {
        CHECK(std::strlen(owner) <= FenceRetirementError::kOwnerTextBytes);
        for (const auto outcome : outcomes) {
            const char* reason = outcome == RenderFenceWait::DEVICE_LOST ? "device removed" :
                                 outcome == RenderFenceWait::WAIT_ERROR ? "host wait failed" :
                                                                         "deadline expired";
            const std::string expected = std::string(owner) + ": fence retirement failed (" + reason + ")";
            bool exact = false;
            {
                DenyAllocations denial;
                const FenceRetirementError error(outcome, owner);
                const FenceRetirementError copied(error);
                FenceRetirementError assigned(RenderFenceWait::SIGNALED, "different owner");
                assigned = error;
                exact = error.outcome() == outcome && copied.outcome() == outcome &&
                        assigned.outcome() == outcome &&
                        std::strcmp(error.what(), expected.c_str()) == 0 &&
                        std::strcmp(copied.what(), expected.c_str()) == 0 &&
                        std::strcmp(assigned.what(), expected.c_str()) == 0;
            }
            CHECK(exact && denied_allocations == 0);
            bool transported = false;
            try { throw FenceRetirementError(outcome, owner); }
            catch (const FenceRetirementError& error) {
                transported = error.outcome() == outcome && std::strcmp(error.what(), expected.c_str()) == 0;
            }
            CHECK(transported);
        }
    }
    constexpr const char* long_owner = "native startup texture upload -- overlong diagnostic label";
    constexpr const char* long_expected = "native startup texture upload: fence retirement failed (host wait failed)";
    bool bounded = false;
    {
        DenyAllocations denial;
        const FenceRetirementError error(RenderFenceWait::WAIT_ERROR, long_owner);
        bounded = error.outcome() == RenderFenceWait::WAIT_ERROR &&
                  std::strcmp(error.what(), long_expected) == 0 &&
                  std::strlen(error.what()) < FenceRetirementError::kMessageBytes;
    }
    CHECK(bounded && denied_allocations == 0);
}

void wait_case(RenderFenceWait terminal, unsigned pending, bool expect_blocked) {
    std::vector<std::uint64_t> waits;
    unsigned remaining = pending;
    auto result = plume::waitForRetirement([&](std::uint64_t timeout) {
        waits.push_back(timeout);
        if (remaining) { --remaining; return RenderFenceWait::TIMEOUT; }
        return terminal;
    });
    CHECK(waits.front() == 0);
    CHECK(waits.size() <= 11);
    for (std::size_t i = 1; i < waits.size(); ++i) CHECK(waits[i] == 1000000000ull);
    CHECK(result.blocked == expect_blocked);
    unsigned completions = 0;
    bool in_flight = true;
    bool thrown = false;
    try {
        plume::completeRetirement(result, "test owner", [&] {
            ++completions;
            in_flight = false;
        });
    } catch (const plume::FenceRetirementError& error) {
        CHECK(error.outcome() == result.outcome);
        thrown = true;
    }
    CHECK(completions == (result.outcome == RenderFenceWait::SIGNALED ? 1u : 0u));
    CHECK(in_flight == (result.outcome != RenderFenceWait::SIGNALED));
    CHECK(thrown == in_flight);
    if (terminal != RenderFenceWait::TIMEOUT && pending < 11) CHECK(waits.size() == pending + 1u);
}
}

int main() {
    allocation_independent_errors();
    const auto sentinel = (std::numeric_limits<std::uint64_t>::max)();
    CHECK(plume::classifyD3D12Fence(sentinel, 2, false) == RenderFenceWait::DEVICE_LOST);
    CHECK(plume::classifyD3D12Fence(sentinel, sentinel, false) == RenderFenceWait::DEVICE_LOST);
    CHECK(plume::classifyD3D12Fence(8, 7, true) == RenderFenceWait::DEVICE_LOST);
    CHECK(plume::classifyD3D12Fence(7, 7, false) == RenderFenceWait::SIGNALED);
    CHECK(plume::classifyD3D12Fence(6, 7, false) == RenderFenceWait::TIMEOUT);
    // A stale signaled host event is never evidence of fence completion.
    CHECK(plume::classifyD3D12Fence(0, 3, false) != RenderFenceWait::SIGNALED);
    wait_case(RenderFenceWait::SIGNALED, 0, false);
    wait_case(RenderFenceWait::SIGNALED, 1, true);
    wait_case(RenderFenceWait::SIGNALED, 10, true);
    wait_case(RenderFenceWait::TIMEOUT, 100, true);
    wait_case(RenderFenceWait::DEVICE_LOST, 0, false);
    wait_case(RenderFenceWait::DEVICE_LOST, 3, true);
    wait_case(RenderFenceWait::WAIT_ERROR, 0, false);
    wait_case(RenderFenceWait::WAIT_ERROR, 2, true);
    // Split/resize guards must all retire before *any* arena reset/list begin.
    for (unsigned failing_slot = 0; failing_slot < 3; ++failing_slot) {
        unsigned resets = 0;
        unsigned begins = 0;
        bool thrown = false;
        try {
            for (unsigned slot = 0; slot < 3; ++slot) {
                auto result = plume::waitForRetirement([&](std::uint64_t) {
                    return slot == failing_slot ? RenderFenceWait::WAIT_ERROR : RenderFenceWait::SIGNALED;
                });
                plume::completeRetirement(result, "split guard", [] {});
            }
            ++resets;
            ++begins;
        } catch (const plume::FenceRetirementError&) { thrown = true; }
        CHECK(thrown);
        CHECK(resets == 0);
        CHECK(begins == 0);
    }
    std::printf("native fence retirement policy: %d failures\n", failures);
    return failures ? 1 : 0;
}
