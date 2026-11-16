# High frame rate (120 / 180 / 240 Hz)

Status 2026-11-16: **foundation in source, opt-in, not yet run in the game.** The work was done without a disc, so it
has been checked only by the runtime's unit tests (below), not on a stage. The default (60) is unchanged.

The ideas come from ShadowRecomp, the Xbox recompilation of the same game (FoxWorkshop/ShadowRecomp,
`ShadowRecomp/patches/fps_patches.cpp`), whose engine has the same frame limiter. No code was copied: their project is
GPL-3.0 and its addresses are the Xbox build's.

## Turning it on

- Command line: `--frame-rate 120` (also `180`, `240`; `60` is the console's rate).
- `display.cfg`: `frame_rate = 120`. The Options menu does not offer it yet, and keeps the line when it saves.
- With a high rate, the presentation rate follows it unless `--render-rate` / `render_rate` sets one. Use a display
  that refreshes at that rate, or turn V-Sync off, to see the extra frames.

## How it works

The game already runs at whatever rate its frame object holds (`0x8057E760 + 0x50`): 60 Hz, or 50 Hz in PAL 50 mode.
Its frame period P, the published dt and much of its per-step scaling follow that word. For example, the effects task
0x801D2318 scales by `60 / hz` (docs/effects/particles.md). A high frame rate puts a multiple of 60 in that word and
moves the parts of the platform tied to the frame along with it:

| piece | at 60 (unchanged) | at a high rate | where |
|---|---|---|---|
| frame object `+0x50` | the game's 60 | the high rate, published at every frame | `NativeFrameLimiter::publish_rate` |
| limiter wait | one 1/60 s period | one period of the rate, on the one time base | `native_frame_limiter.cpp` |
| dt cap (`+0x34`, 3 periods = 50 ms) | 3 × 1/60 s | cap × rate / 60 periods: still 50 ms | `frame_rate::scaled_cap` |
| video field (60 Hz timing) | 1/60 s, frame-locked | one frame of the rate, frame-locked | `NativeVideo::set_frame_rate` |
| real-time pacing ticks | 300 Hz | five per frame (600 / 900 / 1200 Hz) | `System::set_frame_rate`, `frame_rate::pacing_hz` |
| host service (input, savestates) | every 5 ms | every 5 ms | unchanged |
| audio passes | every 5 ms | every 5 ms | unchanged |

What keeps 60 Hz even with a high rate set:
- **In-engine events.** These are the frames where the interval word (`+0x10`) asks for more than one period
  (EventManager sets 2 at 0x80173F84). ShadowRecomp does the same, because event timing and audio sync are authored
  in console frames.
- **Movies** (`r13 + 0x2C50`): the movie decoder is paced by the video refresh.
- **A 50 Hz game.**

The game itself only ever writes 50 or 60 to the word. A supported high rate found there is therefore read as the
game's 60, including after loading a savestate made at another rate. At 60, nothing new is written or scheduled:
tapes and savestates are as before.

**The 50 ms slowdown point.** The game caps one frame's dt at `cap` periods, so a long hitch slows the game down
rather than letting physics skip ahead. On the console that is 3 × 1/60 s = 50 ms. Kept at 3 periods of 1/120 s, an
ordinary 26 ms frame would already be slowed down. ShadowRecomp found the same problem and recomputes the cap to keep
50 ms (their "Sewer note" in `SetFramerateMidAsmHook`). Here the limiter scales the cap it applies; the word in memory
stays the game's.

## Tests (runtime/tests/gekko_core_tests.cpp)

`./gekko_core_tests "frame rate" "frame limiter" "native video" "pacing"` runs them:
- `frame rate grids`: every pacing grid holds the 50 Hz, 60 Hz and its own frame boundaries (also `static_assert`ed in
  `gekko/frame_rate.h`), and the cap and game-rate rules.
- `native frame limiter high frame rate`: 600 frames at 120, 180 and 240 Hz. Each waits exactly one period on the
  grid, and dt and the published word follow the rate.
- `... fallbacks`: events, movies, 50 Hz, the default, and a savestate's word.
- `... keeps the slowdown point`: a 100 ms hitch at 120 Hz is capped at six periods (50 ms); the step count is not
  capped (as on the console).
- `native video follows the frame rate`, `system pacing follows the frame rate`.

## To verify in the game (needs the disc)

Run with `--frame-rate 120` first, then 240. Watch for:
1. **Game speed.** A stage's in-game timer against a stopwatch over 60 s. It must match 60 Hz exactly.
2. **Retrace counting.** The video now raises 120 retraces a second. Anything that counts retraces (VI retrace
   callbacks, `VIGetRetraceCount` users) runs twice as often. The loading screen's draw throttle follows the field
   (`display_field_us`) and is fine.
3. **CRI servers.** ADX streaming and Sofdec run per retrace (docs/CRI_SERVERS.md). Listen for gaps in music and voices,
   and check that an in-stage movie plays at its own speed.
4. **Events.** They must drop to 60 Hz and stay in sync with their voices. Check the switch back at the event's end.
5. **The per-object list below.** At 60 Hz each item behaves as on the console; at a high rate, each runs too fast or
   breaks until it is fixed.

## Per-object fixes: the checklist

ShadowRecomp fixed about 100 sites where the game counts frames instead of time. The game logic is the same on the
GameCube, so the same sites will misbehave here. Each needs finding in the PAL DOL, usually by class name. Run
`python tools/guest_classes.py <main.dol>` to list the RTTI classes and their virtual methods, then confirm each fix
there and add it as a native owner or lowered site. Addresses below are GameCube (PAL) where already known; "find"
means not located yet.

Kinds of fix (ShadowRecomp's patterns):
- **dt**: replace a per-frame constant step with `step × dt × 60`.
- **60 Hz step**: run the logic in fixed 1/60 s steps and carry the remainder (for smoothing tuned to 60 steps a
  second).
- **slice timer**: rebuild a parity toggle (odd/even frame) from a delta-time timer cut into 1/60 s slices.
- **hold key**: snap to the key the 60 Hz frame would have shown, instead of blending between keys.
- **grace**: ignore collision for the first 1/60 s of a launch, then take the 60 Hz destination.

### Priority 1: the player, rings and common objects (seen on every stage)

| object | what goes wrong above 60 | fix | GameCube |
|---|---|---|---|
| Player run state (Behavior::Ground) | stick input applied per frame: slow walk, can stick in Walk | dt | player behaviours (docs/SHADOW_PLAYER_NATIVE_RECONSTRUCTION.md: Ground among the 38 Behavior vtables); find |
| Player turn rate (shared by player and partners) | turns faster | dt | find |
| Player jump init / exec, speed boost panels, ground start speed | jump height and boost differ | dt | find |
| Ring spin, scattered rings (ScatteringRing) | spin and scatter too fast | dt | ScatteringRing 0x8011ADC0 |
| Moving platforms (FootingMovable linear mode), pulley (UpDownReel) | move too fast | dt | Footing* 0x80149C98 / 0x8014BB94 (candidates); UpDownReel: find |
| Containers / Shadow boxes (ContainerBehavior fall speed) | fall too fast | dt | ContainerNormal 0x800F8660, ContainerHard 0x800FB6E4, ContainerWeapon 0x800FE944 |
| Ground stick (collision, airborne) | bumps on small slopes | scoped stick | find (collision; docs/GAME_MAP.md) |
| Jump boards, catapults, Dark Spin launch (CharaColliMove) | "bump" or death from a short swept move | grace | CharaColliMove 0x80094E08; catapult 0x80142178 / 0x80142BFC (candidates) |
| Hit, death and damage blinks (enemies, bosses, bomb, battleships) | blink looks random | slice timer | ThrowBomb 0x8020A02C; others: find |
| Hero / Dark gauge fill | fills too fast | dt | find (HUD) |

### Priority 2: stage objects and enemies

| object | what goes wrong | fix | GameCube |
|---|---|---|---|
| Rocket | too fast | dt | RocketBase 0x801158E8 |
| Digital Circuit searchlight (ElecSearchLight) | launches too hard | dt | ElecSearchLight 0x80155B10 |
| Death Ruins launch plant (StretchGrass modes 2–4) | charge grows with the frame rate | dt | find |
| Falling weight (states 5 and 6) | too fast | dt | Weight candidates 0x8012B00C / 0x8012C0C8 |
| Killer plant fireball (Vine::FireBall) | too fast | dt | 0x8014ED78 (candidate) |
| BkLarva generator | dt applied twice | remove one | find (BK_LARVA, docs/DRAW_DISTANCE.md) |
| Enemy turn to target, AI aim, aim-bone snap | turn too fast | dt | find (EnemyBase) |
| Lock-on and auto-fire intervals (ControlCannon / Gun / Lockon) | fire rate off | dt | LockonMark 0x80061F90 (draw); control: find |
| Scattered weapons from boxes | collide mid-air | 60 Hz step | find |
| Gadget break effects (EffCrash3D) | wrong pieces / white flash | 60 Hz step | find |
| Tornado towing end check | never ends | fixed check | find |
| Boss / special camera | snaps | dt | find |

### Priority 3: vehicles

| object | what goes wrong | fix | GameCube |
|---|---|---|---|
| Cars and bike (TVehicleCar) | cannot move at high rates; air control | 60 Hz step + dt | Vehicle::TVehicleCar (docs/GAME_MAP.md vehicles); find |
| Air Saucer, Walker (jump mechs) | collide-and-slide tuned to 60 | 60 Hz step | Vehicle::AirSaucer, Vehicle::Walker; find |
| Black Volt / Hawk (BirdBase) | stick-release rotation | dt | Vehicle::BirdBase; find |
| GUN lifts | aim speed | dt | Vehicle::GunLift; find |

### Priority 4: effects, animation, menus, events

| object | what goes wrong | fix | GameCube |
|---|---|---|---|
| Effect alpha fades (single and three channel) | fade dies early at 61+ Hz: missing backdrops, red tint | carry leftover | find (EffectTask exec 0x801D2318 already scales by 60 / hz) |
| Effect commands (scalar / vector) | fade and motion off | carry leftover | find |
| UV flipbooks (1/60 s keys) | half rows | hold key | find |
| CSD colour keys | flashes between keys | hold key | find |
| Melee trail and hit sweep | blade tip spikes | single-frame mode | find |
| Belt / locus ribbons | fade per frame | dt | Test::EffBelt 0x802DE4BC (draw) |
| Library menu scroll and cooldown | scrolls 4–5 items | frame cap | AdvReal::StoryLibrary; find |
| Egg Dealer roulette bounce, Advertise message timer | too fast | dt | find |
| Sound-effect durations | cut early | dt | find (SE manager, docs/SE_MANAGER_MAP.md) |
| Jingle / music fade timing | wrong length | dt | find |
| Event camera cuts, motion-key teleports | snap at 61+ Hz | hold key | events stay at 60 here, so lower priority |
| 2P battle fade | ends early | dt | find |

## Next steps

1. Run the verification list above on the disc at 120 Hz and record what breaks.
2. Locate priority 1 in the DOL (`tools/guest_classes.py`), then fix and test each site at 60 (unchanged) and 120.
3. Add the rate to the Options menu once priority 1 is done.
