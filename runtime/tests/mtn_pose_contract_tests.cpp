// liveness-roots: reference-interpreter-only
// Original-DOL contract for the owned Mtn pose pass (gekko/native_mtn_pose.h): the quaternion chain (0x8042193C, 0x80421BB0,
// 0x80421774, 0x8042167C, 0x804121BC), the four bone-list callbacks (0x8041EC24, 0x8041E5D0, 0x8041E8EC, 0x8041F174), one bone's Euler
// angles (0x80420B9C) and the recursive bone-tree walk (0x804196F0). Every instruction comes from the pinned DOL and runs in the
// reference interpreter on one machine; the owner runs on a second machine with the same DOL and the same memory. Compared after every
// call: the whole arena (matrix stack, argument structures, matrices, vectors), the pass scratch and channel weight tables of the DOL's
// .bss, the words above the entry r1 (the caller's frame), r1 / r2 / r13 and the non-volatile registers, FPSCR except FPCC, and the
// return values (r3 where the original returns 1). The dead frames under r1 are not compared: the owner does not write them (a local
// the original reads back, or reads without having written, lives at the same guest address in the owner). Negative controls at the end.
// Round 4 (docs/agents/s1-mtn.md): the five folded NodeWalk callbacks (0x80420B28, 0x80420D10, 0x8041FA14, 0x804202E0, 0x8041FDDC) are compared
// against their original bodies one by one and inside whole walks with the fold on, with their own negative controls.
#include "gekko/cpu.h"
#include "gekko/gekko_estimate.h"
#include "gekko/guest_memory.h"
#include "gekko/native_mtn_pose.h"
#include "gekko/native_mtn_track.h"
#include "gekko/ppc_ops.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace gekko;
constexpr u32 kReturn = 0x817FF000u, kStack = 0x817E0000u, kR2 = 0x805FA780u, kR13 = 0x805EC500u;
constexpr u32 kArena = 0x81400000u, kArenaSize = 0x5000u;
constexpr u32 kScratchLo = 0x805E2300u, kScratchHi = 0x805E2700u, kIdentity = 0x805E17E0u;
constexpr u32 kStackObject = kArena + 0x000u, kStackMatrices = kArena + 0x100u;
constexpr u32 kArgs = kArena + 0x300u, kList = kArena + 0x360u, kIndices = kArena + 0x380u, kParents = kArena + 0x3A0u,
              kAngles = kArena + 0x400u, kFlagWords = kArena + 0x480u, kBaseMatrix = kArena + 0x500u;
constexpr u32 kGuestFn = 0x81500000u, kGuestPost = 0x81500100u;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
std::set<u32> g_visited;
struct TraceEntry { u32 function, r3, r4, r1, hash; };
u32 state_hash(GuestMemory& m) {
    u32 h = 2166136261u;
    for (u32 a = 0x81400000u; a < 0x81400300u; a += 4u) h = (h ^ m.peek32(a)) * 16777619u;
    for (u32 a = 0x805E2300u; a < 0x805E2700u; a += 4u) h = (h ^ m.peek32(a)) * 16777619u;
    return h;
}
std::vector<TraceEntry> g_original_trace;
bool g_tracing = false;
bool traced_entry(u32 pc) {
    return pc == 0x804196F0u || pc == 0x8041EC24u || pc == 0x8041E5D0u || pc == 0x8041E8ECu || pc == 0x8041F174u || pc == 0x80420B9Cu ||
           pc == kGuestFn || pc == kGuestPost;
}

// Every runtime switch of the owners under test (the fold, the tree and the phases B / C / D) is a process-wide default that the build options
// change (SHADOW_MTN_FOLD / _TREE / _PHASE_B / _PHASE_C / _PHASE_D). The contract must not depend on that default: the guard records the build
// defaults, pins every switch OFF (the original arm never runs a hook, so a chain compares the native body alone), the chains that need a switch
// set it explicitly around themselves, and the destructor restores the defaults on every exit path.
// the interpreter's step budget for one original call; the huge-time set raises it for its terminating cases
unsigned long g_step_budget = 3000000ul;
struct SwitchGuard {
    bool fold = mtn_pose::fold_enabled(), tree = mtn_pose::tree_enabled(), b = mtn_pose::phase_b_enabled(), c = mtn_pose::phase_c_enabled(),
         d = mtn_pose::phase_d_enabled();
    SwitchGuard() { pin_all_off(); std::printf("switch defaults of this build: fold %d tree %d phase B %d C %d D %d (pinned off for the chains)\n", fold, tree, b, c, d); }
    ~SwitchGuard() {
        mtn_pose::set_fold_enabled(fold);
        mtn_pose::set_tree_enabled(tree);
        mtn_pose::set_phase_b_enabled(b);
        mtn_pose::set_phase_c_enabled(c);
        mtn_pose::set_phase_d_enabled(d);
    }
    static void pin_all_off() {
        mtn_pose::set_fold_enabled(false);
        mtn_pose::set_tree_enabled(false);
        mtn_pose::set_phase_b_enabled(false);
        mtn_pose::set_phase_c_enabled(false);
        mtn_pose::set_phase_d_enabled(false);
    }
    SwitchGuard(const SwitchGuard&) = delete;
    SwitchGuard& operator=(const SwitchGuard&) = delete;
};
// Wall time per chain (original interpreter arm and native arm separately; a skipped call still costs its whole step budget).
struct ChainTime {
    double original = 0.0, native = 0.0;
    unsigned calls = 0, skipped = 0;
};
std::map<std::string, ChainTime> g_chain_time;
double seconds_since(std::chrono::steady_clock::time_point t) { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count(); }

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
    bool run_original() {
        unsigned long steps = 0;
        while (cpu.pc != kReturn && !cpu.stopped() && steps++ < g_step_budget) {
            g_visited.insert(cpu.pc);
            if (g_tracing && traced_entry(cpu.pc)) g_original_trace.push_back({cpu.pc, cpu.gpr[3], cpu.pc == 0x804196F0u ? cpu.gpr[4] : 0u, cpu.gpr[1], state_hash(memory)});
            cpu.run_steps(1);
        }
        return cpu.pc == kReturn && !cpu.stopped() && cpu.gpr[1] == kStack;
    }
    // Runs until the return (0), until the loop tests of phase C (0x8041A094 / A0E0 / A228 / A274: the four wrap loops) have been arrived at `cap` times
    // and the next arrival is about to execute (1: frozen), or the step budget ends (2).
    int run_original_frozen(unsigned long long cap) {
        unsigned long steps = 0;
        unsigned long long arrivals = 0;
        while (cpu.pc != kReturn && !cpu.stopped() && steps++ < g_step_budget) {
            const u32 pc = cpu.pc;
            if (pc == 0x8041A094u || pc == 0x8041A0E0u || pc == 0x8041A228u || pc == 0x8041A274u) {
                if (++arrivals > cap) return 1;
            }
            g_visited.insert(pc);
            cpu.run_steps(1);
        }
        return (cpu.pc == kReturn && !cpu.stopped() && cpu.gpr[1] == kStack) ? 0 : 2;
    }
};

u32 g_seed = std::getenv("MTN_POSE_SEED") ? static_cast<u32>(std::strtoul(std::getenv("MTN_POSE_SEED"), nullptr, 0)) : 0xA11CE5EDu;
u32 rnd() { g_seed = g_seed * 1664525u + 1013904223u; return g_seed >> 8 ^ (g_seed << 16); }
u32 pick(u32 n) { return rnd() % n; }
u32 f2w(float f) { u32 w; std::memcpy(&w, &f, 4); return w; }
float w2f(u32 w) { float f; std::memcpy(&f, &w, 4); return f; }
double unit() { return double(rnd() & 0xFFFFFFu) / double(0x1000000); }

u32 float_word() {  // any class
    switch (pick(12)) {
        case 0: return 0u;
        case 1: return 0x80000000u;
        case 2: return (pick(2) << 31) | (1u + pick(0x7FFFFFu));
        case 3: return (pick(2) << 31) | 0x00800000u | pick(4);
        case 4: return (pick(2) << 31) | 0x7F800000u;
        case 5: return (pick(2) << 31) | 0x7F800000u | (1u + pick(0x7FFFFFu));
        case 6: return (pick(2) << 31) | (0x7F7FFFFFu - pick(4));
        case 7: return (pick(2) << 31) | ((0x3F800000u + (pick(9) << 23)) & 0x7FFFFFFFu) | pick(2);
        case 8: return (pick(2) << 31) | ((0x20u + pick(0x20u)) << 23) | pick(0x800000u);
        default: return rnd();
    }
}
u32 sane_word(float scale = 4.0f) { return f2w(float((unit() * 2.0 - 1.0) * scale)); }
u32 value_word() { return pick(12) == 0 ? float_word() : sane_word(); }

void fill(Machine& a, Machine& b, u32 at, u32 word) { a.memory.write32(at, word); b.memory.write32(at, word); }

int g_failures = 0, g_checked = 0, g_skipped = 0;
std::map<std::string, int> g_skipped_by;
void report(const char* what, const std::string& detail) {
    ++g_failures;
    if (g_failures < 30) std::fprintf(stderr, "FAIL %s: %s\n", what, detail.c_str());
}

bool same_state(Machine& o, Machine& n, const char* name, bool ni, std::string& detail, bool compare_r3) {
    const auto word_differs = [&](u32 at, const char* what) {
        if (o.memory.peek32(at) == n.memory.peek32(at)) return false;
        char t[220];
        std::snprintf(t, sizeof t, "%s ni=%d %s at %08X: original %08X native %08X", name, ni, what, at, o.memory.peek32(at), n.memory.peek32(at));
        detail = t;
        return true;
    };
    for (u32 a = kArena; a < kArena + kArenaSize; a += 4u) if (word_differs(a, "arena")) return false;
    for (u32 a = kScratchLo; a < kScratchHi; a += 4u) if (word_differs(a, "scratch")) return false;
    for (u32 a = 0x81420000u; a < 0x81428000u; a += 4u) if (word_differs(a, "clip memory")) return false;
    for (u32 a = kStack; a < kStack + 0x100u; a += 4u) if (a != kStack + 4u && word_differs(a, "caller frame")) return false;  // +4: the callee's link save slot
    if (word_differs(kR13 + 0x5F50u, "stack object pointer")) return false;
    for (u32 a : {kR13 + 0x5F78u, kR13 + 0x5F7Cu, kR13 + 0x5F80u}) if (word_differs(a, "sampler pointer")) return false;
    if ((o.cpu.fpscr & ~kFpscrFPCC) != (n.cpu.fpscr & ~kFpscrFPCC)) {
        char t[200];
        std::snprintf(t, sizeof t, "%s ni=%d FPSCR: original %08X native %08X", name, ni, o.cpu.fpscr, n.cpu.fpscr);
        detail = t;
        return false;
    }
    for (unsigned r : {1u, 2u, 13u, 14u, 15u, 16u, 17u, 18u, 19u, 20u, 21u, 22u, 23u, 24u, 25u, 26u, 27u, 28u, 29u, 30u, 31u})
        if (o.cpu.gpr[r] != n.cpu.gpr[r]) {
            char t[120];
            std::snprintf(t, sizeof t, "%s ni=%d r%u: original %08X native %08X", name, ni, r, o.cpu.gpr[r], n.cpu.gpr[r]);
            detail = t;
            return false;
        }
    for (unsigned r = 14; r < 32; ++r)
        if (o.cpu.fpr[r].bits[0] != n.cpu.fpr[r].bits[0] || o.cpu.fpr[r].bits[1] != n.cpu.fpr[r].bits[1]) {
            char t[120];
            std::snprintf(t, sizeof t, "%s ni=%d f%u differs", name, ni, r);
            detail = t;
            return false;
        }
    if (compare_r3 && o.cpu.gpr[3] != n.cpu.gpr[3]) {
        detail = std::string(name) + ": r3 differs";
        return false;
    }
    return true;
}

// ---- inputs ------------------------------------------------------------------------------------------------------------------------
struct Quatd { double x, y, z, w; };
Quatd random_quat() {
    Quatd q{unit() * 2 - 1, unit() * 2 - 1, unit() * 2 - 1, unit() * 2 - 1};
    if (pick(4) == 0) {  // a dominant axis, to reach every branch
        double* c[4] = {&q.x, &q.y, &q.z, &q.w};
        *c[pick(4)] *= 4.0;
    }
    const double len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (len < 1e-3) return {0, 0, 0, 1};
    return {q.x / len, q.y / len, q.z / len, q.w / len};
}
void put_quat(Machine& o, Machine& n, u32 at, bool exotic) {
    const Quatd q = random_quat();
    const float v[4] = {float(q.x), float(q.y), float(q.z), float(q.w)};
    for (unsigned i = 0; i < 4; ++i) fill(o, n, at + 4u * i, exotic && pick(6) == 0 ? float_word() : f2w(v[i]));
}
// A rotation matrix (rows 0 / 0x10 / 0x20, translation in column 3), with a little noise and the odd exotic element.
void put_matrix(Machine& o, Machine& n, u32 at, bool exotic) {
    const Quatd q = random_quat();
    const double x = q.x, y = q.y, z = q.z, w = q.w;
    const double m[12] = {1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y), (unit() - 0.5) * 20,
                          2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x), (unit() - 0.5) * 20,
                          2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y), (unit() - 0.5) * 20};
    const double scale = pick(4) == 0 ? 0.5 + unit() * 2 : 1.0;
    for (unsigned i = 0; i < 12; ++i) {
        float f = float(m[i] * ((i & 3u) == 3u ? 1.0 : scale));
        if (pick(8) == 0) f += float((unit() - 0.5) * 0.02);
        u32 word = f2w(f);
        if (exotic && pick(16) == 0) word = float_word();
        fill(o, n, at + 4u * i, word);
    }
}
void put_float_matrix(Machine& o, Machine& n, u32 at) {  // columns of any length, some tiny / zero
    for (unsigned i = 0; i < 12; ++i) {
        u32 w;
        switch (pick(8)) {
            case 0: w = 0u; break;
            case 1: w = float_word(); break;
            case 2: w = f2w(float(unit()) * 1e-4f); break;
            default: w = sane_word(2.0f); break;
        }
        fill(o, n, at + 4u * i, w);
    }
}
void put_identity(Machine& o, Machine& n) {
    for (unsigned i = 0; i < 12; ++i) fill(o, n, kIdentity + 4u * i, (i == 0 || i == 5 || i == 10) ? 0x3F800000u : 0u);
}
u32 time_word() { return pick(6) == 0 ? float_word() : (pick(4) == 0 ? (pick(2) ? 0x3F800000u : 0u) : f2w(float(unit()))); }

