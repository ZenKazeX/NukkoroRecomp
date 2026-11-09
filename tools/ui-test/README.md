# UI test scripts and layout measurement

Development-only. None of this is reachable from normal gameplay.

## Scripts

Fed to the runtime with `--ui-test-script <file>`. Format is documented in
`runtime/include/gekko/ui_test.h`; briefly:

```
wait 30                 advance 30 VI fields with no input
press START 6           hold Start for 6 VI fields
press A,UP 4            hold several buttons
stick 0 -100 10         hold the analog stick
label enter-loading     name the following steps in the log
loop                    restart from the first step
```

| Script | Reaches | Budget |
|---|---|---|
| `mash.uitest` | mashes A and START forever — use to explore an unknown flow | any |
| `to-story.uitest` | boot → title → main menu → 1P GAME submenu, then settles | 20e9 |
| `to-select.uitest` | …→ SELECT MODE stage grid, then moves the cursor forever | 26e9 |
| `to-loading.uitest` | …→ confirms a stage → loading screen → stage title → gameplay | 34e9 |
| `autosave.uitest` | holds the boot autosave dialog open, including its opening animation | 2.5e9 |
| `fade-loop.uitest` | walks into a submenu and straight back out, for ever — a complete fade-out/fade-in pair per iteration | 35e9 |
| `menu-cycle.uitest` | main menu → 1P submenu → Select mode → back out, repeatedly — every menu enter and exit transition | 40e9 |
| `pause.uitest` | …→ into gameplay, then opens and closes the pause menu repeatedly | 40e9 |
| `aspect-sweep.uitest` | a settled tour that stops on each menu and then holds, for frame-for-frame A/B across aspect ratios | 30e9 |
| `westopolis-play.uitest` | ...into Westopolis and then actually plays - stick held forward with jumps - so mission-gated HUD can come up | 50e9 |

Approximate budgets on the boot route: ~8.7M retired instructions per VI field.
Title ≈ 9e9, main menu ≈ 13e9, 1P submenu ≈ 20e9, Select menu ≈ 26e9, gameplay ≈ 34e9.

The built-in modes behind `--ui-test <mode>` (`boot`, `fade`, `loading`, `select`, `hud`) are
starting points only; these files are what the real sequences were derived with, without a rebuild
per attempt.

## Example

```bash
./ShadowTheHedgehog.exe --game-data "H:/ShadowTheHedgehogDiskGC_Extract" --headless \
  --width 1920 --height 1080 --aspect 16:9 --budget 34000000000 --no-call-trace \
  --ui-test-script GekkoForge/tools/ui-test/to-loading.uitest \
  --filmstrip out169 --filmstrip-stride 20 --hud-report out169.csv --ui-overlay
```

`--game-data` must use forward slashes under bash, or the path is eaten and the runtime exits 2.

## --service-interval is MANDATORY for layout work

A headless run installs **no host service**, so it flushes the GX batch only at recovered copy
boundaries. The windowed runtime services the host every 65,536 retired instructions and flushes
every time — measured 11.0 geometry-carrying flushes per displayed frame on the stage-load route,
26.8 on Westopolis, 47.0 on the boot route. Every 2D regression found in the 2026-08-28 passes was
invisible without it. Always pass `--service-interval 65536`.

## flicker-scan.py

The instability check. A flicker the player sees is, in this layer, a primitive whose **guest** box
is identical between two frames but whose **mapped** box is not — the guest did not move it, the
host did. Nothing else distinguishes that from the guest animating something.

```bash
python GekkoForge/tools/ui-test/flicker-scan.py <hud-report.csv> [--from N] [--to N]
```

Current expected baseline: **20 host-side movements, every one exactly 1 change per widget** — the
one-frame settle when a gameplay HUD widget first appears. Menu and fade routes measure **0**.

## cast-map.py

