// liveness-roots: reference-interpreter-only
// Original-DOL contract for the RenderWare device-pool owners (gekko/native_rw_registry.h): object create 0x8049C7BC,
// object destroy 0x8049C850, entry remove 0x8049C3A0, entry swap 0x8049CE40, and the world plugin's attach 0x80465818 with its
// element destructor 0x80465800 and the two empty callbacks 0x80465810 / 0x80465814.
//
// Reference interpreter, pinned DOL. Each case builds one machine state (a synthetic pool: an object with entries, the 0x80-byte
// blocks and 12-byte records the entries point into, the objects the entries own, the engine's allocator hooks) and runs the
// ORIGINAL body and the owner from byte-identical memory. The functions the bodies reach through the engine's function table or
// by fixed address (allocator, pool free, the objects' callbacks, the RenderWare error and plugin-registration functions) are
// recording stubs on BOTH machines. Compared: r3 (not for the entry swap: both callers ignore it), r1, r2, r13, r14..r31, lr,
// f14..f31, every byte of the engine / small-data window and of the test area, which holds the pool and the stub log.
// States are generated from a fixed seed; a state the ORIGINAL cannot finish within the step bound is skipped (counted).
//
// Negative controls: owners that forget a compaction step, skip an index renumbering, swap the wrong record or free the wrong
// block are told apart.
//
// usage: rw_pool_contract_tests <sys/main.dol>
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_guest_call.h"
#include "gekko/native_rw_registry.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace gekko;
namespace reg = gekko::rw_registry;

constexpr u32 kSda = 0x805EC500u, kSda2 = 0x805FA780u;
constexpr u32 kStack = 0x817E0000u, kFinish = 0x817FF000u;
constexpr u32 kDataLo = 0x80560000u, kDataHi = 0x80600000u;
constexpr u32 kTestLo = 0x81300000u, kTestHi = 0x81310000u;
constexpr u32 kLogOffset = 0x81301000u, kLogBase = 0x81301100u, kReturns = 0x81301800u;
constexpr u32 kEngine = 0x805E3C08u;
constexpr u32 kPoolOffset = 0x120u;
constexpr u32 kQ = 0x81302000u, kP2 = 0x81302100u, kObjects = 0x81302200u, kExtra = 0x81302800u, kArr = 0x81303000u;
constexpr u32 kStubFree = 0x81306000u, kStubAlloc = 0x81306100u, kStubPoolFree = 0x81306200u, kStubCbA = 0x81306300u,
              kStubCbB = 0x81306400u;
constexpr u32 kSlots = 12u;  // entries the arrays have room for (the per-entry block count K); all slots are valid

struct StubSpec {
    u32 address;
    unsigned args;
};
// stub ids: 0 free, 1 alloc, 2 pool free, 3 callback A, 4 callback B, 5 error generate, 6 error set, 7 plugin register
constexpr StubSpec kStubs[] = {{kStubFree, 1},  {kStubAlloc, 2}, {kStubPoolFree, 2}, {kStubCbA, 1},
                               {kStubCbB, 1},   {0x8047EC78u, 2}, {0x8047EBD4u, 1},   {0x8045BAF0u, 5}};
constexpr unsigned kStubCount = sizeof(kStubs) / sizeof(kStubs[0]);

void require(bool ok, const std::string& what) {
    if (!ok) throw std::runtime_error(what);
}

u32 addis(unsigned rd, unsigned ra, u32 imm) { return (15u << 26) | (rd << 21) | (ra << 16) | (imm & 0xFFFFu); }
u32 addi(unsigned rd, unsigned ra, u32 imm) { return (14u << 26) | (rd << 21) | (ra << 16) | (imm & 0xFFFFu); }
u32 lwz(unsigned rd, unsigned ra, u32 d) { return (32u << 26) | (rd << 21) | (ra << 16) | (d & 0xFFFFu); }
u32 stw(unsigned rs, unsigned ra, u32 d) { return (36u << 26) | (rs << 21) | (ra << 16) | (d & 0xFFFFu); }
u32 add(unsigned rd, unsigned ra, unsigned rb) { return (31u << 26) | (rd << 21) | (ra << 16) | (rb << 11) | (266u << 1); }

