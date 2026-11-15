// liveness-roots: reference-interpreter-only
// Original-DOL contract for the owned Mtn track sampler and Hermite interpolators (gekko/native_mtn_track.h): every instruction
// comes from the pinned DOL and runs in the reference interpreter; the native owner runs on a second machine that has the same
// DOL and the same memory. The sampler is driven over random track states (key index and count at their bounds, flags, cached
// value, keys A and B, a packed key stream of structured and random half words, the context's time and interpolator pointer
// including a pointer the owner does not serve, so the guest-call path runs too) in both FPSCR[NI] modes with float classes
// (zeros, denormals, infinities, quiet and signalling NaNs, tiny and huge values, values on the eps window edges). Compared: the
// whole arena (state, keys, stream), the sampling context, f1, r1 and the FPSCR except FPCC (the last compare's result code, which
// nothing reads). Negative controls: a native with the wrong FPSCR[NI] mode, with a one-ulp-off result, and with a stale flag
// store must be caught by the same comparison. Fixture addresses are outside the DOL's text.
#include "gekko/cpu.h"
#include "gekko/gekko_estimate.h"
#include "gekko/guest_memory.h"
#include "gekko/native_mtn_track.h"
#include "gekko/ppc_ops.h"

#include <immintrin.h>

#include <bit>
#include <cstdio>
#include <set>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace gekko;
constexpr u32 kReturn = 0x817FF000u, kStack = 0x817E0000u, kR2 = 0x805FA780u, kR13 = 0x805EC500u;
constexpr u32 kArena = 0x81400000u, kArenaSize = 0x400u;
constexpr u32 kState = kArena + 0x100u, kStream = kArena + 0x200u;
constexpr u32 kCtx = mtn_track::kContext;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::set<u32> g_visited;  // original instruction addresses executed by the original runs (coverage report)

struct Machine {
    GuestMemory memory;
    Cpu cpu{memory};
    explicit Machine(const std::vector<u8>& dol) {
        const auto word = [&](unsigned offset) { return load_be32(dol.data() + offset); };
        for (unsigned i = 0; i < 18; ++i) {
            const u32 offset = word(i * 4), address = word(0x48 + i * 4), size = word(0x90 + i * 4);
            require(u64(offset) + size <= dol.size(), "invalid DOL section");
            if (size) memory.write_block(address, dol.data() + offset, size);
        }
    }
    void prepare(u32 entry, bool ni) {
        cpu.reset();
        cpu.set_msr(cpu.msr() | kMsrFP);
        cpu.fpscr = ni ? kFpscrNI : 0u;
        cpu.pc = entry; cpu.npc = entry + 4; cpu.lr = kReturn;
        cpu.gpr[1] = kStack; cpu.gpr[2] = kR2; cpu.gpr[13] = kR13;
    }
    void run_original() {
        unsigned steps = 0;
        while (cpu.pc != kReturn && !cpu.stopped() && steps++ < 200000) {
            g_visited.insert(cpu.pc);
            cpu.run_steps(1);
        }
        require(cpu.pc == kReturn && !cpu.stopped() && cpu.gpr[1] == kStack, "original call failed return/stack contract");
    }
};

u32 g_seed = 0x5EEDB00Bu;
u32 rnd() { g_seed = g_seed * 1664525u + 1013904223u; return g_seed >> 8 ^ (g_seed << 16); }
u32 pick(u32 n) { return rnd() % n; }
u32 f2w(float f) { u32 w; std::memcpy(&w, &f, 4); return w; }
float w2f(u32 w) { float f; std::memcpy(&f, &w, 4); return f; }

