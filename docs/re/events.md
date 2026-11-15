# Events, cutscenes, movies and the Task framework (S2 field `events`)

Field agent `events` of session S2 (2026-11-12), branch `s2/events`, base 700f72d (+ session contract 3aef29e).
Scope: in-game events (TPjsEventAction*), the event script and scene data, movies (TMovieManager, StageMovie, CRI Sofdec
as the game drives them), the Task tree and the `GameplayScheduler` that walks it, and the CharCommand dispatch base.
Evidence labels: **P** proven (disassembly of the pinned PAL `main.dol` plus constants read from the image, or a byte-level
check against the real files in `game/`), **I** inferred (consistent with the code, not exercised), **U** unknown.
The external decomp was not used for any name, layout or code in this file; every name below is ours and rests on the
evidence named next to it. Addresses are PAL (`GUPP8P`) main.dol; `r13 = 0x805EC500`, `r2 = 0x805FA780`
(set at 0x8000332C..0x80003338, so `lwz rX, off(r13)` is the absolute address `0x805EC500 + off`).

Reproduce: `tools/re/events_dol.py` (strings, code references, words), `tools/re/events_scan.py` (parses every file of
`game/files/event` and the `.sfd` files), `tools/re/events_task_census.py` (Task subclass census),
`tools/re/events_layout.txt` + `tools/re/events_run.sh` (behaviour-trace layout and Westopolis runner for the runtime
checks in the last section).

## 1. Address ranges of the field

From the S2 ownership table (`s2_ownership.tsv`, column `field = events`): 986 functions, 119.7 KB.

| subsystem (table) | functions | bytes | ranges |
|---|---:|---:|---|
| game framework (Task, Fade, CharCommand, DebugDisp, Vibration, Heap, ...) | 555 | 51,540 | 0x80039948-0x80039EFC, 0x800441D4-0x80045FC8, 0x8004EA80-0x8004F0FC (Task base), 0x8006E47C-0x8006E5E0 (CharCommand base), 0x801DA048-0x801DCE5C (command manager + command ctors), 0x802E14B8-0x802E2F84 (DebugDisp), 0x8034AF38-0x8034FFD8 (Vibration), 0x8040BC18-0x8040F360 (Heap) and small stubs |
| in-game events | 197 | 31,608 | 0x8016FA60-0x801748E8 (Scene, Manager, Folder), 0x80329740-0x8032AC4C (Fade / Scene sub-objects), 0x80228AC0-0x80228D5C, 0x8025E154-0x8025E2C8 (StageTriggerEventTask), 0x80047870-0x80048EB8, 0x800C9A8C-0x800CB8F0 and small pieces |
| video / movies (own code) | 3 | 1,760 | 0x800367A8-0x80036E88 (TMovieManager update / dtor) |
| CRI Sofdec (movies) | 231 | 36,016 | 0x803D09CC-0x803D0E68, 0x803EFF90-0x803F008C, 0x803FA168-0x803FFAAC, 0x80403A20-0x804049F0, 0x80407684-0x80409488 |

The table is a mechanical assignment: roughly a third of the entries are stubs or helpers whose real owner is another
field (for instance `Vibration` and `DebugDisp` are framework code only because they derive from Task). Not in the
table but part of the subject (marked `-` there): `TMovieManager__construct` 0x80036E88, `StageTriggerEventTask` factory
0x8025DFB4, `StageMovie` 0x802D2DB0-0x802D33A4, `StageAction::EventAction` 0x80204E34/0x80204EEC. They are mapped here
and not claimed (no code is written for them).

## 2. The Task framework (P)

### 2.1 Node layout (Task base constructor 0x8004F014, destructor 0x8004EF18, both read instruction by instruction)

