// native_audio_mixer_tests: the native mixer (runtime/include/native_audio_mixer.h) against independent models
// written from the specifications (docs/audio-engine-2026-09-17/cleanroom/05_DSP_MICROCODE_SEMANTICS.md), not
// copied from the mixer's source.
//
// AU step 3. SRC, src_type 1 (05 section 7.2), modelled the way the section states it: per ms of 32 outputs, N new
// samples are appended to the 4-sample history window, the phase is advanced before each output, the two newest
// window taps give y = sat16(s0 + round(sat16(s1 - s0) * f / 65536)) with ADDPAXZ's half-to-even rounding (single
// source, see the mixer), and the window's last 4 samples carry to the next ms / frame. Other SRC modes keep the
// stand-in two-tap floor arithmetic. Envelope (7.4): v_i = env + i*delta (mod 2^16, signed), x = sat16(floor(v*y/32768)).
// Routing (7.6): a bus receives floor(x*v/32768) only when its mixer-control enable bit is set; v steps by delta each
// sample when its ramp bit is set. Output: main L/R saturated to s16. Mode 2 (5.1): main L += aux B L of the previous
// frame, main R -= aux B R of the previous frame. Mode 0 adds preceding main S
// to both channels (original-word proof in audio_output_contract_tests).

#include "native_audio_mixer.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <map>
#include <random>
#include <vector>

using namespace shadow::audio;

namespace {

int g_failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            ++g_failures;                                                           \
        }                                                                           \
    } while (0)

std::int64_t floor_div(std::int64_t n, std::int64_t d) {
    std::int64_t q = n / d;
    if ((n % d != 0) && ((n < 0) != (d < 0))) --q;
    return q;
}
std::int64_t sat16(std::int64_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : v; }

struct ModelVoice {
    std::uint32_t slot = 0;
    bool restart = false;
    std::vector<std::int64_t> start_history = {0, 0, 0, 0};
    std::vector<std::int16_t> x;
    std::uint32_t ratio = 0x10000, frac = 0;
    SrcMode src = SrcMode::Linear;
    std::uint16_t env = 0x7FFF, env_delta = 0, mc = 0x3;
    std::uint16_t vol[kBusCount] = {}, delta[kBusCount] = {};
};

const struct { Bus bus; std::uint16_t enable, ramp; } kBits[] = {
    {kMainL, 0x1, 0x8}, {kMainR, 0x2, 0x8}, {kMainS, 0x4, 0x8}, {kAuxAL, 0x10, 0x40}, {kAuxAR, 0x20, 0x40},
    {kAuxAS, 0x80, 0x100}, {kAuxBL, 0x200, 0x800}, {kAuxBR, 0x400, 0x800}, {kAuxBS, 0x1000, 0x2000}};

struct Model {
    std::int64_t prev_bl[kFrameSamples] = {}, prev_br[kFrameSamples] = {}, prev_s[kFrameSamples] = {};
    std::map<std::uint32_t, std::vector<std::int64_t>> history;
    // The room path (test_room_effect_against_model): this block's aux A send, and the return it adds.
    std::array<std::array<std::int32_t, kFrameSamples>, 3> room_send{};
    const std::array<std::array<std::int32_t, kFrameSamples>, 3>* room_return = nullptr;

