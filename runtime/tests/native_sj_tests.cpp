// liveness-roots: reference-interpreter-only
// CRI's stream joints (gekko/native_sj.h) against the ORIGINAL words of the game's DOL, in the reference interpreter.
// Two machines run the same calls: one executes the original SJ functions (with the original SJCRS lock, OS interrupt
// calls, memcpy and SVM error report), the other serves them with NativeSj. The sequence: Init of both modules, a
// scripted part (every function, null / destroyed handles, illegal lanes, a zero-size ring, guest error callbacks via
// EntryErrFunc - which must run inside the SJCRS lock and before the original's post-callback updates - pool
// exhaustion, Finish) and 3,000 seeded random operations on two rings (with mirror areas) and a memory joint: get /
// put / unget on both lanes, chunk splits, payload writes, queries. After every call both machines must agree on the
// returned r3 (where the function returns a value), both pools with their use counts, the SJCRS words, every ring /
// memory buffer byte (mirrors included), the chunk structs and the callback trace. Also: NativeSj::addresses() equals
// the SJ section of extract/native-owners.txt.
//
// usage: native_sj_tests <sys/main.dol> <extract/native-owners.txt>

#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
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

// vtable slots
constexpr u32 kDestroy = 0x0C, kGetUuid = 0x10, kReset = 0x14, kGetChunk = 0x18, kUngetChunk = 0x1C,
              kPutChunk = 0x20, kGetNumData = 0x24, kIsGetChunk = 0x28, kEntryErrFunc = 0x2C;
constexpr u32 kMemVtable = 0x80569D40u, kRbfVtable = 0x80569D70u;
constexpr u32 kMemCreate = 0x803CA824u, kMemInit = 0x803CAA44u, kMemFinish = 0x803CA9F0u, kMemGetBufSize = 0x803C9EDCu;
constexpr u32 kRbfCreate = 0x803CB9BCu, kRbfInit = 0x803CBBE4u, kRbfFinish = 0x803CBB90u, kRbfGetCount = 0x803CAACCu,
              kRbfGetXtrSize = 0x803CABA0u, kRbfGetBufSize = 0x803CAC4Cu, kRbfGetBufPtr = 0x803CACF8u;
constexpr u32 kSplit = 0x803CBF34u;

constexpr u32 kR13 = 0x805EC500u, kR2 = 0x805FA780u;
constexpr u32 kFinishPc = 0x817FF000u, kStack = 0x817E0000u;
constexpr u32 kCode = 0x81700000u, kTrace = 0x81710000u;
constexpr u32 kBuffers = 0x81600000u, kBuffersSize = 0x2000u;
constexpr u32 kChunks = 0x81604000u, kChunksSize = 0x100u;  // 16 chunk slots + the count words
constexpr u32 kCount = kChunks + 0x80u;

