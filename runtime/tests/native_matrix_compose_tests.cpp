// Independent original-word oracle for affine composition and overlap order.
// liveness-roots: reference-interpreter-only
#include "math/matrix.h"
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>
#include <immintrin.h>

namespace {
using namespace gekko;
constexpr u32 kEntry = 0x8040CB14u, kBuffer = 0x81001000u;
constexpr u32 kReturn = 0x817FF000u, kStack = 0x817E0000u;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
u32 float_bits(float value) {
    u32 result;
    std::memcpy(&result, &value, 4);
    return result;
}
struct Oracle {
    GuestMemory memory;
    Cpu cpu{memory};
    unsigned cases = 0;
    u64 instructions = 0;
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
        require(memory.peek32(kEntry) == 0x9421FFA0u && memory.peek32(0x8040CD00u) == 0x4E800020u,
                "matrix composition reference differs");
    }
    void check(const std::array<u32, 64>& initial, int distance, bool flush) {
        constexpr unsigned source = 24;
        const unsigned destination = unsigned(int(source) + distance);
        std::array<float, 64> actual{};
        std::memcpy(actual.data(), initial.data(), sizeof(initial));
        for (unsigned i = 0; i < initial.size(); ++i) memory.write32(kBuffer + i * 4, initial[i]);
        cpu.reset();
        cpu.pc = kEntry; cpu.lr = kReturn;
        cpu.gpr[1] = kStack;
        cpu.gpr[3] = kBuffer + destination * 4;
        cpu.gpr[4] = kBuffer + source * 4;
        cpu.fpscr = flush ? 4u : 0u;
        const auto tick = cpu.game_clock().now();
        unsigned steps = 0;
        while (cpu.pc != kReturn && !cpu.stopped() && steps++ < 400) cpu.run_steps(1);
        require(!cpu.stopped() && cpu.pc == kReturn && cpu.gpr[1] == kStack && cpu.game_clock().now() == tick,
                "original composition did not return with stack/time preserved");
        require(cpu.fpscr == (flush ? 4u : 0u), "composition changed floating-point control/status");
        const auto control = _mm_getcsr() & 0xffc0u;
        shadow::math::compose_affine_matrix(actual.data() + destination, actual.data() + source,
            flush ? shadow::math::SubnormalMode::Flush : shadow::math::SubnormalMode::Preserve);
        require((_mm_getcsr() & 0xffc0u) == control, "native math changed host floating-point control");
        for (unsigned i = 0; i < initial.size(); ++i) {
            const u32 expected = memory.peek32(kBuffer + i * 4), native = float_bits(actual[i]);
            if (expected != native) {
                std::fprintf(stderr, "case=%u distance=%d flush=%u element=%u expected=%08X native=%08X\n",
                             cases, distance, flush, i, expected, native);
                for (unsigned k = 0; k < 12; ++k)
                    std::fprintf(stderr, "%u left=%08X right=%08X\n", k, initial[destination+k], initial[source+k]);
                throw std::runtime_error("native matrix composition differs from original words");
            }
        }
        ++cases; instructions += steps;
    }

