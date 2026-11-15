// Stage 1b of the S1 target owner: the sphere / view-cone / range filters and the nearest scan (0x801E82C4 / 0x801E84C0 / 0x801E8528 /
// 0x801E879C). Hand models of the functions with floating-point work, branches, loops and indirect calls (included by
// native_s1_target2.cpp after native_s1_target2_models.inc; not scanned by the liveness tool: the address literals are the original's
// return addresses and callee addresses, stored or called as values). Each is written from the disassembly named in its comment; the
// floating-point operations are the interpreter's own instruction code (ppc::fps_*, fp_compare), so values and status bits are the
// original's. The virtual call of the original (the Target's position, slot 2) is devirtualised: the proof pass in serve() checked the
// vtable word of every target (0x8053ECB0) and that its slot 2 is 0x801E76FC.

inline u64 FL(u32 w) noexcept { return single_to_double_bits(w); }          // lfs
inline u32 FS(u64 b) noexcept { return double_to_single_bits_flush(b); }    // stfs
// fcmpo cr0, a, b: the condition code (8 = LT, 4 = GT, 2 = EQ, 1 = unordered); FPSCR as the instruction leaves it.
static u32 fcmpo0(Ctx& x, u64 a, u64 b) {
    ppc::fp_compare(x.c, 0u, a, b, true);
    return (x.c.cr >> 28) & 0xFu;
}

// ---- the vector leaves (no frame) ------------------------------------------------------------------------------------------------------
// 0x8040DDC4 Sub: out = a - b (all six operands are loaded before the first store).
static void leaf_sub(Ctx& x, u32 out, u32 a, u32 b) {
    Cpu& c = x.c;
    const u64 ax = FL(x.rd32(a)), bx = FL(x.rd32(b)), ay = FL(x.rd32(a + 4u)), by = FL(x.rd32(b + 4u)), az = FL(x.rd32(a + 8u)), bz = FL(x.rd32(b + 8u));
    const u64 rx = ppc::fps_sub(c, ax, bx), ry = ppc::fps_sub(c, ay, by), rz = ppc::fps_sub(c, az, bz);
    x.wr32(out, FS(rx));
    x.wr32(out + 4u, FS(ry));
    x.wr32(out + 8u, FS(rz));
}
// 0x8040DF7C Length2: f1 = z*z + (x*x + y*y), each step rounded to single.
static u64 leaf_len2(Ctx& x, u32 v) {
    Cpu& c = x.c;
    const u64 vx = FL(x.rd32(v)), vy = FL(x.rd32(v + 4u)), vz = FL(x.rd32(v + 8u));
    const u64 x2 = ppc::fps_mul(c, vx, vx), y2 = ppc::fps_mul(c, vy, vy), z2 = ppc::fps_mul(c, vz, vz);
    return ppc::fps_add(c, z2, ppc::fps_add(c, x2, y2));
}
// 0x8040DFE0 Dot: f1 = fmadds(z, z', fmadds(x, x', y * y')).
static u64 leaf_dot(Ctx& x, u32 a, u32 b) {
    Cpu& c = x.c;
    const u64 ay = FL(x.rd32(a + 4u)), by = FL(x.rd32(b + 4u)), ax = FL(x.rd32(a)), bx = FL(x.rd32(b)), az = FL(x.rd32(a + 8u)), bz = FL(x.rd32(b + 8u));
    u64 t = ppc::fps_mul(c, ay, by);
    t = ppc::fps_madd(c, ax, bx, t);
    return ppc::fps_madd(c, az, bz, t);
}

