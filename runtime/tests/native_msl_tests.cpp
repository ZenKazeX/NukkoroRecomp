// Independent original-word oracle for the native MSL math library (math/msl.h, docs/NATIVE_MSL_MATH.md).
// liveness-roots: reference-interpreter-only
//
// Every routine runs from main.dol in the interpreter until it returns to a sentinel LR; the native routine runs on
// the same input. Each case compares:
// - the result bits (f1 / r3, and for rem_pio2 the two doubles it stores);
// - that nothing outside the routine's own stack frames and its output was written;
// - that the stack and game time are back where they were and FPSCR's control bits are unchanged.
// Both FPSCR[NI] modes run. The inputs cover every branch threshold (the high words, each just below, at and above),
// the exceptional values (zeros, denormals, infinities, quiet and signalling NaNs with payloads), multiples of pi/2,
// every exponent, and random bit patterns.
//
// Usage: native_msl_tests <main.dol>

#include "math/msl.h"
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace gekko;
namespace msl = shadow::math::msl;
using shadow::math::SubnormalMode;

// The originals (main.dol).
constexpr u32 kSin = 0x803B2AFCu, kCos = 0x803B2594u, kAtan = 0x803B2210u, kAtan2 = 0x803AFAECu,
              kAcos = 0x803AF678u, kRemPio2 = 0x803B0C74u, kKernelSin = 0x803B1F5Cu, kKernelCos = 0x803B1014u,
              kScalbn = 0x803B283Cu, kFloor = 0x803B2668u, kSinf = 0x8000FFE8u, kCosf = 0x8000F3B0u,
              kAtan2f = 0x80016260u, kAcosf = 0x801B2150u, kTan = 0x803B2BD4u, kKernelTan = 0x803B1FFCu,
              kTanf = 0x8000F3D4u, kAtanf = 0x8000F3F8u, kCosf2 = 0x803B2F74u, kPow = 0x803B0444u,
              kPowWrapper = 0x803B2D0Cu;
constexpr u32 kErrno = 0x805F2398u;  // MSL errno: pow's domain error writes EDOM (33) here
constexpr u32 kTwoOverPi = 0x80515398u, kPio2Pieces = 0x80515530u;
constexpr u32 kR2 = 0x805FA780u, kR13 = 0x805EC500u;
constexpr u32 kStack = 0x817E0000u, kStackLow = 0x817D0000u, kOut = 0x81001000u, kReturn = 0x817FF000u;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
u64 b64(double d) {
    u64 u;
    std::memcpy(&u, &d, 8);
    return u;
}
double d64(u64 u) {
    double d;
    std::memcpy(&d, &u, 8);
    return d;
}

struct Oracle {
    GuestMemory memory;
    Cpu cpu{memory};
    bool recording = false;
    std::vector<std::pair<u32, u32>> writes;
    u64 cases = 0, instructions = 0;

    explicit Oracle(const char* path) {
        std::ifstream input(path, std::ios::binary);
        const std::vector<u8> dol{std::istreambuf_iterator<char>(input), {}};
        require(dol.size() >= 256, "missing DOL");
        const auto word = [&](std::size_t offset) { return load_be32(dol.data() + offset); };
        for (unsigned i = 0; i < 18; ++i) {
            const u32 offset = word(i * 4), address = word(0x48 + i * 4), size = word(0x90 + i * 4);
            require(u64(offset) + size <= dol.size(), "invalid DOL section");
            if (size) memory.write_block(address, dol.data() + offset, size);
        }
        memory.set_access_trace([this](u32 address, std::size_t size, bool writing) {
            if (recording && writing) writes.push_back({address, static_cast<u32>(size)});
        });
    }