// The matrix stack object, its matrices, the scratch tables.
void setup_world(Machine& o, Machine& n) {
    for (u32 off = 0; off < kArenaSize; off += 4u) fill(o, n, kArena + off, rnd());
    for (u32 a = kScratchLo; a < kScratchHi; a += 4u) fill(o, n, a, rnd());
    for (u32 a = 0x81420000u; a < 0x81428000u; a += 4u) fill(o, n, a, rnd());
    put_identity(o, n);
    for (u32 a : {kR13 + 0x5F78u, kR13 + 0x5F7Cu, kR13 + 0x5F80u}) fill(o, n, a, rnd());  // the sampler pointers (the pick sets them or leaves them)
    fill(o, n, kR13 + 0x5F50u, kStackObject);
    fill(o, n, kStackObject, kStackMatrices);
    fill(o, n, kStackObject + 8u, pick(5));
    for (unsigned k = 0; k < 8; ++k) put_matrix(o, n, kStackMatrices + 0x30u * k, false);
    for (u32 a = 0x805E2590u; a < 0x805E25B0u; a += 4u) fill(o, n, a, time_word());  // weights (0x805E23A0 + 0x210)
    for (u32 a = 0x805E25B0u; a < 0x805E25F0u; a += 4u) fill(o, n, a, sane_word());  // channel weights
    // the junk under r1: both machines hold the same words, the original overwrites some of them
    for (u32 a = kStack - 0x400u; a < kStack + 0x100u; a += 4u) fill(o, n, a, rnd());
    // a guest callback: stores r3 at kArena + 0x1F00 and returns; the post callback stores r3 at +0x1F04 and the struct's first word
    const u32 fn[] = {0x3C808140u, 0x90641F00u, 0x4E800020u};
    for (unsigned i = 0; i < 3; ++i) fill(o, n, kGuestFn + 4u * i, fn[i]);
    const u32 post[] = {0x3C808140u, 0x90641F04u, 0x80A30000u, 0x90A41F08u, 0x4E800020u};
    for (unsigned i = 0; i < 5; ++i) fill(o, n, kGuestPost + 4u * i, post[i]);
}

// A bone list: n entries (maybe ended early by -1), indices from a small set, parents forming a forest.
struct Bones { u32 count; };
Bones put_bones(Machine& o, Machine& n, u32 max_index) {
    const u32 count = std::getenv("MTN_POSE_STALE") ? pick(6) : 1 + pick(5);  // an empty list leaves the accumulator local unwritten (stale stack)
    u32 ids[8];
    for (u32 i = 0; i < 8; ++i) ids[i] = i;
    for (u32 i = 7; i > 0; --i) { const u32 j = pick(i + 1); const u32 t = ids[i]; ids[i] = ids[j]; ids[j] = t; }
    fill(o, n, kList, count + (pick(10) == 0 ? 1u : 0u));
    fill(o, n, kList + 4u, kIndices);
    fill(o, n, kList + 0xCu, kParents);
    u32 rank[8];
    for (u32 i = 0; i < 8; ++i) rank[ids[i]] = pick(8) * 8 + i;  // a random order, ties broken
    for (u32 i = 0; i < 8; ++i) {
        u32 v = ids[i] % (max_index + 1);
        fill(o, n, kIndices + 4u * i, (pick(14) == 0 && i > 0) ? 0xFFFFFFFFu : v);
    }
    for (u32 v = 0; v < 8; ++v) {
        u32 parent = 0xFFFFFFFFu;
        for (unsigned tries = 0; tries < 3 && pick(3) != 0; ++tries) {
            const u32 candidate = pick(max_index + 1);
            if (rank[candidate] < rank[v]) { parent = candidate; break; }
        }
        fill(o, n, kParents + 4u * v, parent);
    }
    // the same index twice would let a parent chain loop through the aliasing; keep the indices distinct within the list
    for (u32 i = 0; i < 8; ++i)
        for (u32 j = 0; j < i; ++j)
            if (o.memory.peek32(kIndices + 4u * i) == o.memory.peek32(kIndices + 4u * j) && o.memory.peek32(kIndices + 4u * i) != 0xFFFFFFFFu)
                fill(o, n, kIndices + 4u * i, 0xFFFFFFFFu);
    // A realistic list has a root (parent -1) that is processed, so the pass's accumulator local is written before it is read. A list
    // without one makes the original read an uninitialised stack local whose value is a dead write of whatever ran before it: that
    // depends on the stale stack, which the owner does not keep (MTN_POSE_STALE=1 keeps such lists).
    if (!std::getenv("MTN_POSE_STALE")) {
        const u32 first = o.memory.peek32(kIndices);
        if (first != 0xFFFFFFFFu) fill(o, n, kParents + 4u * first, 0xFFFFFFFFu);
    }
    return {count};
}
void put_args(Machine& o, Machine& n, bool euler_angles_big) {
    fill(o, n, kArgs, kList);
    fill(o, n, kArgs + 4u, pick(6) == 0 ? 0u : kBaseMatrix);
    put_float_matrix(o, n, kBaseMatrix);
    if (pick(2)) put_matrix(o, n, kBaseMatrix, false);
    for (u32 idx = 0; idx < 8; ++idx) {
        fill(o, n, kArgs + 8u + idx * 8u, kAngles + idx * 0x10u);
        fill(o, n, kArgs + 8u + idx * 8u + 4u, kFlagWords + idx * 4u);
        fill(o, n, kFlagWords + idx * 4u, (pick(5) == 0 ? 0u : 1u) | (rnd() & 0xFFFFFFFEu));
        for (u32 k = 0; k < 3; ++k) {
            u32 w;
            if (euler_angles_big && pick(3) == 0) w = f2w(float((unit() * 2 - 1) * 40.0));
            else if (pick(4) == 0) w = 0x3F800000u;  // exactly 1.0: the scale passes compare their accumulator with it
            else if (pick(10) == 0) w = float_word();
            else if (pick(5) == 0) w = f2w(float((unit() * 2 - 1) * 1e-5));
            else w = f2w(float((unit() * 2 - 1) * 6.4));
            fill(o, n, kAngles + idx * 0x10u + 4u * k, w);
        }
    }
}

// ---- the runner ----------------------------------------------------------------------------------------------------------------------
using Native = void (*)(Cpu&);
struct Args { u32 r3 = 0, r4 = 0, r5 = 0; u64 f1 = 0; u64 f2 = 0; u32 r6 = 0; };
// `setup` fills both machines and returns the registers; the native may be wrapped by a negative control.
template <class Setup>
void run_cases(Machine& o, Machine& n, const char* name, u32 entry, Native native, unsigned calls, bool expect_clean, bool compare_r3,
               Setup setup, unsigned* mismatches = nullptr) {
    for (unsigned i = 0; i < calls; ++i) {
        const bool ni = (i & 1u) != 0u;
        setup_world(o, n);
        const Args args = setup();
        std::vector<u32> snapshot;
        if (std::getenv("MTN_POSE_DUMP"))
            for (u32 a = kArena; a < kArena + kArenaSize; a += 4u) snapshot.push_back(o.memory.peek32(a));
        std::string skip_dump;
        if (std::getenv("MTN_POSE_SKIPDUMP")) {  // the inputs of a call the original may not finish (read before the original runs)
            char buf[256];
            std::snprintf(buf, sizeof buf, "SKIPCASE %s ni %d dt %08X mot14 %08X mot20 %08X head %08X\n", name, ni ? 1 : 0, o.memory.peek32(kR13 + 0x2910u),
                          o.memory.peek32(kArena + 0x2014u), o.memory.peek32(kArena + 0x2020u), o.memory.peek32(kArena + 0x2224u));
            skip_dump = buf;
            for (u32 k = 0; k < 4; ++k) {
                const u32 ch = kArena + 0x2400u + 0x130u * k, pb = o.memory.peek32(ch + 0x120u);
                std::snprintf(buf, sizeof buf, "  ch%u flags %08X start %08X end %08X rate %08X time %08X w94 %08X | par %08X kind %08X p10 %08X p18 %08X p1C %08X p20 %08X\n", k,
                              o.memory.peek32(ch + 0x90u), o.memory.peek32(ch + 0xA0u), o.memory.peek32(ch + 0xA4u), o.memory.peek32(ch + 0xA8u), o.memory.peek32(ch + 0xACu),
                              o.memory.peek32(ch + 0x94u), o.memory.peek32(pb), o.memory.peek32(pb + 8u), o.memory.peek32(pb + 0x10u), o.memory.peek32(pb + 0x18u),
                              o.memory.peek32(pb + 0x1Cu), o.memory.peek32(pb + 0x20u));
                skip_dump += buf;
            }
            std::snprintf(buf, sizeof buf, "  table %08X %08X %08X %08X %08X %08X\n", o.memory.peek32(kArena + 0x2A00u), o.memory.peek32(kArena + 0x2A04u), o.memory.peek32(kArena + 0x2A08u),
                          o.memory.peek32(kArena + 0x2A0Cu), o.memory.peek32(kArena + 0x2A10u), o.memory.peek32(kArena + 0x2A14u));
            skip_dump += buf;
        }
        o.prepare(entry, ni);
        n.prepare(entry, ni);
        for (unsigned r = 14; r < 32; ++r) {
            const u32 v = rnd();
            o.cpu.gpr[r] = n.cpu.gpr[r] = v;
            const u64 d = single_to_double_bits(rnd() | 0x3F000000u);
            o.cpu.fpr[r].bits[0] = n.cpu.fpr[r].bits[0] = d;
            o.cpu.fpr[r].bits[1] = n.cpu.fpr[r].bits[1] = single_to_double_bits(f2w(float(unit())));
        }
        for (Machine* m : {&o, &n}) { m->cpu.gpr[3] = args.r3; m->cpu.gpr[4] = args.r4; m->cpu.gpr[5] = args.r5; m->cpu.gpr[6] = args.r6; m->cpu.fpr[1].bits[0] = args.f1; m->cpu.fpr[2].bits[0] = args.f2; }
        g_tracing = std::getenv("MTN_POSE_DUMP") != nullptr;
        g_original_trace.clear();
        const auto t_original = std::chrono::steady_clock::now();
        // Phase C family: a follow channel (kind 1) can warp the playback time by up to ~1e7 (the partner's position divided by a ~1e-6 span), and the original
        // then wraps it back one span at a time: up to ~6e6 loop iterations, terminating (the float spacing at that time is below the span). The default
        // budget is far too small for that, so the family gets a budget sized for it; a call that still does not finish is a skip and is listed.
        const bool phase_c_family = std::strncmp(name, "PhaseC", 6) == 0 || std::strcmp(name, "MotionUpdate(phaseC)") == 0;
        g_step_budget = phase_c_family ? 4000000000ul : 3000000ul;
        const bool finished = o.run_original();
        ChainTime& chain_time = g_chain_time[name];
        chain_time.original += seconds_since(t_original);
        g_step_budget = 3000000ul;
        ++chain_time.calls;
        if (!finished) { if (!skip_dump.empty()) { std::fputs(skip_dump.c_str(), stderr); std::fprintf(stderr, "  stuck pc %08X lr %08X r1 %08X stopped %d\n", o.cpu.pc, o.cpu.lr, o.cpu.gpr[1], o.cpu.stopped() ? 1 : 0);
                        for (u32 k = 0; k < 4; ++k) {  // the channel words where the original gave up (a loop in flight shows its current time)
                            const u32 ch = kArena + 0x2400u + 0x130u * k;
                            std::fprintf(stderr, "  at-stuck ch%u flags %08X start %08X end %08X rate %08X time %08X\n", k, o.memory.peek32(ch + 0x90u), o.memory.peek32(ch + 0xA0u), o.memory.peek32(ch + 0xA4u), o.memory.peek32(ch + 0xA8u), o.memory.peek32(ch + 0xACu));
                        }
                        std::fprintf(stderr, "  at-stuck steps %lu\n", g_step_budget); } ++g_skipped; ++g_skipped_by[name]; ++chain_time.skipped; continue; }  // the original did not finish (a loop the random data made endless)
        const auto t_native = std::chrono::steady_clock::now();
        native(n.cpu);
        chain_time.native += seconds_since(t_native);
        std::string detail;
        const bool same = same_state(o, n, name, ni, detail, compare_r3);
        ++g_checked;
        if (!same) {
            if (mismatches) ++*mismatches;
            if (expect_clean) {
                report(name, detail);
                static int dumps = 0;
                if (std::getenv("MTN_POSE_DUMP") && dumps++ < 1) {
                    std::fprintf(stderr, "  inputs: r3 %08X r4 %08X r5 %08X ni %d; arena words:\n", args.r3, args.r4, args.r5, ni);
                    for (const TraceEntry& t : g_original_trace)
                        std::fprintf(stderr, "   original entry %08X r3 %08X r4 %08X r1 %08X state %08X\n", t.function, t.r3, t.r4, t.r1, t.hash);
                    for (u32 w = 0; w < snapshot.size(); w += 4u)
                        std::fprintf(stderr, "  %08X: %08X %08X %08X %08X\n", kArena + 4u * w, snapshot[w], snapshot[w + 1], snapshot[w + 2], snapshot[w + 3]);
                }
            }
        }
    }
}
u64 sf(u32 word) { return single_to_double_bits(word); }

