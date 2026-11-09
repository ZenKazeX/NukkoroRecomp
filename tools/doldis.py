#!/usr/bin/env python
"""doldis.py - disassemble / dump ranges of Shadow the Hedgehog's main.dol.

Usage:
  python tools/doldis.py 800BF9D8:64            disassemble 64 instructions
  python tools/doldis.py 800BF9D8:64 8000F654:40
  python tools/doldis.py --func 800BF9D8        disassemble until blr/terminator (cap 4000)
  python tools/doldis.py --f32 805F27C0:4       dump 4 big-endian floats
  python tools/doldis.py --u32 8058516C:12      dump 12 big-endian words
  python tools/doldis.py --xref 800BF9D8        every `bl`/`b` site targeting that address
  python tools/doldis.py --findf32 1.140625     every aligned f32 equal to the value
Addresses are hex without 0x.  Never shadows stdlib `dis`.
"""
import os, struct, sys

if __package__:
    from .graphics_lift_inventory import Dol
    from .platform_effect_inventory import implicit_consumers
else:
    from graphics_lift_inventory import Dol
    from platform_effect_inventory import implicit_consumers

DOL = os.environ.get('SHADOW_DOL', os.path.join(os.path.dirname(__file__), '..', '..', 'game', 'sys', 'main.dol'))
_d = None
_secs = []

def _load():
    global _d, _secs
    if _d is None:
        with open(DOL, 'rb') as source:
            raw = source.read(64 * 1024 * 1024 + 1)
        if len(raw) > 64 * 1024 * 1024:
            raise ValueError('DOL exceeds supported original-image bound')
        image = Dol(raw)
        _d = raw
        _secs = [(s['address'], s['file_offset'], s['size'], s['index']) for s in image.sections]

def is_text(i):   return i < 7
def f2a(a):
    _load()
    for addr, off, size, i in _secs:
        if addr <= a < addr + size:
            return off + (a - addr)
    return None

def word(a):
    _load()
    for address, offset, size, _ in _secs:
        if address <= a and a + 4 <= address + size:
            return struct.unpack_from('>I', _d, offset + a - address)[0]
    return None

def _md():
    try:
        import capstone
    except ImportError:
        return None  # Raw words/dependencies remain visible without Capstone.
    return capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN)

def decode_word(word, address, md=None):
    """One word in, one row out. Unsupported encodings never truncate a range."""
    op, xo6 = word >> 26, (word >> 1) & 63
    d, a, b, c = (word >> 21) & 31, (word >> 16) & 31, (word >> 11) & 31, (word >> 6) & 31
    short = (word >> 1) & 31
    # Display-only names/operand fields from ppcrecomp emit_group4. No numeric
    # execution, semantic recognition, validity or ownership claim is made.
    paired_a = {10: 'ps_sum0', 11: 'ps_sum1', 12: 'ps_muls0', 13: 'ps_muls1',
                14: 'ps_madds0', 15: 'ps_madds1', 18: 'ps_div', 20: 'ps_sub',
                21: 'ps_add', 23: 'ps_sel', 24: 'ps_res', 25: 'ps_mul',
                26: 'ps_rsqrte', 28: 'ps_msub', 29: 'ps_madd', 30: 'ps_nmsub', 31: 'ps_nmadd'}
    if op in (56, 57, 60, 61):
        name = {56: 'psq_l', 57: 'psq_lu', 60: 'psq_st', 61: 'psq_stu'}[op]
        disp = word & 0xfff
        if disp & 0x800:
            disp -= 0x1000
        operands = f'f{d}, {disp}(r{a}), {(word >> 15) & 1}, {(word >> 12) & 7}'
    elif op == 4 and xo6 in (6, 7, 38, 39):
        name = {6: 'psq_lx', 7: 'psq_stx', 38: 'psq_lux', 39: 'psq_stux'}[xo6]
        operands = f'f{d}, r{a}, r{b}, {(word >> 10) & 1}, {(word >> 7) & 7}'
    elif op == 4 and short in paired_a:
        name = paired_a[short] + ('.' if word & 1 else '')
        if short in (24, 26):
            operands = f'f{d}, f{b}'
        elif short in (12, 13, 25):
            operands = f'f{d}, f{a}, f{c}'
        elif short in (18, 20, 21):
            operands = f'f{d}, f{a}, f{b}'
        elif short in (10, 11):
            operands = f'f{d}, f{a}, f{b}, f{c}'
        else:
            operands = f'f{d}, f{a}, f{c}, f{b}'
    elif op == 4:
        extended = (word >> 1) & 1023
        names = {0: 'ps_cmpu0', 32: 'ps_cmpo0', 64: 'ps_cmpu1', 96: 'ps_cmpo1',
                 40: 'ps_neg', 72: 'ps_mr', 136: 'ps_nabs', 264: 'ps_abs',
                 528: 'ps_merge00', 560: 'ps_merge01', 592: 'ps_merge10', 624: 'ps_merge11',
                 1014: 'dcbz_l'}
        name = names.get(extended, '.long')
        if extended in (0, 32, 64, 96):
            operands = f'cr{(word >> 23) & 7}, f{a}, f{b}'
        elif extended in (40, 72, 136, 264):
            operands = f'f{d}, f{b}'
        elif extended in (528, 560, 592, 624):
            operands = f'f{d}, f{a}, f{b}'
        elif extended == 1014:
            operands = f'r{a}, r{b}'
        else:
            # Generic PPC decoders interpret some Gekko primary-4 words as
            # AltiVec. Never present that unrelated ISA as original semantics.
            operands = f'0x{word:08X} ; undecoded Gekko primary-4 word'
        if name != '.long' and extended not in (0, 32, 64, 96, 1014) and word & 1:
            name += '.'
    else:
        instruction = next(md.disasm(struct.pack('>I', word), address), None) if md else None
        name, operands = (instruction.mnemonic, instruction.op_str) if instruction else ('.long', f'0x{word:08X}')
    consumers = [c for c in implicit_consumers(word) if c['consumer'] in ('psq_load', 'psq_store')]
    note = ', '.join(f"{c['consumer']}: format=GQR{c['spr'] - 912}, mask={c['required_mask']}" for c in consumers)
    return name, operands, note

