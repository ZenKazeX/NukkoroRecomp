// Independent original-instruction oracle for removed, unused argument frames.
// Original callers use the interpreter; the selected leaves use generated code.
#include "recomp_decls.h"

#include <array>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace {
using namespace gekko;
constexpr std::array<u32, 3> entries{0x8000D374u, 0x8001C574u, 0x8047EC78u};
constexpr std::array<u32, 3> sizes{80, 84, 104};
constexpr u32 kStack = 0x817D0000u, kReturn = 0x817FF000u;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

struct State {
    std::array<u32, 32> gpr{};
    std::array<u64, 64> fpr{};
    u32 cr, xer, ctr, lr, pc, fpscr, msr;
    u64 time;
    bool operator==(const State& b) const {
        return gpr == b.gpr && fpr == b.fpr && cr == b.cr && xer == b.xer &&
               ctr == b.ctr && lr == b.lr && pc == b.pc && fpscr == b.fpscr &&
               msr == b.msr && time == b.time;
    }
};

struct Oracle {
    GuestMemory memory;
    Cpu cpu{memory};
    RecompiledCode code;
    std::array<std::vector<u8>, 3> original;
    u64 instructions = 0, original_write_bytes = 0;
    unsigned cases = 0, dispatch_cases = 0, cut_cases = 0, caller_cases = 0;
    std::array<unsigned, 3> caller_hits{};

    explicit Oracle(const char* path) {
        std::ifstream input(path, std::ios::binary);
        const std::vector<u8> dol{std::istreambuf_iterator<char>(input), {}};
        require(dol.size() >= 256, "missing original DOL");
        u64 hash = 14695981039346656037ull;
        for (u8 byte : dol) hash = (hash ^ byte) * 1099511628211ull;
        require(hash == 0xd2cfb4a1ca897f9bull, "original image changed");
        for (unsigned i = 0; i < 18; ++i) {
            const u32 offset = load_be32(dol.data() + i*4);
            const u32 address = load_be32(dol.data() + 0x48 + i*4);
            const u32 size = load_be32(dol.data() + 0x90 + i*4);
            require(u64(offset) + size <= dol.size(), "invalid original DOL section");
            if (size) memory.write_block(address, dol.data() + offset, size);
        }
        require(recomp::kFunctionCount == 3 && recomp::kRangeCount == 0 && recomp::kLiftSiteCount == 3,
                "fixture contains unexpected bodies, ranges or dispatch records");
        code.install(recomp::kFunctions, recomp::kFunctionCount);
        code.install_ranges(recomp::kRanges, recomp::kRangeCount);
        for (unsigned i = 0; i < entries.size(); ++i) {
            original[i].resize(sizes[i]);
            memory.read_block(entries[i], original[i].data(), sizes[i]);
            require(recomp::kFunctions[i].address == entries[i] && code.lookup(entries[i]),
                    "missing generated semantic entry");
            for (u32 offset = 4; offset < sizes[i]; offset += 4)
                require(!code.lookup(entries[i] + offset) && !code.find_range(entries[i] + offset),
                        "retired interior instruction remains dispatchable");
        }
    }

    State state() const {
        State result{};
        for (unsigned i = 0; i < 32; ++i) {
            result.gpr[i] = cpu.gpr[i];
            result.fpr[i*2] = cpu.fpr[i].bits[0];
            result.fpr[i*2+1] = cpu.fpr[i].bits[1];
        }
        result.cr = cpu.cr; result.xer = cpu.xer; result.ctr = cpu.ctr; result.lr = cpu.lr;
        result.pc = cpu.pc; result.fpscr = cpu.fpscr; result.msr = cpu.msr();
        result.time = cpu.game_clock().now();
        return result;
    }

