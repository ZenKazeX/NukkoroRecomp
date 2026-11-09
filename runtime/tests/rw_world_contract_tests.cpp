// liveness-roots: reference-interpreter-only
// Original-DOL contract for the owned world-sector reader (gekko/native_rw_registry.h): 0x804662B0 with the body it calls,
// 0x80465AE4 (reads one world-sector chunk: stream chunk lookup, three 4-byte reads, the engine allocation, two block reads,
// the pointer relocation, the object header).
//
// Reference interpreter, pinned DOL. Each case builds one machine state and runs the ORIGINAL 0x804662B0 and the owner from
// byte-identical memory. The RenderWare stream functions (0x8047DFA0 chunk lookup, 0x8047E8B8 4-byte read, 0x80481D9C block
// read), the engine allocator and the RenderWare error functions are recording stubs on BOTH machines: the chunk and read
// stubs write scripted values through the pointers the body hands them (so the body sees a chunk of the chosen size, version
// and counts), the block-read stub returns the requested length plus a scripted delta. DCFlushRange and GXInvalidateVtxCache,
// which the original also calls, are the production silent owners (no cache, no vertex cache) on the original's machine and
// are dropped by the owner. Compared: r3, r1, r2, r13, r14..r31, lr, f14..f31, every byte of the engine / small-data window
// and the test area (the object, the stub log). The stack is outside both windows.
//
// Negative controls: owners that relocate with a wrong base, skip the version check, forget the header fill or read in a
// different order are told apart.
//
// usage: rw_world_contract_tests <sys/main.dol>
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_gx_state.h"
#include "gekko/native_guest_call.h"
#include "gekko/native_rw_registry.h"

#include <cstdio>
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
constexpr u32 kLogOffset = 0x81301000u;
constexpr u32 kEngine = 0x805E3C08u;
constexpr u32 kWorld = 0x81302000u, kExtent = 0x81302100u, kBuffer = 0x81304000u, kStubAlloc = 0x81306000u;
// scripted stub data (test area, 0x8130xxxx via r12 = 0x81300000 + offset)
constexpr u32 kFindSize = 0x81301900u, kFindVersion = 0x81301904u, kReadCounter = 0x81301908u, kBytesCounter = 0x8130190Cu;
constexpr u32 kReadValues = 0x81301910u, kReadRets = 0x81301950u, kBytesDeltas = 0x81301940u, kFixedRets = 0x81301800u;

void require(bool ok, const std::string& what) {
    if (!ok) throw std::runtime_error(what);
}

u32 addis(unsigned rd, unsigned ra, u32 imm) { return (15u << 26) | (rd << 21) | (ra << 16) | (imm & 0xFFFFu); }
u32 addi(unsigned rd, unsigned ra, u32 imm) { return (14u << 26) | (rd << 21) | (ra << 16) | (imm & 0xFFFFu); }
u32 lwz(unsigned rd, unsigned ra, u32 d) { return (32u << 26) | (rd << 21) | (ra << 16) | (d & 0xFFFFu); }
u32 stw(unsigned rs, unsigned ra, u32 d) { return (36u << 26) | (rs << 21) | (ra << 16) | (d & 0xFFFFu); }
u32 add(unsigned rd, unsigned ra, unsigned rb) { return (31u << 26) | (rd << 21) | (ra << 16) | (rb << 11) | (266u << 1); }
u32 lwzx(unsigned rd, unsigned ra, unsigned rb) { return (31u << 26) | (rd << 21) | (ra << 16) | (rb << 11) | (23u << 1); }

struct Rng {
    u32 state;
    explicit Rng(u32 seed) : state(seed * 2654435761u + 12345u) {}
    u32 next() {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    }
};

struct Snapshot {
    u32 r3, r1, r2, r13, lr;
    u32 saved[18];
    u64 f[18];
    std::vector<u8> data, test;
};

// stub ids: 0 find chunk, 1 stream read (4 bytes), 2 block read, 3 engine alloc, 4 error generate, 5 error set
struct Stub {
    u32 address;
    unsigned args;
};
constexpr Stub kStubs[] = {{0x8047DFA0u, 4}, {0x8047E8B8u, 3}, {0x80481D9Cu, 3}, {kStubAlloc, 2}, {0x8047EC78u, 1}, {0x8047EBD4u, 1}};
constexpr unsigned kStubCount = sizeof(kStubs) / sizeof(kStubs[0]);

