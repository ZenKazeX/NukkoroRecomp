#!/usr/bin/env python3
"""End-to-end mod tests that prove what they claim (docs/mods/TESTING.md).

Each scenario installs real downloads with the real manager (ModManager.exe, command line), runs the real
game headless on a fixed input tape, and checks the result from three independent sides:

  plan      the manager's resolved plan (`plan`, JSON)
  applied   what the game says it applied (`--mods-report`)
  read      what the game actually read: every guest file read from `--asset-trace`, its bytes hashed by the
            runtime, compared here with the bytes of the file the plan names (and, for every other file, with the
            disc's bytes or with the stock run's own reads when the port itself serves a file differently)

plus the picture at a fixed game tick and the end-of-run state report, against a stock run made twice (a
determinism control: if the two stock runs differ, picture/state comparisons are reported UNSTABLE, never PASS).

Verdicts per check: PASS, FAIL, UNSTABLE (the control did not hold), SKIP (an input is missing; never counted as a
pass). A scenario with "expect_fail" is a negative control: it states something false on purpose and passes only
when the harness reports that check as FAIL, which shows the check can fail.

The runtime's FNV-1a uses the offset basis 1469598103934665603 (the standard basis is 14695981039346656037; the
runtime's constant lacks the last digit). Hashes here use the runtime's basis so they can be compared.

Usage:
  python -I tools/mods/modtest.py run --exe GAME.exe --manager MGR.exe --game GAME_DATA --downloads DL --work WORK
         [--only NAME[,NAME]] [--scenarios FILE] [--report OUT.json]
  python -I tools/mods/modtest.py gate --base BASE.exe --exe CAND.exe --game GAME_DATA --work WORK --repo REPO
         [--westopolis DIR] [--final-haunt DIR] [--layout FILE]
"""
import argparse
import bisect
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time

RUNTIME_FNV_BASIS = 1469598103934665603
FNV_PRIME = 0x100000001B3
MASK = 0xFFFFFFFFFFFFFFFF
TICKS_PER_FRAME = 810000

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_SCENARIOS = os.path.join(HERE, "modtest_scenarios.json")
DEFAULT_TAPE_DIR = "H:/ShadowTheHedgehogDiskGC_Extract/source/.build/platform/ab-westopolis/host-a"
DEFAULT_FH_DIR = "H:/ShadowTheHedgehogDiskGC_Extract/source/.build/recordings/final-haunt"


def fnv_runtime(data):
    h = RUNTIME_FNV_BASIS
    for b in data:
        h = ((h ^ b) * FNV_PRIME) & MASK
    return h


def sha(path, n=16):
    if not os.path.exists(path):
        return None
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()[:n]


def md5(path):
    with open(path, "rb") as f:
        return hashlib.md5(f.read()).hexdigest()


def read_range(path, offset, length):
    with open(path, "rb") as f:
        f.seek(offset)
        data = f.read(length)
    return data + bytes(length - len(data))  # the file API zero-fills past the end


def host_path(p):
    """A path from a report or plan as Python can open it (strip a long-path prefix)."""
    p = p.replace("\\", "/")
    if p.startswith("//?/"):
        p = p[4:]
    return p


class ComposedVoices:
    """The port's switchable voice archive (runtime/src/gekko/voice_language.cpp, VoiceArchive), rebuilt here from
    the same two files: PRS_VOICE_E.afs (the disc's, or an enabled mod's) and the player's PRS_VOICE_J.afs. When the
    Japanese file is present the game is served this composed archive under the English name, so a read of it can be
    checked byte for byte against the English (or Japanese) file it was composed from, not only against the stock run."""
    ALIGN = 2048

    def __init__(self, english, japanese):
        self.files = [english, japanese]
        tocs, names = [], None
        for i, path in enumerate(self.files):
            with open(path, "rb") as f:
                head = f.read(8)
                if len(head) < 8 or head[:3] != b"AFS":
                    raise ValueError(path + " is not an AFS")
                count = int.from_bytes(head[4:8], "little")
                table = f.read(8 * count + 8)
            tocs.append([(int.from_bytes(table[8 * e:8 * e + 4], "little"), int.from_bytes(table[8 * e + 4:8 * e + 8], "little"))
                         for e in range(count)])
            if i == 0:
                names = (int.from_bytes(table[8 * count:8 * count + 4], "little"), int.from_bytes(table[8 * count + 4:8 * count + 8], "little"))
        if len(tocs[0]) != len(tocs[1]):
            raise ValueError("the two voice archives differ in entry count")
        at = max(tocs[0][0][0], tocs[1][0][0])
        self.entries = []  # (slot, [source_e, source_j], [size_e, size_j])
        for e in range(len(tocs[0])):
            sizes = [tocs[0][e][1], tocs[1][e][1]]
            self.entries.append((at, [tocs[0][e][0], tocs[1][e][0]], sizes))
            at += (max(max(sizes), 1) + self.ALIGN - 1) // self.ALIGN * self.ALIGN
        with open(english, "rb") as f:
            f.seek(names[0])
            self.names = f.read(names[1])
        self.names_at = at
        head = bytearray(self.entries[0][0])
        head[0:3] = b"AFS"  # byte 3 stays 0, as the runtime writes it
        head[4:8] = len(self.entries).to_bytes(4, "little")
        for e, (slot, _, sizes) in enumerate(self.entries):
            head[8 + 8 * e:12 + 8 * e] = slot.to_bytes(4, "little")
            head[12 + 8 * e:16 + 8 * e] = max(sizes).to_bytes(4, "little")
        n = len(self.entries)
        head[8 + 8 * n:12 + 8 * n] = self.names_at.to_bytes(4, "little")
        head[12 + 8 * n:16 + 8 * n] = len(self.names).to_bytes(4, "little")
        self.head = bytes(head)
        self.slots = [e[0] for e in self.entries]

    def read(self, offset, length, japanese):
        out = bytearray(length)
        end = offset + length
        if offset < len(self.head):
            n = min(end, len(self.head)) - offset
            out[0:n] = self.head[offset:offset + n]
        if end > self.names_at and offset < self.names_at + len(self.names):
            a, b = max(offset, self.names_at), min(end, self.names_at + len(self.names))
            out[a - offset:b - offset] = self.names[a - self.names_at:b - self.names_at]
        i = max(bisect.bisect_right(self.slots, offset) - 1, 0)
        which = 1 if japanese else 0
        with open(self.files[which], "rb") as f:
            while i < len(self.entries) and self.entries[i][0] < end:
                slot, src, sizes = self.entries[i]
                a, b = max(offset, slot), min(end, slot + sizes[which])
                if a < b:
                    f.seek(src[which] + (a - slot))
                    out[a - offset:b - offset] = f.read(b - a)
                i += 1
        return bytes(out)