    std::array<u32, 12> caller(u32 entry, unsigned seed, bool native) {
        std::array<bool, 2048> retired{};
        constexpr u32 low = kStack - 2048;
        for (u32 at = low; at < kStack + 16; at += 4) memory.write32(at, 0xA5A5A5A5u);
        for (unsigned i = 0; i < 12; ++i)
            memory.write32(kBuffer + i * 4, float_bits(float(int(seed * 17 + i * 13) % 97 - 48) / 16.0f));
        cpu.reset(); cpu.pc = entry; cpu.lr = kReturn; cpu.gpr[1] = kStack;
        cpu.gpr[2] = 0x805FA780u; cpu.gpr[13] = 0x805EC500u; cpu.gpr[3] = kBuffer;
        for (unsigned i = 1; i < 4; ++i)
            cpu.fpr[i].bits[0] = double_to_bits((double(seed) - 32.0) * 0.025 * i);
        bool in_composition = false;
        u32 private_low = 0, private_high = 0, continuation = 0;
        memory.set_access_trace([&](u32 address, std::size_t size, bool writing) {
            for (std::size_t i = 0; i < size; ++i) {
                const u64 at = u64(address) + i;
                if (at < low || at >= kStack) continue;
                if (writing) retired[at - low] = in_composition && at >= private_low && at < private_high;
                else if (!in_composition) require(!retired[at - low], "caller read retired matrix stack before overwrite");
            }
        });
        unsigned steps = 0, hits = 0;
        const auto time = cpu.game_clock().now();
        while (cpu.pc != kReturn && !cpu.stopped() && steps++ < 50000) {
            if (cpu.pc == kEntry) {
                ++hits; continuation = cpu.lr;
                if (native) {
                    float left[12], right[12];
                    for (unsigned i = 0; i < 12; ++i) {
                        const u32 l = memory.read32(cpu.gpr[3] + i * 4), r = memory.read32(cpu.gpr[4] + i * 4);
                        std::memcpy(left + i, &l, 4); std::memcpy(right + i, &r, 4);
                    }
                    shadow::math::compose_affine_matrix(left, right, shadow::math::SubnormalMode::Preserve);
                    for (unsigned i = 0; i < 12; ++i) memory.write32(cpu.gpr[3] + i * 4, float_bits(left[i]));
                    cpu.pc = continuation;
                    continue;
                }
                in_composition = true; private_high = cpu.gpr[1]; private_low = private_high - 96;
            } else if (in_composition && cpu.pc == continuation) in_composition = false;
            cpu.run_steps(1);
        }
        memory.set_access_trace({});
        require(!cpu.stopped() && cpu.pc == kReturn && cpu.gpr[1] == kStack && hits == 1 &&
                cpu.game_clock().now() == time, "matrix outer caller failed stack/time/return contract");
        std::array<u32, 12> result{};
        for (unsigned i = 0; i < 12; ++i) result[i] = memory.peek32(kBuffer + i * 4);
        return result;
    }

    void callers() {
        unsigned pairs = 0;
        for (u32 entry : {0x8040C85Cu, 0x8040C898u, 0x8040C8D4u, 0x8040C910u})
            for (unsigned seed = 0; seed < 64; ++seed) {
                const auto expected = caller(entry, seed, false);
                require(caller(entry, seed, true) == expected, "enclosing matrix caller output differs");
                ++pairs;
            }
        std::printf("PASS matrix outer callers: %u pairs; no read of retired matrix stack before overwrite\n", pairs);
    }

    void discarded_collision_output() {
        // Final Haunt's no-hit caller computes a temporary direction from an
        // output buffer the failed query never writes. A removed matrix spill
        // changes that stale input. Replay the original numeric/control-flow
        // suffix to its discard branch; do not manufacture matching FPSCR bits.
        const std::array<std::array<u32, 3>, 2> stale{{
            {{0x3FB110E4u, 0x60000000u, 0x3D888723u}},
            {{0x8060C318u, 0x8040C130u, 0x3F800000u}}
        }};
        unsigned checks = 0;
        for (float radius : {0.0f, 1.0f, 15.0f, 100.0f})
            for (float motion : {0.0f, 0.25f, 100.0f, 32000.0f})
                for (unsigned variant = 0; variant < stale.size(); ++variant) {
                    for (u32 at = kStack - 256; at < kStack + 600; at += 4) memory.write32(at, 0);
                    for (unsigned i = 0; i < 3; ++i) {
                        memory.write32(kStack + 0x120 + i * 4, stale[variant][i]);
                        memory.write32(kStack + 0x1A4 + i * 4, float_bits(float(int(i) * 60 - 60)));
                    }
                    memory.write32(kStack + 0x198, float_bits(motion));
                    memory.write32(kBuffer, kBuffer + 0x100);
                    memory.write32(kBuffer + 0x374, float_bits(radius));
                    cpu.reset(); cpu.pc = 0x800979E8u; cpu.gpr[1] = kStack;
                    cpu.gpr[2] = 0x805FA780u; cpu.gpr[13] = 0x805EC500u; cpu.gpr[27] = kBuffer;
                    cpu.fpr[1].bits[0] = double_to_bits(-1.0); cpu.fpscr = 4;
                    memory.set_access_trace([&](u32 address, std::size_t size, bool writing) {
                        if (writing) require(address >= kStack - 256 && u64(address) + size <= kStack + 600,
                                             "discarded no-hit direction escaped its local frame");
                    });
                    unsigned steps = 0;
                    while (cpu.pc != 0x80097BA8u && !cpu.stopped() && steps++ < 2000) cpu.run_steps(1);
                    memory.set_access_trace({});
                    require(!cpu.stopped() && cpu.pc == 0x80097BA8u && cpu.gpr[1] == kStack &&
                            cpu.gpr[26] == 0 && bits_to_double(cpu.fpr[30].bits[0]) == 100000.0,
                            "no-hit caller did not discard its temporary direction");
                    require(bool(cpu.fpscr & 0xA0080000u) == (variant == 0),
                            "recovered stale-input exception difference was not reproduced");
                    ++checks;
                }
        std::printf("PASS original no-hit caller: %u bounded cases discard both stale vectors; exception difference retained\n", checks);
    }

