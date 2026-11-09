# Game errata: deliberate fixes of original-game bugs

Each entry is a DELIBERATE DEVIATION from the original game, chosen by the user. They are recorded here and kept apart
from exact-equivalence results: an A/B against the original runs with `SHADOW_ERRATA=0` (which restores the original
behaviour for that comparison only), and any difference an erratum causes is reported as the erratum's, never as
parity. Code: `runtime/include/gekko/game_errata.h`, `runtime/src/gekko/game_errata.cpp`. Test: `game_errata_tests`
(CTest, labels `reference;errata`), on the original DOL words in the reference interpreter.

Earlier data errata live with their data (sound table row 90 SE_Z1090_WARPHOLE: `runtime/include/gekko/native_sound_tables.h`).

## E1 - EnemyBaseAI state-machine lock byte never initialised (user decision 2026-11-02)

**Bug.** `EnemyBaseAI::EnemyBaseAI` (0x8019F1D4) zeroes its state machine at +0x28 (owner +0x28, current state +0x2C,
previous state +0x30) but not its lock byte at +0x34, then calls `StateMachine::Start` (0x8019F528, this + 0x28).
Start makes no transition while the lock byte is nonzero; it then only clears the byte (0x8019F5C8). Heap memory is
not cleared, so the byte is whatever the memory's previous user left (traced: a freed thread stack). With a nonzero
byte the enemy's AI skips its initial state: it starts with no state instead of `EnemyAIState_EnemyBaseAI_None`
(singleton 0x805F03F4; its enter 0x801A0000 records the owner) until its first `ChangeState` (0x8019FD00), which then
runs without a None exit.

**Evidence that it is uninitialised.** Base constructor 0x8019B72C writes +0x0C..+0x18; 0x8019F1D4 writes +0x18,
+0x1C/+0x74 (flagged), +0x28, +0x2C, +0x30, +0x38 block (0x8019FC18), +0x60 block (0x8019F690), +0x6C, +0x70 - never
+0x34; none of the 10 derived constructors (0x80183940, 0x80184378, 0x80186038, 0x80189704, 0x8019A99C, 0x8028F080,
0x80290328, 0x8029F624, 0x802AF4CC, 0x802B4144) stores to +0x34 before calling it. Census at Start (Westopolis replay,
`GEKKO_CALL_CENSUS_ADDRS=8019F528 GEKKO_CALL_CENSUS_R3_WORD=C`, original behaviour): the byte at Start is 0x00 for
13 of the 30 AIs and garbage for 17 (0x21, 0xB9, 0x80, 0xC2, 0x99, 0x56, 0xE5, 0xF8, 0x81, 0x3F, 0xC1, 0xBF, 0x42, 0x7B, 0x81,
0xC5, 0xC5): in the original game most enemies of this replay skip their initial state.

**Evidence that 0 is intended.** The sibling state machine at +0x38 is initialised by 0x8019FC18, which zeroes its own
lock byte; Start leaves the byte 0 on every path; the AIs whose byte happens to be 0 enter None as designed.

**Why it surfaced.** Running CRI's servers without GameCube threads changes what earlier thread stacks leave in heap
memory: the bridge run gave a third AI the byte 0x80 (0x80379494, OSWakeupThread's return address from SelectThread).
Reproducing console stack garbage was rejected (user: no stack-garbage replication).

**Fix.** At the constructor's entry the lock byte is set to 0 (a declining hook at 0x8019F1D4; the constructor's own
body then runs unchanged). When EnemyBaseAI is reconstructed natively, its constructor initialises the byte itself and
the hook goes.

**Regression test.** `game_errata_tests` runs the real constructor up to Start's return with lock bytes 0x00, 0x21,
0xB9, 0x80, 0xFF: without the fix exactly the nonzero bytes skip the initial state (the bug reproduces); with it every
AI enters `EnemyAIState_EnemyBaseAI_None`, the None enter records the owner, the byte is 0. It also checks the hook is
installed and that `SHADOW_ERRATA=0` installs nothing.

**In-game result (Westopolis tape, 90 s, exe .build/watch 15:57, runs .build/ab-jobs/o0, e0, e1).**

| run | E1 | CRI bridge | AIs constructed | garbage cleared | None enters (0x801A0000) | PCM sha256 | frames |
|---|---|---|---|---|---|---|---|
| o0 | off (SHADOW_ERRATA=0) | off | 30 | - | 26 (13 AIs) | 015445f2... | 5,360 |
| e0 | on | off | 30 | 17 | 60 (all 30) | 015445f2... | 5,360 |
| e1 | on | on | 30 | 17 | 60 (all 30) | 015445f2... | 5,360 |

