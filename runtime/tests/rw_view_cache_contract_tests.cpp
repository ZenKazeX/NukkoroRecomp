// liveness-roots: reference-interpreter-only
// Original-DOL contract for the driver camera view-matrix cache (gekko/native_rw_view_cache.h): 0x804759B4 and 0x80475A1C.
//
// Reference interpreter, pinned DOL. Each case builds one memory state (the device-state global 0x805E2A10 with its matrix stack,
// view matrix, extents, cache words and camera; the camera's frame with its LTM; the matrix module's globals) and runs the ORIGINAL
// bodies and the owners from byte-identical memory, then compares the device-state block 0x805E2A00..0x805E2D80, a 4 KiB window,
// r3, r1, r2, r13, r14..r31, LR and both slots of f14..f31. The three pad words of the three cached matrices (the original copies
// stale stack bytes into them) are masked; nothing else is. The RenderWare callees run as the original DOL's own code on both sides.
//
// Negative controls: an owner that never sets the dirty flag, one that swaps the scale signs, one that skips the second flag clear.
//
// usage: rw_view_cache_contract_tests <sys/main.dol>
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_rw_view_cache.h"

#include <cmath>
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
namespace vc = gekko::rw_view_cache;

constexpr u32 kSda = 0x805EC500u, kSda2 = 0x805FA780u;
constexpr u32 kStack = 0x817E0000u, kFinish = 0x817FF000u;
constexpr u32 kWindow = 0x81302000u, kWindowBytes = 0x1000u;
constexpr u32 kDevice = 0x805E2A10u, kDeviceBlock = 0x805E2A00u, kDeviceBytes = 0x380u;
constexpr u32 kEngine = 0x805E3C08u, kModuleOffset = 0x300u;
constexpr u32 kMatrixA = 0x81302100u, kCamera = 0x81302200u, kFrame = 0x81302300u, kSyncObject = 0x81302500u;

void require(bool ok, const std::string& what) {
    if (!ok) throw std::runtime_error(what);
}

struct Rng {
    u32 state;
    explicit Rng(u32 seed) : state(seed * 2654435761u + 12345u) {}
    u32 next() {
        state = state * 1664525u + 1013904223u;
        return state >> 8 | (state << 24);
    }
    u32 below(u32 n) { return next() % n; }
};

u32 float_bits(float f) {
    u32 w;
    std::memcpy(&w, &f, 4);
    return w;
}

u32 random_float(Rng& rng) {
    const u32 sign = rng.below(2) ? 0x80000000u : 0u;
    switch (rng.below(16)) {
        case 0: return sign;
        case 1: return sign | (rng.next() & 0x007FFFFFu);
        case 2: return sign | 0x7F800000u;
        case 3: return sign | 0x7F800000u | (rng.next() & 0x007FFFFFu) | 1u;
        case 4: return sign | 0x7FC00000u | (rng.next() & 0x003FFFFFu);
        case 5: return sign | 0x00800000u | (rng.next() & 0x000FFFFFu);
        case 6: return sign | 0x7F7FFFFFu;
        default: return float_bits(static_cast<float>(static_cast<int>(rng.below(2001)) - 1000) / 64.0f);
    }
}

// An ordinary matrix: a rotation-like block and a translation.
void ordinary(Rng& rng, u32 out[12]) {
    const float a = static_cast<float>(rng.below(6283)) / 1000.0f, b = static_cast<float>(rng.below(6283)) / 1000.0f;
    const float s = 0.25f + static_cast<float>(rng.below(400)) / 100.0f;
    const float ca = std::cos(a), sa = std::sin(a), cb = std::cos(b), sb = std::sin(b);
    const float m[9] = {ca * cb * s, sa * s, -ca * sb * s, -sa * cb * s, ca * s, sa * sb * s, sb * s, 0.0f, cb * s};
    for (int i = 0; i < 9; ++i) out[i] = float_bits(m[i]);
    for (int i = 9; i < 12; ++i) out[i] = float_bits(static_cast<float>(static_cast<int>(rng.below(2001)) - 1000) / 16.0f);
}

