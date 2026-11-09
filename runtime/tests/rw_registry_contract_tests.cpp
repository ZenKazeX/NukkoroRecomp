// liveness-roots: reference-interpreter-only
// Original-DOL contract for the RenderWare plugin-registry owners (gekko/native_rw_registry.h): plugin init 0x80494EB4,
// driver plugin destruct / construct 0x8049BFE4 / 0x8049C048, device pool construct / destruct 0x8049C11C / 0x8049C160 and
// the texture free-list element callbacks 0x8049AB74 / 0x8049AB88.
//
// Reference interpreter, pinned DOL. Each case builds one machine state and runs the ORIGINAL body and the owner from
// byte-identical memory. The RenderWare core functions the bodies call (free-list and list helpers, the block and pool
// creators) are replaced on BOTH machines by recording stubs that log (id, r3..r8) and return a per-case value: those
// functions are other rows with their own contracts, and the stubs make the comparison exact about what the body passes
// them and in which order. Compared per case: r3, r1, r2, r13, r14..r31, lr, f14..f31, every byte of two memory windows
// (the engine data and small-data area 0x80560000..0x80600000 and the test area 0x81300000..0x81310000, which holds the
// stub log). The stack is outside both windows.
//
// Negative controls: owners that store a wrong value, skip a store, change the call order or pass a wrong argument
// are told apart.
//
// usage: rw_registry_contract_tests <sys/main.dol>
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
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
constexpr u32 kLogOffset = 0x81301000u, kLogBase = 0x81301100u, kReturns = 0x81301800u;
constexpr u32 kEngine = 0x805E3C08u;

// the RenderWare core callees, in stub-id order
constexpr u32 kCallees[] = {0x8048ACA4u, 0x8048E2D0u, 0x804A09E0u, 0x804A0998u, 0x804A08FCu, 0x804A0A6Cu,
                            0x804A01B8u, 0x804A011Cu, 0x80480C54u, 0x80480D2Cu, 0x804A02F4u};
constexpr unsigned kCalleeCount = sizeof(kCallees) / sizeof(kCallees[0]);
constexpr unsigned kArgs[kCalleeCount] = {5, 5, 1, 1, 1, 1, 1, 1, 6, 1, 1};  // register arguments each callee takes

void require(bool ok, const std::string& what) {
    if (!ok) throw std::runtime_error(what);
}

// A tiny assembler for the stubs.
u32 addis(unsigned rd, unsigned ra, u32 imm) { return (15u << 26) | (rd << 21) | (ra << 16) | (imm & 0xFFFFu); }
u32 addi(unsigned rd, unsigned ra, u32 imm) { return (14u << 26) | (rd << 21) | (ra << 16) | (imm & 0xFFFFu); }
u32 lwz(unsigned rd, unsigned ra, u32 d) { return (32u << 26) | (rd << 21) | (ra << 16) | (d & 0xFFFFu); }
u32 stw(unsigned rs, unsigned ra, u32 d) { return (36u << 26) | (rs << 21) | (ra << 16) | (d & 0xFFFFu); }
u32 add(unsigned rd, unsigned ra, unsigned rb) { return (31u << 26) | (rd << 21) | (ra << 16) | (rb << 11) | (266u << 1); }

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
        // The recording stubs replace the RenderWare core callees on both machines.
        for (unsigned k = 0; k < kCalleeCount; ++k) {
            // the stub logs the arguments the callee really takes (kArgs[k] registers from r3), not the registers
            // the caller happens to leave in r4..r8
            std::vector<u32> code = {addis(12, 0, 0x8130), addi(10, 12, 0x1100), lwz(11, 12, 0x1000), add(10, 10, 11),
                                     addi(9, 0, k), stw(9, 10, 0)};
            for (unsigned a = 0; a < kArgs[k]; ++a) code.push_back(stw(3 + a, 10, 4 + 4 * a));
            code.push_back(addi(11, 11, 0x20));
            code.push_back(stw(11, 12, 0x1000));
            code.push_back(lwz(3, 12, 0x1800 + 4 * k));
            code.push_back(0x4E800020u);
            for (unsigned i = 0; i < code.size(); ++i) memory.write32(kCallees[k] + 4u * i, code[i]);
        }
        if (native) {
            reg::install(cpu);
            if (wrong) wrong(cpu);
        }
    }

    void fill() {
        for (u32 a = kDataLo; a < kDataHi; a += 4) memory.write32(a, 0xA5000000u ^ (a * 2654435761u));
        for (u32 a = kTestLo; a < kTestHi; a += 4) memory.write32(a, 0x5A000000u ^ (a * 40503u));
        memory.write32(kLogOffset, 0u);
        for (unsigned k = 0; k < 16; ++k) memory.write32(kReturns + 4 * k, 0u);
        memory.write32(kSda + 0x615Cu, kEngine);
    }
    void sda(u32 offset, u32 value) { memory.write32(kSda + offset, value); }

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
        while (cpu.pc != kFinish && !cpu.stopped() && steps++ < 400000) cpu.run_steps(1);
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