    struct Result {
        u64 f1;
        u32 r3;
    };
    Result run(u32 entry, u32 fpscr, u64 f1, u64 f2 = 0, u32 r3 = 0) {
        cpu.reset();
        cpu.set_msr(cpu.msr() | kMsrFP);
        cpu.pc = entry;
        cpu.npc = entry + 4;
        cpu.lr = kReturn;
        cpu.gpr[1] = kStack;
        cpu.gpr[2] = kR2;
        cpu.gpr[13] = kR13;
        cpu.gpr[3] = r3;
        cpu.fpr[1].bits[0] = cpu.fpr[1].bits[1] = f1;
        cpu.fpr[2].bits[0] = cpu.fpr[2].bits[1] = f2;
        cpu.fpscr = fpscr;
        const auto tick = cpu.game_clock().now();
        writes.clear();
        recording = true;
        unsigned steps = 0;
        while (cpu.pc != kReturn && !cpu.stopped() && steps < 100000) {
            cpu.run_steps(1);
            ++steps;
        }
        recording = false;
        instructions += steps;
        require(cpu.pc == kReturn && !cpu.stopped() && cpu.gpr[1] == kStack && cpu.game_clock().now() == tick,
                "original did not return with stack and time preserved");
        require((cpu.fpscr & 0xFFu) == (fpscr & 0xFFu), "original changed FPSCR control bits");
        for (const auto& [address, size] : writes) {
            // Its own frames, and the LR save word of the caller's linkage area (r1 + 4).
            const bool stack = address >= kStackLow && address + size <= kStack + 8u;
            const bool out = address >= kOut && address + size <= kOut + 16u;
            const bool err = (entry == kPow || entry == kPowWrapper) && address == kErrno && size == 4u;
            if (!(stack || out || err)) {
                char text[96];
                std::snprintf(text, sizeof text, "original %08X wrote %u bytes at %08X", entry, size, address);
                throw std::runtime_error(text);
            }
        }
        ++cases;
        return {cpu.fpr[1].bits[0], cpu.gpr[3]};
    }
};

// ---- inputs
std::vector<double> double_inputs(std::mt19937_64& rng, const std::vector<u32>& thresholds, unsigned random) {
    std::vector<double> v;
    const u64 specials[] = {0x0000000000000000ull, 0x0000000000000001ull, 0x000FFFFFFFFFFFFFull,
                            0x0010000000000000ull, 0x3FF0000000000000ull, 0x7FEFFFFFFFFFFFFFull,
                            0x7FF0000000000000ull, 0x7FF8000000000000ull, 0x7FF0000000000001ull,
                            0x7FF4000000001234ull, 0x7FFFFFFFFFFFFFFFull, 0x3E40000000000000ull,
                            0x3FE0000000000000ull, 0x4000000000000000ull, 0x3CA0000000000000ull};
    for (const u64 s : specials) {
        v.push_back(d64(s));
        v.push_back(d64(s | 0x8000000000000000ull));
    }
    for (const u32 t : thresholds) {
        for (const u64 h : {u64(t) - 1u, u64(t), u64(t) + 1u}) {
            for (const u64 l : {0x00000000ull, 0x00000001ull, 0x80000000ull, 0xFFFFFFFFull}) {
                v.push_back(d64((h << 32) | l));
                v.push_back(d64((h << 32) | l | 0x8000000000000000ull));
            }
        }
    }
    // Multiples of pi/2, the reduction's hardest cases, at every size; and every exponent.
    for (int n = 1; n < 2000; n += 7) {
        const double m = n * 1.5707963267948966;
        for (int u = -3; u <= 3; ++u) v.push_back(d64(b64(m) + u64(std::int64_t(u))));
    }
    for (int e = -1074; e <= 1023; ++e) {
        const double p = std::ldexp(1.0, e);
        v.push_back(p);
        v.push_back(-p * 1.2345678901234567);
        v.push_back(std::nextafter(p, 0.0));
    }
    // Known hard reductions (fdlibm's worst cases for x mod pi/2).
    for (const u64 hard : {0x5AF0000000000000ull, 0x7F6FD3D6E8EBE5FAull, 0x6A70B1E2D2D6A8E7ull, 0x4D3D2F05F55FC4DBull})
        v.push_back(d64(hard));
    std::uniform_int_distribution<u64> any;
    std::uniform_real_distribution<double> unit(-1.0, 1.0);
    for (unsigned i = 0; i < random; ++i) {
        v.push_back(d64(any(rng)));                           // every class, every exponent
        v.push_back(unit(rng) * 8.0);                          // the angles the game uses
        v.push_back(std::ldexp(unit(rng), int(any(rng) % 1100) - 40));  // large reductions
    }
    return v;
}

