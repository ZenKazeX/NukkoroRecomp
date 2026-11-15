#!/usr/bin/env python3
"""Which game context reads which disc file (feeds the mod manager's "affects" line).

Three sub-commands, all stdlib, all read-only on the game data and the game executable:

  run    boot the stock executable headless once per context (stage, 2P arena, story, front end ...) with
         --asset-trace and keep every trace under <out>/<tag>/trace.jsonl plus <out>/runs.json.
  build  read those traces, the extracted disc tree, the guest stage table (--list-stages output) and the event
         table in main.dol, and write test_data/mods/file_contexts.json.
  check  compare installed mods (a folder of <id>/files, <id>/options/*/*/files, <id>/afs, <id>/textures) with
         the map: per mod, the contexts it touches and how many of its files the game never opens.

Evidence levels in the output ("source"): "observed" = the file was read in at least one traced run;
"derived" = not read in a run but its path or a game table names the context ("basis" says which);
"none" = no run and no rule covers it (the game probably never opens it).

    python map_file_contexts.py run   --exe game.exe --game-data <dir with sys/ and files/> --out <dir>
    python map_file_contexts.py build --game-data <dir> --runs <dir> --list-stages list_stages.txt --out file_contexts.json
    python map_file_contexts.py check --map file_contexts.json --mods <mods dir> [--json out.json]
"""
import argparse
import collections
import json
import os
import re
import struct
import subprocess
import sys

LANGS = ["EN", "FR", "GE", "IT", "JP", "SP"]

# Display names. The game stores its stage titles as pictures (csdFiles/stageTitle/StageTitleNNNN_XX.txd) and the
# message tables (fonts/stgNNNN/*.fnt) carry dialogue only, so no text table in the disc holds them. The names are the
# English stage titles as the repo's own launcher lists them (tools/launcher.py STAGE_TITLES, checked against the
# stage-title pictures by the author of that table); the guest stage table (--list-stages) gives the internal names.
STAGE_TITLES = {
    "stg0100": "Westopolis", "stg0200": "Digital Circuit", "stg0201": "Glyphic Canyon", "stg0202": "Lethal Highway",
    "stg0210": "Black Bull (Lethal Highway)", "stg0300": "Cryptic Castle", "stg0301": "Prison Island",
    "stg0302": "Circus Park", "stg0310": "Egg Breaker (Cryptic Castle)", "stg0400": "Central City",
    "stg0401": "The Doom", "stg0402": "Sky Troops", "stg0403": "Mad Matrix", "stg0404": "Death Ruins",
    "stg0410": "Heavy Dog", "stg0411": "Egg Breaker (Mad Matrix)", "stg0412": "Black Bull (Death Ruins)",
    "stg0500": "The ARK", "stg0501": "Air Fleet", "stg0502": "Iron Jungle", "stg0503": "Space Gadget",
    "stg0504": "Lost Impact", "stg0510": "Blue Falcon", "stg0511": "Egg Breaker (Iron Jungle)",
    "stg0600": "GUN Fortress", "stg0601": "Black Comet", "stg0602": "Lava Shelter", "stg0603": "Cosmic Fall",
    "stg0604": "Final Haunt", "stg0610": "Black Doom (GUN Fortress)", "stg0611": "Sonic & Diablon (GUN Fortress)",
    "stg0612": "Egg Dealer (Black Comet)", "stg0613": "Sonic & Diablon (Black Comet)",
    "stg0614": "Egg Dealer (Lava Shelter)", "stg0615": "Egg Dealer (Cosmic Fall)",
    "stg0616": "Black Doom (Cosmic Fall)", "stg0617": "Black Doom (Final Haunt)",
    "stg0618": "Sonic & Diablon (Final Haunt)", "stg0700": "The Last Way", "stg0710": "Devil Doom",
}

