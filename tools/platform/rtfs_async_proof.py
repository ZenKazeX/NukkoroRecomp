"""Static proof that no file in this game is opened asynchronously through RenderWare's RtFS (docs/RTGCNFS_SPEC.md).

    python -B tools/platform/rtfs_async_proof.py <main.dol> [--json out.json]

The native RtGCNFS owner (runtime/src/gekko/native_rtgcnfs.cpp) serves only the synchronous contract and refuses an
asynchronous open loudly; this check is what makes that a proven contract, not an assumption. A file record becomes
asynchronous only through
  - the access mode bit 0x10 reaching _rtFSManagerFOpenOnFS 0x8047AE1C (directly, or via RtFSManagerFOpen 0x8047AFA0),
  - or fs+0x10 (defaultToAsync) set non-zero by RtFileSystemDefaultToAsync 0x8047AA08 (_rtFSInit writes 0).

1. Completeness of the DIRECT paths, from the DOL bytes (not from a disassembler's call graph): every branch (b / bl,
   any form) into those three functions, every aligned word (data or text) equal to one of their addresses, and every
   lis + addi / ori pair forming one. Each must be a site listed below, else the proof fails.
   NOT covered: indirect calls through the file system's open slot (+0x28) that bypass _rtFSManagerFOpenOnFS, and
   stores into fs+0x10 other than through RtFileSystemDefaultToAsync - a byte-level check cannot type the receiver of
   the 105 untyped +0x28 slot calls. The owner refuses an asynchronous open / record on every path, so those cases
   stop the run loudly; this proof is what says the refusal never triggers on the direct paths.
2. Per site, the instructions that produce the argument, checked as bytes of the original code:
   - 0x8047A860 (fExists): li r5,1.
   - 0x8047AFE8 (RtFSManagerFOpen forwarding its own r4 as the mode): its only caller 0x8004547C sets li r4,1.
   - 0x8047B3B4 (RwFopen 0x8047B1C8): r5 = r31; inside RwFopen r31 is written only by li r31,0 and ori r31,r31,k with
     k in {1,2,4,8} (every instruction of the function with rA/rD = r31 is checked; r31 is callee-saved across its calls).
   - 0x8047AA08 calls: li r4,0, or r4 = a value only IsDefaultedToAsync 0x8047AA10 can have produced (dominance in the
     enclosing function: every earlier write is mr rX,r3 after that call, no path skips them, no loop carries back).
   Register writes are decoded per instruction form; an undecoded form counts as writing every register it names.
   --self-test applies seven mutations (one per check) that must each be refused.
Exit 0 = proven, 1 = not proven (the reason printed).
"""

from __future__ import annotations

import argparse
import json
import struct
import sys

OPEN_ON_FS, MANAGER_FOPEN, DEFAULT_TO_ASYNC, IS_DEFAULT_ASYNC = 0x8047AE1C, 0x8047AFA0, 0x8047AA08, 0x8047AA10
RWFOPEN, RWFOPEN_END = 0x8047B1C8, 0x8047B3D8
TARGETS = {OPEN_ON_FS: "_rtFSManagerFOpenOnFS", MANAGER_FOPEN: "RtFSManagerFOpen",
           DEFAULT_TO_ASYNC: "RtFileSystemDefaultToAsync"}
# Every reference the proof accounts for: site -> target.
EXPECTED = {0x8047A860: OPEN_ON_FS, 0x8047AFE8: OPEN_ON_FS, 0x8047B3B4: OPEN_ON_FS, 0x8004547C: MANAGER_FOPEN,
            0x800444E8: DEFAULT_TO_ASYNC, 0x800445DC: DEFAULT_TO_ASYNC, 0x80306BA0: DEFAULT_TO_ASYNC,
            0x80306C38: DEFAULT_TO_ASYNC}


def load_dol(path: str) -> tuple[dict[int, int], list[tuple[int, int]]]:
    data = open(path, "rb").read()
    words: dict[int, int] = {}
    text: list[tuple[int, int]] = []
    for i in range(18):
        off, addr, size = (struct.unpack_from(">I", data, base + i * 4)[0] for base in (0, 0x48, 0x90))
        if size == 0:
            continue
        for k in range(0, size, 4):
            words[addr + k] = struct.unpack_from(">I", data, off + k)[0]
        if i < 7:
            text.append((addr, addr + size))
    return words, text


def is_text(at: int, text: list[tuple[int, int]]) -> bool:
    return any(a <= at < b for a, b in text)


def branch_target(at: int, w: int) -> int | None:
    op = w >> 26
    if op == 18:  # b / bl / ba / bla
        li = w & 0x03FFFFFC
        if li & 0x02000000:
            li -= 0x04000000
        return (li if w & 2 else at + li) & 0xFFFFFFFF
    if op == 16:  # bc
        bd = w & 0xFFFC
        if bd & 0x8000:
            bd -= 0x10000
        return (bd if w & 2 else at + bd) & 0xFFFFFFFF
    return None