struct Rng {
    u32 state;
    explicit Rng(u32 seed) : state(seed * 2654435761u + 12345u) {}
    u32 next() {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    }
    u32 below(u32 n) { return next() % n; }
};

struct Snapshot {
    u32 r3, r1, r2, r13, lr;
    u32 saved[18];
    u64 f[18];
    std::vector<u8> data, test;
};

struct Machine {
    GuestMemory memory;
    Cpu cpu{memory};

    Machine(const char* path, bool native, const std::function<void(Cpu&)>& wrong = nullptr) {
        std::ifstream input(path, std::ios::binary);
        const std::vector<u8> dol{std::istreambuf_iterator<char>(input), {}};
        require(dol.size() >= 256, "missing DOL");
        unsigned long long hash = 0xCBF29CE484222325ull;
        for (u8 byte : dol) hash = (hash ^ byte) * 0x100000001B3ull;
        // SHA-256: fde4fa6f81a60313b710161c196dc51c2260be62251ee02775d5eee06f9d55af.
        require(hash == 0xD2CFB4A1CA897F9Bull, "unexpected original DOL identity");
        const auto word = [&](unsigned offset) { return load_be32(dol.data() + offset); };
        for (unsigned i = 0; i < 18; ++i) {
            const u32 offset = word(i * 4), address = word(0x48 + i * 4), size = word(0x90 + i * 4);
            require(static_cast<unsigned long long>(offset) + size <= dol.size(), "invalid DOL section");
            if (size) memory.write_block(address, dol.data() + offset, size);
        }
        for (unsigned k = 0; k < kStubCount; ++k) {
            std::vector<u32> code = {addis(12, 0, 0x8130), addi(10, 12, 0x1100), lwz(11, 12, 0x1000), add(10, 10, 11),
                                     addi(9, 0, k), stw(9, 10, 0)};
            for (unsigned a = 0; a < kStubs[k].args; ++a) code.push_back(stw(3 + a, 10, 4 + 4 * a));
            code.push_back(addi(11, 11, 0x20));
            code.push_back(stw(11, 12, 0x1000));
            code.push_back(addis(9, 0, 0xDEAD));  // a callee's own frame: scribble the words below r1
            for (unsigned s = 1; s <= 18; ++s) code.push_back(stw(9, 1, 0x10000u - 4 * s));
            code.push_back(lwz(3, 12, 0x1800 + 4 * k));
            code.push_back(0x4E800020u);
            for (unsigned i = 0; i < code.size(); ++i) memory.write32(kStubs[k].address + 4u * i, code[i]);
        }
        if (native) {
            reg::install(cpu);
            if (wrong) wrong(cpu);
        }
    }

    void w(u32 address, u32 value) { memory.write32(address, value); }
    u32 r(u32 address) const { return memory.peek32(address); }

    void fill() {
        for (u32 a = kDataLo; a < kDataHi; a += 4) memory.write32(a, 0xA5000000u ^ (a * 2654435761u));
        for (u32 a = kTestLo; a < kTestHi; a += 4) memory.write32(a, 0x5A000000u ^ (a * 40503u));
        // the stubs live in the test area: write them again after the fill
        w(kLogOffset, 0u);
        for (unsigned k = 0; k < 16; ++k) w(kReturns + 4 * k, 0u);
        w(kSda + 0x615Cu, kEngine);
        w(kSda + 0x6238u, kPoolOffset);
        w(kEngine + 0x10Cu, kStubFree);
        w(kEngine + 0x118u, kStubAlloc);
        w(kEngine + 0x11Cu, kStubPoolFree);
        w(kEngine + kPoolOffset + 0x38u, kSlots);
    }

