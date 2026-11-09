"""Semantic state probes: what a checkpoint's logical game state is, read out of a running or finished build.

The game's logic still runs translated in both builds, so these values live in the game's address window; the
workstation - not the game - knows where (docs/comparator/README.md "Architecture"). Each probe names a guest range
(an address, or a pointer path "A>o1>o2", the state report's syntax) and how to decode it. A build whose subsystem is
lifted will report its values natively; its probe then changes here, nowhere else.

Sources: Shadow-The-Hedgehog-RE/gameplay (GAMEPLAY_STRUCTURES.md, MISSION_SYSTEM.md, COLLECTIBLES_AND_SCORING.md),
GekkoForge/docs/visibility-2026-09-21 (the camera holder), gameplay-recon knowledge (the MT19937 random generator);
the camera rows were checked live: both builds agree on the modelling matrix, its LTM cache is not maintained.
"""
from __future__ import annotations

import hashlib
import struct
from dataclasses import dataclass


@dataclass(frozen=True)
class Probe:
    name: str        # dotted, grouped in the UI by its first part
    range: str       # guest range, state-report syntax
    kind: str        # s32 u32 u8 f32 f32x3 hex sha
    note: str = ''


PROBES = (
    Probe('stage.number', '0x8057E808:4', 's32', 'running stage, decimal (100 = Westopolis)'),
    Probe('stage.index', '0x8057E80C:4', 's32', 'record index in the stage table'),
    Probe('stage.state', '0x80577040:4', 'u32', 'StageState (0x80577038) +0x8: 3 = playing'),
    Probe('game.frame_counter', '0x8057E7F0:4', 'u32', 'game frame counter (likely)'),
    Probe('game.time_remaining', '0x8057E800:4', 'f32', 'mission timer, seconds left'),
    Probe('game.paused', '0x805EF27C:1', 'u8', 'pause flag'),
    Probe('game.rng_state', '0x805E1894:2504', 'sha', 'MT19937 state (624 words + index): the random stream'),
    Probe('player1.handle', '0x805EF630:4', 'hex', 'P1 object handle'),
    Probe('player1.rings', '0x805777CC:4', 's32', 'GameState P1 record +0x28'),
    Probe('player1.score', '0x805777D0:4', 's32', 'GameState P1 record +0x2C'),
    Probe('player1.hero_score', '0x805777D4:4', 's32', 'GameState P1 record +0x30'),
    Probe('player1.dark_score', '0x805777D8:4', 's32', 'GameState P1 record +0x34'),
    Probe('player1.record', '0x805777A4:0x4C', 'sha', 'the whole P1 record'),
    # Player::PlayerShadow, through the Player layer: TaskManager (0x80571C6C) layer 13 -> its "Shadow" task -> the
    # first component's next (ShadowExecuter) -> +0x28 the player (semantics.py; its vptr at +0x38 is 0x80522D70).
    Probe('player1.class', '0x80571CA4>0x14>0x14>0xC>0x28>0x38:4', 'hex', 'the player object vptr (80522d70)'),
    Probe('player1.position', '0x80571CA4>0x14>0x14>0xC>0x28>0x0:12', 'f32x3', 'Player::PlayerShadow +0x0'),
    Probe('player1.rotation', '0x80571CA4>0x14>0x14>0xC>0x28>0xC:12', 'f32x3', 'Euler radians (+0x10 heading)'),
    Probe('player1.velocity', '0x80571CA4>0x14>0x14>0xC>0x28>0x18:12', 'f32x3', 'Player::PlayerShadow +0x18'),
    Probe('checkpoint.reached', '0x8057707C:4', 's32', 'GameState +0: the highest checkpoint (savepoint) reached'),
    Probe('mission.slot', '0x80576FDC:4', 's32', 'MissionManager +0x20: 0 Dark, 1 Normal, 2 Hero'),
    Probe('karma.hero', '0x80577788:12', 'hex', 'hero gauge {cur, max, lock}'),
    Probe('karma.dark', '0x80577794:12', 'hex', 'dark gauge {cur, max, lock}'),
    Probe('mission.manager', '0x80576FBC:0x40', 'sha', 'MissionManager block'),
    Probe('enemies.defeated', '0x80580C0C:12', 'hex', 'EnemyManager +0x34: defeated per team (GUN, Eggman, Black Arms)'),
    Probe('enemies.defeated_by_player', '0x80580C18:4', 'u32', 'EnemyManager +0x40'),
    Probe('camera.hfov', '0x8056FF40:4', 'f32', 'live horizontal FOV, radians (console 1.2592; stuck 1.1039)'),
    Probe('camera.vfov', '0x8056FF44:4', 'f32', 'live vertical FOV, radians'),
    Probe('camera.view_window', '0x8056FF4C:8', 'f32x2', 'RwCamera view window (tan of half angles)'),
    Probe('camera.right', '0x8056FF1C>0x4>0x10:12', 'f32x3', 'camera frame modelling matrix: right'),
    Probe('camera.up', '0x8056FF1C>0x4>0x20:12', 'f32x3', 'camera frame modelling matrix: up'),
    Probe('camera.at', '0x8056FF1C>0x4>0x30:12', 'f32x3', 'camera frame modelling matrix: at'),
    Probe('camera.eye', '0x8056FF1C>0x4>0x40:12', 'f32x3', 'camera frame modelling matrix: position'),
)


