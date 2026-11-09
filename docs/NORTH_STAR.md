# NORTH STAR - READ THIS AGAIN ANY TIME YOU ARE UNSURE OF THE DIRECTION

(A compact version for quick re-reading: docs/NORTH_STAR_COMPACT.md - asked for by the user 2026-10-07. This full,
verbatim text outranks it.)

The user's words, verbatim (2026-10-06, third session). This file outranks
every plan, design note and handoff in this repository: when a document here
disagrees with it, the document is wrong.

---

We are NOT emulating the GameCube.

We are NOT patching around GameCube limitations.

We are NOT translating native PC pointers back and forth through fake 32-bit addresses.

We are NOT building a larger fake MEM1.

We are NOT preserving obsolete hardware architecture merely because the original game expected it.

We are teaching the ACTUAL GAME CODE to become a real native 64-bit PC program.

Whenever you encounter an original GameCube assumption, ask:

"What did this code actually need to accomplish?"

Then implement that behavior natively for modern PC hardware.

If the original code assumes:
- 32-bit pointers → make the actual code 64-bit correct.
- MEM1 → replace the dependency with native memory ownership.
- ARAM → replace its actual purpose with native storage/resource/audio systems.
- fixed addresses → remove the address dependency.
- GameCube heaps → replace them with native allocation/lifetime management.
- hardware registers → replace the underlying behavior with native platform functionality.
- DMA → implement the real data movement/ownership semantics natively.
- console-specific resource limits → eliminate them where they are not gameplay rules.

DO NOT solve architectural problems with wrappers whose purpose is to preserve the old architecture.

DO NOT create a permanent GuestPtr/AddressTranslator/fake-address world and call that 64-bit support.

Temporary migration scaffolding is acceptable ONLY if it exists to help delete itself.

The desired dependency direction is always:

```
GAME CODE
    ↓
NATIVE PC SYSTEM
```

NOT:

```
GAME CODE
    ↓
COMPATIBILITY LAYER
    ↓
FAKE GAMECUBE
    ↓
PC
```

The final executable must behave like a game whose engine was genuinely ported and rewritten for modern 64-bit PC hardware.

The original GameCube binary is a behavioral specification, NOT the architecture of the finished program.

Preserve:
- gameplay behavior
- algorithms
- data meaning
- timing semantics where required
- deterministic behavior where required
- asset interpretation
- game rules

Replace:
- hardware assumptions
- memory architecture
- pointer-width assumptions
- fixed-address dependencies
- console allocation systems
- obsolete resource limits
- hardware transport mechanisms

Most importantly:

DO NOT CONVERT THE PROBLEM.

CHANGE THE CODE THAT HAS THE PROBLEM.

If a native function still fundamentally thinks it is running on a GameCube after migration, that migration is not finished.

The game itself becomes 64-bit.

Native pointers.
Native ownership.
Native allocations.
Native resources.
Native threading.
Native IO.
Native rendering.
Native audio.
Native platform behavior.

No emulation.
No cheating.
No architectural cosplay.

When in doubt, return to this rule:

MAKE THE ORIGINAL GAME CODE UNDERSTAND THE MODERN MACHINE.
DO NOT MAKE THE MODERN MACHINE PRETEND TO BE THE ORIGINAL CONSOLE.

---

## What it settles in the current work (recorded with it)

* The M1 address window (`gekko/guest_memory.h`) and everything that keeps
  the translated PowerPC code running (the Cpu, the interpreter, the
  generated shards, the native OS exception entry) are TEMPORARY MIGRATION
  SCAFFOLDING. They are allowed only because they let the unrewritten game
  run while its code is rewritten, and each rewrite deletes part of them. No
  new capability is built on them; nothing may depend on them that is not
  itself scheduled to be rewritten.
* "Rewritten" means the game's own code, as native C++ that owns native
  pointers, allocations, sizes and objects, and talks directly to native PC
  systems - not a native function that still takes 32-bit addresses and
  reaches guest memory through the window.
* The five directives and the decision this follows from:
  `docs/memory-architecture-20261006/DIRECTIVE.md`.

---

## Reminder from the user (2026-10-06, verbatim, sent twice)

REMINDER — DO NOT DRIFT BACK INTO GAMECUBE THINKING

The target is NOT “make the old GameCube memory model work on PC.”

The target is:

THE REAL GAME CODE BECOMES NATIVE 64-BIT PC CODE.

Every time you are about to add:
- fake addresses
- address translators
- guest pointers
- enlarged MEM1
- compatibility RAM
- 32-bit pointer preservation
- emulated hardware ownership
- permanent conversion layers

STOP.

Ask instead:

“Why does this code still need to think like a GameCube at all?”

Then rewrite that actual code path so it directly uses the modern native equivalent.

Do not preserve the old architecture and hide it behind abstractions.

Do not translate around the limitation.

REMOVE THE LIMITATION FROM THE CODE THAT CONTAINS IT.

A successful migration means the rewritten subsystem no longer needs to know:
- what MEM1 was
- what ARAM was
- what a GameCube address looked like
- that pointers used to be 32-bit
- that the original hardware had fixed memory regions

Preserve behavior.
Replace infrastructure.

Temporary compatibility code is allowed only as disposable scaffolding on the way to native ownership. It is NOT an acceptable final architecture.

The final question for every migrated subsystem is:

“Could this code have been written today for a 64-bit PC without knowing that the GameCube ever existed?”

If the answer is no, the migration is not finished.

THE GAME MUST LEARN THE PC.

THE PC MUST NEVER PRETEND TO BE THE GAMECUBE.

---

## From the user (2026-10-06, verbatim)

No wrapper, no emulation.

---

## Second reminder from the user (2026-10-06, verbatim)

REMINDER — NO WRAPPERS. NO FAKE SOLUTIONS. NO BULLSHIT.

The target is NOT to make the old GameCube architecture appear compatible with PC.

The target is to rewrite the ACTUAL GAME CODE so it becomes genuine native 64-bit PC code.

STRICT RULES:

- NO wrapper architecture as the final solution.
- NO GuestPtr.
- NO AddressTranslator.
- NO fake MEM1.
- NO enlarged MEM1.
- NO fake 32-bit address space.
- NO compatibility RAM.
- NO permanent conversion shim.
- NO “native-looking” layer hiding GameCube assumptions underneath.
- NO preserving broken 32-bit assumptions behind abstractions.
- NO pretending something is native when it is still fundamentally using the old architecture.

If the solution only wraps, translates, redirects, masks, maps, or disguises the original GameCube dependency, then the problem has NOT been solved.

Rewrite the code that has the wrong assumption.

If the code assumes 32-bit pointers, rewrite it for real 64-bit pointers.

If the code assumes fixed GameCube addresses, remove that assumption.

If the code assumes MEM1, give it real native ownership and allocation.

If the code assumes ARAM, replace the actual purpose of ARAM with the proper native subsystem.

If the code assumes hardware-specific transfer behavior, implement the real required behavior natively.

DO NOT CONVERT AROUND THE PROBLEM.

DO NOT WRAP THE PROBLEM.

DO NOT HIDE THE PROBLEM.

DELETE THE ASSUMPTION THAT CREATES THE PROBLEM.

Temporary migration scaffolding is acceptable only if it is explicitly disposable and actively being removed.

A wrapper is NEVER considered completion.

A compatibility layer is NEVER considered the destination.

The final code must directly operate as native 64-bit PC code without needing to pretend that GameCube memory or GameCube addresses still exist.

Use this test constantly:

“If I removed every GameCube compatibility layer tomorrow, would this subsystem still work?”

If the answer is no, the subsystem is not truly migrated.

Another test:

“Could this subsystem have been written from scratch today for a modern 64-bit PC?”

If the answer is no, keep rewriting.

Preserve game behavior.

Do NOT preserve obsolete architecture.

NO WRAPPERS.
NO EMULATION.
NO FAKE ADDRESSES.
NO ARCHITECTURAL CHEATING.
NO CLAIMING SUCCESS UNTIL THE REAL GAME CODE ITSELF IS NATIVE.

MAKE THE GAME UNDERSTAND THE PC.

DO NOT MAKE THE PC PRETEND TO BE A GAMECUBE.

---

## The two tests, applied to every lift (docs/lift-20261006/LIFT_PROGRAMME.md)

1. "If I removed every GameCube compatibility layer tomorrow, would this subsystem still work?"
2. "Could this subsystem have been written from scratch today for a modern 64-bit PC?"

A lift is reported as done only when both answers are yes for the lifted code, and progress is never described as "the game is native" while any of the game's code still runs translated.

---

## Third reminder from the user (2026-10-06, verbatim)

REMINDER — GO TO THE REAL CODE. RECONSTRUCT IT PROPERLY.

Do NOT stop at wrappers, shims, translators, compatibility layers, fake address spaces, or surface-level patches.

Go all the way down to the actual reconstructed/decompiled C/C++ logic.

Understand what the original PowerPC/GameCube code is REALLY doing at the C-level:

- data ownership
- pointer semantics
- allocation behavior
- lifetimes
- object relationships
- buffers
- resource references
- offsets
- synchronization
- IO
- streaming
- temporary memory
- hardware-dependent behavior

Do not preserve an old implementation merely because it already exists.

Reverse the behavior down to understandable C-level semantics first.

Then reconstruct that behavior as if the game were being engineered TODAY as a native modern 64-bit PC application.

