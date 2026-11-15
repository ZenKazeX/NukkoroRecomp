// hand-written models of the S1 target owner (included by native_s1_target2.cpp; not scanned by the liveness tool: the address literals are
// the original's return addresses and callee addresses, stored or called as values)
// A guest call of the allocator exactly as the original's `bl`: r1 = `sp` (the calling function's frame), LR = `site`, no extra frame
// (gekko::guest_call pushes a 16-byte frame of its own, which would shift every word the callee writes). The callee-saved registers
// are the ones the original holds there (the callee saves some of them into its frame).
u32 guest_call_alloc(Ctx& x, u32 target, u32 sp, u32 site, const Regs& R, u32 a0) {
    if (x.dead) return 0u;
    Cpu& c = x.c;
    const u32 saved_r2 = c.gpr[2], saved_r13 = c.gpr[13], saved_ctr = c.ctr, saved_cr = c.cr, saved_pc = c.pc, saved_npc = c.npc;
    c.gpr[1] = sp;
    for (u32 r = 14u; r <= 31u; ++r) c.gpr[r] = R.r[r];
    c.gpr[3] = a0;
    c.gpr[12] = target;
    c.ctr = target;
    c.lr = site;
    // The call as the translated code makes it (no frame is built, nothing is saved); the seam compiled into both runtime variants.
    if (!s1_chars::call_guest(c, target) || c.exiting()) {
        if (!c.stopped()) c.request_stop(StopReason::ExecutionFault, "s1 target2: an allocator call did not return to its site");
        x.dead = true;
        return 0u;
    }
    const u32 result = c.gpr[3];
    c.gpr[2] = saved_r2; c.gpr[13] = saved_r13; c.ctr = saved_ctr; c.cr = saved_cr; c.pc = saved_pc; c.npc = saved_npc;
    return result;
}
u32 do_new(Ctx& x, u32 sp, u32 site, const Regs& R, u32 size) {
    return guest_call_alloc(x, 0x803A1380u, sp, site, R, size);
}
u32 do_delete(Ctx& x, u32 sp, u32 site, const Regs& R, u32 ptr) {
    return guest_call_alloc(x, 0x803A1334u, sp, site, R, ptr);
}

// ---- hand-written models (loops, branches, allocator calls) --------------------------------------------------------------------
// A prologue: stwu r1, -size(r1); mflr r0; stw r0, size + 4(r1). Returns the new r1.
u32 enter(Ctx& x, u32 cs, u32 size, u32 lr) {
    const u32 sp = cs - size;
    x.wr32(sp, cs);
    x.wr32(cs + 4u, lr);
    return sp;
}
#define HDR(name) static u32 name(Ctx& x, u32 sp_in, u32 lr_in, u32 a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5, const Regs& R, u32 f1_in)
#define UNUSED (void)x; (void)sp_in; (void)lr_in; (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)R; (void)f1_in

// 0x800169AC UseCount(r3 = &weak_count): the pointer to the control block, or its use count word [+4]; a leaf.
HDR(H_800169AC) {
    UNUSED;
    const u32 pn = x.rd32(a0);
    return pn != 0u ? x.rd32(pn + 4u) : 0u;
}

// 0x80016914 add_ref_lock(r3 = pn): stwu -0x20, saves r31; use == 0: throws (excluded by the proof); else use + 1.
HDR(H_80016914) {
    UNUSED;
    const u32 sp = enter(x, sp_in, 0x20u, lr_in);
    x.wr32(sp + 0x1Cu, R.r[31]);
    const u32 use = x.rd32(a0 + 4u);
    x.wr32(a0 + 4u, use + 1u);
    return use;
}

// 0x80016838 shared_count(weak_count) (r3 = dst, r4 = src): [dst] = [src]; add_ref_lock of the block (a null block throws: excluded).
HDR(H_80016838) {
    UNUSED;
    const u32 sp = enter(x, sp_in, 0x20u, lr_in);
    x.wr32(sp + 0x1Cu, R.r[31]);
    const u32 pn = x.rd32(a1);
    x.wr32(a0, pn);
    Regs Rc = R;
    Rc.r[31] = a0;
    if (pn != 0u) H_80016914(x, sp, 0x80016864u, pn, 0, 0, 0, 0, 0, Rc, f1_in);
    return a0;
}