std::vector<u32> stub_code(unsigned k) {
    std::vector<u32> code = {addis(12, 0, 0x8130), addi(10, 12, 0x1100), lwz(11, 12, 0x1000), add(10, 10, 11),
                             addi(9, 0, k), stw(9, 10, 0)};
    for (unsigned a = 0; a < kStubs[k].args; ++a) code.push_back(stw(3 + a, 10, 4 + 4 * a));
    code.push_back(addi(11, 11, 0x20));
    code.push_back(stw(11, 12, 0x1000));
    code.push_back(addis(9, 0, 0xDEAD));  // a callee's own frame: scribble the words below r1
    for (unsigned k = 1; k <= 18; ++k) code.push_back(stw(9, 1, 0x10000u - 4 * k));
    switch (k) {
        case 0:  // chunk lookup: *r5 = size, *r6 = version, r3 = fixed return
            code.push_back(lwz(9, 12, 0x1900)); code.push_back(stw(9, 5, 0));
            code.push_back(lwz(9, 12, 0x1904)); code.push_back(stw(9, 6, 0));
            code.push_back(lwz(3, 12, 0x1800));
            break;
        case 1:  // 4-byte read: *r4 = next scripted value, r3 = next scripted return
            code.push_back(lwz(9, 12, 0x1908));
            code.push_back(addi(8, 12, 0x1910)); code.push_back(lwzx(7, 8, 9)); code.push_back(stw(7, 4, 0));
            code.push_back(addi(8, 12, 0x1950)); code.push_back(lwzx(3, 8, 9));
            code.push_back(addi(9, 9, 4)); code.push_back(stw(9, 12, 0x1908));
            break;
        case 2:  // block read: r3 = r5 + next scripted delta
            code.push_back(lwz(9, 12, 0x190C));
            code.push_back(addi(8, 12, 0x1940)); code.push_back(lwzx(7, 8, 9)); code.push_back(add(3, 5, 7));
            code.push_back(addi(9, 9, 4)); code.push_back(stw(9, 12, 0x190C));
            break;
        default:
            code.push_back(lwz(3, 12, 0x1800 + 4 * k));
            break;
    }
    code.push_back(0x4E800020u);
    return code;
}

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
        gx_state::install(cpu);  // GXInvalidateVtxCache; DCFlushRange is served by the production cache owner (not installed
                                 // here: the original's dcbf loop only touches the cache model, which writes nothing)
        if (native) {
            reg::install(cpu);
            if (wrong) wrong(cpu);
        }
    }

    void w(u32 address, u32 value) { memory.write32(address, value); }

    void fill() {
        for (u32 a = kDataLo; a < kDataHi; a += 4) memory.write32(a, 0xA5000000u ^ (a * 2654435761u));
        for (u32 a = kTestLo; a < kTestHi; a += 4) memory.write32(a, 0x5A000000u ^ (a * 40503u));
        for (unsigned k = 0; k < kStubCount; ++k) {
            const std::vector<u32> code = stub_code(k);
            for (unsigned i = 0; i < code.size(); ++i) memory.write32(kStubs[k].address + 4u * i, code[i]);
        }
        // DCFlushRange (dcbf loop and a system call): the production owner returns (no cache), so both machines return
        w(0x80372534u, 0x4E800020u);
        w(kLogOffset, 0u);
        w(kReadCounter, 0u);
        w(kBytesCounter, 0u);
        for (unsigned i = 0; i < 16; ++i) w(kFixedRets + 4u * i, 0u);
        w(kSda + 0x615Cu, kEngine);
        w(kEngine + 0x108u, kStubAlloc);
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

std::string diff(const Snapshot& a, const Snapshot& b) {
    char text[120];
    if (a.r3 != b.r3) { std::snprintf(text, sizeof text, "r3 original=%08X native=%08X", a.r3, b.r3); return text; }
    if (a.r1 != b.r1 || a.r2 != b.r2 || a.r13 != b.r13 || a.lr != b.lr) return "r1 / r2 / r13 / lr differ";
    if (std::memcmp(a.saved, b.saved, sizeof a.saved) != 0) return "a callee-saved register differs";
    if (std::memcmp(a.f, b.f, sizeof a.f) != 0) return "a callee-saved FPR differs";
    for (u32 i = 0; i < a.data.size(); ++i)
        if (a.data[i] != b.data[i]) { std::snprintf(text, sizeof text, "data byte 0x%08X differs", kDataLo + i); return text; }
    for (u32 i = 0; i < a.test.size(); ++i)
        if (a.test[i] != b.test[i]) { std::snprintf(text, sizeof text, "test byte 0x%08X differs", kTestLo + i); return text; }
    return {};
}

struct Params {
    bool find_ok = true;
    u32 size = 0x2000u, version = 0x36000u;
    u32 tag = 6u, first = 0x100u, second = 0x180u;
    u32 ret_tag = 1u, ret_first = 1u, ret_second = 1u;
    u32 delta_first = 0u, delta_second = 0u;
    u32 entries = 3u;      // [header + 8]
    u32 count = 5u;        // the halfword the 0x804662B0 body reads from [r4 + 0x78] + 4
    u32 alloc = kBuffer;
    u32 seed = 1u;
};

unsigned g_cases = 0;
const std::function<void(Cpu&)>* g_wrong = nullptr;

void setup(Machine& m, const Params& p) {
    m.fill();
    Rng rng(p.seed);
    for (u32 a = kBuffer; a < kBuffer + 0x400u; a += 4) m.w(a, rng.next());
    m.w(kBuffer + 0x18u + 8u, p.entries);
    m.w(kWorld + 0x78u, kExtent);
    m.memory.write16(kExtent + 4u, static_cast<u16>(p.count));
    m.memory.write16(kSda + 0x2AF0u, static_cast<u16>(0x4000u + p.seed));
    m.w(kFixedRets + 0x00u, p.find_ok ? 1u : 0u);
    m.w(kFixedRets + 0x0Cu, p.alloc);
    m.w(kFixedRets + 0x10u, 0x1234u);
    m.w(kFindSize, p.size);
    m.w(kFindVersion, p.version);
    const u32 values[3] = {p.tag, p.first, p.second};
    const u32 rets[3] = {p.ret_tag, p.ret_first, p.ret_second};
    for (unsigned i = 0; i < 3; ++i) {
        m.w(kReadValues + 4u * i, values[i]);
        m.w(kReadRets + 4u * i, rets[i]);
    }
    m.w(kBytesDeltas + 0, p.delta_first);
    m.w(kBytesDeltas + 4, p.delta_second);
}

void compare(const char* path, const char* what, const Params& p) {
    Machine original(path, false), native(path, true, g_wrong ? *g_wrong : std::function<void(Cpu&)>{});
    setup(original, p);
    setup(native, p);
    original.call(reg::kWorldSectorRead, 0x81302900u, kWorld);
    native.call(reg::kWorldSectorRead, 0x81302900u, kWorld);
    const std::string d = diff(original.snapshot(), native.snapshot());
    if (!d.empty()) {
        char text[200];
        std::snprintf(text, sizeof text, "%s (seed %u): %s", what, p.seed, d.c_str());
        throw std::runtime_error(text);
    }
    ++g_cases;
}

void run_all(const char* path) {
    g_cases = 0;
    Params p;
    {
        Params q = p; q.find_ok = false; compare(path, "no chunk", q);
    }
    for (u32 version : {0x34FFFu, 0x35000u, 0x36000u, 0x37002u, 0x37003u, 0u, 0xFFFFFFFFu}) {
        Params q = p; q.version = version; q.seed = version & 0xFFu; compare(path, "version", q);
    }
    {
        Params q = p; q.ret_tag = 0; compare(path, "tag read fails", q);
        q = p; q.tag = 5; compare(path, "wrong tag", q);
        q = p; q.ret_first = 0; compare(path, "first read fails", q);
        q = p; q.ret_second = 0; compare(path, "second read fails", q);
        q = p; q.delta_first = 1; compare(path, "first block short", q);
        q = p; q.delta_second = 0xFFFFFFFFu; compare(path, "second block short", q);
    }
    u32 seed = 100;
    for (u32 entries : {0u, 1u, 3u, 6u})
        for (u32 count : {0u, 1u, 5u, 8u, 9u, 17u, 24u})
            for (u32 first : {0x40u, 0x105u}) {
                Params q = p;
                q.entries = entries; q.count = count; q.first = first; q.second = 0x20u + count * 3u; q.seed = ++seed;
                compare(path, "world sector read", q);
            }
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
        require(argc == 2, "usage: rw_world_contract_tests main.dol");
        run_all(argv[1]);
        const unsigned cases = g_cases;
        require(caught(argv[1], [](Cpu& cpu) {
            cpu.install_hle(reg::kWorldSectorRead, [](Cpu& c) {
                c.gpr[3] = c.gpr[4];  // does nothing but return
                return true;
            }, "wrong: no read");
        }), "negative control: a reader that does nothing was not told apart");
        require(caught(argv[1], [](Cpu& cpu) {
            cpu.install_hle(reg::kWorldSectorRead, [](Cpu& c) {
                const u32 argument = c.gpr[4];
                u32 r = 0;
                const u32 frame = c.gpr[1] - 0x50u;
                // reads without looking the chunk up first
                guest_call(c, 0x8047E8B8u, reg::kWorldSectorRead + 4u, {c.gpr[3], frame + 8u, 4u}, r);
                c.gpr[3] = argument;
                return true;
            }, "wrong: skips the chunk lookup");
        }), "negative control: a reader that skips the chunk lookup was not told apart");
        std::printf("PASS rw world contract: %u original-vs-native cases\n", cases);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL rw world contract: %s\n", error.what());
        return 1;
    }
}
