#!/usr/bin/env python3
"""The blinking corridor lights of a stage against the painted slots of their walls: measure, and generate the fix.

The corridor walls (texture *_zn013_mp) are plain concrete with a black slot painted along the bottom edge of every
texture tile: u 0.25..0.75, v 0.875..0.9375 (the rows below are a full black band). The blinking light of a slot is
a separate quad in the stage's glare worlds (*_G_NN.RG1, texture pattern *_zn055.1..8), laid over the slot by hand.
In GUN Fortress (stg0600) 310 of the 568 lights fit their slot, 258 are off along the wall by up to a fifth of a
panel (12 units), and 8 slots have no light at all (docs/RENDER_FLICKER.md "Corridor lights").

    world_dump <game files> <dump folder> stg0600            (tools/stage_preview/world_dump.cpp)
    python tools/assets/light_slots.py <dump folder>/stg0600.obj [--emit runtime/lift/assets/light_slots_table.inc]

The fix moves every vertex of a misplaced light so its left / right edges land on the slot's (its own height and its
distance from the wall kept), and adds a copy of the nearest light of the same wall side to every empty slot. Lights
and slots are matched one to one, nearest first. Positions are the disc's floats, so the runtime finds them exactly.
"""
from __future__ import annotations

import argparse
import re
import struct
import sys
from pathlib import Path

import numpy as np

SLOT_U = (0.25, 0.75)
SLOT_V = 0.906          # the slot's middle row
EXACT = 0.01            # a light within this of its slot (in panel widths) is left alone


def load_stage(path: Path):
    positions, uvs, groups, obj = [], [], [], None
    for line in path.read_text().splitlines():
        t = line.split()
        if not t:
            continue
        if t[0] == 'o':
            obj = t[1]
        elif t[0] == 'v':
            positions.append([float(x) for x in t[1:4]])
        elif t[0] == 'vt':
            uvs.append([float(x) for x in t[1:3]])
        elif t[0] == 'g':
            groups.append([obj, t[2], []])
        elif t[0] == 'f':
            groups[-1][2].append([int(x) - 1 for x in t[1:4]])
    return np.array(positions), np.array(uvs), groups


def components(faces):
    parent = {}

    def find(a):
        while parent.setdefault(a, a) != a:
            a = parent[a]
        return a
    for f in faces:
        for v in f[1:]:
            parent[find(v)] = find(f[0])
    out = {}
    for i, f in enumerate(faces):
        out.setdefault(find(f[0]), []).append(i)
    return list(out.values())


class Wall:
    """One wall triangle: world <-> texture coordinates on its plane."""

    def __init__(self, T, UV):
        self.T, self.UV = T, UV
        n = np.cross(T[1] - T[0], T[2] - T[0])
        self.n = n / np.linalg.norm(n)
        A = np.array([[UV[1, 0] - UV[0, 0], UV[2, 0] - UV[0, 0]], [UV[1, 1] - UV[0, 1], UV[2, 1] - UV[0, 1]]])
        self.ok = abs(np.linalg.det(A)) > 1e-12
        self.Ai = np.linalg.inv(A) if self.ok else None
        self.M = np.column_stack([T[1] - T[0], T[2] - T[0]])

    def world(self, u, v):
        b = self.Ai @ (np.array([u, v]) - self.UV[0])
        return self.T[0] + b[0] * (self.T[1] - self.T[0]) + b[1] * (self.T[2] - self.T[0])

    def bary(self, p):
        return np.linalg.lstsq(self.M, p - self.T[0], rcond=None)[0]

    def uv(self, p):
        b = self.bary(p)
        return self.UV[0] + b[0] * (self.UV[1] - self.UV[0]) + b[1] * (self.UV[2] - self.UV[0])

    def depth(self, p):
        return (p - self.T[0]) @ self.n


