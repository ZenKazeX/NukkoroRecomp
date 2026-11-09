"""What a snapshot's moment IS, read out of its savestate - the workstation's understanding of the game's world
(docs/comparator/README.md "Snapshot semantics").

The game does not describe itself; this module knows where the game keeps what (from Shadow-The-Hedgehog-RE/gameplay:
GAMEPLAY_STRUCTURES.md, PLAYER_GAMEPLAY_RECOVERY.md, STAGE_GAMEPLAY_DATA_FORMATS.md, MISSION_SYSTEM.md,
CHECKPOINTS_AND_RESTART.md, GAMEPLAY_UPDATE_PIPELINE.md; the respawn table from fn_8016F7B8, read 2026-10-07):

    TaskManager      bss 0x80571C6C   +0 root task, +4+4i layer i (19 layers: ... 11 Gadget, 12 Vehicle, 13 Player,
                                      14 Enemy, 16 Particle, 18 Camera); Task {+0 name, +4 flags, +8 prev, +0xC next,
                                      +0x10 parent, +0x14 first child, +0x18 vptr}
    class names      a vptr points at its vtable's typeinfo pointer; typeinfo +0 is the class name (MWCC RTTI)
    player           Player::PlayerShadow (vptr 0x80522D70 at +0x38): +0 position, +0xC rotation (radians, +0x10
                     heading), +0x18 velocity, +0x54 player index; the Player layer's components hold it at +0x28
    SetManager       sbss 0x805EF77C -> SetData: +0 records, +8 alive objects, +0x14 the catalog (SetObjDesc**,
                     0x8052C1A0: +0 name, +0x18 id, +0x1A category 1 gadget / 2 enemy / 4 character), +0x18 slots
                     (0x20 each, +0xC the live object), +0x41C records (0x2C each: +0 position, +0xC rotation in
                     degrees, +0x18 runtime flags, +0x20 type id, +0x28 parameters - a SAVEPOINT's first is its
                     priority, the checkpoint's number)
    GameState        bss 0x8057707C   +0 the highest checkpoint reached; +0xC + 0x1C*n the respawn point of
                                      checkpoint n (position, rotation, valid byte at +0x18)
    MissionManager   bss 0x80576FBC   +0x20 the current mission slot (0 Dark, 1 Normal, 2 Hero); +8+8*slot the
                                      mission (+0xC state: 0 running, 1 cleared, 2 failed; +0x14 accepted)
    stage            0x8057E808 the stage number (100 = Westopolis), 0x8057E80C its record index
    camera           holder 0x8056FF1C: +4 its frame, whose modelling matrix rows are at +0x10 right, +0x20 up,
                     +0x30 at (the view direction), +0x40 position; 0x8056FF40 / 44 the horizontal / vertical FOV
    random generator 0x805E1894: the MT19937 state (624 words + index)

A lifted subsystem will keep its state natively; its reader here changes then, nowhere else.
"""
from __future__ import annotations

import hashlib
import math
import re

from .statefile import GuestImage

TASK_MANAGER = 0x80571C6C
SET_MANAGER = 0x805EF77C
GAME_STATE = 0x8057707C
MISSION_MANAGER = 0x80576FBC
STAGE_NUMBER = 0x8057E808
STAGE_INDEX = 0x8057E80C
GAME_FRAME_COUNTER = 0x8057E7F0
TIME_REMAINING = 0x8057E800
P1_RECORD = 0x805777A4
CAMERA_HOLDER = 0x8056FF1C
CAMERA_HFOV = 0x8056FF40
CAMERA_VFOV = 0x8056FF44
RNG_STATE = 0x805E1894
RNG_BYTES = 2504
PLAYER_VTABLE = 0x80522D70
SET_CATALOG = 0x8052C1A0

LAYERS = ('System', 'Audio', 'Debug', 'Scene', 'PostSystem', 'Render', 'Manager', 'Controller', 'Command',
          'CharaColli', 'Landscape', 'Gadget', 'Vehicle', 'Player', 'Enemy', 'PostManager', 'Particle', 'Editor',
          'Camera')
