# Game bugs ShadowRecomp fixes, and which ones are ours too

**Source and licence.** Notes on ShadowRecomp (FoxWorkshop/ShadowRecomp, GPL-3.0), the Xbox recompilation of the same game, written from a read of its code on 2026-11-16. Mechanisms and data are described in our words; no code was taken, and none should be. Xbox addresses are hints for finding the same code in the GameCube DOL (tools/guest_classes.py for class names).

**What it means for us.**
- **The 20 GAME-LOGIC entries are in the game's own code, so they are likely present in our GameCube build too.**
- **Their comments confirm several on GameCube directly:**
  - "This happens on GC too": the Egg Dealer roulette picks sprite 6 of 5 (misc_patches.cpp:429).
  - "bug in all versions (PS2/Xbox/GC)": animated textures (TextureChange) sometimes do not play
    (animated_texture_patches.cpp:3). It is a different bug from our erratum E3 (docs/GAME_ERRATA.md: TTextureChange
    clocks per streamed block). Both live in the same set code.
  - "an original bug": the Egg Dealer / final mech camera callbacks run on a freed camera, harmless on hardware
    (multi_core_patches.cpp:429). Our allocator or native heap may expose it.
- **Fixed only by matching GameCube, so ours should already be right:**
  - the enemy collider's hidden state ("XboxEarlyTargetBehavior");
  - the synopsis skip button;
  - the subtitle alpha, because GameCube has no in-game Restart/Quit prompts.
- **Not ours:** the XBOX-PORT and RECOMP-HOST entries. Read them as background only.
- **How to fix one.** Each entry we confirm becomes an erratum in docs/GAME_ERRATA.md, the way our E-series are done:
  - a reproduction on the disc;
  - the user's decision;
  - a native fix behind its own `SHADOW_ERRATA_*` switch;
  - a regression test.

## The notes

Source: /home/user/foxworkshop/shadowrecomp/ShadowRecomp/patches/ (commit 9fcbc85). Mid-asm hook addresses were cross-checked in ShadowRecompLib/config/recompConfig.toml. Xbox addresses below are hints only (retail Xbox build, image base-relative). Patch authors' comments are quoted only as short phrases.

Conventions: "always on" = no Config gate. Config names are from user/config_def.h (hidden "Codes" entries default false unless stated).

Classification key: GAME-LOGIC (likely on GC), XBOX-PORT, RECOMP-HOST, ENHANCEMENT. Where the comments do not settle it I say "lean X" and give the reasoning.

### Direct evidence about the GameCube build (comments that mention GC)

| Where | What the comment says | Meaning for us |
|---|---|---|
| ResetMessageBrushColor (misc) | sub_28D1E0 (SaveMessage) is reused for the in-game Restart/Quit prompts; "GC/PS2 don't have those prompts, so they never have the bug" | Subtitle fade bug is Xbox-only. GC has no in-game restart/quit prompts. |
| Synopsis skip button (misc) | "Originally on GameCube, the synopsis shows a skip button at the bottom right"; Xbox sets a hide flag | GC already shows it. Nothing to do. |
| Egg Dealer roulette flash (misc) | "This happens on GC too, rarely the intro cutscene picks sprite 6 (there are only 5)" | Confirmed on GC. Worth fixing. |
| Animated textures (animated_texture) | "bug in all versions (PS2/Xbox/GC)" | Confirmed on GC. |
| Enemy colliders (code_patches) | "Matches the GC enemy hidden state handle", "GC inits these at 0,0,0"; Xbox Oct 16 builds changed it by accident | GC has the correct behavior. Xbox regression only. |
| Task list (multi_core) | Task children at parent+0x1C, next at node+0x10, prev at node+0x14, "same as GameCube" | Task struct layout is the same as GC. Useful for locating Task::Task / ~Task. |
| Egg Dealer camera UAF (multi_core) | "actually an original bug, but the callback fails without consequence on hardware" | Latent on GC. GC hardware shrugs it off, an allocator that clobbers freed memory will not. |
| Grind rail (misc) | "bug introduced sometime after beta 4" | Not platform-specific. Suggests a retail-code regression, likely shared. |
| PAD assignment (misc) | "Xbox resets PAD1 to unmapped on stage start normally" | Xbox controller-port logic. Not GC. |