o0 is byte-identical in PCM to the pre-erratum baseline (full0). The erratum changes no audio in this replay; its effect
is the AI state sequence above. With E1 on, the bridge no longer changes any AI start (the stack-garbage dependence is
gone), which removed the only gameplay difference the CRI bridge had shown: the e0 / e1 censuses (Start and the None
enter: address, tick, r3, r4, LR) are identical; the recorded word differs only in its three bytes after the lock byte
(+0x35..+0x37, padding before the +0x38 state machine; Start / ChangeState read the lock with lbz only).

## E2 - Player::PlayerMotion's toggle counter +0x3EC never initialised (user decision 2026-11-03)

**Bug.**
- **The reader:** `Player::Shadow::ShadowMotion::update` sub0 (0x800B4B3C). Whenever bit 0x18 of the motion core's
  flag set is set (motion +0x58 → +0xA8, tested by fn_8006CEA0), it increments the counter at +0x3EC:
  `lwz r3,0x3ec(r30); addi r3,r3,1; clrlwi. r0,r3,31; stw r3,0x3ec(r30)`.
- **The choice:** the new value's parity selects ShadowMotion vf13 (0x800B54C8) when even or vf14 (0x800B541C) when
  odd.
- **What they do:** a matched pair. vf13 sets flag 3 (fn_800613A4) and runs `RpClumpForAllAtomics` with callback
  0x80039298; vf14 clears it (fn_80014E1C) with callback 0x800393A8. It toggles the player model's atomics (a visual
  toggle; what raises bit 0x18 is not traced).
- **The leftover:** the motion object (0x868 bytes, `new` at 0x800B14D0) is allocated over a freed loader thread's
  stack, so the counter starts from that stack's leftover bytes.

**Evidence that it is uninitialised.**
- The base constructor `Player::PlayerMotion` (0x800AAE18) memsets +0xD0..+0x3DF and stores +0x3E0, +0x3E8 and +0x3F0,
  but not +0x3E4 or +0x3EC.
- `ShadowMotion`'s constructor (0x800B5844) writes +0x18, +0x458..+0x474, +0x47C.., +0x490.. and +0x7E0..+0x7EC.., but
  not +0x3EC.
- The only store to +0x3EC in the program is the increment (0x800B4B98).

**How it was found** (`docs/NATIVE_WORKERS.md` 5a, W0b):
1. Poisoning the freed loader stacks with 0xA5 and with 0x5A changes about 906 KB of later heap and image state
   (Westopolis tape, 85 s). The control: two unpoisoned runs are byte-identical.
2. The first flow is in frame 1615.
3. A guest watch on the 5,117 stale bytes left at frame 1614 shows one read: this increment. On Westopolis the object
   is 0x80F121E8 and the counter starts as 0x810132E0 (even), a leftover pointer.

**Why it surfaced.** The native background read (`GEKKO_NATIVE_BG_READ=1`) changes which leftover bytes the object
lands on. The counter starts with the opposite parity on 9 of 40 stages (stg0201, 0300, 0302, 0400, 0404, 0501, 0503,
0604, 0615). A native loader cannot reproduce the original's leftovers.

**Fix.** At the base constructor's entry +0x3EC is set to 0, by a declining hook at 0x800AAE18; the constructor's own
body then runs unchanged.
- The first toggle then always takes vf14, on every stage, whatever was loaded before.
- This equals Westopolis's original phase (even), and differs from the original where the leftover was odd.
- +0x3E4, also skipped by that constructor, is read at 0x800B2BD8..0x800B2D84. It is not proven to be read
  uninitialised, so it is not touched.

**Regression test.** `game_errata_tests` runs the real reader 0x800B4B3C on its original words, with a motion whose
core answers 0 and has bit 0x18 set, and stand-ins for vf13 / vf14. The inputs are the start values 0, 0x810132E0
(Westopolis), 0xFDC301FF / 0x00800000 (stg0201 original / native read), 0x57533577 and 0xA5A5A5A5.
- **Without the fix:** the branch follows the parity (the bug reproduces: stg0201's two values take different
  branches).
- **With the fix:** every value starts from 0 and the first toggle takes vf14.
- It also checks that neither constructor's words store to +0x3EC, and that the hook is installed.

**In-game result** (`.build/loadfix`, E2 on; `.build/stage_loading/abE2`). Original read vs native read on stg0201,
stg0604 (both flip stages without E2) and Westopolis:
- +0x3EC starts at 0 in both modes on all three;
- the semantic readout at 16 frame pairs is identical, apart from the player's heap address (`STAGE_LOADING.md` 9e).

