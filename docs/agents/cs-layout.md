# cs-layout (CPU-code swarm, field 6): Cpu object layout and hot-member access

Branch cs/layout, base d526569. Scope: member order / alignment in runtime/include/gekko/cpu.h. No timing anywhere in this
report; every number is a count, an offset or a byte size.

## Result in one paragraph

The base layout puts the 512-byte fpr array and the architected scalars between the gprs and everything else the generated bodies
touch. All 32 gprs are inside the one-byte displacement reach, but every other member the bodies use (fm_base_, lr, pc, cr, msr_,
exit_pending_, stop_.reason ...) sits at offset 640..5200 and costs a 4-byte displacement. Declaring the hot scalars first and the
gpr array behind them (commit b6fe742, cpu.h only) turns ~33,000 disp32 accesses per shard into disp8 ones and shrinks shard
recomp_000's code from 2,111,579 to 1,990,895 bytes (-120,684 B, -5.7 %), measured by compiling the real generated source with the
real shard flags. Extrapolated to the 91.7 MB `.text` of the exe: about -5 MB (shard 000 measured; shard 023's disassembly census
predicts the same within 10 %, not compiled). It is exact by construction (layout only). The earlier s3/mem reorder (0234188,
withdrawn) was the same idea with a smaller prefix: exe -3.5 %, quiet-window speed median -0.6 % (four of five rounds faster)
= not a clear gain. This version reaches more code bytes (adds interrupt_lines_, stop_requested_, npc, fpscr, ctr) but I cannot
claim more speed than that earlier measurement bounds: the evidence here is code bytes and lines touched, as the contract asks.
Candidate for the coordinator, to be applied last; the main session decides whether 5 MB of I-cache / uop-cache footprint is
worth a 45-60 min full rebuild.

## 1. Which members the generated code touches (census)

Static source census of the 48 generated shards (37,139 bodies; `.build/cs/census.py` over
wt-opt-integrate/.build/watch/generated):

| member | source refs |
|---|---|
| c.gpr[i] | 2,953,787 (r0 527k, r1 531k, r3 523k, r4 322k, r31 299k, r30 169k, r5 139k, r29 72k ...) |
| c.fpr[i] | 310,464 (f0 116k, f1 82k, f2 30k, f31 17k) |
| c.mem_read32 / mem_write32 | 325,597 / 303,922 (each loads fm_base_) |
| c.pc / c.lr | 316,583 / 301,740 |
| c.exiting() + finish_transfers() | 238,275 (exit_pending_, then stop_.reason) |
| c.msr() | 92,817 |
| c.leave_to_dispatcher | 48,771 (inlined, 7 members each) |
| c.hle_clear + enter_slow (one thunk per body) | 37,139 each |
| c.ctr / c.cr / c.fpscr | 21,978 / 11,134 / 261 |
| c.needs_dispatch() | 14,700 |
| mem_read8 / 16 / 64, mem_write8 / 16 / 64 | 19k / 4k / 11k, 22k / 6k / 7k |

Per call: one `c.lr = ...` store, the call, `exiting()` (two loads), a `c.pc` compare after a bl; per back edge:
`needs_dispatch()` (stop_requested_, interrupt_lines_, msr_); per guest memory access one fm_base_ load (no restrict in this
build: SHADOW_RECOMP_RESTRICT off); per thunk one hle_slots_ load.

Disassembly census (dumpbin /disasm of recomp_000.obj and recomp_023.obj, `[reg+disp]` operands, base layout; per-shard
average of the two): fm_base_ 13,059; lr 7,894; pc 7,658; exit_pending_ 6,748; cr 4,994; stop_.reason 4,932; msr_ 2,206; xer
1,824; npc 888; fpscr 694; ctr 638; interrupt_lines_ 56; stop_requested_ 56; leave_to_dispatcher group (pending_native_call_
has_value, leave_site_, leave_reason_, interpreter_exits, record_fallbacks_, fallback_totals_[NotRecompiled]) 817-886 each.
gpr[0..31] operands (avg): r0 9,776; r1 9,776; r2 518; r3 8,139; r4 5,694; r5 2,446; r6 1,168; r7 541 ... r28 668; r29 965;
r30 2,490; r31 4,975; r17..r27 together ~1,500. fpr (offsets 128..639, always disp32): 6,421. The code uses r0/r1/r3/r4 and
r30/r31; r14..r27 are rare.

