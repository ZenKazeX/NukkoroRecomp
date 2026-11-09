"""Static storage in CodeWarrior's runtime: start-up initializers, exit-time destructors and function-local static
getters, as names.py rules N14 / N15 read them (Astra answer 5 item 16; answer 6 items 1 and 4).

The runtime is matched by dtk's exact-byte library names and pinned here as WHOLE bodies (RUNTIME_BODIES): every word
exact, except the two halves of the _ctors address (which must build _ctors) and the small-data offset of
__global_destructor_chain (one offset shared by every chain access). Read from the PAL binary 2026-11-01:
  __init_cpp               r31 = _ctors; loop (entered at its test) { mtlr r12; blrl; r31 += 4; r12 = *r31;
                           cmplwi r12, 0; bne }: every .ctors word before the first 0 is called once, before main
  __register_global_object node r5 = {chain, destructor r4, object r3}; chain = r5
  __destroy_global_chain   while (chain) { node = chain; chain = node[0]; node[1](node[2], -1) }
"""
from __future__ import annotations

import collections

from . import ppc
from . import vcalls

HA, LO, CHAIN = "ha", "lo", "chain"          # placeholders: (kind, the instruction's high 16 bits)
RUNTIME_BODIES = {
    "__init_cpp": [0x7C0802A6, 0x90010004, 0x9421FFF0, 0x93E1000C, (HA, 0x3C60), (LO, 0x3803), 0x7C1F0378,
                   0x48000004, 0x48000004, 0x48000010, 0x7D8803A6, 0x4E800021, 0x3BFF0004, 0x819F0000, 0x280C0000,
                   0x4082FFEC, 0x80010014, 0x83E1000C, 0x38210010, 0x7C0803A6, 0x4E800020],
    "__register_global_object": [(CHAIN, 0x800D), 0x90050000, 0x90850004, 0x90650008, (CHAIN, 0x90AD), 0x4E800020],
    "__destroy_global_chain": [0x9421FFF0, 0x7C0802A6, 0x90010014, 0x48000020, 0x80030000, 0x3880FFFF,
                               (CHAIN, 0x900D), 0x81830004, 0x80630008, 0x7D8903A6, 0x4E800421, (CHAIN, 0x806D),
                               0x28030000, 0x4082FFDC, 0x80010014, 0x7C0803A6, 0x38210010, 0x4E800020],
}


def runtime_proven(image, address, size, ctors) -> bool:
    """True when the three runtime functions (address: {name: start}, size: {start: bytes}) are RUNTIME_BODIES and
    __init_cpp's lis / addi build ctors, the address of _ctors."""
    chain, hi, lo = set(), None, None
    for name, body in RUNTIME_BODIES.items():
        a = address.get(name)
        if a is None or size.get(a) != 4 * len(body):
            return False
        for k, want in enumerate(body):
            w = image.word(a + 4 * k)
            if isinstance(want, tuple):
                if w >> 16 != want[1]:
                    return False
                if want[0] == CHAIN:
                    chain.add(w & 0xFFFF)
                elif want[0] == HA:
                    hi = w & 0xFFFF
                else:
                    lo = w & 0xFFFF
            elif w != want:
                return False
    return (len(chain) == 1 and ctors is not None and hi is not None and lo is not None
            and ((hi << 16) + ppc.simm(lo)) & 0xFFFFFFFF == ctors)


def static_initializers(image, ctors) -> list:
    """[(.ctors slot, function)] for every word from _ctors up to the first 0 (what __init_cpp calls)."""
    sec = image.section_at_address(ctors)
    out, a = [], ctors
    while sec is not None and a + 4 <= sec.address_end and image.word(a) != 0:
        out.append((a, image.word(a)))
        a += 4
    return out


