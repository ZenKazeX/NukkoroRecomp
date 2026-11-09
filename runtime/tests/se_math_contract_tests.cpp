// The SE manager's positional math against the original instructions (gekko/native_se_math.h).
// liveness-roots: reference-interpreter-only
//
// Both sides get the same guest memory: main.dol (and optionally a MEM1 image of the running game, with main.dol's
// text laid over it), plus synthetic fixtures written identically to both - an actor registry with one or two
// players (alive, missing, expired, without a count block, behind decoys), a camera manager with one camera, a
// channel, a table row. The original function runs from main.dol in the interpreter until it returns to a sentinel
// LR; the native function runs on the other side. After every call the result (r3 or the f1 bits), the FPSCR, and
// every byte either side wrote (the CPU's access trace; the original's own stack frames excepted) must be equal;
// every 512 calls and at the end, the whole of guest memory (minus that stack window) is compared as well.
//
// Usage: se_math_contract_tests <main.dol> [<MEM1 image, 24 MiB, loaded at 0x80000000>]

#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_se_math.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace gekko;
using se_math::Vec3;

// The original functions (main.dol).
constexpr u32 kListenerDistance = 0x801D9D04u, kPosPan = 0x801D95B4u, kPosVolume = 0x801D979Cu,
              kMaxDistance = 0x801D9F6Cu, kDopplerBend = 0x801D9408u;
// The routines they reach, tested on their own: MSL __ieee754_atan2, sin, atan and the game's sqrt.
constexpr u32 kAtan2 = 0x803AFAECu, kSin = 0x803B2AFCu, kAtan = 0x803B2210u, kSqrt = 0x8000FF04u;
constexpr u32 kR2 = 0x805FA780u, kR13 = 0x805EC500u, kConfig = 0x8057E760u;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::vector<u8> read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "cannot read " + path.string());
    return {std::istreambuf_iterator<char>(input), {}};
}

u32 f2u(float value) {
    u32 bits;
    std::memcpy(&bits, &value, 4);
    return bits;
}
float u2f(u32 bits) {
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
}

struct Extent {
    u32 address;
    u32 size;
};

struct Side {
    GuestMemory memory;
    Cpu cpu{memory};
    std::vector<Extent> writes;
    bool recording = false;
    Side() {
        memory.set_access_trace([this](u32 address, std::size_t size, bool writing) {
            if (recording && writing) writes.push_back({address, static_cast<u32>(size)});
        });
    }
};

// Where the fixtures, the stack and the sentinel live (synthetic: a fixed heap area; image: a zero window found in it).
struct Layout {
    u32 registry, elements, outer_counts, nodes, weak_counts, actors, camera, camera_elements, camera_object,
        camera_counts, chan, out, row, vec, stack, stack_low, sentinel;
    explicit Layout(u32 base) {
        registry = base;
        elements = base + 0x100u;
        outer_counts = base + 0x200u;
        nodes = base + 0x400u;
        weak_counts = base + 0x600u;
        actors = base + 0x1000u;          // 0x400 each, up to 8
        camera = base + 0x3000u;
        camera_elements = base + 0x3100u;
        camera_object = base + 0x3200u;
        camera_counts = base + 0x3300u;
        chan = base + 0x4000u;
        out = base + 0x4100u;
        row = base + 0x4200u;
        vec = base + 0x4300u;
        stack = base + 0xF000u;
        stack_low = base + 0x8000u;       // the original's frames: [stack_low, stack)
        sentinel = base + 0xFF00u;        // the return address; never executed
    }
};

struct Harness {
    Side original, native;
    Layout at;
    u64 calls = 0, instructions = 0, full_compares = 0, compared_bytes = 0;
    u64 per_function[5] = {};
    u64 routine_calls = 0;
    u64 fpscr_cycle = 0;
    std::set<u32> untouched_by_native;  // non-stack bytes the original wrote and the native side did not
    std::set<u32> original_outside_stack;

    explicit Harness(u32 base) : at(base) {}

    void load_dol(const std::vector<u8>& dol, bool text_only) {
        const auto word = [&](unsigned offset) { return load_be32(dol.data() + offset); };
        for (unsigned i = 0; i < 18; ++i) {
            const u32 offset = word(i * 4), address = word(0x48 + i * 4), size = word(0x90 + i * 4);
            require(u64(offset) + size <= dol.size(), "invalid DOL section");
            if (size == 0 || (text_only && i >= 7)) continue;
            for (Side* s : {&original, &native}) s->memory.write_block(address, dol.data() + offset, size);
        }
    }
    void load_image(const std::vector<u8>& image) {
        require(image.size() == 0x01800000u, "the MEM1 image must be 24 MiB");
        const u32 split = GuestMemory::kImageEnd - 0x80000000u;
        for (Side* s : {&original, &native}) {
            s->memory.write_block(0x80000000u, image.data(), split);
            s->memory.write_block(GuestMemory::kImageEnd, image.data() + split, image.size() - split);
        }
    }