int failures = 0;
std::map<std::string, int> failures_by;
void compare(const char* what, double input, double input2, u32 fpscr, u64 expected, u64 native) {
    if (expected == native) return;
    ++failures_by[std::string(what) + ((fpscr & 4u) ? " NI=1" : " NI=0")];
    if (++failures <= 40)
        std::fprintf(stderr, "%s x=%016llX y=%016llX NI=%u expected=%016llX native=%016llX\n", what,
                     (unsigned long long)b64(input), (unsigned long long)b64(input2), (fpscr & 4u) ? 1u : 0u,
                     (unsigned long long)expected, (unsigned long long)native);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: native_msl_tests <main.dol>\n");
        return 2;
    }
    try {
        Oracle o(argv[1]);
        // The tables as compiled in match main.dol.
        for (unsigned i = 0; i < 66; ++i)
            require(u32(msl::detail::two_over_pi()[i]) == o.memory.peek32(kTwoOverPi + 4 * i), "two_over_pi differs");
        for (unsigned i = 0; i < 8; ++i) {
            const u64 dol = (u64(o.memory.peek32(kPio2Pieces + 8 * i)) << 32) | o.memory.peek32(kPio2Pieces + 8 * i + 4);
            require(b64(msl::detail::pio2_pieces()[i]) == dol, "PIo2 differs");
        }

        std::mt19937_64 rng(0x5EED2026ull);
        const std::vector<double> trig = double_inputs(
            rng, {0x3E400000u, 0x3FD33333u, 0x3FE90000u, 0x3FE921FBu, 0x3FF921FBu, 0x4002D97Cu, 0x413921FBu,
                  0x7FF00000u, 0x4012D97Cu, 0x400921FBu},
            10000);
        const std::vector<double> inverse = double_inputs(
            rng, {0x3E200000u, 0x3FDC0000u, 0x3FE60000u, 0x3FF30000u, 0x40038000u, 0x44100000u, 0x3FF00000u,
                  0x3FE00000u, 0x3C600000u, 0x7FF00000u},
            10000);

        for (const u32 fpscr : {0u, kFpscrNI}) {
            const SubnormalMode mode = fpscr ? SubnormalMode::Flush : SubnormalMode::Preserve;
            for (const double x : trig) {
                compare("sin", x, 0, fpscr, o.run(kSin, fpscr, b64(x)).f1, b64(msl::sin(x)));
                compare("cos", x, 0, fpscr, o.run(kCos, fpscr, b64(x)).f1, b64(msl::cos(x)));
                const Oracle::Result r = o.run(kRemPio2, fpscr, b64(x), 0, kOut);
                const msl::detail::Reduced n = msl::detail::rem_pio2(x);
                const u64 y0 = (u64(o.memory.peek32(kOut)) << 32) | o.memory.peek32(kOut + 4);
                const u64 y1 = (u64(o.memory.peek32(kOut + 8)) << 32) | o.memory.peek32(kOut + 12);
                compare("rem_pio2 n", x, 0, fpscr, r.r3, u32(n.n));
                compare("rem_pio2 y0", x, 0, fpscr, y0, b64(n.y0));
                compare("rem_pio2 y1", x, 0, fpscr, y1, b64(n.y1));
                compare("floor", x, 0, fpscr, o.run(kFloor, fpscr, b64(x)).f1, b64(msl::detail::floor(x)));
                for (const int e : {-1100, -60, -24, -1, 0, 1, 24, 60, 1100}) {
                    compare("scalbn", x, double(e), fpscr, o.run(kScalbn, fpscr, b64(x), 0, u32(e)).f1,
                            b64(msl::detail::scalbn(x, e, mode)));
                }
                const float xf = float(x);
                const u64 xs = b64(double(xf));
                compare("sinf", x, 0, fpscr, o.run(kSinf, fpscr, xs).f1, b64(double(msl::sinf(xf, mode))));
                compare("cosf", x, 0, fpscr, o.run(kCosf, fpscr, xs).f1, b64(double(msl::cosf(xf, mode))));
                compare("cosf (803B2F74)", x, 0, fpscr, o.run(kCosf2, fpscr, xs).f1, b64(double(msl::cosf(xf, mode))));
                compare("tan", x, 0, fpscr, o.run(kTan, fpscr, b64(x)).f1, b64(msl::tan(x)));
                compare("tanf", x, 0, fpscr, o.run(kTanf, fpscr, xs).f1, b64(double(msl::tanf(xf, mode))));
            }
            // The kernels on their own: |x| <= pi/4, tails up to an ulp of x.
            std::uniform_real_distribution<double> quarter(-0.7853981633974483, 0.7853981633974483);
            for (unsigned i = 0; i < 20000; ++i) {
                const double x = i < 10000 ? quarter(rng) : std::ldexp(quarter(rng), -int(i % 40));
                const double y = std::ldexp(quarter(rng), std::ilogb(x == 0.0 ? 1.0 : x) - 54);
                const int iy = int(i & 1u);
                compare("kernel_sin", x, y, fpscr, o.run(kKernelSin, fpscr, b64(x), b64(y), u32(iy)).f1,
                        b64(msl::detail::kernel_sin(x, iy ? y : 0.0, iy)));
                compare("kernel_cos", x, y, fpscr, o.run(kKernelCos, fpscr, b64(x), b64(y)).f1,
                        b64(msl::detail::kernel_cos(x, y)));
                const int ty = (i & 2u) ? 1 : -1;  // tan's iy: 1, or -1 for -1/tan
                compare("kernel_tan", x, y, fpscr, o.run(kKernelTan, fpscr, b64(x), b64(y), u32(ty)).f1,
                        b64(msl::detail::kernel_tan(x, y, ty)));
            }
            for (const double x : inverse) {
                compare("atan", x, 0, fpscr, o.run(kAtan, fpscr, b64(x)).f1, b64(msl::atan(x)));
                compare("acos", x, 0, fpscr, o.run(kAcos, fpscr, b64(x)).f1, b64(msl::acos(x)));
                const float xf = float(x);
                compare("acosf", x, 0, fpscr, o.run(kAcosf, fpscr, b64(double(xf))).f1,
                        b64(double(msl::acosf(xf, mode))));
                compare("atanf", x, 0, fpscr, o.run(kAtanf, fpscr, b64(double(xf))).f1,
                        b64(double(msl::atanf(xf, mode))));
            }
            // atan2 on pairs: every class against every class, and random pairs at every relative size.
            std::uniform_int_distribution<std::size_t> pick(0, inverse.size() - 1);
            std::vector<std::pair<double, double>> pairs;
            for (unsigned i = 0; i < 60; ++i)
                for (unsigned j = 0; j < 60; ++j) pairs.push_back({inverse[i], inverse[j]});
            for (unsigned i = 0; i < 60000; ++i) pairs.push_back({inverse[pick(rng)], inverse[pick(rng)]});
            for (unsigned i = 0; i < 20000; ++i) {
                const double x = quarter(rng) * 1e3;
                pairs.push_back({std::ldexp(quarter(rng), -1000 - int(i % 74)), x});  // tiny quotients
            }
            for (const auto& [y, x] : pairs) {
                compare("atan2", y, x, fpscr, o.run(kAtan2, fpscr, b64(y), b64(x)).f1,
                        b64(msl::atan2(y, x, mode)));
                const float yf = float(y), xf = float(x);
                compare("atan2f", y, x, fpscr, o.run(kAtan2f, fpscr, b64(double(yf)), b64(double(xf))).f1,
                        b64(double(msl::atan2f(yf, xf, mode))));
            }
        }
        // pow on pairs: every special against every special, the image gamma tables' operands (i / 255 to the
        // gamma and to its inverse), integer exponents of negative bases, |y| > 2^31 near 1, the overflow and
        // underflow boundaries, the exponents with their own paths (+-1, 2, 0.5), random pairs; both modes.
        {
            std::vector<std::pair<double, double>> pairs;
            const std::vector<double> specials = double_inputs(rng, {0x3FF00000u, 0x3FE00000u, 0x40000000u,
                                                                      0x41E00000u, 0x43F00000u, 0x43400000u,
                                                                      0x3FEFFFFFu, 0x00100000u, 0x7FF00000u},
                                                     0);
            std::vector<double> few;
            for (std::size_t i = 0; i < specials.size(); i += 29) few.push_back(specials[i]);
            for (const double x : few)
                for (const double y : few) pairs.push_back({x, y});
            std::uniform_real_distribution<double> unit(0.0, 1.0);
            std::uniform_int_distribution<u64> any;
            for (unsigned g = 0; g < 400; ++g) {
                const float gamma = g < 200 ? float(0.05 + 4.0 * unit(rng)) : float(std::ldexp(unit(rng), int(any(rng) % 40) - 20));
                const float inv = 1.0f / gamma;
                for (int i = 1; i < 256; i += (g < 20 ? 1 : 17)) {
                    const float xf = float(i) / 255.0f;
                    pairs.push_back({double(xf), double(inv)});
                    pairs.push_back({double(xf), double(gamma)});
                }
            }
            for (unsigned i = 0; i < 8000; ++i) {
                const double x = std::ldexp(unit(rng) + 0.5, int(any(rng) % 2100) - 1075);
                const double sign = (i & 1u) ? -1.0 : 1.0;
                pairs.push_back({sign * x, double(std::int64_t(any(rng) % 64) - 32)});            // integers
                pairs.push_back({sign * x, (unit(rng) - 0.5) * 64.0});                             // fractions
                pairs.push_back({d64(any(rng)), d64(any(rng))});                                   // every class
                const double l2 = std::log2(std::fabs(x));
                if (l2 != 0.0) {
                    for (const double edge : {1024.0, -1022.0, -1074.0, -1075.0})
                        pairs.push_back({x, std::nextafter(edge / l2, double(int(i % 3) - 1) * 1e300)});
                }
                pairs.push_back({std::nextafter(1.0, (i & 2u) ? 2.0 : 0.0) * sign, std::ldexp(unit(rng) + 1.0, 31 + int(i % 40))});
                pairs.push_back({x, (i % 5u) == 0 ? 0.5 : (i % 5u) == 1 ? 2.0 : (i % 5u) == 2 ? 1.0 : (i % 5u) == 3 ? -1.0 : -0.5});
            }
            // the bases with their own results (zeros, infinities, one) to odd, even and fractional powers
            for (const double x : {0.0, -0.0, 1.0, -1.0, d64(0x7FF0000000000000ull), d64(0xFFF0000000000000ull)}) {
                for (const double y : {3.0, -3.0, 5.0, -5.0, 4.0, -4.0, 2.5, -2.5, 1e10, -1e10, 2147483649.0,
                                       -2147483649.0, 9007199254740993.0, 1e300, -1e300, 0.5, -0.5}) {
                    pairs.push_back({x, y});
                }
            }
            // found by search (about one in 90 million random pairs): the log step's fused multiply-add decides the
            // result's last bit
            for (const auto& [x, y] : {std::pair<u64, u64>{0x3FE9980330490E82ull, 0xC040B93F1E79E6AEull},
                                       {0x3FF128E0D386F851ull, 0xC03B1456B4DE0098ull},
                                       {0x40F3BC331DDB74DEull, 0xC00B2C6E3118DF72ull},
                                       {0x45E618A21E126A6Aull, 0xC006176F838CDDA8ull}}) {
                pairs.push_back({d64(x), d64(y)});
            }
            // bases near 1 to moderate powers: the logarithm's low-order part decides the last bit
            for (unsigned i = 0; i < 30000; ++i) pairs.push_back({0.75 + 0.5 * unit(rng), (unit(rng) - 0.5) * 400.0});
            for (const u32 fpscr : {0u, kFpscrNI}) {
                const SubnormalMode mode = fpscr ? SubnormalMode::Flush : SubnormalMode::Preserve;
                for (const auto& [x, y] : pairs) {
                    compare("pow", x, y, fpscr, o.run(kPow, fpscr, b64(x), b64(y)).f1, b64(msl::pow(x, y, mode)));
                }
                for (std::size_t i = 0; i < pairs.size(); i += 97) {
                    const auto& [x, y] = pairs[i];
                    compare("pow (803B2D0C)", x, y, fpscr, o.run(kPowWrapper, fpscr, b64(x), b64(y)).f1,
                            b64(msl::pow(x, y, mode)));
                }
            }
        }
        for (const auto& [name, count] : failures_by) std::printf("  %s: %d differences\n", name.c_str(), count);
        std::printf("native_msl_tests: %llu original calls, %llu instructions, %d differences\n",
                    (unsigned long long)o.cases, (unsigned long long)o.instructions, failures);
        return failures == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "native_msl_tests: %s\n", error.what());
        return 1;
    }
}
