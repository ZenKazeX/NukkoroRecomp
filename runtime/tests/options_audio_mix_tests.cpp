// options_audio_mix_tests: the per-category volume path (Options > Audio: Music, Sound effects, Voices).
//
// Covers gekko/audio_categories.h (percent -> Q15, the guest-table walk that tells music / voice / sfx AX voices apart)
// and the native mixer's per-voice gain (native_audio_mixer.h VoiceFrame::gain): unity is bit-identical to the
// untouched path, a gain scales the voice after its envelope (and before its bus sends), 0 silences it, and the
// category of a voice picks the gain. The guest tables are modelled by a fake word map laid out exactly as the header
// documents them (addresses and offsets of main.dol GUPP8P).
//
// Sources to link: runtime/src/native_audio_mixer.cpp, runtime/lift/audio/{reverb,output_stage,effect_stage}.cpp.
// Include paths: runtime/include, runtime/lift. Nothing else (no guest, no device). Example (clang-cl or MSVC, C++20):
//   cl /std:c++latest /EHsc /O2 /arch:AVX2 /I runtime\include /I runtime\lift runtime\tests\options_audio_mix_tests.cpp
//      runtime\src\native_audio_mixer.cpp runtime\lift\audio\reverb.cpp runtime\lift\audio\output_stage.cpp
//      runtime\lift\audio\effect_stage.cpp

#include <cstdint>
#include <cstdio>
#include <map>
#include <vector>

#include "gekko/audio_categories.h"
#include "native_audio_mixer.h"

using namespace shadow::audio;
namespace cat = gekko::audio_category;

namespace {

int g_failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

// ---- the guest's tables, as a word map -------------------------------------------------------------------------------
struct FakeGuest {
    std::map<std::uint32_t, std::uint32_t> words;
    std::uint32_t operator()(std::uint32_t address) const {
        const auto it = words.find(address);
        return it == words.end() ? 0u : it->second;
    }
    void put(std::uint32_t address, std::uint32_t value) { words[address] = value; }

    // Every AXVPB gets the next slot of the DSP PB array (word +0x18), as AXInit lays them out.
    std::map<std::uint32_t, std::uint32_t> slots;
    void vpb(std::uint32_t address) {
        if (slots.count(address) != 0) return;
        const std::uint32_t slot = static_cast<std::uint32_t>(slots.size());
        slots[address] = slot;
        put(address + cat::kVpbIndex, slot);
    }
    // The DSP parameter block the mixer's voice list carries for that AXVPB.
    std::uint32_t pb(std::uint32_t address) const {
        const auto it = slots.find(address);
        return it == slots.end() ? 0x80FFFFF0u : cat::kDspPbBase + it->second * cat::kDspPbStride;
    }