    // Fixture writes go to both sides, untraced.
    void w32(u32 address, u32 value) {
        for (Side* s : {&original, &native}) s->memory.write32(address, value);
    }
    void w16(u32 address, u16 value) {
        for (Side* s : {&original, &native}) s->memory.write16(address, value);
    }
    void w8(u32 address, u8 value) {
        for (Side* s : {&original, &native}) s->memory.write8(address, value);
    }
    void wvec(u32 address, const Vec3& v) {
        w32(address, v.x);
        w32(address + 4, v.y);
        w32(address + 8, v.z);
    }
    u32 r32(u32 address) const { return original.memory.peek32(address); }

    u32 next_fpscr() {
        static constexpr u32 kStates[] = {0x00000000u, kFpscrNI, kFpscrVE, 0x0001F004u, 0x00008000u};
        return kStates[fpscr_cycle++ % 5u];
    }

    bool in_stack(u32 address) const { return address >= at.stack_low && address < at.stack + 0x100u; }

    struct Original {
        u32 r3;
        u64 f1;
        u32 fpscr;
    };
    Original run_original(u32 entry, u32 r3, u32 r4, u32 fpscr, u64 f1 = 0, u64 f2 = 0) {
        Cpu& cpu = original.cpu;
        cpu.reset();
        cpu.set_msr(cpu.msr() | kMsrFP);
        cpu.pc = entry; cpu.npc = entry + 4; cpu.lr = at.sentinel;
        cpu.gpr[1] = at.stack; cpu.gpr[2] = kR2; cpu.gpr[13] = kR13;
        cpu.gpr[3] = r3; cpu.gpr[4] = r4;
        cpu.fpr[1].bits[0] = cpu.fpr[1].bits[1] = f1;
        cpu.fpr[2].bits[0] = cpu.fpr[2].bits[1] = f2;
        cpu.fpscr = fpscr;
        original.writes.clear();
        original.recording = true;
        unsigned steps = 0;
        while (cpu.pc != at.sentinel && !cpu.stopped() && steps < 200000) {
            cpu.run_steps(1);
            ++steps;
        }
        original.recording = false;
        instructions += steps;
        require(cpu.pc == at.sentinel && !cpu.stopped() && cpu.gpr[1] == at.stack, "original did not return");
        return {cpu.gpr[3], cpu.fpr[1].bits[0], cpu.fpscr};
    }
    Cpu& begin_native(u32 fpscr) {
        native.cpu.reset();
        native.cpu.set_msr(native.cpu.msr() | kMsrFP);
        native.cpu.fpscr = fpscr;
        native.writes.clear();
        native.recording = true;
        return native.cpu;
    }

    void full_compare(const std::string& what) {
        for (const GuestMemory::Region& region : original.memory.regions()) {
            const u8* a = original.memory.peek_pointer(region.base, region.committed);
            const u8* b = native.memory.peek_pointer(region.base, region.committed);
            require(a != nullptr && b != nullptr, "region not readable");
            for (u32 i = 0; i < region.committed; ++i) {
                if (a[i] == b[i]) continue;
                const u32 address = region.base + i;
                if (in_stack(address)) continue;
                char text[96];
                std::snprintf(text, sizeof text, ": guest memory differs at %08X (%02X vs %02X)", address, a[i], b[i]);
                throw std::runtime_error(what + text);
            }
        }
        ++full_compares;
    }

    void finish(const std::string& what, const Original& o, u32 native_r3, u64 native_f1, bool use_f1) {
        native.recording = false;
        if (use_f1) {
            if (o.f1 != native_f1) {
                char text[128];
                std::snprintf(text, sizeof text, ": f1 %016llX vs %016llX", static_cast<unsigned long long>(o.f1),
                              static_cast<unsigned long long>(native_f1));
                throw std::runtime_error(what + text);
            }
        } else if (o.r3 != native_r3) {
            throw std::runtime_error(what + ": r3 " + std::to_string(static_cast<s32>(o.r3)) + " vs " +
                                     std::to_string(static_cast<s32>(native_r3)));
        }
        // FPSCR's control byte is the contract; its status bits are not (docs/NATIVE_MSL_MATH.md).
        if (((o.fpscr ^ native.cpu.fpscr) & 0xFFu) != 0u) {
            char text[96];
            std::snprintf(text, sizeof text, ": FPSCR %08X vs %08X", o.fpscr, native.cpu.fpscr);
            throw std::runtime_error(what + text);
        }
        std::set<u32> native_bytes;
        for (const Extent& e : native.writes)
            for (u32 i = 0; i < e.size; ++i) native_bytes.insert(e.address + i);
        std::set<u32> bytes = native_bytes;
        for (const Extent& e : original.writes)
            for (u32 i = 0; i < e.size; ++i) {
                const u32 address = e.address + i;
                if (in_stack(address)) continue;
                bytes.insert(address);
                original_outside_stack.insert(address);
                if (!native_bytes.count(address)) untouched_by_native.insert(address);
            }
        for (u32 address : bytes) {
            require(!in_stack(address), what + ": the native side wrote the original's stack window");
            if (original.memory.peek8(address) != native.memory.peek8(address)) {
                char text[96];
                std::snprintf(text, sizeof text, ": byte %08X differs (%02X vs %02X)", address,
                              original.memory.peek8(address), native.memory.peek8(address));
                throw std::runtime_error(what + text);
            }
        }
        compared_bytes += bytes.size();
        ++calls;
        if (calls % 512u == 0u) full_compare(what);
    }

