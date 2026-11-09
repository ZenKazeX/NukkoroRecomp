// liveness-roots: reference-interpreter-only
// Original-DOL contract for the owned native texture writer (gekko/native_rw_registry.h): 0x8049A604.
//
// Reference interpreter, pinned DOL. Each case builds one machine state (a texture, its raster and the raster's platform data)
// and runs the ORIGINAL body and the owner from byte-identical memory. The RenderWare functions it calls (0x8047DF40 chunk header,
// 0x80481F20 stream write, 0x8048AD34 raster filter query) are recording stubs on BOTH machines; the stream-write stub copies
// every buffer it is handed into a capture area (so the bytes of the texture header, the raster header, the palette and the
// pixels are compared, not only the pointers), and returns a scripted success or failure per call. The raster size row
// 0x804965A8 is another driver row with its own owner: both machines get the same test owner for it (the original reaches it by
// `bl`, the native body through Cpu::serve_owner). The four GX texture-object getters the original calls stay the original
// guest code on the original machine and are inlined in the owner. Compared: r3, r1, r2, r13, r14..r31, lr, f14..f31, every byte
// of the engine / small-data window and of the test area (the texture, the stub log and the capture area).
//
// Negative controls: owners that write nothing, that write only the headers, or that report success on a failed write are
// told apart.
//
// usage: rw_texture_write_contract_tests <sys/main.dol>
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
constexpr u32 kLogOffset = 0x81301000u;
constexpr u32 kCaptureOffset = 0x81301960u, kWriteCounter = 0x81301964u, kWriteRets = 0x81301980u, kFixedRets = 0x81301800u;
constexpr u32 kBytesRet = 0x81301F10u, kBytesLog = 0x81301F00u;
constexpr u32 kTexture = 0x81302000u, kRaster = 0x81302200u, kParent = 0x81302300u, kPixels = 0x81302600u, kPalette = 0x81302800u;
constexpr u32 kTexturePlugin = 0x60u, kRasterPlugin = 0x40u;

void require(bool ok, const std::string& what) {
    if (!ok) throw std::runtime_error(what);
}

u32 addis(unsigned rd, unsigned ra, u32 imm) { return (15u << 26) | (rd << 21) | (ra << 16) | (imm & 0xFFFFu); }
u32 addi(unsigned rd, unsigned ra, u32 imm) { return (14u << 26) | (rd << 21) | (ra << 16) | (imm & 0xFFFFu); }
u32 lwz(unsigned rd, unsigned ra, u32 d) { return (32u << 26) | (rd << 21) | (ra << 16) | (d & 0xFFFFu); }
u32 stw(unsigned rs, unsigned ra, u32 d) { return (36u << 26) | (rs << 21) | (ra << 16) | (d & 0xFFFFu); }
u32 lbz(unsigned rd, unsigned ra, u32 d) { return (34u << 26) | (rd << 21) | (ra << 16) | (d & 0xFFFFu); }
u32 stb(unsigned rs, unsigned ra, u32 d) { return (38u << 26) | (rs << 21) | (ra << 16) | (d & 0xFFFFu); }
u32 add(unsigned rd, unsigned ra, unsigned rb) { return (31u << 26) | (rd << 21) | (ra << 16) | (rb << 11) | (266u << 1); }
u32 lwzx(unsigned rd, unsigned ra, unsigned rb) { return (31u << 26) | (rd << 21) | (ra << 16) | (rb << 11) | (23u << 1); }
u32 mtctr(unsigned rs) { return 0x7C0903A6u | (rs << 21); }

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

struct Stub {
    u32 address;
    unsigned args;
};
// ids: 0 chunk header, 1 stream write (captures), 2 raster filter
constexpr Stub kStubs[] = {{0x8047DF40u, 5}, {0x80481F20u, 3}, {0x8048AD34u, 1}};
constexpr unsigned kStubCount = 3;