// 0x8000FF04 sqrt (leaf with a 0x10-byte frame that is only written back-chain deep): f1 -> result bits. Only the 0 / NaN arm stores (stfs f1, 8(r1)).
static u64 sqrt_model(Ctx& x, u32 sp_in, u64 f1) {
    const u32 sp = sp_in - 0x10u;
    x.wr32(sp, sp_in);
    const u64 zero_s = FL(x.rd32(x.r2 - 0x7F30u));
    if (fcmpo0(x, f1, zero_s) & 4u) return se_math::detail::guest_sqrt(x.c, f1);
    const u64 zero_d = x.rd64(x.r2 - 0x7F08u);
    if (fcmpo0(x, f1, zero_d) & 8u) return FL(x.rd32(0x805EED78u));
    const u32 word = FS(f1);
    x.wr32(sp + 8u, word);
    const u32 e = word & 0x7F800000u;
    if (e == 0x7F800000u && (word & 0x007FFFFFu) != 0u) return FL(x.rd32(0x805EED78u));  // fpclassify: NaN -> the NaN constant
    return f1;
}
// 0x8000FEE4 (stwu, stw lr; calls 0x8000FF04) -> f1.
static u64 h_8000FEE4_model(Ctx& x, u32 sp_in, u32 lr_in, u64 f1) {
    const u32 sp = enter(x, sp_in, 0x10u, lr_in);
    return sqrt_model(x, sp, f1);
}
// 0x8040DFA0 Length(r3 = v) -> f1.
static u64 length_model(Ctx& x, u32 sp_in, u32 lr_in, u32 v) {
    const u32 sp = enter(x, sp_in, 0x10u, lr_in);
    return h_8000FEE4_model(x, sp, 0x8040DFD0u, leaf_len2(x, v));
}
// 0x8040DEC4 Normalize(r3 = v) in place: v / |v|, or the zero vector when |v| < 1e-6.
static void normalize_model(Ctx& x, u32 sp_in, u32 lr_in, u32 v, const Regs& R, u32 f1_in) {
    const u32 sp = enter(x, sp_in, 0x10u, lr_in);
    x.wr32(sp + 0xCu, R.r[31]);
    Regs Rc = R;
    Rc.r[31] = v;
    const u64 len = length_model(x, sp, 0x8040DEDCu, v);
    const u64 eps = FL(x.rd32(x.r2 + 0x167Cu));
    if ((fcmpo0(x, len, eps) & 8u) != 0u) {  // len < eps: copy the zero vector (0x800091D0 from 0x805E1860)
        F_800091D0(x, sp, 0x8040DEF8u, v, 0x805E1860u, 0, 0, 0, 0, Rc, f1_in);
        return;
    }
    for (u32 k = 0; k < 3u; ++k) {
        const u64 q = ppc::fps_div(x.c, FL(x.rd32(v + 4u * k)), len);
        x.wr32(v + 4u * k, FS(q));
    }
}
// 0x8040DF38 NormalizeCopy(r3 = out, r4 = in): in copied to a local, normalised, copied out.
HDR(H_8040DF38) {
    UNUSED;
    const u32 sp = enter(x, sp_in, 0x20u, lr_in);
    x.wr32(sp + 0x1Cu, R.r[31]);
    Regs Rc = R;
    Rc.r[31] = a0;
    F_800091EC(x, sp, 0x8040DF54u, sp + 8u, a1, 0, 0, 0, 0, Rc, f1_in);
    normalize_model(x, sp, 0x8040DF5Cu, sp + 8u, Rc, f1_in);
    F_800091EC(x, sp, 0x8040DF68u, a0, sp + 8u, 0, 0, 0, 0, Rc, f1_in);
    return a0;
}

// The devirtualised Target slot 2 (0x801E76FC: out = [px + 8 ..]); the guard ran in the proof pass.
static u32 target_pos(Ctx& x, u32 sp, u32 site, u32 out, u32 px, u32 arg, const Regs& Rc, u32 f1_in) {
    return F_801E76FC(x, sp, site, out, px, arg, 0, 0, 0, Rc, f1_in);
}

// ---- generic loops: find_if / remove_copy_if over the invoke chain `Inv` --------------------------------------------------------------------
using ModelFn = u32 (*)(Ctx&, u32, u32, u32, u32, u32, u32, u32, u32, const Regs&, u32);
// find_if(r3 = first, r4 = last, r5 = pred): stwu -0x20, stmw r29, 0x14; Inv(pred, &slot) at LR `site`.
static u32 find_if_model(Ctx& x, u32 sp_in, u32 lr_in, u32 first, u32 last, u32 pred, const Regs& R, u32 f1_in, ModelFn inv, u32 site) {
    const u32 sp = enter(x, sp_in, 0x20u, lr_in);
    for (u32 k = 0; k < 3u; ++k) x.wr32(sp + 0x14u + 4u * k, R.r[29 + k]);
    u32 cur = first;
    for (; cur != last; cur += 4u) {
        Regs Rc = R;
        Rc.r[29] = cur; Rc.r[30] = last; Rc.r[31] = pred;
        if ((inv(x, sp, site, pred, cur, 0, 0, 0, 0, Rc, f1_in) & 0xFFu) != 0u) break;
        if (x.dead) break;
    }
    return cur;
}
// remove_copy_if(r3 = first, r4 = last, r5 = out, r6 = pred): stwu -0x20, stmw r28, 0x10.
static u32 remove_copy_if_model(Ctx& x, u32 sp_in, u32 lr_in, u32 first, u32 last, u32 out, u32 pred, const Regs& R, u32 f1_in, ModelFn inv, u32 site) {
    const u32 sp = enter(x, sp_in, 0x20u, lr_in);
    for (u32 k = 0; k < 4u; ++k) x.wr32(sp + 0x10u + 4u * k, R.r[28 + k]);
    u32 cur = first;
    for (; cur != last; cur += 4u) {
        Regs Rc = R;
        Rc.r[28] = cur; Rc.r[29] = last; Rc.r[30] = out; Rc.r[31] = pred;
        if ((inv(x, sp, site, pred, cur, 0, 0, 0, 0, Rc, f1_in) & 0xFFu) == 0u) {
            x.wr32(out, x.rd32(cur));
            out += 4u;
        }
        if (x.dead) break;
    }
    return out;
}