    // ---- the five functions -----------------------------------------------------------------------------------

    void listener_distance(const Vec3& p, const std::string& what) {
        wvec(at.vec, p);
        const u32 fpscr = next_fpscr();
        const Original o = run_original(kListenerDistance, at.vec, 0, fpscr);
        Cpu& c = begin_native(fpscr);
        const u64 f1 = se_math::listener_distance(c, at.vec);
        finish("ListenerDistance " + what, o, 0, f1, true);
        ++per_function[0];
    }
    void max_distance(u8 hundreds) {
        w8(at.row + 0xC, hundreds);
        const u32 fpscr = next_fpscr();
        const Original o = run_original(kMaxDistance, at.row, 0, fpscr);
        Cpu& c = begin_native(fpscr);
        const u64 f1 = se_math::max_distance(c, at.row);
        finish("MaxDist " + std::to_string(hundreds), o, 0, f1, true);
        ++per_function[1];
    }
    void pos_volume(const Vec3& p, u32 max_bits, s8 offset, s8 ceiling, const std::string& what) {
        wvec(at.chan + 0x1C, p);
        w32(at.chan + 0x28, max_bits);
        w8(at.chan + 7, static_cast<u8>(offset));
        w8(at.chan + 8, static_cast<u8>(ceiling));
        const u32 fpscr = next_fpscr();
        const Original o = run_original(kPosVolume, at.chan, 0, fpscr);
        Cpu& c = begin_native(fpscr);
        const s8 v = se_math::pos_volume(c, at.chan);
        finish("PosVolume " + what, o, static_cast<u32>(static_cast<s32>(v)), 0, false);
        ++per_function[2];
    }
    void pos_pan(const Vec3& p, bool with_out, const std::string& what) {
        wvec(at.chan + 0x1C, p);
        w8(at.out, 0x5A);  // a sentinel, so a store of 0 shows
        const u32 fpscr = next_fpscr();
        const Original o = run_original(kPosPan, at.chan, with_out ? at.out : 0u, fpscr);
        Cpu& c = begin_native(fpscr);
        s8 surround = static_cast<s8>(native.memory.peek8(at.out));
        const s32 v = se_math::pos_pan(c, at.chan, with_out ? &surround : nullptr);
        if (with_out && surround != static_cast<s8>(native.memory.peek8(at.out)))
            native.memory.write8(at.out, static_cast<u8>(surround));
        finish("PosPan " + what, o, static_cast<u32>(v), 0, false);
        ++per_function[3];
    }
    void doppler(const Vec3& p, s16 bend, u32 previous, bool set_previous, const std::string& what) {
        wvec(at.chan + 0x1C, p);
        w16(at.chan + 0xA, static_cast<u16>(bend));
        if (set_previous) w32(at.chan + 0x2C, previous);
        const u32 fpscr = next_fpscr();
        const Original o = run_original(kDopplerBend, at.chan, 0, fpscr);
        Cpu& c = begin_native(fpscr);
        const s32 v = se_math::doppler_bend(c, at.chan);
        finish("DopplerBend " + what, o, static_cast<u32>(v), 0, false);
        ++per_function[4];
    }

    // One ported routine on double-format inputs: f1 (and f2) in, f1 out.
    template <class Native>
    void routine(u32 entry, const char* name, u64 f1, u64 f2, Native&& call) {
        const u32 fpscr = next_fpscr();
        const Original o = run_original(entry, 0, 0, fpscr, f1, f2);
        Cpu& c = begin_native(fpscr);
        const u64 result = call(c);
        char text[96];
        std::snprintf(text, sizeof text, "%s(%016llX, %016llX)", name, static_cast<unsigned long long>(f1),
                      static_cast<unsigned long long>(f2));
        finish(text, o, 0, result, true);
        ++routine_calls;
    }

    // ---- fixtures ---------------------------------------------------------------------------------------------