// 0x800097E0 shared_count copy (r3 = dst, r4 = src): [dst] = [src]; a non-null block: use + 1. A leaf.
HDR(H_800097E0) {
    UNUSED;
    const u32 pn = x.rd32(a1);
    x.wr32(a0, pn);
    if (pn != 0u) x.wr32(pn + 4u, x.rd32(pn + 4u) + 1u);
    return a0;
}

// 0x8000945C release(r3 = pn): use - 1; zero: dispose (excluded by the proof).
HDR(H_8000945C) {
    UNUSED;
    const u32 sp = enter(x, sp_in, 0x10u, lr_in);
    x.wr32(sp + 0xCu, R.r[31]);
    const u32 use = x.rd32(a0 + 4u) - 1u;
    x.wr32(a0 + 4u, use);
    return a0;
}

// 0x80009408 shared_count dtor (r3 = this, r4 = flag): release the block when set; flag > 0 (s16): operator delete.
HDR(H_80009408) {
    UNUSED;
    const u32 sp = enter(x, sp_in, 0x10u, lr_in);
    x.wr32(sp + 8u, R.r[30]);
    x.wr32(sp + 0xCu, R.r[31]);
    Regs Rc = R;
    Rc.r[30] = a0;
    Rc.r[31] = a1;
    if (a0 != 0u) {
        const u32 pn = x.rd32(a0);
        if (pn != 0u) H_8000945C(x, sp, 0x80009434u, pn, 0, 0, 0, 0, 0, Rc, f1_in);
        if (static_cast<s16>(a1) > 0) do_delete(x, sp, 0x80009444u, Rc, a0);
    }
    return a0;
}

// 0x80057850 shared_ptr dtor (r3 = this, r4 = flag): shared_count dtor of this + 4 with -1; flag > 0: operator delete.
HDR(H_80057850) {
    UNUSED;
    const u32 sp = enter(x, sp_in, 0x10u, lr_in);
    x.wr32(sp + 8u, R.r[30]);
    x.wr32(sp + 0xCu, R.r[31]);
    Regs Rc = R;
    Rc.r[30] = a0;
    Rc.r[31] = a1;
    if (a0 != 0u) {
        H_80009408(x, sp, 0x80057878u, a0 + 4u, 0xFFFFFFFFu, 0, 0, 0, 0, Rc, f1_in);
        if (static_cast<s16>(a1) > 0) do_delete(x, sp, 0x80057888u, Rc, a0);
    }
    return a0;
}

// 0x800578A0 weak lock (r3 = out, r4 = holder): the live path only (an expired holder is excluded by the proof).
HDR(H_800578A0) {
    UNUSED;
    const u32 sp = enter(x, sp_in, 0x30u, lr_in);
    for (u32 k = 0; k < 4u; ++k) x.wr32(sp + 0x20u + 4u * k, R.r[28 + k]);
    Regs Rc = R;
    Rc.r[31] = a1; Rc.r[30] = a0; Rc.r[29] = 0u; Rc.r[28] = 0u;
    const u32 tmp = sp + 8u;
    F_80057990(x, sp, 0x800578C8u, a1, 0, 0, 0, 0, 0, Rc, f1_in);
    F_8005793C(x, sp, 0x800578F0u, tmp, a1, 0, 0, 0, 0, Rc, f1_in);
    Rc.r[28] = 1u;
    F_80057810(x, sp, 0x80057900u, a0, tmp, 0, 0, 0, 0, Rc, f1_in);
    H_80057850(x, sp, 0x80057914u, tmp, 0xFFFFFFFFu, 0, 0, 0, 0, Rc, f1_in);
    return a0;
}

// 0x8001BC70 vector buffer allocation (r3 = allocator (unused), r4 = n): operator new(4n); a null result prints an error and aborts (excluded).
HDR(H_8001BC70) {
    UNUSED;
    const u32 sp = enter(x, sp_in, 0x10u, lr_in);
    x.wr32(sp + 0xCu, R.r[31]);
    return do_new(x, sp, 0x8001BC88u, R, a1 << 2);
}