// ---- the sphere filter 0x801E82C4 (binder 0x801EAAA4, predicate 0x801E90F0) ----------------------------------------------------------------
// 0x801E9CAC invoke (r3 = &functor + 4, r4 = &byte, r5 = functor, r6 = &slot): fn(*slot, functor + 8, functor + 0x14, f1 = [functor + 0x20]).
HDR(H_801E9CAC) {
    UNUSED;
    const u32 sp = enter(x, sp_in, 0x20u, lr_in);
    for (u32 k = 0; k < 6u; ++k) x.wr32(sp + 8u + 4u * k, R.r[26 + k]);
    Regs Rc = R;
    Rc.r[26] = a0; Rc.r[28] = a3; Rc.r[27] = a2;
    const u32 slot = x.rd32(a3);
    Rc.r[29] = slot;
    const u32 r30 = F_801E96D4(x, sp, 0x801E9CD8u, a3, a0 + 0x1Cu, 0, 0, 0, 0, Rc, f1_in);
    Rc.r[30] = r30;
    const u32 r31 = F_801E8794(x, sp, 0x801E9CE8u, a3, a0 + 0x10u, 0, 0, 0, 0, Rc, f1_in);
    Rc.r[31] = r31;
    const u32 vec1 = F_801E8794(x, sp, 0x801E9CF8u, a3, a0 + 4u, 0, 0, 0, 0, Rc, f1_in);
    const u32 fn = x.rd32(a2);
    const u32 holder = x.rd32(slot);
    const u32 f1 = x.rd32(r30);
    if (fn == 0x801E90F0u) return H_801E90F0(x, sp, 0x801E9D14u, holder, vec1, r31, 0, 0, 0, Rc, f1);
    x.dead = true;
    return 0u;
}
// 0x801E90F0 predicate (r3 = holder, r4 = centre, r5 = axis, f1 = threshold): the direction to the target, normalised, dotted with the axis; <= threshold removes... (returns 1 when not greater).
HDR(H_801E90F0) {
    UNUSED;
    const u32 sp = enter(x, sp_in, 0x50u, lr_in);
    x.wr64(sp + 0x48u, R.f0[31]);
    Regs Rc = R;
    Rc.f0[31] = FL(f1_in);
    x.wr32(sp + 0x40u, R.r[30]);
    x.wr32(sp + 0x44u, R.r[31]);
    Rc.r[30] = a1; Rc.r[31] = a2;
    H_800578A0(x, sp, 0x801E911Cu, sp + 8u, a0, 0, 0, 0, 0, Rc, f1_in);
    const u32 px = x.rd32(sp + 8u);
    target_pos(x, sp, 0x801E9138u, sp + 0x1Cu, px, a1, Rc, f1_in);
    leaf_sub(x, sp + 0x28u, sp + 0x1Cu, a1);
    F_800091EC(x, sp, 0x801E9154u, sp + 0x34u, sp + 0x28u, 0, 0, 0, 0, Rc, f1_in);
    H_80057850(x, sp, 0x801E9160u, sp + 8u, 0xFFFFFFFFu, 0, 0, 0, 0, Rc, f1_in);
    H_8040DF38(x, sp, 0x801E916Cu, sp + 0x10u, sp + 0x34u, 0, 0, 0, 0, Rc, f1_in);
    const u64 dot = leaf_dot(x, a2, sp + 0x10u);
    return (fcmpo0(x, dot, Rc.f0[31]) & 4u) != 0u ? 0u : 1u;
}
// 0x801E9B1C remove_if(r3 = first, r4 = last, r5 = &functor): find_if (0x801E9C14), a functor copy (0x801E9B7C), remove_copy_if (0x801EB300).
HDR(H_801E9C14) { UNUSED; return find_if_model(x, sp_in, lr_in, a0, a1, a2, R, f1_in, F_801E9C6C, 0x801E9C4Cu); }
HDR(H_801EB300) { UNUSED; return remove_copy_if_model(x, sp_in, lr_in, a0, a1, a2, a3, R, f1_in, F_801E9C6C, 0x801EB330u); }
HDR(H_801E9B1C) {
    UNUSED;
    const u32 sp = enter(x, sp_in, 0x40u, lr_in);
    for (u32 k = 0; k < 3u; ++k) x.wr32(sp + 0x34u + 4u * k, R.r[29 + k]);
    Regs Rc = R;
    Rc.r[30] = a1; Rc.r[31] = a2;
    const u32 found = H_801E9C14(x, sp, 0x801E9B38u, a0, a1, a2, 0, 0, 0, Rc, f1_in);
    if (found == a1) return found;
    Rc.r[29] = found;
    F_801E9B7C(x, sp, 0x801E9B54u, sp + 8u, a2, 0, 0, 0, 0, Rc, f1_in);
    return H_801EB300(x, sp, 0x801E9B68u, found + 4u, a1, found, sp + 8u, 0, 0, Rc, f1_in);
}