def analyse(path: Path):
    V, VT, groups = load_stage(path)
    walls, lights = [], []
    for obj, texture, faces in groups:
        faces = np.array(faces)
        if texture.lower().endswith('zn013_mp'):
            for f in faces:
                w = Wall(V[f], VT[f])
                if w.ok and abs(w.n[1]) < 0.1:
                    walls.append(w)
        elif re.search(r'_zn055\.1$', texture.lower()):
            for comp in components(faces):
                tris = faces[comp]
                verts = np.unique(tris.ravel())
                lights.append(dict(obj=obj, tris=tris, verts=verts, P=V[verts]))
    slots = []
    for w in walls:
        for ku in range(int(np.floor(w.UV[:, 0].min())), int(np.ceil(w.UV[:, 0].max()))):
            for kv in range(int(np.floor(w.UV[:, 1].min())), int(np.ceil(w.UV[:, 1].max()))):
                p = w.world(ku + 0.5, kv + SLOT_V)
                b = w.bary(p)
                if b.min() < -1e-4 or b.sum() > 1 + 1e-4:
                    continue
                if all(np.linalg.norm(p - s['p']) > 2 for s in slots):
                    slots.append(dict(p=p, wall=w, tile=np.array([ku, kv], dtype=float)))
    pairs = []
    for i, s in enumerate(slots):
        for j, L in enumerate(lights):
            c = L['P'].mean(0)
            dist = np.linalg.norm((s['p'] - c)[[0, 2]])
            if dist < 40 and abs(s['p'][1] - c[1]) < 5:
                pairs.append((dist, i, j))
    pairs.sort()
    slot_light, light_slot = {}, {}
    for dist, i, j in pairs:
        if i in slot_light or j in light_slot:
            continue
        slot_light[i], light_slot[j] = j, i
    return V, VT, lights, slots, slot_light


def fitted(slot, P, u_range):
    """Positions of a light's vertices `P` moved so its u range `u_range` covers the slot's."""
    w, tile = slot['wall'], slot['tile']
    u0, u1 = u_range
    out = []
    for p in P:
        u, v = w.uv(p) - tile
        u_new = SLOT_U[0] + (u - u0) / (u1 - u0) * (SLOT_U[1] - SLOT_U[0])
        out.append(w.world(tile[0] + u_new, tile[1] + v) + w.depth(p) * w.n)
    return np.array(out, dtype=np.float32)


def same_wall(a, b):
    """Slots whose walls are one plane."""
    wa, wb = a['wall'], b['wall']
    return wa.n @ wb.n > 0.999 and abs((b['p'] - a['p']) @ wa.n) < 0.01