### A. Fixes for shared game logic (GAME-LOGIC and likely-GAME-LOGIC)

#### A1. ElecCore mission counter skipped (ElecCoreProgram::Exec) - GAME-LOGIC
- Object: ElecCoreProgram (the electric core target object), CountMission::AddCount, MissionManagerTask.
- Player sees: destroying the core does not increment the mission counter in a "destroy N cores" style mission.
- Root cause (their comment): the original sets the mission counter in the wrong branch, so it is set after the mission is cleared. "OG has this too, but its in the wrong branch".
- Fix: wrapper before the original Exec. If the core is flagged destroyed and its HP is at least the total-HP global, and the MissionManagerTask has a current mission and handle, and the mission's "already completed" field is not 1, then set a byte on the mission (+0x14 = 1), then call the mission's virtual AddCount (vtable slot 0x1C) with count 1 and dark affinity 0. Then run the original.
- Always on.
- Find it: ElecCore's Exec method. The object has fields +0x44 destroyed, +0x2E4 HP, global total HP float. Use the MissionManagerTask singleton and the virtual AddCount(count, affinity) on the CountMission class. Compare the original's branch order around the "clear mission" call. Likely identical logic on GC since it is gameplay code.

#### A2. Egg Dealer roulette white flash - GAME-LOGIC (confirmed on GC)
- Object: Egg Dealer roulette UI sprite selection (sub_114650 takes an angle argument).
- Player sees: a one-frame white flash on the roulette UI.
- Cause: the intro cutscene occasionally selects sprite index 6 while only 5 exist, from float rounding when the angle lands on a slot boundary.
- Fix: before the original, convert angle to frame units using two global float constants (a scale and a per-slot frame count). Compute the nearest slot boundary. If the angle is just below that boundary (difference in [0, 0.05)), nudge it to boundary + 0.01 frames so it rounds onto the next slot.
- Always on.
- Find it: the roulette UI function that computes the slot from an angle times scale divided by per-slot frames. Look for the sprite index lookup with 5 entries.

#### A3. Egg Dealer EggLastMechCamera use-after-free - GAME-LOGIC (latent; surfaced by host allocator)
- Object: EggLastMechCamera ctor/dtor plus its update callback and camera-switch callback.
- Player sees: crash after dying in the lava in the middle of the arena, or the roulette-bomb death. Easiest repro per their note: roulette bomb, fall into lava in the center.
- Cause: callbacks run on a camera already destroyed. On hardware reading freed memory fails harmlessly. They crash because their o1heap allocator clobbers 16 bytes of freed block versus 8 on the original.
- Fix: keep a mutex-guarded set of live EggLastMechCamera pointers (insert on ctor return, erase at start of dtor). The update and switch callbacks return immediately (popping their args) if the camera is not in the set.
- Always on. They say other bosses may have the bug but could not reproduce.
- Find it: EggLastMechCamera class (Egg Dealer / Black Comet boss camera). On GC the freed-camera callbacks are probably the same: check whether a callback registered at construct time is removed in the dtor. Worth a guard if the GC allocator poisons freed memory.