struct Snapshot {
    u32 r3, r1, r2, r13, lr;
    u32 saved[18];
    u64 f[18], f_second[18];
    std::vector<u8> window, device;
};

struct Machine {
    GuestMemory memory;
    Cpu cpu{memory};
    u64 f1_value = 0, f2_value = 0;

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
        if (native) {
            vc::install(cpu);
            if (wrong) wrong(cpu);
        }
    }

    void w(u32 address, u32 value) { memory.write32(address, value); }

    void call(u32 entry, bool ni) {
        cpu.reset();
        cpu.set_msr(cpu.msr() | 0x2000u);
        cpu.spr[SPR_GQR0] = 0u;
        cpu.fpscr = ni ? kFpscrNI : 0u;
        cpu.pc = entry; cpu.npc = entry + 4; cpu.lr = kFinish;
        for (unsigned i = 0; i < 32; ++i) cpu.gpr[i] = 0x1000u + i * 0x111u;
        cpu.gpr[1] = kStack; cpu.gpr[2] = kSda2; cpu.gpr[13] = kSda;
        for (unsigned i = 0; i < 32; ++i) {
            const u64 value = 0x4000000000000000ull + i;
            std::memcpy(&cpu.fpr[i].bits[0], &value, sizeof(value));
            std::memcpy(&cpu.fpr[i].bits[1], &value, sizeof(value));
        }
        cpu.fpr[1].bits[0] = cpu.fpr[1].bits[1] = f1_value;
        cpu.fpr[2].bits[0] = cpu.fpr[2].bits[1] = f2_value;
        unsigned steps = 0;
        while (cpu.pc != kFinish && !cpu.stopped() && steps++ < 2000000) cpu.run_steps(1);
        require(cpu.pc == kFinish && !cpu.stopped(), "the call did not return");
    }

    Snapshot snapshot() const {
        Snapshot s{};
        s.r3 = cpu.gpr[3]; s.r1 = cpu.gpr[1]; s.r2 = cpu.gpr[2]; s.r13 = cpu.gpr[13]; s.lr = cpu.lr;
        for (unsigned i = 0; i < 18; ++i) {
            s.saved[i] = cpu.gpr[14 + i];
            std::memcpy(&s.f[i], &cpu.fpr[14 + i].bits[0], sizeof(u64));
            std::memcpy(&s.f_second[i], &cpu.fpr[14 + i].bits[1], sizeof(u64));
        }
        s.window.resize(kWindowBytes);
        for (u32 i = 0; i < kWindowBytes; ++i) s.window[i] = memory.peek8(kWindow + i);
        s.device.resize(kDeviceBytes);
        for (u32 i = 0; i < kDeviceBytes; ++i) s.device[i] = memory.peek8(kDeviceBlock + i);
        return s;
    }
};

bool is_pad(u32 address) {
    for (const u32 base : {0x258u, 0x298u, 0x2D8u})
        for (const u32 pad : {0x1Cu, 0x2Cu, 0x3Cu})
            if (address == kDevice + base + pad) return true;
    return false;
}

std::string diff(const Snapshot& a, const Snapshot& b) {
    char text[160];
    if (a.r3 != b.r3) { std::snprintf(text, sizeof text, "r3 original=%08X native=%08X", a.r3, b.r3); return text; }
    if (a.r1 != b.r1 || a.r2 != b.r2 || a.r13 != b.r13 || a.lr != b.lr) return "r1 / r2 / r13 / lr differ";
    if (std::memcmp(a.saved, b.saved, sizeof a.saved) != 0) return "a callee-saved register differs";
    if (std::memcmp(a.f, b.f, sizeof a.f) != 0) return "a callee-saved FPR differs";
    if (std::memcmp(a.f_second, b.f_second, sizeof a.f_second) != 0) return "a callee-saved FPR second slot differs";
    for (u32 i = 0; i < kWindowBytes; i += 4) {
        u32 x, y;
        std::memcpy(&x, &a.window[i], 4);
        std::memcpy(&y, &b.window[i], 4);
        if (x != y) { std::snprintf(text, sizeof text, "window word 0x%08X differs (original %08X native %08X)", kWindow + i, x, y); return text; }
    }
    for (u32 i = 0; i < kDeviceBytes; i += 4) {
        u32 x, y;
        std::memcpy(&x, &a.device[i], 4);
        std::memcpy(&y, &b.device[i], 4);
        if (x != y && !is_pad(kDeviceBlock + i)) {
            std::snprintf(text, sizeof text, "device word 0x%08X differs (original %08X native %08X)", kDeviceBlock + i, x, y);
            return text;
        }
    }
    return {};
}