def address_of(v, sda):
    """An absolute address for a must-analysis value: an int, or (sdaN, d) with sda {N: base}."""
    if isinstance(v, int):
        return v
    if isinstance(v, tuple) and v[0] in ("sda2", "sda13") and sda.get(int(v[0][3:])) is not None:
        return (sda[int(v[0][3:])] + v[1]) & 0xFFFFFFFF
    return None


def exit_destructors(image, register, registrants, size, entries, sda=None) -> tuple[dict, list]:
    """({destructor: {(registering function, call site, object, node)}}, [(function, site) with no constant
    destructor]) over every `bl __register_global_object` in the registrants (object / node: their addresses, None
    when not constant)."""
    sda = sda or {}
    out, unresolved = collections.defaultdict(set), []
    for g in sorted(registrants):
        def visit(pc, w, regs, consts, g=g):
            b = ppc.branch(w, pc)
            if b is not None and b.link and b.target == register:
                if isinstance(consts.get(4), int):
                    out[consts[4]].add((g, pc, address_of(consts.get(3), sda), address_of(consts.get(5), sda)))
                else:
                    unresolved.append((g, pc))
        vcalls.Function(image, g, size[g], entries.get(g, ())).scan(set(), visit)
    return out, unresolved


def dominators(blocks, start) -> dict:
    """{block: set of blocks dominating it} for vcalls.Function.blocks() reached from start."""
    preds = collections.defaultdict(set)
    reach, todo = {start}, [start]
    while todo:
        s = todo.pop()
        for t in blocks[s][1]:
            preds[t].add(s)
            if t not in reach:
                reach.add(t)
                todo.append(t)
    dom = {s: set(reach) for s in reach}
    dom[start] = {start}
    changed = True
    while changed:
        changed = False
        for s in sorted(reach - {start}):
            new = {s} | set.intersection(*(dom[p] for p in preds[s] if p in reach)) if preds[s] else {s}
            if new != dom[s]:
                dom[s], changed = new, True
    return dom


def singleton(image, g, size, entries, register, sda, receivers) -> tuple | None:
    """(object, destructor, cache, constructors run on the object) when g is a function-local static's getter as CodeWarrior compiles one, proven
    step by step (Astra answer 6 item 1); None otherwise:
      1. exactly one `bl __register_global_object`, with a constant object O and destructor D;
      2. a function proven to return this ran on O before it, on every path (vcalls ("built", O));
      3. its block is dominated by a block entered only where a guard byte G was loaded and tested 0 (an "isz"
         edge fact), and is not g's entry;
      4. the block after the call (its only successor) stores a non-zero byte to G: the guard is published after
         construction and registration;
      5. every reached return (blr, or a conditional bclr) gives O, or a load of one global C made where C is
         known non-zero (a non-zero test on every path, or O stored there on every path)."""
    fn = vcalls.Function(image, g, size, entries)
    fn.loads = fn.statics = True
    fn.sda = dict(sda or {})
    for c in receivers:
        fn.returns[c] = "receiver"
    blocks = fn.blocks()
    state_at, regs, rets, byte_stores = {}, [], [], []

    def visit(pc, w, regs_, consts):
        if pc in blocks:
            state_at[pc] = dict(consts)
        b = ppc.branch(w, pc)
        if b is not None and b.link and b.target == register:
            regs.append((pc, address_of(consts.get(3), fn.sda), consts.get(4), consts.get(("built", consts.get(3)))))
        if vcalls.is_return(w, pc):
            rets.append(consts.get(3))
        op, src, ra, _, imm = vcalls.fields(w)
        if op == 38:
            base = 0 if ra == 0 else fn.sda.get(ra, consts.get(ra))
            if isinstance(base, int):
                byte_stores.append((pc, (base + ppc.simm(imm)) & 0xFFFFFFFF, consts.get(src)))
    fn.scan(set(), visit)
    if len(regs) != 1 or not rets:
        return None
    pc, obj, dtor, built = regs[0]
    if obj is None or not isinstance(dtor, int) or not built or built == vcalls.ANY:
        return None
    block = max(s for s in blocks if s <= pc)
    dom = dominators(blocks, fn.start)
    if block not in dom or block == fn.start:
        return None
    guards = {k[1][1] for s in dom[block] for k in state_at.get(s, {})
              if isinstance(k, tuple) and k[0] == "isz" and k[1][0] == "b" and isinstance(k[1][1], int)}
    after = blocks[block][1]
    if blocks[block][0] != pc + 4 or after != [pc + 4]:
        return None
    end = blocks[pc + 4][0]
    if not any(pc + 4 <= p < end and a in guards and isinstance(v, int) and v & 0xFF for p, a, v in byte_stores):
        return None
    caches = set()
    for v in rets:
        if v == obj or address_of(v, fn.sda) == obj:               # li r3, X@sda21 is (sda13, d)
            continue
        if isinstance(v, tuple) and v[0] == "load_nz" and isinstance(v[1], int):
            caches.add(v[1])
        else:
            return None
    if len(caches) > 1:
        return None
    return obj, dtor, caches.pop() if caches else None, built