    void reset(unsigned leaf, unsigned sample, bool floating, u32 fpscr) {
        cpu.reset();
        cpu.attach_recompiled(nullptr); cpu.set_lift_sites(nullptr, 0);
        cpu.set_verify_recompiled(false);
        u64 random = 0xD1B54A32D192ED03ull ^ (u64(sample) * 0x9E3779B97F4A7C15ull);
        auto next = [&] { random ^= random << 13; random ^= random >> 7; random ^= random << 17; return random; };
        constexpr u64 special[] = {0, 0x8000000000000000ull, 1, 0x000FFFFFFFFFFFFFull,
            0x0010000000000000ull, 0x7FF0000000000000ull, 0xFFF0000000000000ull,
            0x7FF0000000000001ull, 0x7FF8000000000001ull, 0x7FEFFFFFFFFFFFFFull};
        for (unsigned i = 0; i < 32; ++i) {
            cpu.gpr[i] = static_cast<u32>(next());
            for (unsigned lane = 0; lane < 2; ++lane)
                cpu.fpr[i].bits[lane] = sample < 10 ? special[(sample+i+lane)%10] : next();
        }
        cpu.gpr[1] = kStack - (sample%16)*256;
        cpu.pc = entries[leaf]; cpu.lr = kReturn + sample%128;
        cpu.cr = static_cast<u32>(next());
        cpu.cr = (cpu.cr & ~0x02000000u) | (floating ? 0x02000000u : 0u);
        cpu.xer = static_cast<u32>(next()); cpu.ctr = static_cast<u32>(next());
        cpu.fpscr = fpscr; cpu.set_msr(cpu.msr() | kMsrFP);
        for (u32 address = cpu.gpr[1]-160; address < cpu.gpr[1]+32; ++address)
            memory.write8(address, static_cast<u8>(address + sample));
    }

    void check(unsigned leaf, unsigned sample, bool floating, u32 fpscr) {
        reset(leaf, sample, floating, fpscr);
        memory.write_block(entries[leaf], original[leaf].data(), sizes[leaf]);
        const u32 stack = cpu.gpr[1], end = cpu.lr & ~3u, frame = leaf == 2 ? 128u : 112u;
        std::array<u8, 192> before{}, after{};
        memory.peek_block(stack-160, before.data(), before.size());
        unsigned written = 0, fetches = 0;
        std::array<bool, 192> touched{};
        memory.set_access_trace([&](u32 address, std::size_t size, bool writing) {
            // The reference interpreter reports instruction fetches through
            // the same trace as data. Admit only its current four-byte word.
            if (!writing && address == cpu.pc && size == 4 &&
                address >= entries[leaf] && address < entries[leaf] + sizes[leaf]) {
                ++fetches; return;
            }
            require(writing, "original empty leaf unexpectedly read data");
            require(address >= stack-frame && u64(address)+size <= stack,
                    "original empty leaf published data outside its frame");
            written += static_cast<unsigned>(size);
            for (std::size_t i = 0; i < size; ++i) touched[address-(stack-160)+i] = true;
        });
        unsigned steps = 0;
        while (cpu.pc != end && !cpu.stopped() && steps++ < 32) cpu.run_steps(1);
        memory.set_access_trace({});
        require(!cpu.stopped() && cpu.pc == end, "original empty leaf did not return");
        require(fetches == steps, "unexpected instruction-fetch trace population");
        require(written == (leaf == 2 ? 48u : 36u) + (floating ? 64u : 0u),
                "original argument-save traffic differs from contract");
        instructions += steps; original_write_bytes += written;
        const State expected = state();
        memory.peek_block(stack-160, after.data(), after.size());
        for (unsigned i = 0; i < before.size(); ++i)
            require(before[i] == after[i] || touched[i], "unattributed original memory difference");
        reset(leaf, sample, floating, fpscr);
        // No original instruction or other retired body may serve the replacement.
        for (unsigned i = 0; i < entries.size(); ++i) memory.clear_block(entries[i], sizes[i]);
        memory.set_access_trace([](u32, std::size_t, bool) {
            throw std::runtime_error("generated empty leaf performed a data access");
        });
        code.lookup(entries[leaf])(cpu);
        memory.set_access_trace({});
        require(!cpu.stopped() && !cpu.exiting() && state() == expected,
                "generated semantic result differs from original register/clock state");
        memory.peek_block(stack-160, after.data(), after.size());
        require(before == after, "generated empty leaf changed stack bytes");
        ++cases;
    }

