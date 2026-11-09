// The native scalar contract against the original DOL's square-root words.
// liveness-roots: reference-interpreter-only
#include "math/scalar.h"
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"

#include <array>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>
#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif

namespace {
using namespace gekko;
constexpr u32 kEntry = 0x803B2D2Cu;
constexpr u32 kReturn = 0x817FF000u;
constexpr u32 kStack = 0x817E0000u;
constexpr u32 kSda = 0x805EC500u;
constexpr u32 kError = kSda + 0x5E98u;
constexpr u32 kErrorSentinel = 0x12345678u;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Oracle {
    GuestMemory memory;
    Cpu cpu{memory};
    unsigned cases = 0;
    u64 instructions = 0;
    unsigned caller_cases = 0;
    std::array<unsigned, 4> caller_sites{};

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
        require(memory.peek32(kEntry) == 0x9421FFE0u &&
                    memory.peek32(0x803B2F4Cu) == 0x4E800020u &&
                    memory.peek32(0x805EED78u) == 0x7FFFFFFFu,
                "square-root reference image differs");
    }

    void check(u64 bits, u32 mode) {
        cpu.reset();
        cpu.pc = kEntry;
        cpu.lr = kReturn;
        cpu.gpr[1] = kStack;
        cpu.gpr[2] = 0x805FA780u;
        cpu.gpr[13] = kSda;
        cpu.fpscr = mode;
        cpu.fpr[1].bits[0] = bits;
        memory.write32(kError, kErrorSentinel);
        const auto start_tick = cpu.game_clock().now();
        unsigned steps = 0;
        while (cpu.pc != kReturn && !cpu.stopped() && steps++ < 5000)
            cpu.run_steps(1);
        require(!cpu.stopped() && cpu.pc == kReturn, "original square root did not return");
        require(cpu.gpr[1] == kStack && cpu.game_clock().now() == start_tick,
                "original square root changed stack ownership or game time");
        const auto actual = shadow::math::square_root(bits_to_double(bits));
        const u64 actual_bits = double_to_bits(actual.value);
        const u32 actual_error = actual.domain_error ? 33u : kErrorSentinel;
        if (actual_bits != cpu.fpr[1].bits[0] || actual_error != memory.peek32(kError)) {
            std::fprintf(stderr, "sqrt input=%016llX mode=%08X original=%016llX native=%016llX "
                                 "error=%08X/%08X\n", bits, mode, cpu.fpr[1].bits[0], actual_bits,
                         memory.peek32(kError), actual_error);
            throw std::runtime_error("native square-root contract differs from original");
        }
        instructions += steps;
        ++cases;
    }

