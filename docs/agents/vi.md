# vi: graphics.vi (GC_REMOVAL_PLAN row 10), 2026-11-07

Worktree `wt-agent-vi`, branch `agent/vi` (from 1c0c789). Every build, test, generator and game run went through
`tools/agents/capped_run.ps1` (10 % cap; job CPU shares quoted below).

## Analysis (names from the disassembly; the sdk-identify names agree where they exist)

Frontier rows of graphics.vi (removal_plan.json): 0x8037ECD4, 0x8037FD38, 0x8037FE68, 0x8037FED4, 0x8037FEDC, 0x80380138.

| address | SDK function | evidence |
|---|---|---|
| 0x8037ECD4 | VIWaitForRetrace | OSDisableInterrupts; `retraceCount` (r13+0x5BCC) read; loop `OSSleepThread(&r13+0x5BD4)` until it changes. NOT owned (below) |
| 0x8037FD38 | VIFlush | `changeMode |= flushMode`; `shdwChanged |= changed`; for each set bit of `changed` (cntlzd 0x803A2280 / shl2i 0x803A225C) `shdwRegs[i] = regs[i]`; `flushFlag = 1`; `NextBufAddr = HorVer.bufAddr` |
| 0x8037FE68 | VISetNextFrameBuffer | `HorVer.bufAddr = fb` (+0x120); r13+0x5C20 = 1; setFramebuffer 0x8037ED28 |
| 0x8037FED4 | VIGetNextFrameBuffer-style getter | `lwz r3, 0x5C18(r13)` (the buffer VIFlush made current) |
| 0x8037FEDC | VISetBlack | `+0x130 = black`; setVerticalRegs 0x8037EFFC |
| 0x80380138 | VIGetTvFormat | jump table 0x805631AC over `CurrTvMode` (r13+0x5C14): 0,3,6,7 -> 0; 1,4 -> 1; 2,5 -> the mode |
| 0x8037E4F0 | VISetPreRetraceCallback (sdk-identify: PROVEN) | `old = [r13+0x5BDC]; [r13+0x5BDC] = cb; return old` |

Already native before this work (native_video.cpp): VIInit/__VIInit, VIGetNextField, VIGetCurrentLine,
getCurrentFieldEvenOdd, the retrace delivery (`retrace_handler`: pre callback, commit of the staged display control /
clock registers, post callback, OSWakeupThread of the retrace queue).

## Decision: VIWaitForRetrace stays game code

It sleeps on the OS thread queue (OSSleepThread loop) and the CRI vsync / svtype-4 threads (0x803C53FC, 0x803C5454,
0x803C5730) enter it too: owning it is thread / interrupt-dispatch territory (cri-threads). The main session agreed.
what_if: owning all six frontier rows frees the same 5 rows as owning the five without it.

## What was written (runtime/src/gekko/native_video.cpp / .h)

Per the main session's rule (no rebuilding of the VI hardware register logic): the owners write only the guest words
game code reads.

- VIFlush: changeMode / flushFlag / NextBufAddr bookkeeping, and the staging of ONLY shadow registers 1 (display
  control) and 54 (clock select) with their shdwChanged bits: the one surviving reader is the native retrace commit
  (`NativeVideo::retrace_handler`, shdwRegs[1] and [54], shdwChanged), proven by the guest watch (Gate 3b). The other
  62 registers are no longer copied, the 64-bit loop is gone; `changed` is cleared as before.
- VISetNextFrameBuffer: `bufAddr` and the r13+0x5C20 flag only. RETIRED: setFramebuffer's four buffer-address words
  (static+0x124 / 0x128 / 0x13C / 0x140), registers 0x1C-0x2B and changed bits 0x10000 / 0x20000 / 0x1000 / 0x2000
  (+ 0x4000 / 0x8000 / 0x400 / 0x800 with a right-eye buffer).
- VISetBlack: the black flag (+0x130) only. RETIRED: setVerticalRegs' registers 0x00 and 0x0C-0x13 and changed bits
  0x80000000 / 0x01000000 / 0x02000000 / 0x00400000 / 0x00800000 (setVerticalRegs stays live: VIConfigure calls it).
