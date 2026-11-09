"""What the game is: every function of PAL main.dol assigned to a subsystem, with the evidence kind.

    python -B -m dolrecon.game_map <dolrecon root> <callgraph.json> <out game_map.json>

Moved here from source/.build/dolrecon/gameplay_scope/game_map.py (sha256 3ae17cab..., 2026-11-02 07:50) unchanged in
its rules, so other tools (player census) can import the assignment instead of copying it. The call graph is an input:
the scratch script read pal-v30/callgraph.json, written before dolrecon.callgraph stopped dropping 417 edges.

Evidence, strongest first:
  class   - the function's class (skeleton/functions.csv) matches a subsystem rule by its name or a flattened RTTI base
  unit    - no class; the nearest classed functions before and after it in link order (one translation unit is
            contiguous) are of the same subsystem, with no other subsystem between (at most UNIT_SPAN apart)
  callers - no class; every caller is of one subsystem (to a fixed point)
  library - a library layer (spine): RenderWare, CRI, SDK, MSL, runtime, Boost; split by name prefix
  unknown - none of the above
"""
from __future__ import annotations

import bisect
import collections
import csv
import json
import pathlib
import re
import sys

PLAYER = "player: Shadow (behaviours, control, motion, weapons in hand)"

# (subsystem, pattern on the class name or any base) - first match wins; order matters
RULES = [
    (PLAYER, r"^(Player::(?!Npc)|@unnamed@Player|ShadowWeapon|ControlGun)"),
    ("front end: logo, title, mode select, story library, options, 2P battle select, synopsis, credits", r"^(AdvReal::|AdvReal$|@unnamed@Adv)"),
    ("partners / NPCs (Npc, Chao)", r"^(Player::Npc|@unnamed@Npc|Npc|Chao|@unnamed@Chao)"),
    ("weapons (guns, bombs, special weapons, pickups)", r"^(Weapon::|@unnamed@Weapon|@unnamed@ScatteringWeapon|SetWeapon|ThrowBomb|Bomb(Server)?$|Bomb::|Rocket|AttackObject|ThrowObject)"),
    ("vehicles", r"^(Vehicle::|TVehicle|@unnamed@Vehicle)"),
    ("bosses", r"^(BossCtrlUnit|Boss::|BossRootTask|BossVarParam|BossCharInfo|BlackDoom|DevilDoom|BlackBull|HeavyDog|EggMeca|EggLastMech|EggDealer|EggBreaker|EggShadowAndroid|Diablon|@unnamed@Boss)"),
    ("enemies (AI states, common components, enemy types)", r"^(T?Enemy|EnemyAIState|TEnemyAIState|FlyerCommon|AttackCallbackContainer|Gun[A-Z]|G?Bk[A-Z]|BK[A-Z]|BA[A-Z]|Egg(Pawn|Pierrot|Bomber|Cannon|Balloon|Searcher|Hawk|Gunner)|@unnamed@Enemy)"),
    ("missions, stage flow, results", r"^(Mission::|@unnamed@Mission|StageAction::|StageSequence::|GoalRing|ResultTask|MissionClear|DebugMissionClear|StageManager|TStageInitalizeTask|StageRouteDisplay|SavePoint|StageMovie|StageStaffroll|GameSystem)"),
    ("camera", r"(Camera|CameraMode)"),
    ("collision, ranges, physics", r"(CharaColli|::Range::|LightColli|Colli)"),
    ("effects", r"^(Effect::|@unnamed@.*Effect|.*Effect[A-Z0-9]|Particle|Shimmer)"),
    ("sound", r"(Audio|Sound|^SE[A-Z]|SeManager|gcax|TObjBGM|BGM|^SetSe|TObjSERIF)"),
    ("rendering and stage environment (fog, light, sky, background)", r"^(TFog|TLight|TSky|Sky|Fog|TObjBG|Lens|Shadow(Map|Volume))"),
    ("in-game events (scripted scenes, fades, triggers)", r"^(TPjsEvent|StageTriggerEvent|TEvent|Event[A-Z])"),
    ("video / movies", r"(Movie|Sofdec|Sfd)"),
    ("menus, HUD, 2D (CSD), text", r"^(Gi[A-Z]|TGi|csd|Csd|TPjsCsd|TMessage|Menu|Title|Option|Pause|Select|Font|Hud|TExpand|Display|Staffroll|Message|TBG|Wallpaper|Ranking|Library|Lyric|Subtitle)"),
    ("save data / memory card", r"(Save|Card)"),
    ("resources, loading, stage data", r"(Resource|Land[A-Z]|Loader|Load[A-Z]|NowLoading|SetItem|SetObject|LandManager)"),
    ("mini games", r"(ShootingGame|MiniGame)"),
    ("stage data, object placement (set data)", r"^(Set(Generator|Control|Adapter)|SetGenerator::|SetControl::|SetAdapter)"),
    ("motion / animation", r"^(Motion|MotionSequence|MotionCallback|Anim)"),
    ("stage objects (gadgets: rings, springs, doors, traps, switches)", r"^(Vacuum|Balloon::Vacuum|GadgetCrash3D|TornadoTowing|ScatteringRing|WarpHole|Parachute|Vine|Gong|HealingUnit|@unnamed@VacuumRing|FallingStone|StonePole|EscapePlane|ContainerBehavior|GadgetModelInfo|GadgetParticle|RegularRing|.*SetState$|.*Data$|CityGadgetState|Target$|TargetList|LineTarget|PathObject|StageBase::)"),
    (PLAYER, r"^(TargetSearchList|HandleUser)"),
    ("stage objects (gadgets: rings, springs, doors, traps, switches)", r"^(Gadget|GadgetCharInfo)$"),
    ("game framework (tasks, commands, character info, input)", r"^(Task|CharCommand|CharInfo|.*Command|CharCommandUnit|SonicteamUSA::System::|Sonicteam|Vibration|Input|Pad)"),
    ("Boost (shared_ptr and other templates)", r"^boost::"),
]
RULES = [(s, re.compile(p)) for s, p in RULES]
GENERIC_BASES = {"Task", "boost::detail::sp_counted_base"}

