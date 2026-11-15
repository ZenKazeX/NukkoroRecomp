# 08 Runtime baseline (unfixed game, measured)

Agent 8. All numbers below were measured on the unmodified game logic with the diagnostic trace described here.
Raw logs: `08_logs/raw/*.tsv`; per-line tables: `08_logs/table_*.md`; scripts: `08_logs/analyze.py`, `08_logs/run*.sh`.

## 1. How to enable the trace

```
set GEKKO_VOICE_TRACE=<file.tsv>        (PowerShell: $env:GEKKO_VOICE_TRACE="C:\path\x.tsv")
ShadowTheHedgehog.exe ...                 (any normal launch)
```
- With the variable unset nothing is installed: no hook, no file, behaviour unchanged. Not compiled under `GEKKO_SHIPPING`.
- Every hook only reads guest memory and chains to any earlier handler at the same address (`voice_trace.cpp observe()`); it never writes guest state and never claims the call.
- Line format: `wall_ms  game_ticks(40.5 MHz)  game_frame(0x8057E7F0)  event  key=value...`. Event list is documented in `runtime/include/gekko/voice_trace.h`.
- Report per line: `python -B docs/voice_subtitle_timing/08_logs/analyze.py <file.tsv> [--md]`.

Files changed (all mine):
- NEW `runtime/include/gekko/voice_trace.h`, NEW `runtime/src/gekko/voice_trace.cpp`
- `runtime/src/gekko/system.cpp`: `#include "gekko/voice_trace.h"` and one line `voice_trace::install(cpu_);` after `game_memory_->install();`
- `CMakeLists.txt`: `voice_trace.cpp` added to the runtime source list
- `runtime/src/gekko/system.cpp` and `runtime/include/gekko/system.h`: **debug boot fix for Devil Doom** (section 6), gated by `--boot-story-stage`, no effect on normal boots. New field `boot_stage_.story_stage_play_handed`.
- Tools only (not game code): `08_logs/run.sh` (tape replay of Westopolis), `run_cs.sh`, `run_stage.sh`, `analyze.py`.

Hooked guest functions (entry only): MessageTask update `0x801FB5AC` (polls window + all four ADXT handles each frame), enqueue `0x801FB210`, queue pop `0x801FBA18`, voice request `0x801D5540`, voice stop request `0x801D551C`, voice slot update `0x801D6E14`, `ADXT_StartAfs 0x803C3E40`, `ADXT_StartFname 0x803C3D94`, `ADXT_Stop 0x803C2F90` (with 3 callers up the stack).

Timing resolution: window open/close and ADXT status are observed once per game frame (16.7 ms); the ADXT status `3` (playing) is the "audio started" point, `5` (play end) the natural end. The wall-clock column is not game-synchronous when the host is busy; use the ticks column for intervals.

## 2. Mechanism observed (proven from the traces)

- Message window and voice are not one object. `fn_801FB210` (enqueue) queues a message; `MessageTask` (`0x801FB5AC`) opens the window (manager `0x80580B18 +0x14` = id) and asks for the voice (`voice_request`, caller `0x801FB904`, file index into `PRS_VOICE_<lang>.afs`, `ADXT_StartAfs` partition 0). The window length is the text record's frame count (+0xC, 0 means 180), counted in seconds by a float timer at task+0x28. When the timer passes it, `+0x14` is set to -1. **Nothing in the window close stops the voice** (no `adx_stop` at any `win_close` that was by timer).
- A voice ends in one of three ways, all visible in the logs:
  1. natural: slot update finds ADXT status 5 (`voice_slot_end`).
  2. cut by a new message: `fn_801FB210` accepted a new message and calls the queue clear `0x801FAEF4`, which issues `voice_stop_req` (caller `0x801FAF18`, caller2 `0x801FB2B8`) and `ADXT_Stop`.
  3. cut by the MessageTask itself starting the next voice (`voice_stop_req` caller `0x801FB8E8`): the old voice is stopped right before the new one (`0x801D551C` then `0x801D5540`). This is the "next part of the same message" case (page with its own voice id, e.g. 414 page 2).
- Other cut: queue clear called from a stage-state function (`0x801E3308 bl 0x801FAEF4`, return `0x801E330C`).
- Only two voice slots exist; `0x801D7C64` picks one. In every trace the second slot was never used (all dialogue went to slot `voice0`), so **no slot stealing was observed**.
- A message's later pages (`b = 1, 2`) usually have no voice (`voice=-1`); a page can carry its own voice (Devil Doom 414 page 2).
- A queued message holds 36 variants; non-existing variants are dropped one per frame (not logged).

## 3. Westopolis (stg0100, tape replay), English voice, measured

Run `en_full` (tape `westopolis-cpu-spike`, windowed, audio on, 187 s of game time before the host ended the run; the stage restarted once at wall 86 s, so lines repeat). Table: `08_logs/table_en_full.md`. Voice durations from agent 05 (`05_voice_durations.csv`).