def disassembly_rows(raw, start, md=None):
    if start < 0 or start % 4 or len(raw) % 4 or start + len(raw) > 1 << 32:
        raise ValueError('disassembly requires aligned words within the original 32-bit address range')
    for i, (value,) in enumerate(struct.iter_unpack('>I', raw)):
        address = start + i * 4
        yield address, value, *decode_word(value, address, md)

def print_row(row):
    address, value, name, operands, note = row
    print('%08X  %08X  %-10s %s%s' % (address, value, name, operands, (' ; ' + note) if note else ''))

def range_bytes(start, n):
    _load()
    if start < 0 or start % 4 or n < 0 or start + n * 4 > 1 << 32:
        raise ValueError('invalid aligned range')
    for address, offset, size, _ in _secs:
        if address <= start < address + size:
            if n * 4 > address + size - start:
                raise ValueError('requested range crosses an original section boundary')
            begin = offset + start - address
            return _d[begin:begin + n * 4]
    raise ValueError('%08X is not file-backed' % start)

def disasm(start, n):
    for row in disassembly_rows(range_bytes(start, n), start, _md()):
        print_row(row)

def func(start, cap=4000):
    """Disassemble to the last blr/bctr before the next function prologue-ish boundary."""
    o = f2a(start)
    if o is None:
        print('%08X  <not mapped>' % start); return
    if cap < 1 or cap > 4000:
        raise ValueError('heuristic function cap must be 1..4000')
    available = next((address + size - start) // 4 for address, _, size, _ in _secs if address <= start < address + size)
    depth_end = None
    out = []
    print('# heuristic extent only; not procedure or consumer-closure proof')
    for row in disassembly_rows(range_bytes(start, min(cap, available)), start, _md()):
        out.append(row)
        if row[1] in (0x4e800020, 0x4e800420):
            depth_end = len(out)
        # heuristic stop: a `blr` followed by a new stwu r1 prologue or padding
        if depth_end is not None and len(out) > depth_end:
            w = row[1]
            if (w >> 26) == 37 and ((w >> 16) & 31) == 1:  # stwu rX,-n(r1)
                out = out[:depth_end]; break
            if w == 0:
                out = out[:depth_end]; break
    for row in out:
        print_row(row)

def dump(start, n, kind):
    for k, (w,) in enumerate(struct.iter_unpack('>I', range_bytes(start, n))):
        a = start + k * 4
        if kind == 'f32':
            print('%08X  %08X  %.9g' % (a, w, struct.unpack('>f', struct.pack('>I', w))[0]))
        else:
            print('%08X  %08X  %d' % (a, w, w))

def xref(target):
    _load()
    n = 0
    for addr, off, size, i in _secs:
        if not is_text(i): continue
        for k in range(0, size, 4):
            w = struct.unpack_from('>I', _d, off + k)[0]
            if (w >> 26) != 18: continue
            li = w & 0x03FFFFFC
            if li & 0x02000000: li -= 0x04000000
            aa, lk = (w >> 1) & 1, w & 1
            dst = li if aa else (addr + k) + li
            if dst == target:
                print('%08X  %s  ->  %08X' % (addr + k, 'bl' if lk else 'b', dst)); n += 1
    print('# %d site(s)' % n)

def findf32(val):
    _load()
    pat = struct.pack('>f', float(val)); n = 0
    for addr, off, size, i in _secs:
        for k in range(0, size - 3, 4):
            if _d[off + k:off + k + 4] == pat:
                print('%08X  (section %d)' % (addr + k, i)); n += 1
    print('# %d hit(s) for %r' % (n, val))

def main(args=None):
    args = sys.argv[1:] if args is None else args
    if args in (['--help'], ['-h']):
        print(__doc__)
        return 0
    if not args:
        print(__doc__)
        return 1
    mode = 'dis'
    for a in args:
        if a in ('--f32', '--u32', '--xref', '--findf32', '--func'):
            mode = a[2:]; continue
        if mode == 'xref':
            xref(int(a, 16)); mode = 'dis'; continue
        if mode == 'findf32':
            findf32(a); mode = 'dis'; continue
        if mode == 'func':
            func(int(a, 16)); print('---'); mode = 'dis'; continue
        s, _, c = a.partition(':')
        start = int(s, 16); n = int(c) if c else 32
        if mode in ('f32', 'u32'): dump(start, n, mode)
        else: disasm(start, n)
        print('---')
        mode = 'dis'
    return 0

if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as error:
        print('disassembly refused: ' + str(error), file=sys.stderr)
        raise SystemExit(1)