// compared regions: SJCRS words + SJMEM count + pool, SJRBF count + pool, buffers, chunks, trace. Not compared:
// 0x805BBD7C, the interrupt level SJCRS_Lock saves at depth 0 - dead between calls (every outermost lock writes it
// before the unlock that reads it), and NativeSj takes no lock where no guest code runs (gekko/native_sj.h).
struct Region {
    u32 at, size;
};
constexpr Region kRegions[] = {
    {0x805BBD78u, 4u}, {0x805BBD80u, 8u}, {0x805BBD88u, 4u + 0x480u}, {0x805BC210u, 4u + 0x4000u},
    {kBuffers, kBuffersSize}, {kChunks, kChunksSize}, {kTrace, 0x400u},
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

u32 addi(u32 d, u32 a, s32 imm) { return (14u << 26) | (d << 21) | (a << 16) | (static_cast<u32>(imm) & 0xFFFFu); }
u32 addis(u32 d, u32 a, u32 imm) { return (15u << 26) | (d << 21) | (a << 16) | (imm & 0xFFFFu); }
u32 ori(u32 a, u32 s, u32 imm) { return (24u << 26) | (s << 21) | (a << 16) | (imm & 0xFFFFu); }
u32 lwz(u32 d, s32 off, u32 a) { return (32u << 26) | (d << 21) | (a << 16) | (static_cast<u32>(off) & 0xFFFFu); }
u32 stw(u32 s, s32 off, u32 a) { return (36u << 26) | (s << 21) | (a << 16) | (static_cast<u32>(off) & 0xFFFFu); }
u32 rlwinm(u32 a, u32 s, u32 sh, u32 mb, u32 me) {
    return (21u << 26) | (s << 21) | (a << 16) | (sh << 11) | (mb << 6) | (me << 1);
}
u32 add(u32 d, u32 a, u32 b) { return (31u << 26) | (d << 21) | (a << 16) | (b << 11) | (266u << 1); }
constexpr u32 kBlr = 0x4E800020u;

struct Machine {
    GuestMemory bus;
    Cpu cpu{bus};
    NativeSvm svm{cpu};
    NativeSj sj{cpu, svm};
    bool native;
    u32 errcb = 0;

    Machine(const std::vector<u8>& dol, bool native_sj) : native(native_sj) {
        const auto word = [&](std::size_t offset) { return load_be32(dol.data() + offset); };
        for (u32 i = 0; i < 18u; ++i) {
            const u32 offset = word(i * 4u), address = word(0x48u + i * 4u), size = word(0x90u + i * 4u);
            if (!size) continue;
            require(std::uint64_t(offset) + size <= dol.size(), "invalid DOL extent");
            bus.write_block(address, dol.data() + offset, size);
        }
        require(bus.read32(kMemVtable + kGetChunk) == 0x803CA334u && bus.read32(kRbfVtable + kGetChunk) == 0x803CB580u,
                "unexpected SJ vtable evidence");
        cpu.reset();
        cpu.gpr[2] = kR2;
        cpu.gpr[13] = kR13;
        cpu.set_msr(cpu.msr() | kMsrDR | kMsrIR | kMsrFP | kMsrEE);
        bus.write32(kTrace, 0u);
        if (native) sj.install();
        // A guest error callback: records r3 (its object), r4 (the code), the SJCRS lock depth (it must run inside
        // the lock) and the word at r3 + 0x28 (a ring's free-got counter: the original updates it AFTER the callback).
        std::vector<u32> w;
        const auto load = [&](u32 reg, u32 value) {
            w.push_back(addis(reg, 0, value >> 16));
            w.push_back(ori(reg, reg, value & 0xFFFFu));
        };
        const auto record = [&](u32 reg) {  // trace[count++] = reg (uses r7..r9)
            load(7, kTrace);
            w.push_back(lwz(8, 0, 7));
            w.push_back(addi(9, 8, 1));
            w.push_back(stw(9, 0, 7));
            w.push_back(rlwinm(8, 8, 2, 0, 29));
            w.push_back(add(8, 8, 7));
            w.push_back(stw(reg, 4, 8));
        };
        record(3);
        record(4);
        load(10, 0x805BBD78u);
        w.push_back(lwz(10, 0, 10));
        record(10);
        w.push_back(lwz(10, 0x28, 3));
        record(10);
        w.push_back(kBlr);
        for (std::size_t i = 0; i < w.size(); ++i) bus.write32(kCode + static_cast<u32>(i) * 4u, w[i]);
        errcb = kCode;
    }

    bool call(u32 entry, std::vector<u32> args, u32* result = nullptr) {
        u32 r = 3u;
        for (const u32 v : args) cpu.gpr[r++] = v;
        cpu.gpr[1] = kStack;
        cpu.lr = kFinishPc;
        cpu.pc = entry;
        u64 steps = 0;
        while (cpu.pc != kFinishPc && !cpu.stopped() && steps++ < 5'000'000ull) {
            decltype(&cpu.hle_entries().front()) hook = nullptr;
            if (native)
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
        return cpu.pc == kFinishPc && !cpu.stopped() && cpu.gpr[1] == kStack;
    }
    u32 method(u32 handle, u32 slot) { return handle != 0u ? bus.read32(bus.read32(handle) + slot) : 0u; }
};

std::vector<u8> read_file(const char* path) {
    std::ifstream input(path, std::ios::binary);
    return std::vector<u8>{std::istreambuf_iterator<char>(input), {}};
}

struct Pair {
    Machine& o;
    Machine& n;
    u64 steps = 0;

    // One call on both machines through an absolute entry, compared afterwards.
    u32 run(const std::string& what, u32 entry, std::vector<u32> args, bool returns) {
        u32 ro = 0, rn = 0;
        ++steps;
        check(o.call(entry, args, &ro), what + ": the original returned");
        check(n.call(entry, args, &rn), what + ": native SJ returned");
        if (returns) check(ro == rn, what + ": r3 " + hex(ro) + " vs " + hex(rn));
        for (const Region& g : kRegions)
            for (u32 a = g.at; a < g.at + g.size; a += 4u)
                if (o.bus.read32(a) != n.bus.read32(a)) {
                    check(false, what + ": word at " + hex(a) + " " + hex(o.bus.read32(a)) + " vs " + hex(n.bus.read32(a)));
                    break;
                }
        return ro;
    }
    // A virtual call: the entry comes from the handle's vtable (the original machine's; both pools agree).
    u32 virt(const std::string& what, u32 sj, u32 slot, std::vector<u32> args, bool returns) {
        args.insert(args.begin(), sj);
        return run(what, o.method(sj, slot), args, returns);
    }
    void poke(u32 at, u32 value) {
        o.bus.write32(at, value);
        n.bus.write32(at, value);
    }
};

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::printf("usage: native_sj_tests <sys/main.dol> <extract/native-owners.txt>\n");
        return 2;
    }
    try {
        const std::vector<u8> dol = read_file(argv[1]);
        require(dol.size() > 0x100u, "cannot read the DOL");
        Machine original(dol, false), native(dol, true);
        Pair p{original, native};
        const u32 errcb = original.errcb;
        const auto ck = [](u32 i) { return kChunks + 8u * i; };

        p.run("SJRBF_Init", kRbfInit, {}, false);
        p.run("SJMEM_Init", kMemInit, {}, false);
        p.run("SJRBF_Init again (count 2)", kRbfInit, {}, false);

        // ---- scripted --------------------------------------------------------------------------------------
        const u32 ring = p.run("SJRBF_Create 0x100 + 0x40", kRbfCreate, {kBuffers, 0x100u, 0x40u}, true);
        require(ring != 0u, "no ring");
        const u32 empty = p.run("SJRBF_Create size 0", kRbfCreate, {kBuffers + 0x400u, 0u, 0u}, true);
        const u32 mem = p.run("SJMEM_Create 0x180", kMemCreate, {kBuffers + 0x800u, 0x180u}, true);
        for (u32 i = 0; i < 0x180u; i += 4u) p.poke(kBuffers + 0x800u + i, 0xA5000000u | i);
        p.virt("ring GetUuid", ring, kGetUuid, {}, true);
        p.virt("mem GetUuid", mem, kGetUuid, {}, true);
        p.run("ring GetBufPtr", kRbfGetBufPtr, {ring}, true);
        p.run("ring GetBufSize", kRbfGetBufSize, {ring}, true);
        p.run("ring GetXtrSize", kRbfGetXtrSize, {ring}, true);
        p.run("mem GetBufSize", kMemGetBufSize, {mem}, true);
        for (u32 lane = 0; lane < 3u; ++lane) {
            p.virt("ring GetNumData lane " + std::to_string(lane), ring, kGetNumData, {lane}, true);
            p.virt("mem GetNumData lane " + std::to_string(lane), mem, kGetNumData, {lane}, true);
            p.virt("ring IsGetChunk lane " + std::to_string(lane), ring, kIsGetChunk, {lane, 0x30u, kCount}, true);
            p.virt("mem IsGetChunk lane " + std::to_string(lane), mem, kIsGetChunk, {lane, 0x30u, kCount}, true);
            for (u32 kind = 0; kind < 2u; ++kind)
                p.run("ring counter", kRbfGetCount, {ring, lane, kind}, true);
        }
        // null and zero-size handles
        p.run("GetBufPtr null", kRbfGetBufPtr, {0u}, true);
        p.run("mem GetBufSize null", kMemGetBufSize, {0u}, true);
        p.run("ring GetChunk null (via the vtable entry)", 0x803CB580u, {0u, 1u, 4u, ck(0)}, false);
        p.run("mem GetChunk null", 0x803CA334u, {0u, 1u, 4u, ck(0)}, false);
        p.virt("zero-size ring GetChunk", empty, kGetChunk, {0u, 0x10u, ck(0)}, false);
        p.virt("zero-size ring UngetChunk", empty, kUngetChunk, {0u, ck(0)}, false);
        // a producer cycle across the ring's end and through the mirror
        for (u32 round = 0; round < 6u; ++round) {
            const std::string r = " #" + std::to_string(round);
            p.virt("ring get free" + r, ring, kGetChunk, {0u, 0x70u, ck(0)}, false);
            const u32 data = original.bus.read32(ck(0)), len = original.bus.read32(ck(0) + 4u);
            for (u32 i = 0; i < len; i += 4u) p.poke(data + i, (round << 24) | i);
            p.virt("ring put data" + r, ring, kPutChunk, {1u, ck(0)}, false);
            p.virt("ring get data" + r, ring, kGetChunk, {1u, 0x60u, ck(1)}, false);
            p.run("split" + r, kSplit, {ck(1), 0x20u, ck(2), ck(3)}, false);
            p.virt("ring unget rest" + r, ring, kUngetChunk, {1u, ck(3)}, false);
            p.virt("ring put free" + r, ring, kPutChunk, {0u, ck(2)}, false);
        }
        // errors through the default callback (SJRBF_Error -> the SVM report)
        p.virt("ring unget a stale chunk (mismatch)", ring, kUngetChunk, {1u, ck(2)}, false);
        p.virt("ring get lane 2", ring, kGetChunk, {2u, 0x10u, ck(4)}, false);
        p.virt("mem get lane 2", mem, kGetChunk, {2u, 0x10u, ck(4)}, false);
        p.virt("mem unget lane 0", mem, kUngetChunk, {0u, ck(1)}, false);
        // guest error callbacks
        p.virt("ring EntryErrFunc (guest)", ring, kEntryErrFunc, {errcb, ring}, false);
        p.virt("mem EntryErrFunc (guest)", mem, kEntryErrFunc, {errcb, mem}, false);
        p.virt("ring unget stale with a guest callback", ring, kUngetChunk, {0u, ck(2)}, false);
        p.virt("ring IsGetChunk lane 5 with a guest callback", ring, kIsGetChunk, {5u, 0x10u, kCount}, true);
        p.virt("ring GetNumData lane 7 with a guest callback", ring, kGetNumData, {7u}, true);
        p.virt("ring put lane 3 with a guest callback", ring, kPutChunk, {3u, ck(1)}, false);
        p.virt("mem get lane 1", mem, kGetChunk, {1u, 0x50u, ck(5)}, false);
        p.virt("mem unget lane 1, matching", mem, kUngetChunk, {1u, ck(5)}, false);
        p.virt("mem get lane 1 again", mem, kGetChunk, {1u, 0x50u, ck(5)}, false);
        p.virt("mem get lane 1 more", mem, kGetChunk, {1u, 0x30u, ck(6)}, false);
        p.virt("mem unget the older chunk (mismatch, guest callback)", mem, kUngetChunk, {1u, ck(5)}, false);
        p.virt("mem IsGetChunk lane 4 (guest callback)", mem, kIsGetChunk, {4u, 0x10u, kCount}, true);
        p.virt("mem put lane 1 (nothing)", mem, kPutChunk, {1u, ck(6)}, false);
        p.virt("mem put lane 9 (guest callback)", mem, kPutChunk, {9u, ck(6)}, false);
        p.virt("mem Reset", mem, kReset, {}, false);
        p.virt("ring Reset", ring, kReset, {}, false);
        // signed remainders (divw), chosen so an unsigned remainder would accept the unget: a chunk starting 0x10
        // below the ring (data lane at 0: (0 + 0x100 - 0x10) % 0x100 = 0xF0 vs -0x10), and a free chunk longer than
        // the ring ((0 + 0x100 - 0x7FFFFF10) % 0x100 = -0x10 vs 0xF0, its offset 0xF0)
        p.poke(ck(7), kBuffers - 0x10u);
        p.poke(ck(7) + 4u, 0x10u);
        p.virt("ring unget a chunk below the buffer", ring, kUngetChunk, {1u, ck(7)}, false);
        p.poke(ck(7), kBuffers + 0xF0u);
        p.poke(ck(7) + 4u, 0x7FFFFF10u);
        p.virt("ring unget a chunk longer than the ring", ring, kUngetChunk, {0u, ck(7)}, false);
        p.virt("ring Reset again", ring, kReset, {}, false);
        // pools: SJMEM has 32 joints, SJRBF 256
        for (u32 i = 0; i < 33u; ++i)
            p.run("SJMEM_Create #" + std::to_string(i), kMemCreate, {kBuffers + 0x1000u, 0x40u + i}, true);
        for (u32 i = 0; i < 257u; ++i)
            p.run("SJRBF_Create #" + std::to_string(i), kRbfCreate, {kBuffers + 0x1000u, 0x40u, i}, true);
        for (u32 i = 0; i < 40u; ++i) {
            const u32 h = 0x805BC214u + 0x40u * (i * 6u % 256u);
            p.virt("SJRBF Destroy", h, kDestroy, {}, false);
        }
        p.virt("mem Destroy", mem, kDestroy, {}, false);
        p.run("mem GetUuid destroyed (its vtable word is 0 now: direct)", 0x803CA6C4u, {mem}, true);
        p.run("SJRBF_Destroy on a destroyed memory joint", 0x803CB908u, {mem}, false);

        // ---- random ----------------------------------------------------------------------------------------
        const u32 r1 = p.run("random ring 1", kRbfCreate, {kBuffers + 0x1400u, 0x200u, 0x80u}, true);
        const u32 r2 = p.run("random ring 2", kRbfCreate, {kBuffers + 0x1700u, 0xC0u, 0x0u}, true);
        const u32 m1 = p.run("random mem", kMemCreate, {kBuffers + 0x1900u, 0x300u}, true);
        require(r1 != 0u && r2 != 0u && m1 != 0u, "random joints");
        u32 seed = 0x5EEDu;
        const auto next = [&seed](u32 n) {
            seed = seed * 1103515245u + 12345u;
            return (seed >> 16) % n;
        };
        const u32 joints[3] = {r1, r2, m1};
        for (u32 i = 0; i < 3000u; ++i) {
            const u32 j = next(3u), h = joints[j], lane = next(10u) == 0u ? 2u + next(3u) : next(2u);
            const u32 slot = j * 4u + next(4u);  // each joint its own chunk slots
            const std::string what = "random " + std::to_string(i);
            switch (next(9u)) {
                case 0:
                case 1:
                    p.virt(what + " get", h, kGetChunk, {lane, 1u + next(0x180u), ck(slot)}, false);
                    break;
                case 2:
                case 3:
                    p.virt(what + " put", h, kPutChunk, {lane, ck(slot)}, false);
                    break;
                case 4:
                    p.virt(what + " unget", h, kUngetChunk, {lane, ck(slot)}, false);
                    break;
                case 5:
                    p.run(what + " split", kSplit, {ck(slot), next(0x100u), ck(j * 4u + next(4u)), ck(j * 4u + next(4u))},
                          false);
                    break;
                case 6: {  // payload into a chunk that points into this joint's buffer
                    const u32 data = original.bus.read32(ck(slot)), len = original.bus.read32(ck(slot) + 4u);
                    if (data >= kBuffers && data + len <= kBuffers + kBuffersSize && len < 0x400u)
                        for (u32 k = 0; k + 4u <= len; k += 4u) p.poke(data + k, (i << 12) ^ k);
                    break;
                }
                case 7:
                    p.virt(what + " numdata", h, kGetNumData, {lane}, true);
                    break;
                default:
                    p.virt(what + " isget", h, kIsGetChunk, {lane, next(0x200u), kCount}, true);
                    break;
            }
        }
        p.run("SJMEM_Finish", kMemFinish, {}, false);
        p.run("SJRBF_Finish (count 1)", kRbfFinish, {}, false);
        p.run("SJRBF_Finish (count 0: clears)", kRbfFinish, {}, false);
        check(native.sj.stats().guest_callbacks >= 6u, "guest callbacks ran natively (" +
                                                           std::to_string(native.sj.stats().guest_callbacks) + ")");
        std::printf("calls %llu, native: gets %llu puts %llu ungets %llu splits %llu errors %llu guest callbacks %llu, "
                    "trace words %u\n",
                    static_cast<unsigned long long>(p.steps), static_cast<unsigned long long>(native.sj.stats().gets),
                    static_cast<unsigned long long>(native.sj.stats().puts),
                    static_cast<unsigned long long>(native.sj.stats().ungets),
                    static_cast<unsigned long long>(native.sj.stats().splits),
                    static_cast<unsigned long long>(native.sj.stats().errors),
                    static_cast<unsigned long long>(native.sj.stats().guest_callbacks), native.bus.read32(kTrace));

        // The owners list.
        std::set<u32> listed;
        {
            std::ifstream owners(argv[2]);
            std::string line;
            bool in_section = false;
            while (std::getline(owners, line)) {
                if (!line.empty() && line[0] == '#' && line.find("runtime/src/gekko/") != std::string::npos)
                    in_section = line.find("runtime/src/gekko/native_sj.cpp") != std::string::npos;
                if (!in_section || line.empty() || line[0] == '#') continue;
                listed.insert(static_cast<u32>(std::stoul(line.substr(0, 10), nullptr, 16)));
            }
        }
        std::size_t count = 0;
        const NativeSj::Address* a = NativeSj::addresses(count);
        std::set<u32> owned;
        for (std::size_t i = 0; i < count; ++i) owned.insert(a[i].address);
        check(listed == owned, "NativeSj::addresses() equals the SJ section of native-owners.txt (" +
                                   std::to_string(listed.size()) + " listed, " + std::to_string(owned.size()) + " owned)");
    } catch (const std::exception& error) {
        std::printf("ERROR: %s\n", error.what());
        return 1;
    }
    std::printf("native SJ: %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
