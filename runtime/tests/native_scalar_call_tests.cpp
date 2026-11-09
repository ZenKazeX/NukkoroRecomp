// Exercise the actual generated caller's marshalling and ABI. The separate
// native_scalar_math_tests executable establishes the numeric API contract
// against the original words without keeping their translation alive.
#include "math/scalar.h"
#include "recomp_decls.h"

#include <cstdio>
#include <stdexcept>

int main() {
    using namespace gekko;
    try {
        GuestMemory memory;
        Cpu cpu(memory);
        u64 random = 0x72ca6c13d254981bull;
        unsigned cases = 0;
        const auto check = [&](u64 bits) {
            constexpr u32 stack = 0x817E0000u, finish = 0x817FF000u;
            constexpr u32 sda = 0x805EC500u, error = sda + 0x5E98u;
            constexpr u32 sentinel = 0x12345678u;
            cpu.reset();
            cpu.pc = 0x803B2F98u;
            cpu.lr = finish;
            cpu.gpr[1] = stack;
            cpu.gpr[13] = sda;
            cpu.fpr[1].bits[0] = bits;
            for (u32 i = 14; i < 32; ++i) {
                cpu.gpr[i] = 0x23456780u + i;
                cpu.fpr[i].bits[0] = bits ^ i;
            }
            memory.write32(error, sentinel);
            const auto tick = cpu.game_clock().now();
            const auto expected = shadow::math::square_root(bits_to_double(bits));
            recomp::fn_803B2F98(cpu);
            if (cpu.stopped() || cpu.pc != finish || cpu.gpr[1] != stack ||
                cpu.game_clock().now() != tick || cpu.fpr[1].bits[0] != double_to_bits(expected.value) ||
                memory.peek32(error) != (expected.domain_error ? 33u : sentinel))
                throw std::runtime_error("lowered square-root call violated its value/error/return contract");
            for (u32 i = 14; i < 32; ++i) {
                if (cpu.gpr[i] != 0x23456780u + i || cpu.fpr[i].bits[0] != (bits ^ i))
                    throw std::runtime_error("lowered square-root call clobbered nonvolatile state");
            }
            ++cases;
        };
        for (u64 bits : {0ull, 0x8000000000000000ull, 1ull, 0x8000000000000001ull,
                         0x3ff0000000000000ull, 0x4000000000000000ull, 0xbff0000000000000ull,
                         0x7ff0000000000000ull, 0xfff0000000000000ull, 0x7ff0000000000001ull,
                         0xfff123456789abcdull, 0x7fffffffffffffffull}) check(bits);
        for (unsigned i = 0; i < 20000; ++i) {
            random ^= random << 13; random ^= random >> 7; random ^= random << 17;
            check(random);
        }
        std::printf("PASS native scalar caller: %u cases; value, error, return, time and nonvolatile state\n", cases);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL native scalar caller: %s\n", error.what());
        return 1;
    }
}