    std::vector<std::int64_t> src(const ModelVoice& v) {
        std::vector<std::int64_t>& h = history[v.slot];
        if (h.size() != 4) h.assign(4, 0);  // a slot never mixed before starts from silence
        if (v.restart) h = v.start_history;
        std::vector<std::int64_t> y;
        std::size_t next = 0;
        auto read = [&]() -> std::int64_t { return next < v.x.size() ? v.x[next++] : (next++, 0); };
        if (v.src != SrcMode::Linear) {
            for (std::uint32_t i = 0; i < kFrameSamples; ++i) {
                const std::uint64_t p = std::uint64_t(i) * v.ratio + v.frac;
                const std::uint64_t f = v.src == SrcMode::None ? 0 : (p & 0xFFFF);
                auto at = [&](std::uint64_t k) -> std::int64_t { return k < v.x.size() ? v.x[k] : 0; };
                const std::int64_t a = at(p >> 16), b = f ? at((p >> 16) + 1) : a;
                y.push_back(floor_div(a * (65536 - std::int64_t(f)) + b * std::int64_t(f), 65536));
            }
        }
        std::uint32_t frac = v.frac;
        for (int ms = 0; ms < 5; ++ms) {
            const std::uint64_t n = (frac + 32ull * v.ratio) >> 16;
            std::vector<std::int64_t> w(h);
            for (std::uint64_t k = 0; k < n; ++k) w.push_back(read());
            if (v.src == SrcMode::Linear) {
                for (std::uint32_t i = 0; i < 32; ++i) {
                    const std::uint64_t t = frac + std::uint64_t(i + 1) * v.ratio;
                    const std::uint64_t k = t >> 16;
                    const std::int64_t f = std::int64_t(t & 0xFFFF);
                    const std::int64_t s0 = w[k + 2], d = sat16(w[k + 3] - s0);
                    const std::int64_t p = d * f;
                    y.push_back(sat16(s0 + floor_div(p + ((p & 0x10000) ? 0x8000 : 0x7FFF), 65536)));
                }
            }
            h.assign(w.end() - 4, w.end());
            frac = std::uint32_t((frac + 32ull * v.ratio) & 0xFFFF);
        }
        return y;
    }

    void frame(const std::vector<ModelVoice>& voices, bool dolby, StereoFrame& out) {
        std::int64_t bus[kBusCount][kFrameSamples] = {};
        for (const ModelVoice& v : voices) {
            const std::vector<std::int64_t> y = src(v);
            for (std::uint32_t i = 0; i < kFrameSamples; ++i) {
                const std::int64_t ve = std::int16_t(std::uint16_t(v.env + i * v.env_delta));
                const std::int64_t s = sat16(floor_div(ve * y[i], 32768));
                for (const auto& r : kBits) {
                    if (!(v.mc & r.enable)) continue;
                    const std::int64_t vol = (v.mc & r.ramp) ? std::uint16_t(v.vol[r.bus] + i * v.delta[r.bus])
                                                             : v.vol[r.bus];
                    bus[r.bus][i] += floor_div(s * vol, 32768);
                }
            }
        }
        for (std::uint32_t i = 0; i < kFrameSamples; ++i) {
            for (unsigned c = 0; c < 3u; ++c) room_send[c][i] = std::int32_t(bus[kAuxAL + c][i]);
            std::int64_t l = bus[kMainL][i], r = bus[kMainR][i], s = bus[kMainS][i];
            if (dolby) {
                l += prev_bl[i]; r -= prev_br[i];
                prev_bl[i] = bus[kAuxBL][i]; prev_br[i] = bus[kAuxBR][i];
            } else { l += prev_s[i]; r += prev_s[i]; }
            if (room_return != nullptr) {
                l += (*room_return)[0][i]; r += (*room_return)[1][i]; s += (*room_return)[2][i];
            }
            prev_s[i] = s;
            out.left[i] = std::int16_t(sat16(l));
            out.right[i] = std::int16_t(sat16(r));
        }
    }
};

VoiceFrame to_frame(const ModelVoice& m) {
    VoiceFrame f;
    f.slot = m.slot;
    f.restart = m.restart;
    for (int j = 0; j < 4; ++j) f.start_history[j] = std::int16_t(m.start_history[j]);
    f.source = SourceView{m.x.data(), std::uint32_t(m.x.size())};
    f.ratio = m.ratio; f.frac = m.frac; f.src = m.src;
    f.envelope = m.env; f.envelope_delta = m.env_delta; f.mixer_control = m.mc;
    for (std::uint32_t b = 0; b < kBusCount; ++b) { f.volume[b].value = m.vol[b]; f.volume[b].delta = m.delta[b]; }
    return f;
}

void test_phase_is_two_minus_ratio() {
    // Ramp source x[k] = 64k at ratio 1.0, zero history: output i is the window sample at (i+1) - 2 + 4 - 4, i.e. the
    // source sample i-1: the microcode lags the stand-in by (2 - ratio) source samples (AU_REPORT.md 3.5).
    ModelVoice v;
    v.x.resize(400);
    for (std::size_t k = 0; k < v.x.size(); ++k) v.x[k] = std::int16_t(64 * k);
    v.restart = true; v.mc = 0x1; v.vol[kMainL] = 0x7FFF;
    VoiceFrame f = to_frame(v);
    Mixer mixer; StereoFrame out; MixStats stats;
    mixer.mix(FrameSettings{}, &f, 1, out, stats);
    const auto gains = [](std::int64_t y) { return sat16(floor_div(sat16(floor_div(y * 0x7FFF, 32768)) * 0x7FFF, 32768)); };
    CHECK(out.left[0] == 0);
    CHECK(out.left[5] == gains(64 * 4));
    CHECK(out.left[159] == gains(64 * 158));
}