unsigned g_cases = 0;
const std::function<void(Cpu&)>* g_wrong = nullptr;
u32 g_only_entry = 0;

struct Case {
    u32 entry, seed, index, dirty, camera_type;
    bool ni;
};

void fill(Machine& m, const Case& k) {
    for (u32 a = kWindow; a < kWindow + kWindowBytes; a += 4) m.w(a, 0x5A5A5A5Au ^ (a * 40503u));
    for (u32 a = kDeviceBlock; a < kDeviceBlock + kDeviceBytes; a += 4) m.w(a, 0xA5A5A5A5u ^ (a * 2654435761u));
    Rng rng(k.seed);
    const auto put_matrix = [&](u32 address, bool flags_random) {
        u32 words[12];
        if (rng.below(4) != 0) ordinary(rng, words);
        else
            for (u32& word : words) word = random_float(rng);
        const u32 rows[4] = {0x00u, 0x10u, 0x20u, 0x30u};
        for (u32 r = 0; r < 4; ++r)
            for (u32 j = 0; j < 3; ++j) m.w(address + rows[r] + 4 * j, words[3 * r + j]);
        static const u32 flags[] = {0x0u, 0x1u, 0x2u, 0x3u, 0x20000u, 0x20003u};
        m.w(address + 0x0Cu, flags_random ? flags[rng.below(6)] : 0x3u);
        for (u32 pad : {0x1Cu, 0x2Cu, 0x3Cu}) m.w(address + pad, rng.next());
    };
    // the matrix stack: every slot points at its own matrix in the window
    m.w(kDevice + 0x244u, k.index);
    for (u32 i = 0; i < 4; ++i) {
        m.w(kDevice + 0x1C4u + 4u * i, kMatrixA + 0x40u * i);
        put_matrix(kMatrixA + 0x40u * i, true);
    }
    put_matrix(kDevice + 0x184u, true);                      // the view matrix
    put_matrix(kDevice + 0x258u, true);                      // the cached matrices start with other content
    put_matrix(kDevice + 0x298u, true);
    put_matrix(kDevice + 0x2D8u, true);
    m.w(kDevice + 0x248u, rng.below(5) == 0 ? random_float(rng) : float_bits(0.05f + static_cast<float>(rng.below(1000)) / 500.0f));
    m.w(kDevice + 0x24Cu, rng.below(5) == 0 ? random_float(rng) : float_bits(0.05f + static_cast<float>(rng.below(1000)) / 500.0f));
    m.w(kDevice + 0x320u, rng.below(5) == 0 ? random_float(rng) : float_bits(0.5f + static_cast<float>(rng.below(1000)) / 300.0f));
    m.w(kDevice + 0x318u, k.dirty);
    m.w(kDevice + 0x32Cu, rng.next() | 1u);
    // the camera, its frame (the LTM at +0x50) and the frame's sync object (low bit of the byte at +3 clear: no sync)
    m.w(kDevice + 0x34Cu, kCamera);
    m.w(kCamera + 0x14u, k.camera_type);
    m.w(kCamera + 4u, kFrame);
    put_matrix(kFrame + 0x50u, true);
    m.w(kFrame + 0xA0u, kSyncObject);
    m.w(kSyncObject, 0u);
    // the engine instance and the matrix module's globals
    m.w(kSda + 0x615Cu, kEngine);
    m.w(kSda + 0x6108u, kModuleOffset);
    static const u32 optimisations[] = {0u, 0x20000u};
    m.w(kEngine + kModuleOffset + 4u, optimisations[rng.below(2)]);
    m.f1_value = 0;
}