// 0x800D19C0 reserve(r3 = vector, r4 = n) for an empty vector (no old buffer): the old-buffer path is excluded by the proof.
HDR(H_800D19C0) {
    UNUSED;
    const u32 sp = enter(x, sp_in, 0x20u, lr_in);
    for (u32 k = 0; k < 3u; ++k) x.wr32(sp + 0x14u + 4u * k, R.r[29 + k]);
    Regs Rc = R;
    Rc.r[29] = a0; Rc.r[30] = a1;
    const u32 cap = x.rd32(a0);
    if (a1 <= cap) return a0;
    F_8001BCC8(x, sp, 0x800D19F0u, a0, 0, 0, 0, 0, 0, Rc, f1_in);
    const u32 old = x.rd32(a0 + 8u);
    Rc.r[31] = old;
    const u32 buf = H_8001BC70(x, sp, 0x800D1A30u, 0, a1, 0, 0, 0, 0, Rc, f1_in);
    x.wr32(a0 + 8u, buf);
    x.wr32(a0, a1);  // capacity (the old-buffer branch needs old == 0: excluded)
    return a0;
}

// 0x8006CEC4 insert(r3 = vector, r4 = pos, r5 = count, r6 = value*): the push_back fast path only (pos == end, size + count <= capacity).
HDR(H_8006CEC4) {
    UNUSED;
    const u32 sp = enter(x, sp_in, 0x30u, lr_in);
    for (u32 k = 0; k < 8u; ++k) x.wr32(sp + 0x10u + 4u * k, R.r[24 + k]);
    Regs Rc = R;
    if (a2 == 0u) return a0;
    Rc.r[26] = a2; Rc.r[25] = a0; Rc.r[28] = a1; Rc.r[27] = a3;
    const u32 maxn = F_8001BCC8(x, sp, 0x8006CEECu, a0, 0, 0, 0, 0, 0, Rc, f1_in);
    Rc.r[31] = maxn;
    F_8001BCFC(x, sp, 0x8006CF30u, a0, 0, 0, 0, 0, 0, Rc, f1_in);
    for (u32 k = 0; k < a2; ++k) x.wr32(a1 + 4u * k, x.rd32(a3));
    x.wr32(a0 + 4u, x.rd32(a0 + 4u) + a2);
    return a1 + 4u * a2;
}

// 0x80019890 vector buffer dealloc (r3 = vector, r4 = flag): operator delete of the data when set; flag > 0: operator delete of the vector.
HDR(H_80019890) {
    UNUSED;
    const u32 sp = enter(x, sp_in, 0x20u, lr_in);
    for (u32 k = 0; k < 3u; ++k) x.wr32(sp + 0x14u + 4u * k, R.r[29 + k]);
    Regs Rc = R;
    Rc.r[29] = a0; Rc.r[30] = a1;
    if (a0 != 0u) {
        const u32 data = x.rd32(a0 + 8u);
        if (data != 0u) {
            Rc.r[31] = data;
            do_delete(x, sp, 0x800198D8u, Rc, data);
        }
        if (static_cast<s16>(a1) > 0) do_delete(x, sp, 0x800198E8u, Rc, a0);
    }
    return a0;
}

// 0x801E8A1C / 0x801E89D0 / 0x801E8974: the destructor chain of a TargetSearchList (each: frame, stmw r30, call, optional delete).
HDR(H_801E8A1C) {
    UNUSED;
    const u32 sp = enter(x, sp_in, 0x10u, lr_in);
    x.wr32(sp + 8u, R.r[30]);
    x.wr32(sp + 0xCu, R.r[31]);
    Regs Rc = R;
    Rc.r[30] = a0; Rc.r[31] = a1;
    if (a0 != 0u) {
        H_80019890(x, sp, 0x801E8A40u, a0, 0, 0, 0, 0, 0, Rc, f1_in);
        if (static_cast<s16>(a1) > 0) do_delete(x, sp, 0x801E8A50u, Rc, a0);
    }
    return a0;
}
HDR(H_801E89D0) {
    UNUSED;
    const u32 sp = enter(x, sp_in, 0x10u, lr_in);
    x.wr32(sp + 8u, R.r[30]);
    x.wr32(sp + 0xCu, R.r[31]);
    Regs Rc = R;
    Rc.r[30] = a0; Rc.r[31] = a1;
    if (a0 != 0u) {
        H_801E8A1C(x, sp, 0x801E89F4u, a0, 0, 0, 0, 0, 0, Rc, f1_in);
        if (static_cast<s16>(a1) > 0) do_delete(x, sp, 0x801E8A04u, Rc, a0);
    }
    return a0;
}
HDR(H_801E8974) {
    UNUSED;
    const u32 sp = enter(x, sp_in, 0x10u, lr_in);
    x.wr32(sp + 8u, R.r[30]);
    x.wr32(sp + 0xCu, R.r[31]);
    Regs Rc = R;
    Rc.r[30] = a0; Rc.r[31] = a1;
    if (a0 != 0u) {
        x.wr32(a0, 0x8053ED04u);
        H_801E89D0(x, sp, 0x801E89A8u, a0 + 4u, 0xFFFFFFFFu, 0, 0, 0, 0, Rc, f1_in);
        if (static_cast<s16>(a1) > 0) do_delete(x, sp, 0x801E89B8u, Rc, a0);
    }
    return a0;
}

