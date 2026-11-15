# cs2 field `ram` (2026-11-13): where the RAM goes, and what was removed

Branch cs2/ram from 4ea5050. Candidate exe: `.build/cand/ram-7e94964f.exe` (+ .pdb, .map), md5 7e94964fb7e88575304505921681fdf8,
built from committed source 7fbabc6 (probe build: `ram-probe2-6d21ea7c.exe`, measurement only, never a gate exe).
Working notes and proposals for the other fields: `cs2-notes/ram.md`.

## 1. How the RAM was measured

| tool | what it gives |
|---|---|
| `tools/ramwalk.ps1` (commit 16a4263) | VMMap-style walk of a running process: VirtualQueryEx over the address space, QueryWorkingSetEx per page (resident bit), PEB heaps, TEB stacks. The process is suspended (NtSuspendProcess) during a snapshot; `-Name <exe> -IntervalMs N` snapshots a run until it exits. Summaries: `.build/ram/summ.py`. |
| xperf ETW (`PROC_THREAD+LOADER+VIRT_ALLOC -stackwalk VirtualAlloc`) + `.build/ram/etwparse.py`, `match.py` | Commit call stacks resolved against the exe's linker map. Exact for direct allocations; a heap-segment commit is charged to whoever grew the heap. |
| `SHADOW_RAM_PROBE=ON` (commit 16a4263, off by default) | Per-call-stack live / total bytes of the exe's `operator new` (`GEKKO_RAM_PROBE=<tsv>`, `.build/ram/probereport.py`). The owner of the host C++ heap, exactly. |
| existing `GEKKO_GPU_MEMORY=<csv>` | D3D12MA blocks and allocations by name (render side). |
| `session.json` host_stats.memory | peak working set / peak private, the benchmark's numbers. |

## 2. The map (base 4ea5050)

Headless, Westopolis 316 s tape, final state: peak WS 288 MiB, peak private 201 MiB.

| class | commit | resident |
|---|---|---|
| private (heap + VirtualAlloc) | 161 MiB | 118 |
| mapped host files (3,577 files, 1.78 GiB of address space) | 1,782 | **47** |
| mapped, pagefile-backed (guest windows 33 MiB commit, a second view of the same sections counted twice) | 68 | 42 |
| image (exe 101 commit / 49 resident, nvwgf2umx 86/10, nvgpucomp64 94/4, system DLLs) | 360 | 84 |

Private 161 MiB, by owner:
- **~109 MiB: the D3D12 device, even headless** (gpu field). Write-combined regions (protect 0x404) 30.7 + 24.2 + 16.0 MiB
  (the last is a D3D12MA 16 MiB block), the driver's own 22.4 + 15.8 MiB made inside `render::Device::create` / `Queue::create`.
  GEKKO_GPU_MEMORY: DEFAULT blocks 34.2 MiB (28.3 used, 487 allocations), UPLOAD blocks 17.8 MiB (2), non-local usage 49.9 MiB.
- ~51 MiB host C++ heap and tables. operator-new live bytes 48.7 MiB at the end of the run (probe), by owner:
  button-glyph scaled sheets 7.3 (+ 1.2 captured copy), Options_*.txd tile-art replacements 6.4, dispatch table 4.7
  (`RecompiledCode::install`, cpu), PRS decode-ahead 4.1 (audio), mount `read_tree` copies of unmodified originals 3.1, Hungarian
  font builder caches and variants ~5.3, other replacement copies, namespace / registry strings ~1.5.
- Thread stacks are not a cost: 21 stacks with 1 MiB reserved and 8-28 KiB committed; the main thread's 16 MiB (/STACK) is
  reserve only (348 KiB committed). Reserve counts to neither private bytes nor working set. Left as is.

Working set only: the mount read ~68 KiB of each of 477 `.txd` files through the mapped view (187 button-glyph files + the
replacement builders) = 28.6 MiB of the 47 MiB mapped-file WS, resident from the first seconds. The rest is real reads
(adx / mlt / one, ~14 MiB).

### The ~100 MiB windowed-only private (windowed 90 s run: peak WS 320, peak private 307 MiB)
The extra private is the D3D12 resources of the swap-chain path. Write-combined private grows 71 -> 159.5 MiB commit (resident
28.7 -> 44.7), in step with the D3D12MA DEFAULT heap 28 -> 107 MiB used: on this driver (RTX 4080) each resource in a DEFAULT
heap is also billed to the process as write-combined private commit, so private bytes follow the resource bytes about 1:1.
GEKKO_GPU_MEMORY at the end of the windowed run (names from D3D12MA): `render_target_memory` 48 MiB (2 blocks), `view depth` 16 MiB,
`shadow.upload_stream` 16 MiB (UPLOAD ring), `shadow.asset_texture` 14.65 MiB (280 textures), `shadow.mesh_streams` 8.6 MiB,
`shadow.view_history` 7.5 MiB, glare targets ~6.4 MiB; UPLOAD blocks 26.2 MiB (25 used by 2 allocations: the ring + the frame ring),
READBACK 8.4 MiB (one empty block). Total D3D12MA 119 MiB in 133 DEFAULT blocks. These belong to the gpu field: proposals in
`cs2-notes/ram.md` (sizing of `render_target_memory`, the 16 MiB depth, the ring, the empty readback block) with their cost.

## 3. What changed (all behind one switch: `GEKKO_CS2_RAM=0` restores the previous behaviour; default on)