    void boundaries() {
        for (unsigned leaf = 0; leaf < entries.size(); ++leaf) {
            reset(leaf, 11, true, 4);
            code.lookup(entries[leaf])(cpu);
            const State expected = state();
            for (bool attached : {false, true}) {
                reset(leaf, 11, true, 4);
                if (attached) cpu.attach_recompiled(&code);
                cpu.set_lift_sites(recomp::kLiftSites, recomp::kLiftSiteCount);
                cpu.run_steps(1);
                require(!cpu.stopped() && state() == expected && cpu.npc == cpu.pc,
                        "dispatcher changed prepared return or dispatched the continuation twice");
                ++dispatch_cases;
            }
            reset(leaf, 12, true, 4);
            cpu.set_lift_sites(recomp::kLiftSites, recomp::kLiftSiteCount);
            cpu.set_verify_recompiled(true);
            const State before = state();
            code.lookup(entries[leaf])(cpu);
            require(state() == before, "verification boundary executed semantic writes before cutting");
            ++cut_cases;
        }
    }

    struct CallerResult {
        State state;
        std::array<u8, 2112> stack{};
        std::array<u8, 1024> objects{};
        std::array<u32, 4> globals{};
        std::array<bool, 2112> retired{};
        unsigned hits = 0, allocations = 0;
    };

    CallerResult caller(unsigned leaf, unsigned sample, bool native) {
        constexpr u32 object = 0x81000000u, allocation = object+256u, errors = object+512u;
        constexpr u32 low = kStack-2048u, sda = 0x805EC500u;
        constexpr u32 callers[] = {0x801612B4u, 0x800543C0u, 0x8049C90Cu};
        reset(leaf, sample, false, 4);
        cpu.pc = callers[leaf]; cpu.lr = kReturn; cpu.gpr[1] = kStack; cpu.gpr[13] = sda;
        cpu.gpr[3] = leaf == 2 ? (sample%16)*4u : object;
        cpu.gpr[4] = object+128u; cpu.gpr[5] = object+192u;
        for (u32 a = low; a < kStack+64; ++a) memory.write8(a, static_cast<u8>(a+sample));
        for (u32 i = 0; i < 1024; ++i) memory.write8(object+i, static_cast<u8>(i*17+sample));
        memory.write32(sda+0x2B08, object+768u);
        memory.write32(sda+0x2B0C, object+752u);
        memory.write32(sda+0x615C, errors);
        memory.write32(sda+0x6100, 0);
        memory.write32(errors, sample%3 == 1 ? 1u : 0u);
        memory.write32(errors+4, sample%3 == 2 ? 0u : 0x80000000u);
        for (unsigned i = 0; i < entries.size(); ++i) {
            if (native) memory.clear_block(entries[i], sizes[i]);
            else memory.write_block(entries[i], original[i].data(), sizes[i]);
        }
        CallerResult result{};
        bool inside = false;
        u32 leaf_return = 0, leaf_bottom = 0, leaf_top = 0;
        memory.set_access_trace([&](u32 address, std::size_t size, bool writing) {
            if (!writing && address == cpu.pc && size == 4) return; // exact instruction fetch
            if (writing)
                require((address >= low && u64(address)+size <= kStack+64) ||
                        (address >= object && u64(address)+size <= object+1024) ||
                        (address == sda+0x2B08 && size == 4), "caller wrote outside its fixture-owned data");
            for (std::size_t i = 0; i < size; ++i) {
                const u64 at = u64(address)+i;
                if (at < low || at >= kStack+64) continue;
                const auto index = static_cast<std::size_t>(at-low);
                if (writing) result.retired[index] = inside && at >= leaf_bottom && at < leaf_top;
                else if (!inside) require(!result.retired[index], "caller read removed frame bytes before overwrite");
            }
        });
        unsigned steps = 0;
        while (cpu.pc != kReturn && !cpu.stopped() && steps++ < 2000) {
            if (inside && cpu.pc == leaf_return) inside = false;
            if (cpu.pc == 0x803A1380u) {
                // Explicit fixture service: the two constructors request one
                // 20-byte allocation. Both modes receive the same owned block.
                // This test makes no assertion about the original allocator.
                require(leaf != 2 && cpu.gpr[3] == 20 && result.allocations++ == 0,
                        "constructor changed its allocation contract");
                cpu.gpr[3] = allocation; cpu.pc = cpu.lr & ~3u;
                continue;
            }
            if (cpu.pc == entries[leaf]) {
                ++result.hits;
                leaf_return = cpu.lr & ~3u; leaf_top = cpu.gpr[1];
                leaf_bottom = leaf_top - (leaf == 2 ? 128u : 112u);
                if (native) { code.lookup(entries[leaf])(cpu); continue; }
                inside = true;
            }
            cpu.run_steps(1); ++instructions;
        }
        memory.set_access_trace({});
        require(!cpu.stopped() && cpu.pc == kReturn && cpu.gpr[1] == kStack,
                "original caller did not return with its stack restored");
        result.state = state();
        memory.peek_block(low, result.stack.data(), result.stack.size());
        memory.peek_block(object, result.objects.data(), result.objects.size());
        result.globals = {memory.peek32(sda+0x2B08), memory.peek32(sda+0x2B0C),
                          memory.peek32(sda+0x615C), memory.peek32(sda+0x6100)};
        return result;
    }

