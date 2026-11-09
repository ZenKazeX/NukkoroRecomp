"""Lift purity check (docs/lift-20261006/LIFT_PROGRAMME.md, docs/NORTH_STAR.md).

The final question for every lifted subsystem: could this code have been written today for a 64-bit PC
without knowing that the GameCube ever existed? This check makes the mechanical part of that answer a test.

  check_purity.py [--root runtime/lift]

Every directory under --root is one lifted subsystem. None of its sources (*.h, *.hpp, *.cpp, *.inl) may
contain (comments included: a lifted subsystem does not explain itself in console terms either; the docs
carry the history):

  * the translated machine: gekko::Cpu, GuestMemory, GcAddr, the generated code (recomp), HLE hooks,
    guest registers (gpr[ / fpr[ / spr[), the runtime's console headers (gekko/cpu.h, gekko/guest_memory.h,
    gekko/system.h, gekko/recomp.h, gekko/native_*);
  * guest addresses: any hexadecimal literal 0x8XXXXXXX / 0xCCXXXXXX / 0xE0000000 (window, device, locked
    cache);
  * console memory and hardware: MEM1, ARAM, DSP, AI DMA / AIDMA, ARQ, BAT, physical address, real mode,
    uncached, Gekko, Flipper, GameCube.

The renderer (runtime/lift/render, docs/lift-20261006/R_DESIGN.md) is held to more: it knows neither the
console's graphics pipeline (GX, TEV, EFB, XFB, TMEM, TLUT, display lists, the command FIFO) nor RenderWare
(the engine's scene library: Rw* / Rp* / Rx* names). Game code hands it views, passes, draw records and
assets as values; decoding the disc's asset formats belongs to the importers, not to the renderer.

There are no boundary adapter files: translated callers reach a lifted subsystem through call sites the
generator lowers into direct calls of its native API (docs/lift-20261006/LIFT_PROGRAMME.md section 2), so
every file under --root is checked, and a file named *_boundary.* is itself reported as a violation.

Exit 1 with every violation (file:line: what) when any is found; exit 0 when the tree is pure (also when
--root does not exist yet: nothing lifted, nothing to check)."""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

RULES = [
    ('the translated CPU', re.compile(r'\bgekko::Cpu\b|\bCpu\s*&|\bclass\s+Cpu\b')),
    ('the guest memory window', re.compile(r'\bGuestMemory\b|\bGcAddr\b|\bguest_memory\b|\bpeek_pointer\b')),
    ('generated code / HLE', re.compile(r'\brecomp::|\binstall_hle\b|\btry_hle\b|\bRecompiledCode\b')),
    ('guest registers', re.compile(r'\b(?:gpr|fpr|spr)\[')),
    ('a console runtime header', re.compile(r'#\s*include\s*[<"]gekko/(?:cpu|guest_memory|system|recomp|native_[a-z_]+|'
                                            r'sample_memory|game_memory|gc_addr)\.h[>"]')),
    ('a guest address', re.compile(r'\b0[xX](?:8[0-9A-Fa-f]{7}|[cC][cC][0-9A-Fa-f]{6}|[eE]0000000)\b')),
    ('console memory or hardware', re.compile(
        r'\b(?:MEM1|ARAM|aram|DSP|dsp|AIDMA|AI DMA|ARQ|BATs?|real[- ]mode|uncached|Gekko|Flipper|GameCube|'
        r'gamecube|physical address)\b')),
]


# Extra rules for one lifted subsystem (its directory name under --root).
SUBSYSTEM_RULES = {
    'render': [
        ('the console graphics pipeline', re.compile(
            r'\b(?:GX\w*|TEV|EFB|XFB|TMEM|TLUT|FIFO)\b|display[ _]list|\bgx_|\bgx::')),
        ('RenderWare', re.compile(r'RenderWare|\bRw[A-Z]\w*|\bRp[A-Z]\w*|\bRx[A-Z]\w*|\brw::')),
    ],
}


def boundary(path: Path) -> bool:
    return re.search(r'_boundary\.(?:cpp|h|hpp)$', path.name) is not None


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument('--root', type=Path, default=ROOT / 'runtime/lift')
    a = p.parse_args()
    if not a.root.exists():
        print(f'lift purity: {a.root} does not exist yet - nothing lifted, nothing to check')
        return 0
    violations = []
    files = 0
    for path in sorted(a.root.rglob('*')):
        if path.suffix not in ('.h', '.hpp', '.cpp', '.inl', '.hlsl', '.hlsli'):
            continue
        if boundary(path):
            violations.append(f'{path}: a boundary adapter file - lifted subsystems have none (call sites are '
                              f'lowered by the generator)')
            continue
        files += 1
        relative = path.relative_to(a.root)
        rules = RULES + SUBSYSTEM_RULES.get(relative.parts[0] if len(relative.parts) > 1 else '', [])
        for n, line in enumerate(path.read_text(encoding='utf-8', errors='replace').splitlines(), 1):
            for what, pattern in rules:
                m = pattern.search(line)
                if m:
                    shown = path.relative_to(ROOT) if path.is_relative_to(ROOT) else path
                    violations.append(f'{shown}:{n}: {what}: {m.group(0)!r}')
    if violations:
        print(f'lift purity: {len(violations)} violation(s) in {files} lifted source file(s):')
        for v in violations:
            print('  ' + v)
        return 1
    print(f'lift purity: {files} lifted source file(s), no GameCube concept')
    return 0


if __name__ == '__main__':
    sys.exit(main())