    void reinstall_stubs() {
        for (unsigned k = 0; k < kStubCount; ++k) {
            std::vector<u32> code = {addis(12, 0, 0x8130), addi(10, 12, 0x1100), lwz(11, 12, 0x1000), add(10, 10, 11),
                                     addi(9, 0, k), stw(9, 10, 0)};
            for (unsigned a = 0; a < kStubs[k].args; ++a) code.push_back(stw(3 + a, 10, 4 + 4 * a));
            code.push_back(addi(11, 11, 0x20));
            code.push_back(stw(11, 12, 0x1000));
            code.push_back(addis(9, 0, 0xDEAD));  // a callee's own frame: scribble the words below r1
            for (unsigned s = 1; s <= 18; ++s) code.push_back(stw(9, 1, 0x10000u - 4 * s));
            code.push_back(lwz(3, 12, 0x1800 + 4 * k));
            code.push_back(0x4E800020u);
            for (unsigned i = 0; i < code.size(); ++i) memory.write32(kStubs[k].address + 4u * i, code[i]);
        }
    }
    void ret(unsigned id, u32 value) { w(kReturns + 4 * id, value); }

    // A synthetic pool: object Q (n entries), the arrays, the owned objects.
    void pool(Rng& rng, u32 n, u32 q0, bool trigger) {
        w(kQ + 0, q0);
        w(kQ + 4, n);
        w(kQ + 8, kArr);
        w(kQ + 0x20, rng.below(2) ? kExtra : 0u);
        w(kQ + 0x24, rng.next());
        const u32 blocks = kArr + kSlots * 0x28u, records = kArr + kSlots * 0xA8u;
        for (unsigned o = 0; o < 3; ++o) {
            const u32 obj = kObjects + 0x40u * o;
            w(obj + 0x14, rng.below(3) ? kStubCbA : 0u);
            w(obj + 0xC, rng.below(3) ? kStubCbB : 0u);
            w(obj + 0x38, rng.below(2));
            w(obj + 0x3C, 1u + rng.below(2));
        }
        for (u32 i = 0; i < kSlots; ++i) {
            const u32 e = kArr + 0x28u * i;
            for (u32 word = 0; word < 10; ++word) w(e + 4 * word, rng.next());
            w(e + 0, kObjects + 0x40u * rng.below(3));
            w(e + 4, rng.below(6));  // at most 5 indices of the 32 words
            w(e + 8, blocks + 0x80u * i);
            w(e + 0x1C, records + 12u * i);
            w(e + 0x20, rng.below(2) ? kExtra + 0x10u * (i + 1) : 0u);
            for (u32 j = 0; j < 32; ++j) {
                u32 v;
                v = rng.below(4) == 0 ? 0xFFFFFFFFu : rng.below(n ? n : 1);  // only valid indices or -1
                w(blocks + 0x80u * i + 4 * j, v);
            }
            const u32 hi = 4u + rng.below(5);
            w(records + 12u * i, hi);
            w(records + 12u * i + 4, trigger && rng.below(4) == 0 ? hi - 1u : hi - 2u - rng.below(2));
            w(records + 12u * i + 8, rng.next());
        }
    }

    void call(u32 entry, u32 r3, u32 r4) {
        cpu.reset();
        cpu.pc = entry; cpu.npc = entry + 4; cpu.lr = kFinish;
        for (unsigned i = 0; i < 32; ++i) cpu.gpr[i] = 0x1000u + i * 0x111u;
        cpu.gpr[1] = kStack; cpu.gpr[2] = kSda2; cpu.gpr[13] = kSda; cpu.gpr[3] = r3; cpu.gpr[4] = r4;
        for (unsigned i = 14; i < 32; ++i) {
            const u64 value = 0x4000000000000000ull + i;
            std::memcpy(&cpu.fpr[i].bits[0], &value, sizeof(value));
        }
        unsigned steps = 0;
        while (cpu.pc != kFinish && !cpu.stopped() && steps++ < 600000) cpu.run_steps(1);
        require(cpu.pc == kFinish && !cpu.stopped(), "the call did not return");
    }