u32 float_word() {
    switch (pick(14)) {
        case 0: return 0u;
        case 1: return 0x80000000u;
        case 2: return (pick(2) << 31) | (1u + pick(0x7FFFFFu));                       // denormal
        case 3: return (pick(2) << 31) | 0x00800000u | pick(4);                         // just above the boundary
        case 4: return (pick(2) << 31) | 0x7F800000u;                                   // infinity
        case 5: return (pick(2) << 31) | 0x7F800000u | (1u + pick(0x7FFFFFu));          // NaN, any payload
        case 6: return (pick(2) << 31) | (0x7F7FFFFFu - pick(4));                       // huge
        case 7: return (pick(2) << 31) | ((0x3F800000u + (pick(9) << 23)) & 0x7FFFFFFFu) | pick(2);  // around 1..256
        case 8: return (pick(2) << 31) | ((0x20u + pick(0x20u)) << 23) | pick(0x800000u);  // tiny normals
        default: return rnd();
    }
}
// A plausible animation value: a decoded half word, a small float, or any class.
u32 value_word() {
    switch (pick(7)) {
        case 0: return float_word();
        case 6: return (pick(2) << 31) | ((20u + pick(40u)) << 23) | pick(0x800000u);  // 2^-107 .. 2^-68: around the fast path's tiny bound
        case 1: return 0u;
        case 2: return f2w(float(int(pick(41)) - 20) * 0.25f);
        default: {
            const u32 h = pick(4) == 0 ? 0u : (rnd() & 0xFFFFu);
            return h == 0u ? 0u : ((h & 0x8000u) << 16) | (((h & 0x7FFFu) << 13) + 0x38000000u);
        }
    }
}
u32 time_word() {
    switch (pick(8)) {
        case 0: return float_word();
        case 1: return f2w(float(pick(14)));
        case 2: return f2w(float(pick(14)) + 0.5f);
        default: return f2w(float(pick(12)) + float(pick(64)) * (1.0f / 64.0f));
    }
}
const float kEps = w2f(0x38D1B717u);

void fill(Machine& a, Machine& b, u32 at, u32 word) {
    a.memory.write32(at, word);
    b.memory.write32(at, word);
}
void fill16(Machine& a, Machine& b, u32 at, u32 half) {
    a.memory.write16(at, static_cast<u16>(half));
    b.memory.write16(at, static_cast<u16>(half));
}

int g_failures = 0, g_checked = 0;
void report(const char* what, const std::string& detail) {
    ++g_failures;
    if (g_failures < 24) std::fprintf(stderr, "FAIL %s: %s\n", what, detail.c_str());
}

bool same_state(Machine& o, Machine& n, const char* name, bool ni, std::string& detail, bool compare_f1 = true) {
    for (u32 off = 0; off < kArenaSize; off += 4u)
        if (o.memory.peek32(kArena + off) != n.memory.peek32(kArena + off)) {
            char t[200];
            std::snprintf(t, sizeof t, "%s ni=%d at arena+0x%03X: original %08X native %08X", name, ni, off,
                          o.memory.peek32(kArena + off), n.memory.peek32(kArena + off));
            detail = t;
            return false;
        }
    for (u32 off = 0; off < 0x20u; off += 4u)
        if (o.memory.peek32(kCtx + off) != n.memory.peek32(kCtx + off)) {
            char t[200];
            std::snprintf(t, sizeof t, "%s ni=%d at ctx+0x%02X: original %08X native %08X", name, ni, off,
                          o.memory.peek32(kCtx + off), n.memory.peek32(kCtx + off));
            detail = t;
            return false;
        }
    if (compare_f1 && o.cpu.fpr[1].bits[0] != n.cpu.fpr[1].bits[0]) {
        char t[200];
        std::snprintf(t, sizeof t, "%s ni=%d f1: original %016llX native %016llX", name, ni,
                      static_cast<unsigned long long>(o.cpu.fpr[1].bits[0]),
                      static_cast<unsigned long long>(n.cpu.fpr[1].bits[0]));
        detail = t;
        return false;
    }
    if ((o.cpu.fpscr & ~kFpscrFPCC) != (n.cpu.fpscr & ~kFpscrFPCC)) {
        char t[200];
        std::snprintf(t, sizeof t, "%s ni=%d FPSCR: original %08X native %08X", name, ni, o.cpu.fpscr, n.cpu.fpscr);
        detail = t;
        return false;
    }
    if (o.cpu.gpr[1] != n.cpu.gpr[1] || o.cpu.gpr[2] != n.cpu.gpr[2] || o.cpu.gpr[13] != n.cpu.gpr[13]) {
        detail = std::string(name) + ": r1 / r2 / r13 differ";
        return false;
    }
    return true;
}