// 0x801EA340 copy loop (r3 = &first {ptr, extra}, r4 = &last, r5 = dest): [dest++] = value(first) while first != last; first advances 4 per step.
HDR(H_801EA340) {
    UNUSED;
    const u32 sp = enter(x, sp_in, 0x20u, lr_in);
    for (u32 k = 0; k < 3u; ++k) x.wr32(sp + 0x14u + 4u * k, R.r[29 + k]);
    Regs Rc = R;
    Rc.r[29] = a0; Rc.r[30] = a1;
    u32 dest = a2;
    while (x.rd32(a0) != x.rd32(a1)) {
        Rc.r[31] = dest;
        const u32 value = F_801EA3C0(x, sp, 0x801EA368u, a0, 0, 0, 0, 0, 0, Rc, f1_in);
        x.wr32(dest, value);
        dest += 4u;
        x.wr32(a0, x.rd32(a0) + 4u);
    }
    return dest;
}

// 0x801EA1F8 vector assign (r3 = vector, r4 = &first, r5 = &last): reallocate when the capacity is short (the vector is empty: no old buffer), copy, size.
HDR(H_801EA1F8) {
    UNUSED;
    const u32 sp = enter(x, sp_in, 0x40u, lr_in);
    for (u32 k = 0; k < 5u; ++k) x.wr32(sp + 0x2Cu + 4u * k, R.r[27 + k]);
    Regs Rc = R;
    const u32 lastp = x.rd32(a2), laste = x.rd32(a2 + 4u), firstp = x.rd32(a1), firste = x.rd32(a1 + 4u);
    x.wr32(sp + 0x18u, lastp);
    x.wr32(sp + 0x1Cu, laste);
    x.wr32(sp + 0x20u, firstp);
    x.wr32(sp + 0x24u, firste);
    Rc.r[29] = a0; Rc.r[30] = a1; Rc.r[31] = a2;
    const u32 m = F_801EA3FC(x, sp, 0x801EA240u, sp + 0x20u, sp + 0x18u, 0, 0, 0, 0, Rc, f1_in);
    Rc.r[27] = m;
    const u32 cap = F_8001BCFC(x, sp, 0x801EA250u, a0, 0, 0, 0, 0, 0, Rc, f1_in);
    if (m > cap) {
        F_8001BCC8(x, sp, 0x801EA260u, a0, 0, 0, 0, 0, 0, Rc, f1_in);
        x.wr32(a0 + 4u, 0u);
        // the old buffer is excluded by the proof (the target is a freshly initialised vector)
        const u32 buf = H_8001BC70(x, sp, 0x801EA2E8u, a0, m, 0, 0, 0, 0, Rc, f1_in);
        x.wr32(a0 + 8u, buf);
        x.wr32(a0, m);
    }
    x.wr32(sp + 8u, x.rd32(a2));
    x.wr32(sp + 0xCu, x.rd32(a2 + 4u));
    x.wr32(sp + 0x10u, x.rd32(a1));
    x.wr32(sp + 0x14u, x.rd32(a1 + 4u));
    H_801EA340(x, sp, 0x801EA328u, sp + 0x10u, sp + 8u, x.rd32(a0 + 8u), 0, 0, 0, Rc, f1_in);
    x.wr32(a0 + 4u, m);
    return a0;
}