def li(rd: int, imm: int) -> int:  # addi rd, 0, imm
    return (14 << 26) | (rd << 21) | (imm & 0xFFFF)


def mr(ra: int, rs: int) -> int:  # or ra, rs, rs
    return (31 << 26) | (rs << 21) | (ra << 16) | (rs << 11) | (444 << 1)


# GPR destinations per instruction form; anything not decoded here counts as writing every register it names
# (conservative: an unknown form can only make the proof fail, never pass).
DEST_RD = {7, 8, 12, 13, 14, 15, 32, 34, 40, 42}            # mulli subfic addic addic. addi addis lwz lbz lhz lha
DEST_RD_UPDATE = {33, 35, 41, 43}                         # lwzu lbzu lhzu lhau: rD and rA
DEST_RA = {20, 21, 23, 24, 25, 26, 27, 28, 29}            # rlwimi rlwinm rlwnm ori oris xori xoris andi. andis.
NO_GPR = {3, 10, 11, 16, 17, 18, 19, 36, 38, 44, 47, 48, 50, 52, 54, 56, 57, 59, 60, 61, 63, 4}
STORE_UPDATE = {37, 39, 45, 49, 51, 53, 55}               # stwu stbu sthu lfsu lfdu stfsu stfdu: rA only
X_LOGICAL = {28, 60, 124, 284, 316, 412, 444, 476, 24, 536, 792, 824, 26, 954, 922, 986}  # and andc nor eqv xor orc or nand slw srw sraw srawi cntlzw extsh extsb extsw: rA
X_NO_GPR = {0, 32, 4, 151, 215, 407, 663, 727, 919, 983, 662, 918, 150, 86, 54, 278, 246, 1014, 470, 598, 854, 982,
            144, 146, 210, 242}                           # cmp cmpl tw stores dcb* sync eieio isync mtcrf mtmsr mtsr
X_UPDATE = {55, 119, 311, 375, 183, 247, 439, 695, 759}   # indexed load/store with update


def may_write(w: int, reg: int) -> bool:
    op = w >> 26
    rd, ra = (w >> 21) & 31, (w >> 16) & 31
    if op in NO_GPR:
        return False
    if op in DEST_RD:
        return rd == reg
    if op in DEST_RD_UPDATE:
        return rd == reg or ra == reg
    if op in STORE_UPDATE:
        return ra == reg
    if op in DEST_RA:
        return ra == reg
    if op == 46:  # lmw: rD..r31
        return reg >= rd
    if op == 31:
        xo10 = (w >> 1) & 0x3FF
        if xo10 in X_NO_GPR:
            return False
        if xo10 in X_LOGICAL:
            return ra == reg
        if xo10 in X_UPDATE:
            return ra == reg or (xo10 in (55, 119, 311, 375, 790, 534) and rd == reg)
        # XO-form arithmetic, loads, mfspr / mfcr / mfmsr: rD
        return rd == reg
    return rd == reg or ra == reg  # unknown form


def branch_targets(words: dict[int, int], text: list[tuple[int, int]]) -> set[int]:
    out = set()
    for at, w in words.items():
        if is_text(at, text):
            t = branch_target(at, w)
            if t is not None:
                out.add(t)
    return out


def last_write(words: dict[int, int], site: int, reg: int, floor: int,
               joins: set[int] | None = None) -> tuple[int, int] | None:
    """The nearest instruction before `site`, on the single straight-line path back to `floor`, that may write `reg`.
    None when the path is not straight-line: a conditional or unconditional branch, a return, or a join point (a
    branch target) between the write and the site. bl is passed only for callee-saved r14-r31."""
    at = site - 4
    while at >= floor:
        if joins is not None and (at + 4) in joins and at + 4 != site:
            return None
        w = words[at]
        op = w >> 26
        if op == 18 and (w & 1):
            if reg >= 14:
                at -= 4
                continue
            return None
        if op == 16 and not (w & 1):  # a conditional branch leaving the path: fall-through continues it
            at -= 4
            continue
        if op in (16, 18, 19):  # an unconditional branch / return / bctr, or a branch-and-link
            return None
        if may_write(w, reg):
            return at, w
        at -= 4
    return None


# The functions holding the DefaultToAsync calls: (start, end) from dtk's symbol table (sizes 0x2B4 / 0x238).
FUNCTIONS = {0x800444E8: (0x80044344, 0x800445F8), 0x800445DC: (0x80044344, 0x800445F8),
             0x80306BA0: (0x80306B48, 0x80306D80), 0x80306C38: (0x80306B48, 0x80306D80)}