    // An ADX handle: object -> ADXT -> rna (used, channels) -> AXVPBs.
    void adx(std::uint32_t objects_slot, std::uint32_t object, std::uint32_t adxt_offset, std::uint32_t adxt,
             std::uint32_t rna, std::uint8_t used, std::int8_t channels, std::vector<std::uint32_t> vpbs) {
        put(objects_slot, object);
        put(object + adxt_offset, adxt);
        put(adxt + cat::kAdxtRna, rna);
        put(rna + cat::kRnaUsed, (std::uint32_t(used) << 24) | (std::uint32_t(channels) & 0xFFu));
        for (std::size_t k = 0; k < vpbs.size(); ++k) {
            put(rna + cat::kRnaVoice + 4u * std::uint32_t(k), vpbs[k]);
            vpb(vpbs[k]);
        }
    }
    // A GCAX voice i of the context.
    void gcax(std::uint32_t context, std::uint32_t i, std::uint32_t flags, std::uint32_t vpb_address, std::uint32_t bank) {
        put(cat::kGcaxContextPointer, context);
        const std::uint32_t gv = context + cat::kGcaxVoices + i * cat::kGcaxVoiceBytes;
        put(gv + cat::kGvFlags, flags);
        put(gv + cat::kGvVpb, vpb_address);
        vpb(vpb_address);
        put(gv + cat::kGvBank, bank);
    }
};

void test_percent_to_gain() {
    CHECK(cat::gain_from_percent(100) == cat::kUnityGain);
    CHECK(cat::kUnityGain == kUnityVoiceGain);
    CHECK(cat::gain_from_percent(0) == 0u);
    CHECK(cat::gain_from_percent(50) == 0x4000u);
    CHECK(cat::gain_from_percent(5) == 1638u);
    CHECK(cat::gain_from_percent(-7) == 0u);
    CHECK(cat::gain_from_percent(250) == cat::kUnityGain);
    std::uint32_t last = 0;
    bool monotonic = true;
    for (int p = 0; p <= 100; ++p) {
        const std::uint32_t g = cat::gain_from_percent(p);
        monotonic = monotonic && g >= last && g <= cat::kUnityGain;
        last = g;
    }
    CHECK(monotonic);
}

void test_settings_slots() {
    cat::set_percent(cat::Kind::Music, 100);
    cat::set_percent(cat::Kind::Sfx, 100);
    cat::set_percent(cat::Kind::Voice, 100);
    CHECK(cat::all_unity());
    cat::set_percent(cat::Kind::Music, 40);
    CHECK(!cat::all_unity());
    CHECK(cat::gain(cat::Kind::Music) == cat::gain_from_percent(40));
    CHECK(cat::gain(cat::Kind::Sfx) == cat::kUnityGain && cat::gain(cat::Kind::Voice) == cat::kUnityGain);
    CHECK(cat::gain(cat::Kind::Other) == cat::kUnityGain);  // an unattributed voice is never scaled
    cat::set_percent(cat::Kind::Other, 0);                  // has no slot: no effect
    CHECK(cat::gain(cat::Kind::Other) == cat::kUnityGain);
    cat::set_percent(cat::Kind::Music, 100);
    CHECK(cat::all_unity());
}

void test_category_routing() {
    FakeGuest g;
    // Music: BGM object 0, stereo ADX (two AX voices). Voice: SERIF object 1, mono. A stale BGM object 1 (rna unused).
    g.adx(cat::kBgmObjects + 0, 0x80700000u, cat::kBgmAdxt, 0x80710000u, 0x805C3E4Cu, 1, 2, {0x80800000u, 0x80800200u});
    g.adx(cat::kBgmObjects + 4, 0x80700100u, cat::kBgmAdxt, 0x80710100u, 0x805C3E4Cu + 0xECu, 0, 2, {0x80800400u});
    g.adx(cat::kSerifObjects + 4, 0x80700200u, cat::kSerifAdxt, 0x80710200u, 0x805C3E4Cu + 2 * 0xECu, 1, 1,
          {0x80800600u});
    // GCAX voices: banks 0, 1, 2, 6, 7 are effects; 3, 4, 5 are the character banks. Plus a free and a dropped voice.
    const std::uint32_t ctx = 0x80900000u, used = 0x80000000u;
    g.gcax(ctx, 0, used, 0x80A00000u, 0);
    g.gcax(ctx, 1, used, 0x80A00200u, 1);
    g.gcax(ctx, 2, used | 2u, 0x80A00400u, 2);
    g.gcax(ctx, 3, used, 0x80A00600u, 3);
    g.gcax(ctx, 4, used, 0x80A00800u, 4);
    g.gcax(ctx, 5, used, 0x80A00A00u, 5);
    g.gcax(ctx, 6, used, 0x80A00C00u, 6);
    g.gcax(ctx, 7, used, 0x80A00E00u, 7);
    g.gcax(ctx, 8, 0u, 0x80A01000u, 3);                 // free: not listed
    g.gcax(ctx, 9, used | 0x40000000u, 0x80A01200u, 3);  // its AX voice was dropped: not listed
    cat::VoiceKinds kinds;
    cat::build_voice_kinds([&](std::uint32_t a) { return g(a); }, kinds);

    CHECK(kinds.find(g.pb(0x80800000u)) == cat::Kind::Music);
    CHECK(kinds.find(g.pb(0x80800200u)) == cat::Kind::Music);
    CHECK(kinds.find(g.pb(0x80800400u)) == cat::Kind::Other);  // the unused rna handle's stale voice
    CHECK(kinds.find(g.pb(0x80800600u)) == cat::Kind::Voice);
    CHECK(kinds.find(g.pb(0x80A00000u)) == cat::Kind::Sfx);
    CHECK(kinds.find(g.pb(0x80A00200u)) == cat::Kind::Sfx);
    CHECK(kinds.find(g.pb(0x80A00400u)) == cat::Kind::Sfx);
    CHECK(kinds.find(g.pb(0x80A00600u)) == cat::Kind::Voice);
    CHECK(kinds.find(g.pb(0x80A00800u)) == cat::Kind::Voice);
    CHECK(kinds.find(g.pb(0x80A00A00u)) == cat::Kind::Voice);
    CHECK(kinds.find(g.pb(0x80A00C00u)) == cat::Kind::Sfx);
    CHECK(kinds.find(g.pb(0x80A00E00u)) == cat::Kind::Sfx);
    CHECK(kinds.find(g.pb(0x80A01000u)) == cat::Kind::Other);
    CHECK(kinds.find(g.pb(0x80A01200u)) == cat::Kind::Other);
    CHECK(kinds.find(g.pb(0x80B00000u)) == cat::Kind::Other);  // a movie soundtrack, or anything unlisted
    CHECK(kinds.count == 2u + 1u + 8u);

    // No guest at all (before the game's objects exist): an empty table, every voice Other.
    FakeGuest empty;
    cat::build_voice_kinds([&](std::uint32_t a) { return empty(a); }, kinds);
    CHECK(kinds.count == 0u && kinds.find(g.pb(0x80800000u)) == cat::Kind::Other);
    CHECK(cat::is_character_bank(3) && cat::is_character_bank(4) && cat::is_character_bank(5));
    CHECK(!cat::is_character_bank(0) && !cat::is_character_bank(2) && !cat::is_character_bank(6) &&
          !cat::is_character_bank(7));
}

// ---- the mixer ----------------------------------------------------------------------------------------------------
struct Source {
    std::vector<std::int16_t> x;
    Source() : x(400) {
        for (std::size_t k = 0; k < x.size(); ++k) x[k] = std::int16_t((k % 2 ? -1 : 1) * int(100 * (k % 97) + 7));
    }
};

VoiceFrame voice(const Source& s, std::uint32_t slot, std::uint16_t mc, std::uint16_t main_l, std::uint16_t aux_a_l) {
    VoiceFrame f;
    f.slot = slot;
    f.restart = true;
    f.source = SourceView{s.x.data(), std::uint32_t(s.x.size())};
    f.ratio = 0x10000;
    f.envelope = 0x6000;
    f.mixer_control = mc;
    f.volume[kMainL].value = main_l;
    f.volume[kMainR].value = main_l;
    f.volume[kAuxAL].value = aux_a_l;
    return f;
}

StereoFrame mix_once(const std::vector<VoiceFrame>& voices, bool aux = false) {
    Mixer mixer;
    FrameSettings settings;
    settings.aux_a = aux;
    StereoFrame out;
    MixStats stats;
    mixer.mix(settings, voices.data(), std::uint32_t(voices.size()), out, stats);
    return out;
}

bool same(const StereoFrame& a, const StereoFrame& b) { return a.left == b.left && a.right == b.right; }
bool silent(const StereoFrame& a) {
    for (std::uint32_t i = 0; i < kFrameSamples; ++i)
        if (a.left[i] != 0 || a.right[i] != 0) return false;
    return true;
}

void test_unity_is_bit_identical() {
    const Source s;
    std::vector<VoiceFrame> base;
    base.push_back(voice(s, 0, 0x3, 0x7000, 0));
    base.push_back(voice(s, 1, 0x3, 0x5555, 0));
    base.back().ratio = 0x13333;
    base.push_back(voice(s, 2, 0x3, 0x7FFF, 0));
    base.back().envelope_delta = 0xFFF0;
    CHECK(base[0].gain == kUnityVoiceGain);  // the default is unity
    const StereoFrame want = mix_once(base);
    CHECK(!silent(want));
    std::vector<VoiceFrame> explicit_unity = base;
    for (VoiceFrame& f : explicit_unity) f.gain = kUnityVoiceGain;
    CHECK(same(want, mix_once(explicit_unity)));
    // A value above unity is no boost: treated as unity.
    std::vector<VoiceFrame> above = base;
    for (VoiceFrame& f : above) f.gain = kUnityVoiceGain + 1000u;
    CHECK(same(want, mix_once(above)));
    // The same through the reverb send.
    std::vector<VoiceFrame> sends = base;
    for (VoiceFrame& f : sends) {
        f.mixer_control = 0x13;
        f.volume[kAuxAL].value = 0x3000;
    }
    const StereoFrame with_aux = mix_once(sends, true);
    for (VoiceFrame& f : sends) f.gain = kUnityVoiceGain;
    CHECK(same(with_aux, mix_once(sends, true)));
}

void test_scaling_matches_the_model() {
    // One voice, ratio 1.0, zero history: output[i] = source[i - 1] (SRC lag, see native_audio_mixer_tests). The chain
    // is envelope -> category gain -> bus volume, each floor(x * g / 32768).
    const Source s;
    const std::uint16_t env = 0x6000, bus = 0x7000;
    for (const std::uint32_t gain : {0u, 1u, 1638u, 0x2000u, 0x4000u, 0x7FFFu}) {
        VoiceFrame f = voice(s, 0, 0x1, bus, 0);
        f.gain = gain;
        const StereoFrame out = mix_once({f});
        bool ok = true;
        for (std::uint32_t i = 1; i < kFrameSamples; ++i) {
            const std::int32_t y = s.x[i - 1];
            const std::int32_t e = (y * std::int32_t(env)) >> 15;
            const std::int32_t gd = (e * std::int32_t(gain)) >> 15;
            const std::int32_t b = (gd * std::int32_t(bus)) >> 15;
            ok = ok && out.left[i] == b;
        }
        CHECK(ok);
        CHECK(out.right[5] == 0);  // main R not enabled in the mixer control
    }
}

void test_zero_silences_main_and_reverb_send() {
    const Source s;
    VoiceFrame f = voice(s, 0, 0x13, 0x7000, 0x7000);
    f.gain = 0u;
    CHECK(silent(mix_once({f}, true)));
    f.gain = kUnityVoiceGain;
    CHECK(!silent(mix_once({f}, true)));
}

void test_category_picks_the_gain() {
    // A music, a voice and an sfx voice with the same source; music 0 %, voices 50 %, sfx 100 %.
    const Source s;
    FakeGuest g;
    g.adx(cat::kBgmObjects, 0x80700000u, cat::kBgmAdxt, 0x80710000u, 0x805C3E4Cu, 1, 1, {0x80800000u});
    g.adx(cat::kSerifObjects, 0x80700200u, cat::kSerifAdxt, 0x80710200u, 0x805C3E4Cu + 0xECu, 1, 1, {0x80800200u});
    g.gcax(0x80900000u, 0, 0x80000000u, 0x80A00000u, 6);
    cat::VoiceKinds kinds;
    cat::build_voice_kinds([&](std::uint32_t a) { return g(a); }, kinds);
    cat::set_percent(cat::Kind::Music, 0);
    cat::set_percent(cat::Kind::Voice, 50);
    cat::set_percent(cat::Kind::Sfx, 100);

    VoiceFrame music = voice(s, 0, 0x1, 0x3000, 0), spoken = voice(s, 1, 0x1, 0x2000, 0), effect = voice(s, 2, 0x1, 0x1800, 0);
    music.gain = cat::gain(kinds.find(g.pb(0x80800000u)));
    spoken.gain = cat::gain(kinds.find(g.pb(0x80800200u)));
    effect.gain = cat::gain(kinds.find(g.pb(0x80A00000u)));
    CHECK(music.gain == 0u && spoken.gain == 0x4000u && effect.gain == kUnityVoiceGain);

    // The mix equals the sum of the voices mixed alone (each gain applied to its own voice only).
    const StereoFrame all = mix_once({music, spoken, effect});
    const StereoFrame m = mix_once({music}), v = mix_once({spoken}), e = mix_once({effect});
    CHECK(silent(m));
    bool ok = true;
    for (std::uint32_t i = 0; i < kFrameSamples; ++i) ok = ok && all.left[i] == v.left[i] + e.left[i];
    CHECK(ok);
    // The effect voice alone is exactly what the unscaled voice gives (sfx at 100 % is untouched).
    VoiceFrame plain = voice(s, 2, 0x1, 0x1800, 0);
    CHECK(same(e, mix_once({plain})));
    // Changing one slider moves only its category.
    cat::set_percent(cat::Kind::Sfx, 25);
    effect.gain = cat::gain(kinds.find(g.pb(0x80A00000u)));
    spoken.gain = cat::gain(kinds.find(g.pb(0x80800200u)));
    CHECK(effect.gain == 0x2000u && spoken.gain == 0x4000u);
    cat::set_percent(cat::Kind::Music, 100);
    cat::set_percent(cat::Kind::Voice, 100);
    cat::set_percent(cat::Kind::Sfx, 100);
    CHECK(cat::all_unity());
}

}  // namespace

int main() {
    test_percent_to_gain();
    test_settings_slots();
    test_category_routing();
    test_unity_is_bit_identical();
    test_scaling_matches_the_model();
    test_zero_silences_main_and_reverb_send();
    test_category_picks_the_gain();
    if (g_failures == 0) std::printf("options_audio_mix_tests: all passed\n");
    else std::printf("options_audio_mix_tests: %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