# Runs. (tag, boot arguments, game-millisecond budget, kind). kind decides how the trace is read in build().
STAGES_BOOT = ["0100", "0200", "0201", "0202", "0300", "0301", "0302", "0400", "0401", "0402", "0403", "0404",
               "0500", "0501", "0502", "0503", "0504", "0600", "0601", "0602", "0603", "0604", "0210", "0310",
               "0410", "0411", "0412", "0510", "0511", "0610", "0611", "0612", "0613", "0614", "0615", "0616",
               "0617", "0618"]
STAGES_STORY = ["0000", "0001", "0002", "0003", "0005", "0700", "0710", "0800", "0801", "0802", "0901", "0902",
                "0903", "0904", "0905", "0906", "0907", "0908", "0909", "0910", "0911", "6000", "8900", "9900",
                "9901"]


def run_table():
    runs = []
    for s in STAGES_BOOT:
        runs.append(("bs" + s, ["--boot-stage", "stg" + s], 30000, "stage", "stg" + s))
    for s in STAGES_STORY:
        runs.append(("ss" + s, ["--boot-story-stage", "stg" + s], 30000, "stage", "stg" + s))
    for s in ["0800", "0801", "0802"]:
        runs.append(("p2" + s, ["--boot-2p", "stg" + s], 60000, "route", "mode:2p-battle"))
    runs.append(("p2three", ["--boot-2p-three"], 60000, "route", "mode:2p-battle"))
    for s in ["0901", "0902", "0903", "0904", "0905", "0906", "0907", "0908", "0909", "0910", "0911"]:
        runs.append(("be" + s, ["--boot-event", "event" + s], 60000, "stage", "stg" + s))
    runs.append(("story", ["--boot-story"], 600000, "route", "mode:story"))
    runs.append(("laststory", ["--boot-last-story"], 600000, "route", "mode:last-story"))
    runs.append(("flashback", ["--boot-flashback"], 120000, "route", "mode:flashback"))
    runs.append(("expert0", ["--boot-expert", "0"], 60000, "route", "mode:expert"))
    runs.append(("fe_pal", [], 20000, "boot", "boot"))
    runs.append(("fe_title", ["--tv-format", "ntsc"], 60000, "boot", "boot"))
    runs.append(("fe_select", ["--ui-test", "select"], 120000, "frontend", "frontend"))
    return runs


def cmd_run(args):
    runs = run_table()
    if args.only:
        runs = [r for r in runs if r[0] in args.only.split(",")]
    os.makedirs(args.out, exist_ok=True)
    manifest = {}
    for tag, extra, budget, kind, ctx in runs:
        d = os.path.join(args.out, tag)
        os.makedirs(d, exist_ok=True)
        # A cold boot into a stage stops at the PAL 50/60 Hz screen unless the console settings hold 60 Hz;
        # --tv-format ntsc skips it (the only file that differs is palsel/PalSelEn.txd, read by the PAL screen:
        # run "fe_pal" has none). No memory card is given: the game starts without a save.
        tv = [] if (tag == "fe_pal" or "--tv-format" in extra) else ["--tv-format", "ntsc"]
        cmd = [args.exe, "--game-data", args.game_data] + extra + tv + [
            "--headless", "--no-audio", "--state-file", "unused.gekkostate", "--snapshot-key", "none",
            "--no-xinput", "--budget-ms", str(budget), "--width", "640", "--height", "480",
            "--asset-trace", "trace.jsonl"]
        with open(os.path.join(d, "stdout.log"), "wb") as out, open(os.path.join(d, "stderr.log"), "wb") as err:
            rc = subprocess.call(cmd, cwd=d, stdout=out, stderr=err, timeout=1800)
        manifest[tag] = {"args": extra, "budget_ms": budget, "kind": kind, "context": ctx, "exit": rc}
        print(tag, "exit", rc, flush=True)
    path = os.path.join(args.out, "runs.json")
    if os.path.exists(path) and args.only:
        old = json.load(open(path))
        old.update(manifest)
        manifest = old
    json.dump(manifest, open(path, "w"), indent=1)