using Setup = std::function<void(Machine&)>;
unsigned g_cases = 0;
const std::function<void(Cpu&)>* g_wrong = nullptr;

void compare(const char* path, const char* what, u32 entry, u32 r3, u32 r4, const Setup& setup) {
    Machine original(path, false), native(path, true, g_wrong ? *g_wrong : std::function<void(Cpu&)>{});
    for (Machine* m : {&original, &native}) {
        m->fill();
        setup(*m);
    }
    original.call(entry, r3, r4);
    native.call(entry, r3, r4);
    const std::string d = diff(original.snapshot(), native.snapshot());
    if (!d.empty()) {
        char text[200];
        std::snprintf(text, sizeof text, "%s r3=%08X r4=%08X: %s", what, r3, r4, d.c_str());
        throw std::runtime_error(text);
    }
    ++g_cases;
}

// stub return values: index in kCallees
enum Callee { kListA, kListB, kRel20, kRel1C, kBuild1C, kBuild20, kBlockCreate, kBlockDestroy, kPoolCreate, kPoolDestroy,
              kPoolListInit };
void ret(Machine& m, Callee c, u32 value) { m.memory.write32(kReturns + 4u * static_cast<u32>(c), value); }

void run_all(const char* path) {
    g_cases = 0;
    const u32 values[] = {0u, 0x81302100u, 0xFFFFFFFFu};
    // 0x80494EB4
    for (u32 a : values)
        for (u32 b : values)
            compare(path, "plugin init", reg::kPluginInit, 0x11u, 0x22u, [&](Machine& m) { ret(m, kListA, a); ret(m, kListB, b); });
    // 0x8049BFE4 / 0x8049C048
    for (u32 off : {0x100u, 0x7F4u})
        for (u32 refs : {0u, 1u, 5u})
            for (u32 object : {0u, 0x81300500u}) {
                compare(path, "driver destruct", reg::kDriverDestruct, object, 0u, [&](Machine& m) {
                    m.sda(0x622C, off); m.sda(0x6230, refs); m.sda(0x6228, 0x12345678u);
                });
                for (u32 first : {0u, 1u, 0x81302300u})
                    for (u32 second : {0u, 1u}) {
                        compare(path, "driver construct", reg::kDriverConstruct, object, off, [&](Machine& m) {
                            m.sda(0x6230, refs); m.sda(0x622C, 0x55u); m.sda(0x6228, 0x66u);
                            ret(m, kBuild1C, first); ret(m, kBuild20, second);
                        });
                    }
            }
    // 0x8049C11C / 0x8049C160
    for (u32 ready : {0u, 1u})
        for (u32 block : {0u, 0x81302400u})
            for (u32 pool : {0u, 0x81302500u})
                for (u32 off : {0x120u, 0x800u}) {
                    const Setup setup = [&](Machine& m) {
                        m.sda(0x6240, ready); m.sda(0x6244, 0x77u); m.sda(0x2AF8, 0xA1u); m.sda(0x2AFC, 0xB2u);
                        m.sda(0x2B00, 0xC3u); m.sda(0x2B04, 0xD4u);
                        ret(m, kBlockCreate, block); ret(m, kPoolCreate, pool); ret(m, kPoolListInit, 9u);
                        ret(m, kBlockDestroy, 3u); ret(m, kPoolDestroy, 4u);
                    };
                    compare(path, "pool construct", reg::kPoolConstruct, 0x81300600u, off, setup);
                    compare(path, "pool destruct", reg::kPoolDestruct, 0x81300600u, 0u, [&](Machine& m) {
                        setup(m);
                        m.sda(0x6238, off);
                    });
                }
    // 0x8049AB74 / 0x8049AB88
    for (u32 plugin : {0x40u, 0x1F8u})
        compare(path, "element init", reg::kElementInit, 0x81300700u, 0u, [&](Machine& m) { m.sda(0x6218, plugin); });
    for (unsigned mask = 0; mask < 256; mask += 5)
        for (u32 r3 : {0x81300800u, 0u})
            compare(path, "element term", reg::kElementTerm, r3, 0u, [&](Machine& m) {
                for (unsigned i = 0; i < 8; ++i)
                    m.memory.write32(0x8056FCDCu + 4u * i, (mask >> i & 1u) ? r3 : 0x81300900u + i);
            });
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
        require(argc == 2, "usage: rw_registry_contract_tests main.dol");
        run_all(argv[1]);
        const unsigned cases = g_cases;
        require(caught(argv[1], [](Cpu& cpu) {
            cpu.install_hle(reg::kElementInit, [](Cpu& c) {
                c.mem_write32(c.gpr[3] + c.mem_read32(c.gpr[13] + 0x6218u) + 0x20u, 0x01000001u);
                return true;
            }, "wrong: the initialiser stores another value");
        }), "negative control: a wrong element initialiser was not told apart");
        require(caught(argv[1], [](Cpu& cpu) {
            cpu.install_hle(reg::kElementTerm, [](Cpu& c) {
                for (u32 i = 0; i < 7u; ++i) {  // forgets the eighth slot
                    const u32 slot = 0x8056FCDCu + 4u * i;
                    if (c.mem_read32(slot) == c.gpr[3]) c.mem_write32(slot, 0u);
                }
                return true;
            }, "wrong: seven slots");
        }), "negative control: an element terminator that skips a slot was not told apart");
        require(caught(argv[1], [](Cpu& cpu) {
            cpu.install_hle(reg::kPluginInit, [](Cpu& c) {
                u32 raster = 0, texture = 0;
                guest_call(c, 0x8048E2D0u, 0x8049AC74u, {0x24u, 0x40Cu, reg::kElementInit, reg::kElementTerm, 0u}, texture);
                guest_call(c, 0x8048ACA4u, 0x80496464u, {0x34u, 0x40Cu, 0u, 0u, 0u}, raster);  // swapped order
                c.mem_write32(c.gpr[13] + 0x6200u, raster);
                c.mem_write32(c.gpr[13] + 0x6218u, texture);
                c.gpr[3] = 1u;
                return true;
            }, "wrong: calls in the wrong order");
        }), "negative control: a plugin init that swaps its two calls was not told apart");
        require(caught(argv[1], [](Cpu& cpu) {
            cpu.install_hle(reg::kDriverDestruct, [](Cpu& c) {
                const u32 object = c.gpr[3];
                u32 ignored = 0;
                const u32 instance = c.mem_read32(c.gpr[13] + 0x615Cu) + c.mem_read32(c.gpr[13] + 0x622Cu);
                guest_call(c, 0x804A09E0u, 0x8049C00Cu, {instance + 0x1Cu}, ignored);  // wrong field
                guest_call(c, 0x804A0998u, 0x8049C020u, {instance + 0x1Cu}, ignored);
                c.mem_write32(c.gpr[13] + 0x6230u, c.mem_read32(c.gpr[13] + 0x6230u) - 1u);
                c.gpr[3] = object;
                return true;
            }, "wrong: wrong list argument");
        }), "negative control: a driver destruct that passes a wrong argument was not told apart");
        require(caught(argv[1], [](Cpu& cpu) {
            cpu.install_hle(reg::kPoolDestruct, [](Cpu& c) {
                c.gpr[3] = c.gpr[3];  // forgets to destroy the pool
                return true;
            }, "wrong: no destroy");
        }), "negative control: a pool destruct that does nothing was not told apart");
        std::printf("PASS rw registry contract: %u original-vs-native cases\n", cases);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL rw registry contract: %s\n", error.what());
        return 1;
    }
}
