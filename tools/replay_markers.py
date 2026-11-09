#!/usr/bin/env python3
"""Replay markers: list the moments marked during a tape replay, and replay around one.

While a recorded tape plays (--input-tape-play), the marker key (default F8, --marker-key) appends one line per press
to <tape>.markers.jsonl (or --marker-file): the game tick, the render and video frame, and the tape sample. Several
markers per recording, several replay sessions per file. docs/tools/REPLAY_MARKERS.md.

The range key (default F9, --range-key) marks a stretch instead: its first press is the range's start ("kind":
"range-start"), the next its end ("range-end"), both numbered ("range": 1, 2, ...). inspect / play take --range N in
place of --marker N: the state is saved at the start, inspect captures every frame to the end, and play replays the
range over and over (--loops; default until the window is closed before the range ends).

  python tools/replay_markers.py replay --recording .build/recordings/final-haunt --stage stg0604 [--fullscreen]
      watch the recording replay (window, audio) and press F8 (--marker-key) at each bug: markers are appended to
      <recording>/input.tape.markers.jsonl, one session per replay
  python tools/replay_markers.py list <markers.jsonl>
  python tools/replay_markers.py inspect --recording .build/recordings/final-haunt --stage stg0604 --marker 2
      [--markers FILE] [--session S] [--before 2] [--after 2] [--every 1] [--size 1280x720] [--out DIR]
  python tools/replay_markers.py play --recording ... --stage ... --marker 2 [--before 3]   (windowed, watch it)
  python tools/replay_markers.py play --recording ... --stage ... --range 1 [--loops 5] [--fullscreen]   (loop a range)
  python tools/replay_markers.py inspect --recording ... --stage ... --range 1 [--every 1] [--size 1920x1080]

Pointing at the bug: while `play` (or `replay`) runs, a left click on the picture - or F8 with the mouse over it - adds
a point marker with the spot ("x", "y": fractions of the picture from the top left). `inspect --marker N` captures
around it and circles the spot on the marker's frame (marker-point.png).

inspect / play first replay the tape from boot to `before` seconds ahead of the marker and save a savestate there
(--save-state-at-tick, then --stop-at-tick). inspect then loads that state, lets the tape carry on and captures every
`every`-th frame up to `after` seconds past the marker, plus a contact sheet; play loads it in a window with audio so
the moment can be watched again and again (the savestate stays in the output folder). The recording's initial/ set
(card, SRAM, display.cfg, pad.cfg) is copied, never used in place.
"""
from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

SOURCE = Path(__file__).resolve().parent.parent
DEFAULT_EXE = SOURCE / '.build' / 'diag' / 'bin' / 'Release' / 'ShadowTheHedgehog.exe'
GAME = SOURCE.parent / 'game'
TICKS = 40_500_000  # game ticks per second
PROFILE = ('card.raw', 'SRAM.pc.raw', 'display.cfg', 'pad.cfg')


def read_markers(path: Path) -> list[dict]:
    markers = []
    for line in path.read_text(encoding='utf-8').splitlines():
        line = line.strip()
        if line:
            markers.append(json.loads(line))
    return markers


def pick(markers: list[dict], number: int, session: str | None) -> dict:
    sessions = [m['session'] for m in markers]
    session = session or (sessions[-1] if sessions else None)
    for m in markers:
        if m['session'] == session and m['marker'] == number:
            return m
    raise SystemExit(f'no marker {number} in session {session}; see "list"')


def pick_range(markers: list[dict], number: int, session: str | None) -> tuple[dict, dict]:
    # The latest session that has the range, unless one is named.
    sessions = [m['session'] for m in markers if m.get('range') == number]
    session = session or (sessions[-1] if sessions else None)
    ends = {m.get('kind'): m for m in markers if m['session'] == session and m.get('range') == number}
    if 'range-start' not in ends or 'range-end' not in ends:
        raise SystemExit(f'range {number} is not complete in session {session} (press F9 twice); see "list"')
    return ends['range-start'], ends['range-end']