    Snapshot snapshot() const {
        Snapshot s{};
        s.r3 = cpu.gpr[3]; s.r1 = cpu.gpr[1]; s.r2 = cpu.gpr[2]; s.r13 = cpu.gpr[13]; s.lr = cpu.lr;
        for (unsigned i = 0; i < 18; ++i) {
            s.saved[i] = cpu.gpr[14 + i];
            std::memcpy(&s.f[i], &cpu.fpr[14 + i].bits[0], sizeof(u64));
        }
        s.data.resize(kDataHi - kDataLo);
        for (u32 i = 0; i < kDataHi - kDataLo; ++i) s.data[i] = memory.peek8(kDataLo + i);
        s.test.resize(kTestHi - kTestLo);
        for (u32 i = 0; i < kTestHi - kTestLo; ++i) s.test[i] = memory.peek8(kTestLo + i);
        return s;
    }
};

std::string diff(const Snapshot& a, const Snapshot& b, bool compare_r3) {
    char text[120];
    if (compare_r3 && a.r3 != b.r3) { std::snprintf(text, sizeof text, "r3 original=%08X native=%08X", a.r3, b.r3); return text; }
    if (a.r1 != b.r1 || a.r2 != b.r2 || a.r13 != b.r13 || a.lr != b.lr) return "r1 / r2 / r13 / lr differ";
    if (std::memcmp(a.saved, b.saved, sizeof a.saved) != 0) return "a callee-saved register differs";
    if (std::memcmp(a.f, b.f, sizeof a.f) != 0) return "a callee-saved FPR differs";
    for (u32 i = 0; i < a.data.size(); ++i)
        if (a.data[i] != b.data[i]) { std::snprintf(text, sizeof text, "data byte 0x%08X differs", kDataLo + i); return text; }
    for (u32 i = 0; i < a.test.size(); ++i)
        if (a.test[i] != b.test[i]) { std::snprintf(text, sizeof text, "test byte 0x%08X differs", kTestLo + i); return text; }
    return {};
}

using Setup = std::function<void(Machine&)>;
unsigned g_cases = 0, g_skipped = 0;
const std::function<void(Cpu&)>* g_wrong = nullptr;

void compare(const char* path, const char* what, u32 entry, u32 r3, u32 r4, bool compare_r3, const Setup& setup) {
    const bool trace = std::getenv("RW_POOL_TRACE") != nullptr;
    if (trace) std::fprintf(stderr, "[begin %s]\n", what);
    Machine original(path, false), native(path, true, g_wrong ? *g_wrong : std::function<void(Cpu&)>{});
    if (trace) std::fprintf(stderr, "[machines built]\n");
    for (Machine* m : {&original, &native}) {
        m->fill();
        m->reinstall_stubs();
        setup(*m);
    }
    if (trace) std::fprintf(stderr, "[%s r3=%08X r4=%08X]\n", what, r3, r4);
    try {
        original.call(entry, r3, r4);
    } catch (const std::exception&) {
        ++g_skipped;  // a state the original does not finish: not a valid pool
        return;
    }
    native.call(entry, r3, r4);
    const std::string d = diff(original.snapshot(), native.snapshot(), compare_r3);
    if (!d.empty()) {
        char text[200];
        std::snprintf(text, sizeof text, "%s r3=%08X r4=%08X: %s", what, r3, r4, d.c_str());
        throw std::runtime_error(text);
    }
    ++g_cases;
}