    struct Actor {
        u32 id;
        Vec3 position;
        bool has_counts = true;
        u32 use = 1;
    };
    void registry(const std::vector<Actor>& actors) {
        w32(kR13 + 0x2E88, at.registry);
        w32(at.registry + 4, static_cast<u32>(actors.size()));
        w32(at.registry + 8, at.elements);
        for (u32 i = 0; i < actors.size(); ++i) {
            const Actor& a = actors[i];
            const u32 outer = at.outer_counts + 0x10 * i, node = at.nodes + 0x10 * i, weak = at.weak_counts + 0x10 * i,
                      actor = at.actors + 0x400 * i;
            w32(at.elements + 8 * i, node);
            w32(at.elements + 8 * i + 4, outer);
            w32(outer, 0); w32(outer + 4, 1); w32(outer + 8, 1);   // vtable (never called), use, weak
            w32(node, actor);
            w32(node + 4, a.has_counts ? weak : 0u);
            w32(weak, 0); w32(weak + 4, a.use); w32(weak + 8, a.use + 2);
            wvec(actor, a.position);
            w32(actor + 0x298, a.id);
        }
    }
    void camera(const Vec3& eye, const Vec3& target) {
        w32(kR13 + 0x2B34, at.camera);
        w32(at.camera + 8, 1);
        w32(at.camera + 0xC, at.camera_elements);
        w32(at.camera_elements, at.camera_object);
        w32(at.camera_elements + 4, at.camera_counts);
        w32(at.camera_counts, 0); w32(at.camera_counts + 4, 1); w32(at.camera_counts + 8, 1);
        wvec(at.camera_object + 8, eye);
        wvec(at.camera_object + 0x14, target);
        w32(at.camera_object + 0x20, f2u(0.75f));
    }
    void config(u32 mode, bool two_players) {
        w32(kConfig + 0x78, mode);
        w8(kConfig + 0xB2, two_players ? 1 : 0);
    }
    void ids(u32 one, u32 two) {
        w32(kR13 + 0x3130, one);
        w32(kR13 - 0x6AD8, two);
    }
};

Vec3 vec(float x, float y, float z) { return {f2u(x), f2u(y), f2u(z)}; }
Vec3 offset(const Vec3& base, double distance, double azimuth, double elevation) {
    const double c = std::cos(elevation);
    return vec(static_cast<float>(u2f(base.x) + distance * c * std::cos(azimuth)),
               static_cast<float>(u2f(base.y) + distance * std::sin(elevation)),
               static_cast<float>(u2f(base.z) + distance * c * std::sin(azimuth)));
}

struct Random {
    u64 state = 0x9E3779B97F4A7C15ull;
    u32 next() {
        state ^= state << 13; state ^= state >> 7; state ^= state << 17;
        return static_cast<u32>(state >> 16);
    }
    double unit() { return next() / 4294967296.0; }
};

constexpr double kPi = 3.14159265358979323846;
const float kNaN = std::numeric_limits<float>::quiet_NaN();
const float kInf = std::numeric_limits<float>::infinity();

// Distances that sit on, just inside and just outside every threshold the math tests.
std::vector<double> distances() {
    return {0.0, 1e-30, 1e-6, 1e-3, 0.5, 1.0, 10.0, 49.99, 49.9999, 50.0, 50.0001, 50.01, 59.99, 59.9999, 60.0,
            60.0001, 60.01, 99.9, 100.0, 100.01, 250.0, 399.99, 400.0, 400.01, 799.9, 800.0, 800.01, 1000.0,
            25499.0, 25500.0, 25501.0, 1e5, 1e7, 1e19, 3e19};
}

