# opt-behave (round 3): the behaviour-equivalence gate

Branch agent/opt-behave from d533d86. Every Tier M ("modern math") change must pass this gate before it merges.

## What exists

| piece | file |
|---|---|
| runtime options `--behaviour-trace <file.jsonl>`, `--behaviour-layout <file>`, `--behaviour-every <n>` | `runtime/src/shadow_main.cpp` (class `BehaviourTrace`, hooked into both host-service lambdas; one flag test when off) |
| field list for the PAL build, one line per field with its evidence | `tools/behaviour_layout.txt` |
| benchmark-tape run with the trace (same invocation as the coordinator's `run.sh`) | `tools/behaviour_run.sh` |
| comparison, per-field epsilons, first divergence, max deviation, verdict, exit code | `tools/behaviour_diff.py` |
| test-only negative-control knob `GEKKO_BEHAVIOUR_NUDGE_LENGTH=1` (Length result +1 ULP) | `runtime/src/gekko/native_vec_math.cpp` (separate serve function installed only when the variable is set; nothing on the normal path) |

Why a new option and not `--report-peek`: report-peek writes ranges once, at the end of the run. The gate needs the state at
every moment (first divergence frame) and event lines, so the trace samples on the host-service grid (5 ms of game time,
a fixed grid, so row n of two runs of the same tape is the same moment) and reuses report-peek's pointer-path syntax.
Read-only (`GuestMemory::peek`), game thread, committed state. Trace on vs off: `state.json` md5 identical (3a75268e85cd both).

## How other agents run the gate

```
# 1. base and candidate, same tape, headless, unpaced (35 s wall each); both through capped_run.ps1 -CpuPercent 6
bash tools/behaviour_run.sh <base.exe>      316000 <dirA>
bash tools/behaviour_run.sh <candidate.exe> 316000 <dirB>
# 2. compare
python tools/behaviour_diff.py <dirA>/behaviour.jsonl <dirB>/behaviour.jsonl      # exit 0 PASS / 1 FAIL
python tools/behaviour_diff.py --spread a1.jsonl a2.jsonl a3.jsonl                # A/A spread
```
Needs an exe built from this branch's runtime (the option lives in shadow_main.cpp); a candidate built on another branch must
carry the same hunk (merge this branch first, as the contract says the gate lands first). Sampling: `BEHAVIOUR_EVERY=10`
(every 50 ms; default). Epsilons are overridable: `--eps 'player.pos=1e-2'`, `--exact`, `--event-slack <ticks>`.

PASS means: header/row count/ticks equal; every integer, flag, counter and the scheduler counters (`sched.traversals/nodes/
executes/destroys`, the live-task-count proxy) exact; every float within its epsilon; the event sequence (field, from, to)
identical with ticks within the slack. Default epsilons (first matching glob): `*pos*` 1e-3 abs (game units; ~100-1000 ULP of a
coordinate near 100), `*vel*`/`*speed*` 1e-2, `*rot*`/`*angle*` 1e-4 rad, other floats 1e-4 abs + 1e-5 rel. The report also prints
the first row where ANY bit differs and the max deviation in absolute terms and in ULP, so drift inside the epsilon is visible.

## Game state found (PAL, GUPP8P main.dol; evidence: runtime dumps of the Westopolis tape, decomp read for structure only)

Names are this project's own. "Evidence" = how it was confirmed at runtime (dump via `--report-peek` of 24 MiB MEM1 at 60 s, then
the raw-block trace sampled every 0.5-1 s over the whole 316 s tape and analysed for which words change and how).