#### A4. Animated textures (ADB "TextureChange") sometimes do not play - GAME-LOGIC (confirmed on GC)
- Objects: TTextureChange (setupEntry, destructor), stage-geometry async clump request/completion, RwTexDictionary lookups. Example scenarios: Lethal Highway; green fluid rooms in the space gadget (refcount mismatch).
- Player sees: some texture animations (water, lights, signs) static or missing.
- Cause (their comments): the async stage geometry clump callback never sets the current TXD before creating the object, so the wrong dictionary is current when TextureChange looks up the texture. Plus ref count mismatches on the original texture.
- Fix (several parts):
  1. At the stage GEO request, record which dictionary is current in a 128-entry ring keyed by the stream object.
  2. Right before the clump completion callback creates the object, set the current dictionary to the recorded one (saving the old one), and restore it right after.
  3. In TextureChange setupEntry, add one reference to the original texture (offset +0x54).
  4. In the TextureChange destructor, drop that reference per entry (count at +0x30, entries 0x14 stride from +0x54, texture at entry+8). If refs <= 1 the dictionary was already destroyed, so zero the owner and refcount.
  5. Replace RwTexDictionaryFindNamedTexture: look in the requested dictionary first (case-insensitive), then fall back to every dictionary in the global list. Side effect: events can use textures from shadow.one.
- Always on.
- Find it: TextureChange / ADB TextureChange setup, stage GEO async request and clump completion callbacks, RwTexDictionaryFindNamedTexture, RwTexDictionarySetCurrent (RenderWare is shared on GC too, but addresses and layout differ).

#### A5. EffectDrawGlobalLensFlareTask ctor leaves +0x34 uninitialised - lean GAME-LOGIC
- Cause: the ctor never initialises or frees data at +0x34. If the task dies early the destructor uses garbage.
- Fix: after the ctor returns, zero that field.
- Always on.
- Find it: lens flare task class by name "EffectDrawGlobalLensFlareTask". Likely identical on GC unless the compiler zeroed it. Cheap guard.

#### A6. Grind rail exit "fling" - lean GAME-LOGIC
- Object: grind rail exit state (reads player velocity and rail direction).
- Player sees: leaving a rail (grind/jump off) flings the character oddly.
- Cause: they say a regression introduced "sometime after beta 4"; the jump velocity is applied twice (exit already handles the jump) and the exit velocity direction is stale.
- Fix: two hooks. (1) At grind exit, recompute the player velocity as speed times a direction. The direction is the normalized movement since the previous position when that distance is sane (below 2x speed times |dt|, nonzero), otherwise the rail's stored tangent. (2) Remove the redundant second jump-velocity application, by stubbing the hook that skips it.
- Always on.
- Find it: grind-rail Exit/Jump functions; compare player offsets: pos +4..+0xC, previous pos at +0xD0, velocity +0x1C.

