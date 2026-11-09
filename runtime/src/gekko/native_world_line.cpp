// RenderWare's world line query (fn_804421D8), native. Contract and the original's structure:
// include/gekko/native_world_line.h; the algorithm word by word: docs/engine/WORLD_COLLISION.md.

#include "gekko/native_world_line.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <unordered_map>
#include <vector>

#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/ppc_ops.h"
#include "gekko/recomp.h"

namespace gekko {

namespace {

constexpr u32 kQuery = 0x804421D8u;
constexpr u32 kCallbackLink = 0x80442BA4u;  // the bctrl's return address
constexpr u32 kFrame = 0x640u;
constexpr u32 kMsrFp = 0x2000u;
// FPSCR control bits the owner requires clear: the enables and RN (round to nearest). NI (0x4) is the game's own mode
// and is honoured by the core's helpers, the same ones the translated body calls.
constexpr u32 kFpscrControlNotNi = 0xFBu;
constexpr u32 kTriangle = 8u;      // the RpCollisionTriangle in the frame: r1 + 8
constexpr u32 kPseudoNode = 0x64u; // the collision tree's root descriptor in the frame
constexpr u32 kWorldBase = 0x22Cu; // the world stack's first entry (r3 at a normal return)

// .sdata2 constants at r2 + 0x1880.. (0x805FC000): 1, 0, the tolerance scale, the determinant epsilon, the edge slack.
constexpr u32 kOne = 0x3F800000u, kZero = 0u, kTolScale = 0x358637BDu, kDetEps = 0x322BCC77u, kEdge = 0x3727C5ACu;

using ppc::fps_add;
using ppc::fps_div;
using ppc::fps_madd;
using ppc::fps_msub;
using ppc::fps_mul;
using ppc::fps_round;
using ppc::fps_sub;

using V = u64;  // an FPR's double-format bits, exactly as the translated code holds the value

GEKKO_ALWAYS_INLINE V lfs(u32 word) noexcept { return single_to_double_bits(word); }
GEKKO_ALWAYS_INLINE u32 stfs(V v) noexcept { return double_to_single_bits_flush(v); }
GEKKO_ALWAYS_INLINE double val(V v) noexcept { return bits_to_double(v); }
GEKKO_ALWAYS_INLINE V fneg(V v) noexcept { return v ^ 0x8000000000000000ull; }
// The original's |x|: fcmpo x, 0 then x or fneg x (a NaN takes the fneg).
GEKKO_ALWAYS_INLINE V magnitude(V x) noexcept { return val(x) >= 0.0 ? x : fneg(x); }

// A stack entry as the frame holds it: the node, then t0 and t1 as stored singles.
struct Entry {
    u32 node;
    u32 t0, t1;
};

}  // namespace

NativeWorldLine::NativeWorldLine(Cpu& cpu) noexcept : cpu_(cpu) {
#if !defined(GEKKO_SHIPPING)
    if (const char* v = std::getenv("GEKKO_NATIVE_WORLD_LINE"); v != nullptr && v[0] == '0')
        enabled_ = false;
    if (const char* v = std::getenv("GEKKO_VERIFY_WORLD_LINE"); v != nullptr && v[0] != '\0')
        verify_every_ = std::strtoull(v, nullptr, 10);
    census_ = std::getenv("GEKKO_WORLD_LINE_CENSUS") != nullptr;
#endif
}

NativeWorldLine::~NativeWorldLine() {
    if (stats_.calls != 0u && (verify_every_ != 0u || std::getenv("GEKKO_WORLD_LINE_STATS") != nullptr))
        std::fprintf(stderr,
                     "[world-line] calls %llu served %llu declined %llu sectors %llu triangles %llu hits %llu stopped %llu"
                     " | verified %llu differed %llu unverifiable %llu\n",
                     static_cast<unsigned long long>(stats_.calls), static_cast<unsigned long long>(stats_.served),
                     static_cast<unsigned long long>(stats_.declined), static_cast<unsigned long long>(stats_.sectors),
                     static_cast<unsigned long long>(stats_.triangles), static_cast<unsigned long long>(stats_.hits),
                     static_cast<unsigned long long>(stats_.stopped), static_cast<unsigned long long>(stats_.verified),
                     static_cast<unsigned long long>(stats_.differed),
                     static_cast<unsigned long long>(stats_.unverifiable));
    if (!census_rows_.empty()) {
        std::vector<std::pair<std::array<u32, 4>, std::array<u64, 4>>> rows(census_rows_.begin(), census_rows_.end());
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second[0] > b.second[0]; });
        std::fprintf(stderr, "[world-line census] callback,filter,ret2,ret3,calls,hits,stops,triangles\n");
        for (const auto& [k, v] : rows)
            std::fprintf(stderr, "[world-line census] %08X,%08X,%08X,%08X,%llu,%llu,%llu,%llu\n", k[0], k[1], k[2], k[3],
                         static_cast<unsigned long long>(v[0]), static_cast<unsigned long long>(v[1]),
                         static_cast<unsigned long long>(v[2]), static_cast<unsigned long long>(v[3]));
    }
}