def target(args, markers: list[dict]) -> tuple[dict, int | None]:
    # The marker to work around, and for a range its end tick (a range's start is its marker; the work starts there).
    if args.range is None:
        return pick(markers, args.marker, args.session), None
    start, end = pick_range(markers, args.range, args.session)
    if args.before is None:
        args.before = 0.0
    args.after = (int(end['game_ticks']) - int(start['game_ticks'])) / TICKS
    return start, int(end['game_ticks'])


def common_args(work: Path, stage: str | None) -> list[str]:
    # The replay from boot names the stage; a run that loads a savestate must not (it starts where the state is).
    args = ['--game-data', str(GAME), '--no-xinput', '--memory-card', str(work / 'card.raw'),
            '--display-cfg', str(work / 'display.cfg'), '--pad-cfg', str(work / 'pad.cfg'),
            '--snapshot-key', 'none', '--marker-key', 'none']
    return args + ['--boot-stage', stage] if stage else args


def prepare(args, marker: dict) -> tuple[Path, Path, int]:
    label = f"r{marker['range']}" if marker.get('kind') == 'range-start' else f"m{marker['marker']}"
    out = Path(args.out) if args.out else SOURCE / '.build' / 'markers' / f"{marker['session'].replace(':', '')}-{label}"
    out = out.resolve()
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    recording = Path(args.recording).resolve()
    for name in PROFILE:
        shutil.copy2(recording / 'initial' / name, out / name)
    shutil.copy2(recording / 'input.tape', out / 'input.tape')
    start = max(0, int(marker['game_ticks']) - int(args.before * TICKS))
    state = out / 'before.gekkostate'
    print(f"marker {marker['marker']} at {marker['game_seconds']:.3f} s (tick {marker['game_ticks']}); "
          f"saving a state {args.before} s before it (tick {start})")
    argv = [str(args.exe), *common_args(out, args.stage), '--headless', '--no-audio',
            '--input-tape-play', str(out / 'input.tape'), '--state-file', str(out / 'unused.gekkostate'),
            '--save-state-at-tick', f'{start}:{state}', '--stop-at-tick', str(start + 1)]
    with open(out / 'replay-to-marker.log', 'wb') as log:
        subprocess.run(argv, cwd=out, stdout=log, stderr=subprocess.STDOUT, check=False)
    if not state.exists():
        raise SystemExit(f'no savestate written; see {out / "replay-to-marker.log"}')
    return out, state, start