GAMEPLAY_LAYERS = ('Gadget', 'Vehicle', 'Player', 'Enemy')
MISSION_SLOTS = {0: 'Dark', 1: 'Normal', 2: 'Hero'}
MISSION_STATES = {0: 'running', 1: 'cleared', 2: 'failed'}
CATEGORY = {1: 'gadget', 2: 'enemy', 4: 'character'}
BOSS_IDS = range(0xB4, 0xBF)
SAVEPOINT_ID = 0x0005
ENCOUNTER_RADIUS = 1500.0      # world units: live enemies this close make the moment an "encounter"
AT_CHECKPOINT_RADIUS = 150.0

# The English stage titles (tools/launcher.py STAGE_TITLES: the disc has no id -> name table).
STAGE_TITLES = {
    'stg0100': 'Westopolis', 'stg0200': 'Digital Circuit', 'stg0201': 'Glyphic Canyon', 'stg0202': 'Lethal Highway',
    'stg0210': 'Black Bull (Lethal Highway)', 'stg0300': 'Cryptic Castle', 'stg0301': 'Prison Island',
    'stg0302': 'Circus Park', 'stg0310': 'Egg Breaker (Cryptic Castle)', 'stg0400': 'Central City',
    'stg0401': 'The Doom', 'stg0402': 'Sky Troops', 'stg0403': 'Mad Matrix', 'stg0404': 'Death Ruins',
    'stg0410': 'Heavy Dog', 'stg0411': 'Egg Breaker (Mad Matrix)', 'stg0412': 'Black Bull (Death Ruins)',
    'stg0500': 'The ARK', 'stg0501': 'Air Fleet', 'stg0502': 'Iron Jungle', 'stg0503': 'Space Gadget',
    'stg0504': 'Lost Impact', 'stg0510': 'Blue Falcon', 'stg0511': 'Egg Breaker (Iron Jungle)',
    'stg0600': 'GUN Fortress', 'stg0601': 'Black Comet', 'stg0602': 'Lava Shelter', 'stg0603': 'Cosmic Fall',
    'stg0604': 'Final Haunt', 'stg0610': 'Black Doom (GUN Fortress)', 'stg0611': 'Sonic & Diablon (GUN Fortress)',
    'stg0612': 'Egg Dealer (Black Comet)', 'stg0613': 'Sonic & Diablon (Black Comet)',
    'stg0614': 'Egg Dealer (Lava Shelter)', 'stg0615': 'Egg Dealer (Cosmic Fall)',
    'stg0616': 'Black Doom (Cosmic Fall)', 'stg0617': 'Black Doom (Final Haunt)',
    'stg0618': 'Sonic & Diablon (Final Haunt)', 'stg0700': 'The Last Way', 'stg0710': 'Devil Doom',
}


def stage_title(stage_id: str, stages=()) -> str:
    if stage_id in STAGE_TITLES:
        return STAGE_TITLES[stage_id]
    for stg, name, _bootable in stages or ():
        if stg == stage_id:
            return name
    return stage_id or ''