void NativeWorldLine::install() {
    if (!enabled_)
        return;
    cpu_.install_hle(kQuery, [this](Cpu& c) { return serve(c); }, "RpCollisionWorldForAllIntersections (line), native");
}

bool NativeWorldLine::serve(Cpu& c) {
    if (reference_)
        return false;
    ++stats_.calls;
    if ((c.msr() & kMsrFp) == 0u || (c.fpscr & kFpscrControlNotNi) != 0u) {
        if (++stats_.declined <= 4u && verify_every_ != 0u)
            std::fprintf(stderr, "[world-line] declined: msr %08X fpscr %08X\n", c.msr(), c.fpscr);
        return false;
    }
    if (census_) {
        // Return addresses up the guest stack: each frame's back chain, the LR its callee saved at back chain + 4.
        const u32 f1 = c.mem_read32(c.gpr[1]), f2 = f1 != 0u ? c.mem_read32(f1) : 0u;
        const std::array<u32, 4> key{c.gpr[5], c.mem_read32(c.gpr[13] + 0x2BF0u), f2 != 0u ? c.mem_read32(f2 + 4u) : 0u,
                                     f2 != 0u && c.mem_read32(f2) != 0u ? c.mem_read32(c.mem_read32(f2) + 4u) : 0u};
        const u64 hits = stats_.hits, stops = stats_.stopped, tris = stats_.triangles;
        const bool served = verify_every_ != 0u && depth_ == 0u && stats_.calls % verify_every_ == 0u ? verify(c) : run(c);
        auto& row = census_rows_[key];
        ++row[0];
        row[1] += stats_.hits - hits;
        row[2] += stats_.stopped - stops;
        row[3] += stats_.triangles - tris;
        return served;
    }
    if (verify_every_ != 0u && depth_ == 0u && stats_.calls % verify_every_ == 0u)
        return verify(c);
    return run(c);
}

