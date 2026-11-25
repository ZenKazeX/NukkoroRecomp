#!/usr/bin/env python
"""lift_host.py - re-express original functions as host C++ with no emulated machine.

Input: the pinned DOL, the function table (extract/functions.csv: address, size) and the verified jump tables
(extract/jump-tables.txt). Every instruction is decoded from its word. Output: one header with a class template
`Name<M>`; every listed function becomes a static member `fn_XXXXXXXX(M& m, Regs& s)`. Inside a function the
registers are C++ locals; there is no Cpu object, no FPSCR, no dispatcher. What the code needs from its
surroundings comes through the policy `M`:

    memory      m.r8/r16/r32/r64(address), m.w8/w16/w32/w64(address, value), m.zero32(address)
    float       m.lfs / m.stfs (single <-> register format), m.fadds ... m.fnmsub, m.frsp, m.fres, m.frsqrte,
                m.fctiw / m.fctiwz, m.psq_l / m.psq_st (quantised), m.ps(op, ...) (paired singles), m.fpscr_*
                (register values as IEEE double bits; the second paired-single half is kept beside the first)
    calls out   m.external(address, s)   a callee outside the closure
                m.indirect(address, s)   a call through a register (virtual calls, callbacks)

The same generated body therefore runs (a) on a guest memory image with the exact PowerPC rounding rules, to be
compared with the original instructions, and (b) on host structures with host arithmetic. Constants loaded from the
read-only small data area (r2) are folded into the source from the DOL.

Refused, not guessed: any instruction form not handled below raises with its address (privileged and
string instructions, overflow-recording arithmetic, absolute branches, branches into another function's interior).

  lift_host.py --dol <main.dol> --functions <functions.csv> [--jump-tables <file>] --name N --out <file.h> ADDR...
"""
import argparse
import csv
import re
import struct
import sys

R2 = 0x805FA780
R13 = 0x805EC500
SIGN = '0x8000000000000000ull'


class Refused(Exception):
    pass


class Dol:
    def __init__(self, path):
        self.d = open(path, 'rb').read()
        self.secs = []
        for i in range(18):
            off, adr, size = (struct.unpack_from('>I', self.d, b + i * 4)[0] for b in (0, 0x48, 0x90))
            if size:
                self.secs.append((adr, size, off))

    def word(self, address):
        for adr, size, off in self.secs:
            if adr <= address <= adr + size - 4:
                return struct.unpack_from('>I', self.d, off + address - adr)[0]
        return None


def read_table(path):
    """extract/functions.csv -> {address: size in bytes}."""
    out = {}
    with open(path, newline='') as f:
        for row in csv.DictReader(f):
            out[int(row['addr_hex'], 16)] = int(row['size'])
    return out


def read_jump_tables(path):
    out = {}
    if not path:
        return out
    for line in open(path):
        p = line.split()
        if len(p) == 3 and not line.startswith('#'):
            out[int(p[0], 16)] = (int(p[1], 16), int(p[2]))
    return out


def s16(x):
    return x - 0x10000 if x & 0x8000 else x


def hexs(d):
    """A signed displacement as a C++ addend."""
    return '+ 0x%Xu' % d if d >= 0 else '- 0x%Xu' % -d