The required workflow is:

ORIGINAL PPC / RECOMPILED CODE
→ understand exact behavior
→ reconstruct clear C-level semantics
→ identify the true intent of the subsystem
→ research modern state-of-the-art solutions
→ design the best native 64-bit PC equivalent
→ implement it directly into the real game code
→ remove the obsolete GameCube implementation

DO NOT mechanically translate old code.

RECONSTRUCT IT.

Research current best practices and modern engine architecture before deciding how each major subsystem should work.

Investigate modern approaches to:

- 64-bit memory management
- virtual memory
- allocators
- arenas and pools
- resource ownership
- handles versus pointers
- async asset loading
- asset streaming
- multithreading
- job systems
- lock contention
- NUMA/cache behavior where relevant
- memory-mapped IO
- large-file support
- GPU resource ownership
- staging/upload memory
- audio streaming
- lifetime management
- ECS/DOD where genuinely beneficial
- modern C++ ownership patterns
- profiling and memory instrumentation

Use current high-performance PC engines, systems programming practices, platform APIs, technical papers, documentation, and proven production architectures as research references.

Do NOT blindly copy an engine architecture.

Understand the problem first and choose the best solution specifically for Shadow.

STRICTLY FORBIDDEN AS FINAL SOLUTIONS:

- fake MEM1
- enlarged MEM1
- GuestPtr
- permanent AddressTranslator
- fake 32-bit address spaces
- pointer wrapping
- compatibility RAM
- emulated GameCube heaps
- permanent GameCube allocation semantics
- “native” APIs that secretly forward into the old system
- wrappers whose only purpose is hiding legacy architecture
- patching individual symptoms while leaving the root assumption intact

NO WRAPPERS.
NO FAKE SOLUTIONS.
NO ARCHITECTURAL CHEATING.

If an original routine assumes a GameCube-specific concept, descend far enough into the code to understand WHY that concept existed.

Then replace the requirement itself.

Example philosophy:

Do not ask:
“How do I make this 32-bit GameCube pointer work on 64-bit PC?”

Ask:
“What does this pointer represent, who owns the object, what is its lifetime, and how should that relationship exist in a modern 64-bit program?”

Then reconstruct it accordingly.

Do not ask:
“How do I emulate this MEM1 allocation?”

Ask:
“Why is this data allocated, who owns it, how long does it live, and what modern allocation strategy best serves that purpose?”

The old GameCube code is evidence of BEHAVIOR.

It is NOT the required architecture.

Preserve:
- gameplay
- algorithms
- intended state
- timing semantics where necessary
- resource meaning
- deterministic behavior where required

Reconstruct:
- memory architecture
- pointer model
- resource ownership
- IO
- threading
- streaming
- allocation
- hardware interaction
- platform infrastructure

The final implementation must use genuine native 64-bit pointers, native sizes, native ownership, native resources, and native PC facilities directly.

There must not be a hidden GameCube underneath.

Use this test after every subsystem:

“Did I modernize the actual implementation, or did I merely put modern-looking code around the old implementation?”

If it is the latter, KEEP GOING DEEPER.

Another test:

“Could this reconstructed subsystem plausibly have been written from scratch in 2026 for a high-performance 64-bit PC game?”

If not, research further and redesign it.

Do not be afraid to replace large amounts of legacy code when the semantics are understood and tests prove behavioral equivalence.

GO DOWN TO C-LEVEL SEMANTICS.

UNDERSTAND THE ORIGINAL INTENT.

RESEARCH THE BEST MODERN SOLUTION.

RECONSTRUCT IT AS REAL 64-BIT PC CODE.

NO WRAPPERS.
NO PRETENDING.
NO SHORTCUTS.

---

## The four tests, applied to every lift

1. "If I removed every GameCube compatibility layer tomorrow, would this subsystem still work?"
2. "Could this subsystem have been written from scratch today for a modern 64-bit PC?"
3. "Did I modernize the actual implementation, or did I merely put modern-looking code around the old implementation?"
4. "Could this reconstructed subsystem plausibly have been written from scratch in 2026 for a high-performance 64-bit PC game?"

The required workflow (original code -> exact behaviour -> C-level semantics -> true intent -> research -> design -> implement in the real game code -> remove the GameCube implementation) is the method of `docs/lift-20261006/LIFT_PROGRAMME.md` section 1.

---

## Course correction from the user: the renderer too (2026-10-06, verbatim)

CRITICAL COURSE CORRECTION — REWRITE THE RENDERER TOO

You are still thinking too narrowly.

This is NOT only a memory-system modernization.

If the rendering architecture is still fundamentally based on GameCube GX, RenderWare/GameCube assumptions, legacy command semantics, fake hardware behavior, translated state, or compatibility ownership, THEN TEAR THAT SYSTEM APART TOO.

Do not protect the existing renderer merely because it currently produces an image.

Go down to the original game/rendering logic at C/C++ semantic depth.

Understand:

- what each GX call actually means
- what RenderWare is trying to express
- material behavior
- TEV intent
- texture usage
- geometry ownership
- skinning
- transforms
- lighting
- blending
- depth behavior
- alpha testing
- culling
- render ordering
- passes
- camera behavior
- post-processing
- framebuffer/EFB semantics
- resource lifetime
- CPU/GPU synchronization

Then reconstruct those INTENTIONS as a modern native 64-bit PC renderer.

Do NOT preserve GX as the architecture.

Do NOT preserve RenderWare GameCube internals as the architecture.

Do NOT build a permanent GX wrapper.

Do NOT build a translation machine that spends the rest of the project's life converting GameCube render commands into PC commands.

Do NOT call that a native renderer.

The final architecture should be something that could genuinely have been designed for a modern PC engine.

Research current state-of-the-art rendering architecture before designing it.

Investigate modern approaches to:

- DirectX 12 / Vulkan style explicit rendering
- render graphs
- GPU-driven rendering
- bindless resources
- descriptor management
- PSO/material caching
- shader permutation management
- modern material systems
- resource residency
- upload/staging systems
- asynchronous GPU transfers
- multithreaded command recording
- modern skinning
- instancing
- batching
- visibility/culling
- indirect drawing where beneficial
- persistent GPU resources
- frame allocators
- transient render resources
- pipeline synchronization
- modern texture/mesh streaming
- frame pacing
- high refresh-rate rendering

The workflow must be:

ORIGINAL PPC / GX / RENDERWARE BEHAVIOR
→ reverse to understandable C/C++ semantics
→ determine visual/gameplay intent
→ research modern rendering solutions
→ reconstruct the renderer natively
→ migrate game rendering code onto it
→ remove obsolete GX/RenderWare/GameCube infrastructure

The SAME RULE applies to memory:

Do not wrap the GameCube system.

Rewrite the actual code.

The SAME RULE applies to rendering:

Do not wrap GX.

Rewrite the actual rendering architecture.

If some existing renderer component is genuinely modern and well designed, keep it only after proving that it is native architecture rather than a compatibility mechanism.

Do not be afraid to delete and replace large parts of the current rendering implementation.

Behavioral and visual correctness matter.

Architectural preservation does NOT.

Preserve the intended final pixels and game behavior.

You do NOT need to preserve the mechanism that originally produced them.

NO GX EMULATION.
NO PERMANENT GX TRANSLATION LAYER.
NO RENDERWARE CONSOLE ARCHITECTURE HIDING UNDER A DX12 FRONTEND.
NO FAKE EFB AS A FUNDAMENTAL ENGINE DESIGN.
NO WRAPPERS AS THE FINAL SOLUTION.
NO PRETENDING.

GO DOWN TO THE REAL C/C++ LOGIC.

REVERSE THE INTENT.

THEN REBUILD BOTH:

1. MEMORY ARCHITECTURE
2. RENDERING ARCHITECTURE

AS GENUINE MODERN 64-BIT PC SYSTEMS.

The final game should not be:

GAMECUBE ENGINE → TRANSLATORS → PC

It should become:

SHADOW GAME LOGIC
        ↓
MODERN NATIVE ENGINE SYSTEMS
        ↓
64-BIT PC / MODERN GPU

If removing the GameCube compatibility infrastructure would destroy the renderer, then the renderer has NOT yet been ported.

KEEP GOING UNTIL THE GAMECUBE RENDERING ARCHITECTURE IS NO LONGER REQUIRED.

---

Applied: the lift programme has two architecture tracks - the memory architecture and the rendering architecture - besides the leaf lifts (`docs/lift-20261006/LIFT_PROGRAMME.md` section 5). The current native renderer is audited component by component; a component stays only after it is shown to be native architecture rather than a GX / RenderWare compatibility mechanism.

---

## Fourth reminder from the user (2026-10-06, verbatim)

REMINDER — FULL NATIVE RECONSTRUCTION. DO NOT DRIFT.

Stop preserving GameCube architecture.

This project is NOT:

GameCube code
→ wrapper
→ translator
→ compatibility layer
→ modern API

It must become:

Shadow game logic
→ genuinely modern native 64-bit PC engine systems

This applies to EVERYTHING, including MEMORY AND RENDERING.

NO wrappers.
NO fake MEM1.
NO GuestPtr.
NO fake 32-bit addresses.
NO permanent address translation.
NO GX wrapper as the final renderer.
NO permanent GX→DX12 command translation architecture.
NO RenderWare/GameCube internals hidden underneath modern-looking APIs.
NO compatibility system presented as modernization.
NO architectural cheating.