void test_rounding_and_saturation() {
    // ratio 0.5, history {0,0,0,32767} and source {-32768, ...}: the tap difference -65535 saturates to -32768;
    // t_0 = 0.5: k = 0, s0 = w[2] = 0, s1 = w[3] = 32767 -> d = 32767, f = 0x8000: p = 32767 * 32768 = 0x3FFF8000,
    // bit 16 set -> +0x8000 -> 16384 (half to even: 16383.5 -> 16384).
    ModelVoice v;
    v.x.assign(400, -32768);
    v.start_history = {0, 0, 0, 32767};
    v.restart = true; v.ratio = 0x8000; v.mc = 0x1; v.vol[kMainL] = 0x7FFF; v.env = 0x7FFF;
    Model model; StereoFrame want;
    model.frame({v}, false, want);
    VoiceFrame f = to_frame(v);
    Mixer mixer; StereoFrame out; MixStats stats;
    mixer.mix(FrameSettings{}, &f, 1, out, stats);
    CHECK(out.left == want.left);
    const std::int64_t y0 = 16384;
    CHECK(out.left[0] == sat16(floor_div(sat16(floor_div(y0 * 0x7FFF, 32768)) * 0x7FFF, 32768)));
}

void test_random_against_model(bool dolby) {
    std::mt19937 rng(dolby ? 0xD0B1u : 20261019u);
    std::uniform_int_distribution<int> sample(-32768, 32767);
    const std::uint32_t ratios[] = {0x10000, 0x18000, 0xB066, 0x7CBB, 0x5833, 0x40000, 0x8426, 0xC000, 1u, 0x3FFFF};
    int mismatches = 0;
    Mixer mixer;
    Model model;
    FrameSettings settings;
    settings.mode = dolby ? OutputMode::DolbyMatrix : OutputMode::StereoFold;
    for (int trial = 0; trial < 400; ++trial) {
        std::vector<ModelVoice> voices;
        const int n = 1 + int(rng() % 12u);
        for (int k = 0; k < n; ++k) {
            ModelVoice v;
            v.slot = std::uint32_t(k);
            v.restart = trial == 0 || rng() % 6u == 0u;
            for (auto& h : v.start_history) h = sample(rng);
            v.ratio = ratios[rng() % 10u];
            v.frac = rng() & 0xFFFFu;
            v.src = (rng() % 5u == 0u) ? SrcMode::None : SrcMode::Linear;
            v.env = std::uint16_t(rng() % 3u == 0u ? 0x7FFF : rng() & 0xFFFFu);
            v.env_delta = std::uint16_t(rng() % 3u == 0u ? rng() & 0xFFFFu : 0);
            v.mc = std::uint16_t(rng() & 0x3FFFu);
            for (std::uint32_t b = 0; b < kBusCount; ++b) {
                v.vol[b] = std::uint16_t(rng() % 4u == 0u ? 0 : rng() & 0xFFFFu);
                v.delta[b] = std::uint16_t(rng() % 2u ? 0 : rng() & 0xFFFFu);
            }
            const std::uint64_t reach = ((std::uint64_t(kFrameSamples) * v.ratio + v.frac) >> 16) + 1u;
            v.x.resize(rng() % 4u == 0u ? std::size_t(rng() % (reach + 1u)) : std::size_t(reach));
            for (auto& s : v.x) s = std::int16_t(rng() % 7u == 0u ? (rng() & 1u ? 32767 : -32768) : sample(rng));
            voices.push_back(std::move(v));
        }
        std::vector<VoiceFrame> frames;
        for (const ModelVoice& v : voices) frames.push_back(to_frame(v));
        StereoFrame got, want; MixStats stats;
        mixer.mix(settings, frames.data(), std::uint32_t(frames.size()), got, stats);
        model.frame(voices, dolby, want);
        if (got.left != want.left || got.right != want.right) ++mismatches;
    }
    CHECK(mismatches == 0);
    std::printf("  random frames vs model (%s): 400 frames, %d mismatches\n", dolby ? "mode 2" : "mode 0", mismatches);
}