## 2. Offsets (offsetof probe `.build/cs/probe.cpp`, `#define private public`; sizeof(Cpu) 74,528 in both layouts)

| member | base | new |
|---|---|---|
| gpr[32] | 0..127 | 60..187 |
| fpr[32] (16 B each) | 128..639 | 192..703 |
| fm_window_ / fm_null_window_ / fm_base_ | 5032 / 5040 / 5048 | 0 / 8 / 16 |
| lr / pc / cr | 648 / 640 / 660 | 24 / 28 / 32 |
| msr_ | 5060 | 36 |
| xer / npc / fpscr / ctr | 656 / 644 / 664 / 652 | 40 / 44 / 48 / 52 |
| exit_pending_ / interrupt_lines_ / stop_requested_ | 5192 / 5112 / 5740 | 56 / 57 / 58 |
| stop_ (reason at +0, 48 B struct) | 5064 | 5064 |
| hle_slots_ | 7168 | 7168 |

Offset 0 needs no displacement byte, so gpr[0] loses one byte per reference in the new layout (~9,800 per shard); that is
already inside the measured total.

## 3. Displacement bytes in the lifted code

Base, shard 000: 108,642 `[reg+disp]` operands (disp != 0); 66,894 of them have disp >= 128 (4 bytes). New layout: 141,008
operands (gpr[0] now carries a byte), 106,708 one-byte, 34,300 disp32. What remains disp32: stop_.reason (5,805), the
leave_to_dispatcher group (6 x 867), gpr[17..31] at offsets 128..187 (~20k operands, mostly r30 and r31), fpr (always) and
hle_slots_[k] (32 KB table indexed by the hook hash: inherently disp32).

Measured effect, same compiler and flags (copied from the lifted tree's CL.command tlog: /O2 /Ob2 /arch:AVX2 /fp:precise
/std:c++20 /d2OptimizeHugeFunctions /bigobj, restrict off): `.text$mn` of recomp_000.obj 2,111,579 -> 1,990,895 B (5,571
sections both), -5.7 %. Raw data: `.build/cs/hdr_base.txt`, `hdr_new.txt` (dumpbin /headers), `.build/cs/dis/`.

A brute-force layout search over which scalars to place in front (real per-offset operand counts, 0 / 1 / 4 byte model)
predicted -127 KB per shard for the best possible prefix of all 13 scalars; the committed prefix is that set (stop_ excepted,
see section 5), and the compile confirms it (-120.7 KB).

## 4. Cache lines per call (line = 64 bytes from the object start)

Base: call + return + exiting() + loop test touch lines 10 (pc, lr, cr at 640..667), 78 (fm_base_ 5048), 79 (msr_ 5060,
stop_.reason 5064, interrupt_lines_ 5112), 81 (exit_pending_ 5192), 89 (stop_requested_ 5740), the gpr lines 0-1 and the
thunk's hle_slots_ line: 5 header lines besides the gprs. New: line 0 holds fm_window_, fm_null_window_, fm_base_, lr, pc, cr,
msr_, xer, npc, fpscr, ctr, exit_pending_, interrupt_lines_, stop_requested_ and gpr[0]; lines 1-2 the other gprs; line 79 only
stop_.reason; plus hle_slots_. exiting() is one load in line 0 and one in line 79; needs_dispatch() is three loads, all in
line 0. So 2 header lines (0 and 79) instead of 5. The hle_slots_ load is one line per distinct hooked address by construction
(hash index into a 32 KB table); no layout can share it with the scalars. These lines are hot and stay in L1 either way: this
is the smaller of the two wins, the code bytes are the larger.