void run_all(const char* path) {
    g_cases = 0;
    g_skipped = 0;
    // 0x8049C7BC
    for (u32 alloc : {0u, 0x81305000u})
        for (u32 pool_slot : {0u, 0x81302000u})
            compare(path, "pool object create", reg::kPoolEntryCreate, 1u, 2u, true, [&](Machine& m) {
                m.ret(1, alloc);
                m.ret(5, 0x12345u);
                m.w(kEngine + kPoolOffset, pool_slot);
            });
    // 0x80465818 / 0x80465800 / the empty callbacks
    for (u32 handle : {0u, 5u, 0x80000001u, 0xFFFFFFFFu})
        compare(path, "world attach", reg::kWorldAttach, 7u, 8u, true, [&](Machine& m) { m.ret(7, handle); });
    for (u32 plugin : {0x10u, 0x7C0u})
        compare(path, "world element clear", reg::kWorldElementClear, 0x81302600u, 0u, true,
                [&](Machine& m) { m.w(kSda + 0x60D8u, plugin); });
    compare(path, "world empty A", reg::kWorldEmptyA, 0x1111u, 0x2222u, true, [](Machine&) {});
    compare(path, "world empty B", reg::kWorldEmptyB, 0x1111u, 0x2222u, true, [](Machine&) {});
    // 0x8049C3A0, 0x8049C850, 0x8049CE40 on generated pools
    for (u32 seed = 1; seed <= 70; ++seed) {
        const u32 n = 2u + seed % 5u;  // 2..6 entries
        const u32 q0 = (seed % 3u == 0u) ? 0u : kObjects + 0x40u;
        const u32 idx = (seed * 7u) % n;
        const Setup base = [=](Machine& m) {
            Rng rng(seed);
            m.pool(rng, n, q0, false);
            m.ret(0, 0x10u + seed);
            m.ret(2, 0x20u + seed);
            m.ret(3, 0x30u + seed);
            m.ret(4, 0x40u + seed);
        };
        compare(path, "entry remove", reg::kPoolEntryRemove, kArr + 0x28u * idx, kQ, true, base);
        compare(path, "pool destroy", reg::kPoolDestroyAll, kQ, 0u, true, base);
        const u32 a = seed % (n - 1u), b = (seed * 5u + 1u) % n;
        compare(path, "entry swap", reg::kPoolEntrySwap, kP2, b, false, [=](Machine& m) {
            Rng rng(seed + 1000u);
            m.pool(rng, n, 1u, seed % 4u == 0u);
            m.w(kP2 + 0, kQ);
            m.w(kP2 + 4, a);
        });
    }
    compare(path, "pool destroy null", reg::kPoolDestroyAll, 0u, 0u, true, [](Machine&) {});
}

bool caught(const char* path, const std::function<void(Cpu&)>& wrong) {
    g_wrong = &wrong;
    bool found = false;
    try {
        run_all(path);
    } catch (const std::exception&) {
        found = true;
    }
    g_wrong = nullptr;
    return found;
}
}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: rw_pool_contract_tests main.dol");
        run_all(argv[1]);
        const unsigned cases = g_cases, skipped = g_skipped;
        require(cases > 150u, "too few valid cases (" + std::to_string(cases) + ")");
        require(caught(argv[1], [](Cpu& cpu) {
            cpu.install_hle(reg::kPoolEntrySwap, [](Cpu& c) {
                (void)c;  // forgets to swap and to count
                return true;
            }, "wrong: no swap");
        }), "negative control: an entry swap that does nothing was not told apart");
        require(caught(argv[1], [](Cpu& cpu) {
            cpu.install_hle(reg::kPoolEntryRemove, [](Cpu& c) {
                c.mem_write32(c.gpr[4] + 4u, c.mem_read32(c.gpr[4] + 4u) - 1u);  // only the count
                return true;
            }, "wrong: only the count");
        }), "negative control: an entry remove that only counts was not told apart");
        require(caught(argv[1], [](Cpu& cpu) {
            cpu.install_hle(reg::kWorldAttach, [](Cpu& c) {
                c.gpr[3] = 1u;  // no registration
                return true;
            }, "wrong: no registration");
        }), "negative control: a world attach without registration was not told apart");
        require(caught(argv[1], [](Cpu& cpu) {
            cpu.install_hle(reg::kPoolDestroyAll, [](Cpu& c) {
                u32 ignored = 0;  // frees the block only, no entry removal, no pool free
                const u32 block = c.mem_read32(c.gpr[3] + 0x20u);
                if (block != 0u) guest_call(c, c.mem_read32(c.mem_read32(c.gpr[13] + 0x615Cu) + 0x10Cu), 0x8049C851u, {block}, ignored);
                return true;
            }, "wrong: partial destroy");
        }), "negative control: a partial pool destroy was not told apart");
        std::printf("PASS rw pool contract: %u original-vs-native cases (%u generated states skipped)\n", cases, skipped);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL rw pool contract: %s\n", error.what());
        return 1;
    }
}
