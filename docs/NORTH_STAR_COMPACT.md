# NORTH STAR - compact (read this whenever context is lost or the direction is unclear)

Asked for by the user 2026-10-07. A distillation of docs/NORTH_STAR.md (the user's words, verbatim) - the full text
outranks this page; when in doubt, read it.

## The goal
**MAKE THE ORIGINAL GAME CODE UNDERSTAND THE MODERN MACHINE. DO NOT MAKE THE MODERN MACHINE PRETEND TO BE THE ORIGINAL
CONSOLE.** Shadow the Hedgehog (GameCube PAL, GUPP8P) is being reconstructed as a genuine native 64-bit PC game. The
GameCube binary is the BEHAVIOURAL REFERENCE, not the architecture. "The GameCube is the reference; the PC version is
the new engine."

## Preserve / replace
- Preserve ONLY: gameplay behaviour, game rules, algorithms, asset meaning, VISUAL INTENT, required timing semantics,
  determinism where required.
- NOTHING TO KEEP structurally: memory, pointers, allocation, renderer, resources, IO, streaming, threading, sync, audio,
  platform code - and this project's own runtime. A component survives only if a new 64-bit PC engine would choose it.

## Forbidden as a final solution
Fake / enlarged MEM1, GuestPtr, AddressTranslator, fake 32-bit address space, compatibility RAM, emulated heaps,
permanent wrappers / shims / translators, "native" APIs that forward into the old system, GX emulation, a permanent
GX -> D3D12 translator, RenderWare-GameCube internals under a D3D12 front, a fake EFB as engine design, legacy kept
because it works, symptom patches (magic offsets, compensation constants, "looks close enough").
Scaffolding is allowed ONLY when it exists to delete itself: marked ("SCAFFOLDING (R2): ... deleted by <step>"), never
extended with new capability.

## Core rule
**DO NOT CONVERT, WRAP OR HIDE THE PROBLEM - CHANGE THE CODE THAT HAS THE PROBLEM.** Port the MEANING, not the hardware
mechanism. Ask "what was this code actually trying to accomplish?", not "how do I make the old thing work here?".

## The method, part by part (the user's loop)
DISASSEMBLE -> TRACE -> RECONSTRUCT C/C++ SEMANTICS (ownership, pointers, lifetimes, buffers, sync, IO) -> TRUE INTENT
-> RESEARCH modern solutions -> DESIGN for Shadow -> REWRITE NATIVELY in the real game code -> RUN -> COMPARE OLD vs NEW
(mandatory; find the FIRST divergence) -> FIX THE ROOT CAUSE -> VALIDATE -> DELETE LEGACY -> next part.
Do not guess. Do not translate assembly blindly. Understand first.

## Exact native math (the user's directive, 2026-10-29)
"Do some research, find the way to replace systems with identical behavior lightweight x64 PC math. No cheating, no
fake results. Cut the obsolete systems out and replace them with brand-new ones written from scratch."
The method is docs/NATIVE_MATH_METHOD.md:
- Identical means the same output bits on every input class, plus the same side effects.
- The game's own algorithm is written in C++ in its own operation order. Fused stays fused. Never the CRT's sin,
  atan2 or pow.
- Pin, contract, from-scratch rewrite, original-word oracle, replay gate with per-call verification, then the old
  body is pruned from the link.
- Unported paths fail closed; they never fall back to the old code.
Its section 4 is the ranked worklist.

## The tests (a part is done only when every answer is right)
1. If every GameCube compatibility layer disappeared now, would it still work?
2. Could it have been written from scratch in 2026 for a high-performance 64-bit PC game?
3. Did I modernize the implementation, or put modern-looking code around the old one?
4. Am I fixing the native system, or escaping back into the old architecture?
Never call the game "native" while its code still runs translated (~98.5 % of its functions still do).

## Renderer decisions (the user's)
GO NATIVE: visual intent, not console artefacts (no 320x240 history, deflicker, forced point sampling, missing AA);
native output resolution and modern image quality. Direct D3D12 (plume not needed), Shader Model 6.6 minimum,
two-player split screen in scope. Rendering bugs are fixed in the native renderer - never by bringing GX back.
Validation = behaviour equality + reviewed visual intent + byte-exact self-determinism, not pixel parity with the old
renderer.
FRAMING (decided 2026-10-07): SQUARE PIXELS + the real output aspect are the DEFAULT; `--framing game` (the console's
framing) is an optional reference mode that must never constrain the architecture. Everything screen-related is
decided against the ACTUAL ACTIVE VIEWPORT of each camera (split screen, any aspect: 4:3 .. 32:9) - no 4:3 constants,
no fake 4:3 region. Authored camera composition, where proven, is kept as camera semantics, never as a global
pixel-aspect hack. Effects are rebuilt by intent (trail -> history texture, heat haze -> scene-colour refraction,
screen colour -> compositing, flare -> projected light + visibility / occlusion, glare -> native pass).
Evidence levels: PROVEN / STRONGLY SUPPORTED / STRUCTURAL MATCH / UNKNOWN.

## Validation without cheating
Fix a noisy field's cause; never exclude fields, cherry-pick runs, weaken tests or claim past a failed gate; attribute
every difference. After a migration: build, CTest, captures, compare, D3D12 validation, lifetimes, determinism,
performance, delete dead code, update the docs.

## Working rules
- After a context loss: read README.md and docs/ACTIVE_SOURCE_LINEAGE.md FIRST; the repository
  is the truth; never redo completed work; never trust a stale plan over working code.
- Update the handoff ON THE FLY after every finding (the user: "Create handoffs on the fly").
- A running ShadowTheHedgehog* process the user started = the user is playing: no gates, benchmarks or CTest
  meanwhile. For a fresh BUILD it may be closed without warning (the user, 2026-10-28); never touch its card or cfg.
- Deploy to the root ShadowTheHedgehog.exe ONLY when asked (dated backup + replace + own-named copy). Test builds go
  to the root under their own name; launch for the user (--fullscreen --audio, the -full card) when asked. Never touch
  the user's cards or saves.
- Edit the single active source tree in place, preserving line endings. Do not accumulate source snapshots or experiment archives.
- Before trusting a Dolphin reference, read the user's Dolphin GameSettings/GUPP8P.ini: an enabled Gecko cheat
  (`$16/9 screen ratio`) once produced a false "console" FOV.
- A remap / ratio applied to the old code's RESULT (e.g. u' = 1/2 + (u - 1/2) w / a) is a compensation around the old
  code (test 3) - the user had to say "Read north star compact" (2026-10-07). Find the ONE place that has the problem
  and change it there (that time: the camera sync, which gave the renderer's camera a window it did not draw).

## Current workspace (2026-10-23)

The user requested one active source tree, one deployed game, one comparator reference, the RenderWare SDK, and no historical experiments or output archives. Source is in source/; playable files and personal data are in game/; RW37SDK/ is the research SDK. The latest combined source was preserved from the 2026-10-21 candidate; its complete game validation was pending. Retained executable identities and build commands are in README.md. Historical scratch and resource-reconstruction campaign paths no longer exist.

## Current work (2026-10-26)

Optimization increments 21-23 are documented in OPTIMIZATION_RESULTS.md. The next session starts from
docs/NEXT_SESSION.md (handoff and the CPU/GPU/RAM plan under the user's 2026-10-26 modernization directive).