// ---- the flag filters 0x801E83D0 / 0x801E8448 (round 5's tree, now from generated frames + these loops) ------------------------------
// 0x801E9964 invoke (r3 = &pred + 4, r4 = &policy byte, r5 = pred, r6 = &element slot): fn(*element, bound mask) through bctrl.
HDR(H_801E9964) {
    UNUSED;
    const u32 sp = enter(x, sp_in, 0x10u, lr_in);
    x.wr32(sp + 8u, R.r[30]);
    x.wr32(sp + 0xCu, R.r[31]);
    Regs Rc = R;
    Rc.r[30] = a2;
    const u32 elem_addr = x.rd32(a3);
    Rc.r[31] = elem_addr;
    const u32 fn = x.rd32(a2);
    const u32 mask = x.rd32(a0 + 4u);
    const u32 value = x.rd32(elem_addr);
    if (fn == 0x801E926Cu) return F_801E926C(x, sp, 0x801E99A4u, value, mask, 0, 0, 0, 0, Rc, f1_in);
    if (fn == 0x801E92CCu) return F_801E92CC(x, sp, 0x801E99A4u, value, mask, 0, 0, 0, 0, Rc, f1_in);
    x.dead = true;  // a predicate this entry does not model: excluded by the proof
    return 0u;
}
// 0x801E98CC find_if(r3 = first, r4 = last, r5 = &pred).
HDR(H_801E98CC) {
    UNUSED;
    const u32 sp = enter(x, sp_in, 0x20u, lr_in);
    for (u32 k = 0; k < 3u; ++k) x.wr32(sp + 0x14u + 4u * k, R.r[29 + k]);
    u32 cur = a0;
    for (; cur != a1; cur += 4u) {
        Regs Rc = R;
        Rc.r[29] = cur; Rc.r[30] = a1; Rc.r[31] = a2;
        if ((F_801E9924(x, sp, 0x801E9904u, a2, cur, 0, 0, 0, 0, Rc, f1_in) & 0xFFu) != 0u) break;
    }
    return cur;
}
// 0x801EB230 remove_copy_if(r3 = first, r4 = last, r5 = out, r6 = &pred).
HDR(H_801EB230) {
    UNUSED;
    const u32 sp = enter(x, sp_in, 0x20u, lr_in);
    for (u32 k = 0; k < 4u; ++k) x.wr32(sp + 0x10u + 4u * k, R.r[28 + k]);
    u32 cur = a0, out = a2;
    for (; cur != a1; cur += 4u) {
        Regs Rc = R;
        Rc.r[28] = cur; Rc.r[29] = a1; Rc.r[30] = out; Rc.r[31] = a3;
        if ((F_801E9924(x, sp, 0x801EB260u, a3, cur, 0, 0, 0, 0, Rc, f1_in) & 0xFFu) == 0u) {
            x.wr32(out, x.rd32(cur));
            out += 4u;
        }
    }
    return out;
}
// 0x801E9864 remove_if(r3 = first, r4 = last, r5 = &pred): find, then the copy loop from the match + 1 with a copy of the functor.
HDR(H_801E9864) {
    UNUSED;
    const u32 sp = enter(x, sp_in, 0x20u, lr_in);
    x.wr32(sp + 0x18u, R.r[30]);
    x.wr32(sp + 0x1Cu, R.r[31]);
    Regs Rc = R;
    Rc.r[30] = a1; Rc.r[31] = a2;
    const u32 first = H_801E98CC(x, sp, 0x801E9880u, a0, a1, a2, 0, 0, 0, Rc, f1_in);
    if (first == a1) return first;
    const u32 w0 = x.rd32(a2), w1 = x.rd32(a2 + 4u), w2 = x.rd32(a2 + 8u);
    x.wr32(sp + 8u, w0);
    x.wr32(sp + 0xCu, w1);
    x.wr32(sp + 0x10u, w2);
    return H_801EB230(x, sp, 0x801E98B8u, first + 4u, a1, first, sp + 8u, 0, 0, Rc, f1_in);
}