def peek_list() -> str:
    return ','.join(p.range for p in PROBES)


def decode(probe: Probe, hex_text):
    """The probe's value from the report's hex (None when the range was not readable)."""
    if hex_text is None:
        return None
    data = bytes.fromhex(hex_text)
    k = probe.kind
    if k == 's32':
        return struct.unpack('>i', data[:4])[0]
    if k == 'u32':
        return struct.unpack('>I', data[:4])[0]
    if k == 'u8':
        return data[0]
    if k == 'f32':
        return struct.unpack('>f', data[:4])[0]
    if k in ('f32x2', 'f32x3'):
        n = 2 if k == 'f32x2' else 3
        return tuple(struct.unpack('>%df' % n, data[:4 * n]))
    if k == 'sha':
        return hashlib.sha256(data).hexdigest()[:16]
    return data.hex()


def format_value(value) -> str:
    if value is None:
        return '-'
    if isinstance(value, float):
        return f'{value:.6g}'
    if isinstance(value, tuple):
        return '(' + ', '.join(f'{v:.6g}' for v in value) + ')'
    return str(value)


def decode_report(report: dict) -> dict:
    """{probe name: value} from a state report (engine JSON)."""
    peeks = report.get('peeks', {}) if report else {}
    return {p.name: decode(p, peeks.get(p.range)) for p in PROBES}


def same(a, b) -> bool:
    return a == b


def compare_reports(before: dict, after: dict):
    """Rows (name, before, after, equal, note) for the state table, clocks first, then the probes, then memory."""
    rows = []
    for key in ('game_ticks', 'boundary_tick', 'vi_frames', 'render_frames'):
        a = before.get(key) if before else None
        b = after.get(key) if after else None
        note = 'process counter (frames since this process started)' if key == 'render_frames' else ''
        rows.append((key, a, b, a == b if key != 'render_frames' else None, note))
    va, vb = decode_report(before), decode_report(after)
    for p in PROBES:
        rows.append((p.name, va[p.name], vb[p.name], same(va[p.name], vb[p.name]), p.note))
    if before and after and 'memory' in before and 'memory' in after:
        differing, total, where = memory_difference(before['memory'], after['memory'])
        rows.append(('memory.blocks_differing', f'{differing} of {total}', '', differing == 0,
                     ('64 KiB blocks of the address window; ' + ', '.join(where[:8])) if where else
                     '64 KiB blocks of the address window'))
    return rows


def memory_difference(a: dict, b: dict):
    differing = total = 0
    where = []
    regions_b = {r['name']: r for r in b.get('regions', [])}
    for ra in a.get('regions', []):
        rb = regions_b.get(ra['name'])
        if rb is None:
            continue
        for i, (x, y) in enumerate(zip(ra['blocks'], rb['blocks'])):
            total += 1
            if x != y:
                differing += 1
                where.append(f"{ra['name']}+0x{i * a.get('block_bytes', 65536):X}")
    return differing, total, where
