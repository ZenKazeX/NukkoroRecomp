# The game's spine (2026-10-31)

Question: which code in main.dol is Shadow's own game logic, independent of the GameCube environment, so that it can
be compiled natively for the PC instead of being translated as guest code?

Tool: `tools/dolrecon/spine.py` (`python -B -m dolrecon.spine ../.build/dolrecon/pal-v30 ../.build/dolrecon/spine`,
run from `source/tools`, about 2 s). Output in `.build/dolrecon/spine/`:

| file | contents |
|---|---|
| `functions.csv` | every function: layer, the rule that gave it, the evidence, environment instructions, calls per layer |
| `boundary.csv` | every non-game function the game calls directly, with call sites and callers |
| `game_platform_glue.csv` | the game functions that call the SDK or touch hardware, with the SDK callees |
| `summary.json` | the numbers below |

## Method (evidence only; no layer from an address range alone)

Each function gets one layer from the first rule that applies:

| rule | evidence |
|---|---|
| A1 name | dtk's exact-byte library names (the dtk-only `pal` project), `main` is game |
| A2 rw | STRONG RenderWare anchors from `RW37SDK/_mined/rw_guest_naming_candidates.csv` |
| A3 class | vtable slot or constructor of an RTTI class; namespace decides (boost:: / std:: / game) |
| A4 string | a referenced literal with a library signature (CRI error codes, SDK and runtime messages) |
| S1 between | unanchored code between two anchors of the same layer in link order, nothing else between |
| G1 graph | all resolved CALLERS have one layer, and its callees are ones that layer may call |
| S2 game unit | between a game anchor and a Boost anchor: in the game's object files, game or Boost undecidable |

Every instruction is also scanned for GameCube environment use: hardware registers (lis 0xCC00/0xCC01/0xCD00), the
uncached mirror, locked cache, low-memory OS globals, supervisor SPRs, MSR, time base, GQR, rfi/sc, cache control,
barriers. Paired-single math is counted separately (it is a CPU SIMD feature, portable as float math).

The RTTI layer depends on the corrected RTTI model (same day): a CodeWarrior RTTI record is {name, base list}, the
base list is {record, offset}... 0, and a vtable is the data object {record, offset, methods}. The earlier model
read the base list as the vtable and missed every class without bases; 1,620 classes and 6,988 virtual slots now
(audit B6: all slots are function starts), up from 1,080 and 4,997.

## Update 2026-11-01: pal-v30 (G1 no longer decides game inside game / Boost intervals)

Astra review 6: S2 ran after G1, so a G1 "game" inside an interval between a game anchor and a Boost anchor survived,
although being called only by game code is what a template instance is. G1 now skips those (S2 gives them
game-unit), and a class that is only used (N2 construct, class_uses) gives a layer only when it is a game class.
game 22,015, game-unit 5,584, unresolved 1,795 (pal-v29: 26,952 / 1,332 / 1,089); the 708 newly unresolved are their
callees outside the intervals. The game-side total (game + game-unit) is 27,599 (pal-v29 28,284).

## Update 2026-11-01: pal-v29 (Boost shared_ptr code recognised by body)

names.py N16 (docs/DOLRECON.md "Boost shared_ptr / shared_count by body") gives 488 functions Boost's class; A3 reads
it. 479 of them had been game (S1 285, G1 194: the weak spot below, Boost code called only by game code). Their
neighbours lost link-order claims: game 26,952, game-unit 1,332, boost 1,366, unresolved 1,089 (pal-v26: 28,056 /
981 / 878 / 824). S1 "game" inside a game unit means game code or an unrecognised template instance; these 479 show
it, so the lost claims stay lost until more instances are recognised by body.

## Update 2026-11-01: pal-v26 (secondary vtables and thunks are class evidence)