// ---- the same-target filter 0x801E8158 -----------------------------------------------------------------------------------------------
// 0x80017930 weak_count copy (r3 = dst, r4 = src): [dst] = [src]; a non-null block: weak + 1. A leaf.
HDR(H_80017930) {
    UNUSED;
    const u32 pn = x.rd32(a1);
    x.wr32(a0, pn);
    if (pn != 0u) x.wr32(pn + 8u, x.rd32(pn + 8u) + 1u);
    return a0;
}
// 0x80017454 weak_count dtor (r3 = this, r4 = flag): weak - 1 (zero: destroy, excluded by the proof); flag > 0: operator delete.
HDR(H_80017454) {
    UNUSED;
    const u32 sp = enter(x, sp_in, 0x10u, lr_in);
    x.wr32(sp + 8u, R.r[30]);
    x.wr32(sp + 0xCu, R.r[31]);
    Regs Rc = R;
    Rc.r[30] = a0; Rc.r[31] = a1;
    if (a0 != 0u) {
        const u32 pn = x.rd32(a0);
        if (pn != 0u) {
            const u32 weak = x.rd32(pn + 8u) - 1u;
            x.wr32(pn + 8u, weak);
            if (weak == 0u) x.dead = true;
        }
        if (static_cast<s16>(a1) > 0) do_delete(x, sp, 0x800174A8u, Rc, a0);
    }
    return a0;
}
// A destructor wrapper of the chain 0x801E81D8 -> 0x801E8228 -> 0x801E8278 -> 0x8005FF1C -> 0x80017454: (this, flag): the next with (this + k, -1), then delete.
static u32 dtor_link(Ctx& x, u32 sp_in, u32 lr_in, u32 a0, u32 a1, const Regs& R, u32 f1_in, u32 next_off, u32 next, u32 site_call, u32 site_delete);
HDR(H_8005FF1C) { UNUSED; return dtor_link(x, sp_in, lr_in, a0, a1, R, f1_in, 4u, 0x80017454u, 0x8005FF44u, 0x8005FF54u); }
HDR(H_801E8278) { UNUSED; return dtor_link(x, sp_in, lr_in, a0, a1, R, f1_in, 0u, 0x8005FF1Cu, 0x801E829Cu, 0x801E82ACu); }
HDR(H_801E8228) { UNUSED; return dtor_link(x, sp_in, lr_in, a0, a1, R, f1_in, 4u, 0x801E8278u, 0x801E8250u, 0x801E8260u); }
HDR(H_801E81D8) { UNUSED; return dtor_link(x, sp_in, lr_in, a0, a1, R, f1_in, 4u, 0x801E8228u, 0x801E8200u, 0x801E8210u); }
static u32 dtor_link(Ctx& x, u32 sp_in, u32 lr_in, u32 a0, u32 a1, const Regs& R, u32 f1_in, u32 next_off, u32 next, u32 site_call, u32 site_delete) {
    const u32 sp = enter(x, sp_in, 0x10u, lr_in);
    x.wr32(sp + 8u, R.r[30]);
    x.wr32(sp + 0xCu, R.r[31]);
    Regs Rc = R;
    Rc.r[30] = a0; Rc.r[31] = a1;
    if (a0 != 0u) {
        const u32 t = a0 + next_off, m = 0xFFFFFFFFu;
        switch (next) {
            case 0x80017454u: H_80017454(x, sp, site_call, t, m, 0, 0, 0, 0, Rc, f1_in); break;
            case 0x8005FF1Cu: H_8005FF1C(x, sp, site_call, t, m, 0, 0, 0, 0, Rc, f1_in); break;
            case 0x801E8278u: H_801E8278(x, sp, site_call, t, m, 0, 0, 0, 0, Rc, f1_in); break;
            default: H_801E8228(x, sp, site_call, t, m, 0, 0, 0, 0, Rc, f1_in); break;
        }
        if (static_cast<s16>(a1) > 0) do_delete(x, sp, site_delete, Rc, a0);
    }
    return a0;
}
// 0x801E9EF8 invoke (r3 = &pred + 4, r4 = &byte, r5 = pred, r6 = &element slot): fn(*element, &handle) with the handle at pred + 8.
HDR(H_801E9EF8) {
    UNUSED;
    const u32 sp = enter(x, sp_in, 0x10u, lr_in);
    x.wr32(sp + 8u, R.r[30]);
    x.wr32(sp + 0xCu, R.r[31]);
    Regs Rc = R;
    Rc.r[30] = a2;
    const u32 elem_addr = x.rd32(a3);
    Rc.r[31] = elem_addr;
    const u32 fn = x.rd32(a2);
    if (fn == 0x801E906Cu) return F_801E906C(x, sp, 0x801E9F34u, x.rd32(elem_addr), a0 + 4u, 0, 0, 0, 0, Rc, f1_in);
    x.dead = true;
    return 0u;
}
// 0x801E9E60 find_if with the invoke 0x801E9EB8.
HDR(H_801E9E60) {
    UNUSED;
    const u32 sp = enter(x, sp_in, 0x20u, lr_in);
    for (u32 k = 0; k < 3u; ++k) x.wr32(sp + 0x14u + 4u * k, R.r[29 + k]);
    u32 cur = a0;
    for (; cur != a1; cur += 4u) {
        Regs Rc = R;
        Rc.r[29] = cur; Rc.r[30] = a1; Rc.r[31] = a2;
        if ((F_801E9EB8(x, sp, 0x801E9E98u, a2, cur, 0, 0, 0, 0, Rc, f1_in) & 0xFFu) != 0u) break;
    }
    return cur;
}
// 0x801EB368 remove_copy_if with the invoke 0x801E9EB8.
HDR(H_801EB368) {
    UNUSED;
    const u32 sp = enter(x, sp_in, 0x20u, lr_in);
    for (u32 k = 0; k < 4u; ++k) x.wr32(sp + 0x10u + 4u * k, R.r[28 + k]);
    u32 cur = a0, out = a2;
    for (; cur != a1; cur += 4u) {
        Regs Rc = R;
        Rc.r[28] = cur; Rc.r[29] = a1; Rc.r[30] = out; Rc.r[31] = a3;
        if ((F_801E9EB8(x, sp, 0x801EB398u, a3, cur, 0, 0, 0, 0, Rc, f1_in) & 0xFFu) == 0u) {
            x.wr32(out, x.rd32(cur));
            out += 4u;
        }
    }
    return out;
}
// 0x801E9D28 remove_if(r3 = first, r4 = last, r5 = &pred) with a functor copy on the stack (0x801E9DB0) and its destruction (0x801E81D8).
HDR(H_801E9D28) {
    UNUSED;
    const u32 sp = enter(x, sp_in, 0x30u, lr_in);
    for (u32 k = 0; k < 4u; ++k) x.wr32(sp + 0x20u + 4u * k, R.r[28 + k]);
    Regs Rc = R;
    Rc.r[30] = a1; Rc.r[31] = a2;
    const u32 first = H_801E9E60(x, sp, 0x801E9D44u, a0, a1, a2, 0, 0, 0, Rc, f1_in);
    Rc.r[29] = first; Rc.r[28] = 0u;
    u32 result = first;
    bool copied = false;
    if (first != a1) {
        F_801E9DB0(x, sp, 0x801E9D68u, sp + 8u, a2, 0, 0, 0, 0, Rc, f1_in);
        Rc.r[28] = 1u;
        copied = true;
        Rc.r[31] = a2;
        result = H_801EB368(x, sp, 0x801E9D80u, first + 4u, a1, first, sp + 8u, 0, 0, Rc, f1_in);
        Rc.r[31] = result;
    }
    if (copied) H_801E81D8(x, sp, 0x801E9D98u, sp + 8u, 0xFFFFFFFFu, 0, 0, 0, 0, Rc, f1_in);
    return result;
}