// The tree for NodeWalk: nodes at kArena + 0x800 + 0x80 * k.
constexpr u32 kMotion = kArena + 0x600u, kChannels = kArena + 0x640u, kNodes = kArena + 0x800u, kEntries = kArena + 0xC00u;
u32 g_node_count, g_entry_count;
bool g_walk_fold = false;  // entries may be the five folded callbacks
u32 put_node(Machine& o, Machine& n, unsigned depth) {
    const u32 node = kNodes + 0x40u * g_node_count++;
    for (u32 off = 0; off < 0x40u; off += 4u) fill(o, n, node + off, 0u);
    // the callback list
    u32 head = 0, tail = 0;
    const unsigned mask = std::getenv("MTN_POSE_MASK") ? static_cast<unsigned>(std::atoi(std::getenv("MTN_POSE_MASK"))) : 0u;
    const unsigned entries = (mask & 1u) ? 0u : pick(3);
    for (unsigned e = 0; e < entries && g_entry_count < 24; ++e) {
        const u32 entry = kEntries + 0x60u * g_entry_count++;
        static const u32 fns[] = {mtn_pose::kRotationPass, mtn_pose::kScalePass, mtn_pose::kScaleLengthsPass, mtn_pose::kTranslationPass,
                                  mtn_pose::kEulerBone, kGuestFn, 0u};
        static const u32 fold_fns[] = {mtn_pose::kFoldScaleCallback, mtn_pose::kFoldTranslateCallback, mtn_pose::kFoldScaleBank,
                                       mtn_pose::kFoldTranslationBank, mtn_pose::kFoldRotationBank, mtn_pose::kRotationPass, kGuestFn};
        const u32 fn = g_walk_fold ? fold_fns[pick(7)]
                       : std::getenv("MTN_POSE_FN") ? fns[std::atoi(std::getenv("MTN_POSE_FN")) % 7] : fns[pick(7)];
        fill(o, n, entry, fn);
        fill(o, n, entry + 4u, 0u);
        if (tail) fill(o, n, tail + 4u, entry); else head = entry;
        tail = entry;
    }
    fill(o, n, node + 8u, head);
    fill(o, n, node + 0xCu, kArena + 0x1F80u);  // points at a word
    fill(o, n, node + 0x10u, (pick(3) == 0 && !(mask & 4u)) ? kArena + 0x1E00u : 0u);
    fill(o, n, node + 0x14u, kArena + 0x1A00u);
    fill(o, n, node + 0x18u, kArena + 0x1B00u);
    fill(o, n, node + 0x1Cu, (pick(3) == 0 && !(mask & 2u)) ? kGuestPost : 0u);
    fill(o, n, node + 0x20u, rnd());
    if (depth < 3 && !(mask & 8u) && pick(3) == 0) fill(o, n, node + 0x24u, put_node(o, n, depth + 1));
    if (depth < 3 && !(mask & 8u) && pick(3) == 0) fill(o, n, node + 0x28u, put_node(o, n, depth + 1));
    return node;
}

// ---- round 5: the MotionUpdate tree ---------------------------------------------------------------------------------------------------
// One motion object, four channels (0x130 bytes each), the channel table, the callback lists, the node chain the bone pass reads.
constexpr u32 kSec = kArena + 0x3C80u, kBaseM = kArena + 0x3A00u, kPar2 = kArena + 0x3B00u, kRecs = kArena + 0x3B40u;
std::vector<u32> g_c_words;  // the original words of 0x80419A9C
std::vector<u32> g_d_words;  // the original words of 0x80418618 (the MotionUpdate tests replace it by a stub)
constexpr u32 kMot = kArena + 0x2000u, kNodeA = kArena + 0x2200u, kHead = kArena + 0x2220u, kChans = kArena + 0x2400u, kTable = kArena + 0x2A00u,
              kParams = kArena + 0x2A80u, kLists = kArena + 0x2C00u, kStubLog = kArena + 0x3E00u;
// A stub for a guest phase (the five phase bodies are replaced on both machines): logs r3 and either r4 (the channel phase) or a tag.
void put_stub(Machine& o, Machine& n, u32 at, u32 tag, bool log_r4) {
    std::vector<u32> w = {0x3D608140u, 0x818B3E00u, 0x558A1838u, 0x7D4A5A14u, 0x906A3E10u};
    if (log_r4) w.push_back(0x908A3E14u);
    else { w.push_back(0x39200000u | tag); w.push_back(0x912A3E14u); }
    w.push_back(0x398C0001u);
    w.push_back(0x918B3E00u);
    w.push_back(0x4E800020u);
    for (size_t i = 0; i < w.size(); ++i) fill(o, n, at + 4u * static_cast<u32>(i), w[i]);
}
// 0x8041ABF8 stand-in: logs r3 and r6, writes a vector derived from r5 into r3
void put_wrap_stub(Machine& o, Machine& n, u32 at) {
    const u32 w[] = {0x3D608140u, 0x818B3E00u, 0x558A1838u, 0x7D4A5A14u, 0x906A3E10u, 0x90CA3E14u, 0x81250000u, 0x91230000u, 0x81250004u, 0x91230004u,
                     0x81250008u, 0x91230008u, 0x398C0001u, 0x918B3E00u, 0x4E800020u};
    for (size_t i = 0; i < sizeof(w) / sizeof(w[0]); ++i) fill(o, n, at + 4u * static_cast<u32>(i), w[i]);
}
void put_stubs(Machine& o, Machine& n, bool stub_d, bool stub_c = true) {
    put_stub(o, n, 0x8041BB8Cu, 0, true);
    put_stub(o, n, 0x8041AFACu, 1, false);
    put_stub(o, n, 0x8041A33Cu, 2, false);
    if (stub_c) put_stub(o, n, 0x80419A9Cu, 3, false);
    else for (size_t i = 0; i < g_c_words.size(); ++i) fill(o, n, 0x80419A9Cu + 4u * static_cast<u32>(i), g_c_words[i]);
    put_stub(o, n, 0x8041BF20u, 6, true);
    put_stub(o, n, 0x8041DC68u, 7, true);
    put_stub(o, n, 0x80414290u, 8, false);
    put_stub(o, n, 0x8041A6BCu, 9, false);
    put_wrap_stub(o, n, 0x8041ABF8u);
    if (stub_d) put_stub(o, n, 0x80418618u, 4, false);
    else for (size_t i = 0; i < g_d_words.size(); ++i) fill(o, n, 0x80418618u + 4u * static_cast<u32>(i), g_d_words[i]);
    put_stub(o, n, 0x8041DDBCu, 5, true);
}
u32 rate_word() {
    switch (pick(8)) {
        case 0: return 0u;
        case 1: return 0x80000000u;
        case 2: return 0x7FC00000u | pick(4);
        case 3: return 0x7F800001u;
        case 4: return f2w(float(unit() * 4 - 2));
        case 5: return f2w(float(-(unit() * 3 + 0.01)));
        default: return f2w(float(unit() * 3 + 0.01));
    }
}
// Exotic values for the TIME fixtures (frame time, playback time) that cannot make the original's wrap loops run for ever: zeros, denormals, NaNs and
// moderate normals. An infinite or huge finite time would make the ORIGINAL need millions of loop iterations (the call is then skipped after its whole
// 3 M-step budget, which is what made the contract run past 1800 s); the NaN / denormal / signed-zero compare paths stay covered.
u32 bounded_exotic_word() {
    switch (pick(8)) {
        case 0: return 0u;
        case 1: return 0x80000000u;
        case 2: return (pick(2) << 31) | (1u + pick(0x7FFFFFu));          // denormal
        case 3: return (pick(2) << 31) | 0x7FC00000u | pick(0x400000u);   // quiet NaN
        case 4: return (pick(2) << 31) | 0x7F800001u | pick(0x3FFFFFu);   // signalling NaN
        default: return f2w(float((unit() * 2.0 - 1.0) * 8.0));
    }
}
u32 int_ish_word() {
    switch (pick(7)) {
        case 0: return f2w(1.0e10f);
        case 1: return 0x7FC00000u;
        case 2: return float_word();
        default: return f2w(float((unit() * 2 - 1) * 3000.0));
    }
}
void put_motion(Machine& o, Machine& n, bool with_node_data, bool stub_d = true, int eligible = 1, bool stub_c = true, bool timing = false) {
    put_stubs(o, n, stub_d, stub_c);
    fill(o, n, kStubLog, 0u);
    const u32 count = pick(10) == 0 ? 0u : 1u + pick(4);
    for (u32 off = 0; off < 0x160u; off += 4u) fill(o, n, kMot + off, value_word());
    fill(o, n, kMot + 4u, kBaseM);
    put_matrix(o, n, kBaseM, false);
    fill(o, n, kMot + 0x2Cu, kPar2);
    for (u32 i = 0; i < 6; ++i) fill(o, n, kPar2 + 4u * i, pick(3) == 0 ? 0xFFFFFFFFu : pick(4));
    for (u32 r = 0; r < 8; ++r) {
        fill(o, n, kRecs + 0x20u * r, pick(3) ? 1u : 0u);
        for (u32 k = 1; k < 8; ++k) fill(o, n, kRecs + 0x20u * r + 4u * k, value_word());
    }
    fill(o, n, kMot + 0xCu, kNodeA);
    fill(o, n, kMot + 0x10u, kChans);
    fill(o, n, kMot + 0x14u, (rnd() & 0xFFFFFFF0u) | (pick(2) ? 8u : 0u) | (pick(2) ? 1u : 0u) | (pick(4) == 0 ? 2u : 0u) | (pick(3) ? 4u : 0u) | (pick(3) == 0 ? 0x10000u : 0u));
    fill(o, n, kMot + 0x20u, count);
    fill(o, n, kMot + 0x24u, kTable);
    for (u32 off = 0; off < 0x40u; off += 4u) fill(o, n, kNodeA + off, 0u);
    fill(o, n, kNodeA + 0xCu, kHead);
    fill(o, n, kNodeA + 0x18u, kRecs);
    fill(o, n, kHead + 4u, ((pick(10) == 0 ? pick(4) : 1u) << 16) | (pick(6) == 0 ? 0u : (pick(2) ? (rnd() & 7u) | 1u : (rnd() & 0xFFFFu))));
    for (u32 i = 0; i < 6; ++i) fill(o, n, kTable + 4u * i, (i > 0 && pick(8) == 0) ? 0xFFFFFFFFu : pick(4));
    put_matrix(o, n, kMot + 0x30u, false);
    put_matrix(o, n, kMot + 0x90u, false);
    put_matrix(o, n, kMot + 0xC0u, false);
    for (u32 k = 0; k < 4; ++k) {
        const u32 ch = kChans + 0x130u * k;
        for (u32 off = 0; off < 0x130u; off += 4u) fill(o, n, ch + off, value_word());
        put_matrix(o, n, ch, false);
        put_matrix(o, n, ch + 0x60u, false);
        const u32 flags = (pick(6) ? 4u : 0u) | (pick(2) ? 1u : 0u) | (pick(3) == 0 ? 0x80000u : 0u) | (pick(3) == 0 ? 0x100000u : 0u) |
                          (pick(3) == 0 ? 0x200000u : 0u) | (pick(3) == 0 ? 8u : 0u) | (pick(4) == 0 ? 0x1000000u : 0u) | (pick(4) == 0 ? (rnd() & 0xFFFF0000u & ~0x01F80000u) : 0u) | (pick(3) == 0 ? (rnd() & 0xF0u) : 0u);
        fill(o, n, ch + 0x90u, flags);
        fill(o, n, ch + 0x94u, (pick(2) ? 0x400u : 0u) | (pick(3) == 0 ? 0x2000u : 0u) | (rnd() & 0x3FFu));
        fill(o, n, ch + 0xA8u, rate_word());
        fill(o, n, ch + 0xACu, int_ish_word());
        fill(o, n, ch + 0x120u, kParams + 0x40u * k);
        { static const u32 kinds[] = {0u, 1u, 1u, 0u, 2u, 0xFFFFFFFFu}; fill(o, n, kParams + 0x40u * k + 8u, kinds[pick(6)]); }
        fill(o, n, kParams + 0x40u * k, (pick(2) ? 1u : 0u) | (rnd() & 0xFFFFFFFEu));
        fill(o, n, kParams + 0x40u * k + 0x10u, pick(6) == 0 ? float_word() : f2w(float(unit())));
        const u32 e0 = kLists + 0x200u * k;  // two entries per channel, 0x100 apart (the sample owners read three tracks behind an entry)
        fill(o, n, ch + 0x124u, pick(8) == 0 ? 0u : e0);
        for (u32 e = 0; e < 2; ++e) {
            const u32 at = e0 + 0x100u * e;
            for (u32 off = 0; off < 0x100u; off += 4u) fill(o, n, at + off, 0u);
            static const u32 fns[] = {0u, kGuestFn, mtn_track::kSampleVec3Reciprocal, mtn_track::kSampleVec3, mtn_track::kSampleVec3At20, mtn_track::kSampleVec3Scaled};
            fill(o, n, at, fns[pick(6)]);
            fill(o, n, at + 4u, fns[pick(6)]);
            fill(o, n, at + 8u, e == 0 && pick(5) ? at + 0x100u : 0u);
        }
    }
    fill(o, n, kMot + 0x28u, kSec);
    for (u32 i = 0; i < 6; ++i) fill(o, n, kSec + 4u * i, pick(3) == 0 ? 0xFFFFFFFFu : pick(4));
    if (timing) {
        const u32 dts[] = {f2w(1.0f / 60.0f), f2w(1.0f / 30.0f), f2w(0.0f), f2w(0.5f), f2w(-0.02f), bounded_exotic_word()};
        fill(o, n, kR13 + 0x2910u, dts[pick(pick(12) == 0 ? 6u : 4u)]);
        for (u32 k = 0; k < 4; ++k) {
            const u32 ch = kChans + 0x130u * k;
            const bool odd = pick(25) == 0;
            const float start = float(unit() * 2.0), len = float(0.2 + unit() * 3.0);
            fill(o, n, ch + 0xA0u, f2w(start));
            fill(o, n, ch + 0xA4u, pick(30) == 0 ? f2w(start + 1.0e-6f) : f2w(start + len));
            fill(o, n, ch + 0xA8u, odd ? rate_word() : (pick(8) == 0 ? 0u : f2w(float((unit() * 2.0 - 1.0) * 3.0))));
            fill(o, n, ch + 0xACu, odd ? bounded_exotic_word() : f2w(start + len * float(unit() * 4.0 - 1.5)));
            fill(o, n, ch + 0x128u, ((pick(2) ? 1u + pick(5) : 0u) << 16) | (rnd() & 0xFFFFu));
            fill(o, n, ch + 0x90u, o.memory.peek32(ch + 0x90u) | (pick(2) ? 2u : 0u) | (pick(6) == 0 ? 0x40000u : 0u) | (pick(8) == 0 ? 0x20000u : 0u) | (pick(3) == 0 ? 0x10u : 0u));
            const u32 pb = kParams + 0x40u * k;
            fill(o, n, pb + 0x10u, odd ? float_word() : f2w(float(unit() * 1.6 - 0.3)));
            fill(o, n, pb + 0x18u, f2w(float(0.1 + unit() * 2.0)));
            fill(o, n, pb + 0x1Cu, f2w(float(unit() * 2.0)));
            fill(o, n, pb + 0x20u, f2w(float(unit() * 1.2)));
        }
    }
    if (eligible >= 0) {  // the phase's hand-back conditions: indices 0..3, first active channel without a partner
        const u32 cnt = o.memory.peek32(kMot + 0x20u);
        for (u32 i = 0; i < cnt && i < 6; ++i) {
            const u32 idx = o.memory.peek32(kTable + 4u * i);
            if (idx == 0xFFFFFFFFu) break;
            if (o.memory.peek32(kChans + 0x130u * idx + 0x90u) & 4u) { fill(o, n, kPar2 + 4u * idx, 0xFFFFFFFFu); break; }
        }
    }
    if (with_node_data) {  // what the NodeWalk inside the bone pass reads
        fill(o, n, kNodeA + 0x14u, kArena + 0x1A00u);
        fill(o, n, kNodeA + 0x18u, kArena + 0x1B00u);
        for (u32 i = 0; i < 0x200u; i += 4u) fill(o, n, kArena + 0x1A00u + i, value_word());
        for (u32 i = 0; i < 0x200u; i += 16u) fill(o, n, kArena + 0x1B00u + i, (i < 0x80u && (i & 0x10u) == 0u) || pick(3) ? 1u : 0u);
    }
}

