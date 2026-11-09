# User directive, 2026-10-26: complete GameCube architecture elimination and native x64 engine replacement

Saved verbatim at the user's request ("Save this for next session"). It is the governing brief for the next session,
alongside docs/NORTH_STAR.md / NORTH_STAR_COMPACT.md, which it sharpens. Start from docs/NEXT_SESSION.md.

---

SHADOW THE HEDGEHOG RECOMP
COMPLETE GAMECUBE ARCHITECTURE ELIMINATION AND NATIVE x64 ENGINE REPLACEMENT

ABSOLUTE DIRECTIVE
We are not building a better GameCube emulator.
We are not building a GameCube compatibility layer.
We are not preserving the original GameCube rendering architecture.
WE ARE REPLACING THE OBSOLETE CONSOLE IMPLEMENTATION WITH A GENUINELY NATIVE, LIGHTWEIGHT, HIGH-PERFORMANCE x64 PC IMPLEMENTATION.
The original GameCube code exists only as an offline mathematical and behavioral reference.
You have access to the original operations, instruction sequences, state transitions, mathematical transformations, and observable results.
Use that information to determine WHAT each system computes, not to preserve HOW the GameCube computed it.
The final runtime must not depend on any GameCube-specific hardware model, instruction interpreter, compatibility subsystem, or obsolete framebuffer architecture.

1. REPLACE THE IMPLEMENTATION, NOT THE BEHAVIOR
The fundamental engineering principle:
Equivalent observable behavior does not require equivalent instructions, algorithms, data structures, or hardware abstractions.
Study the original mathematical operations and derive their actual computational purpose.
Then implement that purpose using the most appropriate native PC algorithms.
For example:

* Replace legacy coordinate transformations with optimized native matrix operations.
* Replace GameCube-specific framebuffer calculations with native DirectX 12 rendering.
* Replace GX state translation with precomputed native pipeline states.
* Replace obsolete command-processing structures with direct graphics operations.
* Replace unnecessary memory copies with efficient GPU resource operations.
* Replace hardware-specific synchronization with modern dependency management.
* Replace instruction-by-instruction mathematical sequences with equivalent optimized formulas.
* Replace expensive legacy data layouts with cache-efficient native structures.

Do not mechanically translate obsolete algorithms if a simpler mathematical solution exists.
The original implementation is evidence, not a design requirement.

2. NO GAMECUBE RUNTIME ARCHITECTURE
The production executable must not contain or depend on:

* GameCube GPU emulation.
* GX compatibility execution.
* EFB emulation.
* XFB emulation.
* PowerPC instruction interpretation.
* GameCube register emulation.
* GameCube memory-mapped hardware interfaces.
* GameCube hardware timing simulation.
* Console framebuffer resolution limitations.
* Console-specific rendering command processing.
* Legacy hardware abstraction wrappers.
* Hardware-specific cache emulation.
* Redundant console memory organization.

Replace these systems with genuinely native PC implementations.
Do not merely rename old structures or hide them behind new interfaces.
Do not replace one compatibility layer with another compatibility layer.
If an existing subsystem still requires obsolete console behavior, trace its dependencies and replace the underlying computation.
The target is zero GameCube hardware dependencies in the production runtime.

3. COMPLETELY REPLACE THE 640×480 RENDERING MODEL
The old internal rendering resolution must disappear as an architectural concept.
Do not use 640×480 as a hidden rendering base.
Do not use integer resolution multipliers.
Do not calculate arbitrary intermediate resolutions.
Do not render low-resolution scenes and enlarge them.
The renderer must operate directly at the exact resolution selected by the player.
Examples:

* 1920×1080 → Actual 1920×1080 rasterization.
* 2560×1440 → Actual 2560×1440 rasterization.
* 3840×2160 → Actual 3840×2160 rasterization.
* 3440×1440 → Actual 3440×1440 rasterization.
* 5120×1440 → Actual 5120×1440 rasterization.

Use native DirectX 12 resources, pipelines, viewports, depth buffers, and render targets.
Design the renderer around dynamic physical framebuffer dimensions rather than console-era constants.
Support standard aspect ratios including 4:3, 5:4, 16:9, 16:10, commercial 21:9, 32:10, and 32:9 formats.
No obsolete console framebuffer should remain in the rendering pipeline.

4. MATHEMATICAL RECONSTRUCTION INSTEAD OF EMULATION
You can inspect the original mathematics.
Use that advantage.
For each legacy operation:

1. Identify its inputs.
2. Identify its outputs.
3. Determine the mathematical relationship.
4. Identify the observable behavioral requirements.
5. Derive a simpler native equivalent.
6. Implement that equivalent using modern CPU or GPU capabilities.
7. Validate the replacement against the original reference.
8. Remove the obsolete runtime implementation.

Prefer direct native computation over reproduced console instruction sequences.
Examples of acceptable transformations:

* Multiple legacy matrix operations → One combined matrix transformation when mathematically valid.
* Repeated coordinate conversions → One precomputed transformation.
* Redundant scalar operations → Optimized SIMD operations.
* Obsolete framebuffer copies → Direct native render-target operations.
* Legacy graphics state changes → Efficient cached DirectX 12 PSOs.
* Unnecessary temporary memory buffers → Direct GPU resource access where safe.
* Hardware-specific synchronization → Native CPU/GPU synchronization with equivalent ordering guarantees.

Use SSE2, AVX2, FMA, or other applicable CPU capabilities only when supported by the target and proven beneficial.
Provide appropriate CPU feature dispatch where necessary.
Avoid introducing numerical differences that affect game logic.
Do not assume that mathematically equivalent expressions always produce identical floating-point results.
When numerical precision, rounding, operation order, or timing affects observable behavior, determine the actual required tolerances experimentally.