class Lifter:
    def __init__(self, dol, sizes, jump_tables, closure):
        self.dol = dol
        self.sizes = sizes
        self.jump_tables = jump_tables
        self.closure = closure
        self.constants = 0
        self.name = 'Lifted'

    # ---- helpers ----
    def set_cr(self, field, expr):
        shift = 28 - 4 * field
        return 'cr = (cr & ~(0xFu << %d)) | ((%s) << %d);' % (shift, expr, shift)

    def rc0(self, reg):
        return ' ' + self.set_cr(0, 'icmp(static_cast<s32>(%s), 0)' % reg)

    def crget(self, bit):
        return '((cr >> %d) & 1u)' % (31 - bit)

    def readonly(self, address):
        return R2 - 0x8000 <= address < R2 + 0x8000 and self.dol.word(address) is not None

    def ea_d(self, ra, d):
        """Displacement address -> (expression, constant or None)."""
        if ra == 0:
            return '0x%08Xu' % (d & 0xFFFFFFFF), d & 0xFFFFFFFF
        if ra in (2, 13):
            a = ((R2 if ra == 2 else R13) + d) & 0xFFFFFFFF
            return '0x%08Xu' % a, a
        return ('r%d' % ra if d == 0 else 'r%d %s' % (ra, hexs(d))), None

    def ea_x(self, ra, rb):
        return 'r%d' % rb if ra == 0 else 'r%d + r%d' % (ra, rb)

    def call(self, target, link, kind='external'):
        lr = 'lr = 0x%08Xu; ' % link if link is not None else ''
        if target in self.closure:
            self.callees.add(target)
            return '%sSAVE_%08X(); fn_%08X(m, s); LOAD_%08X();' % (lr, target, target, target)
        self.calls_out = True
        return '%sSAVE(); m.%s(0x%08Xu, s); LOAD();' % (lr, kind, target)

    def tail_call(self, target):
        """A transfer that does not come back here. Everything this function holds goes back to the register block
        first: a call's own SAVE_<callee> writes only what the callee can reach, which is right when control returns
        (the rest stays in the locals) and loses the rest when it does not."""
        text = re.sub(r' LOAD(_[0-9A-F]{8})?\(\);', '', self.call(target, None))
        return re.sub(r'SAVE_[0-9A-F]{8}\(\);', 'SAVE();', text) + ' return;'

    def branch_to(self, at, target, begin, end, link):
        """An unconditional transfer as a statement."""
        if link:
            return self.call(target, at + 4)
        if begin <= target < end:
            return 'goto L_%08X;' % target
        if target not in self.sizes:
            raise Refused('branch into another function\'s interior')
        return self.tail_call(target)

    def condition(self, bo, bi):
        parts = []
        pre = ''
        if not bo & 4:
            pre = '--ctr; '
            parts.append('ctr %s 0u' % ('==' if bo & 2 else '!='))
        if not bo & 16:
            parts.append('%s%s' % ('' if bo & 8 else '!', self.crget(bi)))
        return pre, ' && '.join(parts) if parts else None

    # ---- one instruction ----
    def emit(self, at, w, begin, end):
        op = w >> 26
        rd, ra, rb = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
        rs = rd
        imm = w & 0xFFFF
        simm = s16(imm)
        rc = w & 1
        D, A, B, C = 'r%d' % rd, 'r%d' % ra, 'r%d' % rb, None
        S = D
        if op == 14:  # addi
            if ra == 0:
                return '%s = 0x%08Xu;' % (D, simm & 0xFFFFFFFF)
            if ra in (2, 13):
                return '%s = 0x%08Xu;' % (D, ((R2 if ra == 2 else R13) + simm) & 0xFFFFFFFF)
            return '%s = %s %s;' % (D, A, hexs(simm))
        if op == 15:  # addis
            if ra == 0:
                return '%s = 0x%08Xu;' % (D, (imm << 16) & 0xFFFFFFFF)
            return '%s = %s + 0x%08Xu;' % (D, A, (imm << 16) & 0xFFFFFFFF)
        if op in (12, 13):  # addic, addic.
            line = ('{ const u64 t = static_cast<u64>(%s) + 0x%08Xull; %s = static_cast<u32>(t); ca = static_cast<u32>(t >> 32); }'
                    % (A, simm & 0xFFFFFFFF, D))
            return line + (self.rc0(D) if op == 13 else '')
        if op == 8:  # subfic
            return ('{ const u64 t = static_cast<u64>(static_cast<u32>(~%s)) + 0x%08Xull + 1u; %s = static_cast<u32>(t); ca = static_cast<u32>(t >> 32); }'
                    % (A, simm & 0xFFFFFFFF, D))
        if op == 7:
            return '%s = static_cast<u32>(static_cast<s32>(%s) * %d);' % (D, A, simm)
        if op in (10, 11):
            if rd & 1:
                raise Refused('64-bit compare')
            field = rd >> 2
            if op == 11:
                return self.set_cr(field, 'icmp(static_cast<s32>(%s), %d)' % (A, simm))
            return self.set_cr(field, 'ucmp(%s, 0x%Xu)' % (A, imm))
        if op in (24, 25, 26, 27, 28, 29):
            v = imm << 16 if op in (25, 27, 29) else imm
            if op in (24, 25):
                if v == 0 and rs == ra:
                    return '/* nop */'
                return '%s = %s | 0x%08Xu;' % (A, S, v)
            if op in (26, 27):
                return '%s = %s ^ 0x%08Xu;' % (A, S, v)
            return '%s = %s & 0x%08Xu;' % (A, S, v) + self.rc0(A)
        if op in (20, 21, 23):  # rlwimi, rlwinm, rlwnm
            mb, me = (w >> 6) & 31, (w >> 1) & 31
            mask = 0
            b = mb
            while True:
                mask |= 1 << (31 - b)
                if b == me:
                    break
                b = (b + 1) & 31
            if op == 23:
                rot = 'rotl32(%s, %s & 31u)' % (S, B)
            else:
                rot = 'rotl32(%s, %du)' % (S, rb) if rb else S
            if op == 20:
                line = '%s = (%s & 0x%08Xu) | (%s & 0x%08Xu);' % (A, rot, mask, A, ~mask & 0xFFFFFFFF)
            else:
                line = '%s = %s & 0x%08Xu;' % (A, rot, mask)
            return line + (self.rc0(A) if rc else '')
        if op == 18:
            if w & 2:
                raise Refused('absolute branch')
            d = w & 0x03FFFFFC
            if d & 0x02000000:
                d -= 0x04000000
            return self.branch_to(at, (at + d) & 0xFFFFFFFF, begin, end, rc)
        if op == 16:
            if w & 2:
                raise Refused('absolute conditional branch')
            if rc:
                raise Refused('conditional branch and link')
            target = (at + s16(imm & 0xFFFC)) & 0xFFFFFFFF
            pre, cond = self.condition(rd, ra)
            go = self.branch_to(at, target, begin, end, 0)
            return pre + ('if (%s) { %s }' % (cond, go) if cond else go)
        if op == 19:
            xo = (w >> 1) & 0x3FF
            if xo == 16:  # bclr
                pre, cond = self.condition(rd, ra)
                if rc:  # blrl: a call through the link register, which then holds the return address
                    self.calls_out = True
                    body = '{ const u32 target = lr; lr = 0x%08Xu; SAVE(); m.indirect(target, s); LOAD(); }' % (at + 4)
                    return pre + ('if (%s) %s' % (cond, body) if cond else body)
                return pre + ('if (%s) { SAVE(); return; }' % cond if cond else 'SAVE(); return;')
            if xo == 528:  # bcctr
                if not rd & 4:
                    raise Refused('bcctr decrementing ctr')
                _, cond = self.condition(rd, ra)
                if rc:
                    self.calls_out = True
                    body = 'lr = 0x%08Xu; SAVE(); m.indirect(ctr, s); LOAD();' % (at + 4)
                elif at in self.jump_tables:
                    table, count = self.jump_tables[at]
                    cases = []
                    for k in range(count):
                        target = self.dol.word(table + 4 * k)
                        if not begin <= target < end:
                            raise Refused('jump table entry outside the function')
                        cases.append(target)
                    self.table_targets.update(cases)
                    body = 'switch (ctr) { %s default: m.bad_jump(0x%08Xu, ctr); SAVE(); return; }' % (
                        ' '.join('case 0x%08Xu: goto L_%08X;' % (t, t) for t in sorted(set(cases))), at)
                else:
                    self.calls_out = True
                    body = 'SAVE(); m.indirect(ctr, s); return;'
                return 'if (%s) { %s }' % (cond, body) if cond else body
            crops = {257: '&', 449: '|', 193: '^', 225: '~&', 33: '~|', 289: '~^', 129: '&~', 417: '|~'}
            if xo in crops:
                a, b = self.crget(ra), self.crget(rb)
                o = crops[xo]
                if o.startswith('~'):
                    e = '(~(%s %s %s) & 1u)' % (a, o[1], b)
                elif o.endswith('~'):
                    e = '(%s %s (~%s & 1u))' % (a, o[0], b)
                else:
                    e = '(%s %s %s)' % (a, o, b)
                return 'cr = (cr & ~(1u << %d)) | (%s << %d);' % (31 - rd, e, 31 - rd)
            if xo == 0:  # mcrf
                return self.set_cr(rd >> 2, '(cr >> %d) & 0xFu' % (28 - 4 * (ra >> 2)))
            if xo == 150:
                return '/* isync */'
            raise Refused('group 19 extended %d' % xo)
        if 32 <= op <= 47:
            ea, const = self.ea_d(ra, simm)
            update = op & 1 and op not in (46, 47)
            if update and (ra == 0):
                raise Refused('update form with r0')
            if op in (46, 47):
                parts = []
                if op == 46 and rd <= ra:
                    # lmw whose base register is among those loaded: take the address once (InitMetroTRK's
                    # lmw r0, 0(r3)); otherwise the reload of the base would move the remaining loads.
                    loads = ' '.join('r%d = m.r32(ea_ + 0x%Xu);' % (r, 4 * k) for k, r in enumerate(range(rd, 32)))
                    return '{ const u32 ea_ = %s; %s }' % (ea, loads)
                for k, r in enumerate(range(rd, 32)):
                    at_k = '%s + 0x%Xu' % (ea, 4 * k) if k else ea
                    parts.append('r%d = m.r32(%s);' % (r, at_k) if op == 46 else 'm.w32(%s, r%d);' % (at_k, r))
                return ' '.join(parts)
            kind = {32: 'r32', 34: 'r8', 40: 'r16', 42: 'r16s', 36: 'w32', 38: 'w8', 44: 'w16'}[op & ~1]
            if update:
                if kind.startswith('w'):
                    if ra == rs:
                        # the stored value is the register before the update (stwu r1: the back chain)
                        return '{ const u32 v = %s; %s = %s; m.%s(%s, v); }' % (S, A, ea, kind, A)
                    return '%s = %s; m.%s(%s, %s);' % (A, ea, kind, A, S)
                if ra == rd:
                    raise Refused('load with update into its own base')
                load = 'm.r16(%s)' % A if kind != 'r16s' else 'static_cast<u32>(static_cast<s32>(static_cast<std::int16_t>(m.r16(%s))))' % A
                if kind in ('r32', 'r8'):
                    load = 'm.%s(%s)' % (kind, A)
                return '%s = %s; %s = %s;' % (A, ea, D, load)
            if kind.startswith('w'):
                return 'm.%s(%s, %s);' % (kind, ea, S)
            if kind == 'r16s':
                return '%s = static_cast<u32>(static_cast<s32>(static_cast<std::int16_t>(m.r16(%s))));' % (D, ea)
            return '%s = m.%s(%s);' % (D, kind, ea)
        if 48 <= op <= 55:
            ea, const = self.ea_d(ra, simm)
            F, P = 'f%d' % rd, 'p%d' % rd
            update = op & 1
            pre = '%s = %s; ' % (A, ea) if update else ''
            if update:
                if ra == 0:
                    raise Refused('update form with r0')
                ea = A
            base = op & ~1
            if base == 48:
                if const is not None and self.readonly(const) and not update:
                    word = self.dol.word(const)
                    value = struct.unpack('>f', struct.pack('>I', word))[0]
                    if value == value and abs(value) != float('inf'):
                        bits = struct.unpack('>Q', struct.pack('>d', value))[0]
                        self.constants += 1
                        return '%s = %s = 0x%016Xull;  /* %r */' % (F, P, bits, value)
                return '%s%s = %s = m.lfs(%s);' % (pre, F, P, ea)
            if base == 50:
                if const is not None and self.readonly(const) and not update:
                    bits = (self.dol.word(const) << 32) | self.dol.word(const + 4)
                    self.constants += 1
                    return '%s = 0x%016Xull;' % (F, bits)
                return '%s%s = m.r64(%s);' % (pre, F, ea)
            if base == 52:
                return '%sm.stfs(%s, %s);' % (pre, ea, F)
            return '%sm.w64(%s, %s);' % (pre, ea, F)
        if op in (56, 57, 60, 61):  # psq_l, psq_lu, psq_st, psq_stu
            d = w & 0xFFF
            d = d - 0x1000 if d & 0x800 else d
            wbit, gqr = (w >> 15) & 1, (w >> 12) & 7
            if ra == 0:
                raise Refused('quantised access with r0')
            ea = 'r%d' % ra if d == 0 else 'r%d %s' % (ra, hexs(d))
            update = op & 1
            fn = 'psq_l' if op < 60 else 'psq_st'
            line = 'm.%s(%s, %du, %du, f%d, p%d);' % (fn, ea, wbit, gqr, rd, rd)
            if update:
                line = '%s = %s; m.%s(%s, %du, %du, f%d, p%d);' % (A, ea, fn, A, wbit, gqr, rd, rd)
            return line
        if op == 59:
            xo = (w >> 1) & 31
            if rc:
                raise Refused('float record form')
            fa, fb, fc, fd = 'f%d' % ra, 'f%d' % rb, 'f%d' % ((w >> 6) & 31), 'f%d' % rd
            pd = 'p%d' % rd
            two = {18: ('fdivs', fa, fb), 20: ('fsubs', fa, fb), 21: ('fadds', fa, fb), 25: ('fmuls', fa, fc)}
            three = {28: 'fmsubs', 29: 'fmadds', 30: 'fnmsubs', 31: 'fnmadds'}
            if xo in two:
                n, x, y = two[xo]
                return '%s = %s = m.%s(%s, %s);' % (fd, pd, n, x, y)
            if xo in three:
                return '%s = %s = m.%s(%s, %s, %s);' % (fd, pd, three[xo], fa, fc, fb)
            if xo == 24:
                return '%s = %s = m.fres(%s);' % (fd, pd, fb)
            raise Refused('group 59 extended %d' % xo)
        if op == 63:
            xo10 = (w >> 1) & 0x3FF
            xo5 = (w >> 1) & 31
            fa, fb, fc, fd = 'f%d' % ra, 'f%d' % rb, 'f%d' % ((w >> 6) & 31), 'f%d' % rd
            if xo10 in (0, 32):
                return self.set_cr(rd >> 2, 'm.fcmp(%s, %s, %s)' % (fa, fb, 'true' if xo10 == 32 else 'false'))
            if rc:
                raise Refused('float record form')
            one = {12: 'm.frsp(%s)', 14: 'm.fctiw(%s)', 15: 'm.fctiwz(%s)', 26: 'm.frsqrte(%s)', 40: '%s ^ ' + SIGN, 72: '%s',
                   136: '%s | ' + SIGN, 264: '%s & ~' + SIGN}
            if xo10 in one:
                if xo10 == 12:
                    return '%s = p%d = m.frsp(%s);' % (fd, rd, fb)
                return '%s = %s;' % (fd, one[xo10] % fb)
            two = {18: ('fdiv', fa, fb), 20: ('fsub', fa, fb), 21: ('fadd', fa, fb), 25: ('fmul', fa, fc)}
            three = {28: 'fmsub', 29: 'fmadd', 30: 'fnmsub', 31: 'fnmadd'}
            if xo5 in two:  # A-form: the frC field is part of xo10 (fmul)
                n, x, y = two[xo5]
                return '%s = m.%s(%s, %s);' % (fd, n, x, y)
            if xo5 in three:
                return '%s = m.%s(%s, %s, %s);' % (fd, three[xo5], fa, fc, fb)
            if xo5 == 23:
                return '%s = fsel(%s, %s, %s);' % (fd, fa, fc, fb)
            if xo10 == 583:
                return '%s = m.fpscr_read();' % fd
            if xo10 == 711:
                return 'm.fpscr_write(0x%02Xu, %s);' % ((w >> 17) & 0xFF, fb)
            if xo10 in (38, 70):
                return 'm.fpscr_bit(%du, %du);' % (rd, 1 if xo10 == 38 else 0)
            if xo10 == 134:
                return 'm.fpscr_field(%du, %du);' % (rd >> 2, (w >> 12) & 15)
            raise Refused('group 63 extended %d' % xo10)
        if op == 4:
            xo10 = (w >> 1) & 0x3FF
            xo5 = (w >> 1) & 31
            if rc:
                raise Refused('paired-single record form')
            if xo5 in (6, 7, 38, 39) and ((w >> 1) & 0x3F) in (6, 7, 38, 39):  # psq_lx, psq_stx, psq_lux, psq_stux
                kind = (w >> 1) & 0x3F
                wbit, gqr = (w >> 10) & 1, (w >> 7) & 7
                fn = 'psq_l' if kind in (6, 38) else 'psq_st'
                if kind in (38, 39):
                    if ra == 0:
                        raise Refused('update form with r0')
                    return '%s = %s + %s; m.%s(%s, %du, %du, f%d, p%d);' % (A, A, B, fn, A, wbit, gqr, rd, rd)
                return 'm.%s(%s, %du, %du, f%d, p%d);' % (fn, self.ea_x(ra, rb), wbit, gqr, rd, rd)
            if xo10 == 1014:
                return 'm.zero32((%s) & ~31u);  /* dcbz_l */' % self.ea_x(ra, rb)
            if xo10 in (0, 32, 64, 96):
                # ps_cmpu0, ps_cmpo0, ps_cmpu1, ps_cmpo1
                half = 'f' if xo10 < 64 else 'p'
                return self.set_cr(rd >> 2, 'm.fcmp(%s%d, %s%d, %s)' % (half, ra, half, rb, 'true' if xo10 in (32, 96) else 'false'))
            rc_ = (w >> 6) & 31
            args = 'f%d, p%d, f%d, p%d, f%d, p%d, f%d, p%d' % (rd, rd, ra, ra, rb, rb, rc_, rc_)
            key = xo10 if xo10 in (40, 72, 136, 264, 528, 560, 592, 624) else xo5
            names = {18: 'div', 20: 'sub', 21: 'add', 25: 'mul', 24: 'res', 26: 'rsqrte', 23: 'sel', 10: 'sum0', 11: 'sum1',
                     12: 'muls0', 13: 'muls1', 14: 'madds0', 15: 'madds1', 28: 'msub', 29: 'madd', 30: 'nmsub', 31: 'nmadd',
                     40: 'neg', 72: 'mr', 136: 'nabs', 264: 'abs', 528: 'merge00', 560: 'merge01', 592: 'merge10', 624: 'merge11'}
            if key not in names:
                raise Refused('paired single %d / %d' % (xo10, xo5))
            return 'm.ps_%s(%s);' % (names[key], args)
        if op == 31:
            xo = (w >> 1) & 0x3FF
            arith = xo & 0x1FF
            oe = (w >> 10) & 1
            carry = lambda expr: ('{ const u64 t = %s; %s = static_cast<u32>(t); ca = static_cast<u32>(t >> 32); }' % (expr, D)) + (
                self.rc0(D) if rc else '')
            plain = lambda expr: '%s = %s;' % (D, expr) + (self.rc0(D) if rc else '')
            u = lambda r: 'static_cast<u64>(%s)' % r
            table = {
                266: lambda: plain('%s + %s' % (A, B)),
                40: lambda: plain('%s - %s' % (B, A)),
                104: lambda: plain('0u - %s' % A),
                235: lambda: plain('static_cast<u32>(static_cast<s32>(%s) * static_cast<s32>(%s))' % (A, B)),
                75: lambda: plain('static_cast<u32>((static_cast<std::int64_t>(static_cast<s32>(%s)) * static_cast<s32>(%s)) >> 32)' % (A, B)),
                11: lambda: plain('static_cast<u32>((%s * %s) >> 32)' % (u(A), u(B))),
                491: lambda: plain('divw(%s, %s)' % (A, B)),
                459: lambda: plain('divwu(%s, %s)' % (A, B)),
                10: lambda: carry('%s + %s' % (u(A), u(B))),
                8: lambda: carry('static_cast<u64>(static_cast<u32>(~%s)) + %s + 1u' % (A, u(B))),
                138: lambda: carry('%s + %s + ca' % (u(A), u(B))),
                136: lambda: carry('static_cast<u64>(static_cast<u32>(~%s)) + %s + ca' % (A, u(B))),
                202: lambda: carry('%s + ca' % u(A)),
                200: lambda: carry('static_cast<u64>(static_cast<u32>(~%s)) + ca' % A),
                234: lambda: carry('%s + 0xFFFFFFFFull + ca' % u(A)),
                232: lambda: carry('static_cast<u64>(static_cast<u32>(~%s)) + 0xFFFFFFFFull + ca' % A),
            }
            if arith in table and xo not in (523, 520, 650, 648, 714, 712, 746, 744, 747, 778, 552, 616, 1003, 971):
                if oe:
                    raise Refused('overflow-recording arithmetic')
                if (xo & 0x200) and arith in table and xo != arith:
                    raise Refused('overflow-recording arithmetic')
                return table[arith]()
            logic = {28: '%s & %s', 444: '%s | %s', 316: '%s ^ %s', 476: '~(%s & %s)', 124: '~(%s | %s)', 284: '~(%s ^ %s)',
                     60: '%s & ~%s', 412: '%s | ~%s'}
            if xo in logic:
                return '%s = %s;' % (A, logic[xo] % (S, B)) + (self.rc0(A) if rc else '')
            if xo == 24:
                return '%s = (%s & 0x20u) ? 0u : %s << (%s & 31u);' % (A, B, S, B) + (self.rc0(A) if rc else '')
            if xo == 536:
                return '%s = (%s & 0x20u) ? 0u : %s >> (%s & 31u);' % (A, B, S, B) + (self.rc0(A) if rc else '')
            if xo == 792:
                return '{ const u32 n = %s & 0x3Fu; sraw(%s, n, %s, ca); }' % (B, S, A) + (self.rc0(A) if rc else '')
            if xo == 824:
                return 'sraw(%s, %du, %s, ca);' % (S, rb, A) + (self.rc0(A) if rc else '')
            if xo == 26:
                return '%s = clz32(%s);' % (A, S) + (self.rc0(A) if rc else '')
            if xo == 922:
                return '%s = static_cast<u32>(static_cast<s32>(static_cast<std::int16_t>(%s)));' % (A, S) + (self.rc0(A) if rc else '')
            if xo == 954:
                return '%s = static_cast<u32>(static_cast<s32>(static_cast<std::int8_t>(%s)));' % (A, S) + (self.rc0(A) if rc else '')
            if xo in (0, 32):
                if rd & 1:
                    raise Refused('64-bit compare')
                if xo == 0:
                    return self.set_cr(rd >> 2, 'icmp(static_cast<s32>(%s), static_cast<s32>(%s))' % (A, B))
                return self.set_cr(rd >> 2, 'ucmp(%s, %s)' % (A, B))
            if xo == 19:
                return '%s = cr;' % D
            if xo == 144:
                mask = 0
                for k in range(8):
                    if (w >> (12 + k)) & 1:
                        mask |= 0xF << (4 * k)
                return 'cr = (cr & 0x%08Xu) | (%s & 0x%08Xu);' % (~mask & 0xFFFFFFFF, S, mask)
            if xo in (339, 467):
                spr = ((w >> 16) & 31) | ((w >> 6) & 0x3E0)
                names = {8: 'lr', 9: 'ctr'}
                if spr in names:
                    return '%s = %s;' % ((D, names[spr]) if xo == 339 else (names[spr], S))
                if spr == 1:
                    return '%s = ca << 29;' % D if xo == 339 else 'ca = (%s >> 29) & 1u;' % S
                if 912 <= spr <= 919:
                    return '%s = m.gqr_read(%du);' % (D, spr - 912) if xo == 339 else 'm.gqr_write(%du, %s);' % (spr - 912, S)
                raise Refused('special register %d' % spr)
            loads = {23: ('r32', 0), 55: ('r32', 1), 87: ('r8', 0), 119: ('r8', 1), 279: ('r16', 0), 311: ('r16', 1), 343: ('r16s', 0),
                     375: ('r16s', 1), 534: ('r32x', 0), 790: ('r16x', 0)}
            stores = {151: ('w32', 0), 183: ('w32', 1), 215: ('w8', 0), 247: ('w8', 1), 407: ('w16', 0), 439: ('w16', 1),
                      662: ('w32x', 0), 918: ('w16x', 0)}
            if xo in loads or xo in stores:
                kind, update = loads.get(xo) or stores.get(xo)
                ea = self.ea_x(ra, rb)
                pre = ''
                if update:
                    if ra == 0 or (xo in loads and ra == rd):
                        raise Refused('indexed update form')
                    pre = '%s = %s; ' % (A, ea)
                    ea = A
                if xo in stores:
                    value = S
                    if kind == 'w32x':
                        return '%sm.w32(%s, bswap32(%s));' % (pre, ea, value)
                    if kind == 'w16x':
                        return '%sm.w16(%s, bswap16(%s));' % (pre, ea, value)
                    if update and ra == rs:
                        return '{ const u32 v = %s; %s m.%s(%s, v); }' % (S, pre, kind, ea)
                    return '%sm.%s(%s, %s);' % (pre, kind, ea, value)
                expr = {'r32': 'm.r32(%s)', 'r8': 'm.r8(%s)', 'r16': 'm.r16(%s)',
                        'r16s': 'static_cast<u32>(static_cast<s32>(static_cast<std::int16_t>(m.r16(%s))))',
                        'r32x': 'bswap32(m.r32(%s))', 'r16x': 'bswap16(m.r16(%s))'}[kind] % ea
                return '%s%s = %s;' % (pre, D, expr)
            floats = {535: ('lfs', 0), 567: ('lfs', 1), 599: ('lfd', 0), 631: ('lfd', 1), 663: ('stfs', 0), 695: ('stfs', 1),
                      727: ('stfd', 0), 759: ('stfd', 1), 983: ('stfiwx', 0)}
            if xo in floats:
                kind, update = floats[xo]
                ea = self.ea_x(ra, rb)
                pre = ''
                if update:
                    if ra == 0:
                        raise Refused('indexed update form')
                    pre = '%s = %s; ' % (A, ea)
                    ea = A
                F, P = 'f%d' % rd, 'p%d' % rd
                return pre + {'lfs': '%s = %s = m.lfs(%s);' % (F, P, ea), 'lfd': '%s = m.r64(%s);' % (F, ea),
                              'stfs': 'm.stfs(%s, %s);' % (ea, F), 'stfd': 'm.w64(%s, %s);' % (ea, F),
                              'stfiwx': 'm.w32(%s, static_cast<u32>(%s));' % (ea, F)}[kind]
            if xo in (54, 86, 246, 278, 598, 854, 982, 470):
                return '/* cache / sync hint */'
            if xo == 1014:
                return 'm.zero32((%s) & ~31u);  /* dcbz */' % self.ea_x(ra, rb)
            raise Refused('group 31 extended %d' % xo)
        raise Refused('primary opcode %d' % op)

    # ---- one function ----
    def function(self, address):
        size = self.sizes[address]
        begin, end = address, address + size
        self.table_targets = set()
        self.callees = set()
        self.calls_out = False
        statements = []
        targets = set()
        last = None
        for at in range(begin, end, 4):
            w = self.dol.word(at)
            if w is None:
                raise Refused('no code at 0x%08X' % at)
            try:
                statements.append((at, w, self.emit(at, w, begin, end)))
            except Refused as error:
                raise Refused('0x%08X (word %08X): %s' % (at, w, error))
            op = w >> 26
            if op == 18 and not w & 3:
                d = w & 0x03FFFFFC
                if d & 0x02000000:
                    d -= 0x04000000
                targets.add((at + d) & 0xFFFFFFFF)
            elif op == 16:
                targets.add((at + s16(w & 0xFFFC)) & 0xFFFFFFFF)
            last = w
        targets |= self.table_targets
        # the last instruction must not fall out of the function
        op = last >> 26
        ends = (op == 18 and not last & 1) or (op == 19 and ((last >> 1) & 0x3FF) in (16, 528) and (last >> 21) & 20 == 20 and not last & 1)
        if not ends:
            # execution runs on into the next row of the table: a tail call of it
            if end not in self.sizes:
                raise Refused('0x%08X: the function runs past its end into no function' % (end - 4))
            statements.append((end, 0, self.tail_call(end) + '  /* falls through into the next function */'))
        touched = set()
        for at, w, text in statements:
            code = text.split('//')[0]
            touched.update(re.findall(r'\b[rfp](?:[12]?\d|3[01])\b', code))
            touched.update(x for x in ('cr', 'lr', 'ctr', 'ca') if re.search(r'\b%s\b' % x, code))
        return {'address': address, 'statements': statements, 'targets': targets, 'touched': touched,
                'callees': set(self.callees), 'calls_out': self.calls_out, 'insns': size // 4}

    def render(self, fn, reach):
        """One lifted function as text. reach(callee) = the registers a call of it may read or write (None: any)."""
        def order(names):
            key = lambda x: ('rfp'.find(x[0]) if x[0] in 'rfp' and x[1:].isdigit() else 3, int(x[1:]) if x[1:].isdigit() else 0, x)
            return sorted(names, key=key)
        slot = lambda x: 's.%s[%s]' % (x[0], x[1:]) if x[0] in 'rfp' and x[1:].isdigit() else 's.%s' % x
        held = order(fn['touched'])
        address = fn['address']
        lines = ['template <class M> void %s<M>::fn_%08X(M& m, Regs& s) {' % (self.name, address)]
        ints = [x for x in held if x[0] == 'r' and x[1:].isdigit()] + [x for x in held if x in ('cr', 'lr', 'ctr', 'ca')]
        floats = [x for x in held if x[0] in 'fp' and x[1:].isdigit()]
        if ints:
            lines.append('    u32 ' + ', '.join('%s = %s' % (x, slot(x)) for x in ints) + ';')
        if floats:
            lines.append('    u64 ' + ', '.join('%s = %s' % (x, slot(x)) for x in floats) + ';')
        def pair(suffix, names):
            names = order(names)
            lines.append('    const auto SAVE%s = [&]() { %s };' % (suffix, ' '.join('%s = %s;' % (slot(x), x) for x in names)))
            lines.append('    const auto LOAD%s = [&]() { %s };' % (suffix, ' '.join('%s = %s;' % (x, slot(x)) for x in names)))
            lines.append('    (void)SAVE%s; (void)LOAD%s;' % (suffix, suffix))
        pair('', held)
        for callee in sorted(fn['callees']):
            r = reach(callee)
            pair('_%08X' % callee, held if r is None else [x for x in held if x in r])
        for at, w, text in fn['statements']:
            if at in fn['targets']:
                lines.append('L_%08X:;' % at)
            lines.append('    %s  // %08X' % (text, at))
        lines.append('}')
        return lines


HEADER = '''#pragma once
#include <cstdint>
#include <cstring>

namespace shadow::lifted {

#ifndef SHADOW_LIFTED_REGS
#define SHADOW_LIFTED_REGS
using u32 = std::uint32_t; using s32 = std::int32_t; using u64 = std::uint64_t;
// The values a call passes: f = a float register's first half as IEEE double bits, p = its second paired-single half.
struct Regs { u32 r[32]; u64 f[32]; u64 p[32]; u32 cr, lr, ctr, ca; };
inline u32 icmp(s32 a, s32 b) noexcept { return a < b ? 8u : a > b ? 4u : 2u; }
inline u32 ucmp(u32 a, u32 b) noexcept { return a < b ? 8u : a > b ? 4u : 2u; }
inline u32 fcmp(u64 a, u64 b) noexcept {
    double x, y; std::memcpy(&x, &a, 8); std::memcpy(&y, &b, 8);
    return x < y ? 8u : x > y ? 4u : x == y ? 2u : 1u;
}
inline u64 fsel(u64 a, u64 c, u64 b) noexcept { double x; std::memcpy(&x, &a, 8); return x >= -0.0 ? c : b; }
inline u32 clz32(u32 v) noexcept { u32 n = 0; while (n < 32u && (v & (0x80000000u >> n)) == 0u) ++n; return n; }
inline u32 rotl32(u32 v, u32 n) noexcept { return n == 0u ? v : (v << n) | (v >> (32u - n)); }
inline u32 bswap32(u32 v) noexcept { return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24); }
inline u32 bswap16(u32 v) noexcept { return ((v >> 8) & 0xFFu) | ((v << 8) & 0xFF00u); }
// divw / divwu: the quotient the hardware leaves for the undefined cases (as the translated code's helpers).
inline u32 divw(u32 a, u32 b) noexcept {
    const s32 x = static_cast<s32>(a), y = static_cast<s32>(b);
    if (y == 0 || (a == 0x80000000u && y == -1)) return x < 0 ? 0xFFFFFFFFu : 0u;
    return static_cast<u32>(x / y);
}
inline u32 divwu(u32 a, u32 b) noexcept { return b == 0u ? 0u : a / b; }
inline void sraw(u32 value, u32 n, u32& out, u32& ca) noexcept {
    const s32 v = static_cast<s32>(value);
    if (n >= 32u) { out = v < 0 ? 0xFFFFFFFFu : 0u; ca = v < 0 ? 1u : 0u; return; }
    out = static_cast<u32>(v >> n);
    ca = (v < 0 && n != 0u && (value & ((1u << n) - 1u)) != 0u) ? 1u : 0u;
}
#endif
'''


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--dol', required=True)
    p.add_argument('--functions', required=True)
    p.add_argument('--jump-tables')
    p.add_argument('--name', required=True)
    p.add_argument('--out', required=True)
    p.add_argument('--entry', action='append', default=[],
                   help='a function entered from a register file: emit enter_ / leave_ copying exactly its reach')
    p.add_argument('addresses', nargs='+')
    a = p.parse_args()
    wanted = []
    for x in a.addresses:
        if x.startswith('@'):
            wanted.extend(int(line.split()[0], 16) for line in open(x[1:]) if line.strip())
        else:
            wanted.append(int(x, 16))
    sizes = read_table(a.functions)
    missing = [x for x in wanted if x not in sizes]
    if missing:
        raise SystemExit('lift_host: not in the function table: ' + ' '.join('%08X' % x for x in missing))
    lifter = Lifter(Dol(a.dol), sizes, read_jump_tables(a.jump_tables), set(wanted))
    lifter.name = a.name
    out = ['// GENERATED by tools/native/lift_host.py - do not edit. %d functions; the first: %s' % (
               len(wanted), ' '.join('%08X' % x for x in wanted[:40])),
           '// The original functions of the pinned DOL re-expressed as host C++: registers are locals; memory, float',
           '// rounding and calls out of the closure go through the policy M (see the generator\'s header).',
           HEADER, 'template <class M> struct %s {' % a.name]
    for x in wanted:
        out.append('    static void fn_%08X(M& m, Regs& s);' % x)
    out.append('@ENTRIES@')
    out.append('};')
    out.append('')
    count = 0
    lifted = {}
    for x in wanted:
        try:
            lifted[x] = lifter.function(x)
        except Refused as error:
            raise SystemExit('lift_host: fn_%08X refused: %s' % (x, error))
        count += lifted[x]['insns']
    # What a call of a function may read or write: its own registers and its callees', to a fixed point; None when
    # the call can leave the closure (an outside callee sees and may change every register).
    reach = {x: (None if fn['calls_out'] else set(fn['touched'])) for x, fn in lifted.items()}
    changed = True
    while changed:
        changed = False
        for x, fn in lifted.items():
            if reach[x] is None:
                continue
            for callee in fn['callees']:
                if reach[callee] is None:
                    reach[x] = None
                    changed = True
                    break
                if not reach[callee] <= reach[x]:
                    reach[x] |= reach[callee]
                    changed = True
    for x in wanted:
        out.extend(lifter.render(lifted[x], lambda callee: reach[callee]))
        out.append('')
    entries = []
    for text in a.entry:
        x = int(text, 16)
        if reach[x] is None:
            raise SystemExit('lift_host: --entry %08X can leave the closure: its reach is every register' % x)
        def side(name, into):
            if name[0] in 'rfp' and name[1:].isdigit():
                i = int(name[1:])
                host = {'r': 'c.gpr[%d]' % i, 'f': 'c.fpr[%d].bits[0]' % i, 'p': 'c.fpr[%d].bits[1]' % i}[name[0]]
                mine = 's.%s[%d]' % (name[0], i)
            elif name == 'ca':
                return 's.ca = (c.xer >> 29) & 1u;' if into else 'c.xer = (c.xer & ~0x20000000u) | (s.ca << 29);'
            else:
                host, mine = 'c.' + name, 's.' + name
            return '%s = %s;' % ((mine, host) if into else (host, mine))
        names = sorted(reach[x])
        entries.append('    // fn_%08X entered from a register file: exactly the %d registers a call of it can read or write.' % (x, len(names)))
        entries.append('    template <class C> static void enter_%08X(const C& c, Regs& s) { %s }' % (x, ' '.join(side(n, True) for n in names)))
        entries.append('    template <class C> static void leave_%08X(C& c, const Regs& s) { %s }' % (x, ' '.join(side(n, False) for n in names)))
    out[out.index('@ENTRIES@')] = '\n'.join(entries)
    with open(a.out + '.reach', 'w', newline='\n') as f:
        for x in wanted:
            f.write('%08X %s\n' % (x, 'any' if reach[x] is None else ' '.join(sorted(reach[x]))))
    out.append('}  // namespace shadow::lifted')
    with open(a.out, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(out) + '\n')
    print('lift_host: %d functions, %d instructions, %d read-only constants folded -> %s' % (len(wanted), count, lifter.constants, a.out))
    return 0


if __name__ == '__main__':
    sys.exit(main())