// Every listener configuration crossed with positions, running each function on each.
void synthetic(Harness& h) {
    for (u32 v = 0; v < 256; ++v) h.max_distance(static_cast<u8>(v));
    for (u32 v = 0; v < 256; ++v) h.max_distance(static_cast<u8>(v));

    const Vec3 listeners[] = {vec(0, 0, 0), vec(12.5f, -3.25f, 100.0f), vec(-19.609758f, -160.77798f, -1480.8339f)};
    const Vec3 second = vec(300.0f, 10.0f, -200.0f);
    struct Configuration {
        const char* name;
        bool two;
        int kind;  // 0 alive, 1 missing, 2 expired, 3 no count block, 4 behind decoys, 5 empty registry,
                   // 6 2P both alive, 7 2P p2 missing, 8 2P p1 missing, 9 2P both missing, 10 2P p1 expired
    };
    const Configuration configurations[] = {
        {"1P alive", false, 0}, {"1P missing", false, 1}, {"1P expired", false, 2}, {"1P no counts", false, 3},
        {"1P decoys", false, 4}, {"1P empty registry", false, 5}, {"2P both", true, 6}, {"2P p2 missing", true, 7},
        {"2P p1 missing", true, 8}, {"2P none", true, 9}, {"2P p1 expired", true, 10}};
    const Vec3 eyes[] = {vec(0, 50, -100), vec(-12.5f, -130.0f, -1436.5f), vec(5, 5, 5), vec(0, 0, 0)};
    const Vec3 targets[] = {vec(0, 0, 0), vec(-1152.41f, -3636.43f, -8544.0f), vec(5, 5, 5),     // degenerate
                            vec(0, 100, 0)};                                                       // straight up
    const s8 offsets[] = {0, -128, -127, -1, 1, 30, 100, 127};
    Random random;
    u64 position_index = 0;
    for (const Configuration& cfg : configurations)
        for (const Vec3& listener : listeners) {
            const u32 id1 = 0x10u + position_index % 7u, id2 = 0x20u;
            h.ids(id1, id2);
            std::vector<Harness::Actor> actors;
            switch (cfg.kind) {
                case 0: case 6: actors = {{id1, listener}, {id2, second}}; break;
                case 1: case 8: actors = {{0x99u, listener}, {id2, second}}; break;
                case 2: actors = {{id1, listener, true, 0}}; break;
                case 3: actors = {{id1, listener, false}}; break;
                case 4: actors = {{0x77u, second}, {0x78u, second, false}, {0x79u, second, true, 0}, {id1, listener},
                                  {id1, second}}; break;
                case 5: break;
                case 7: actors = {{id1, listener}}; break;
                case 9: actors = {{0x99u, listener}}; break;
                case 10: actors = {{id1, listener, true, 0}, {id2, second}}; break;
            }
            h.registry(actors);
            for (double distance : distances())
                for (int direction = 0; direction < 9; ++direction) {
                    const u64 n = position_index++;
                    double azimuth, elevation;
                    if (direction < 4) { azimuth = direction * kPi / 2; elevation = 0; }      // exact axis distances
                    else if (direction == 4) { azimuth = 0; elevation = kPi / 2; }
                    else { azimuth = random.unit() * 2 * kPi; elevation = (random.unit() - 0.5) * kPi; }
                    const Vec3 p = offset(listener, distance, azimuth, elevation);
                    const std::string what = std::string(cfg.name) + " d " + std::to_string(distance) + " dir " +
                                             std::to_string(direction);
                    h.config(n % 5u == 0u ? 2u : 0u, cfg.two);
                    h.listener_distance(p, what);
                    // Volume: the max distances MaxDist produces, and odd ones (below 60, NaN), every mode.
                    const float maxima[] = {400.0f, 100.0f, 1000.0f, 25500.0f, 60.0f, 50.0f, 0.0f, kNaN};
                    for (u32 mode : {0u, 1u, 2u}) {
                        h.config(mode, cfg.two);
                        const float max = maxima[(n + mode) % 8u];
                        h.pos_volume(p, f2u(max), offsets[n % 8u], offsets[(n / 8u + mode) % 8u],
                                     what + " mode " + std::to_string(mode));
                        h.pos_volume(p, f2u(400.0f), 0, 0, what + " shipped mode " + std::to_string(mode));
                    }
                    // Pan: camera variants, modes, out pointer.
                    h.camera(eyes[n % 4u], targets[(n / 4u) % 4u]);
                    for (u32 mode : {0u, 1u, 2u}) {
                        h.config(mode, cfg.two);
                        h.pos_pan(p, (n + mode) % 3u != 0u, what + " mode " + std::to_string(mode));
                    }
                    // Doppler: zero, equal, near and far previous distances, any bend.
                    h.config(0u, cfg.two);
                    const s16 bends[] = {0, 1, -1, 6144, -6144, 32767, -32768, 1234};
                    const float previous[] = {0.0f, static_cast<float>(distance), static_cast<float>(distance) + 0.5f,
                                              static_cast<float>(distance) - 400.0f, 1000.0f, kNaN};
                    h.doppler(p, bends[n % 8u], f2u(previous[n % 6u]), true, what);
                    h.doppler(p, bends[(n + 3) % 8u], 0, false, what + " repeat");  // the stored distance
                }
        }

    // Pan sweep: every angle around the camera, at the 50 boundary and beyond, stereo and surround.
    h.config(0u, false);
    h.ids(0x10u, 0x20u);
    h.registry({{0x10u, vec(0, 0, 0)}});
    const Vec3 sweep_eyes[] = {vec(0, 50, -100), vec(1000.5f, -20, 333.25f)};
    const Vec3 sweep_targets[] = {vec(0, 0, 0), vec(1001.0f, -20, 330.0f)};
    for (u32 cam = 0; cam < 2; ++cam) {
        h.camera(sweep_eyes[cam], sweep_targets[cam]);
        for (double distance : {0.0, 1e-4, 1.0, 25.0, 49.999, 50.0, 50.001, 100.0, 1e3, 1e5})
            for (int degree = 0; degree < 360; degree += 3) {
                const Vec3 p = offset(sweep_eyes[cam], distance, degree * kPi / 180.0, (degree % 7) * 0.1);
                for (u32 mode : {0u, 2u}) {
                    h.config(mode, false);
                    h.pos_pan(p, true, "sweep cam " + std::to_string(cam) + " d " + std::to_string(distance) +
                                           " deg " + std::to_string(degree) + " mode " + std::to_string(mode));
                }
            }
    }
    // Degenerate and non-finite camera and channel values.
    h.config(0u, false);
    const Vec3 odd[] = {vec(kNaN, 0, 0), vec(kInf, 0, 0), vec(0, 0, -kInf), vec(1e-40f, 0, 1e-40f),
                        vec(-0.0f, 0, -0.0f), vec(3e38f, 0, 3e38f), vec(0, 0, 1e-7f)};
    for (const Vec3& eye : odd)
        for (const Vec3& p : odd) {
            h.camera(eye, vec(1, 2, 3));
            h.pos_pan(p, true, "odd camera/channel");
            h.camera(vec(1, 2, 3), eye);
            h.pos_pan(p, true, "odd target/channel");
            h.listener_distance(p, "odd channel");
            h.pos_volume(p, f2u(400.0f), 0, 0, "odd channel");
            h.doppler(p, 5, f2u(10.0f), true, "odd channel");
        }
    // Doppler sequences: a channel moving towards and away from the listener, frame by frame.
    h.registry({{0x10u, vec(0, 0, 0)}});
    for (double speed : {0.0, 0.01, 1.0, 10.0, 50.0, 400.0, 900.0}) {
        h.w32(h.at.chan + 0x2C, 0u);
        for (int frame = 0; frame < 40; ++frame) {
            const double d = 900.0 - speed * frame;
            h.doppler(offset(vec(0, 0, 0), std::fabs(d), 0.3, 0.1), static_cast<s16>(frame * 97 - 1000), 0, false,
                      "sequence speed " + std::to_string(speed) + " frame " + std::to_string(frame));
        }
    }
}

