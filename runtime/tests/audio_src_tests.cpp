// Contract of the linear SRC kernel (runtime/include/native_audio_src.h): the AVX2 kernel, the scalar kernel and the
// loop that native_audio_mixer.cpp ran before the kernel was split out (copied below as the oracle, with its helpers)
// give the same 160 outputs for random and extreme windows, phases and ratios. Negative controls: a rounding constant
// of 0x7FFF for every product, and a kernel without the saturation of the sample difference, must each be caught.
#include "native_audio_src.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {
using namespace shadow::audio;

struct Rng {
    std::uint64_t s = 0x13198A2E03707344ull;
    std::uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
};

inline std::int32_t sat16(std::int64_t v) noexcept {
    return v > 32767 ? 32767 : (v < -32768 ? -32768 : static_cast<std::int32_t>(v));
}
inline std::int64_t product_round16(std::int64_t p) noexcept { return (p + ((p & 0x10000) ? 0x8000 : 0x7FFF)) >> 16; }

// The former loop of render_voice, verbatim in arithmetic.
void original(const std::int16_t* flat, std::uint64_t frac, std::uint64_t ratio, std::int32_t* y) {
    for (std::uint32_t i = 0; i < 160; ++i) {
        const std::uint64_t t = frac + static_cast<std::uint64_t>(i + 1u) * ratio;
        const std::size_t k = static_cast<std::size_t>(t >> 16);
        const std::int64_t f = static_cast<std::int64_t>(t & 0xFFFFu);
        const std::int32_t s0 = flat[k + 2u];
        const std::int32_t d = sat16(static_cast<std::int64_t>(flat[k + 3u]) - s0);
        y[i] = sat16(s0 + product_round16(static_cast<std::int64_t>(d) * f));
    }
}
// Wrong variants.
void wrong_rounding(const std::int16_t* flat, std::uint32_t frac, std::uint32_t ratio, std::int32_t* y) {
    for (std::uint32_t i = 0; i < 160; ++i) {
        const std::uint64_t t = static_cast<std::uint64_t>(frac) + static_cast<std::uint64_t>(i + 1u) * ratio;
        const std::size_t k = static_cast<std::size_t>(t >> 16);
        const std::int64_t f = static_cast<std::int64_t>(t & 0xFFFFu);
        const std::int32_t s0 = flat[k + 2u];
        const std::int32_t d = sat16(static_cast<std::int64_t>(flat[k + 3u]) - s0);
        y[i] = sat16(s0 + ((static_cast<std::int64_t>(d) * f + 0x7FFF) >> 16));
    }
}
void wrong_saturation(const std::int16_t* flat, std::uint32_t frac, std::uint32_t ratio, std::int32_t* y) {
    for (std::uint32_t i = 0; i < 160; ++i) {
        const std::uint64_t t = static_cast<std::uint64_t>(frac) + static_cast<std::uint64_t>(i + 1u) * ratio;
        const std::size_t k = static_cast<std::size_t>(t >> 16);
        const std::int64_t f = static_cast<std::int64_t>(t & 0xFFFFu);
        const std::int32_t s0 = flat[k + 2u];
        const std::int64_t d = static_cast<std::int64_t>(flat[k + 3u]) - s0;  // no sat16
        y[i] = sat16(s0 + product_round16(d * f));
    }
}
}  // namespace

int main() {
    Rng rng;
    constexpr std::size_t kFlat = 4u + 1024u + 4u;
    std::array<std::int16_t, kFlat + 8> flat{};
    std::uint64_t cases = 0, scalar_bad = 0, vector_bad = 0, wrong_round_caught = 0, wrong_sat_caught = 0, saturated = 0;
    for (int iteration = 0; iteration < 200000; ++iteration) {
        const std::uint64_t r = rng.next();
        // ratio 0 .. just under the flat window's bound (a window holds frac + 160 * ratio < 1028 samples).
        std::uint32_t ratio;
        switch (r & 7u) {
            case 0: ratio = 0u; break;
            case 1: ratio = 0x10000u; break;                                   // 1.0
            case 2: ratio = static_cast<std::uint32_t>(rng.next() % 0x40001u); break;  // up to 4.0
            default: ratio = static_cast<std::uint32_t>(rng.next() % 0x68000u); break;  // up to 6.5
        }
        std::uint32_t frac = static_cast<std::uint32_t>(rng.next() % 0x10000u);
        if ((r & 0x700) == 0) frac = 0xFFFFu;
        const std::uint64_t reach = ((static_cast<std::uint64_t>(frac) + 160u * ratio) >> 16) + 4u;
        if (reach > kFlat) { ratio = 0x40000u; frac = 0u; }
        for (std::size_t i = 0; i < flat.size(); ++i) {
            const std::uint64_t v = rng.next();
            switch (v & 7u) {
                case 0: flat[i] = 32767; break;
                case 1: flat[i] = -32768; break;
                case 2: flat[i] = 0; break;
                default: flat[i] = static_cast<std::int16_t>(v >> 32); break;
            }
        }
        std::int32_t want[160], scalar[160], vector[160], tmp[160];
        original(flat.data(), frac, ratio, want);
        linear_src_scalar(flat.data(), frac, ratio, scalar, 160);
        std::memset(vector, 0x5A, sizeof(vector));
#if defined(__AVX2__)
        linear_src_avx2(flat.data(), frac, ratio, vector, 160);
#else
        std::memcpy(vector, scalar, sizeof(vector));
#endif
        ++cases;
        if (std::memcmp(want, scalar, sizeof(want)) != 0) ++scalar_bad;
        if (std::memcmp(want, vector, sizeof(want)) != 0) ++vector_bad;
        wrong_rounding(flat.data(), frac, ratio, tmp);
        if (std::memcmp(want, tmp, sizeof(want)) != 0) ++wrong_round_caught;
        wrong_saturation(flat.data(), frac, ratio, tmp);
        if (std::memcmp(want, tmp, sizeof(want)) != 0) ++wrong_sat_caught;
        for (int i = 0; i < 160; ++i) if (want[i] == 32767 || want[i] == -32768) ++saturated;
    }
    const bool ok = scalar_bad == 0 && vector_bad == 0 && wrong_round_caught > 0 && wrong_sat_caught > 0 && saturated > 0;
    std::printf("audio_src_tests: %llu windows (%llu saturated outputs): scalar kernel %llu, avx2 kernel %llu mismatches vs the former loop; "
                "negative controls: rounding constant caught in %llu, missing saturation in %llu: %s\n",
                static_cast<unsigned long long>(cases), static_cast<unsigned long long>(saturated),
                static_cast<unsigned long long>(scalar_bad), static_cast<unsigned long long>(vector_bad),
                static_cast<unsigned long long>(wrong_round_caught), static_cast<unsigned long long>(wrong_sat_caught),
                ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}
