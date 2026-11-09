"""Exact-arithmetic oracle for the Gekko floating-point rules (audit batch b7).

Prints the C++ vector table embedded in runtime/tests/ppc_recomp_tests.cpp
(`kFpVectors`). Written from gekko_user_manual.txt, independently of both C++
engines: values are Python Fractions, rounding is done here bit by bit.

Rules (see the comment above ppc::fp_arith_slow in runtime/include/gekko/ppc_ops.h):
  1. NaN operand: first of frA, frB, frC, quiet bit set, sign/payload kept;
     single results keep frX[0-34] (payload truncated), not negated.
  2. FPSCR[NI]: denormalised operands -> zero of their sign (single ops:
     0 < |x| < 2^-126; double ops: subnormal).
  3. Invalid operation -> 0x7FF8000000000000.
  4. Fused forms: one rounding of the exact a*c +- b (to single for single
     forms). Other forms: IEEE double result; single forms round it to single.
  5. NI: result zero (sign kept) when the value before the final rounding is
     below 2^-126 / 2^-1022; that value is exact for fused forms and double
     mul/div, the double result otherwise.
  6. fnmadd/fnmsub negate the rounded result.

    python -B tools/ppcrecomp/fp_semantics_oracle.py > vectors.txt
"""
from fractions import Fraction
import math
import random
import struct
import sys

SIGN = 1 << 63
QUIET = 1 << 51
DEFAULT_QNAN = 0x7FF8000000000000


def d2b(x):
    return struct.unpack('<Q', struct.pack('<d', x))[0]


def b2d(b):
    return struct.unpack('<d', struct.pack('<Q', b))[0]


def is_nan(b):
    return ((b >> 52) & 0x7FF) == 0x7FF and (b & ((1 << 52) - 1)) != 0


def is_inf(b):
    return (b & ~SIGN) == 0x7FF0000000000000


def exact(b):
    """Fraction of a finite double."""
    e = (b >> 52) & 0x7FF
    f = b & ((1 << 52) - 1)
    if e == 0:
        v = Fraction(f, 1 << 1074)
    else:
        v = Fraction(f | (1 << 52)) * (Fraction(2) ** (e - 1075))
    return -v if b & SIGN else v


def round_binary(v, precision, emin, emax_bits, neg_zero=False):
    """Round Fraction v to an IEEE binary format: returns (sign, magnitude Fraction or 'inf').
    precision: mantissa bits (53/24); emin: smallest normal exponent (-1022/-126)."""
    sign = v < 0 or (v == 0 and neg_zero)
    m = abs(v)
    if m == 0:
        return sign, Fraction(0)
    e = m.numerator.bit_length() - m.denominator.bit_length()
    if Fraction(2) ** e > m:
        e -= 1
    # quantum
    q_exp = max(e, emin) - (precision - 1)
    q = Fraction(2) ** q_exp
    n = m / q
    lo = n.numerator // n.denominator
    rem = n - lo
    if rem > Fraction(1, 2) or (rem == Fraction(1, 2) and lo % 2 == 1):
        lo += 1
    r = lo * q
    if r >= Fraction(2) ** (emax_bits):
        return sign, 'inf'
    return sign, r


def to_double_bits(sign, mag):
    if mag == 'inf':
        return (SIGN if sign else 0) | 0x7FF0000000000000
    x = float(mag)  # exact: mag is representable
    assert Fraction(x) == mag
    return (SIGN if sign else 0) | d2b(x)


def round_double(v, neg_zero=False):
    return to_double_bits(*round_binary(v, 53, -1022, 1024, neg_zero))


def round_single(v, neg_zero=False):
    return to_double_bits(*round_binary(v, 24, -126, 128, neg_zero))


def single_of_double_bits(b):
    """Host-style round of a double to single (ties even), sign kept for zero/inf."""
    if is_inf(b):
        return b
    return round_single(exact(b), bool(b & SIGN))


OPS = ['add', 'sub', 'mul', 'div', 'sqrt', 'res', 'rsqrte', 'round', 'madd', 'msub', 'nmadd', 'nmsub']