void test_dolby_sign() {
    ModelVoice v;
    v.x.assign(400, 1000);
    v.start_history = {1000, 1000, 1000, 1000};
    v.restart = true;
    v.mc = 0x1 | 0x200 | 0x400; v.vol[kMainL] = 0x4000; v.vol[kAuxBL] = 0x7FFF; v.vol[kAuxBR] = 0x7FFF;
    VoiceFrame f = to_frame(v);
    FrameSettings s; s.mode = OutputMode::DolbyMatrix;
    Mixer mixer; StereoFrame f1, f2; MixStats stats;
    mixer.mix(s, &f, 1, f1, stats);
    f.restart = false;
    mixer.mix(s, &f, 1, f2, stats);
    const std::int64_t y = floor_div(1000 * 0x7FFF, 32768);
    CHECK(f1.left[7] == floor_div(y * 0x4000, 32768) && f1.right[7] == 0);
    CHECK(f2.left[7] == floor_div(y * 0x4000, 32768) + floor_div(y * 0x7FFF, 32768));
    CHECK(f2.right[7] == -floor_div(y * 0x7FFF, 32768));
}

void test_clamp_and_peak() {
    std::vector<ModelVoice> voices;
    for (int k = 0; k < 7; ++k) {
        ModelVoice v;
        v.slot = std::uint32_t(k);
        v.restart = true;
        const std::int16_t level = std::int16_t(k % 2 ? 32767 : -32768);
        v.start_history = {level, level, level, level};
        v.x.assign(400, level);
        v.vol[kMainL] = 0xFFFF; v.vol[kMainR] = 0xFFFF;
        voices.push_back(v);
    }
    std::vector<VoiceFrame> frames;
    for (const ModelVoice& v : voices) frames.push_back(to_frame(v));
    Mixer mixer; StereoFrame out; MixStats stats;
    mixer.mix(FrameSettings{}, frames.data(), std::uint32_t(frames.size()), out, stats);
    CHECK(out.left[3] == -32768 && out.right[3] == -32768 && stats.peak == 32768);
}

// ---- the compressor (05 section 10) ----
// Model: per frame, triggered = any |main L or R| >= threshold (s32 before the output clamp); triggered -> row =
// counter, counter = release; else counter > 0 -> counter -= 1, row = 11 + counter; else no gain. Gain: floor(x*g/32768).
struct CompressorModel {
    int counter = 0;
    int row_for(const std::vector<std::int64_t>& l, const std::vector<std::int64_t>& r, int threshold, int release) {
        bool trig = false;
        for (std::size_t i = 0; i < l.size(); ++i) trig |= (l[i] < 0 ? -l[i] : l[i]) >= threshold || (r[i] < 0 ? -r[i] : r[i]) >= threshold;
        if (trig) { const int row = counter; counter = release; return row; }
        if (counter > 0) { --counter; return 11 + counter; }
        return -1;
    }
};

// Two voices per channel, SRC "none" at ratio 1.0 (output i = source i), envelope 0x8000 (-1.0) and volume 0x8000
// (1.0): each contributes sat16(-y) = -y, so source -floor(s/2) and -(s - floor(s/2)) put exactly s on the bus.
void bus_voices(const std::vector<std::int64_t>& target, Bus bus, std::uint32_t slot0, std::vector<std::vector<std::int16_t>>& store,
                std::vector<VoiceFrame>& frames) {
    std::vector<std::int16_t> a(kFrameSamples + 2), b(kFrameSamples + 2);
    for (std::uint32_t i = 0; i < kFrameSamples; ++i) {
        const std::int64_t half = floor_div(target[i], 2);
        a[i] = std::int16_t(-half);
        b[i] = std::int16_t(-(target[i] - half));
    }
    store.push_back(a); store.push_back(b);
    for (int k = 0; k < 2; ++k) {
        VoiceFrame f;
        f.slot = slot0 + std::uint32_t(k);
        const auto& src = store[store.size() - 2 + std::size_t(k)];
        f.source = SourceView{src.data(), std::uint32_t(src.size())};
        f.ratio = 0x10000; f.frac = 0; f.src = SrcMode::None;
        f.envelope = 0x8000;
        f.mixer_control = std::uint16_t(bus == kMainL ? 0x1 : 0x2);
        f.volume[bus].value = 0x8000;
        frames.push_back(f);
    }
}

