// liveness-roots: reference-interpreter-only
// CRI's ADX output to the AX voices (gekko/native_adx_output.h) against the ORIGINAL words of the game's DOL, in the
// reference interpreter. One machine runs the original output (0x803CEEB4 / 0x803CEF10 / 0x803CF408 and the two
// completions) with the original SJ code; its ARQPostRequest (0x80384500) is a stand-in that does what the runtime's
// transfer did until 2026-11-03 - the record filled, the chunk copied, the completion callback run before the spin
// reads its busy word (the runtime delivered it at the spin's first back edge). The other machine serves the output
// with NativeAdxOutput over NativeSj. Scenario: three handles in the table - a stereo stream playing and consuming,
// a mono stream consuming then playing, a silence fill - with SJRBF input / output rings, fed with seeded random
// amounts of PCM, the voices' play positions advanced each pass (including wrap-around and repeats); the server
// runs every handle each pass, single handles and the consume step are also called directly. After every call both
// machines must agree on the handle table, the 32-entry position history, both SJ pools, every buffer byte (input
// rings, output rings, zero buffer) and the transfer records. Also: NativeAdxOutput::addresses() equals its section
// of extract/native-owners.txt.
//
// usage: native_adx_output_tests <sys/main.dol> <extract/native-owners.txt>

#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_adx_output.h"
#include "gekko/native_sj.h"
#include "gekko/native_svm.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace gekko;

constexpr u32 kExecAll = 0x803CEEB4u, kExec = 0x803CEF10u, kConsume = 0x803CF408u;
constexpr u32 kArqPost = 0x80384500u, kDcFlushRange = 0x80372534u;
constexpr u32 kRbfCreate = 0x803CB9BCu, kRbfInit = 0x803CBBE4u;
constexpr u32 kGetChunk = 0x18, kPutChunk = 0x20;

constexpr u32 kHandles = 0x805C3E4Cu, kHandleSize = 0xECu;
constexpr u32 kZeroWord = 0x805C2D7Cu;

constexpr u32 kR13 = 0x805EC500u, kR2 = 0x805FA780u;
constexpr u32 kFinishPc = 0x817FF000u, kStack = 0x817E0000u;
constexpr u32 kBuffers = 0x81400000u, kBuffersSize = 0x40000u;
constexpr u32 kVoices = 0x81480000u;  // fake AX parameter blocks (only +0x1B2 is read)
constexpr u32 kChunk = 0x81490000u;   // the test's own chunk struct

struct Region {
    u32 at, size;
};
constexpr Region kRegions[] = {
    {kHandles, 16u * kHandleSize}, {0x805C2D7Cu, 0x4u}, {0x805C2D88u, 0x84u},
    {0x805BBD78u, 4u}, {0x805BBD80u, 8u}, {0x805BC210u, 4u + 0x4000u}, {kBuffers, kBuffersSize},
};

void require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}
std::string hex(u32 v) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08X", v);
    return text;
}
int g_failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) {
        ++g_failures;
        if (g_failures < 40) std::printf("FAIL: %s\n", what.c_str());
    }
}

struct Machine {
    GuestMemory bus;
    Cpu cpu{bus};
    NativeSvm svm{cpu};
    NativeSj sj{cpu, svm};
    NativeAdxOutput adx{cpu, sj, [](Cpu& c, u32 source, u32 destination, u32 bytes, std::string&) {
                            for (u32 i = 0; i < bytes; ++i) c.mem_write8(destination + i, c.mem_read8(source + i));
                            return true;
                        }};
    bool native;
    u64 transfers = 0;