    void callers() {
        for (unsigned leaf = 0; leaf < entries.size(); ++leaf) for (unsigned sample = 0; sample < 96; ++sample) {
            const auto expected = caller(leaf, sample, false);
            const auto actual = caller(leaf, sample, true);
            require(expected.state == actual.state && expected.objects == actual.objects &&
                    expected.globals == actual.globals && expected.hits == actual.hits &&
                    expected.allocations == actual.allocations, "original caller observable state changed");
            for (unsigned i = 0; i < expected.stack.size(); ++i)
                require(expected.stack[i] == actual.stack[i] || expected.retired[i],
                        "caller stack difference was not an unread retired argument-save byte");
            caller_hits[leaf] += actual.hits; ++caller_cases;
        }
        for (unsigned hits : caller_hits) require(hits != 0, "caller fixture never reached its selected leaf");
    }
};
}

int main(int argc, char** argv) {
    try {
        require(argc == 2, "expected original DOL argument");
        Oracle oracle(argv[1]);
        for (unsigned leaf = 0; leaf < entries.size(); ++leaf)
            for (u32 mode : {0u, 4u, 0xFFFFFFFFu, 0xF1234567u})
                for (bool floating : {false, true})
                    for (unsigned sample = 0; sample < 512; ++sample)
                        oracle.check(leaf, sample, floating, mode);
        oracle.boundaries();
        oracle.callers();
        std::printf("PASS: %u original comparisons, %llu original instructions, %llu original private write bytes; "
                    "generated bodies perform zero data accesses; %u dispatcher and %u verification-cut cases.\n",
                    oracle.cases, oracle.instructions, oracle.original_write_bytes,
                    oracle.dispatch_cases, oracle.cut_cases);
        std::printf("PASS: %u caller pairs, leaf hits %u/%u/%u; all register/object/global state agrees, "
                    "no removed stack byte read before overwrite. Two constructors use an explicit 20-byte fixture allocator.\n",
                    oracle.caller_cases, oracle.caller_hits[0], oracle.caller_hits[1], oracle.caller_hits[2]);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