void test_compressor_table() {
    // The game's table (main.dol 0x80563420; 05 section 10): attack rows start 32673 .. 20675 and end at 20675,
    // release rows 11..20 run 31302 -> 32768 .. 20681 -> 21649.
    const auto& t = compressor_gain_table();
    const int starts[21] = {32673, 31212, 29816, 28482, 27208, 25991, 24828, 23717, 22656, 21643, 20675,
                            31302, 29893, 28547, 27263, 26036, 24864, 23745, 22676, 21655, 20681};
    const int ends[21] = {20675, 20675, 20675, 20675, 20675, 20675, 20675, 20675, 20675, 20675, 20675,
                          32768, 31293, 29884, 28539, 27255, 26028, 24857, 23738, 22669, 21649};
    for (int r = 0; r < 21; ++r) {
        CHECK(t[std::size_t(r) * 160] == starts[r]);
        CHECK(t[std::size_t(r) * 160 + 159] == ends[r]);
    }
}

void test_compressor_against_model() {
    std::mt19937 rng(0xC0C0u);
    std::uniform_int_distribution<int> quiet(-20000, 20000), loud(-60000, 60000);
    Mixer mixer;
    CompressorModel model;
    FrameSettings s;
    s.compressor = true;
    int mismatches = 0, triggers = 0, rows_seen[21] = {};
    for (int frame = 0; frame < 2000; ++frame) {
        // Loud frames at random (gaps of every length, so the counter is at every value when one triggers), plus
        // exact-threshold edges.
        const bool is_loud = rng() % 6u == 0u;
        std::vector<std::int64_t> l(kFrameSamples), r(kFrameSamples);
        for (std::uint32_t i = 0; i < kFrameSamples; ++i) {
            l[i] = is_loud ? loud(rng) : quiet(rng);
            r[i] = quiet(rng);
        }
        if (frame % 50 == 7) l[rng() % kFrameSamples] = 32768;   // exactly the threshold: triggers (>=)
        if (frame % 50 == 9) r[rng() % kFrameSamples] = -32768;  // |-32768| = threshold
        if (frame % 50 == 11) { for (auto& v : l) v = 32767; for (auto& v : r) v = -32767; }  // one below: no trigger
        std::vector<std::vector<std::int16_t>> store;
        store.reserve(4);
        std::vector<VoiceFrame> frames;
        bus_voices(l, kMainL, 0, store, frames);
        bus_voices(r, kMainR, 2, store, frames);
        StereoFrame out; MixStats stats;
        mixer.mix(s, frames.data(), std::uint32_t(frames.size()), out, stats);
        const int row = model.row_for(l, r, 0x8000, 10);
        if (row >= 0) ++rows_seen[row];
        if (stats.compressor_triggered) ++triggers;
        bool same = stats.compressor_row == row;
        const auto& t = compressor_gain_table();
        for (std::uint32_t i = 0; i < kFrameSamples && same; ++i) {
            const std::int64_t gl = row < 0 ? l[i] : floor_div(l[i] * t[std::size_t(row) * 160 + i], 32768);
            const std::int64_t gr = row < 0 ? r[i] : floor_div(r[i] * t[std::size_t(row) * 160 + i], 32768);
            same = out.left[i] == sat16(gl) && out.right[i] == sat16(gr);
        }
        if (!same) ++mismatches;
    }
    int rows_used = 0;
    for (int k = 0; k < 21; ++k) rows_used += rows_seen[k] != 0;
    CHECK(mismatches == 0);
    CHECK(rows_used == 21);  // every attack and release row exercised
    std::printf("  compressor vs model: 2000 frames, %d triggered, %d of 21 rows used, %d mismatches\n", triggers,
                rows_used, mismatches);
}