class Check:
    def __init__(self, name, verdict, evidence):
        self.name, self.verdict, self.evidence = name, verdict, evidence

    def as_dict(self):
        return {"check": self.name, "verdict": self.verdict, "evidence": self.evidence}


# ---- running things ---------------------------------------------------------------------------------------------

def run(cmd, cwd=None, env=None, timeout=900):
    t0 = time.time()
    p = subprocess.run(cmd, cwd=cwd, env=env, capture_output=True, timeout=timeout)
    def norm(b):  # the game writes CRLF
        return b.decode("utf-8", "replace").replace("\r\n", "\n")

    return p.returncode, norm(p.stdout), norm(p.stderr), time.time() - t0


def manager(args, mgr, game, mods_dir, *cmd):
    rc, out, err, _ = run([mgr, "--mods-dir", mods_dir, "--game-data", game] + list(cmd))
    return rc, out + err


def game_run(args, exe, workdir, stage, tick, budget_ms, extra, tape=True, audio=False, boot=None, options_cfg=None):
    """One headless run: capture at `tick`, state report, asset trace. Returns a dict of outputs. `boot` replaces
    the default "--boot-stage <stage>" (e.g. a movie through --boot-cutscene); `tape` False plays no input tape."""
    if os.path.isdir(workdir):
        shutil.rmtree(workdir)
    os.makedirs(workdir)
    tape_dir = args.tape_dir
    for f in ("card.raw", "SRAM.pc.raw", "display.cfg", "pad.cfg"):
        shutil.copy(os.path.join(tape_dir, f), workdir)
    if options_cfg is not None:
        # The PC settings (options.cfg beside display.cfg), e.g. "voice=0" for the Japanese voice setting.
        with open(os.path.join(workdir, "options.cfg"), "wb") as f:
            f.write(options_cfg.encode("utf-8") + b"\n")
    cmd = [exe, "--game-data", args.game] + (list(boot) if boot is not None else ["--boot-stage", stage]) + ["--headless", "--memory-card", "card.raw",
           "--display-cfg", "display.cfg", "--pad-cfg", "pad.cfg", "--state-file", "unused.gekkostate",
           "--snapshot-key", "none", "--no-xinput", "--budget-ms", str(budget_ms), "--native-output", "1280x720",
           "--width", "1280", "--height", "720", "--state-report", "state.json", "--asset-trace", "trace.jsonl"]
    if audio:
        cmd += ["--audio-dump", "audio.pcm"]
    else:
        cmd += ["--no-audio"]
    if tick:
        cmd += ["--capture-at-tick", "%d:cap.png" % tick, "--stop-after-capture"]
    if tape:
        cmd += ["--input-tape-play", os.path.join(tape_dir, "input.tape").replace("\\", "/")]
    cmd += extra
    env = dict(os.environ, SHADOW_R2_MSAA="1")
    rc, out, err, secs = run(cmd, cwd=workdir, env=env)
    with open(os.path.join(workdir, "stdout.log"), "w", encoding="utf-8") as f:
        f.write(out)
    with open(os.path.join(workdir, "stderr.log"), "w", encoding="utf-8") as f:
        f.write(err)
    return {"dir": workdir, "exit": rc, "log": out + err, "seconds": round(secs, 1),
            "frame": frame_sha(workdir), "state": sha(os.path.join(workdir, "state.json")),
            "pcm": sha(os.path.join(workdir, "audio.pcm")),
            "trace": os.path.join(workdir, "trace.jsonl"), "report": os.path.join(workdir, "mods-report.json")}