    void exception_index() {
        // Execute the original registration and lookup, independently of the
        // generator's liveness decision. Range keys are never executed.
        const auto run = [&](u32 entry) {
            cpu.pc = entry; cpu.lr = kReturn; cpu.gpr[1] = kStack;
            unsigned steps = 0;
            while (cpu.pc != kReturn && !cpu.stopped() && steps++ < 2000) cpu.run_steps(1);
            require(!cpu.stopped() && cpu.pc == kReturn && cpu.gpr[1] == kStack, "exception index oracle failed");
        };
        cpu.reset(); memory.write32(0x805A61F8u, 0);
        cpu.gpr[3] = 0x80008D14u; cpu.gpr[4] = 0x805FA780u; run(0x803A3954u);
        require(cpu.gpr[3] == 0, "original fragment registration failed");
        for (u32 pc : {kEntry, kEntry + 4, 0x8040CD00u}) {
            cpu.gpr[3] = pc; cpu.gpr[4] = kBuffer; run(0x803A371Cu);
            require(memory.peek32(kBuffer) == 0x800057D4u && memory.peek32(kBuffer + 4) == kEntry &&
                    memory.peek32(kBuffer + 8) == 0, "matrix index did not return its empty-cleanup descriptor");
        }
        std::puts("PASS original exception index: registration and matrix range lookup; no cleanup selected");
    }
};
}

