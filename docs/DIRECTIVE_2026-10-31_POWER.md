# Directive 2026-10-31: native power management and frame pacing (verbatim from the user)

SHADOW RECOMP — NATIVE POWER MANAGEMENT & FRAME PACING ARCHITECTURE
PRIMARY OBJECTIVE
Design and implement a lightweight, intelligent, cross-vendor CPU/GPU power-management system for Shadow Recomp.
Deliver stable 60 FPS with minimum unnecessary energy consumption, low input latency, consistent frame pacing, and absolutely no rendering or gameplay compromises.
Target real-world gaming resolutions first: 1920×1080, 2560×1440, and 3840×2160. Retain ultrawide and higher-resolution support as separate test configurations.
The existing RTX 4080 measurements show significant GPU frequency changes under light workloads. Investigate this behavior rigorously rather than assuming that low clocks are inherently a problem.
PHASE 1 — INSTRUMENT THE ENTIRE FRAME
Build an independent performance and power telemetry system.
Measure:

* Actual GPU clock frequency, power consumption, and utilization when available.
* CPU package power, per-thread load, and processor residency where supported.
* CPU game-thread execution time.
* GPU execution time using timestamp queries.
* GPU queue waiting and CPU submission overhead.
* Frame submission, presentation, and actual display timing.
* Frame-time p50, p90, p99, maximum, and missed deadlines.
* CPU/GPU idle periods, synchronization stalls, and unnecessary wakeups.
* Energy per completed frame when trustworthy power measurements are available.

Distinguish game simulation frames, rendered frames, submitted presents, displayed frames, and repeated images.
The previously observed 142 repeated frames must be investigated individually. Determine whether they originated from scheduling, present synchronization, GPU stalls, or intentional game behavior.
Never infer actual displayed FPS from GPU execution time.
Do not fabricate unavailable telemetry.
PHASE 2 — REVERSE-ENGINEER FRAME SCHEDULING
Trace the game's original 60 Hz clock, animation timing, simulation updates, render submission, and synchronization.
Reconstruct the complete timing dependency chain.
Identify and eliminate:

* Busy-wait loops.
* Excessive CPU polling.
* Redundant synchronization.
* Unnecessary thread wakeups.
* Avoidable GPU queue stalls.
* Redundant frame submissions.
* Legacy console timing dependencies.

Investigate DXGI frame-latency controls, frame-latency waitable objects, presentation modes, and appropriate operating-system scheduling mechanisms.
Do not change gameplay timing, physics, animation speed, audio synchronization, or simulation results.
PHASE 3 — DESIGN THE POWER CONTROLLER
Implement a frame-budget-aware controller with three policies:
Balanced (default): Maintain stable 60 FPS while minimizing unnecessary CPU/GPU activity and energy use.
Performance: Prioritize consistent frame delivery and low latency, without artificial GPU workloads or mandatory clock overrides.
Efficient: Further reduce idle CPU activity, scheduling overhead, and avoidable resource usage while preserving the same 60 FPS and rendering quality.
Use measured workload trends, appropriate headroom, hysteresis, and bounded control adjustments to avoid oscillation.
React to genuine sustained deadline pressure, not isolated clock readings.
Allow the operating system and GPU driver to manage hardware frequencies unless an explicitly supported, user-authorized control mechanism is available.
Do not silently modify system-wide power plans.
PHASE 4 — OPTIMIZE ACTUAL ENGINE WORK
Investigate native engine changes that reduce energy consumption by reducing real computation:

* Remove unnecessary CPU wakeups and dispatch overhead.
* Consolidate compatible tasks without changing their execution dependencies.
* Improve GPU command submission efficiency.
* Reduce unnecessary synchronization and memory traffic.
* Optimize asset-loading and stage-transition power spikes.
* Improve frame pacing at the presentation boundary.
* Preserve GPU idle opportunities when the frame is complete.

Prefer doing less work over trying to force hardware to run faster.
Keep every optimization portable across NVIDIA, AMD, and Intel, with future Vulkan support in mind.
PHASE 5 — ANTI-CHEATING VALIDATION
This phase is mandatory.
Establish an immutable baseline and run controlled A/B tests with the exact same:

* Executable configuration and gameplay tape.
* Internal rendering resolution.
* 4× MSAA and all rendering effects.
* Gameplay simulation timing.
* Texture quality, filtering, and draw workload.
* Audio and animation behavior.
* Frame presentation requirements.

Perform both normal-driver-clock and explicitly labeled diagnostic locked-clock benchmarks.
Only normal-driver-clock results qualify for real-world power-management success.
Report CPU/GPU frame times, displayed-frame timing, power, energy per frame, missed deadlines, and repeated images.
Collect multiple cold and warm runs. Preserve raw logs, settings, timestamps, and reproducible test commands.
A reduction in reported GPU milliseconds is not sufficient if real frame delivery, latency, or energy efficiency becomes worse.
ABSOLUTELY FORBIDDEN

* Artificial GPU workload intended to keep clocks high.
* Hidden resolution scaling.
* Lowering MSAA or graphics quality.
* Dropping particles or expensive effects.
* Skipping simulation or rendering work.
* Manipulating replay timing.
* Reporting theoretical FPS as observed FPS.
* Silently forcing GPU clocks or operating-system power settings.
* Cherry-picking favorable frames.
* Excluding CPU preparation, synchronization, or presentation overhead.
* Hardcoding optimizations for recorded benchmark scenes.
* Weakening gameplay or rendering validation.
* Calling a GPU frequency increase an engine optimization.

PHASE 6 — ACCEPTANCE GATES
The new system is accepted only if it demonstrates:

1. Stable, correctly paced 60 FPS in the supported test configurations.
2. No new missed deadlines, unintended repeated frames, or latency regressions.
3. Identical gameplay, animation, audio, and required rendering outputs.
4. Lower measured energy consumption for equivalent completed work, or independently demonstrated improvements in frame pacing without misleading energy claims.
5. Reproducible results under normal driver power management.
6. Clean fallback behavior when power telemetry is unavailable.
7. No vendor lock-in or hidden system-wide changes.

Benchmark Westopolis and Final Haunt, including dense smoke, stage loading, and long uninterrupted gameplay sessions.
Test AMD, Intel, and Vulkan separately when hardware or backends become available. Never claim untested compatibility as validated.
Keep the controller disabled by default until all required gates pass.
FINAL ENGINEERING DIRECTIVE
Reverse-engineer the timing architecture first. Build the telemetry system second. Optimize actual scheduling and work third. Add adaptive power policies only after their effects can be measured.
The power manager must never manufacture performance by degrading the result.
Our goal is a genuinely efficient native PC engine, not impressive-looking benchmark numbers.
SAME GAME. SAME IMAGE. SAME 60 FPS. LESS ENERGY. BETTER FRAME PACING.
NO CHEATING. NO FAKE RESULTS. NO QUALITY REDUCTIONS.