| voice (EN file) | msg | window | voice played | end | notes |
|---|---|---|---|---|---|
| 3585 cty01_e04 (6.38 s) | 755/1 | 395 fr = 6.58 s | 6.43 s | natural | window closes 0.07 s after voice end |
| 285 act04_g34 (1.48 s) | 1880/0 | 47 fr = 0.78 s | 1.57 s | natural | **window closes 0.87 s before the voice ends**; voice not cut |
| 901 all02_h13gc (6.02 s) | 31/1 (423 fr = 7.05 s) | 0.72 s then 5.25 s on second visit | 0.63 s / 5.17 s | **CUT** both times | enqueue of msg 520-2 (accepted, `0x801FB2B8`) replaced it; window and voice end together; 5.4 s / 0.85 s of voice unplayed |
| 3636 cty01_n00 (8.63 s) | 520/2, 3 pages | 561 fr = 9.38 s | 8.73 s | natural | window closes 0.57 s after the voice |
| 591 all01_n00 (2.75 s) | 711/2 | 229 fr = 3.40 s | 2.85 s | natural | window outlasts voice |
| 3610 cty01_e16 (3.19 s) | 767/2 | 229 fr = 3.82 s | 3.27 s | natural | |
| 326 act04_g75 (1.60 s) | 1950/0 | 69 fr = 1.15 s | 1.27 s | **CUT** (first run only) | enqueue of 31-1 at +1.27 s; 0.33 s unplayed |
| 335 act04_g84 (2.72 s) | 1951/0 | 118 fr = 1.62 s | 1.53 s | **CUT** | enqueue of 767-2 at +1.53 s; about 1.2 s unplayed |
| 4432, 13, 3 (short effort/character voices) | none | no window | 0.8-1.05 s | natural; voice 13 re-triggered and cut by the next 0x802DC1D0 request | gameplay barks, no subtitle |

Findings:
- In all cases where a voice was cut, the cause was **a newer message being enqueued** (`0x801FB2B8`), not the window timer. Window and voice end in the same frame there.
- Where the window is shorter than the voice (285: window 0.78 s vs voice 1.48 s), the voice still plays out because nothing links them. A visual mismatch, not an audio cut.
- Whether cuts by newer messages are intended is not decided here. The code path is the game's own replacement rule (priority compare in `fn_801FB210`, agents 01-04).
- One log pair (EN vs JP, same tape, same frames) shows identical enqueue frames in both languages: the message timing is language independent; only the voice length differs.

## 4. Japanese baseline (voice=0, text English), Westopolis tape, 126 s of game time

Table `08_logs/table_ja_full.md`. Japanese voices are served from `EXTRACTED_DISKS/Japan/files`, selected by `options.cfg voice=0` (the run dir's own copy; the user's settings were not touched).
- Same enqueue frames as English. JP voices are mostly the same length or longer than the window: 3585 6.68 s vs window 6.58 s (voice ends 0.18 s after), 285 0.80 s vs 0.78 s (+0.10 s), 3636 9.42 s vs 9.38 s.
- JP 591 (3.84 s file) was **cut** at 3.32 s by the enqueue of 767-2 (about 0.5 s of voice unplayed, the English one of 2.75 s ended naturally 0.5 s earlier). JP 901 cut as in English.
- So the same fixed window (229 frames) that fits the English voice cuts the longer Japanese one in this pair. This is the only Japanese-only difference observed.

## 5. Cutscenes

Reached: none played a voice that could be measured.
- `--boot-cutscene event1101 / event2001 / event3001` (script cutscenes): the hand-off line prints, but 456 frames later the story continues in Westopolis; no event frame was shown ("in-engine event frames shown 4:3 0") and no cutscene voice or `sng_EV*` stream started. Raw logs `cs1101_en`, `cs2001_en`, `cs3001_en`. Not investigated further: the cutscenes did not play on this route (cause unknown).
- `--boot-cutscene event1001` (movie `E1001.sfd`): ran 58 s of game time; movie played (frames 49..1744). The subtitle window opens for messages 3000..3003 (`msg_enqueue` caller `0x802D34C4`, the movie subtitle producer) with `voice=-1`, durations 96-252 frames each. The movie's own audio does not go through the traced ADXT/voice path, so voice/window relation is **not measured** for movies. Run exit code 5 (not diagnosed; log `cs1001_en`).
- Therefore in-engine cutscenes (`sng_EV*_E.adx` through the bgm slots) and movie audio are untested.

## 6. Devil Doom (stg0710), English voice