# ---------------------------------------------------------------------------------------------------------------
# build
# ---------------------------------------------------------------------------------------------------------------

def read_traces(runs_dir):
    """tag -> ordered list of distinct disc paths (first-read order)."""
    manifest = json.load(open(os.path.join(runs_dir, "runs.json")))
    out = {}
    for tag in manifest:
        p = os.path.join(runs_dir, tag, "trace.jsonl")
        if not os.path.exists(p):
            continue
        seen = []
        have = set()
        for line in open(p, encoding="utf-8"):
            rec = json.loads(line)
            if rec.get("ev") == "file_read" and rec.get("ok", True):
                f = rec["file"]
                if f not in have:
                    have.add(f)
                    seen.append(f)
        out[tag] = seen
    return manifest, out


STAGE_DIR = re.compile(r"^(?:fonts/)?(stg\d{4})/")


def named_stage(path):
    m = STAGE_DIR.match(path)
    return m.group(1) if m else None


def truncate_at_foreign_stage(files, stage):
    """--boot-story-stage / --boot-event hand the stage to the story loop, which then goes on to the first story
    stage. Keep the reads up to the first file of a different stage directory."""
    out = []
    for f in files:
        s = named_stage(f)
        if s and s != stage:
            break
        out.append(f)
    return out


def parse_dol_events(dol_path):
    """The event table: 86 pointers at 0x8054B338, each to {event id, stage id} (two big-endian words)."""
    d = open(dol_path, "rb").read()
    off = struct.unpack(">18I", d[0:72])
    addr = struct.unpack(">18I", d[72:144])
    size = struct.unpack(">18I", d[144:216])

    def rd(a, n):
        for i in range(18):
            if size[i] and addr[i] <= a < addr[i] + size[i]:
                return d[off[i] + a - addr[i]: off[i] + a - addr[i] + n]
        raise ValueError(hex(a))

    pairs = []
    for i in range(0x56):
        ptr = struct.unpack(">I", rd(0x8054B338 + 4 * i, 4))[0]
        ev, st = struct.unpack(">2i", rd(ptr, 8))
        pairs.append((ev, st, ptr))
    return pairs


def parse_list_stages(path):
    stages = {}
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.match(r"\s+(stg\d{4})\s+(\S+)\s+(bootable|not bootable)", line)
        if m:
            stages[m.group(1)] = {"internal_name": m.group(2), "bootable": m.group(3) == "bootable"}
    return stages


DOL_EXT = "one|dat|bin|adx|sfd|txd|dff|gncp|fnt|met|mlt|afs|inf|tp|fog|scr|srd|png|bnr|conf|mtn|uva|snb"
NAME_DIRS = ("sng_", "stg", "event", "palsel", "fonts/", "csdfiles/", "sound/", "common/", "enemy/", "character/")


def dol_strings(dol_path, disc_lower):
    """File-name strings in main.dol. names: lower-case literal paths worth keeping (a directory the disc uses, a
    sng_/stg/event stem, or the name of a disc file); formats: printf-style names with a literal stem."""
    d = open(dol_path, "rb").read()
    names, formats = set(), set()
    base = {k.rsplit("/", 1)[-1] for k in disc_lower}
    for m in re.finditer(rb"[ -~]{3,100}", d):
        for t in re.findall(r"[A-Za-z0-9_/\.%\-]+", m.group().decode()):
            if not re.search(r"\.(?:%s)$" % DOL_EXT, t, re.I):
                continue
            tl = t.lower().replace("\\", "/")
            if "%" in tl:
                if re.match(r"^(?:[a-z_/]+%|[a-z]+/)", tl) and not tl.startswith(("%", "o%")):
                    formats.add(t.replace("\\", "/"))
            elif tl in disc_lower or tl.rsplit("/", 1)[-1] in base or tl.startswith(NAME_DIRS):
                names.add(tl)
    return sorted(names), sorted(formats)


