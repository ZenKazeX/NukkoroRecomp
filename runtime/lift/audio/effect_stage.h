#pragma once

#include "audio/output_stage.h"
#include "audio/reverb.h"

namespace shadow::audio {

// Owns the game's room effect, pending signal and output composition. Each
// block is 5 ms at the authored 32 kHz rate. Voice decoding, compression and
// device delivery are separate owners. Control, render and checkpoint
// operations are serialized by the owning audio thread; game commands are
// delivered at block boundaries. No device work or dynamic allocation occurs
// while rendering.
//
// A block is begin() (the output mode's fold, before the voices are added),
// finish() (this block's room send, the returns and the matrix) and then, when
// the game's effect callback runs for it, process(). A block whose callback
// does not run leaves its pending samples and the effect tail as they are.
// render() is that sequence driven by the stage's own control values.
class EffectStage final {
public:
    using Buffer = OutputStage::Buffer;
    using Channels = std::array<Buffer, 3>; // left, right, surround
    using Mode = OutputStage::Mode;
    struct Control {
        Mode mode = Mode::Stereo;
        bool enabled = true;
    };
    struct Input {
        Channels main{};
        Channels room{};
        Channels spatial{};
    };
    struct History {
        Reverb::History effect;
        OutputStage::History output;
        std::array<Channels, 3> pending{};
        std::array<Buffer, 3> spatial_right{};
        unsigned next = 0;
        Control scheduled;
        Control live;
        bool awaiting = false; // finished block whose callback has not run yet
    };

    // Reset is a new stream, not an enable/disable operation. Invalid controls,
    // parameters or histories leave the current stream unchanged.
    bool reset(const Reverb::Parameters& parameters, Control control) noexcept;
    // The game's effect initialization: new parameters and a cleared effect
    // tail. Pending samples and output history belong to the stream and stay.
    bool configure(const Reverb::Parameters& parameters) noexcept;
    bool control(Control next) noexcept;
    History history() const noexcept;
    bool restore(const History& history) noexcept;

    // The next block's sends/output mode were scheduled at the preceding
    // boundary. Live control governs effect processing, then becomes the next
    // schedule. Disabling freezes the effect and preserves pending samples.
    Channels render(const Input& input,
                    math::SubnormalMode mode = math::SubnormalMode::Preserve) noexcept;

    // The split block, for an owner whose schedule and callbacks come from the
    // game (the command list built at the preceding boundary, and the game's
    // own callback dispatch). Invalid modes are refused before any change.
    bool begin(Mode mode, Buffer& left, Buffer& right, Buffer& surround) const noexcept;
    bool finish(Control scheduled, Buffer& left, Buffer& right, Buffer& surround, const Channels& room,
                const Channels& spatial) noexcept;
    // The callback of the finished block: the room samples sent one block
    // earlier are processed in place. False when no block is waiting for it.
    bool process(math::SubnormalMode mode = math::SubnormalMode::Preserve) noexcept;

private:
    void advance() noexcept;

    Reverb effect_;
    OutputStage output_;
    std::array<Channels, 3> pending_{};
    std::array<Buffer, 3> spatial_right_{};
    unsigned next_ = 0;
    Control scheduled_;
    Control live_;
    bool awaiting_ = false;
};

} // namespace shadow::audio