#### A7. BkBird (No Control Lock) not aligning to splines - GAME-LOGIC
- Object: BkBird (Black Arms bird that grabs the player). Stage: Cryptic Castle softlock reduction; "CC Dark strat of bumping into the torch" is deliberately preserved.
- Cause: when the bird has the player locked, the center alignment pull is not applied.
- Fix: after the original per-frame update, if state is locked (state field 0x2 and flag mask 0x404 set), compare the squared offset with the pull "stop" distance (read from the bird's param at +0x34). If outside, set flag 0x20 so the existing align logic runs.
- Always on.
- Find it: BkBird update with states and a "No Control Lock" flag. Likely identical on GC.

#### A8. Per-viewport material animation (auras and vehicles) - GAME-LOGIC (only visible with multiple viewports)
- Objects: shared RpMaterial animations (super/chaos aura effects) and vehicle materials.
- Player sees: material/effect cycling runs faster by the number of viewports drawing the object (splitscreen / extended multiplayer) and vehicle materials cycle at the wrong rate. They call it "effect cycling per viewport (wtf)".
- Cause: advance function is called per viewport but should advance once per frame.
- Fix: brackets (begin/end mid-asm hooks and a wrapper at the vehicle material function) set a call id. In the material animate function, if a call id is active and the effect arg is negative, keep a map material -> (frame, caller). If the same material was already advanced this frame by a different caller, skip. Map is cleared above 4096 entries. Player bracket is only active when Config::ExtendedMultiplayer, the vehicle bracket is unconditional.
- Find it: material animation advance routine used by player draw and vehicle draw; the frame counter global.
- GC relevance: GC split screen has the same issue if the same shared material is drawn twice.

#### A9. Ground-stick tolerance at collision seams (HPP/Westopolis pole bump) - GAME-LOGIC
- Objects: player ground collision (sub_84E50 region), ground ray and sweep, player offsets 0xA8 (grounded flag 0x4), 0x1C8-0x1D0 (ground normal), 0x274 (radius).
- Player sees: tiny bumps/stutter when running over triangle seams and vertices, e.g. the light pole before the Westopolis goal ring, most visible in the Heroes Power Plant port at high framerate.
- Cause: float imprecision at shared triangle edges.
- Fix: if already grounded and the ground normal is valid, and the ground ray misses, re-cast the same ray shifted +/-0.05 along the ground tangents (up to four tries); accept only if within reach (radius x 2.37 constant) and the hit normal is floor-like (cosine vs ground normal at least the global 80-degree value). If the ray hits farther than stick (radius x 2.0) clamp to stick to snap rather than doing the second ray. Plus "snap fallback" (ray found floor but the sweep missed): stay grounded for roughly one frame (0.75/60 s accumulator; streak reset after 1.5/60 s) and use the ray normal if the sweep normal fails the slope check. Normals work under any gravity direction.
- Always on. Fairly invasive and it calls the game's own ray function.
- Find it: the grounded-check function that casts a ray, then a second ray/sweep. The constants are the same game globals as on GC (cos 80 deg, 2.37, 2.0).
- Priority: worth checking for seam bumps on GC.

#### A10. Gravity overhaul (SpinDash / Slide / state transitions) - GAME-LOGIC
- Object: player gravity vector (target gravity up at player+0xE8, velocity +0x1C, euler yaw +0x14, base matrix +0x1E4), Sliding::Exec, SpinDash Exec.
- Player sees: in non-normal gravity (anything other than -Y), spin dash and slide steering rotate wrongly or transitions between gravity states misapply rotation.
- Cause: they say the original "assumed NegY regardless of gravity mode".
- Fix: only active per player when the target gravity is not (0,1,0) and it is a tracked main player (not partner structs).
  1. Replace the FromTo-quaternion helper: compute R = FromTo(up, gravity up), transform the input normal into gravity space with R^-1, apply the original helper there, then output R x result.
  2. Track which player is current by wrapping the gravity transition function and the per-frame gravity update function.
  3. After the steering call in Slide and SpinDash, rotate the velocity by the yaw delta in the base-relative frame (inverse base matrix, rotate by dyaw, back).
- Always on.
- Find it: FromTo quaternion function, gravity transition function, per-frame gravity updater; Sliding::Exec and SpinDash Exec. These also exist on GC with different addresses. Gravity zones are used in a few stages.

#### A11. Coaster hangbar bullet collision - GAME-LOGIC
- Fix: a single mid-asm write sets a collision filter to 0x40000 so bullets can hit the Coaster's bar object. Always on.
- Find it: the Coaster hangbar collision creation, which sets the collision filter flags.

#### A12. Lethal Highway spline stg0202_gr_001 bad last vertex - GAME-LOGIC (data)
- Stage: Lethal Highway grind rail named "stg0202_gr_001" in PATH.PTP.
- Cause: the last vertex of the spline is bad, with a wrong rotation and a stray length.
- Fix: when the path is loaded, find the spline by name (also checks its vertex count 35), subtract the last segment length from total length, copy the previous vertex' rotation to the last one, zero the last segment, recompute the min Y over 34 vertices, and set vertex count 34. Name-based so mods keep the fix.
- Always on.
- Find it: PATH.PTP loader and spline struct (count, total length, vertices 0x20 each). The same data file ships on GC, so confirm the last vertex is also bad.

#### A13. Black Comet / Final Haunt fence polygons across the trench floor - GAME-LOGIC (data)
- Stages: Black Comet and Final Haunt (names STG0601_18_COLI_ and STG0604_18_COLI_, matched by vertex/poly count plus a XXH3 hash of the vertex data, so modded stages are untouched).
- Player sees: at framerates above 60, a small wall lying across the trench floor stops the player until enough speed (tiny bump at 60 FPS).
- Cause: a few floor polygons are authored as fences ("copy paste layout moment").
- Fix: when the stage section COLI world loads, verify the target and source polygons (sorted values), then overwrite each fence polygon's 8 bytes with a neighboring floor polygon's data. The world root sector layout is checked (RpWorld +0x1C, sector counts at +0x82/+0x84).
- Find it: stage COLI loader callback. GC polygon indices/hash will differ, so rebuild the table from GC data. The author credits Heroes Power Plant and Blender.
- GC relevance: matters if the GC port runs above 60 FPS or if the player is slow.

#### A14. BombingWall breaks with Chaos Blast - GAME-LOGIC (probable)
- Object: BombingWall attacker filter. If the attack flag 0x10 is set (as read from ECX) also set 0x200.
- Player sees: Chaos Blast does not break the wall in some stages (they say for "CB").
- Always on.

#### A15. BkTank takes damage once in StopAction - GAME-LOGIC
- Object: BKTank (Black Arms tank enemy) StopAction state; damage callback ignored.
- Fix: a hook returns true (skip) if the state's timer at esi+0x28 is positive, to let the damage callback run.
- Always on.

#### A16. Katana bullet sweep too narrow - lean ENHANCEMENT, but fixes a visible mismatch
- Objects: Bullet types 5 (Katana01) and 6 (Katana02).
- Player sees: the katana wave visual is wide but only a thin line hits enemies.
- Fix: widen the swept segment perpendicular to its motion (ignoring Y) by half widths 9 and 22 units, then reuse the collision check. Computed from old/new positions, writes a temporary buffer.
- Always on.
- Find it: bullet frame update step for weapon types 5/6, directly before the collision sweep call. Likely applies on GC but is a design choice.

#### A17. Egg Pierrot loads the wrong .ONE (stage id typo) - lean GAME-LOGIC
- Cause: the function that picks the .ONE for the stage compares against 501 but should read 511 ("typo (501 -> 511)").
- Fix: mid-asm test equals 511 (stage id in eax) to take the same path.
- Always on.
- Find it: the .ONE selection function that switches on stage id; look for the Egg Pierrot asset names. Check if GC uses the same ids.

#### A18. Jet effect missing 4th flame (Shadow's jet boots) - lean GAME-LOGIC
- Object: anonymous_namespace::Jet::Jet, uses SHADOW_FOOTEFF_R.DFF cloned 3 times into slots +0x5C, +0x60, +0x64; 4th slot (+0x68) is null in the original.
- Player sees: one flame emitter missing on the 4th bone.
- Fix: after the ctor, if first flame exists and slot 4 is null, clone the model and store it. Destructor, show and hide already handle four slots.
- Always on.
- Find it: the Jet effect class with a model-clone call and a 4-entry array. Check whether GC also leaves the slot empty.

#### A19. Title screen FMV flag persists ("OG bug") - lean GAME-LOGIC
- Fix: after the menu manager task hook runs, if the menu mode is TitleScreen set a "kill" bit (bit 0 of byte +0xC) on the MovieManager.
- Always on. Source comment says "OG bug where the FMV flag persists".
- Find it: the opening/title menu task and the movie manager singleton.

#### A20. Vacuum on BkNinja reads freed memory - GAME-LOGIC (latent, gated)
- Object: CEnemyVacuumObject ctor, BkNinja's head-aim callback node.
- Cause: BkNinja puts a head aim callback with the enemy as data on a bone, so sucking it makes the game read freed data and eventually corrupts and crashes.
- Fix: recursively clear callback and callback data on every hierarchy node of the vacuum object's model (depth limit 64).
- Only runs when Config::VacuumAnyEnemy is on (hidden cheat, default false); otherwise the bug is not reachable normally.

### B. Xbox-specific port problems (XBOX-PORT)

#### B1. Subtitle gets transparent after Restart/Quit prompt (Xbox-exclusive alpha) - XBOX-PORT
- Object: MessageManager (global at 0x544F3C), TMessageDisp (+0x10), its brush at +0x58; MessageTaskExec; sub_28D1E0 (SaveMessage helper reused for in-game prompts).
- Player sees: subtitles become more transparent after using the Xbox in-game Restart/Quit prompts.
- Cause: the alpha of the last drawn element is applied to the main brush.
- Fix: at the start of every MessageTaskExec, reset the brush to the default 0xFEFEFEFE color (corner colors set to 254.0, deltas 0, packed color word, clear a flag bit). Also sets a flag during the call (host uses it for aspect ratio).
- Always on. GC has no such prompts so skip.

#### B2. Mipmapped textures break on 3D models (anisotropy plugin) - XBOX-PORT
- Cause: after a level TXD loads, the game sets max anisotropy 4 on every mipmapped texture, but the anisotropy plugin was never attached on Xbox so the texture ends up bound as null.
- Fix: drop the anisotropy write (function returns the texture) and let the host handle filtering.
- Always on. Find it: RwTextureSetMaxAnisotropy. GC does not use this.

#### B3. DMA vertex pack overflow (rpXboxVtxFmtInstV3d) - XBOX-PORT
- Object: Xbox vertex-format instancing for packed (DMA, type 4) normals.
- Player sees: bad lighting or normals (commander underflowed, Shadow overflowed in the Maria event).
- Cause: the original packs signed values into 11/11/10 bits with no clamping.
- Fix: replacement loop. Multiply x,y by 1023 and z by 511 (as ints), clamp both sides to +/-1023, +/-511, pack as (z<<22)|(y&0x7FF<<11)|(x&0x7FF). Other formats go through the original.
- Always on. No GC equivalent (Xbox-only vertex packing).

#### B4. Synopsis skip button hidden - XBOX-PORT
- Object: CSD "csdfiles/synopsis.xncp/button/skip_btn" and ".../skip_btn/skip_txt".
- Cause: Xbox sets a hide flag that GC does not.
- Fix: when those casts are created, clear the first dword of the cast info (offset +0x30 of the cast).
- GC: "Originally on GameCube, the synopsis shows a skip button" so already fine on ours.

#### B5. PAD1/PAD2 port assignment - XBOX-PORT
- Fix: force PAD1 = port 0 and PAD2 = port 1 on the PAD manager init, the Advertise screen, and the Battle mode char/stage select backout paths (globals 0x5356E4/0x5356E8). Also fixes Partner Control since "Xbox resets PAD1 to unmapped on stage start normally". Port value 5 means any controller.
- Always on.
- GC: not applicable.

#### B6. Enemy hidden state / collider init (XboxEarlyTargetBehavior) - XBOX-PORT
- Objects: EnemyStatusCommon slot 0 (sub_CCC90), enemy collider creation (sub_CE6C0), BkWorm and BkNinja ("WaitAppear" enemies), EnemyCollider state hook.
- Player sees: homing attack targets BkWorms out of bounds or BkNinjas in the warping state, and WaitAppear enemies can be homed on.
- Cause: the Oct 16 Xbox build changed this by accident. The Oct 12 and earlier Xbox builds created colliders on init; GC "inits these at 0,0,0".
- Fix: default build creates the colliders in EnemyStatusCommon slot 0 and zeros the first collider vector fields (+0x8, +0xC, +0x10 for the first two) as GC does. Then a hook returns true on the enemy collider state check unless Config::XboxEarlyTargetBehavior is on.
- Default false = fix applied. True restores Oct 16 behavior.
- GC: already correct, so this one tells us what the right behavior is. Compare enemy hidden state handling against GC.

#### B7. Lock-on laser hides drop shadows - uncertain, lean XBOX-PORT/RenderWare state
- Object: KageTask (draws SDW_ALL).
- Cause: KageTask inherits the previous render state, so the laser's alpha leaks into SDW_ALL.
- Fix: before KageTask runs, if the render-state setter global is non-null, force four RW states: ids 9, A, B, C with values 2, 5, 6, 0 (blend modes and so on).
- Always on. GC uses a different render path so skip unless seen.

#### B8. Gadget fade white flash (SetFadeAlpha / _A vs _AL) - uncertain, lean XBOX-PORT
- Object: gadgets via sub_17E290 ObjectUpdateVisibility, SetFadeAlpha, fade end, gadget atomic render, render-layer table (_O, _OS, _A, _AL...).
- Player sees: a white flash and a frame where an object appears at origin ("dropped frame") when gadgets fade.
- Cause: fading moves clumps into world 10 (_AL), which applies light; most gadgets should use _A. Also moving worlds triggers RW LTM sync that resets the object to origin.
- Fix: around fade begin/end save frame private flags; after the original, clear the newly set LTM sync flags. Record clumps that were in an unlit layer. When rendering, temporarily set the current world back to the original unlit one (keeps _AL backface culling). Lit-flagged gadgets are skipped.
- Always on. Find it: SetFadeAlpha function and fade-end. GC may use similar logic but different render passes.

### C. Recomp host / threading (RECOMP-HOST)

None of these apply to a real console or a single-threaded port. They exist because the Xbox is single-core and the recomp runs the loading thread in parallel. Always on.
- RwFrameUpdateObject and RwFrameSyncDirty: mutex around the dirty frame list (global 0x57DEBC) which two threads modify during loading.
- RwTexDictionarySetCurrent/GetCurrent/Destroy plus inlined GetCurrent hooks: per-thread "current texture dictionary" so the loading thread cannot swap the dictionary under another thread (null textures).
- CSDPushNewNode / CSDPopNode: spin/sleep gate (100 ms, up to 300 times) plus critical section so only one thread transforms CSD nodes at a time (they say a mutex alone does not work), and a null check in the node transform function.
- FMV: critical section and null guard around two MWSFSVR calls (rare crash when skipping an FMV).
- CameraBeginUpdate/EndUpdate and WorldCameraBeginUpdate/EndUpdate: critical sections, and a null check on the global current world in the camera function (sub_2D4A90). RwCameraFrustumTestSphere has a debug-only check.
- Task::Task and Task::~Task: a global mutex (the task list is mutated by main thread and the async loader; the "Start Infinite Loading Fix"). Layout same as GameCube.
- CNowLoadingThread destructor: only clears the NowLoading manager's thread pointer if it still points at this thread (otherwise a late dtor wipes a new thread's pointer). Seen only with their experimental boot-to-event.
- Debug print stubs: several sub_XXXX with 4-byte pops (Artificial Chaos logs, leftover debug printf-style functions) are nulled because they crash or spam; one "m_eDealloctor not initialized" function is left as a hard lock.
- Boot stubs (start_patches): TryOpenVolume, MapLetterToDirectory, XMountUtilityDrive, XAPILIBXGetParentalControlSetting and one more startup function are stubbed to succeed so the game boots.

