#pragma once

// An EMULATION OF THE AX DSP MICROCODE'S MIXING ARITHMETIC: one 5 ms frame (160 stereo samples at 32 kHz) from
// per-voice values (NATIVE_AUDIT.md C7b).
//
// What it computes is what the GameCube's AX DSP microcode computed for this game (docs/audio-engine-2026-09-17/
// cleanroom/05_DSP_MICROCODE_SEMANTICS.md, 04_GUEST_AUDIO_STACK.md), routine by routine (IRAM addresses below, the
// accumulator rounding taken from Dolphin's DSP interpreter): per voice, resample its source samples, apply the volume
// envelope, mix into the DSP's nine buses (main / aux A / aux B, each L R S); then the output mode's frame stages (the
// stereo fold of the previous frame's surround, or the Dolby-style matrix of aux B), the effect returns and the
// compressor. It holds no guest pointers, but it is not a native audio engine and is not meant to be kept: NE-AUDIO
// replaces it (and its caller) with a native voice engine; where the game-level arithmetic is kept, this code is the
// reference its output is compared with. The caller (System::run_ax_mix, SCAFFOLDING (R2), a DSP stand-in) translates
// the guest's command list and parameter blocks into these values and writes the returned PCM where the guest's audio
// interface DMA model reads it.
//
// The microcode corrections land one at a time (AU_REPORT.md section 4):
//   step 1  the former in-line arithmetic moved here unchanged;
//   step 2  nine buses routed by the mixer control word (with the per-sample volume ramps), and the mode-2 (Dolby)
//           frame stage: aux B L/R return one frame later, main L += it, main R -= it;
//   step 3  the microcode's linear SRC (phase advanced before the output, the two newest taps of a 4-sample window
//           carried per voice, saturated difference, ADDPAXZ rounding) and its signed, ramped, saturating envelope;
//   step 4  the output compressor (command 0x12): a frame with any main L/R sample at or beyond the threshold
//           selects an attack gain row, the following quiet frames walk a 10-row release, gains from the game's
//           21 x 160 table (main.dol 0x80563420);
//   step 5  stereo/opposite fold now belongs to native OutputStage and checkpoint history;
//   step 6  aux A: the game's room reverb. Each block sends aux A (and in matrix output aux B surround) and adds the
//           returns the list schedules; the effect itself runs when the game's callback does (EffectStage,
//           gekko/native_reverb_owner.h).
// Evidence labels follow the cleanroom documents: the stage ORDER and data flow are CONFIRMED-BINARY (the microcode
// image uploaded from main.dol 0x80564E60); opcode ARITHMETIC is SINGLE-SOURCE (Dolphin's DSP interpreter) where marked.

#include <array>
#include <cstddef>
#include <cstdint>
#include "audio/effect_stage.h"