class World:
    """Readers over one savestate's memory."""

    def __init__(self, image: GuestImage):
        self.g = image
        self._class_cache = {}

    # ---- RTTI ----
    def class_name(self, vptr):
        if not vptr:
            return None
        if vptr in self._class_cache:
            return self._class_cache[vptr]
        g = self.g
        name = None
        ti = g.u32(vptr)
        if ti and g.region_of(ti):
            text = g.u32(ti)
            if text and g.region_of(text):
                name = g.cstr(text)
                if name is not None and not re.match(r'^[A-Za-z_@][\w:@<>,* ]*$', name):
                    name = None
        self._class_cache[vptr] = name
        return name

    # ---- the task tree ----
    def children(self, task):
        g = self.g
        first = g.u32(task + 0x14)
        child = first
        seen = set()
        while child and child not in seen and g.region_of(child):
            seen.add(child)
            yield child
            child = g.u32(child + 0x0C)
            if child == first:
                break

    def subtree(self, task, depth=0, limit=12, out=None):
        out = [] if out is None else out
        for child in self.children(task):
            out.append((depth, child))
            if depth < limit and len(out) < 20000:
                self.subtree(child, depth + 1, limit, out)
        return out

    def layer(self, name: str):
        index = LAYERS.index(name)
        return self.g.pointer(TASK_MANAGER + 4 + 4 * index)

    def task_class(self, task):
        return self.class_name(self.g.u32(task + 0x18))

    # ---- the catalog and the SET ----
    def catalog(self):
        """{type id: (name, category)} from the game's own SetObjDesc table."""
        g = self.g
        out = {}
        for i in range(1024):
            desc = g.u32(SET_CATALOG + 4 * i)
            if not desc:
                break
            name_ptr = g.u32(desc)
            name = g.cstr(name_ptr) if name_ptr and g.region_of(name_ptr) else None
            type_id = g.u16(desc + 0x18)
            category = g.u8(desc + 0x1A)
            if type_id is not None:
                out[type_id] = (name or f'type {type_id:04X}', CATEGORY.get(category, f'category {category}'))
        return out


def _vec(v, digits=3):
    return [round(float(x), digits) for x in v] if v is not None else None


def _finite(v):
    return v is not None and all(math.isfinite(x) for x in v)


def find_player(world: World):
    """P1's Player::PlayerShadow: from the Player layer's components (their +0x28), else by its vtable."""
    g = world.g
    candidates = []
    layer = world.layer('Player')
    if layer:
        for _depth, task in world.subtree(layer, limit=3):
            target = g.u32(task + 0x28)
            if target and g.region_of(target) and g.u32(target + 0x38) == PLAYER_VTABLE:
                candidates.append(target)
    if not candidates:
        candidates = [a - 0x38 for a in g.find_u32(PLAYER_VTABLE) if g.region_of(a - 0x38)
                      and not g.region_of(a).name == 'image']
    players = []
    for obj in dict.fromkeys(candidates):
        index = g.s32(obj + 0x54)
        players.append((0 if index == 0 else 1, obj, index))
    players.sort()
    return players[0][1] if players else None