GO DEEPER.

Reverse the original PPC/generated code down to understandable C/C++ semantics.

Determine what the code ACTUALLY intends to accomplish.

Then reconstruct that functionality as modern 64-bit PC code.

For rendering:

DO NOT ask:
“How can I translate this GX call?”

Ask:
“What rendering operation was the game trying to express?”

Recover:

- geometry
- materials
- shaders
- TEV intent
- textures
- transforms
- skinning
- lighting
- render passes
- blending
- depth
- alpha
- culling
- ordering
- cameras
- framebuffer behavior
- resource ownership
- synchronization

Then implement those concepts directly in a modern renderer.

If necessary, TEAR THE CURRENT RENDER SYSTEM APART AND REWRITE IT.

The goal is NOT a better GameCube renderer.

The goal is a NEW PC RENDERER FOR SHADOW.

Research modern 2026 rendering and engine techniques before committing to architecture:

- explicit DX12/Vulkan-style resource ownership
- modern render graphs
- bindless resources
- GPU-driven rendering where appropriate
- modern shader/material systems
- PSO management
- async uploads
- GPU residency
- multithreaded command generation
- modern mesh/texture streaming
- modern skinning
- batching/instancing
- visibility systems
- frame resources
- synchronization and fences

Do the same for memory and every other legacy subsystem.

The original code is a SOURCE OF SEMANTICS.

It is NOT the architecture we are required to preserve.

Preserve what the game DOES.

Replace HOW the GameCube did it.

Use this test constantly:

“If all GameCube compatibility code disappeared right now, would this subsystem still work?”

If NO:
IT IS NOT NATIVE YET.

And:

“Could this subsystem plausibly have been written from scratch in 2026 for a native 64-bit PC game?”

If NO:
KEEP RESEARCHING, REVERSING, AND REWRITING.

Do not stop because something currently works.

Working through emulation, wrapping, translation, or compatibility is NOT completion.

GO TO C/C++ SEMANTIC DEPTH.
UNDERSTAND THE INTENT.
RESEARCH THE MODERN SOLUTION.
RECONSTRUCT IT NATIVELY.
DELETE THE OLD ARCHITECTURE.

NO WRAPPERS.
NO PRETENDING.
NO HALF-MEASURES.

THE GAMECUBE IS THE REFERENCE.

THE PC VERSION IS THE NEW ENGINE.

---

## Additional rule from the user (2026-10-06, verbatim)

ADDITIONAL RULE — NOTHING TO KEEP.

Assume NOTHING in the old architecture is sacred.

Nothing has to survive just because it already exists.
Nothing has to be wrapped.
Nothing has to be preserved structurally.
Nothing has to remain compatible internally.

If rewriting the entire subsystem produces the better native PC architecture, REWRITE IT.

Memory system? Rewrite it.
Renderer? Rewrite it.
Resource system? Rewrite it.
Streaming? Rewrite it.
Object ownership? Rewrite it.
Platform layer? Rewrite it.
Legacy engine infrastructure? Rewrite it.

Preserve only:
- game behavior
- gameplay rules
- asset meaning
- visual intent
- required timing semantics

Everything else is replaceable.

NOTHING TO KEEP.

REWRITE EVERYTHING NECESSARY.

Do not modernize the shell around legacy code.

RECONSTRUCT THE GAME AS A TRUE MODERN 64-BIT PC PROGRAM.

---

Applied: no existing component is kept by default - including the current renderer's "native" infrastructure
and this project's own runtime. A component stays only when, judged on its own, it is the design a new
64-bit PC engine for Shadow would choose; otherwise it is rewritten.

---

## Renderer decisions from the user (2026-10-06, verbatim)

DECISIONS:

1. GO NATIVE.

Preserve the artistic/visual intent of the original game, NOT GameCube hardware artifacts.

Do NOT preserve 320x240 blur history, deflicker behavior, forced point sampling, lack of AA, or other console-output limitations as architectural requirements.

Render natively at the actual output resolution with modern PC image quality. A legacy visual mode can exist later as an optional feature, but it must never constrain the renderer architecture.

2. YES — replace Plume if necessary.

Direct D3D12 ownership is absolutely acceptable and preferred if it results in a cleaner, faster, more genuinely native renderer.

Nothing in Plume needs to be preserved merely because it currently exists.

3. YES — Shader Model 6.6 is an acceptable minimum.

We are targeting a genuinely modern 64-bit PC renderer. Do not cripple the architecture to preserve obsolete GPU compatibility.

4. YES — two-player split-screen is in scope.

It is part of the actual game functionality and must survive the reconstruction. Implement it properly through the new renderer rather than preserving old GameCube rendering machinery for it.

GENERAL RULE:

Preserve GAME BEHAVIOR and VISUAL INTENT.

Do NOT preserve GameCube rendering artifacts, limitations, implementation details, or obsolete architecture.

If replacing the renderer completely with direct modern D3D12 is the technically superior solution, DO IT.

---

## Context reset from the user (2026-10-06, third session, verbatim)

CONTEXT RESET — READ THIS BEFORE CONTINUING

The conversation was compacted. Re-align immediately with the actual project goal.

THIS IS A FULL NATIVE RECONSTRUCTION OF SHADOW THE HEDGEHOG FOR MODERN 64-BIT PC.

NOT emulation.
NOT patching.
NOT wrapping.
NOT translating around old assumptions.
NOT preserving GameCube architecture.

NOTHING TO KEEP STRUCTURALLY.
REWRITE EVERYTHING NECESSARY.

STRICTLY FORBIDDEN AS FINAL ARCHITECTURE:

- fake MEM1
- enlarged MEM1
- GuestPtr
- AddressTranslator
- fake 32-bit address space
- compatibility RAM
- permanent wrappers/shims
- GX emulation
- permanent GX → D3D12 translation architecture
- RenderWare/GameCube internals hidden behind modern APIs
- “native-looking” systems that still fundamentally depend on GameCube behavior

The ACTUAL GAME CODE must learn to operate as native 64-bit PC code.

Go down to PPC / generated code / reconstructed C/C++ semantic depth.

Understand exactly what each subsystem was trying to accomplish.

Then reconstruct that behavior using the best modern PC architecture.

WORKFLOW:

original implementation
→ reverse exact semantics
→ reconstruct understandable C/C++ behavior
→ determine true intent
→ research modern 2026 solutions
→ design the best native replacement
→ implement directly
→ delete obsolete GameCube architecture

This applies to EVERYTHING:

- memory
- pointers
- allocations
- renderer
- resources
- streaming
- IO
- audio
- threading
- synchronization
- object ownership
- platform systems
- engine infrastructure

MEMORY:

The actual code becomes 64-bit aware.

Native pointers.
Native size_t / offsets where appropriate.
Native ownership.
Dynamic allocations.
Modern lifetimes.
No fake GameCube address model underneath.

RENDERING:

TEAR THE RENDERER APART AND REWRITE IT IF THAT IS THE BETTER SOLUTION.

Do not preserve GX as the architecture.

Recover the real rendering intent:

- geometry
- materials
- TEV behavior
- shaders
- textures
- transforms
- skinning
- lighting
- passes
- blending
- alpha
- depth
- culling
- cameras
- ordering
- framebuffer behavior
- resource ownership
- synchronization

Then rebuild it as a modern native PC renderer.

Direct D3D12 ownership is acceptable and preferred if it produces the best architecture.

Plume does NOT need to survive.

Shader Model 6.6 is an acceptable minimum.

Research modern rendering techniques such as:

- explicit D3D12 resource management
- bindless resources
- render graphs
- modern material/shader systems
- GPU-driven rendering where appropriate
- async uploads
- GPU residency
- multithreaded command recording
- modern skinning
- batching / instancing
- visibility systems
- streaming
- frame allocators
- fences and proper synchronization

VISUAL POLICY:

Preserve ARTISTIC INTENT.

Do NOT preserve GameCube hardware artifacts as architectural requirements.

320x240 blur history, deflicker, forced point sampling, lack of AA, etc. are NOT sacred.

Render natively at modern output resolution.

A retro/legacy visual mode may be added later, but must not constrain the new renderer.

GAMEPLAY:

Preserve gameplay behavior, rules, asset meaning, required timing semantics and deterministic behavior where necessary.

Two-player split-screen IS in scope and must work through the new native renderer.

CORE RULE:

DO NOT CONVERT THE PROBLEM.
DO NOT WRAP THE PROBLEM.
CHANGE THE CODE THAT HAS THE PROBLEM.

Ask constantly:

“What was this original code actually trying to accomplish?”

Then implement THAT natively.

And after every subsystem ask:

“If every GameCube compatibility layer disappeared right now, would this subsystem still work?”

If NO, it is NOT finished.

Also ask:

“Could this subsystem plausibly have been written from scratch in 2026 for a modern 64-bit PC game?”

If NO, keep researching and rewriting.

The GameCube version is the behavioral reference.

IT IS NOT THE ARCHITECTURE OF THE FINAL GAME.

NOTHING TO KEEP.
NO WRAPPERS.
NO PRETENDING.
REWRITE EVERYTHING NECESSARY.

MAKE THE GAME UNDERSTAND THE PC.
NEVER MAKE THE PC PRETEND TO BE A GAMECUBE.