A3 now reads every vtable slot (corrections.slot_holders, docs/DOLRECON.md "RTTI conflicts, secondary vtables and
thunks in ownership"): the bodies in 563 secondary vtables and 1,039 this-adjusting thunks anchor their class's layer,
and a body counts as emitted in its users only when the class it belongs to is a template / Boost / std class.
Layers: game 28,056, game-unit 981, unresolved 824, the rest unchanged (pal-v25: 27,779 / 1,206 / 876); 225
game-unit and 52 unresolved functions became game, none left it. A3 anchors 6,697 -> 8,198.

## Update 2026-11-01: pal-v24 (layer rules corrected after Astra review 5)

A3 now reads the class names.py records (names.json "class", "class_uses"), not its evidence prose: the prose regex
had never matched template class names, so no sp_counted_base_impl<T> constructor was a Boost anchor. Two rules
were corrected once they were: G1 no longer gives "boost" to a function only Boost calls (Boost templates
instantiated for a game type call that type's code: deleters, bind), and template / Boost / std class anchors, whose
code CodeWarrior emits in the units that use it, end an S1 span but seed none (they had spread "boost" and "msl" over
game code between them). A4 takes pooled strings per use (dolrecon.strrefs) and no layer from conflicting strings.
Layers: game 27,781, game-unit 1,205, boost 877, rw 978, cri 1,180, sdk 969, msl 142, runtime 37, unresolved 868
(pal-v22: 28,048 / 980 / 1,403 / 976 / 1,180 / 969 / 135 / 37 / 309). Claims went down where the evidence was link
order across a template instance (+559 unresolved). Remaining weak spot: G1 still calls a Boost-emitted function
"game" when only game code calls it (sp_counted_base::release 0x8000945C).

## Update 2026-10-31 late: pal-v10 (constructors, destructors, sizes, matched source)

With names rules N6/N7 (constructors and destructors read from the object code) and M0 (matched source), the class
evidence grew and the spine moved accordingly: game 27,811, game-unit 1,149, boost 1,471, rw 976, cri 1,180,
msl 135, runtime 37, sdk 969, unresolved 309. Game-side 28,960 functions; still only the same 4 touch hardware;
121 call the SDK; 28,338 (3.45 MB) are pure logic. The source skeleton generated from it:
`.build/dolrecon/skeleton/` (tools/dolrecon/skeleton.py: 1,620 class headers, functions.csv, platform/boundary.h).
Matched source lives in `source/decomp/` (9 functions so far). The table below is the first (pal-v5) run.

## Result (PAL, 34,037 functions; first run, pal-v5)

| layer | functions | bytes | what it is |
|---|---:|---:|---|
| game | 25,709 | 3,377,980 | Sonic Team code: Player, enemies, bosses, objects, stages, missions, camera, HUD, saves, engine libraries |
| game-unit | 1,680 | 158,496 | inside the game's object files, game code or an instantiated Boost template |
| boost | 3,030 | 268,560 | Boost templates (shared_ptr, function, bind): portable C++ |
| rw | 976 | 321,868 | RenderWare Graphics 3.7 (core, world, plugins, GameCube driver) |
| cri | 1,180 | 347,100 | CRI ADX, Sofdec (SFH 1.19, SUD 0.05, MW player) |
| msl | 135 | 53,244 | Metrowerks C library, std:: exceptions, operator new/delete |
| runtime | 37 | 3,788 | CodeWarrior startup and C++ ABI helpers |
| sdk | 969 | 214,572 | Dolphin SDK (OS, DVD, VI, PAD, SI, EXI, AI/AR/AX/DSP/CARD, GX) and MetroTRK |
| unresolved | 321 | 113,280 | no rule applies; ~200 of them sit between the Sonic Team libraries and RenderWare |

Link order (CodeWarrior links object files and archives contiguously): game units 0x80008D40-0x8037xxxx, SDK +
TRK + runtime + MSL 0x8037-0x803B, CRI 0x803B-0x8040, Sonic Team engine libraries 0x8040-0x8044, RenderWare
0x8045-0x804A.

### How independent the game code is

Of the 27,389 game-side functions (game + game-unit, 3.54 MB):

- **4 touch the GameCube directly.** Three read the bus clock at 0x800000F8 after `OSGetTime` (the inlined
  `OSTicksToMilliseconds` macro: fn_80051414, fn_80051460, fn_80440FAC); one writes vertices to the GX FIFO
  0xCC008000 (the disc-error screen, 0x8032E038, inlined GX immediate mode).
- **121 call the Dolphin SDK directly** (352 sites into 151 SDK functions): interrupts as critical sections
  (OSDisable/RestoreInterrupts 23 functions each), memory card (CARDProbeEx 17, CARDUnmount...), OSReport, threads,
  time, arena, cache flush, VI TV format, DVD status, reset button, GX for 2D/debug drawing, AX sound.
- **471 call RenderWare** (1,410 sites into 193 RW functions), **36 call CRI** (103 sites into 42 functions).
- **26,772 functions (3.30 MB) are pure logic**: no hardware, no SDK, no RenderWare, no CRI. They call only each
  other, Boost, and the C/C++ runtime (operator new/delete 4,706 sites, memset/memcpy, strings, math).
- 1,032 game functions use paired-single instructions (vector math). Those are portable float math.
- No library references a game function by address: callbacks reach the SDK only as arguments the game passes.

### What a native PC build needs, by layer

| layer | PC side |
|---|---|
| game + game-unit + boost | the game's source, as real C++ compiled for x64 (decompiled functions, Boost from source) |
| the 4 + 121 glue functions | rewritten against a small PC platform layer (time, critical sections, memory card = save files, pad, reset) |
| RenderWare 3.7 | RW 3.7 for PC exists in `RW37SDK/` (D3D9 / OpenGL libraries, same API version); the GameCube-only RW calls in game code need a list (next step) |
| CRI | ADX / Sofdec decoding on the PC (42 CRI entry points called by the game) |
| msl + runtime | the host C/C++ runtime |
| sdk | not ported: replaced by the platform layer above |

## What this does not prove yet

- **Layer assignment is evidence-based, not complete:** 321 functions are unresolved, and S1/S2 rest on link
  order. A function wrongly placed inside a game run would still be scanned for hardware; none of the 27,389 shows
  hardware use beyond the four above.
- **Data dependencies are not counted:** game reads of SDK or RenderWare globals through pointers are not in the
  boundary, only direct calls are.
- **Struct layouts are not covered:** "pure logic" means no environment instructions and no direct environment
  calls. Code written against GameCube-sized structures (32-bit pointers, big-endian data files) still needs those
  layouts kept or converted on the PC.
- **This is a map, not source:** the C++ in `runtime/tests/generated/` is register-level (State/Mem, guest
  semantics). Native source means decompiling these functions into real C++. `tools/dolrecon/match.py` (same day)
  is the start of that: it compiles candidate C/C++ with the game's CodeWarrior compiler and accepts a function only
  when it is byte-identical.

## Known naming defects found on the way

- FIXED (pal-v10): 0x803A1380 is `operator new` and 0x803A1334 `operator delete`, now PROVEN by matching
  (source/decomp/msl/New.cpp) and named by rule M0.
