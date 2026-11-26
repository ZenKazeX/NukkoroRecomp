#!/usr/bin/env python3
"""Apply the chosen code mods: a PAL patch set, a patched DOL, a recompiled variant executable (docs/mods/codemods/README.md).

    python -I tools/mods/codemods/build_variant.py --repo <source tree> --manager <ModManager.exe> \
        --mods-dir <mods> --game <game data> --src-dol <NTSC-U main.dol> [--prepare-only] [--jobs 3]

1. Reads the chosen code items of the enabled mods from the manager (`code-plan`: one source of truth).
2. Turns each item into a PAL patch set with patchset.py (Gecko / MCM / a replacement DOL's diff, translated from
   the NTSC-U or Reloaded executable, injected code relocated and verified).
3. Decides every patch, never silently:
     apply     static patches and hook+cave pairs whose words land in translated code (the recompiler emits it),
               written into a copy of the PAL DOL; caves get function rows so they are translated too
     runtime   data writes that need the per-frame code engine (bss/heap, conditional, pointer codes)
     skip      ineffective (the function is a native C++ owner, lifted, pruned or never emitted), refused by the
               recompiler (a lowered site), low or unresolved confidence, templates needing a user value, or a
               collision with another chosen item (same address, overlapping caves)
4. Unless --prepare-only, configures a variant build tree <repo>/.build/variant (SHADOW_RECOMP_DOL = the patched copy,
   SHADOW_RECOMP_VARIANT_OF = the retail DOL, SHADOW_RECOMP_FUNCTIONS = a copy with the cave rows), builds it and
   copies the executable to <mods>/.variants/ShadowTheHedgehog-code-<signature>.exe.
5. Writes <mods>/.variants/<signature>.json: what was applied, sent to the engine, skipped and why. The manager's
   Play and the game use it only while the chosen code still has that signature.

Nothing is written into the game folder or the repository; the variant embeds the PAL DOL, so a built variant is
for this machine only (it must not be shared: it contains the game's code).
"""
import argparse
import hashlib
import importlib.util
import json
import os
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent


