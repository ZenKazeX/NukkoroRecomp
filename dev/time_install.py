"""Times one fresh install the way a player runs it, phase by phase.

    python dev/time_install.py --package <NukkoroRecomp-X.zip> --work <empty dir> --disc <extracted PAL disc> [--jobs N]

It unpacks the release package into <work>/src exactly as the downloadable setup does (tar --strip-components=1), runs
installer/Install.ps1 (the scripted install, the same steps as the setup window) into <work>/game, and stamps every line
the install log gains with the wall time it appeared. Nothing is reused between runs: <work> must not exist.
The phases are read back from the stamped log (see analyse()); <work>/phases.json holds the result.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import threading
import time
import xml.etree.ElementTree as ET
from pathlib import Path

GAME_FREE_TARGETS = {
    'shadow_host_isa_gate', 'shadow_native_execution', 'shadow_native_memory', 'shadow_native_gameplay', 'shadow_msl',
    'shadow_lift', 'shadow_assets', 'shadow_motion', 'shadow_d3d12ma', 'shadow_render', 'shadow_rwim',
    'shadow_render_lifted', 'shadow_runtime_common', 'shadow_runtime', 'ShadowPadConfig', 'shadow_render_shaders',
    'shadow_prebuilt_stub',
}


def tail(log: Path, out: list, stop: threading.Event):
    offset = 0
    partial = b''
    while True:
        done = stop.is_set()
        if log.exists():
            with open(log, 'rb') as f:
                f.seek(offset)
                data = f.read()
            if data:
                now = time.monotonic()
                offset += len(data)
                lines = (partial + data).split(b'\n')
                partial = lines.pop()
                for line in lines:
                    out.append((now, line.decode('utf-8', 'replace').rstrip('\r')))
        if done:
            if partial:
                out.append((time.monotonic(), partial.decode('utf-8', 'replace')))
            return
        time.sleep(0.1)


def source_owners(build: Path):
    """File name (no directory) -> owning vcxproj, from the generated projects."""
    owners = {}
    for proj in build.glob('*.vcxproj'):
        try:
            root = ET.parse(proj).getroot()
        except ET.ParseError:
            continue
        for el in root.iter():
            if el.tag.endswith('ClCompile') and 'Include' in el.attrib:
                owners.setdefault(Path(el.attrib['Include']).name.lower(), proj.stem)
    return owners


def analyse(stamped, t0, build: Path):
    rel = [(t - t0, line) for t, line in stamped]
    marks = {}
    for t, line in rel:
        m = re.match(r'^==== ([^:]+):', line)
        if m:
            marks.setdefault(m.group(1), t)
    end = rel[-1][0] if rel else 0.0
    owners = source_owners(build)
    spans = {}

    def touch(name, t):
        s = spans.setdefault(name, [t, t])
        s[0] = min(s[0], t)
        s[1] = max(s[1], t)

    for t, line in rel:
        s = line.strip()
        m = re.match(r'^(?:\d+>)?\s*([\w.\-]+\.(?:cpp|c|cxx))$', s)
        if m:
            touch(owners.get(m.group(1).lower(), '?' + m.group(1)), t)
            continue
        m = re.match(r'^(?:\d+>)?\s*(\w+)\.vcxproj -> ', s)
        if m:
            touch(m.group(1), t)
            continue
        if 'Compiling renderer shader' in s:
            touch('shadow_render_shaders', t)
        elif 'Recompiling PowerPC functions' in s:
            touch('ppcrecomp_run', t)
        elif s.startswith('PASS ') or s.startswith('{"errors"') or s.startswith('Pinned '):
            touch('post_link_checks', t)
    build_start = marks.get('Build', 0.0)
    build_end = marks.get('Copy game', end)
    shard = spans.get('shadow_recomp')
    first_shard = min((t for t, line in rel if re.match(r'^\s*(?:\d+>)?\s*recomp_\d{3}\.cpp$', line.strip())), default=None)
    translate = None
    if 'ppcrecomp_run' in spans and first_shard is not None:
        translate = [spans['ppcrecomp_run'][0], first_shard]
    exe_done = spans.get('ShadowTheHedgehog', [None, None])[1]
    shard_done = spans.get('shadow_recomp', [None, None])[1]
    free = [spans[n] for n in GAME_FREE_TARGETS if n in spans]
    phases = {
        'unpack_and_checks': [0.0, marks.get('Configure', 0.0)],
        'configure': [marks.get('Configure', 0.0), build_start],
        'build_total': [build_start, build_end],
        'game_code_free_libraries': [min(s[0] for s in free), max(s[1] for s in free)] if free else None,
        'ppcrecomp_compile': spans.get('ppcrecomp'),
        'shaders': spans.get('shadow_render_shaders'),
        'shard_translation': translate,
        'shard_compile': [first_shard, shard_done] if first_shard is not None and shard_done else None,
        'link': [max(shard_done, max((s[1] for s in free), default=0.0)), exe_done] if exe_done and shard_done else None,
        'post_link_checks': spans.get('post_link_checks'),
        'copy_exe': [marks.get('Copy game', end), marks.get('Copy game files', end)],
        'copy_game_files_and_finish': [marks.get('Copy game files', end), end],
    }
    return {'phases': {k: (None if v is None else {'start': round(v[0], 1), 'end': round(v[1], 1),
                                                    'seconds': round(v[1] - v[0], 1)}) for k, v in phases.items()},
            'projects': {k: {'start': round(v[0], 1), 'end': round(v[1], 1)} for k, v in sorted(spans.items(), key=lambda kv: kv[1][0])},
            'total_seconds': round(end, 1)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--package', required=True)
    ap.add_argument('--work', required=True)
    ap.add_argument('--disc', required=True)
    ap.add_argument('--jobs', type=int, default=0)
    args = ap.parse_args()
    work = Path(args.work)
    if work.exists():
        sys.exit(f'{work} exists: every timed install starts from nothing')
    src = work / 'src'
    src.mkdir(parents=True)
    t0 = time.monotonic()
    subprocess.run([os.path.join(os.environ['SystemRoot'], 'System32', 'tar.exe'), '-xf', str(Path(args.package).resolve()),
                    '--strip-components=1'], cwd=src, check=True)
    t_unpacked = time.monotonic()
    log = src / '.build' / 'install.log'
    stamped = [(t0, '==== Unpack: tar'), (t_unpacked, '==== Checks: Install.ps1')]
    stop = threading.Event()
    reader = threading.Thread(target=tail, args=(log, stamped, stop))
    reader.start()
    cmd = ['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', str(src / 'installer' / 'Install.ps1'),
           '-DiscPath', args.disc, '-InstallPath', str(work / 'game'), '-NoShortcuts']
    if args.jobs:
        cmd += ['-Jobs', str(args.jobs)]
    with open(work / 'console.log', 'wb') as console:
        rc = subprocess.run(cmd, stdout=console, stderr=subprocess.STDOUT).returncode
    t_end = time.monotonic()
    stop.set()
    reader.join()
    stamped.append((t_end, '==== End'))
    with open(work / 'stamped.log', 'w', encoding='utf-8') as f:
        for t, line in stamped:
            f.write(f'{t - t0:9.2f}  {line}\n')
    result = analyse(stamped, t0, src / '.build' / 'release')
    result['exit_code'] = rc
    result['unpack_seconds'] = round(t_unpacked - t0, 1)
    result['package'] = str(Path(args.package).resolve())
    result['jobs_argument'] = args.jobs
    (work / 'phases.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps({'exit_code': rc, 'total_seconds': result['total_seconds'],
                      'phases': {k: (v or {}).get('seconds') for k, v in result['phases'].items()}}, indent=1))
    sys.exit(rc)


if __name__ == '__main__':
    main()
