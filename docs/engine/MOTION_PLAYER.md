# The motion player (animation hierarchy) - reverse engineering record

Started 2026-10-30 (Phase VIII). Evidence levels: P = proven from the code word / runtime capture cited, S = strongly
supported, U = unknown. Addresses are the PAL DOL (game/sys/main.dol, tools/doldis.py). Earlier notes:
docs/distant-rendering/lod_streaming.md section 3, docs/CPU_FINAL_HAUNT.md.

## 1. Cost (P, measured)

Baseline binary .build/validation/perf-2026-10-30/base8 (exe + map kept together), GEKKO_SELF_SAMPLE 50 us with
stacks, tools/sample_subtree.py (exclusive inside the subtree: no double counting).

| | Westopolis | Final Haunt |
|---|---|---|
| fn_80416EF4 (motion player update) inclusive | **11.69 %** of the game thread | **12.88 %** |
| child 0x80419094 (hierarchy evaluation) | 5.92 % | 6.72 % |
| child 0x804198F0 (track sampling) | 3.61 % | 4.05 % |
| child 0x80419A9C (time advance) | 0.92 % | 0.71 % |
| children 0x80418618 / 0x8041A33C / 0x8041AFAC / 0x8041BB8C | 0.53 / 0.45 / 0.11 / 0.06 % | 0.62 / 0.51 / 0.10 / 0.04 % |
| net leaf 0x8041669C (Hermite interpolation) | 1.38 % | 1.78 % |
| net leaf 0x804162AC (track sampler) | 0.87 % | 0.94 % |
| net 0x8000FF04 (sqrt, also from particles) / fp_compare | 0.42 / 0.47 % | 0.47 / 0.36 % |

The largest single subsystem of the game thread (the particle engine is ~13 % in two subtrees).

## 2. Structure (P from the disassembly unless marked)

- 0x80416EF4(motion): enter the motion lock ([r13+0x5F50], 0x8040D254), clear +0x14, per channel (0x130-byte channel
  records at motion+0x10, index table motion+0x28) 0x8041BB8C, then 0x8041AFAC, 0x8041A33C, 0x80419A9C (time
  advance), 0x804198F0 (track sampling), 0x80419094 (hierarchy), 0x80418618, unlock 0x8040D294 (also
  lod_streaming.md). 31 call sites.
- Track sampling 0x804198F0 -> typed samplers 0x80421358 / 0x804213FC / 0x8042148C -> track sampler 0x804162AC
  (section 3) -> interpolator through a global function pointer.
- Hierarchy 0x80419094 -> recursive node walk 0x804196F0 (-> 0x80420B9C, 0x8041F174 per node; 0x8041EC24 ->
  0x8042167C -> 0x80421774 / 0x8042193C / 0x80421BB0 with paired-single loads; MSL sin / cos via rem_pio2; sqrt
  0x8000FEE4 via length 0x8040DFA0).
- Time advance 0x80419A9C -> 0x8041BF20 -> recursive 0x8041BF6C -> 0x8041C01C -> 0x80414B80 -> 0x80415300 (U: role).
- Channel value setter 0x80417E18(motion, channel r4, f1): channel = motion+0x10 + index(motion+0x28)[r4] x 0x130;
  stores f1 at channel+0x108; flags channel+0x94 |= 1 | 0x40. The next function (0x80417E70) stores +0x10C the same
  way (U: what +0x108 / +0x10C mean - time vs speed).

## 3. The track sampler 0x804162AC (P)

Global sampling context 0x805E2380: +0xC the time being sampled, +0x14 the interpolator (function pointer; the
Hermite variants 0x8041669C / 0x80416728 are reached only through it - no direct call sites).

Track state (r3): +0 key index (s16), +2 key count (s16), +4 flags (bit 0 finished / held, bit 3 flat segment),
+8 pointer into the compressed key stream, +0x10 cached value, +0x20 key A, +0x30 key B, each {time, in-tangent,
out-tangent, value} (4 floats). epsilon = [r2+0x16E0].

1. bit 0 set: return the cached value.
2. t < A.time + eps: return A.value.
3. t < B.time - eps: flat flag (bit 3) -> cached value; else interpolate(A, B) through the context's pointer.
4. t < B.time + eps: return B.value.
5. else advance while index < count: A = B; decode the next key from the stream - header u16: time = (h & 0x3FFF) as
   an integer converted by the 0x4330 magic-double trick (minus [r2+0x16D8]); bit 0x8000 = the in-tangent repeats the
   out-tangent; then u16 half-precision values decoded by bit placement (`rlwinm 13` + 0x38000000 bias + sign
   bit; 0 kept as 0; no denormal / infinity handling), for the tangents and the value. A segment with both tangents 0
   and equal values sets the flat flag and caches the value.
6. past the last key: out-tangent 0 -> hold (bit 0, cached value); else linear extrapolation value + tangent x
   (t - B.time) (fmadds).

## 4. The interpolator 0x8041669C (P)

Cubic Hermite: s = (t - A.time) x (1 / (B.time - A.time)) with one fdivs, basis from the constants at r2+0x16E8..
0x16F4 = {1, 2, 3, 6} (0x805FBE68), combined with fmadds from A.value (+0xC), B.value, A.out-tangent (+8) and
B.in-tangent (+4). 0x80416728: the same with the basis rearranged (a second form - U which data selects it).
0x804167AC: copy a key (constant / step).

## 5. Runtime census: repeated updates are distinct evaluations (P)

GEKKO_CALL_CENSUS (system.cpp, non-shipping; every call of the listed functions logged and DECLINED - the guest runs
unchanged; state report byte-identical to the baseline with and without it):

| | Westopolis | Final Haunt |
|---|---|---|
| 0x80416EF4 calls / frames | 57,883 / 5,267 (3 - 26 per frame, median 10) | 93,876 / 5,243 (median 19) |
| same motion object updated again in the same frame | 8,114 calls in 3,391 frames | 12,570 in 4,106 |
| repeats from call site 0x80063F64 (LR 0x80063F68) | 8,110 | 12,561 |
| callers (LR) | 0x80063F68 16,260, 0x801A4D20 13,659, 0x800B1400 10,092, 0x8016351C 8,604, 0x800B4DE4 5,268 | 0x8016351C 36,318, 0x80063F68 28,099, ... |

0x80063ED4(container, f1, f2) loops over its entries: RwFrameGetLTM (0x804882E8) -> stack matrix (0x80016414) ->
0x80418328(motion, matrix) -> 0x80417E18(motion, 0, f2) -> 0x80416EF4. It is called from 0x80064508 (f1 = 1/60) and
0x800646F8 (f1 = 0). Every same-frame repeat (8,092 of 8,092 checked) sets a DIFFERENT channel value (e.g. 0.0 then
1.0): two distinct evaluations, not redundant work. Hypothesis "skip the repeated update" is disproven.

## 6. Native reconstruction notes

- The sampler + Hermite pair is closed and fully understood (sections 3-4); a native owner must reproduce the track
  state writes, the cached value, the register file and FPSCR (through the ppc_ops fps_* helpers, as the CharaColli
  owners do), and call a non-Hermite interpolator through call_guest. A leaf transliteration alone is not expected to
  be faster (RwMatrixMultiply precedent, CPU_FINAL_HAUNT.md); the gain has to come from structure - e.g. sampling all
  tracks of a channel in one native pass (no 3 x n guest dispatches), decoding keys once.
- Open: the meaning of channel +0x108 / +0x10C, which tracks use 0x80416728, the hierarchy walk's per-node maths
  (0x80420B9C, 0x8041F174, 0x8041EC24 chain), the time-advance recursion.
