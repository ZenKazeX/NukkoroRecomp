#!/usr/bin/env python3
"""Boot into a stage and restart it on demand, through the development link.

Launches a non-shipping build with --boot-stage (any of the game's stages) and --dev-link, then drives it:
the game's own pause-menu Restart route (StageAction request 7, the "restart_stage" link command; see
docs/tools/STAGE_CONTROL.md) restarts the stage from its start, as many times as asked. No button presses, no
dying, no menu.

  python tools/stage_control.py --stage stg0100                       interactive: restart / status / quit
  python tools/stage_control.py --stage stg0100 --restart-every 20 --restarts 3
  python tools/stage_control.py --stage stg0604 --headless --restart-every 15 --restarts 1 -- --draw-distance 4

Everything after "--" is passed to the game. The card, SRAM, display.cfg and pad.cfg are COPIED into the work
directory first (default: the Westopolis recording's initial/ set), so the user's own saves are never touched.
A restart costs a life, as in the menu; the game refuses it with no life left, in two-player stages, and outside
the Play action (loading, events, goal), and the tool retries until the stage is in play.
"""
from __future__ import annotations

import argparse
import json
import shutil
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

SOURCE = Path(__file__).resolve().parent.parent
DEFAULT_EXE = SOURCE / '.build' / 'diag' / 'bin' / 'Release' / 'ShadowTheHedgehog.exe'
DEFAULT_GAME = SOURCE.parent / 'game'
DEFAULT_PROFILE = SOURCE / '.build' / 'recordings' / 'westopolis-cpu-spike' / 'initial'
PROFILE_FILES = ('card.raw', 'SRAM.pc.raw', 'display.cfg', 'pad.cfg')


class Link:
    """One game on the development link: JSON objects, one per line; replies carry the request's id."""

    def __init__(self, connection: socket.socket):
        self.connection = connection
        self.reader = connection.makefile('rb')
        self.next_id = 1
        self.hello = None
        self.lock = threading.Lock()

    def read_message(self):
        line = self.reader.readline()
        if not line:
            raise ConnectionError('the game closed the link')
        return json.loads(line.decode('utf-8', errors='replace'))

    def request(self, cmd: str, **fields) -> dict:
        with self.lock:
            rid = self.next_id
            self.next_id += 1
            message = {'id': rid, 'cmd': cmd, **{k: v for k, v in fields.items() if v is not None}}
            self.connection.sendall((json.dumps(message) + '\n').encode('utf-8'))
            while True:
                answer = self.read_message()
                if answer.get('id') == rid:
                    return answer
                if answer.get('event') == 'hello':
                    self.hello = answer


def launch(args, work: Path, endpoint: str) -> subprocess.Popen:
    argv = [str(args.exe), '--game-data', str(args.game_data),
            '--headless' if args.headless else '--windowed', '--no-audio' if args.no_audio else '--audio',
            '--memory-card', 'card.raw', '--display-cfg', 'display.cfg', '--pad-cfg', 'pad.cfg',
            '--state-file', 'unused.gekkostate', '--snapshot-key', 'none',
            '--boot-stage', args.stage, '--dev-link', endpoint, '--dev-link-name', 'stage-control', *args.game_args]
    (work / 'invocation.json').write_text(json.dumps({'argv': argv}, indent=2), encoding='utf-8')
    stdout = open(work / 'stdout.log', 'wb')
    stderr = open(work / 'stderr.log', 'wb')
    return subprocess.Popen(argv, cwd=work, stdout=stdout, stderr=stderr, stdin=subprocess.DEVNULL)


def restart(link: Link, timeout: float) -> dict:
    """Asks until the stage is in play (it refuses while loading); returns the accepted answer."""
    deadline = time.monotonic() + timeout
    while True:
        answer = link.request('restart_stage')
        if answer.get('ok'):
            return answer
        error = answer.get('error', '')
        retryable = 'not in play' in error or 'does not exist yet' in error or 'refuses' in error
        if not retryable or time.monotonic() > deadline:
            raise RuntimeError('restart refused: ' + error)
        time.sleep(0.25)


def wait_game_seconds(link: Link, seconds: float):
    start = link.request('status').get('game_seconds', 0.0)
    while link.request('status').get('game_seconds', 0.0) - start < seconds:
        time.sleep(0.25)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--stage', required=True, help='stgNNNN or NNNN, as --boot-stage takes it')
    parser.add_argument('--exe', type=Path, default=DEFAULT_EXE, help='a non-shipping build (the link is not in shipping)')
    parser.add_argument('--game-data', type=Path, default=DEFAULT_GAME)
    parser.add_argument('--profile', type=Path, default=DEFAULT_PROFILE,
                        help='folder with card.raw, SRAM.pc.raw, display.cfg, pad.cfg to COPY (never used in place)')
    parser.add_argument('--work', type=Path, default=SOURCE / '.build' / 'stage-control')
    parser.add_argument('--headless', action='store_true')
    parser.add_argument('--no-audio', action='store_true')
    parser.add_argument('--restart-every', type=float,
                        help='game seconds (from launch, then from each restart) before each restart')
    parser.add_argument('--restarts', type=int, default=1)
    parser.add_argument('--then-run', type=float, default=0.0, help='game seconds to run after the last restart')
    parser.add_argument('--timeout', type=float, default=120.0, help='wall seconds to wait for the stage to be in play')
    parser.add_argument('game_args', nargs='*', help='after "--": passed to the game')
    args = parser.parse_args(argv)

    if args.work.exists():
        shutil.rmtree(args.work)
    args.work.mkdir(parents=True)
    for name in PROFILE_FILES:
        shutil.copy2(args.profile / name, args.work / name)

    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.bind(('127.0.0.1', 0))
    server.listen(1)
    server.settimeout(args.timeout)
    endpoint = '127.0.0.1:%d' % server.getsockname()[1]
    game = launch(args, args.work, endpoint)
    try:
        connection, _ = server.accept()
    except socket.timeout:
        game.kill()
        print('the game never connected; see', args.work / 'stderr.log', file=sys.stderr)
        return 1
    link = Link(connection)
    link.hello = link.read_message()
    print('connected:', link.hello.get('name', ''), link.hello.get('build', ''))

    try:
        if args.restart_every is not None:
            for index in range(args.restarts):
                wait_game_seconds(link, args.restart_every)
                answer = restart(link, args.timeout)
                print('restart %d at %.2f s of game time (lives before it: %s)' %
                      (index + 1, answer.get('game_seconds', 0.0), answer.get('lives')))
            if args.then_run > 0:
                wait_game_seconds(link, args.then_run)
            link.request('quit')
        else:
            print('commands: restart | status | quit')
            for line in sys.stdin:
                command = line.strip()
                if command == 'restart':
                    try:
                        answer = restart(link, args.timeout)
                        print('restarted at %.2f s (lives before it: %s)' % (answer.get('game_seconds', 0.0),
                                                                        answer.get('lives')))
                    except RuntimeError as error:
                        print(error)
                elif command == 'status':
                    print(json.dumps(link.request('status')))
                elif command == 'quit':
                    link.request('quit')
                    break
                elif command:
                    print('unknown command:', command)
    except ConnectionError as error:
        print(error, file=sys.stderr)
    finally:
        try:
            game.wait(timeout=30)
        except subprocess.TimeoutExpired:
            game.kill()
    print('logs:', args.work)
    return 0


if __name__ == '__main__':
    sys.exit(main())