def format_regex(fmt):
    """printf-style game file name -> regex over lower-case paths."""
    out = ""
    for part in re.split(r"(%0?\d*[dsc])", fmt.lower()):
        if re.fullmatch(r"%0?\d*[dsc]", part):
            n = re.search(r"\d+", part)
            out += (r"\d{%d}" % int(n.group())) if part.endswith("d") and n else (r"[^/]+" if part.endswith("s") else ".")
        else:
            out += re.escape(part)
    return re.compile("^" + out + "$")


def walk_files(root):
    out = []
    for r, _, fs in os.walk(root):
        for f in fs:
            out.append(os.path.relpath(os.path.join(r, f), root).replace("\\", "/"))
    return sorted(out)


LANG_TOKEN = re.compile(r"(?<![A-Za-z])(EN|FR|GE|IT|JP|SP)(?=[._0-9])")
PALSEL = re.compile(r"^palsel/PalSel(En|Fr|Ge|It|Sp)\.txd$")


def language_twins(path):
    """The same file in the other five languages (path strings only)."""
    twins = []
    m = PALSEL.match(path)
    if m:
        for l in ["En", "Fr", "Ge", "It", "Sp"]:
            if l != m.group(1):
                twins.append((path.replace("PalSel" + m.group(1), "PalSel" + l), l.upper()))
        return twins
    m = LANG_TOKEN.search(path)
    if m:
        for l in LANGS:
            if l != m.group(1):
                twins.append((path[:m.start()] + l + path[m.end():], l))
    return twins


def event_id_of(path):
    m = re.match(r"^event/event(\d{4})", path)
    return m.group(1) if m else None


def stage_label(sid, stages):
    t = STAGE_TITLES.get(sid)
    if t:
        return t
    st = stages.get(sid, {}).get("internal_name")
    return st or sid


def derive(path, stages_known, events_known):
    """Path-pattern contexts for a file no run read. Returns (contexts, basis) or None."""
    m = re.match(r"^(?:fonts/)?(stg\d{4})/", path)
    if m:
        ctx = ["stage:" + m.group(1)]
        if path.endswith("_hrd.dat"):
            ctx.append("mode:expert")
        return ctx, "stage folder name"
    m = re.match(r"^csdFiles/(?:stageTitle/StageTitle|pause/pause)(\d{4})_[A-Z]{2}\.txd$", path)
    if m:
        return ["stage:stg" + m.group(1)], "stage number in the file name"
    m = re.match(r"^sng_(stg\d{4})b?\.adx$", path)
    if m:
        return ["stage:" + m.group(1)], "stage number in the file name"
    m = re.match(r"^sng_EV(\d{4})_[EJ]\.adx$", path)
    if m:
        return ["event:event" + m.group(1)], "event number in the file name (the event manager formats sng_EV%04d_E.adx)"
    m = re.match(r"^sng_E(\d{4})\.adx$", path)
    if m:
        return ["movie:E" + m.group(1)], "movie number in the file name"
    m = re.match(r"^E(\d{4})\.sfd$", path)
    if m:
        return ["movie:E" + m.group(1)], "movie number in the file name (E%04d.sfd)"
    e = event_id_of(path)
    if e:
        return ["event:event" + e], "event number in the file name"
    if path.startswith("fonts/Save/"):
        return ["frontend"], "font folder name (memory-card / save dialogs)"
    if path.startswith("fonts/Advertise/"):
        return ["boot"], "font folder name (start-up legal / advertise screens; observed for the English files)"
    if path.startswith("fonts/Movie/"):
        return ["movie"], "font folder name (cutscene / movie subtitles; observed in cutscene runs)"
    if path.startswith("stafroll/"):
        return ["credits"], "folder name (staff roll)"
    if re.match(r"^character/shadow2p", path):
        return ["mode:2p-battle"], "file name (shadow2p* = 2P battle skins; shadow2py observed in 0800-0802)"
    m = re.match(r"^sound/(B3_SHADOW|B4_SUBHERO|B5_SUBDARK)_J\.mlt$", path)
    if m:
        return ["lang:JP"], "Japanese variant of the observed %s_E.mlt" % m.group(1)
    if path in ("sng_Battle1.adx", "sng_Battle2.adx"):
        return ["mode:2p-battle"], "docs/re/events.md 3.5: fixed streams of events 950..978 (2P intros)"
    if re.match(r"^sng_sys0\d\.adx$", path):
        return ["frontend"], "name (system music; sys01/sys03 observed in the title run)"
    if path.startswith("csdFiles/") and re.search(r"(SelectMode|Options|Story_Library|Synopsis|MediaMenu|honban|Logo|BattleMode|2P_|MChara|Mchara)", path):
        if re.search(r"(2P_|BattleMode)", path):
            return ["mode:2p-battle"], "UI layout name (2P)"
        return ["frontend"], "UI layout name (menus)"
    if path.startswith("csdFiles/") and re.search(r"(GameOver|MissionFailed|Result|Score|HPBar|Gauge|Hissatu|CommandWindow|TWindow|VHArrow|BossGauge|BbuttonWindow|EggBoss|BChara|InfoWindow)", path):
        return ["in-stage-ui"], "UI layout name (stage HUD / result)"
    return None