// ---- the view-cone filter 0x801E84C0 (binder 0x801EA638 / 0x801EA698, predicate 0x801E9324) ----------------------------------------------
// 0x801E9814 invoke (r3 = &functor + 4, r4 = &byte, r5 = functor, r6 = &slot): fn(*slot, functor + 8).
HDR(H_801E9814) {
    UNUSED;
    const u32 sp = enter(x, sp_in, 0x10u, lr_in);
    x.wr32(sp + 8u, R.r[30]);
    x.wr32(sp + 0xCu, R.r[31]);
    Regs Rc = R;
    Rc.r[30] = a2;
    const u32 slot = x.rd32(a3);
    Rc.r[31] = slot;
    const u32 vec = F_801E8794(x, sp, 0x801E983Cu, a3, a0 + 4u, 0, 0, 0, 0, Rc, f1_in);
    const u32 fn = x.rd32(a2);
    const u32 holder = x.rd32(slot);
    if (fn == 0x801E9324u) return H_801E9324(x, sp, 0x801E9850u, holder, vec, 0, 0, 0, 0, Rc, f1_in);
    x.dead = true;
    return 0u;
}
// 0x801E9324 predicate (r3 = holder, r4 = centre): only a target with class bit 1; returns centre - position, normalised, dotted with [target + 0x14] < [target + 0x24].
HDR(H_801E9324) {
    UNUSED;
    const u32 sp = enter(x, sp_in, 0x70u, lr_in);
    x.wr64(sp + 0x60u, R.f0[31]);
    x.psq_st(sp + 0x68u, R.f0[31], R.f1[31]);
    x.wr64(sp + 0x50u, R.f0[30]);
    x.psq_st(sp + 0x58u, R.f0[30], R.f1[30]);
    x.wr32(sp + 0x4Cu, R.r[31]);
    Regs Rc = R;
    Rc.r[31] = a1;
    H_800578A0(x, sp, 0x801E9354u, sp + 8u, a0, 0, 0, 0, 0, Rc, f1_in);
    F_80057810(x, sp, 0x801E9360u, sp + 0x10u, sp + 8u, 0, 0, 0, 0, Rc, f1_in);
    H_80057850(x, sp, 0x801E936Cu, sp + 8u, 0xFFFFFFFFu, 0, 0, 0, 0, Rc, f1_in);
    const u32 px = x.rd32(sp + 0x10u);
    u32 result = 0u;
    if ((x.rd32(px + 0x30u) & 2u) == 0u) {
        H_80057850(x, sp, 0x801E938Cu, sp + 0x10u, 0xFFFFFFFFu, 0, 0, 0, 0, Rc, f1_in);
    } else {
        target_pos(x, sp, 0x801E93B0u, sp + 0x18u, px, a1, Rc, f1_in);
        leaf_sub(x, sp + 0x24u, a1, sp + 0x18u);
        H_8040DF38(x, sp, 0x801E93CCu, sp + 0x30u, sp + 0x24u, 0, 0, 0, 0, Rc, f1_in);
        F_800091EC(x, sp, 0x801E93D8u, sp + 0x3Cu, sp + 0x30u, 0, 0, 0, 0, Rc, f1_in);
        const u64 dot = leaf_dot(x, sp + 0x3Cu, px + 0x14u);
        Rc.f0[31] = dot;
        Rc.f0[30] = FL(x.rd32(px + 0x24u));
        H_80057850(x, sp, 0x801E9400u, sp + 0x10u, 0xFFFFFFFFu, 0, 0, 0, 0, Rc, f1_in);
        result = (fcmpo0(x, Rc.f0[31], Rc.f0[30]) & 8u) != 0u ? 1u : 0u;
    }
    x.requant30 = true;
    x.requant31 = true;
    return result;
}
HDR(H_801E977C) { UNUSED; return find_if_model(x, sp_in, lr_in, a0, a1, a2, R, f1_in, F_801E97D4, 0x801E97B4u); }
HDR(H_801EB1C8) { UNUSED; return remove_copy_if_model(x, sp_in, lr_in, a0, a1, a2, a3, R, f1_in, F_801E97D4, 0x801EB1F8u); }
// 0x801E96DC remove_if(r3 = first, r4 = last, r5 = &functor).
HDR(H_801E96DC) {
    UNUSED;
    const u32 sp = enter(x, sp_in, 0x30u, lr_in);
    for (u32 k = 0; k < 3u; ++k) x.wr32(sp + 0x24u + 4u * k, R.r[29 + k]);
    Regs Rc = R;
    Rc.r[30] = a1; Rc.r[31] = a2;
    const u32 found = H_801E977C(x, sp, 0x801E96F8u, a0, a1, a2, 0, 0, 0, Rc, f1_in);
    if (found == a1) return found;
    Rc.r[29] = found;
    F_801E973C(x, sp, 0x801E9714u, sp + 8u, a2, 0, 0, 0, 0, Rc, f1_in);
    return H_801EB1C8(x, sp, 0x801E9728u, found + 4u, a1, found, sp + 8u, 0, 0, Rc, f1_in);
}