def extract(image: GuestImage, stages=()) -> dict:
    """The snapshot's semantics (JSON-able)."""
    world = World(image)
    g = image
    out = {'format': 'shadow-snapshot-semantics 1', 'game_ticks': g.game_ticks, 'renderer_kind': g.renderer_kind}

    # ---- the stage ----
    number = g.s32(STAGE_NUMBER)
    in_stage = number is not None and number >= 0
    stage_id = f'stg{number:04d}' if in_stage else None
    out['stage'] = {'number': number, 'index': g.s32(STAGE_INDEX), 'id': stage_id,
                    'name': stage_title(stage_id, stages) if stage_id else 'menu / not in a stage'}

    # ---- the mission ----
    slot = g.s32(MISSION_MANAGER + 0x20)
    mission = {'slot': slot, 'name': MISSION_SLOTS.get(slot, f'slot {slot}')}
    pointer = g.pointer(MISSION_MANAGER + 0x08 + 8 * slot) if slot in MISSION_SLOTS else None
    if pointer:
        mission['state'] = MISSION_STATES.get(g.u32(pointer + 0x0C), g.u32(pointer + 0x0C))
        mission['accepted'] = bool(g.u8(pointer + 0x14))
    out['mission'] = mission

    # ---- the player ----
    player = find_player(world) if in_stage else None
    if player:
        pos, rot, vel = g.vec3(player), g.vec3(player + 0x0C), g.vec3(player + 0x18)
        out['player'] = {
            'address': f'0x{player:08X}', 'class': world.class_name(g.u32(player + 0x38)),
            'index': g.s32(player + 0x54), 'character': g.s32(player + 0x64),
            'position': _vec(pos), 'rotation_rad': _vec(rot, 5),
            'heading_deg': round(math.degrees(rot[1]), 2) if _finite(rot) else None,
            'velocity': _vec(vel), 'speed': round(math.sqrt(sum(v * v for v in vel)), 3) if _finite(vel) else None,
        }
    else:
        out['player'] = None

    # ---- the camera ----
    camera = {}
    frame = g.pointer(CAMERA_HOLDER)
    frame = g.pointer(frame + 0x4) if frame else None
    if frame:
        right, up, at, eye = (g.vec3(frame + o) for o in (0x10, 0x20, 0x30, 0x40))
        camera = {'eye': _vec(eye), 'forward': _vec(at, 5), 'up': _vec(up, 5), 'right': _vec(right, 5)}
        if _finite(at):
            camera['yaw_deg'] = round(math.degrees(math.atan2(at[0], at[2])), 2)
            camera['pitch_deg'] = round(math.degrees(math.asin(max(-1.0, min(1.0, at[1])))), 2)
    hfov, vfov = g.f32(CAMERA_HFOV), g.f32(CAMERA_VFOV)
    if hfov is not None and math.isfinite(hfov):
        camera['hfov_deg'] = round(math.degrees(hfov), 4)
    if vfov is not None and math.isfinite(vfov):
        camera['vfov_deg'] = round(math.degrees(vfov), 4)
    out['camera'] = camera or None

    # ---- the live objects (the task tree) ----
    by_layer, by_class, total = {}, {}, 0
    for name in GAMEPLAY_LAYERS + ('Particle',):
        layer = world.layer(name)
        if not layer:
            continue
        top = list(world.children(layer))
        everything = world.subtree(layer)
        by_layer[name] = {'objects': len(top), 'tasks': len(everything)}
        if name in GAMEPLAY_LAYERS:
            total += len(top)
            for task in top:
                cls = world.task_class(task) or '?'
                by_class[cls] = by_class.get(cls, 0) + 1
    out['entities'] = {'active': total, 'by_layer': by_layer,
                       'by_class': dict(sorted(by_class.items(), key=lambda kv: (-kv[1], kv[0]))[:24])}

    # ---- the stage's objects (SET), the checkpoints ----
    set_info, nearby, enemies, bosses, savepoints = {}, [], {}, [], []
    setdata = g.pointer(SET_MANAGER)
    if in_stage and setdata and g.u8(SET_MANAGER + 4):
        catalog = world.catalog()
        records = g.pointer(setdata + 0x41C)
        slots = g.pointer(setdata + 0x18)
        count = min(g.u32(setdata) or 0, 0x800)
        live_by_type = {}
        here = out['player']['position'] if out.get('player') else (camera.get('eye') if camera else None)
        for i in range(count):
            rec = records + 0x2C * i if records else None
            if rec is None:
                break
            pos = g.vec3(rec)
            type_id = g.u16(rec + 0x20)
            name, category = catalog.get(type_id, (f'type {type_id:04X}', '?'))
            live = bool(slots and g.u32(slots + 0x20 * i + 0x0C))
            if live:
                live_by_type[name] = live_by_type.get(name, 0) + 1
            distance = math.dist(pos, here) if (here and _finite(pos)) else None
            if type_id == SAVEPOINT_ID:
                params = g.pointer(rec + 0x28)
                savepoints.append({'record': i, 'number': g.s32(params) if params else None, 'position': _vec(pos, 1),
                                   'distance': round(distance, 1) if distance is not None else None})
            if type_id >= 0x2580 or name.startswith('type '):
                continue
            if category == 'enemy' and live:
                if distance is not None and distance <= ENCOUNTER_RADIUS:
                    enemies[name] = enemies.get(name, 0) + 1
                if type_id in BOSS_IDS:
                    bosses.append(name)
            if distance is not None:
                nearby.append((distance, i, name, category, live, pos))
        nearby.sort(key=lambda n: n[0])
        set_info = {'records': count, 'alive': g.u32(setdata + 8),
                    'live_by_type': dict(sorted(live_by_type.items(), key=lambda kv: (-kv[1], kv[0])))}
    out['set'] = set_info or None
    out['nearby'] = [{'type': name, 'record': i, 'category': category, 'live': live, 'distance': round(d, 1),
                      'position': _vec(pos, 1)} for d, i, name, category, live, pos in nearby[:10]]
    out['enemies_near'] = {'radius': ENCOUNTER_RADIUS, 'count': sum(enemies.values()), 'types': enemies}
    out['bosses'] = sorted(set(bosses))

    reached = g.s32(GAME_STATE) if in_stage else None
    checkpoint = {'reached': reached if reached is not None and 0 <= reached < 32 else None}
    if checkpoint['reached']:
        entry = GAME_STATE + 0x0C + 0x1C * checkpoint['reached']
        checkpoint['respawn'] = _vec(g.vec3(entry), 1)
    savepoints.sort(key=lambda s: (s['number'] if s['number'] is not None else 99, s['record']))
    checkpoint['savepoints'] = len(savepoints)
    closest = min((s for s in savepoints if s['distance'] is not None), key=lambda s: s['distance'], default=None)
    if closest:
        checkpoint['nearest'] = {'number': closest['number'], 'distance': closest['distance']}
    numbers = [s['number'] for s in savepoints if s['number'] is not None]
    if numbers and checkpoint['reached'] is not None:
        ahead = [n for n in numbers if n > checkpoint['reached']]
        checkpoint['next'] = min(ahead) if ahead else None
    out['checkpoint'] = checkpoint

    # ---- the game's counters, the random stream ----
    rng = g.read(RNG_STATE, RNG_BYTES)
    out['game'] = {'frame_counter': g.u32(GAME_FRAME_COUNTER), 'time_remaining': g.f32(TIME_REMAINING),
                   'rings': g.s32(P1_RECORD + 0x28), 'score': g.s32(P1_RECORD + 0x2C),
                   'rng_sha': hashlib.sha256(rng).hexdigest()[:16] if rng else None}

    # ---- what is happening ----
    out['event'] = describe_event(out)
    out['area'] = describe_area(out)
    return out


