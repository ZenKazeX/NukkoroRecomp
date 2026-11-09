"""The new renderer against itself (docs/lift-20261006/R_DESIGN.md 2.9, gate 3: byte-exact self-regression).

Renders a stage's land twice with tools/stage_preview on WARP (the deterministic software rasterizer) and requires the
two images to be byte-identical; also requires the tool to have resolved every texture and drawn something.

  render_determinism.py --tool <stage_preview.exe> --files <disc files dir> [--stage stg0100] [--out <dir>]
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path


def render(tool: Path, files: Path, stage: str, out: Path) -> str:
    result = subprocess.run([str(tool), str(files), stage, str(out), '--warp', '--width', '480', '--height', '270',
                             '--skip-far', '--rise', '0.06', '--side', '0.12'],
                            capture_output=True, text=True, timeout=600)
    if result.returncode != 0:
        print(result.stdout)
        print(result.stderr)
        raise SystemExit(f'stage_preview failed with exit code {result.returncode}')
    return result.stdout


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument('--tool', type=Path, required=True)
    p.add_argument('--files', type=Path, required=True)
    p.add_argument('--stage', default='stg0100')
    p.add_argument('--out', type=Path)
    a = p.parse_args()
    if not (a.files / a.stage).exists():
        print(f'render_determinism: no game data at {a.files / a.stage} - skipped')
        return 0
    out = a.out or Path(tempfile.mkdtemp(prefix='render-determinism-'))
    out.mkdir(parents=True, exist_ok=True)
    first_log = render(a.tool, a.files, a.stage, out / 'first.png')
    second_log = render(a.tool, a.files, a.stage, out / 'second.png')
    failures = []
    if 'debug layer:' in first_log or 'debug layer:' in second_log:
        failures.append('debug-layer messages')
    m = re.search(r'(\d+) meshes, (\d+) draws, (\d+) triangles, (\d+) draws without their texture', first_log)
    if not m or int(m.group(2)) == 0 or int(m.group(4)) != 0:
        failures.append('expected draws with every texture resolved: ' + (m.group(0) if m else 'no summary line'))
    a_bytes = (out / 'first.png').read_bytes()
    b_bytes = (out / 'second.png').read_bytes()
    if a_bytes != b_bytes:
        failures.append(f'the two renders differ ({len(a_bytes)} vs {len(b_bytes)} bytes)')
    print(first_log.strip())
    if failures:
        for f in failures:
            print('FAIL: ' + f)
        return 1
    print(f'render_determinism: two WARP renders of {a.stage} are byte-identical ({len(a_bytes)} bytes)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
