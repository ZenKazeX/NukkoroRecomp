# The gameplay recorder (input tape version 3, 2026-11-05)

Record a run once and replay it at **any** resolution, aspect, render scale, MSAA, filtering and window mode. The
replay checks itself against the recording as it goes.

## Why a replay can change the picture

Only one display value reaches the game's simulation: the picture's **aspect**, through two gameplay tests
(docs/RESOLUTION_AUDIT.md section 13):
- the enemy AI's "is the nearest character on screen?" test (`displayed_screen_point`, EnemyBaseAI vf5 0x8019DA1C,
  12 enemy classes);
- ElecCoreWall's animation start (its `RwCameraFrustumTestSphere` call returning to 0x80264CF8).

Everything else the aspect touches is read only by drawing and the HUD: the frustum planes, the sector array, the
enemy gauge, the CSD helpers, the lens flare. The render size, render scale, MSAA, filtering and window size never
reach game state.

So those two tests take a **logic aspect** of their own:
- live play: the picture's (`gameplay_aspect = follow`, the default - unchanged);
- the console's 4:3 (`original`), or any ratio;
- a replay: the recorded one.

The picture is free.

## Recording

- `--record <dir>`: makes `<dir>/input.tape`.
  - `<dir>/initial/` gets pristine copies of the card, SRAM, display.cfg and pad.cfg, taken before the first frame.
  - `<dir>/manifest.json` records the date, exe, command line, start route, card hash and display settings.
- `--input-tape-record <file>` (as before) writes only the tape.
- `--tape-check-seconds <s>`: the checkpoint interval (default 1; 0 = none).

## Tape format (text; versions 1 and 2 still read)

```
shadow-input-tape 3
start <tick>
display <w>x<h>                    the configured window size (informational)
meta <key> <value>                 recorder, date, exe, command_line, boot_stage, video, card, sram, display,
                                   draw_distance, gameplay_aspect, framing, native_visibility, check_interval_s
output <tick> <w>x<h>              the picture (informational in version 3; --replay-as-recorded shows it)
logic <tick> <aspect>              the aspect the game logic used from this tick on (binary32, %.9g: exact)
setting <tick> <key> <value>       draw_distance changes during the run (replayed: they move the spawn range)
check <tick> stage=.. rng=.. player=.. mission=.. camera=..   a checkpoint (gekko/replay_check.h)
sample <tick> <port> ...           the latched controller samples, on the 5 ms host-service grid
stop / saved <tick>                optional
end
```

A version 2 tape's `output` w/h is its logic aspect. That is exactly what the recording's logic used, so old tapes
(the Lava Shelter one) replay at any resolution too. A version 1 tape has none.

## Replaying

- `--replay <dir>`:
  - copies `<dir>/initial/`'s card and SRAM into `<dir>/replays/<date-time>/`; the recording's are never written;
  - plays `<dir>/input.tape` (or the older `run/input.tape`);
  - takes the start route from the tape's meta.
- `--input-tape-play <file>` as before.
- The picture comes from the replay's own display.cfg / CLI.
  - `--replay-as-recorded`: the recording's picture and display.cfg instead.
  - `--gameplay-aspect follow | original | W:H | recorded`: overrides the logic aspect (recorded is the default; any
    other value is no longer a faithful replay).
- **Checkpoints:** every recorded `check` is recomputed at its tick. The run prints `[replay] checkpoints: N of N
  compared matched`, or the first divergence with its game time and the groups that differ. `--replay-strict` stops
  there.
- **Checkpoint groups** (`gekko/replay_check.h`, addresses from the comparator's `probes.py`):
  - stage: number, state, frame counter, timer, pause;
  - rng: the MT19937 state;
  - player: the P1 record, plus position / rotation / velocity;
  - mission: manager, gauges, checkpoint, enemies defeated;
  - camera: the frame's aim, not its field of view.

## One-command report

`python -B tools/replay_report.py <dir> --resolution 2560x1440 --mode windowed --render 2560x1440 --msaa 8
--anisotropy 16 [--boot-stage stg0602]`
- Replays with the present / GPU / GPU-memory traces and the NVML clock sampler.
- Writes `<dir>/reports/<date-time>/report.md` + `report.json`:
  - fps average, 1-s min / max, 1 % low;
  - frame times and repeated refreshes;
  - latch-to-vblank latency;
  - GPU times, VRAM and clocks;
  - the replay's checkpoint result.
- `--report-only --out <folder>` rebuilds the report from an earlier run without running the game.
- Frame times come from DXGI's present counts, not from the time between two statistics reads. A read can skip
  presents (two or three shown between reads). A gap of n presents over r refreshes is n frames: n - 1 of one
  refresh, and the r - n extra refreshes put on one frame (the worst case for the lows).

## Verified (2026-11-05)

`.build/perf/recorder-verify*/`.

- **Westopolis, 40 s.** A version 3 tape was recorded headless at 1920x1080 by replaying the old tape. Replayed at
  3840x1200 (32:10) and 1280x960 (4:3): 39 / 39 checkpoints matched, 0 missed, and the audio dump was byte-identical.
  - The end state differs only in 8-11 of 510 guest memory blocks: the frustum / sector / HUD / flare data the
    picture's aspect writes (presentation-only).
  - This route ran the enemy on-screen test only 4 times. The logic-follows-picture control matched too (0 blocks
    differ from the split replay), so the split had nothing to change here.
- **Lava Shelter, 178 s.** A version 3 tape was recorded at 3840x1200 by replaying the user's version 2 tape, then
  replayed at 1920x1080.
  - Stage, RNG, player and mission matched at all 177 checkpoints, and the audio was byte-identical.
  - The camera group differed at 4 checkpoints from 46.0 s: an aspect-dependent camera path not yet found
    (docs/RESOLUTION_AUDIT.md section 13).
  - The logic-follows-picture control gave the same result.

## Limits

- Checkpoints hash gameplay state only, not every object: a divergence in an enemy that touches none of the groups is
  found only once it reaches them (the player, the RNG, the mission counters).
- The meta records the exe by path and size, not a content hash.
- The start route is taken from the meta for `--boot-stage` only (events, story and load-state replays still need
  their flags).