    // Run the enclosing math callers, not only the scalar wrapper. A read of
    // retired square-root scratch before overwrite is an error, even if the
    // ultimate numeric result happens to agree. The alternate execution
    // replaces only the scalar API and leaves all volatile registers/scratch
    // untouched, as production lowering does.
    u64 run_caller(u32 entry, double x, double y, bool native) {
        constexpr u32 low = kStack - 1024u;
        std::array<bool, 1040> retired{};
        bool in_root = false;
        u32 root_return = 0, root_stack = 0;
        cpu.reset();
        cpu.pc = entry;
        cpu.lr = kReturn;
        cpu.gpr[1] = kStack;
        cpu.gpr[2] = 0x805FA780u;
        cpu.gpr[13] = kSda;
        cpu.fpr[1].bits[0] = double_to_bits(x);
        cpu.fpr[2].bits[0] = double_to_bits(y);
        for (u32 address = low; address < kStack + 16u; ++address)
            memory.write8(address, 0xA5u);
        memory.write32(kError, kErrorSentinel);
        memory.set_access_trace([&](u32 address, std::size_t size, bool writing) {
            if (writing && !(address >= low && u64(address) + size <= kStack + 16u))
                require(address == kError && size == 4, "math caller wrote outside its stack/error contract");
            for (std::size_t i = 0; i < size; ++i) {
                const u64 at = u64(address) + i;
                if (at < low || at >= kStack + 16u) continue;
                const auto index = std::size_t(at - low);
                if (writing) retired[index] = in_root && at >= root_stack && at < root_stack + 32u;
                else if (!in_root) require(!retired[index], "caller read retired square-root scratch before overwrite");
            }
        });
        unsigned steps = 0;
        while (cpu.pc != kReturn && !cpu.stopped() && steps++ < 10000) {
            if (in_root && cpu.pc == root_return) in_root = false;
            if (cpu.pc == kEntry) {
                root_return = cpu.lr;
                root_stack = cpu.gpr[1] - 32u;
                constexpr u32 sites[] = {0x803AF788u, 0x803AF814u, 0x803AFA30u, 0x803B0648u};
                const u32 outer_return = memory.peek32(cpu.gpr[1] + 20u);
                if (!native) for (unsigned i = 0; i < 4; ++i)
                    if (outer_return == sites[i] + 4u) ++caller_sites[i];
                if (native) {
                    const auto result = shadow::math::square_root(bits_to_double(cpu.fpr[1].bits[0]));
                    cpu.fpr[1].bits[0] = double_to_bits(result.value);
                    if (result.domain_error) cpu.mem_write32(kError, 33u);
                    cpu.pc = root_return;
                    continue;
                }
                in_root = true;
            }
            cpu.run_steps(1);
        }
        memory.set_access_trace({});
        require(!cpu.stopped() && cpu.pc == kReturn && cpu.gpr[1] == kStack,
                "enclosing math caller did not return with its stack restored");
        require(memory.peek32(kError) == kErrorSentinel, "finite caller unexpectedly changed error state");
        return cpu.fpr[1].bits[0];
    }

    void callers() {
        for (unsigned i = 0; i <= 400; ++i) {
            const double x = (double(i) - 200.0) / 200.0;
            for (u32 entry : {0x803AF678u, 0x803AF8B4u}) {
                const auto expected = run_caller(entry, x, 0, false);
                require(run_caller(entry, x, 0, true) == expected, "enclosing inverse-trig result changed");
                ++caller_cases;
            }
            const double positive = 0.001 + double(i) * 0.125;
            const auto expected = run_caller(0x803B0444u, positive, 0.5, false);
            require(run_caller(0x803B0444u, positive, 0.5, true) == expected, "enclosing power result changed");
            ++caller_cases;
        }
        for (unsigned hits : caller_sites) require(hits > 0, "an original square-root caller was not exercised");
        std::printf("PASS enclosing math callers: %u pairs; four sites [%u,%u,%u,%u]; "
                    "no read of retired square-root scratch before overwrite\n", caller_cases,
                    caller_sites[0], caller_sites[1], caller_sites[2], caller_sites[3]);
    }
};
}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: native_scalar_math_tests main.dol");
#if defined(_M_X64) || defined(__x86_64__)
        require((_mm_getcsr() & 0x6040u) == 0, "requires round-to-nearest and DAZ disabled");
#endif
        Oracle oracle(argv[1]);
        const std::array<u64, 5> fractions{{0, 1, 0x7ffffffffffffull,
                                          0xffffffffffffeull, 0xfffffffffffffull}};
        for (u64 exponent = 0; exponent < 2048; ++exponent)
            for (u64 fraction : fractions)
                for (u64 sign : {0ull, 0x8000000000000000ull})
                    oracle.check(sign | (exponent << 52) | fraction,
                                 (exponent & 1) ? 4u : 0u);
        u64 random = 0x64351a92d70cbe21ull;
        for (unsigned i = 0; i < 20000; ++i) {
            random ^= random << 13;
            random ^= random >> 7;
            random ^= random << 17;
            oracle.check(random, (i & 1) ? 4u : 0u);
        }
        std::printf("PASS native square root: %u cases, %llu original instructions; "
                    "all exponents, signed zero, subnormals, infinities, NaN payloads and error state\n",
                    oracle.cases, oracle.instructions);
        oracle.callers();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL native square root: %s\n", error.what());
        return 1;
    }
}