## 5. Not done, with numbers

- stop_.reason: 5,805 disp32 operands in shard 000, 238,275 exiting() sites in the exe; both loads in reach would save about
  2 x 3 bytes per site (~1.4 MB). CpuStop is a 48-byte struct (std::string inside) that must stay contiguous; 48 bytes in front
  of the gprs would push almost all of them out of reach, so it is NOT moved. Routed to cs-exit: if stopped() is folded into
  exit_pending_ (contract: only with a stop-from-every-path test) the second load and its 7 bytes per site disappear; otherwise a
  one-byte mirror of "stopped" next to exit_pending_ (written wherever stop_.reason is) gives the same in-reach load. With
  exit_pending_ at 56 and stop_requested_ at 58 a new byte goes at offset 59.
- The leave_to_dispatcher group (pending_native_call_ 8 B, interpreter_exits 8 B, leave_site_ 4 B, leave_reason_ 1 B,
  record_fallbacks_ 1 B; 867 inline sites per shard x 5-6 operands). Moving it in front costs ~24 prefix bytes (gpr[11..16] out
  of reach, ~1.3k operands) for ~4.4k operands in reach: model estimate +10 KB per shard (+0.5 %), NOT measured, not committed
  (it spreads the diff over members other agents edit).
- hle_slots_: fixed by the hash design (disp32 in every thunk: 37,139 x 3 bytes = 111 KB). Nothing in member order helps.

## 6. Routed proposal (tools/ppcrecomp is opt-runtime's): a biased base, NOT proposed as a diff

disp8 covers [-128, 127]. Today `c` points at offset 0, so the reachable 128 bytes are shared between the gprs (128 B) and the
scalars (60 B): gpr[17..31] (about 20k operands per shard, ~60 KB) fall out. If `c` pointed 128 bytes into the allocation, the
scalars at negative offsets and all 32 gprs at 0..127 would be disp8 and gpr[0] would again need no byte. A C++ `Cpu&` has no
negative-offset members, so this needs the emitter to pass a pointer to a middle-of-block hot struct and every `c.member`
spelling in the emitter (and every Cpu construction in the tests) to change: large, and I have no verified diff. Estimated extra
gain over the committed layout: ~+43 KB per shard (about 2 % of .text). Not proposed this round.

## 7. Patch to apply last (re-apply recipe)

Commit b6fe742 (cpu.h only, 30 insertions / 14 deletions, LF kept). Target layout: at the top of the class, before
`u32 gpr[32]`, in this order: private fm_window_, fm_null_window_, fm_base_ (this relative order is required by
difftest/f1c_canary_driver.cpp, which locates them by value); public lr, pc, cr; private msr_; public xer, npc, fpscr, ctr;
private exit_pending_, interrupt_lines_, stop_requested_; then public gpr, fpr. Remove the later declarations of those members.
If cs-exit adds a byte next to exit_pending_, put it right after stop_requested_ (offset 59: gpr stays at 60). If cs-hle or
cs-exit remove one of the prefix members, drop it; the rest still fits below 60. Rule: gpr must start at a multiple of 4 with
gpr[16] <= 127, i.e. the prefix must stay <= 63 bytes. No code reads a member by offset (grep: only sizeof(Cpu) in the F1C
driver); savestate and the state-diff tool go field by field.

## 8. Tests

Tests tree configured per the contract (`.build/tests`), targets gekko_core_tests and ppc_recomp_tests built with b6fe742 under
the capped runner (386 s wall), `ctest -C Release -R '^(gekko_core_tests|ppc_recomp_tests)$'`: both Passed (8.54 s, 0.70 s),
100 % of 2. Layout-only change: there is no failing-before test (nothing behavioural changes); the evidence is the compile
measurement above. Not run (shared slot, contract round 1): the full ctest, f1c_canary_driver (already a baseline failure in
phase B1; its fm_window_ < fm_null_window_ < fm_base_ order assumption is kept), the state.json / PCM / behaviour gates
(coordinator).
