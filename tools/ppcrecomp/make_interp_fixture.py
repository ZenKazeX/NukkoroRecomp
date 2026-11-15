"""Generate the instruction-family programs for native_interp_diff_tests (swarm field cs-interp, RC-gen-14).

Every program is one generator row. The real ppcrecomp translates them with its PRODUCTION switches (gap sweep and
wide entries on); native_interp_diff_tests runs each program through the generated native code and through the
reference interpreter from identical random architected state, entering at the first instruction and, for the
programs flagged ENTRIES, at every interior instruction (the cold `_at` trampoline), and compares ALL state.

The families are ENUMERATED here (every rD x rA overlap of lmw/stmw, every NB of lswi/stswi, every BO of
bc/bclr/bcctr, every FM of mtfsf, ...) where the F1a/F1b difftests (tools/difftest) draw them at random.
No semantics live in this file: the spec is the other engine.
"""
import argparse
import csv
from pathlib import Path
import struct
import subprocess

BASE = 0x80010000
STRIDE = 0x80            # 32 words per program row
BLR = 0x4E800020
NOP = 0x60000000

# flags (mirrored in the C++ test)
ENTRIES = 1              # also enter at every interior instruction
STRING = 2               # XER[25:31] (the byte count) is drawn per case
VARY_FP = 4              # MSR[FP] is cleared in some cases
VARY_LCE = 8             # HID2[LCE] is cleared in some cases
SMALL_CTR = 16           # CTR is drawn from 1..4 (the program loops on it)
# kinds
PLAIN, DFORM, XFORM = 0, 1, 2


def xo(primary, d, a, b, extended, rc=0, oe=0):
    return (primary << 26) | (d << 21) | (a << 16) | (b << 11) | (oe << 10) | (extended << 1) | rc


def d_form(primary, d, a, imm):
    return (primary << 26) | (d << 21) | (a << 16) | (imm & 0xFFFF)


def a_form(primary, d, a, b, c, extended, rc=0):
    return (primary << 26) | (d << 21) | (a << 16) | (b << 11) | (c << 6) | (extended << 1) | rc


def psq_d(primary, s, a, w, index, offset):
    return (primary << 26) | (s << 21) | (a << 16) | (w << 15) | (index << 12) | (offset & 0xFFF)


def psq_x(extended, d, a, b, w, index):
    return (4 << 26) | (d << 21) | (a << 16) | (b << 11) | (w << 10) | (index << 7) | (extended << 1)


def bc(bo, bi, displacement, lk=0):
    return (16 << 26) | (bo << 21) | (bi << 16) | (displacement & 0xFFFC) | lk


def b_rel(displacement, lk=0):
    return (18 << 26) | (displacement & 0x03FFFFFC) | lk


def bclr(bo, bi, lk=0):
    return (19 << 26) | (bo << 21) | (bi << 16) | (16 << 1) | lk


def bcctr(bo, bi, lk=0):
    return (19 << 26) | (bo << 21) | (bi << 16) | (528 << 1) | lk


def mtspr(spr, rs):
    return (31 << 26) | (rs << 21) | ((spr & 31) << 16) | ((spr >> 5) << 11) | (467 << 1)


def mfspr(rd, spr):
    return (31 << 26) | (rd << 21) | ((spr & 31) << 16) | ((spr >> 5) << 11) | (339 << 1)


def addi(d, a, imm):
    return d_form(14, d, a, imm)


def rlwinm(a, s, sh, mb, me, rc=0):
    return (21 << 26) | (s << 21) | (a << 16) | (sh << 11) | (mb << 6) | (me << 1) | rc


MTLR_R9 = mtspr(8, 9)    # the return sentinel travels in r9 (set by the test)
MTCTR_R9 = mtspr(9, 9)


class Builder:
    def __init__(self):
        self.programs = []

    def add(self, name, words, kind=PLAIN, ra=0, rb=0, disp=0, span=0, flags=0):
        assert 1 <= len(words) <= STRIDE // 4 - 1, name
        self.programs.append((name, tuple(words), kind, ra, rb, disp, span, flags))


