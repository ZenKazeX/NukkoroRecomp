#include "gekko/system.h"
#include "gekko/savestate.h"

#include <algorithm>
#include <limits>

namespace gekko {
namespace {
void put_flags(std::vector<u8>& out, const std::array<bool, 64>& flags) {
    u64 bits = 0;
    for (unsigned i = 0; i < flags.size(); ++i) bits |= u64(flags[i]) << i;
    state::put(out, bits);
}

bool get_flags(const u8*& cursor, const u8* end, std::array<bool, 64>& flags) {
    u64 bits = 0;
    if (!state::get(cursor, end, bits)) return false;
    for (unsigned i = 0; i < flags.size(); ++i) flags[i] = (bits & (u64{1} << i)) != 0;
    return true;
}
}  // namespace

// Kept beside savestate orchestration, outside the per-frame path. Source
// windows, accepted-list traversal and working mix buses are fully consumed
// before the checkpoint boundary. Only persistent history is recorded.
void System::save_host_audio_state(std::vector<u8>& out) const {
    state::put<u8>(out, u8(ax_sample_resampling_) | (u8(ax_adpcm_cursor_) << 1));
    state::put(out, ax_voice_pb_);
    state::put(out, ax_position_fraction_);
    put_flags(out, ax_voice_was_running_);
    put_flags(out, ax_voice_restarted_);
    state::put(out, ax_adpcm_yn1_);
    state::put(out, ax_adpcm_yn2_);
    state::put(out, ax_adpcm_position_);
    for (std::size_t voice = 0; voice < ax_adpcm_cursor_state_.size(); ++voice) {
        const auto& c = ax_adpcm_cursor_state_[voice];
        state::put<u8>(out, c.valid ? 1u : 0u);
        if (!c.valid) continue;
        state::put(out, c.nibble);
        state::put(out, c.predictor);
        state::put(out, c.scale);
        state::put(out, c.yn1);
        state::put(out, c.yn2);
        state::put(out, c.decoded);
        // Store only samples the cursor can read, oldest first. Dead ring
        // entries are neither meaningful state nor deterministic after reset.
        const u64 count = std::min(c.decoded, kAxAdpcmRingSamples);
        const auto& ring = ax_adpcm_ring_->voice[voice];
        for (u64 i = c.decoded - count; i < c.decoded; ++i)
            state::put(out, ring[static_cast<std::size_t>(i & (kAxAdpcmRingSamples - 1u))]);
    }
    const auto mixer = audio_mixer_->history();
    state::put(out, mixer.compressor_release);
    state::put(out, mixer.aux_b_return);
    state::put(out, mixer.source);
    state::put(out, mixer.main_surround);
    state::put(out, mixer.alternate_matrix);
    state::put<u8>(out, mixer.alternate_return ? 1u : 0u);
    // Version 23: the room effect - which game object it serves, its parameters and tail, and its pending blocks.
    state::put(out, reverb_binding_.context);
    state::put<u8>(out, reverb_binding_.configured ? 1u : 0u);
    const auto& p = mixer.effect.parameters;
    for (const float value : {p.color, p.level, p.time, p.damping, p.pre_delay, p.crossfeed}) state::put(out, value);
    state::put(out, mixer.effect.samples);
    state::put(out, mixer.effect.positions);
    state::put(out, mixer.effect.delayed);
    state::put(out, mixer.effect.low_pass);
    state::put(out, mixer.effect.previous_input);
    state::put(out, mixer.pending);
    state::put(out, mixer.spatial_right);
    state::put(out, mixer.next);
    state::put<u8>(out, mixer.awaiting ? 1u : 0u);
    // Version 31: the DSP side's lagging copy of the voice parameter blocks and update rings (ax_pb_image.h).
    state::put(out, ax_pb_image_.pb);
    state::put(out, ax_pb_image_.ring);
    // Version 32: the frame builder's two next-frame lists (native_ax_frame.h).
    for (const AxLists::List& list : ax_lists_.slot) {
        state::put<u8>(out, list.valid ? 1u : 0u);
        state::put<u8>(out, static_cast<u8>(list.settings.mode));
        state::put<u8>(out, list.settings.aux_a ? 1u : 0u);
        state::put<u8>(out, list.settings.aux_b ? 1u : 0u);
        state::put<u8>(out, list.settings.compressor ? 1u : 0u);
        state::put<u16>(out, list.settings.compressor_threshold);
        state::put<u16>(out, list.settings.compressor_release_frames);
        state::put<u32>(out, list.output);
    }
}

bool System::load_host_audio_state(const u8*& cursor, const u8* end, u32 version) {
    // The block's layout is version 23's in every later version (24: no ARAM image; 25-27: blocks elsewhere). The
    // upper bound follows the runtime's own version: a fixed 24 here refused every state saved since 25.
    if (version < 20u || version > save_state_version()) return false;
    u8 modes = 0;
    const u8 expected = u8(ax_sample_resampling_) | (u8(ax_adpcm_cursor_) << 1);
    // Diagnostic decoder modes change arithmetic. Refuse incompatible files
    // instead of silently changing the selected mode or claiming equivalence.
    if (!state::get(cursor, end, modes) || modes != expected) return false;
    reset_host_audio_state();
    if (!state::get(cursor, end, ax_voice_pb_) ||
        !state::get(cursor, end, ax_position_fraction_) ||
        !get_flags(cursor, end, ax_voice_was_running_) ||
        !get_flags(cursor, end, ax_voice_restarted_) ||
        !state::get(cursor, end, ax_adpcm_yn1_) ||
        !state::get(cursor, end, ax_adpcm_yn2_) ||
        !state::get(cursor, end, ax_adpcm_position_)) return false;
    for (std::size_t voice = 0; voice < ax_voice_pb_.size(); ++voice) {
        const u32 pb = ax_voice_pb_[voice];
        if (ax_position_fraction_[voice] > 0xffffu) return false;
        if (pb == 0u) continue;
        if ((pb & 1u) || !memory_.mapped(GcAddr(pb), 244u)) return false;
        for (std::size_t previous = 0; previous < voice; ++previous) {
            const u32 other = ax_voice_pb_[previous];
            if (other != 0u && u64(pb) < u64(other) + 244u && u64(other) < u64(pb) + 244u)
                return false;
        }
    }
    for (std::size_t voice = 0; voice < ax_adpcm_cursor_state_.size(); ++voice) {
        u8 valid = 0;
        if (!state::get(cursor, end, valid) || valid > 1u) return false;
        if (!valid) continue;
        auto& c = ax_adpcm_cursor_state_[voice];
        if (!state::get(cursor, end, c.nibble) ||
            !state::get(cursor, end, c.predictor) ||
            !state::get(cursor, end, c.scale) ||
            !state::get(cursor, end, c.yn1) ||
            !state::get(cursor, end, c.yn2) ||
            !state::get(cursor, end, c.decoded)) return false;
        if (c.predictor > 7u || c.scale > 15u ||
            c.yn1 < -32768 || c.yn1 > 32767 || c.yn2 < -32768 || c.yn2 > 32767 ||
            c.decoded > std::numeric_limits<u64>::max() - kAxSourceWindowSamples) return false;
        const u64 count = std::min(c.decoded, kAxAdpcmRingSamples);
        auto& ring = ax_adpcm_ring_->voice[voice];
        for (u64 i = c.decoded - count; i < c.decoded; ++i)
            if (!state::get(cursor, end, ring[static_cast<std::size_t>(i & (kAxAdpcmRingSamples - 1u))]))
                return false;
        c.valid = true;
    }
    shadow::audio::MixerHistory mixer;
    if (!state::get(cursor, end, mixer.compressor_release) ||
        !state::get(cursor, end, mixer.aux_b_return) ||
        !state::get(cursor, end, mixer.source)) return false;
    if (version >= 21u && !state::get(cursor, end, mixer.main_surround)) return false;
    if (version >= 22u) {
        u8 phase = 0;
        if (!state::get(cursor, end, mixer.alternate_matrix) ||
            !state::get(cursor, end, phase) || phase > 1u) return false;
        mixer.alternate_return = phase != 0;
    }
    reverb::Binding binding;
    if (version >= 23u) {
        u8 configured = 0, awaiting = 0;
        float values[6] = {};
        if (!state::get(cursor, end, binding.context) || !state::get(cursor, end, configured) || configured > 1u)
            return false;
        for (float& value : values)
            if (!state::get(cursor, end, value)) return false;
        mixer.effect.parameters = {values[0], values[1], values[2], values[3], values[4], values[5]};
        if (!state::get(cursor, end, mixer.effect.samples) || !state::get(cursor, end, mixer.effect.positions) ||
            !state::get(cursor, end, mixer.effect.delayed) || !state::get(cursor, end, mixer.effect.low_pass) ||
            !state::get(cursor, end, mixer.effect.previous_input) || !state::get(cursor, end, mixer.pending) ||
            !state::get(cursor, end, mixer.spatial_right) || !state::get(cursor, end, mixer.next) ||
            !state::get(cursor, end, awaiting) || awaiting > 1u)
            return false;
        binding.configured = configured != 0;
        mixer.awaiting = awaiting != 0;
    }
    // Older files have no effect history: the callback binds the effect from the game's parameters (an empty tail).
    if (version >= 31u) {
        if (!state::get(cursor, end, ax_pb_image_.pb) || !state::get(cursor, end, ax_pb_image_.ring)) return false;
    } else {
        import_ax_pb_image_from_guest();  // the guest memory the file restored holds the SDK's PB array
    }
    if (version >= 32u) {
        for (AxLists::List& list : ax_lists_.slot) {
            u8 valid = 0, mode = 0, aux_a = 0, aux_b = 0, compressor = 0;
            u16 threshold = 0, release = 0;
            u32 output = 0;
            if (!state::get(cursor, end, valid) || !state::get(cursor, end, mode) || !state::get(cursor, end, aux_a) ||
                !state::get(cursor, end, aux_b) || !state::get(cursor, end, compressor) || !state::get(cursor, end, threshold) ||
                !state::get(cursor, end, release) || !state::get(cursor, end, output) || mode > 3u)
                return false;
            list.valid = valid != 0u;
            list.settings.mode = static_cast<shadow::audio::OutputMode>(mode);
            list.settings.aux_a = aux_a != 0u;
            list.settings.aux_b = aux_b != 0u;
            list.settings.compressor = compressor != 0u;
            list.settings.compressor_threshold = threshold;
            list.settings.compressor_release_frames = release;
            list.output = output;
        }
    } else {
        import_ax_lists_from_guest();  // the lists the file restored sit in the SDK's command-list slots
    }
    if (!audio_mixer_->restore_history(mixer)) return false;
    reverb_binding_ = binding;
    if (version < 33u) {
        // A state of a build whose AX engine still ran as guest code (frame, AI / DSP callbacks, DSP task): the engine's state is all in the
        // SDK's words and the lists / image taken over above, so it continues natively once the AI DMA callback names the native frame and
        // the DSP's resume callback's effect (the state word at 1) is applied. A state saved while the CPU was inside the guest engine is
        // refused by the loader (savestate.cpp); nothing of the DSP task / mailbox model is kept.
        const u32 sda = cpu_.gpr[13];
        if (memory_.peek32(sda + ax_frame::kSdaAiCallback) == 0x80385950u) memory_.write32(sda + ax_frame::kSdaAiCallback, ax_frame::kAiCallback);  // liveness-roots: value-only (an old state's callback word; never a call target)
        if (memory_.peek32(sda + ax_frame::kSdaState) == 0u) memory_.write32(sda + ax_frame::kSdaState, 1u);
        interrupts_->clear(NativeInterrupts::Source::Dsp);
    }
    return true;
}
}  // namespace gekko
