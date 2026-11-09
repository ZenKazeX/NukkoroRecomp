"""F1c long-run canary guest programs (imported by gen_f1c.py; test-only).

OS exception handlers (M1, docs/memory-architecture-20261006: no real mode). The CPU performs the
architected entry and then the OS's exception entry, which enters the handler __OSExceptionTable names with
r4 = the current OSContext, where the interrupted r3-r5, CR, SRR0 and SRR1 are saved; the driver installs the
entry. The handlers are translated rows in the window and return the way OSLoadContext does:
  CANARY_BASE + 0x500 external    addi r23,r23,1 ; lwz r21,0x19C(r4) (SRR1) ; rlwinm r22,r21,0,17,15 (EE off) ;
                                  mtspr SRR1,r22 ; SRR0, CR, r3, r5, r4 from the context ; rfi
  CANARY_BASE + 0x900 decrement.  addi r24,r24,1 ; lis/ori r22,DEC_RELOAD ; mtspr DEC,r22 ; SRR1, SRR0, CR, r3,
                                  r5, r4 from the context ; rfi
The system call is PPCSync's head, which the entry performs natively: no guest handler runs.

`longrun` (entry CANARY_BASE; LR = sentinel; r5 = outer iterations; r6 = data window; r10/r11 scratch):
  outer:  EE on (mfmsr/ori/mtmsr), r25++
          inner loop A (EE = 1, INNER_A iterations): r3++, a store and a load that straddle the 16 KiB host
            page boundary at 0x80303FFE, compare (a mismatch branches to `fail`), bl LEAF, bctrl LEAF, a local
            jump table (bctr to one of four hot labels, r27 += case + 1), back edge
          EE off (mfmsr/rlwinm/mtmsr)
          inner loop B (EE = 0, INNER_B iterations): arithmetic, back edge
          r28++, sc (the native entry performs PPCSync's head and resumes after it through the dispatcher)
          RAM poll idle loop `lwz r0,0x20(r6); cmplwi r0,0; beq poll` (native time: the driver's device sets
            the word when the guest arrives at the loop head - the coalescing to the next hardware service it
            was written for is deleted with the instruction clock), r26++, clear the word
          r4++ ; blt outer
  exit:   mtlr r31 ; blr (untranslated sentinel)
  fail:   r29 = 0xDEAD, exit

`pollwrap` (entry CANARY_BASE + 0x400): EE = 0 RAM poll idle loop only, then exit; used by the counter
wraparound probes at huge service intervals - canary phases B2/B3, RETIRED with the instruction clock (native
time: the driver refuses them). The row stays so every other row keeps its address.

Guest-visible counters the driver checks against host ledgers:
  r3 = outer*INNER_A + outer*INNER_B, r4 = outer, r7 = 2*outer*INNER_A (leaf calls), r23 = 0x500 entries,
  r24 = 0x900 entries, r25 = outer, r26 = outer (poll exits), r27 = sum over A iterations of ((r3 & 3) + 1),
  r28 = sc executions = outer (the driver checks them against the sc census), r29 = 0 (no store/load
  mismatch).
"""
from __future__ import annotations

import gen_f1c as G
import ppcenc as E
import ppcenc_ext as X

INNER_A = 2000
INNER_B = 3000
DEC_RELOAD = 100000          # the guest's mtdec reload in time-base ticks; the driver delivers each edge as the
                             # game-clock scheduler does (it was 1.2M retired instructions under the instruction clock)
POLL_WORD = 0x20
STRADDLE = 0x3FFE            # r6 + 0x3FFE = 0x80303FFE: a 4-byte access across the 16 KiB host page boundary
POLLWRAP_OFFSET = 0x400


def mtcrf_all(s):
    """mtcrf 0xFF,rS"""
    return (31 << 26) | (s << 21) | (0xFF << 12) | (144 << 1)


def return_through_context():
    """SRR0, CR and r3-r5 from the OSContext in r4 (r4 last), then rfi - OSLoadContext's tail."""
    mt = E.mtspr
    return [G.lwz(22, 4, 0x198), mt(22, 26), G.lwz(22, 4, 0x80), mtcrf_all(22),
            G.lwz(3, 4, 0x0C), G.lwz(5, 4, 0x14), G.lwz(4, 4, 0x10), X.RFI]


def handlers(fx):
    mt = E.mtspr
    ext = G.Asm(G.CANARY_BASE + 0x500)
    ext.w(E.addi(23, 23, 1), G.lwz(21, 4, 0x19C), X.rlwinm(22, 21, 0, 17, 15), mt(22, 27),
          *return_through_context())
    fx.add_rows(ext.rows("canary_ext"))
    fx.os_handlers["External"] = G.CANARY_BASE + 0x500
    dec = G.Asm(G.CANARY_BASE + 0x900)
    dec.w(E.addi(24, 24, 1), E.addis(22, 0, DEC_RELOAD >> 16), E.ori(22, 22, DEC_RELOAD & 0xFFFF), mt(22, 22),
          G.lwz(22, 4, 0x19C), mt(22, 27), *return_through_context())
    fx.add_rows(dec.rows("canary_dec"))
    fx.os_handlers["Decrementer"] = G.CANARY_BASE + 0x900