def describe_event(sem: dict):
    if sem.get('bosses'):
        return 'boss: ' + ', '.join(sem['bosses'])
    near = sem.get('enemies_near') or {}
    if near.get('count'):
        kinds = ', '.join(f'{n} {t}' for t, n in sorted(near['types'].items(), key=lambda kv: -kv[1]))
        return f'enemy encounter: {kinds}'
    cp = sem.get('checkpoint') or {}
    nearest = cp.get('nearest') or {}
    if nearest.get('distance') is not None and nearest['distance'] <= AT_CHECKPOINT_RADIUS:
        return f"at checkpoint {nearest.get('number')}"
    return None


def describe_area(sem: dict) -> str:
    stage = sem.get('stage') or {}
    if not stage.get('id'):
        return 'menu'
    cp = sem.get('checkpoint') or {}
    reached = cp.get('reached')
    nxt = cp.get('next')
    if not reached:
        text = 'from the start' + (f' to checkpoint {nxt}' if nxt else '')
    else:
        text = f'after checkpoint {reached}' + (f', before {nxt}' if nxt else '')
    notable = next((n for n in sem.get('nearby') or () if n['category'] in ('enemy', 'character')
                    or n['type'] in ('SAVEPOINT', 'GOALRING')), None)
    if notable is None and sem.get('nearby'):
        notable = sem['nearby'][0]
    if notable:
        text += f", near {notable['type']} #{notable['record']}"
    return text


# ---- labels ----

def _slug(text: str, limit: int = 24) -> str:
    text = re.sub(r'\(.*?\)', '', text or '')
    words = re.findall(r'[A-Za-z0-9]+', text)
    return ''.join(w[:1].upper() + w[1:] for w in words)[:limit] or 'Game'


def _coord(value) -> str:
    return f'{value:.0f}' if value is not None and math.isfinite(value) else 'na'