| state | address | evidence |
|---|---|---|
| player object (Player::PlayerShadow) | heap, found by scanning for vtable word 0x80522D70 at +0x38 (anchor, rescanned if the word no longer matches). 0x80FEEDE0 in these runs | vtable address from the project symbols (PlayerShadow); object has the name string "Shadow" at +0x3C |
| position `+0x00` f32x3, rotation `+0x0C`, vector `+0x18` (velocity-like vector; max |v| 2300 over the tape, ~0 standing; units not established), `+0x24` | CharInfo base layout (decomp header read for structure) | run: x -939..66, z -4337..260 over the tape; position and camera.pos agree within a few units; vel is near 0 when standing and large during fast movement |
| `+0xA0` flag word, `+0xA4`, `+0xA8` 96-bit set (action/ability bits), `+0xB8/+0xDC/+0xE8` vectors, `+0x1E4` 3x4 basis | PlayerBase layout; values change in steps at action changes (744 flag events, 593 bit events in the tape) | words flip between few values at the same times in repeated runs |
| stage global, 0x8057E760 (0x150 bytes) | referenced by TGameInfomation ctor 0x800C0870 in the PAL asm (lbl 0x8057E760) | `+0xA8` = 100 (stg0100), `+0xAC` = 5, `+0x94/+0x98` seconds that advance, `+0x88/+0x90` integers that advance, `+0x00` 8032..21600 |
| player-status singleton, function-local static at 0x8057707C (accessor 0x8007993C, used by TGiHdGage::update at 0x800BD4B8 with `+0x70C`) | the HUD gauge update reads two 3-word entries at `+0x70C`/`+0x718` | `gauge.a` 0,1000,3000,0,1000,2000,4000; `gauge.b` 0,500,3500,4000,7000; both drop to 0 together at 83.5 s (gauge spent) |
| six running totals at 0x805777D0..0x805777E4 | found by a scan for monotone small counters over 0x80570000..0x80590000 and 0x805E0000..0x805F2000 | e.g. 10,50,60,100,...180 then reset, and 80,99,661,1010,...4646; meaning (score-like) NOT proven |
| counters 0x80580BD8 (0,1,2,4,6,7,8), 0x80580C0C (3 words), 0x805EF1E8 (34,35,36), 0x805777B0 (3 words) | same scan | step counters; meaning not proven |
| camera: holder 0x8056FF1C > +4 = RwFrame; modelling matrix at +0x10 (right, up, at, pos at +0x40) | pointer path from existing project code comments; verified: pos within ~65 units of the player in the same sample | `camera.pos` -874,-5142,-4241 vs `player.pos` -924,-5152,-4241 |
| task activity | `GameplayScheduler::stats()` (traversals, nodes visited, executes, destroys) in every row | ~60 traversals per game second |

NOT found: a ring count and a life count as named fields (the tape may not pick up rings; the six totals and counters above are
the nearest candidates and ARE compared exactly, so a changed ring/score/life/kill outcome will fail the gate either way).
Open: name them from a replay that visibly collects rings. Stage events (goal, death) appear as changes of `player.bits`,
`player.flags`, `stage.*` and the counters in the event stream; the Westopolis tape does not reach the stage goal within 316 s
(`stage.mode` 5, `stage.limit` 21600 at the end).

## Validation

Base exe = this branch's `.build/watch/bin/Release/ShadowTheHedgehog.exe` (nudge knob off). All runs headless, `capped_run -CpuPercent 6` (job CPU 3.1 % each).

**A/A (3 runs, 316 game s, 6320 rows each, 1521 events each):** `behaviour_diff.py --spread` over the three: no field differs in any bit,
events identical, PASS twice. (The game is deterministic on the tape.) `state.json` md5 also identical (3a75268e85cd), and equal to a run
with no trace option (so the trace does not perturb the run).

**Negative control:** `GEKKO_BEHAVIOUR_NUDGE_LENGTH=1` (the game's vector Length owner 0x8040DFA0 returns its result 1 ULP up, nothing else
changed). Result against the A run: FAIL. First row where any bit differs: row 31 (tick 62977500, VI frame 44, about 1.5 s of game
time), field `player.pos`; first divergence beyond epsilon: row 31, `player.basis` (a 0.000345 rad tilt appears); event sequence diverges at
event #154 (`player.flags` 137 -> 160 at tick 1035585000 vs 1036192500). By the end 6289 of 6320 rows differ; player.pos max deviation
5752 units; counters, gauges and the scheduler's executes/nodes differ; `state.json` md5 4cb95adb04c0 (differs). Per-field table is in
`behaviour_diff.py` output (reproduce: `GEKKO_BEHAVIOUR_NUDGE_LENGTH=1 bash tools/behaviour_run.sh ...`).

Reading for Tier M authors: the tape is an open-loop replay of a real player's pad input, and a single-ULP error in one widely used
helper tilts the player within 1.5 s and the run is in a different place after a few seconds (chaotic amplification through collision and
ground contact). So the gate is strict by nature: a Tier M owner can pass only if its error does not reach player physics/collision
(render-only or camera-visual results: low risk), or is so small that no collision decision flips. Always run it; do not widen
epsilons to make a change pass. `--spread` between two candidates' own repeated runs shows whether a candidate is itself deterministic.

## Limits

* Samples are taken at the host-service grid, which can fall inside a task traversal; both arms are sampled at the same game ticks, so this is
  consistent for comparison, but a row is not a "between frames" snapshot.
* Epsilons are the stated ones above; the compare is only as complete as the layout (extend `tools/behaviour_layout.txt`, one line per field).
* Rings/lives are not individually named (see above). Only the Westopolis tape exists; Tier M authors should ask the coordinator for a second tape.