// ---- the range filter 0x801E8528 (pass 1: binder 0x801EA468 + for_each 0x801E85F8 with 0x801E94E8; pass 2: 0x801EA574, 0x801E9580 with 0x801E9430) ----
// 0x801E8744 invoke (pass 1): fn(*slot, functor + 8).
HDR(H_801E8744) {
    UNUSED;
    const u32 sp = enter(x, sp_in, 0x10u, lr_in);
    x.wr32(sp + 8u, R.r[30]);
    x.wr32(sp + 0xCu, R.r[31]);
    Regs Rc = R;
    Rc.r[30] = a2;
    const u32 slot = x.rd32(a3);
    Rc.r[31] = slot;
    const u32 vec = F_801E8794(x, sp, 0x801E876Cu, a3, a0 + 4u, 0, 0, 0, 0, Rc, f1_in);
    const u32 fn = x.rd32(a2);
    const u32 holder = x.rd32(slot);
    if (fn == 0x801E94E8u) return H_801E94E8(x, sp, 0x801E8780u, holder, vec, 0, 0, 0, 0, Rc, f1_in);
    x.dead = true;
    return 0u;
}
// 0x801E94E8 pass-1 predicate (r3 = holder, r4 = centre): [target + 4] = |position - centre|^2 (a store into the target).
HDR(H_801E94E8) {
    UNUSED;
    const u32 sp = enter(x, sp_in, 0x40u, lr_in);
    x.wr32(sp + 0x38u, R.r[30]);
    x.wr32(sp + 0x3Cu, R.r[31]);
    Regs Rc = R;
    Rc.r[30] = a0; Rc.r[31] = a1;
    H_800578A0(x, sp, 0x801E950Cu, sp + 0x10u, a0, 0, 0, 0, 0, Rc, f1_in);
    H_800578A0(x, sp, 0x801E9518u, sp + 8u, a0, 0, 0, 0, 0, Rc, f1_in);
    const u32 px = x.rd32(sp + 8u);
    target_pos(x, sp, 0x801E9534u, sp + 0x18u, px, a1, Rc, f1_in);
    leaf_sub(x, sp + 0x24u, sp + 0x18u, a1);
    const u32 tgt = x.rd32(sp + 0x10u);
    Rc.r[31] = tgt;
    const u64 d2 = leaf_len2(x, sp + 0x24u);
    x.wr32(tgt + 4u, FS(d2));
    H_80057850(x, sp, 0x801E9560u, sp + 8u, 0xFFFFFFFFu, 0, 0, 0, 0, Rc, f1_in);
    H_80057850(x, sp, 0x801E956Cu, sp + 0x10u, 0xFFFFFFFFu, 0, 0, 0, 0, Rc, f1_in);
    return a0;
}
// 0x801E85F8 for_each(r3 = out functor, r4 = first, r5 = last, r6 = functor): 0x801E8704 per element, then the functor copied out (0x801E8654).
HDR(H_801E85F8) {
    UNUSED;
    const u32 sp = enter(x, sp_in, 0x20u, lr_in);
    for (u32 k = 0; k < 4u; ++k) x.wr32(sp + 0x10u + 4u * k, R.r[28 + k]);
    Regs Rc = R;
    Rc.r[28] = a0; Rc.r[30] = a2; Rc.r[31] = a3;
    for (u32 cur = a1; cur != a2; cur += 4u) {
        Rc.r[29] = cur;
        F_801E8704(x, sp, 0x801E8628u, a3, cur, 0, 0, 0, 0, Rc, f1_in);
        if (x.dead) return 0u;
    }
    Rc.r[29] = a2;
    return F_801E8654(x, sp, 0x801E8640u, a0, a3, 0, 0, 0, 0, Rc, f1_in);
}
// 0x801E9680 invoke (pass 2): fn(*slot, functor + 8, f1 = [functor + 8]).
HDR(H_801E9680) {
    UNUSED;
    const u32 sp = enter(x, sp_in, 0x10u, lr_in);
    x.wr32(sp + 8u, R.r[30]);
    x.wr32(sp + 0xCu, R.r[31]);
    Regs Rc = R;
    Rc.r[30] = a2;
    const u32 slot = x.rd32(a3);
    Rc.r[31] = slot;
    const u32 vec = F_801E96D4(x, sp, 0x801E96A8u, a3, a0 + 4u, 0, 0, 0, 0, Rc, f1_in);
    const u32 fn = x.rd32(a2);
    const u32 holder = x.rd32(slot);
    if (fn == 0x801E9430u) return H_801E9430(x, sp, 0x801E96C0u, holder, vec, 0, 0, 0, 0, Rc, x.rd32(vec));
    x.dead = true;
    return 0u;
}
// 0x801E9430 pass-2 predicate (r3 = holder, f1 = range squared): [target + 4] > f1.
HDR(H_801E9430) {
    UNUSED;
    const u32 sp = enter(x, sp_in, 0x30u, lr_in);
    x.wr64(sp + 0x20u, R.f0[31]);
    x.psq_st(sp + 0x28u, R.f0[31], R.f1[31]);
    x.wr32(sp + 0x1Cu, R.r[31]);
    Regs Rc = R;
    Rc.f0[31] = FL(f1_in);
    H_800578A0(x, sp, 0x801E9458u, sp + 8u, a0, 0, 0, 0, 0, Rc, f1_in);
    const u32 px = x.rd32(sp + 8u);
    const u64 d2 = FL(x.rd32(px + 4u));
    const u32 gt = (fcmpo0(x, d2, Rc.f0[31]) & 4u) != 0u ? 1u : 0u;
    Rc.r[31] = gt;
    H_80057850(x, sp, 0x801E9478u, sp + 8u, 0xFFFFFFFFu, 0, 0, 0, 0, Rc, f1_in);
    x.requant31 = true;
    return gt;
}
HDR(H_801E95E8) { UNUSED; return find_if_model(x, sp_in, lr_in, a0, a1, a2, R, f1_in, F_801E9640, 0x801E9620u); }
HDR(H_801EB160) { UNUSED; return remove_copy_if_model(x, sp_in, lr_in, a0, a1, a2, a3, R, f1_in, F_801E9640, 0x801EB190u); }
// 0x801E9580 remove_if(r3 = first, r4 = last, r5 = &functor {fn, byte, float}): find, a 3-word functor copy on the stack, remove_copy_if.
HDR(H_801E9580) {
    UNUSED;
    const u32 sp = enter(x, sp_in, 0x20u, lr_in);
    x.wr32(sp + 0x18u, R.r[30]);
    x.wr32(sp + 0x1Cu, R.r[31]);
    Regs Rc = R;
    Rc.r[30] = a1; Rc.r[31] = a2;
    const u32 found = H_801E95E8(x, sp, 0x801E959Cu, a0, a1, a2, 0, 0, 0, Rc, f1_in);
    if (found == a1) return found;
    const u32 w0 = x.rd32(a2), w1 = x.rd32(a2 + 4u), w2 = x.rd32(a2 + 8u);
    x.wr32(sp + 8u, w0);
    x.wr32(sp + 0xCu, w1);
    x.wr32(sp + 0x10u, w2);
    return H_801EB160(x, sp, 0x801E95D4u, found + 4u, a1, found, sp + 8u, 0, 0, Rc, f1_in);
}
// 0x801E8528 RangeFilter(r3 = query, r4 = centre, f1 = range): pass 1 stores the squared distance into every target, pass 2 removes those beyond range^2.
HDR(H_801E8528) {
    UNUSED;
    const u32 sp = enter(x, sp_in, 0x80u, lr_in);
    x.wr64(sp + 0x70u, R.f0[31]);
    x.psq_st(sp + 0x78u, R.f0[31], R.f1[31]);
    x.wr32(sp + 0x6Cu, R.r[31]);
    const u32 policy = x.rd8(x.r13 + 0x4608u);
    Regs Rc = R;
    Rc.r[31] = a0;
    Rc.f0[31] = FL(f1_in);
    x.wr8(sp + 0xCu, policy);
    const u32 vec = F_800091EC(x, sp, 0x801E8558u, sp + 0x28u, a1, 0, 0, 0, 0, Rc, f1_in);
    F_801EA468(x, sp, 0x801E8570u, sp + 0x34u, 0x801E94E8u, sp + 0xCu, vec, 0, 0, Rc, f1_in);
    const u32 begin = F_801E7CF0(x, sp, 0x801E8578u, a0 + 4u, 0, 0, 0, 0, 0, Rc, f1_in);
    const u32 end = x.rd32(a0 + 0x10u);
    H_801E85F8(x, sp, 0x801E858Cu, sp + 0x48u, begin, end, sp + 0x34u, 0, 0, Rc, f1_in);
    const u32 policy2 = x.rd8(x.r13 + 0x4608u);
    const u32 f1sq = FS(ppc::fps_mul(x.c, Rc.f0[31], Rc.f0[31]));
    x.wr8(sp + 8u, policy2);
    F_801EA574(x, sp, 0x801E85ACu, sp + 0x10u, 0x801E9430u, sp + 8u, 0, 0, 0, Rc, f1sq);
    const u32 w0 = x.rd32(sp + 0x10u), w1 = x.rd32(sp + 0x14u), w2 = x.rd32(sp + 0x18u);
    x.wr32(sp + 0x1Cu, w0);
    x.wr32(sp + 0x20u, w1);
    x.wr32(sp + 0x24u, w2);
    const u32 begin2 = F_801E7CF0(x, sp, 0x801E85CCu, a0 + 4u, 0, 0, 0, 0, 0, Rc, f1_in);
    const u32 end2 = x.rd32(a0 + 0x10u);
    const u32 nend = H_801E9580(x, sp, 0x801E85D8u, begin2, end2, sp + 0x1Cu, 0, 0, 0, Rc, f1_in);
    x.wr32(a0 + 0x10u, nend);
    x.requant31 = true;
    return nend;
}