def dominated_restore(words: dict[int, int], bounds: tuple[int, int], src: int, use: int) -> bool:
    """r`src` (callee-saved) at `use` holds an IsDefaultedToAsync result: every instruction in [start, use) of the
    function that may write it is mr src,r3 directly after bl IsDefaultedToAsync; no branch located before the first
    such definition targets (first, use] (so the caller's value cannot reach `use`); and no branch located after
    `use` targets [start, use] (no loop carries a later value back)."""
    start, end = bounds
    if src < 14:
        return False
    before = [at for at in range(start, use, 4) if may_write(words[at], src)]
    # the prologue's stmw / stw save the caller's value: they store, they are not writes (may_write says so)
    if not before:
        return False
    for at in before:
        if words[at] != mr(src, 3) or branch_target(at - 4, words[at - 4]) != IS_DEFAULT_ASYNC:
            return False
    first = before[0]
    for at in range(start, end, 4):
        t = branch_target(at, words[at])
        if t is None:
            continue
        if at < first and first < t <= use:
            return False
        if at > use and start <= t <= use:
            return False
    return True


def indirect_slot_calls(words: dict[int, int], text: list[tuple[int, int]], offset: int) -> list[int]:
    """lwz rN,offset(rX) followed within 7 instructions by mtctr rN and then bctrl."""
    out = []
    for at, w in sorted(words.items()):
        if not is_text(at, text) or w >> 26 != 32 or (w & 0xFFFF) != offset:
            continue
        rn = (w >> 21) & 31
        for k in range(1, 8):
            y = words.get(at + 4 * k)
            if y == (31 << 26) | (rn << 21) | (9 << 16) | (467 << 1):
                if any(words.get(at + 4 * j) == 0x4E800421 for j in range(k + 1, k + 6)):
                    out.append(at)
                break
    return out