void test_checkpoint_history() {
    Mixer running, restored, reset_control;
    FrameSettings settings; settings.mode = OutputMode::DolbyMatrix;
    settings.compressor = true; settings.compressor_threshold = 4096;
    ModelVoice voice; voice.x.resize(700); voice.mc = 0x603;
    voice.ratio = 0xB066; voice.frac = 0x9182;
    voice.vol[kMainL] = voice.vol[kMainR] = 0x7FFF;
    voice.vol[kAuxBL] = voice.vol[kAuxBR] = 0x4000;
    std::mt19937 rng(0xC0FFEE);
    unsigned reset_differences = 0, releases = 0, returns = 0, histories = 0;
    for (unsigned frame = 0; frame < 100; ++frame) {
        const auto saved = running.history();
        releases += saved.compressor_release != 0;
        returns += std::any_of(saved.aux_b_return[0].begin(), saved.aux_b_return[0].end(), [](auto x) { return x != 0; });
        histories += std::any_of(saved.source[0].begin(), saved.source[0].end(), [](auto x) { return x != 0; });
        CHECK(restored.restore_history(saved));
        auto invalid = saved; invalid.compressor_release = 11;
        CHECK(!restored.restore_history(invalid));
        for (auto& sample : voice.x) sample = static_cast<std::int16_t>(rng() & 0xffffu);
        voice.env = frame % 13u == 0u ? 0x7FFF : 0x100;
        const auto input = to_frame(voice);
        StereoFrame want, actual, cold; MixStats ws, as, cs;
        running.mix(settings, &input, 1, want, ws);
        restored.mix(settings, &input, 1, actual, as);
        reset_control.reset(); reset_control.mix(settings, &input, 1, cold, cs);
        CHECK(want.left == actual.left && want.right == actual.right);
        CHECK(ws.compressor_row == as.compressor_row && ws.compressor_triggered == as.compressor_triggered);
        reset_differences += want.left != cold.left || want.right != cold.right;
    }
    CHECK(releases && returns && histories && reset_differences);
    std::printf("  checkpoint: 100 exact mixer continuations, %u reset controls differed\n", reset_differences);
}


void test_surround_output_history() {
    unsigned continuations = 0;
    for (auto mode : {OutputMode::StereoFold, OutputMode::OppositeFold}) {
        Mixer running, restored;
        FrameSettings settings; settings.mode = mode;
        ModelVoice voice; voice.src = SrcMode::None; voice.mc = 0x4;
        voice.vol[kMainS] = 0x8000; voice.x.assign(160, 10000);
        auto input = to_frame(voice);
        StereoFrame first, next, replay, cold; MixStats stats;
        running.mix(settings, &input, 1, first, stats);
        CHECK(std::all_of(first.left.begin(), first.left.end(), [](auto v) { return v == 0; }));
        const auto saved = running.history();
        CHECK(saved.main_surround[0] == 9999);
        CHECK(restored.restore_history(saved));
        running.mix(settings, nullptr, 0, next, stats);
        restored.mix(settings, nullptr, 0, replay, stats);
        CHECK(next.left == replay.left && next.right == replay.right);
        for (unsigned i = 0; i < kFrameSamples; ++i) {
            CHECK(next.right[i] == 9999);
            CHECK(next.left[i] == (mode == OutputMode::StereoFold ? 9999 : -9999));
        }
        restored.reset(); restored.mix(settings, nullptr, 0, cold, stats);
        CHECK(next.left != cold.left && next.right != cold.right);
        running.mix(settings, nullptr, 0, next, stats);
        CHECK(next.left == cold.left && next.right == cold.right);
        ++continuations;
    }
    std::printf("  surround: %u exact one-block continuations; cold controls differ; empty tail clears\n", continuations);
}