    Machine(const std::vector<u8>& dol, bool native_output) : native(native_output) {
        const auto word = [&](std::size_t offset) { return load_be32(dol.data() + offset); };
        for (u32 i = 0; i < 18u; ++i) {
            const u32 offset = word(i * 4u), address = word(0x48u + i * 4u), size = word(0x90u + i * 4u);
            if (!size) continue;
            require(std::uint64_t(offset) + size <= dol.size(), "invalid DOL extent");
            bus.write_block(address, dol.data() + offset, size);
        }
        require(bus.read32(kExecAll) == 0x9421FFF0u && bus.read32(kExec) == 0x9421FFB0u,
                "unexpected ADX output instruction evidence");
        cpu.reset();
        cpu.gpr[2] = kR2;
        cpu.gpr[13] = kR13;
        cpu.set_msr(cpu.msr() | kMsrDR | kMsrIR | kMsrFP | kMsrEE);
        if (native) {
            sj.install();
            adx.install();
        } else {
            // ARQPostRequest(request, owner, type, priority, source, destination, bytes, callback) as the runtime
            // served it: the record, the copy, then the completion before the spin's first read of its busy word.
            cpu.install_hle(
                kArqPost,
                [this](Cpu& c) {
                    const u32 request = c.gpr[3], source = c.gpr[7], destination = c.gpr[8], bytes = c.gpr[9],
                              callback = c.gpr[10];
                    c.mem_write32(request + 0x0u, 0u);
                    c.mem_write32(request + 0x4u, c.gpr[4]);
                    c.mem_write32(request + 0x8u, c.gpr[5]);
                    c.mem_write32(request + 0x10u, source);
                    c.mem_write32(request + 0x14u, destination);
                    c.mem_write32(request + 0x18u, bytes);
                    c.mem_write32(request + 0x1Cu, callback);
                    for (u32 i = 0; i < bytes; ++i) c.mem_write8(destination + i, c.mem_read8(source + i));
                    ++transfers;
                    c.gpr[3] = request;
                    c.pc = c.npc = callback;  // returns to the poster's LR: the spin, which then sees busy = 0
                    return true;
                },
                "test: ARQPostRequest stand-in");
            // DCFlushRange: a cache flush (it ends in `sc`, which this bare machine has no OS vector for); no memory
            // effect, and the native output does not call it.
            cpu.install_hle(kDcFlushRange, [](Cpu&) { return true; }, "test: DCFlushRange (no effect)");
        }
    }

    bool call(u32 entry, std::vector<u32> args, u32* result = nullptr) {
        u32 r = 3u;
        for (const u32 v : args) cpu.gpr[r++] = v;
        cpu.gpr[1] = kStack;
        cpu.lr = kFinishPc;
        cpu.pc = entry;
        u64 steps = 0;
        while (cpu.pc != kFinishPc && !cpu.stopped() && steps++ < 20'000'000ull) {
            decltype(&cpu.hle_entries().front()) hook = nullptr;
            for (const auto& e : cpu.hle_entries())
                if (e.address == cpu.pc) hook = &e;
            if (hook != nullptr) {
                const u32 before = cpu.pc;
                if (!hook->handler(cpu)) return false;
                if (cpu.pc == before) cpu.pc = cpu.npc = cpu.lr;
                continue;
            }
            cpu.run_steps(1u);
        }
        if (result) *result = cpu.gpr[3];
        const bool ok = cpu.pc == kFinishPc && !cpu.stopped() && cpu.gpr[1] == kStack;
        if (!ok)
            std::printf("  %s machine: pc %s lr %s r1 %s steps %llu stop '%s' at %s\n", native ? "native" : "original",
                        hex(cpu.pc).c_str(), hex(cpu.lr).c_str(), hex(cpu.gpr[1]).c_str(),
                        static_cast<unsigned long long>(steps), cpu.stop().detail.c_str(),
                        hex(cpu.stop().address).c_str());
        return ok;
    }
};

std::vector<u8> read_file(const char* path) {
    std::ifstream input(path, std::ios::binary);
    return std::vector<u8>{std::istreambuf_iterator<char>(input), {}};
}

struct Pair {
    Machine& o;
    Machine& n;
    u64 calls = 0;

