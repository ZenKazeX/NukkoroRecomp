# High frame rate: the per-object fixes in detail

Companion to docs/HIGH_FRAME_RATE.md, whose checklist names each object; this page says how each one is fixed.

**Source and licence.** These are notes on ShadowRecomp's `ShadowRecomp/patches/fps_patches.cpp` (FoxWorkshop/
ShadowRecomp, GPL-3.0), the Xbox recompilation of the same game. They describe each fix's mechanism in words and short
formulas so it can be rewritten here; no code was taken, and none should be. Addresses and offsets are the Xbox build's
(x86, `sub_XXXXXX`), given only as hints for finding the same code in the GameCube DOL (tools/guest_classes.py for the
class names, then the original code's shape and constants).

**How reliable.** Written by three reading passes over the file (2026-11-16), one per third. The formulas that matter
most for feel were re-checked against the source by hand:
- the global turn rate;
- the Ground run-speed decay;
- the car's 50/50 velocity blend redone as `0.5^frames`.

Entries the notes mark as empirical or unclear stay so marked.

## Read this first

- **One unit.** `frames = dt × 60`: how many 60 Hz frames this real frame is worth (0.5 at 120 FPS, 2 at 30 FPS).
- **60 is untouched.** Nearly every fix is skipped when `frames` is within 0.0001 of 1, so 60 FPS stays exactly the
  original. Do the same here: the 60 Hz path must stay bit-identical (tapes, `--verify-recomp`).
- **Two dt conventions.** The effect command nodes take dt in 60 Hz frame units, clamped to at least 1. The player and
  behaviour code takes seconds (the player's dt pointer) and converts to frames itself. Check which one a site uses
  before porting.
- **The recurring patterns:**
  - **dt-scale.** A per-frame step `x += k` becomes `x += k × frames`. A per-frame factor `x *= r` becomes
    `x *= r^frames`. A per-frame lag `x += (target − x) × s` becomes `s' = 1 − (1 − s)^frames`.
  - **First-order lag over N frames.** `before − (before − after) × (1 − r^frames) / (1 − r)` (car input).
  - **Fixed 60 Hz step.** Run the solver in whole 1/60 s substeps (they cap it at 4), carry the leftover phase, and
    interpolate the drawn position between the last two steps. Used for vehicles.
  - **Slice timer.** A blink that toggles on odd/even frames is rebuilt from a dt timer cut into 1/60 s slices.
  - **Hold key.** Where data has keys every 1/60 s (UV flipbooks, CSD colour keys, event motion), snap to the key the
    60 Hz frame would have shown instead of blending between keys.
  - **Grace period.** For launches (jump board, catapult, Dark Spin), skip collision for the first 1/60 s, then run
    the original collision once from the captured origin.
  - **Carry leftover.** Effect alpha fades keep the remainder of the last step, so a fade completes instead of being
    cut off at 61+ FPS.
- **What to port first** (the reading passes' consensus):
  1. the player's turn rate, run-speed decay and speed-boost start speed;
  2. the ground sample clock and jump flip hold;
  3. scattered rings;
  4. the enemy and boss blink slice timer;
  5. the launch grace;
  6. the vehicle fixed step;
  7. the effect alpha fades.

  Events stay at 60 Hz in our port (docs/HIGH_FRAME_RATE.md), so the event-camera fixes can wait.

## Part 1: the time model, platforms, rings, blinks, the vehicle step (source lines 1-1130)

Notation: `frames = g_deltaTime * 60` ("F", elapsed time in 60 Hz frames, 1.0 at 60 FPS). Most patches are "the original code still runs once per rendered frame with its 60 Hz constant, so add the missing (F-1) frames worth of the same step". Xbox offsets are hints only. Most hooks early-out when F is within 0.0001 of 1 (identity), which keeps 60 FPS bit-exact.

### 0. Global time model and helpers

#### 0.1 Frame limiter replacement (sub_56FC0) and delta-time globals
- Original: per-frame limiter. Computes how many reference frames elapsed (rounded to whole microseconds), clamps to [1, maxFrames], writes deltaTime (float seconds, = frames/refRate), then calls the "frame step" updater (sub_258340: writes frame step count, scaled dt etc.) and the running-total updater (sub_258680: frame counters, unpaused counters, play time).
- Globals they use: refRate (ref fps, int), frameRateDivisor (1 normally, 2 in events), maxFrames (cap on how many ref-frames one real frame may count), deltaTime (rounded), unroundedDeltaTime (pre-round), fixedDeltaTime (debug; >0 forces dt, "no writers at runtime"), frameStep (int frames passed), timeScale, scaledDeltaTime, frameCount, unpausedFrameCount(2), playTime. Flags: yieldEachFrame (loading/cutscene: call NtYieldExecution), frameLimiterOff (loading/FMV: skip wait), frameClockReset (set at boot, cleared first frame: resets last-time), pauseFrameCounters (pause), movieManager (FMV: call the vblank wait).
- Fixed (locked FPS) mode:
  - Keep an absolute start time. Frame index of a time = floor(t * refRate) computed in whole seconds plus remainder to avoid overflow. Start time of frame n = ceil(n / refRate) ns.
  - Wait target = start + time of (frame-index-of-last-frame + divisor). Wait is skipped if limiter-off flag or divisor<=0. Wait via their frame pacer: sleep then spin.
  - After waking: frames = frameIndex(now) - lastFrame; deltaTime = frames/refRate; unrounded = real elapsed since lastTime; lastTime = now. This removes the original's microsecond rounding drift (60 FPS original = 60.0024, 240 = 240.038, 960 = 960.615).
- Unlocked mode (no wait, flag set by a mid-asm hook): elapsed = real seconds since last frame, clamp to [1e-6, maxElapsed] where maxElapsed = 1.0 s during events else 0.05 s. refRate := round(1/clamped) (>=1), maxFrames := ceil(refRate*maxElapsed) (>=1); store last measured rate (g_unlockedRefRate), write refRate and maxFrames back into game globals; frames = 1; deltaTime = clamped; unrounded = raw elapsed. I.e. every frame is "one frame of whatever the current rate is", because the game's timers advance by whole frames.
- Common tail: if fixedDeltaTime>0 override dt. Clamp deltaTime to [1/refRate, maxFrames/refRate] (skipped in unlocked mode unless fixedDeltaTime used, since the rounded rate would nudge dt). Store deltaTime/unrounded, push (frames, deltaTime), call sub_258340 with ecx = frameStep global, then sub_258680 with ecx = frameCount global and pause flag as arg.
- Pattern: other (core). Find: the function that waits on QueryPerformanceCounter/vblank, computes dt, then calls the two counter updaters; strings none. On GC the equivalent is whatever computes dt from VI/OS time. Their caution: "kinda ripped from Ghidra with cleanup".

#### 0.2 SetFramerateMidAsmHook (refRate / maxFrames policy), mid-asm in the limiter setup
- During events with cutscene framerate not unlocked: force locked mode (unlocked=false) and return untouched.
- Else: refRate = target fps (or last measured rate in unlocked mode). During events refRate = max(refRate, 30) ("events must not go below 30 or they desync"). Write refRate.
- Outside events: maxFrames = max(ceil(refRate * 0.050), 1). Reason (Sewer's note): the original lets time slip when fps falls under fpsMax/3 (a 50 ms frame cap, "20 FPS") so physics does not tunnel; with a huge target fps the integer divisor would make the game slow-mo, so recompute the cap as the number of frames in 50 ms. Inside events: maxFrames = max(refRate,1) (matching event audio sync of 1 second).
- Pattern: other. Find: where the game sets refRate and its "max frames" divisor (originally 3 at 60 fps -> 20 fps floor). Constant: 50 ms cap.

#### 0.3 Event tracking
- TPjsEventActionManager::Exec (sub_228740) wrapper sets g_isEventPlaying=true and g_isStartingEventAudio (thread-local, only during the call; distinct from isEventPlaying because of in-stage event invocation and chained event audio like Last Story). TPjsEventActionManager destructor (sub_2293E0) clears isEventPlaying. Comment: do NOT use isEventPlaying for audio detection. The event manager also sets frameRateDivisor=2 for events.
- GetInternalFrameRate(): refRate/divisor if divisor>0 and result within [1, unlocked fps max], else 0 meaning "no internal request".

#### 0.4 SFX timer patch (SFXTimingMidAsmHook, FreeSFXMap hook)
- Object: SFX map entries (esi = entry, 16-bit remaining timer at +0xC).
- Symptom: sounds cut off too early above 60 FPS.
- Root cause: SFX duration timer is an integer decremented once per frame (hardcoded 60 FPS).
- Fix: hook just after the game's decrement. Restore the original value (+1). Keep a per-entry float accumulator in a map keyed by entry pointer: acc += dt*60. steps = int(acc + 0.5) (the +0.5 is to avoid 0.99996 never passing). steps = min(steps,1) because looping sounds are re-armed to 2 every frame by a task and a faster decrement would stop/restart them and double-play. If steps>0: acc = min(acc - steps, 1.0); timer = max(timer - steps, 0). Store timer. A free hook erases the map entry when the entry is freed.
- Pattern: carry-leftover (capped). Find: SFX entry update that does `timer--` on a u16 with a stop at 0.

#### 0.5 FPS config callback
- Rounds Config::FPS to 2 decimals, <0.01 -> 0 (unlimited), clamp to max unlocked fps. UI/config only.

#### 0.6 ScaleFirstOrderLag(before, after, retention, frames) helper (vehicles)
- Models n repeated steps of `x = after_k` where the original does `x_next = x*r + target*(1-r)`-style exponential smoothing: span = 1 - r; if span<=0 return `after`. Result = before - (before - after) * (1 - r^frames) / span. Here `after` is the value the single original step produced, so (before-after) is one step's change; the geometric sum of F steps of that change (decaying by r each) is (1-r^F)/(1-r). For F=1 it returns `after`.
- RescaleVehicleTurn(yawAddr, yawBefore, dirAddr, turn, cap, frames): if yaw unchanged, return. Recompute the original turn: delta = wrap(atan2(-dir.x, -dir.z) - yawBefore); t = min(turn/|delta|, 1 - (1-cap)^F); yaw = wrap(yawBefore + delta*t). So only the proportional-turn cap is per-frame; the turn-rate limit already uses dt.

---
### 1. Fixes

#### 1. Ground stick / launch interaction (CollisionGroundStickMidAsmHook)
- Object: player collision ground ray (esi -> holder of player ptr). 
- Symptom: above 60 FPS the "ray to ground stick" breaks on even small slopes; also spots that break at 60 (Space Gadget CC orb area, Death Ruins spawn).
- Fix: mid-asm that returns true (skip to the second sweep) only if: not currently inside the jump board launch replay pass (g_launchCollisionPass==0, see part 2), player pointer non-null, player behavior flags (+0xA8) do not have bit 0x4 (Grounded), and the ray DID hit (a stack byte flag). Scoped to non-grounded so camera/snap/slide-Y-push issues of the "61FPS+" patch are avoided.
- Pattern: other. Find: ground ray cast in the player collision update followed by a second sweep. Marked as "collision patch globally".

#### 2. FootingMovableLinear (platforms, "mode linear")
- Symptom: linear-moving platforms run too fast above 60.
- Root cause: phase angle (degrees, +0x7C) advances by speed (degrees/frame, +0x54), no dt.
- Fix: after the original per-frame add, add speed*(F-1) to phase.
- Pattern: dt-scale (extra-frames add).

#### 3. UpDownReel (pulley)
Three/four hooks:
- Raise/Lower: pre-bias the reel value (+0x58) by sign * step(0.18, a constant) * (F-1) (+ for raise, - for lower) before the original adds/subtracts one step.
- Angle (+0x7C): the original eases angle toward 0 by a constant rate per frame; extra = rate * (F-1); if extra >= |angle| set 0 else angle -= sign(angle)*extra.
- Hang turn: multiply the turn rate (in eax, float) by F, skipped if F ~ 1 or F<=0.
- Pattern: dt-scale.

#### 4. StretchGrass (Death Ruins launcher, "SetStretchGrass")
Only mode 5 used dt. Others patched:
- Mode 2 (charge): original has acc(+0x4C) += step (0.005) and value(+0x48) += acc. Hook: acc += step*F, store acc - step (so original's own add nets F steps), and value += acc*(F-1). Effect: launch strength no longer grows with fps.
- Mode 3 (recover): value(+0x48) -= 0.42*(F-1).
- Mode 4 (decay): game multiplies value by -0.93 each frame. Keep per-object carry += F; steps = min(int(carry),128); carry -= steps. If steps==0 divide value by the factor (undoing the game's own multiply); else multiply by factor (steps-1) more times. When |value*factor|<0.001 erase carry entry.
- Pattern: dt-scale (modes 2,3) and carry-leftover (mode 4).

#### 5. Rocket (sub_1AD1A0)
- Symptom: rocket way too fast above 60.
- Root cause: part of velocity logic uses raw velocity (+0x4C,+0x50,+0x54).
- Fix: wrap the function: multiply the three velocity components by F, call original, restore original values.
- Pattern: dt-scale (temporary scale).

#### 6. ElecSearchLight pull (Digital Circuit light that yeets Shadow)
- Symptom: launch speed too high above 60.
- Fix: scale the launch vector scale (float on the stack at esp, stored just before the launch) by F and clamp to 45.
- Pattern: dt-scale with cap.

#### 7. ScatteringRing physics (rings dropped when hit)
- Symptom: too fast above 60 FPS.
- Root cause: per-frame integration: X/Z velocity multiplied by damp 0.984373 each frame; Y velocity multiplied by (1-0.1) with gravity 0.12138 added; positions integrated with velocity (velocity at +0x54..0x5C, position at +0x48..0x50).
- Fix (closed-form for F frames, then feed the original one step so that it ends at the right place): damp d, drag q=0.9, gravity g.
  - dN = d^F, qN = q^F, terminal = -g/(1-q).
  - endVelXZ = v*dN; endVelY = terminal + (vY - terminal)*qN.
  - distance: sumDamp = d(1-dN)/(1-d); sumDrag = q(1-qN)/(1-q); travelXZ = v*sumDamp; travelY = terminal*F + (vY-terminal)*sumDrag.
  - Write velocity so the original step lands on end velocity: vXZ' = endVelXZ/d; vY' = (endVelY + g)/q.
  - Add to position (travel - endVel) per axis, since the original step will itself add endVel.
  - Skip if F ~ 1.
- Pattern: dt-scale (analytic, geometric series).  Find: ring physics with constants 0.98437, 0.1, 0.12138.

#### 8. ScatteringRing spin
- Rotation angle (+0x64) += pi/180*1.5 * (F-1). Pattern: dt-scale.

#### 9. Regular ring spin/sway
- Symptom: rings wobble/spin too fast.
- Root cause: per ring entry a phase (+0x24) += 0.01 per frame and the ring offset (+0x1C) += 0.05*sin(phase).
- Fix with extra = F-1, step s=0.01, amp A=0.05: add to offset the sum of A*sin(phase + k*s) for k=1..extra in closed form = A * sin(extra*s/2)/sin(s/2) * sin(phase + (extra+1)*s/2); and phase += s*extra. Entry = array + offset (eax+ebp). Skip if F~1. 
- Pattern: dt-scale (trig series sum; fractional F handled by the same formula).

#### 10. Weight (big black block, states 5 and 6)
- Rise: Y-like coord (+0x8 of the transform) += 1.0*(F-1). Fall: -= 2.5*(F-1). Skip if F~1.
- Pattern: dt-scale.

#### 11. ContainerBehavior fall (item boxes: Normal, Hard, Unbreak, Crate, Weapon, Secret)
- Y (+0x20) += fallSpeed(+0x34, per-frame) * (F-1). Pattern: dt-scale.

#### 12. Shadow / NPC damage blink (ShadowMotion::Exec, partner motion)
- Symptom: invulnerability blink at wrong rate above 60.
- Root cause: parity of an integer frame counter (+0x3EC) while the remaining-invuln time (float at player +0x27C, delta-timed) is correct.
- Fix: replace parity by parity of int(timeLeft*60 + 0.25). If timeLeft is invalid (<=0, non-finite, >60) fall back to the original counter parity. Same helper for the partner (player ptr at +0x458 vs +0x58 in Shadow).
- Pattern: slice-timer derived directly from a dt timer (1/60 s slices, 0.25 rounding bias).

#### 13. EggFleet / PowerDeviceNaked hit blink (Sky Troops)
- Symptom: blink doesn't stop/alternate correctly.
- Root cause: remaining-hit timer is dt-correct, but the original blink check (& 2 of the unpaused frame counter) is frame based.
- Fix: phase_off = (int(timeLeft*60+0.25) & 2) == 0; invalid timer falls back to (unpausedFrameCount2 & 2)==0. Gives 2 slices on, 2 slices off of 1/60 s. Timer locations: EggFleet +0x44 (fleet; for fleet parts it reads the parent pointer at +4), PowerDeviceNaked +0x60. Hooks return true=regular draw, false=hit tint at three branch sites (sub_1C50A0, part draw, sub_1C78E0).
- Pattern: slice-timer.

#### 14. Enemy / boss / object blink (EnemyBlinkPatch)
- Symptom: hit flash, death blink, bomb flash, soldier flashes, material blink on bosses look random above 60 fps.
- Root cause: they test the low bit of a frame counter (all-frames counter or unpaused counter 2).
- Fix, "slice timer": a struct per counter source keeps lastFrame, slices (float), phase, stepped. Once per distinct counter value (so repeated calls in the same frame are consistent): slices += F if 0<F<0.99 else 1 (60 or lower still steps one slice per frame); wrap slices at 4 (while>=4: -=4); newPhase = int(slices+0.25)&3; stepped = newPhase != oldPhase. Return int(slices+0.25); callers use bit 0 as odd flag. Two instances: one keyed on frameCount (all frames, incl. pause) and one on unpausedFrameCount2.
- Hook users: MaterialBlinkTask::Exec (boss hit tint, all-frames), enemy hit blink (hide on odd, unpaused), enemy death blink, BkLarva death blink, ThrowBombTask (full/dim red), shared model draw classes (3 sites), soldier flash parity (writes 0/1 into ecx or edx), and a generic "skip blink toggle" hook that uses a third instance and returns "skip" when the phase did not step this frame.
- Their remark: wrap at 4 is "lowkey arbitrary, but should be good enough"; suggest renaming the struct.
- Pattern: slice-timer (carry-leftover). Find: any `if (frameCounter & 1)` in model/enemy draw, damage flash, death flash.

#### 15. Vehicle shared fixed step (AirSaucer, Walker / WalkerCannon)
- Symptom: collide-and-slide (sub_24E780) is tuned for 60 FPS steps; vehicles misbehave above 60.
- Fix concept: keep the solver and mover at a fixed 60 Hz simulation and interpolate the displayed position at higher FPS.
- TryStepVehicle(vehicle, colliderOffset, solver, mover, afterMove, jumped), replaces the solver call (dt as stack arg):
  1. Bail (run original) if collider null, dt<=0, F<=0, or (F~1 and not mid-step). "midStep" = a record exists with same collider, (phase<0.9999 or groundHidden) and last update within 1.5 * frame time (App::s_time / s_deltaTime) of now.
  2. stepDt = dt/F. Read position (+4,+8,+0xC) and acceleration (+0x28..+0x30).
  3. Reset record if collider changed (mount, rail) or stale (>1.5 frames): record from=to=shown=pos. Remember last time.
  4. Shift from/to by (current pos - last shown pos) to preserve external motion (platforms, bumps).
  5. phase += F. While phase>1.0001 and steps<4: if step>0 call afterMove (damp) with stepDt; restore the hidden real ground flags if set; write 'to' as position (+4 and +0x64) and saved accel; call solver then mover with stepDt; from=to; to=new position; accumulate edge flags (collider+0x1C bits 1 and 2: kill surface, landed; bit 0 grounded); phase -= 1.
  6. phase = min(phase,1). Flags = current non-edge flags | accumulated edge flags.
  7. If no step ran this frame and a jump was requested and not already hidden: save real flags, clear grounded bit (bit 0) so the jump isn't skipped, mark hidden (restored next stepped frame).
  8. shown = from + (to-from)*phase written to position. Record stepped vehicle/step count; pop 8 bytes of args and return true (original solver skipped).
- SkipIfStepped: for the mover/other calls for the same vehicle, pop args and skip original when it was already stepped this frame.
- CallWithDt: copies context, sets ecx=vehicle, pushes dt, calls function.
- Pattern: fixed-60Hz-step with render interpolation (+ carry-leftover in phase). Max 4 substeps. Their individual function addresses (AirSaucer solver 0x24B6F0, mover 0x24B5F0, idle damp 0x24C190, grip/lean anchor 0x24BEC0, heading turn 0x24C600, tilt slerps 0x24B880/0x24BA90) continue past line 1130 (part 2); only the mover wrapper begins within this range.
- Uncertainty: none stated; hidden ground flag trick is pragmatic.

### Count and gameplay-feel ranking
Documented 15 numbered fixes (several with multiple hooks) plus helpers 0.1-0.6.
Top 5 for feel: (0.1/0.2) frame limiter + dt/maxFrames policy; (15) vehicle fixed step; (7) scattering rings; (14) enemy/boss blink slice timer; (12) Shadow damage blink (also 1 ground stick).

## Part 2: vehicles, the library menu, launches, trails (source lines 1130-2470)

Source: ShadowRecomp/patches/fps_patches.cpp (Xbox build, x86, addresses like sub_24B6F0). Helper code from lines 929-1122 was read because the fixes depend on it.

Conventions used in the source:
- `frames = g_deltaTime * 60`. It is "how many 60 Hz frames this real frame is worth". At 120 FPS it is 0.5, at 240 it is 0.25, at 30 it is 2.
- `NeedsFpsRescale(frames)` is `frames > 0 && |frames - 1| >= 0.0001`. Every fix is a strict no-op at exactly 60 FPS, so the 60 FPS behaviour is bit-identical to the original.
- Hooks are either function overrides (call original, optionally pre/post-process) or "mid-asm hooks" that sit in the middle of a function and edit registers or stack slots. For a GameCube port you would do the same thing at the equivalent instruction.
- Offsets are Xbox-only hints. Look the fields up by meaning.

Recurring math, so entries can refer to it:
- **Compound a per-frame exponential blend over N frames.** If the game does `x += (target - x) * a` once per frame, then over `frames` frames the equivalent single blend is `a' = 1 - (1 - a)^frames`.
- **First-order lag rescale** (`ScaleFirstOrderLag(before, after, retention, frames)`). The game ran one step of `x' = target + (x - target) * r`, with `r` the retention, and `after` is that result. Recover the N-frame result without knowing the target: `before - (before - after) * (1 - r^frames) / (1 - r)`. Returns `after` if `1 - r <= 0`.
- **Extra fixed per-frame increment.** The game adds a fixed amount `s` per call. Add `s * (frames - 1)` after the original runs. This is negative above 60 FPS, so the net add is `s * frames`.
- **Carry / remainder counter** (for integer per-frame counters and gates). `carry += frames; n = (int)(carry + 0.0001); carry -= n;` Then run `n` times (or run if `n >= 1`). Keep one carry per object instance (hash map keyed by the object address, with stale entries erased on destroy or cleared past a size cap).

---

### A. Shared infrastructure: fixed-60 Hz step for vehicles (AirSaucer and Walker)

#### A1. Vehicle fixed step ("TryStepVehicle")
- **Object / system:** Shared by Vehicle::AirSaucer (the flying saucer) and Vehicle::Walker / WalkerCannon (GUN Jump Mechs). They are the same class. Hooks the vehicle's *solver* (per-frame physics function taking dt) and *mover* (applies the result and runs the collide-and-slide).
- **Symptom above 60 FPS:** Collide-and-slide (`sub_24E780`) is tuned for 60 Hz sized steps. The vehicles fly or walk wrongly, with sticky, slow or jittery movement. Jump behaviour breaks and grounded or landing flags are misread.
- **Root cause:** The solver and collision code has per-frame constants and sweep-length assumptions baked in, and dt is not enough to correct them. The simplest correct fix is to run the exact 60 Hz physics and show an interpolated result.
- **Mechanism, step by step:**
  1. Per-vehicle record (hash map keyed by vehicle address): collider pointer, last wall time, `phase` (double, starts at 1.0), `from`/`to` positions (the two most recent simulated 60 Hz positions), `shownPos` (what was last written into the vehicle), a `groundHidden` bool and `realFlags`.
  2. Preconditions to take over: collider pointer non-null, `dt > 0`, `frames > 0`. Otherwise run the original.
  3. Run the stepper if `NeedsFpsRescale(frames)`, OR if the vehicle is mid-step (`phase < 0.9999` or `groundHidden`, and the record was touched within 1.5 frame durations). This second case drains the phase cleanly if the frame rate dips to exactly 60 mid-sequence, so there is no snap.
  4. Sub-step dt is `stepDt = dt / frames`. This equals 1/60 when dt is the real frame time. It also respects any time scale that dt carries.
  5. Reset the record (set from = to = shown = current position, phase = 1.0) if the collider pointer changed (mount, touching a rail) or wall time since last touch was over 1.5 frame durations (pause, long hitch). If it resets while `groundHidden`, first restore the real collider flags.
  6. External displacement: `moved = currentPos - shownPos`. Add it to both `from` and `to`. This keeps platforms, bumps and teleports from being overwritten by the interpolated position.
  7. Snapshot the vehicle's accel vector (+0x28..0x30), which the solver or mover consume or overwrite.
  8. `phase += frames`. While `phase > 1.0001` and fewer than 4 sub-steps (hard cap, protects very low FPS):
     - If this is not the first sub-step and an "after-move" function exists (the saucer idle damp), call it with `stepDt`. The game's own call of it after the mover handles the final sub-step.
     - If `groundHidden`, restore the real flags and clear the hide.
     - Force the vehicle's position and previous position (+0x4 and +0x64) to `to`, and restore the accel snapshot. This puts the vehicle exactly where the last simulated step ended.
     - Call solver then mover with `stepDt` (calls are made on a copied context with the dt pushed).
     - `from = to; to = vehicle position after the mover`. OR the collider's edge flags into an accumulator. `phase -= 1`.
  9. Clamp `phase` to at most 1.0.
  10. Collider flags word (+0x1C): bit0 grounded, bit1 kill surface, bit2 landed. Final flags = current non-edge bits | OR of edge bits (bits 1, 2) seen on any sub-step. So a landing or kill that happened in a sub-step is not lost.
  11. **Ground hiding:** if zero sub-steps ran this frame AND the caller says "jumped" (see saucer/walker) AND not already hidden, save the flags, set `groundHidden`, and clear the grounded bit in what the game sees. This prevents the game's jump logic from seeing "grounded" on frames with no physics and skipping or re-triggering the jump. The real flags come back before the next sub-step.
  12. Display position: `shown = lerp(from, to, phase)` using `phase` after subtracting. Write it to the vehicle (+0x4..0xC) and store it as `shownPos`.
  13. Publish `g_steppedVehicle = vehicle` and `g_steppedVehicleSteps = steps`. Pop the solver's two stack words (arg + return) and return true, so the original solver is not run again.
  14. The *mover* hook, when called by the game afterwards, checks `ecx == g_steppedVehicle` and returns immediately, since the mover already ran inside the sub-steps.
- **Worked example, 120 FPS (frames 0.5):** Fresh record, phase 1.0. Frame 1: phase 1.5, one sub-step, phase 0.5, shown = midway between from and to. Frame 2: phase 1.0, not > 1.0001, so zero sub-steps, shown = to. Frame 3: phase 1.5, one step, and so on. At 240 FPS it steps once every 4 frames and interpolates in between. At 30 FPS it steps twice per frame.
- **Pattern:** fixed-60Hz-step with interpolation (and a carry: `phase` is the remainder accumulator).
- **How to find it:** Find the vehicle update that calls a "solver(dt)" then a "mover(dt)" and a collide-and-slide routine. Characteristic constants are the collider flag word with grounded/landed/kill bits, and the vehicle's position at +4 with previous position at +0x64. In the GameCube build, locate the AirSaucer and Walker Exec functions, then the two child calls.
- **Uncertainty / hacks:** Author comments: "handle inconsistent framerate", "collider can change on mount and touching a rail", "hide grounded to prevent jump skip". The 4-step cap and the 1.5-frame timeout are heuristics. The visual lags the simulation by up to one 60 Hz step because it interpolates between two past steps.

---

### B. Vehicle::AirSaucer

All saucer functions are keyed to the saucer object. Offsets: vel at +0x1C, stick at +0x10C, grip at +0x154, anchor at +0x148, collider at +0x9C, state at +0x17C, yaw at +0x11C.

#### B1. Saucer solver, mover and idle damp wiring
- **Object:** Vehicle::AirSaucer. Solver `sub_24B6F0`, mover `sub_24B5F0`, idle damp `sub_24C190`.
- **Symptom:** see A1.
- **Mechanism:**
  - Solver override calls A1 with the saucer's collider at +0x9C, mover = `sub_24B5F0`, and the idle damp as the after-move function only when `state == 0`. State 0 means nobody is on the saucer (neither player nor enemy), so the damp is the only thing running.
  - "Jumped" argument = `(collider flags & 1) != 0 && (state == 3 || state == 4)`. States 3 and 4 are jump-related states.
  - Mover override: skip if A1 already moved this vehicle.
  - Idle damp override: if the vehicle is the stepped one and zero sub-steps ran, skip the damp. So damp runs exactly as many times as sub-steps.
- **Pattern:** fixed-60Hz-step.
- **Find it:** The saucer class vtable or Exec (strings and RTTI "AirSaucer"). The solver is the one that reads the stick and applies grip and accel. The mover applies velocity and calls collide-and-slide. Idle damp is a small function that decays velocity when state is empty.

#### B2. Grip and lean-anchor (sub_24BEC0)
- **Symptom:** Saucer handling, grip and the follow/lean anchor depend on frame rate (too slippery or too tight, floaty lean).
- **Root cause:** Two quantities are smoothed per frame, not per second. Grip chases a speed-dependent target by a fixed 0.1 per frame. The lean anchor chases a goal point by a fixed fraction per frame.
- **Mechanism (replaces the original entirely when not 60 FPS; pops 8 bytes of args):**
  1. `speed = |vel|`. `gripMax = 0.96875`. `target = (1 - speed * 0.01) * gripMax`.
  2. If the stick vector has squared length `>= 0.0001`, then `target = min(target, grip)`. Grip only drops while steering, never rises.
  3. `grip = target + (grip - target) * (1 - 0.1)^frames`, clamped to `[0, gripMax]`. Store.
  4. Floor velocity: the collider's move target (+0x28 of the move object, then +0x98), or a static zero-vector when absent. This is a copy of the game's `sub_23E930`.
  5. `chase60 = clamp(dt / frames * K * (1 - grip), 0, 1)`, where K is a rate constant read from the data section (`0x456784`). `chase = 1 - (1 - chase60)^frames`.
  6. Per axis: `goal = pos + (0, 5, 0 on Y only) + vel / 128`. `anchor = anchor + floorVel * dt`. Then `anchor += (goal - anchor) * chase`.
  7. On Y only: `gap = goal - anchor`. Then `anchor += gap - clamp(gap, -2, 2)`. This keeps the vertical lag within 2 units.
- **Pattern:** dt-scale with compounding of per-frame exponential (dt-scale of the floor velocity, compound of the chase).
- **Find it:** A saucer function that takes dt and computes a vector sqrt of velocity and a `1 - speed*0.01` grip. Constants 0.96875, 0.01, 0.0001, 0.1, 5.0, 2.0, 128.
- **Uncertainty:** The author reimplemented the function. The K constant is only an address and is not documented; verify its value in your build (likely a 60-ish rate).

#### B3. Heading turn (sub_24C600) and RescaleVehicleTurn
- **Shared by saucer and walker.**
- **Symptom:** Turn speed varies with FPS.
- **Root cause:** The original turns the yaw by a fraction of the remaining angle, `t = min(turnAmount / |delta|, cap)`, where `cap` (a fraction) is a per-frame constant.
- **Mechanism:** Remember yaw before. Run the original. If the yaw changed, recompute:
  - `delta = wrap(atan2(-dir.x, -dir.z) - yawBefore)` (dir = desired heading vector, ex: ESP arg or walker +0xF8).
  - `t = min(turn / |delta|, 1 - (1 - cap)^frames)`.
  - `yaw = wrap(yawBefore + delta * t)`.
  - Saucer: `turn = (1 - grip) * dt * 250 * pi/180`, `cap = 0.1`. Walker: `turn = dt * 125 * pi/180`, `cap = 0.5`. The max-turn-rate part already scales with dt in the original, so only the cap fraction needed compounding. They recompute turn anyway for safety.
- **Pattern:** dt-scale / compound.
- **Find it:** The function that sets yaw via atan2 of the negated direction and clamps with a constant fraction. Constants 250, 125, 0.1, 0.5 (degrees per second).

#### B4. Tilt slerps (sub_24B880 lean, sub_24BA90 align-to-floor)
- **Symptom:** Saucer body lean and floor alignment follow too slowly or too quickly with FPS.
- **Root cause:** The original does a slerp with `t = min(dt * rate, 1)` per call. The subtlety is that this is a per-frame lerp that is not frame-rate independent (a proper dt form would be exponential).
- **Mechanism:** Before the original runs, rewrite its dt argument on the stack: `step = min(dt / frames * rate, 1)`, then `dt' = (1 - (1 - step)^frames) / rate`. The original then computes `dt' * rate`, giving the N-frame compound. Rates: lean 15.0, align 7.5. Skip if rate <= 0 or 60 FPS.
- **Pattern:** dt-scale (arg substitution) with compounding.
- **Find it:** Two slerp helper functions called per frame by the saucer's update, each with a rate constant (15 and 7.5).

---

### C. Vehicle::Walker / WalkerCannon (GUN Jump Mechs)

#### C1. Walker solver, mover
- Solver `sub_2549F0`, mover `sub_2546B0`. Uses A1 with collider at +0xA8, no after-move function, and `jumped = false` (the jump already clears the grounded flag by itself).
- Mover skip: same as saucer.
- **Pattern:** fixed-60Hz-step.

#### C2. Walker heading turn (sub_255780)
- Same as B3 with walker constants (125 deg/s, cap 0.5). The function also steers the ground drive, which is why it matters for movement direction.

---

### D. Vehicle TVehicleCar (CarType vehicles)

Covers the jeep ("Norael" / Normal Car), Open Car (sports car), Armed Car (armoured GUN car), and Bike (GUN motorcycle). The author calls it a mess. The car already uses real gravity and steps, but has many "almost dt" per-frame smoothings. Per-vehicle config table at `0x51CCD8 + 0xAC * index` (index at car+0x25C or +604).

Car layout hints: front axle record at +184 (0xB8), rear at +236 (0xEC); each axle has position at +0, velocity at +12, grounded flag at +0x24 (bit0), previous pos at car+468 (front) / +520 (rear), "drift stored position" at axle +0x11C.

#### D1. Input low-pass (sub_243BB0)
- **Symptom:** Steering, accelerate, reverse and brake inputs ramp too fast or slow with FPS (steering response, throttle lag).
- **Root cause:** Four analog values (steer at +0x58, accel +0x64, reverse +0x68, brake +0x6C) are smoothed with a per-frame first-order filter `x' = (x*k + target)/(k+1)`, retention `r = k/(k+1)`. `k` depends on whether the input is "held" or "released". Per-input byte flags (accel +0x5E, reverse +0x5F, brake +0x60) say which. Steering has no flag and always uses the held K.
- **Mechanism:** Save the four values before. Run the original. Then for each channel pick the K (held vs released, constants read from the data table `0x51CC1C..0x51CC34`), skip if `K <= 0`, and apply the first-order lag rescale to the new value with `r = k/(k+1)`. Skip entirely at 60 FPS.
- **Pattern:** dt-scale via exponential compound (before/after reconstruction; no target needed).
- **Find it:** The car input update that converts pad axes/triggers into steer/accel/brake values with a held/released K pair stored in a small table.
- **Uncertainty:** The need to record which K the game used is why they copy the flag bytes.

#### D2. Axle velocity blend ("VehicleRelaxAxleVelocity", tail of sub_246120) plus recorders
- **Symptom:** At high FPS the car can barely move or accelerate (author: "Fix not being able to move at higher fps"), in-air movement is wrong, drift turning differs.
- **Root cause:** After the car's axles are moved, the per-axle velocity is recomputed from measured movement and averaged 50/50 with the previous velocity every frame: `v = (previous + measured) / 2` where `measured = (pos - prevPos)/dt`. At high FPS, `dt` is small and each half-blend re-injects stale velocity too often. Also, in-air drag, per-frame adhesion bias and the drift turn move are applied per call.
- **Recorders (set up data for the tail fix):**
  - `sub_242240` (axle integrator, normal) and `sub_242470` (drift axle integrator, called by the drift turn `sub_247210` for left stick + A drift): before running, copy the accel vector the axle integrator was given into a per-wheel global (wheel 1 if the axle is the rear one, else 0). The drift variant also records `driftMove = axle position - axle stored position (+0x11C)`; the plain variant stores zero.
  - `sub_245FB0` (see D5) records `req = pos - prevPos` (the requested sweep) per wheel before it runs.
- **Mechanism (runs after the original `sub_246120`, per axle, only when dt > 0 and not 60 FPS):**
  - `invDt = 1/dt`, `dt60 = dt / frames`, `retention = 0.5^frames`.
  - Config adhesion value (config + 0x38, about -9) is a flat adhesion bias applied only while the wheel is airborne. An axle is "air" when its grounded bit (axle flag byte, bit0) is clear; then `adhesion = config value`, else 0.
  - Undo the original's blend: `measured = (pos - prevPos) * invDt`; `blended = stored velocity`; `previous = 2*blended - measured`.
  - **Ground branch (adhesion == 0):** `transfer = clamp(dot(measured, previous)/|previous|^2, 0, 1)` (0 if previous is tiny). `blend = (1 - retention) + (retention - 0.5) * transfer`. `v = previous + (measured - previous) * blend`. If `|v|^2` is not tiny: `across = frames - 1`, `along = (0.5*frames - blend - across) * dot(v, driftMove) / |v|^2`, then `v += (driftMove * across + v * along) * invDt`. The idea: the drift turn moves the axle once per call regardless of dt, so add the missing (frames-1) drift moves, and cancel how the blend would have rescaled the along-velocity part.
  - **Air branch (adhesion != 0):** Compute `v0 = previous`. Per axis: `m60 = measured + (v0 * drag - accel) * (dt60 - dt) * 0.5`, with per-axis in-air drag constants 3.75 (x, z) and 0.6667 (y) (these are from `sub_242240`'s in-air drag). `axis = req + (0, adhesion*dt, 0)`, normalized. `dot = dot(v0, axisNorm)`. `v60 = (axisNorm * dot + m60) * 0.5`. New velocity `= v0 + (1 - retention) * 2 * (v60 - v0)`. Comment: "trapezoid fix over a 60 fps frame". If the axis length is tiny, do nothing.
- **Pattern:** other (analytical rescale of a per-frame 50% blend; needs recorded side data).
- **Find it:** The car update function that after moving both axles loops over the two axles and computes `(newPos - oldPos)/dt`, blends with the stored velocity, and uses 0.5. Distinctive constants: 3.75, 0.6667, a config field around -9 (adhesion), and the two-axle layout.
- **Uncertainty:** The author says the car patch is "a bit of a mess". The air and ground algebra is empirical; treat it as a tuned approximation. The recorded globals are shared state across cars, so two cars in one frame could cross-contaminate (vehicle index is just front/rear, not per car).

#### D3. Frame-counted timers (sub_246120 head, sub_2443F0 head)
- **Symptoms:** Damage cooldown (damage flash / invulnerability after a hit) and the car's "drive_dam" motion (motion 5) hold time run too short at high FPS. Motion (MTN) files can reference per-frame timers.
- **Root cause:** Integer counters (car+0x44 damage cooldown; car+0x2A4 motion-5 hold timer) are decremented by 1 per call.
- **Mechanism ("PreStepCarCounter"):** before the original runs, if the counter is positive and not 60 FPS, add `frames` to a per-counter-address carry, `steps = (int)(carry + 0.0001)`, subtract steps from carry, and write `max(counter - steps, 0) + 1`. The `+1` cancels the decrement the original is about to do, so the net decrement is `steps` (0 on most frames above 60 FPS). If the counter is zero or negative or FPS is 60, erase that counter's carry.
- **Pattern:** carry-leftover (a frame counter).
- **Find it:** Search the car update for a counter that is checked `> 0` and decremented by 1, around where the damage function (`sub_2440A0`) is called; and the motion frame function (`sub_2443F0`).

#### D4. Car motion frame ("bounce over surface" animation)
- **Hook:** mid-function in `sub_2443F0`. Not at 60 FPS: `motionFrame (car+0x40) += (config+0x54 / config+0x18 * car+0x23C) * (frames - 1)`.
- **Mechanism:** adds the missing/negative extra of the per-frame increment; at >60 FPS it reduces the next original add, so the net per real frame is `step * frames`.
- **Pattern:** dt-scale (extra fixed per-frame increment).
- **Find it:** The car's motion-frame advance where the animation frame increases by a config-derived per-frame amount, as ratio of two config fields times a car speed-like value.

#### D5. Sweep slide gate and resting pad (sub_245FB0)
- **Symptom:** At high FPS the wheel sweep and "slide" treatment fires too often and the car sticks, or its velocity projection gets rewritten every frame (in the air as well).
- **Root cause:** sub_245FB0 (in-air velocity projection / tail of sweep handling) uses two global float constants tuned for 60 Hz sized movement: a "slide gate" (data address `0x42A18C`, a squared-distance-like threshold) and a resting pad (`0x51CCD4`, a length). With smaller per-frame movement, thresholds are too big relative to movement.
- **Mechanism:** Before the original, with `frames = max(rawFrames, 0.05)` (or 1 if rawFrames <= 0): temporarily write `gate * frames^2` and `pad * frames` to those global constants. Run the original. Restore both. Also before the original, save the wheel's velocity; after, if the correction applies (dt > 0, not 60 FPS) and the wheel is not grounded (axle flag bit0 clear), write the saved velocity back, because D2 handles in-air velocity itself.
- **Pattern:** dt-scale of constants (temporary global patch) plus other.
- **Find it:** Two shared read-only floats compared with per-wheel move lengths in the car's sweep/slide handler. Squared-vs-linear is why gate scales with frames squared.
- **Hack flag:** modifying global constants in memory is hacky; not thread safe.

#### D6. Angle smoothing (sub_1D860, called from sub_246120)
- **Symptoms:** Car body roll/pitch and roll damper settle too fast at high FPS.
- **Root cause:** `sub_1D860` smooths an angle with two stack arguments: `rate` (at call-esp + 8) and `weight` (at +0xC), as a per-frame weighted blend `(value*rate + target*weight)/(rate+weight)` type of filter. (Their code only uses ratios, so the exact form is: one parameter is mixing fraction `rate/(rate+weight)`, the other `weight/(rate+weight)`.)
- **Mechanism ("SetCarAngleBlend", edits the `rate` argument in place; skip if 60 FPS or rate/weight <= 0):**
  - Roll damper (mode `valueFirst = false`): `keep = (rate/(rate+weight))^frames; blend = min(keep, 0.99999)`.
  - Body pitch/roll chase (`valueFirst = true`): `keep = (weight/(rate+weight))^frames; blend = min(1 - keep, 0.99999)`.
  - New `rate = weight * blend / (1 - blend)`. This makes the function's own mixing fraction equal the N-frame compound.
  - Two mid-hooks: one at the roll damper call, one at the pitch/roll chase call.
- **Pattern:** dt-scale via compounded exponential smoothing.
- **Find it:** Calls to a small angle-lerp helper with two floats (rate, weight) from the car's main update.

#### D7. Steer lean (mid-hook)
- **Symptom:** The body leans into corners with FPS-dependent strength.
- **Root cause:** The lean target is derived from "yaw change since last frame" which is smaller per real frame above 60 FPS.
- **Mechanism:** Not at 60 FPS: `turn = wrap(car yaw (+0x18) - previous yaw (+0x134)) / frames`. `target = config(+0x98) * turn`. If `car+0x18C > 0` add `config(+0x9C) * car(+0x84)`. Clamp to `+-config(+0xA0)`. Write over the stack argument that the angle smoother will use as the target. Then apply the roll-damper blend fix (D6, valueFirst false).
- **Pattern:** dt-scale (normalise per-frame delta to 60 Hz units) plus compound.

---

### E. Library menu (scroll speed)

#### E1. Held-direction repeat (sub_2A7560)
- **Object:** The Library (extras/library) menu list. Function `sub_2A7560` is the generic "is this key down or repeating" query, `ecx = pad manager`, args: slot (0-3 or 6/7 = "the pad this menu owns" or other values = any pad) and a mask. Returns bool in `al`.
- **Symptom:** Holding the stick scrolls through 4-5 items per tap instead of 1. Holding scrolls way too quickly.
- **Root cause:** Per pad slot there are 8 direction groups of held-frame counters, `uint16` each, at `pad + 16*slot + 2*group`. They are incremented once per real frame. The repeat logic compares them against delay/interval thresholds, so at 240 FPS the first-repeat delay and the repeat rate are 4x faster in real time. Also the function is called more than once per frame, so counter-change detection is needed to know whether a new frame passed.
- **Mechanism (per call, not at 60 FPS special-cased, always runs; guarded by a reentrancy flag, `pad == 0` passthrough):**
  1. `MenuHoldUpdate`: for each slot (4) and group (8): `raw = counter`. Per-group state: `lastRaw`, virtual `ticks`, `carry`, `ticked`. If `raw == lastRaw` skip (same frame). If `raw == 0` reset state. Otherwise `rawSteps = raw - lastRaw`; `ticked = false`. If a new hold (`lastRaw == 0` or `raw < lastRaw`): `ticks = 1; carry = 0; ticked = (raw == 1); rawSteps = raw - 1` (keep the game's own first frame of 1). Set `lastRaw = raw`. If `rawSteps > 1` (missed frames between calls): `carry += frames * (rawSteps - 1)`, convert to whole steps. If `rawSteps > 0`: `carry += frames`, take whole steps, and if any, `ticked = true`; add them. `ticks = min(ticks + steps, 0x7000)`.
  2. Overwrite the game's raw counters with the virtual `ticks`, call the original (so all delay/interval math runs on a 60 Hz clock), then restore `lastRaw` as the counters.
  3. If the original returned false, return. If it returned true, resolve slot (6 -> `pad+404`, 7 -> `pad+408`). Accept it if the key is **pressed this frame** (edge state at `pad + 40*slot + 88`, tested with a 3-way mask match: all bits of mask, or all bits in the 0x3C0 direction nibble, or all bits in the 0x3C00000... specifically 0x3C0000 nibble) **or** any direction group whose mask (table of 8 uint32 masks, at the data address `0x47B698`) overlaps the queried mask ticked on this real frame. For slot >= 4, test all four pads. Otherwise force `al = 0`, which is "repeat from a duplicated frame, give up".
- **Pattern:** hold-key (virtual 60 Hz hold counter with carry) combined with a "ticked" edge gate.
- **Find it:** The pad query function used by menus with a held counter array of 8 uint16 per pad and a 16-byte stride; the 8-entry mask table. Menu code calls it with direction masks.
- **Uncertainty:** Author: "hacky but whatever work" regarding the reentrancy flag; the 0x7000 cap avoids uint16 wrap.

#### E2. Library route-list cursor cooldown (sub_291B70)
- **Symptom:** Cursor still moves too fast above 60 FPS even after E1.
- **Root cause:** After a single cursor move, the route list ignores input for 2 frames using its own counter at `list+284`, incremented without dt.
- **Mechanism:** Record the field before, run the original. If the value increased (cursor moved this call) and `0 < frames < 1` (only above 60 FPS): `scaled = (uint32)(after / frames + 0.5)`, clamp to 64, store back. So a 2 frame cooldown becomes `2/frames` real frames.
- **Pattern:** dt-scale of a frame-counted cooldown (set-time scaling, not a carry).
- **Find it:** The route list/ "Library route" menu update that moves a cursor index and writes a small counter right after.
- **Uncertainty:** I cannot tell from the code whether +284 is the cooldown itself or an index; the author's logic only assumes it increases on a move and reads as "frames to wait".

---

### F. Launch / Jump Board / Catapult / Dark Spin collision grace

This is the "grace" mechanism. State machine and rationale below.

#### F1. CharaColliMove footing flag (sub_80BA0)
- **Object:** CharaColliMove, the player's moving-surface (footing) handling, used by catapults (Prison Island launchers), moving platforms, etc.
- **Mechanism:** Sets a global `g_footingCarry = true` for the duration of the original call, then false. It does nothing else; it just tells the collision hook (F2) that a "moving surface carry" call is on the stack.
- **Pattern:** other (context flag).

#### F2. Launch collision grace (jump board, dash-launchers, catapult)
- **Objects:** Player::Behavior::Launch (Start `sub_76DE0`, Exec `sub_76B20`, End `sub_76A70`) and the collision routine `sub_84E50`. Launch is invoked via `sub_87760` (a counter). Jump boards, springs/launchers in space gadgets and Prison Island catapults funnel into it.
- **Symptom above 60 FPS:** Shadow gets a "bump" when launched. Death can occur in some places (space gadget).
- **Root cause:** Player collision is a swept move from the previous position to the intended destination. When launched, the first frame's sweep starts inside the ramp. At 60 FPS the sweep (about 1/60 s of motion) is long enough to cross the ramp surface cleanly. Above 60 FPS the sweep is shorter, stays inside the ramp geometry, and the collision pushes Shadow around.
- **Mechanism:** hold off collision for the first 1/60 s of the launch, then run it once, as 60 FPS would, from the correct start point.
  - Per-player slot (4 slots) `LaunchGrace`: `player`, `elapsed`, `sweepOrigin[3]`, `originValid`, `replayed`, `skipped`, `selfTimed`.
  - **Start:** find the player's slot, or a free one, else slot 0 as a failsafe. Reset everything: player = this player, elapsed = 0, originValid = false, replayed = false, skipped = 0, selfTimed = false.
  - **Exec (every frame, before the original):** `elapsed += dt` (the dt argument), or if dt <= 0 set `elapsed = 1/60` (so grace is spent immediately). **Why the grace must account for this:** the original Launch::Exec adds the time and updates movement before it reaches collision in the same call. At exactly 60 FPS, dt = 1/60, so by the time the collision routine runs `elapsed` already equals the grace period and nothing is ever skipped: identical to the original. If the hook did not add dt itself first, the grace check inside collision would read "elapsed = 0" even at 60 FPS and skip the first collision, changing 60 FPS behaviour (and the replay would corrupt it). Adding the dt in Exec, ahead of the original, makes the timing correct and keeps 60 FPS untouched. At 120 FPS the first frame's elapsed is 1/120, still under 1/60, so grace is active.
  - **Collision hook `sub_84E50`** (arg 1 = pointer to a struct whose first dword is the player): find the player's grace. If `selfTimed` (Dark Spin), it adds the player's own dt (`*(*(player+0x60))`, falling back to 1/60 if not positive) to `elapsed`, and drops the grace if elapsed is already >= period with nothing skipped (the 60 FPS case). If a grace remains:
    1. On the first call, capture `sweepOrigin` from the player's collision "old position/force" block at +0xD0..+0xD8 and set `originValid`. This is "where 60 FPS would start the sweep".
    2. If `elapsed < 1/60 - 1e-6`: skip. While skipping, if `g_footingCarry` is set (a catapult or moving surface is carrying the player), advance `sweepOrigin` by `playerPos(+4..+0xC) - old(+0xD0..)`, so the moving platform's displacement is not lost. Increment `skipped`. Return 0 (no collision hit) and pop the routine's single stack argument.
    3. Otherwise if `skipped > 0` and not `replayed`: set `replayed`. Save current +0xD0..D8, overwrite them with `sweepOrigin`, increment `g_launchCollisionPass` (a counter that disables the separate high-FPS "ground stick" patch so it will not interfere), call the original once, decrement, restore +0xD0..D8. Return. The result is the sweep from the original launch point to the current destination, i.e. the one collision pass 60 FPS would have produced.
    4. Otherwise fall through to the original normally.
  - **End:** clear the player's slot (player = 0, elapsed = 0, flags and skipped zero).
- **State machine summary:** IDLE -> Start -> ARMED (elapsed 0) -> [Exec adds dt] -> GRACE (elapsed < 1/60: collisions skipped, origin captured/advanced) -> grace spent -> REPLAY (exactly one original collision call from the captured origin, only if anything was skipped) -> PASS-THROUGH (normal collision) -> End -> IDLE. If nothing was skipped (60 FPS or lower), GRACE never happens and it goes directly to PASS-THROUGH.
- **Pattern:** grace-period.
- **Find it:** Launch behavior (Behavior::Launch Start/Exec/End; player behaviours are a family of Start/Exec/End), the player collision query that takes a pointer to the player and returns "hit", and a stored previous position/force triple near +0xD0 in the player's CharaColliMove data. Jump boards in space gadgets are the repro.
- **Uncertainty:** Author says "Hacky, but this is good enough for now" and "Would be nice to have a better way". If all four slots are busy it clobbers slot 0.

#### F3. Light Dash End velocity keep (sub_76FD0)
- **Object:** Player::Behavior::LightDash::End.
- **Symptom:** Dash rings overlapping with ring trails: the launch velocity is wiped when the light dash ends while a new launch is starting.
- **Mechanism:** If a launch invoke (counter incremented in `sub_87760` around the original) is in progress and the player exists: save the player's velocity (+0x1C..). Compute `ahead = sum((lightDash.target(+0x30..) - playerPos) * lightDash.dir(+0x24..))`. If `ahead <= 0` (player is already past the end of the dash), keep: after the original End, restore the saved velocity.
- **Pattern:** other (event-ordering fix). The author does not state an FPS cause; it rides along with the launch fix because the grace holds the launch for several frames.
- **Uncertainty:** Rationale is a guess from the comment "stack velocity (for DashRings overlapping with ring trails)".

#### F4. Dark Spin exit (sub_6D700), defined later in the file but uses F2
- Not in my line range but part of the same mechanism: when DarkSpin::Exec is in its last state (state field == 2, which always switches to Jump), it registers a fresh `selfTimed` grace for the player. Because there is no Launch::Exec to add dt, the collision hook accumulates the player's own dt. A spin ends and Shadow could get stuck on the red slime thing (Death Ruins). The grace is cleared when it finds nothing was held back; otherwise the slot lingers until reused.

---

### G. Other fixes in range

#### G1. GUNLiftLocal / GUNLiftExpress aim speed (mid-hook, returns true to replace)
- **Symptom:** Aiming the GUN lift turret is too slow above 60 FPS.
- **Root cause:** Aim angle changes by `stick * sensitivity` once per frame.
- **Mechanism:** Not at 60 FPS: `delta = (pi/180) * 1.5 * frames` (1.5 degrees per frame, the GUNLift sensitivity). `pitch = pitch(+0x160) + command.stickY(+0x1C) * delta`, clamped to `[-10 deg, 60 deg]`. `yaw(+0x164) -= command.stickX(+0x14) * delta` (no clamp). Returns true to skip the original.
- **Pattern:** dt-scale. Wait: since `frames < 1` above 60 FPS, the delta shrinks, which is correct.
- **Find it:** Vehicle with vertical angle, horizontal angle, command with stick axes, constants -10, 60, 1.5.

#### G2. SetWatchCamera (cutscene-ish watch camera) idle chase and position
- **Symptom:** Camera sight chase speed varies with FPS. (Exec respects delta time. The sub-functions do not.)
- **Idle chase (mid-hook, around 0x21B670):** There is a ramp field (+0xC4) that increases 0.01 per frame to 1 and scales the chase. Not at 60 FPS: `ramp = min(ramp + 0.01*frames, 1)`; store `min(ramp - 0.01, 1)` back (because the original adds 0.01 afterward); if `ramp > 0.0001`, scale the 3-float chase delta in a stack temp by `(1 - (1 - ramp)^frames) / ramp`.
- **Position update (mid-hook, around 0x21BCF0):** `pos(+0xE0..) += velocity(+0xC8..) * (frames - 1)`.
- **Pattern:** dt-scale (compound for the chase, extra per-frame increment for position).

#### G3. Vine::FireBall (Killer Plant fireball) travel
- Mid-hook at `Vine::FireBall::Exec` (0x1E3540). Only when state (+0x60) == 1: `fireballPos = *(vine+0x48) + 4`; `pos += dir(+0x38..) * (frames - 1)`.
- **Symptom:** The projectile travels slower/faster in real time. **Pattern:** dt-scale (extra per-frame increment).

#### G4. ScatteringWeapon (Shadow Boxes, weapon drops hang)
- **Symptom:** Dropped weapons (Katana02 at 240 FPS especially) get stuck in mid-air and never settle. Early Shadow versions had weapons colliding with each other too.
- **Root cause:** A "settled" test compares a per-frame movement magnitude with the constant 0.1 (data `0x46B380`; this address is a shared constant, also used as the saucer grip step). With small per-frame movement the ray test hits while still mid-air.
- **Mechanism:** Two mid-hooks around the compare. First saves the constant and writes `0.1 * frames`. Second restores it. 60 FPS leaves the constant untouched (but still saves).
- **Pattern:** dt-scale of a threshold.
- **Hack:** modifies a shared global constant temporarily.

#### G5. Belt effect trails (HomingAttack, TriJump, JumpDash, Slide, Spin)
- **Push gate (Belt::Exec):** one trail segment per game frame. Per `BeltTask` carry map: `carry += frames; if carry >= 1: carry -= floor(carry); push; else skip`. At 60 FPS always push.
- **Fade (Eff::Belt update, call near 0x260716):** a per-frame alpha fade of 8 on a byte at `belt+8`. The hook uses wall clock time (steady_clock), not `g_deltaTime`: `elapsed*60` clamped to 4, times 8, plus carry; `step = (int)total`, carry the remainder; then `alpha = clamp(alpha + (8 - step), 0..255)`. The `8 - step` correction suggests the hook sits after the original already changed alpha by 8 (so it undoes 8 and applies `step` instead; sign of the original change is not stated, I could not verify). First sighting of an effect just records the time. When alpha hits 0 the entry is erased; the map is cleared if it exceeds 64 entries.
- **Pattern:** carry-leftover (push gate and fade).
- **Uncertainty:** the use of wall-clock time instead of the game delta (so it will not slow with a pause or time scale) is not explained.

#### G6. Eff::LocusMultiVtx (melee swing trails)
- **Symptom:** Trails shorter/thinner or fade too fast at high FPS; and with split-screen views the effect stepped once per view.
- **Mechanism:** `EffectLocusMultiVtxStep(locus, force)`: per-locus record {last frame number, carry}. If already stepped this frame count (`g_frameCount`), return false (so multiple views do not advance it twice). At 60 FPS or when forced return true. Else `carry += frames`; if `carry >= 1`, subtract floor, return true; else false. Two hooks in `sub_260B40`: push (forced when the locus has zero samples, so the effect starts reliably; "not perfect depending on the MTN") and fade (not forced). Map is cleared past 64 entries.
- **Pattern:** carry-leftover plus once-per-frame dedupe.

#### G7. Destructible/Break debris (TObjEffCrash3DChildObj and TObjEffCrash3DRChildObj Exec)
- **Symptom:** Debris pieces from broken gadgets behave per frame (fall/rotate too slowly at high FPS).
- **Mechanism:** Gate the Exec call: at non-60 FPS, `carry += frames` per piece; if `carry < 1` skip the call (pop its stack word); else `carry -= floor(carry)` and call the original; if the piece's dead flag (byte +0xC bit0) is set afterward, erase its carry. Map cleared past 512 tasks.
- **Pattern:** carry-leftover (60 Hz step gate). Called "similar to ScatterWeapon fix".
- **Uncertainty:** Only runs once per rendered frame, so below 60 FPS (frames > 1) pieces run slower than real time; they do not run multiple times.

#### G8. Not a fix
Lines ~2467-2470 are only the header comment of the "Global FX Alpha Fade" patch (alpha += (int)(fadeRate * dt), truncating to zero at 61 FPS with rate 1/60). It continues past this range and is out of scope for this document.

---

### Count and ranking

Distinct fixes documented: **28** (A1; B1-B4; C1-C2; D1-D7; E1-E2; F1-F4; G1-G7; G8 is a non-fix note). Counting each numbered entry once: A1, B1, B2, B3, B4, C1, C2, D1, D2, D3, D4, D5, D6, D7, E1, E2, F1, F2, F3, F4, G1, G2, G3, G4, G5, G6, G7 is 27 entries, plus G5's two hooks listed as one. Counting push and fade in G5 separately gives 28.

Top 5 for gameplay feel:
1. A1 vehicle fixed 60 Hz step with interpolation (saucer and walker) with ground hiding.
2. F2 launch / jump board / catapult collision grace (and its Exec dt ordering).
3. D2 car axle velocity relax (car could not accelerate properly at high FPS).
4. D1 + D6 + D7 car input low-pass and angle smoothing set (steering, throttle and body feel).
5. B2/B3 saucer grip, anchor and heading turn (handling feel of the saucer).

## Part 3: effects, animation, events, weapons, the player (source lines 2470-3856)

Source: `ShadowRecomp/patches/fps_patches.cpp`. Addresses are Xbox (x86) and are hints only. Hook addresses live in `ShadowRecompLib/config/recompConfig.toml`.

### Conventions

- "frame" means one 60 Hz tick (1/60 s).
- Two dt conventions appear in the code, and the difference matters when porting.
  - Many task `Exec(dt)` functions (the effect command nodes) receive dt in **frame units**. 1.0 means one 60 Hz frame, and it is clamped to at least 1.0 at 60 FPS or below. The Xbox game's own timer clamps this way (`minDeltaTime = 1/refRate`). That is why the effect fixes test `dt >= 1.0` for "60 FPS or lower" and `dt < 1.0` for "above 60".
  - `g_deltaTime` and the player or behavior dt (`player+0x60 -> float`) are in **seconds**. `frames = dt * 60`.
- `NeedsFpsRescale(frames)` = `frames > 0 && |frames - 1| >= 0.0001`. It is a "not exactly 60 FPS" test, so the 60 FPS path stays bit-identical to the original. It is used throughout.
- `g_unroundedDeltaTime` is the raw frame time in seconds. `g_deltaTime` is rounded or clamped. Event and audio-sync code use the unrounded one.
- Patch patterns used: dt-scale, fixed-60Hz-step, slice-timer, hold-key, grace-period, carry-leftover, other.
- Almost every fix leaves the dt >= 1 (60 FPS or lower) path untouched and only alters the high-FPS path. Do the same, so 60 FPS stays pixel-identical to the original.

---

### A. Effect system (global FX command nodes)

The author notes that every FX object, including implicit ones in the GDT files, goes through these command nodes. The fix also cured many colour bugs as a side effect. The root of the effect task is `Effect::PJS::EffectTask::Exec` (sub_C1080). It handles dt correctly and was left alone.

#### Common FX command node layout (this = ecx, `Exec(dt)` takes dt at [esp+4], callee pops 8 bytes)

| Offset | Meaning |
|---|---|
| +0x04 | type pointer. `type+2 != 0` means the owner wants a redraw. |
| +0x10 | started flag (byte) |
| +0x14 | owner/parent pointer, whose first word gets bit 0 set as a dirty flag |
| +0x18 | pointer to the animated target value (alpha, float, or vec3) |
| +0x1C | elapsed timer, in frame units |
| +0x20 | duration, in frame units |
| +0x24 | rate. Alpha is an int32 per-frame rate. For EffCmdSingle it is a float per-frame delta. For Vector3 it is 3 floats (+0x24/+0x28/+0x2C). |

Return value: 1 means keep the node alive, 0 means done and remove it.

#### A1. Alpha fade, single channel (sub_316BC0 wrapper around the original exec logic)

- **Object / system:** FX alpha-fade command node, single-channel variant. The original names it `EffectTask` command exec. Known cases: Cosmic Fall BackDrop FX DFF files, and destructible "gadget" break effects such as Sky Troops pillars.
- **Symptom above 60 FPS:**
  1. At 61 FPS the fade is killed immediately. The Cosmic Fall backdrop never fades in.
  2. At some specific FPS (for example 165 or 270) a white effect lingers for about 0.5 s after a gadget break animation. The alpha fade never reaches the completion value, so the effect stays alive and visible.
- **Root cause:** alpha is a fixed-point int32 (0x3FC00000 = full). It advances by `(int)(rate * dt)` each tick, truncated toward zero. The author's own explanation is only: "Fade rate is 1/60 so 61FPS truncates to fade kill". That is, with the rate near 1/60 of the range and dt just under 1.0, the truncated step lands on the wrong side (zero or the wrong sign, or an under-threshold value), so the node's "killed" test (alpha < 0x3FFFFF) trips and the fade is removed. My reading, not stated by the author: the exact numeric path is not documented, so verify against your build. Also, on a final tick the original stops once `timer > duration` without applying the remainder, so some FPS values leave alpha short of full (the lingering white effect).
- **Original semantics (needed to reproduce):**
  1. If the target pointer is null, mark the owner dirty and return 0.
  2. If `started == 0`, set started and return 1. This is a one-frame delay.
  3. Otherwise `timer += dt`.
  4. If `timer > duration`, mark dirty (when `type != 0 && type+2 != 0 && owner != 0`, OR 1 into `*owner`) and return 0.
  5. Else if `rate != 0`, do `alpha += (int)(rate*dt)`. If `alpha > 0x3FC00000` (signed compare), clamp it to 0x3FC00000 and zero the rate. If `alpha < 0x3FFFFF`, zero the target pointer (+0x18) and the rate, which "kills" the fade. Return 1.
- **Mechanism (two layers):**
  1. **Slice timer, in the sub_316BC0 override.** If `dt >= 1.0`, call the re-implementation directly (a 1:1 copy of the original). Otherwise keep a per-node float accumulator keyed by node address. Add dt to it. If the total is below 1.0, return 1 (alive) without running anything, pop the arg (`esp += 8`), and do not touch the node timer. When it reaches or exceeds 1.0, subtract 1.0, overwrite the dt argument with exactly 1.0, and run the 60 FPS logic. So the whole node effectively ticks at 60 Hz with dt = 1.0. On completion (return 0) erase the accumulator. This stops per-frame truncation at high rates and keeps timer, duration, and rate math identical to the original.
  2. **Carry-leftover in the re-implementation (`FxAlphaStepSingle`).** It computes `exact = rate*frames + carry[node]` in double, steps by `(int)exact`, and stores `carry = exact - step`. It then clamps with the same checks as the original: above 0x3FC00000 sets the full value, zeroes the rate, and erases the carry. For `rate < 0` and `alpha < 0x3FFFFF`, it zeroes +0x18 and the rate. The `rate < 0` guard is new: it stops a positive-rate fade from being "killed" because of the low threshold. The carry map is cleared if it exceeds 4096 entries.
  3. **Completion leftover.** When `timer > duration` the original just stops. The patch calls `FxCmdLeftoverFrames(prevTimer, duration, dt)` and applies one more alpha step of that many frames, so the value lands at its final value. This is the white-effect fix. `FxCmdLeftoverFrames` returns 0 if `dt >= 1`, else `clamp((duration - 0.0001) - prevTimer, 0, dt)`, meaning the remaining frames in the fade that this tick's dt would have overshot past the end.
- **Pattern:** slice-timer (outer) plus carry-leftover (inner).
- **How to find it:** an Exec(dt) member with a `started` byte, a float timer and float duration, an int rate, and a pointer to a 32-bit value. Constants: 0x3FC00000 (full-scale alpha, about 1.5 as a float bit pattern, used as fixed-point) and 0x3FFFFF. Called from the effect task command list; the sister function is the three-channel version. Used by GDT-defined FX and by "gadget" destructibles.
- **Notes:** the author says they ended up not needing to patch the effect-task root. The carry map is keyed by node address and trimmed by a crude `clear()` at 4096. A reused node address could inherit a stale carry; the erase calls on completion mitigate that.

#### A2. Alpha fade, three channel (sub_3167D0)

- **Object / system:** same family, three-channel (RGB-ish colour) alpha fade command. Cases: Glyphic Canyon platform, and RingShot bullet colour.
- **Symptom:** red FX and other weird colour side effects at 61+ FPS. The three channels truncate to a kill, and the platform ends up red.
- **Root cause:** same as A1, with three ints at target+0, +4, +8 and three int rates at node+0x24/+0x28/+0x2C.
- **Mechanism:**
  1. Null target: mark dirty, return 0. Not started: set started, return 1. (Unlike A1, no carry erase on the null path.)
  2. `timer += dt`. On completion (`timer > duration`), compute leftover frames the same way as A1. For each channel with leftover > 0, apply one carry step (`FxAlphaStepChannel`). Always erase the three carry entries (keyed by rate address). Mark dirty, return 0.
  3. Else per channel: skip if the rate is 0. If `dt >= 1.0`, use the original step `(int)(rate*dt)`, then clamp. Above 0x3FC00000 gives 0x3FC00000 and a zero rate. Below 0x3FFFFF gives value 0 and a zero rate. This is the three-channel difference: it zeroes the value instead of clearing the target pointer. If `dt < 1`, use `FxAlphaStepChannel`, which is the carry version with the same clamps (the low clamp only applies when `rate < 0`). Return 1.
  4. There is no 60 Hz slice accumulator here, unlike A1. The carry alone makes it correct.
- **Pattern:** carry-leftover. The per-channel carry key is the rate address.
- **Find it:** the sibling of A1, with three consecutive rates at +0x24 and a loop of 3 int channels. Same clamp constants.

#### A3. EffCmdSingleExec (sub_315220)

- **Object:** float-valued effect command. The comment says it also accounts for gravity, friction, amplitude and so on.
- **Symptom:** the animated float ends short of its final value at high FPS. The last partial frame is dropped, because the command ends when `timer > duration` and the original adds nothing for the remainder.
- **Mechanism:** wrapper. Before calling the original, capture target, started flag, and previous timer. Call the original. If the target was non-null, the node had started, and the return value (al) is 0 (finished this call), compute `leftover = FxCmdLeftoverFrames(prevTimer, duration, dt)`. If leftover > 0, add `rate_float * leftover` to `*target`. (Rate float at +0x24.)
- **Pattern:** carry-leftover (final flush).
- **Find:** the Exec of the float command; same node layout. Hook it, snapshot, call, then top up on completion.

#### A4. EffCmdVector3Exec (sub_316240)

- Same as A3 but for 3D position, rotation, velocity and scale. After the original returns 0, flush the 3 floats: `target[i] += rate[i] * leftover` for i = 0..2, with rates at +0x24, +0x28, +0x2C. Pattern: carry-leftover.

---

### B. UV animation baked flipbook (sub_2C9B50 and sub_2CA020)

- **Object:** RenderWare UV animation interpolators. The "param scheme" and "linear scheme" interpolate callbacks. Example asset: `EFGUNTHUNDER.UVA`, which drives the GunBeetle electric shield.
- **Symptom:** the UVA is a "baked flipbook", with a key every 1/60 s. RenderWare blends linearly between keys, so at any rate other than 60 FPS the sample lands between two keys and shows half a row. It looks like skipped frames.
- **Fix (hold-key):** both interpolate callbacks (args on the stack: out, key0, key1, time) are intercepted. A key is a record of time at +0x04 followed by six floats at +0x08..+0x1C. If `|(key1.time - key0.time) - 1/60| <= 0.0001`, it is a baked flipbook segment, so skip the blend and copy the six floats of one key to the output. The key chosen is key1 if `(time - key0.time)/span >= 0.999`, else key0. The 0.999 is because a 60 FPS sample sometimes lands just before the next key, so near-end counts as the next key. Return from the callback manually (`esp += 4`; this build's calling convention pops a 4-byte arg, so the override returns with the stack adjusted). Otherwise call the original.
- **Pattern:** hold-key.
- **Find:** the UVA interpolators called from the RW UV-anim evaluation. Distinguishing feature: keys 1/60 apart. Both schemes share the same signature.

---

### C. Enemy AI rotation and aim

#### C1. EnemyBase rotation to target (sub_C23B0, "Shared_RotateToPlayerCalc")

- **Symptom:** enemies turn too slowly or too fast at high FPS.
- **Root cause:** the yaw step uses a per-frame easing term that does not scale with time.
- **Mechanism (shared helper `TurnEnemyYaw(model, targetYaw, dt, rate, frames)`):**
  - `minStep = pi * (1/180)` (a one-degree minimum step; constants at 0x47E0A4 and 0x42A880).
  - `delta = wrap(target - yaw)`.
  - `step = max( min(dt*rate, |delta| * (1 - (1 - 0.5)^frames)), minStep*frames )`. So the proportional part is "half the remaining angle per 60 Hz frame", expressed as `1 - 0.5^frames`; it is capped by the rate-limited step and floored by a minimum step scaled by `frames`.
  - If `|delta| > step`, `yaw = wrap(yaw + sign(delta)*step)`, else snap to the target.
  - Returns "facing" = `|target - newYaw| < minStep`.
  - Enemy yaw lives at (enemy+0x30)->+0x14, and the target yaw at enemy+0x88. The turn rate is fetched via a vtable call (`enemy+4 -> vtable +0x2C`), which returns on the FPU stack. The dt used is a float pointed to by `(enemyModel+0x60)`.
  - It only replaces the original when `NeedsFpsRescale(frames)`.
- **Pattern:** dt-scale (exponential ease) plus a scaled minimum step.

#### C2. AI aim / turn (sub_C3580)

- Same helper, different layout. The turner (ecx) points to the enemy; the direction vector is a stack arg. Target yaw = `atan2(-dir.x, -dir.z)`. dt is read from `(turner+4)+0x20`, and the turn rate from `turner+0x8`. Same only-when-rescaling gate. Returns a facing flag in al; pops 8 bytes.

#### C3. Aim bone snap (EnemyAimLerpMidAsmHook, six mid-asm sites)

- **Objects:** BkSoldier, BkNinja, GUNRobot. The author notes BkNinja's head calls this but passes it to the arm: "BkNinja was rushed".
- **Fix:** an aim-lerp factor `t` is read from the stack at esp+8. If it is strictly between 0 and 1 and the framerate is not 60, replace it with `1 - (1 - t)^frames`. This turns a per-frame blend factor into a time-correct one. Pattern: dt-scale (exponential compounding).
- **Find:** a bone aim lerp where t is a constant per-frame blend factor.

---

### D. BirdBase vehicle (BlackVolt / BlackHawk) turn on stick release (two mid-asm sites, rotation update 0x251F00 called from bird Exec 0x253740)

- **Symptom:** only the stick-release return rotation was not using dt, so the vehicle straightened wrongly above 60 FPS.
- **Fix:** at the hook site, the "rotation rate" float on the stack (esp+0x10) is multiplied by `frames`, clamped to at most 1.0. So `rate = min(rate * dt*60, 1)`. It does nothing when frames is about 1. Pattern: dt-scale (linear, per-frame blend factor).

---

### E. DarkSpin exit (Player::Behavior::DarkSpin::Exec, sub_6D700)

- **Object:** the red slime Shadow absorbs with a spin dash which carries him along a path.
- **Symptom:** above 60 FPS, the jump at the end can leave Shadow stuck inside it (Death Ruins is one example).
- **Fix (grace-period):** it reuses the "launch grace" mechanism written for jump boards and catapults. When state (+0x5C) == 2, which is the last state and always switches to Jump, register a launch grace record for the player (player = behavior+4), with `selfTimed = true`. Find an existing record or a free slot of 4, zero it, and set player and selfTimed. The grace system ignores collision for the first 1/60 s of the launch, then runs the original collision code once so the true destination is computed as at 60 FPS. The generic launch grace is defined earlier in the file (`g_launchGracePeriod = 1/60`; fields player, elapsed, sweep origin from player+0xD0 (old position), originValid, replayed, skipped, selfTimed). Then call the original.
- **Author remark:** "Hacky, but good enough".

---

### F. TornadoTowing (sub_1CD350)

- **Symptom:** the end-of-path check fails at high FPS. The check uses dt itself (`remaining distance <= speed*dt`), so with a tiny dt the object barely misses the target forever.
- **Fix:** read speed from `(this+0x28)+0x54`, current position at +0x38..0x40 (vec3) and target at +0x44..0x4C. Compute `len = |target - pos|`. When `dt > 0`, `speed > 0` and `dt < 1/60`: if `len > speed*dt` and `len <= speed/60`, overwrite the dt stack arg (esp+0xC) with `len/speed * 1.001`, so it lands on the target in this step. Then call the original. Pattern: other (dt override for the final step, emulates the 60 FPS tolerance).

---

### G. UI

#### G1. Hero/Dark chaos gauge fill (draw callback sub_280B60; mid-asm hook)

- **Original:** every frame the displayed value moves half the gap towards the target, and always renders 1 below the target at the end, never above.
- **Fix:** with `target = eax`, `shown = ecx` (ints). Skip when `target - shown <= 1`, `dt <= 0`, or `dt >= 1/60`. Else `step = (target-shown) * (1 - 0.5^(dt*60))`, `next = min(shown + ceil(step), target - 1)`, and write `next` to both eax and ecx. Pattern: dt-scale (exponential).

#### G2. Egg Dealer roulette reel bounce (sub_115A50)

- **Symptom:** the stop wobble looks wrong above 60 FPS.
- **Fix:** call the original, then (only if `*reel == 3` (the bounce state) and `0 < dt < 1/60`) recompute the reel angle (+0x38). The wobble timer in the original counts down from 0.2 (reel+0x14 is time remaining), per frame. The fix quantizes the elapsed time to whole 60 Hz frames: `elapsed = 0.2 - remaining`, `timer = 0.2 - max(1, floor(elapsed*60 + 0.001))/60`. Then it re-evaluates the original formula with that timer: `bounce = (0.5/c1) * timer * 5 * sin(c2 * timer)`, and `angle = index(reel+0x10) * width + bounce`, with a wrap subtraction of constant 0x4F998C if it is exceeded. (c1 = 0x4F9984, width = 0x4F9988, c2 = 0x54BED8.) Pattern: fixed-60Hz-step (quantize to frame ticks).

#### G3. Advertise message timer (sub_2A6020, two mid-asm sites)

- The advertise message has a 1.5 s duration with a hardcoded 1/60 step instead of dt. Fix: compute `excess = (1/60) - dt` when `0 < dt < 1/60`. Subtract excess from the elapsed timer (+0x24) and add it to the hold timer (+0x20) (esi = message), so net progress per tick equals dt instead of 1/60. (Constant 0x42BE10 = 1/60.) Pattern: dt-scale (compensate the hardcoded step).

#### G4. CSD colour key lerp (sub_3899C0)

- **Object:** CSD (Scaleform-like UI) animation colour key interpolation. Args at esp+8 (key0), esp+12 (key1), esp+16 (time as float).
- **Symptom:** at 60 FPS the CSD timer is always a whole frame, so two colour keys give a smooth transition. At high FPS the timer reaches between frames and produces an intermediate blend the game never showed. The pause "story route" relies on this: the whole screen flashes brighter for a couple of frames.
- **Fix (hold-key):** key frame numbers are ints at the start of each key. If `frame1 - frame0 == 1` (adjacent frames): if `time >= frame1 - 0.001`, set time to `frame1`; otherwise set it to `frame0`. The 0.001 handles CSDs that end on partial frames. Then call the original. This forces a step instead of a blend between adjacent-frame keys. Pattern: hold-key.
- **Find:** the colour key interpolation with two key pointers and a time float, where key ints are frame indices.

---

### H. Melee weapon trail and hit sweep (sub_9FA10, sub_2704F0, sub_995C0, sub_374A60)

- **Symptom:** the hand bone blends rotation between animation keys. Off 60 FPS, frames land between keys, the pose goes off the intended path, and the blade tip and trail spike up or down.
- **Fix: force the "single frame" animation mode that events use for the swinging player's skeleton.**
  1. Weapon update (sub_9FA10, task+0x58): record the holder player (this+0x28) in a global during the call, and clear it after.
  2. Melee hit sweep (sub_2704F0): stamp `now` into a map keyed by the holder player.
  3. Player skeleton update pass (sub_995C0): `data = this+0x58` is the player; if the map shows a swing stamp within the last 200 ms, set a global "swinging model" to `this+0x3E8` (the skeleton) for the call, else 0. Clear after.
  4. Skeleton animation sampler (sub_374A60, arg0 = model): if the model equals the swinging model, temporarily OR 0x8 into `model+0x14` (flags) around the original call, then restore. Flag 0x8 is the single-frame mode, which samples the key at floor(frame) and does not blend.
  - This is scoped per player so enemies with melee weapons are not affected, and the 200 ms window keeps the mode on through the whole swing and its recovery.
- **Pattern:** other (hold-key via an animation flag). Uses wall-clock `steady_clock`, which breaks with pause or slow motion, though this is only a 200 ms window.
- **Find:** the skeleton sampler with a flags word where 0x8 is single-frame. Events already set it, so search for the event single-frame mode setter (sub_22A540).

---

### I. Event (cutscene) system

Cutscenes are "TPjs EventAction" and run through an event manager. Normally the event runs at 30 FPS with a frame divisor. Option "Cutscene framerate: Unlocked" makes them run at the game's FPS. All the following apply only when that option is unlocked.

- **I1. Creation (EventCreatedMidAsmHook, after the manager constructor sets the divisor):** reset the camera-cut flag, stream-wait carry, and battle fade hold. Set the global "event playing" flag (so that events run at 30 even if the player runs lower). If unlocked, set the frame-rate divisor to 1.
- **I2. Clock step (EventClockStepMidAsmHook, sub_22B840 at +0x19):** overwrite the dt arg (esp+0xC) with `min(unroundedDeltaTime, 1.0)` seconds. Real frame time with no rounding, so event audio does not desync. The event clock is a float at 0x54D498 (current frame of the clip/event), and frames per second at 0x448150 (30).
- **I3. Single-frame MTN setter skip (EventMotionSingleFrameMidAsmHook, sub_22A540, two sites):** returns true when unlocked, to skip the code that enables single-frame motion mode, so event motions blend normally between frames. The jump targets skip the setter. (Opposite of H, which forces it on.)
- **I4. Event camera cut detection (EventCameraFrameMidAsmHook at sub_22BBE0, frame arg at esp+4, camera motion in `*esi`):**
  - **.SNB camera motion format:** +0xC track count, +0x10 tracks pointer. Each track is 0x28 bytes: +0x0 channel type (masked with 0xFFFFFF00), +0x4 interpolation (low 16 bits), +0x14 start frame (float), +0x18 end frame, +0x1C key count, +0x20 key size, +0x24 keys pointer. Each key is (frame float, value float, ...). Position channels are types 0x100/0x200/0x400 (X/Y/Z). Target (look-at) channels are 0x40000/0x80000/0x100000. Interpolation modes 2 and 4 are used by events; 0x20 is used by boss cameras and in-gameplay SNBs.
  - **IsCameraMotionCut(motion, frame):** for each track that is a position or target channel with interpolation 2, 4, or 0x20, and with the frame inside [start, end]: require at least 2 keys and `frame >= first key frame`. Binary search for the key index `i` with `keyFrame(i) <= frame` (largest). Skip if there is no next key or if `keyFrame(i+1) - keyFrame(i) > 1.0`. Compute `neighbourSlope` as the max of the absolute per-frame slope of the previous segment (i-1 to i) and the following segment (i+1 to i+2), where available. `jump = |value(i+1) - value(i)|`. A cut is detected if `jump > 1.0` and `jump > neighbourSlope * 4`. Any track hitting this returns true. So: a one-frame jump that is large and much steeper than its neighbours is a camera cut (teleport).
  - **Use:** when a cut is detected, set the thread-local "cutting" flag and use `trunc(frame)` instead of the fractional frame for the camera sample, so at 61+ FPS the camera uses the prior whole frame on a switch, avoiding an awkward blended snap between two shots. Else use the raw frame.
- **I5. Actor frame (EventActorFrameMidAsmHook, sub_22A240, frame at ebp+0xC):** if the camera is cutting this tick, truncate the actor's frame too, so actors and the camera stay consistent across the cut.
- **I6. Actor motion sampling and motion-key teleport detection (sub_22A240 override and sub_379D20):**
  - sub_22A240 (event actor update, calls sub_36E270) sets a thread-local "sampling event motion" flag when unlocked.
  - sub_379D20 is the motion key (Hermite-like) interpolation. Args: prevKey, nextKey, sample frame (float, esp+0xC), result on the FPU stack. A motion key layout: +0x0 frame, +0x4 in slope, +0x8 out slope, +0xC value. When the flag is set and `floor(sample) >= prevKey.frame` and `floor(sample)+1 <= nextKey.frame`: evaluate the curve at `floor(sample)` (whole frame), and if the sample is fractional and the segment is not a "pop", also evaluate at `floor+1` and linearly interpolate between the two results by the fractional part: `v = vA + (vB - vA) * (sample - floor)`. This reproduces the 30 FPS look (curve is sampled at whole frames, i.e. the "single frames blended equally") while giving smooth motion at high FPS. Otherwise fall through to the original.
  - **IsEventMotionPop(prev, next):** if the keys are more than 1 frame apart it is not a pop. Else `maxSlope = max |slope|` over the four slope fields; `jump = |value delta|`. A pop (teleport) is `jump > 0.05` and `jump > maxSlope * 4`. When it is a pop, skip the second sample and the blend and return the floor sample only, so the teleport stays a hard cut rather than a streak.
  - **Pattern:** other (hold-key plus lerp between whole-frame samples, with teleport detection).
- **I7. Event stream wait (EventStreamWaitMidAsmHook, sub_228740 at 0x228BB6):** the original pauses on a wait counter to align with the audio track. At unlocked rate, accumulate `eventDt * 30` into a carry. If the carry is below 1.0, decrement ecx by 1 (cancelling the counter decrement, so the wait does not advance this tick) and return. Otherwise `carry = min(carry - 1, 1)`. Pattern: slice-timer (30 Hz ticks of the wait counter).
- **I8. 2P battle fade (TPjsEventActionFade::Exec, sub_2280D0):** fade mode value at +0x28 == 2 means a black fade for 2P battle only. At high FPS it ended too early. The patch accumulates dt (stack arg at esp+4) into a global hold timer. While the hold timer is less than `1/30` it returns immediately (pops 8), else it resets the timer to 0 and runs the original. So the fade's per-call step is gated to 30 Hz. Pattern: slice-timer. The state is global, not per instance.

---

### J. Jingle, music, and fade tick timing (sub_237DA0, "GoalAction", four mid-asm sites)

- **Symptom:** jingle, music and fade timing tick per frame.
- **Fix (carry-leftover tick count):** the original reads a goal tick rate (float at 0x456784) and advances by one tick per frame. The patch replaces the per-call tick with `carry += unroundedDeltaTime * tickRate`, `ticks = floor(carry)`, `carry -= ticks`, and writes `ticks` into the register the original uses (eax or ecx, depending on the site). So an integer number of ticks per frame, with the fractional part kept for later frames. The tick count can be 0 at high FPS. The carry is global (one goal action at a time).
- **Find:** the goal-result jingle/music state machine and its timer decrement; the constant 0x456784 is the tick rate. Four increment sites.

---

### K. Boss / special camera (sub_23950)

- **Symptom at 61+ FPS:** snaps and interpolation glitches across cuts in boss or special camera SNBs (the in-gameplay SNBs, interpolation 0x20).
- **Fix:** args are camera motion (esp+8) and frame (esp+0xC). Test `IsCameraMotionCut` (see I4) for frames `frame-1`, `frame`, `frame+1`. If any is a cut, replace the frame arg with `trunc(frame)`. Then call the original. Pattern: hold-key.

---

### L. Weapon fire rate (ControlCannon sub_A0CD0, ControlGun sub_A1740, ControlLockon sub_A1C10, lock-on exec sub_268C40)

- **Symptom:** auto-fire cooldown depends on FPS. For the 2P GunAndroid the wait is 100 ms + one frame: 100 + 16 ms at 60 FPS, 100 + 4 ms at 240 FPS.
- **Root cause:** a cooldown is set to a fixed number of seconds, then decremented by dt each frame. The first check happens one frame later, so the effective wait is the interval rounded up to a whole frame.
- **Helper `Get60FPSWait(wait, late, dt, holdFrames)`:**
  1. `step = (1/60) * (dt / g_deltaTime)`, one 60 Hz frame expressed in the units of the function's own dt.
  2. `steps = holdFrames`, and subtract `step` from a copy of `wait` until it is <= 0, incrementing steps each time. So `steps = holdFrames + ceil(wait/step)`.
  3. `wait60 = steps*step - holdFrames*dt`. This is what the cooldown would be at 60 FPS (rounded up to whole frames), with the hold frames paid in actual frame time.
  4. `late = clamp(late, 0, max(wait60, 0))`, which is overshoot already spent.
  5. Return `max(wait60 - late - 1e-6, 0)`.
- **Wrapper `WeaponFireRate60FPS`:** run the original, then if frames != 1, `dt > 0`, the new cooldown (control+0x10) is positive and larger than the previous value (it was just re-armed), then store `Get60FPSWait(newCooldown, prev > 0 ? dt - prev : 0, dt)`. `late` is the unused part of the previous frame's dt when the previous cooldown expired inside this frame.
  - **ControlCannon and ControlGun:** cooldown at +0x10, dt at [esp+4], `holdFrames = 0`.
  - **ControlLockon (sub_A1C10):** cooldown at +0x14, `late = 0`, `holdFrames = 2` (two frames of hold are accounted for at actual dt). The reason for 2 is not documented; it is probably two frames of state delay in the lock-on control before the cooldown starts counting. Treat as empirical.
  - **Lock-on execution (sub_268C40):** +0x54 is the shot interval and +0x58 the lock interval. Snapshot both before the call. After, if the lock interval (+0x58) is positive and rose (re-armed), set it to `Get60FPSWait(after, before > 0 ? dt - before : 0, dt)`. For the shot interval (+0x54), the re-arm test is `after > before - dt` (since it decays by dt inside the call), using the same late formula.
- **Pattern:** fixed-60Hz-step (round cooldown up to 60 Hz frames, minus carried lateness).
- **Find:** weapon control classes (Cannon, Gun, Lockon) with a float cooldown that is set and decremented in `Exec(dt)`.
- **Minor:** the author calls this "a pretty minor fix".

---

### M. Awake (Chaos Control / Chaos Blast) idle drain (AwakeIdleDrainMidAsmHook at 0x8EB8F, esi = awake)

- **Symptom:** the gauge drains too slowly or too fast. The original drains an integer amount per frame.
- **Fix (carry-leftover):** per-awake-instance float carry. `carry += dt * (-1500.0 constant at 0x449BD0) / divisor`. `drain = (int)carry` (truncate toward zero), `carry -= drain`, and put `drain` in ecx for the original code to subtract. dt is `*((*(awake+0x28))+0x60)`, the player's dt in seconds. The divisor is 1 normally. In the multiplayer mod it is the number of awake players, to split the drain. Because the constant is negative, `drain` is negative and truncation toward zero keeps the carry symmetric.

---

### N. Player ground, run, jump, speed boost, turn rate (most important for feel)

All in `Player::Behavior::Ground` (run state sub_72AC0, Exec sub_733F0) unless noted. Offsets are on the Ground behavior object (`ground` = edi/esi).

#### N1. Run speed decay (GroundSpeedDecayMidAsmHook at 0x72C56, jumps over the original to 0x72C78)

- **Symptom:** stick-slow movement at high FPS, which was subtle. Shadow could get stuck in Walk. (Credit "Jacko" for catching it.)
- **Root cause:** when the stick is relaxed, the run speed (ground+0x2C) is multiplied by a constant decay factor per frame, then clamped to at least the walk speed (ground+0x24). At high FPS the multiply happens far more often.
- **Fix:** `frames = dt*60`, where dt is a stack local at esp+0x74 (seconds). `speed = ground.speed(+0x2C) * decayConst^frames`, where decayConst is the float at 0x4480FC (the original per-frame factor). `ground.speed = max(speed, walkSpeed)` where walkSpeed is at ground+0x24. Pattern: dt-scale (exponential, `pow(k, frames)`).

#### N2. Weapon kickback (GroundWeaponKickbackMidAsmHook at 0x734BD)

- A per-frame damping factor (stack at esp+8). If `0 < factor < 1`, replace it with `factor^frames`, where `frames = dt*60` and dt is at esp+0x34. Pattern: dt-scale (exponential).

#### N3. Sample clock and walk reset (GroundSampleClockMidAsmHook at 0x72AFD, GroundWalkResetMidAsmHook at 0x72D1E)

- **Purpose:** certain parts of the run state (the "walk reset" and the more tolerant jump-reverse flip, noticeable on Lethal Highway) are written for one sample per 60 Hz frame. The patch builds a virtual 60 Hz sample clock per Ground object.
- **Sample clock:** with `dt` at esp+0x70 (seconds): if `dt >= 0.75/60` (about 80 FPS or lower), `clock.time = 0` and `isSample = true` (every frame samples, exactly as original; decay starts mattering about 80 FPS). Otherwise `clock.time += dt`, `isSample = clock.time >= 1/60 - 0.0001`. When it samples, `clock.time = fmod(max(clock.time - 1/60, 0), 1/60)`, i.e. carry the remainder.
- **Walk reset hook:** returns true (take the jump that skips the reset code) if the clock exists and this frame is not a sample. So the walk-reset logic only runs on virtual 60 Hz sample frames. Pattern: slice-timer (with carry-leftover). Clock state is a map keyed by ground pointer.

#### N4. Speed boost / start speed (sub_7EED0 physics step record and GroundStartSpeedMidAsmHook at 0x72789)

- **Symptom:** the "start" speed on Ground entry (the 2000 unit boost from a dash panel) sometimes ends above 120 because the speed carried into Ground was computed at the wrong step length.
- **Physics step recording (sub_7EED0 override):** args: position pointer (+4), force pointer (+0xC), a pointer to a flags word (+0x14), dt (+0x18). Before calling the original, log into a map keyed by the position pointer: wall-clock time (`App::s_time`), the step dt, `grounded = (flags & 0x284) == 0x4`, and the 3 force floats.
- **Start-speed hook:** look up the record by `player + 4`. Bail (run the original) if no record, not grounded, `dt <= 0`, `dt >= 1/60`, or the record is older than 1.5 app delta times. Otherwise reconstruct what the speed would have been after a full 60 Hz step. The physics model is velocity with linear drag: `v' = settle + (v - settle) * e^(-drag * t)`, with `settle = force/drag` per axis and `drag` the float at 0x4ED6A0. The patch uses the remaining time `t = 1/60 - recordedDt`, because the physics step already advanced by dt. `decay = exp(-drag*(1/60 - dt))`. Per axis, `v_i = settle_i + (player.vel_i - settle_i) * decay` (velocity at player+0x1C, +0x20, +0x24). `vel = |v|`.
  - **Result:** `ground.speed(+0x2C) = (vel < 30) ? walk : walk + (max - walk) * min(vel/750, 1)`, where walk = ground+0x24, max = ground+0x28. Then it returns true to skip the original start-speed code (jump to 0x72790).
- **Pattern:** other (reconstruct 60 Hz-equivalent velocity, then normal remap).
- **Find:** the Ground behavior constructor / start code that maps actual velocity to run speed: it divides by 750 and mixes between walk and max speed. The 750 and 30 are constants in this fix, which presumably mirror the original code.

#### N5. Jump flip animation hold (Strafe Jump, sub_75390 init, sub_75B70 exec)

- **Symptom:** the jump flip animation (during strafe jump) was shortened at high FPS, because state 4 is a one-frame transitional state.
- **Fix:** on init, erase the per-jump held timer. On Exec, if the jump state (+0x1C) is 4, accumulate dt into the held timer; if `held < 1/60 - 0.0001`, rewrite the state to 3 (hold the earlier state), else erase the timer and let state 4 proceed. Then call the original. So the flip state stays for at least one real 60 Hz frame. Pattern: slice-timer / hold-key.

#### N6. Global turn rate (sub_7ED90, player and partner behaviors)

- **Symptom:** turn rate depends on FPS.
- **Root cause:** a per-frame turn blend factor `step` in (0,1) is applied each frame, giving exponential approach.
- **Formula:** args: player pointer at `**(esp+4)`, step at esp+0xC. `dt = *(player+0x60)` (float, seconds), `frames = dt*60`. If `0 < step < 1` and `frames > 0`: `step' = (step >= frames) ? 1 : 1 - (1 - step/frames)^frames`. Store it back, then call the original. Rationale: at 60 FPS (frames = 1) this is the identity, and at high FPS the per-tick factor `1 - (1 - s/f)^f` is the compound fraction. Note this is not `1 - (1-s)^(1/f)`; it is chosen so the factor scales with dt in the small limit (`step' ~ step` as f -> 1), and the guard `step >= frames` clamps to 1 to avoid a negative base. Note that the author's formula treats `step` as already being a per-step rate (probably step = rate*dt style), so the division by frames recovers the per-60Hz-frame fraction. Treat the exact semantics as empirical when porting: check whether your step arg already includes dt.
- **Pattern:** dt-scale (exponential).
- **Find:** the shared turn helper used by the Ground behavior and by partner behaviors. It takes a blend factor in (0,1) and an angle.

---

### O. BkLarvaGenerator (sub_138A00 parent; LarvaMotionSpeedMidAsmHook)

- **Symptom:** larva speed scales wrongly with FPS: the parent already uses dt, so the factor was applied twice.
- **Fix:** divide the speed value at esp+0x30 by `step` (the larva motion step float at 0x532A48, which is the dt-driven value) if `step > 0`. Pattern: dt-scale (cancel double scaling).

---

### Uncertainty and hacky remarks

- DarkSpin exit: "same fix than the jump board, but a bit simpler"; the launch grace is described as hacky but good enough.
- Melee: uses wall-clock time and a 200 ms window. It affects only the sampler flag.
- Fx carry maps are keyed by raw addresses and cleared when oversized.
- Event fade hold time and stream-wait carry are global, not per event.
- Lock-on `holdFrames = 2` is empirical and unexplained.
- The turn-rate formula in N6 is unexplained in comments; reimplement from the formula but verify by checking that the 60 FPS identity holds.
- Cosmic Fall, Glyphic Canyon, Death Ruins and Lethal Highway are the named test cases.

### Count

34 distinct fixes (A1-A4, B, C1-C3, D, E, F, G1-G4, H, I1-I8 grouped as 6 distinct event fixes, J, K, L, M, N1-N6, O).
