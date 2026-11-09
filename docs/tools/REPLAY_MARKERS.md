# Replay markers: mark a bug while a recording plays, replay around it

Part of the existing input-tape replay (`--input-tape-play`); nothing else records. While a tape plays, the marker key
(default **F8**, `--marker-key <F1..F24 | letter | digit | none>`) appends one JSON line per press to the marker file
(default `<tape>.markers.jsonl` beside the tape being played, or `--marker-file <path>`). A live or recording run
marks the same way (`mode` says which); without a tape and without `--marker-file` the marker is only printed.

One line per marker:

| field | meaning |
|---|---|
| `marker` | 1, 2, 3 ... within this session |
| `session` | when this replay started (local time); one file collects every session of the recording |
| `mode` | `play`, `record` or `live` |
| `game_ticks`, `game_seconds` | the game clock at the press (40.5 MHz ticks): the exact replay timestamp |
| `render_frame`, `vi_frame` | frames rendered and video fields since the run started |
| `tape`, `tape_sample`, `tape_samples` | the tape and its playback position (the next sample) |
| `wall_seconds` | wall time since the run started |

The press is taken at the game's frame boundary (the tick the game had published when the window saw the key, as the
snapshot key does), so a marker can be late by the time it took to react, not by a host frame. A replay is
deterministic: the same tape always reaches the same tick in the same state, so `inspect` can always find a marker.

## The tool: `tools/replay_markers.py`

    python tools/replay_markers.py replay  --recording .build/recordings/final-haunt --stage stg0604 [--fullscreen]
    python tools/replay_markers.py list    .build/recordings/final-haunt/input.tape.markers.jsonl
    python tools/replay_markers.py inspect --recording .build/recordings/final-haunt --stage stg0604 --marker 2 \
        [--before 2] [--after 2] [--every 1] [--size 1280x720] [--session S]
    python tools/replay_markers.py play    --recording ... --stage ... --marker 2 [--before 3]

- `replay` plays the recording's own tape (read only) in a window with audio, on copies of its initial/ card, SRAM
  and configs (`.build/markers/replay-work`); markers go to `<recording>/input.tape.markers.jsonl`.
- `inspect` replays the tape headless to `before` seconds ahead of the marker and saves a savestate there
  (`--save-state-at-tick` + `--stop-at-tick`), then loads it and lets the tape carry on (playback catches up to the
  loaded tick and continues with the recorded inputs), capturing every `every`-th frame to `after` seconds past the
  marker, plus `contact-sheet.png` with the marker frame labelled. Output: `.build/markers/<session>-m<n>/`.
- `play` builds the same savestate and opens it in a window with audio, the tape continuing: watch the moment again.
  The savestate (`before.gekkostate`) can be reused with any diagnostics (`--load-state --state-file ...`).

Verified 2026-10-27 on the Final Haunt tape (330 s): a marker at 101.767 s - replay to 1 s before it took 21 s,
inspect captured 30 frames (every 4th) from 1 s before to 1 s after it with the tape's inputs continuing.

### Pointing at the bug

While a tape plays (`replay`, `play`), a left click on the picture is a point marker too, and so is F8 with the mouse
over the window: the line then also carries `"x"`, `"y"` - where the user pointed, as fractions of the window's client
area from the top left. `play` writes these to the recording's own marker file (`<recording>/input.tape.markers.jsonl`,
or `--markers`), so a range can be looped and the bug clicked on each time it shows. `list` prints the point;
`inspect --marker N` captures around the marker and saves `marker-point.png`, the marker's frame with the spot circled.
(The capture fills the same picture as the window when both have the window's aspect.)

### Ranges

The range key (default **F9**, `--range-key`) marks a stretch: its first press writes a marker with `"kind":
"range-start"`, the next `"range-end"`, both with `"range": n` (1, 2 ... within the session); the marker key's lines
have `"kind": "point"` (lines without `kind` are points too). Live, recording and replay runs alike.

    python tools/replay_markers.py inspect --recording ... --stage ... --range 1 [--every 1] [--size 1920x1080]
    python tools/replay_markers.py play    --recording ... --stage ... --range 1 [--loops 5] [--fullscreen]

- `inspect --range n` saves the state at the range's start (`--before` adds a lead) and captures every `every`-th
  frame to its end. Output: `.build/markers/<session>-r<n>/`.
- `play --range n` replays the range from that state to its end (`--stop-at-tick`), then again: each pass is a fresh
  run from the same state, so every pass shows the same frames. `--loops` limits the passes (0, the default: until
  the window is closed before the range's end).

## Overlap: where two surfaces fight for the same pixels

`GEKKO_DEPTH_TEST=less` (diagnostics, renderer.cpp) resolves depth ties the other way for base passes (a mesh's own
environment and dual passes keep less-or-equal: they lie on it by design). Capture the same deterministic frame with
and without it (a savestate from a snapshot or from `inspect`); the pixels that differ are exactly where two different
surfaces overlap at equal depth. The locked-spring spot in Final Haunt (the user's F5 snapshot): 0-5 such pixels per
frame - no coplanar overlap there; its flicker is two glow particle systems drawn only on some frames.