def frame_sha(workdir):
    """The captured frame's hash. The runtime writes the capture it was asked for as cap.png or, when the path it
    takes has no PNG writer (the renderer of a tests build), as cap.ppm beside cap.capture.json; either is the frame."""
    for name in ("cap.png", "cap.ppm"):
        h = sha(os.path.join(workdir, name))
        if h is not None:
            return h
    return None

def reads(trace):
    out = []
    if not os.path.exists(trace):
        return out
    with open(trace, encoding="utf-8") as f:
        for line in f:
            try:
                e = json.loads(line)
            except ValueError:
                continue
            if e.get("ev") == "file_read" and e.get("ok"):
                out.append((e["file"], int(e["file_offset"]), int(e["length"]), int(e["bytes_fnv1a"], 16)))
    return out


# ---- checks -----------------------------------------------------------------------------------------------------

def check_reads(args, run_out, stock_reads, applied):
    """Every read: a modded path must carry the modded file's bytes; every other path the disc's (or the stock
    run's, for files the port itself transforms). Returns the checks and per-path stats."""
    stock_by_key = {(p.lower(), o, n): h for p, o, n, h in stock_reads}
    stock_transformed = set()
    for p, o, n, h in stock_reads:
        disc = os.path.join(args.game, "files", p)
        if os.path.exists(disc) and fnv_runtime(read_range(disc, o, n)) != h:
            stock_transformed.add(p.lower())
    modded = {k.lower(): host_path(v) for k, v in applied.items()}
    stats = {"modded_match": 0, "modded_mismatch": 0, "modded_port_transformed": 0, "other_match": 0,
             "other_mismatch": 0, "other_port_transformed_same_as_stock": 0, "other_port_transformed_unknown": 0,
             "modded_match_composed_voices": 0, "other_match_composed_voices": 0}
    examples = {"modded_mismatch": [], "other_mismatch": []}
    per_path = {}
    # The switchable voice archive: with the player's Japanese voices present the port serves PRS_VOICE_E.afs
    # composed from the English file (the disc's or the mod's) and PRS_VOICE_J.afs. Its reads are checked against
    # that composition (English clips first; Japanese clips when the run chose them), byte for byte.
    composed = None
    voice_key = "prs_voice_e.afs"
    if args.jp_voices and os.path.exists(args.jp_voices):
        english = modded.get(voice_key) or os.path.join(args.game, "files", "PRS_VOICE_E.afs")
        composed = ComposedVoices(english, args.jp_voices)

    def composed_match(o, n, h):
        return composed is not None and any(fnv_runtime(composed.read(o, n, jp)) == h for jp in (False, True))

    for p, o, n, h in reads(run_out["trace"]):
        key = p.lower()
        if key == voice_key and composed_match(o, n, h):
            if key in modded:
                stats["modded_match_composed_voices"] += 1
                per_path.setdefault(key, [0, 0])[0] += 1
            else:
                stats["other_match_composed_voices"] += 1
            continue
        if key in modded:
            got = fnv_runtime(read_range(modded[key], o, n))
            per_path.setdefault(key, [0, 0])
            if got == h:
                stats["modded_match"] += 1
                per_path[key][0] += 1
            elif key in stock_transformed:
                stats["modded_port_transformed"] += 1
            else:
                stats["modded_mismatch"] += 1
                per_path[key][1] += 1
                if len(examples["modded_mismatch"]) < 5:
                    examples["modded_mismatch"].append([p, o, n])
        else:
            disc = os.path.join(args.game, "files", p)
            if os.path.exists(disc) and fnv_runtime(read_range(disc, o, n)) == h:
                stats["other_match"] += 1
            elif key in stock_transformed:
                if stock_by_key.get((key, o, n)) == h:
                    stats["other_port_transformed_same_as_stock"] += 1
                else:
                    stats["other_port_transformed_unknown"] += 1
            else:
                stats["other_mismatch"] += 1
                if len(examples["other_mismatch"]) < 5:
                    examples["other_mismatch"].append([p, o, n])
    return stats, examples, per_path