// Full signed-sample and fractional domains against the existing wide model.
// Endpoints include signed envelope overflow, unsigned gains and ramp wrap.
void test_sample_width_edges() {
    std::uint64_t checked = 0;
    const auto compare = [&](const ModelVoice& v) {
        Model model; Mixer mixer; StereoFrame want, got; MixStats stats;
        model.frame({v}, false, want);
        const VoiceFrame f = to_frame(v);
        FrameSettings settings; settings.mode = OutputMode::StereoFold;
        mixer.mix(settings, &f, 1, got, stats);
        CHECK(got.left == want.left && got.right == want.right);
        const auto actual = mixer.history().source[v.slot];
        const auto& expected = model.history[v.slot];
        for (unsigned j = 0; j < 4; ++j) CHECK(actual[j] == expected[j]);
        checked += kFrameSamples;
    };
    const std::uint16_t edges[] = {0, 1, 0x7FFF, 0x8000, 0xFFFF};
    for (const auto env : edges) for (const auto delta : edges) {
        for (unsigned begin = 0; begin < 65536; begin += kFrameSamples) {
            ModelVoice v; v.restart = true; v.src = SrcMode::None;
            v.env = env; v.env_delta = delta; v.mc = 0xB;
            v.vol[kMainL] = 0xFFFF; v.vol[kMainR] = 0x8000;
            v.delta[kMainL] = delta; v.delta[kMainR] = std::uint16_t(-delta);
            v.x.resize(kFrameSamples);
            for (unsigned i = 0; i < kFrameSamples; ++i)
                v.x[i] = std::int16_t(std::uint16_t(begin + i));
            compare(v);
        }
    }
    const std::int16_t taps[] = {-32768, 0, 32767};
    for (const auto a : taps) for (const auto b : taps) {
        for (unsigned begin = 0; begin < 65536; begin += kFrameSamples) {
            ModelVoice v; v.restart = true; v.ratio = 1; v.frac = begin;
            v.start_history = {a, b, a, b}; v.x = {a, b, a, b};
            v.env = 0x7FFF; v.vol[kMainL] = 0xFFFF; v.vol[kMainR] = 1;
            compare(v);
        }
    }
    std::printf("  sample widths: %llu output samples plus source history vs wide model\n",
                static_cast<unsigned long long>(checked));
}

// A wrapped 32-bit addition would reverse the sign at these endpoints.
// Keep the real restored-history path in this test, not just a SIMD helper.
void test_restored_bus_saturation() {
    const std::int32_t edges[] = {INT32_MIN, INT32_MIN + 1, -65536, -1, 0, 1, 65536, INT32_MAX - 1, INT32_MAX};
    for (const std::int16_t sample : {std::int16_t(-32768), std::int16_t(32767)}) {
        Mixer mixer; Model model; ModelVoice voice; StereoFrame got, want; MixStats stats;
        auto saved = mixer.history();
        for (unsigned i = 0; i < kFrameSamples; ++i)
            model.prev_s[i] = saved.main_surround[i] = edges[i % 9];
        CHECK(mixer.restore_history(saved));
        voice.restart = true; voice.src = SrcMode::None; voice.env = 0x7FFF;
        voice.vol[kMainL] = 0xFFFF; voice.vol[kMainR] = 0x8000;
        voice.x.assign(kFrameSamples, sample);
        const auto frame = to_frame(voice);
        FrameSettings settings; settings.mode = OutputMode::StereoFold;
        mixer.mix(settings, &frame, 1, got, stats);
        model.frame({voice}, false, want);
        CHECK(got.left == want.left && got.right == want.right);
    }
}

// Optional isolated CPU workload. This is not a game FPS benchmark or an
// original-DSP oracle. Hash every output block; run alternating binary pairs.
int benchmark_mixer() {
    std::mt19937 random(0xA0D1024u);
    constexpr unsigned frames = 12000;
    for (const unsigned count : {1u, 16u, 64u}) {
        std::array<std::array<std::int16_t, 650>, kMaxVoices> sources{};
        std::array<VoiceFrame, kMaxVoices> voices{};
        for (unsigned v = 0; v < count; ++v) {
            for (auto& sample : sources[v]) sample = std::int16_t(random());
            auto& voice = voices[v]; voice.slot = v;
            voice.source = {sources[v].data(), unsigned(sources[v].size())};
            voice.ratio = (v % 3 == 0 ? 0x10000u : v % 3 == 1 ? 0xB066u : 0x18000u);
            voice.frac = random() & 0xFFFFu; voice.envelope = 0x7FFF;
            voice.envelope_delta = v % 4 == 0 ? 1 : 0;
            voice.mixer_control = v % 4 == 0 ? 0x3FFF : 0x237;
            for (auto& gain : voice.volume) { gain.value = std::uint16_t(2048 + random() % 8192); gain.delta = 1; }
        }
        FrameSettings settings; settings.mode = OutputMode::StereoFold; settings.compressor = true;
        Mixer mixer; StereoFrame out; MixStats stats;
        for (unsigned i = 0; i < 200; ++i) mixer.mix(settings, voices.data(), count, out, stats);
        mixer.reset();
        std::uint64_t digest = 1469598103934665603ull;
        const auto start = std::chrono::steady_clock::now();
        for (unsigned i = 0; i < frames; ++i) {
            mixer.mix(settings, voices.data(), count, out, stats);
            for (unsigned j = 0; j < kFrameSamples; ++j) {
                digest = (digest ^ std::uint16_t(out.left[j])) * 1099511628211ull;
                digest = (digest ^ std::uint16_t(out.right[j])) * 1099511628211ull;
            }
        }
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count();
        std::printf("{\"voices\":%u,\"frames\":%u,\"ns\":%lld,\"digest\":\"%016llx\"}\n",
                    count, frames, static_cast<long long>(ns), static_cast<unsigned long long>(digest));
    }
    return 0;
}

}  // namespace