// ---- track states ---------------------------------------------------------------------------------------------------------------
// One random track state at `st` (its stream pointer into the shared packed stream at kStream).
void randomize_track(Machine& o, Machine& n, u32 st, u32 record) {
    // the keys
    const u32 a_time = time_word();
    const u32 b_time = pick(3) == 0 ? time_word() : f2w(w2f(a_time) + float(pick(5)));
    fill(o, n, st + 0x20u, a_time);
    fill(o, n, st + 0x24u, value_word());
    fill(o, n, st + 0x28u, pick(4) == 0 ? 0u : value_word());
    fill(o, n, st + 0x2Cu, value_word());
    fill(o, n, st + 0x30u, b_time);
    fill(o, n, st + 0x34u, pick(4) == 0 ? 0u : value_word());
    fill(o, n, st + 0x38u, pick(4) == 0 ? 0u : value_word());
    fill(o, n, st + 0x3Cu, pick(3) == 0 ? o.memory.peek32(st + 0x2Cu) : value_word());  // equal values -> flat segments
    if (pick(5) == 0) {  // a flat segment: zero tangents, equal values
        const u32 v = value_word();
        fill(o, n, st + 0x28u, 0u);
        fill(o, n, st + 0x34u, 0u);
        fill(o, n, st + 0x2Cu, v);
        fill(o, n, st + 0x3Cu, v);
    }
    fill(o, n, st + 0x10u, value_word());  // cached value
    fill(o, n, st + 0x0Cu, record);
    // index / count / flags / stream pointer
    const s32 count = pick(10) == 0 ? s32(pick(3)) - 1 : s32(pick(7));
    const s32 idx = pick(10) == 0 ? s32(pick(9)) - 1 : (count > 0 ? s32(pick(u32(count) + 1u)) : 0);
    fill16(o, n, st + 0u, u32(idx) & 0xFFFFu);
    fill16(o, n, st + 2u, u32(count) & 0xFFFFu);
    u32 flags = rnd();
    if (pick(3) != 0) flags &= ~1u;         // mostly not finished
    if (pick(2) == 0) flags &= ~8u;
    fill(o, n, st + 4u, flags);
    fill(o, n, st + 8u, kStream + 2u * pick(8));
}

// The shared packed stream: structured keys (rising small times) with random half words between, and the record's first key.
void randomize_stream(Machine& o, Machine& n) {
    u32 cursor = kStream, time = pick(6);
    for (u32 k = 0; k < 16; ++k) {
        time += pick(4);
        const u32 h = (pick(8) == 0 ? (rnd() & 0x3FFFu) : (time & 0x3FFFu)) | (pick(3) == 0 ? 0x8000u : 0u) | (pick(3) == 0 ? 0x4000u : 0u);
        fill16(o, n, cursor, h);
        cursor += 2u;
        for (u32 w = 0; w < 3; ++w) {
            const u32 half = pick(5) == 0 ? 0u : (pick(3) == 0 ? (rnd() & 0xFFFFu) : (0x3C00u + pick(0x800u)) | (pick(2) << 15));
            fill16(o, n, cursor, half);
            cursor += 2u;
        }
    }
}

