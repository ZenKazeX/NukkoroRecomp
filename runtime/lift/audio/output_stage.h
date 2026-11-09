#pragma once

#include <array>
#include <cstdint>

namespace shadow::audio {

// Authored 5 ms channel history at 32 kHz. The voice engine supplies ordinary
// sample arrays; this stage owns the preceding block needed by output routing.
class OutputStage final {
public:
    static constexpr unsigned kSamples = 160;
    using Buffer = std::array<std::int32_t, kSamples>;
    enum class Mode { Direct, Stereo, Opposite, Matrix };
    struct History {
        Buffer surround{};
        std::array<Buffer, 2> matrix{};
        std::array<Buffer, 2> alternate_matrix{};
        bool alternate_return = false;
    };

    void reset() noexcept { history_ = {}; }
    const History& history() const noexcept { return history_; }
    void restore(const History& history) noexcept { history_ = history; }
    // Establish the main channels before adding this block's voices/effects.
    void begin(Mode mode, Buffer& left, Buffer& right, Buffer& surround) const noexcept;
    // Resolve delayed matrix sends, then remember this block's surround.
    // Both retained blocks survive mode changes; their return phase advances
    // every audio block, including blocks using stereo/opposite output.
    // Compression and final output conversion follow this stage.
    void finish(Mode mode, Buffer& left, Buffer& right, const Buffer& surround,
                const Buffer& matrix_left, const Buffer& matrix_right) noexcept;
private:
    History history_{};
};

} // namespace shadow::audio
