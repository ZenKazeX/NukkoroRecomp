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
import shutil
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


def analyse(stamped, t0, build: Path, t0_wall=None):
    """Phases on the build's critical path, from fixed events of the stamped log (seconds from the unpack's start).

    The build runs projects in parallel, so per-project spans overlap; the phases below are consecutive instead:
    each ends where the next event the game's executable waits for happens."""
    rel = [(t - t0, line) for t, line in stamped]
    marks = {}
    for t, line in rel:
        m = re.match(r'^==== ([^:]+):', line)
        if m:
            marks.setdefault(m.group(1), t)
    end = rel[-1][0] if rel else 0.0
    done = {}
    first_file = {}
    owners = source_owners(build)
    for t, line in rel:
        s = line.strip()
        m = re.match(r'^(?:\d+>)?\s*(\w+)\.vcxproj -> ', s)
        if m:
            done.setdefault(m.group(1), t)
            continue
        m = re.match(r'^(?:\d+>)?\s*([\w.\-]+\.(?:cpp|c|cxx))$', s)
        if m:
            first_file.setdefault(owners.get(m.group(1).lower(), '?'), t)
    def first(pattern):
        return min((t for t, line in rel if re.search(pattern, line)), default=None)
    def last(pattern):
        return max((t for t, line in rel if re.search(pattern, line)), default=None)
    build_start = marks.get('Build')
    translate_start = first(r'Recompiling PowerPC functions')
    translate_end = None
    stamp = build / 'generated' / 'recomp.stamp'
    if t0_wall is not None and stamp.exists():
        translate_end = stamp.stat().st_mtime - t0_wall
    first_shard = first(r'^\s*(?:\d+>)?\s*recomp_(?:\d{3}|table)\.cpp$')
    shards_done = done.get('shadow_recomp')
    exe_done = done.get('ShadowTheHedgehog')
    checks_done = last(r'^\s*(PASS |Pinned |\{"errors")')
    pad_done = done.get('ShadowPadConfig')
    free_libs = [done[n] for n in GAME_FREE_TARGETS if n in done and n != 'ShadowPadConfig']
    def span(a, b):
        return None if a is None or b is None else [a, b]
    phases = {
        'unpack_and_checks': [0.0, marks.get('Configure', 0.0)],
        'configure': span(marks.get('Configure'), build_start),
        'before_translation (game-code-free libraries, ppcrecomp, shaders)': span(build_start, translate_start),
        'shard_translation (ppcrecomp run)': span(translate_start, translate_end),
        'wait_for_libraries (shards need shadow_render_lifted)': span(translate_end, first_shard),
        'shard_compile': span(first_shard, shards_done),
        'link (launcher + exe)': span(shards_done, exe_done),
        'post_link_checks': span(exe_done, checks_done) if checks_done and exe_done and checks_done > exe_done else None,
        'ShadowPadConfig': span(checks_done if checks_done and exe_done and checks_done > exe_done else exe_done, pad_done),
        'copy_and_finish': span(marks.get('Copy game'), end),
    }
    info = {
        'game_code_free_libraries_done_at': round(max(free_libs), 1) if free_libs else None,
        'ppcrecomp_done_at': round(done['ppcrecomp'], 1) if 'ppcrecomp' in done else None,
        'shaders': [round(x, 1) for x in (first('Compiling renderer shader'), last('Compiling renderer shader'))] if first('Compiling renderer shader') else None,
        'project_done_at': {k: round(v, 1) for k, v in sorted(done.items(), key=lambda kv: kv[1])},
        'project_first_file_at': {k: round(v, 1) for k, v in sorted(first_file.items(), key=lambda kv: kv[1])},
    }
    return {'phases': {k: (None if v is None else {'start': round(v[0], 1), 'end': round(v[1], 1),
                                                    'seconds': round(v[1] - v[0], 1)}) for k, v in phases.items()},
            'build_seconds': round((marks.get('Copy game', end) - build_start), 1) if build_start else None,
            'info': info, 'total_seconds': round(end, 1)}


def reanalyse(work: Path):
    """Recomputes phases.json from a finished run's stamped.log."""
    stamped = []
    for line in (work / 'stamped.log').read_text(encoding='utf-8').splitlines():
        stamped.append((float(line[:9]), line[11:]))
    old = json.loads((work / 'phases.json').read_text(encoding='utf-8'))
    t0_wall = old.get('t0_wall')
    if t0_wall is None:  # older runs: stamped.log was written right at the end
        t0_wall = (work / 'stamped.log').stat().st_mtime - stamped[-1][0]
    src = Path(old.get('src') or work / 'src')
    result = analyse(stamped, 0.0, src / '.build' / 'release', t0_wall)
    for k in ('exit_code', 'unpack_seconds', 'package', 'jobs_argument', 'src', 'drop_prebuilt'):
        result[k] = old.get(k)
    result['t0_wall'] = t0_wall
    (work / 'phases.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    return result


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--package')
    ap.add_argument('--work', required=True)
    ap.add_argument('--disc')
    ap.add_argument('--jobs', type=int, default=0)
    ap.add_argument('--src', help='unpack here instead of <work>/src (must not exist)')
    ap.add_argument('--drop-prebuilt', action='store_true', help='delete the package prebuilt/ before installing')
    ap.add_argument('--reanalyse', action='store_true', help='recompute phases.json of a finished run')
    args = ap.parse_args()
    work = Path(args.work)
    if args.reanalyse:
        print(json.dumps(reanalyse(work)['phases'], indent=1))
        return
    if work.exists():
        sys.exit(f'{work} exists: every timed install starts from nothing')
    src = Path(args.src) if args.src else work / 'src'
    if src.exists():
        sys.exit(f'{src} exists')
    work.mkdir(parents=True)
    src.mkdir(parents=True)
    t0 = time.monotonic()
    t0_wall = time.time()
    subprocess.run([os.path.join(os.environ['SystemRoot'], 'System32', 'tar.exe'), '-xf', str(Path(args.package).resolve()),
                    '--strip-components=1'], cwd=src, check=True)
    if args.drop_prebuilt:
        shutil.rmtree(src / 'prebuilt', ignore_errors=True)
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
    result = analyse(stamped, t0, src / '.build' / 'release', t0_wall)
    result['t0_wall'] = t0_wall
    result['exit_code'] = rc
    result['unpack_seconds'] = round(t_unpacked - t0, 1)
    result['package'] = str(Path(args.package).resolve())
    result['jobs_argument'] = args.jobs
    result['src'] = str(src)
    result['drop_prebuilt'] = args.drop_prebuilt
    (work / 'phases.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps({'exit_code': rc, 'total_seconds': result['total_seconds'],
                      'phases': {k: (v or {}).get('seconds') for k, v in result['phases'].items()}}, indent=1))
    sys.exit(rc)


if __name__ == '__main__':
    main()