namespace shadow::audio {

constexpr std::uint32_t kFrameSamples = 160u;  // one 5 ms frame at 32 kHz
constexpr std::uint32_t kMaxVoices = 64u;

// A voice's source samples for one frame: source[k] is the k-th source sample from the voice's position at the
// start of the frame (k = 0 is the sample at that position). Reads at or past `count` are silence - the producer
// sizes the view to everything the frame can reach.
struct SourceView {
    const std::int16_t* samples = nullptr;
    std::uint32_t count = 0;
    std::int32_t at(std::uint64_t k) const noexcept {
        return k < count ? static_cast<std::int32_t>(samples[static_cast<std::size_t>(k)]) : 0;
    }
};

// Sample-rate conversion selected for a voice (the SDK's three DSP modes).
enum class SrcMode : std::uint8_t { Polyphase = 0, Linear = 1, None = 2 };

// The nine mix buses, in the microcode's buffer order (05 section 4).
enum Bus : std::uint8_t { kMainL, kMainR, kMainS, kAuxAL, kAuxAR, kAuxAS, kAuxBL, kAuxBR, kAuxBS, kBusCount };

struct BusVolume {
    std::uint16_t value = 0;  // Q15, unsigned
    std::uint16_t delta = 0;  // signed per-sample step (two's complement), used when the bus ramps
};

// A host-side per-voice gain (Options > Audio category volumes, gekko/audio_categories.h): Q15, applied after the
// voice's envelope and before its bus sends. Unity is skipped entirely, so the DSP's arithmetic is untouched.
constexpr std::uint32_t kUnityVoiceGain = 0x8000u;

struct VoiceFrame {
    std::uint32_t slot = 0;                     // stable voice identity 0..63 (native per-voice state)
    bool restart = false;                         // the voice (re)started: take start_history
    std::array<std::int16_t, 4> start_history{};  // the 4 samples "before" a (re)start, SDK-primed (oldest first)
    SourceView source;
    std::uint32_t ratio = 0;                      // 16.16 source samples per output sample
    std::uint32_t frac = 0;                       // 16.16 fraction of the frame's first position
    SrcMode src = SrcMode::Linear;
    std::uint16_t envelope = 0;                   // Q15, signed
    std::uint16_t envelope_delta = 0;             // signed per-sample step
    std::uint16_t mixer_control = 0;              // bus enables and ramps (05 section 7.6)
    std::array<BusVolume, kBusCount> volume{};
    std::uint32_t gain = kUnityVoiceGain;         // host category gain, 0..kUnityVoiceGain (not part of the PB)
};

// How the frame's buses become the stereo output (the SDK's AXSetMode, as the list builder 0x803850AC expresses it).
enum class OutputMode : std::uint8_t {
    Plain,         // no surround stage (lists without 0x07 / 0x11 / 0x10)
    StereoFold,    // mode 0: main L = main R = the previous frame's main S, main S = 0, before the voices
    OppositeFold,  // mode 1: main L = -previous S, main R = +previous S (not selected by the observed game routes)
    DolbyMatrix,   // mode 2: aux B L/R return one frame later, main L += it, main R -= it
};

struct FrameSettings {
    OutputMode mode = OutputMode::Plain;
    bool aux_a = false;       // aux A effect send + return (the game's reverb; step 6), as the list scheduled it
    bool aux_b = false;       // aux B effect send + return in modes 0/1 (never registered by this game)
    bool compressor = false;                       // command 0x12 present (the SDK always emits it)
    std::uint16_t compressor_threshold = 0x8000;   // |sample| >= threshold triggers
    std::uint16_t compressor_release_frames = 10;  // frames of release after a trigger (<= 10: rows 11..20)
};

struct StereoFrame {
    std::array<std::int16_t, kFrameSamples> left{};
    std::array<std::int16_t, kFrameSamples> right{};
};

struct MixStats {
    std::uint32_t voices_mixed = 0;      // voices with a main L/R gain and a non-zero envelope
    std::uint32_t ramped_voices = 0;     // voices whose mixer control asked for a volume ramp
    std::uint32_t enveloped_voices = 0;  // voices with a non-zero envelope delta
    std::int32_t peak = 0;               // largest |sample| written this frame
    bool compressor_triggered = false;   // a main L/R sample reached the threshold this frame
    std::int32_t compressor_row = -1;    // gain row applied this frame (0..20), -1 = none
};

// The compressor's gain table: 21 rows x 160 Q15 gains (rows 0..10 attack, 11..20 release), the game's own data.
constexpr std::uint32_t kCompressorRows = 21u;
const std::array<std::uint16_t, kCompressorRows * kFrameSamples>& compressor_gain_table() noexcept;

// Values that affect subsequent frames. Working buses are rebuilt by mix().
struct MixerHistory {
    std::uint16_t compressor_release = 0;
    std::array<std::array<std::int32_t, kFrameSamples>, 2> aux_b_return{};
    std::array<std::array<std::int16_t, 4>, kMaxVoices> source{};
    OutputStage::Buffer main_surround{};
    std::array<OutputStage::Buffer, 2> alternate_matrix{};
    bool alternate_return = false;
    // The room effect (step 6): its tail and parameters, the three pending room blocks, the matrix surround sends,
    // the slot of the next block and whether the last finished block still waits for its callback.
    Reverb::History effect{};
    std::array<EffectStage::Channels, 3> pending{};
    std::array<OutputStage::Buffer, 3> spatial_right{};
    std::uint8_t next = 0;
    bool awaiting = false;
};

class Mixer final {
public:
    MixerHistory history() const noexcept;
    bool restore_history(const MixerHistory& saved) noexcept;
    // Forget everything carried between frames when restarting the audio stream.
    void reset() noexcept;
    void mix(const FrameSettings& settings, const VoiceFrame* voices, std::uint32_t count, StereoFrame& out,
             MixStats& stats) noexcept;
    // The room effect the mixer sends to and returns from (configured and run by the effect's owner).
    EffectStage& effect() noexcept { return effect_; }
    const EffectStage& effect() const noexcept { return effect_; }

private:
    using Buffer = std::array<std::int32_t, kFrameSamples>;
    void render_voice(const VoiceFrame& voice, MixStats& stats) noexcept;
    void compress(const FrameSettings& settings, MixStats& stats) noexcept;

    std::array<Buffer, kBusCount> bus_{};
    // Frames of release left (the microcode keeps it in DSP DRAM 0x0E44; never uploaded at boot, so its first value is
    // undefined on hardware - 0 here, INFERRED in 05 section 2.2).
    std::uint16_t compressor_counter_ = 0;
    EffectStage effect_; // the room effect, its pending blocks, and the preceding surround and matrix blocks
    // Per voice: the last 4 source samples read (oldest first) - the microcode keeps them in PB words 86..89.
    std::array<std::array<std::int16_t, 4>, kMaxVoices> history_{};
};

}  // namespace shadow::audio
