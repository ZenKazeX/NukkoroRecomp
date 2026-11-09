"""Generate synthetic PPC input for the compiled native fallback regression test."""
import argparse
import csv
from pathlib import Path
import struct
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--generator", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    base = 0x80004000
    programs = []
    for to in range(32):
        for immediate in (-1, 0, 1):
            programs.append((3 << 26) | (to << 21) | (3 << 16) | (immediate & 0xFFFF))
        programs.append((31 << 26) | (to << 21) | (3 << 16) | (4 << 11) | (4 << 1))
    image = bytearray(0x100 + 0x4800)
    for offset, value in ((0, 0x100), (0x48, base), (0x90, len(image) - 0x100), (0xE0, base)):
        struct.pack_into(">I", image, offset, value)
    rows = []
    for index, instruction in enumerate(programs):
        address = base + index * 0x20
        struct.pack_into(">4I", image, 0x100 + address - base,
                         0x38A50001, instruction, 0x38C60001, 0x4E800020)
        rows.append((hex(address), address, f"trap_{index}", 16, 4, "", "", ""))
    hook = 0x80008000
    struct.pack_into(">3I", image, 0x100 + hook - base, 0x38630001, 0x38840001, 0x4E800020)
    rows.append((hex(hook), hook, "hook", 12, 3, "", "", ""))
    polls = (
        ("Alias", 0x80008400, (0x80630000, 0x28030000, 0x4182FFF8, 0x4E800020)),
        ("Stable", 0x80008420, (0x80640000, 0x28030000, 0x4182FFF8, 0x4E800020)),
        ("HotCompare", 0x80008440,
         (0x48000008, 0x80640000, 0x28030000, 0x4182FFF8, 0x4E800020)),
        ("HotBranch", 0x80008460,
         (0x4800000C, 0x80640000, 0x28030000, 0x4182FFF8, 0x4E800020)),
        ("Absolute", 0x80008480, (0x80001000, 0x28000000, 0x4182FFF8, 0x4E800020)),
        ("CompareOne", 0x800084A0, (0x80640000, 0x28030001, 0x4182FFF8, 0x4E800020)),
    )
    for name, address, words in polls:
        struct.pack_into(f">{len(words)}I", image, 0x100 + address - base, *words)
        rows.append((hex(address), address, f"poll_{name.lower()}", len(words) * 4,
                     len(words), "", "", ""))

    # Instruction-semantics programs (audit batch b2): each is translated by the
    # real generator and executed natively and through the reference
    # interpreter by native_fallback_tests, against hand-computed results.
    def xo(primary, d, a, b, extended, rc=False, oe=False):
        return ((primary << 26) | (d << 21) | (a << 16) | (b << 11) | (int(oe) << 10) |
                (extended << 1) | int(rc))

    blr = 0x4E800020
    semantics = [
        # TRK__write_aram prologue: clrlwi r11,r1,27; mr r12,r1; subfic r11,r11,-128;
        # stwux r1,r1,r11 - the back chain stored must be the ORIGINAL r1.
        ("TrkPrologue", (0x542B06FE, 0x7C2C0B78, 0x216BFF80, 0x7C21596E, blr)),
        ("SthuxAliased", (xo(31, 5, 5, 6, 439), blr)),
        ("StbuxAliased", (xo(31, 5, 5, 6, 247), blr)),
        ("Stbux", (xo(31, 7, 5, 6, 247), blr)),
        ("UpdateChain", (xo(31, 1, 5, 6, 695), xo(31, 2, 5, 6, 759), xo(31, 7, 5, 6, 55),
                         xo(31, 8, 5, 6, 119), xo(31, 9, 5, 6, 311), xo(31, 10, 5, 6, 375),
                         xo(31, 3, 5, 6, 567), xo(31, 4, 5, 6, 631), blr)),
        # lwarx r6,0,r3; stwcx. r4,0,r3; mfcr r7; stwcx. r5,0,r3; mfcr r8; blr
        ("Reservation", (xo(31, 6, 0, 3, 20), xo(31, 4, 0, 3, 150, rc=True), xo(31, 7, 0, 0, 19),
                         xo(31, 5, 0, 3, 150, rc=True), xo(31, 8, 0, 0, 19), blr)),
        ("StwcxUnreserved", (xo(31, 4, 0, 3, 150, rc=True), xo(31, 7, 0, 0, 19), blr)),
        ("Cntlzw", (xo(31, 4, 3, 0, 26), blr)),
    ]
    # <form>o. r5,r7,r9 (unary forms leave rB = 0).
    for name, extended, unary in (("Addo", 266, False), ("Addco", 10, False), ("Addeo", 138, False),
                                  ("Addzeo", 202, True), ("Addmeo", 234, True),
                                  ("Subfo", 40, False), ("Subfco", 8, False), ("Subfeo", 136, False),
                                  ("Subfzeo", 200, True), ("Subfmeo", 232, True),
                                  ("Nego", 104, True)):
        semantics.append((name, (xo(31, 5, 7, 0 if unary else 9, extended, rc=True, oe=True), blr)))

    # Floating-point programs (audit batch b6). Compares into cr3 of f1 vs f2;
    # A-form paired ops as <op> frD,frA,frC,frB with the fields in encoding order.
    def a_form(primary, d, a, b, c, extended, rc=False):
        return (primary << 26) | (d << 21) | (a << 16) | (b << 11) | (c << 6) | (extended << 1) | int(rc)

    def psq(primary, s, a, w, index, offset):
        return (primary << 26) | (s << 21) | (a << 16) | (int(w) << 15) | (index << 12) | (offset & 0xFFF)

    semantics += [
        ("Fcmpu", (xo(63, 3 << 2, 1, 2, 0), blr)),
        ("Fcmpo", (xo(63, 3 << 2, 1, 2, 32), blr)),
        ("PsCmpu0", (xo(4, 3 << 2, 1, 2, 0), blr)),
        ("PsCmpo0", (xo(4, 3 << 2, 1, 2, 32), blr)),
        ("PsCmpu1", (xo(4, 3 << 2, 1, 2, 64), blr)),
        ("PsCmpo1", (xo(4, 3 << 2, 1, 2, 96), blr)),
        ("PsSum0", (a_form(4, 3, 1, 2, 5, 10), blr)),       # ps_sum0 f3,f1,f5,f2
        ("PsSum0Alias", (a_form(4, 2, 1, 2, 5, 10), blr)),  # ps_sum0 f2,f1,f5,f2
        ("PsSum1", (a_form(4, 3, 1, 2, 5, 11), blr)),       # ps_sum1 f3,f1,f5,f2
        ("PsSum1Alias", (a_form(4, 5, 1, 2, 5, 11), blr)),  # ps_sum1 f5,f1,f5,f2
        ("PsSel", (a_form(4, 3, 1, 2, 5, 23), blr)),        # ps_sel f3,f1,f5,f2
        ("PsSelAlias", (a_form(4, 1, 1, 2, 5, 23), blr)),   # ps_sel f1,f1,f5,f2
        # psq_st f1 through GQR4..7 (u8/u16/s8/s16), W=0, at r5+0x00/0x10/0x20/0x30.
        ("PsqStInteger", tuple(psq(60, 1, 5, False, 4 + i, 0x10 * i) for i in range(4)) + (blr,)),
        # psq_l f1,0(r5),0,0 ; psq_st f1,8(r5),0,0 ; psq_lu f2,0x10(r5),1,3 ;
        # psq_l f3,0x10(r5),0,6 (r5 was advanced by 0x10, so original r5+0x20)
        ("PsqGqr", (psq(56, 1, 5, False, 0, 0), psq(60, 1, 5, False, 0, 8),
                    psq(57, 2, 5, True, 3, 0x10), psq(56, 3, 5, False, 6, 0x10), blr)),
    ]
    # Floating-point semantics (audit batch b7). Operands frA = f1, frB = f2,
    # frC = f3; each result in its own register. The last single form writes
    # frB (f2) to check that operands are read before the store.
    semantics += [
        ("FpSingles", (a_form(59, 4, 1, 2, 0, 21), a_form(59, 5, 1, 2, 0, 20), a_form(59, 6, 1, 2, 0, 18),
                       a_form(59, 7, 1, 0, 3, 25), a_form(59, 8, 0, 2, 0, 22), a_form(59, 9, 0, 2, 0, 24),
                       a_form(59, 10, 1, 2, 3, 29), a_form(59, 11, 1, 2, 3, 28), a_form(59, 12, 1, 2, 3, 31),
                       a_form(59, 13, 1, 2, 3, 30), a_form(59, 2, 1, 2, 3, 29), blr)),
        ("FpDoubles", (a_form(63, 4, 1, 2, 0, 21), a_form(63, 5, 1, 2, 0, 20), a_form(63, 6, 1, 2, 0, 18),
                       a_form(63, 7, 1, 0, 3, 25), a_form(63, 8, 0, 2, 0, 22), a_form(63, 9, 0, 2, 0, 26),
                       a_form(63, 10, 1, 2, 3, 29), a_form(63, 11, 1, 2, 3, 28), a_form(63, 12, 1, 2, 3, 31),
                       a_form(63, 13, 1, 2, 3, 30), xo(63, 14, 0, 2, 12), xo(63, 15, 0, 2, 14),
                       xo(63, 16, 0, 2, 15), blr)),
        ("FpPaired", (a_form(4, 4, 1, 2, 0, 21), a_form(4, 5, 1, 2, 0, 20), a_form(4, 6, 1, 2, 0, 18),
                      a_form(4, 7, 1, 0, 3, 25), a_form(4, 8, 0, 2, 0, 24), a_form(4, 9, 0, 2, 0, 26),
                      a_form(4, 10, 1, 2, 3, 29), a_form(4, 11, 1, 2, 3, 28), a_form(4, 12, 1, 2, 3, 31),
                      a_form(4, 13, 1, 2, 3, 30), a_form(4, 14, 1, 2, 3, 14), a_form(4, 15, 1, 2, 3, 15),
                      a_form(4, 16, 1, 0, 3, 12), a_form(4, 17, 1, 0, 3, 13), blr)),
        # lfs f1,0(r5); stfs f1,8(r5); lfsx f2,r5,r6; psq_st f2,0x10(r5),0,0;
        # psq_l f3,0x10(r5),0,0; stfsux f2,r5,r7
        ("FpConversions", ((48 << 26) | (1 << 21) | (5 << 16), (52 << 26) | (1 << 21) | (5 << 16) | 8,
                           xo(31, 2, 5, 6, 535), psq(60, 2, 5, False, 0, 0x10), psq(56, 3, 5, False, 0, 0x10),
                           xo(31, 2, 5, 7, 695), blr)),
        # HID2 gating (paired-single-10): dcbz_l 0,r5 ; ps_add f4,f1,f2 ; ps_sub f5,f1,f2 ;
        # ps_add, mtspr 920,r8, ps_sub (the mtspr must re-arm the hoisted guard);
        # psq_l f1,0(r5),0,0 ; psq_lx f1,r5,r6,0,0.
        ("Hid2Dcbzl", ((4 << 26) | (5 << 11) | (1014 << 1), blr)),
        ("Hid2Paired", (a_form(4, 4, 1, 2, 0, 21), a_form(4, 5, 1, 2, 0, 20), blr)),
        ("Hid2Mtspr", (a_form(4, 4, 1, 2, 0, 21), 0x7D18E3A6, a_form(4, 5, 1, 2, 0, 20), blr)),
        ("Hid2PsqL", (psq(56, 1, 5, False, 0, 0), blr)),
        ("Hid2PsqLx", (xo(4, 1, 5, 6, 6), blr)),
    ]
    semantic_programs = []
    for index, (name, words) in enumerate(semantics):
        address = 0x80006000 + index * 0x40
        assert len(words) <= 16
        struct.pack_into(f">{len(words)}I", image, 0x100 + address - base, *words)
        rows.append((hex(address), address, f"semantics_{name.lower()}", len(words) * 4,
                     len(words), "", "", ""))
        semantic_programs.append((name, address, words))
    # Jump-table loop (audit finding branch-sys-1): the shape of fn_803E1D58's
    # token decoder. A computed bctr selects a case; every case ends in a
    # branch back to the loop head. The two cases are rows of their own that
    # the loop's row absorbs, exactly like the gap-swept cases in the DOL
    # (this fixture runs without the gap sweep, so the entry path reaches the
    # cases through a never-taken fall-through instead). A trailing row closes
    # every extent at the same end address so the cases are contained.
    #
    #   +00 li r3,0            +24 head: cmplw r3,r5
    #   +04 li r4,0            +28 bge exit
    #   +08 lis r8,hi(case1)   +2C addi r3,r3,1
    #   +0C ori r8,r8,lo       +30 rlwinm r6,r3,2,29,29   ; (r3 & 1) << 2
    #   +10 cmpwi r3,0         +34 add r7,r8,r6           ; case1 or case2
    #   +14 beq head           +38 mtctr r7
    #   +18 case1: addi r4,r4,1 +3C bctr
    #   +1C case2: addi r4,r4,2 +40 exit: blr
    #   +20 b head
    bctr_loop = 0x80007000
    case1 = bctr_loop + 0x18
    bctr_words = (
        0x38600000, 0x38800000, 0x3D000000 | (case1 >> 16), 0x61080000 | (case1 & 0xFFFF),
        0x2C030000, 0x41820010, 0x38840001, 0x38840002, 0x48000004,
        0x7C032840,  # cmplw cr0,r3,r5
        0x40800018,
        0x38630001,
        (21 << 26) | (3 << 21) | (6 << 16) | (2 << 11) | (29 << 6) | (29 << 1),
        xo(31, 7, 8, 6, 266),
        0x7CE903A6, 0x4E800420, blr)
    struct.pack_into(f">{len(bctr_words)}I", image, 0x100 + bctr_loop - base, *bctr_words)
    struct.pack_into(">I", image, 0x100 + bctr_loop + 0x44 - base, blr)
    rows.append((hex(bctr_loop), bctr_loop, "bctr_loop", 0x44, len(bctr_words), "", "", ""))
    rows.append((hex(case1), case1, "bctr_case1", 12, 3, "", "", ""))
    rows.append((hex(case1 + 4), case1 + 4, "bctr_case2", 8, 2, "", "", ""))
    rows.append((hex(bctr_loop + 0x44), bctr_loop + 0x44, "bctr_after", 4, 1, "", "", ""))
    # Interrupt recognition programs (audit batch b5). Each is translated by the
    # real generator and run natively and through the reference interpreter by
    # native_fallback_tests, which checks both stop at the same recognition
    # point with the same latched condition.
    #   MtmsrDisable - OSDisableInterrupts: mfmsr r3; rlwinm r4,r3,0,17,15;
    #                  mtmsr r4; addi r5,r5,1; blr
    #   MtmsrEnable  - OSEnableInterrupts:  mfmsr r3; ori r4,r3,0x8000;
    #                  mtmsr r4; addi r5,r5,1; blr
    #   CallReturn   - addi r3,r3,1; bl callee; addi r4,r4,1; addi r5,r5,1; blr
    #                  callee (own row): addi r6,r6,1; blr
    #   CrossLoop    - row 1: li r3,0; head: addi r3,r3,1 (falls into row 2)
    #                  row 2: cmpw r3,r5; blt head; blr - the loop's only back
    #                  edge crosses the record boundary (branch-sys-2).
    irq_programs = [
        ("MtmsrDisable", 0x80007400, (0x7C6000A6, 0x5464045E, 0x7C800124, 0x38A50001, blr), []),
        ("MtmsrEnable", 0x80007440, (0x7C6000A6, 0x60648000, 0x7C800124, 0x38A50001, blr), []),
        ("CallReturn", 0x80007480, (0x38630001, 0x4800001D, 0x38840001, 0x38A50001, blr), []),
        ("CrossLoop", 0x800074C0, (0x38600000, 0x38630001, 0x7C032800, 0x4180FFF8, blr), [2]),
    ]
    for name, address, words, splits in irq_programs:
        struct.pack_into(f">{len(words)}I", image, 0x100 + address - base, *words)
        cuts = [0] + splits + [len(words)]
        for first, last in zip(cuts, cuts[1:]):
            row = address + first * 4
            rows.append((hex(row), row, f"irq_{name.lower()}_{first}", (last - first) * 4,
                         last - first, "", "", ""))
    # CallReturn's callee row.
    struct.pack_into(">2I", image, 0x100 + 0x800074A0 - base, 0x38C60001, blr)
    rows.append((hex(0x800074A0), 0x800074A0, "irq_callreturn_callee", 8, 2, "", "", ""))
    # Two bctr sites in one function: a conditional backward jump followed by
    # a forward jump, plus an untaken arm and arbitrary external/interior targets.
    multi_bctr = 0x80007800
    multi_words = (0x2C030000, 0x4182001C, 0x38840001, 0x7D2903A6,
                   0x4E800420, 0x38840002, blr, blr, 0x7D0903A6,
                   0x4D820420, 0x38840004, 0x4BFFFFE8)
    struct.pack_into(f">{len(multi_words)}I", image, 0x100 + multi_bctr - base, *multi_words)
    for start, size, name in ((multi_bctr, 0x30, "multi_bctr"),
                              (multi_bctr + 8, 0x14, "multi_case1"),
                              (multi_bctr + 0x14, 8, "multi_case2")):
        rows.append((hex(start), start, name, size, size // 4, "", "", ""))
    dol = args.out / "fallback.dol"
    dol.write_bytes(image)
    functions = args.out / "functions.csv"
    with functions.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(("addr_hex", "addr", "name", "size", "ins", "tags", "strings", "callees"))
        writer.writerows(rows)
    (args.out / "fallback_fixture.h").write_text(
        "#pragma once\nconstexpr unsigned kTrapWords[] = {\n" +
        "".join(f"    0x{word:08X}u,\n" for word in programs) + "};\n" +
        "".join(f"constexpr unsigned kPoll{name}Address = 0x{address:08X}u;\n" +
                f"constexpr unsigned kPoll{name}Words[] = {{\n" +
                "".join(f"    0x{word:08X}u,\n" for word in words) + "};\n"
                for name, address, words in polls) +
        "".join(f"constexpr unsigned kSem{name}Address = 0x{address:08X}u;\n" +
                f"constexpr unsigned kSem{name}Words[] = {{\n" +
                "".join(f"    0x{word:08X}u,\n" for word in words) + "};\n"
                for name, address, words in semantic_programs) +
        f"constexpr unsigned kMultiBctrAddress = 0x{multi_bctr:08X}u;\n" +
        "constexpr unsigned kMultiBctrWords[] = {\n" +
        "".join(f"    0x{word:08X}u,\n" for word in multi_words) + "};\n" +
        f"constexpr unsigned kBctrLoopAddress = 0x{bctr_loop:08X}u;\n" +
        "constexpr unsigned kBctrLoopWords[] = {\n" +
        "".join(f"    0x{word:08X}u,\n" for word in bctr_words) + "};\n" +
        "".join(f"constexpr unsigned kIrq{name}Address = 0x{address:08X}u;\n" +
                f"constexpr unsigned kIrq{name}Words[] = {{\n" +
                "".join(f"    0x{word:08X}u,\n" for word in words) + "};\n"
                for name, address, words, _ in irq_programs) +
        "constexpr unsigned kIrqCallReturnCalleeAddress = 0x800074A0u;\n", encoding="utf-8")
    subprocess.run([str(args.generator.resolve()), "--dol", str(dol), "--functions", str(functions),
                    "--out", str(args.out), "--shards", "1",
                    "--no-gap-sweep", "--no-wide-entries"],
                   check=True)


if __name__ == "__main__":
    main()