std::vector<u32> stub_code(unsigned k) {
    std::vector<u32> code = {addis(12, 0, 0x8130), addi(10, 12, 0x1100), lwz(11, 12, 0x1000), add(10, 10, 11),
                             addi(9, 0, k), stw(9, 10, 0)};
    for (unsigned a = 0; a < kStubs[k].args; ++a) code.push_back(stw(3 + a, 10, 4 + 4 * a));
    code.push_back(addi(11, 11, 0x20));
    code.push_back(stw(11, 12, 0x1000));
    code.push_back(addis(9, 0, 0xDEAD));  // a callee's own frame: scribble the words below r1
    for (unsigned s = 1; s <= 18; ++s) code.push_back(stw(9, 1, 0x10000u - 4 * s));
    if (k == 1) {
        // capture the buffer (r4, r5 bytes) and return the next scripted result
        code.push_back(lwz(9, 12, 0x1960));
        code.push_back(addi(8, 12, 0x7000));
        code.push_back(add(8, 8, 9));
        code.push_back(mtctr(5));
        code.push_back(addi(6, 4, 0));
        code.push_back(lbz(7, 6, 0));
        code.push_back(stb(7, 8, 0));
        code.push_back(addi(6, 6, 1));
        code.push_back(addi(8, 8, 1));
        code.push_back(0x4200FFF0u);  // bdnz to the lbz
        code.push_back(add(9, 9, 5));
        code.push_back(stw(9, 12, 0x1960));
        code.push_back(lwz(9, 12, 0x1964));
        code.push_back(addi(8, 12, 0x1980));
        code.push_back(lwzx(3, 8, 9));
        code.push_back(addi(9, 9, 4));
        code.push_back(stw(9, 12, 0x1964));
    } else {
        code.push_back(lwz(3, 12, 0x1800 + 4 * k));
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
        // the raster size row's test owner (both machines): logs its argument, answers the scripted size
        cpu.install_hle(0x804965A8u, [this](Cpu& c) {
            memory.write32(kBytesLog, c.gpr[3]);
            c.gpr[3] = memory.peek32(kBytesRet);
            return true;
        }, "test owner: raster bytes");
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
        w(kLogOffset, 0u);
        w(kCaptureOffset, 0u);
        w(kWriteCounter, 0u);
        for (unsigned i = 0; i < 16; ++i) w(kFixedRets + 4u * i, 0u);
        w(kFixedRets + 0u, 1u);
        w(kFixedRets + 8u, 1u);
        w(kSda + 0x6218u, kTexturePlugin);
        w(kSda + 0x6200u, kRasterPlugin);
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

unsigned g_cases = 0;
const std::function<void(Cpu&)>* g_wrong = nullptr;

void setup(Machine& m, u32 seed) {
    m.fill();
    Rng rng(seed);
    for (u32 a = kTexture; a < kTexture + 0x100u; a += 4) m.w(a, rng.next());
    for (u32 a = kRaster; a < kRaster + 0x40u; a += 4) m.w(a, rng.next());
    for (u32 a = kParent; a < kParent + 0x80u; a += 4) m.w(a, rng.next());
    const u32 flag_choices[] = {0x00u, 0x20u, 0x40u, 0x60u, 0x80u, 0xFFu, 0x1Fu};
    const u32 depth_choices[] = {0u, 1u, 2u, 4u, 5u, 8u};
    const u32 format = flag_choices[rng.below(7)];
    const u32 depth = depth_choices[rng.below(6)];
    m.w(kTexture + 0, kRaster);
    m.w(kTexture + 0x50u, rng.next());
    m.w(kTexture + kTexturePlugin + 0x20u, (rng.below(3) == 0) ? 0x01000000u | rng.next() : (rng.next() & ~0x01000000u));
    m.w(kRaster + 0, kParent);
    m.w(kRaster + 0xCu, rng.below(1024));
    m.w(kRaster + 0x10u, rng.below(1024));
    m.w(kRaster + 0x14u, depth);
    m.memory.write8(kRaster + 0x20u, static_cast<u8>(rng.next()));
    m.memory.write8(kRaster + 0x23u, static_cast<u8>(format));
    m.w(kParent + kRasterPlugin + 0xCu, rng.next());
    m.w(kParent + kRasterPlugin + 0x10u, rng.next());
    m.w(kParent + kRasterPlugin + 0x14u, rng.next());
    m.w(kParent + kRasterPlugin + 0x1Cu, kPixels);
    m.w(kParent + kRasterPlugin + 0x20u, kPalette);
    for (u32 a = kPixels; a < kPixels + 0x200u; a += 4) m.w(a, rng.next());
    for (u32 a = kPalette; a < kPalette + 0x200u; a += 4) m.w(a, rng.next());
    const u32 sizes[] = {1u, 0x40u, 0x123u, 0x1F0u};
    m.w(kBytesRet, sizes[rng.below(4)]);
    // scripted results: every call succeeds unless this seed injects a failure
    for (unsigned i = 0; i < 8; ++i) m.w(kWriteRets + 4u * i, 1u);
    if (seed % 5u == 0u) m.w(kWriteRets + 4u * rng.below(5), 0u);
    if (seed % 11u == 0u) m.w(kFixedRets + 0u, 0u);
}

void compare(const char* path, u32 seed) {
    Machine original(path, false), native(path, true, g_wrong ? *g_wrong : std::function<void(Cpu&)>{});
    setup(original, seed);
    setup(native, seed);
    original.call(reg::kNativeTextureWrite, 0x81302F00u, kTexture);
    native.call(reg::kNativeTextureWrite, 0x81302F00u, kTexture);
    const std::string d = diff(original.snapshot(), native.snapshot());
    if (!d.empty()) {
        char text[200];
        std::snprintf(text, sizeof text, "native texture write (seed %u): %s", seed, d.c_str());
        throw std::runtime_error(text);
    }
    ++g_cases;
}

void run_all(const char* path) {
    g_cases = 0;
    for (u32 seed = 1; seed <= 90; ++seed) compare(path, seed);
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
        require(argc == 2, "usage: rw_texture_write_contract_tests main.dol");
        run_all(argv[1]);
        const unsigned cases = g_cases;
        require(caught(argv[1], [](Cpu& cpu) {
            cpu.install_hle(reg::kNativeTextureWrite, [](Cpu& c) {
                c.gpr[3] = 1u;  // writes nothing
                return true;
            }, "wrong: writes nothing");
        }), "negative control: a texture writer that writes nothing was not told apart");
        require(caught(argv[1], [](Cpu& cpu) {
            cpu.install_hle(reg::kNativeTextureWrite, [](Cpu& c) {
                u32 r = 0;
                guest_call(c, 0x8047DF40u, reg::kNativeTextureWrite + 4u, {c.gpr[3], 1u, 0x58u, 0x37002u, 0x37u}, r);
                c.gpr[3] = 1u;  // the chunk header only, and success reported
                return true;
            }, "wrong: only the chunk header");
        }), "negative control: a texture writer that stops after the chunk header was not told apart");
        std::printf("PASS rw texture contract: %u original-vs-native cases\n", cases);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL rw texture contract: %s\n", error.what());
        return 1;
    }
}