bool NativeWorldLine::run(Cpu& c) {
    ++depth_;
    ++stats_.served;
    const u32 world = c.gpr[3], line = c.gpr[4], callback = c.gpr[5], data = c.gpr[6];
    const u32 caller_sp = c.gpr[1], link = c.lr;
    const u32 sp = caller_sp - kFrame;
    // The prologue's visible words: the caller's LR in its own frame's slot (at or above r1), the back chain.
    c.mem_write32(caller_sp + 4u, link);
    c.mem_write32(sp, caller_sp);

    // The line, its delta, the inverse delta (0 for a zero component) and the per-axis tolerance
    // 1e-6 x max(|start|, |end|) - all captured once, as the original copies them into its frame.
    // Values the original keeps in its frame go through stfs / lfs exactly where it stores and reloads them: the
    // start as raw words; the delta, inverse and tolerance stored as singles. x's inverse is computed from the delta
    // still in its register, y's and z's from the reloaded words; x's and y's tolerance are reloaded before the
    // scale, z's is rounded (frsp) in its register.
    V S[3], D[3], inv[3], tol[3];
    for (u32 i = 0; i < 3u; ++i) {
        S[i] = lfs(c.mem_read32(line + 4u * i));
        const V e = lfs(c.mem_read32(line + 0xCu + 4u * i));
        const V delta = fps_sub(c, e, S[i]);
        D[i] = lfs(stfs(delta));
        const V divisor = i == 0u ? delta : D[i];
        V r = lfs(kZero);
        if (!(val(lfs(kZero)) == val(divisor)))
            r = fps_div(c, lfs(kOne), divisor);
        inv[i] = lfs(stfs(r));
        const V t = val(magnitude(S[i])) >= val(magnitude(e)) ? magnitude(S[i]) : magnitude(e);
        tol[i] = lfs(stfs(fps_mul(c, lfs(kTolScale), i == 2u ? fps_round(c, t) : lfs(stfs(t)))));
    }

    // One clip step of either tree: the entry's interval [t0, t1] against the planes (lo + tol) and (hi - tol) along
    // the node's axis. Returns which children the interval reaches; the caller applies the original's writes.
    const u32 plugin = c.mem_read32(c.gpr[13] + 0x5FC8u);
    std::vector<Entry> world_stack;
    world_stack.reserve(64);
    world_stack.push_back({c.mem_read32(world + 0x1Cu), kZero, kOne});
    std::vector<Entry> tree_stack;
    tree_stack.reserve(32);
    u32 result = sp + kWorldBase;
    bool stop = false;

    for (;;) {
        // The world tree down to a sector, from the top entry.
        for (;;) {
            Entry& top = world_stack.back();
            const u32 node = top.node;
            const s32 type = static_cast<s32>(c.mem_read32(node));
            if (type < 0)
                break;
            const u32 axis = static_cast<u32>(type) >> 2;  // 0, 4, 8: x, y, z
            const V t1 = lfs(top.t1), t0 = lfs(top.t0);
            const V d = D[axis], s = S[axis], k = tol[axis];
            const bool ascending = val(d) > val(lfs(kZero));
            const V exit = fps_madd(c, t1, d, s);
            const V entry = fps_madd(c, t0, d, s);
            const V f6 = fps_sub(c, lfs(c.mem_read32(node + 0x14u)), k);
            const V f5 = fps_add(c, lfs(c.mem_read32(node + 0x10u)), k);
            const u32 left = node + 8u, right = node + 0xCu;  // the child pointer words
            u32 near_child, far_child;
            bool clip_new, clip_old;
            V new_delta, old_delta;
            if (ascending) {
                if (val(f6) > val(exit)) {
                    top.node = c.mem_read32(left);
                    continue;
                }
                if (val(f5) < val(entry)) {
                    top.node = c.mem_read32(right);
                    continue;
                }
                near_child = left, far_child = right;
                clip_new = val(f5) < val(exit);
                clip_old = val(f6) > val(entry);
                new_delta = clip_new ? fps_sub(c, f5, exit) : 0u;
                old_delta = clip_old ? fps_sub(c, f6, entry) : 0u;
            } else {
                if (val(f5) < val(exit)) {
                    top.node = c.mem_read32(right);
                    continue;
                }
                if (val(f6) > val(entry)) {
                    top.node = c.mem_read32(left);
                    continue;
                }
                near_child = right, far_child = left;
                clip_new = val(f6) > val(exit);
                clip_old = val(f5) < val(entry);
                new_delta = clip_new ? fps_sub(c, f6, exit) : 0u;
                old_delta = clip_old ? fps_sub(c, f5, entry) : 0u;
            }
            // Straddles: a new entry (the near child, the old interval) goes on top; the old entry becomes the far
            // child; each interval is clipped where the plane cuts it.
            Entry pushed{c.mem_read32(near_child), top.t0, top.t1};
            if (clip_new)
                pushed.t1 = stfs(fps_madd(c, inv[axis], new_delta, lfs(pushed.t1)));
            top.node = c.mem_read32(far_child);
            if (clip_old)
                top.t0 = stfs(fps_madd(c, inv[axis], old_delta, lfs(top.t0)));
            world_stack.push_back(pushed);
        }

        // A world sector: its collision data, then its collision tree.
        const Entry leaf = world_stack.back();
        const u32 sector = leaf.node;
        const u32 coll = c.mem_read32(sector + plugin);
        if (coll != 0u) {
            ++stats_.sectors;
            const u32 triangles = c.mem_read32(sector + 4u), vertices = c.mem_read32(sector + 8u);
            // The root descriptor, written into the frame as the original does: an interior node 0 when the tree
            // has nodes, else a leaf of all (+0x1C's low byte) triangles from 0.
            const u32 pseudo = sp + kPseudoNode;
            c.mem_write16(pseudo + 2u, 0u);
            c.mem_write8(pseudo + 1u, c.mem_read16(coll + 0x1Eu) != 0u ? 0xFFu : static_cast<u8>(c.mem_read16(coll + 0x1Cu)));
            tree_stack.clear();
            tree_stack.push_back({pseudo, stfs(lfs(leaf.t0)), stfs(lfs(leaf.t1))});
            for (;;) {
                Entry& top = tree_stack.back();
                const u32 desc = top.node;
                const u32 count = c.mem_read8(desc + 1u);
                if (count == 0xFFu) {
                    const u32 T = c.mem_read32(coll + 0x20u) + (static_cast<u32>(c.mem_read16(desc + 2u)) << 4);
                    const V t1 = lfs(top.t1), t0 = lfs(top.t0);
                    const u32 axis = (c.mem_read8(T) & 0x0Cu) >> 2;
                    const V d = D[axis], s = S[axis], k = tol[axis];
                    const V f7 = fps_sub(c, lfs(c.mem_read32(T + 0xCu)), k);
                    const bool ascending = val(d) > val(lfs(kZero));
                    const V exit = fps_madd(c, t1, d, s);
                    const V entry = fps_madd(c, t0, d, s);
                    const V f4 = fps_add(c, lfs(c.mem_read32(T + 4u)), k);
                    u32 near_child, far_child;
                    bool clip_new, clip_old;
                    V new_delta, old_delta;
                    if (ascending) {
                        if (val(f7) > val(exit)) {
                            top.node = T;
                            continue;
                        }
                        if (val(f4) < val(entry)) {
                            top.node = T + 8u;
                            continue;
                        }
                        near_child = T, far_child = T + 8u;
                        clip_new = val(f4) < val(exit);
                        clip_old = val(f7) > val(entry);
                        new_delta = clip_new ? fps_sub(c, f4, exit) : 0u;
                        old_delta = clip_old ? fps_sub(c, f7, entry) : 0u;
                    } else {
                        if (val(f4) < val(exit)) {
                            top.node = T + 8u;
                            continue;
                        }
                        if (val(f7) > val(entry)) {
                            top.node = T;
                            continue;
                        }
                        near_child = T + 8u, far_child = T;
                        clip_new = val(f7) > val(exit);
                        clip_old = val(f4) < val(entry);
                        new_delta = clip_new ? fps_sub(c, f7, exit) : 0u;
                        old_delta = clip_old ? fps_sub(c, f4, entry) : 0u;
                    }
                    Entry pushed{near_child, top.t0, top.t1};
                    if (clip_new)
                        pushed.t1 = stfs(fps_madd(c, inv[axis], new_delta, lfs(pushed.t1)));
                    top.node = far_child;
                    if (clip_old)
                        top.t0 = stfs(fps_madd(c, inv[axis], old_delta, lfs(top.t0)));
                    tree_stack.push_back(pushed);
                    continue;
                }

                // A leaf: triangles [start, start + count) (through the remap table when there is one).
                const u32 start = c.mem_read16(desc + 2u);
                const s32 end = static_cast<s32>(start + count);
                for (u32 r18 = start; static_cast<s32>(r18 & 0xFFFFu) < end && !stop; ++r18) {
                    ++stats_.triangles;
                    const u32 remap = c.mem_read32(coll + 0x24u);
                    const u32 index = remap != 0u ? c.mem_read16(remap + ((r18 << 1) & 0x1FFFEu)) : r18;
                    const u32 tri = triangles + ((index << 3) & 0x7FFF8u);
                    const u32 p0 = vertices + c.mem_read16(tri) * 0xCu;
                    const u32 pb = vertices + c.mem_read16(tri + 4u) * 0xCu;
                    const u32 pc = vertices + c.mem_read16(tri + 2u) * 0xCu;
                    const V p0z = lfs(c.mem_read32(p0 + 8u)), p0x = lfs(c.mem_read32(p0)), p0y = lfs(c.mem_read32(p0 + 4u));
                    const V e1z = fps_sub(c, lfs(c.mem_read32(pb + 8u)), p0z);
                    const V e1x = fps_sub(c, lfs(c.mem_read32(pb)), p0x);
                    const V e1y = fps_sub(c, lfs(c.mem_read32(pb + 4u)), p0y);
                    const V pcx = lfs(c.mem_read32(pc)), pcz = lfs(c.mem_read32(pc + 8u));
                    const V f10a = fps_mul(c, D[0], e1z);
                    const V f12a = fps_mul(c, D[2], e1y);
                    const V f11a = fps_mul(c, D[1], e1x);
                    const V e2y = fps_sub(c, lfs(c.mem_read32(pc + 4u)), p0y);
                    const V f23 = fps_msub(c, D[2], e1x, f10a);
                    const V e2x = fps_sub(c, pcx, p0x);
                    const V f24 = fps_msub(c, D[1], e1z, f12a);
                    V det = fps_mul(c, e2y, f23);
                    const V e2z = fps_sub(c, pcz, p0z);
                    const V f22a = fps_msub(c, D[0], e1y, f11a);
                    det = fps_madd(c, e2x, f24, det);
                    det = fps_madd(c, e2z, f22a, det);
                    if (!(val(det) > val(lfs(kDetEps))))
                        continue;
                    const V sy = fps_sub(c, S[1], p0y);
                    const V sx = fps_sub(c, S[0], p0x);
                    V f5 = fps_mul(c, sy, f23);
                    const V lower = fps_mul(c, lfs(kEdge), fneg(det));
                    const V sz = fps_sub(c, S[2], p0z);
                    f5 = fps_madd(c, sx, f24, f5);
                    const V upper = fps_sub(c, det, lower);
                    const V u = fps_madd(c, sz, f22a, f5);
                    if (!(val(u) >= val(lower)) || !(val(u) <= val(upper)))
                        continue;
                    V f6 = fps_mul(c, sx, e2z);
                    const V f11b = fps_mul(c, sz, e2y);
                    V f5b = fps_mul(c, sy, e2x);
                    const V q13 = fps_msub(c, sz, e2x, f6);
                    f6 = fps_msub(c, sy, e2z, f11b);
                    f5b = fps_msub(c, sx, e2y, f5b);
                    V v = fps_mul(c, D[1], q13);
                    v = fps_madd(c, D[0], f6, v);
                    v = fps_madd(c, D[2], f5b, v);
                    if (!(val(v) >= val(lower)))
                        continue;
                    if (!(val(fps_add(c, u, v)) <= val(upper)))
                        continue;
                    V t = fps_mul(c, e1y, q13);
                    t = fps_madd(c, e1x, f6, t);
                    t = fps_madd(c, e1z, f5b, t);
                    if (!(val(t) >= val(lower)) || !(val(t) <= val(upper)))
                        continue;
                    t = fps_div(c, t, det);

                    // The hit: the RpCollisionTriangle at r1 + 8, the normal scaled by the table reciprocal sqrt.
                    ++stats_.hits;
                    // RpCollisionTriangle: +0 normal, +0xC point (the first vertex's words), +0x18 index,
                    // +0x1C / +0x20 / +0x24 the vertices (tri + 0, tri + 2, tri + 4).
                    const u32 tri_at = sp + kTriangle;
                    c.mem_write32(tri_at + 0x0Cu, c.mem_read32(p0));
                    c.mem_write32(tri_at + 0x10u, c.mem_read32(p0 + 4u));
                    c.mem_write32(tri_at + 0x14u, c.mem_read32(p0 + 8u));
                    c.mem_write32(tri_at + 0x18u, index & 0xFFFFu);
                    c.mem_write32(tri_at + 0x1Cu, p0);
                    c.mem_write32(tri_at + 0x20u, pc);
                    c.mem_write32(tri_at + 0x24u, pb);
                    const V ax = fps_sub(c, lfs(c.mem_read32(pc)), lfs(c.mem_read32(p0)));
                    const V bx = fps_sub(c, lfs(c.mem_read32(pb)), lfs(c.mem_read32(p0)));
                    const V by = fps_sub(c, lfs(c.mem_read32(pb + 4u)), lfs(c.mem_read32(p0 + 4u)));
                    const V ay = fps_sub(c, lfs(c.mem_read32(pc + 4u)), lfs(c.mem_read32(p0 + 4u)));
                    const V az = fps_sub(c, lfs(c.mem_read32(pc + 8u)), lfs(c.mem_read32(p0 + 8u)));
                    const V bz = fps_sub(c, lfs(c.mem_read32(pb + 8u)), lfs(c.mem_read32(p0 + 8u)));
                    const V m0 = fps_mul(c, ay, bx);
                    const V m2 = fps_mul(c, az, by);
                    const V m1 = fps_mul(c, ax, bz);
                    const V nz = fps_msub(c, ax, by, m0);
                    const V nx = fps_msub(c, ay, bz, m2);
                    const V ny = fps_msub(c, az, bx, m1);
                    const V zz = fps_mul(c, nz, nz);
                    c.mem_write32(tri_at + 8u, stfs(nz));
                    const V xx = fps_mul(c, nx, nx);
                    const V yy = fps_mul(c, ny, ny);
                    c.mem_write32(tri_at + 0u, stfs(nx));
                    c.mem_write32(tri_at + 4u, stfs(ny));
                    const V len2 = fps_add(c, zz, fps_add(c, xx, yy));
                    // 0x804833E4: RenderWare's reciprocal square root from its table.
                    u32 word = stfs(len2);
                    if (word != 0u) {
                        word += 0x800u;
                        const u32 table =
                            c.mem_read32(c.mem_read32(c.gpr[13] + 0x615Cu) + c.mem_read32(c.gpr[13] + 0x6140u) + 4u);
                        word = (((~word) >> 1) & 0x3FC00000u) + c.mem_read32(table + ((word >> 10) & 0x3FFCu));
                    }
                    const V scale = lfs(word);
                    c.mem_write32(tri_at + 0u, stfs(fps_mul(c, lfs(c.mem_read32(tri_at + 0u)), scale)));
                    c.mem_write32(tri_at + 4u, stfs(fps_mul(c, lfs(c.mem_read32(tri_at + 4u)), scale)));
                    c.mem_write32(tri_at + 8u, stfs(fps_mul(c, lfs(c.mem_read32(tri_at + 8u)), scale)));

                    // callback(r3 line, r4 sector, r5 &triangle, f1 t, r6 data), from the original's frame.
                    c.gpr[1] = sp;
                    c.gpr[3] = line;
                    c.gpr[4] = sector;
                    c.gpr[5] = tri_at;
                    c.gpr[6] = data;
                    c.gpr[12] = callback;
                    c.ctr = callback;
                    c.fpr[1].bits[0] = t;
                    recomp::call_indirect(c, callback, kCallbackLink);
                    if (c.exiting())
                        c.finish_transfers();
                    c.gpr[1] = sp;
                    if (c.exiting()) {
                        --depth_;
                        return true;  // the machine is stopping: unwind
                    }
                    if (c.gpr[3] == 0u) {
                        ++stats_.stopped;
                        stop = true;
                        result = 0u;
                    }
                }
                if (stop || tree_stack.size() <= 1u)
                    break;
                tree_stack.pop_back();
            }
        }
        if (stop || world_stack.size() <= 1u)
            break;
        world_stack.pop_back();
    }

    c.gpr[1] = caller_sp;
    c.gpr[3] = result;
    c.lr = link;
    --depth_;
    return true;
}