def circle_point(out: Path, shots: list[Path], marker_index: int, marker: dict) -> None:
    """The frame at a pointed marker (a click in `play`), the spot circled: <out>/marker-point.png."""
    if 'x' not in marker or not shots:
        return
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        return
    image = Image.open(shots[marker_index]).convert('RGB')
    x, y = marker['x'] * image.width, marker['y'] * image.height
    r = max(12, image.height // 24)
    draw = ImageDraw.Draw(image)
    for k in range(3):
        draw.ellipse((x - r - k, y - r - k, x + r + k, y + r + k), outline=(255, 230, 0))
    image.save(out / 'marker-point.png')
    print(f"pointed at ({marker['x']:.3f}, {marker['y']:.3f}) of the picture: {out / 'marker-point.png'}")


def contact_sheet(out: Path, shots: list[Path], marker_index: int) -> None:
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        return
    if not shots:
        return
    thumbs = [Image.open(p).convert('RGB') for p in shots]
    w = 320
    h = int(thumbs[0].height * w / thumbs[0].width)
    columns = 8
    rows = (len(thumbs) + columns - 1) // columns
    sheet = Image.new('RGB', (columns * w, rows * (h + 16)), (16, 16, 16))
    draw = ImageDraw.Draw(sheet)
    for i, t in enumerate(thumbs):
        x, y = (i % columns) * w, (i // columns) * (h + 16)
        sheet.paste(t.resize((w, h)), (x, y + 16))
        label = shots[i].stem + ('  <- marker' if i == marker_index else '')
        draw.text((x + 4, y + 2), label, fill=(255, 220, 0) if i == marker_index else (220, 220, 220))
    sheet.save(out / 'contact-sheet.png')


def inspect(args) -> int:
    markers = read_markers(Path(args.markers) if args.markers else Path(args.recording) / 'input.tape.markers.jsonl')
    marker, _ = target(args, markers)
    out, state, start = prepare(args, marker)
    frames = int((args.before + args.after) * 60 / args.every)
    width, height = args.size.split('x')
    argv = [str(args.exe), *common_args(out, None), '--headless', '--no-audio',
            '--load-state', '--state-file', str(state), '--input-tape-play', str(out / 'input.tape'),
            '--native-output', args.size, '--width', width, '--height', height,
            '--native-screenshot', str(out / 'frame.png'), '--native-screenshot-frame', '1',
            '--native-screenshot-stride', str(args.every), '--native-screenshot-count', str(frames),
            '--budget-ms', str(int((args.before + args.after + 0.5) * 1000))]
    with open(out / 'inspect.log', 'wb') as log:
        subprocess.run(argv, cwd=out, stdout=log, stderr=subprocess.STDOUT, check=False)
    shots = sorted(out.glob('frame-*.png'), key=lambda p: int(p.stem.split('-')[1]))
    marker_index = min(len(shots) - 1, int(args.before * 60 / args.every)) if shots else -1
    contact_sheet(out, shots, marker_index)
    circle_point(out, shots, marker_index, marker)
    print(f'{len(shots)} frames ({args.before} s before .. {args.after} s after, every {args.every}) in {out}')
    if shots:
        print(f'the marker is about frame {shots[marker_index].name}; contact sheet: {out / "contact-sheet.png"}')
    print(f'savestate {args.before} s before the marker: {state}')
    return 0


def replay(args) -> int:
    recording = Path(args.recording).resolve()
    work = SOURCE / '.build' / 'markers' / 'replay-work'
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)
    for name in PROFILE:
        shutil.copy2(recording / 'initial' / name, work / name)
    markers = recording / 'input.tape.markers.jsonl'
    argv = [str(args.exe), *common_args(work, args.stage), '--fullscreen' if args.fullscreen else '--windowed',
            '--audio', '--input-tape-play', str(recording / 'input.tape'), '--state-file', str(work / 'unused.gekkostate'),
            '--marker-key', args.marker_key, '--marker-file', str(markers)]
    argv.remove('--marker-key')  # common_args turned the key off; this run wants it
    argv.remove('none')
    print(f'replaying {recording / "input.tape"}; press {args.marker_key} at each bug, F9 at the start and end of a '
          f'stretch to loop - markers go to {markers}')
    subprocess.run(argv, cwd=work, check=False)
    if markers.exists():
        print('markers so far:')
        main(['list', str(markers)])
    return 0


def play(args) -> int:
    markers = read_markers(Path(args.markers) if args.markers else Path(args.recording) / 'input.tape.markers.jsonl')
    marker, end = target(args, markers)
    out, state, start = prepare(args, marker)
    argv = [str(args.exe), *common_args(out, None), '--fullscreen' if args.fullscreen else '--windowed', '--audio',
            '--load-state', '--state-file', str(state), '--input-tape-play', str(out / 'input.tape')]
    # Point at the bug: a click on the picture (or F8 with the mouse over it) appends a marker with the spot to the
    # recording's marker file; `inspect --marker N` then circles it.
    marker_file = Path(args.markers) if args.markers else Path(args.recording).resolve() / 'input.tape.markers.jsonl'
    argv[argv.index('--marker-key') + 1] = 'F8'
    argv += ['--marker-file', str(marker_file)]
    print(f'click on the bug (or press F8 with the mouse on it): markers go to {marker_file}')
    if end is None:
        print('playing in a window from the saved state; close the window when done')
        subprocess.run(argv, cwd=out, check=False)
        return 0
    # A range: from its start to its end, again and again. Each pass is a fresh run from the saved state, so every
    # pass shows the same frames; closing the window before the range's end stops the loop.
    # The budget only backs the stop up (an exe from before windowed --stop-at-tick would play on).
    argv += ['--stop-at-tick', str(end), '--budget-ms', str(int((end - start) / TICKS * 1000) + 5000)]
    passes = 0
    print(f'looping range {args.range}: {(end - start) / TICKS:.2f} s, tick {start} to {end}; close the window to stop')
    while args.loops == 0 or passes < args.loops:
        passes += 1
        with open(out / 'play.log', 'wb') as log:
            subprocess.run(argv, cwd=out, stdout=log, stderr=subprocess.STDOUT, check=False)
        if b'[stop-at-tick] asked' not in (out / 'play.log').read_bytes():
            break  # closed before the range's end
    print(f'{passes} pass(es)')
    return 0


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    replaying = sub.add_parser('replay')
    replaying.add_argument('--recording', required=True, help='folder with input.tape and initial/')
    replaying.add_argument('--stage', required=True, help='the --boot-stage the tape was recorded with')
    replaying.add_argument('--marker-key', default='F8')
    replaying.add_argument('--fullscreen', action='store_true')
    replaying.add_argument('--exe', type=Path, default=DEFAULT_EXE)
    listing = sub.add_parser('list')
    listing.add_argument('markers')
    for name in ('inspect', 'play'):
        p = sub.add_parser(name)
        p.add_argument('--recording', required=True, help='folder with input.tape and initial/')
        p.add_argument('--stage', required=True, help='the --boot-stage the tape was recorded with')
        which = p.add_mutually_exclusive_group(required=True)
        which.add_argument('--marker', type=int)
        which.add_argument('--range', type=int, help='a range marked with the range key (F9)')
        p.add_argument('--markers', help='default: <recording>/input.tape.markers.jsonl')
        p.add_argument('--session', help='default: the latest session in the file')
        p.add_argument('--before', type=float, help='seconds before the marker (default 2; a range: 0)')
        p.add_argument('--after', type=float, default=2.0, help='seconds after the marker (inspect)')
        p.add_argument('--every', type=int, default=1, help='capture every n-th frame (inspect)')
        p.add_argument('--size', default='1280x720', help='capture size (inspect)')
        p.add_argument('--exe', type=Path, default=DEFAULT_EXE)
        p.add_argument('--out')
        if name == 'play':
            p.add_argument('--fullscreen', action='store_true')
            p.add_argument('--loops', type=int, default=0, help='range passes (0: until the window is closed)')
    args = parser.parse_args(argv)
    if getattr(args, 'marker', None) is not None and args.before is None:
        args.before = 2.0
    if args.command == 'list':
        markers = read_markers(Path(args.markers))
        for m in markers:
            tape = f" tape sample {m.get('tape_sample')}/{m.get('tape_samples')}" if 'tape_sample' in m else ''
            tape += f"  at ({m['x']:.3f}, {m['y']:.3f})" if 'x' in m else ''
            kind = m.get('kind', 'point')
            what = f"{kind} {m['range']}" if kind != 'point' else 'point'
            print(f"{m['session']}  #{m['marker']:<3} {what:<14} {m['mode']:<6} {m['game_seconds']:9.3f} s"
                  f"  tick {m['game_ticks']}  render frame {m['render_frame']}  vi {m['vi_frame']}{tape}")
        return 0
    if args.command == 'replay':
        return replay(args)
    return inspect(args) if args.command == 'inspect' else play(args)


if __name__ == '__main__':
    sys.exit(main())