// ---- round 7, candidate B: the clip start phase and the bind tree ----------------------------------------------------------------------
// obj-side node tree: OA = kNodeA (arena) with child OB and sibling OC; per node a data block (key word first) and four chained lists of entry
// pointers; clip side: an info block with an embedded node tree; key records for the cursor stand-in.
constexpr u32 kBig = 0x81420000u, kBigSize = 0x8000u;
constexpr u32 kOB = kBig + 0x100u, kOC = kBig + 0x200u, kListsB = kBig + 0x1000u, kEntriesB = kBig + 0x2000u, kDataB = kBig + 0x5000u;
constexpr u32 kRootInfo = kBig + 0x5300u, kInfo = kBig + 0x5400u, kClipNodes = kBig + 0x5500u, kRecsB = kBig + 0x5800u;
constexpr u32 kRetTables = kArena + 0x4000u;
std::vector<u32> g_b_words;  // the original words of 0x8041A33C
u32 ret_stub_word(unsigned n, unsigned i) {
    const u32 disp = 0x4000u + 0x20u * n;
    const u32 w[] = {0x3D608140u, 0x818B3E00u, 0x718A001Fu, 0x554A1838u, 0x7D4A5A14u, 0x906A3E10u, 0x908A3E14u, 0x392C0001u, 0x912B3E00u,
                     0x718C0007u, 0x558C103Au, 0x7D8C5A14u, 0x806C0000u | disp, 0x4E800020u};
    if (i == 6 && n != 0) return 0x906A3E14u;  // only the first table's function (0x80412E3C) takes a second argument
    return w[i];
}
void put_ret_stub(Machine& o, Machine& n, u32 at, unsigned table) {
    for (unsigned i = 0; i < 14; ++i) fill(o, n, at + 4u * i, ret_stub_word(table, i));
}
void put_b_stubs(Machine& o, Machine& n, bool stub_b) {
    if (stub_b) put_stub(o, n, 0x8041A33Cu, 2, false);
    else for (size_t i = 0; i < g_b_words.size(); ++i) fill(o, n, 0x8041A33Cu + 4u * static_cast<u32>(i), g_b_words[i]);
    put_stub(o, n, 0x80414368u, 10, false);
    put_stub(o, n, 0x8041DEC4u, 11, true);
    put_stub(o, n, 0x8041A934u, 12, true);
    put_stub(o, n, 0x8041A508u, 13, false);
    put_ret_stub(o, n, 0x80412E3Cu, 0);
    put_ret_stub(o, n, 0x80413644u, 1);
    put_ret_stub(o, n, 0x8041342Cu, 2);
    put_ret_stub(o, n, 0x8041174Cu, 3);
    put_ret_stub(o, n, 0x8041352Cu, 4);
}
// the obj-side nodes and the clip side; `rich` makes the flags dense so every branch of the bind code is reached
void put_clip_world(Machine& o, Machine& n) {
    const u32 keys[3] = {0x11u + pick(3), 0x20u + pick(3), 0x30u + pick(3)};
    const u32 nodes[3] = {kNodeA, kOB, kOC};
    const u32 datas[3] = {kHead, kDataB, kDataB + 0x80u};
    for (u32 i = 0; i < 3; ++i) {
        const u32 x = nodes[i];
        if (i > 0) for (u32 off = 0; off < 0x40u; off += 4u) fill(o, n, x + off, 0u);
        const u32 flags = (pick(2) ? 0x400u : 0u) | (pick(2) ? 7u & rnd() : 0u) | (pick(2) ? 0x38u & rnd() : 0u) | (pick(2) ? 0x380u & rnd() : 0u) | (pick(4) == 0 ? 0x40u : 0u);
        fill(o, n, x, flags);
        fill(o, n, x + 8u, kListsB + 0x400u * i);
        fill(o, n, x + 0xCu, datas[i]);
        if (i > 0) for (u32 k = 0; k < 8; ++k) fill(o, n, datas[i] + 4u * k, rnd());
        fill(o, n, datas[i], keys[i]);
        if (i == 0) fill(o, n, kHead + 4u, ((pick(10) == 0 ? pick(4) : 1u) << 16) | (pick(6) == 0 ? 0u : (rnd() & 0xFFFFu) | 1u));
        for (u32 k = 0; k < 4; ++k) {
            const u32 list = kListsB + 0x400u * i + 0x80u * k;
            fill(o, n, list, 0u);  // the walk reads this chain as callback entries: no function
            fill(o, n, list + 4u, k < 3 ? list + 0x80u : 0u);
            for (u32 idx = 0; idx < 4; ++idx) fill(o, n, list + 0x10u + 8u * idx, kEntriesB + ((i * 4u + k) * 4u + idx) * 0xE0u);
        }
        for (u32 off = 0x10u; off < 0x50u; off += 4u) if (i == 0) fill(o, n, kHead + off, value_word());
    }
    fill(o, n, kNodeA + 0x24u, kOB);
    fill(o, n, kNodeA + 0x28u, kOC);
    for (u32 g = 0; g < 4; ++g) {  // grandchildren for the search, outside the entry blocks
        const u32 x = kBig + 0x6000u + 0x80u * g;
        for (u32 off = 0; off < 0x40u; off += 4u) fill(o, n, x + off, 0u);
        fill(o, n, x + 0xCu, kBig + 0x6400u + 0x80u * g);
        fill(o, n, kBig + 0x6400u + 0x80u * g, pick(3) ? 0x11u + pick(0x30u) : 0x99u);
    }
    if (pick(2)) { fill(o, n, kOB + 0x24u, kBig + 0x6000u); fill(o, n, kOB + 0x28u, kBig + 0x6080u); }
    if (pick(2)) { fill(o, n, kOC + 0x24u, kBig + 0x6100u); fill(o, n, kOC + 0x28u, kBig + 0x6180u); }
    // the record stand-ins for the cursor
    for (u32 r = 0; r < 8; ++r) {
        const u32 rec = kRecsB + 0x100u * r;
        fill(o, n, rec, 0x80u + 4u * pick(8));
        fill(o, n, rec + 4u, ((2u + pick(4)) << 16) | ((pick(4) << 8) & 0xFF00u) | (rnd() & 0xFFu));
        fill(o, n, rec + 4u, ((2u + pick(4)) << 16) | (pick(4) << 8));
        for (u32 off = 8; off < 0xF0u; off += 4u) fill(o, n, rec + off, pick(3) ? ((pick(0x3Fu) << 24) | (pick(2) ? 0x80000000u : 0u) | (rnd() & 0x00FFFFFFu)) : rnd());
        fill(o, n, kRetTables + 0x20u * 4u + 4u * r, rec);  // 0x8041352C returns records
    }
    for (u32 r = 0; r < 8; ++r) {
        fill(o, n, kRetTables + 4u * r, pick(2));                            // 0x80412E3C
        fill(o, n, kRetTables + 0x20u + 4u * r, pick(5) ? 1u : 0u);          // 0x80413644: lookup hit
        fill(o, n, kRetTables + 0x40u + 4u * r, pick(2) ? 0x5A5A0001u : 0x5A5A0002u);  // 0x8041342C: handle
        fill(o, n, kRetTables + 0x60u + 4u * r, pick(5) ? kInfo : 0u);       // 0x8041174C: clip info
    }
    fill(o, n, kRootInfo, 0x5A5A0001u);
    fill(o, n, kRootInfo + 8u, pick(6) == 0 ? float_word() : f2w(float(unit() * 3.0 + 0.2)));
    fill(o, n, kMot + 8u, kRootInfo);
    fill(o, n, kInfo + 8u, pick(6) == 0 ? float_word() : f2w(float(unit() * 3.0 + 0.2)));
    // clip-side node tree embedded at kInfo + 0x2C: root, two children
    const u32 cn[3] = {kInfo + 0x2Cu, kClipNodes, kClipNodes + 0x100u};
    for (u32 i = 0; i < 3; ++i) {
        const u32 x = cn[i];
        if (i > 0) for (u32 off = 0; off < 0x60u; off += 4u) fill(o, n, x + off, 0u);
        fill(o, n, x, pick(4) ? keys[pick(3)] : 0x77u);
        fill(o, n, x + 4u, rnd());
        fill(o, n, x + 4u, (rnd() & 0xFFFF0000u) | (pick(3) ? (rnd() & 0x3FFu) : (rnd() & 0xFFFFu)));
        fill(o, n, x + 0x48u, i == 0 ? cn[1] : 0u);
        fill(o, n, x + 0x4Cu, i == 0 ? cn[2] : (i == 1 && pick(3) == 0 ? cn[2] : 0u));
    }
}