// ---- the nearest scan 0x801E879C -------------------------------------------------------------------------------------------------------------
// 0x80009E4C shared_count assign (r3 = &dst count slot, r4 = &src count slot): a different block: its use + 1, release the old block, store.
HDR(H_80009E4C) {
    UNUSED;
    const u32 sp = enter(x, sp_in, 0x10u, lr_in);
    x.wr32(sp + 8u, R.r[30]);
    x.wr32(sp + 0xCu, R.r[31]);
    Regs Rc = R;
    Rc.r[30] = a0;
    const u32 pn = x.rd32(a1);
    Rc.r[31] = pn;
    if (pn != x.rd32(a0)) {
        if (pn != 0u) x.wr32(pn + 4u, x.rd32(pn + 4u) + 1u);
        const u32 old = x.rd32(a0);
        if (old != 0u) H_8000945C(x, sp, 0x80009E94u, old, 0, 0, 0, 0, 0, Rc, f1_in);
        x.wr32(a0, pn);
    }
    return a0;
}
// 0x801E879C Nearest(r3 = out handle, r4 = query, r5 = point): the entry of the query whose target is nearest the point (strictly less wins, the first on ties).
HDR(H_801E879C) {
    UNUSED;
    const u32 sp = enter(x, sp_in, 0x80u, lr_in);
    x.wr64(sp + 0x70u, R.f0[31]);
    x.psq_st(sp + 0x78u, R.f0[31], R.f1[31]);
    for (u32 k = 0; k < 5u; ++k) x.wr32(sp + 0x5Cu + 4u * k, R.r[27 + k]);
    Regs Rc = R;
    Rc.r[27] = a0; Rc.r[28] = a1; Rc.r[29] = a2;
    const u32 handle = sp + 0x18u;
    F_80057980(x, sp, 0x801E87C8u, handle, 0, 0, 0, 0, 0, Rc, f1_in);
    u32 first = F_801E7CF0(x, sp, 0x801E87D0u, a1 + 4u, 0, 0, 0, 0, 0, Rc, f1_in);
    const u32 last = x.rd32(a1 + 0x10u);
    if (first == last) {
        F_80061168(x, sp, 0x801E87E4u, a0, 0, 0, 0, 0, 0, Rc, f1_in);
        x.requant31 = true;
        return H_80057850(x, sp, 0x801E87F0u, handle, 0xFFFFFFFFu, 0, 0, 0, 0, Rc, f1_in);
    }
    u32 r31 = F_801E7CF0(x, sp, 0x801E87FCu, a1 + 4u, 0, 0, 0, 0, 0, Rc, f1_in);
    Rc.r[31] = r31;
    u32 best = x.rd32(r31);
    Rc.r[30] = best;
    H_800578A0(x, sp, 0x801E8810u, sp + 0x10u, best, 0, 0, 0, 0, Rc, f1_in);
    F_8008A310(x, sp, 0x801E881Cu, handle, sp + 0x10u, 0, 0, 0, 0, Rc, f1_in);
    H_80057850(x, sp, 0x801E8828u, sp + 0x10u, 0xFFFFFFFFu, 0, 0, 0, 0, Rc, f1_in);
    u64 f31;
    const u32 px0 = x.rd32(handle);
    if (px0 == 0u) {
        f31 = FL(x.rd32(x.r2 - 0x3654u));
    } else {
        target_pos(x, sp, 0x801E8854u, sp + 0x38u, px0, a2, Rc, f1_in);
        leaf_sub(x, sp + 0x44u, sp + 0x38u, a2);
        f31 = leaf_len2(x, sp + 0x44u);
    }
    Rc.f0[31] = f31;
    r31 += 4u;
    Rc.r[31] = r31;
    while (r31 != x.rd32(a1 + 0x10u)) {
        const u32 holder = x.rd32(r31);
        H_800578A0(x, sp, 0x801E8884u, sp + 8u, holder, 0, 0, 0, 0, Rc, f1_in);
        F_8008A310(x, sp, 0x801E8890u, handle, sp + 8u, 0, 0, 0, 0, Rc, f1_in);
        H_80057850(x, sp, 0x801E889Cu, sp + 8u, 0xFFFFFFFFu, 0, 0, 0, 0, Rc, f1_in);
        const u32 px = x.rd32(handle);
        if (px != 0u) {
            target_pos(x, sp, 0x801E88C0u, sp + 0x20u, px, a2, Rc, f1_in);
            leaf_sub(x, sp + 0x2Cu, sp + 0x20u, a2);
            const u64 d2 = leaf_len2(x, sp + 0x2Cu);
            if ((fcmpo0(x, Rc.f0[31], d2) & 4u) != 0u) {
                Rc.f0[31] = d2;
                best = x.rd32(r31);
                Rc.r[30] = best;
            }
        }
        r31 += 4u;
        Rc.r[31] = r31;
        if (x.dead) return 0u;
    }
    F_8005FEDC(x, sp, 0x801E8904u, a0, best, 0, 0, 0, 0, Rc, f1_in);
    x.requant31 = true;
    return H_80057850(x, sp, 0x801E8910u, handle, 0xFFFFFFFFu, 0, 0, 0, 0, Rc, f1_in);
}