---

## Context reset from the user (2026-10-07, verbatim)

CONTEXT RESET — CONTINUE THE FULL NATIVE REWRITE

You may have lost prior context. Re-align immediately.

The project goal is NOT to preserve, emulate, wrap, translate, or cosmetically modernize the GameCube architecture.

The goal is to REWRITE Shadow the Hedgehog into a genuine modern native 64-bit PC game.

Current direction is correct: the source is being reconstructed from scratch and the new native renderer is already drawing real stage geometry.

DO NOT FALL BACK TO LEGACY THINKING.

NOTHING TO KEEP STRUCTURALLY.

If an old subsystem, API, memory model, renderer path, resource path, or ownership model is obsolete, REWRITE IT.

STRICTLY FORBIDDEN AS FINAL ARCHITECTURE:

- fake MEM1
- enlarged MEM1
- GuestPtr
- AddressTranslator
- fake 32-bit address space
- compatibility RAM
- permanent wrappers
- permanent shims
- GX emulation
- permanent GX → D3D12 translation architecture
- RenderWare/GameCube internals hidden under modern APIs
- legacy subsystems kept alive just because they already work
- “native-looking” code that still fundamentally depends on GameCube assumptions

CORE RULE:

DO NOT CONVERT THE PROBLEM.
DO NOT WRAP THE PROBLEM.
DO NOT HIDE THE PROBLEM.

REWRITE THE CODE THAT HAS THE PROBLEM.

Go down to original PPC / generated code / reconstructed C/C++ semantic depth.

Understand WHAT the original code actually intends to do.

Then rebuild that behavior directly as modern native 64-bit PC code.

THE GAMECUBE VERSION IS A BEHAVIORAL REFERENCE ONLY.

It is NOT the architecture of the new engine.

PRESERVE:

- gameplay behavior
- game rules
- asset meaning
- visual intent
- required timing semantics
- deterministic behavior where required

REWRITE FREELY:

- memory
- renderer
- resource management
- object ownership
- IO
- streaming
- threading
- synchronization
- audio infrastructure
- platform code
- file/runtime representations
- engine architecture

RENDERER RULE:

The new renderer is a NEW PC RENDERER FOR SHADOW.

Do not try to make GX survive.

Reverse the intended rendering semantics:

- geometry
- transforms
- cameras
- materials
- textures
- TEV/material intent
- lighting
- skinning
- blending
- alpha test
- depth
- culling
- render ordering
- passes
- framebuffer effects
- resource lifetime
- CPU/GPU synchronization

Then implement them directly with modern PC graphics architecture.

Direct D3D12 ownership is acceptable.

Plume is not sacred.

GX is not sacred.

RenderWare GameCube internals are not sacred.

Research current modern solutions when needed, including:

- D3D12
- Shader Model 6.6+
- bindless resources
- render graphs
- descriptor management
- async uploads
- resource residency
- modern material systems
- modern shader systems
- multithreaded command generation
- batching and instancing
- GPU-driven rendering where beneficial
- modern skinning
- streaming
- frame resources
- fences and explicit synchronization

MEMORY RULE:

The actual game code becomes truly 64-bit.

Use:

- real native pointers
- native ownership
- native allocations
- native sizes and offsets
- dynamic capacity
- proper lifetimes

Do not preserve a fake GameCube memory universe underneath.

CONTINUE FROM CURRENT REPOSITORY STATE.

Before doing new work:

1. inspect the current repository
2. inspect current status/design notes
3. determine exactly what has already been rewritten
4. identify the latest verified working point
5. continue from there

DO NOT restart completed work.
DO NOT undo validated native systems.
DO NOT trust stale plans over the current implementation.

CURRENT IMPORTANT MILESTONE:

The rewritten native renderer is already producing real stage geometry.

Treat that as proof that the correct direction is full reconstruction.

Now continue correcting and completing the native implementation:

- geometry correctness
- transforms
- winding/culling
- material interpretation
- texture mapping
- depth
- lighting
- render passes
- missing meshes
- visual parity
- performance
- ownership

Do NOT react to rendering bugs by reintroducing old GX infrastructure.

Fix the native renderer itself.

Use this test constantly:

“If every GameCube compatibility layer disappeared right now, would this subsystem still work?”

If NO:
IT IS NOT FINISHED.

Also ask:

“Could this subsystem plausibly have been written from scratch today for a modern 64-bit PC game?”

If NO:
KEEP RESEARCHING AND REWRITING.

NOTHING TO KEEP.

REWRITE EVERYTHING NECESSARY.

NO WRAPPERS.
NO FAKE SOLUTIONS.
NO ARCHITECTURAL CHEATING.
NO PRETENDING.

THE GAMECUBE IS THE REFERENCE.

THE PC VERSION IS THE NEW ENGINE.

MAKE THE GAME UNDERSTAND THE PC.

NEVER MAKE THE PC PRETEND TO BE A GAMECUBE.

---

## What it settles in the current work (recorded with it, 2026-10-07)