def plan(path: Path):
    V, VT, lights, slots, slot_light = analyse(path)
    moves, adds = [], []
    report = dict(lights=len(lights), slots=len(slots), exact=0, moved=0, raised=0, added=0)
    # How far in front of its wall each light lies (its nearest vertex). A light at or behind its wall is hidden by
    # the wall's own depth, wholly or in part (the glare sources test the scene's depth); the levels' other lights all
    # lie clear of their walls, and a faulty one takes the offset its sound neighbours on the same wall have.
    offset = {}
    for i, s in enumerate(slots):
        if i in slot_light:
            offset[i] = min(s['wall'].depth(p) for p in lights[slot_light[i]]['P'])
    sound = [i for i in offset if offset[i] > 0]

    def sibling_offset(i):
        same = [offset[k] for k in sound if same_wall(slots[i], slots[k])]
        if same:
            return float(np.median(same))
        k = min((k for k in sound if slots[k]['wall'].n @ slots[i]['wall'].n > 0.99),
                key=lambda k: np.linalg.norm(slots[k]['p'] - slots[i]['p']))
        return offset[k]

    for i, s in enumerate(slots):
        if i not in slot_light:
            continue
        L = lights[slot_light[i]]
        uv = np.array([s['wall'].uv(p) for p in L['P']]) - s['tile']
        u_range = (uv[:, 0].min(), uv[:, 0].max())
        off_slot = max(abs(u_range[0] - SLOT_U[0]), abs(u_range[1] - SLOT_U[1])) > EXACT
        behind = offset[i] <= 0
        if not off_slot and not behind:
            report['exact'] += 1
            continue
        report['moved'] += off_slot
        report['raised'] += behind
        target = fitted(s, L['P'], u_range) if off_slot else L['P'].astype(np.float32)
        if behind:
            # Flat on the wall's plane at its siblings' offset (a light crossing its wall is levelled with it).
            d = sibling_offset(i)
            target = np.array([q - (s['wall'].depth(q) - d) * s['wall'].n for q in target], dtype=np.float32)
        for p, q in zip(L['P'], target):
            moves.append((p.astype(np.float32), q))
    for i, s in enumerate(slots):
        if i in slot_light:
            continue
        # the nearest soundly lit slot on a wall facing the same way: its light, laid into this slot
        best = None
        for k, t in enumerate(slots):
            if k not in slot_light or offset[k] <= 0 or t['wall'].n @ s['wall'].n < 0.99:
                continue
            d = np.linalg.norm(t['p'] - s['p'])
            if best is None or d < best[0]:
                best = (d, k)
        t = slots[best[1]]
        L = lights[slot_light[best[1]]]
        uv = np.array([t['wall'].uv(p) for p in L['P']]) - t['tile']
        u_range = (uv[:, 0].min(), uv[:, 0].max())
        # the template's shape in its own slot (fitted), carried over to this slot
        local = [(t['wall'].uv(p) - t['tile'], t['wall'].depth(p)) for p in L['P']]
        new = []
        for (u, v), d in local:
            u_new = SLOT_U[0] + (u - u_range[0]) / (u_range[1] - u_range[0]) * (SLOT_U[1] - SLOT_U[0])
            new.append(s['wall'].world(s['tile'][0] + u_new, s['tile'][1] + v) + d * s['wall'].n)
        index = {int(v): n for n, v in enumerate(L['verts'])}
        tris = [[index[int(a)] for a in tri] for tri in L['tris']]
        adds.append((L['P'].astype(np.float32), np.array(new, dtype=np.float32), tris, s['p']))
        report['added'] += 1
    return moves, adds, report


def f32(x: float) -> str:
    return repr(float(np.float32(x))) + 'f'


def emit(moves, adds, stage: str) -> str:
    lines = [f'// Generated by tools/assets/light_slots.py from the disc ({stage}) - do not edit by hand.',
             f'// {len(moves)} vertex moves, {len(adds)} added lights.']
    lines.append('constexpr LightSlotMove kLightSlotMoves[] = {')
    for p, q in moves:
        lines.append('    {{%s, %s, %s}, {%s, %s, %s}},' % (*map(f32, p), *map(f32, q)))
    lines.append('};')
    lines.append('constexpr LightSlotAdd kLightSlotAdds[] = {')
    for template, new, tris, _ in adds:
        tv = ', '.join('{%s, %s, %s}' % tuple(map(f32, p)) for p in template)
        nv = ', '.join('{%s, %s, %s}' % tuple(map(f32, p)) for p in new)
        tr = ', '.join('{%d, %d, %d}' % tuple(t) for t in tris)
        lines.append('    {%d, {%s}, {%s}, %d, {%s}},' % (len(template), tv, nv, len(tris), tr))
    lines.append('};')
    return '\n'.join(lines) + '\n'


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('dump', type=Path, help='world_dump output of the stage (<stage>.obj)')
    ap.add_argument('--emit', type=Path, help='write the runtime table (C++ include)')
    args = ap.parse_args(argv)
    moves, adds, report = plan(args.dump)
    print(f"{args.dump.stem}: {report['lights']} lights, {report['slots']} slots; {report['exact']} fit, "
          f"{report['moved']} moved onto their slot, {report['raised']} brought in front of their wall "
          f"({len(moves)} vertices), {report['added']} added")
    for template, new, tris, at in adds:
        print('  added a light at', np.round(at, 1).tolist())
    if args.emit:
        args.emit.write_bytes(emit(moves, adds, args.dump.stem).encode())
        print('wrote', args.emit)
    return 0


if __name__ == '__main__':
    sys.exit(main())