// The sampling context: time near the key times and the eps window edges, rate, interpolator pointers.
void randomize_context(Machine& o, Machine& n, u32 st) {
    const u32 pointers[] = {0x80416728u, 0x80416728u, 0x80416728u, 0x80416728u, 0x8041669Cu, 0x804167ACu, 0x80416728u};  // 0x804167AC: a guest function the owner does not serve (copies the key into the context)
    fill(o, n, kCtx + 0x10u, 0u);
    fill(o, n, kCtx + 0x14u, pointers[pick(7)]);
    fill(o, n, kCtx + 0x18u, 0x8041669Cu);
    fill(o, n, kCtx + 0x1Cu, 0x80416580u);
    const float bt = w2f(o.memory.peek32(st + 0x30u)), at = w2f(o.memory.peek32(st + 0x20u));
    u32 t;
    switch (pick(10)) {
        case 0: t = float_word(); break;
        case 1: t = f2w(bt + kEps); break;
        case 2: t = f2w(bt - kEps); break;
        case 3: t = f2w(at + kEps); break;
        case 4: t = f2w(at - kEps); break;
        case 5: t = f2w(bt); break;
        case 6: t = f2w(at); break;
        case 7: t = time_word(); break;
        default: t = f2w(at + (bt - at) * (float(pick(1000)) / 1000.0f)); break;
    }
    fill(o, n, kCtx + 0xCu, t);
    fill(o, n, kCtx + 0x0u, o.memory.peek32(st + 0x20u));
    fill(o, n, kCtx + 0x4u, rnd());
    // the rate: mostly forward, often negative, sometimes zero / NaN / any class
    const u32 rate = pick(3) == 0 ? 0xBF800000u | pick(0x1000u) : (pick(8) == 0 ? float_word() : 0x3F800000u);
    fill(o, n, kCtx + 0x8u, rate);
}

// ---- generic runner: one entry, random state, original vs native ------------------------------------------------------------------
enum class Kind { Sample, Seek, Init, Vec3 };
void run_track_case(Machine& o, Machine& n, const char* name, u32 entry, Kind kind, u32 vec_base, u32 tracks_stride_count,
                    const std::function<void(Cpu&)>& native, unsigned calls, bool expect_clean, unsigned* mismatches) {
    constexpr u32 kRecord = kStream - 8u;
    for (unsigned i = 0; i < calls; ++i) {
        const bool ni = (i & 1u) != 0u;
        for (u32 off = 0; off < kArenaSize; off += 4u) fill(o, n, kArena + off, rnd());
        randomize_stream(o, n);
        u32 object = kState;
        if (kind == Kind::Vec3) {
            object = kArena + 0x08u;  // x y z at +0..+0xB; tracks from +vec_base
            for (u32 k = 0; k < tracks_stride_count; ++k) {
                const u32 st = object + vec_base + 0x40u * k;
                randomize_track(o, n, st, kRecord);
                if (pick(4) == 0) fill(o, n, st + 8u, 0u);  // a track without a stream is skipped
            }
            if (vec_base == 0x20u) fill(o, n, object + 0x10u, pick(3) == 0 ? value_word() : f2w(0.5f * float(pick(6))));  // the scale of the last form
            fill(o, n, kR13 + 0x5F78u, pick(6) == 0 ? 0x80416020u : 0x804162ACu);
            randomize_context(o, n, object + vec_base + 0x00u);
        } else {
            randomize_track(o, n, object, kRecord);
            if (kind == Kind::Init) {
                fill(o, n, kRecord, 0x10u + 2u * pick(0x40u));   // the record's byte length
                fill(o, n, kRecord + 4u, (rnd() & 0xFFFF00FFu) | (((pick(17) - 8u) & 0xFFu) << 8));  // byte 6: small, read signed
            }
            randomize_context(o, n, object);
        }
        const u64 f1 = (u64(rnd()) << 32) | rnd();
        o.prepare(entry, ni);
        n.prepare(entry, ni);
        for (Machine* m : {&o, &n}) { m->cpu.gpr[3] = object; m->cpu.fpr[1].bits[0] = f1; }
        try {
            o.run_original();
        } catch (const std::exception& e) {
            char t[400];
            std::snprintf(t, sizeof t, "%s: %s (pc %08X lr %08X r3 %08X count %d idx %d rate %08X time %08X)", name, e.what(), o.cpu.pc,
                          o.cpu.lr, object, int(s16(o.memory.peek16(object + 2))), int(s16(o.memory.peek16(object))),
                          o.memory.peek32(kCtx + 8u), o.memory.peek32(kCtx + 0xCu));
            throw std::runtime_error(t);
        }
        native(n.cpu);
        std::string detail;
        const bool same = same_state(o, n, name, ni, detail, kind == Kind::Sample);  // f1 is scratch for the other entries
        ++g_checked;
        if (!same) {
            if (mismatches) ++*mismatches;
            if (expect_clean) report(name, detail);
        }
    }
}