// 0x801EB438 remove_copy_if(r3 = first, r4 = last, r5 = out, r6 = pred): the plain predicate through bctrl.
HDR(H_801EB438) {
    UNUSED;
    const u32 sp = enter(x, sp_in, 0x20u, lr_in);
    for (u32 k = 0; k < 4u; ++k) x.wr32(sp + 0x10u + 4u * k, R.r[28 + k]);
    u32 cur = a0, out = a2;
    for (; cur != a1; cur += 4u) {
        Regs Rc = R;
        Rc.r[28] = cur; Rc.r[29] = a1; Rc.r[30] = out; Rc.r[31] = a3;
        const u32 remove = F_801E9498(x, sp, 0x801EB46Cu, x.rd32(cur), 0, 0, 0, 0, 0, Rc, f1_in) & 0xFFu;
        if (remove == 0u) {
            x.wr32(out, x.rd32(cur));
            out += 4u;
        }
    }
    return out;
}

// 0x801EA178 remove_if(r3 = first, r4 = last, r5 = pred): find, then remove_copy_if from first + 1.
HDR(H_801EA178) {
    UNUSED;
    const u32 sp = enter(x, sp_in, 0x20u, lr_in);
    for (u32 k = 0; k < 3u; ++k) x.wr32(sp + 0x14u + 4u * k, R.r[29 + k]);
    u32 cur = a0;
    for (; cur != a1; cur += 4u) {
        Regs Rc = R;
        Rc.r[29] = a1; Rc.r[30] = a2; Rc.r[31] = cur;
        const u32 remove = F_801E9498(x, sp, 0x801EA1B4u, x.rd32(cur), 0, 0, 0, 0, 0, Rc, f1_in) & 0xFFu;
        if (remove != 0u) break;
    }
    if (cur == a1) return cur;
    Regs Rc = R;
    Rc.r[29] = a1; Rc.r[30] = a2; Rc.r[31] = cur;
    return H_801EB438(x, sp, 0x801EA1DCu, cur + 4u, a1, cur, a2, 0, 0, Rc, f1_in);
}