// Every n-th served call: the native run, rolled back, then the guest body from the same state; contract registers
// and every byte either run wrote anywhere except the current thread's dead stack (its stack end up to the caller's
// r1) compared; the guest's result kept.
bool NativeWorldLine::verify(Cpu& c) {
    const RecompiledCode* code = c.recompiled();
    const RecompiledCode::Range* body = code != nullptr ? code->find_range(kQuery) : nullptr;
    GuestMemory& memory = c.guest_memory();
    if (body == nullptr || memory.journaling())
        return run(c);
    struct Regs {
        u32 gpr[32], lr, cr, fpscr;
        u64 fpr[32];
    };
    auto take = [&](Regs& r) {
        std::memcpy(r.gpr, c.gpr, sizeof(r.gpr));
        r.lr = c.lr;
        r.cr = c.cr;
        r.fpscr = c.fpscr;
        for (u32 i = 0; i < 32u; ++i)
            r.fpr[i] = c.fpr[i].bits[0];
    };
    auto put = [&](const Regs& r) {
        std::memcpy(c.gpr, r.gpr, sizeof(r.gpr));
        c.lr = r.lr;
        c.cr = r.cr;
        c.fpscr = r.fpscr;
        for (u32 i = 0; i < 32u; ++i)
            c.fpr[i].bits[0] = r.fpr[i];
    };
    Regs before, native, guest;
    take(before);
    const u32 pc = c.pc;
    const u32 caller_sp = before.gpr[1];
    // The dead stack: the current OSThread's (0x800000E4) stack end (+0x308) up to the caller's r1.
    const u32 thread = c.mem_read32(0x800000E4u);
    const u32 dead_lo = thread != 0u ? c.mem_read32(thread + 0x308u) : caller_sp;
    const auto live = [&](u32 a) { return a < dead_lo || a >= caller_sp; };

    memory.journal_begin();
    run(c);
    take(native);
    std::unordered_map<u32, u8> native_bytes;  // every live byte the native run wrote: its final value
    bool unverifiable = memory.journal_overflowed() || memory.journal_touched_device();
    for (const GuestMemory::JournalEntry& e : memory.journal())
        for (u32 i = 0; i < e.size; ++i)
            if (live(e.address + i))
                native_bytes[e.address + i] = c.mem_read8(e.address + i);
    memory.journal_rollback();
    memory.journal_end();
    put(before);

    memory.journal_begin();
    reference_ = true;
    body->at(c, kQuery);
    c.finish_transfers();
    reference_ = false;
    take(guest);
    std::unordered_map<u32, u8> guest_bytes;
    unverifiable = unverifiable || memory.journal_overflowed() || memory.journal_touched_device();
    std::unordered_map<u32, u8> previous;  // the value before either run, for a byte only one run wrote
    for (const GuestMemory::JournalEntry& e : memory.journal())
        for (u32 i = 0; i < e.size; ++i)
            if (live(e.address + i)) {
                guest_bytes[e.address + i] = c.mem_read8(e.address + i);
                previous.emplace(e.address + i, static_cast<u8>(e.previous >> (8u * (e.size - 1u - i))));
            }
    memory.journal_end();
    c.pc = pc;
    if (unverifiable || c.lr != before.lr || dead_lo > caller_sp) {
        ++stats_.unverifiable;
        return true;
    }
    ++stats_.verified;
    u32 first = 0u;
    bool differs = false;
    for (u32 i = 1; i < 32u && !differs; ++i)
        if ((i <= 3u || i >= 13u) && native.gpr[i] != guest.gpr[i])
            differs = true, first = i;
    for (u32 i = 14; i < 32u && !differs; ++i)
        if (native.fpr[i] != guest.fpr[i])
            differs = true, first = 100u + i;
    if (native.lr != guest.lr || ((native.cr ^ guest.cr) & 0x00FFF000u) != 0u || ((native.fpscr ^ guest.fpscr) & 0xFFu) != 0u)
        differs = true;
    u32 byte_at = 0u;
    for (const auto& [address, value] : native_bytes) {
        const auto g = guest_bytes.find(address);
        // a byte the guest did not write keeps the value before both runs: the native run's write must equal it
        const u8 guest_value = g != guest_bytes.end() ? g->second : c.mem_read8(address);
        if (guest_value != value) {
            differs = true;
            byte_at = address;
            break;
        }
    }
    for (const auto& [address, value] : guest_bytes) {
        if (native_bytes.count(address) == 0u && previous[address] != value) {
            differs = true;
            byte_at = address;
            break;
        }
    }
    if (differs && ++stats_.differed <= 8u)
        std::fprintf(stderr, "[world-line] native differs from the guest body: register %u, byte %08X (r1 %08X)\n", first,
                     byte_at, caller_sp);
    return true;
}

}  // namespace gekko