| offset | size | meaning | evidence |
|---|---|---|---|
| +0x00 | u32 | pointer to the class-name C string (debug / profiling tag). Base value `[0x805E4A38] -> "Task"`; every subclass constructor overwrites it from its own r13 slot: `TPjsEventActionManager` `[0x805E7900]`, `TPjsEventActionScene` `[0x805E78D0]`, `CharCommandManagerTask` `[0x805E9C18]` (strings read from the image) | ctor 0x8004F07C-0x8004F080; the three strings above |
| +0x04 | u16 | flags: 0x0001 removal requested, 0x000E subtree disabled, 0x0020 destructor ran, 0x0100 runs while paused | dtor sets 0x20 (0x8004EF44); traversal 0x8004ECDC-0x8004ED00 |
| +0x08 | ptr | previous sibling (the first child's slot holds the LAST child, so append is O(1)) | ctor 0x8004F094-0x8004F0C4 |
| +0x0C | ptr | next sibling (0 at the end) | same |
| +0x10 | ptr | parent | ctor 0x8004F074 |
| +0x14 | ptr | first child | ctor 0x8004F078 |
| +0x18 | ptr | vtable: `[0] type word`, `[1] 0`, `[2]` destructor (+0x08), `[3]` update (+0x0C, called with f1 = dt) | vtable 0x8051E768; the other three vtables in section 3 |
| +0x20/+0x24 | u32 x2 | zero pair written by 0x8004F0F0 (profiling accumulator, inferred) | ctor |

A node is `0x28` bytes for the pure layer nodes (allocated `li r3,0x28`) and larger for subclasses (Manager 0x90, Scene
0x1C60, TMovieManager 0x138, StageTriggerEventTask 0x30). Constructor and destructor bracket the list surgery with
0x80050C40 / 0x80050BF8 (a lock pair, inferred) and maintain the live-node counter `[0x805EF278]` (`r13+0x2D78`, +1 in
the ctor, -1 in the dtor). The destructor, when `flags` is set to 0x20, unlinks the node from its parent's child list
(three cases: only child, first child, middle / last) and frees with 0x803A1334 when called with a positive delete flag.

### 2.2 Globals of the traversal (P, all in `.sbss`, only the traversal reads them)

| address | r13 offset | meaning |
|---|---|---|
| 0x805EF270 | +0x2D70 | recursion depth (inc on entry, dec on exit of 0x8004ECAC) |
| 0x805EF274 | +0x2D74 | profiling switch: non-zero routes each update through the wrapper 0x8004EDF0 |
| 0x805EF278 | +0x2D78 | live node count |
| 0x805EF27C | +0x2D7C | pause byte (written by 0x801EB668 = 0 and 0x801EB674 = 1 only) |
| 0x805EF0D0 | +0x2BD0 | pointer to the task manager singleton (accessor 0x8001C4F8, object 0x80571C6C) |

### 2.3 The frame and the traversal (P)

```
0x80049078  bl 0x801E2210      (the only caller; "main loop body", 624 B)
0x801E2210  per-frame driver: 0x8032D1D0, input/pad refresh, timers, then
  0x801E2270  bl 0x8001C4F8 (manager)  f1 = dt = [0x805F0A58 (r13+0x4558)]
  0x801E2278  bl 0x801EB680            "pump the root": r3 = manager; r3 = [r3+0]; if non-null bl 0x8004ECAC
0x8004ECAC  traversal (recursive; 83 instructions)
  per child: flags&0xE (disabled subtree) -> skip;  flags&1 (removal requested) -> if pause byte 0 { clear bit, if !(flags&0x20)
  walk the grandchildren away (0x8004EECC mark + 0x8004ECAC), call the destructor (vtable+8, arg 1) }
  else (runnable): if pause byte 0 or flags&0x100: update = vtable+0x0C (or the profiling wrapper 0x8004EDF0 when
  [0x805EF274] != 0), then recurse into the child
```

Other entries into 0x8004ECAC: 0x801EB624 (scene reset: marks the Scene layer, walks it, rebuilds the 13 stage layers
with 0x801EB4A4), 0x801E4FA0 / 0x801E501C (two manager-driven subtree pumps, no direct callers: reached through a
function-pointer table), and 0x801EB698. `docs` already covers the native replacement: `runtime/src/gekko/gameplay_scheduler.cpp`
(`shadow::gameplay::Walk`) implements exactly this algorithm and calls the guest `execute` / destructor through the
dispatcher; the node memory stays in guest MEM1.

### 2.4 The layer tree (P: names read from the image)

The manager constructor 0x801EB734 creates the root (`0x28` bytes, no parent) and then 0x801EB53C and 0x801EB4A4:

* 0x801EB53C: six base layers as children of the root, slot ids `0..5` from the table at 0x8053ED6C, flag 0x100 (run while
  paused), name tag pointer `table[0x8053ED20 + 4*slot]`.
* 0x801EB4A4: thirteen stage layers as children of slot 3 (`manager+0x10`), slot ids from the table at 0x8053ED84,
  appended in that order, flag 0x100.

Name strings (the tag words at 0x8053ED20, strings read from the image):

| slot | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | 16 | 17 | 18 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| name | System | Audio | Debug | Scene | PostSystem | Render | Manager | Controller | Command | CharaColli | Landscape | Gadget | Vehicle | Player | Enemy | PostManager | Particle | Editor | Camera |

The tree therefore updates in this order each frame: `System, Audio, Debug, Scene{Manager, Controller, Command,
CharaColli, Landscape, Gadget, Vehicle, Enemy, Player, PostManager, Particle, Editor, Camera}, PostSystem, Render`
(Enemy before Player: the table order is 6,7,8,9,10,11,12,14,13,15..18). `0x801EB5D4` marks slots 11, 12 and 14
(Gadget, Vehicle, Enemy) for removal; `0x801EB624` marks and rebuilds the whole Scene layer. `StageTriggerEventTask`
is created under `0x801EB614(manager, 3)` = the Scene layer node itself.

### 2.5 Census of Task subclasses (P)

Every subclass constructor starts with `bl 0x8004F014`. 230 call sites in 227 functions (`docs/re/events_task_census.tsv`,
generated by `tools/re/events_task_census.py`; the scheduler header's "159 classes" counts classes, the census also
holds secondary constructors, per-variant constructors and a few non-constructor wrappers such as `Player__PlayerShadow__vf10`).
Of the 227, the events field owns: Fade (`FadeDrawTask` 0x800441D4, `FadeTask` 0x80044228), `TPjsEventActionScene`
0x80171E4C, `TPjsEventActionManager` 0x80173EF4, `TPjsEventFolder` 0x801748A4, `TPjsEventActionFade` 0x8032A10C,
`CharCommandManagerTask` 0x801DA140, `DebugDisp` 0x802E1A38, `Vibration::ManagerTask` 0x8034C880. The rest belong to the
other fields (the census gives the field column).

## 3. In-game events (TPjsEventAction*)

### 3.1 Classes (P unless marked)

| our name | ctor / dtor / update | vtable | size | name evidence |
|---|---|---|---|---|
| `TPjsEventFolder` (singleton, static at 0x805812C8, accessor 0x801727C4) | 0x801748A4 / 0x8017484C | - | 12 | typeinfo-like record at 0x805E7920 -> string "TPjsEventFolder" |
| `TPjsEventActionManager` (Task) | 0x80173EF4 / 0x80173DC0 / 0x80173418 | 0x8052CB9C | 0x90 | string "TPjsEventActionManager" (0x804C4DE0); name tag `[0x805E7900]` |
| `TPjsEventActionScene` (Task) | 0x80171E4C / 0x80171D74 / 0x80171544 | 0x8052CB44 | 0x1C60 | string "TPjsEventActionScene" (0x804C47F8); name tag `[0x805E78D0]` |
| `TPjsEventActionFade` (Task) | 0x8032A10C | - | ? | string "TPjsEventActionFade" (0x8050EB9C) |
| `StageTriggerEventTask` (factory + Task) | 0x8025DFB4 (factory) / 0x8025E270 / 0x8025E154 | 0x8054C794 | 0x30 | string "StageTriggerEventTask" (0x804F6250) |
| `StageAction::EventAction` (not a Task: stage-flow action, 8 B) | 0x80204EEC / 0x80204E34 | - | 8 | boost sp_counted_base template names for `StageAction::EventAction` in the image |

`TPjsEventActionData` also appears as a string (0x804C4810 / 0x804C4DF8) next to the Scene and Manager names; its
role is **U** (the strings sit in the type records of the two classes, probably the shared base).

### 3.2 `TPjsEventFolder` and the start call (P)

`0x801746F0(folder, eventId)`: if `folder+4 != 0` it allocates `0x90` bytes and constructs the Manager
(`0x80173EF4(mem, parentLayer = folder[1], eventId)`; the result goes to `folder+0`), then maps the id through
`0x8025D674`, asks the game-info singleton (0x800BBB34 + 0x1754, helper 0x80174808) and sets `folder+8` to 0 / 1 (event
already seen?), then marks it (0x8017482C). Callers of 0x801746F0: `StageTriggerEventTask` factory (0x8025E03C),
`StageAction` code at 0x802057C4 and four sites in 0x8034xxxx / 0x8035xxxx (U: front-end paths). The folder's `+0` is
also the "an event is active" test used everywhere (`bl 0x801727C4; lwz r0,0(r3)`).

### 3.3 `StageTriggerEventTask` factory 0x8025DFB4(taskSlot, kind) (P)

1. `0x8025D810(queue, &eventId, kind, pop = 1)` takes the next story event id from the story queue (this is what the
   `GAME_SPINE` notes call "pop the story queue"); on failure the factory returns 0.
2. Formats `"event/event%04d_sceneA.one"` (string 0x804F6268) with the id and asks the file system object 0x80037200
   whether it exists (vtable +4). If it does, `0x801746F0(folder, id)` starts the event.
3. Unless `kind == 1` it allocates the 0x30-byte Task under the Scene layer (`0x801EB614(manager, 3)`), vtable
   0x8054C794, `+0x28` = "skip" byte (set when the byte at the factory's first argument is non-zero), `+0x29 = 0`.

`StageTriggerEventTask::update` 0x8025E154: if no event is active (`folder[0] == 0`): when `+0x28` is set it writes a
fixed 2 into `0x80074598()+0x10` and runs 0x801762D0(.., 0x10), otherwise it waits for the stage-flow gate
(`[0x8051DFA8]` object: `+0x40 == 0` and byte `+0x978 == 0`, the same "settled" gate the event Manager uses) and
runs 0x801762D0(.., 3) and clears byte `0x80580000-0x18A0+0xC0`; in both cases the task removes itself (flag bit 0). If an event is active and the Manager is in
state 5, it requests the fade once (`0x80173C18(1, 1.0)`).

### 3.4 `TPjsEventActionManager` (P)

Constructor 0x80173EF4(this, parent, eventId): Task ctor; vtable 0x8052CB9C; `+0x34 = 0` (skip request), `+0x44 = 0`,
`+0x50 = 0`; stores the event id to the global `[0x805F0348]` (`r13+0x3E48`), initialises the folder/sound/camera
helpers (0x800C9F6C, 0x800CB8C0, 0x801732CC, 0x80173CEC, 0x80171170, 0x80174500, 0x8016FD74, 0x80329AEC, 0x80329C98),
sets the global pace flag `[0x805F035D] = 1` (r13+0x3E5D: 30-unit timeline) and `[0x805F0364]` (r13+0x3E64) = whether
the id has bit-parity with 0x3B6 (`((id ^ 0x3B6) >> 1) - ((id ^ 0x3B6) & 0x3B6)`, top bit), calls the audio manager's
event-open (0x801D4FD8(.., -0x14)) and for the ids 0x1B59, 0x1B63, 0x1C21, 0x1C2B, 0x1E83 (compare chain at
0x80173FF4-0x80174030, read) calls 0x80015344(1). The script holder singleton (0x80171170, see 3.7) receives the
script request in 0x80174500 (called from this constructor).

Layout (offsets from the object, derived from the constructor, update and `sub4`):

| offset | type | meaning |
|---|---|---|
| +0x10 | char[0x18] | script path `"event/event%04d.scr"` (string 0x804C4E80, `sprintf` at 0x80174528) |
| +0x28 | s32 | state machine 0..8, section 3.5 |
| +0x2C | s32 | index (0/1) of the current scene slot in the pair at `[0x805F0354..0x805F0358]` (r13+0x3E54) |
| +0x30 | f32 | event time accumulated across scenes |
| +0x34 | u8 | skip requested |
| +0x38 | u32 | saved value of the global at 0x80580000-0x18A0+0x10 (restored by the dtor) |
| +0x40 | s32 | hold counter (set to 2 when the event ends, counts down) |
| +0x44 | s32 | count of "scene almost over" observations |
| +0x48 | s32 | wait counter for the audio manager |
| +0x4C | s32 | (read only) |
| +0x50 | u8 | event over (starts at 0) |
| +0x54.. | s32[12] | scene letters present, filled by 0x80173CEC (A=0 .. L=11), count at +0x84, cursor at +0x88 |

`0x80173CEC` (P): for letters `'A'..'L'` it formats `"event\event%04d_scene%c.one"` (0x804C4E54) and asks the file system
object whether the file exists (vtable +4 of 0x80037200); each hit appends its index to the list at `+0x54` and increments
`+0x84`; if none exists the state is set to 8 (end). This is why `event1101_sceneA/B` and `event6001_sceneA..F` etc. play as
chains and why events 1001/4101/... that only have a `.scr` (their video is an `.sfd`) do not.

The Manager keeps at most TWO scene objects alive (`r13+0x3E54` ring of two pointers, `0x80173364` = index of the other
slot), so scene N+1 is loaded and built while N plays.

### 3.5 Manager state machine (P: jump table 0x80530000-0x33E0 = 0x8052CC20, handlers read)

| state | handler | action |
|---|---|---|
| 0 | 0x801735E8 | take the next scene letter (`+0x88++`), create the Scene object (`0x80173380`, 0x1C60 bytes, `0x80171E4C(mem, folder_ptr, letterIndex)`); on success go to 1 with hold 2, else 8 |
| 1 | 0x8017363C | wait until the scene reaches state 7 (running), then decrement the hold; when 0, reset the event time `[0x805F0350]` to 0 and go to 2 |
| 2 | 0x801736C0 | when the stage-flow object (`[0x8051DFA8]`) is settled (`+0x40 == 0 && byte +0x978 == 0`, same gate as the trigger task), start the scene: add the scene's times, `[0x805F034C] = scene+0x28`, state 3, and start the event music stream through the sound manager (0x801D55EC / 0x801D5494 with a file name): for the ids 950, 951, 970, 972, 974, 976, 978 (jump table 0x8052CBAC; these are the ids whose `.scr` has the `+0x40 = 2` record) the fixed streams `sng_Battle1.adx` (when `+0x4C == 0`) or `sng_Battle2.adx`; for every other id `sng_EV%04d_J.adx` or `sng_EV%04d_E.adx` formatted with the id into the buffer at 0x80581300 (language byte `[0x80580000-0x18A0+0x5C]` == 0 selects `_J`; strings read at 0x804C4E0C..0x804C4E40); event 0x200B uses 0x801D5494 (start without the loop flag). The 70 `sng_EV*_E.adx` files in `game/files` are these streams |
| 3 | 0x80173828 | poll the sound manager (0x801D7EAC; results 1 and 2 count as "not ready yet", at most 100 polls, I: the music stream opening), then state 4, start the in-fade (`0x80173C18(1, ..)`, section 3.9), release the scene (`0x801716D8(scene,1)`) |
| 4 | 0x80173898 | queue the next scene letter's load (`0x80173380` with slot -1) and go to 5 |
| 5 | 0x801738D0 | the running state: ask the scene whether it is over (`0x80171508`: state 7, not held, byte `+0x18C` set); when over: mark the current scene for removal, switch to the other slot, start it (`0x801716D8`), state 4 (if it is already running) or 7 (otherwise); also detects "scene close to its end" (time window 50 units, `0x80173A8C`) and, after 3 observations, sets the event-over byte and, for a fixed list of ids (0x1901, 0x1389, 0x10D7, 0x10CD, 0x14B5, 0x151B..0x17D5, 0x1839, 0x1B63, 0x1EE7, 0x1F46, 0x1FA5..0x1FAA, 0x200A - read at 0x80173AC8-0x80173B98), sets the stage-flow flag `0x80580000-0x18A0+0x124` |
| 6 | (no-op) | |
| 7 | 0x80173BB8 | wait for the other scene to reach state 7, then raise skip, state 4 and start the out-fade (0x80173C5C) |
| 8 | 0x80173BF8 | set `flags |= 1` (remove the Manager) |

Before the switch the update handles skip: with `+0x34` set and state 5 it asks `0x80174618` whether the event may be
skipped (id table), starts the out-fade (0x80173C5C), and ends. The skip request is also raised from `0x80173CA8` (the pad 0xD1 flag).

### 3.6 `TPjsEventActionScene` (P for what is listed)

Constructor 0x80171E4C(this, parent, letterIndex): Task ctor, vtable 0x8052CB44, `+0x28 = letterIndex`,
`+0x2C..+0x38 = 0.0`, `+0x3C = -1.0`, `+0x60 = 1` (state), `+0x64 = 0` (`One*`), `+0x68 = 1` (hold), `+0x69 = 0`,
`+0x6C/+0x70/+0x74 = 0` (archive size, retries), sub-objects at `+0x78` (0x8032AB6C: the fade/overlay helper, class in
the Fade range 0x80329740-0x8032AC4C), `+0x180` (0x80172364: timeline "tour" with a pointer to a record whose float at
+8 is the length; accessor 0x80171F1C returns it or 0.0), `+0x1E0` (0x80228D34: camera/node controller in the
0x80228AC0 range), `+0x234` (0x80173130), `+0x1040` and `+0x164C` (0x80172900: two intrusive lists whose heads point
at themselves, `+0x103C` and `+0x1648`), `+0x1C58 = 0`. The path buffer for the archive is `+0x40` (0x28 B).

State machine (jump table 0x8052CB54, `TPjsEventActionScene__update__sub11` 0x801713DC):

| state | handler | meaning |
|---|---|---|
| 1 | 0x80171414 -> 0x80171BA0 | prepare: only when the global serialiser byte `[0x805F035C]` (r13+0x3E5C) is 0. Builds `event\event%04d_scene%c.one` into the path buffer, checks the file exists, sizes it with 0x80044E5C (aligned to 0x20 by 0x8004E8A0), allocates a probe buffer through the RenderWare allocator (0x80049BB8, hint 2, category 0x40 around it), retries up to 30 frames with a debug print, then constructs a `One` (0x58 bytes, 0x8004CFD8) and starts the asynchronous open (0x8004CC94, callback 0x80171AC4 area); state 2, sets the serialiser |
| 2 | (no-op) | async read in flight |
| 3 | 0x80171438 -> 0x801719AC | loading poll |
| 5 | 0x80171440 -> 0x8017175C | build the scene from the archive (models, bones, motions, camera) |
| 6 | 0x80171448 | wait until the sub-objects report ready (`+0x1040`, `+0x180`, `+0x1E0`, `+0x234`, `+0x164C`, the script holder 0x80171170: `+4 == 1` means the script is still loading), then free the `One` (0x8004CF90; `+0x64`, serialiser byte cleared) and go to 7 |
| 7 | update | running |
| 0, 4 | (no-op) | |

The running update 0x80171544 (state 7 only; other states call the table): advances the event time
`t' = t + dt * 30` (or `* 60` when `[0x805F035D] == 0`; constants 30.0 / 60.0 at 0x805F5CF0 / 0x805F5CF4; the ctor of the
Manager sets the 30 mode), clamps to the tour length when `[0x805F0364]` is set (the 0x3B6-parity ids), stores `+0x34 = t - start`,
and then in this order: tour (`0x8017219C`), the `+0x78` sub-object update (returns a "ready" flag), camera/node
controller (`0x80228BC8`), `+0x234` (`0x80172944`), the two lists (`0x801723C4`), and the script dispatcher
`0x801740B8(scriptHolder = 0x80171170(), sceneIndex, tNew, tOld)` followed by 0x8016FD74 and 0x803298E8.

### 3.7 Script dispatch (P, byte-level checked against 90 real files)

`0x801740B8(holder, sceneIndex, tNew, tOld)` (the holder is the singleton returned by `0x80171170`, static object
0x80581294; I: it is the `TPjsEventActionData` of the type strings) runs when `holder+4 == 2` (the `.scr` is loaded; `holder+8` = the file buffer). The record
table starts at buffer `+0x188`, stride `0x80`, count at buffer `+4`. A record fires when `tOld < record.time <= tNew`
(two `fcmpo`, 0x8017411C and 0x80174134) - i.e. exactly once as the shared event time passes it. A fired record then:

| field (record offset) | action |
|---|---|
| +0x08 `msgId >= 0` | `0x80174054(msgId)`: through the MessageTask singleton (0x8010A14C; 0x801FB210 shows the message / subtitle, `0x801FADB4` / `0x801FADC4` bracket it) - the subtitle line |
| +0x0C `voiceId >= 0` | sound manager `0x801D5540(voiceId, -1, -1)` (all 516 shipped records have -1) |
| +0x10 `seId >= 0` | sound manager `0x801D59AC(seId & 0xFFFF, 0, 0)` (all -1) |
| +0x14 `cue[0] != 0` | sound manager `0x801D54D8(cue)` (a 32-byte cue name; every shipped record has `cue[0] == 0` or garbage with `cue[0] == 0`, so never) |
| +0x34 `fade > 0` | `0x8017103C(sceneSlot, fade, fadeFlag != 0, fadeParam)` (the fade helper; sub 8 sub 6; all shipped values 0) |
| +0x40 `> 0` | `0x8032A434()` (Scene sub 8 sub 7; shipped values 1, 2, 3, 36) |
| +0x44 `> 1` (signed, read big-endian) | camera singleton `0x8000DCC0`: `0x8048665C(camera, (float)value)` sets the view window ratio (camera interface only; owned by re-gameplay) |

The holder is filled by `0x80174500`: it formats the script path into `holder+0x10` (`"event/event%04d.scr"` with the id
`[0x805F0348]`), saves the camera object's view ratio into `holder+0x0C` (read from `0x8000DCC0()+0x84`), sets the ratio
with `0x8048665C` and starts the asynchronous read through the file manager (`0x8004506C`, request: path, buffer slot,
`holder`, callback `0x8017438C`); `holder+4` becomes 1 (loading) or 0 (failed). The callback (0x8017438C, on completion)
sets `holder+4 = 2`, `holder+8 = buffer` and byte-swaps in place (0x80043D34 swaps every 4-byte word of the range):
`+0` (4 B), `+4` (4 B), the whole 0x180-byte header area at `+8`, then per record the five words `+0x00,+0x04,+0x08,+0x0C,+0x10` and `+0x34,+0x38,+0x3C,+0x40`.
**`+0x44` is read by the dispatcher but never swapped**, and `+0x14..+0x33` (the cue name) is a byte string.
Consequence (P by arithmetic on the real files): the five records in the table below carry a little-endian authored
value that the GameCube build reads big-endian as a huge positive number and so passes a huge ratio to the camera. This
is an original-game data hazard, not something to change; all five are inside movie / cutscene events whose 3D camera is
not on screen at that moment (U for the exact visual).

| file | record | time | msg | raw +0x44 as read (BE) |
|---|---|---|---|---|
| event1001.scr | 3 | 810 | 3003 | 0x01000000 |
| event1001.scr | 7 | 2019 | 3007 | 0x30000000 |
| event1001.scr | 8 | 2157 | 3008 | 0x01000000 |
| event1001.scr | 10 | 2646 | 3010 | 0x04020000 |
| event8001.scr | 1 | 1 | 0 | 0x50C30000 |

### 3.8 File formats

#### `game/files/event/eventNNNN.scr` (90 files, 516 records; all verified by `tools/re/events_scan.py`)

All fields little-endian on disc (the guest loader converts as in section 3.7). Header: `u32 eventId` (equals NNNN for
all 90 files - checked), `u32 count`, then `0x180` bytes of header area (all zero in 89 files; non-zero stale bytes in
`event1001.scr`, never read), then `count` records of 0x80 bytes. File size equals `0x188 + count * 0x80` for all 90
files. Record fields: section 3.7. Usage counts over the 516 records: `msgId >= 0` 473, `voiceId >= 0` 0, `seId >= 0` 0, cue
name 0 (9 records have non-zero stale bytes after a zero first byte), `fade > 0` 0, `+0x40 > 0` 44, `+0x44` positive after
BE read 5, `+0x00 != 0` 2. The bytes `+0x48..+0x7F` are stale heap content in 11 records (never read). The time unit
is the 30-per-second event frame (section 3.6). The `.scr` files for events 1001, 4101, 4102, 4201, 5024, 8201 are the
subtitle tracks of the movies `E1001.sfd` etc.; those six events have no `_scene*.one` (the Manager finds no letter and
goes to state 8 after the movie).

#### `game/files/event/eventNNNN_sceneX.one` (175 files)

"One Ver 0.60" archives, RW chunk header (12 bytes: type 0, length = size-12, library id 0x1C020037) followed by the
directory documented in `docs/ONE_LAYER.md` (count at data+0x10, entries at data+0x34 stride 0x38: name 0x2C, size,
offset, flags). All 175 files parse: member extensions in the scenes: `.SNB` 175 (exactly one per scene: the scene
node / timeline binary, **U** internal layout), `.BON` 1086 (bone data), `.MTN` 1734 (motions), `.DFF` 1694 (RenderWare
clumps), `.DMA` 539, `.TXD` 174 (texture dictionaries), `.UVA` 31 (UV animation), `.ADB` 10, `.CEN` 66. Scene letters
run A..I; events with `.scr` but no scene: 1001, 4101, 4102, 4201, 5024, 8201; events with scenes and no `.scr` (so no
subtitle track, 63 ids: 0800-0826, 0900-0926, 0960/0961, 0971/0973/0975/0977/0979/0980, 6102) are the plain cutscenes
and the 2P / bonus intros. The scene's real members are read by the build step (0x8017175C); the content of `.SNB` is the
open item for the next round.

#### Movies `game/files/*.sfd` (8 files)

MPEG program streams (`000001BA` pack header), MPEG-2 video 512x480, frame-rate code 5 (30 fps), sizes 11-67 MB:
`E1001` 64.2 MB, `E4101` 34.3, `E4102` 34.3, `E4201` 13.5, `E5024` 19.3, `E8003` 11.2, `E8201` 24.0, `Op` 66.9. Names are
`E%04d.sfd` (strings 0x80502164 / 0x80502404 / 0x80513A30) plus the fixed list at 0x804D34CC (the seven `E*.sfd`) and
"Op.sfd" (0x805F7108). The 241-entry root listing and the debug "movie test" selector are described in `DISC_SYSTEM_MAP.md`.

### 3.9 System fade (FadeTask / FadeDrawTask, P)

The event code does not draw its own fades: `0x80173C18(flag, duration)` forwards to the Fade singleton (accessor
0x8004427C, static object 0x80573E54) as `0x800447E0(fade, flag, duration)` (start the "in" fade: state 2) and
`0x80173C5C(flag, duration)` forwards to `0x800446F8` (start the "out" fade: state 3). `FadeTask` (ctor 0x80044228,
update 0x80044D7C -> 0x80044B00) holds: `+0` level 0..1, `+4` rate scale, `+8` speed, `+0xC` target, `+0x10..+0x12` colour
RGB, `+0x14` state (0 off, 1 full, 2 in, 3 out), `+0x444` "fixed" flag; every update it moves the level by `speed * dt`,
clamps against the target, and writes the alpha byte `(int)(level * 255.0)` into the colour of **40 vertices** (stride 0x18,
bytes at `+0x24 + 0x18*i`), which `FadeDrawTask` (ctor 0x800441D4, update 0x800448C8; both Tasks with flag 0x100, so a
fade also runs while the game is paused) draws as a screen quad. Constants 255.0, 0.0, 1.0, 0.2, 0.4 read from 0x805F5xxx
(r2-relative). The `TPjsEventActionFade` Task (0x8032A10C) is the per-event overlay of the Scene's `+0x78` helper (I).

## 4. Movies (P unless marked)

### 4.1 `TMovieManager`

`0x80036E88(file, mode, w, h)` (construct, 888 B, still "-" in the table): checks the file exists (file system object
0x80037200 vtable +4); on first use initialises the global player block at `0x80572248` (0x120 bytes: Sofdec player
handle at +0, parameter block at +4 with the NTSC / PAL float at +0, path string at +0xEC, flags at +0x10C / +0x110 /
+0x114) and starts the Sofdec server (`0x80407BE0` = `MWSFSVR_MainThrdProc`-named init, evidence: the CRI string in the
image). Then it copies one 32-byte parameter row `mode` from the table at `0x804AC198` (stride 0x20) into the create
parameter block, creates the player (`0x8040534C`, the CRI "mwPlyCreateSofdec" function; 4,148 B), stores the handle,
copies the file name and starts playback (`0x804084D4`), and allocates the `TMovieManager` Task (0x138 bytes, vtable
0x8052DEFC, singleton pointer `[0x805EF150]`, mode `[0x805EF154]`). Parameter rows (read from the image, P):

| mode | stream type | bitrate | width x height |
|---|---|---|---|
| 0 | 0x11 | 3,500,000 bps (0x3567E0) | 640 x 480 |
| 1 | 0x11 | 3,000,000 bps (0x2DC6C0) | 256 x 256 |
| 2 | 0x11 | 2,000,000 bps (0x1E8480) | 128 x 128 |

(The shipped `.sfd` files are 512x480; the mode-0 parameters are the maximum the player is created for.)

`TMovieManager::update` 0x80036B0C: fetches the current frame descriptor (`0x80403E3C` = a "get current frame" CRI
function; the descriptor is copied by `0x800367A8` into the manager at `+0xA8..+0xF4` and `+0x130/+0x134`), releases the
frame (`0x80403A20`), asks the status (`0x804086D8` = `mwPlyGetStat`) and when the status is 3 or 4 (play end /
error) and `+0x90` is set it removes itself (flag bit 0). The blit of the frame to the screen (`0x80036764`) is lowered to
a native draw (`ASPECT_INVENTORY.md` M1).

### 4.2 `StageMovie` (stage flow)

`0x802D33A4(this, "E%04d.sfd")`: stage-flow phase object (base ctor 0x802D2AB8, vtable 0x80559580). It builds the
request with `0x802D3560` and sets state `+0x50 = 0`; its `vf1` 0x802D3118 is a state machine: state 0 waits for the
phase (calls 0x802D2DB0 which constructs the TMovieManager for the movie), then starts the in-fade (`0x80173C18(1, ..)`) and
goes to 1 (or to 4 for movie 0x3E9 = 1001); state 1 waits until the movie manager singleton is gone or skipped, and then
state 4 counts down `0x4C -= dt-ish` and finishes. The skip test reads the pad word `0xD0(0x8005185C())` bit 0x400.

### 4.3 CRI Sofdec as the game drives it

The game side calls only: server init (`0x80407BE0`), create (`0x8040534C`), start (`0x804084D4`), get / release frame
(`0x80403E3C` / `0x80403A20`), status (`0x804086D8`) and execute (`0x80408A68`). Server threads (idle `0x80408C44` ->
`0x80408B30` -> per-handle decode `0x80408B10`; vsync `0x80408CEC` -> `0x803FE6B0`) are described in `docs/CRI_SERVERS.md`:
all Sofdec decode stays lifted guest code; no tape covers a movie (the Westopolis tape boots into the stage), so no
Sofdec function can be pinned by a contract test against recorded behaviour. The runtime streams `.sfd` through the
host file device (`NativeCriFileDevice`).

## 5. CharCommand dispatch base (P)

* `CharCommand` base object (0x14 bytes): `+0` vtable 0x8053E1E8 (corrected in round 4, see rwalloc.md section on corrections) (set at 0x8006E598 and the dtor 0x8006E4CC), `+4` u16
  command id, `+6` u8 target slot / category, `+7` u8 flags (bit 0 = already handled, bit 2 = deferred, addressed by
  handle), `+8` u32 argument, `+0xC` u32 pair `{handle, serial}` zeroed by 0x8006E5D0. Constructor 0x8006E57C(this, id,
  arg); derived commands (`SetPositionCommand` 0x8009E8E8, `SetVelocityCommand` 0x800DEA9C, `DeadCommand`, ... the
  command handlers belong to re-gameplay) set their own vtable on top.
* `CharCommandManagerTask` (Task, ctor 0x801DA140, vtable 0x8052E20C, name tag "CharCommandManagerTask", flag
  0x100 so it runs while paused): it registers with the singleton `0x80099B58` (+0xB4, helper 0x80014E1C) and its update
  0x801DA0A0 calls `0x801DA5A4(manager, dt)`.
* `0x801DA5A4` (the dispatcher; singleton object at 0x805778B4, accessor 0x80099B58): two buffers (index `+4`, flipped by
  `cntlzw` at the end) of seven priority lists (stride 0xC, buffer stride 0x54, base +0xC). Per frame, for each of the
  seven levels it takes the commands of the CURRENT buffer in order and, for each command, (a) handled (`+7 & 1`): just
  delete (vtable+8, arg 1); (b) deferred (`+7 & 4`): resolve the target by handle (helpers 0x801DA760, 0x80079BA4) and, if it
  exists, append the command to the target's inbox (0x801A02B4 at target+4); (c) otherwise deliver to the target slot
  (`+6`) through the dispatcher chain `0x800C9F6C` / `0x800CB948` and delete. After a level the list is cleared
  (0x801DA738) and reset (0x801DA274); after seven levels `+8 = 7`, the buffer index flips and the busy byte `+0` clears.
  Commands queued during the frame go to the OTHER buffer, so a command is delivered the frame after it is sent.
* What the command classes do (player / enemy / object handlers) is not in this field.

## 6. Hot parts on the benchmark (profile)

Source: the earlier main-thread sampling of the Westopolis replay in the 2026-11-11 round
(`wt-gc-removal/.build/perf/samp-2026-11-11/samples.json`, 11,522 samples of the game thread, 20 s) mapped to the
S2 ownership table. A fresh sample on 700f72d is part of the "S2 report" section if the slot allowed it; the numbers
below are those earlier samples.

| group | samples | share of the game thread |
|---|---:|---:|
| native `GameplayScheduler` (Walk::run 43, dispatch 21, dispatch_execute 12, Graph::link 10, advance 8, hook lambda 7, vector / push_level / leave_level 6, enable lambda 1) | 108 | 0.94 % |
| events field, game framework functions (CharCommand ctor / update, Fade, Vibration, DebugDisp, Heap, Task::update stub) | 47 | 0.41 % |
| events field, Sofdec (idle thread work reached on the main thread) | 12 | 0.10 % |
| events field, event scene / manager code | 0 | 0 % |

Conclusion: the task-tree walk is already native and costs under 1 % of the game thread in total; the whole field
(Task framework residue, CharCommand dispatch, events, movies) is about 0.5 % of the main-thread samples. The event
Manager / Scene code does not appear at all on the Westopolis benchmark. There is no profitable owner in this field:
a perfect rewrite of everything above would be bounded by roughly 0.5 % of one thread, below the benchmark noise, and
the behaviour gate and three alternating rounds would cost more than they could prove.

## 7. Runtime checks (headless Westopolis tape)

Round 1 could not get the slot; round 2 ran the traces (see "Hazard +0x44: runtime evidence"). On the Westopolis tape
(120 s) the Manager / Scene never appear (`ev.id` stays 0), so the Manager / Scene layouts of sections 3.4-3.6 still rest on
disassembly (P) and have no runtime dump. The layout `tools/re/events_layout.txt` watches the event Manager and Scene
(found by vtable), the event globals, the task counters, the CharCommand manager and the movie globals at every host
service; `tools/re/events_run.sh` is the runner.

## Hazard +0x44: runtime evidence (round 2)

Exe: the shared `wt-s2-integrate/.build/s2/base-700f72d.exe`, no build of my own; every run through `capped_run.ps1
-CpuPercent 7 -Lock s2-slot-a -WorkingDirectory <worktree>`; headless, `--no-audio`, `--behaviour-trace` with
`tools/re/events_cam_layout.txt`, runner `tools/re/events_cut.sh` (cold boot with `--boot-cutscene <id>`, the
Westopolis card / pad / display files, no tape). Not timing runs; none of this changes game data or code.

**Route.** `--boot-cutscene eventNNNN` exists (it needs `event/eventNNNN.scr` or `ENNNN.sfd`). A first attempt without the
card / pad / display files stopped after 0.48 game seconds (stop reason HostRequest); with them the runs go to the budget.

| run | game time | what the trace shows |
|---|---|---|
| event1001 (hazard records 3, 7, 8, 10) | 240 s | `TMovieManager` live from 1.42 s to 122.0 s (the E1001.sfd movie), then the stage flow continues; `ev.id` (0x805F0348) stays 0, event Manager never created, script holder state (0x80581298) stays 0 (never 1 or 2), `TPjsEventFolder` stays `{0, 0, 0}` until 127.3 s |
| event4101 (control, sane records, also a movie + .scr) | 100 s | same shape: movie 1.42-65.0 s, `ev.id` 0, holder state 0 |
| event8001 (hazard record 1, t = 1) | 100 s | no movie, no Manager: `ev.id` 0, holder state 0 for the whole 100 s; the folder is constructed at 8.4 s and then nothing |
| Westopolis tape (`events_run.sh`, Manager / Scene trace) | 120 s | `ev.id` 0, no Manager / Scene found by vtable, folder constructed at 1.32 s and never started; the CharCommand double buffer toggles every frame |

**Findings (P, from these traces).**
1. In the movie route (`--boot-cutscene` 1001 / 4101) the `.scr` dispatcher of section 3.7 is **never entered**: the script holder
   never reaches state 2 and no TPjsEventActionManager exists. So the four event1001 hazard records are not
   executed through that dispatcher while the movie plays in this route; how the movie's subtitle track (messages 3000-3011) is
   driven is therefore **U** (not through the TPjsEventAction* path, which is a correction of the section 3.8 remark
   that those six `.scr` files are consumed by the Manager). The control event 4101 behaves identically.
2. event8001 never starts its event in 100 headless seconds without input (the story hand-off probably waits for
   a pad press or a stage load); the dispatcher was not reached, so the `t = 1` hazard record was not exercised either.
3. The camera fields watched (`0x8056FF9C`, `+0x80/+0x84` of the camera holder, and the frame matrix through `[0x8056FF20] + 0x10`) read
   `[0, -1.1666e-38]` then `[0, -1.1668e-38]` in all three runs, identical for hazard and control events, and the matrix
   pointer read as zeros. Those values are denormal garbage, not a view ratio, so the field mapping of the camera holder at 0x8056FF1C
   is **not validated** by this run (the singleton is probably not constructed in the cutscene route); no effect of `+0x44` on the
   camera is observed, and none can be claimed.

**Conclusion.** The visible effect of the unswapped `+0x44` is **not measurable** on the shared exe without a new input tape
that makes the event Manager run (event8001 through the story / event-stage flow, or event 1001 via a route that really uses
the `.scr`). That needs a recording with the user (or a code route), which this task excluded. Next step proposed: record a short
windowed tape that starts event8001 from the 2P / event stage menu, then rerun with this layout (add the real camera object
once its address is located from the running Manager: the object returned by 0x8000DCC0 after the Manager ctor).

## S2 report

**Proven (P):** Task node layout, constructor / destructor / traversal behaviour, the layer tree names and update order
(strings read from the image), the 227-function Task subclass census, the event Manager / Scene state machines and
jump tables, the script dispatcher semantics (time window, field actions), the `.scr` loader's byte-swap coverage
(including the unswapped `+0x44` hazard in 5 records), the `.scr` / `.one` / `.sfd` formats checked against all 90 / 175 / 8
files (`tools/re/events_scan.py` exits clean, header id == file name for all 90), movie parameter rows, the CharCommand
dispatcher structure, the fade task.
**Inferred (I):** script holder = `TPjsEventActionData`; lock pair 0x80050C40/0x80050BF8; sound-manager poll meaning;
`TPjsEventActionFade` role. **Unknown (U):** `.SNB` internal layout, scene build 0x8017175C internals, StageMovie details,
who calls 0x801746F0 from 0x8034xxxx, what the camera does with the 5 hazard records.
**Owners: none.** The task walk is already native (0.94 % of the game thread in the 2026-11-11 samples) and the whole
field is about 0.5 %; event Manager / Scene code never appears on the Westopolis benchmark, and no tape covers a movie
(Sofdec cannot be pinned by a contract test). No gain possible = no owner, no code written, no dead code removed. No
shared-list lines added. No build of my own was made (the contract's `.build/watch` build never got the slot), so the
profile above is the earlier sample set, not a fresh 700f72d one.
**Coordinator re-run:** `python -B tools/re/events_scan.py --explain`, `python -B tools/re/events_task_census.py <s2_ownership.tsv>`
(output equals docs/re/events_task_census.tsv), and when a slot is free
`capped_run -Lock s2-slot-a ... tools/re/events_run.sh <exe> 120000 <out>` for the missing runtime trace.
**Open questions:** runtime trace of Manager/Scene state timeline on the tape (Doom's Eye / Sonic events); `.SNB` format;
whether the unswapped `+0x44` records visibly change the E1001 / event8001 presentation; fresh profile on 700f72d.
