#pragma once

// Per-category volume (Options > Audio: Music, Sound effects, Voices), applied per AX voice in the native mixer.
//
// The game mixes everything into one stereo stream, so the categories are told apart at the AX voice, by who owns it:
//
//   Music  - the ADX handles of the BGM objects (TObjBGM, vtable 0x8053E014): objects at 0x805E9BC0[2] (r13 - 0x2940,
//            built by 0x801D844C through 0x801D77F8), ADXT handle at object + 0x44.
//   Voice  - the ADX handles of the voice objects (TObjSERIF, vtable 0x8053DFDC: PRS_VOICE dialogue, sng_EV* cutscene
//            voices, sng_jin_* calls): objects at 0x805E9BD0[2] (r13 - 0x2930, built through 0x801D6FEC), ADXT handle at
//            object + 0x3C - and the GCAX voices of the character banks 3 / 4 / 5 (B3_SHADOW, B4_SUBHERO, B5_SUBDARK:
//            SelectBank 0x801F2A80's jump table 0x8053F498 cases 3, 4, 5 load those three .mlt files).
//   Sfx    - every other GCAX voice (banks 0, 1, 2, 6, 7: system, common, weapons, enemies, stage).
//   Other  - an AX voice neither list names (the Sofdec movie soundtracks: ADXT created by 0x803EED68): only the master
//            volume applies.
//
// An ADX handle's AX voices: ADXT handle (0x803C38E8) + 0xC = its rna output handle (0x803CFAB0; table 0x805C3E4C,
// 16 x 0xEC): byte +0 used, byte +3 channel count, the AXVPB* of channel k at +0x08 + 4k. A GCAX voice (context
// *0x805F24AC, 64 voices from ctx + 0x54, stride 0x10C): word +0 flags (bit 31 used, bit 30 AX voice dropped), +0x34 the
// AXVPB*, +0xE4 the bank gcaxVoiceStart (0x8043DACC) was handed. The mixer's voice list carries the address of the DSP's
// own parameter block, which is NOT the PB inside the AXVPB (AXVPB + 0x138): AXInit (0x80386E88) keeps the 64 DSP PBs
// in a separate array (0x8058FCA0, stride 0xF4) and the AXVPB's word +0x18 is its slot in it (the AXVPB array is
// 0x805989A0, stride 0x22C), so a voice is keyed by 0x8058FCA0 + 0xF4 * index.
//
// Reading the lists is stateless (nothing is kept between frames, so a savestate needs nothing), and it only runs while
// some category is not at 100 %: at the default the mixer's path is the one it always was (VoiceFrame::gain stays
// unity and the mixer skips the multiply, so the output is bit-identical).
//
// The gain is a Q15 factor (0x8000 = unity) multiplied into the voice after its envelope and before its bus sends, so
// the voice's reverb send follows it. 0 % silences the voice; the game's own volume fields are never touched.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace gekko::audio_category {

enum class Kind : std::uint8_t { Other = 0, Music = 1, Voice = 2, Sfx = 3 };

inline constexpr std::uint32_t kUnityGain = 0x8000u;  // Q15, matches shadow::audio::kUnityVoiceGain

// The three user-set gains (Kind::Other has none). Written by the Options menu thread, read by the mixer thread.
inline std::atomic<std::uint32_t>& gain_slot(Kind kind) noexcept {
    static std::atomic<std::uint32_t> slots[4] = {kUnityGain, kUnityGain, kUnityGain, kUnityGain};
    return slots[static_cast<unsigned>(kind) & 3u];
}

// percent 0..100 (out of range is clamped) -> Q15, linear like the master volume.
inline std::uint32_t gain_from_percent(int percent) noexcept {
    if (percent <= 0) return 0u;
    if (percent >= 100) return kUnityGain;
    return static_cast<std::uint32_t>((percent * static_cast<int>(kUnityGain) + 50) / 100);
}

inline void set_percent(Kind kind, int percent) noexcept {
    if (kind == Kind::Other) return;
    gain_slot(kind).store(gain_from_percent(percent), std::memory_order_relaxed);
}

inline std::uint32_t gain(Kind kind) noexcept {
    return kind == Kind::Other ? kUnityGain : gain_slot(kind).load(std::memory_order_relaxed);
}

// True while no category is changed (the mixer's untouched path).
inline bool all_unity() noexcept {
    return gain(Kind::Music) == kUnityGain && gain(Kind::Voice) == kUnityGain && gain(Kind::Sfx) == kUnityGain;
}

// The guest's character banks (GCAX bank = the game's bank id = SelectBank slot).
constexpr bool is_character_bank(std::uint32_t bank) noexcept { return bank >= 3u && bank <= 5u; }

