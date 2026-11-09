"""Reconciled specification model, area "loadstore" (integer load/store, lmw/stmw, strings,
lwarx/stwcx., byte-reverse loads/stores, dcbz, dcbz_l) of the IBM Gekko.

Built from two independently written models (loadstore_A.py, loadstore_B.py), cross-checked by
crosscheck_loadstore.py (decode + edge + random vectors, both models from identical cloned
states, full arch_dict + mem.log + mem.bytes compared) until 0 disagreements.  Every
disagreement was settled against docs/reference/ppc/gekko_user_manual.txt ("PAGE N") and the wrong
model was fixed in place with a page-cited comment.  This module re-exports the corrected
loadstore_A (whose contract it keeps) and adds undefined_masks().

Contract (same as loadstore_A)
    step(state, word)            normal completion, or raises UndefinedBehaviour WITHOUT touching the
                                 state; exc.undefined names what the manual leaves undefined:
                                   ('all',)  invalid form / boundedly undefined / contradictory
                                   Unmodelled subclass, ('all',): defined by the manual but dependent on
                                             state gekko_state.State lacks (see hooks)
                                   otherwise exc.apply_defined(state) applies the defined parts
    step_with_markers(state, w)  applies the defined parts and returns {location: UNDEFINED}
    undefined_masks(exc)         -> {field: bitmask} ('gpr[N]', 'cr', 'srr1', 'dsisr', 'hid2',
                                 'reserve_addr', 'pc', 'srr0', 'msr', 'exception')

Optional harness hooks on the State (consulted only with MSR[DR]=1, except dcache_hit / wpar):
    state.data_wimg(ea) -> WIMG nibble (W=8 I=4 M=2 G=1); real mode is 0b0011 (PAGE 134)
    state.data_phys(ea) -> physical address (RESERVE_ADDR <- physical_addr(EA), PAGE 450)
    state.dcache_hit(ea) -> bool (dcbz_l hit/miss, PAGE 311/384)
    state.wpar           -> WPAR (write gather pipe, PAGE 314)
Harness precondition: data accesses translate without a DSI (translation/protection not modelled).

Disagreement classes found and how they were resolved
  C1 exception SRR1/MSR  SRR1 copies MSR bits 5-9,16-23,25-31 (bit 28 "Reserved. Full function" and
                         "Full function reserved bits are saved in SRR1" PAGE 165; PM bit 29 inside
                         "SRR1[5-9, 16-31]" PAGE 166) -> A right on 28/29; bit 24 (AL, neither full
                         nor partial function, PAGE 164; absent from PAGE 537) undefined -> B right.
                         New-MSR reserved bits are 0: "Reserved bits are read as if written as 0."
                         PAGE 168 -> B right, A fixed.
  C2 dcbz/dcbz_l W/I     alignment on write-through / caching-inhibited pages (PAGE 142, PAGE 175,
                         PAGE 311, Table 5-4 PAGE 193); real-mode WIMG 0b0011 (PAGE 134) -> B right;
                         A's "caching-allowed" assumption removed (Unmodelled with DR=1 and no hook).
  C3 dcbz_l armed MC     bytes are cleared on a hit too (PAGE 384 hit bullet), only the machine check
                         fields are undefined (SRR0 best-effort, Table 4-10 PAGE 173) -> A right.
  C4 dcbz_l MSR[PR]=1    legal: PDF page 384 (pdftotext -layout) puts "Yes" under "Gekko Specific",
                         Supervisor Level blank -> A right, B's UB removed.
  C5 dcbz_l hit known    with dcache_hit attached DCHERR is defined (PAGE 311) -> B right, A extended.
  C6 lswx/stswx n=0 LE   alignment: Table 5-4 PAGE 194 "instruction attempted while MSR[LE] = 1", and
                         PAGE 198 calls the zero-length case an access -> A right.
  C7 lwarx RESERVE_ADDR  physical_addr(EA) PAGE 450; not derivable with DR=1 -> B right, A fixed.
  C8 CR0 on alignment    lwarx Rc=1 / stwcx. Rc=0 that take alignment: PAGE 119 (CR0 undefined) vs
                         PAGE 161 (not completed) do not say which wins -> CR0 undefined (A), B fixed.
  C9 write gather pipe   a store that may be gathered into the PAGE 314 FIFO is Unmodelled -> B right,
                         A extended.
  common-mode gap        lwarx/stwcx. with W=1: DSI per Table 5-4 PAGE 193 vs no DSI per PAGE 103 and
                         PAGE 119 -> UndefinedBehaviour (added to both; Unmodelled with DR=1, no hook).

Remaining documented ambiguities (both models raise UndefinedBehaviour / mark undefined)
  * string operand crossing 4K (misaligned) / 256M (aligned): PAGE 105 "always causes an alignment
    exception" vs the Gekko cause lists PAGE 159/175 and "may be invoked" PAGE 448/449/561/562
  * lmw / lswi / lswx with rA=0 ("including the case in which rA = 0", PAGE 447/448/449); lswx with
    rA or rB in the loaded range ("either ... illegal instruction ... or boundedly undefined", PAGE 449)
  * lwarx/stwcx. W=1 (above) and CR0 on the C8 exception path
  * MSR[LE]=1 byte ordering of single-register loads/stores, lwarx/stwcx., dcbz/dcbz_l (deferred to
    the Programming Environments manual, PAGE 89/105)
  * SRR1[0-4,10-15,24] and DSISR of alignment/program exceptions (PAGE 166, deferred to the PEM);
    SRR1[0] is arguably a copy (PAGE 165 note + PAGE 537) but is conservatively left undefined
  * dcbz_l machine check: pc/srr0/srr1/msr/exception (PAGE 173/311)
  * write gather pipe address window ("upper 26 bits" vs "bits 0-26", PAGE 314); gathering of stwcx.
  * lswx with n = 0: rD undefined (PAGE 449 - a manual statement, not an ambiguity)
  * reserved-field encodings (bit 31 of X-form loads/stores except stwcx., bits 6-10 of dcbz/dcbz_l):
    boundedly undefined PAGE 87/103/119
"""
from __future__ import annotations

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import loadstore_A as _A  # noqa: E402
from gekko_state import MASK32  # noqa: E402