    u32 run(const std::string& what, u32 entry, std::vector<u32> args) {
        u32 ro = 0, rn = 0;
        ++calls;
        check(o.call(entry, args, &ro), what + ": the original returned");
        check(n.call(entry, args, &rn), what + ": the native output returned");
        compare(what);
        return ro;
    }
    void compare(const std::string& what) {
        for (const Region& g : kRegions)
            for (u32 a = g.at; a < g.at + g.size; a += 4u)
                if (o.bus.read32(a) != n.bus.read32(a)) {
                    check(false, what + ": word at " + hex(a) + " " + hex(o.bus.read32(a)) + " vs " + hex(n.bus.read32(a)));
                    return;
                }
    }
    void poke(u32 at, u32 value) {
        o.bus.write32(at, value);
        n.bus.write32(at, value);
    }
    void poke16(u32 at, u32 value) {
        o.bus.write16(at, static_cast<u16>(value));
        n.bus.write16(at, static_cast<u16>(value));
    }
};

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::printf("usage: native_adx_output_tests <sys/main.dol> <extract/native-owners.txt>\n");
        return 2;
    }
    try {
        const std::vector<u8> dol = read_file(argv[1]);
        require(dol.size() > 0x100u, "cannot read the DOL");
        Machine original(dol, false), native(dol, true);
        Pair p{original, native};
        u32 seed = 0xADC0u;
        const auto next = [&seed](u32 n) {
            seed = seed * 1103515245u + 12345u;
            return (seed >> 16) % n;
        };

        p.run("SJRBF_Init", kRbfInit, {});
        // Rings: per channel an input of 0x3000 bytes with a 0x400 mirror (decoded PCM) and an output of 0x2000 bytes
        // (0x1000 samples, the AX ring), as ADXT sets them up. A zero buffer of 0x2000 bytes.
        u32 at = kBuffers;
        const auto ring = [&](u32 size, u32 extra) {
            const u32 sj = p.run("SJRBF_Create", kRbfCreate, {at, size, extra});
            require(sj != 0u, "ring");
            at += size + extra + 0x100u;
            return sj;
        };
        struct Stream {
            u32 handle, channels, flags;
            u32 in[2], out[2], voice[2], base[2];
            u32 position = 0;
        };
        Stream streams[3] = {{kHandles + 0u * kHandleSize, 2u, 3u}, {kHandles + 5u * kHandleSize, 1u, 2u},
                             {kHandles + 9u * kHandleSize, 1u, 2u}};
        const u32 zero = at;
        at += 0x2100u;
        p.poke(kZeroWord, zero);
        for (u32 s = 0; s < 3u; ++s) {
            Stream& st = streams[s];
            const u32 h = st.handle, index = (h - kHandles) / kHandleSize;
            for (u32 a = h; a < h + kHandleSize; a += 4u) p.poke(a, 0u);
            p.poke(h, 0x01000000u | (st.flags << 16) | st.channels);  // used, flags, -, channel count
            p.poke(h + 0x04u, 0xFFFFFFFFu);                            // no consume cursor yet
            p.poke(h + 0x20u, 0x1000u);                                // the ring's length in samples
            for (u32 ch = 0; ch < st.channels; ++ch) {
                st.in[ch] = ring(0x3000u, 0x400u);
                st.out[ch] = ring(0x2000u, 0u);
                st.voice[ch] = kVoices + 0x200u * (s * 2u + ch);
                st.base[ch] = 0x01000000u + 0x10000u * (s * 2u + ch);  // the voice's ring base address (samples)
                p.poke(h + 0x08u + 4u * ch, st.voice[ch]);
                p.poke(h + 0x18u + 4u * ch, st.base[ch]);
                p.poke(h + 0x28u + 4u * ch, index * 2u + ch);
                p.poke(h + 0x30u + 4u * ch, st.in[ch]);
                p.poke(h + 0x38u + 4u * ch, st.out[ch]);
                p.poke(st.voice[ch] + 0x1B0u, 0u);
                p.poke(st.voice[ch] + 0x1B4u, 0u);
                p.poke16(st.voice[ch] + 0x1B2u, st.base[ch] >> 16);
                p.poke16(st.voice[ch] + 0x1B4u, st.base[ch] & 0xFFFFu);
            }
        }

        const auto feed = [&](u32 sj, u32 bytes, u32 tag) {  // decoded PCM into an input ring (get free, fill, put data)
            const u32 vt = original.bus.read32(sj);
            p.run("feed get", original.bus.read32(vt + kGetChunk), {sj, 0u, bytes, kChunk});
            const u32 data = original.bus.read32(kChunk), len = original.bus.read32(kChunk + 4u);
            for (u32 i = 0; i + 4u <= len; i += 4u) p.poke(data + i, tag ^ (i * 0x9E3779B1u));
            p.run("feed put", original.bus.read32(vt + kPutChunk), {sj, 1u, kChunk});
        };
        const auto play = [&](Stream& st, u32 samples) {  // the voices' play position moves on
            st.position = (st.position + samples) % 0x1000u;
            for (u32 ch = 0; ch < st.channels; ++ch) {
                const u32 v = st.base[ch] + st.position;
                p.poke16(st.voice[ch] + 0x1B2u, v >> 16);
                p.poke16(st.voice[ch] + 0x1B4u, v & 0xFFFFu);
            }
        };

        for (u32 pass = 0; pass < 600u; ++pass) {
            const std::string w = "pass " + std::to_string(pass);
            for (u32 s = 0; s < 2u; ++s)
                for (u32 ch = 0; ch < streams[s].channels; ++ch)
                    if (next(3u) != 0u) feed(streams[s].in[ch], 0x100u + next(0x1400u), (pass << 16) | (s << 8) | ch);
            for (Stream& st : streams)
                if (next(4u) != 0u) play(st, next(5u) == 0u ? 0u : next(0x900u));
            if (pass == 200u) p.poke(streams[1].handle, original.bus.read32(streams[1].handle) | 0x00010000u);  // + play
            switch (next(8u)) {
                case 0:
                    p.run(w + " one handle", kExec, {streams[next(3u)].handle});
                    break;
                case 1:
                    p.run(w + " consume", kConsume, {streams[next(2u)].handle});
                    break;
                default:
                    p.run(w + " every handle", kExecAll, {});
                    break;
            }
        }
        p.run("null handle", kExec, {0u});
        check(native.adx.stats().transfers > 100u && native.adx.stats().silence_transfers > 0u &&
                  native.adx.stats().consumes > 100u,
              "the scenario exercised play, silence and consume");
        check(original.transfers == native.adx.stats().transfers + native.adx.stats().silence_transfers,
              "transfer counts: original " + std::to_string(original.transfers));
        std::printf("calls %llu; native: passes %llu, handles %llu, consumes %llu (samples %llu), transfers %llu "
                    "(%llu bytes), silence transfers %llu (%llu bytes); original transfers %llu\n",
                    static_cast<unsigned long long>(p.calls), static_cast<unsigned long long>(native.adx.stats().passes),
                    static_cast<unsigned long long>(native.adx.stats().handles),
                    static_cast<unsigned long long>(native.adx.stats().consumes),
                    static_cast<unsigned long long>(native.adx.stats().consumed_samples),
                    static_cast<unsigned long long>(native.adx.stats().transfers),
                    static_cast<unsigned long long>(native.adx.stats().transfer_bytes),
                    static_cast<unsigned long long>(native.adx.stats().silence_transfers),
                    static_cast<unsigned long long>(native.adx.stats().silence_bytes),
                    static_cast<unsigned long long>(original.transfers));

        std::set<u32> listed;
        {
            std::ifstream owners(argv[2]);
            std::string line;
            bool in_section = false;
            while (std::getline(owners, line)) {
                if (!line.empty() && line[0] == '#' && line.find("runtime/src/gekko/") != std::string::npos)
                    in_section = line.find("runtime/src/gekko/native_adx_output.cpp") != std::string::npos;
                if (!in_section || line.empty() || line[0] == '#') continue;
                listed.insert(static_cast<u32>(std::stoul(line.substr(0, 10), nullptr, 16)));
            }
        }
        std::size_t count = 0;
        const NativeAdxOutput::Address* a = NativeAdxOutput::addresses(count);
        std::set<u32> owned;
        for (std::size_t i = 0; i < count; ++i) owned.insert(a[i].address);
        check(listed == owned, "NativeAdxOutput::addresses() equals its section of native-owners.txt");
    } catch (const std::exception& error) {
        std::printf("ERROR: %s\n", error.what());
        return 1;
    }
    std::printf("native ADX output: %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