def labels(sem: dict, when, unique: str) -> dict:
    """{'name': the snapshot's folder name (unique, file-system safe), 'title': the readable label}."""
    stage = sem.get('stage') or {}
    mission = sem.get('mission') or {}
    cp = sem.get('checkpoint') or {}
    ticks = sem.get('game_ticks') or 0
    stamp = when.strftime('%Y%m%d-%H%M%S')
    parts = [_slug(stage.get('name') if stage.get('id') else 'Menu')]
    if stage.get('id'):
        if mission.get('name') and mission['name'] != 'Normal':
            parts.append(mission['name'])
        parts.append(f"CP{cp.get('reached') or 0}")
    parts.append(f'T{ticks}')
    pos = (sem.get('player') or {}).get('position') or (sem.get('camera') or {}).get('eye')
    if pos and stage.get('id'):
        parts += [f'X{_coord(pos[0])}', f'Y{_coord(pos[1])}', f'Z{_coord(pos[2])}']
    parts.append(stamp)
    name = re.sub(r'[^A-Za-z0-9_.-]', '', '_'.join(parts))[:96]
    title_parts = [stage.get('name') or 'menu']
    if stage.get('id'):
        title_parts.append(f"{mission.get('name', '?')} mission")
        title_parts.append(sem.get('area') or '')
        if sem.get('event'):
            title_parts.append(sem['event'])
    title_parts.append(f"{ticks / 40_500_000:.2f} s (tick {ticks})")
    title = ' / '.join(p for p in title_parts if p)
    return {'name': name, 'title': title, 'id': unique}


def log_lines(sem: dict, name: str, side_label: str) -> list:
    """The "[Snapshot]" lines the tool's log shows for a new snapshot."""
    stage = sem.get('stage') or {}
    lines = [f'[Snapshot] Capturing comparison state... ({side_label})']
    if stage.get('id'):
        mission = sem.get('mission') or {}
        cp = sem.get('checkpoint') or {}
        lines.append(f"[Snapshot] Stage: {stage.get('name')} ({stage.get('id')}), {mission.get('name')} mission"
                     f"{' (accepted)' if mission.get('accepted') else ''}, checkpoint {cp.get('reached') or 0}")
    else:
        lines.append('[Snapshot] Stage: none (a menu or a movie)')
    game = sem.get('game') or {}
    lines.append(f"[Snapshot] Tick: {sem.get('game_ticks')} ({(sem.get('game_ticks') or 0) / 40_500_000:.3f} s of "
                 f"game time; game frame {game.get('frame_counter')})")
    player = sem.get('player')
    if player:
        x, y, z = player['position']
        lines.append(f"[Snapshot] Player: ({x:.2f}, {y:.2f}, {z:.2f}) heading {player.get('heading_deg')} deg, speed "
                     f"{player.get('speed')}")
    camera = sem.get('camera') or {}
    if camera.get('eye'):
        lines.append(f"[Snapshot] Camera captured: eye {tuple(camera['eye'])}, yaw {camera.get('yaw_deg')} deg, "
                     f"pitch {camera.get('pitch_deg')} deg, hFOV {camera.get('hfov_deg')} deg")
    entities = sem.get('entities') or {}
    layers = ', '.join(f"{k} {v['objects']}" for k, v in (entities.get('by_layer') or {}).items())
    lines.append(f"[Snapshot] {entities.get('active', 0)} active entities captured ({layers})")
    if sem.get('event'):
        lines.append(f"[Snapshot] Event: {sem['event']}")
    if sem.get('area'):
        lines.append(f"[Snapshot] Area: {sem['area']}")
    lines.append(f"[Snapshot] RNG state captured (MT19937 sha {game.get('rng_sha')})")
    lines.append(f'[Snapshot] Saved as: {name}')
    return lines


def search_text(meta: dict) -> str:
    """Everything a library search matches on."""
    sem = meta.get('semantics') or {}
    stage = sem.get('stage') or {}
    bits = [meta.get('name', ''), meta.get('title', ''), meta.get('description', ''), stage.get('name') or '',
            stage.get('id') or '', (sem.get('mission') or {}).get('name') or '', sem.get('area') or '',
            sem.get('event') or '', ' '.join(n['type'] for n in sem.get('nearby') or ()),
            ' '.join((sem.get('entities') or {}).get('by_class', {}).keys()), meta.get('snapshot_id', ''),
            ' '.join(meta.get('tags', []))]
    return ' '.join(bits).lower()
