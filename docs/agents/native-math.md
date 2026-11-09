# Agent report: native-math (branch agent/native-math, cut from gc-removal 352f2db, 2026-11-08)

Status: measurement done (this section); no conversion landed yet.

## Measurement: which guest FP code is called most

Tool: `GEKKO_CALL_COUNT=<out csv> GEKKO_CALL_COUNT_LIST=<entries file>` (system.cpp, non-shipping; the hook declines, so
the guest body runs as before; totals are written at exit). List: the 6,336 live bodies of the removal plan's live graph
that contain floating-point or paired-single opcodes (static scan of main.dol; 1,522 contain psq_l/psq_st or ps ops).
Runs: census exe (base + the counter), headless tape replays of 90 s: Westopolis (stg0100), Final Haunt (stg0604),
Lethal Highway (stg0202), each from the recording's `initial/` card, SRAM and configs.

**Limit:** the rank is calls x the body's static word count. It is not an executed-instruction count: early exits and
loops are not weighed, so a share is an upper-bound estimate. The sampler (tools/native_sample.py) is the check for any
single function.

| entry | Westopolis | Final Haunt | Lethal Highway | what it is |
|---|---|---|---|---|
| 0x804162AC | 22.6 % (7.46M calls) | 24.4 % (8.94M) | 20.2 % (7.82M) | half-float key track sampler (docs/engine/MOTION_PLAYER.md section 3) |
| 0x800221A8 | 4.7 % | 7.4 % | 9.2 % | 1,598 words, 855 FP opcodes, unnamed |
| 0x8000FF04 | 3.3 % (3.5M) | 2.2 % | 3.2 % | the game's sqrtf (frsqrte + 3 Newton steps + frsp) |
| 0x80414B80 | 3.7 % | 2.0 % | 1.7 % | curve rewind (MOTION_PLAYER) |
| 0x80423F20, 0x80415300, 0x80424958, 0x80425B20 | 1.6-2.8 % | | | motion/curve family |
| 0x804803E8, 0x804805EC, 0x80480100, 0x80471CA4, 0x80457720 | 1.2-2.9 % | | | RW range (native-boot's) |
| 0x8040DDF8, 0x8040E048, 0x8040DDC4, 0x8040DFA0, 0x8040C6CC/C5EC/C7E0 | 0.6-1.3 % each | | | small game vector helpers, 1-6M calls each |

Finding: the curve chain (0x804162AC, 0x80414B80, 0x80415300 ...) already has a pure native port,
`runtime/lift/motion/curves.{h,cpp}` (cursor + chunk bytes), but the translated guest functions still run; converting
the guest entries needs a wrapper that maps the guest track state (stream pointer, key A/B at +0x20/+0x30) onto it.
Raw data (untracked, in the worktree's .build/math/): west/fh/lh-calls.csv, fp_functions.csv.

## Game-side vector helpers, read to the instruction (0x8040DDC4 .. 0x8040E07C; all scalar single, no paired-single)

All are leaves on r3 (destination) / r4, r5 (sources) / f1 (scalar), `lfs` loads, `fadds/fsubs/fmuls/fmadds/fmsubs`,
`stfs` stores in the order below. Direct-call counts from `tools/doldis.py --xref` (interior entries included):

| entry | body | callers |
|---|---|---|
| 0x8040DDC4 | r3 = r4 - r5 (x, y, z: all loads first for x, then y/z interleaved; stores x, y, z) | 651 |
| 0x8040DDF8 | r3 = r4 * f1 | 434 |
| 0x8040DE20 | r3 = r4 + r5 | 378 |
| 0x8040DE54 | r3 -= r4 in place (store x, then load y of r3: aliasing-sensitive) | - |
| 0x8040E048 | r3 += r4 in place (same aliasing order) | 284 |
| 0x8040DFE0 | f1 = fmadds(r3.z, r4.z, fmadds(r3.x, r4.x, r3.y*r4.y)) (dot; note the order: y*y first, then x, then z) | 262 |
| 0x8040E008 | r3 = r4 x r5 with fmuls / fmsubs (cross) | 102 |
| 0x8040DFA0 | f1 = sqrt(x*x + y*y + z*z) through 0x8000FEE4 (adds: (x2 + y2) + z2) | 286 |
| 0x8040DEC4 | normalize: len = 0x8040DFA0; len < [r2+0x167C] (1e-6) -> store the zero vector (0x8040E07C's copy at 0x805E1860) via 0x800091D0, return 0; else x/len, y/len, z/len with fdivs | 153 |
| 0x8000FF04 | the game's sqrtf: x <= 0 handling, then frsqrte + 3 Newton steps in double, frsp; 3 direct callers + the wrapper 0x8000FEE4 (103) | 3 + 103 |

Mechanism decided: these have hundreds of direct callers each, so conversion is by site lists (extract/lift-sites-*.txt
`site <addr> <word> call <fn> <argc>`, generated from the xref of each entry and pinned by the DOL word), with the
function under `::gekko::scaffold` taking the host-visible values through `Cpu` memory accessors, the arithmetic in
`runtime/lift/math` with the FPSCR[NI] policy read at the call (a flush of denormal operands and results when NI is set;
see the open question in the notes about psq_l versus lfs). The first step is a benchmark: a lowered call must cost
less than the generated 10-word body or the conversion is not a gain, and then it is reported as "no measurable gain".

## Step 1: 3-vector helper owners (gekko::vec_math)

Owners (runtime/src/gekko/native_vec_math.cpp): Sub 0x8040DDC4, Scale 0x8040DDF8, Add 0x8040DE20, SubFrom 0x8040DE54, Length 0x8040DFA0,
Dot 0x8040DFE0, Cross 0x8040E008, AddTo 0x8040E048.

Ran: `vec_math_contract_tests` PASS, 1,240,000 calls against the original DOL bodies in the reference interpreter (both FPSCR[NI]
modes, overlapping operands, all float classes), negative controls caught (unfused dot 3,143, wrong NI 20,000). `verify_native_math.py`
PASS (earlier MSL ports only). Liveness roots check PASS. Westopolis 90 s headless, base vs candidate alternating 2+2, default and with
GEKKO_NATIVE_BG_READ/EXPAND_JOBS/ONE=0: PCM prefix b8e749ac, state report and game ticks (3,645,000,000) identical in all eight runs.
Process CPU overlaps (base 8.0-8.5 s, candidate 7.0-9.2 s): no measurable gain or loss.

Found by the test: the game's sqrt of +infinity is the default NaN 0x7FC00000 (inf*0 in the Newton chain), not +infinity. Scale's
multiplier is a single (outside-architecture non-single frC: SHADOW_PLAYER_COVERAGE_AUDIT section 12).

Not run: the 5-PNG check, strict oracle mode B, a guest watch (nothing retired).