STORES = vcalls.STORE_BYTES


def store_index(image, funcs, size) -> dict:
    """{("sda2" | "sda13", d) | ("hi", h): functions}: every function with a D-form store at r2 / r13 + d, or a
    `lis` of h - the ones that can write a global directly. Both the ha-adjusted and the plain high half of an
    address are looked up (lis + addi vs lis + ori)."""
    out = collections.defaultdict(set)
    for f in funcs:
        for x in range(f, f + size[f], 4):
            w = image.word(x)
            if w >> 26 in STORES and (w >> 16) & 31 in (2, 13):
                out[(f"sda{(w >> 16) & 31}", ppc.simm(w & 0xFFFF))].add(f)
            elif w >> 26 == 15 and (w >> 16) & 31 == 0:
                out[("hi", w & 0xFFFF)].add(f)
    return out


def cache_holds_only(image, index, size, entries, cache, obj, sda, in_bss) -> bool:
    """True when the global `cache` starts as 0 or obj (in_bss: an unmapped cache must lie in .bss / .sbss) and
    every store into its 4 bytes in the functions store_index lists for it is a `stw` of obj or 0 at its address.
    An indexed store with one operand a known constant within 32 KiB of the cache makes it unprovable. Not seen: a
    store through a pointer the must-analysis does not know."""
    sec = image.section_at_address(cache)
    if sec is None and not in_bss or sec is not None and image.word(cache) not in (0, obj):
        return False
    funcs = set()
    for h in {((cache + 0x8000) >> 16) & 0xFFFF, (cache >> 16) & 0xFFFF}:
        funcs |= index.get(("hi", h), set())
    for n, base in (sda or {}).items():
        if base is not None:
            for d in range(cache - base - 31 * 4, cache - base + 4):       # stmw may cover it from below
                funcs |= index.get((f"sda{n}", d), set())
    ok = True
    for f in sorted(funcs):
        fn = vcalls.Function(image, f, size[f], entries.get(f, ()))
        fn.sda = dict(sda or {})

        def visit(pc, w, regs, consts):
            nonlocal ok
            op, src, ra, rb, imm = vcalls.fields(w)
            if op == 31 and (w >> 1) & 0x3FF in vcalls.X_STORES:
                if any(isinstance(consts.get(r), int) and abs(consts[r] - cache) < 0x8000 for r in (ra, rb)):
                    ok = False
                return
            if op not in STORES:
                return
            base = 0 if ra == 0 else fn.sda.get(ra, consts.get(ra))
            if not isinstance(base, int):
                return
            at = (base + ppc.simm(imm)) & 0xFFFFFFFF
            width = 4 * (32 - src) if op == 47 else STORES[op]
            if at < cache + 4 and cache < at + width:
                if not (op == 36 and at == cache and consts.get(src) in (obj, 0)):
                    ok = False
        fn.scan(set(), visit)
        if not ok:
            return False
    return True