AREA = "loadstore"
UNDEFINED = _A.UNDEFINED
UndefinedBehaviour = _A.UndefinedBehaviour
Unmodelled = _A.Unmodelled
supports = _A.supports
mnemonic = _A.mnemonic
step = _A.step
step_with_markers = _A.step_with_markers

_LOCATION_MASKS = {
    "srr1[0-4,10-15,24]": {"srr1": _A.SRR1_UNDEFINED_MASK},
    "dsisr": {"dsisr": MASK32},
    "cr0": {"cr": 0xF0000000},
    "hid2[DCHERR]": {"hid2": _A.HID2_DCHERR},
    "reserve_addr": {"reserve_addr": MASK32},
    "machine_check(pc,srr0,srr1,msr,exception)": {"pc": MASK32, "srr0": MASK32, "srr1": MASK32,
                                                  "msr": MASK32, "exception": 1},
}


def undefined_masks(exc):
    """{field: bitmask} for an UndefinedBehaviour raised by step(); {'all': 1} when nothing is defined."""
    out = {}
    for loc in exc.undefined:
        if loc == "all":
            return {"all": 1}
        m = re.fullmatch(r"gpr\[(\d+)\]", loc)
        items = {"gpr[%d]" % int(m.group(1)): MASK32} if m else _LOCATION_MASKS[loc]
        for k, v in items.items():
            out[k] = out.get(k, 0) | v
    return out


if __name__ == "__main__":
    import subprocess

    here = os.path.dirname(os.path.abspath(__file__))
    rc = subprocess.call([sys.executable, os.path.join(here, "loadstore_A.py")])
    from gekko_state import State

    fails = 0
    # every location name the model can emit must map to a mask
    probes = []
    s = State(); s.gpr[1] = 0x80001001
    probes.append((s, 0xBBC10000))                         # lmw misaligned -> alignment
    s = State(); s.msr = 0x0000B022; s.hid0 = 0x4000; s.hid2 = 0xF0080000; s.gpr[5] = 0xE0000040
    probes.append((s, 0x100007EC | (5 << 11)))            # dcbz_l armed, no hook
    s = State(); s.data_wimg = lambda ea: 0b0010; s.gpr[5] = 0x80001000
    probes.append((s, 0x7C602829 & ~(4 << 16)))           # lwarx Rc=1, DR=1, no data_phys
    s = State(); s.xer = 0; s.gpr[4] = 0x80001000
    probes.append((s, (31 << 26) | (8 << 21) | (4 << 16) | (7 << 11) | (533 << 1)))  # lswx n=0
    for st, w in probes:
        try:
            step(st.clone(), w)
            print("FAIL probe %08X completed" % w); fails += 1
        except UndefinedBehaviour as e:
            try:
                mk = undefined_masks(e)
                assert mk and "all" not in mk, mk
            except Exception as ex:  # noqa: BLE001
                print("FAIL probe %08X: %r %r" % (w, e.undefined, ex)); fails += 1
    print("loadstore (reconciled): %d mask probes, %d failures; loadstore_A self-test rc=%d" % (len(probes), fails, rc))
    sys.exit(1 if (fails or rc) else 0)