def oracle(op, single, ni, A, B, C):
    fused = op in ('madd', 'msub', 'nmadd', 'nmsub')
    uses = {'A': op in ('add', 'sub', 'mul', 'div') or fused,
            'B': op != 'mul',
            'C': op == 'mul' or fused}
    for name, bits in (('A', A), ('B', B), ('C', C)):
        if uses[name] and is_nan(bits):
            q = bits | QUIET
            if single:
                # keep sign, exponent and frX[12-34]; drop the low 29 payload bits
                q &= ~((1 << 29) - 1)
            return q
    if ni:
        def zero_tiny(bits):
            if is_nan(bits) or is_inf(bits):
                return bits
            v = exact(bits)
            if v != 0 and (abs(v) < Fraction(2) ** -126 if single else ((bits >> 52) & 0x7FF) == 0):
                return bits & SIGN
            return bits
        A, B, C = zero_tiny(A), zero_tiny(B), zero_tiny(C)
    a, b, c = b2d(A), b2d(B), b2d(C)
    negate = op in ('nmadd', 'nmsub')
    exact_v = None  # the exact value before the final rounding, where it is defined
    if op == 'add':
        r = a + b
    elif op == 'sub':
        r = a - b
    elif op == 'mul':
        r = a * c
    elif op == 'div':
        if b == 0:
            r = math.nan if (a == 0) else math.copysign(math.inf, a) * math.copysign(1.0, b)
        else:
            r = a / b
    elif op == 'sqrt':
        r = math.sqrt(b) if b >= 0 else math.nan
    elif op == 'res':
        r = 1.0 / b if b != 0 else math.copysign(math.inf, b)
    elif op == 'rsqrte':
        s = math.sqrt(b) if b >= 0 else math.nan
        r = math.nan if math.isnan(s) else (1.0 / s if s != 0 else math.copysign(math.inf, s))
    elif op == 'round':
        r = b
    else:
        addend = -b if op in ('msub', 'nmsub') else b
        if math.isinf(a) or math.isinf(c) or math.isinf(addend):
            r = (a * c) + addend
        else:
            exact_v = exact(d2b(a)) * exact(d2b(c)) + exact(d2b(addend))
            if exact_v == 0:
                # IEEE: an exact zero sum is +0 unless both terms are -0
                product_negative = (math.copysign(1.0, a) * math.copysign(1.0, c)) < 0
                r = -0.0 if (product_negative and math.copysign(1.0, addend) < 0) else 0.0
            else:
                r = b2d(round_double(exact_v))
    if math.isnan(r):
        return DEFAULT_QNAN
    if not single and not math.isinf(r):
        if op == 'mul':
            exact_v = exact(d2b(a)) * exact(d2b(c))
        elif op == 'div' and not math.isinf(b):
            exact_v = exact(d2b(a)) / exact(d2b(b))
    rbits = d2b(r)
    if ni and not math.isinf(r):
        v = exact_v if exact_v is not None else exact(rbits)
        if abs(v) < Fraction(2) ** (-126 if single else -1022):
            out = rbits & SIGN if v == 0 else (SIGN if v < 0 else 0)
            return out ^ (SIGN if negate else 0)
    if single:
        if exact_v is not None and exact_v != 0:
            out = round_single(exact_v)
        else:
            out = single_of_double_bits(rbits)
    else:
        out = rbits
    return out ^ (SIGN if negate else 0)


def f32(word):
    """double bits of a single-precision word (value-exact; no NaNs here)."""
    return d2b(struct.unpack('<f', struct.pack('<I', word))[0])


def sparse(rng, lo, hi):
    e = rng.randint(lo, hi)
    m = 1 << 52
    for _ in range(rng.randint(0, 3)):
        m |= 1 << rng.randint(0, 51)
    return (SIGN if rng.random() < 0.5 else 0) | ((e + 1023) << 52) | (m & ((1 << 52) - 1))


def sparse_single(rng, lo, hi):
    e = rng.randint(lo, hi)
    m = 1 << 23
    for _ in range(rng.randint(0, 3)):
        m |= 1 << rng.randint(0, 22)
    return f32((0x80000000 if rng.random() < 0.5 else 0) | ((e + 127) << 23) | (m & 0x7FFFFF))


CLASSES = [
    0x0000000000000000, 0x8000000000000000,  # +-0
    0x0000000000000001, 0x800FFFFFFFFFFFFF,  # double subnormals
    0x0010000000000000, 0x0020000000000000,  # 2^-1022, 2^-1021
    0x36A0000000000000, f32(0x800116C2),     # 2^-149, -1e-40f
    0x3810000000000000, 0x3820000000000000,  # 2^-126, 2^-125
    d2b(2.0 ** -100), d2b(2.0 ** -70), d2b(1.0), d2b(-1.5),
    0x3FF0000002000000, d2b(0.1), f32(0x397FFFC0), f32(0x3F800001),
    0x47EFFFFFE0000000, d2b(2.0 ** 200), d2b(-2.0 ** 1023), 0x7FF0000000000000, 0xFFF0000000000000,
    0x7FF8000000000123, 0xFFF8000000000000, 0x7FF4000000000000, 0xFFF0000000000001,
]


