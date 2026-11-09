// liveness-roots: reference-interpreter-only
// Original-DOL contract for the RenderWare device render-state owners (gekko/native_rw_device_state.h): 0x804960FC (camera
// view-matrix upload) and 0x804987C8 (the alpha-test / z-compare-location mode setter).
//
// Reference interpreter, pinned DOL. Each case builds one machine state, runs the ORIGINAL body (every GX function it calls
// is the production silent owner, gx_state::install, on both machines) and the owner from byte-identical memory, and compares:
// r3, r1, r2, r13, r14..r31, lr, f14..f31 and every byte of two memory windows (the engine's data and small-data area
// 0x805D0000..0x80600000 and the test area 0x81300000..0x81310000). The stack is outside both windows (a dead frame).
//
// Negative controls: an owner that skips the cache store, one that stores the wrong value, and a view-matrix upload that
// stores a word are each told apart by the comparison.
//
// usage: rw_device_state_contract_tests <sys/main.dol>
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_gx_state.h"
#include "gekko/native_rw_device_state.h"

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

constexpr u32 kSda = 0x805EC500u, kSda2 = 0x805FA780u;
constexpr u32 kStack = 0x817E0000u, kFinish = 0x817FF000u;
constexpr u32 kDataLo = 0x805D0000u, kDataHi = 0x80600000u;
constexpr u32 kTestLo = 0x81300000u, kTestHi = 0x81310000u;

void require(bool ok, const std::string& what) {
    if (!ok) throw std::runtime_error(what);
}

struct Snapshot {
    u32 r3, r1, r2, r13, lr;
    u32 saved[18];
    u64 f[18];
    std::vector<u8> data, test;
};

struct Machine {
    GuestMemory memory;
    Cpu cpu{memory};
    u64 steps = 0;

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
        gx_state::install(cpu);
        if (native) {
            rw_device_state::install(cpu);
            if (wrong) wrong(cpu);
        }
    }

    // One state shared by both machines.
    void prepare(u32 mode_cache) {
        for (u32 a = kDataLo; a < kDataHi; a += 4) memory.write32(a, 0xA5000000u ^ (a * 2654435761u));
        for (u32 a = kTestLo; a < kTestHi; a += 4) memory.write32(a, 0x5A000000u ^ (a * 40503u));
        memory.write32(rw_device_state::kModeCache, mode_cache);
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
        unsigned steps_taken = 0;
        while (cpu.pc != kFinish && !cpu.stopped() && steps_taken++ < 400000) cpu.run_steps(1);
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

struct Counters { unsigned cases = 0; };

// Runs original and native on one state; throws with the difference.
void compare(const char* path, const char* what, u32 entry, u32 r3, u32 r4, u32 mode_cache,
             const std::function<void(Cpu&)>& wrong, Counters& n) {
    Machine original(path, false), native(path, true, wrong);
    original.prepare(mode_cache);
    native.prepare(mode_cache);
    original.call(entry, r3, r4);
    native.call(entry, r3, r4);
    const std::string d = diff(original.snapshot(), native.snapshot());
    if (!d.empty()) {
        char text[200];
        std::snprintf(text, sizeof text, "%s entry %08X r3=%08X r4=%08X cache=%08X: %s", what, entry, r3, r4,
                      mode_cache, d.c_str());
        throw std::runtime_error(text);
    }
    ++n.cases;
}

unsigned run_all(const char* path, const std::function<void(Cpu&)>& wrong, bool include_view) {
    Counters n;
    const u32 caches[] = {0u, 1u, 2u, 0x101u, 0xFFFFFFFFu};
    // 0x804987C8: every (mode, cache) pair
    for (u32 mode : {0u, 1u, 2u, 3u, 0x7Fu, 0x100u, 0x101u, 0xFFFFFFFFu})
        for (u32 cache : caches)
            compare(path, "mode setter", rw_device_state::kModeSetter, mode, 0u, cache, wrong, n);
    // 0x804960FC: no matrix and a matrix, with and without the normal matrix
    if (include_view)
        for (u32 camera : {0u, 0x81300400u})
            for (u32 normal : {0u, 1u})
                compare(path, "view-matrix upload", rw_device_state::kViewMatrixUpload, camera, normal, 0x5Au, wrong, n);
    return n.cases;
}

bool caught(const char* path, const std::function<void(Cpu&)>& wrong) {
    try {
        run_all(path, wrong, true);
    } catch (const std::exception&) {
        return true;
    }
    return false;
}
}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: rw_device_state_contract_tests main.dol");
        const unsigned cases = run_all(argv[1], nullptr, true);
        // The view-matrix upload has no guest-visible effect: the original's run changed nothing but r3 (the compare above
        // would have found a stored word in either window).
        require(caught(argv[1], [](Cpu& cpu) {
            cpu.install_hle(rw_device_state::kModeSetter, [](Cpu&) { return true; }, "wrong: skips the cache store");
        }), "negative control: a mode setter that skips the store was not told apart");
        require(caught(argv[1], [](Cpu& cpu) {
            cpu.install_hle(rw_device_state::kModeSetter, [](Cpu& c) {
                c.mem_write32(rw_device_state::kModeCache, c.gpr[3] + 1u);
                return true;
            }, "wrong: stores the wrong mode");
        }), "negative control: a mode setter that stores the wrong value was not told apart");
        require(caught(argv[1], [](Cpu& cpu) {
            cpu.install_hle(rw_device_state::kViewMatrixUpload, [](Cpu& c) {
                c.mem_write32(0x81300400u, 1u);
                c.gpr[3] = 0u;
                return true;
            }, "wrong: stores");
        }), "negative control: a view-matrix upload that stores a word was not told apart");
        std::printf("PASS rw device state contract: %u original-vs-native cases\n", cases);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL rw device state contract: %s\n", error.what());
        return 1;
    }
}