SDK_PREFIX = [
    ("SDK: OS (threads, alarms, interrupts, memory, reset)", r"^(OS|__OS|PPC|__PPC|EXI|__EXI|SI|__SI|DB|__DB)"),
    ("SDK: DVD (disc)", r"^(DVD|__DVD|dvd)"),
    ("SDK: GX / VI (graphics, video out)", r"^(GX|__GX|VI|__VI|GD)"),
    ("SDK: PAD (controllers)", r"^(PAD|__PAD|Pad)"),
    ("SDK: audio (AX, AI, AR, DSP, ARQ)", r"^(AX|__AX|AI|__AI|AR|__AR|ARQ|DSP|__DSP|MIX|SYN|SND|sp)"),
    ("SDK: CARD (memory card)", r"^(CARD|__CARD)"),
    ("SDK: MTX / math", r"^(MTX|PSMTX|PSVEC|C_MTX|VEC|PSQUAT|QUAT|C_VEC)"),
    ("SDK: debugger (TRK)", r"^(TRK|__TRK|trk|nub|ddh|gdev|udp|ser|InitMetroTRK|__init_hardware)"),
]
CRI_PREFIX = [
    ("CRI: ADX (sound streams)", r"(?i)^(adx|ADXT|ADXF|ADXB|ADXSJE|ADXPD|adxt|adxf|adxb|adxs|adxpd|adxsje|xefic)"),
    ("CRI: Sofdec (movies)", r"(?i)^(sfd|mwsfd|mwPly|mfci|sfx|sfh|sfbuf|sftim|sfmps|sfset|mpv|MPV|sud|mwl)"),
]
SDK_PREFIX = [(s, re.compile(p)) for s, p in SDK_PREFIX]
CRI_PREFIX = [(s, re.compile(p)) for s, p in CRI_PREFIX]
LIB = {"rw": "RenderWare 3.7 (core, world, toolkits, GameCube driver)", "msl": "MSL (C / C++ library)",
       "runtime": "CodeWarrior runtime", "boost": "Boost (shared_ptr and other templates)"}
GAME_LAYERS = ("game", "game-unit", "unresolved")
UNIT_SPAN = 0x2000   # bytes between the two classed (or named) neighbours: 8 KB


def load_functions(root: pathlib.Path) -> dict[int, dict]:
    rows = csv.DictReader(open(root / "skeleton/functions.csv", encoding="utf-8"))
    return {int(r["address"], 16): r for r in rows}


def load_bases(root: pathlib.Path) -> dict[str, list[str]]:
    bases = {}
    for h in (root / "skeleton/classes").rglob("*.h"):
        t = h.read_text(encoding="utf-8", errors="replace")
        name = t.splitlines()[0][3:].strip()
        m = re.search(r"base subobjects \(RTTI, flattened\): (.*)", t)
        bases[name] = [b.split(" +")[0].strip() for b in m.group(1).split(", ")] if m else []
    return bases