def main():
    rng = random.Random(0xB7F0)
    vectors = []

    def add(op, single, ni, A, B, C):
        vectors.append((op, single, ni, A, B, C, oracle(op, single, ni, A, B, C)))

    unary = ('sqrt', 'res', 'rsqrte', 'round')
    binary_ab = ('add', 'sub', 'div')
    # Class products: every unary/binary pair, fused on a class subset.
    for single in (False, True):
        for ni in (0, 1):
            for op in unary:
                if op in ('round', 'res') and not single:
                    continue
                for B in CLASSES:
                    add(op, single, ni, 0, B, 0)
            for op in binary_ab + ('mul',):
                for X in CLASSES:
                    for Y in CLASSES:
                        if op == 'mul':
                            add(op, single, ni, X, 0, Y)
                        else:
                            add(op, single, ni, X, Y, 0)
            subset = CLASSES[::3]
            for op in ('madd', 'msub', 'nmadd', 'nmsub'):
                for A in subset:
                    for B in subset:
                        for C in subset:
                            add(op, single, ni, A, B, C)
    # Sparse random operands near ties, fused forms and mul/div boundaries.
    for _ in range(1500):
        op = rng.choice(('madd', 'msub', 'nmadd', 'nmsub'))
        single = rng.random() < 0.6
        ni = rng.randint(0, 1)
        if single:
            A, C, B = sparse_single(rng, -60, 60), sparse_single(rng, -60, 60), sparse_single(rng, -60, 60)
        else:
            A, C, B = sparse(rng, -600, 600), sparse(rng, -600, 600), sparse(rng, -1100 // 2, 600)
        add(op, single, ni, A, B, C)
    # Constructed single-precision ties (the paired-single-3 shape): b a single
    # in [2^k, 2^(k+1)), a*c = (2^2n - 1) * 2^-(2n+24-k) = half a single ulp of b
    # minus 2^-(2n+24-k). The fused double lands exactly on the midpoint b +- ulp/2.
    for _ in range(120):
        n = rng.randint(15, 23)
        k = rng.randint(-30, 30)
        b_word = ((k + 127) << 23) | rng.randint(0, (1 << 23) - 1)
        if rng.random() < 0.5:
            b_word |= 0x80000000
        split = rng.randint(0, 2 * n + 24 - k)
        a_bits = d2b(float((1 << n) - 1) * 2.0 ** -split)
        c_bits = d2b(float((1 << n) + 1) * 2.0 ** -(2 * n + 24 - k - split))
        if rng.random() < 0.5:
            a_bits ^= SIGN
            c_bits ^= SIGN
        op = rng.choice(('madd', 'msub', 'nmadd', 'nmsub'))
        add(op, True, rng.randint(0, 1), a_bits, f32(b_word), c_bits)
    # Fused doubles one double ulp either side of, or on, a single midpoint with a
    # negligible product (the neighbour of r is itself the midpoint).
    near_midpoint = len(vectors)
    for _ in range(80):
        k = rng.randint(-40, 40)
        mid = ((k + 1023) << 52) | (rng.randint(0, (1 << 23) - 1) << 29) | (1 << 28)
        b_bits = (mid + rng.choice((-1, 0, 1))) | (SIGN if rng.random() < 0.5 else 0)
        a_bits = sparse(rng, -300, -200)
        c_bits = sparse(rng, -300, 100)
        add(rng.choice(('madd', 'msub', 'nmadd', 'nmsub')), True, rng.randint(0, 1), a_bits, b_bits, c_bits)
    # Crafted: fpu-2, paired-single-3, NI tininess boundaries (exact value just
    # below the smallest normal while the rounded value equals it).
    crafted = len(vectors)
    add('msub', False, 0, 0x3FF0000002000000, d2b(1.0), 0x3FF0000002000000)
    add('madd', True, 0, f32(0x397FFFC0), f32(0x3F800001), f32(0x39800020))
    add('madd', True, 1, d2b(-(2.0 ** -100)), 0x3810000000000000, d2b(2.0 ** -100))
    add('madd', True, 0, d2b(-(2.0 ** -100)), 0x3810000000000000, d2b(2.0 ** -100))
    add('madd', False, 1, d2b(-(2.0 ** -540)), 0x0010000000000000, d2b(2.0 ** -540))
    add('mul', False, 1, 0x3FEFFFFFFFFFFFFF, 0, 0x0010000000000000)
    add('mul', False, 0, 0x3FEFFFFFFFFFFFFF, 0, 0x0010000000000000)
    add('div', False, 1, 0x3FEFFFFFFFFFFFFF, d2b(2.0 ** 1022), 0)
    add('div', False, 0, 0x3FEFFFFFFFFFFFFF, d2b(2.0 ** 1022), 0)
    # fpu-3: NaN priority frA > frB > frC, sign kept and never negated.
    add('nmsub', False, 0, 0x7FF8000000000123, 0xFFF4000000000000, 0x7FF0000000000001)
    add('nmsub', True, 0, 0x7FF8000000000123, 0xFFF4000000000000, 0x7FF0000000000001)
    add('nmadd', False, 0, d2b(1.0), 0xFFF4000000000000, 0x7FF0000000000001)
    add('nmadd', True, 1, d2b(1.0), d2b(2.0), 0xFFF0000000000001)
    add('madd', False, 0, 0x7FF0000000000000, 0xFFF8000000000000, 0)
    add('div', True, 0, 0x8000000000000000, 0x8000000000000000, 0)
    add('sqrt', False, 0, 0, d2b(-1.0), 0)
    add('sub', False, 0, 0x7FF0000000000000, 0x7FF0000000000000, 0)
    # Found by fp_helpers_match_reference: r one double ulp above a midpoint.
    add('nmsub', True, 0, 0x69B0008000040000, 0xBFE0010010000001, 0x8940000000000000)

    names = {'add': 'kFpAdd', 'sub': 'kFpSub', 'mul': 'kFpMul', 'div': 'kFpDiv', 'sqrt': 'kFpSqrt',
             'res': 'kFpRes', 'rsqrte': 'kFpRsqrte', 'round': 'kFpRound', 'madd': 'kFpMAdd',
             'msub': 'kFpMSub', 'nmadd': 'kFpNMAdd', 'nmsub': 'kFpNMSub'}
    if len(sys.argv) > 1 and sys.argv[1] == '--plain':
        for op, single, ni, A, B, C, R in vectors:
            print(f"{OPS.index(op) | (0x10 if single else 0)} {ni} {A:016X} {B:016X} {C:016X} {R:016X}")
        return

    def naive(op, single, A, B, C):
        """The pre-fix engines: host arithmetic, two roundings, x86 NaNs (0xFFF8 default,
        first-operand propagation approximated), unary minus on NaNs."""
        a, b, c = b2d(A), b2d(B), b2d(C)
        table = {'add': lambda: a + b, 'sub': lambda: a - b, 'mul': lambda: a * c,
                 'madd': lambda: a * c + b, 'msub': lambda: a * c - b,
                 'nmadd': lambda: -(a * c + b), 'nmsub': lambda: -(a * c - b)}
        if op not in table:
            return None
        try:
            r = table[op]()
        except (OverflowError, ZeroDivisionError):
            return None
        if math.isnan(r):
            return None
        bits = d2b(r)
        return single_of_double_bits(bits) if single else bits

    # Embedded subset: every crafted vector, every fused vector whose single
    # rounding needed the exact residual, and a sample of the NI-sensitive,
    # NaN-producing and changed-from-naive class vectors.
    chosen = list(range(near_midpoint, len(vectors)))
    samples = {}
    for index, (op, single, ni, A, B, C, R) in enumerate(vectors[:near_midpoint]):
        category = None
        fused = op in ('madd', 'msub', 'nmadd', 'nmsub')
        if index >= near_midpoint - 1620:
            if single and fused:
                a, b, c = b2d(A), b2d(B), b2d(C)
                addend = -b if op in ('msub', 'nmsub') else b
                if all(map(math.isfinite, (a, b, c))) and not is_nan(R):
                    twice = single_of_double_bits(round_double(exact(A) * exact(C) + exact(d2b(addend))))
                    if op in ('nmadd', 'nmsub'):
                        twice ^= SIGN
                    if twice != R:
                        category = 'tie'
        elif is_nan(R):
            category = ('nan', op, single)
        elif R != oracle(op, single, 1 - ni, A, B, C):
            category = ('ni', op, single, ni)
        elif naive(op, single, A, B, C) not in (None, R):
            category = ('changed', op, single)
        if category is None:
            continue
        if category == 'tie':
            chosen.append(index)
            continue
        samples.setdefault(category, []).append(index)
    for category, members in sorted(samples.items(), key=lambda item: str(item[0])):
        step = max(1, len(members) // 6)
        chosen.extend(members[::step][:6])
    chosen = sorted(set(chosen))
    print(f"    // {len(chosen)} of {len(vectors)} oracle vectors (tools/ppcrecomp/fp_semantics_oracle.py)")
    for index in chosen:
        op, single, ni, A, B, C, R = vectors[index]
        print(f"    {{{names[op]}{' | kFpSingle' if single else ''}, {ni}u, 0x{A:016X}ull, 0x{B:016X}ull, "
              f"0x{C:016X}ull, 0x{R:016X}ull}},")


if __name__ == '__main__':
    main()