std::vector<u8> read_file(const char* path) {
    std::ifstream input(path, std::ios::binary);
    return std::vector<u8>{std::istreambuf_iterator<char>(input), {}};
}
}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: mtn_pose_contract_tests main.dol");
        const std::vector<u8> dol = read_file(argv[1]);
        require(dol.size() >= 256, "missing DOL");
        u64 hash = 0xCBF29CE484222325ull;
        for (u8 byte : dol) hash = (hash ^ byte) * 0x100000001B3ull;
        require(hash == 0xD2CFB4A1CA897F9Bull, "unexpected original DOL identity");
        SwitchGuard switches;
        Machine original(dol), native(dol);
        for (u32 a = 0x80418618u; a < 0x80419094u; a += 4u) g_d_words.push_back(original.memory.peek32(a));
        for (u32 a = 0x80419A9Cu; a < 0x8041A33Cu; a += 4u) g_c_words.push_back(original.memory.peek32(a));
        for (u32 a = 0x8041A33Cu; a < 0x8041A508u; a += 4u) g_b_words.push_back(original.memory.peek32(a));
        // The constants compiled into the owner are the words of the DOL.
        {
            const struct { u32 address, word; } k[] = {{kR2 + 0x1700u, 0x3F800000u}, {kR2 + 0x1730u, 0x3F800000u}, {kR2 + 0x16B0u, 0x3F800000u},
                                                      {kR2 + 0x1728u, 0u}, {kR2 + 0x172Cu, 0x40000000u}, {kR2 + 0x1738u, 0x3F000000u},
                                                      {kR2 + 0x1734u, 0x3F7FF972u}, {kR2 + 0x171Cu, 0x47800000u}, {kR2 + 0x1720u, 0x40C90FDBu},
                                                      {kR2 + 0x173Cu, 0x40800000u}, {0x805EEDA0u, 0x34000000u}, {kR2 + 0x1708u, 0x43300000u},
                                                      {kR2 + 0x170Cu, 0x80000000u}};
            for (const auto& c : k) require(original.memory.peek32(c.address) == c.word, "a constant of the owner differs from the DOL");
        }
        const unsigned kCtl = std::getenv("MTN_POSE_CTRL") ? static_cast<unsigned>(std::atoi(std::getenv("MTN_POSE_CTRL"))) : 20000u;
        const unsigned kN = std::getenv("MTN_POSE_N") ? static_cast<unsigned>(std::atoi(std::getenv("MTN_POSE_N"))) : 40000u;
        auto chain = [&](const char* name, u32 entry, Native fn, unsigned calls, bool r3, auto setup, bool clean = true, unsigned* mm = nullptr) {
            if (const char* only = std::getenv("MTN_POSE_ONLY")) {
                bool hit = false;
                for (std::string rest = only; !rest.empty();) {
                    const size_t comma = rest.find(',');
                    const std::string one = rest.substr(0, comma);
                    if (std::string(name).find(one) != std::string::npos) hit = true;
                    rest = comma == std::string::npos ? std::string() : rest.substr(comma + 1);
                }
                if (!hit) return;
            }
            run_cases(original, native, name, entry, fn, calls, clean, r3, setup, mm);
        };
        Machine& O = original;
        Machine& N = native;
        // ---- the quaternion chain ----
        chain("MatrixToQuat", mtn_pose::kMatrixToQuat, mtn_pose::serve_matrix_to_quat, kN, false, [&] {
            pick(3) == 0 ? put_float_matrix(O, N, kArena + 0x40u) : put_matrix(O, N, kArena + 0x40u, true);
            return Args{kArena + 0x20u, kArena + 0x40u, 0, 0};
        });
        chain("QuatToMatrix", mtn_pose::kQuatToMatrix, mtn_pose::serve_quat_to_matrix, kN, false, [&] {
            put_quat(O, N, kArena + 0x40u, true);
            return Args{kArena + 0x80u, kArena + 0x40u, 0, 0};
        });
        chain("QuatSlerp", mtn_pose::kQuatSlerp, mtn_pose::serve_quat_slerp, kN, true, [&] {
            put_quat(O, N, kArena + 0x40u, true);
            if (pick(4) == 0) for (unsigned i = 0; i < 4; ++i) fill(O, N, kArena + 0x50u + 4u * i, O.memory.peek32(kArena + 0x40u + 4u * i) ^ (pick(2) ? 0u : 0x80000000u));
            else put_quat(O, N, kArena + 0x50u, true);
            if (pick(5) == 0) for (unsigned i = 0; i < 4; ++i) fill(O, N, kArena + 0x50u + 4u * i, O.memory.peek32(kArena + 0x40u + 4u * i));
            return Args{kArena + 0x20u, kArena + 0x40u, kArena + 0x50u, sf(pick(3) == 0 ? float_word() : f2w(float(unit())))};
        });
        for (unsigned variant = 0; variant < 2; ++variant)
            chain("QuatSlerp(plain)", mtn_pose::kQuatSlerp, mtn_pose::serve_quat_slerp, kN, true, [&] {
                put_quat(O, N, kArena + 0x40u, false);
                put_quat(O, N, kArena + 0x50u, false);
                return Args{kArena + 0x20u, kArena + 0x40u, kArena + 0x50u, sf(variant ? float_word() : f2w(float(unit())))};
            });
        for (unsigned alias = 0; alias < 3; ++alias)
            chain("MatrixBlend", mtn_pose::kMatrixBlend, mtn_pose::serve_matrix_blend, kN, false, [&] {
                put_matrix(O, N, kArena + 0x40u, true);
                put_matrix(O, N, kArena + 0x80u, true);
                if (pick(6) == 0) put_float_matrix(O, N, kArena + 0x80u);
                const u32 out = alias == 1 ? kArena + 0x40u : alias == 2 ? kArena + 0x80u : kArena + 0xC0u;
                return Args{out, kArena + 0x40u, kArena + 0x80u, sf(pick(8) == 0 ? float_word() : f2w(float(unit())))};
            });
        for (unsigned variant = 0; variant < 4; ++variant)
            chain("MatrixNormalize", mtn_pose::kMatrixNormalize, mtn_pose::serve_matrix_normalize, kN, false, [&] {
                put_float_matrix(O, N, kArena + 0x40u);
                put_matrix(O, N, kArena + 0x80u, true);
                const u32 dst = (variant & 1u) ? 0u : kArena + 0xC0u;
                const u32 src = (variant & 2u) ? 0u : (dst == 0u || pick(3) ? kArena + 0x80u : kArena + 0xC0u);
                return Args{dst, src, 0, 0};
            });
        // ---- the bone-list passes ----
        const auto pass_setup = [&](bool angles) { put_bones(O, N, std::getenv("MTN_POSE_MAXIDX") ? static_cast<u32>(std::atoi(std::getenv("MTN_POSE_MAXIDX"))) : 3u); put_args(O, N, angles); return Args{kArgs, 0, 0, 0}; };
        const struct { const char* name; u32 entry; Native fn; } passes[] = {
            {"RotationPass", mtn_pose::kRotationPass, mtn_pose::serve_rotation_pass},
            {"ScalePass", mtn_pose::kScalePass, mtn_pose::serve_scale_pass},
            {"ScaleLengthsPass", mtn_pose::kScaleLengthsPass, mtn_pose::serve_scale_lengths_pass},
            {"TranslationPass", mtn_pose::kTranslationPass, mtn_pose::serve_translation_pass}};
        for (const auto& p : passes) chain(p.name, p.entry, p.fn, kN, false, [&] { return pass_setup(true); });
        chain("EulerBone", mtn_pose::kEulerBone, mtn_pose::serve_euler_bone, kN, false, [&] { return pass_setup(true); });
        // ---- the tree walk ----
        chain("NodeWalk", mtn_pose::kNodeWalk, mtn_pose::serve_node_walk, kN / 2, false, [&] {
            pass_setup(true);
            g_node_count = g_entry_count = 0;
            fill(O, N, kMotion + 0x14u, pick(4) == 0 ? 2u : 0u);
            fill(O, N, kMotion + 0x20u, (std::getenv("MTN_POSE_MASK") && (std::atoi(std::getenv("MTN_POSE_MASK")) & 16)) ? 0u : pick(5));
            fill(O, N, kMotion + 0x24u, kChannels);
            for (u32 i = 0; i < 6; ++i) fill(O, N, kChannels + 4u * i, pick(8) == 0 ? 0xFFFFFFFFu : pick(4));
            for (u32 i = 0; i < 0x200u; i += 4u) fill(O, N, kArena + 0x1A00u + i, value_word());
            // A channel in the table has a record (an unrecorded channel's slot is never saved, and the original restores the stale word of the
            // frame into the weight table: undefined data, not kept by the owner). Records of channels 4..7 may be empty.
            for (u32 i = 0; i < 0x200u; i += 16u) fill(O, N, kArena + 0x1B00u + i, (i < 0x80u && (i & 0x10u) == 0u) || pick(3) ? 1u : 0u);
            const u32 root = put_node(O, N, 0);
            for (u32 k = 0; k < g_entry_count; ++k)  // every entry's args block is the pass block {list, matrix, pairs}
                for (u32 w = 0; w < 18; ++w) fill(O, N, kEntries + 0x60u * k + 8u + 4u * w, O.memory.peek32(kArgs + 4u * w));
            return Args{kMotion, root, 0, 0};
        });
        // ---- round 4: the folded callbacks, one by one against the original bodies ----
        const auto bank_setup = [&](bool base_needed, bool one_vectors) {
            put_bones(O, N, 3u);
            put_args(O, N, true);
            if (base_needed) fill(O, N, kArgs + 4u, kBaseMatrix);
            if (one_vectors)  // the callbacks compare the first bone vector with exactly (1, 1, 1)
                for (u32 idx = 0; idx < 8; ++idx)
                    if (pick(2)) for (u32 k = 0; k < 3; ++k) fill(O, N, kAngles + idx * 0x10u + 4u * k, 0x3F800000u);
            return Args{kArgs, 0, 0, 0};
        };
        chain("FoldScaleCallback", mtn_pose::kFoldScaleCallback, mtn_pose::serve_fold_scale_callback, kN, false, [&] { return bank_setup(false, true); });
        chain("FoldTranslateCallback", mtn_pose::kFoldTranslateCallback, mtn_pose::serve_fold_translate_callback, kN, false, [&] { return bank_setup(false, false); });
        chain("FoldScaleBank", mtn_pose::kFoldScaleBank, mtn_pose::serve_fold_scale_bank, kN, false, [&] { return bank_setup(false, true); });
        chain("FoldTranslationBank", mtn_pose::kFoldTranslationBank, mtn_pose::serve_fold_translation_bank, kN, false, [&] { return bank_setup(true, false); });
        chain("FoldRotationBank", mtn_pose::kFoldRotationBank, mtn_pose::serve_fold_rotation_bank, kN, false, [&] { return bank_setup(true, false); });
        // ---- the walk with the fold on: the five callbacks run inside it, the original walk enters their guest bodies ----
        const unsigned long long hits_before[mtn_pose::kFoldKinds] = {mtn_pose::fold_hits(0), mtn_pose::fold_hits(1), mtn_pose::fold_hits(2),
                                                                     mtn_pose::fold_hits(3), mtn_pose::fold_hits(4)};
        mtn_pose::set_fold_enabled(true);
        g_walk_fold = true;
        chain("NodeWalk(fold)", mtn_pose::kNodeWalk, mtn_pose::serve_node_walk, kN / 2, false, [&] {
            pass_setup(true);
            g_node_count = g_entry_count = 0;
            fill(O, N, kMotion + 0x14u, pick(4) == 0 ? 2u : 0u);
            fill(O, N, kMotion + 0x20u, pick(5));
            fill(O, N, kMotion + 0x24u, kChannels);
            for (u32 i = 0; i < 6; ++i) fill(O, N, kChannels + 4u * i, pick(8) == 0 ? 0xFFFFFFFFu : pick(4));
            for (u32 i = 0; i < 0x200u; i += 4u) fill(O, N, kArena + 0x1A00u + i, value_word());
            for (u32 i = 0; i < 0x200u; i += 16u) fill(O, N, kArena + 0x1B00u + i, (i < 0x80u && (i & 0x10u) == 0u) || pick(3) ? 1u : 0u);
            const u32 root = put_node(O, N, 0);
            for (u32 k = 0; k < g_entry_count; ++k)
                for (u32 w = 0; w < 18; ++w) fill(O, N, kEntries + 0x60u * k + 8u + 4u * w, O.memory.peek32(kArgs + 4u * w));
            return Args{kMotion, root, 0, 0};
        });
        g_walk_fold = false;
        mtn_pose::set_fold_enabled(false);
        unsigned long long walk_hits[mtn_pose::kFoldKinds];
        for (unsigned k = 0; k < mtn_pose::kFoldKinds; ++k) {
            walk_hits[k] = mtn_pose::fold_hits(k) - hits_before[k];
            std::printf("fold kind %u: %llu folded executions inside walks, %llu handed back to the guest body\n", k, walk_hits[k], mtn_pose::fold_handed_back(k));
        }
        if (!std::getenv("MTN_POSE_ONLY"))
            for (unsigned k = 0; k < mtn_pose::kFoldKinds; ++k) require(walk_hits[k] > 0, "a folded callback never ran inside a walk");
        // ---- round 5: the MotionUpdate tree (sampler pick, bone pass, the whole update with the guest phases stubbed on both machines) ----
        const unsigned tree_n = kN / 2;
        chain("SamplerPick", 0x804198F0u, mtn_pose::serve_sampler_pick, tree_n, false, [&] { put_motion(O, N, false); return Args{kMot, 0, 0, 0}; });
        chain("BonePass", 0x80419094u, mtn_pose::serve_bone_pass, tree_n, false, [&] { put_motion(O, N, true); return Args{kMot, 0, 0, 0}; });
        chain("MotionUpdate", mtn_pose::kMotionUpdate, mtn_pose::serve_motion_update, tree_n, false, [&] { put_motion(O, N, true); return Args{kMot, 0, 0, 0}; });
        chain("MotionUpdate(null)", mtn_pose::kMotionUpdate, mtn_pose::serve_motion_update, 200, false, [&] { put_motion(O, N, true); return Args{0u, 0, 0, 0}; });
        // negative controls: the wrong NI mode in the bone pass, a flipped context word after the sampler pick, a missed pop in the update
        unsigned caught_tree_ni = 0, caught_tree_ctx = 0, caught_tree_pop = 0;
        chain("BonePass(ni)", 0x80419094u, [](Cpu& c) { c.fpscr ^= kFpscrNI; mtn_pose::serve_bone_pass(c); }, kCtl, false,
              [&] { put_motion(O, N, true); return Args{kMot, 0, 0, 0}; }, false, &caught_tree_ni);
        chain("SamplerPick(ctx)", 0x804198F0u, [](Cpu& c) { mtn_pose::serve_sampler_pick(c); c.mem_write32(0x805E2380u, c.mem_read32(0x805E2380u) ^ 1u); }, kCtl, false,
              [&] { put_motion(O, N, false); return Args{kMot, 0, 0, 0}; }, false, &caught_tree_ctx);
        chain("MotionUpdate(pop)", mtn_pose::kMotionUpdate, [](Cpu& c) { mtn_pose::serve_motion_update(c); const u32 obj = c.mem_read32(0x805EC500u + 0x5F50u); c.mem_write32(obj + 8u, c.mem_read32(obj + 8u) + 1u); }, kCtl, false,
              [&] { put_motion(O, N, true); return Args{kMot, 0, 0, 0}; }, false, &caught_tree_pop);
        if (!std::getenv("MTN_POSE_ONLY")) require(caught_tree_ni > 0, "negative control: the wrong FPSCR[NI] mode in the bone pass was not caught");
        if (!std::getenv("MTN_POSE_ONLY")) require(caught_tree_ctx > 0, "negative control: a flipped sampling context word was not caught");
        if (!std::getenv("MTN_POSE_ONLY")) require(caught_tree_pop > 0, "negative control: a missed matrix stack pop was not caught");
        // ---- round 6, candidate D: the pose rebuild phase 0x80418618 (the original body runs on one machine, the native body on the other) ----
        chain("PhaseD", 0x80418618u, mtn_pose::serve_phase_d, tree_n, false, [&] { put_motion(O, N, true, false); return Args{kMot, 0, 0, 0}; });
        unsigned caught_d_ni = 0, caught_d_ulp = 0, caught_d_reset = 0;
        chain("PhaseD(ni)", 0x80418618u, [](Cpu& c) { c.fpscr ^= kFpscrNI; mtn_pose::serve_phase_d(c); }, kCtl, false,
              [&] { put_motion(O, N, true, false); return Args{kMot, 0, 0, 0}; }, false, &caught_d_ni);
        chain("PhaseD(ulp)", 0x80418618u, [](Cpu& c) { mtn_pose::serve_phase_d(c); c.mem_write32(0x81402400u + 4u, c.mem_read32(0x81402400u + 4u) ^ 1u); }, kCtl, false,
              [&] { put_motion(O, N, true, false); return Args{kMot, 0, 0, 0}; }, false, &caught_d_ulp);
        chain("PhaseD(reset)", 0x80418618u, [](Cpu& c) { mtn_pose::serve_phase_d(c); c.mem_write32(0x81402000u + 0x14u, c.mem_read32(0x81402000u + 0x14u) | 0x01000000u); }, kCtl, false,
              [&] { put_motion(O, N, true, false); return Args{kMot, 0, 0, 0}; }, false, &caught_d_reset);
        mtn_pose::set_phase_d_enabled(true);  // the native MotionUpdate calls the native phase directly (the original runs the real body)
        chain("MotionUpdate(phaseD)", mtn_pose::kMotionUpdate, mtn_pose::serve_motion_update, tree_n, false, [&] { put_motion(O, N, true, false); return Args{kMot, 0, 0, 0}; });
        mtn_pose::set_phase_d_enabled(false);
        // ---- round 6, candidate C: the time advance phase 0x80419A9C ----
        chain("PhaseC", 0x80419A9Cu, mtn_pose::serve_phase_c, tree_n, false, [&] { put_motion(O, N, true, true, -1, false, true); return Args{kMot, 0, 0, 0}; });
        unsigned caught_c_ni = 0, caught_c_ulp = 0, caught_c_skip = 0;
        chain("PhaseC(ni)", 0x80419A9Cu, [](Cpu& c) { c.fpscr ^= kFpscrNI; mtn_pose::serve_phase_c(c); }, kCtl, false,
              [&] { put_motion(O, N, true, true, -1, false, true); return Args{kMot, 0, 0, 0}; }, false, &caught_c_ni);
        chain("PhaseC(ulp)", 0x80419A9Cu, [](Cpu& c) { mtn_pose::serve_phase_c(c); c.mem_write32(0x81402400u + 0xACu, c.mem_read32(0x81402400u + 0xACu) ^ 1u); }, kCtl, false,
              [&] { put_motion(O, N, true, true, -1, false, true); return Args{kMot, 0, 0, 0}; }, false, &caught_c_ulp);
        chain("PhaseC(skip)", 0x80419A9Cu, [](Cpu&) {}, kCtl, false,
              [&] { put_motion(O, N, true, true, -1, false, true); return Args{kMot, 0, 0, 0}; }, false, &caught_c_skip);
        mtn_pose::set_phase_c_enabled(true);  // the native MotionUpdate calls the native phase directly (the original runs the real body)
        chain("MotionUpdate(phaseC)", mtn_pose::kMotionUpdate, mtn_pose::serve_motion_update, tree_n, false, [&] { put_motion(O, N, true, true, -1, false, true); return Args{kMot, 0, 0, 0}; });
        mtn_pose::set_phase_c_enabled(false);
        unsigned c_cases = 0, c_declined = 0;
        if (!std::getenv("MTN_POSE_ONLY") || std::string(std::getenv("MTN_POSE_ONLY")).find("PhaseC") != std::string::npos) {
            for (unsigned i = 0; i < 1000; ++i) {
                setup_world(O, N);
                put_motion(O, N, true, true, -1, true, true);
                const u32 idx = pick(4);
                fill(O, N, kTable, idx);
                fill(O, N, kMot + 0x20u, 1u + pick(4));
                fill(O, N, kChans + 0x130u * idx + 0x90u, O.memory.peek32(kChans + 0x130u * idx + 0x90u) | 4u);
                fill(O, N, kChans + 0x130u * idx + 0x120u, 0u);  // an active channel without a parameter block
                std::vector<u32> before;
                for (u32 a = kArena; a < kArena + kArenaSize; a += 4u) before.push_back(N.memory.peek32(a));
                for (u32 a = kScratchLo; a < kScratchHi; a += 4u) before.push_back(N.memory.peek32(a));
                N.prepare(0x80419A9Cu, false);
                N.cpu.gpr[3] = kMot;
                mtn_pose::set_phase_c_enabled(true);
                const bool declined = mtn_pose::serve_phase_c_hand_back(N.cpu);
                mtn_pose::set_phase_c_enabled(false);
                bool same = true;
                size_t w = 0;
                for (u32 a = kArena; a < kArena + kArenaSize && same; a += 4u) same = N.memory.peek32(a) == before[w++];
                for (u32 a = kScratchLo; a < kScratchHi && same; a += 4u) same = N.memory.peek32(a) == before[w++];
                ++c_cases;
                if (declined) ++c_declined;
                else report("PhaseC hand-back", "the hook ran on a state it cannot prove");
                if (!same) report("PhaseC hand-back", "the declined hook wrote memory");
                ++g_checked;
            }
            setup_world(O, N);
            put_motion(O, N, true, true, -1, true, true);
            N.prepare(0x80419A9Cu, false);
            N.cpu.gpr[3] = kMot;
            mtn_pose::set_phase_c_enabled(false);
            if (!mtn_pose::serve_phase_c_hand_back(N.cpu)) report("PhaseC hand-back", "the hook ran with the switch off");
        }
        if (!std::getenv("MTN_POSE_ONLY")) {
            require(caught_c_ni > 0, "negative control: the wrong FPSCR[NI] mode in phase C was not caught");
            require(caught_c_ulp > 0, "negative control: a one-ulp-off phase C time was not caught");
            require(caught_c_skip > 0, "negative control: a skipped phase C was not caught");
        }
        // hand-back: a state the phase cannot prove (a bone index above 3, or a first active channel with a partner) or the switch off must
        // leave the hook declining with nothing written
        unsigned d_cases = 0, d_declined = 0;
        if (!std::getenv("MTN_POSE_ONLY") || std::string(std::getenv("MTN_POSE_ONLY")).find("PhaseD") != std::string::npos) {
            for (unsigned i = 0; i < 2000; ++i) {
                setup_world(O, N);
                put_motion(O, N, true, true, -1);
                if (i & 1u) {  // an index above 3 first in the table
                    fill(O, N, kTable, 5u + pick(3));
                    fill(O, N, kMot + 0x20u, 1u + pick(4));
                } else {       // the first active channel gets a partner
                    const u32 idx = pick(4);
                    fill(O, N, kTable, idx);
                    fill(O, N, kMot + 0x20u, 1u + pick(4));
                    fill(O, N, kChans + 0x130u * idx + 0x90u, O.memory.peek32(kChans + 0x130u * idx + 0x90u) | 4u);
                    fill(O, N, kPar2 + 4u * idx, pick(4));
                }
                std::vector<u32> before;
                for (u32 a = kArena; a < kArena + kArenaSize; a += 4u) before.push_back(N.memory.peek32(a));
                for (u32 a = kScratchLo; a < kScratchHi; a += 4u) before.push_back(N.memory.peek32(a));
                N.prepare(0x80418618u, false);
                N.cpu.gpr[3] = kMot;
                mtn_pose::set_phase_d_enabled(true);
                const bool declined = mtn_pose::serve_phase_d_hand_back(N.cpu);
                mtn_pose::set_phase_d_enabled(false);
                bool same = true;
                size_t w = 0;
                for (u32 a = kArena; a < kArena + kArenaSize && same; a += 4u) same = N.memory.peek32(a) == before[w++];
                for (u32 a = kScratchLo; a < kScratchHi && same; a += 4u) same = N.memory.peek32(a) == before[w++];
                ++d_cases;
                if (declined) ++d_declined;
                else report("PhaseD hand-back", "the hook ran on a state it cannot prove");
                if (!same) report("PhaseD hand-back", "the declined hook wrote memory");
                ++g_checked;
            }
            // switch off: declines without writing
            setup_world(O, N);
            put_motion(O, N, true, true);
            N.prepare(0x80418618u, false);
            N.cpu.gpr[3] = kMot;
            mtn_pose::set_phase_d_enabled(false);
            if (!mtn_pose::serve_phase_d_hand_back(N.cpu)) report("PhaseD hand-back", "the hook ran with the switch off");
        }
        if (!std::getenv("MTN_POSE_ONLY")) {
            require(caught_d_ni > 0, "negative control: the wrong FPSCR[NI] mode in phase D was not caught");
            require(caught_d_ulp > 0, "negative control: a one-ulp-off phase D result was not caught");
            require(caught_d_reset > 0, "negative control: a missing phase D flag reset was not caught");
        }
        // ---- round 7, candidate B: the clip start phase 0x8041A33C and the bind tree ----
        const auto b_world = [&](bool stub_b, bool timing = true) {
            put_motion(O, N, true, true, -1, true, timing);
            put_b_stubs(O, N, stub_b);
            put_clip_world(O, N);
            for (u32 k = 0; k < 4; ++k) {
                const u32 ch = kChans + 0x130u * k;
                fill(O, N, ch + 0x110u, (pick(5) ? (1u + pick(0x7FFEu)) << 16 : 0u) | (rnd() & 0xFFFFu));
                fill(O, N, ch + 0x94u, (pick(6) ? 1u : 0u) | (pick(2) ? 0x78u & rnd() : 0u) | (pick(4) == 0 ? 0x2000u : 0u) | (pick(2) ? 0x800u : 0u) | (pick(3) == 0 ? 0x1000u : 0u) |
                                           (pick(2) ? 2u : 0u) | (pick(2) ? 4u : 0u) | (rnd() & 0x7000u & ~0x2000u & ~0x1000u));
                fill(O, N, ch + 0x98u, pick(8) == 0 ? float_word() : f2w(float(unit() * 1.5 - 0.2)));
                fill(O, N, ch + 0x9Cu, pick(8) == 0 ? float_word() : f2w(float(unit() * 1.5 - 0.2)));
                fill(O, N, ch + 0x100u, value_word());
                fill(O, N, ch + 0x104u, value_word());
                fill(O, N, ch + 0x108u, value_word());
                fill(O, N, ch + 0x10Cu, value_word());
            }
        };
        const auto key_of = [&](u32 i) { return O.memory.peek32(i == 0 ? kHead : (i == 1 ? kDataB : kDataB + 0x80u)); };
        chain("RangeFlags", 0x8041BDE4u, mtn_pose::serve_range_flags, tree_n, true, [&] {
            const u32 a = kArena + 0x40u, b = kArena + 0x60u;
            for (u32 k = 0; k < 4; ++k) {
                fill(O, N, a + 4u * k, pick(6) == 0 ? float_word() : f2w(float(unit() * 3.0 - 1.0)));
                fill(O, N, b + 4u * k, pick(5) == 0 ? float_word() : (pick(2) ? f2w(float(unit() * 4.0 - 1.5)) : (pick(2) ? 0u : f2w(float((unit() - 0.5) * 2e-4)))));
            }
            return Args{a, b, 0, sf(pick(8) == 0 ? float_word() : f2w(float(unit() * 2.0 - 0.5))), sf(pick(8) == 0 ? float_word() : f2w(float(unit() * 2.0)))};
        });
        chain("FindNode", 0x8041E498u, mtn_pose::serve_find_node, tree_n, true, [&] {
            b_world(true);
            const u32 nodes[3] = {kNodeA, kOB, kOC};
            return Args{nodes[pick(3)], pick(5) == 0 ? 0x99u : key_of(pick(3)), 0, 0};
        });
        chain("ResetTracks", 0x8041C74Cu, mtn_pose::serve_reset_tracks, tree_n, false, [&] {
            b_world(true);
            const u32 nodes[3] = {kNodeA, kOB, kOC};
            return Args{nodes[pick(3)], pick(4), 0, 0};
        });
        chain("StateInit", 0x80414E88u, [](Cpu& c) { mtn_track::state_init(c, c.gpr[3], c.gpr[4]); }, tree_n, false, [&] {
            b_world(true);
            return Args{kArena + 0x1100u, kRecsB + 0x100u * pick(8), 0, 0};
        });
        chain("BindNodes", 0x8041C30Cu, mtn_pose::serve_bind_nodes, tree_n, false, [&] {
            b_world(true);
            return Args{kMot, kInfo + 0x2Cu, pick(4), sf(pick(6) == 0 ? float_word() : f2w(float(unit() * 2.0))), 0};
        });
        chain("ClipBind", 0x8041C1C0u, mtn_pose::serve_clip_bind, tree_n, true, [&] {
            b_world(true);
            return Args{kMot, pick(4), 1u + pick(0x7000u), 0, 0};
        });
        chain("PhaseB", 0x8041A33Cu, mtn_pose::serve_phase_b, tree_n, false, [&] { b_world(false); return Args{kMot, 0, 0, 0, 0}; });
        unsigned caught_b_ni = 0, caught_b_ulp = 0, caught_b_skip = 0;
        chain("PhaseB(ni)", 0x8041A33Cu, [](Cpu& c) { c.fpscr ^= kFpscrNI; mtn_pose::serve_phase_b(c); }, kCtl, false,
              [&] { b_world(false); return Args{kMot, 0, 0, 0, 0}; }, false, &caught_b_ni);
        chain("PhaseB(ulp)", 0x8041A33Cu, [](Cpu& c) { mtn_pose::serve_phase_b(c); c.mem_write32(0x81402400u + 0xA0u, c.mem_read32(0x81402400u + 0xA0u) ^ 1u); }, kCtl, false,
              [&] { b_world(false); return Args{kMot, 0, 0, 0, 0}; }, false, &caught_b_ulp);
        chain("PhaseB(skip)", 0x8041A33Cu, [](Cpu&) {}, kCtl, false, [&] { b_world(false); return Args{kMot, 0, 0, 0, 0}; }, false, &caught_b_skip);
        mtn_pose::set_phase_b_enabled(true);  // the native MotionUpdate calls the native phase directly (the original runs the real body)
        chain("MotionUpdate(phaseB)", mtn_pose::kMotionUpdate, mtn_pose::serve_motion_update, tree_n, false, [&] { b_world(false); return Args{kMot, 0, 0, 0, 0}; });
        mtn_pose::set_phase_b_enabled(false);
        unsigned b_cases = 0, b_declined = 0;
        if (!std::getenv("MTN_POSE_ONLY") || std::string(std::getenv("MTN_POSE_ONLY")).find("PhaseB") != std::string::npos) {
            for (unsigned i = 0; i < 1000; ++i) {
                setup_world(O, N);
                b_world(true);
                const u32 idx = pick(4);
                fill(O, N, kTable, idx);
                fill(O, N, kMot + 0x20u, 1u + pick(4));
                fill(O, N, kChans + 0x130u * idx + 0x94u, O.memory.peek32(kChans + 0x130u * idx + 0x94u) | 1u);
                fill(O, N, kChans + 0x130u * idx + 0x90u, O.memory.peek32(kChans + 0x130u * idx + 0x90u) | 4u);
                fill(O, N, kChans + 0x130u * idx + 0x120u, 0u);  // an active channel without a parameter block
                std::vector<u32> before;
                for (u32 a = kArena; a < kArena + kArenaSize; a += 4u) before.push_back(N.memory.peek32(a));
                for (u32 a = kScratchLo; a < kScratchHi; a += 4u) before.push_back(N.memory.peek32(a));
                for (u32 a = kBig; a < kBig + kBigSize; a += 4u) before.push_back(N.memory.peek32(a));
                N.prepare(0x8041A33Cu, false);
                N.cpu.gpr[3] = kMot;
                mtn_pose::set_phase_b_enabled(true);
                const bool declined = mtn_pose::serve_phase_b_hand_back(N.cpu);
                mtn_pose::set_phase_b_enabled(false);
                bool same = true;
                size_t w = 0;
                for (u32 a = kArena; a < kArena + kArenaSize && same; a += 4u) same = N.memory.peek32(a) == before[w++];
                for (u32 a = kScratchLo; a < kScratchHi && same; a += 4u) same = N.memory.peek32(a) == before[w++];
                for (u32 a = kBig; a < kBig + kBigSize && same; a += 4u) same = N.memory.peek32(a) == before[w++];
                ++b_cases;
                if (declined) ++b_declined;
                else report("PhaseB hand-back", "the hook ran on a state it cannot prove");
                if (!same) report("PhaseB hand-back", "the declined hook wrote memory");
                ++g_checked;
            }
            setup_world(O, N);
            b_world(true);
            N.prepare(0x8041A33Cu, false);
            N.cpu.gpr[3] = kMot;
            mtn_pose::set_phase_b_enabled(false);
            if (!mtn_pose::serve_phase_b_hand_back(N.cpu)) report("PhaseB hand-back", "the hook ran with the switch off");
        }
        if (!std::getenv("MTN_POSE_ONLY")) {
            require(caught_b_ni > 0, "negative control: the wrong FPSCR[NI] mode in phase B was not caught");
            require(caught_b_ulp > 0, "negative control: a one-ulp-off phase B channel float was not caught");
            require(caught_b_skip > 0, "negative control: a skipped phase B was not caught");
        }
        // ---- the fixed huge-time set (frame time / playback time at and beyond every wrap threshold, FLT_MAX, 1e30, 1e10, +-inf) ----
        // Random inputs stay off the huge range (see bounded_exotic_word); this set is explicit. Per case the channel is active with the wrap flag (2) or
        // without, a head-ok or plain motion, rate and frame time chosen so the playback time is the value under test.
        //  * TERMINATING cases (every finite time that the wrap loop brings back below the end / above the start in a finite number of iterations, the
        //    values just past each threshold, NaN, and every huge value with the wrap flag CLEAR, where the code clamps once): compared in full with a step
        //    budget of 400 M on the original; any of them that does not finish is a failure, never a skip.
        //  * NON-TERMINATING cases (wrap flag set and a time with `time - length == time` in single precision, or an infinity): the original's loop
        //    `while (cur >= end) cur = cur - (end - start)` (forward) / `while (cur <= start) cur = cur + (end - start)` (backward) makes no progress, so it
        //    runs for ever; the native loop does the same operations. Both are stopped at the same point (the original frozen when the loop test of
        //    0x8041A094 / A0E0 / A228 / A274 is reached for the cap+1 th time, the native stopped by the test hook at the same arrival) and the memory is
        //    compared; the time word must be unchanged between caps (no progress).
        struct HugeCase { const char* name; float time; float rate; float dt; bool wrap; bool head; bool infinite; bool heavy; };
        const float kInf = std::numeric_limits<float>::infinity(), kMax = std::numeric_limits<float>::max();
        const HugeCase huge_cases[] = {
            // forward, terminating
            {"end exactly", 1.5f, 1.0f, 0.0f, true, false, false, false}, {"end + 1 ulp", std::nextafter(1.5f, 2.0f), 1.0f, 0.0f, true, false, false, false},
            {"end * 1.0000001", 1.5f * 1.0000001f, 1.0f, 0.0f, true, true, false, false}, {"3 lengths past", 4.5f, 1.0f, 0.0f, true, true, false, false},
            {"100", 100.0f, 1.0f, 0.0f, true, false, false, false}, {"100 (head)", 100.0f, 1.0f, 0.0f, true, true, false, false},
            {"1e5", 1.0e5f, 1.0f, 0.0f, true, false, false, false}, {"1e5 (head)", 1.0e5f, 1.0f, 0.0f, true, true, false, false},
            {"1e6", 1.0e6f, 1.0f, 0.0f, true, false, false, true},
            {"NaN", std::numeric_limits<float>::quiet_NaN(), 1.0f, 0.0f, true, false, false, false}, {"NaN (head)", std::numeric_limits<float>::quiet_NaN(), 1.0f, 0.0f, true, true, false, false},
            // backward, terminating
            {"start exactly", 0.5f, -1.0f, 0.0f, true, false, false, false}, {"start - 1 ulp", std::nextafter(0.5f, 0.0f), -1.0f, 0.0f, true, false, false, false},
            {"-3 (head)", -3.0f, -1.0f, 0.0f, true, true, false, false}, {"-100", -100.0f, -1.0f, 0.0f, true, false, false, false},
            {"-1e5", -1.0e5f, -1.0f, 0.0f, true, false, false, false}, {"-1e5 (head)", -1.0e5f, -1.0f, 0.0f, true, true, false, false},
            {"-1e6", -1.0e6f, -1.0f, 0.0f, true, false, false, true},
            // the wrap flag clear: every huge value is clamped once
            {"FLT_MAX, no wrap", kMax, 1.0f, 0.0f, false, false, false, false}, {"1e30, no wrap", 1.0e30f, 1.0f, 0.0f, false, true, false, false},
            {"1e10, no wrap", 1.0e10f, 1.0f, 0.0f, false, false, false, false}, {"+inf, no wrap", kInf, 1.0f, 0.0f, false, false, false, false},
            {"-FLT_MAX, no wrap", -kMax, -1.0f, 0.0f, false, false, false, false}, {"-1e30, no wrap", -1.0e30f, -1.0f, 0.0f, false, true, false, false},
            {"-1e10, no wrap", -1.0e10f, -1.0f, 0.0f, false, false, false, false}, {"-inf, no wrap", -kInf, -1.0f, 0.0f, false, false, false, false},
            {"dt FLT_MAX, no wrap", 0.75f, 1.0f, kMax, false, false, false, false}, {"dt -FLT_MAX, no wrap", 0.75f, -1.0f, kMax, false, true, false, false},
            // forward, never terminating (absorption or infinity)
            {"FLT_MAX", kMax, 1.0f, 0.0f, true, false, true, false}, {"FLT_MAX (head)", kMax, 1.0f, 0.0f, true, true, true, false},
            {"1e30", 1.0e30f, 1.0f, 0.0f, true, false, true, false}, {"1e30 (head)", 1.0e30f, 1.0f, 0.0f, true, true, true, false},
            {"1e10", 1.0e10f, 1.0f, 0.0f, true, false, true, false}, {"1e10 (head)", 1.0e10f, 1.0f, 0.0f, true, true, true, false},
            {"2^26", 67108864.0f, 1.0f, 0.0f, true, false, true, false}, {"2^26 (head)", 67108864.0f, 1.0f, 0.0f, true, true, true, false},
            {"+inf", kInf, 1.0f, 0.0f, true, false, true, false}, {"+inf (head)", kInf, 1.0f, 0.0f, true, true, true, false},
            {"dt FLT_MAX", 0.75f, 1.0f, kMax, true, false, true, false}, {"dt FLT_MAX (head)", 0.75f, 1.0f, kMax, true, true, true, false},
            // backward, never terminating
            {"-FLT_MAX", -kMax, -1.0f, 0.0f, true, false, true, false}, {"-FLT_MAX (head)", -kMax, -1.0f, 0.0f, true, true, true, false},
            {"-1e30", -1.0e30f, -1.0f, 0.0f, true, false, true, false}, {"-1e30 (head)", -1.0e30f, -1.0f, 0.0f, true, true, true, false},
            {"-1e10", -1.0e10f, -1.0f, 0.0f, true, false, true, false}, {"-1e10 (head)", -1.0e10f, -1.0f, 0.0f, true, true, true, false},
            {"-2^26", -67108864.0f, -1.0f, 0.0f, true, false, true, false}, {"-2^26 (head)", -67108864.0f, -1.0f, 0.0f, true, true, true, false},
            {"-inf", -kInf, -1.0f, 0.0f, true, false, true, false}, {"-inf (head)", -kInf, -1.0f, 0.0f, true, true, true, false},
            {"rate*dt -> -FLT_MAX", 0.75f, -1.0f, kMax, true, false, true, false}, {"rate*dt -> -FLT_MAX (head)", 0.75f, -1.0f, kMax, true, true, true, false},
        };
        const auto huge_world = [&](const HugeCase& hc) {
            setup_world(O, N);
            put_motion(O, N, true, true, -1, false, true);  // real 0x80419A9C on both machines, the other phases stubbed
            fill(O, N, kMot + 0x20u, 1u);
            fill(O, N, kTable, 0u);
            fill(O, N, kTable + 4u, 0xFFFFFFFFu);
            const u32 ch = kChans;
            fill(O, N, ch + 0x90u, 4u | (hc.wrap ? 2u : 0u));
            fill(O, N, ch + 0x94u, 0u);
            fill(O, N, ch + 0x120u, kParams);
            fill(O, N, kParams, 0u);  // no parameter flag: straight to the playback time
            fill(O, N, ch + 0xA0u, f2w(0.5f));
            fill(O, N, ch + 0xA4u, f2w(1.5f));
            fill(O, N, ch + 0xA8u, f2w(hc.rate));
            fill(O, N, ch + 0xACu, f2w(hc.time));
            fill(O, N, kR13 + 0x2910u, f2w(hc.dt));
            fill(O, N, kHead + 4u, (1u << 16) | (hc.head ? 1u : 0u));
        };
        const auto prime = [&](u32 entry, bool ni) {
            O.prepare(entry, ni);
            N.prepare(entry, ni);
            for (unsigned r = 14; r < 32; ++r) {
                const u32 v = rnd();
                O.cpu.gpr[r] = N.cpu.gpr[r] = v;
                const u64 d = single_to_double_bits(rnd() | 0x3F000000u);
                O.cpu.fpr[r].bits[0] = N.cpu.fpr[r].bits[0] = d;
                O.cpu.fpr[r].bits[1] = N.cpu.fpr[r].bits[1] = single_to_double_bits(f2w(float(unit())));
            }
            O.cpu.gpr[3] = N.cpu.gpr[3] = kMot;
        };
        const auto same_memory = [&](std::string& detail) {
            for (u32 a = kArena; a < kArena + kArenaSize; a += 4u) if (O.memory.peek32(a) != N.memory.peek32(a)) { char t[120]; std::snprintf(t, sizeof t, "arena at %08X: original %08X native %08X", a, O.memory.peek32(a), N.memory.peek32(a)); detail = t; return false; }
            for (u32 a = kScratchLo; a < kScratchHi; a += 4u) if (O.memory.peek32(a) != N.memory.peek32(a)) { char t[120]; std::snprintf(t, sizeof t, "scratch at %08X: original %08X native %08X", a, O.memory.peek32(a), N.memory.peek32(a)); detail = t; return false; }
            for (u32 a = 0x81420000u; a < 0x81428000u; a += 4u) if (O.memory.peek32(a) != N.memory.peek32(a)) { char t[120]; std::snprintf(t, sizeof t, "clip memory at %08X: original %08X native %08X", a, O.memory.peek32(a), N.memory.peek32(a)); detail = t; return false; }
            return true;
        };
        struct HugeEntry { const char* name; u32 entry; Native fn; bool motion; };
        const HugeEntry huge_entries[] = {{"PhaseC", 0x80419A9Cu, mtn_pose::serve_phase_c, false}, {"MotionUpdate(phaseC)", mtn_pose::kMotionUpdate, mtn_pose::serve_motion_update, true}};
        unsigned huge_term_compared = 0, huge_term_failed = 0, huge_inf_cases = 0, huge_inf_compared = 0, huge_inf_failed = 0;
        const unsigned long long caps[] = {1ull, 3ull, 50ull};
        if (!std::getenv("MTN_POSE_ONLY") || std::string(std::getenv("MTN_POSE_ONLY")).find("Huge") != std::string::npos) {
            for (const HugeEntry& he : huge_entries) {
                for (const HugeCase& hc : huge_cases) {
                    for (unsigned ni = 0; ni < 2; ++ni) {
                        std::string label = std::string("huge ") + he.name + " " + hc.name;
                        if (he.motion) mtn_pose::set_phase_c_enabled(true);
                        if (!hc.infinite) {
                            huge_world(hc);
                            prime(he.entry, ni != 0);
                            g_step_budget = 400000000ul;
                            const bool finished = O.run_original();
                            g_step_budget = 3000000ul;
                            if (!finished) { char t[160]; std::snprintf(t, sizeof t, "a terminating case did not finish within 400 M steps on the original (pc %08X, lr %08X, r1 %08X)", O.cpu.pc, O.cpu.lr, O.cpu.gpr[1]); report(label.c_str(), t); ++huge_term_failed; mtn_pose::set_phase_c_enabled(false); continue; }
                            he.fn(N.cpu);
                            std::string detail;
                            if (!same_state(O, N, label.c_str(), ni != 0, detail, false)) { report(label.c_str(), detail); ++huge_term_failed; } else ++huge_term_compared;
                            ++g_checked;
                        } else {
                            ++huge_inf_cases;
                            u32 time_word[3] = {0, 0, 0};
                            for (unsigned k = 0; k < 3; ++k) {
                                huge_world(hc);
                                prime(he.entry, ni != 0);
                                const int frozen = O.run_original_frozen(caps[k]);
                                if (frozen != 1) { report(label.c_str(), "a case classified as non-terminating finished or ran out of budget before the cap"); ++huge_inf_failed; continue; }
                                mtn_pose::set_wrap_loop_cap(caps[k]);
                                he.fn(N.cpu);
                                mtn_pose::set_wrap_loop_cap(0);
                                std::string detail;
                                if (!same_memory(detail)) { report(label.c_str(), detail); ++huge_inf_failed; } else ++huge_inf_compared;
                                time_word[k] = N.memory.peek32(kChans + 0xACu);
                                ++g_checked;
                            }
                            if (time_word[0] != time_word[1] || time_word[1] != time_word[2]) { report(label.c_str(), "the playback time changed between caps: the loop is not stuck"); ++huge_inf_failed; }
                        }
                        if (he.motion) mtn_pose::set_phase_c_enabled(false);
                    }
                }
            }
        }
        // ---- negative controls ----
        unsigned caught_ni = 0, caught_ulp = 0, caught_flag = 0;
        chain("MatrixBlend(ni)", mtn_pose::kMatrixBlend, [](Cpu& c) { c.fpscr ^= kFpscrNI; mtn_pose::serve_matrix_blend(c); }, kCtl, true, [&] {
            put_matrix(O, N, kArena + 0x40u, true);
            put_matrix(O, N, kArena + 0x80u, true);
            return Args{kArena + 0xC0u, kArena + 0x40u, kArena + 0x80u, sf(f2w(float(unit())))};
        }, false, &caught_ni);
        chain("QuatToMatrix(ulp)", mtn_pose::kQuatToMatrix, [](Cpu& c) { mtn_pose::serve_quat_to_matrix(c); c.mem_write32(c.gpr[3] + 0x14u, c.mem_read32(c.gpr[3] + 0x14u) ^ 1u); },
              kCtl, false, [&] { put_quat(O, N, kArena + 0x40u, false); return Args{kArena + 0x80u, kArena + 0x40u, 0, 0}; }, false, &caught_ulp);
        chain("RotationPass(flag)", mtn_pose::kRotationPass, [](Cpu& c) { mtn_pose::serve_rotation_pass(c); c.mem_write32(0x805E23A0u + 0x200u, c.mem_read32(0x805E23A0u + 0x200u) ^ 1u); },
              kCtl, false, [&] { return pass_setup(true); }, false, &caught_flag);
        unsigned caught_fold_ni = 0, caught_fold_ulp = 0, caught_fold_flag = 0, caught_fold_skip = 0;
        chain("FoldRotationBank(ni)", mtn_pose::kFoldRotationBank, [](Cpu& c) { c.fpscr ^= kFpscrNI; mtn_pose::serve_fold_rotation_bank(c); }, kCtl, false,
              [&] { return bank_setup(true, false); }, false, &caught_fold_ni);
        chain("FoldTranslationBank(ulp)", mtn_pose::kFoldTranslationBank, [](Cpu& c) {
                  mtn_pose::serve_fold_translation_bank(c);
                  const u32 obj = c.mem_read32(0x805EC500u + 0x5F50u);
                  const u32 top = c.mem_read32(obj) + c.mem_read32(obj + 8u) * 0x30u;
                  c.mem_write32(top + 0xCu, c.mem_read32(top + 0xCu) ^ 1u); },
              kCtl, false, [&] { return bank_setup(true, false); }, false, &caught_fold_ulp);
        chain("FoldScaleBank(flag)", mtn_pose::kFoldScaleBank, [](Cpu& c) { mtn_pose::serve_fold_scale_bank(c); c.mem_write32(0x805E23A0u + 0x200u, c.mem_read32(0x805E23A0u + 0x200u) ^ 1u); },
              kCtl, false, [&] { return bank_setup(false, true); }, false, &caught_fold_flag);
        chain("FoldScaleCallback(skip)", mtn_pose::kFoldScaleCallback, [](Cpu&) {}, kCtl, false, [&] { return bank_setup(false, true); }, false, &caught_fold_skip);
        if (!std::getenv("MTN_POSE_ONLY")) require(caught_fold_ni > 0, "negative control: the wrong FPSCR[NI] mode in the rotation bank was not caught");
        if (!std::getenv("MTN_POSE_ONLY")) require(caught_fold_ulp > 0, "negative control: a one-ulp-off translation bank was not caught");
        if (!std::getenv("MTN_POSE_ONLY")) require(caught_fold_flag > 0, "negative control: a stale flag in the scale bank was not caught");
        if (!std::getenv("MTN_POSE_ONLY")) require(caught_fold_skip > 0, "negative control: a skipped scale callback was not caught");
        if (!std::getenv("MTN_POSE_ONLY")) require(caught_ni > 0, "negative control: the wrong FPSCR[NI] mode was not caught");
        if (!std::getenv("MTN_POSE_ONLY")) require(caught_ulp > 0, "negative control: a one-ulp-off matrix was not caught");
        if (!std::getenv("MTN_POSE_ONLY")) require(caught_flag > 0, "negative control: a stale flag was not caught");

        struct Range { const char* name; u32 begin, end; };
        const Range ranges[] = {{"NodeWalk", 0x804196F0u, 0x804198F0u}, {"RotationPass", 0x8041EC24u, 0x8041F174u}, {"TranslationPass", 0x8041F174u, 0x8041F470u},
                                {"ScalePass", 0x8041E5D0u, 0x8041E8ECu}, {"ScaleLengthsPass", 0x8041E8ECu, 0x8041EC24u},
                                {"EulerBone", 0x80420B9Cu, 0x80420D10u}, {"MatrixBlend", 0x8042167Cu, 0x804216F4u},
                                {"QuatSlerp", 0x80421774u, 0x8042193Cu}, {"MatrixToQuat", 0x8042193Cu, 0x80421BB0u},
                                {"QuatToMatrix", 0x80421BB0u, 0x80421C80u}, {"MatrixNormalize", 0x804121BCu, 0x80412378u},
                                {"FoldScaleCallback", 0x80420B28u, 0x80420B9Cu}, {"FoldTranslateCallback", 0x80420D10u, 0x80420D5Cu},
                                {"FoldScaleBank", 0x8041FA14u, 0x8041FDDCu}, {"FoldTranslationBank", 0x804202E0u, 0x80420740u},
                                {"FoldRotationBank", 0x8041FDDCu, 0x804202E0u},
                                {"MotionUpdate", 0x80416EF4u, 0x80416FE8u}, {"SamplerPick", 0x804198F0u, 0x80419A9Cu},
                                {"BonePass", 0x80419094u, 0x804196F0u}, {"MatrixLerp", 0x804215E0u, 0x8042167Cu},
                                {"TopTranslationOf", 0x80412520u, 0x80412598u},
                                {"PhaseD", 0x80418618u, 0x80419094u}, {"PhaseC", 0x80419A9Cu, 0x8041A33Cu},
                                {"PhaseB", 0x8041A33Cu, 0x8041A508u}, {"ClipStart", 0x8041ADACu, 0x8041AFACu}, {"ClipBind", 0x8041C1C0u, 0x8041C30Cu},
                                {"BindNodes", 0x8041C30Cu, 0x8041C74Cu}, {"ResetTracks", 0x8041C74Cu, 0x8041C8D4u}, {"FindNode", 0x8041E498u, 0x8041E5D0u},
                                {"StateInit", 0x80414E88u, 0x80415170u}, {"RangeFlags", 0x8041BDE4u, 0x8041BF20u}, {"RotateByTop", 0x804125CCu, 0x80412684u}};
        for (const Range& r : ranges) {
            unsigned hit = 0, total = 0;
            std::string missed;
            for (u32 a = r.begin; a < r.end; a += 4u) {
                ++total;
                if (g_visited.count(a)) ++hit;
                else if (missed.size() < 150) { char t[16]; std::snprintf(t, sizeof t, " %08X", a); missed += t; }
            }
            std::printf("coverage %-18s %u / %u original instructions executed; missed:%s\n", r.name, hit, total, missed.c_str());
        }
        {
            double total = 0.0;
            std::vector<std::pair<double, std::string>> rows;
            for (const auto& kv : g_chain_time) { rows.push_back({kv.second.original + kv.second.native, kv.first}); total += kv.second.original + kv.second.native; }
            std::sort(rows.rbegin(), rows.rend());
            std::printf("chain times (seconds: original interpreter + native, calls, skipped), total %.1f s\n", total);
            for (const auto& row : rows) {
                const ChainTime& t = g_chain_time[row.second];
                std::printf("  %-26s %8.1f s (original %8.1f + native %6.2f)  calls %7u  skipped %5u\n", row.second.c_str(), row.first, t.original, t.native, t.calls, t.skipped);
            }
        }
        std::printf("calls compared %d, skipped (original did not finish) %d\n", g_checked, g_skipped);
        for (const auto& kv : g_skipped_by) std::printf("  skipped in %s: %d\n", kv.first.c_str(), kv.second);
        if (g_failures != 0) {
            std::fprintf(stderr, "FAIL mtn pose contract: %d mismatches of %d calls\n", g_failures, g_checked);
            return 1;
        }
        std::printf("PASS mtn pose contract: %d calls compared (controls caught %u + %u + %u + fold %u + %u + %u + %u)\n", g_checked, caught_ni, caught_ulp, caught_flag,
                    caught_fold_ni, caught_fold_ulp, caught_fold_flag, caught_fold_skip);
        std::printf("tree controls caught %u + %u + %u\n", caught_tree_ni, caught_tree_ctx, caught_tree_pop);
        std::printf("huge-time set, terminating cases (step budget 400 M): compared %u, skipped 0, failed %u\n", huge_term_compared, huge_term_failed);
        std::printf("huge-time set, non-terminating cases (original frozen at caps 1, 3, 50 loop-test arrivals, native stopped at the same arrival): %u cases, compared %u, failed %u\n", huge_inf_cases, huge_inf_compared, huge_inf_failed);
        std::printf("phase B controls caught %u + %u + %u; hand-back %u declined of %u\n", caught_b_ni, caught_b_ulp, caught_b_skip, b_declined, b_cases);
        std::printf("phase C controls caught %u + %u + %u; hand-back %u declined of %u\n", caught_c_ni, caught_c_ulp, caught_c_skip, c_declined, c_cases);
        std::printf("phase D controls caught %u + %u + %u; hand-back cases %u declined of %u\n", caught_d_ni, caught_d_ulp, caught_d_reset, d_declined, d_cases);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL mtn pose contract: %s\n", error.what());
        return 1;
    }
}