- **Where the work stands when it arrived** (checked against the repository, not a plan): the new renderer
  (`runtime/lift/render/`: direct D3D12, SM 6.6 bindless, render graph, upload stream, compute skin cache) draws the
  game's land live from the game's own lowered RenderWare draw calls; R2e milestone 1 is gated (`gate-r2e-m1c`
  PASS: behaviour identical to today's build on both routes) - the latest verified working point. Static models
  are drawn (not gated yet); skinned models were in progress (renderer side: per-frame skinning compute pass into the
  skin cache; game side: the bone palette).
- **The two tests, applied to what exists.** The renderer passes both: it takes value-only frame packets, owns every
  GPU resource, is tested on its own (`render_core_tests` on WARP) and nothing in it knows a GameCube, GX or
  RenderWare. The R2 driver layer (`runtime/scaffold/rw_driver/`) fails the first test by construction - it reads
  RenderWare objects in the address window to describe frames - and is therefore not finished: it is scaffolding
  with named deleters (R4 2D, R5 render tree, R6 scene graph: the game code that owns models, skins, cameras and
  lights describes its frames natively and the layer is deleted with the window's last render readers).
- **Rendering bugs are fixed in the native renderer** (or in the reconstructed game-side description), never by
  bringing back GX state, GX formats or a GX-shaped API.

---

## Context reset from the user (2026-10-07, second, verbatim)

CONTEXT RESET — CONTINUE THE TRUE NATIVE RECONSTRUCTION

If context was compacted, re-align with this immediately.

PROJECT GOAL:

Shadow the Hedgehog is being reconstructed as a TRUE modern native 64-bit PC game.

NOT emulation.
NOT wrapping.
NOT patching.
NOT translating old GameCube architecture forever.
NOT preserving obsolete systems because they already exist.

NOTHING IS SACRED STRUCTURALLY.

Preserve only:
- gameplay behavior
- game rules
- asset meaning
- visual intent
- required timing semantics
- deterministic behavior where necessary

Everything else may be rewritten.

CORE RULE:

DO NOT CONVERT THE PROBLEM.
DO NOT WRAP THE PROBLEM.
REWRITE THE CODE THAT HAS THE PROBLEM.

The current direction is correct.

The new native D3D12 renderer is already rendering:
- stage geometry
- models
- GPU skinning
- particles
- fog
- glare
- multiple real gameplay scenes with strong visual parity

DO NOT FALL BACK TO OLD GX / RenderWare / compatibility architecture just because a remaining effect or subsystem is difficult.

Fix and extend the NEW native systems.

RENDERER RULE:

This is a NEW PC RENDERER FOR SHADOW.

No permanent GX wrapper.
No permanent GX → D3D12 translator.
No legacy renderer hiding underneath.
No fake EFB architecture unless a specific visual behavior genuinely requires an equivalent native effect.
No Plume dependency if direct D3D12 is better.

Recover rendering INTENT, then implement it natively.

Continue reconstructing:
- materials
- shaders
- lighting
- particles
- fog
- glare
- transparency
- post effects
- reflections
- environment passes
- UI/HUD
- remaining effect paths
- camera behavior
- render ordering
- resource ownership
- synchronization
- performance

MEMORY RULE:

The actual game code becomes truly 64-bit.

No fake MEM1.
No GuestPtr.
No AddressTranslator.
No permanent compatibility RAM.
No fake 32-bit address world.

Use native pointers, native ownership, native allocations, native resource lifetimes and modern dynamic capacity.

GO DEEP.

When something remains legacy:

1. trace the original PPC / generated code
2. reconstruct its real C/C++ semantics
3. understand WHY it exists
4. research the best modern solution
5. implement the behavior natively
6. remove the obsolete implementation

Do NOT mechanically port hardware mechanisms.

Port the MEANING.

IMPORTANT CONTINUATION RULE:

Inspect the CURRENT repository before acting.

Determine:
- what is already native
- what has already passed tests
- what is partially migrated
- what remains legacy
- what the latest visual/runtime checkpoint is

Do not restart completed work.
Do not overwrite validated native systems with older ideas.
Do not trust stale plans over working current code.

The repository is the authoritative state.

After every major migration:
- build
- run tests
- run gameplay captures
- compare against reference output
- check D3D12 validation
- check resource lifetime
- check determinism where relevant
- check performance
- remove dead legacy code
- update status documentation

USE THESE TESTS CONSTANTLY:

“If all GameCube compatibility infrastructure disappeared right now, would this subsystem still work?”

If NO:
IT IS NOT FINISHED.

“Could this subsystem plausibly have been written from scratch in 2026 for a modern native 64-bit PC game?”

If NO:
KEEP RESEARCHING AND REWRITING.

“Am I fixing the new native system, or escaping back into the old architecture?”

If it is the latter:
STOP AND CORRECT COURSE.

NOTHING TO KEEP STRUCTURALLY.
NO WRAPPERS.
NO FAKE SOLUTIONS.
NO ARCHITECTURAL CHEATING.
NO PRETENDING.

THE GAMECUBE VERSION IS THE BEHAVIORAL REFERENCE.

THE PC VERSION IS THE NEW ENGINE.

CONTINUE FROM THE LATEST VERIFIED NATIVE IMPLEMENTATION.

---

## What it settles in the current work (recorded with it, 2026-10-07, second reset)

- **Where the work stands when it arrived** (checked against the repository): the new renderer draws, from the game's
  own lowered RenderWare driver calls, the land, static models with their MatFX passes, GPU-skinned models, PTank
  particles, fog, the native glare and Im3D (subtitles) - gated on Westopolis and Final Haunt, latest
  `scratch/lift-20261006/r2/gate-r2e-m2f` PASS (behaviour identical to the default build, deterministic). In
  progress: Im2D / the HUD (findings in SESSION_STATE 4c bullet "R2e MILESTONE 2", paragraph "IM2D DESIGN"). Not
  drawn yet: the HUD, menus, fade and loading screen; the radial blur / motion trail, screen colour, heat shimmer and
  lens flare; split-screen views.
- **What is still legacy, named so it is never mistaken for finished**: (1) the default build still draws through the
  old renderer (GX reconstruction, RwCapture, plume) - deleted by R2f when the new renderer draws everything; (2) the
  R2 driver layer `runtime/scaffold/rw_driver` reads RenderWare objects through the address window - it fails the
  first test by construction and is deleted by R4 (2D), R5 (render tree), R6 (scene graph); (3) the flat 32-bit
  address window (M1) under every function not yet lifted - nothing new is built on it, it shrinks with each lift and
  goes with the last one; (4) the translated RenderWare object factories, CSD (the 2D layout library) and the scene
  graph - lifted in R4-R6.
- **The new renderer has no plume dependency** (checked: nothing under `runtime/lift` names plume; plume links the
  renderer's D3D12MA, not the reverse) and knows no GX or RenderWare (`tools/lift/check_purity.py`).
- **The HUD is built as a native canvas layer**: the game's 2D draws become canvas-space draws (authored 640 x 480 at
  4:3) carrying the anchor the game's layout implies (fill / left / centre / right); the renderer places them for the
  real output aspect and resolution. No 640 x 480 render target, no EFB, no GX orthographic state; the anchors come
  from the game's own layout announcements until R4 lifts CSD and they become data.
- **The remaining screen effects are rebuilt as native post-processing**: radial blur / motion trail, screen colour,
  heat shimmer and lens flare become render-graph passes over the scene colour (a history texture where the effect is
  a feedback), recovered from what they are meant to look like - an EFB copy into a camera texture is how the console
  achieved them, not what they are.

---

## Context reset from the user (2026-10-07, third, verbatim)

CONTEXT RESET — CONTINUE THE TRUE NATIVE RECONSTRUCTION

If context was compacted, re-align with this immediately.

FIRST ACTION AFTER ANY CONTEXT RESET:

READ THE CURRENT `NEXT_SESSION` FILE / DOCUMENT COMPLETELY BEFORE DOING ANY WORK.

Treat `NEXT_SESSION` as the handoff from the previous context:
- read it first
- understand what was completed
- understand what is currently in progress
- understand known blockers
- understand the next intended steps
- cross-check it against the current repository state

Do NOT start from memory.
Do NOT restart already completed work.
Do NOT blindly follow stale assumptions if the repository has moved forward.

`NEXT_SESSION` + current repository state are the authoritative continuation point.

PROJECT GOAL:

Shadow the Hedgehog is being reconstructed as a TRUE modern native 64-bit PC game.

NOT emulation.
NOT wrapping.
NOT patching.
NOT translating old GameCube architecture forever.
NOT preserving obsolete systems because they already exist.

NOTHING IS SACRED STRUCTURALLY.

Preserve only:
- gameplay behavior
- game rules
- asset meaning
- visual intent
- required timing semantics
- deterministic behavior where necessary

Everything else may be rewritten.

CORE RULE:

DO NOT CONVERT THE PROBLEM.
DO NOT WRAP THE PROBLEM.
REWRITE THE CODE THAT HAS THE PROBLEM.

The current direction is correct.

The new native D3D12 renderer is already rendering real game content including:
- stage geometry
- models
- GPU skinning
- particles
- fog
- glare
- multiple gameplay scenes with strong visual parity

DO NOT FALL BACK TO GX / RenderWare / compatibility architecture when something becomes difficult.

Fix and extend the NEW native systems.

RENDERER RULE:

This is a NEW PC RENDERER FOR SHADOW.

No permanent GX wrapper.
No permanent GX → D3D12 translator.
No legacy renderer hiding underneath.
No fake compatibility architecture.
No Plume dependency if direct D3D12 is the better solution.

Recover rendering INTENT and implement it natively.

MEMORY RULE:

The actual game code becomes truly 64-bit.

No fake MEM1.
No GuestPtr.
No AddressTranslator.
No permanent compatibility RAM.
No fake 32-bit address world.

Use real native pointers, ownership, allocations, resource lifetimes and dynamic capacity.

GO DEEP.

For every remaining legacy subsystem:

1. trace the original PPC / generated code
2. reconstruct the real C/C++ semantics
3. understand what the system was actually trying to accomplish
4. research the best modern solution
5. implement that behavior natively
6. delete the obsolete architecture

DO NOT PORT HARDWARE MECHANISMS.

PORT THE MEANING.

CONTINUATION ORDER AFTER CONTEXT LOSS:

1. READ `NEXT_SESSION`
2. inspect the current repository
3. inspect current status/design/research notes
4. identify the latest verified working state
5. verify what is already native
6. identify the exact unfinished work
7. continue from that point

DO NOT restart completed work.
DO NOT undo validated native systems.
DO NOT trust stale plans over working code.

After every major migration:
- build
- run tests
- run gameplay captures
- compare against reference output
- check D3D12 validation
- check resource lifetime
- check determinism where relevant
- check performance
- delete dead legacy code
- UPDATE `NEXT_SESSION` with the exact new continuation state

IMPORTANT:

`NEXT_SESSION` must always be left useful for the next context window.

Record:
- what was completed
- what passed
- what failed
- current blockers
- important discoveries
- files/subsystems currently being modified
- exact next action
- anything the next session must NOT redo

USE THESE TESTS CONSTANTLY:

“If all GameCube compatibility infrastructure disappeared right now, would this subsystem still work?”

If NO:
IT IS NOT FINISHED.

“Could this subsystem plausibly have been written from scratch in 2026 for a modern native 64-bit PC game?”

If NO:
KEEP RESEARCHING AND REWRITING.

“Am I fixing the native system, or escaping back into the old architecture?”

If it is the latter:
STOP AND CORRECT COURSE.

NOTHING TO KEEP STRUCTURALLY.
NO WRAPPERS.
NO FAKE SOLUTIONS.
NO ARCHITECTURAL CHEATING.
NO PRETENDING.

THE GAMECUBE VERSION IS THE BEHAVIORAL REFERENCE.

THE PC VERSION IS THE NEW ENGINE.

READ `NEXT_SESSION` FIRST.

THEN CONTINUE FROM THE LATEST VERIFIED NATIVE IMPLEMENTATION.

---

## What it settles in the current work (recorded with it, 2026-10-07, third reset)

- **`NEXT_SESSION` is `NEXT_SESSION_PROMPT.md`** (the handoff the user pastes into a new session); its top section is
  the continuation point, cross-checked against the repository at the start of every session. From this reset on
  that section carries the user's fields explicitly: completed, passed, failed, current blockers, important
  discoveries, files / subsystems being modified, the exact next action, and what the next session must NOT redo.
- **Where the work stands when it arrived** (checked against the repository: no source edited after the handoff, no
  game or build running): R2 is DEPLOYED (root `d95d1814`, the gate-r2e-m2j candidate) - the new renderer draws the
  land, models with MatFX, GPU skinning, particles, fog, the native glare, Im3D, the HUD / menus / fades (canvas
  layer), the motion trail, heat haze / refraction, the screen colour's multiply and movies; the oracle PASS on both
  reference runners. Exact unfinished work, in the handoff's order: the Lava Shelter heat-haze gap (the user's play
  session reported 4,664 screen-pass draws "not described"), the lens flare check, split-screen views, R2f.
- **Still legacy, unchanged since the second reset** (named so it is never mistaken for finished): the default
  build's old renderer (GX reconstruction, RwCapture, plume - deleted by R2f); the R2 driver scaffold
  `runtime/scaffold/rw_driver` (reads RenderWare objects through the window - deleted by R4 / R5 / R6); the M1
  address window under every function not yet lifted; the translated RenderWare object factories, CSD and scene graph.

---

## Session objective from the user (2026-10-07, end of the fifth session, verbatim)

UPDATE THE HANDOFF AND PREPARE THE NEXT SESSION FOR CONTINUED DEEP RECONSTRUCTION.

Before ending this session:

1. Update `NEXT_SESSION` with the exact current state of the project.
2. Record what has already been disassembled, understood, rewritten, validated, and what is still legacy.
3. Record the current blockers, unfinished subsystems, important discoveries, and the exact next starting point.
4. Make the next session continue from the repository as it exists now — do NOT restart completed work.

NEXT SESSION OBJECTIVE:

Continue disassembling the original game deeper, subsystem by subsystem and function by function.

For each part:

ORIGINAL PPC / BINARY BEHAVIOR
→ disassemble and trace it
→ reconstruct its real C/C++ semantics
→ understand what the code actually does and why
→ rewrite that functionality as clean modern native 64-bit PC code
→ integrate it into the new architecture
→ compare the new implementation against the original/reference behavior
→ fix differences until behavior matches
→ remove the obsolete legacy path

Do this PART BY PART.

Do not guess.
Do not blindly translate assembly.
Do not preserve old architecture just because it works.

Understand first, then reconstruct.

COMPARISON IS MANDATORY.

After every rewritten subsystem or meaningful code path, compare old vs new using all relevant evidence available:

- gameplay behavior
- frame captures
- rendered output
- state values
- transforms
- materials
- animation
- physics
- AI
- timing
- audio behavior
- resource behavior
- deterministic traces
- performance where relevant

If the comparison does not match, investigate the root cause before moving on.

The workflow should be:

DISASSEMBLE
→ UNDERSTAND
→ RECONSTRUCT
→ REWRITE NATIVELY
→ RUN
→ COMPARE
→ FIX
→ VALIDATE
→ DELETE LEGACY IMPLEMENTATION
→ MOVE TO THE NEXT PART

The long-term goal is to progressively replace the remaining original/GameCube implementation until the actual game itself exists as reconstructed modern native 64-bit PC code.

At the end of every session, update `NEXT_SESSION` again so the next context can resume immediately from the exact verified point.

---

## What it settles in the current work (recorded with it, 2026-10-07, end of the fifth session)

- **The operating procedure of every following session** is the user's loop, part by part: DISASSEMBLE -> UNDERSTAND ->
  RECONSTRUCT -> REWRITE NATIVELY -> RUN -> COMPARE -> FIX -> VALIDATE -> DELETE LEGACY -> NEXT PART. A part is not
  finished until its comparison against the original / reference behaviour matches (or every difference is root-caused
  and intended) and its legacy path is deleted. Comparison evidence per part is chosen from the user's list (gameplay,
  captures, state values, transforms, materials, animation, physics, AI, timing, audio, resources, deterministic traces,
  performance); the tools that exist for it are listed in `docs/lift-20261006/RECONSTRUCTION_LEDGER.md` section 4.