// ---- the interpolators ----------------------------------------------------------------------------------------------------------
u32 ordinary_value() {  // a decoded half word of moderate size: the fast path's own territory
    const u32 h = 0x3000u + pick(0x1800u);
    return ((pick(2) << 31) | (((h & 0x7FFFu) << 13) + 0x38000000u));
}
void run_hermite_case(Machine& o, Machine& n, const char* name, u32 entry, void (*serve)(Cpu&) noexcept, unsigned calls,
                      bool expect_clean, bool ordinary = false) {
    for (unsigned i = 0; i < calls; ++i) {
        const bool ni = (i & 1u) != 0u;
        const u32 a = kArena + 0x40u, b = kArena + 0x80u;
        for (u32 off = 0; off < kArenaSize; off += 4u) fill(o, n, kArena + off, rnd());
        for (u32 k = 0; k < 4; ++k) {
            const u32 aw = k == 0 ? (ordinary ? f2w(float(pick(14)) + float(pick(8)) * 0.125f) : time_word()) : (ordinary ? ordinary_value() : value_word());
            const u32 bw = k == 0 ? (ordinary ? f2w(float(pick(14)) + float(pick(8)) * 0.125f) : time_word()) : (ordinary ? ordinary_value() : value_word());
            fill(o, n, a + 4u * k, aw);
            fill(o, n, b + 4u * k, bw);
        }
        if (pick(3) != 0) fill(o, n, b, f2w(w2f(o.memory.peek32(a)) + float(1 + pick(6))));
        const u32 tw = (!ordinary && pick(3) == 0) ? float_word() : f2w(w2f(o.memory.peek32(a)) + float(pick(1000)) * 0.004f);
        o.prepare(entry, ni);
        n.prepare(entry, ni);
        for (Machine* m : {&o, &n}) {
            m->cpu.gpr[3] = a; m->cpu.gpr[4] = b;
            m->cpu.fpr[1].bits[0] = single_to_double_bits(tw);
        }
        o.run_original();
        serve(n.cpu);
        std::string detail;
        const bool same = same_state(o, n, name, ni, detail);
        ++g_checked;
        if (!same && expect_clean) report(name, detail);
    }
}