The decomposition table. The `cast` column of a `--hud-report` names the authored Chao::CSD cast
that drew each primitive; cast pointers of one loaded `.gncp` are contiguous on a `0x50` stride, so
clustering them recovers "which asset" and the offset within a cluster is the authored cast index.

```bash
python GekkoForge/tools/ui-test/cast-map.py <hud-report.csv> [--gap 0x4000] [--top 40]
```

Prints, per asset region, one row per cast: index, frames drawn, guest canvas box, the anchors it
took, and what its geometry *allows*. The last column is advisory — what is geometrically possible,
not what the composition wants.

## anchor-ownership-audit.py

A **static** check - it needs no route, which matters because the harness cannot reach most of the
game's HUD states. It finds widgets whose ART and whose TEXT would take different anchors: a CSD
widget that is not in `kHudStripDrawSites` (so it falls to `Center`) whose own text is drawn through
a site that is in `kHudStripTextSites` (so it takes `Left`/`Right` by canvas thirds). A parent on
`Center` with children on an edge can only line up at 4:3. This is the class of defect that produced
the mission-objective window bug.

```bash
python GekkoForge/tools/ui-test/anchor-ownership-audit.py [--depth 5]
```

Two things make it precise, and both were learned by the tool getting the wrong answer first:

* it **stitches virtual dispatch** - the extractor splits a function at a linking indirect branch
  and prints it as `bctr`/`blr`, so a vtable call breaks the callee graph. Without stitching the
  audit misses the very bug it was written for (37,724 edges are added);
* it **attributes per call site and stops at other widgets** - one function often draws several
  widgets, and a shared subsystem is reachable from any of them.

Current expected output: **one candidate**, `0x80201B2C` (TGiScoreDisplay), which is deliberately
not fixed - it draws zero primitives in 1P and can only matter in 2P, where the layout is skipped
entirely and where an edge anchor would be wrong anyway because edges are measured against the full
640 canvas. Re-run it whenever `kHudStripDrawSites` or `kHudStripTextSites` changes; the script
prints both tables so drift against `system.cpp` is visible in its own output.

## widescreen-acceptance.py

Checks a directory of `--hud-report` CSVs against the layout invariants: every anchored primitive
satisfies `x' = A + s*(x - A)`, the timer is centred, the Hero/Dark gauge margins are the original
margins scaled by `s`, full-canvas draws are not moved, and nothing that started inside the canvas
leaves it.

```bash
python GekkoForge/tools/ui-test/widescreen-acceptance.py <dir-of-csvs>
```

It keys the expected aspect off the CSV filename prefix (`a169_*`, `a219_*`, `a329_*`,
`a1610_*`, `acustom`, `aodd`), so name captures accordingly.

## scale-fit.py — the correction check

```bash
python GekkoForge/tools/ui-test/scale-fit.py <ref-4x3-dir> <wide-dir> --aspect 16:9
```

Fits every frame pair against the expected scale and against 1.0, and reports any frame that
received **no** correction. 0 is a pass. Verified at 16:9, 21:9, 16:10 and 32:9.

## Scale-fit check

The measurement that catches a screen receiving no widescreen correction at all: warp the 4:3 frame
by `x' = 320 + (x-320)/s` for `s` in {0.75, 1.00} and compare against the 16:9 frame of the same
index. 0.750 means the correction was applied; 1.000 means it was not. See
`SESSION_CONTINUATION_STATE.md` §6.

## Before handing a build over for testing

The `POST_BUILD` step copies the exe to the extract root, and **that copy fails silently while the
game is running** - `Device or resource busy`, visible in the MSBuild log only as a wall of
`error MSB3073` lines *after* the "Deploying" message. Testing a stale binary looks exactly like a
fix not working. Check the timestamps match:

```bash
ls -la --time-style=+%H:%M:%S GekkoForge/build-native/bin/Release/ShadowTheHedgehog.exe \
                              ShadowTheHedgehog.exe
```
