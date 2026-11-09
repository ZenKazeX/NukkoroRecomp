# Native OS migration - master integration plan (2026-11-02)

Integrates 01-06 in this folder (six read-only Sonnet 5.5 research agents). Nothing here is implemented; no SDK body
has been removed by this research. Claims marked VERIFIED were re-checked against the code by the integrating
session; everything else carries the tag its source document gave it.

## 1. Where the GameCube OS stands (03, verified counts from the tests-tree generator graph)

- 1,063 platform-SDK bodies still execute as translated PowerPC (shipping tree 1,057); 296 frontier rows.
- 513 SDK rows already pruned by the generator (reachability proof, not a tape).
- 96 native owners exist, every one still reads or writes guest structures; 0 SDK rows are natively reconstructed
  with no guest dependency.
- Runtime -> translated-SDK call sites: 11 (6 after the RtGCNFS step, which is in the wt-gc-removal worktree, NOT
  built / ctest-verified yet).
- 16 services (650 bodies) form one strongly connected cycle (os.*, audio.ax/dsp, gx, vi, card, trk, unidentified).
  The RenderWare GameCube driver sits above it (364 calls into GX).
- Biggest unknown: ~2,000 "unresolved" rows are GX's largest caller (probably RenderWare glue, unverified).

## 2. Facts the migration must respect (VERIFIED unless tagged)

1. Guest execution is one host thread on a virtual clock; interrupts are recognised only at fixed points. Hazards
   are guest-visible ORDERING problems, not host data races (02, INFERRED from code).
2. Two asynchronous paths: native sources via NativeInterrupts::deliver, and the decrementer (OS alarms) via
   Cpu::enter_os_exception -> guest DecrementerExceptionHandler 0x80371E14 (cpu.cpp:1359, 1437-1451 VERIFIED).
   A native scheduler must own or preserve both and their relative order.
3. Game / RenderWare / CRI never touch MSR; the interrupt contract is OSDisable/Enable/RestoreInterrupts
   (02, PROVEN by grep).
4. Every OSCreateThread call passes attr = 1 (detached): 0x8035A1F0, 0x803C5210/40/70/A0 (VERIFIED). No
   OSJoinThread, mutex, semaphore or message queue in the image (01). SwitchThreadCallback is a blr stub (01).
5. Threads: main (16), Sonicteam Thread workers (16, one-shot), CRI threads at 8 / 12 / 14 / 24 (01, PROVEN creation
   sites; per-stage population UNKNOWN). The priority-8 CRI loop 0x803C5510 increments a counter and re-checks a
   flag at +0xCB4 each pass (VERIFIED - bounded by that flag, not unbounded as 01 states).
6. Ordering rules: lowest priority number first, FIFO within a priority, ties never preempt, no time slicing,
   OSWakeupThread wakes the whole queue, handlers + callbacks finish before a woken thread runs, a preempted thread
   requeues at the tail, OSSleepThread with the scheduler disabled returns at once (01, PROVEN from code).
7. The idle spin 0x803788FC is already a wait point (extract/wait-points.txt:214, VERIFIED).
8. Only SDK code (os.threads + the mutex-release helper 0x80376C78) reads OSThread fields 0x2C8-0x304; a
   diagnostic FP probe in gameplay_scheduler.cpp reads 0x800000DC / +0x2FC and must be rewritten or deleted before
   any flip (04).

## 3. Target architecture (04 + 02, PROPOSED)

- Simulation domain: one deterministic game thread; translated, lifted and native code share one priority run
  queue (32 FIFO + mask); cooperative switching through leave_to_dispatcher; no locks inside it.
- Host domain: render, audio, I/O, decode, jobs as std::jthread; they reach the simulation only through the packet
  queue, an ordered I/O inbox and stop flags. Simulation timers in game ticks only.
- Events: an ordered EventHub; IrqGate owns the three interrupt functions; an Activation stack replaces guest
  handler frames and OSContext; idle = advance_to_next_event.
- Rejected: OS thread per guest thread (breaks the single register file and determinism); fibers (conflict with
  leave_to_dispatcher, not savestate-able).
- Unit of migration: an API owner plus the state it exclusively owns; ownership flips per build, proven by the
  generator's pruning; a guest structure is retired mirror -> poison -> delete.