def evaluate(args, sc, run_out, stock, stock_stable, plan, applied_report, others=None):
    others = others or {}
    checks = []
    exp = sc.get("expect", {})
    log = run_out["log"]

    def add(name, ok, evidence, unstable=False):
        checks.append(Check(name, "UNSTABLE" if unstable else ("PASS" if ok else "FAIL"), evidence))

    add("exit", run_out["exit"] == exp.get("exit", 0), {"exit": run_out["exit"], "want": exp.get("exit", 0)})
    for rx in exp.get("log_contains", []):
        m = re.search(rx, log, re.M)
        add("log contains /%s/" % rx, m is not None, {"line": m.group(0) if m else None})
    for rx in exp.get("log_absent", []):
        m = re.search(rx, log, re.M)
        add("log lacks /%s/" % rx, m is None, {"line": m.group(0) if m else None})

    applied = {}
    if applied_report is not None:
        applied = {f["game_path"]: f["host"] for f in applied_report.get("files", [])}
    if "active" in exp:
        got = bool(applied_report and applied_report.get("active"))
        add("loader active = %s" % exp["active"], got == exp["active"], {"report_active": got})

    # plan (manager) == applied (game)
    if plan is not None and applied_report is not None and exp.get("plan_matches_applied", True):
        plan_paths = {f["game_path"].lower() for f in plan.get("files", [])}
        plan_paths |= {a["archive"].lower() for a in plan.get("afs", [])}
        applied_paths = {k.lower() for k in applied}
        add("manager plan == game applied", plan_paths == applied_paths,
            {"plan_only": sorted(plan_paths - applied_paths)[:5], "applied_only": sorted(applied_paths - plan_paths)[:5],
             "files": len(applied_paths)})

    # reads
    if exp.get("read_proof", True) and os.path.exists(run_out["trace"]):
        stats, examples, per_path = check_reads(args, run_out, stock["reads"], applied)
        add("every read of a non-modded file carries the disc's bytes (or the stock run's)",
            stats["other_mismatch"] == 0 and stats["other_port_transformed_unknown"] == 0,
            {"stats": stats, "examples": examples["other_mismatch"]})
        if applied:
            add("every read of a modded file carries the mod's bytes",
                stats["modded_mismatch"] == 0, {"stats": stats, "examples": examples["modded_mismatch"]})
        for path in exp.get("must_read", []):
            got = per_path.get(path.lower(), [0, 0])
            add("the game read %s from the mod" % path, got[0] > 0 and got[1] == 0,
                {"reads_matching_mod": got[0], "reads_not_matching": got[1]})
        for path, mod_id in exp.get("winner", {}).items():
            host = applied.get(path) or applied.get(path.lower())
            ok = host is not None and ("/%s/" % mod_id) in host_path(host)
            add("%s comes from %s" % (path, mod_id), ok, {"host": host})

    # picture and state against stock
    for what in ("frame", "state", "pcm"):
        want = exp.get(what)
        if want is None:
            continue
        mine, ref = run_out[what], stock[what]
        if mine is None or ref is None:
            checks.append(Check("%s %s stock" % (what, want), "SKIP", {"mine": mine, "stock": ref}))
            continue
        same = mine == ref
        ok = same if want == "equals" else not same
        add("%s %s stock" % (what, want), ok, {"mine": mine, "stock": ref}, unstable=not stock_stable[what])
    # Against another scenario of this run (same stage, tick and boot), e.g. code chosen vs the same data without it.
    vs = exp.get("vs")
    if vs:
        other = others.get(vs["scenario"])
        for what in ("frame", "state", "pcm"):
            want = vs.get(what)
            if want is None:
                continue
            name = "%s %s %s" % (what, want, vs["scenario"])
            mine, ref = run_out[what], other[what] if other else None
            if mine is None or ref is None:
                checks.append(Check(name, "SKIP", {"mine": mine, "other": ref, "why": "no such run in this pass" if not other else ""}))
                continue
            same = mine == ref
            add(name, same if want == "equals" else not same, {"mine": mine, "other": ref}, unstable=not stock_stable[what])
    if "installed_mods" in exp:
        mods_dir = os.path.join(os.path.dirname(run_out["dir"]), "mods")
        got = sorted(d for d in os.listdir(mods_dir) if not d.startswith(".") and os.path.isdir(os.path.join(mods_dir, d)))
        add("installed mods = %d" % exp["installed_mods"], len(got) == exp["installed_mods"], {"folders": got})
    return checks


# ---- scenario setup ---------------------------------------------------------------------------------------------

def game_folder_snapshot(game):
    """The player's own files in the game folder: SHA-256 of every file outside files/ and sys/ (saves, memory
    cards, settings, reports), size and time of the executables, size and time of every disc file."""
    snap = {}
    for root, dirs, files in os.walk(game):
        rel_root = os.path.relpath(root, game).replace("\\", "/")
        for name in files:
            path = os.path.join(root, name)
            rel = (name if rel_root == "." else rel_root + "/" + name)
            st = os.stat(path)
            top = rel.split("/", 1)[0].lower()
            if top in ("files", "sys") or name.lower().endswith(".exe"):
                snap[rel] = "%d/%d" % (st.st_size, st.st_mtime_ns)
            else:
                with open(path, "rb") as f:
                    snap[rel] = hashlib.sha256(f.read()).hexdigest()
    return snap