def build():
    b = Builder()
    add = b.add

    # ---- lmw / stmw: every rD against rA below, equal to, above and far from the range -----------------
    for rd in (0, 1, 5, 15, 16, 26, 27, 28, 29, 30, 31):
        ras = sorted({r for r in (rd - 1, rd, rd + 1, 1, 3, 20, 31) if 1 <= r <= 31})
        for ra in ras:
            for name, primary in (("lmw", 46), ("stmw", 47)):
                add(f"{name}_d{rd}_a{ra}", (d_form(primary, rd, ra, 8), BLR), DFORM, ra, 0, 8, 4 * (32 - rd) + 16)

    # ---- string instructions: every NB, register wrap, rA overlapping the range ---------------------------
    for nb in range(32):
        for rd in (0, 5, 30, 31):
            add(f"lswi_nb{nb}_d{rd}", (xo(31, rd, 20, nb, 597), BLR), DFORM, 20, 0, 0, 40)
            add(f"stswi_nb{nb}_d{rd}", (xo(31, rd, 20, nb, 725), BLR), DFORM, 20, 0, 0, 40)
    for nb in (0, 1, 4, 5):
        add(f"lswi_alias_nb{nb}", (xo(31, 20, 20, nb, 597), BLR), DFORM, 20, 0, 0, 40)
        add(f"lswi_alias2_nb{nb}", (xo(31, 19, 20, nb, 597), BLR), DFORM, 20, 0, 0, 40)
    for rd in (0, 5, 29, 31):
        for ra, rb in ((20, 21), (0, 21), (21, 21), (rd, 21), (20, rd)):
            add(f"lswx_d{rd}_a{ra}_b{rb}", (xo(31, rd, ra, rb, 533), BLR), XFORM, ra, rb, 0, 140, STRING)
            add(f"stswx_d{rd}_a{ra}_b{rb}", (xo(31, rd, ra, rb, 661), BLR), XFORM, ra, rb, 0, 140, STRING)
    # a store then a load of the same bytes (sequence, interior entries)
    add("str_seq", (xo(31, 5, 20, 3, 725), xo(31, 30, 20, 8, 597), addi(3, 3, 1), BLR), DFORM, 20, 0, 0, 40,
        ENTRIES)

    # ---- load/store, update and non-update, D and X forms ---------------------------------------------------
    dload = (("lwz", 32, 4), ("lbz", 34, 1), ("lhz", 40, 2), ("lha", 42, 2), ("lfs", 48, 4), ("lfd", 50, 8))
    dstore = (("stw", 36, 4), ("stb", 38, 1), ("sth", 44, 2), ("stfs", 52, 4), ("stfd", 54, 8))
    for name, primary, size in dload + dstore:
        for update in (0, 1):
            for rd, ra in ((5, 20), (20, 20), (0, 20), (31, 20)):
                word = d_form(primary + update, rd, ra, -4 & 0xFFFF if update else 6)
                add(f"{name}{'u' if update else ''}_d{rd}_a{ra}", (word, BLR), DFORM, ra, 0,
                    -4 if update else 6, size + 4)
    xloads = (("lwz", 23, 55, 4), ("lbz", 87, 119, 1), ("lhz", 279, 311, 2), ("lha", 343, 375, 2),
              ("lfs", 535, 567, 4), ("lfd", 599, 631, 8))
    xstores = (("stw", 151, 183, 4), ("stb", 215, 247, 1), ("sth", 407, 439, 2), ("stfs", 663, 695, 4),
               ("stfd", 727, 759, 8))
    for name, plain, upd, size in xloads + xstores:
        for update, ext in ((0, plain), (1, upd)):
            for rd, ra, rb in ((5, 20, 21), (20, 20, 21), (21, 20, 21), (5, 21, 21), (5, 20, 20)):
                if update and ra == 0:
                    continue
                add(f"{name}{'u' if update else ''}x_d{rd}_a{ra}_b{rb}", (xo(31, rd, ra, rb, ext), BLR),
                    XFORM, ra, rb, 0, size + 4)
            if not update:
                add(f"{name}x_d5_a0_b21", (xo(31, 5, 0, 21, ext), BLR), XFORM, 0, 21, 0, size + 4)
    for name, ext, size in (("lwbrx", 534, 4), ("lhbrx", 790, 2), ("stwbrx", 662, 4), ("sthbrx", 918, 2),
                            ("stfiwx", 983, 4), ("eciwx", 310, 4), ("ecowx", 438, 4)):
        for ra, rb in ((20, 21), (0, 21), (21, 21)):
            add(f"{name}_a{ra}_b{rb}", (xo(31, 5, ra, rb, ext), BLR), XFORM, ra, rb, 0, size + 4)

    # ---- dcbz / dcbz_l: the test sweeps the EA over 96 offsets through each 32-byte line -----------------
    for ra, rb in ((20, 21), (0, 21), (21, 21)):
        add(f"dcbz_a{ra}_b{rb}", (xo(31, 0, ra, rb, 1014), BLR), XFORM, ra, rb, 0, 32)
        add(f"dcbz_l_a{ra}_b{rb}", (xo(4, 0, ra, rb, 1014), BLR), XFORM, ra, rb, 0, 32, VARY_LCE)

    # ---- paired-single quantised loads and stores: every GQR index, W, update and indexed forms ----------
    for index in range(8):
        for w in (0, 1):
            add(f"psq_l_g{index}_w{w}", (psq_d(56, 5, 20, w, index, 4), BLR), DFORM, 20, 0, 4, 16)
            add(f"psq_st_g{index}_w{w}", (psq_d(60, 5, 20, w, index, 4), BLR), DFORM, 20, 0, 4, 16)
            add(f"psq_lu_g{index}_w{w}", (psq_d(57, 5, 20, w, index, 4), BLR), DFORM, 20, 0, 4, 16)
            add(f"psq_stu_g{index}_w{w}", (psq_d(61, 5, 20, w, index, -4), BLR), DFORM, 20, 0, -4, 16)
            for name, ext in (("psq_lx", 6), ("psq_stx", 7), ("psq_lux", 38), ("psq_stux", 39)):
                add(f"{name}_g{index}_w{w}", (psq_x(ext, 5, 20, 21, w, index), BLR), XFORM, 20, 21, 0, 16)
    for name, ext in (("psq_lx", 6), ("psq_stx", 7)):
        add(f"{name}_a0", (psq_x(ext, 5, 0, 21, 0, 3), BLR), XFORM, 0, 21, 0, 16)
    # update with the destination FPR aliasing nothing but the pointer GPR taken from a store source
    add("psq_stu_seq", (psq_d(61, 5, 20, 0, 1, 8), psq_d(56, 6, 20, 1, 2, 4), psq_d(57, 7, 20, 0, 4, -8), BLR),
        DFORM, 20, 0, 8, 40, ENTRIES)

    # ---- paired singles and fsel: every op with frD aliasing none/frA/frB/frC, Rc off and on ------------
    ps_a = (("ps_sum0", 10), ("ps_sum1", 11), ("ps_muls0", 12), ("ps_muls1", 13), ("ps_madds0", 14),
            ("ps_madds1", 15), ("ps_sel", 23), ("ps_mul", 25), ("ps_msub", 28), ("ps_madd", 29),
            ("ps_nmsub", 30), ("ps_nmadd", 31), ("ps_add", 21), ("ps_sub", 20), ("ps_div", 18))
    for name, ext in ps_a:
        for alias, (d, a, bb, c) in (("none", (6, 1, 2, 3)), ("a", (1, 1, 2, 3)), ("b", (2, 1, 2, 3)),
                                     ("c", (3, 1, 2, 3)), ("all", (1, 1, 1, 1))):
            for rc in (0, 1):
                add(f"{name}_{alias}_rc{rc}", (a_form(4, d, a, bb, c, ext, rc), BLR), flags=VARY_FP)
    for name, ext in (("ps_merge00", 528), ("ps_merge01", 560), ("ps_merge10", 592), ("ps_merge11", 624),
                      ("ps_neg", 40), ("ps_mr", 72), ("ps_nabs", 136), ("ps_abs", 264)):
        for alias, (d, a, bb) in (("none", (6, 1, 2)), ("a", (1, 1, 2)), ("b", (2, 1, 2)), ("all", (1, 1, 1))):
            for rc in (0, 1):
                add(f"{name}_{alias}_rc{rc}", (xo(4, d, a, bb, ext, rc), BLR), flags=VARY_FP)
    for alias, (d, a, bb, c) in (("none", (6, 1, 2, 3)), ("a", (1, 1, 2, 3)), ("b", (2, 1, 2, 3)),
                                 ("c", (3, 1, 2, 3)), ("all", (1, 1, 1, 1))):
        for rc in (0, 1):
            add(f"fsel_{alias}_rc{rc}", (a_form(63, d, a, bb, c, 23, rc), BLR), flags=VARY_FP)
    for name, ext in (("ps_cmpu0", 0), ("ps_cmpo0", 32), ("ps_cmpu1", 64), ("ps_cmpo1", 96)):
        for crf in (0, 3, 7):
            add(f"{name}_cr{crf}", (xo(4, crf << 2, 1, 2, ext), BLR))

    # ---- FPSCR moves: every FM of mtfsf, every field of mtfsfi, every bit of mtfsb0/1, every mcrfs source ---
    for fm in range(256):
        for rc in (0, 1):
            add(f"mtfsf_fm{fm:02X}_rc{rc}", ((63 << 26) | (fm << 17) | (2 << 11) | (711 << 1) | rc, BLR))
    for crf in range(8):
        for imm in range(16):
            for rc in (0, 1):
                add(f"mtfsfi_cr{crf}_i{imm}_rc{rc}", ((63 << 26) | (crf << 23) | (imm << 12) | (134 << 1) | rc, BLR))
    for bit in range(32):
        for name, ext in (("mtfsb0", 70), ("mtfsb1", 38)):
            for rc in (0, 1):
                add(f"{name}_b{bit}_rc{rc}", ((63 << 26) | (bit << 21) | (ext << 1) | rc, BLR))
    for crfs in range(8):
        for crfd in (0, 5):
            add(f"mcrfs_s{crfs}_d{crfd}", ((63 << 26) | (crfd << 23) | (crfs << 18) | (64 << 1), BLR))
            # read the same field twice: whether the first read cleared the bits it copied is visible
            add(f"mcrfs_twice_s{crfs}_d{crfd}",
                ((63 << 26) | (crfd << 23) | (crfs << 18) | (64 << 1),
                 (63 << 26) | (7 << 23) | (crfs << 18) | (64 << 1), BLR), flags=ENTRIES)
    for rc in (0, 1):
        add(f"mffs_rc{rc}", (xo(63, 7, 0, 0, 583, rc), BLR))
    # an FP op whose result depends on FPSCR[NI] right after the move, and across an interior entry
    add("mtfsf_then_fadd", ((63 << 26) | (0xFF << 17) | (2 << 11) | (711 << 1), a_form(63, 6, 1, 2, 0, 21),
                            a_form(59, 7, 1, 2, 0, 21), xo(63, 8, 0, 0, 583), BLR), flags=ENTRIES)

    # ---- branches: every BO against a spread of BI, forward bc, bclr/bcctr and their link forms ----------
    for bo in range(32):
        for bi in (0, 2, 5, 17, 31):
            add(f"bc_bo{bo}_bi{bi}", (bc(bo, bi, 12), addi(3, 3, 1), addi(4, 4, 1), addi(5, 5, 1), BLR),
                flags=ENTRIES)
            add(f"bcl_bo{bo}_bi{bi}", (bc(bo, bi, 12, 1), addi(3, 3, 1), MTLR_R9, addi(4, 4, 1), BLR),
                flags=ENTRIES)
            add(f"bclr_bo{bo}_bi{bi}", (bclr(bo, bi), addi(3, 3, 1), BLR))
            add(f"bclrl_bo{bo}_bi{bi}", (bclr(bo, bi, 1), addi(3, 3, 1), MTLR_R9, BLR))
            if bo & 4:   # bcctr with CTR decrement is an invalid form
                add(f"bcctr_bo{bo}_bi{bi}", (MTCTR_R9, bcctr(bo, bi), addi(3, 3, 1), BLR))
                add(f"bcctrl_bo{bo}_bi{bi}", (MTCTR_R9, bcctr(bo, bi, 1), addi(3, 3, 1), MTLR_R9, BLR))
    # a CTR loop whose back edge is a bc, bounded by the test's CTR draw, with a counter in r3
    for bo in (16, 18, 8, 9, 0, 2):
        add(f"ctr_loop_bo{bo}", (addi(3, 3, 1), bc(bo, 2, -4), BLR), flags=ENTRIES | SMALL_CTR)

    # ---- CR fusion: producer, optional label between, consumer; every way into the label ----------------
    producers = (
        ("cmpw", lambda crf: xo(31, crf << 2, 3, 4, 0)),
        ("cmplw", lambda crf: xo(31, crf << 2, 3, 4, 32)),
        ("cmpwi", lambda crf: d_form(11, crf << 2, 3, -5)),
        ("cmplwi", lambda crf: d_form(10, crf << 2, 3, 5)),
        ("fcmpu", lambda crf: xo(63, crf << 2, 1, 2, 0)),
        ("fcmpo", lambda crf: xo(63, crf << 2, 1, 2, 32)),
        ("add.", lambda crf: xo(31, 5, 3, 4, 266, 1)),
        ("andi.", lambda crf: d_form(28, 3, 5, 0xF0F0)),
    )
    consumers = (
        ("beq", lambda crf: bc(12, crf * 4 + 2, 12)),
        ("bne", lambda crf: bc(4, crf * 4 + 2, 12)),
        ("blt", lambda crf: bc(12, crf * 4 + 0, 12)),
        ("bso", lambda crf: bc(12, crf * 4 + 3, 12)),
        ("bgtlr", lambda crf: bclr(12, crf * 4 + 1)),
        ("crand_beq", None),
    )
    for pname, produce in producers:
        for cname, consume in consumers:
            for crf in ((0,) if pname in ("add.", "andi.") else (0, 3)):
                if consume is None:
                    words = (produce(crf), (19 << 26) | (28 << 21) | ((crf * 4 + 2) << 16) | ((crf * 4 + 1) << 11) | (257 << 1),
                             bc(12, 28, 8), addi(5, 5, 1), BLR)
                else:
                    words = (produce(crf), consume(crf), addi(5, 5, 1), addi(6, 6, 1), BLR)
                add(f"fuse_{pname}_{cname}_cr{crf}", words, flags=ENTRIES)
                # the producer is skipped when cr7.eq is clear: the consumer then sees the register CR at a label
                if consume is not None:
                    words = (bc(4, 30, 12), produce(crf), NOP, consume(crf), addi(5, 5, 1), addi(6, 6, 1), BLR)
                    add(f"label_{pname}_{cname}_cr{crf}", words, flags=ENTRIES)
    for name, ext in (("crand", 257), ("crandc", 129), ("cror", 449), ("crorc", 417), ("crxor", 193),
                      ("crnand", 225), ("crnor", 33), ("creqv", 289)):
        for d, a, c in ((0, 1, 2), (31, 0, 31), (5, 5, 5), (9, 9, 12), (30, 17, 3)):
            add(f"{name}_{d}_{a}_{c}", ((19 << 26) | (d << 21) | (a << 16) | (c << 11) | (ext << 1), bc(12, d, 8),
                                        addi(5, 5, 1), BLR), flags=ENTRIES)
    for crfd in (0, 3, 7):
        for crfs in (0, 2, 7):
            add(f"mcrf_{crfd}_{crfs}", ((19 << 26) | (crfd << 23) | (crfs << 18), bc(12, crfd * 4 + 2, 8),
                                        addi(5, 5, 1), BLR), flags=ENTRIES)
    for crm in (0x00, 0x80, 0x01, 0x55, 0xAA, 0xFF):
        add(f"mtcrf_{crm:02X}", ((31 << 26) | (6 << 21) | (crm << 12) | (144 << 1), xo(31, 7, 0, 0, 19), BLR))

    # ---- FP guard and HID2 hoisting across labels, calls-free sequences and entries ------------------------
    add("fp_pair_label", (a_form(63, 6, 1, 2, 0, 21), bc(4, 30, 8), a_form(63, 7, 1, 2, 0, 21), a_form(63, 8, 1, 2, 0, 20),
                          BLR), flags=ENTRIES | VARY_FP)
    add("fp_mtmsr_between", (a_form(63, 6, 1, 2, 0, 21), xo(31, 10, 0, 0, 83),
                             rlwinm(10, 10, 0, 19, 17), xo(31, 10, 0, 0, 146), a_form(63, 7, 1, 2, 0, 21), BLR),
        flags=ENTRIES)
    add("hid2_mtspr_between", (xo(4, 0, 20, 21, 1014), mfspr(10, 920), rlwinm(10, 10, 0, 4, 2),
                               mtspr(920, 10), xo(4, 0, 20, 21, 1014), BLR), XFORM, 20, 21, 0, 40,
        ENTRIES | VARY_LCE)
    add("ps_after_hid2_write", (a_form(4, 6, 1, 2, 0, 21), mfspr(10, 920), mtspr(920, 10), a_form(4, 7, 1, 2, 0, 20),
                                psq_d(56, 8, 20, 0, 1, 0), BLR), DFORM, 20, 0, 0, 16, ENTRIES)
    add("fp_loads_label", (d_form(48, 6, 20, 0), bc(12, 30, 8), d_form(50, 7, 20, 8), d_form(52, 6, 20, 16),
                           BLR), DFORM, 20, 0, 0, 24, ENTRIES | VARY_FP)
    return b.programs


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--generator", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    programs = build()
    image = bytearray(0x100 + len(programs) * STRIDE)
    for offset, value in ((0, 0x100), (0x48, BASE), (0x90, len(image) - 0x100), (0xE0, BASE)):
        struct.pack_into(">I", image, offset, value)
    rows = []
    for index, (name, words, *_rest) in enumerate(programs):
        address = BASE + index * STRIDE
        struct.pack_into(f">{len(words)}I", image, 0x100 + index * STRIDE, *words)
        label = "".join(ch if ch.isalnum() else "_" for ch in name)
        rows.append((hex(address), address, f"interp_{index}_{label}", len(words) * 4, len(words), "", "", ""))
    dol = args.out / "interp.dol"
    dol.write_bytes(image)
    functions = args.out / "functions.csv"
    with functions.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(("addr_hex", "addr", "name", "size", "ins", "tags", "strings", "callees"))
        writer.writerows(rows)
    lines = ["#pragma once", "// Generated by tools/ppcrecomp/make_interp_fixture.py.",
             "struct InterpProgram { const char* name; unsigned address; unsigned count; const unsigned* words;",
             "                      unsigned kind, ra, rb; int disp; unsigned span, flags; };",
             f"constexpr unsigned kInterpBase = 0x{BASE:08X}u, kInterpStride = 0x{STRIDE:X}u;"]
    for index, (name, words, *_rest) in enumerate(programs):
        lines.append(f"constexpr unsigned kWords{index}[] = {{" + ",".join(f"0x{w:08X}u" for w in words) + "};")
    lines.append("constexpr InterpProgram kInterpPrograms[] = {")
    for index, (name, words, kind, ra, rb, disp, span, flags) in enumerate(programs):
        lines.append(f'    {{"{name}", 0x{BASE + index * STRIDE:08X}u, {len(words)}u, kWords{index}, '
                     f"{kind}u, {ra}u, {rb}u, {disp}, {span}u, {flags}u}},")
    lines.append("};")
    lines.append(f"constexpr unsigned kInterpProgramCount = {len(programs)}u;")
    (args.out / "interp_fixture.h").write_text("\n".join(lines) + "\n", encoding="utf-8")
    subprocess.run([str(args.generator.resolve()), "--dol", str(dol), "--functions", str(functions),
                    "--out", str(args.out), "--shards", "2"], check=True)


if __name__ == "__main__":
    main()