def load(name):
    spec = importlib.util.spec_from_file_location(name, HERE / f"{name}.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def run(cmd, **kw):
    p = subprocess.run(cmd, capture_output=True, **kw)
    return p.returncode, p.stdout.decode("utf-8", "replace") + p.stderr.decode("utf-8", "replace")


def dol_sections(raw):
    h = struct.unpack(">18I18I18I", raw[:0xD8])
    return [(h[i], h[18 + i], h[36 + i]) for i in range(18) if h[36 + i]]


def file_offset(secs, addr, length):
    for off, ad, sz in secs:
        if ad <= addr and addr + length <= ad + sz:
            return off + addr - ad
    return None


def gecko_sources(path):
    """{code name: [(a, b), ...]} of a Dolphin-style ini ([Gecko]/[ActionReplay] $name blocks) or a plain code text."""
    codes, name, section = {}, None, "gecko"
    text = Path(path).read_text(encoding="utf-8", errors="replace")
    for raw in text.splitlines():
        line = raw.strip()
        if line.startswith("["):
            section = line.strip("[]").lower()
            name = None
            continue
        if section not in ("gecko", "actionreplay"):
            continue
        if line.startswith("$"):
            name = line[1:].split("[", 1)[0].strip()
            codes.setdefault(name, [])
            continue
        parts = line.split()
        if name and len(parts) >= 2 and all(len(x) == 8 for x in parts[:2]):
            try:
                codes[name].append((int(parts[0], 16), int(parts[1], 16)))
            except ValueError:
                pass
    return codes


def gecko_text_source(path):
    lines = []
    for raw in Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
        parts = raw.split()
        if len(parts) >= 2 and all(len(x) == 8 for x in parts[:2]):
            try:
                lines.append((int(parts[0], 16), int(parts[1], 16)))
            except ValueError:
                pass
    return lines


def write_lines(addr, data):
    """Unconditional Gecko writes (00/02/04) that store `data` at PAL `addr`."""
    out, i = [], 0
    while i < len(data):
        a = addr + i
        hi = 0x01000000 if a & 0x01000000 else 0
        if (len(data) - i) >= 4 and a % 4 == 0:
            out.append((0x04000000 | hi | (a & 0x00FFFFFF), int.from_bytes(data[i:i + 4], "big")))
            i += 4
        elif (len(data) - i) >= 2 and a % 2 == 0:
            out.append((0x02000000 | hi | (a & 0x00FFFFFF), int.from_bytes(data[i:i + 2], "big")))
            i += 2
        else:
            out.append((0x00000000 | hi | (a & 0x00FFFFFF), data[i]))
            i += 1
    return out


def remap_gecko(lines, mapping, static_targets):
    """Source-region Gecko lines -> PAL lines through the translator's address pairs. Unconditional writes the
    variant already carries are dropped. Returns (lines, None) or (None, why)."""
    out, depth, i = [], 0, 0
    while i < len(lines):
        a, b = lines[i]
        t = a >> 24
        base = t & 0xFE
        follow = 0
        if base == 0x06:
            follow = (b + 7) // 8
        elif base == 0x08:
            follow = 1
        if base <= 0x2E and base not in (0x0A, 0x0C, 0x0E) and not (t & 0x10):
            src = 0x80000000 | (a & 0x01FFFFFF)
            src_aligned = src & ~1 if base >= 0x20 else src
            pal = mapping.get(src_aligned)
            if pal is None:
                return None, f"its line {a:08X} {b:08X} reaches 0x{src_aligned:08X}, which has no PAL address"
            if base < 0x20 and depth == 0 and pal in static_targets:
                i += 1 + follow
                continue
            if base < 0x20 and pal in static_targets:
                return None, f"a conditional write to 0x{pal:08X}, which is code built into the variant"
            low = (pal & 0x01FFFFFF) | (a & 1 if base >= 0x20 else 0)
            out.append(((t & 0xFE) << 24 | low, b))
            if base >= 0x20 and not (a & 1):
                depth += 1
        elif base >= 0x20 and base <= 0x2E:  # po-relative condition
            out.append((a, b))
            if not (a & 1):
                depth += 1
        else:
            if t == 0xE0:
                depth = 0
            elif t == 0xE2:
                depth = max(0, depth - (a & 0xFF))  # E2T000XX: XX endifs (T=1: else)
            elif t in (0x42, 0x4A) and not (a & 0x00110000):
                pal = mapping.get(b)
                if pal is None:
                    return None, f"its line {a:08X} {b:08X} loads a base address with no PAL counterpart"
                b = pal
            out.append((a, b))
        for k in range(follow):
            if i + 1 + k < len(lines):
                out.append(lines[i + 1 + k])
        i += 1 + follow
    return out, None


def constant_accesses(words):
    """-> [(kind, address)] for the stores and loads of `words` (one routine's PowerPC words in order) whose base
    register holds a constant built by lis/ori/addi. kind is "store" or "load". A linear scan: registers are
    forgotten when overwritten by anything else."""
    regs, out = {}, []
    for w in words:
        op = w >> 26
        rd, ra, imm = (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
        simm = imm - 0x10000 if imm & 0x8000 else imm
        if op == 15 and ra == 0:                       # lis
            regs[rd] = (imm << 16) & 0xFFFFFFFF
            continue
        if op == 24:                                   # ori rA, rS, imm (rd field is rS)
            if rd in regs:
                regs[ra] = regs[rd] | imm
            else:
                regs.pop(ra, None)
            continue
        if op == 14 and ra != 0:                       # addi
            if ra in regs:
                regs[rd] = (regs[ra] + simm) & 0xFFFFFFFF
            else:
                regs.pop(rd, None)
            continue
        if op in (36, 37, 38, 39, 44, 45, 47, 52, 53, 54, 55):   # stores (d-form)
            if ra in regs and ra != 0:
                out.append(("store", (regs[ra] + simm) & 0xFFFFFFFF))
            if op in (37, 39, 45, 53, 55):
                regs.pop(ra, None)
            continue
        if op in (32, 33, 34, 35, 40, 41, 42, 43, 46, 48, 49, 50, 51):  # loads (d-form)
            if ra in regs and ra != 0:
                out.append(("load", (regs[ra] + simm) & 0xFFFFFFFF))
            regs.pop(rd, None)
            continue
        if op == 31:
            xo = (w >> 1) & 0x3FF
            if xo in (151, 215, 407, 663, 727, 183, 247, 439, 695, 759, 150, 918, 662):  # indexed stores
                continue
            regs.pop(rd, None)                         # most X-form ops write rD (or rA: forget both)
            regs.pop(ra, None)
            continue
        if op in (16, 18, 19):
            continue
        regs.pop(rd, None)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--repo", required=True, help="the source tree (tools/, extract/, CMakeLists.txt)")
    ap.add_argument("--manager", required=True)
    ap.add_argument("--mods-dir", required=True)
    ap.add_argument("--game", required=True, help="game data folder (sys/main.dol, files/)")
    ap.add_argument("--src-dol", required=True, help="the NTSC-U (GUPE8P) main.dol the code was written against")
    ap.add_argument("--prepare-only", action="store_true", help="decide and report; no build")
    ap.add_argument("--allow-medium", action="store_true", default=True,
                    help="apply medium-confidence patches (default: yes, flagged in the report)")
    ap.add_argument("--apply-named", action="store_true",
                    help="also apply patches in functions the runtime enters its own way (translated-named); unproven")
    ap.add_argument("--jobs", type=int, default=3)
    ap.add_argument("--generator", default="Visual Studio 18 2026")
    ap.add_argument("--dolrecon", default=None, help="SHADOW_DOLRECON_DIR (default: <repo>/../wt-opt-integrate/.build/dolrecon)")
    a = ap.parse_args()

    repo, mods_dir, game = Path(a.repo), Path(a.mods_dir), Path(a.game)
    variants = mods_dir / ".variants"
    work = variants / "work"
    work.mkdir(parents=True, exist_ok=True)
    pal_dol = game / "sys" / "main.dol"

    rc, text = run([a.manager, "--mods-dir", str(mods_dir), "--game-data", str(game), "code-plan"])
    if rc != 0:
        sys.exit("manager code-plan failed: " + text[-400:])
    plan = json.loads(text)
    sig = plan["signature"]
    report = {"signature": sig, "started": time.strftime("%Y-%m-%d %H:%M:%S"), "items": [], "applied": [],
              "runtime": [], "skipped": [], "exe": None, "build": None}
    if not sig:
        print("no code is chosen: nothing to do")
        return 0

    patchset = HERE / "patchset.py"
    classify = load("classify_sites")
    funcs = classify.read_functions(str(repo / "extract" / "functions.csv")) if hasattr(classify, "read_functions") else None
    own = classify.Ownership(str(repo / "test_data" / "mods" / "codemods" / "pal_code_ownership.json"),
                             sorted(funcs) if funcs else None)

    taken = {}      # PAL word address -> patch id (collisions between items)
    caves = []      # (start, end, patch id)
    apply, runtime, skipped, rows = [], [], [], []

    def feature_of(p):
        """The unit a patch belongs to: an MCM entry, a Gecko code, a translator feature; else the whole item."""
        prov = p.get("provenance") or {}
        name = prov.get("feature") or prov.get("entry") or prov.get("code")
        return f"{p.get('_item')}#{name}" if name else f"{p.get('_item')}#(whole item)"

    def skip(p, why):
        skipped.append({"id": p.get("id"), "address": p.get("address"), "item": p.get("_item"), "why": why,
                        "feature": feature_of(p), "provenance": p.get("provenance")})

    scratch_map = work / "scratch-map.json"  # one map for all items of this build: their scratch never overlaps
    if scratch_map.exists():
        scratch_map.unlink()

    item_inputs = {}  # "mod:item" -> (kind, path)
    for item in plan["items"]:
        kind, path = item["input"].split(":", 1)
        item_inputs[f"{item['mod']}:{item['item']}"] = (kind, path)
        out = work / f"{item['mod']}--{item['item'].replace(':', '_').replace('/', '_')}"
        if out.exists():
            shutil.rmtree(out)  # results of an earlier build must never be read as this one's
        out.mkdir(parents=True)
        cmd = [sys.executable, "-I", str(patchset), "--src-dol", a.src_dol, "--pal-dol", str(pal_dol),
               "--address-map", str(repo / "extract" / "address-maps" / "GUPE8P.map"),
               "--pal-functions", str(repo / "extract" / "functions.csv"),
               "--source-region", "GUPE8P", "--out-dir", str(out), "--scratch-map", str(scratch_map)]
        mcm_dirs = []
        if kind == "dol-diff":
            # An executable that ships with its MCM libraries is taken apart into its named entries (features), so one
            # entry that cannot work here costs only itself; what the DOL changes beyond them is reported as residue.
            codes_root = Path(path)
            while codes_root.name.lower() != "codes" and codes_root.parent != codes_root:
                codes_root = codes_root.parent
            if codes_root.name.lower() == "codes":
                mcm_dirs = sorted({t.parent for t in codes_root.rglob("*.txt")
                                   if "-==-" in t.read_text(encoding="utf-8", errors="replace")})
            for d in mcm_dirs:
                cmd += ["--input", f"mcm:{d}"]
            if not mcm_dirs:
                cmd += ["--input", f"{kind}:{path}"]
            cmd += ["--mod-dol", path]
        else:
            cmd += ["--input", f"{kind}:{path}"]
        rc, log = run(cmd)
        sets = sorted(out.glob("*.patchset.json"))
        if rc != 0 or not sets:
            report["items"].append({"item": item, "error": log[-600:]})
            continue
        docs = [json.loads(f.read_text(encoding="utf-8")) for f in sets]
        doc = {"patches": [p for d in docs for p in d["patches"]],
               "unresolved": [u for d in docs for u in d.get("unresolved", [])],
               "extra": {k: v for d in docs for k, v in (d.get("extra") or {}).items()}}
        patches = doc["patches"]
        if kind == "gecko-ini":  # one named code of the file
            # "gecko:<name>", or "gecko:<name> [<ini path>]" when another ini of the mod offers the same name (the
            # manager cuts " [..." off every code name, so the suffix is always the path)
            name = item["item"].split(":", 1)[1].split(" [", 1)[0]
            patches = [p for p in patches if (p.get("provenance") or {}).get("code") == name]
        extra = doc.get("extra") or {}
        unresolved = doc.get("unresolved", [])
        if kind == "gecko-ini":  # the file's other codes' unresolved lines are not this item's
            mine = {p.get("id") for p in patches}
            unresolved = [u for u in unresolved if u.get("id") in mine]
        report["items"].append({"item": item, "patches": len(patches), "unresolved": len(unresolved),
                                "mcm_libraries": [str(d) for d in mcm_dirs],
                                "dol_residue_unexplained_words": extra.get("residue_unexplained_words")})
        for u in unresolved:
            skipped.append({"item": item["item"], "why": "unresolved by the translator", "detail": u})
        for p in patches:
            p["_item"] = f"{item['mod']}:{item['item']}"
            conf = p.get("confidence")
            if p.get("needs_user_value"):
                skip(p, "a template code: it needs a value the user must fill in")
                continue
            if any("heap/scratch" in n and "relocated" not in n for n in (p.get("notes") or [])):
                skip(p, "its code uses fixed heap/scratch addresses of the other region that are not relocated")
                continue
            if conf in ("low", "unresolved") or (conf == "medium" and not a.allow_medium):
                skip(p, f"translation confidence {conf}")
                continue
            if p["kind"] == "runtime":
                runtime.append(p)
                continue
            if not p.get("new_bytes") or not p.get("address"):
                skip(p, "the translator gave no PAL bytes or address for it" + (f" ({p.get('kind')})" if p.get("kind") else ""))
                continue
            addr = int(p["address"], 16)
            nbytes = len(p["new_bytes"]) // 2
            if p.get("role") == "cave":
                end = addr + nbytes
                clash = [c for c in caves if c[0] < end and addr < c[1]]
                if clash:
                    skip(p, f"its cave overlaps the cave of {clash[0][2]} (a shared cave allocator is not built)")
                    continue
                caves.append((addr, end, p["_item"]))
                rows.append((addr, nbytes))
                apply.append(p)
                continue
            cls, detail = own.classify(addr)
            verdict = classify.VERDICT.get(cls, cls)
            p["_class"] = cls
            if verdict == "ineffective":
                skip(p, f"no effect in this build: {cls} ({detail})")
                continue
            if verdict == "build-refused":
                skip(p, f"refused by the recompiler: {detail}")
                continue
            if cls == "translated-named" and not a.apply_named:
                # The runtime enters this function its own way (a native caller tail-calls it, an HLE hook...), so
                # its calling convention can differ from the original's; injected code written for the original
                # (Reloaded's timer hooks in 0x80337DD4 returned into the native frame limiter and stopped the game)
                # is not applied unless asked for (--apply-named) and proven.
                skip(p, f"the runtime enters this function its own way ({detail}); not applied without --apply-named")
                continue
            words = range(addr, addr + max(4, nbytes), 4)
            clash = [taken[w] for w in words if w in taken]
            if clash:
                skip(p, f"the same address is changed by {clash[0]}")
                continue
            for w in words:
                taken[w] = p["_item"]
            if cls.startswith("data") and p.get("pal_section") in (None, "", "bss", "sbss", "sbss2", "heap"):
                runtime.append(p)  # bss / heap: not in the DOL file, only a running game holds it
                continue
            apply.append(p)

    # A feature applies whole or not at all: a hook without its cave, or half of a mechanic, is worse than none.
    def atomic():
        nonlocal apply, runtime
        failed = {}
        for x in skipped:
            if x.get("feature") and not x.get("whole_feature"):
                failed.setdefault(x["feature"], x["why"])
        if not failed:
            return 0
        n = 0
        for lst_name in ("apply", "runtime"):
            lst = apply if lst_name == "apply" else runtime
            keep = []
            for p in lst:
                f = feature_of(p)
                if f in failed:
                    skipped.append({"id": p.get("id"), "address": p.get("address"), "item": p.get("_item"),
                                    "feature": f, "whole_feature": True, "provenance": p.get("provenance"),
                                    "why": f"part of feature {f.split('#', 1)[1]}, which cannot be applied whole: {failed[f]}"})
                    n += 1
                else:
                    keep.append(p)
            if lst_name == "apply":
                apply = keep
            else:
                runtime = keep
        return n

    dropped_by_feature = atomic()

    # Relocated "scratch" (docs/mods/codemods/TRANSLATION.md): the translator moves every constant address outside the
    # source DOL image into the PAL scratch region. That is right for memory the mod keeps for itself, wrong for a game
    # heap object the source region's heap layout happens to put there (Reloaded reads a pointer at NTSC 0x807D71A0
    # that its game allocated; the empty scratch word made the weapon code load from null). A cluster counts as the
    # mod's own memory only when some applied mod code stores into it; features that read a cluster nothing writes
    # are skipped whole.
    smap = work / "scratch-map.json"
    clusters = []
    if smap.exists():
        for c in json.loads(smap.read_text(encoding="utf-8")).get("scratch", []):
            clusters.append((int(c["pal_range"][0], 16), int(c["pal_range"][1], 16), c["ntsc_range"][0]))
    if clusters:
        def accesses(p):
            data = bytes.fromhex(p.get("new_bytes") or "")
            return constant_accesses([int.from_bytes(data[i:i + 4], "big") for i in range(0, len(data) - 3, 4)])

        acc = {id(p): accesses(p) for p in apply}
        written = set()  # words of the scratch region some applied code stores to
        for p in apply:
            written.update(x & ~3 for kind, x in acc[id(p)] if kind == "store")
        for p in runtime:  # engine writes count too
            written.add(int(p["address"], 16) & ~3)

        def unwritten_read(x):
            return any(c0 <= x < c1 for c0, c1, _ in clusters) and (x & ~3) not in written
        report["scratch_clusters"] = [{"pal": f"0x{c0:08X}-0x{c1:08X}", "source": src,
                                       "words_written_by_mod": sum(1 for x in written if c0 <= x < c1)}
                                      for c0, c1, src in clusters]
        bad = 0
        for p in list(apply):
            for kind, x in acc[id(p)]:
                if kind == "load" and unwritten_read(x):
                    c0, c1, src = next(c for c in clusters if c[0] <= x < c[1])
                    skip(p, f"it reads 0x{x:08X} (relocated from the source region's heap near {src}), which no applied "
                            f"code writes: a game object the source region's heap layout put there; its PAL address "
                            f"is not known")
                    apply.remove(p)
                    bad += 1
                    break
        if bad:
            dropped_by_feature += atomic()

    # Codes for the in-game code engine (docs/mods/codemods/ENGINE.md): every feature with runtime patches becomes one
    # PAL Gecko code. Gecko items keep their own structure (conditions, else, registers) with the translator's
    # address pairs; other items' runtime patches are unconditional data writes. A feature whose code cannot be
    # written this way is skipped whole.
    def runtime_codes():
        static_targets = set()
        for p in apply:
            if p.get("address") and p.get("new_bytes"):
                a0 = int(p["address"], 16)
                static_targets.update(range(a0, a0 + len(p["new_bytes"]) // 2))
        by_feature = {}
        for p in runtime:
            by_feature.setdefault(feature_of(p), []).append(p)
        codes, failed = [], []
        source_cache = {}
        for f, ps in sorted(by_feature.items()):
            item = ps[0]["_item"]
            kind, path = item_inputs.get(item, ("", ""))
            mine = [q for q in apply + runtime if feature_of(q) == f]
            needs_variant = any(feature_of(q) == f for q in apply)
            lines, why = None, None
            if kind in ("gecko-ini", "gecko-text"):
                if path not in source_cache:
                    source_cache[path] = gecko_sources(path) if kind == "gecko-ini" else {"": gecko_text_source(path)}
                name = (ps[0].get("provenance") or {}).get("code") or ""
                src_lines = source_cache[path].get(name if kind == "gecko-ini" else "")
                if not src_lines:
                    why = "its source lines were not found again"
                else:
                    mapping = {}
                    for q in mine:
                        if q.get("source_address") and q.get("address"):
                            mapping[int(q["source_address"], 16)] = int(q["address"], 16)
                        for c in q.get("condition") or []:
                            if c.get("source_addr") and c.get("addr") and not c.get("indirect"):
                                mapping[int(c["source_addr"], 16)] = int(c["addr"], 16)
                    lines, why = remap_gecko(src_lines, mapping, static_targets)
            else:
                lines = []
                for q in ps:
                    if q.get("condition"):
                        why = "a conditional write outside a Gecko code (no source lines to keep its structure)"
                        break
                    lines += write_lines(int(q["address"], 16), bytes.fromhex(q["new_bytes"]))
            if why:
                failed.append((f, why, ps))
                continue
            codes.append({"name": f, "lines": [f"{x:08X} {y:08X}" for x, y in lines], "needs_variant": needs_variant})
        for f, why, ps in failed:
            for q in ps:
                skip(q, f"its in-game code cannot be written: {why}")
        return codes, bool(failed)

    engine_codes, any_failed = runtime_codes()
    if any_failed:
        dropped_by_feature += atomic()
        dropped = {x["feature"] for x in skipped if x.get("feature")}
        engine_codes = [c for c in engine_codes if c["name"] not in dropped]
    caves = [c for c in caves if any(p.get("role") == "cave" and int(p["address"], 16) == c[0] for p in apply)]

    # Injected code: walk it from every hook that is applied. Only reachable cave code is translated (rows sized to
    # what the walk reaches); a hook whose cave code jumps into the middle of code this build replaced (a native or
    # lifted owner) cannot work and is skipped with its reason; unreachable cave code (its hook was skipped) is left
    # out of the function table, so the recompiler never sees it.
    def in_caves(x):
        return any(c0 <= x < c1 for c0, c1, _ in caves)

    cave_words = {}
    for p in apply:
        if p.get("role") == "cave":
            base, data = int(p["address"], 16), bytes.fromhex(p["new_bytes"])
            for i in range(0, len(data) - 3, 4):
                cave_words[base + i] = int.from_bytes(data[i:i + 4], "big")

    def branch_target(word, at):
        op = word >> 26
        if op == 18:
            li = word & 0x03FFFFFC
            if li & 0x02000000:
                li -= 0x04000000
            return (li if word & 2 else at + li) & 0xFFFFFFFF
        if op == 16:
            bd = word & 0xFFFC
            if bd & 0x8000:
                bd -= 0x10000
            return (bd if word & 2 else at + bd) & 0xFFFFFFFF
        return None

    def bad_exit(target):
        cls, detail = own.classify(target)
        if cls in ("native-owner", "lifted-owner"):
            r = own._in(own.owner_starts, own.d["owners"], target)
            if r is not None and r[0] != target:
                return f"its injected code jumps into the middle of 0x{r[0]:08X} ({cls}: {detail})"
        if cls in ("pruned", "unemitted") and not in_caves(target):
            return f"its injected code jumps to code this build never emits ({cls}, 0x{target:08X})"
        return None

    def walk(entry):
        """-> (end of the reached code, problems) for one routine; follows branches inside the caves."""
        seen, todo, end, problems = set(), [entry], entry, []
        while todo:
            at = todo.pop()
            while at not in seen and at in cave_words:
                seen.add(at)
                end = max(end, at + 4)
                w = cave_words[at]
                op = w >> 26
                t = branch_target(w, at)
                if t is not None:
                    if in_caves(t):
                        todo.append(t)
                    else:
                        why = bad_exit(t)
                        if why:
                            problems.append(why)
                    if op == 18 and not (w & 1):  # b: no fall-through
                        break
                    if op == 16 and (w >> 21) & 0x14 == 0x14:  # bc always
                        break
                elif op == 19 and ((w >> 1) & 0x3FF) in (16, 528) and (w >> 21) & 0x14 == 0x14:  # blr / bctr always
                    break
                elif op == 19 and ((w >> 1) & 0x3FF) == 50:  # rfi
                    break
                at += 4
        return end, problems

    hooks = []  # (patch, target) for applied patches outside the caves that branch into them
    for p in apply:
        if p.get("role") == "cave":
            continue
        base, data = int(p["address"], 16), bytes.fromhex(p["new_bytes"])
        for i in range(0, len(data) - 3, 4):
            t = branch_target(int.from_bytes(data[i:i + 4], "big"), base + i)
            if t is not None and in_caves(t):
                hooks.append((p, t))
    routines, dropped_hooks = {}, set()
    for p, t in hooks:
        if t not in routines:
            routines[t] = walk(t)
        end, problems = routines[t]
        # routines called from this one (bl inside the caves) are walked too and their problems count
        if problems:
            dropped_hooks.add(id(p))
            skip(p, problems[0])
    apply = [p for p in apply if id(p) not in dropped_hooks]
    dropped_by_feature += atomic()  # a hook dropped for its cave code takes its feature with it
    live_entries = {t for p, t in hooks if id(p) not in dropped_hooks}
    # entries reached by bl from live routines
    frontier = list(live_entries)
    while frontier:
        e = frontier.pop()
        end, _ = routines.setdefault(e, walk(e))
        for at in range(e, end, 4):
            w = cave_words.get(at)
            if w is not None and w >> 26 == 18 and (w & 1):
                t = branch_target(w, at)
                if in_caves(t) and t not in live_entries:
                    live_entries.add(t)
                    routines.setdefault(t, walk(t))
                    frontier.append(t)
    cave_rows = sorted((e, routines[e][0] - e) for e in live_entries if routines[e][0] > e)
    # Unreachable injected code (its hook was skipped) can still be found by the recompiler through pointers kept
    # in the cave data. Its branches into the middle of replaced code would stop the build, so each becomes `blr`.
    # No applied hook reaches that code, so this changes nothing that runs; the report lists every such word.
    live_words = set()
    for e, size in cave_rows:
        live_words.update(range(e, e + size, 4))
    neutralized = []
    for p in apply:
        if p.get("role") != "cave":
            continue
        base, data = int(p["address"], 16), bytearray.fromhex(p["new_bytes"])
        for i in range(0, len(data) - 3, 4):
            at = base + i
            if at in live_words:
                continue
            w = int.from_bytes(data[i:i + 4], "big")
            t = branch_target(w, at)
            if t is not None and not in_caves(t) and bad_exit(t):
                data[i:i + 4] = (0x4E800020).to_bytes(4, "big")
                neutralized.append(f"0x{at:08X} (to 0x{t:08X})")
        p["new_bytes"] = data.hex()
    report["unreachable_branches_neutralized"] = neutralized
    if neutralized:
        print(f"caves: {len(neutralized)} branch(es) in unreachable injected code into replaced code made blr: "
              + ", ".join(neutralized[:4]))
    report["cave_routines"] = {"live": len(cave_rows), "hooks_dropped": len(dropped_hooks)}
    report["patches_dropped_with_their_feature"] = dropped_by_feature
    features_applied = sorted({feature_of(p) for p in apply + runtime})
    features_skipped = sorted({x["feature"] for x in skipped if x.get("feature")} - set(features_applied))
    report["features"] = {"applied": features_applied, "skipped": features_skipped}
    print(f"features: {len(features_applied)} applied, {len(features_skipped)} skipped whole")
    print(f"caves: {len(cave_rows)} reachable routines, {len(dropped_hooks)} hooks dropped (their code cannot run here)")

    report["applied"] = [{k: v for k, v in p.items() if not k.startswith("_") or k in ("_item", "_class")} for p in apply]
    report["runtime"] = runtime
    # Read by the game at start (shadow::mods::prepare_runtime) when the chosen codes have this signature.
    report["engine_codes"] = engine_codes
    report["skipped"] = skipped
    counts = {"applied": len(apply), "runtime": len(runtime), "skipped": len(skipped)}
    report["counts"] = counts
    print(f"signature {sig}: {counts['applied']} patches to build in, {counts['runtime']} for the code engine, "
          f"{counts['skipped']} skipped")
    for s in skipped[:12]:
        print("  skipped:", s.get("item"), s.get("address"), "-", s["why"])

    # The patched DOL in a variant sys/ folder (the executable embeds boot.bin and bi2.bin beside it).
    vsys = work / "sys"
    if vsys.exists():
        shutil.rmtree(vsys)
    shutil.copytree(game / "sys", vsys)
    raw = bytearray(pal_dol.read_bytes())
    secs = dol_sections(raw)
    for p in apply:
        addr, data = int(p["address"], 16), bytes.fromhex(p["new_bytes"])
        off = file_offset(secs, addr, len(data))
        if off is None:
            sys.exit(f"{p['address']} ({p['_item']}) is not inside a file-backed DOL section")
        raw[off:off + len(data)] = data
    (vsys / "main.dol").write_bytes(bytes(raw))
    # Function rows for the injected code. A cave holds one routine or many (a replacement DOL's cave area is one
    # block with every routine of the mod): an entry is every place a branch lands in a cave. The stock table's
    # rows for the interrupt-vector stubs the caves overwrite are dropped (docs/mods/codemods/TRANSLATION.md).
    functions_copy = work / "functions.csv"
    import csv
    with open(repo / "extract" / "functions.csv", encoding="utf-8", newline="") as f:
        rd = csv.reader(f)
        header = next(rd)
        table = list(rd)
    if caves:
        cols = {n: i for i, n in enumerate(header)}
        before = len(table)
        table = [r for r in table if not in_caves(int(r[cols["addr"]]))]
        dropped = before - len(table)
        for e, size in cave_rows:
            r = [""] * len(header)
            r[cols["addr_hex"]], r[cols["addr"]] = f"0x{e:08x}", str(e)
            r[cols["name"]], r[cols["size"]], r[cols["ins"]] = f"cave_{e:08x}", str(size), str(size // 4)
            table.append(r)
        table.sort(key=lambda r: int(r[cols["addr"]]))
        report["cave_rows"] = {"rows": len(cave_rows), "stock_rows_dropped": dropped}
    with open(functions_copy, "w", encoding="utf-8", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(header)
        w.writerows(table)
    report["patched_dol_md5"] = hashlib.md5(bytes(raw)).hexdigest()

    exe_name = f"ShadowTheHedgehog-code-{sig}.exe"
    if apply and not a.prepare_only:
        tree = repo / ".build" / "variant"
        dolrecon = a.dolrecon or str(repo.parent / "wt-opt-integrate" / ".build" / "dolrecon")
        t0 = time.time()
        run([sys.executable, "-B", str(repo / "tools" / "platform" / "regen_manifest.py")], cwd=str(repo))
        cfg = ["cmake", "-S", str(repo), "-B", str(tree), "-G", a.generator, "-A", "x64",
               "-DSHADOW_RENDER_LIFTED=ON", "-DSHADOW_BUILD_REFERENCE=ON", "-DSHADOW_SHIPPING=OFF",
               "-DSHADOW_BUILD_TESTS=OFF", "-DSHADOW_ENABLE_PLUME=OFF", "-DSHADOW_WATCH_SYMBOLS=ON",
               "-DSHADOW_RECOMP_SHARDS=48", "-DSHADOW_RECOMP_COMPILE_JOBS=4", "-DSHADOW_DEPLOY_DIR=",
               f"-DSHADOW_RECOMP_DOL={(vsys / 'main.dol').as_posix()}", f"-DSHADOW_RECOMP_VARIANT_OF={pal_dol.as_posix()}", f"-DSHADOW_CODE_VARIANT={sig}",
               f"-DSHADOW_RECOMP_FUNCTIONS={functions_copy.as_posix()}", f"-DSHADOW_DOLRECON_DIR={dolrecon}"]
        rc, log = run(cfg)
        (work / "configure.log").write_text(log, encoding="utf-8")
        if rc != 0:
            report["build"] = {"ok": False, "stage": "configure", "log": str(work / "configure.log")}
        else:
            rc, log = run(["cmake", "--build", str(tree), "--config", "Release", "--target", "ShadowTheHedgehog", "--",
                           f"-m:{a.jobs}", "-v:m"])
            (work / "build.log").write_text(log, encoding="utf-8")
            exe = tree / "bin" / "Release" / "ShadowTheHedgehog.exe"
            if rc == 0 and exe.exists():
                shutil.copy(exe, variants / exe_name)
                report["exe"] = exe_name
                report["exe_md5"] = hashlib.md5((variants / exe_name).read_bytes()).hexdigest()
                report["build"] = {"ok": True, "seconds": round(time.time() - t0)}
            else:
                report["build"] = {"ok": False, "stage": "build", "log": str(work / "build.log")}
        print("build:", report["build"])
    elif not apply:
        report["build"] = {"ok": True, "note": "nothing to build in: the stock executable runs (runtime codes only)"}

    (variants / f"{sig}.json").write_text(json.dumps(report, indent=1), encoding="utf-8")
    print("report:", variants / f"{sig}.json")
    return 0 if (report["build"] or {}).get("ok", True) else 1


if __name__ == "__main__":
    sys.exit(main())
