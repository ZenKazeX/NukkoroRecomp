#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "math/matrix.h"

namespace shadow::audio {

// Recovered game effect at its authored 32 kHz sample rate. No CPU state,
// console addresses, callback slots, allocation hooks or device protocol.
// Integration into the owning voice/output service is still pending; this
// component is currently exercised by the original-instruction contract oracle.
class Reverb final {
public:
    struct Parameters {
        float color = 0.5f;
        float level = 1.0f;
        float time = 1.0f;
        float damping = 0.8f;
        float pre_delay = 0.01f;
        float crossfeed = 0.3f;
    };
    static constexpr std::size_t kLines = 18;
    static constexpr std::size_t kDelaySamples = 20296;

    // All persistent values, including inaudible tails, are explicit so a
    // checkpoint can continue exactly. Derived coefficients are reconstructed.
    struct History {
        Parameters parameters;
        std::array<float, kDelaySamples> samples{};
        std::array<std::uint16_t, kLines> positions{};
        std::array<float, kLines> delayed{};
        std::array<float, 3> low_pass{};
        std::array<float, 3> previous_input{};
    };

    Reverb() noexcept;
    // Invalid finite/nonfinite parameters leave the existing effect untouched.
    // Successful configuration resets its tail, with no allocation.
    bool configure(const Parameters& parameters) noexcept;
    void reset() noexcept;
    const History& history() const noexcept { return history_; }
    bool restore(const History& history) noexcept;

    // Three disjoint, writable signed-sample arrays: left, right, surround.
    // Any block length is allowed; calls share the same continuous history.
    // Bypass is implemented by the owner not calling process (tail freezes).
    // Like the native matrix API: nearest rounding, host DAZ/FTZ disabled.
    // Flush also preserves the authored invalid-conversion result INT32_MIN;
    // Preserve saturates positive overflow to INT32_MAX.
    void process(const std::array<std::int32_t*, 3>& channels, std::size_t frames,
                 math::SubnormalMode mode = math::SubnormalMode::Preserve) noexcept;

private:
    void coefficients() noexcept;
    float exchange(std::size_t line, float input) noexcept;
    History history_{};
    std::array<float, 3> comb_{};
    float damping_ = 0, wet_ = 0, dry_ = 0, cross_ = 0, direct_ = 0;
    bool pre_delay_ = false;
};

} // namespace shadow::audio