std::vector<u8> read_file(const char* path) {
    std::ifstream input(path, std::ios::binary);
    return std::vector<u8>{std::istreambuf_iterator<char>(input), {}};
}
}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: mtn_track_contract_tests main.dol");
        const std::vector<u8> dol = read_file(argv[1]);
        require(dol.size() >= 256, "missing DOL");
        u64 hash = 0xCBF29CE484222325ull;
        for (u8 byte : dol) hash = (hash ^ byte) * 0x100000001B3ull;
        require(hash == 0xD2CFB4A1CA897F9Bull, "unexpected original DOL identity");
        Machine original(dol), native(dol);

        // Directed: the fused multiply-add the interpolators' fast path uses (one rounding, as _mm_fmadd_ss) against the translated
        // code's ppc::fps_madd on operands whose exact result sits on or next to a single-precision midpoint (the one case the
        // translated fast path hands to its exact slow path).
        {
            Machine& m = original;
            m.prepare(0x80416728u, true);
            unsigned midpoints = 0, differing = 0;
            for (unsigned i = 0; i < 400000; ++i) {
                const u32 ia = 1u + pick(4095u), ic = 1u + pick(4095u);
                const float a = (pick(2) ? -1.0f : 1.0f) * (1.0f + float(ia) * (1.0f / 4096.0f));
                const float c = (1.0f + float(ic) * (1.0f / 4096.0f)) * (pick(4) == 0 ? 0.5f : 1.0f);
                float b;
                switch (pick(4)) {
                    case 0: b = 0.0f; break;
                    case 1: b = -(a * c); break;  // cancels to a tiny remainder
                    case 2: b = w2f(f2w(a * c) ^ (pick(3) == 0 ? 0u : 1u)) * (pick(2) ? 1.0f : -1.0f); break;
                    default: b = float(int(pick(64)) - 32) * (1.0f / 1024.0f); break;
                }
                const u64 want = ppc::fps_madd(m.cpu, single_to_double_bits(f2w(a)), single_to_double_bits(f2w(c)), single_to_double_bits(f2w(b)));
                const double exact = double(a) * double(c) + double(b);
                const u64 eb = static_cast<u64>(std::bit_cast<u64>(exact));
                if ((eb & 0x1FFFFFFFull) == 0x10000000ull) ++midpoints;
                const float got = _mm_cvtss_f32(_mm_fmadd_ss(_mm_set_ss(a), _mm_set_ss(c), _mm_set_ss(b)));
                if (single_to_double_bits(f2w(got)) != want) {
                    ++differing;
                    if (differing < 5) std::fprintf(stderr, "FAIL fused: a %08X c %08X b %08X: fps_madd %016llX fma %08X\n", f2w(a), f2w(c), f2w(b),
                                                    static_cast<unsigned long long>(want), f2w(got));
                }
            }
            require(differing == 0, "the fused fast path differs from ppc::fps_madd");
            require(midpoints > 100, "the directed fused cases did not reach the midpoint case");
            std::printf("fused multiply-add: 400000 directed triples equal ppc::fps_madd (%u of them on a single midpoint)\n", midpoints);
        }
        constexpr unsigned kCalls = 120000;
        run_hermite_case(original, native, "HermiteValue", mtn_track::kHermiteValue, mtn_track::serve_hermite_value, kCalls, true);
        run_hermite_case(original, native, "HermiteSlope", mtn_track::kHermiteSlope, mtn_track::serve_hermite_slope, kCalls, true);
        run_hermite_case(original, native, "HermiteValue(ordinary)", mtn_track::kHermiteValue, mtn_track::serve_hermite_value, kCalls, true, true);
        run_hermite_case(original, native, "HermiteSlope(ordinary)", mtn_track::kHermiteSlope, mtn_track::serve_hermite_slope, kCalls, true, true);
        const auto sample = [](Cpu& cpu) { mtn_track::serve_track_sample(cpu); };
        run_track_case(original, native, "TrackSample", mtn_track::kTrackSample, Kind::Sample, 0, 0, sample, 3 * kCalls, true, nullptr);
        run_track_case(original, native, "TrackSeekForward", mtn_track::kSeekForward, Kind::Seek, 0, 0,
                       [](Cpu& cpu) { mtn_track::serve_seek_forward(cpu); }, kCalls, true, nullptr);
        run_track_case(original, native, "TrackSeekBackward", mtn_track::kSeekBackward, Kind::Seek, 0, 0,
                       [](Cpu& cpu) { mtn_track::serve_seek_backward(cpu); }, kCalls, true, nullptr);
        run_track_case(original, native, "TrackInit", mtn_track::kTrackInit, Kind::Init, 0, 0,
                       [](Cpu& cpu) { mtn_track::serve_track_init(cpu); }, 2 * kCalls, true, nullptr);
        run_track_case(original, native, "SampleVec3Reciprocal", mtn_track::kSampleVec3Reciprocal, Kind::Vec3, 0x10u, 3,
                       [](Cpu& cpu) { mtn_track::serve_sample_vec3_reciprocal(cpu); }, kCalls / 2, true, nullptr);
        run_track_case(original, native, "SampleVec3", mtn_track::kSampleVec3, Kind::Vec3, 0x10u, 3,
                       [](Cpu& cpu) { mtn_track::serve_sample_vec3(cpu); }, kCalls / 2, true, nullptr);
        run_track_case(original, native, "SampleVec3At20", mtn_track::kSampleVec3At20, Kind::Vec3, 0x20u, 3,
                       [](Cpu& cpu) { mtn_track::serve_sample_vec3_at_20(cpu); }, kCalls / 2, true, nullptr);
        run_track_case(original, native, "SampleVec3Scaled", mtn_track::kSampleVec3Scaled, Kind::Vec3, 0x20u, 3,
                       [](Cpu& cpu) { mtn_track::serve_sample_vec3_scaled(cpu); }, kCalls / 2, true, nullptr);

        // Negative controls: the same comparison must catch a wrong native.
        unsigned caught_ni = 0, caught_ulp = 0, caught_flag = 0, caught_seek = 0;
        run_track_case(original, native, "TrackSample", mtn_track::kTrackSample, Kind::Sample, 0, 0, [](Cpu& cpu) {  // the wrong FPSCR[NI] mode
            cpu.fpscr ^= kFpscrNI;
            mtn_track::serve_track_sample(cpu);
        }, 20000, false, &caught_ni);
        require(caught_ni > 0, "negative control: the wrong FPSCR[NI] mode was not caught");
        run_track_case(original, native, "TrackSample", mtn_track::kTrackSample, Kind::Sample, 0, 0, [](Cpu& cpu) {  // a result one ulp off
            mtn_track::serve_track_sample(cpu);
            cpu.fpr[1].bits[0] ^= 1ull << 29;
        }, 20000, false, &caught_ulp);
        require(caught_ulp > 0, "negative control: a one-ulp-off result was not caught");
        run_track_case(original, native, "TrackSample", mtn_track::kTrackSample, Kind::Sample, 0, 0, [](Cpu& cpu) {  // a stale flat flag
            mtn_track::serve_track_sample(cpu);
            cpu.mem_write32(cpu.gpr[3] + 4u, cpu.mem_read32(cpu.gpr[3] + 4u) | 8u);
        }, 20000, false, &caught_flag);
        require(caught_flag > 0, "negative control: a stale flat flag was not caught");
        run_track_case(original, native, "TrackSeekForward", mtn_track::kSeekForward, Kind::Seek, 0, 0, [](Cpu& cpu) {  // an off-by-one stream pointer
            mtn_track::serve_seek_forward(cpu);
            cpu.mem_write32(cpu.gpr[3] + 8u, cpu.mem_read32(cpu.gpr[3] + 8u) + 2u);
        }, 20000, false, &caught_seek);
        require(caught_seek > 0, "negative control: a wrong stream pointer was not caught");

        // Coverage of the original bodies by the random runs: every instruction of each owned function the runs reached.
        struct Range { const char* name; u32 begin, end; };
        const Range ranges[] = {{"TrackSample", 0x804162ACu, 0x80416580u}, {"HermiteSlope", 0x8041669Cu, 0x80416728u},
                                {"HermiteValue", 0x80416728u, 0x804167ACu}, {"TrackSeekForward", 0x80415300u, 0x80415488u},
                                {"TrackSeekBackward", 0x80415170u, 0x80415300u}, {"TrackInit", 0x80414B80u, 0x80414E60u},
                                {"SampleVec3Reciprocal", 0x80421358u, 0x804213FCu}, {"SampleVec3", 0x804213FCu, 0x8042148Cu},
                                {"SampleVec3At20", 0x8042148Cu, 0x80421518u}, {"SampleVec3Scaled", 0x80421518u, 0x804215E0u}};
        for (const Range& r : ranges) {
            unsigned hit = 0, total = 0;
            std::string missed;
            for (u32 a = r.begin; a < r.end; a += 4u) {
                const u32 word = original.memory.peek32(a);
                (void)word;
                ++total;
                if (g_visited.count(a)) ++hit;
                else if (missed.size() < 120) { char t[16]; std::snprintf(t, sizeof t, " %08X", a); missed += t; }
            }
            std::printf("coverage %-22s %u / %u original instructions executed; missed:%s\n", r.name, hit, total, missed.c_str());
        }
        if (g_failures != 0) {
            std::fprintf(stderr, "FAIL mtn track contract: %d mismatches of %d calls\n", g_failures, g_checked);
            return 1;
        }
        std::printf("PASS mtn track contract: %d calls compared (controls caught %u + %u + %u + %u)\n", g_checked, caught_ni,
                    caught_ulp, caught_flag, caught_seek);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL mtn track contract: %s\n", error.what());
        return 1;
    }
}