u64 d2u(double value) {
    u64 bits;
    std::memcpy(&bits, &value, 8);
    return bits;
}

// MSL atan2 / sin / atan and the sqrt directly: random doubles over every branch's range, every threshold word,
// non-finite and denormal inputs. sin's inputs stay below 2^19 pi/2 (its __kernel_rem_pio2 path is not ported).
void routines(Harness& h) {
    Random random;
    std::vector<u64> values = {d2u(0.0), d2u(-0.0), d2u(1.0), d2u(-1.0), d2u(0.5), d2u(2.0), 0x7FF0000000000000ull,
                               0xFFF0000000000000ull, 0x7FF8000000000000ull, 0xFFF8000000000001ull,
                               0x7FF0000000000001ull, 0x0000000000000001ull, 0x800FFFFFFFFFFFFFull,
                               0x0010000000000000ull, d2u(1e-300), d2u(1e300), d2u(3.14159265358979), d2u(16384.0)};
    // The branch thresholds (high words) of atan, atan2, rem_pio2, sin, cos, each just below, at and above.
    for (u32 high : {0x3E200000u, 0x3E400000u, 0x3FD33333u, 0x3FDC0000u, 0x3FE60000u, 0x3FE90000u, 0x3FE921FBu,
                     0x3FF00000u, 0x3FF30000u, 0x3FF921FBu, 0x40038000u, 0x4002D97Cu, 0x40100000u, 0x413921FBu,
                     0x44100000u, 0x7FEFFFFFu})
        for (u32 delta : {0xFFFFFFFFu, 0u, 1u})
            for (u32 low : {0u, 0x54442D18u, 0xFFFFFFFFu}) {
                const u64 bits = (u64(high + delta) << 32) | low;
                values.push_back(bits);
                values.push_back(bits | 0x8000000000000000ull);
            }
    // Near multiples of pi/2 (rem_pio2's refinement steps) and random magnitudes, as doubles and as singles.
    for (int k = 1; k < 400; ++k) {
        const double m = k * 1.5707963267948966;
        values.push_back(d2u(m));
        values.push_back(d2u(m) + 1);
        values.push_back(d2u(m) - 1);
        values.push_back(d2u(static_cast<double>(static_cast<float>(m))));
    }
    for (int i = 0; i < 6000; ++i) {
        const double magnitude = std::ldexp(random.unit() + 0.5, static_cast<int>(random.next() % 90) - 60);
        const double v = (random.next() & 1) ? -magnitude : magnitude;
        values.push_back(d2u(v));
        values.push_back(d2u(static_cast<double>(static_cast<float>(v))));
    }
    for (u64 x : values) {
        h.routine(kSin, "sin", x, 0, [&](Cpu& c) { return se_math::detail::msl_sin(c, x); });
        h.routine(kAtan, "atan", x, 0, [&](Cpu& c) { return se_math::detail::msl_atan(c, x); });
        // The sqrt sees single-precision sums (fadds); give it singles, and the raw double as well.
        const u64 single = single_to_double_bits(double_to_single_bits(x));
        h.routine(kSqrt, "sqrt", single, 0, [&](Cpu& c) { return se_math::detail::guest_sqrt(c, single); });
    }
    // sqrt takes the host root for positive finite singles: sweep that whole range (every 65521st single, the
    // denormals' edges, the largest finite and +infinity) against the original routine.
    for (u64 word = 1; word < 0x7F800001ull; word += 65521u) {
        const u64 single = single_to_double_bits(static_cast<u32>(word));
        h.routine(kSqrt, "sqrt", single, 0, [&](Cpu& c) { return se_math::detail::guest_sqrt(c, single); });
    }
    for (u32 word : {0x00000001u, 0x00000002u, 0x007FFFFFu, 0x00800000u, 0x00800001u, 0x3F800000u, 0x3F800001u,
                     0x7F7FFFFFu, 0x7F800000u, 0x80000000u, 0x00000000u, 0xBF800000u}) {
        const u64 single = single_to_double_bits(word);
        h.routine(kSqrt, "sqrt", single, 0, [&](Cpu& c) { return se_math::detail::guest_sqrt(c, single); });
    }
    for (std::size_t i = 0; i < values.size(); ++i) {
        const u64 y = values[i], x = values[(i * 7919u + 13u) % values.size()];
        h.routine(kAtan2, "atan2", y, x, [&](Cpu& c) { return se_math::detail::msl_atan2(c, y, x); });
        h.routine(kAtan2, "atan2", x, y, [&](Cpu& c) { return se_math::detail::msl_atan2(c, x, y); });
    }
    for (u64 y : {d2u(0.0), d2u(-0.0), 0x7FF0000000000000ull, 0xFFF0000000000000ull, d2u(1.0), d2u(-3.0)})
        for (u64 x : {d2u(0.0), d2u(-0.0), 0x7FF0000000000000ull, 0xFFF0000000000000ull, d2u(1.0), d2u(-3.0),
                      d2u(1e-200), d2u(-1e200)})
            h.routine(kAtan2, "atan2", y, x, [&](Cpu& c) { return se_math::detail::msl_atan2(c, y, x); });
}