### D. Enhancements, cheats and options (ENHANCEMENT)

| Item | What it does | Gate / default |
|---|---|---|
| Hints toggle (HintRingExec) | Hide the "?" hint capsule if Hints off; uses game's hide flag | Config::Hints, default true |
| Subtitles toggle | Skip the message draw call when Subtitles == 0 | Config::Subtitles, default true |
| EnablePartnerIntro | Skip the partner intro | default true |
| Always skip cutscenes | Clears a "cannot skip" byte after two cutscene init functions | always on |
| Expert / Last Story credits skip | Forces a "can skip" arg | always on |
| Always-allow logo skip | Removes the title logo skip check | always on |
| Quick Boot | Forces boot intro to title screen | Config::QuickBoot (hidden), default false |
| Boot to stage / event (--l, --e id/all) | Command-line options that redirect game mode to StageSequence with a custom event playlist from the story table | CLI only |
| Quit prompt on title, update-available prompt | Title screen B opens a quit message after a 4 s lockout | always on |
| Options menu replacement | Replaces the game options screen with the host UI | always on |
| Pause menu: Achievements and Options entries | Adds two items, B to resume, fonts and offsets for mission text, aspect ratio fixes. Restart hidden when game hides it (2P or no lives). | always on |
| F8 toggle HUD | Toggles the HUD flag (0x54DE78) | always on |
| AllStagesUnlocked | Forces the "only unlocked" argument to 0 on stage, route and boss lookups and the "visited" check | Config::AllStagesUnlocked (default false) |
| EasyLastStoryUnlock / ReloadedMod | Lower the thresholds to unlock Last Story (5 or 3) | default false |
| Select Mode Expert Stages / Last Story + Devil Doom slots | Hold X for Expert, extra menu slots | default false (or ReloadedMod) |
| AllPartnersCanHomingAttack | Flags in the character table | default false |
| HoldLTForceJumpDash | LT swaps HomingAttack behavior (id 8) for JumpDash (6) | default false (or ReloadedMod) |
| Chaos gauge fill (Black + RT / LT, F6/F7) | Fills the hero or dark gauge | default false |
| InfiniteAmmo / InfiniteLives | Turn negative deltas into 0 | default false |
| VacuumAnyEnemy | Sets collision flag 0x80 and skips BkWingLarge/BkChaos resistance | default false |
| VacuumGivePointsOnFireSuck | Calls the ScoreCommand on sucking fire | default false ("debatable if this was a bug") |
| PlayAsBattleCharacters / ExtendedMultiplayer | Characters 0x0F-0x13 | default false |
| ShadowAndroidsNoEmbeddedWeapons | Skips embedded gun/bazooka flags | default false |
| SkipLoadingAnyEvents | Skips event loading | default false |
| SingleFrameInvulnRecovery | Damage recovery in one frame | default false |
| Ball switch trigger (extend to melee) | Accept melee swing (0x80) and generic melee (0x2) attacker flags | always on; arguably a game quirk fix |
| Fade delta-time (sub_17E290) | Scales fade/anim rate by dt*60 (capped at 1) | always on; part of the high-FPS support |
| Motion system for mods (motion_patches) | Looks up animations named "<skeleton>_<motion>" so custom skeletons and anims work; validates track counts; event query swap; BON id fix-up after load | always on; modding support, not a bug fix |
| limit_break_patches | Everything commented out (loadAllChunks, level geo unrestrict, a BVH stack overflow redirect marked "corrupts memory") | inactive |
| Misc debug | RwSetTextureName probe, texture names (E630A_c1, memory01-07) | no effect |

### E. Where to look first on GC

The 8 GAME-LOGIC fixes most worth checking on the GameCube build: A1 (ElecCore mission counter), A2 (roulette sprite 6), A3 (EggLastMechCamera use-after-free), A4 (TextureChange / stage TXD), A7 (BkBird), A9 (ground seam tolerance), A10 (gravity overhaul), A13 (BC/FH fence polygons).

### F. Summary counts (entries as listed above)

- GAME-LOGIC: A1-A20 (A5, A6, A17, A18, A19 are "lean") = 20
- XBOX-PORT: B1-B6 = 6, plus B7, B8 (uncertain) = 8
- RECOMP-HOST: section C groups, 9 distinct items
- ENHANCEMENT: section D, 30 table rows