def longrun(fx, base):
    a = G.Asm(base)
    a.w(E.MFLR(31), G.li(4, 0), G.li(29, 0))
    # Never-taken branches (r5 != 0) that make the jump-table cases hot labels of this record.
    a.w(G.cmplwi(0, 5, 0))
    for k in range(4):
        a.br("beq", f"case{k}")
    a.L("outer")
    a.w(X.mfmsr(9), E.ori(9, 9, 0x8000), X.mtmsr(9), E.addi(25, 25, 1))
    a.w(G.li(10, INNER_A))
    a.L("in_a")
    a.w(E.addi(3, 3, 1), G.stw(3, 6, STRADDLE), G.lwz(8, 6, STRADDLE), E.cmplw(0, 8, 3))
    a.br("bne", "fail")
    a.br("bl_abs", str(G.LEAF))
    a.w(G.lwz(12, 6, 0), E.MTCTR(12), E.bcctr(20, 0, 1))
    # r11 = ((r3 & 3) << 2) + 0x24 ; r12 = [r6 + r11] ; bctr
    a.w(X.rlwinm(11, 3, 2, 28, 29), E.addi(11, 11, 0x24), E.x_form(31, 12, 11, 6, 23), E.MTCTR(12),
        E.bcctr(20, 0))
    for k in range(4):
        a.L(f"case{k}")
        a.w(E.addi(27, 27, k + 1))
        a.br("b", "join")
    a.L("join")
    a.w(E.addi(10, 10, -1), G.cmplwi(0, 10, 0))
    a.br("bne", "in_a")
    a.w(X.mfmsr(9), X.rlwinm(9, 9, 0, 17, 15), X.mtmsr(9))
    a.w(G.li(10, INNER_B))
    a.L("in_b")
    a.w(E.addi(3, 3, 1), E.xo_form(11, 3, 10, 266), X.rlwinm(11, 11, 3, 0, 31), E.addi(10, 10, -1),
        G.cmplwi(0, 10, 0))
    a.br("bne", "in_b")
    a.w(E.addi(28, 28, 1), X.SC)
    a.L("poll")
    a.w(G.lwz(0, 6, POLL_WORD), G.cmplwi(0, 0, 0))
    a.br("beq", "poll")
    a.w(E.addi(26, 26, 1), G.li(0, 0), G.stw(0, 6, POLL_WORD))
    a.w(E.addi(4, 4, 1), E.cmplw(0, 4, 5))
    a.br("blt", "outer")
    a.L("exit")
    a.w(E.MTLR(31), E.BLR)
    a.L("fail")
    a.w(E.addis(29, 0, 0), E.ori(29, 29, 0xDEAD))
    a.br("b", "exit")
    rows = a.rows("canary_longrun")
    fx.add_rows(rows)
    table = {0: G.LEAF, 16: G.SENTINEL}
    for k in range(4):
        table[0x24 + 4 * k] = a.labels[f"case{k}"]
    fx.canary.append({"name": "longrun", "entry": base, "rows": [r["address"] for r in rows],
                      "table": {str(k): v for k, v in sorted(table.items())},
                      "labels": dict(a.labels), "inner_a": INNER_A, "inner_b": INNER_B, "dec_reload": DEC_RELOAD,
                      "poll_word": POLL_WORD, "straddle": STRADDLE})


def pollwrap(fx, base):
    a = G.Asm(base)
    a.w(E.MFLR(31))
    a.w(E.addi(3, 3, 1))              # 1 instruction before the loop (plus mflr: 2)
    a.L("poll")
    a.w(G.lwz(0, 6, POLL_WORD), G.cmplwi(0, 0, 0))
    a.br("beq", "poll")
    a.w(E.MTLR(31), E.BLR)
    rows = a.rows("canary_pollwrap")
    fx.add_rows(rows)
    fx.canary.append({"name": "pollwrap", "entry": base, "rows": [r["address"] for r in rows],
                      "table": {"0": G.LEAF, "16": G.SENTINEL}, "labels": dict(a.labels),
                      "prefix_instructions": 2, "loop_instructions": 3, "poll_word": POLL_WORD})


def build(fx):
    handlers(fx)
    longrun(fx, G.CANARY_BASE)
    pollwrap(fx, G.CANARY_BASE + POLLWRAP_OFFSET)