- 0x8037FED4, VISetPreRetraceCallback, VIGetTvFormat: pure. VIGetTvFormat stops the machine for a mode > 7 (the original
  returned its caller's r31 there: garbage, never replicated; no such mode is committed).
- The opt-in UI probe on 0x80380138 (system.cpp) now chains to the owner as the VIInit probe does.

Shared list lines changed (append-only): `extract/native-owners.txt` (a commented block after VIGetCurrentLine:
0x8037FD38, 0x8037FE68, 0x8037FED4, 0x8037FEDC, 0x80380138, 0x8037E4F0), `extract/liveness-roots.txt`
(+0x8037E4F0, 0x8037FD38, 0x8037FE68, 0x8037FED4, 0x8037FEDC; 0x80380138 was already a root; the checked-in file is
otherwise stale against its generator - a baseline failure of native_liveness_roots - so it was NOT regenerated).
CMakeLists.txt and the ratchet are untouched.

## Gate 1: what_if (lock `generator`; cpu 2.9 % of the machine)

`--own` the five entries: **frees 5 rows** (baseline 751 -> 756), 0 revived, no refusal: 0x80380170, 0x80380178,
0x80380180 (VIGetTvFormat's jump-table targets) and 0x803A225C / 0x803A2280 (`__shl2i` / `__cntlzd`, VIFlush's only
users). Owning the sixth, 0x8037E4F0, was measured only as part of the real build (its row is its own). The 2 listed
sites it makes stale (extract/jump-tables.txt line 126, extract/wait-points.txt line 222) did not stop the generator,
which accepted the lists in the real build (both files are regenerated from the DOL, not from the owner list).
The real candidate build (watch tree, lifted renderer) generated and linked: no refusal.
The removal-plan service-level body counts were not re-run (no ratchet change).

## Gate 2: contract test (reference interpreter, pinned main.dol)

`native_video_reference_tests` (extended): ORIGINAL runs the SDK's own words, NATIVE the owners, from byte-identical
MEM1 left by the original VIInit + VIConfigure(PAL / EURGB60); every MEM1 byte outside the call's stack window is
compared. Cases: VISetNextFrameBuffer (4 buffers x default / panned / odd field / stereo / stereo above 16 MB),
VIFlush (default, mixed masks incl. registers 0, 15, 21, 22, 63, low word only, nothing), VISetBlack (0 / 1 / -1 x
default / odd field / 54 MHz), the current-buffer getter, VISetPreRetraceCallback (0 and a callback, with one
installed), VIGetTvFormat over modes 0-7. Retirements are PINNED: on a cleared slate the original must have written the
retired register words, the native must not, and the changed mask must be the original's minus exactly the retired bits.
Negative control: one flipped shadow-register byte after a VIFlush is exactly one detected difference.
Result: **372 checks, 0 failures** (the first run had 116 failures, all in my own retirement-pinning checks - the slate
was not clean, VIConfigure had already written those words - fixed in the test, no owner change).

## Gate 3: guest watch (no surviving reader), Westopolis, headless

`.build/ab-jobs/vi-ranges.txt`: regs 0x00 / 0x0C-0x13 / 0x1C-0x2B and their shadows, the four buffer-address words,
`changed`, `shdwChanged`, plus the two stack spots of the A/B differences below; whole 90 s tape from boot, ORIGINAL
(base.exe) vs candidate. Unattributed faults 0 (base and cand). Readers of the retired words in the original: only
VIFlush (0x8037FD38 copy loop) and setFramebuffer (0x8037ED28 reading its own words) / VIConfigure's helpers; in the
candidate: the owners' own bookkeeping (attributed to their caller, the RenderWare GameCube swap 0x80494B0C / 0x80494F18),
VIConfigure's helpers 0x8037EFFC / 0x8037F19C / 0x8037ED28, the native retrace handler (reads shdwChanged only) and
the harness report. No game, CRI or RenderWare row reads any retired word. Reports: `.build/ab-jobs/watch-{base,cand}.csv`.

## Gate 3b: who reads shdwRegs and the masks (default config, Westopolis, headless, base 99fe8c3 vs merged candidate)

Ranges: all of shdwRegs (0x80587CA0, 0x78 bytes), `changed` (0x805F20F8), `shdwChanged` (0x805F2108); unattributed
faults 0. Candidate readers: shdwRegs: `NativeVideo::retrace_handler` (+0xEC -> 0x80587D0C = reg 54, +0x10D ->
0x80587CA2 = reg 1) and the harness state report only; shdwChanged: the retrace handler and the owner (via its caller,
the RenderWare swap 0x80494B0C / 0x80494F18); changed: VIConfigure's helpers 0x8037ED28 / 0x8037EFFC / 0x8037F19C
(OR into it) and the owner. No game row reads any other shadow register. Reports `.build/ab-jobs/watch2-{base,cand}.csv`.

## Gate 4: Westopolis 90 s A/B, DEFAULT configuration (no SHADOW_NATIVE_JOBS), alternating base, cand, base, cand, headless

base.exe = gc-removal 99fe8c3 built unchanged (f8d0e00f...), cand.exe = agent/vi merged with 99fe8c3 (054e3e22...).
- PCM `015445f28cd10698` (the reference) in all four runs and both peek runs; 3,645,000,000 ticks, 2,697 VI frames,
  5,360 render frames in every run.
- 64 KB memory blocks: A/A 0 differing, B/B 0; every A/B pair 2 differ: 0x80580000 and 0x805A0000 (peek, word level):
  - 0x805A0000: 0x805ACD78/7C/80 (saved registers of the interrupted code in the last retrace delivery's OSContext),
    0x805AF2B0 / 0x805AF2F4 / 0x805AF2F8 (dead back-chain / spill words of the retired SDK frames): the same 6 dead
    words as before the merge.
  - 0x80580000 (the VI static block): regs 0x1C / 0x24 (+0x1C, +0x24: the retired frame-buffer registers), +0x124 /
    +0x128 (retired buffer-address words), shdwRegs[0] and shdwRegs[2..53,55..] and their words (+0x78..+0xE0 except
    registers 1 and 54, which are identical): the retired register image, no reader (Gate 3b).
- Not covered: windowed run, saving/loading, PAL/50 Hz boot.

## Performance (4 alternating runs each, first discarded; guest-run seconds from session.json, headless)

base wall 1.397 / 1.307 / 1.293 (median 1.307), cpu 1.391 / 1.297 / 1.281; cand wall 1.350 / 1.306 / 1.324 (median
1.324), cpu 1.328 / 1.312 / 1.328. The ranges overlap: **no measurable change**. (The harness runs the 90 s tape in
~1.4 s wall; no FPS claim.)

## Removed dependencies

VI: 0x8037FD38, 0x8037FE68, 0x8037FED4, 0x8037FEDC, 0x80380138, 0x8037E4F0 and (generator) 0x80380170 / 0x80380178 /
0x80380180, 0x803A225C, 0x803A2280 no longer compiled. setFramebuffer 0x8037ED28 and setVerticalRegs 0x8037EFFC stay (VIConfigure,
live through the RenderWare swap).

## Open problems / for the main session

- VIWaitForRetrace 0x8037ECD4 is the remaining VI frontier row (thread queue + CRI vsync threads: cri-threads' scope).
- Not covered by any run here: a windowed run (blanking by VISetBlack during the first frames is the native present
  path's job - the black flag word is kept, nothing in the candidate reads it to blank a window; the A/B shows frames
  and render counts identical headless), saving / loading, a PAL/50 Hz boot.
- AGENT_COORDINATION section 3 lacked `-DSHADOW_RECOMP_DOL` (fixed on gc-removal 841f0b7). Parallel full builds hit
  a transient `C1090 PDB API call failed (0x6BA)` twice (candidate shard 37, baseline shard 40): re-running the same build
  command resumed and succeeded; no source change.
- The temporary baseline worktree `wt-agent-vi-base` (git worktree at 1c0c789) was used for base.exe and is removed at the end.

# Step 2 (base gc-removal 0887925): the staged registers are native; VIConfigure is a native owner

## Remaining graphics.vi rows (planner on the merged tree, `.build/platform/removal_plan`)
4 live bodies, 1 frontier row: VIWaitForRetrace 0x8037ECD4 (entered by 0x8032D1D0 and CRI 0x803C53FC / 0x803C5454 /
0x803C5730: cri-threads, NOT owned), and VIConfigure 0x8037F19C (2,056 bytes, entered only by the RenderWare driver
0x80494F18 ...) with its helpers setFramebuffer 0x8037ED28 and setVerticalRegs 0x8037EFFC. (The "12 bodies / 6 frontier"
of the earlier plan predates the first VI merge.) what_if `--own 0x8037F19C`: **frees 19 rows** (17 sdk:unidentified, 2
graphics.vi), revives 0, 1 listed site becomes a work item.

## (1) Shadow registers 1 and 54 are native
`NativeVideo::staged_` (display control staged/enabled, clock select staged/54 MHz) replaces shdwRegs[] / shdwChanged:
VIFlush writes nothing of the shadow image into guest memory; the retrace commit reads the field. `changed_` (display /
clock written by VIConfigure) replaces the guest "changed" mask words. Both ride in the existing savestate byte
(bit 0 clock54, bits 1-4 staged_, bits 5-6 changed_): no version change.

## (2) VIConfigure 0x8037F19C is a native owner (`NativeVideo::vi_configure`)
Written: HorVer +0xF0..+0xF7, +0x106..+0x11B (display position / size, panning words, frame-buffer mode, non-interlace
and TV words), the timing (+0x144, getTiming 0x8037E578's table), the mode-change flag (r13+0x5BF0), the two registers
that stay guest words because the RenderWare video-mode code reads them (static+2 and +0x6C, reader 0x80494F18) and the
native changed flags. Retired (no reader; guest watch): the horizontal / vertical timing, filter and frame-buffer
registers, HorVer's adjusted geometry (+0xF8..+0x105 except the words above, +0x11C..+0x12F, +0x134), the
setFramebuffer / setVerticalRegs calls, the OSReport warning (log only) and OSDisable/RestoreInterrupts (the owner is
atomic). Kind/TV mismatches stop the run (the SDK's assert); PAL/EURGB60 progressive values 2 and 3 (no timing record:
the SDK read through NULL) stop it too (documented difference, never replicated).
Note for the main session: VISetBlack's black flag (+0x130) now has no reader at all (setVerticalRegs is gone); blanking
by VISetBlack is not wired into the native presenter (it never was). Left as is.

## Gates
- what_if: 19 rows (above). The real build generated and linked with 0x8037F19C in native-owners.txt and liveness-roots.txt.
- Contract (reference interpreter, pinned DOL): `native_video_reference_tests` **510 checks, 0 failures, 13 documented
  differences**. VIConfigure: PAL / EURGB60 / NTSC presets (also repeated), synthetic modes over every low-bits value
  (0-3), frame-buffer modes, sizes and both encoder types; every MEM1 byte outside the stack window compared except the
  retired register image; retirement pinned (the original wrote retired bytes); negative control (one flipped HorVer
  byte = exactly one difference); wrong-kind mode: both stop. The VIFlush cases check the native staged field against the
  original's shadow words and that no shadow word is written into guest memory.
- Guest watch (default config, Westopolis whole tape, headless; per-word ranges over the static block and the VI small-data
  words; `.build/ab-jobs/watch3-{base,cand}.csv`): unattributed faults 0. Readers of the retired words in the candidate:
  only the harness state report. Readers of the retained words: 0x80051028 (HorVer +0xF0..+0x114), 0x80494F18 (RenderWare
  video-mode code: static+0, +0x6C, +0x114, +0x118, r13+0x5BE8), 0x80494B0C (+0x120, r13+0x5BF0 / 0x5C00), the retrace
  handler (+0x118, +0x144, staged_). shdwRegs and the changed masks: no reader at all.
- A/B, DEFAULT configuration, alternating base (0887925) / cand, 2+2, headless: PCM 015445f28cd10698 in all runs
  (= the reference), 3,645,000,000 ticks, 2,697 VI frames, 5,360 render frames. A/A, B/B memory identical. A/B: one 64 KB
  block, 0x80580000 (the VI static block), and nowhere else; word level (peek): the retired register image only -
  regs 0x00-0x2F, 0x58, 0x70 (+0x00..+0x7B), shdwRegs[0], HorVer adjusted geometry (+0xFC..+0x104, +0x11C/+0x11D) and
  the address words +0x124 / +0x128. Every retained word is identical.
- Performance (4 alternating runs each, first discarded, guest wall seconds, headless): base 13.06 / 12.97 / 13.34,
  cand 13.25 / 13.45 / 13.39; ranges overlap: no measurable change (candidate marginally higher median, inside the
  base/cand run-to-run spread of 0.4 s).
- Not covered: windowed run, saving / loading (the savestate packing is covered only by the code, not by a round trip
  here), PAL/50 Hz boot.

# Step 3: VISetBlack blanks the presented picture

On the console VISetBlack(TRUE) makes the VI show black from the next flush on (the game keeps drawing and flipping);
natively nothing read the flag. `NativeVideo::black_requested()` (the guest flag word VISetBlack sets) now feeds the lifted
renderer: `RwDriver::submit_frame` (the display copy of the present, game thread) shows a frame finished while the flag
is in force as one black frame - the same presentation-only mechanism as the video-mode-switch blank (the game's frame
and state are untouched). Wiring: `shadow_main.cpp` -> `LiftedRender::set_display_blank` -> `RwDriver::set_display_blank`.
`SHADOW_VI_BLANK=0` switches it off (A/B in one binary); `SHADOW_TRACE_VI_BLACK=1` logs every VISetBlack / VIFlush /
present with the flag (census).

## Census (Westopolis tape, default config, headless, 5,360 presents)
The tape calls VISetBlack four times, all from the RenderWare swap: VISetBlack(1) from 0x804952B4 at tick 0 and tick
21,870,000 (field 27), VISetBlack(0) from 0x80494BA0 at ticks 1,620,000 (field 2) and 23,490,000 (field 29); each is
followed by a VIFlush with the flag. Presents shown black: 1-3 (tick 810,000, the boot frames) and 27 (tick 22,680,000).
The other VISetBlack callers (game 0x8032D29C / 0x8032D774, OS 0x80374714 / 0x80374E10) are not reached by this tape.
Timing note (measured from the log order): the swap flushes the PREVIOUS frame's buffer, so on the console a picture is
shown with the flag staged by the flush that follows its present; the native presents at the present itself. The blank
interval is therefore shifted by one picture at its two edges (e.g. picture 3 is black natively, shown unblanked on the
console; picture 26 is shown natively, blanked on the console). Making it exact needs a one-frame holdback of every
packet (the console's own display latency), which the native path deliberately does not have; here both edge pictures
are already black (the game fades from/to black around its blanks).

## Gates
- Contract: `native_video_reference_tests` 528 checks, 0 failures (new: `black_requested` follows the flag).
- Capture comparison (`--capture-at-tick`, headless, `SHADOW_VI_BLANK=0` vs default; ticks 810,000 / 1,620,000 /
  21,870,000 / 22,680,000 / 23,490,000): 810,000: blanked frame exactly black (mean 0.00) vs the game's near-black picture
  (mean 0.08); the other four byte-identical (the unblanked frames at 1,620,000 and 23,490,000 untouched, 21,870,000 and
  22,680,000 already black).
- Default-config 2+2 (blanking off, on, off, on; same exe, headless): PCM 015445f28cd10698 in all four, 3,645,000,000
  ticks, 2,697 VI frames, 5,360 render frames, memory identical in every pair (presentation-only).
- Not covered: a windowed run, the callers the tape does not reach (loading screens / reset paths).

# Step 4 (rwR1, the device surface): analysis; NOT landed

Frontier rows (19, `test_data/platform/gx_rw_frontier.csv`): 17 had 0 hits in the Westopolis replay (every dispatch site of
the render-state, Im2D and camera standard functions is lowered, extract/lift-sites-r2.txt) and 2 executed: GX/ShowRaster
0x80495CBC (5,360 hits) and the 12-byte device-record accessor 0x804960F0 (4 hits: `lis/addi 0x8056F50C; blr`).
what_if on the merged tree (generator lock, `.build/platform/what_if/rwr1-*`):
- all 19 rows owned: 71 rows freed (52 rw, 19 gx), 797 listed sites become work items, refusal: 1 stale lowered call site.
- the 9 rows that have an exact scaffold equivalent (GcnSet/GetRenderState, GcnCameraBegin/EndUpdate, GcnCameraClear,
  GcnIm2DRenderPrimitive / IndexedPrimitive, the camera-texture copy 0x80496208 and the accessor): 57 rows on the pre-merge
  tree, **42 rows (41 rw, 1 gx) on the merged tree** (the per-build liveness roots already pruned 15), 515 listed sites
  (gx-sites.txt 256, gx-fail-sites-lifted.txt 256, jump-tables.txt 3), no refusal.
- The 10 other rows are not adapters: GcnRasterShowRaster runs the GX copy-display / draw-sync / xfb-ring logic
  (r13+0x61xx ring, GXReadDrawSync, token stores: overlaps gxD, native_gx_sync); TextureBind, ClearQuad, 0x80499394,
  0x80499F10, 0x804960FC, 0x804987C8, 0x80498854 and Im2D triangle/line have no scaffold function (raster / texture /
  pipeline work, rwR2/rwR4).

The adapters themselves are written and kept as a patch (`vi-rwR1-adapters.patch` in the shared notes folder): each owner
calls the SAME scaffold function the lowered sites of that entry call (`rw_render_state_set(c, r3, r4)` ...), argument lists
read from lift-sites-r2.txt, `install_device_surface_owners` called from `LiftedRender::install_owners`, nine lines in
native-owners-r2.txt.

**Why not landed:** the 515 stale listed sites cannot simply be deleted. `extract/gx-sites.txt` and `extract/jump-tables.txt`
are shared with the non-lifted R2-baseline build and are checked EXACTLY against the inventory the DOL produces
(`native_gx_sites`, jump-table check, CMakeLists.txt ~3171 / ~3207), so removing lines fails those tests and breaks the
baseline; only `gx-fail-sites-lifted.txt` is lifted-only. Landing needs per-build site lists like the per-build liveness
roots (`gx-sites-lifted.txt`, `jump-tables-lifted.txt` derived from the inventory minus the sites of functions pruned in
the lifted build, with the inventory check kept on the full files) or a generator that drops (and reports) sites of
functions it prunes because of declared owners. That is build infrastructure outside this subsystem; the retired-line
patch is `vi-rwR1-retired-site-lines.patch`.

## Step 4 landed: the nine device-surface adapters (correction of the section above)
The "blocker" above was wrong for this subset: the generator does NOT refuse the 9-row set (what_if `rwr1-adapt2` on the
merged tree: 42 rows freed, refusal None); the 515 stale listed sites are work items only, exactly as in card-saves' gxA,
which left extract/gx-sites.txt and jump-tables.txt alone (they are inventory-checked against the DOL). Only the larger set
(all 19 rows) is refused ("1 stale lowered call site"). So the adapters need no list edits.

Landed: `install_device_surface_owners` (runtime/scaffold/rw_driver/rw_driver.cpp, called from `LiftedRender::install_owners`),
nine lines in extract/native-owners-r2.txt, three roots (0x804956F4, 0x80495CAC, 0x80496208; the other owners are data words
or already rooted) in extract/liveness-roots-lifted.txt. Each owner calls the same scaffold function the lowered sites of its
entry call, with the sites' argument count from r3 upward (GcnSet/GetRenderState, GcnCameraBegin/EndUpdate, GcnCameraClear,
GcnIm2DRenderPrimitive / IndexedPrimitive, the camera-texture copy (void), and the device-record accessor 0x804960F0 =
0x8056F50C). ShowRaster 0x80495CBC is left alone (copy-display / draw-sync / XFB ring: card-saves' gxD).

Gates:
- Contract: `tools/test_device_surface_owners.py main.dol` (CTest `native_device_surface_owners`): owner set = the nine
  addresses; each calls the expected scaffold function with r3.. in order and the argument count / void-ness of its lowered
  sites in lift-sites-r2.txt; each is declared in native-owners-r2.txt; the accessor equals the original `lis/addi/blr`
  decoded from main.dol. Five negative controls (wrong function, dropped argument, swapped registers, wrong accessor value,
  undeclared owner) are each detected. The game never enters these (0 hits in the replay), so there is no state to compare.
- Both builds: lifted `.build/watch` of the merged tree built and linked with the owners (no refusal); baseline = the same tree
  without them (temporary worktree, removed). The tests tree was not rebuilt for this step (the test is Python).
- Westopolis 2+2, default config, headless, alternating base / cand: PCM 015445f28cd10698 in all, 3,645,000,000 ticks,
  2,697 VI frames, 5,360 render frames, memory identical in every pair (A/A, B/B, A/B, peek of the VI block and the 0x805A0000
  block included).
- Pixels: `--capture-at-tick` at 5,000,000 / 200,000,000 / 900,000,000 / 2,000,000,000 / 3,500,000,000, base vs cand:
  five byte-identical PNGs.
- Retired-reader watch and performance: not applicable (no guest state retired, no executed path changed).
- Not covered: paths the replay does not reach (menus, movies, cutscenes) never enter the table entries by construction
  (every dispatch site is lowered), so an entry there would reach an owner doing what the lowered site does.

# Step 5: rwR4, the world-sector pipeline entries (20 of 22 rows)

Census (observe-only entry probes `--measure r5probes --measure-probes "sets=none;extra=<22 addresses>"`, headless, 240 game
seconds, `.build/ab-jobs/census4.sh`) on three recordings - Westopolis (stg0100), Final Haunt (stg0604) and Lethal Highway (stg0202)
tapes: the same four rows execute on all three (0x804651D8 x4, 0x80465210 x4, 0x80465818 x2, 0x804662B0 x42 / 43 / 120) and the
other 18 have 0 hits on all three. The pipeline's execution is lowered (`rw_pipeline_execute_*`), so its node bodies, instance
helpers and the stream SIZE / WRITE callbacks (nothing in the game sizes or writes native world data) are not entered.

Owned (`runtime/src/gekko/native_rw_world.cpp`, installed by `LiftedRender::install_owners`; 20 lines in native-owners-r2.txt):
- 0x804651D8 (node-definition init: fills the four callback words at `*(r3+0x14)` with 0x80464F60 / 0 / 0x80464E44 /
  0x80463964, returns 1) and 0x80465210 (accessor, 0x8056E9F4): served exactly.
- 18 STOP owners (the other rows): entered, they stop the run naming the entry and the reason. This rests on the census and on
  the lowered execution; a path no recording reaches would stop instead of running GX code with no consumer.
- Left translated, by design: 0x80465818 (plugin attach; calls the translated plugin registry 0x8045BAF0 and stores its result)
  and 0x804662B0 (the native world-sector stream reader, executed at every stage load; calls the 0x80465AE4 reader): serving
  them is the RenderWare reconstruction's job. what_if: owning them as well frees only 2 more rows (119 vs 117).

what_if on the merged tree (`rwr4-20`): **117 rows freed** (101 rw driver, 15 gx, 1 rw layer), refusal "1 stale lowered call
site" = `site 0x803977E8 msl_cosf` of extract/lift-sites-math.txt (inside pruned GX function 0x80397770). Moved to
lift-sites-math-unlifted.txt (the unlifted-only math sites, the file's existing purpose; the lifted generator no longer sees it).
Other stale listed sites (8 jump-tables, 8 gx-sites) are work items only, left in place (jump-table lines must stay: they
exclude the table words from the data-word roots). Root added: 0x80463964 (the pipeline callback the init writes).

Gates:
- Contract (`native_rw_world_reference_tests`, 89 checks, 0 failures): PipelineInit over 3 fills x 2 object/block placements
  (every MEM1 byte outside the stack window + r3 equal to the original), the accessor, a negative control (one flipped byte =
  exactly one difference), the 18 stops (each stops and names its address), and the pin of the frontier's rwR4 set (served +
  stopped + excluded = the 22 rows; stops have 0 census hits, the exclusions > 0).
- Lifted build of the merged tree built and linked (one transient `C1090` PDB error in a shard, resumed by re-running the
  same command): 1,113 pruned rows, 295 owner rows, no refusal.
- Westopolis default-config 2+2 (base = the tree without these owners): PCM 015445f28cd10698, 3,645,000,000 ticks, 2,697 VI
  frames, 5,360 render frames, memory identical in every pair incl. the VI / 0x805A0000 peeks.
- Pixels: --capture-at-tick at 5M / 200M / 900M / 2.0G / 3.5G: five byte-identical PNGs.
- Retired-reader watch: n/a (no guest state retired). Performance: not measured (no executed path changed).
- Not covered: other stages' world loads beyond the three recordings (the stop owners would say so).

# Step 6: the structural twins (tools/platform/structure_match.py)

- VISetPostRetraceCallback 0x8037E534 (`NativeVideo::vi_set_post_retrace_callback`, native-owners.txt): returns the previous
  post-retrace callback (r13 + 0x5BE0) and installs the new one - the twin of VISetPreRetraceCallback (the original brackets
  it in OSDisable/RestoreInterrupts; the owner runs atomically). Executed in the replay (2 callers: 0x80374728, 0x80495494).
- fn_8049C198 (`blr`, called 176,459 times in the replay from 0x804863EC) and fn_8049C6BC (`lwz r3, 0x6244(r13); blr`):
  `install_native_rw_world_owners` (native-owners-r2.txt). what_if: 0 additional rows (the bodies themselves go).
Gates: `native_video_reference_tests` 536 checks / 0 failures (VISetPostRetraceCallback with 0 and a callback over a state
with a callback installed, result and every MEM1 byte); `native_rw_world_reference_tests` 97 / 0 (the empty hook leaves memory
and r3 alone; the getter returns the word for 0 / 0x80123456 / 0xFFFFFFFF); lifted build ok (one transient C1090 PDB error
resumed); Westopolis default 2+2 vs the rwR4 build: PCM 015445f28cd10698, 5,360 render frames, memory identical in all pairs;
five --capture-at-tick PNGs byte-identical. No new liveness root (the regenerated lifted roots equal the checked file).

# Status of graphics.vi (planner on the tree merged with gc-removal 1ea1683, 2026-11-07)
`removal_plan.py` (release tree = `.build/watch`): graphics.vi has **1 live body, 1 frontier row, 0 freed by owning it**:
VIWaitForRetrace 0x8037ECD4 (entered by game 0x8032D1D0 and CRI 0x803C53FC / 0x803C5454 / 0x803C5730). Everything else of the
service is native: VIInit / __VIInit, the retrace delivery, VIFlush, VISetNextFrameBuffer, the current-buffer getter, VISetBlack
(and its blanking), VISetPre/PostRetraceCallback, VIGetTvFormat, VIConfigure (with setFramebuffer / setVerticalRegs pruned), the
staged display-control / clock-select registers. VIWaitForRetrace sleeps the calling guest thread on the retrace queue
(`OSSleepThread` until `retraceCount` changes); it needs a native owner that can block a guest thread, which cri-threads' plan
schedules as S5 after S3 (docs/agents/cri-threads.md sections 7 and 11) - not a VI-only change.

# Step 7: rwR5, the plugin / node registry (9 of the 19 rows; 0x8049C198 was owned in Step 6)

Census (entry probes, three recordings, `.build/ab-jobs/census5.sh`): 16 rows execute with identical counts on all three (they
run at engine start-up: plugin registration and node-definition setup; 0x8049C97C x76, 0x8049C90C x60, 0x8049C9F4 / CC9C / CE40 x20,
BD30 x15, C3A0 / C850 x10 ...); RwPalQuantTerm 0x80491448 and 0x8049CA80 have 0 hits on all three.

Owned (`runtime/src/gekko/native_rw_world.cpp`, native-owners-r2.txt):
- Served exactly (7): 0x8049BCE0 (the current plugin-globals slot: set, or restore from the saved copy, return it), 0x8049C190
  (returns TRUE), 0x8049C9F4 (the number of distinct sort keys of a table), 0x8049C90C / 0x8049C97C (bump allocation from the
  top / floor of the engine's static arena at r13+0x2B08), 0x8049BD30 (push / pop of the slot of kinds 1..6), 0x8049CC9C (registers
  a node record in a pipeline table). Error paths: the original raises RwError and returns 0 / continues; the owners STOP the run
  naming the request (arena exhausted, invalid slot kind, node limits exceeded). memset is inlined (0x8000540C = memset).
- Stop owners (2): 0x80491448 and 0x8049CA80 (0 hits on three recordings).
Left translated, with the reason: 0x8049BFE4 / C048 / C11C / C160 (plugin construct / destruct callbacks that call the translated
plugin registry and other plugin callbacks), 0x80494EB4 (creates pipelines through the translated RxPipeline core 0x80496440 /
0x8049AC48), 0x8049C7BC (allocates through the engine's memory function table, an indirect guest call), 0x8049C3A0 and 0x8049C850
(node removal: indirect guest calls through the node definition and the engine free), 0x8049CE40 (the 252-instruction node swap:
self-contained but left for the rwcore pipeline reconstruction). Serving them needs a native-to-guest call or the reconstruction.

what_if (merged tree): the 7 + 2 rows free 0 further rows (the bodies themselves go); the whole cluster would free 27 more. No refusal,
no new liveness root (the regenerated lifted roots equal the checked file).

Gates:
- Contract `native_rw_world_reference_tests` 565 checks / 0 failures: the original bodies against the owners over generated state
  (every MEM1 byte outside the stack window + r3): the slot setter (2 args x 2 saved x 2 previous), TRUE return, the distinct count over 24
  random tables (duplicates, 0, 0xFFFFFFFF, 0x80000000), the arena allocators (6 sizes x 4 spans: fit, exact, overflow incl. the arena words
  after the error path), the slot stack (kinds 0..7 and -1, push / pop, saved zero / distinct non-zero, invalid kinds stop), the node
  register (counts, key counts and limits at and over the edge); the frontier partition pinned (8 served incl. 0x8049C198, 2 stops, 9
  excluded, with the census hits). Mutation checks: six deliberate owner defects (wrong slot offset, off-by-one in either arena direction, wrong key
  comparison, wrong saved offset, wrong node-register base) were each detected; the equivalent mutant (BD30 pop return of slot vs saved) is identical by construction.
- Lifted build of the merged tree (gc-removal b4c53ee) built and linked. Westopolis default 2+2 vs a baseline built from the same tree
  without these owners (temporary worktree): PCM 015445f28cd10698, 3,645,000,000 ticks, 2,697 VI frames, 5,360 render frames, memory identical
  in every pair incl. the VI / 0x805A0000 peeks. Five --capture-at-tick PNGs byte-identical.
- An earlier A/B against a baseline built before the merge of other agents' work showed 12 differing blocks (the retired GX state, dead stack):
  that was the baseline's age, not this change; the proper baseline (same merged tree) shows none.

# Step 8: rwR3, the game-entered rows (10 of 10)

The planner's "game-entered" labelling is partly wrong; what the evidence shows (entry probes on Westopolis, Final Haunt, Lethal Highway):
- The four Im3D entries are the game's real consumers and are hot on every recording: RwIm3DTransform 0x8049B87C 402,182 / 557,760 /
  959,292 calls, RwIm3DEnd 0x8049B95C 377,681 / 463,435 / 706,897, RwIm3DRenderIndexedPrimitive 0x8049B9AC 363,567 / 456,609 / 690,543,
  RwIm3DRenderPrimitive 0x8049BB48 38,615 / 101,151 / 268,749 (callers: 11 game effect functions + Rt2d, e.g. 0x802DE7C0, 0x804340F0,
  0x8042E00C).
- The six "rtpitexd" rows 0x8049A124 / A304 / A364 / A3B4 / A41C / A480 are NOT called by the game functions the frontier file names
  (0x8040FB60 is an MT19937 random-number generator, 0x8040FD70 a sibling: the planner's edge is a conservative one): they are the
  raster -> image conversions of the platform-independent texture dictionary export - 0x8049A124 loads the jump table at 0x8056FC1C
  and dispatches to the five others by pixel format, and is itself reached only through the device's wrapper words 0x8051D484 / 0x8051D48C
  (0x8049A52C / 0x8049A508 -> 0x8049A124). 0 hits on all three recordings.

Owned (`runtime/src/gekko/native_rw_world.cpp`, native-owners-r2.txt):
- Served exactly (4): the Im3D entries. Their bodies keep the immediate-mode state in the plugin-globals slot (vertex count, vertex array,
  flags, the per-primitive-type pipelines, the execution record with the primitive type, index array and count rounded to whole
  triangles / segments) and run the Im3D pipeline; the owners do the same stores and call the native renderer's pipeline execution
  (`rw_pipeline_execute_im3d_transform / _render`, passed in through `install_native_rw_im3d_owners` so the test observes the call).
  The three lowered call sites inside them (lift-sites-r2.txt: 0x8049B930, 0x8049BAFC, 0x8049BC90) are retired in this step - an owner
  replaces the body that contained them. The original's error paths (vertex count over 65536, a call without RwIm3DTransform, an invalid
  primitive type) raise RwError and carry on; the owners STOP naming the cause.
- Stop owners (6): the texture-dictionary image conversions (0 hits on three recordings; the consumer chain above ends in table words only).
what_if: 0 additional rows (the 10 bodies go; no refusal after the three lowered sites are retired). No new liveness root.

Gates:
- Contract `native_rw_world_reference_tests` 1,482 checks / 0 failures: for every entry the original body against the owner with the pipeline
  execution stubbed identically on both sides (the stub records pipeline, data block - 0x40 bytes of the execution record or vertex block - and
  option; the results compared: r3, the calls and every MEM1 byte outside the stack window): Transform over counts 0 / 1 / 7 / 65535 / 65536, End
  open / closed, RenderPrimitive types 1-5, RenderIndexed types 1-5 x 12 counts (0..7, 100, -3, -1, 0x7FFFFFFF), each with execute results 0 / 1 /
  a pointer; invalid types, counts over the limit and calls without a transform stop. The frontier partition for rwR3 is pinned (6 stops with
  0 census hits + 4 served with hits > 0). Five deliberate owner defects (count rounding modulus, flag constant, pipeline offset, stored
  field offset, line-segment rounding) were each detected.
- Lifted build of the merged tree built and linked. Westopolis default 2+2 vs the rwR5 build: PCM 015445f28cd10698, 3,645,000,000 ticks, 2,697 VI
  frames, 5,360 render frames. Memory identical in A/A and B/B; A/B differs in ONE 64 KB block, 0x805E0000, in ONE word: 0x805E2CA4 holds
  0x8049B998 in the baseline and 0x80000000 in the candidate - 0x8049B998 is the return address of RwIm3DEnd's `bl memset` (the instruction after
  0x8049B994), a dead stack word of the frame the original body had and the owner does not build. Five --capture-at-tick PNGs byte-identical.
- Performance (3 alternating runs each after a discarded first, headless 90 game seconds, wall seconds): base 13.28 / 12.54 / 12.50, cand
  13.53 / 13.29 / 12.73. The ranges overlap, the candidate is on average about 0.5 s (4 %) slower: the four owners are entered 0.4-1.0 million
  times per run through the hle path instead of translated code. Not claimed as "no change"; a candidate for the owners' hot loop if it matters.

## Step 9 - native-owner dispatch cost and the rwR3 timing (main's request)

**Measured, not assumed.** A generated owner thunk (`fn_X(Cpu&) { c.serve_native_owner(X); }`) did: `try_hle` filter, `try_hle_slow`
(a call), `find_hle` (flat slot table, no hash outside verify / fault fill), `std::function` handler, hits++ . The hash map
(`hle_index_`) is only used when `hle_slots_all_` (verification or a latched fault). About one million owner entries per Westopolis run at
roughly 20-30 ns each is 20-30 ms of a 12.3 s guest run (0.2 %), so dispatch cannot account for 4 %.

**Change.** `Cpu::serve_native_owner` reads the owner's entry straight from the slot table when the slot names the address (and
`!hle_slots_all_`), calls the handler and does `try_hle_slow`'s accounting (slow-call count, hits, `pc = lr`, reference-build
`handler_depth_`); any other state (verify, fault, a miss) takes the old path unchanged. No generator change was needed.

**Timing (Westopolis 2+2, headless, jobs=0 default, PCM `015445f28cd10698` in every run, capped runner).** Baseline = gc-removal 1858197
built from a temporary worktree; candidate = agent/vi (rwR3 + this change). Ten alternating runs, `session.json` `host.guest_run_wall_seconds`:

| | runs (s) | mean | median | min |
|---|---|---|---|---|
| base 1858197 | 12.753 12.433 12.390 12.929 12.486 12.771 12.358 13.053 12.209 12.330 | 12.571 | 12.459 | 12.209 |
| cand rwR3 + dispatch | 12.511 12.710 11.881 12.645 12.656 13.436 12.087 12.061 12.515 11.472 | 12.397 | 12.513 | 11.472 |

Run-to-run noise (about 1 s) is far larger than any difference; the means differ by -0.17 s, the medians by +0.05 s. An earlier
five-round pair with the dispatch change absent (cand = rwR3 only): base mean 12.187 (11.787-12.946), cand mean 12.322 (11.477-13.44).
The "4 %" of Step 8 was noise from three runs and a build baseline that differed. Not claimed: any speed-up.

## Step 10 - device-slot survey and the two raster wrappers 0x8049A52C / 0x8049A508

**Table layout.** The RW GameCube device table at 0x8051D400 is (id, fn) pairs, the id BEFORE the function; the engine's standard-function
array is `[r13+0x615C] + 0x48 + 4*id` (calibrated on the live callers: id 7 RasterSetImage called at 0x8048AA98, id 12 SubRaster at 0x8048AE8C,
id 26 at 0x804851D8). Ids: 1 BeginUpdate, 10 EndUpdate, 21 Clear (owned, rwR1), 20 ShowRaster 0x80495CBC, 2 RGBToPixel 0x80492BFC,
3 PixelToRGB 0x80492F10, 7 RasterSetImage 0x80494614, 6 ImageGetRaster 0x80493A40, 9 ImageFindRasterFormat 0x804948D8, 4 RasterCreate 0x80497CA8,
5 RasterDestroy 0x80498058, 8 TextureSetRaster 0x8049819C, 15 Lock 0x80496CA4, 16 Unlock 0x80497104, 23 UnlockEnd 0x804972B8,
24 UnlockPalette 0x80497388, 14 0x8049A52C, 13 0x8049A508, 17 / 18 / 19 RasterRender / Scaled / Fast 0x80499A9C / 0x80499AEC / 0x80499AC4,
11 SetContext 0x8049A570, 12 SubRaster 0x804981B8, 25 GetSize 0x8049A57C, 27 Write 0x8049A604, 26 Read 0x8049A8DC, 28 GetMipLevels 0x804966D8.
Record 0x8056F50C: F510 GcnSystem, F51C / F520 render state (owned), F524 Im2DRenderLine, F528 Im2DRenderTriangle, F52C / F530 Im2D primitive (owned).
Ownership (agreed in writing with card-saves, sdk-identify, main): card-saves 4 5 8 28 15 16 23 24 + 0x80497458 + 0x804981B8 / 0x8049A570 (done);
sdk-identify 2 3 26 9 25 27; vi everything else.

**Census.** Entry probes on the three recordings and on eleven idle boots (title/attract, stg0210 / 0310 / 0410 / 0202 / 0400, 1500-1800 frames
each; stg0000..0005 are not boot-able; Options, display tile / resolution change, FMV, save/load and real boss play are NOT reachable headless).
Live: 0x80495CBC (14,360 / recording), 0x80494614 (1), 0x804981B8 (31k), GcnSystem 0x80494EDC (29k) and card-saves' rows. 0 hits on all 14
runs: ids 2 3 23 24 13 14 17 18 19 11 25 27, F524, F528. Absence is not used as a reason to stop any of them.

**Owned in this step.** 0x8049A52C and 0x8049A508 (the wrappers of the texture-dictionary export 0x8049A124): r3 = `[r13+0x6210]` and the
call of 0x8049A124's owner, i.e. the same stop by the same path (the on-stack rectangle 0x8049A52C builds dies with the frame). Reference test:
both stop naming 0x8049A124 with r3 = the raster; a mutation of the SDA offset is detected (2 failures). liveness-roots(-lifted).txt
gain the two addresses (regenerated; the test passes). Gates: lifted build, default-config 2+2 PCM `015445f28cd10698` in all runs, five
captures byte-identical against a build of gc-removal 7bfb5e7, four alternating timing pairs equal (11.941 vs 11.948 s mean). Not covered:
neither wrapper runs in any recording, so the gates prove no regression, not the wrappers' behaviour (the reference test does).

**Not owned, with reasons (callee census against the owner lists).**
- GcnSystem 0x80494EDC: cases 4-18 and 22 are pure; 0 / 1 / 2 / 3 call GX / VI / OS bodies not yet owned (0x80494CD4, 0x80396098,
  0x8037847C, 0x8037ECD4, 0x80394AF4, 0x80396BD4, 0x80396CCC, 0x80397228, 0x80397430, 0x80399E58, 0x80399FCC, and the RW driver bodies 0x804981E8 /
  0x8049847C / 0x80496440 / 0x8049AC48) and the engine's guest allocator callbacks; case 3 spins on `[r13+0x61C4]`, `[0x61B8]` vs `[0x61BC]`, which
  the driver's own retrace / draw-sync callbacks (0x80494ABC..0x80494C94) and ShowRaster write. A declining hook was rejected (no body removed).
- ShowRaster 0x80495CBC: unowned callees 0x803792C4, 0x80396098, 0x80396BD4, 0x80396E54, 0x80397444 (GXCopyDisp, its site is lowered) + the swap flags above.
- Im2D 0x80491D0C / 0x80491B08: every GX setter in 0x8049148C is owned, but GXBegin (0x80396934) is refused in the lifted build and 0x80491774 /
  0x80498854 are unowned RW driver bodies.
- 0x80499A9C / AEC / AC4: wrappers of 0x80499924; need the guest-call seam (sdk-identify's af380fa, not in gc-removal yet).


## Step 11 - GcnRasterSetImage with its image-conversion bodies native

Redo after main's review of the first attempt (SetImage calling the four format bodies and the three RasterRender wrappers
through the seam): the RW *GameCube driver* is platform code, so a native owner may not guest_call its bodies, and a return
site passed as a link address must not become a liveness root. Owners are in `native_rw_raster_image.cpp`.

* **Owned natively (all exact):** GcnRasterSetImage 0x80494614; the four format bodies 0x80493C38 (4-bit paletted),
  0x80493E90 (8-bit paletted), 0x804940EC (16-bit), 0x80494380 (32-bit); the quantiser 0x80493B74; the five converters
  0x80492B24..0x80492BF8 (RGB5A1, RGB565, RGB4A3, ARGB, AR/GB) as native functions (they were reached only through pointers the
  bodies computed). Each body reproduces its stack frame (0x30 / 0x40 / 0x240 / 0x440 / 0x4020 bytes) because the error report
  and the quantiser context pass pointers into it and the 16 / 32-bit bodies index a palette table at frame + 0x20.
* **Reached through guest_call (RenderWare core, not platform in the manifest):** the lock helpers 0x8048AFC0 / 0x8048ACE4 /
  0x8048ABD8 / 0x8048AB90, RwImageCreate 0x80488D58, RwImageAllocatePixels 0x80488E68, RwImageDestroy 0x80488E00, the
  quantiser context 0x80490F28 / 0x8048EF00 (f1 = the sdata2 single at r2 + 0x1CA0) / 0x80490620 / 0x80490B00 / 0x80491448, and
  the error pair 0x8047EC78 / 0x8047EBD4. No driver (0x8049xxxx) function is called.
* **Link addresses** are `body + 4u` expressions of the owner's own entry constant, so no new literal is a liveness root.
* **Dropped:** the RasterRender / Fast / Scaled wrappers 0x80499A9C / AC4 / AEC. They only call 0x80499924, which ends in
  0x80499394 (GXBegin and direct FIFO stores, refused in the lifted build); they come back with a native 0x80499924.
* **A converter that does not exist.** The original, for a raster format it has no converter for (format nibble 0, 4 (code 2 +
  string), 7, 0xB, 0xF), reports the error and then calls address 0 if it needs a pixel converted. The owner reports the same
  error and stops there (`address 0`) only when a conversion would be needed; with a locked palette / empty raster the
  original returns normally and so does the owner.
* **Evidence.** rw_raster_contract_tests, ORIGINAL words against the owners in the reference interpreter, RenderWare core
  replaced on both sides by logging PPC test doubles (id, argument registers by prototype, `{[r3], [r3+4]}` for RwErrorSet, f1
  for the quantiser): SetImage 2,880 cases, bodies 2,444 cases (11,746 core calls compared word by word), converters 3,072
  cases (every format, alpha 0xFF / 0 / any, both direct bodies). 15 mutations all told apart (green shift, alpha mask, alpha
  shift, opaque test, nibble order, 4-bit step, quantiser entry count and argument order, table index width, error slot,
  depth-8 recursion, temp image not freed, f1 not set, 16-bit frame size, locked-palette test).
* **Lifted build** (agent/vi 132fdd4 = gc-removal 452099e + this step), default config, Westopolis tape: the 5 captures at ticks
  5e6 / 2e8 / 9e8 / 2e9 / 3.5e9 hash 22fdf629 / d22a1750 / b6511d3d / 37299f60 / ae967db4 and PCM 46a70a1051347069 on both
  runs, i.e. main's canonical values. The recordings never enter these rows, so this shows nothing is perturbed, not that the
  owners run in-game. removal_plan.py on this tree: 275 platform bodies compiled; runtime_coupling.py: 5 native -> SDK
  calls (limit 6), none in native_rw_raster*.cpp; its one unclassified use is native_gx_sync.cpp:373 (a data-word table, not
  this step).


## Step 12 - the display-swap unit, specified instruction by instruction (no code yet)

Disassembled from main.dol (PAL GUPP8P): ShowRaster 0x80495CBC; the draw-sync callback 0x80494A2C and the two VI callbacks
0x80494B0C / 0x80494BE8; the video-mode setter 0x80494CD4; the device system function GcnSystem 0x80494EDC (jump table
0x8056F494, 23 words, dispatch value in r3, >0x16 = default); 0x80494EB4; the plugin registrations 0x80496440 / 0x8049AC48; the
render-state default init 0x804981E8; the raster-plugin teardown 0x8049847C.

**Numbering correction.** The jump table has 23 words and the dispatch value is the word index. The rows main and I called
"16" and "17" are dispatch values **17** (0x804954B4 -> bl 0x804981E8) and **18** (0x804954C0 -> bl 0x8049847C); the install of
the standard-function table (0x804954F8) is value **11**, the busy wait (0x804954CC) value **3**. Table: 0 0x80495124,
1 0x804951D8, 2 0x80495214, 3 0x804954CC, 4 0x804950A0, 5 0x80494F34, 6 0x80494F44, 7 0x80494F18, 8 0x80495098, 9 0x804954F0,
10 0x80495088, 11 0x804954F8, 12-16 default (r3 = 0), 17 0x804954B4, 18 0x804954C0, 19 0x804955FC, 20-21 default, 22 0x8049560C.

**State words (r13 = 0x805EC500; SDA offsets):**

| offset | meaning (as the code uses it) |
|---|---|
| 0x61A0 / 0x61A4 | first / second buffer base (the ring of two frame buffers; 0x61A8 / 0x61AC select one) |
| 0x61A8 | the buffer being drawn into (next to copy to); swapped to the other base after each copy |
| 0x61AC | the buffer being shown (set by the VI callback 0x80494B0C) |
| 0x61B0 | draw-sync ring read index; 0x61B4 write index; 0x2AE4 ring size (modulus) |
| 0x61B8 | write index for the VI-side ring of draw-sync tokens (0x61E0 halfword array, one token per slot); 0x61BC read index |
| 0x61C0 | 1 once the first draw-sync callback has been installed by the first copy (0x80394F80 call) |
| 0x61C4 | "display swap pending" flag: 1 while a frame is waiting for its retrace; set 1 / 0 by the callbacks |
| 0x61C8 / 0x61CC | the VI callback 0x80494BE8's handshake pair (retrace seen / draw done) |
| 0x61D0 | the second-field flag for interlaced output; 0x61D8 the thread queue the waiters sleep on (OSInitThreadQueue at init, OSWakeupThread in the callbacks, OSSleepThread in ShowRaster) |
| 0x6198 | 1 = the output is double-field (progressive-from-interlaced scaling), set in case 2; 0x61F0 = half the XFB width |
| 0x2AD8 | 1 = the next ShowRaster must run the "first frame after mode change" copy |
| 0x6188 | the render-to-texture / copy-clear colour index used by every GXCopyDisp (low byte) |
| 0x2AF0 | the next draw-sync token (halfword, 0..0xDFFF, wrap 0xE000: mulhw 0x92492493 divide-by-7 idiom -> mod 0xE000) |
| 0x2AE8 / 0x2AE9 | frames since the last swap (byte counter) / the minimum before a swap is allowed |
| 0x2AE0 | 1 once the VI has been told to show (so the first callback does the VI "unblank" 0x8037FEDC(0)) |
| 0x61F4 | pointer to the current video-mode record (copied by 0x80494CD4 into 0x805E4250 or one of three static records 0x805688C0 / 0x80568938 / 0x805688FC); +4 width, +6 height, +8 XFB height, +0x10, +0x18 interlace byte, +0x19 aa/flag byte, +0x1A sample pattern, +0x32 vfilter |
| 0x6194 | video mode kind (0 / 1 / 2 = the tv-format records, 0x2A = a caller descriptor); 0x618C / 0x6190 = the fbs count / double-field requests |
| 0x2ADC | the extra bytes appended to the XFB (case 0 argument), 0x619C = the engine allocation, 0x61E8 = its 32-byte aligned base, 0x61EC = second FIFO |
| 0x805E4238 | the draw-sync table, 8 bytes per slot: {buffer base, token} (slot index = 0x61B0 / 0x61B4) |

**Draw-sync callback 0x80494A2C (GXSetDrawSyncCallback argument, installed by case 2 via 0x80394F3C).**
Compare the base stored in slot `[0x61B0]` of the table with `[0x61AC]` (the shown buffer): equal -> set `[0x61C8] = 1` and return.
Otherwise next = (`[0x61B0]` + 1) mod `[0x2AE4]`; if next == `[0x61B4]` the ring is full: call 0x80395034 (GXSetDrawSyncCallback(0)?
the "stop" leaf) and clear `[0x61C0]`; else call 0x80394F80 with the next slot's token. `[0x61B0] = next`; OSWakeupThread(0x61D8)
(0x803793B0). Then, if `[0x6198] == 0` set `[0x61C4] = 1`; else if `[0x61D0] == 1` set `[0x61C4] = 1`, `[0x61D0] = 0`; else
`[0x61D0] = 1`.

**VI post-retrace callback 0x80494B0C.** `slot = [0x61E0 + 2*[0x61BC]]` (the token queued by ShowRaster for the frame being
shown); `[0x2AE8] += 1`. If `[0x61C4] == 1` and token-reached(0x8049B7DC)(token) (owned) and `[0x2AE8] >= [0x2AE9]`: `[0x2AE8] = 0`;
`[0x61AC]` = the other buffer base (`[0x61A0]`/`[0x61A4]` toggle); VISetNextFrameBuffer(`[0x61AC]`) (0x8037FE68); if
`[0x2AE0]` then VISetBlack(0) (0x8037FEDC) and `[0x2AE0] = 0`; VIFlush (0x8037FD38); `[0x61BC] = ([0x61BC]+1) mod [0x2AE4]`;
`[0x61CC] = 1`; `[0x61C4] = 0`.

**VI callback 0x80494BE8** (runs when `[0x61C8]` and `[0x61CC]` are both set): it is the draw-sync callback's tail executed from the
retrace side: same ring advance as 0x80494A2C (full ring -> 0x80395034 + `[0x61C0] = 0`, else 0x80394F80(next token)),
`[0x61B0] = next`, OSWakeupThread(0x61D8), the same `[0x61C4]` / `[0x61D0]` logic, then `[0x61C8] = 0`; and in every case
`[0x61CC] = 0`.

**0x80494CD4 (r3 = mode descriptor or 0, r4 = count).** With a descriptor: copy 0x3C bytes to 0x805E4250, `[0x61F4]` = it,
`[0x6194] = 0x2A`; r5 = r4 (2 when descriptor byte +0x19 is non-zero); `[0x618C] = [0x6190] = r5`; the GX-side mirror at 0x8056F408
(+0x48 = width halfword at +4, +0x4C = height halfword at +6, +0x50 = 0x10 for r4 == 2, 0x18 for r4 in [0,2) ..., otherwise error
RwErrorSet(code 2, string 0x8056F468)). Without one: VIGetTvFormat (0x80380138): 0 -> record 0x805688C0 kind 0, 1 or 5 -> 0x80568938
kind 1, 2 -> 0x805688FC kind 2, else error with string 0x8056F480. (The RwErrorSet pair is RenderWare core.)

**ShowRaster 0x80495CBC** (r3 unused). OSDisableInterrupts (0x8037611C) -> r31. pending = `[0x61B4] - [0x61B0] == -1`  or
`[0x2AE4] - 1 == [0x61B4] - [0x61B0]` (ring full, one word; the neg/or/srwi idiom makes it 0/1). If full: OSSleepThread(0x61D8)
(0x803792C4, with interrupts still disabled; the caller's restore is below). OSRestoreInterrupts(r31). Then one of three paths:
(a) `[0x6198] == 0` (single field): GXFlush (0x80396098); 0x803952FC; 0x80394EA4(&sp+0x10, &sp+0xC) (GXGetDrawSync-style:
returns the current token pair); OSDisableInterrupts; table[`[0x61B4]`] = {`[0x61A8]`, sp+0xC}, `[0x61E0 + 2*[0x61B8]]` = `[0x2AF0]`,
`[0x61B4]` = (`[0x61B4]`+1) mod `[0x2AE4]`, `[0x61B8]` likewise; OSRestoreInterrupts; if `[0x61C0] == 0`: `[0x61C0] = 1`,
0x80394F80(sp+0xC); GXCopyDisp(`[0x61A8]`, `[0x6188]` & 0xFF) (0x80397444; owned by axfx-effects, lowered to
render_submit_frame at the call site); GXSetDrawSync(`[0x2AF0]`) (0x80396428); `[0x2AF0] = ([0x2AF0]+1) mod 0xE000`; GXFlush;
`[0x61A8]` = the other base. (b) `[0x6198] != 0` and `[0x2AD8] != 0` (second half of a double-field frame): the same push
without the VI-token store, then `[0x2AD8] = 0`. (c) `[0x6198] != 0` and `[0x2AD8] == 0` (first half): the push as in (a) plus
`0x80396E54(2)`, `0x80396BD4(0, 2, width, height-2)` (GXSetCopyFilter / GXSetDispCopySrc), a second GXCopyDisp at
`[0x61A8] + ((width+15)&~15)*(height-2)*2` with the same colour, GXSetDrawSync + counter, `0x80396E54(3)`,
`0x80396BD4(0,0,width,height)`, GXFlush, `[0x61A8]` toggled, `[0x2AD8] = 1`. Returns r3 = 1.

**GcnSystem cases to own** (values; each with exact effects):
* **0 (0x80495124)** r5 -> r30 = the argument block pointer: if non-null: 0x80494CD4(\*(r30), \*(r30+4)), `[0x2ADC] = *(r30+8)`; else
  0x80494CD4(0, `[0x618C]`). Then engine allocation: size = `[0x2ADC]` + 0x1F + 4 * ((width+15)&~15) * height; ptr = engine
  malloc (`[eng+0x108]`, 0x40411 flags) -> `[0x619C]`; `[0x61E8]` = (ptr+0x1F) & ~0x1F; DCInvalidateRange/flush-style 0x80372508(base, `[0x2ADC]`)
  (zeroing the extra bytes); first buffer = base + `[0x2ADC]`, second = first + 2 * pixels; set `[0x61AC]` = `[0x61A0]` = first, `[0x61A4]` =
  `[0x61A8]` = second. Returns 1.
* **1 (0x804951D8)** GXFlush; engine free (`[eng+0x10C]`)(`[0x619C]`); zero `[0x61A0..0x61AC]`, `[0x61E8]`, `[0x619C]`. Returns 1.
* **2 (0x80495214)** the long one: first call (`[0x61D4] == 0`) creates the GX FIFO(s) (0x80393A88 GXInit-style, `[0x61EC]`), else
  re-initialises them (0x80394AF4 / 0x80394BDC / 0x80394D04 on the temporary and `[0x61EC]`); then waits for the draw-sync token
  (GXSetDrawSync / 0x803964DC GXReadDrawSync loop until equal); VIConfigure(`[0x61F4]`) (0x8037F19C); VISetBlack(1) (0x8037FEDC);
  VIFlush; optionally VIGetNextField (0x80380004) and the GX viewport / scissor / projection setup (0x8039A444 or 0x8039A49C,
  0x8039A4E4, 0x8039A55C, 0x80396BD4, 0x80396CCC, 0x80397228), the aspect and the double-field flag (`[0x6198]`, `[0x61F0]`,
  `[0x2AD8]`); GX copy filter (0x803970E4), 0x80399FCC, 0x80399E58, `[0x2AE0] = 1`, VIWaitForRetrace x1 or x2 (0x8037ECD4, native-boot),
  GXSetDispCopyGamma (0x80397430)(0), `[0x2AE8] = [0x2AE9]`, VISetPreRetraceCallback(0x80494B0C) (0x8037E4F0),
  VISetPostRetraceCallback(0x80494BE8) (0x8037E534), GXSetDrawSyncCallback(0x80494A2C) (0x80394F3C), OSInitThreadQueue(0x61D8).
* **3 (0x804954CC)** the busy wait: spin while `[0x61C4] == 1` or `[0x61B8] != [0x61BC]` (the VI ring has not caught up). Native: wait on the
  condition with the VI/draw-sync owners advancing it; never an unbounded host spin without a stop.
* **17 (0x804954B4)** call 0x804981E8; **18 (0x804954C0)** call 0x8049847C; both return 1.
* **7 (0x80494F18)** pure: r3 = 1 if the signed r6 <= 0 else 0 (the `(1^r6)` idiom). It reads no memory: the `__GXData` words axfx-effects
  saw touched in the replay at this PC are not in this instruction range (see below).

**0x80496440** = RwEngineRegisterPlugin-style call 0x8048ACA4(0x34, 0x40C, 0, 0, 0) -> `[0x6200]` (the raster plugin offset); **0x8049AC48** =
0x8048E2D0(0x24, 0x40C, 0x8049AB74, 0x8049AB88, 0) -> `[0x6218]` (the texture plugin offset). Both are RenderWare core calls plus one SDA store
(0x80494EB4 calls both, returns 1). **0x804981E8** = the driver's render-state default init: a block of stores into the state record
0x805E42D0 (+0x00 / +0x04 / +0x3C = 1, +0x08 = 3, +0x0C = 2, +0x10 = 0, +0x14 = 1, +0x18 = 0, +0x1C..0x1F = 0, +0x20 = 0, floats +0x24 / +0x28 /
+0x2C / +0x30 from sdata2 0x1CCC / 0x1CD0 / 0x1CD4 / 0x1CD0, +0x34 = 5, +0x38 = 6, +0x40 = 4, +0x44 = 7, +0x48 = 0, +0x4C..0x4E = 0) and the
GX state it implies (0x80399DF0, 0x80399E24, 0x80399778, 0x80399D44, 0x80396B54, 0x80397CBC x5, 0x80397B98 x4, 0x80399D98, 0x80399DC4, 0x803971B0,
0x8039A2CC), then 0x8049AC18 (an RW call), the context raster create 0x8048DD2C(0) -> `[0x620C]` with its flags word at +0x50 rewritten
(clear low byte, |= 2; then |= 0x1100 on bits 8..15 ...), 0x8048AEC4(4, ...)/0x8048AFC0(.., 0, 9)/0x8000540C (memset 0xFF, 0x20)/0x8048AB90, result in
`[0x6208]`. **0x8049847C** = RwRaster-style teardown: 0x8048AC2C(`[0x6208]`), `[0x6208] = 0`, 0x8048DA50(`[0x620C]`, 0), 0x8048DDDC(`[0x620C]`), `[0x620C] = 0`.

**Dependencies and ownership rule for the unit.** GX leaves are axfx-effects' (owned, silent in the lifted build; their effect on guest state is
nil except the draw-sync token which NativeGxSync advances); VI (0x8037E4F0 / E534 / ECD4 / F19C / FD38 / FE68 / FEDC / 80004 / 80138) are the
VI owners' (native-boot's 0x8037ECD4 is the one still missing); OSSleepThread / OSWakeupThread / OSInitThreadQueue are cri-threads'. The native
unit owns **ShowRaster, the two VI callbacks and the draw-sync callback, 0x80494CD4, GcnSystem values 0-3, 7, 17, 18, 0x80494EB4, 0x80496440,
0x8049AC48, 0x804981E8, 0x8049847C**: the callbacks run natively when the VI/GX owners fire them (the registration calls become native stores
of the callback addresses into the owners' callback slots, never a guest call into the callback bodies).

**Open question for axfx-effects.** Their replay report put reads of gx+0x1E4 / +0x1EC and writes of gx+0x1EC / gx+0 "at 0x80494F18". That
PC is the case-7 query above (no memory access). The access is probably in the callee chain of case 2 (0x80397228 / 0x80396CCC / 0x80396BD4 /
0x80394AF4 family, which they own) mis-attributed to the nearest RW symbol; nothing in the F510 body itself touches `__GXData`.


## Step 13 - the raster size family, the quantiser end, and the frame audit

* **Native, exact (contract tests with mutations):** 0x80492B04 (floor-log2 loop; a negative argument loops forever in the original, so the owner
  stops), 0x804965A8 (bytes of every mip level), 0x8049A57C (GcnRasterGetSize), 0x80491448 (the quantiser's end: frees the node tree through the engine
  free function [[r13 + 0x615C] + 0x11C], clears the root, frees the allocator through RenderWare core 0x80480D2C) with its tree-free 0x80491124
  (the original unrolls eight levels of one recursion; the order of the frees - children in order, then the node - is what is compared, over random
  16-ary trees up to depth 12). 0x80496478 (one mip level) is card-saves' owner; my copy was dropped, the 804965A8 / 8049A57C paths keep a private
  helper with the original's exact error report until card-saves' exact LevelBytes (385612f) is merged, then they switch to its owner.
* **Why 0x80491448 was missed the first time.** The plan classifies a row by the skeleton function that CONTAINS the address
  (tools/platform/what_if.py `services()`), and the manifest has entries only at function starts, so an exact-address manifest lookup returned
  nothing for it. Every guest_call target of this step is now checked with `what_if.services()`: all `layer:rw` except 0x80491448.
* **Frame audit (guest_call temporaries).** guest_call runs the callee with the current r1 as the frame top. Every owner of this step that passes
  a pointer to a temporary (the quantiser context, the RwErrorSet structs, the size family's mip-count out word) holds its own frame (`Frame`:
  r1 lowered below the temporaries, the back chain written, r1 restored on every exit path). The contract tests' RenderWare-core doubles now scribble 256
  bytes below r1 (a six-word loop): moving the quantiser context or the error struct below r1 is told apart (2,258 / 4,481 failures); the clean owner has none.
* **Duplicates avoided:** 0x80493A40 (RwImageGetRaster) and its three bodies are sdk-identify's (native_rw_image.cpp); an attempt of mine to write them again
  was reverted before commit - check `native-owners*.txt` and `git grep` the address on all branches before starting a row.
* **Lifted build** (agent/vi a2c5b31 = gc-removal + this step), default config, Westopolis: captures 22fdf629 / d22a1750 / b6511d3d / 37299f60 / ae967db4,
  PCM 46a70a1051347069 twice (main's canonical values). removal_plan.py: 193 platform bodies compiled; runtime_coupling.py exit 0: 2 native -> SDK
  calls (limit 6), 0 unclassified. The recordings never enter these rows: this shows nothing is perturbed, not that the owners run in-game.
* **Equivalent mutant:** the 32-byte rounding of a mip level's size never changes a result (every kind's size is already a multiple of 32).

## Step 14 - the display swap is native (lifted build)

Commits c6b66f3 .. 3e0d4ae on agent/vi (merged with gc-removal round 17). Specification: Step 12. Code: `native_rw_display.cpp`, installed by
`scaffold::install_display_owners` from `LiftedRender::install_owners` only (the legacy renderer observes ShowRaster, so the owners are not part of
`rw_raster::install`); rows in `extract/native-owners-r2.txt`.

* **Owned:** ShowRaster 0x80495CBC (+ its resume row 0x80495D38, taken when an interrupt became deliverable during the wait), the draw-sync callback
  0x80494A2C, the VI pre / post-retrace callbacks 0x80494B0C / 0x80494BE8, the video mode setter 0x80494CD4, GcnSystem 0x80494EDC (requests 0-11, 17-19, 22
  and the default), the render-state defaults 0x804981E8, the context teardown 0x8049847C, and the default standard function 0x80494CCC. The plugin
  registrations 0x80494EB4 / 0x80496440 / 0x8049AC48 are native-boot's (native_rw_registry.cpp), not mine.
* **Hooks, not leaves, where the lifted build lowered a site:** ShowRaster's single-field GXCopyDisp is `render_submit_frame(xfb, clear)` and the
  GXSetZMode of 0x804981E8 is `rw_render_state_reset(1, 3, 1)` (the scaffold's functions, passed in as `DisplayHooks`). Their lift-sites lines, the three
  wait points of GcnSystem (wait-points-retired.txt) and its jump table entry were retired with the bodies. Every other GX / VI / OS call is an owner entry
  (`serve_owner`); RenderWare core and the engine's memory functions are `guest_call`.
* **Waits:** ShowRaster's full ring sleeps through os.threads' OSSleepThread owner (interrupts disabled around it, as the original); request 2's two
  VIWaitForRetrace waits are the same words with the same sleep (VIWaitForRetrace itself stays guest and unowned); request 3's busy wait advances the clock
  with `Cpu::idle_wait` and takes a deliverable interrupt by handing back and restarting the request - no spinning, a loud stop only if no event can arrive.
* **Evidence:** rw_raster_contract_tests, original words vs owners, SDK leaves replaced on both sides by logging test owners ({id, r3..r9, f1..f6} per call):
  3,776 display cases, 15,649 leaf calls compared (ShowRaster all three paths x ring states x EE x tokens; the three callbacks x ring / handshake /
  callback-flag states; the video mode setter; every GcnSystem request x counts x argument variants). A harness bug found on the way: `Machine::run` sets r5 / r6
  from its own parameters, so the first test version never applied the count / argument variants (two mutants survived); fixed, 0 failures after.
  Mutation sweep (ONLY_TESTS=display, 18 mutants of native_rw_display.cpp): 18 of 18 told apart. The first full sweep left two survivors, both real test
  gaps, not equivalent mutants: the second VIWaitForRetrace (the mode word's low bit was never set) and the teardown's zeroing of the context word (the
  context started at 0); the tests now set both.
* **Lifted build** (agent/vi 3e0d4ae), default config, Westopolis: PNGs at 5e6 / 2e8 / 9e8 / 2e9 / 3.5e9 = 22fdf629 / d22a1750 / b6511d3d / 37299f60 / ae967db4 and
  PCM 46a70a1051347069 on both runs (main's canonical values). Owner hits in a 90 s run: ShowRaster x5,360 (its resume row x5,335), draw-sync callback x5,359,
  pre / post-retrace callbacks x5,394 each, GcnSystem x10,845, the default standard function x102,256. removal_plan.py: 133 platform bodies compiled
  (graphics.rw-gamecube-driver 39); runtime_coupling.py exit 0 (0 unclassified).

## Step 15 - an unexplained one-pixel observation (2026-11-09), not a pass

agent/vi 4092532 (lifted, default config, Westopolis): the 3.5e9-tick capture of the FIRST gate run of that exe gave 9ef2c884 instead of the canonical
ae967db4; PCM and the other four captures matched. The decoded difference is one pixel, (x 1180, y 813): RGBA a16026ff canonical, a16025ff in the odd run
(blue channel one LSB). Two later reruns of the same exe at the same tick gave ae967db4. Per main, the replay is tick-driven, so this is not read as a wall-clock
effect; the 2026-10-07 precedent (single-LSB pixel differences on the RTX 4080 after a pipeline-state compile on the first run of an exe's bytecode, and in the MSAA
glare pass after a GPU idle pause) fits the facts that the failing capture was the exe's first run on this machine and that removal_plan / runtime_coupling were running
beside it. Neither is proven. The two clean reruns do NOT clear it. To settle it: render that tick on WARP several times, including the first run after clearing the driver's
shader cache for that exe (and check the display config's MSAA setting; none was set through the environment). Not done: lower priority than the frontier rows. Gate runs of this
kind should be made on an otherwise idle machine.

## Step 16 - what is left of the platform SDK in the lifted build (2026-11-09, agent/vi bc37691 = gc-removal fixed tip + vi)

**Measured, not estimated:** `removal_plan.py --release-dir .build/watch` on the lifted build of agent/vi 4092532 (the generator's own live graph; tools/platform/removal_plan.py
checks its liveness equals the generator's): **59 platform-SDK bodies are emitted** (of 39,356 rows, 37,827 live), 24 of them entered from outside the SDK ("frontier"):

| service | emitted bodies | frontier rows (entered from outside) | domain / holder evidence |
|---|---:|---:|---|
| sdk:unidentified (the AX library and GX region callbacks) | 25 | 10 | audio-ax / axfx-effects / sdk-identify docs |
| os.core | 11 | 1 | cri-threads, os-other docs (OSInit family, scheduler support) |
| graphics.rw-gamecube-driver | 10 | 10 | axfx-effects (TextureBind 0x8049AC88, Im bind 0x80498854), os-other (the three RasterRender wrappers with 0x80499924), card-saves (the five 0x8049A304..0x8049A480 interior labels of 0x8049A124) |
| os.other | 3 | 0 | cri-threads, os-other docs |
| os.threads | 3 | 2 | cri-threads (the scheduler: 0x803787B0 / 0x803788E8) |
| audio.dsp | 3 | 0 | audio-ax (called from the AX frame 0x80385798 / 0x80385950) |
| graphics.gx | 2 | 0 | axfx-effects (called from 0x80393634) |
| os.interrupts | 1 | 0 | called from 0x80372C68 |
| graphics.vi | 1 | 1 | VIWaitForRetrace 0x8037ECD4: cri-threads' wait work |

Every row, with what keeps it alive (the live graph's predecessors: a "root seed" is a runtime literal or census root, "data seed" a word of the DOL's data, `code:` a live caller in the
named layer) and the evidence of a holder (an unmerged branch ADDING that address in code, an owners list or its docs, then any docs/agents file that names it). `none` = no branch adds it
and no doc names it:

| address | bytes | name | service | kept alive by | holder (code / docs on agent branches) |
|---|---:|---|---|---|---|
| 0x8037292C | 292 | fn_8037292c | os.other | called by 0x8037292C, 0x80373100, 0x8037310C | docs: cri-threads ; mentioned in cri-threads,os-other |
| 0x80372A50 | 304 | fn_80372a50 | os.other | called by 0x80372A50, 0x80372B78, 0x80373100 +1 | docs: cri-threads ; mentioned in cri-threads,native-boot,os-other |
| 0x80372B78 | 8 | fn_80372b78 | os.other | called by 0x80372A50, 0x80372B78 | docs: cri-threads ; mentioned in cri-threads,os-other |
| 0x80372B80 | 92 | fn_80372b80 | os.core | called by 0x80372B80, 0x80372BB4, 0x80372BD4 +3 | mentioned in cri-threads,native-boot,os-other |
| 0x80372BB4 | 32 | fn_80372BB4 | os.core | called by 0x80372B80, 0x80372BB4 | **none** |
| 0x80372BD4 | 8 | fn_80372BD4 | os.core | called by 0x80372B80, 0x80372BB4, 0x80372BD4 | **none** |
| 0x80372BDC | 12 | fn_80372bdc | os.core | called by 0x80372BDC, 0x803787B0 | **none** |
| 0x80372BE8 | 128 | fn_80372be8 | os.core | called by 0x80372BE8, 0x803787B0 | mentioned in cri-threads,os-other |
| 0x80372C68 | 216 | fn_80372c68 | os.core | called by 0x80372C68, 0x80372D24, 0x803787B0 +1 | mentioned in cri-threads,os-other |
| 0x80372D24 | 28 | fn_80372D24 | os.core | called by 0x80372C68, 0x80372D24 | mentioned in cri-threads,os-other |
| 0x80372D78 | 224 | fn_80372d78 | os.core | called by 0x80372D78, 0x80372D9C, 0x803787B0 +1 | mentioned in cri-threads,os-other |
| 0x80372D9C | 188 | fn_80372d9c | os.core | called by 0x80372D78, 0x80372D9C | mentioned in cri-threads,os-other |
| 0x80373100 | 132 | fn_80373100 | os.core | root seed via 0x80373100; called by 0x80373100, 0x8037310C | docs: cri-threads ; mentioned in cri-threads,os-other |
| 0x8037310C | 120 | fn_8037310C | os.core | called by 0x80373100, 0x8037310C | mentioned in os-other |
| 0x80376128 | 8 | fn_80376128 | os.interrupts | called by 0x80372C68, 0x80376128 | mentioned in cri-threads,os-other |
| 0x803787B0 | 552 | fn_803787b0 | os.threads | root seed via 0x803788FC; root seed via 0x80378904; called by 0x803787B0, 0x803788E8, 0x803789A8 | mentioned in cri-threads,native-boot,os-other |
| 0x803788E8 | 192 | fn_803788e8 | os.threads | root seed via 0x803788FC; root seed via 0x80378904; called by 0x803787B0, 0x803788E8 | mentioned in cri-threads,native-boot,os-other |
| 0x803789A8 | 48 | fn_803789a8 | os.threads | called by 0x803787B0, 0x803788E8, 0x803789A8 | mentioned in cri-threads,os-other |
| 0x8037ECD4 | 84 | fn_8037ecd4 | graphics.vi | code:game fn_8032d1d0 (via 0x8037ECD4); middleware.cri fn_803c53fc (via 0x8037ECD4); middleware.cri fn_803c5454 (via 0x8037ECD4); called by 0x8037ECD4 | mentioned in cri-threads |
| 0x80384718 | 116 | fn_80384718 | sdk:unidentified | root seed via 0x80384718; called by 0x80384718, 0x80385798 | mentioned in audio-ax,sdk-identify |
| 0x803848F4 | 32 | fn_803848f4 | sdk:unidentified | called by 0x80384718, 0x803848F4 | mentioned in audio-ax,sdk-identify |
| 0x80384914 | 16 | fn_80384914 | sdk:unidentified | root seed via 0x80384914; called by 0x80384914 | mentioned in audio-ax,sdk-identify |
| 0x80384924 | 28 | fn_80384924 | sdk:unidentified | called by 0x80384718, 0x80384924 | mentioned in audio-ax,sdk-identify |
| 0x80384940 | 148 | fn_80384940 | sdk:unidentified | called by 0x80384718, 0x80384940 | mentioned in audio-ax,sdk-identify |
| 0x80384E14 | 572 | fn_80384e14 | sdk:unidentified | called by 0x80384E14, 0x80384EC8, 0x80384F24 +2 | mentioned in audio-ax |
| 0x80384EC8 | 92 | fn_80384ec8 | sdk:unidentified | called by 0x80384E14, 0x80384EC8 | **none** |
| 0x80384F24 | 104 | fn_80384f24 | sdk:unidentified | called by 0x80384E14, 0x80384EC8, 0x80384F24 | **none** |
| 0x80384F8C | 196 | fn_80384f8c | sdk:unidentified | called by 0x80384E14, 0x80384F24, 0x80384F8C | **none** |
| 0x80385070 | 60 | fn_80385070 | sdk:unidentified | called by 0x80385070, 0x80385798 | mentioned in audio-ax |
| 0x80385798 | 440 | fn_80385798 | sdk:unidentified | root seed via 0x80385798; root seed via 0x803857CC; root seed via 0x80385808; called by 0x80385798, 0x80385950, 0x80385A20 | mentioned in audio-ax,sdk-identify |
| 0x80385950 | 208 | fn_80385950 | sdk:unidentified | root seed via 0x80385950; root seed via 0x80385990; called by 0x80385950 | mentioned in audio-ax,sdk-identify |
| 0x80385A20 | 96 | fn_80385a20 | sdk:unidentified | root seed via 0x80385A54; called by 0x80385950, 0x80385A20 | mentioned in audio-ax,sdk-identify |
| 0x80385A80 | 28 | fn_80385a80 | sdk:unidentified | called by 0x80385A20, 0x80385A80 | **none** |
| 0x80385FB0 | 1016 | fn_80385fb0 | sdk:unidentified | called by 0x80385798, 0x80385FB0 | mentioned in audio-ax |
| 0x8038646C | 8 | fn_8038646c | sdk:unidentified | called by 0x80385798, 0x8038646C | mentioned in audio-ax,sdk-identify |
| 0x80386474 | 1868 | fn_80386474 | sdk:unidentified | root seed via 0x80386474; called by 0x80386474 | mentioned in audio-ax,sdk-identify |
| 0x80387754 | 72 | fn_80387754 | sdk:unidentified | called by 0x80385798, 0x80387754 | mentioned in audio-ax |
| 0x80388B20 | 948 | fn_80388b20 | sdk:unidentified | called by 0x80388B20, 0x803890B0 | mentioned in axfx-effects,sdk-identify |
| 0x803890B0 | 48 | fn_803890b0 | sdk:unidentified | code:game fn_8043f304 (via 0x803890B0); code:game fn_8043f33c (via 0x803890B0); called by 0x803890B0 | mentioned in audio-ax,axfx-effects,sdk-identify |
| 0x803890E0 | 408 | fn_803890e0 | sdk:unidentified | called by 0x803890E0, 0x8038980C | mentioned in axfx-effects,sdk-identify |
| 0x80389278 | 492 | fn_80389278 | sdk:unidentified | called by 0x80389278, 0x8038980C | mentioned in axfx-effects,sdk-identify |
| 0x8038980C | 848 | fn_8038980c | sdk:unidentified | code:game fn_8043f304 (via 0x8038980C); code:game fn_8043f33c (via 0x8038980C); called by 0x8038980C | mentioned in audio-ax,axfx-effects,sdk-identify |
| 0x80389B5C | 464 | fn_80389b5c | sdk:unidentified | code:game fn_8043f304 (via 0x80389B5C); code:game fn_8043f33c (via 0x80389B5C); called by 0x80389B5C | mentioned in audio-ax,axfx-effects,sdk-identify |
| 0x8038C274 | 16 | fn_8038c274 | audio.dsp | called by 0x80385798, 0x8038C274 | mentioned in audio-ax |
| 0x8038C2AC | 20 | fn_8038c2ac | audio.dsp | called by 0x80385798, 0x8038C2AC | mentioned in audio-ax |
| 0x8038C43C | 200 | dsp_8038c43c | audio.dsp | called by 0x80385950, 0x8038C43C | mentioned in audio-ax |
| 0x80393634 | 288 | fn_80393634 | sdk:unidentified | root seed via 0x80393634; root seed via 0x80393730; called by 0x80393634 | mentioned in axfx-effects,card-saves,sdk-identify |
| 0x8039822C | 8 | fn_8039822c | graphics.gx | called by 0x80393634, 0x8039822C | **none** |
| 0x80398234 | 24 | fn_80398234 | graphics.gx | called by 0x80393634, 0x80398234 | **none** |
| 0x80498854 | 188 | fn_80498854 | graphics.rw-gamecube-driver | code:rw fn_804a0b68 (via 0x80498854); called by 0x80498854 | docs: axfx-effects ; mentioned in axfx-effects,native-boot,os-other |
| 0x80499A9C | 40 | fn_80499a9c | graphics.rw-gamecube-driver | data seed via 0x8051D494; called by 0x80499A9C | docs: axfx-effects ; mentioned in axfx-effects,gx-rw-plan,native-boot,os-other |
| 0x80499AC4 | 40 | fn_80499ac4 | graphics.rw-gamecube-driver | data seed via 0x8051D4A4; called by 0x80499AC4 | docs: axfx-effects ; mentioned in gx-rw-plan,os-other |
| 0x80499AEC | 40 | fn_80499aec | graphics.rw-gamecube-driver | data seed via 0x8051D49C; called by 0x80499AEC | docs: axfx-effects ; mentioned in gx-rw-plan,os-other |
| 0x8049A304 | 516 | fn_8049a304 | graphics.rw-gamecube-driver | code:game fn_8040fb60 (via 0x8049A304); code:game fn_8040fd70 (via 0x8049A304); called by 0x8049A304, 0x8049A364, 0x8049A3B4 +2 | mentioned in card-saves,gx-rw-plan |
| 0x8049A364 | 420 | fn_8049a364 | graphics.rw-gamecube-driver | code:game fn_8040fb60 (via 0x8049A364); code:game fn_8040fd70 (via 0x8049A364); called by 0x8049A304, 0x8049A364, 0x8049A3B4 +2 | mentioned in gx-rw-plan |
| 0x8049A3B4 | 340 | fn_8049a3b4 | graphics.rw-gamecube-driver | code:game fn_8040fb60 (via 0x8049A3B4); code:game fn_8040fd70 (via 0x8049A3B4); called by 0x8049A304, 0x8049A364, 0x8049A3B4 +2 | mentioned in gx-rw-plan |
| 0x8049A41C | 236 | fn_8049a41c | graphics.rw-gamecube-driver | code:game fn_8040fb60 (via 0x8049A41C); code:game fn_8040fd70 (via 0x8049A41C); called by 0x8049A304, 0x8049A364, 0x8049A3B4 +2 | mentioned in gx-rw-plan |
| 0x8049A480 | 136 | fn_8049a480 | graphics.rw-gamecube-driver | code:game fn_8040fb60 (via 0x8049A480); code:game fn_8040fd70 (via 0x8049A480); called by 0x8049A304, 0x8049A364, 0x8049A3B4 +2 | mentioned in gx-rw-plan |
| 0x8049AC88 | 2272 | fn_8049ac88 | graphics.rw-gamecube-driver | code:rw fn_80449d54 (via 0x8049AC88); code:rw fn_8044eec4 (via 0x8049AC88); code:rw fn_80463964 (via 0x8049AC88); called by 0x80498854, 0x8049AC88 | docs: axfx-effects ; mentioned in axfx-effects,native-boot,os-other |

**Reading the table.** Only the 24 frontier rows matter for removal: the other 35 bodies are kept alive solely by calls from frontier bodies and disappear when their entry is owned.
Every frontier row has a domain agent in the evidence above; none is "free" in the sense of no claim. Rows whose liveness is a runtime literal that no code uses at run time:
the entry probes of `measure_probes.cpp` (a measurement table, not behaviour) are the only seed of 0x80384914 (16 bytes) and 0x80386474 (the AX parameter-block sync, 1,868 bytes); the
other probe literals also appear as data or game seeds. Dropping the probe-only seeds (the planner's `drop_seeds`) frees exactly those two bodies: 59 -> 57. That is audio-ax's file and decision.


## Step 17: VIWaitForRetrace 0x8037ECD4 and the two graphics.gx bodies (2026-11-09)

Released by main (cri-threads will not start it).

**VIWaitForRetrace** (14 words, decoded from main.dol; note `0x7C1E0040` is `cmplw`, an unsigned compare: the first test run caught my
`cmpw` reading). Owner `runtime/src/gekko/native_vi_wait.cpp`, lifted-only (extract/native-owners-r2.txt), installed from
`os_threads::install_waits`, so it exists exactly where the native OSSleepThread does. It keeps the original's frame, calls the two
interrupt gates through their own owners (so r3/r4/r5/CR0 and the pending-interrupt rule are the gates'), the native sleep, and has two
resume rows because the gates can hand back: 0x8037ECEC (interrupt taken at the Disable gate's entry) and 0x8037ED10 (interrupt made
deliverable by the restore). Wait point 0x8037ED04 retired.
- Contract test `rw_raster_contract_tests` (ONLY_TESTS=vi): 218 cases original words vs owner: EE 0/1, retrace count incl. signed
  boundaries, deltas, wake on sleep 1/2/3, spurious wakes, XER[SO], interrupt pending at entry (taken at 0x8037ECD4 by both), at the
  Disable gate, and during the sleep (taken before 0x8037ED10; asserted by pc). Compared: all registers r0-r31, CR, MSR, XER, CTR, stack
  frame words, every leaf call (id, r0, r1, r3-r5, r30, r31, LR, MSR, CR0), the native interrupt line. The owner machine has the original
  body blanked, so only the owner and its two rows can run.
- Mutations: 30 mutants. 28 caught; the first sweep had three survivors: two (resume rows missing) were a test gap (original words ran
  as a fallback) and are caught after blanking the body (76 / 9 FAIL lines); one, "r3 forced to 0", is equivalent (the restore gate
  always returns 0 because interrupts are disabled there). SO-dropped survived until an XER[SO] case was added. A mutant that never
  ends the wait had hung the first sweep; the test now stops after 40 sleeps.

**GX getters** 0x8039822C (`lwz r3,20(r3); blr`, Dolphin sig name wrong, shape GXGetTexObjFmt) and 0x80398234 (GXGetTexObjMipMap:
r3 = flags&1, r0 = cntlzw result) in `native_gx_state.cpp`, both trees, rows in native-owners.txt; `gx_state_contract_tests` 12 new
cases (r0 compared), 6 mutants all caught. Their only caller was 0x80393634 (default texture-region callback).

**Gates** (lifted build cand16 = a940053, default config): 5 PNGs 22fdf629 / d22a1750 / b6511d3d / 37299f60 / ae967db4 (canonical, first
run), PCM 46a70a1051347069 twice. Planner on this tree: 38 platform bodies compiled, 20 frontier; runtime_coupling: 0 unclassified uses.
Not run: the full ctest suite, other stages, a gameplay session. Machine was not idle during the gates (other sessions).

**3.5e9 one-pixel observation: not explained.** It occurred once (first run of cand15 under load), not on any other run (cand16 and two
reruns canonical); WARP renders were deterministic; shader-cache clearing was impossible. No root cause found; it is not counted as cleared.