def build_map(args):
    files_root = os.path.join(args.game_data, "files")
    disc = walk_files(files_root)
    manifest, traces = read_traces(args.runs)
    stages_tbl = parse_list_stages(args.list_stages)
    dol_path = os.path.join(args.game_data, "sys", "main.dol")
    ev_pairs = parse_dol_events(dol_path)
    names, formats = dol_strings(dol_path, {f.lower() for f in disc})
    names_set = set(names)

    # --- observed contexts ------------------------------------------------------------------------------------
    stage_runs = [t for t, m in manifest.items() if m["kind"] == "stage" and t in traces]
    stage_ctx_of = {t: manifest[t]["context"] for t in stage_runs}
    all_stage_ctx = sorted(set(stage_ctx_of.values()))
    obs = collections.defaultdict(set)      # path -> contexts
    obs_runs = collections.defaultdict(set)  # path -> tags
    # Shared files (not named after a stage / event / movie) are attributed to the run's own context only where the
    # run is clean: --boot-stage, --boot-2p, the front-end runs. The story-route runs (--boot-story-stage,
    # --boot-event, --boot-story, --boot-last-story, --boot-flashback, --boot-expert) first build the story's own
    # stage 0100 (its sound bank, enemy set ...), so their shared files are attributed only when the Westopolis
    # --boot-stage run did not read them too; their path-named files (stage, event, movie) always count.
    baseline = set(traces.get("bs0100", []))
    for tag, files in traces.items():
        m = manifest[tag]
        route_run = tag.startswith(("ss", "be")) or tag in ("story", "laststory", "flashback", "expert0")
        if tag.startswith(("ss", "be")):
            files = truncate_at_foreign_stage(files, m["context"].replace("stage:", ""))
        for f in files:
            obs_runs[f].add(tag)
            s = named_stage(f)
            e = event_id_of(f)
            if s:
                obs[f].add("stage:" + s)
                if f.endswith("_hrd.dat"):
                    obs[f].add("mode:expert")
            elif e:
                obs[f].add("event:event" + e)
                if m["context"].startswith("mode:"):
                    obs[f].add(m["context"])
            elif f.endswith(".sfd"):
                obs[f].add(("movie:" + os.path.splitext(f)[0]) if f != "Op.sfd" else "frontend")
                if m["context"].startswith("mode:"):
                    obs[f].add(m["context"])
            elif re.match(r"^sng_stg\d{4}b?\.adx$", f):
                obs[f].add("stage:" + re.match(r"^sng_(stg\d{4})", f).group(1))
            elif f.startswith("palsel/"):
                obs[f].add("boot")   # the PAL 50/60 Hz screen, shown at every PAL cold boot
            elif route_run:
                if f not in baseline and m["kind"] == "stage":
                    obs[f].add("stage:" + m["context"])
            elif m["kind"] == "stage":
                obs[f].add("stage:" + m["context"])
            else:
                obs[f].add(m["context"])
                if tag.startswith("p2") and tag != "p2three":
                    obs[f].add("stage:stg" + tag[2:])
    # a file every stage run read -> "all-stages"
    bs_ctxs = {"stage:" + manifest[t]["context"] for t in stage_runs if t.startswith("bs")}
    for f, c in list(obs.items()):
        if len(c & bs_ctxs) >= 0.95 * len(bs_ctxs):
            c -= {x for x in c if x.startswith("stage:")}
            c.add("all-stages")

    # --- derived ----------------------------------------------------------------------------------------------
    files_out = {}
    for f in disc:
        if f in obs:
            ctx = sorted(obs[f])
            files_out[f] = {"contexts": ctx, "source": "observed", "runs": len(obs_runs[f])}
    # language twins of observed files
    for f in list(files_out):
        if files_out[f]["source"] != "observed":
            continue
        for twin, lang in language_twins(f):
            if twin in disc and twin not in files_out:
                files_out[twin] = {"contexts": sorted(set(files_out[f]["contexts"]) | {"lang:" + lang}),
                                   "source": "derived", "basis": "language variant of " + f}
    for f in disc:
        if f in files_out:
            continue
        d = derive(f, stages_tbl, None)
        if d:
            ctx, basis = d
            lm = LANG_TOKEN.search(f)
            if lm and lm.group(1) != "EN":
                ctx = ctx + ["lang:" + lm.group(1)]
            files_out[f] = {"contexts": ctx, "source": "derived", "basis": basis}
        else:
            fl = f.lower()
            if fl in names_set or fl.rsplit("/", 1)[-1] in names_set:
                files_out[f] = {"contexts": ["named-in-code"], "source": "derived",
                                "basis": "the file name is a string in main.dol (the game can open it by this name); "
                                         "no traced run reached it, trigger unknown"}
                continue
            rec = {"contexts": [], "source": "none"}
            top = f.split("/")[0]
            if top in ("enemy", "character", "common", "sound", "csdFiles"):
                rec["note"] = ("shared data directory: loaded on demand; no traced run reached it, so it may still be "
                               "opened later in real play (not confirmed)")
            files_out[f] = rec

    # --- stages -----------------------------------------------------------------------------------------------
    stages_out = {}
    stage_ids = sorted(set(stages_tbl) | {named_stage(f) for f in disc if named_stage(f)})
    for sid in stage_ids:
        info = stages_tbl.get(sid, {})
        num = int(sid[3:])
        if sid not in stages_tbl:
            kind = "not-in-guest-table"   # folder on the disc, no record in the game's stage table: never booted
        elif 900 <= num < 1000 or num == 5:
            kind = "event-stage"
        elif sid in ("stg0800", "stg0801", "stg0802"):
            kind = "2p-arena"
        elif num < 10:
            kind = "debug"
        elif num % 100 >= 10 and num < 700:
            kind = "boss"
        elif num in (700, 710):
            kind = "last-story"
        else:
            kind = "stage"
        runs = [t for t in traces if manifest[t]["context"] == sid and manifest[t]["kind"] == "stage"]
        stages_out[sid] = {
            "name": STAGE_TITLES.get(sid) or info.get("internal_name") or sid,
            "name_source": "launcher STAGE_TITLES (English title)" if sid in STAGE_TITLES else
            ("guest stage table internal name" if info else "folder name only"),
            "internal_name": info.get("internal_name"), "kind": kind,
            "in_guest_stage_table": sid in stages_tbl, "boot_stage_ok": info.get("bootable", False),
            "traced_by": runs,
            "files_observed": sum(1 for f, v in files_out.items() if v["source"] == "observed" and
                                  ("stage:" + sid) in v["contexts"]),
            "files_total": sum(1 for f, v in files_out.items() if ("stage:" + sid) in v["contexts"]),
        }

    # --- events -----------------------------------------------------------------------------------------------
    ev_files = collections.defaultdict(list)
    for f in disc:
        e = event_id_of(f)
        if e:
            ev_files[e].append(f)
    table = {str(ev).zfill(4): (st, hex(ptr)) for ev, st, ptr in ev_pairs}
    movies = {m.group(1) for m in (re.match(r"^E(\d{4})\.sfd$", f) for f in disc) if m}
    events_out = {}
    ids = sorted(set(ev_files) | set(table) | movies)
    for e in ids:
        rec = {"label": "Cutscene " + e}
        fl = sorted(ev_files.get(e, []))
        if fl:
            rec["files"] = fl
        if e in movies:
            rec["movie"] = "E%s.sfd" % e
        music = "sng_EV%s_E.adx" % e
        if music in disc:
            rec["music"] = music
        if e in table:
            st = table[e][0]
            rec["table_stage"] = ("stg%04d" % st) if st > 0 else None
            rec["table_record"] = table[e][1]
            rec["table_basis"] = "main.dol event table (86 {event id, stage id} records, pointers at 0x8054B338)"
        n = int(e)
        if 800 <= n <= 826:
            rec["group"] = "2P intro / bonus scene (ids 0800-0826)"
        elif 900 <= n <= 926 or 950 <= n <= 980:
            rec["group"] = "last-story / 2P / bonus scene (ids 0900-0980)"
        elif e in table:
            rec["group"] = "story cutscene (first digit %s = chapter)" % e[0]
        if 901 <= n <= 911 and ("stg" + e) in stages_out:
            rec["host_stage"] = "stg" + e
            rec["host_basis"] = "event stage of the same number (--boot-event event%s boots stg%s)" % (e, e)
        elif e in table and table[e][0] > 0:
            rec["host_stage"] = "stg%04d" % table[e][0]
            rec["host_basis"] = "table record's stage id (meaning inferred: the stage the cutscene is bound to)"
        obs_f = [f for f in fl if f in obs]
        rec["observed"] = bool(obs_f)
        rec["confidence"] = ("observed" if obs_f else ("table" if e in table else "name only"))
        events_out[e] = rec
    # give an event's own files the host stage as a second context (derived)
    for e, rec in events_out.items():
        hs = rec.get("host_stage")
        for f in rec.get("files", []):
            if hs and files_out.get(f):
                c = files_out[f]["contexts"]
                if "stage:" + hs not in c:
                    c.append("stage:" + hs)
                    c.sort()
    # --- summary ----------------------------------------------------------------------------------------------
    by_source = collections.Counter(v["source"] for v in files_out.values())
    top = collections.Counter()
    for f, v in files_out.items():
        top[f.split("/")[0] if "/" in f else "(root)"] += 1
    summary = {
        "disc_files": len(disc), "by_source": dict(by_source),
        "runs": {t: {"files_read": len(traces[t]), **{k: manifest[t][k] for k in ("args", "budget_ms", "exit")}}
                 for t in sorted(traces)},
        "all_stages_files": sum(1 for v in files_out.values() if "all-stages" in v["contexts"]),
        "stage_runs": len(stage_runs),
    }
    result = {"schema": 1, "dol_names": names, "dol_name_formats": formats, "stages": stages_out, "events": events_out, "files": files_out, "summary": summary,
              "contexts_doc": {
                  "boot": "read at start-up before any menu (all runs)", "frontend": "title / menus",
                  "all-stages": "read by (almost) every traced stage run", "stage:stgNNNN": "that stage",
                  "event:eventNNNN": "that cutscene", "movie:ENNNN": "that FMV", "mode:2p-battle": "2P battle",
                  "mode:story / last-story / flashback / expert": "the game mode's route",
                  "in-stage-ui": "HUD / result layouts (name rule)", "lang:XX": "language variant"}}
    return result