void compare(const char* path, const char* what, const Case& k, u64 f1, u64 f2) {
    if (g_only_entry != 0u && k.entry != g_only_entry) return;
    Machine original(path, false), native(path, true, g_wrong ? *g_wrong : std::function<void(Cpu&)>{});
    fill(original, k);
    fill(native, k);
    original.f1_value = native.f1_value = f1;
    original.f2_value = native.f2_value = f2;
    original.call(k.entry, k.ni);
    native.call(k.entry, k.ni);
    const std::string d = diff(original.snapshot(), native.snapshot());
    if (!d.empty()) {
        char text[300];
        std::snprintf(text, sizeof text, "%s (seed %u, ni %d, index %u, dirty %u, camera type %u): %s", what, k.seed, k.ni ? 1 : 0,
                      k.index, k.dirty, k.camera_type, d.c_str());
        throw std::runtime_error(text);
    }
    ++g_cases;
}

void run_all(const char* path) {
    g_cases = 0;
    u32 seed = 1;
    for (const bool ni : {false, true})
        for (u32 index = 0; index < 4; ++index)
            for (u32 repeat = 0; repeat < 14; ++repeat) {
                Rng rng(seed * 7u + 3u);
                const auto angle = [&]() {
                    if (rng.below(4) == 0) return static_cast<u64>(rng.next()) << 32 | rng.next();
                    return single_to_double_bits(random_float(rng));
                };
                compare(path, "set offset", Case{vc::kSetOffset, seed, index, 0u, 1u, ni}, angle(), angle());
                ++seed;
                for (const u32 dirty : {0u, 1u})
                    for (const u32 type : {1u, 2u, 3u}) {
                        compare(path, "refresh", Case{vc::kRefresh, seed, index, dirty, type, ni}, 0u, 0u);
                        ++seed;
                    }
            }
}

bool caught(const char* path, const std::function<void(Cpu&)>& wrong, u32 only_entry) {
    g_wrong = &wrong;
    g_only_entry = only_entry;
    bool found = false;
    try {
        run_all(path);
    } catch (const std::exception&) {
        found = true;
    }
    g_wrong = nullptr;
    g_only_entry = 0u;
    return found;
}

// kind 0: refresh never sets the dirty flag; kind 1: set offset leaves the second flag; kind 2: refresh returns the scaled matrix
void install_wrong(Cpu& cpu, int kind) {
    if (kind == 0)
        cpu.install_hle(vc::kRefresh, [](Cpu& c) {
            const u32 flag = c.mem_read32(0x805E2A10u + 0x318u);
            const bool ok = vc::serve_refresh(c);
            if (ok) c.mem_write32(0x805E2A10u + 0x318u, flag);
            return ok;
        }, "wrong refresh");
    if (kind == 1)
        cpu.install_hle(vc::kSetOffset, [](Cpu& c) {
            const u32 flag = c.mem_read32(0x805E2A10u + 0x32Cu);
            const bool ok = vc::serve_set_offset(c);
            if (ok) c.mem_write32(0x805E2A10u + 0x32Cu, flag);
            return ok;
        }, "wrong set offset");
    if (kind == 2)
        cpu.install_hle(vc::kRefresh, [](Cpu& c) {
            const bool ok = vc::serve_refresh(c);
            if (ok) c.gpr[3] = 0x805E2A10u + 0x258u;
            return ok;
        }, "wrong refresh result");
}
}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: rw_view_cache_contract_tests main.dol");
        run_all(argv[1]);
        const unsigned cases = g_cases;
        const u32 entries[3] = {vc::kRefresh, vc::kSetOffset, vc::kRefresh};
        for (int kind = 0; kind < 3; ++kind)
            require(caught(argv[1], [kind](Cpu& cpu) { install_wrong(cpu, kind); }, entries[kind]),
                    "negative control: a wrong view cache owner (" + std::to_string(kind) + ") was not told apart");
        std::printf("PASS rw view cache contract: %u original-vs-native cases\n", cases);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL rw view cache contract: %s\n", error.what());
        return 1;
    }
}
