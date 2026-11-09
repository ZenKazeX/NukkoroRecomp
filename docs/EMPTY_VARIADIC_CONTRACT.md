# Empty variadic routines

The original image contains three leaf routines that save arguments into a
private stack frame and never consume them. Their reconstructed intent is an
empty operation, with one routine returning zero. They perform no formatting,
logging, allocation, math, I/O or timing operation. The native implementation
therefore has no argument-save storage or game-memory accesses. The disposable
caller boundary still reads and writes its host register-state structure.

| Original entry | Original bytes | Remaining caller ABI effect | Direct calls |
| --- | ---: | --- | ---: |
| 8000D374 | 80 | Preserve registers and return | 309 |
| 8001C574 | 84 | Set r3 to zero and return | 332 |
| 8047EC78 | 104 | r11 = incoming SP + 8; r0 = incoming SP - 120; r12 = 0x01000000; return | 300 |

Return clears the low two LR bits, as the original return instruction does.
All other GPRs, both lanes of every FPR, CR, XER, CTR, LR, FPSCR and game time
are preserved. The remaining register/return boundary is explicitly disposable
R2 scaffolding; it disappears with the translated callers. There is no new
runtime handler, fake allocation or permanent native API for an empty operation.

The recompiler validates each complete instruction template, exact extent,
entry set, ownership and overlapping contexts before changing generated output.
It removes the old body and interior dispatch range and emits one shared
semantic boundary per routine, rather than a record for each of the 941 calls.
The original DOL remains the reference/resource input; removing generated
machine-code bodies does not mean removing the embedded original image.

`tools/ppcrecomp/make_empty_variadic_fixture.py` pins the complete DOL, each
body, all encoded incoming call edges and their counts. It rejects aligned
data references, incoming interior branches and non-call branches. The third
routine leaves a pointer into its discarded private frame in r0. The maintained
direct-flow tracer checks all 507 declared contexts of its 300 call sites:
there are no reads, unknown barriers or return escapes before overwrite. This
is a direct-flow proof, not certification of arbitrary computed entry points.
The post-link gate checks that old callable symbols and generated interiors
are absent and that all three semantic entries are linked.

The isolated C++ oracle executes the original instructions without generated
code or lowered-site records. Across 12,288 comparisons it exercises both CR1
argument-save paths, four FPSCR settings, varied stack and return addresses,
all GPRs, both FPR lanes, NaNs, infinities, subnormals and randomized bit patterns.
It attributes every original write to the 112- or 128-byte private frame.
The original writes 36/100 bytes or 48/112 bytes per invocation; the replacement
accesses no data memory. All original leaf instructions are erased before
running the generated replacement. Six dispatcher cases and three verification
cuts check return ownership and cut-before-side-effects behavior.

Another 288 paired runs execute three original callers, reaching the selected
leaves 96, 96 and 66 times. Every register, object and global result agrees.
Access tracing rejects a subsequent read of a removed frame byte before
overwrite; every stack difference is attributed to such unread private bytes.
Two constructor fixtures supply one explicitly checked 20-byte allocation to
both modes. They do not validate the original allocator. The third caller and
its error-state publisher execute entirely from original instructions.

The first seven selected CTests passed after the implementation: the empty-leaf oracle,
generator output structure, type-query caller integration, scalar math,
dispatcher execution and both wait-point checks. Generator regressions include
11 malformed/stale/overlapping/ownership cases that must leave existing output
unchanged. Evidence is in `.build/validation/frame-work/empty-variadic-*.log`.

The first isolated fixture incorrectly supplied only three sparse function
rows; the existing generator widens extents to the next row and correctly
refused it. The fixture now uses the original full function table and an explicit
selection. The first oracle run also treated instruction fetches as data reads;
it now counts only exact current-PC four-byte fetches and checks that their
count equals executed instructions. Neither correction weakens data-access
checks. A restricted MSBuild invocation stalled before compilation; it was
cancelled and its workers verified gone before the successful normal-permission
build. Initial failed-gate evidence remains recorded.

The first full release passed static removal gates but grew from 105,581,568
bytes to 105,598,976 bytes: +17,408, including +9,324 virtual unwind-table bytes.
It is a failed size gate, not an optimization result. Returning a continuation
flag after ordinary lowering calls prevented their former tail calls. The
corrected design stores the known policy in the existing 16-byte dispatch
record, replacing an instruction word that dispatch never reads. The original
words remain in a separate nonshipping validation array; the shipping post-link
gate rejects that diagnostic array if it leaks into the executable. Lowerings again have
void signatures. Eleven selected gates pass with this correction, including the full generated
math/controller caller tests and conservative liveness roots. The corrected
release is 105,577,472 bytes (4,096 smaller than the control), SHA-256
`c350a565e2caaca3d8a6746e97a1120aef8776f84a25886fbe55e4fcce8aad18`.
Static post-link gates pass; generated code, original bodies, incoming edges
and the absence of validation words from shipping are checked.

Production generation is enabled in the test candidate. The full recorded
Westopolis route (315.391183 game seconds) passes with byte-identical final
state reports, PCM, checkpoints and captures. A separate restored 30.013317-second
run with live audio, native 2560x1440 rendering and the D3D12 debug layer requested
also matches state, PCM and capture bytes. It retains 1,800 fixed pacing boundaries,
zero skips, render failures or audio underruns, and no interpreter instructions.
The fresh candidate image was visually inspected. These are correctness checks;
the capture stall means their timings are excluded from performance results.

The first three alternating performance pairs render the same 1,797 frames each.
Their CPU ranges overlap; the median change alone does not establish a speedup.
The candidate initially reports about 16 MiB more working set. Repeating both
binaries at the same absolute executable pathname removes that gap in separate
memory diagnostics. The earlier comparison used control.exe versus
ShadowTheHedgehog.exe at different paths; it does not isolate a code-caused RAM
increase. No particular driver or OS component has been proved responsible.
The maintained benchmark now stages verified copies at one pathname, retains
source/execution hashes and removes each staged copy after its process exits.
Three new tests cover stable identity, changed input, exception cleanup and
refusal to overwrite existing files or the source. The benchmark CTest passes,
bringing the selected-gate total to twelve. Matched performance results are
recorded separately; all earlier samples remain visible.

Tests assume valid native-call stack ownership; they do not preserve obsolete
stack faults or certify arbitrary aliases into expired frames. The 800 FPS at
native 1440p goal remains unachieved. The deployed executable is unchanged.

Matched-path CPU medians are 8.312500/8.671875 seconds (+4.32%), with every
candidate pair slower. Identical-control calibration medians are
8.109375/9.031250 seconds (+11.37%) under the same path and workload. Every
run renders 1,797 frames. These overlapping and host-sensitive samples do not
establish a causal CPU gain or regression. The observed slower candidate
samples remain a performance concern; calibration is not subtracted and no
speed/resource improvement is accepted. Peak resident-memory ranges overlap
under matched paths. Further CPU attribution remains part of the active goal.