// Guest addresses (main.dol, PAL GUPP8P).
inline constexpr std::uint32_t kBgmObjects = 0x805E9BC0u;    // TObjBGM*[2]
inline constexpr std::uint32_t kBgmAdxt = 0x44u;
inline constexpr std::uint32_t kSerifObjects = 0x805E9BD0u;  // TObjSERIF*[2]
inline constexpr std::uint32_t kSerifAdxt = 0x3Cu;
inline constexpr std::uint32_t kAdxtRna = 0x0Cu;
inline constexpr std::uint32_t kRnaUsed = 0x00u, kRnaChannels = 0x03u, kRnaVoice = 0x08u;
inline constexpr std::uint32_t kGcaxContextPointer = 0x805F24ACu;
inline constexpr std::uint32_t kGcaxVoices = 0x54u, kGcaxVoiceBytes = 0x10Cu, kGcaxVoiceCount = 64u;
inline constexpr std::uint32_t kGvFlags = 0x00u, kGvVpb = 0x34u, kGvBank = 0xE4u;
inline constexpr std::uint32_t kVpbIndex = 0x18u;
inline constexpr std::uint32_t kDspPbBase = 0x8058FCA0u, kDspPbStride = 0xF4u, kDspPbCount = 64u;

// The DSP parameter block of an AXVPB (0 if its slot is out of range).
template <typename Read32>
std::uint32_t dsp_parameter_block(Read32&& read32, std::uint32_t vpb) noexcept {
    const std::uint32_t index = read32(vpb + kVpbIndex);
    return index < kDspPbCount ? kDspPbBase + index * kDspPbStride : 0u;
}

// The AX voices of this frame and their category, keyed by parameter-block address.
struct VoiceKinds {
    static constexpr std::size_t kCapacity = 64;
    std::array<std::uint32_t, kCapacity> pb{};
    std::array<Kind, kCapacity> kind{};
    std::size_t count = 0;

    void add(std::uint32_t parameter_block, Kind k) noexcept {
        if (count < kCapacity) {
            pb[count] = parameter_block;
            kind[count++] = k;
        }
    }
    Kind find(std::uint32_t parameter_block) const noexcept {
        for (std::size_t i = 0; i < count; ++i)
            if (pb[i] == parameter_block) return kind[i];
        return Kind::Other;
    }
};

// Builds the table from the guest's memory. `read32(address)` returns the big-endian word at a guest address (0 for
// unmapped memory).
template <typename Read32>
void build_voice_kinds(Read32&& read32, VoiceKinds& out) noexcept {
    out.count = 0;
    const auto adx_object = [&](std::uint32_t object, std::uint32_t adxt_offset, Kind kind) {
        if (object == 0u) return;
        const std::uint32_t adxt = read32(object + adxt_offset);
        if (adxt == 0u) return;
        const std::uint32_t rna = read32(adxt + kAdxtRna);
        if (rna == 0u) return;
        const std::uint32_t head = read32(rna + kRnaUsed);  // byte +0 used, +1 flags, +2 / +3 channel count
        if ((head >> 24) == 0u) return;
        const std::int32_t channels = static_cast<std::int8_t>(head & 0xFFu);
        for (std::int32_t ch = 0; ch < channels && ch < 8; ++ch) {
            const std::uint32_t vpb = read32(rna + kRnaVoice + 4u * static_cast<std::uint32_t>(ch));
            const std::uint32_t pb = vpb != 0u ? dsp_parameter_block(read32, vpb) : 0u;
            if (pb != 0u) out.add(pb, kind);
        }
    };
    for (std::uint32_t i = 0; i < 2u; ++i) {
        adx_object(read32(kBgmObjects + 4u * i), kBgmAdxt, Kind::Music);
        adx_object(read32(kSerifObjects + 4u * i), kSerifAdxt, Kind::Voice);
    }
    const std::uint32_t context = read32(kGcaxContextPointer);
    if (context == 0u) return;
    for (std::uint32_t i = 0; i < kGcaxVoiceCount; ++i) {
        const std::uint32_t gv = context + kGcaxVoices + i * kGcaxVoiceBytes;
        const std::uint32_t flags = read32(gv + kGvFlags);
        if ((flags & 0x80000000u) == 0u || (flags & 0x40000000u) != 0u) continue;  // free, or its AX voice was dropped
        const std::uint32_t vpb = read32(gv + kGvVpb);
        if (vpb == 0u) continue;
        const std::uint32_t pb = dsp_parameter_block(read32, vpb);
        if (pb != 0u) out.add(pb, is_character_bank(read32(gv + kGvBank)) ? Kind::Voice : Kind::Sfx);
    }
}

}  // namespace gekko::audio_category