- **The status ledger** of what has been disassembled, understood, rewritten, validated and what is still legacy -
  subsystem by subsystem, with the evidence and the next step - is `docs/lift-20261006/RECONSTRUCTION_LEDGER.md`
  (written with this directive). The honest headline: 38,815 of the 39,388 functions in the game's function table
  still run translated (the generator's report, build-lifted); the native parts are the renderer, input, the asset
  importers and the platform owners; the rest of the game is the next work.
- **`NEXT_SESSION_PROMPT.md`'s top section** is rewritten at the end of every session from the verified state (the
  user's fields: completed / passed / failed / blockers / discoveries / files / exact next action / do not redo).

---

## Course correction and additional requirement from the user (2026-10-07, sixth session; summarised)

"COURSE CORRECTION - BUILD A PYTHON BEFORE/AFTER COMPARISON GUI" and "ADDITIONAL REQUIREMENT - USER-CREATED IN-STAGE
COMPARISON SNAPSHOTS": a real Python GUI (not a CLI harness) launching OLD and NEW at the SAME LOGICAL game state -
named, user-created, mid-stage, repeatable, deterministic snapshots (a restorable game moment, not a screenshot),
LOAD OLD / NEW / BOTH, RELOAD BOTH, deterministic stepping (tick and frame exposed as different clocks), captures with
pixel diff. Verbatim rules kept here: "DO NOT assume the binary serialization format of OLD and NEW must be
identical." "THE STATES MUST BE SEMANTICALLY EQUIVALENT, NOT BINARY IDENTICAL." "The comparison tool is allowed to
understand both worlds. The NEW GAME ENGINE is NOT." "Do NOT turn the new engine back into a GameCube compatibility
architecture for this feature. The comparison layer bridges SEMANTICS." "Do not fake pixel parity by postprocessing
the screenshots to look alike. Fix rendering differences in the actual native renderer." Applied: tools/comparator +
docs/comparator/README.md (per-build savestates v19, input-tape replay, probes as the semantic bridge).

## Additional requirement from the user (2026-10-07, sixth session, part 2; summarised)

"ADDITIONAL REQUIREMENT - ONE-BUTTON AUTO-LABELED COMPARISON SNAPSHOTS": while playing normally, ONE configurable
snapshot hotkey (e.g. F5) at any moment inside a stage; the comparison system automatically captures and labels that
exact moment (stage, mission/route, game tick, frame, player position/orientation, camera position/orientation,
event/checkpoint, nearby area/object, timestamp, unique ID), no manual setup; it appears in the GUI at once with a
thumbnail and readable metadata, renamable afterwards; a searchable library of exact moments. Verbatim rules kept
here: "THIS IS NOT JUST A SAVESTATE. It is a CROSS-RENDERER VISUAL REPRODUCTION SNAPSHOT." "The snapshot must
contain enough semantic information to reconstruct the exact visual state in both implementations." "The NEW
renderer must not merely load approximately the same location. It must reconstruct the same render inputs." "If
exact reconstruction requires renderer-specific temporal history, support that explicitly." "Do NOT reintroduce
legacy architecture into the native renderer." "The comparison system may bridge the two implementations." "The
native renderer must remain genuinely native." Buttons asked for: LOAD OLD / LOAD NEW / LOAD BOTH / RENDER REFERENCE /
RENDER NATIVE / PIXEL COMPARE and RECREATE EXACT FRAME (load, restore, deterministic warm-up, freeze at the target
frame, render OLD and NEW, capture, pixel difference). Applied: docs/comparator/README.md sections 4a, 4b, 6a (the
game only saves its own state, tape, report and next frame at the boundary the key asks for; the workstation reads
the semantics out of the savestate; recreations replay from per-build warm-up states; no renderer change).

---

## Task from the user (2026-10-07, seventh session, verbatim)

NORTH STAR TASK — DEEPLY REVERSE THE 2D ↔ 3D RENDERING BOUNDARY

Continue following the North Star.

Do NOT patch symptoms.
Do NOT add arbitrary offsets.
Do NOT compensate for errors with magic constants.
Do NOT preserve broken GameCube-era coordinate assumptions.

Go deeper.

I want you to systematically reverse-engineer EVERY subsystem that sits on the boundary between the 3D world and 2D screen/UI rendering.

This includes everything that converts, projects, aligns, scales, clips, or anchors 3D information into 2D screen space.

Find ALL of it.

Investigate:

- world → view transforms
- view → clip transforms
- clip → NDC
- NDC → viewport
- viewport → screen pixels
- screen → UI coordinate systems
- camera projection
- FOV
- aspect ratio
- Hor+ handling
- viewport rectangles
- split-screen rectangles
- render-resolution scaling
- UI resolution assumptions
- 640x480 / 4:3 assumptions
- 320x240 assumptions
- safe areas
- screen-space offsets
- half-pixel / texel-center assumptions
- origin conventions
- handedness
- Y-axis inversion
- normalized coordinates
- pixel coordinates
- virtual UI resolutions
- HUD transforms
- sprite transforms
- 2D matrices
- 3D billboards
- projected markers
- enemy indicators
- lock-on markers
- target boxes
- nameplates
- reticles
- crosshairs
- depth-aware HUD elements
- camera-space targeting calculations

MOST IMPORTANT BUG:

The lock-on / targeting crosshairs are horizontally wrong.

When I lock onto an enemy, the crosshair shifts LEFT or RIGHT instead of remaining pixel-perfectly aligned with the enemy.

DO NOT FIX THIS WITH AN X OFFSET.

Find the ROOT CAUSE.

Trace the entire path:

enemy world position
→ targeting point / bone / object position
→ game camera
→ original projection calculation
→ projected 2D coordinate
→ HUD coordinate conversion
→ sprite placement
→ final native viewport pixel

Compare OLD vs NEW at every intermediate step.

For the same snapshot, log something like:

enemy_world
camera_position
camera_rotation
view_space
clip_space
NDC
viewport_rect
screen_pixel
UI_virtual_coordinate
final_crosshair_position

Do this for BOTH old/reference and new/native versions.

Find the FIRST point where they diverge.

That is where the real bug lives.

Use the comparison/snapshot tooling.

Create several deterministic test states:

- enemy exactly screen center
- enemy left side
- enemy right side
- enemy near camera
- enemy far away
- different FOV
- different resolutions
- 4:3
- 16:9
- ultrawide if supported
- split-screen if applicable

The crosshair must remain attached to the exact same projected target point at every resolution and aspect ratio.

Investigate especially whether one system is using:

- original 4:3 projection while another uses modern Hor+
- full backbuffer width while another uses the active viewport
- old virtual UI coordinates
- incorrect viewport center
- incorrect aspect correction
- projection matrix generated for one aspect but converted using another
- camera view_window values incorrectly interpreted
- integer rounding too early
- half-width / half-height assumptions
- screen-space coordinates from the old renderer being transformed twice
- a 2D element being aspect-corrected after already being projected correctly
- stale GameCube framebuffer dimensions
- different projection conventions between world renderer and HUD renderer

DO NOT ASSUME THE CROSSHAIR IS THE ONLY BUG.

Use it as a clue to locate the entire 2D/3D boundary architecture.

Once found, reconstruct that boundary properly for the native engine.

The desired architecture should have ONE authoritative, well-defined transformation path for things such as:

ProjectWorldToViewport()
ViewportToPixels()
PixelsToUI()
UIToPixels()

or an equivalent clean design.

Do not scatter projection math throughout gameplay code.

Do not keep multiple contradictory screen-coordinate systems unless they are genuinely necessary.

Document every coordinate space explicitly.

For every coordinate type, define:

- origin
- units
- axis direction
- range
- aspect behavior
- viewport relation
- owning subsystem

STRICT RULE:

DO NOT MAKE THE CROSSHAIR LOOK CORRECT.

MAKE THE UNDERLYING 3D → 2D MATH CORRECT.

Once the root transformation is correct, the crosshair, lock-on markers, HUD indicators, projected effects and other world-attached 2D elements should all become correct naturally.

Follow:

DISASSEMBLE
→ TRACE
→ RECONSTRUCT C/C++ SEMANTICS
→ COMPARE OLD VS NEW INTERMEDIATE VALUES
→ FIND FIRST DIVERGENCE
→ REDESIGN NATIVE COORDINATE BOUNDARY
→ FIX ROOT CAUSE
→ VALIDATE ACROSS MANY SNAPSHOTS
→ REMOVE LEGACY WORKAROUNDS

Do not stop after fixing the visible crosshair.

Find and modernize the ENTIRE 2D / 3D rendering boundary.

NORTH STAR:

NO WRAPPERS.
NO MAGIC OFFSETS.
NO COMPENSATION HACKS.
NO “looks close enough”.

UNDERSTAND THE ORIGINAL INTENT.
RECONSTRUCT THE MATH.
MAKE IT PIXEL-CORRECT IN THE NATIVE RENDERER.

(Sent by the user during the same turn, verbatim:) Create handoffs on the fly in case you are out of context window.

## What it settles in the current work (recorded with it, 2026-10-07, seventh session)

- **The part this session works on** is the 2D <-> 3D rendering boundary, with the lock-on crosshair as the clue. It
  is the camera FOV deep part of RECONSTRUCTION_LEDGER.md section 5.1 widened to every consumer of the game's own
  projection (LockonMark, the enemy HP gauges, 3D-anchored CSD scenes, the Sprite3D functor, the NDC users) and every
  coordinate space between the game camera and a presented pixel. Its record is
  `docs/boundary-20261007/` (coordinate spaces, the traced paths, the first divergence, the design).
- **Handoffs on the fly:** `NEXT_SESSION_PROMPT.md`'s top block ("SEVENTH SESSION") is updated after every finding,
  not only at the session's end, so a context loss resumes from the last verified point.


## Course decision from the user (2026-10-07, seventh session, later): native framing, 16:9 flare, 2D/3D boundary completion

Verbatim:

```
COURSE DECISION + NEXT ACTION — NATIVE FRAMING, 16:9 FLARE, AND 2D/3D BOUNDARY COMPLETION

Continue from the CURRENT repository state and the current 2D/3D rendering-boundary research.

Do NOT restart completed work.
Do NOT undo already validated renderer work.
Read the current NEXT_SESSION / handoff and the renderer-RE documents produced by the parallel part-agents first.

The repository and current verified implementation are authoritative.


============================================================
1. USER DECISION — DEFAULT FRAMING
============================================================

Keep SQUARE-PIXEL FRAMING as the DEFAULT.

The native PC renderer targets:

- square pixels
- the real output resolution
- the real output aspect ratio
- modern displays
- native 16:9 / ultrawide-capable camera and viewport handling

Do NOT make GameCube pixel-aspect/framebuffer artifacts the default.

Keep the original console framing only as an OPTIONAL reference / compatibility mode:

--framing game

Conceptually:

DEFAULT:
native square-pixel presentation
native output aspect
modern PC framing

OPTIONAL:
--framing game
original console/reference framing

The optional console mode exists for:

- comparison
- reference captures
- purist reproduction
- reverse-engineering validation

It must NOT constrain the architecture of the native renderer.


============================================================
2. IMPORTANT DISTINCTION — CAMERA COMPOSITION VS DISPLAY ARTIFACT
============================================================

Do not blindly classify every difference from the GameCube image as a display artifact.

Recover the difference between:

A) authored camera composition

and:

B) GameCube display / pixel-aspect / framebuffer behavior.

If a camera or gameplay sequence was genuinely authored around a particular framing relationship, preserve that INTENT.

But do not reproduce that intent through a global GameCube pixel-aspect hack.

Instead:

original authored camera intent
        ↓
native camera semantics
        ↓
actual modern viewport/aspect

If a specific camera needs reconstructed horizontal/vertical FOV semantics, fix the actual camera code or camera description.

Do NOT globally distort the output merely to recreate console framebuffer geometry.


============================================================
3. FLARE AT 16:9 — REWRITE THE ACTUAL VISIBILITY RULE
============================================================

Current known issue:

- the flare now visually sits on the sun correctly at 16:9
- however, the game still hides it when the sun leaves the CENTRAL LEGACY 4:3 AREA
- the visibility restriction is inside the flare/game code itself
- therefore this is NOT a renderer-positioning bug anymore
- it requires a native reconstruction of the flare visibility function

Fix this properly after / alongside the current effects reverse engineering.

Do NOT patch around it in the renderer.

Do NOT enlarge a fake 4:3 region.
Do NOT spoof coordinates.
Do NOT clamp the sun back into the old frame.
Do NOT keep the old visibility test and hide the symptom.

Recover the original flare code far enough to determine:

- how the sun/world position becomes camera/view space
- how it becomes projected screen space
- what coordinate system the visibility test uses
- what bounds are compared
- whether the bounds are hard-coded 4:3 assumptions
- whether visibility depends on viewport, raster size, camera raster, safe area, clip space, or CSD/RW state
- whether occlusion and off-screen rejection are separate tests
- whether split-screen changes the tested viewport
- whether the test uses old EFB dimensions
- whether it assumes 640x480 / 4:3 / non-square pixels
- whether there are margins or flare-specific authored thresholds

Then reconstruct the true intended semantic rule natively.


============================================================
4. TARGET FLARE SEMANTICS
============================================================

For the native renderer, flare eligibility must be based on the ACTUAL ACTIVE CAMERA VIEWPORT.

Conceptually:

world-space sun/light position
    ↓
native camera view/projection
    ↓
clip / NDC / viewport position
    ↓
actual viewport visibility test
    ↓
occlusion / flare-specific rules
    ↓
native flare render pass

For normal single-player 16:9:

if the sun is still inside the actual visible viewport,
the flare must NOT disappear merely because it left the old central 4:3 rectangle.

The old 4:3 border is NOT a gameplay rule.

It is an obsolete display assumption unless reverse engineering proves otherwise.


============================================================
5. SPLIT-SCREEN MUST BE DESIGNED CORRECTLY
============================================================

Two-player split-screen is in scope.

Do not solve flare visibility globally against the full window if the flare belongs to one camera.

Each view must use its own:

- camera
- projection
- viewport rectangle
- aspect ratio
- visibility bounds
- post-processing region

Conceptually:

View 0:
camera0 -> viewport0 -> flare0

View 1:
camera1 -> viewport1 -> flare1

The flare code must not assume:

full-window == active camera viewport.

Design this now so the native implementation does not need to be rewritten again for split-screen.


============================================================
6. ULTRAWIDE / NON-16:9 POLICY
============================================================

Do not hard-code 16:9 as the new architectural limit.

The rule is:

ACTUAL VIEWPORT.

16:9 is simply the immediate validation target.

The implementation should naturally support:

4:3
16:9
16:10
21:9
32:9
windowed arbitrary aspect ratios

subject to camera/gameplay-specific policies that are explicitly recovered.

If an effect requires an authored safe area, represent that as explicit semantic data.

Do NOT smuggle 4:3 assumptions back in through constants.


============================================================
7. 2D / 3D RENDERING BOUNDARY RESEARCH
============================================================

Continue and finish the current parallel research into the 2D/3D boundary.

The RenderWare 3.7 SDK headers are available and provide platform-neutral layouts for core types including:

RwCamera
RwFrame
RwRaster
RpWorld
RpAtomic
RpGeometry

Use those headers as RE evidence and layout references.

Do NOT treat the SDK architecture as sacred.

Its purpose here is to help identify the original semantics and object relationships.

The eight part-agents should continue to produce consistent documents and CSVs.

Build a unified boundary index describing:

function
address
class / structure
reads
writes
ownership
camera/view dependency
2D / 3D / mixed classification
RenderWare role
game role
native replacement boundary
confidence
legacy dependency
deletion target


============================================================
8. SPECIFICALLY RECOVER THE 2D/3D HANDOFF
============================================================

Map where the game transitions between:

3D world rendering
camera projection
screen-space geometry
HUD layout
CSD layout
Im2D
Im3D
post-processing
screen effects
flare/glare
subtitles
menus
fades
movies

Identify every place where a 3D position becomes:

- projected position
- normalized screen position
- raster-space position
- 640x480 authored canvas position
- final native output-space position

This is especially important because aspect-ratio bugs often originate at these transitions.


============================================================
9. RETICLE / TARGETING BOUNDARY
============================================================

Use this same research to identify the complete path for targeting reticles and lock-on markers.

A known important problem class is:

reticles shifting left/right when aiming at an enemy.

Recover the full chain:

target world transform
    ↓
camera transform
    ↓
projection
    ↓
screen-space conversion
    ↓
HUD/CSD coordinate conversion
    ↓
anchor/safe-area logic
    ↓
final native canvas placement

Do not patch a fixed horizontal offset.

Determine exactly where the coordinate spaces diverge.

Instrument both OLD and NEW paths with:

world position
view-space position
clip coordinates
NDC
viewport pixel position
CSD/canvas position
final output pixel position

The first mismatch determines the owner of the bug.


============================================================
10. CSD / HUD OBJECT DIFFERENCE
============================================================

Continue the active CSD investigation.

Current observation:

- the new path creates additional CSD 2D-layout objects around 17.27s and 17.55s on the investigated route
- the old/reference build does not
- the allocator trace points to CSD object allocation
- full guest back-chain / caller attribution is being used to determine the cause

Do NOT assume:

extra object == leak.

Determine whether it is:

- legitimate layout rebuild after corrected aspect/FOV
- viewport invalidation
- CSD cache invalidation
- duplicate creation
- ownership/lifetime error
- HUD state difference
- camera-state difference
- render-boundary announcement difference

For every extra object record:

creation stack
object/layout identity
owner
parent
lifetime
destruction stack
visible effect
old-build equivalent
reason for creation

No unexplained extra allocations may be silently accepted.


============================================================
11. COMPARISON IS MANDATORY
============================================================

Use the comparison tooling aggressively.

For every framing / flare / HUD / CSD change, compare OLD and NEW from the SAME LOGICAL STATE.

Use the existing snapshot system where useful:

LOAD OLD
LOAD NEW
LOAD BOTH
RECREATE EXACT FRAME
RENDER REFERENCE
RENDER NATIVE
PIXEL COMPARE

Use user-created mid-stage snapshots where they provide better coverage.

Do not compare merely from approximately the same stage location.

Use deterministic reconstruction of the same visual state.


============================================================
12. REQUIRED CAPTURE SET
============================================================

Build a focused aspect/framing capture matrix.

At minimum:

Westopolis:
- normal gameplay
- sun near centre
- sun near old 4:3 border
- sun beyond old 4:3 border but inside 16:9 viewport
- sun near actual 16:9 border

Final Haunt:
- representative gameplay camera
- large effects / glare scene

HUD:
- target reticle
- subtitles
- score / rings
- weapon HUD
- objective/UI markers

Menus:
- 4:3-authored layout on 16:9 output

Effects:
- flare
- glare
- motion trail
- heat haze
- screen colour
- fade

Later:
- split-screen


============================================================
13. VALIDATE BOTH MODES
============================================================

Validate:

DEFAULT native square-pixel mode

and:

--framing game

The console/reference mode should remain useful for exact reference comparisons.

The native default should represent the intended modern PC presentation.

Do not let fixes for one mode silently corrupt the other.


============================================================
14. CAMERA / FOV RULE
============================================================

The new renderer has already corrected an earlier FOV problem.

Do not regress this.

Instrument camera state where necessary:

vertical FOV
horizontal FOV
aspect
near/far
projection matrix
viewport
camera transform

Determine the canonical game-side camera semantic representation.

Prefer storing/reconstructing a meaningful camera definition such as:

vertical FOV + aspect

or whatever the original semantics prove,

rather than preserving a GameCube projection artifact.

Do not modify camera values merely to make one screenshot line up unless the semantic cause is understood.


============================================================
15. NATIVE HUD POLICY
============================================================

The HUD remains a native canvas-space system.

The known design direction is correct:

game-authored 2D layout
        ↓
semantic anchor / canvas position
        ↓
native viewport-aware placement

Do NOT render the whole HUD through a fake 640x480 GameCube framebuffer.

A 640x480 authored coordinate system may remain as DATA where the original layout genuinely uses it.

That is different from requiring a 640x480 renderer.

Separate:

authored layout coordinate space

from:

physical output raster.


============================================================
16. EFFECTS POLICY
============================================================

Continue rebuilding effects by INTENT.

For each original effect:

PPC / RenderWare / GX implementation
        ↓
recover visible purpose
        ↓
native render-graph implementation

Do not preserve EFB copies simply because the GameCube used them.

Examples:

motion trail / radial blur
→ history texture

heat haze
→ scene-colour refraction

screen colour
→ native fullscreen compositing

flare
→ projected light + visibility / occlusion semantics

glare
→ native glare/bloom-like pass according to recovered behavior

Preserve the artistic behavior.

Replace the hardware mechanism.


============================================================
17. DO NOT RESTORE LEGACY ARCHITECTURE TO FIX BUGS
============================================================

Strictly forbidden:

- bringing GX state back
- recreating the old EFB architecture as a general solution
- adding a fake 4:3 viewport underneath the renderer
- using legacy raster dimensions to make the flare behave
- forcing old projection matrices globally
- adding hidden aspect-ratio compatibility hacks
- routing the native renderer through the old RenderWare/GX driver
- postprocessing screenshots to fake comparison success

If a bug appears in the native path:

find the true semantic owner
and fix that owner.


============================================================
18. EVIDENCE LEVELS
============================================================

Continue using:

PROVEN
STRONGLY SUPPORTED
STRUCTURAL MATCH
UNKNOWN

For flare/framing specifically, do not claim that a constant represents "4:3 clipping" merely because it numerically resembles one.

Trace how it is produced and consumed.

Document the evidence.


============================================================
19. VALIDATION GATE
============================================================

Before adopting the native flare/framing rewrite:

Require:

- deterministic route completion
- old/new gameplay behavior preserved
- GameClock unchanged
- PCM/audio unchanged
- file-read workload unchanged where expected
- no new CSD leaks
- no unexpected CSD object-count divergence
- no D3D12 validation errors
- no descriptor leaks
- no upload-ring overflows
- no GPU lifetime errors
- no unexpected frame pacing change
- correct HUD placement
- correct reticle placement
- correct flare visibility across full native viewport
- correct console/reference mode behavior

Capture before/after screenshots and pixel-diff evidence.

For intentional aspect-ratio differences, classify the difference rather than demanding false pixel identity.


============================================================
20. PERFORMANCE
============================================================

Measure the new implementation.

Do not add CPU work every frame just to emulate old screen-space transformations.

Prefer direct native math and cached semantic state.

Record:

CPU cost
GPU cost
draw/pass count
temporary allocations
CSD allocation count
descriptor usage
upload usage
VRAM impact

The rewrite must remain architecturally clean even if the old path happened to be cheap.


============================================================
21. DOCUMENTATION
============================================================

Update the current renderer-RE documentation with:

- final framing decision
- square-pixel default rationale
- optional --framing game behavior
- flare original behavior
- recovered old 4:3 visibility assumption
- native viewport visibility rule
- split-screen implications
- CSD/HUD findings
- targeting-reticle coordinate chain
- old/new comparison evidence
- deleted legacy dependencies
- remaining UNKNOWN items

Update NEXT_SESSION / handoff with:

completed
passed
failed
blockers
discoveries
files modified
exact next action
what must NOT be redone


============================================================
22. FINAL DECISION
============================================================

The decision is:

KEEP SQUARE-PIXEL / MODERN NATIVE FRAMING AS THE DEFAULT.

KEEP ORIGINAL CONSOLE FRAMING AS OPTIONAL:

--framing game

REWRITE FLARE VISIBILITY AGAINST THE ACTUAL ACTIVE NATIVE VIEWPORT.

Do NOT preserve the central legacy 4:3 visibility region.

If authored camera composition requires special handling, reconstruct it as camera semantics, NOT as a GameCube display artifact.


============================================================
23. NORTH STAR
============================================================

Remember:

The GameCube version is the behavioral and artistic reference.

It is NOT the architecture of the finished renderer.

Preserve:

- visual intent
- gameplay behavior
- authored composition where proven
- required timing
- deterministic behavior where required

Replace:

- pixel-aspect assumptions
- fixed framebuffer assumptions
- 4:3-only screen tests
- EFB implementation details
- GX state
- obsolete raster conventions
- console output limitations

Ask continuously:

"What was this code actually trying to accomplish visually?"

Then implement THAT directly for the native PC camera, viewport and renderer.

Do not make the PC renderer pretend to be a GameCube.

Make the game understand the real modern viewport.

Continue autonomously through:

RESEARCH
→ DISASSEMBLE
→ TRACE
→ UNDERSTAND
→ RECONSTRUCT
→ IMPLEMENT NATIVELY
→ RUN
→ COMPARE
→ FIX
→ VALIDATE
→ DELETE LEGACY
→ UPDATE HANDOFF

Stop only for a real unresolved semantic decision or contradictory evidence.
```