def classify(fn: dict[int, dict], bases: dict[str, list[str]], owners: dict[int, str], edges) -> tuple[dict, dict]:
    """(subsystem by address, evidence by address). `owners`: the proven owner class of a method body
    (dolrecon.receivers, owners.json); `edges`: call graph edges with "from", "to", "kind" (function names)."""
    starts = sorted(fn)

    def subsystem_of_class(c):
        for s, p in RULES:
            if p.search(c):
                return s
        for s, p in RULES:   # then by a base (not the universal roots)
            if any(p.search(b) for b in bases.get(c, []) if b not in GENERIC_BASES):
                return s
        if "Task" in bases.get(c, []):
            return "other in-game tasks (Task-derived, no subsystem rule yet)"
        return None

    sub, how = {}, {}
    for a, r in fn.items():
        layer = r["layer"]
        if layer in GAME_LAYERS:
            for c in [owners[a]] if a in owners else (r["class"].split(" | ") if r["class"] else []):
                s = subsystem_of_class(c)
                if s:
                    sub[a], how[a] = s, "class"
                    break
        elif layer in LIB:
            sub[a], how[a] = LIB[layer], "library"
        elif layer in ("sdk", "cri"):
            table = SDK_PREFIX if layer == "sdk" else CRI_PREFIX
            other = "SDK: other" if layer == "sdk" else "CRI: other (file system, memory, common)"
            sub[a] = next((s for s, p in table if p.search(r["name"])), other)
            how[a] = "library"

    for layer, other in (("sdk", "SDK: other"), ("cri", "CRI: other (file system, memory, common)")):
        lib = [a for a in starts if fn[a]["layer"] == layer]
        named = [a for a in lib if sub[a] != other]
        for a in lib:
            if sub[a] != other:
                continue
            i = bisect.bisect_left(named, a)
            if 0 < i < len(named) and sub[named[i - 1]] == sub[named[i]] and named[i] - named[i - 1] <= UNIT_SPAN:
                sub[a], how[a] = sub[named[i]], "library"
    library_subsystems = {s for a, s in sub.items() if how[a] == "library"}

    game_addrs = [a for a in starts if fn[a]["layer"] in GAME_LAYERS]
    classed = [a for a in game_addrs if how.get(a) == "class"]
    for a in game_addrs:
        if a in sub:
            continue
        i = bisect.bisect_left(classed, a)
        if 0 < i < len(classed) and sub[classed[i - 1]] == sub[classed[i]] and classed[i] - classed[i - 1] <= UNIT_SPAN:
            sub[a], how[a] = sub[classed[i]], "unit"

    n2a = {r["name"]: a for a, r in fn.items()}
    callers = collections.defaultdict(set)
    indirect = collections.defaultdict(set)   # callers known only through a non-direct edge
    for e in edges:
        s, t = n2a.get(e["from"]), n2a.get(e["to"])
        if s is None or t is None:
            continue
        callers[t].add(s)
        if e["kind"] not in ("call", "tail"):
            indirect[t].add(s)
    changed = True
    while changed:
        changed = False
        for a in game_addrs:
            if a in sub:
                continue
            cs = {sub.get(c) for c in callers.get(a, ())}
            if len(cs) == 1 and None not in cs:
                s = cs.pop()
                direct = callers[a] - indirect[a]
                if s in library_subsystems:   # game code that only a library calls: a callback handed to it
                    s = "game callbacks called by a library (" + s.split(" (")[0].split(":")[0] + ")"
                sub[a], how[a] = s, "callers" if direct else "callers (data / address / virtual)"
                changed = True
    for a in fn:
        if a in sub:
            continue
        cs = {sub.get(c) for c in callers.get(a, ())} - {None}
        if len(cs) >= 2:
            sub[a], how[a] = "shared game code (called from several subsystems)", "callers"
        elif not callers.get(a):
            sub[a], how[a] = "no caller found (dead code, or reached through pointers not seen yet)", "unknown"
        else:
            sub[a], how[a] = "unknown (" + fn[a]["layer"] + ")", "unknown"
    return sub, how


def load_owners(path: pathlib.Path) -> dict[int, str]:
    return {int(k, 16): v for k, v in json.load(open(path, encoding="utf-8")).items()}


def main(argv=None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    root, cg_path, out_path = pathlib.Path(argv[0]), argv[1], argv[2]
    fn = load_functions(root)
    owners = load_owners(root / "gameplay_scope/owners.json")
    sub, how = classify(fn, load_bases(root), owners, json.load(open(cg_path, encoding="utf-8"))["edges"])
    table = collections.defaultdict(lambda: {"functions": 0, "bytes": 0, "by": collections.Counter()})
    for a, s in sub.items():
        t = table[s]
        t["functions"] += 1
        t["bytes"] += int(fn[a]["size"])
        t["by"][how[a]] += 1
    out = {s: {"functions": t["functions"], "bytes": t["bytes"], "by_evidence": dict(t["by"])} for s, t in table.items()}
    json.dump(out, open(out_path, "w", encoding="utf-8"), indent=1)
    for s, t in sorted(table.items(), key=lambda x: -x[1]["bytes"]):
        print(f"{t['bytes']:9d} {t['functions']:6d}  {s}  [{', '.join(f'{k} {v}' for k, v in t['by'].most_common())}]")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