def cmd_build(args):
    result = build_map(args)
    with open(args.out, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(result, fh, indent=1, sort_keys=True)
        fh.write("\n")
    s = result["summary"]
    print("files", s["disc_files"], s["by_source"], "events", len(result["events"]), "stages", len(result["stages"]))


# ---------------------------------------------------------------------------------------------------------------
# check
# ---------------------------------------------------------------------------------------------------------------

def mod_files(mod_dir):
    """(disc path, where) for everything the mod puts on the disc tree, afs entries and texture files."""
    res = {"files": [], "afs": [], "textures": 0}
    roots = [(mod_dir, "")]
    od = os.path.join(mod_dir, "options")
    if os.path.isdir(od):
        for opt in sorted(os.listdir(od)):
            for ch in sorted(os.listdir(os.path.join(od, opt))):
                roots.append((os.path.join(od, opt, ch), "option %s=%s" % (opt, ch)))
    for root, tag in roots:
        fd = os.path.join(root, "files")
        if os.path.isdir(fd):
            for p in walk_files(fd):
                res["files"].append((p, tag))
        ad = os.path.join(root, "afs")
        if os.path.isdir(ad):
            for p in walk_files(ad):
                res["afs"].append((p, tag))
        td = os.path.join(root, "textures")
        if os.path.isdir(td):
            res["textures"] += len(walk_files(td))
    return res


def classify_new(path, names_set, regexes):
    """A mod file with no disc counterpart: can the game open this name at all? -> (kind, contexts)."""
    pl = path.lower()
    d = derive(path, None, None)
    ctx = d[0] if d else []
    if any(r.match(pl) for r in regexes) or pl in names_set or pl.rsplit("/", 1)[-1] in names_set:
        return "new_file_name_known_to_game", ctx or ["named-in-code"]
    return "never_opened", []


def cmd_check(args):
    m = json.load(open(args.map, encoding="utf-8"))
    files = m["files"]
    lower = {k.lower(): k for k in files}
    names_set = set(m.get("dol_names", []))
    regexes = [format_regex(f) for f in m.get("dol_name_formats", [])]
    report = {}
    for mod in sorted(os.listdir(args.mods)):
        d = os.path.join(args.mods, mod)
        if not os.path.isdir(d) or mod.startswith("."):
            continue
        mf = mod_files(d)
        ctxs = collections.Counter()
        status = collections.Counter()
        new_known, never = [], []
        for p, tag in mf["files"]:
            k = lower.get(p.lower())
            if k is None:
                kind, cx = classify_new(p, names_set, regexes)
                status[kind] += 1
                (new_known if kind != "never_opened" else never).append(p)
            else:
                e = files[k]
                status[e["source"]] += 1
                cx = e["contexts"]
            for c in cx:
                ctxs[c] += 1
        report[mod] = {"files": len(mf["files"]), "afs_entries": len(mf["afs"]), "texture_files": mf["textures"],
                       "status": dict(status), "contexts": dict(ctxs.most_common()),
                       "new_files_known_to_game": new_known, "never_opened": never}
    out = json.dumps(report, indent=1)
    if args.json:
        with open(args.json, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(out + "\n")
    print(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--exe", required=True)
    r.add_argument("--game-data", required=True)
    r.add_argument("--out", required=True)
    r.add_argument("--only", help="comma separated run tags")
    b = sub.add_parser("build")
    b.add_argument("--game-data", required=True)
    b.add_argument("--runs", required=True)
    b.add_argument("--list-stages", required=True)
    b.add_argument("--out", required=True)
    c = sub.add_parser("check")
    c.add_argument("--map", required=True)
    c.add_argument("--mods", required=True)
    c.add_argument("--json")
    args = ap.parse_args()
    {"run": cmd_run, "build": cmd_build, "check": cmd_check}[args.cmd](args)


if __name__ == "__main__":
    main()