### Boot route
- `--boot-stage stg0710` refuses: "not a Select Mode stage".
- `--boot-story-stage stg0710` loaded the stage and then fell through to Westopolis (as `docs/STAGE_LOADING.md` section "Correction" says): proven by `sng_stg0100.adx` and message 755/1 in `dd_try2`.
- Fix (developer navigation only, `system.cpp` hook at `0x8025D810`): after the first query (stage load step) is answered with stg0710, the second query is answered with the same stage and the story sequence's current-stage word (caller `r31 + 0xC`) set to it, so `0x802D21F8` takes the PlayAction branch `0x802D22B8`. Log line: `[boot-stage] stg0710 handed again with the current-stage word set: play`.
- Proof it is Devil Doom: music `sng_stg0710.adx` at game frame 49; message ids 420, 414, 415, 426 with voices 3260, 3283, 3284, 3285 (files `bos17_*`, the boss 17 voices). **Not proven by the log:** the StageManager(710) construction and the `DevilDoomCtrlUnit` class are not logged by this trace (inferred from the music and the boss voices/messages).
- Command (cwd `wt-gc-removal`): `.build\watch\bin\Release\ShadowTheHedgehog.exe --game-data H:/ShadowTheHedgehogDiskGC_Extract/game --boot-story-stage stg0710 --audio` plus display/pad/card arguments. Gameplay starts about 2 s of game time after the hand-off (about 7 s wall).

### What was exercised
One run (`dd_try3`), no pad input, 60 s budget. The trace covers game frames 49-1358 (about 22 s of fight); then the trace ends: at frame 1358 the game cleared the message queue from a stage-state function and nothing else was queued during the remaining time. The fight was NOT played: no damage was dealt, only the opening and idle boss lines happened. The boss HP-triggered lines (BELOW_HALF), death lines etc. are untested. Japanese Devil Doom: not run (the exe was handed to the user).

### Per line (English), table `08_logs/table_dd_try3.md`; durations from `05_voice_durations.csv`

| voice (file, full length) | msg (window) | played / window | outcome and cause |
|---|---|---|---|
| 3260 bos17_e00 (10.65 s) | 420/0, 484 fr = 8.07 s | voice 8.90 s | **window closes by timer at 8.07 s, 0.92 s before the cut; voice still playing.** Cut at +8.9 s by the enqueue of 414-0 (accepted, `0x801FB2B8`, stop via `0x801FAF18`). About 1.75 s of voice never played. Equivalent to "window 2.6 s shorter than the voice" (also 0.9 s with the window already gone) |
| 3283 bos17_h00 (9.45 s) | 414/0 page 0 300 fr = 5.0 s, page 1 273 fr = 4.55 s (no voice) | voice 9.48 s | stat 4 (decode end) reached; stopped by the MessageTask starting the next part's voice 3284 (`0x801FB8E8`) at the same frame page 1 closed. Heard to the end (the clipped tail is only the buffered remainder, at most about 0.18 s as in the natural ends) |
| 3284 bos17_h01 (1.80 s) | 414/2 (page 2), 81 fr = 1.35 s | 1.35 s | **CUT 0.45 s early** by the enqueue of 415-0 (accepted). Window had closed 0.08 s earlier (timer) |
| 3285 bos17_h02 (5.81 s) | 415/0, 300 fr = 5.0 s | 1.65 s | **CUT 4.2 s early**, window destroyed in the same frame: queue clear (`0x801FAEF4`) called from `0x801E3308` (a stage-state function; the trigger is not identified - possibly the idle player's death or a boss state change) |
| 426 (enqueued at frame 1137 by `0x80310938`) | none | - | **never shown, no voice**: no pop, no window, no stop, no voice request. Dropped/rejected by `fn_801FB210` (priority or lock test; which one is not logged) while 414 was current |

Category check (as requested):
- cut by the next queued part of the same sequence (`0x801FB8E8`): 3283 (once).
- cut by a new enqueue of equal or higher priority (`0x801FB2B8` via `0x801FAF18`): 3260, 3284.
- queue clear from a stage-state function: 3285.
- ADX slot stealing: none (only slot `voice0` was used; the second slot never started).
- dropped / rejected request: enqueue 426 (no window, no voice).
- window with no voice: pages b=1 of 414 (273 fr), expected (text-only page); voice with no window: none in Devil Doom (Westopolis has several gameplay barks 4432/13/3 without windows by design).
- The user's reported "many mismatches" are consistent with: window shorter than voice (3260: 8.07 s window vs 10.65 s voice) and replacement cuts (3284, 3260). Whether replacement is original intended behaviour is not decided here.

## 7. Untested / limitations
- Devil Doom beyond the first 22 s idle; Japanese Devil Doom; in-engine cutscenes; movie audio; the second voice slot; rapid replacement under real combat.
- Only English voice slot 0 was exercised. The Westopolis tape was recorded on an older build and the stage restarted once; it is a measurement of this exe's replay, not a verified identical replay.
- Window lifetime is quantised to frames; "voice played" includes the ADXT prepare latency (stat 3 is reached about 4 frames after the request, the window opens about 1 frame after the request).
- The runs ended early by host request at unexplained times (126 s and 187 s of game time of the 256 s tape; budgets 400 s / 262 s).
- No fix was implemented. The trace hooks are intended to be reused by the fix and validation agents.
