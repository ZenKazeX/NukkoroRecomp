#!/usr/bin/env python3
"""Find 2D widgets whose ART and whose TEXT would take different anchors.

This is the defect class that produced the mission-objective window bug (see
2D_UI_DECOMPOSITION_AND_WIDESCREEN_MAP.md section 6b): the window art is a Chao::CSD widget that is
NOT in kHudStripDrawSites, so it falls to the scene-space centre anchor, while its own counters are
glyph runs at a site that IS in kHudStripTextSites, so they take Left or Right by canvas thirds. A
parent on Center with children on an edge can only line up at 4:3.

The mismatch is visible in the recovered call graph without running anything, which matters because
the routes the --ui-test harness can drive do not reach most of the game's HUD states.

    python anchor-ownership-audit.py [--extract <dir>] [--depth 5]

Two things are needed to make it precise, and both were learned the hard way:

1. STITCH VIRTUAL DISPATCH. The extractor splits a function at a linking indirect branch and prints
   it as `bctr`/`blr`, so a vtable call breaks the callee graph exactly where an object-oriented
   draw path needs it. Without stitching, this audit MISSES the very bug it was written for:
   fn_800BC74C dispatches through a vtable at 0x800BC7D0 and the glyph run lives in the split-off
   continuation fn_800BC7D4.

2. ATTRIBUTE PER CALL SITE, NOT PER FUNCTION. One function often draws several widgets. fn_800B8ACC
   draws the objective window at `this+120` and a second widget at `this+44`; the counter loop
   belongs to the first only. Attributing every glyph the function can reach to both call sites
   reports the second as a false positive. Each Project::Render call site therefore owns only the
   calls made after it and before the next one.
"""
import argparse
import collections
import json
import os
import sys

PROJECT_RENDER_BL = "bl 0x8036e794"
GLYPH_RUN_BL = "bl 0x800bcba4"

# Mirrors GekkoForge/runtime/src/gekko/system.cpp. Kept by hand; the script prints them so drift is
# visible in its own output.
HUD_STRIP_DRAW_SITES = [0x8003FE44, 0x800BD3C8, 0x801FEC84, 0x8020015C, 0x802002E8, 0x800B8AFC]
HUD_STRIP_TEXT_SITES = [
    0x800BC898,
    0x801FED70, 0x801FEE30, 0x801FEEF4,
    0x80200200, 0x802002A4, 0x802003A0, 0x80200458, 0x80200490,
    0x8020175C, 0x802017F0, 0x80201BDC, 0x80201C08, 0x80201C34,
    0x80201C60, 0x80201D50,
]


def load(extract):
    path = os.path.join(extract, "functions.jsonl")
    if not os.path.exists(path):
        sys.exit("no functions.jsonl at %s" % path)
    funcs = {}
    for line in open(path, encoding="utf-8"):
        o = json.loads(line)
        funcs[o["addr"]] = o
    return funcs


def stitch_virtual_dispatch(funcs):
    """A recovered function that starts right after a linking indirect branch is its continuation."""
    stitched = 0
    for addr, o in funcs.items():
        extra = []
        for ins in o["ins"]:
            nxt = ins["pc"] + 4
            if ins["text"] in ("bctr", "blr") and nxt in funcs and nxt != addr:
                extra.append(nxt)
        if extra:
            o["callees"] = list(o.get("callees") or []) + extra
            stitched += len(extra)
    return stitched


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--extract", default=os.path.join(os.path.dirname(__file__),
                                                          "..", "..", "extract"))
    parser.add_argument("--depth", type=int, default=5)
    args = parser.parse_args()

    funcs = load(args.extract)
    stitched = stitch_virtual_dispatch(funcs)

    glyph_in = collections.defaultdict(list)
    for addr, o in funcs.items():
        for ins in o["ins"]:
            if ins["text"] == GLYPH_RUN_BL:
                glyph_in[addr].append(ins["pc"] + 4)

    # Per Project::Render call site, the callees invoked after it and before the next one.
    sites = {}
    for addr, o in funcs.items():
        current = None
        for ins in o["ins"]:
            text = ins["text"]
            if text == PROJECT_RENDER_BL:
                current = ins["pc"] + 4
                sites.setdefault(current, {"owner": addr, "roots": [], "own_glyphs": []})
            elif current is not None:
                if text == GLYPH_RUN_BL:
                    sites[current]["own_glyphs"].append(ins["pc"] + 4)
                elif text.startswith("bl 0x"):
                    sites[current]["roots"].append(int(text[5:], 16))

    # A function that owns a Project::Render of its own is ANOTHER widget. Its glyph runs belong to
    # that widget, not to whoever called into it, so the walk stops there. Without this the results
    # screen looks like a mismatch: fn_80202EB0 -> fn_80202F24 -> fn_80201618 -> fn_80201620 reaches
    # the gameplay timer/score sites at depth 3 through a shared subsystem, while the results
    # screen's own row site 0x80202FD0 is found at depth 1 and is correctly not edge-anchored.
    widget_owners = {info["owner"] for info in sites.values()}

    def reachable(roots, depth, self_owner):
        seen, out, frontier = set(roots), [], list(roots)
        for _ in range(depth):
            nxt = []
            for f in frontier:
                if f in widget_owners and f != self_owner:
                    continue
                out.extend(glyph_in.get(f, []))
                for callee in funcs.get(f, {}).get("callees", []) or []:
                    if callee not in seen:
                        seen.add(callee)
                        nxt.append(callee)
            frontier = nxt
        return sorted(set(out))

    print("kHudStripDrawSites: %s" % ", ".join("0x%08X" % s for s in HUD_STRIP_DRAW_SITES))
    print("%d Project::Render sites, depth %d, %d virtual-dispatch edges stitched\n"
          % (len(sites), args.depth, stitched))

    mismatches = []
    for ret in sorted(sites):
        info = sites[ret]
        art_edge = ret in HUD_STRIP_DRAW_SITES
        glyphs = sorted(set(info["own_glyphs"]) | set(reachable(info["roots"], args.depth, info["owner"])))
        edge_glyphs = [g for g in glyphs if g in HUD_STRIP_TEXT_SITES]
        note = ""
        if edge_glyphs and not art_edge:
            note = "  <== MISMATCH: art centred, its own text edge-anchored"
            mismatches.append(ret)
        elif art_edge and glyphs and not edge_glyphs:
            note = "  <== art edge-anchored, its own text is not"
            mismatches.append(ret)
        print("  0x%08X  owner %s  art=%-6s  own text: %s%s"
              % (ret, funcs[info["owner"]]["hex"], "EDGE" if art_edge else "center",
                 ", ".join("0x%08X%s" % (g, "*" if g in HUD_STRIP_TEXT_SITES else "")
                           for g in glyphs) or "-", note))

    print("\n%d candidate(s). * marks a glyph site that is edge-anchored today." % len(mismatches))
    if mismatches:
        print("A candidate is not a verdict - confirm the pairing in the owner before acting.")
    return 1 if mismatches else 0


if __name__ == "__main__":
    sys.exit(main())