def prove(words: dict[int, int], text: list[tuple[int, int]]) -> tuple[dict[int, tuple[int, str]], list[str]]:
    """The completeness scan and the per-site argument checks (see the module comment)."""
    refs: dict[int, tuple[int, str]] = {}
    for at, w in words.items():
        if is_text(at, text):
            t = branch_target(at, w)
            if t in TARGETS:
                refs[at] = (t, "branch")
            if w >> 26 == 15:  # lis rD, hi: look for the low half in the next 8 instructions
                rd, hi = (w >> 21) & 31, (w & 0xFFFF) << 16
                for k in range(1, 9):
                    w2 = words.get(at + 4 * k)
                    if w2 is None:
                        break
                    if (w2 >> 26 == 14 and (w2 >> 16) & 31 == rd) or (w2 >> 26 == 24 and (w2 >> 21) & 31 == rd):
                        lo = w2 & 0xFFFF
                        v = (hi + (lo - 0x10000 if (w2 >> 26) == 14 and lo & 0x8000 else lo)) & 0xFFFFFFFF
                        if v in TARGETS:
                            refs[at] = (v, "address constant")
            if w in TARGETS and at not in refs:
                refs[at] = (w, "word in text")
        elif w in TARGETS:
            refs[at] = (w, "data word")
    joins = branch_targets(words, text)
    problems: list[str] = []
    for at, (t, how) in sorted(refs.items()):
        if EXPECTED.get(at) != t or how != "branch":
            problems.append(f"unaccounted reference to {TARGETS[t]} at 0x{at:08X} ({how})")
    for at in EXPECTED:
        if at not in refs:
            problems.append(f"expected call at 0x{at:08X} not found (the DOL is not the analysed one)")

    def need(site: int, floor: int, reg: int, allowed: dict[int, str]) -> None:
        got = last_write(words, site, reg, floor, joins)
        if got is None or got[1] not in allowed:
            problems.append(f"0x{site:08X}: r{reg} not set by an accepted instruction "
                            f"({'none' if got is None else f'{got[1]:08X} at 0x{got[0]:08X}'})")

    need(0x8047A860, 0x8047A840, 5, {li(5, 1): "li r5,1"})
    need(0x8047AFE8, 0x8047AFA0, 5, {mr(5, 29): "mr r5,r29"})
    need(0x8047AFE8, 0x8047AFA0, 29, {mr(29, 4): "mr r29,r4"})  # RtFSManagerFOpen's own mode argument
    need(0x8004547C, 0x80045450, 4, {li(4, 1): "li r4,1"})
    need(0x8047B3B4, RWFOPEN, 5, {mr(5, 31): "mr r5,r31"})
    # r31 inside RwFopen: every instruction whose rD / rS (bits 21-25) or rA (bits 16-20) field is r31 must be one
    # of li r31,0 / ori r31,r31,{1,2,4,8} / mr r5,r31; lmw (which reloads r31) only after the open call.
    ok_r31 = {li(31, 0), mr(5, 31)} | {(24 << 26) | (31 << 21) | (31 << 16) | k for k in (1, 2, 4, 8)}
    for at in range(RWFOPEN, RWFOPEN_END, 4):
        w = words[at]
        op = w >> 26
        if op == 46 and at > 0x8047B3B4:  # lmw: the epilogue restores the caller's registers
            continue
        if op == 47:  # stmw: stores the caller's registers (sources)
            continue
        if op == 46:
            problems.append(f"RwFopen reloads r31 (lmw) at 0x{at:08X} before the open")
        if ((w >> 21) & 31 == 31 or (w >> 16) & 31 == 31) and w not in ok_r31 and w >> 26 not in (16, 18, 19):
            problems.append(f"RwFopen names r31 at 0x{at:08X} with {w:08X} (not an accepted instruction)")
    # restore sites inside these functions (start, end) - the dominance argument below needs the whole body
    for site, floor in ((0x800444E8, 0x800444D0), (0x800445DC, 0x800445CC), (0x80306BA0, 0x80306B88),
                        (0x80306C38, 0x80306C28)):
        got = last_write(words, site, 4, floor, joins)
        if got is not None and got[1] == li(4, 0):
            continue
        # restoring: r4 = rX (callee-saved) where rX's last write is mr rX,r3 right after bl IsDefaultedToAsync
        # (no other write of rX in between: last_write walks the path back, across calls only for r14-r31)
        if got is not None and got[1] >> 26 == 31 and ((got[1] >> 1) & 0x3FF) == 444 and (got[1] >> 21) & 31 >= 14:
            src = (got[1] >> 21) & 31
            if dominated_restore(words, FUNCTIONS[site], src, got[0]):
                continue
        problems.append(f"0x{site:08X}: RtFileSystemDefaultToAsync argument not 0 nor a restored value")
    return refs, problems


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("dol")
    ap.add_argument("--json")
    ap.add_argument("--self-test", action="store_true", help="also prove that three mutated images are refused")
    a = ap.parse_args(argv)
    words, text = load_dol(a.dol)
    refs, problems = prove(words, text)
    if a.self_test:
        # Negative controls: an opener passing 0x11, a data word naming FOpenOnFS, RwFopen or-ing 0x10 into the
        # mode. Each must be NOT proven, or the check proves nothing.
        first_data = min(at for at in words if not is_text(at, text))
        ori = lambda ra, rs, k: (24 << 26) | (rs << 21) | (ra << 16) | k
        lis = lambda rd, k: (15 << 26) | (rd << 21) | k
        for name, patch in (("li r5,0x11 at 0x8047A848", {0x8047A848: li(5, 0x11)}),
                            ("a data word = 0x8047AE1C", {first_data: OPEN_ON_FS}),
                            ("ori r31,r31,0x18 at 0x8047B244", {0x8047B244: ori(31, 31, 0x18)}),
                            ("ori r5,r5,0x10 just before the fExists call", {0x8047A858: ori(5, 5, 0x10)}),
                            ("li r4,1 into RtFileSystemDefaultToAsync", {0x800444E4: li(4, 1)}),
                            ("li r4,0x11 at RtFSManagerFOpen's caller", {0x80045464: li(4, 0x11)}),
                            ("lis/ori forming 0x8047AE1C", {0x80300000: lis(3, 0x8047), 0x80300004: ori(3, 3, 0xAE1C)})):
            mutated = dict(words)
            mutated.update(patch)
            if not prove(mutated, text)[1]:
                problems.append(f"self-test: the mutation {name} was not refused")
            else:
                print(f"self-test: {name} refused")
    # Scope: the static proof covers the direct API paths. The open owner is also the file system's slot +0x28; the
    # DOL has indirect calls through a +0x28 slot this byte-level check cannot type (counted here, unproven). On those
    # paths the guarantee is the owner's own refusal (native_rtgcnfs.cpp refuse_async), not this proof.
    slot_calls = indirect_slot_calls(words, text, 0x28)
    print(f"scope: direct API paths proven; {len(slot_calls)} indirect calls through a +0x28 slot not typed here "
          "(guarded by the owner's refusal)")
    result = {"proven": not problems, "untyped_slot_0x28_calls": [f"0x{x:08X}" for x in slot_calls], "references": {f"0x{k:08X}": TARGETS[v[0]] for k, v in sorted(refs.items())},
              "problems": problems}
    if a.json:
        with open(a.json, "w", newline="\n") as f:
            json.dump(result, f, indent=1)
    for p in problems:
        print("NOT PROVEN:", p)
    print(f"rtfs_async_proof: {len(refs)} references, {'proven' if not problems else 'NOT proven'}")
    return 0 if not problems else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
