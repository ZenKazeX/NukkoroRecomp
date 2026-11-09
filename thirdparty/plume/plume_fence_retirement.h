#pragma once

#include <cstdint>
#include <cstddef>
#include <exception>
#include <limits>
#include <utility>

namespace plume {

// DEVICE_LOST requires a confirmed removed device, not a failed host event wait.
enum class RenderFenceWait { SIGNALED, TIMEOUT, DEVICE_LOST, WAIT_ERROR };

inline RenderFenceWait classifyD3D12Fence(std::uint64_t completed, std::uint64_t target,
                                         bool removed) noexcept {
    // D3D12 reports UINT64_MAX after device removal; it is not a completed value.
    if (removed || completed == (std::numeric_limits<std::uint64_t>::max)())
        return RenderFenceWait::DEVICE_LOST;
    return completed >= target ? RenderFenceWait::SIGNALED : RenderFenceWait::TIMEOUT;
}

class FenceRetirementError final : public std::exception {
public:
    // Derived from the longest maintained owner label. Longer external labels
    // are truncated to this many bytes; the reason and outcome remain complete.
    static constexpr std::size_t kOwnerTextBytes = sizeof("native startup texture upload") - 1u;
    static constexpr std::size_t kMessageBytes =
        kOwnerTextBytes + sizeof(": fence retirement failed (deadline expired)");
    FenceRetirementError(RenderFenceWait outcome, const char* owner) noexcept
        : outcome_(outcome) {
        std::size_t used = 0;
        append(used, owner != nullptr ? owner : "unknown native owner", kOwnerTextBytes);
        append(used, ": fence retirement failed (");
        append(used, outcome == RenderFenceWait::DEVICE_LOST ? "device removed" :
                     outcome == RenderFenceWait::WAIT_ERROR ? "host wait failed" :
                                                             "deadline expired");
        append(used, ")");
    }
    FenceRetirementError(const FenceRetirementError&) noexcept = default;
    FenceRetirementError& operator=(const FenceRetirementError&) noexcept = default;
    const char* what() const noexcept override { return message_; }
    RenderFenceWait outcome() const noexcept { return outcome_; }
private:
    void append(std::size_t& used, const char* text,
                std::size_t maximum = kMessageBytes) noexcept {
        for (std::size_t index = 0; index < maximum && text[index] != '\0' &&
                                    used + 1u < kMessageBytes; ++index)
            message_[used++] = text[index];
        message_[used] = '\0';
    }
    RenderFenceWait outcome_;
    char message_[kMessageBytes]{};
};

struct FenceRetirement {
    RenderFenceWait outcome = RenderFenceWait::TIMEOUT;
    bool blocked = false;
};

// Same ten-second deadline as the native GRAPHICS owner. No retry after a
// terminal error. Injecting the wait operation in a unit test does not replace
// the shipping queue: shipping passes its actual RHI fence wait.
template<class Wait>
FenceRetirement waitForRetirement(Wait&& wait) {
    FenceRetirement result{wait(0), false};
    if (result.outcome != RenderFenceWait::TIMEOUT) return result;
    result.blocked = true;
    for (unsigned second = 0; second < 10; ++second) {
        result.outcome = wait(1000000000ull);
        if (result.outcome != RenderFenceWait::TIMEOUT) break;
    }
    return result;
}

template<class Complete>
void completeRetirement(const FenceRetirement& result, const char* owner, Complete&& complete) {
    if (result.outcome != RenderFenceWait::SIGNALED)
        throw FenceRetirementError(result.outcome, owner);
    std::forward<Complete>(complete)();
}

} // namespace plume