| commit | change | measured effect |
|---|---|---|
| 0f282e1 | Replacement files (fonts, Options txd + Display tile, PalSel, front-end tables, save text) held as patches over the mapped original instead of private copies (`MemoryProvider::add_delta`; `AssetSystem::mount` compares each result with its original; the delta is verified before it is used) | held privately 15.24 MiB -> 5.06 MiB (130 copies 2.79 + 244 patches 2.27 MiB serving 12.4 MiB) |
| 01c3d98 | Button glyphs: scaled sheets made when a sheet is first served (the existing `sharp_sheet` fallback; 4 of 27 sheets are served in a Westopolis run) instead of all 27 at attach; attach-time reads of the 187 txd files release their pages | ~8 MiB private; mapped-file WS 47.0 -> 25.7 MiB headless (9 MiB of that is the digest diagnostic's own reads; 15.1 MiB in the windowed ON run) |
| 7fbabc6 | `hungarian::release_build_caches()` after the mount (font cache + glyph atlas were process-lifetime statics) | ~2 MiB private |

Why the bytes are exact: a delta is stored only if `original + patches == replacement` (checked at the mount), and reads
reconstruct from the same original through the same storage reader. Variants not selected at the time (the Hungarian
fonts) are covered too: `GEKKO_CS2_RAM_DIGESTS=<file>` writes the FNV-1a of every held content in creation order; ON and OFF
files are byte-identical (374 contents). The lazy sheets are the same function over the same inputs: `button_glyphs_tests` prints
`sharp-sheet digest eaf55074c52de1ff` (every sharp sheet, all layouts) in both modes.

The OFF path costs nothing it did not cost before (it is the old code: full copies, worker-made sheets, caches kept).
Costs of the ON path, deterministic: mount reads each replaced original once more (~15 MiB through the file cache, once);
the first serve of a sheet makes its scaled art on the calling thread (4 sheets in the tape instead of 27 on a worker at boot).
No timing claim is made; "window candidate" if the coordinator wants the mount time measured (it is not on the frame path).

## 4. Gates (candidate 7e94964f, `.build/cand/ram-7e94964f.exe`)

`bash wt-cs2-integrate/.build/v/gates.sh <exe> <dir> shots` and `cmp_gates.sh`; base = coordinator's gates of 4ea5050.

| comparison | state.json | PCM | behaviour_diff | 10 screenshots |
|---|---|---|---|---|
| ON vs base (82b65d67b2e0 / c64ee7883f980f73) | identical | identical | VERDICT: PASS | 10 of 10 identical |
| OFF (`GEKKO_CS2_RAM=0`, same exe) vs base | identical | identical | PASS | 10 of 10 |
| ON vs OFF | identical | identical | PASS | 10 of 10 |

Windowed 720p, 90 s (slot_bench, cs2-window): state.json identical ON vs OFF (2df97f95cb7d). Final Haunt not run: nothing here
touches motion, collision or rendering (the sharp-sheet bytes are proven equal by the digest above).
Tests (tests tree, Release): `asset_runtime_tests` and `asset_runtime_tests_cs2_ram_off` pass (random reads over identical,
few-range, mostly-changed, longer and grouped replacements; negative controls: one wrong patch byte and one missing patch are
caught by the same comparison); `button_glyphs_tests` and `..._cs2_ram_off` pass with the same sharp-sheet digest.

## 5. RAM deltas, measured the way the benchmark measures (session.json host_stats.memory)

| run (same exe, only the switch differs) | peak WS | peak private |
|---|---|---|
| headless 316 s tape, OFF (= 4ea5050 behaviour) | 284.4 MiB | 200.7 MiB |
| headless 316 s tape, ON | **242.5 MiB (-41.9, -14.7 %)** | **179.8 MiB (-20.9, -10.4 %)** |
| headless + 10 native screenshots, OFF / ON | 327.8 / 286.8 | 321.9 / 304.6 |
| windowed 720p, 90 s, OFF | 320.3 MiB | 306.9 MiB |
| windowed 720p, 90 s, ON | **266.4 MiB (-53.9, -16.8 %)** | **285.8 MiB (-21.1, -6.9 %)** |

Probe build (operator-new live bytes at the end of the headless run): 48.67 -> 24.95 MiB.
Run-to-run noise of these counters is a few MiB (image working set moves with the OS's paging); the deltas are 5-10x that.

## 6. Tried, rejected, left, open

- Thread stack reserve (/STACK:16777216 and 1 MiB thread stacks): reserve only, no private or WS cost. Not changed.
- Native asset manager: already not constructed in the lifted build (ES-M1); nothing to remove.
- Legacy OS heap (guest 0x80610000..0x81800000, 17.9 MiB) is fully resident (the game's arena, RenderWare resources); lazy
  commit would need guest-memory fault handling: guest_memory.h belongs to cpu and the gain is commit, not working set. Not done.
- `button_glyphs` per-family patched spans and `options_display` font copies (~1 MiB) kept: read again on every stage load.
- Dispatch table `RecompiledCode::install` 4.66 MiB (a `std::vector<int>` over the dense span 0x80003100-0x804AAC3C) -> cpu
  field: narrower entries would halve it. PRS decode-ahead (4.1 MiB live, 9.5 MiB at peak, budget 96 MiB) -> audio field.
- Image working set 49 MiB of the exe's 85 MiB `.text` is resident: function ordering (profile-guided /ORDER) would shrink it;
  not attempted (build-system work, no gate cheaper than a full timing window).
- Open: a one-shot read of ADX/MLT/ONE bytes through the mapped view leaves ~14 MiB in the working set for the run; releasing
  pages after each large read would cost a syscall per read and soft faults on a re-read. Not done without a timing window.
- Open (gpu): the windowed +100 MiB private follows the D3D12 DEFAULT heap bytes; any pixel-identical cut of render targets,
  history or depth lowers private bytes 1:1 on this driver.