// The aux A room path (step 6), modelled from its contract rather than EffectStage's code: block N sends the model's
// aux A L/R/S buses; the game's callback after block N runs the effect (the native Reverb, whose arithmetic the
// original-word contract proves) over the block N-1 send in place; block N returns the block N-2 send into main
// L/R/S before main S is kept for the next fold. The mixer runs the list-scheduled send/return and the effect pass.
void test_room_effect_against_model() {
    std::mt19937 rng(0xA0A0u);
    std::uniform_int_distribution<int> sample(-12000, 12000);
    Mixer mixer;
    Model model;
    Reverb effect;
    std::vector<std::array<std::array<std::int32_t, kFrameSamples>, 3>> sends;
    FrameSettings settings;
    settings.mode = OutputMode::StereoFold;
    settings.aux_a = true;
    int mismatches = 0;
    unsigned returned = 0;
    for (int trial = 0; trial < 120; ++trial) {
        std::vector<ModelVoice> voices;
        const int n = 1 + int(rng() % 6u);
        for (int k = 0; k < n; ++k) {
            ModelVoice v;
            v.slot = std::uint32_t(k);
            v.restart = trial == 0;
            v.mc = std::uint16_t(0x3u | (trial < 40 ? 0xB0u : 0u));  // main L/R; aux A L/R/S for 40 blocks, then a tail
            v.vol[kMainL] = v.vol[kMainR] = 0x3000;
            v.vol[kAuxAL] = std::uint16_t(rng() & 0x7FFFu);
            v.vol[kAuxAR] = std::uint16_t(rng() & 0x7FFFu);
            v.vol[kAuxAS] = std::uint16_t(rng() & 0x7FFFu);
            v.x.resize(((std::uint64_t(kFrameSamples) * v.ratio + v.frac) >> 16) + 1u);
            for (auto& s : v.x) s = std::int16_t(sample(rng));
            voices.push_back(std::move(v));
        }
        std::vector<VoiceFrame> frames;
        for (const ModelVoice& v : voices) frames.push_back(to_frame(v));
        StereoFrame got, want; MixStats stats;
        mixer.mix(settings, frames.data(), std::uint32_t(frames.size()), got, stats);
        CHECK(mixer.effect().process());

        // The model: its own buses (Model::frame's routing), plus the room path with the main S return.
        const std::size_t block = sends.size();
        const std::array<std::array<std::int32_t, kFrameSamples>, 3> none{};
        const auto& back = block >= 2 ? sends[block - 2] : none;
        model.room_return = &back;
        model.frame(voices, false, want);
        model.room_return = nullptr;
        for (const auto& channel : back) returned += std::any_of(channel.begin(), channel.end(), [](auto x) { return x; });
        sends.push_back(model.room_send);  // may reallocate: `back` is not used after this
        if (block >= 1) {
            auto& pending = sends[block - 1];
            effect.process({pending[0].data(), pending[1].data(), pending[2].data()}, kFrameSamples);
        }
        if (got.left != want.left || got.right != want.right) ++mismatches;
    }
    CHECK(mismatches == 0 && returned > 100u);
    std::printf("  room effect vs model: 120 blocks, %u nonzero returned channels, %d mismatches\n", returned, mismatches);
}

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--benchmark") == 0) return benchmark_mixer();
    std::printf("native_audio_mixer_tests\n");
    test_phase_is_two_minus_ratio();
    test_rounding_and_saturation();
    test_sample_width_edges();
    test_restored_bus_saturation();
    test_random_against_model(false);
    test_random_against_model(true);
    test_dolby_sign();
    test_clamp_and_peak();
    test_compressor_table();
    test_compressor_against_model();
    test_checkpoint_history();
    test_surround_output_history();
    test_room_effect_against_model();
    if (g_failures) {
        std::printf("FAIL (%d)\n", g_failures);
        return 1;
    }
    std::printf("PASS\n");
    return 0;
}