5. MODERN MEMORY AND CACHE ARCHITECTURE
Eliminate obsolete console-oriented memory layouts.
Replace them with native x64 memory organization optimized for current Intel and AMD processors.
Prioritize:

* L1-friendly frequently accessed data.
* L2-efficient working sets.
* L3-efficient shared data.
* Reduced pointer chasing.
* Reduced cache misses.
* Fewer temporary allocations.
* Data-oriented structures when beneficial.
* SIMD-friendly contiguous memory.
* Appropriate memory alignment.
* Lower synchronization overhead.
* Efficient CPU-to-GPU transfers.

Do not retain obsolete structures merely because the original game used them.
Select data layouts based on actual access patterns and measured performance.
Do not increase memory complexity without demonstrable benefit.

6. GAMEPLAY BEHAVIOR MUST REMAIN EQUIVALENT
This is the only legacy requirement:
THE PLAYER MUST EXPERIENCE THE SAME GAME BEHAVIOR.
Preserve externally observable results, not the original hardware implementation.
The native replacement must reproduce:

* Player movement.
* Collision behavior.
* Physics interactions.
* Animation results.
* Camera behavior.
* Mission progression.
* Enemy behavior.
* Weapon functionality.
* Object interactions.
* Game timing.
* Audio playback behavior.
* Visual effects.
* Save-game functionality.

These outcomes must be achieved through modern native implementations.
Where exact equivalence is necessary, require exact equivalence.
Where higher-resolution rasterization makes pixel-identical output impossible, require equivalent scene geometry, visual semantics, correct composition, and absence of regressions.
Do not retain legacy execution machinery as a shortcut to compatibility.
The production engine should reproduce the game's behavior because the replacement algorithms are correct, not because it continues secretly executing an obsolete console model.

7. NO FAKE NATIVE IMPLEMENTATIONS
The following are prohibited as final solutions:

* A PowerPC interpreter compiled for x64.
* GameCube instruction emulation hidden inside C++.
* An obsolete low-resolution framebuffer hidden behind DX12.
* Repeated legacy calculations wrapped in a native interface.
* A compatibility subsystem presented as a new engine.
* Fake native resolution through post-processing.
* Repeated conversions between obsolete console data formats.
* Redundant hardware synchronization recreated unnecessarily.
* Hardcoded console assumptions hidden in shaders.
* Arbitrary resolution multipliers.

Compiling legacy emulation code into an x64 executable does not make the underlying architecture genuinely native.

8. ORIGINAL IMPLEMENTATION IS AN OFFLINE TEST ORACLE ONLY
The original game and existing research tools may be used outside the production executable to establish mathematical and behavioral reference results.
They must not become runtime dependencies.
For every replacement, construct comparisons using representative states, inputs, and output traces.
Test:

* Numerical results.
* Coordinate transformations.
* Frame-to-frame determinism.
* Animation behavior.
* Camera behavior.
* Collision results.
* Rendering correctness.
* Memory access safety.
* CPU/GPU execution costs.

Maintain separate offline evidence so that the old implementation can eventually be removed from the production source tree without losing validation capability.
Never claim equivalence solely because a new implementation looks correct.

9. PERFORMANCE MUST BE MEASURED
The new implementation should be faster, simpler, and lighter wherever possible.
Measure:

* CPU frame time.
* GPU frame time.
* Draw-call overhead.
* Resource allocation overhead.
* Memory bandwidth.
* Cache misses.
* GPU memory consumption.
* Redundant processing.
* Synchronization stalls.
* Runtime code and data footprint.

Do not replace an old subsystem with a larger, slower modern abstraction merely for architectural appearance.
A replacement is successful only when it satisfies behavioral correctness and the documented performance objectives.
Do not sacrifice correctness to obtain artificial benchmark improvements.

10. EXECUTION STRATEGY
Proceed subsystem by subsystem.
For each subsystem:

1. Audit the original implementation.
2. Extract its mathematical and behavioral specification.
3. Identify obsolete console dependencies.
4. Design the native x64 replacement.
5. Implement the replacement.
6. Compare results against the offline reference.
7. Benchmark performance and resource usage.
8. Integrate the native implementation.
9. Remove the obsolete production runtime code.
10. Run the full regression suite.

Track which GameCube-dependent components still exist.
Maintain an explicit dependency elimination report.
Do not declare a subsystem complete while hidden compatibility execution remains.
Do not remove required behavior before its native replacement is operational and validated.

FINAL ACCEPTANCE CRITERIA
The completed Shadow the Hedgehog PC engine must:

* Run as a native x64 application.
* Use actual native DirectX 12 rendering.
* Render at the user's selected physical resolution.
* Contain no GameCube hardware emulation or runtime compatibility layer.
* Use modern CPU and GPU algorithms.
* Use efficient native memory management.
* Eliminate unnecessary legacy instruction sequences.
* Reproduce the original game's observable behavior.
* Pass numerical, gameplay, rendering, and timing regression tests.
* Demonstrate measurable resource efficiency.

THE ORIGINAL GAMECUBE IMPLEMENTATION IS A MATHEMATICAL REFERENCE, NOT A RUNTIME FOUNDATION.
Do not preserve obsolete architecture.
Do not emulate obsolete architecture.
Understand the computation, derive the native equivalent, validate the results, and eliminate the old implementation.
SAME GAME. SAME BEHAVIOR. COMPLETELY NATIVE EXECUTION. ZERO GAMECUBE HARDWARE DEPENDENCIES.
