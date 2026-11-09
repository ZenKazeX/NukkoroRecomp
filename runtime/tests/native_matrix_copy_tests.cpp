// Original-word oracle for the affine matrix copy's value/memory contract.
// liveness-roots: reference-interpreter-only
#include "math/matrix.h"
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace {
using namespace gekko;
constexpr u32 kEntry = 0x8040CD04u, kEnd = kEntry + 32u, kDuplicate = 0x8040D198u;
constexpr u32 kBuffer = 0x81001000u, kReturn = 0x817FF000u, kStack = 0x817E0000u;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: native_matrix_copy_tests main.dol");
        GuestMemory memory;
        Cpu cpu(memory);
        std::ifstream input(argv[1], std::ios::binary);
        const std::vector<u8> dol{std::istreambuf_iterator<char>(input), {}};
        require(dol.size() >= 256, "missing DOL");
        const auto word = [&](std::size_t offset) { return load_be32(dol.data() + offset); };
        for (unsigned i = 0; i < 18; ++i) {
            const u32 offset = word(i * 4), address = word(0x48 + i * 4), size = word(0x90 + i * 4);
            require(u64(offset) + size <= dol.size(), "invalid DOL section");
            if (size) memory.write_block(address, dol.data() + offset, size);
        }
        require(memory.peek32(kEntry) == 0x3800000Cu && memory.peek32(kEnd - 4) == 0x4E800020u,
                "matrix copy reference differs");
        for (u32 offset = 0; offset < 32u; offset += 4u)
            require(memory.peek32(kEntry + offset) == memory.peek32(kDuplicate + offset),
                    "matrix copy duplicate differs from the complete original body");
        const std::array<u32, 12> exceptional{{0, 0x80000000u, 1, 0x807FFFFFu, 0x00800000u,
            0x7F7FFFFFu, 0x7F800000u, 0xFF800000u, 0x7F800001u, 0xFFC00001u, 0x7FFFFFFFu, 0xFFFFFFFFu}};
        u64 random = 0x774091bacc143e5full, instructions = 0;
        unsigned cases = 0;
        for (unsigned round = 0; round < 4096; ++round) {
            std::array<u8, 256> initial{};
            for (std::size_t i = 0; i < initial.size(); i += 4) {
                random ^= random << 13; random ^= random >> 7; random ^= random << 17;
                store_be32(initial.data() + i, round ? u32(random) : exceptional[(i / 4) % exceptional.size()]);
            }
            for (u32 entry : {kEntry, kDuplicate})
            for (int delta : {-52, -48, -44, -16, -4, 0, 4, 16, 44, 48, 52}) {
                auto actual = initial;
                memory.write_block(kBuffer, initial.data(), initial.size());
                cpu.reset();
                cpu.pc = entry;
                cpu.lr = kReturn;
                cpu.gpr[1] = kStack;
                cpu.gpr[3] = kBuffer + 96u + delta;
                cpu.gpr[4] = kBuffer + 96u;
                const auto start = cpu.game_clock().now();
                unsigned steps = 0;
                while (cpu.pc != kReturn && !cpu.stopped() && steps++ < 100)
                    cpu.run_steps(1);
                require(!cpu.stopped() && cpu.pc == kReturn && cpu.gpr[1] == kStack &&
                        cpu.gpr[3] == kBuffer + 96u + delta && cpu.game_clock().now() == start,
                        "original copy did not preserve its return, destination, stack or time");
                shadow::math::copy_affine_matrix(actual.data() + 96 + delta, actual.data() + 96);
                std::array<u8, 256> expected{};
                memory.peek_block(kBuffer, expected.data(), expected.size());
                if (actual != expected) {
                    std::fprintf(stderr, "matrix copy mismatch: round=%u destination offset=%d\n", round, delta);
                    throw std::runtime_error("native matrix copy differs from original words");
                }
                instructions += steps;
                ++cases;
            }
        }
        // Native storage additionally permits byte alignment and byte overlap.
        for (unsigned source = 49; source < 53; ++source) for (unsigned destination = 1; destination < 130; ++destination) {
            std::array<u8, 256> expected{}, actual{};
            for (unsigned i = 0; i < expected.size(); ++i) expected[i] = u8(i * 37u);
            actual = expected;
            for (unsigned i = 0; i < 48; i += 4) {
                const std::array<u8, 4> element{{expected[source + i], expected[source + i + 1],
                                               expected[source + i + 2], expected[source + i + 3]}};
                std::memcpy(expected.data() + destination + i, element.data(), element.size());
            }
            shadow::math::copy_affine_matrix(actual.data() + destination, actual.data() + source);
            require(actual == expected, "unaligned native storage or overlap failed");
        }
        // Execute both enclosing algorithms from original words. Replacing only
        // the copy leaves its old volatile registers untouched, just as the
        // generated boundary does. This catches callers relying on those values.
        unsigned caller_cases = 0;
        std::array<unsigned, 2> caller_hits{};
        const auto enclosing = [&](u32 entry, const std::array<u8, 256>& data,
                                   bool alias, bool native, unsigned index) {
            memory.write_block(kBuffer, data.data(), data.size());
            std::array<u8, 1040> stack_bytes{};
            stack_bytes.fill(0xA5u);
            memory.write_block(kStack - 1024u, stack_bytes.data(), stack_bytes.size());
            cpu.reset();
            cpu.pc = entry;
            cpu.lr = kReturn;
            cpu.gpr[1] = kStack;
            cpu.gpr[2] = 0x805FA780u;
            cpu.gpr[13] = 0x805EC500u;
            cpu.gpr[3] = kBuffer + 32u;
            cpu.gpr[4] = kBuffer + (alias ? 32u : 128u);
            const auto tick = cpu.game_clock().now();
            unsigned steps = 0;
            while (cpu.pc != kReturn && !cpu.stopped() && steps++ < 2000) {
                if (cpu.pc == kEntry) {
                    if (!native) ++caller_hits[index];
                    else {
                        const auto* source = memory.host_pointer(cpu.gpr[4], 48u);
                        require(source && memory.write_in_place(cpu.gpr[3], 48u, [&](void* destination) {
                            shadow::math::copy_affine_matrix(destination, source);
                        }), "enclosing caller copy extent unavailable");
                        cpu.pc = cpu.lr;
                        continue;
                    }
                }
                cpu.run_steps(1);
            }
            require(!cpu.stopped() && cpu.pc == kReturn && cpu.gpr[1] == kStack &&
                    cpu.game_clock().now() == tick, "enclosing matrix caller did not return correctly");
            std::array<u8, 1300> result{};
            memory.peek_block(kBuffer, result.data(), 256u);
            memory.peek_block(kStack - 1024u, result.data() + 256u, 1040u);
            store_be32(result.data() + 1296u, cpu.gpr[3]);
            return result;
        };
        for (unsigned round = 0; round < 256; ++round) {
            std::array<u8, 256> data{};
            for (unsigned i = 0; i < data.size(); i += 4) {
                random ^= random << 13; random ^= random >> 7; random ^= random << 17;
                const float value = float(int(random % 2049u) - 1024) / 128.0f;
                u32 bits;
                std::memcpy(&bits, &value, sizeof(bits));
                store_be32(data.data() + i, bits);
            }
            for (bool alias : {false, true}) {
                const auto original = enclosing(0x8040CB14u, data, alias, false, 0);
                require(enclosing(0x8040CB14u, data, alias, true, 0) == original,
                        "enclosing matrix multiply output or stack differs");
                ++caller_cases;
            }
            // Three zero linear rows guarantee the singular branch; translation
            // elements stay varied. Exercise separate and self-copy destinations.
            for (unsigned at : {32u, 128u}) for (unsigned row = 0; row < 3; ++row)
                for (unsigned col = 0; col < 3; ++col) store_be32(data.data() + at + row * 16u + col * 4u, 0);
            for (bool alias : {false, true}) {
                const auto original = enclosing(0x8040CF48u, data, alias, false, 1);
                require(enclosing(0x8040CF48u, data, alias, true, 1) == original,
                        "enclosing singular inverse output or stack differs");
                ++caller_cases;
            }
        }
        require(caller_hits[0] == 512 && caller_hits[1] == 512,
                "an enclosing matrix copy site was not exercised");
        // Original matrix-stack push: duplicate the current 3x4 matrix and
        // advance its index. Execute the complete real caller with/without the
        // duplicate copy, retaining arbitrary volatile register starting values.
        unsigned stack_push_cases = 0;
        for (unsigned seed = 0; seed < 256; ++seed) for (u32 index = 0; index < 3; ++index) {
            const auto push = [&](bool native) {
                std::array<u8, 256> data{};
                for (unsigned i = 0; i < data.size(); ++i) data[i] = u8(seed * 71u + i * 37u);
                store_be32(data.data(), kBuffer + 64u);
                store_be32(data.data() + 4u, 4u);
                store_be32(data.data() + 8u, index);
                memory.write_block(kBuffer, data.data(), data.size());
                std::array<u8, 1040> stack{}; stack.fill(0xA5u);
                memory.write_block(kStack - 1024u, stack.data(), stack.size());
                cpu.reset(); cpu.pc = 0x8040D254u; cpu.lr = kReturn;
                cpu.gpr[1] = kStack; cpu.gpr[3] = kBuffer;
                cpu.gpr[0] = seed * 17u; cpu.gpr[5] = seed * 29u; cpu.ctr = seed * 31u;
                cpu.fpr[0].bits[0] = 0x1234567800000000ull + seed;
                const auto tick = cpu.game_clock().now();
                unsigned steps = 0, hits = 0;
                memory.set_access_trace([&](u32 address, std::size_t size, bool writing) {
                    if (writing) require((address >= kBuffer && u64(address) + size <= kBuffer + data.size()) ||
                        (address >= kStack - 1024u && u64(address) + size <= kStack + 16u),
                        "matrix-stack push wrote outside owned output/stack");
                });
                while (cpu.pc != kReturn && !cpu.stopped() && steps++ < 128u) {
                    if (cpu.pc == kDuplicate) {
                        ++hits;
                        if (native) {
                            const auto* source = memory.host_pointer(cpu.gpr[4], 48u);
                            require(source && memory.write_in_place(cpu.gpr[3], 48u, [&](void* destination) {
                                shadow::math::copy_affine_matrix(destination, source);
                            }), "matrix-stack copy spans unavailable");
                            cpu.pc = cpu.lr;
                            continue;
                        }
                    }
                    cpu.run_steps(1);
                }
                memory.set_access_trace({});
                require(hits == 1u && !cpu.stopped() && cpu.pc == kReturn && cpu.gpr[1] == kStack &&
                        cpu.game_clock().now() == tick && memory.peek32(kBuffer + 8u) == index + 1u,
                        "matrix-stack push contract changed");
                std::array<u8, 1300> result{};
                memory.peek_block(kBuffer, result.data(), 256u);
                memory.peek_block(kStack - 1024u, result.data() + 256u, 1040u);
                store_be32(result.data() + 1296u, cpu.gpr[3]);
                return result;
            };
            const auto original = push(false);
            require(push(true) == original, "matrix-stack push output/stack/return differs");
            ++stack_push_cases;
        }
        std::printf("PASS duplicate-copy matrix-stack caller: %u paired calls, full output/stack/return equality\n",
                    stack_push_cases);
        std::printf("PASS enclosing matrix algorithms: %u paired calls, copy sites [%u,%u], "
                    "output and complete initialized stack identical\n", caller_cases, caller_hits[0], caller_hits[1]);
        std::printf("PASS matrix copy: %u original-word cases, %llu instructions, whole-buffer equality; "
                    "NaN payloads, both overlap directions, self-copy; 516 unaligned native cases\n", cases, instructions);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL matrix copy: %s\n", error.what());
        return 1;
    }
}