The exit report counts it ("E2 PlayerMotion constructed 1, counter garbage cleared 1").

## E3 - the landscape's texture-pattern animations run one clock per streamed block (user decision 2026-11-05)

User request: GUN Fortress's red corridor lights should all follow one blink pattern.

**Bug.** The lights are texture pattern `s0600_zn055` (period 64 frames, keys 1..8..1 every 4 frames, frame 1 black;
`RENDER_FLICKER.md` "Second bug") on quads of the blocks' layer worlds. A pattern's phase comes from the clock of
the TTextureChange set that animates its material, and every block layer gets its own set:
- **Made per block layer.** When a block layer's world has streamed in, the RG1 callback 0x80046438 calls 0x8004FBD0
  (`spec_TBlockManage` 3.4 / 3.5). That makes a TTextureChange (0x8004F6D0 -> constructor 0x8004F4A8 -> init
  0x8004F284) as a child of task group 3, holding every pattern material of that world, and stores the set's handle
  {u16, s16} in the TObjLand slot layer (+0x0C + 12 k).
- **Its own clock.** The init stores the clock +0x34 = 0 (`stfs f0,0x34(r26)`); the update 0x8004F3C4 adds 60 dt
  every frame; the frame choice 0x8004F0FC wraps that clock by the period. A global clock (0x805E4A60) overrides it
  only while a cutscene sets it >= 0 (0x80171544; reset to -1 by 0x80050074).
- **Result.** Each block's lights are phased by the moment that block finished loading. Blocks stream in as the
  player moves (TBlockManage), so neighbouring corridors - and any surface split across a block border - blink out
  of step. The original game does the same; how far apart depends on load timing, which the port's faster native
  loading changes but cannot remove. Streaming is a console memory mechanism, not authored intent: a pattern is one
  animation of the landscape.

**Fix.** At the entry of 0x80038148 - called only by that callback, right after the layer's set was made and its handle
stored (a declining hook) - every live land set takes the clock of the oldest one:
- **Land sets** are the TTextureChange children of group 3 (vtable 0x8051E78C) whose handle (+0x2C) a loaded TObjLand
  slot (0x80572560, 32 x 0xC0, state != 0) holds in a layer.
- **The oldest** is the first of them in group 3's child list; the task constructor 0x8004F014 appends at the tail, so
  list order is creation order. The blocks loaded with the stage keep their own clock; every later block joins it.
- **Untouched:** objects' pattern sets (0x8004FA40: bosses, stage objects, events), the cutscene clock, the pattern
  data and the update. Everything is read from guest memory, so savestates need nothing extra.
- `SHADOW_ERRATA_E3=0` leaves E3 out (A/B of this erratum alone); `SHADOW_ERRATA=0` leaves out all errata.

**Regression test.** `game_errata_tests` runs the real set init 0x8004F284, update 0x8004F3C4 and frame choice
0x8004F0FC on an old land set, an object's set and a land set made 37 frames later, as children of group 3 with
TObjLand holding the two land handles (and a free slot holding a stale one):
- **Without the fix:** the later set's clock stays 37 frames behind and it chooses a different key of a period-64
  pattern on 200 of 200 frames (the bug).
- **With the fix:** both clocks are bit-identical on all 200 frames and choose the same keys; the object's set keeps
  its clock; the stats count 2 land sets, 1 aligned.
- **Words:** 0x80038148's only `bl` site is inside 0x80046438; the init and the update store the clock; the hook is
  installed, and `SHADOW_ERRATA_E3=0` leaves it out.

The exit report counts it ("E3 land layers loaded N, land pattern sets seen N, clocks aligned N").

**In-game result** (`.build/corridor-lights`, exe .build/watch 2026-11-05, stg0600 boot, 30 s headless, material probe
`GEKKO_TEXANIM_TRACE=s0600_zn055`, analysed by `clocks.py`):
- the cutscene clock 0x805E4A60 reads -1 on every traced frame of play, so the sets' own clocks decide;
- `SHADOW_ERRATA_E3=0` (orig-idle): two zn055 sets (two blocks, the second loaded 0.45 s later), the second 117 frames
  (53 mod 64) behind; their chosen keys differ on 1657 of 1657 shared frames;
- E3 on (e3-idle): 6 land layers, 1 clock aligned; the clocks are equal on all 1657 frames, the keys differ only on the
  new set's first traced frame (before its first apply).
Not yet done: a before/after picture sequence of several corridor lights (the tape to the first corridor).