// A kernel-only benchmark, separate from the original-word correctness oracle.
// Repeatable inputs and output digests keep baseline/candidate workloads equal;
// the distributions cover ordinary, wide-exponent and very small normal values.
int benchmark(const char* distribution) {
    constexpr unsigned count = 256, rounds = 16384;
    std::array<std::array<float, 12>, count> left{}, right{}, result{};
    u64 random = 0x18d5ca803c9726bfull;
    const auto next = [&]() { random ^= random << 13; random ^= random >> 7; random ^= random << 17; return u32(random); };
    for (unsigned i = 0; i < count; ++i) for (unsigned j = 0; j < 12; ++j) {
        left[i][j] = float(int(next() % 20001) - 10000) / 1024.0f;
        right[i][j] = float(int(next() % 20001) - 10000) / 1024.0f;
        if (std::strcmp(distribution, "bounded") == 0) {
            const u32 a = (next() & 0x807fffffu) | ((98u + next() % 59u) << 23);
            const u32 b = (next() & 0x807fffffu) | ((98u + next() % 59u) << 23);
            std::memcpy(&left[i][j], &a, 4); std::memcpy(&right[i][j], &b, 4);
        } else if (std::strcmp(distribution, "fallback") == 0) {
            left[i][j] *= 0x1p-80f;
        } else require(std::strcmp(distribution, "ordinary") == 0, "unknown benchmark distribution");
    }
    const auto run = [&] {
        for (unsigned i = 0; i < count; ++i) {
            result[i] = left[i];
            shadow::math::compose_affine_matrix(result[i].data(), right[i].data(),
                                               shadow::math::SubnormalMode::Preserve);
        }
    };
    run();
    const auto start = std::chrono::steady_clock::now();
    for (unsigned round = 0; round < rounds; ++round) run();
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    u64 digest = 14695981039346656037ull;
    for (const auto& row : result) for (float x : row) { digest ^= float_bits(x); digest *= 1099511628211ull; }
    std::printf("{\"distribution\":\"%s\",\"calls\":%u,\"seconds\":%.9f,\"digest\":\"%016llx\"}\n",
                distribution, count * rounds, seconds, digest);
    return 0;
}

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::strcmp(argv[1], "--benchmark") == 0) return benchmark(argv[2]);
        require(argc == 2, "usage: native_matrix_compose_tests main.dol");
        require((_mm_getcsr() & 0xe040u) == 0, "requires nearest rounding and host DAZ/FTZ disabled");
        Oracle oracle(argv[1]);
        oracle.exception_index();
        oracle.callers();
        oracle.discarded_collision_output();
        const std::array<u32, 18> edge{{0, 0x80000000u, 1, 0x007fffffu, 0x00800000u, 0x00800001u,
            0x807fffffu, 0x80800000u, 0x7f7fffffu, 0xff7fffffu, 0x7f800000u, 0xff800000u,
            0x7f800001u, 0x7fc12345u, 0xff812345u, 0xffc12345u, 0x3f800000u, 0xbf800000u}};
        u64 random = 0xa7346cf21e908b5full;
        const auto next = [&]() { random ^= random << 13; random ^= random >> 7; random ^= random << 17; return u32(random); };
        for (unsigned round = 0; round < 4096; ++round) {
            std::array<u32, 64> data{};
            for (unsigned i = 0; i < data.size(); ++i) {
                if (round < edge.size()) data[i] = edge[(round + i) % edge.size()];
                else if (round < 2048) data[i] = next();
                else data[i] = float_bits(float(int(next() % 200001) - 100000) / 1024.0f);
            }
            for (bool flush : {false, true}) {
                if (round < 256) for (int distance : {-13,-12,-11,-7,-3,-1,0,1,3,7,11,12,13})
                    oracle.check(data, distance, flush);
                else for (int distance : {-12,0,12}) oracle.check(data, distance, flush);
            }
        }
        // Exponent boundaries, adjacent values and cancellation. Expected
        // values still come from the original DOL words.
        const std::array<u32, 12> bounds{{0u, 0x80000000u, 0x307fffffu, 0x30800000u,
            0x30800001u, 0xb0800000u, 0x4e7fffffu, 0x4e800000u, 0x4e800001u,
            0xce800000u, 0x3f7fffffu, 0x3f800001u}};
        for (unsigned round = 0; round < 512; ++round) {
            std::array<u32, 64> data{};
            for (auto& value : data) value = round < 128 ? bounds[next() % bounds.size()] :
                (next() & 0x807fffffu) | ((97u + next() % 61u) << 23);
            // Both ordinary matrix spans, then every partial-overlap direction.
            for (bool flush : {false, true}) for (int distance : {-12,-11,-7,-3,-1,0,1,3,7,11,12})
                oracle.check(data, distance, flush);
        }
        for (u32 x : {0x30800001u, 0x3f800001u, 0x4e7fffffu}) {
            std::array<u32, 64> data{};
            data[12] = x; data[13] = x - 1u;
            data[24] = x - 2u; data[28] = (x - 1u) ^ 0x80000000u;
            for (bool flush : {false, true}) oracle.check(data, -12, flush);
        }
        // Exact value just below the normal boundary, although both float and
        // double sums round onto it; flush-before-rounding must still yield zero.
        for (u32 sign : {0u, 0x80000000u}) {
            std::array<u32, 64> data{};
            data[12] = 0x80800000u ^ sign; data[13] = 0x3f800000u;
            data[24] = 0x00800000u; data[28] = 0x00800000u ^ sign;
            for (bool flush : {false, true}) oracle.check(data, -12, flush);
        }
        std::printf("PASS native matrix composition: %u original-word cases, %llu instructions; "
                    "all float bit classes, both subnormal policies, self and partial overlap\n", oracle.cases, oracle.instructions);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL native matrix composition: %s\n", error.what());
        return 1;
    }
}