// The running game's memory: sweep positions around the real listener and camera.
void real_image(Harness& h) {
    const u32 registry = h.r32(kR13 + 0x2E88);
    require(registry != 0u && h.r32(kR13 + 0x2B34) != 0u, "the image has no actor registry or camera manager");
    const u32 count = h.r32(registry + 4), data = h.r32(registry + 8);
    const u32 id1 = h.r32(kR13 + 0x3130);
    Vec3 listener = vec(0, 0, 0);
    bool found = false;
    for (u32 i = 0; i < count && !found; ++i) {
        const u32 actor = h.r32(h.r32(data + 8 * i));
        if (h.r32(actor + 0x298) == id1) {
            listener = {h.r32(actor), h.r32(actor + 4), h.r32(actor + 8)};
            found = true;
        }
    }
    require(found, "the image's player 1 is not in the registry");
    std::printf("image: registry %08X (%u actors), player 1 id %u at (%g, %g, %g), mode %u, two players %u\n", registry,
                count, id1, u2f(listener.x), u2f(listener.y), u2f(listener.z), h.r32(kConfig + 0x78),
                h.original.memory.peek8(kConfig + 0xB2));
    const u32 mode = h.r32(kConfig + 0x78);
    const u8 two = h.original.memory.peek8(kConfig + 0xB2);
    Random random;
    for (double distance : distances())
        for (int k = 0; k < 24; ++k) {
            const double azimuth = k * kPi / 12.0 + 0.01, elevation = ((k % 5) - 2) * 0.2;
            const Vec3 p = offset(listener, distance, azimuth, elevation);
            const std::string what = "image d " + std::to_string(distance) + " k " + std::to_string(k);
            h.listener_distance(p, what);
            h.pos_volume(p, f2u(400.0f), 0, 0, what);
            h.pos_volume(p, f2u(k % 2 ? 1000.0f : 100.0f), 0, 0, what);
            h.pos_pan(p, true, what);
            h.pos_pan(p, false, what);
            h.doppler(p, static_cast<s16>(k * 300 - 3000), f2u(static_cast<float>(distance) + 3.0f), k % 3 == 0, what);
        }
    // The same with the other output modes and the two-player flag set (player 2 is not in the registry).
    for (u32 m : {1u, 2u})
        for (u8 t : {u8{0}, u8{1}}) {
            h.w32(kConfig + 0x78, m);
            h.w8(kConfig + 0xB2, t);
            for (int k = 0; k < 120; ++k) {
                const Vec3 p = offset(listener, random.unit() * 900.0, random.unit() * 2 * kPi, random.unit() - 0.5);
                const std::string what = "image mode " + std::to_string(m) + " 2P " + std::to_string(t);
                h.listener_distance(p, what);
                h.pos_volume(p, f2u(400.0f), 0, 0, what);
                h.pos_pan(p, true, what);
            }
        }
    h.w32(kConfig + 0x78, mode);
    h.w8(kConfig + 0xB2, two);
}

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc >= 2, "usage: se_math_contract_tests <main.dol> [<MEM1 image>]");
        const std::vector<u8> dol = read_file(argv[1]);
        u64 hash = 0xCBF29CE484222325ull;
        for (u8 byte : dol) hash = (hash ^ byte) * 0x100000001B3ull;
        // SHA-256: fde4fa6f81a60313b710161c196dc51c2260be62251ee02775d5eee06f9d55af.
        require(hash == 0xD2CFB4A1CA897F9Bull, "unexpected original DOL identity");

        u64 total_calls = 0, total_instructions = 0;
        {
            Harness h(0x81000000u);
            h.load_dol(dol, false);
            // The compiled-in constants are main.dol's.
            std::size_t count = 0;
            const se_math::DolConstant* constants = se_math::dol_constants(count);
            for (std::size_t i = 0; i < count; ++i) {
                const se_math::DolConstant& k = constants[i];
                const u64 value = k.size == 8 ? (u64(h.r32(k.address)) << 32) | h.r32(k.address + 4) : h.r32(k.address);
                char text[96];
                std::snprintf(text, sizeof text, "constant at %08X differs from main.dol", k.address);
                require(k.address != 0u && value == k.value, text);
            }
            std::printf("constants: %zu compiled-in values equal main.dol\n", count);
            h.full_compare("start");
            synthetic(h);
            const u64 before = h.calls, before_instructions = h.instructions;
            routines(h);
            std::printf("routines: %llu direct calls of atan2 / sin / atan / sqrt bit-identical (double results) to "
                        "%llu original instructions\n",
                        static_cast<unsigned long long>(h.calls - before),
                        static_cast<unsigned long long>(h.instructions - before_instructions));
            h.full_compare("end");
            std::printf("synthetic: %llu calls (ListenerDistance %llu, MaxDist %llu, PosVolume %llu, PosPan %llu, "
                        "DopplerBend %llu) identical to %llu original instructions; %llu full-memory compares; "
                        "%zu non-stack bytes the original wrote, %zu of them only by the original (net-zero "
                        "reference counts)\n",
                        static_cast<unsigned long long>(h.calls), static_cast<unsigned long long>(h.per_function[0]),
                        static_cast<unsigned long long>(h.per_function[1]),
                        static_cast<unsigned long long>(h.per_function[2]),
                        static_cast<unsigned long long>(h.per_function[3]),
                        static_cast<unsigned long long>(h.per_function[4]),
                        static_cast<unsigned long long>(h.instructions),
                        static_cast<unsigned long long>(h.full_compares), h.original_outside_stack.size(),
                        h.untouched_by_native.size());
            total_calls += h.calls;
            total_instructions += h.instructions;
        }
        if (argc >= 3) {
            const std::vector<u8> image = read_file(argv[2]);
            // A 64 KiB zero window in the image's heap for the channel, the stack and the sentinel.
            u32 base = 0;
            for (u32 candidate = 0x817F0000u; candidate >= 0x80700000u && base == 0; candidate -= 0x10000u) {
                const u8* p = image.data() + (candidate - 0x80000000u);
                if (std::all_of(p, p + 0x10000, [](u8 b) { return b == 0; })) base = candidate;
            }
            require(base != 0u, "no free window in the image");
            Harness h(base);
            h.load_image(image);
            // main.dol's text over the image: the original instructions are the DOL's.
            u64 differing = 0;
            {
                const auto word = [&](unsigned offset) { return load_be32(dol.data() + offset); };
                for (unsigned i = 0; i < 7; ++i) {
                    const u32 offset = word(i * 4), address = word(0x48 + i * 4), size = word(0x90 + i * 4);
                    for (u32 j = 0; j < size; ++j) differing += image[address - 0x80000000u + j] != dol[offset + j];
                }
            }
            h.load_dol(dol, true);
            std::printf("image: scratch window %08X; %llu text bytes differed from main.dol (replaced)\n", base,
                        static_cast<unsigned long long>(differing));
            h.full_compare("image start");
            real_image(h);
            h.full_compare("image end");
            std::printf("image: %llu calls identical to %llu original instructions; %llu full-memory compares; %zu "
                        "non-stack bytes the original wrote, %zu only by the original (net-zero reference counts)\n",
                        static_cast<unsigned long long>(h.calls), static_cast<unsigned long long>(h.instructions),
                        static_cast<unsigned long long>(h.full_compares), h.original_outside_stack.size(),
                        h.untouched_by_native.size());
            total_calls += h.calls;
            total_instructions += h.instructions;
        }
        std::printf("PASS native SE math: %llu calls identical (result bits, FPSCR, guest memory) to %llu original "
                    "instructions\n",
                    static_cast<unsigned long long>(total_calls), static_cast<unsigned long long>(total_instructions));
        return 0;
    } catch (const std::exception& error) {
        std::printf("FAIL: %s\n", error.what());
        return 1;
    }
}