// 0x801E8C0C TargetSearchList ctor (r3 = this, r4 = registry list): vtable, vector zero-init, reserve(N), one push_back per node, end.
HDR(H_801E8C0C) {
    UNUSED;
    const u32 sp = enter(x, sp_in, 0x30u, lr_in);
    x.wr32(sp + 0x28u, R.r[30]);
    x.wr32(sp + 0x2Cu, R.r[31]);
    Regs Rc = R;
    Rc.r[30] = a0; Rc.r[31] = a1;
    x.wr32(a0, 0x8053ED04u);
    F_801E8BAC(x, sp, 0x801E8C38u, a0 + 4u, 0, 0, 0, 0, 0, Rc, f1_in);
    const u32 n = F_801E7EF0(x, sp, 0x801E8C40u, a1 + 4u, 0, 0, 0, 0, 0, Rc, f1_in);
    F_801E8D90(x, sp, 0x801E8C4Cu, a0 + 4u, n, 0, 0, 0, 0, Rc, f1_in);
    // 0x801E8D58 list begin (out = sp + 0x14, list = r31 + 4): the first node; then it is copied to sp + 0x18.
    F_801E8D58(x, sp, 0x801E8C58u, sp + 0x14u, a1 + 4u, 0, 0, 0, 0, Rc, f1_in);
    x.wr32(sp + 0x18u, x.rd32(sp + 0x14u));
    for (;;) {
        // 0x801E8CC8(sp + 8, r31 + 4): the end iterator = the sentinel node
        F_801E8CC8(x, sp, 0x801E8C94u, sp + 8u, a1 + 4u, 0, 0, 0, 0, Rc, f1_in);
        if (x.rd32(sp + 0x18u) == x.rd32(sp + 8u)) break;
        const u32 node = x.rd32(sp + 0x18u);
        x.wr32(sp + 0x10u, node + 8u);
        F_801E8D18(x, sp, 0x801E8C78u, a0 + 4u, sp + 0x10u, 0, 0, 0, 0, Rc, f1_in);
        // 0x801E8D04 (iterator step, a leaf inlined in the original): out = old, iter = [old + 4]
        x.wr32(sp + 0xCu, node);
        x.wr32(sp + 0x18u, x.rd32(node + 4u));
    }
    const u32 end = F_801E7CB0(x, sp, 0x801E8CACu, a0 + 4u, 0, 0, 0, 0, 0, Rc, f1_in);
    x.wr32(a0 + 0x10u, end);
    return a0;
}

// 0x801E7C40 CreateFromRegistry (r3 = registry): new(0x14), ctor, prune with 0x801E9498, [obj + 0x10] = the new end.
HDR(H_801E7C40) {
    UNUSED;
    const u32 sp = enter(x, sp_in, 0x10u, lr_in);
    x.wr32(sp + 8u, R.r[30]);
    x.wr32(sp + 0xCu, R.r[31]);
    Regs Rc = R;
    Rc.r[31] = a0;
    u32 obj = do_new(x, sp, 0x801E7C5Cu, Rc, 0x14u);
    Rc.r[30] = obj;
    if (obj != 0u) obj = H_801E8C0C(x, sp, 0x801E7C6Cu, obj, a0 + 4u, 0, 0, 0, 0, Rc, f1_in);
    Rc.r[30] = obj;
    const u32 end = F_801E7CB0(x, sp, 0x801E7C78u, obj + 4u, 0, 0, 0, 0, 0, Rc, f1_in);
    Rc.r[31] = end;
    const u32 begin = F_801E7CF0(x, sp, 0x801E7C84u, obj + 4u, 0, 0, 0, 0, 0, Rc, f1_in);
    const u32 nend = H_801EA178(x, sp, 0x801E7C94u, begin, end, 0x801E9498u, 0, 0, 0, Rc, f1_in);
    x.wr32(obj + 0x10u, nend);
    return obj;
}