## 4. Migration order (06 ranking reconciled with 03 leverage)

| # | milestone | old functionality made unnecessary | proof of removal | remains |
|---|---|---|---|---|
| M0 | Measure: per-stage thread census, scheduler / idle cost, replay determinism (gate G0), wake reasons | none | telemetry, repeated identical runs | everything |
| M1 | Finish RtGCNFS async retirement (build, ctests, ratchet 11 -> 6) | 5 OSReport call sites | ctests + liveness roots + runtime_coupling count | 6 sites |
| M2 | Gate 2 dynamic half: generator row marker + taint watch | none (tooling) | negative controls on reverb / OSInit | - |
| M3 | Native thread service (15 APIs + SelectThread / OSLoadContext tail + scheduler words) in MIRROR mode | 26 os.threads bodies (frees 7 alone, more with interrupts) | stage-2 oracle (scripted, exhaustive depth-4, seeded random), replays + schedule-trace diff, generator count | guest structures still mirrored |
| M4 | Interrupt gate + event hub incl. the decrementer / alarm path | os.interrupts, os.alarms, OSContext for interrupts, lazy FPU | 02's H-01..H-20 tests, replays | VI / GX callbacks as game behaviour |
| M5 | Stop mirroring (poison then delete guest thread / context state) | guest OSThread / OSContext / RunQueue writes | gate 2 dynamic watch shows no reader on all replays | - |
| M6 | Reverb init / shutdown, card (native save validation for __CARDVerify), boot chain | audio.ax 9, saves.card, os.core | per-service contract tests + what_if counts | - |
| M7 | RenderWare GameCube driver -> native RW + renderer, then GX | ~681 bodies | pixel gates + generator counts | unidentified rows |
| M8 | Identify and own the 61 unidentified frontier rows; the C++ lift replaces translated game code | remaining SDK + translated execution | lift verification, replays | - |

Async loading (06 S5) waits on M3 (the RtGCNFS owner refuses asynchronous files); audio off the game clock (06 S6)
is not recommended unless measurements show a cost.

## 5. Critical dependencies that must never be broken

- The single deterministic simulation thread and game-tick time base (replays, audio PCM).
- The relative order of interrupt handlers, callbacks and thread switches (01 item 6, 02 H-05).
- Full register state on every switch, FPRs included (the Gun Fortress freeze was an FPR loss).
- The guest structures any surviving SDK body still reads, until gate 2's dynamic half shows none.
- Savestates: a new format only with a one-way converter; never best-effort loading.

## 6. Validation and rollback gates (05)

Four independent proofs before any removal: caller coverage (allow-list tool with mutations), differential tests
against the original words, no reader of retired state (static + dynamic), replays plus a canonical schedule-trace
diff. Binary gates G0-G11, rollback triggers R1-R7 (rollback, never forward-patch). Rollout: shadow -> mirror ->
cut -> prune; the old path stays in the tree for one release. Tool limits: MSVC has ASan only (no TSan / UBSan);
invariants I1-I12, read-traps and a soak harness substitute. Sign-off wording: "equivalent on N scenarios, M random
schedules, 3 replays, these tripwires armed, these assumptions unproven" - never "proven correct".

## 7. Explicit unknowns

Per-stage thread population; replay run-to-run determinism; scheduler / idle CPU cost; whether non-SDK code reads
thread queues or the context below +0x2C8; register set completeness (FPSCR, GQR, the lwarx/stwcx reservation);
whether translated code keeps guest registers in host locals across a switch; the __OSDispatchInterrupt frame size;
whether host threads touch guest RAM; the "unresolved" row layer; whether any run depends on lazy-FPU stale values.

## 8. When is GameCube OS execution fully removed

All of: the generator compiles 0 os.* / interrupt / alarm SDK bodies (ratchet at 0); runtime -> SDK call sites 0;
no guest OSThread / OSContext / RunQueue / low-memory OS word is written or read by any surviving code (gate 2
static + dynamic on all replays); the native scheduler passes the stage-2 oracle and the schedule-trace diff on
all replays; savestates carry no guest OS state. Unreachable, renamed, wrapped or still-translated code does not
count as removed.