def make_synthetic(args, spec, work):
    """Mods built here from real files, for failure paths. Returns a folder to install."""
    kind = spec["synthetic"]
    folder = os.path.join(work, "synthetic", kind)
    if os.path.isdir(folder):
        shutil.rmtree(folder)
    os.makedirs(folder)
    if kind == "truncated-shadow-one":
        src = os.path.join(args.game, "files", "character", "shadow.one")
        os.makedirs(os.path.join(folder, "files", "character"))
        with open(src, "rb") as f:
            data = f.read()
        with open(os.path.join(folder, "files", "character", "shadow.one"), "wb") as f:
            f.write(data[: len(data) // 3])
        with open(os.path.join(folder, "mod.json"), "w") as f:
            json.dump({"id": "truncated-shadow", "name": "Truncated Shadow (synthetic)"}, f)
    elif kind == "needs-missing":
        os.makedirs(os.path.join(folder, "files"))
        shutil.copy(os.path.join(args.game, "files", "nukkoro2.inf"), os.path.join(folder, "files", "nukkoro2.inf"))
        with open(os.path.join(folder, "mod.json"), "w") as f:
            json.dump({"id": "needs-missing", "name": "Needs a missing mod (synthetic)", "dependencies": ["ghost>=2"]}, f)
    elif kind == "voices-from-reloaded":
        src = os.path.join(args.downloads, "dl", "ShdTH-Reloaded", "AFS", "PRS_VOICE_E")
        if not os.path.isdir(src):
            return None
        shutil.copytree(src, os.path.join(folder, "afs", "PRS_VOICE_E"))
        with open(os.path.join(folder, "mod.json"), "w") as f:
            json.dump({"id": "reloaded-voices", "name": "Reloaded voice entries only"}, f)
    else:
        raise ValueError("unknown synthetic mod " + kind)
    return folder


def prepare_mods(args, sc, mods_dir):
    """Install and configure the scenario's mods with the manager. Returns (ok, notes)."""
    if os.path.isdir(mods_dir):
        shutil.rmtree(mods_dir)
    os.makedirs(mods_dir)
    notes = []
    for m in sc.get("mods", []):
        if "install" in m:
            src = os.path.join(args.downloads, m["install"])
            if not os.path.exists(src):
                return False, "download missing: " + m["install"]
        else:
            src = make_synthetic(args, m, os.path.dirname(mods_dir))
            if src is None:
                return False, "input for synthetic mod missing: " + m["synthetic"]
        if m.get("place_directly"):
            # Damage after installation (a disk error, a hand edit): the folder goes in as it is, bypassing the
            # installer's own check, so the runtime's check is what is tested.
            with open(os.path.join(src, "mod.json"), encoding="utf-8") as f:
                mid = json.load(f)["id"]
            shutil.copytree(src, os.path.join(mods_dir, mid))
            manager(args, args.manager, args.game, mods_dir, "list")  # lets the manager add it to mods.json
            notes.append("placed " + mid + " without the installer")
            m["_id"] = mid
            continue
        rc, out = manager(args, args.manager, args.game, mods_dir, "install", src)
        notes.append(out.strip().splitlines()[0] if out.strip() else "")
        if m.get("install_must_fail"):
            # The installer must refuse this download, with the reason matching the pattern.
            ok = rc != 0 and re.search(m["install_must_fail"], out) is not None
            sc.setdefault("_setup_checks", []).append(
                Check("installer refuses: /%s/" % m["install_must_fail"], "PASS" if ok else "FAIL",
                      {"exit": rc, "output": out.strip()[-300:]}))
            m["_refused"] = True
            continue
        if rc != 0:
            return False, "install failed: " + out[-400:]
        for rx in m.get("install_must_say", []):
            # The installer installs the download and says this (a file left out, with the reason).
            found = re.search(rx, out, re.M)
            sc.setdefault("_setup_checks", []).append(
                Check("installer says: /%s/" % rx, "PASS" if found else "FAIL", {"line": found.group(0) if found else None}))
        mid = re.search(r"installed (\S+)", out).group(1)
        m["_id"] = mid
    for m in sc.get("mods", []):
        if m.get("_refused"):
            continue
        if m.get("enable", True):
            manager(args, args.manager, args.game, mods_dir, "enable", m["_id"])
        for opt, choice in m.get("options", {}).items():
            manager(args, args.manager, args.game, mods_dir, "option", m["_id"], opt, choice)
    for i, m in enumerate(sc.get("mods", [])):
        if "priority" in m and not m.get("_refused"):
            manager(args, args.manager, args.game, mods_dir, "move", m["_id"], str(m["priority"]))
    for action in sc.get("after_setup", []):
        if action == "damage-state":
            with open(os.path.join(mods_dir, "mods.json"), "r+b") as f:
                data = f.read()
                f.seek(0)
                f.truncate()
                f.write(data[: len(data) // 2])
        elif action == "master-off":
            manager(args, args.manager, args.game, mods_dir, "master", "off")
    # Code mods: chosen with the manager ("code <mod> <item> on"), then decided by build_variant.py. Only
    # --prepare-only is run here (no recompile): a selection of engine codes needs no variant executable.
    for i, item, state in sc.get("code", []):
        m = sc["mods"][i]
        rc, out = manager(args, args.manager, args.game, mods_dir, "code", m["_id"], item, state)
        if rc != 0:
            return False, "code choice failed: " + out.strip()[-300:]
        notes.append(out.strip())
    if sc.get("prepare_variant"):
        if not os.path.exists(args.src_dol):
            return False, "input missing: the NTSC-U main.dol (--src-dol)"
        rc, out, err, _ = run([sys.executable, "-I", os.path.join(HERE, "codemods", "build_variant.py"), "--repo", args.repo,
                               "--manager", args.manager, "--mods-dir", mods_dir, "--game", args.game,
                               "--src-dol", args.src_dol, "--prepare-only"], timeout=1800)
        tail = (out + err).strip().splitlines()[-3:]
        notes.append("build_variant --prepare-only: exit %d: %s" % (rc, " | ".join(tail)))
        if rc != 0:
            return False, notes[-1]
    return True, notes


# ---- commands ---------------------------------------------------------------------------------------------------

def cmd_run(args):
    with open(args.scenarios, encoding="utf-8") as f:
        scenarios = json.load(f)["scenarios"]
    if args.only:
        wanted = set(args.only.split(","))
        scenarios = [s for s in scenarios if s["name"] in wanted]
    os.makedirs(args.work, exist_ok=True)
    report = {"exe": {"path": args.exe, "md5": md5(args.exe)}, "manager": {"path": args.manager, "md5": md5(args.manager)},
              "started": time.strftime("%Y-%m-%d %H:%M:%S"), "stock": {}, "scenarios": []}
    stocks = {}

    def stock_for(stage, tick, budget, audio, boot=None, run_args=(), tape=True, options_cfg=None):
        key = (stage, tick, budget, audio, None if boot is None else tuple(boot), tuple(run_args), tape, options_cfg)
        if key in stocks:
            return stocks[key]
        tag = stage if boot is None else ("-".join(x.lstrip("-") for x in boot) or "cold-boot")
        if options_cfg:
            tag += "-" + re.sub(r"[^a-z0-9]+", "-", options_cfg.lower()).strip("-")
        a = game_run(args, args.exe, os.path.join(args.work, "stock-%s-%d-a" % (tag, tick)), stage, tick, budget,
                     ["--no-mods"] + list(run_args), audio=audio, boot=boot, tape=tape, options_cfg=options_cfg)
        b = game_run(args, args.exe, os.path.join(args.work, "stock-%s-%d-b" % (tag, tick)), stage, tick, budget,
                     ["--no-mods"] + list(run_args), audio=audio, boot=boot, tape=tape, options_cfg=options_cfg)
        stable = {w: a[w] is not None and a[w] == b[w] for w in ("frame", "state", "pcm")}
        a["reads"] = reads(a["trace"])
        a["reads_b"] = reads(b["trace"])
        same_reads = [(p, o, n, h) for p, o, n, h in a["reads"]] == [(p, o, n, h) for p, o, n, h in a["reads_b"]]
        entry = dict(a, stable=stable)
        report["stock"]["%s@%d" % (tag, tick)] = {"frame": [a["frame"], b["frame"]], "state": [a["state"], b["state"]],
                                                    "pcm": [a["pcm"], b["pcm"]], "reads": len(a["reads"]),
                                                    "reads_identical_between_runs": same_reads, "exit": [a["exit"], b["exit"]]}
        stocks[key] = entry
        return entry

    failures = 0
    others = {}
    for sc in scenarios:
        t0 = time.time()
        stage, tick, budget = sc.get("stage", "stg0100"), int(sc.get("tick", 1200000000)), int(sc.get("budget_ms", 60000))
        audio = bool(sc.get("audio", False))
        result = {"name": sc["name"], "what": sc.get("what", ""), "expect_fail": sc.get("expect_fail", []), "checks": []}
        mods_dir = os.path.join(args.work, sc["name"], "mods")
        boot, run_args, tape = sc.get("boot"), sc.get("run_args", []), sc.get("tape", True)
        options_cfg = sc.get("options_cfg")
        # The player's files beside the disc (saves, memory cards, settings, executables): hashed before the
        # manager touches anything and after the game ran; nothing there may change.
        before = game_folder_snapshot(args.game) if sc.get("expect", {}).get("game_unchanged") else None
        ok, notes = prepare_mods(args, sc, mods_dir)
        result["setup"] = notes
        if not ok:
            result["checks"].append(Check("setup", "SKIP" if "missing" in notes else "FAIL", {"why": notes}).as_dict())
            result["verdict"] = "SKIP" if "missing" in notes else "FAIL"
            report["scenarios"].append(result)
            print("%-34s %s (%s)" % (sc["name"], result["verdict"], notes))
            failures += result["verdict"] == "FAIL"
            continue
        rc, plan_text = manager(args, args.manager, args.game, mods_dir, "plan")
        plan = None
        try:
            plan = json.loads(plan_text)
        except ValueError:
            pass
        stock = stock_for(stage, tick, budget, audio, boot, run_args, tape, options_cfg)
        extra = ["--mods-dir", mods_dir, "--mods-report", "mods-report.json"] + list(run_args) + sc.get("args", [])
        out = game_run(args, args.exe, os.path.join(args.work, sc["name"], "run"), stage, tick, budget, extra, audio=audio,
                       boot=boot, tape=tape, options_cfg=options_cfg)
        applied = None
        if os.path.exists(out["report"]):
            with open(out["report"], encoding="utf-8") as f:
                applied = json.load(f)
        checks = sc.get("_setup_checks", []) + evaluate(args, sc, out, stock, stock["stable"], plan, applied, others)
        others[sc["name"]] = out
        if before is not None:
            after = game_folder_snapshot(args.game)
            changed = sorted(k for k in set(before) | set(after) if before.get(k) != after.get(k))
            checks.append(Check("nothing in the game folder changed (saves, cards, settings hashed; %d files)" % len(before),
                                "PASS" if not changed else "FAIL", {"changed": changed[:10]}))
        # Negative controls: the named checks must FAIL for the scenario to pass.
        neg = sc.get("expect_fail", [])
        verdict = "PASS"
        for c in checks:
            if any(c.name.startswith(n) for n in neg):
                c.verdict = "PASS (control failed as intended)" if c.verdict == "FAIL" else "FAIL (control did not fail: %s)" % c.verdict
            if c.verdict.startswith("FAIL"):
                verdict = "FAIL"
            elif c.verdict == "UNSTABLE" and verdict == "PASS":
                verdict = "UNSTABLE"
        result.update(verdict=verdict, seconds=round(time.time() - t0, 1), run={k: out[k] for k in ("frame", "state", "pcm", "exit", "seconds")},
                      checks=[c.as_dict() for c in checks])
        report["scenarios"].append(result)
        failures += verdict != "PASS"
        print("%-34s %s  (%d checks, %.0f s)" % (sc["name"], verdict, len(checks), time.time() - t0))
        for c in checks:
            if c.verdict != "PASS":
                print("    %-70s %s" % (c.name[:70], c.verdict))
    report["finished"] = time.strftime("%Y-%m-%d %H:%M:%S")
    for k, v in report["stock"].items():
        print("stock %s: frames %s, states %s, reads identical %s" % (k, "same" if v["frame"][0] == v["frame"][1] else "DIFFER",
                                                                   "same" if v["state"][0] == v["state"][1] else "DIFFER",
                                                                   v["reads_identical_between_runs"]))
    out_path = args.report or os.path.join(args.work, "modtest-report.json")
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(report, f, indent=1)
    print("report:", out_path)
    return 1 if failures else 0


def cmd_gate(args):
    """The mods-off regression gate: base vs candidate, Westopolis 316 s and Final Haunt 120 s, state + PCM, and the
    behaviour trace compared bit for bit. A self-check first: the base run twice must agree, or the gate says so."""
    results = {}

    def one(tag, exe, stage, rec_dir, budget, initial):
        d = os.path.join(args.work, "gate", tag)
        if os.path.isdir(d):
            shutil.rmtree(d)
        os.makedirs(d)
        src = os.path.join(rec_dir, "initial") if initial else rec_dir
        for f in ("card.raw", "SRAM.pc.raw", "display.cfg", "pad.cfg"):
            shutil.copy(os.path.join(src, f), d)
        cmd = [exe, "--game-data", args.game, "--boot-stage", stage, "--headless", "--no-xinput", "--memory-card",
               "card.raw", "--display-cfg", "display.cfg", "--pad-cfg", "pad.cfg", "--state-file", "unused.gekkostate",
               "--snapshot-key", "none", "--budget-ms", str(budget), "--input-tape-play",
               os.path.join(rec_dir, "input.tape").replace("\\", "/"), "--native-output", "1280x720", "--width", "1280",
               "--height", "720", "--audio-dump", "audio.pcm", "--state-report", "state.json"]
        rc, out, err, secs = run(cmd, cwd=d, timeout=3600)
        return {"exit": rc, "pcm": sha(os.path.join(d, "audio.pcm")), "state": sha(os.path.join(d, "state.json")),
                "seconds": round(secs, 1)}

    parts = set(args.parts.split(","))
    for name, stage, rec, budget, initial in (("westopolis", "stg0100", args.westopolis, 316000, False),
                                              ("final-haunt", "stg0604", args.final_haunt, 120000, True)):
        if "runs" not in parts:
            break
        base = one(name + "-base", args.base, stage, rec, budget, initial)
        base2 = one(name + "-base2", args.base, stage, rec, budget, initial)
        cand = one(name + "-cand", args.exe, stage, rec, budget, initial)
        stable = base["pcm"] == base2["pcm"] and base["state"] == base2["state"]
        same = cand["pcm"] == base["pcm"] and cand["state"] == base["state"]
        results[name] = {"base": base, "base_repeat": base2, "candidate": cand, "control_stable": stable,
                         "verdict": "UNSTABLE" if not stable else ("PASS" if same else "FAIL")}
        print("%-12s base %s/%s  repeat %s/%s  cand %s/%s  -> %s" % (name, base["pcm"], base["state"], base2["pcm"],
              base2["state"], cand["pcm"], cand["state"], results[name]["verdict"]))
    # Behaviour traces (tools/behaviour_run.sh + behaviour_diff.py --exact). The shell is named explicitly: a bare
    # "bash" from a Windows process finds System32's WSL launcher before Git's bash.
    bash = args.bash or shutil.which("bash")
    if "behaviour" not in parts:
        bash = None
    beh, errors = {}, []
    for tag, exe in ((("base", args.base), ("cand", args.exe)) if bash else ()):
        out = os.path.join(args.work, "gate", "behave-" + tag).replace("\\", "/")
        if os.path.isdir(out):
            shutil.rmtree(out)
        env = dict(os.environ, BEHAVIOUR_LAYOUT=args.layout)
        rc, o, e, _ = run([bash, os.path.join(args.repo, "tools", "behaviour_run.sh").replace("\\", "/"), exe, "316000", out],
                          cwd=args.repo, env=env, timeout=3600)
        beh[tag] = os.path.join(out, "behaviour.jsonl")
        if not os.path.exists(beh[tag]):
            errors.append("%s: no behaviour trace (exit %d): %s" % (tag, rc, (o + e).strip()[-400:]))
    if bash is None:
        pass
    elif errors:
        # The harness could not produce what it compares: that is not a verdict on the game.
        results["behaviour"] = {"verdict": "ERROR", "why": errors}
    else:
        rc, o, e, _ = run([sys.executable, "-B", os.path.join(args.repo, "tools", "behaviour_diff.py"), "--exact",
                           beh["base"], beh["cand"]], cwd=args.repo)
        verdict = "PASS" if "VERDICT: PASS" in o else "FAIL"
        results["behaviour"] = {"verdict": verdict, "tail": o.strip().splitlines()[-3:]}
    if "behaviour" in results:
        print("behaviour   ", results["behaviour"]["verdict"], results["behaviour"].get("why", results["behaviour"].get("tail")))
    # A partial rerun (--parts) adds to the report of the same base and candidate instead of replacing it.
    path = os.path.join(args.work, "gate", "gate-report.json")
    merged = dict(results)
    if os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            old = json.load(f)
        if old.get("base") == md5(args.base) and old.get("candidate") == md5(args.exe):
            merged = dict(old.get("results", {}), **results)
    with open(path, "w", encoding="utf-8") as f:
        json.dump({"base": md5(args.base), "candidate": md5(args.exe), "results": merged}, f, indent=1)
    results = merged
    gate_path = os.path.join(args.work, "gate", "gate-report.json")
    return 0 if results and all(r["verdict"] == "PASS" for r in results.values()) else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--exe", required=True)
    r.add_argument("--manager", required=True)
    r.add_argument("--game", required=True)
    r.add_argument("--downloads", required=True)
    r.add_argument("--work", required=True)
    r.add_argument("--scenarios", default=DEFAULT_SCENARIOS)
    r.add_argument("--only")
    r.add_argument("--report")
    r.add_argument("--tape-dir", default=DEFAULT_TAPE_DIR)
    r.add_argument("--repo", default=os.path.dirname(os.path.dirname(HERE)), help="the source tree, for build_variant.py")
    r.add_argument("--src-dol", default="H:/ShadowTheHedgehogDiskGC_Extract/EXTRACTED_DISKS/USA/sys/main.dol",
                   help="the NTSC-U main.dol code mods were written against (build_variant.py)")
    r.add_argument("--jp-voices", default=None, help="the player's PRS_VOICE_J.afs (default: where the game finds it)")
    g = sub.add_parser("gate")
    g.add_argument("--base", required=True)
    g.add_argument("--exe", required=True)
    g.add_argument("--game", required=True)
    g.add_argument("--work", required=True)
    g.add_argument("--repo", required=True)
    g.add_argument("--westopolis", default=DEFAULT_TAPE_DIR)
    g.add_argument("--final-haunt", default=DEFAULT_FH_DIR)
    g.add_argument("--parts", default="runs,behaviour", help="runs, behaviour or both")
    g.add_argument("--bash", help="the POSIX shell for tools/behaviour_run.sh (default: bash on PATH)")
    g.add_argument("--layout", default="H:/ShadowTheHedgehogDiskGC_Extract/wt-s1-integrate/.build/s1/layout-combined.txt")
    args = ap.parse_args()
    if args.cmd == "run" and args.jp_voices is None:
        # Where the runtime looks (gekko/voice_language.h): <game root>/voice_jp/, then ../EXTRACTED_DISKS/Japan/files/.
        for c in (os.path.join(args.game, "voice_jp", "PRS_VOICE_J.afs"),
                  os.path.join(os.path.dirname(os.path.abspath(args.game)), "EXTRACTED_DISKS", "Japan", "files", "PRS_VOICE_J.afs")):
            if os.path.exists(c):
                args.jp_voices = c
                break
    sys.exit(cmd_run(args) if args.cmd == "run" else cmd_gate(args))


if __name__ == "__main__":
    main()
