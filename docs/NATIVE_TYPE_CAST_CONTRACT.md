# Native type-query migration

Status: value algorithm validated; metadata ownership, caller lowering and
production retirement remain in progress. The release executable still runs
the original translated cast. No size, CPU or FPS gain is claimed yet.

## Selection evidence

The current 105,581,568-byte release (SHA-256
`8608faaa2874b9fd7817715bc1e8397693c68a83dbbbd6ea1fc708aaf6213d6a`)
was sampled on the first 90.016033 game seconds of the recorded Westopolis
route. Of 5,241 game-thread instruction-pointer samples, 215 land inside
`0x803A1AFC`, the highest sampled remaining translated body. The existing
reverb adapter has 216 samples; its previously validated native effect stage
still needs voice/resource ownership integration. Sampling suspends threads
and measures wall-time locations, so these counts are not CPU percentages or
benchmark results. Evidence: `.build/validation/frame-work/selection.json`.

The selected 612-byte original body is pinned by SHA-256
`d1883bb7ab9577c5c68ae53b3e2be3c4c89e7adb64ed5d042ef017fa61ed6569`.
It performs class-name string comparisons repeatedly while looking through
the dynamic class's base-subobject table. An explicit metadata owner can
intern equivalent names once and supply immutable native identities and
host-sized offsets to a small allocation-free query.

Modern RTTI also associates a dynamic type and an offset to the complete
object with virtual dispatch metadata. That separation informs the native
design; the actual lookup rules here come from this game's instructions,
not another compiler's ABI. See the primary [C++ ABI RTTI specification](https://itanium-cxx-abi.github.io/cxx-abi/abi.html#rtti).

## Recovered behavior

The original arguments are object pointer, offset of its virtual-table pointer,
target type descriptor, source type descriptor and a reference-failure flag.
The routine reads the dynamic descriptor and signed complete-object adjustment
from the selected virtual table. Null objects return null immediately. A
non-null object with no dynamic descriptor fails, even for a void query.

With a dynamic descriptor, a null target requests the complete object. Equal
dynamic and target class-name contents also select the complete object. The
original compares contents, so separate descriptors or name storage with the
same bytes must remain equivalent. Native identities must be interned without
hash-collision ambiguity.

The ordered base list contains ordinary target/offset entries and source
restricted groups. Ordinary matches return the listed complete-object offset.
A restricted group is considered only when its target matches and its offset
equals the source object's offset. Its source list must then match both the
source identity and offset. Failure in that matching group is final; searching
a later entry would change behavior. An empty restricted list fails. A group
at a different offset can be skipped. The precise source-group behavior is
proved; its encoding is not used as the native architecture.

On failed lookup, a nonzero low 16 bits of the original reference flag enters
the existing `bad_cast` exception path. The oracle verifies its exception type,
payload and destructor arguments at `0x803A2660`. The separate failure-entry
test below executes the original unwinder and handler; production caller
lowering remains unconnected. Null input bypasses that failure path.

`object::find_subobject` operates only on `ClassInfo`, stable `TypeId` values,
native pointers and `ptrdiff_t` offsets. It never reads objects, translates
addresses, performs string comparisons, allocates or throws. Its result is an
offset from the complete object. The owning caller handles null objects,
pointer adjustment and reference-cast failure. The API supports offsets beyond
32 bits. Source descriptor arrays must outlive queries.

## Validation actually completed

`native_type_cast_tests` executes the pinned original instructions as the
independent oracle. It compares 15,603 synthetic boundary and randomized cases:
5,010 successful returns, 1,253 null/failure returns and 9,340 exception
decisions. Cases include separately allocated equal names, non-ASCII name
bytes, missing type metadata, null and void queries, duplicate bases, group
ordering, empty groups, source offsets and all low-16-bit flag behaviors.
Metadata/object writes, stack ownership, nonvolatile registers, floating
status and game time are checked where the original returns normally.

Another 11,880 queries execute against the actual DOL tables for 44 shipped
descriptors, compared with native metadata read from those tables. The seeds
are 42 target/source descriptors observed in the first 256 development-build
casts, plus PlayerShadow and EffectSystemPJS from verified virtual tables.
The oracle executes 4,894,403 original instructions across both suites. A
separate native-only check uses a subobject offset of 2^40. The initial contract and lift-purity checks passed. The expanded validation
below supersedes that initial coverage count. Historical missing `B45.mem1`
fixtures remain unresolved and the full suite was not rerun.

The supplemental development observation counts 117,146 calls in 22 game
seconds, retaining only the first 256 argument rows. It uses the nonlifted
development renderer and no replay tape, so its rate is not substituted for
the release profile. All retained flags are zero, but this is not a whole-game
proof: the static census finds 268 external direct calls and 13 nearby explicit
reference-cast requests. Two callers set a zero flag farther back than the
initial eight-instruction window. The raw census also finds a data word at
`0x800075BC` naming the body, apparently in the exception index; ownership and
retirement of that record need verification. No external conditional branch
enters this body; computed entry edges still require an audit. The shipping build correctly rejected the first
attempt to enable development probes; that failed invocation remains recorded.

## Metadata recovery and expanded oracle

`tools/lift/type_catalog.py` now recovers the ordered class descriptions, interns
exact name bytes without hash ambiguity, and shares only semantically identical
class views. Equal names with different ordered base lists remain distinct
views. Explicit malformed roots fail; heuristic virtual-table candidates with
invalid descendants are reported and excluded. Bounded parsing rejects invalid
spans, negative source counts and excessive graph sizes. Virtual-table discovery
requires a file-backed first method, so it is a candidate census, not a
completeness proof.

The tool reuses the existing known-bits CFG analyzer with its EABI call-clobber
and SDA assumptions. Overlapping declarations are analyzed in each context;
unknown arguments from an independently declared entry remain unknown. Of 268
literal calls, 267 have agreeing argument constants. The second declared entry
at `0x80435408` leaves the target/source of `0x80435428` unknown, although the
containing function supplies constants. Six unresolved indirect branches are
also retained in the report. This improves the earlier linear heuristic but
does not certify every possible entry edge or mutable pointer.

The initial call-root and virtual-table census contains 4,112 descriptors,
1,661 exact name identities and 2,197 candidate virtual tables. All equal-name
class views in this census also have equal ordered base semantics; they share
1,661 native descriptions containing 3,674 base entries. These observed tables
contain no restricted-source groups; synthetic cases still test those groups.
The generated oracle fixture contains genuine immutable host-pointer arrays.
Its descriptor-address aliases are test inputs, not a production binding API.

The straightforward x64 array model costs 386,808 bytes before sharing and
186,824 bytes after sharing. These numbers cover only ClassInfo, BaseSubobject
and SourceSubobject arrays, whose 24/40/16-byte layouts are checked by the C++
test. They exclude binding tables, PE alignment and all other executable data.
They are neither an executable reduction nor a measured resource improvement.
The production layout still needs to justify its cost against the translated
body it removes.

The independent instruction oracle now executes 630,830 generated-catalog
queries: 197,915 through the general native class views and 432,915 through the
compiled queries described below. These cover all 4,123 recovered descriptors
and 2,210 original virtual tables, including the store-discovered headers below.
The generated-catalog cases reach the original exception entry 225,324 times. The test checks metadata/object
writes, normal-return register/stack ownership, floating status and time.
Combined with the earlier synthetic and 44-seed suites, 658,313 comparisons
execute 150,109,251 original instructions. Native-only checks cover full-width
identities, signed compact offsets and a general subobject offset of 2^40.

Seven selected CTests pass: the native lookup oracle, failure-entry/unwind
oracle, 41 metadata-tool cases, 25 shared CFG classifier cases, native liveness
roots, lift purity and scaffold inventory. There are 110 registered CTests;
the full suite was not rerun. Current evidence:
`.build/validation/frame-work/metadata-uses-test-details.log`.

The tool also fingerprints every accepted metadata span and can compare all
of them, plus candidate virtual-table headers, with a saved state. A changed or
missing byte is reported as a failed observation. Passing snapshots establish
only point-in-time stability, not absence of transient writes, unobserved paths,
dynamically constructed metadata or changes to an object's virtual-table pointer.

The unchanged eighteenth release replayed the complete 315.391183-second
recording and a 30.013317-second continuation from its 120.012667-second
checkpoint. All 5,719 metadata spans (209,017 bytes) and 2,197 virtual-table
headers match the DOL at each of the three captured points. The checkpoint
matches the preceding validation SHA-256
`7819d250d124385c0f9219dad93933e2c027b5c1c27f1384c4853b3354993145`.
These runs were headless with isolated persistence; they establish neither
rendered FPS nor a candidate speedup. The first restore invocation incorrectly
combined `--boot-stage` with `--load-state` and was rejected before loading;
the corrected invocation and that rejection are both recorded in
`.build/validation/frame-work/metadata-observation.json`. Disposable states and
save copies are removed after fingerprinting; the original input tape remains.

To regenerate the analysis and the test-only native arrays:

```powershell
python -B tools/lift/type_catalog.py --dol ../game/sys/main.dol --descriptor 0x805E5AF0 --descriptor 0x805E9D98 --functions extract/functions.csv --jump-tables extract/jump-tables.txt --cast-entry 0x803A1AFC --discover-vtables --discover-stores --metadata-uses-out .build/tests/Testing/type-catalog/metadata-uses.json --out .build/tests/Testing/type-catalog/catalog.json --oracle-header .build/tests/Testing/type-catalog/native_type_catalog.inc
```

Add `--snapshot <path>` to include a point-in-time metadata audit. Build the
`native_type_cast_tests` target to generate its fixture automatically in the
build directory. No generated catalog belongs in the maintained source tree.

## Compiled queries and exception-boundary contract

The statically recovered caller contexts request 105 distinct target identities.
Within the recovered metadata domain there are only 315 successful dynamic-class
and target combinations. `compile_queries` preserves the original first-match
rule and compiles these results into sorted native identity/offset records.
It refuses source-restricted classes, differing class views with one identity,
missing targets, or values outside the compact storage bounds. This is a
specialization for the stated metadata domain, not permission to discard
unknown types or caller states.

`find_compiled_subobject` searches four-byte records without allocation, string
comparison or object-memory access. The stored identity is 16 bits and the stored
offset is signed 16 bits, after checked bounds. The input identity remains
32 bits and is never narrowed; the result offset is host-sized. Thus an input
identity of 65,537 cannot incorrectly match identity 1. General source-sensitive
and wide-offset class queries retain their separate native API.

The 315 match records occupy 1,260 bytes. This excludes plan headers, translated
object bindings, code and PE alignment; it is not an executable size reduction.
The generator's oracle fixture also retains the general class arrays for
independent comparison. Every recovered descriptor is tested against all 105
compiled targets using the original cast instructions. The query manifest is
`.build/validation/frame-work/compiled-queries.json`.

The initially ambiguous entry `0x80435408` follows `bctrl` at `0x80435404`. Its
containing function starts at `0x8043531C`, owns the same exception-index range,
and initializes callee-saved r19 to `0x8056BFB8`. The active generated code already
routes the smaller entry into `fn_8043531C_at`. The second cast therefore receives
target `0x8056BFE8` and source `0x8056BFD4` under that caller/ABI contract. No
literal branch, data pointer or recognized address-construction pair names the
continuation. These scans are bounded evidence, not a proof against arbitrary
computed entry or corrupted restored registers. The conservative CFG report
retains the unknown independently declared context; stronger owner evidence is
recorded separately in `continuation-owner.json`.

`native_type_unwind_tests` pins the full original DOL and compares 72 pairs of
original casts and a reconstructed failure-entry boundary. It covers missing
dynamic metadata, ordinary failure, empty restricted groups and mismatched
restricted sources; positive/negative low-16-bit flags, CR summary-overflow
state, and varied nonvolatile integer/paired-float values. The original
exception registration, search, frame restoration, handler and rethrow execute
as instructions. The catch owner at `0x8042711C` runs its actual prologue and
cleanup at `0x80427330`; the fixture injects the failed cast at its allocation
call. A fixture virtual destructor records a side effect. This is a controlled
exception fixture, not a claim that gameplay itself took this path.

The recovered boundary retains the cast's 32-byte frame, saved caller link/r31,
four-byte exception payload, throw arguments and unwind continuation. The first
catch receives identical nonvolatile state, exception records, stack and flags.
Cleanup executes once and rethrows the same live payload. Both versions follow
identical original instruction traces from the throw entry through unhandled
exception dispatch. All 153 original cast words are zeroed in recovered cases;
only the exception-index range data remains. A deliberately wrong return PC is
rejected by the comparison. Recovered fixtures now erase all 5,733 cataloged
class-metadata spans (209,589 bytes) and replace all 2,210 cataloged virtual-table
descriptor fields with native type IDs. The original exception-index records
and encoded exception type strings remain. Each recovered run checks that the
erased spans and native header fields remain unchanged.

Five additional original/recovered pairs execute the actual typed matcher at
`0x803A17B8`: `std::bad_cast`, `std::exception`, an unrelated class, a pointer
encoding and a catch-all. They produce the expected match results 1, 1, 0, 0,
1 with unchanged adjustment output and game time. The complete test executes
158,539 original instructions. It stops at termination dispatch and does not
invoke the process-abort handler. This proves independence from class metadata
only for the exercised exception paths; it does not permit deleting shared
gameplay strings from the production image.

The test now invokes the generated failure-entry component shared with call
preparation, rather than carrying a separate reconstruction. It still does not
exercise recompiler caller dispatch. It does not make production native or
prove every caller's scratch-register liveness, dynamic-type domain, metadata
lifetime or arbitrary exception-handler chain. The production cast body remains
generated and linked.

## Metadata-store ownership audit

`tools/lift/type_metadata_uses.py` analyzes 39,355 declared entry extents,
retaining overlapping contexts. It examines 183,974 distinct store sites and
finds 2,210 constant stored metadata headers. None of those stored pointer
values requires the entry SDA assumptions. Thirteen headers were absent from
the first-method-in-text census: their first method is null. They add nine
roots and eleven descriptor aliases, including camera, collision and heap base
classes. The aliases share existing class views; the 1,661 native classes,
3,674 shared bases and 315 compact matches do not grow.

The effect constructor at `0x801E29B4` stores `0x8053E8E8` and then
`0x8053E8C0` into the same object's virtual-table field. Both tables were
previously missed. This directly demonstrates a constructor phase change;
keeping an object's first observed type forever would be invalid.

The first audit produced two apparent writes into class-name data because the
shared GX analyzer pins r2/r13 after instructions. The metadata audit now opts
out of that assumption: explicit register changes remain effective, and each
relevant store is also analyzed with unknown entry SDA registers. Original
instructions at `0x8039F570` construct r2 = `0x805A4FB8`; its store at
`0x8039F674` is consequently outside the metadata. One independently declared
entry at `0x8039F67C` still produces an overlap at `0x8039F6BC` only if r2 has
the normal SDA value. It is retained with `requires_entry_sda: true`, not
classified as a proven mutation. The GX classifier's default is unchanged;
its 25 existing tests pass.

The report explicitly retains 176,108 sites with unknown destinations, 188 raw
store words not reached in declared contexts, and 499 unresolved-control
contexts. Unknown destinations can alias metadata; bulk/native/host writes and
snapshot restoration are not covered by this instruction census. Immutability
and a complete dynamic-type domain are still unproven. The next ownership
step must recover the remaining RTTI consumers and initialization/restoration
contract before replacing descriptor storage with native class identities.

CMake now expands the oracle fixture from these stored headers. The expanded
catalog has 5,733 metadata spans covering 209,589 bytes. The three earlier
snapshot observations apply to the original 5,719-span catalog only; the new
headers have not been claimed as observed there. Current full evidence is
`.build/tests/Testing/type-catalog/metadata-uses.json` and `catalog.json`, pinned
by `.build/validation/frame-work/metadata-ownership.json`. No production
metadata is changed by generating these fixtures.

## Metadata consumers and shared gameplay names

The full consumer pass uses the expanded catalog and the shared explicit-load
decoder. It retains 215,231 analyzed load sites, including 185,817 unknown
addresses. No known literal load points into the catalog inputs; this does not
certify loaded/computed pointers or unsupported forms. It also records 603
known metadata values in call argument registers: 540 at the original cast and
63 elsewhere. These are candidate values, with overlapping entry contexts and
possibly stale registers retained; argument use requires inspecting the callee.

That inspection proves real gameplay dependencies. At 32 candidate calls to
`0x8015CF6C`, r6 supplies a class name. The callee saves it and forwards it to
`0x801DCD24`, which calls the bounded string copy at `0x803ADB0C` with count 15
and destination object+`0x3C`, then clears the byte at object+`0x4C`. Names include
`CircusPole`, `ElecCircuit` and `BreakRoad`. Separately, `CircusPole` is passed
through `0x8004F8E0` to the actual byte-scanning length routine at `0x803ADC08`;
the string and its end pointer then feed `0x80041EE0`. These bytes are shared
gameplay inputs, not disposable cast-only names.

The aligned data scan assigns known descriptor/base/header fields their roles
and retains 176 external references: 162 point to class names and 14 to class
descriptors. Several name-bearing records also contain text addresses, but
their structure alone is not a factory or ownership proof. One external base
field at `0x8055B8DC` belongs to the structural `Player::PlayerObject` descriptor
at `0x805EDD38`, outside the current catalog. Other aliases of that class are
already represented. No stored-header witness establishes this additional
alias as live; neither it nor the other external fields is silently discarded.

`System::load_dol` still copies the original image sections after clearing its
image, legacy heap and BSS. Save-state loading restores memory regions and can
remap their storage. Native metadata ownership and any temporary import step
must account for both paths without retaining borrowed image pointers across
restoration. No native production owner is implemented yet.

Seven selected CTests pass: native cast, exception unwind, metadata tools,
existing GX classifier, liveness roots, purity and scaffolding inventory.
The tool tests now contain 45 cases; the CFG classifier retains 25. The full
suite and gameplay were not rerun for these test/tool changes. The full audit
is `.build/validation/frame-work/metadata-consumers.json`; its input hashes,
confirmed instruction paths and limitations are recorded in
`metadata-consumer-contract.json`. The latest selected-test log is
`type-consumer-test-details.log`. Exception-fixture erasure is deliberately
stronger than the safe production deletion scope: shared names must survive.

## Native query data owner

The generator now emits `native_type_queries.h` separately from the oracle's
legacy aliases. Its immutable native arrays contain only 315 identity/offset
matches and 105 query ranges. Each range is two checked 16-bit integers; matches and ranges together occupy
1,680 bytes in the compiled x64 test. The lookup still accepts a full-width
TypeId and returns a host-sized offset.
Callers select a typed query enum. An out-of-range query returns no match.
The native owner contains no original addresses or descriptor pointers. The
oracle maps original target descriptors to native queries; the disposable caller
boundary separately contains the image bindings.

Generating the same class semantics at different image addresses produces
identical native owner source. Generation independently recompiles the queries
and rejects stale match semantics. Provenance stays in the catalog and evidence
rather than becoming runtime data. The original-code oracle now calls this
owner for all 432,915 compiled-query comparisons; it no longer carries its own
copy of the compact match arrays.

Both C++ tests share the `shadow_type_catalog` build dependency. Before this
change, building both targets executed the full metadata scan twice. The
corrected build executes it once. This removes redundant build work; it is
not a game-performance result.

Seven selected CTests pass in 22.86 seconds, including all 658,313 original-code
lookup comparisons, 72 unwind pairs, five typed matcher pairs, 47 metadata-tool
cases and 25 CFG cases. This is validation time, not gameplay performance.
Current owner evidence is `.build/validation/frame-work/native-query-owner.json`
and `native-type-owner-test-details.log`. The prior consumer audit is retained
with its original tool hashes; it was not rerun for this emitter-only change.

This completes ownership of the immutable compact query data only. It does
not identify native dynamic types on live gameplay objects, import/restoration
ownership, or emitted caller/failure boundaries. The release still contains
the original cast. Header/table cost must be combined with those remaining
parts and measured in the actual executable before claiming a size reduction.

## Generated call preparation and exception entry

`call_boundary_header` emits `native_type_boundary.h` for the pinned original
image. It is explicitly disposable scaffolding, separate from the native query
owner. Current object/vtable fields are read for every call; no object type or
borrowed host pointer is cached. The first layout stored all 4,123 bindings as
16-bit descriptor suffixes and native identities (16,492 bytes). The compact
layout below retains exact membership for all descriptors with a bitmap and
stores only the 490 aliases that can match a compiled query (3,453 total bytes).
The separate native query owner remains 1,680 bytes. This is disposable image
binding data, not native object ownership or a production size saving.

The preparation component implements null inputs, pointer-cast failures,
complete-object queries, and reference failures with the original low-16-bit
flag rule. Unknown bindings, invalid queries and changed target contracts stop
explicitly. The reference-failure component constructs the tested frame and
publishes the original throw entry/unwind continuation. The unwind test now
uses that generated component; its separate reconstructed implementation was
removed. Shared gameplay names and all original image metadata remain intact.

The oracle executes 435,125 compiled-query/preparation comparisons: all 4,123
descriptors across 105 queries plus one query through each of the 2,210 actual
original virtual tables. It varies vptr offsets between 0, 0x38 and 0xA8,
checks return/failure policy, frame contents, nonvolatile state, time and object
memory, and verifies that journal rollback restores generated exception writes.
The complete lookup suite now totals 660,523 comparisons and 150,519,072 original
instructions. The existing 72 unwind pairs and five typed matcher pairs pass;
the latter test executes 158,539 original instructions. Seven selected CTests
pass in 23.51 seconds, including 48 metadata-tool cases and 25 CFG cases.

The first selected run passed six checks and failed the liveness gate: the
preparation helper had been placed in the recompiler-dispatch namespace while
the oracle declared original instruction execution as reference-only. The
helper now has the separate `gekko::type_query_boundary` namespace. It never
dispatches game instructions, and both test projects still link the reference
CPU without `shadow_recomp`. The liveness guard and roots were unchanged. The
rejected result is retained in `type-boundary-rejected.log`; the passing run is
`type-boundary-test-details.log`. Full evidence is `type-call-boundary.json` in
`.build/validation/frame-work/`.

This verifies generated preparation, not actual recompiler caller dispatch.
The generic emitter used at that milestone resumes after a lowered call and
cannot serve exception transfers unchanged. The dedicated transfer handling
below is now implemented; remaining work includes complete caller contracts,
metadata lifetime/restoration and production retirement. The static binding assumption must be established before enabling
it in gameplay. The original cast remains linked; no release, replay, FPS or
whole-process resource result is attributed to this component.

## Generated callers and continuation ownership

The generator now accepts `type_query <query ordinal hex> <target descriptor hex>`
for a pinned direct call of the original cast. The emitted call uses the native
query owner through the disposable preparation component. Direct callers honor
normal return, a stopped fault, exception entry, published transfers and unexpected
return addresses. They do not overwrite the throw continuation with site+4.
Pruning follows the remaining throw and exception-destructor dependencies instead
of retaining the old cast edge. Production site lists are not changed.

The dispatcher previously advanced every lowered call to site+4. Each record now
stores `LiftSiteContinuation::Sequential` or `Prepared`; existing kinds retain
sequential completion, while a type query owns its prepared PC. The policy
replaces an unused runtime instruction word: `sizeof(LiftSiteRecord)` remains
16 on x64. Original words remain in a separate nonshipping validation array.
The first policy-return design prevented tail calls in existing lowerings and
increased the full release size; it was replaced after that measured failure.
Lowerings retain a void signature and can tail-call their native operation.
The native value algorithm still has no CPU, image-address or exception-ABI dependency.

The new fixture compiles the unchanged 68-byte caller at `0x80077494`, whose cast
at `0x800774B0` conditionally sets a flag on the returned object. All 4,123 dynamic
descriptors run through both normal entry and a real cold interior entry, compared
with the complete original caller. The 8,246 comparisons check object bytes,
return/stack/link/control state, nonvolatile GPRs, FPSCR and game time. Null cases
also pass. Every generated run erases the original cast's 612 instruction bytes;
the fixture's generated table contains no old cast entry. The cold path executes
only the original stack-allocation instruction before entering the generated
interior body. Its full original-instruction count is 394,334.

Exception probes check exactly one entry/continuation and retained stack/link
values for stops, unexpected returns and published transfers. They are transfer
mechanics probes, not reconstructed exception-runtime substitutes. The separate
72-pair original catch/cleanup/rethrow oracle remains the exception-semantic
proof. Dispatcher entry, rejected target contracts, pre-operation verification
cuts and legacy sequential continuation are also tested. End-to-end generated
exception-runtime execution and all remaining callers still need validation.

The first fixture build omitted required `0x` prefixes and was rejected. The
first 12-check run passed 11 and rejected the fixture's incorrect expectation of
one dispatch entry: the generator correctly also emits a post-call continuation
thunk. The test now requires exactly those two entries and one body range. The
corrected caller test passed. The full generated library and existing controller,
matrix and scalar call tests then rebuilt; all 15 selected checks pass in 37.77
seconds. The pruning gate retains throw/destructor code while removing the old
cast in its isolated fixture. The complete game suite and gameplay were not rerun.
The stalled sandbox MSBuild invocation was confirmed live, cancelled by exact
process identity, and rerun with normal worker permissions; no duplicate build
was launched. Rejected build/test evidence is retained under
`.build/validation/frame-work/`.

The original cast remains in the release, with live-object metadata lifetime and
restoration still unproved. This work does not claim executable savings, gameplay
speedup, deployment, or completion of native object ownership.

## Compact descriptor-domain bindings

Only 203 native dynamic classes (490 original descriptor aliases) can match any
of the 105 compiled queries. All other known descriptors must still be accepted
as valid inputs and take the ordinary cast-failure path. Unknown descriptors
must continue to stop explicitly. The generator now represents these separately:
1,960 bytes of query-relevant identity records and 1,493 bytes of exact domain
bits. Two exact high-prefix branches, alignment checks, trimmed-range checks and
bit membership precede lookup. Valid classes with no compiled match use a
full-width sentinel outside the validated 16-bit identity range. Invalid query
indices are rejected before using that sentinel. Target, null, void, pointer
failure and reference-failure behavior is unchanged.

The representation-independent C++ test enumerates every byte position in both
represented high-prefix regions and a known suffix in every other prefix:
196,606 cases, covering holes, alignment, range boundaries and bitmap padding.
It derives query-relevant classes through the general native class algorithm,
independently of the compact representation. The same harness passed with both
layouts. All 660,523 original lookup comparisons, 8,246 generated original-caller
comparisons, 72 original unwind pairs and five typed matcher pairs pass again.
Five new generator cases cover alias retention, exact membership, prefix edges,
empty queries, stable ordering, invalid addresses, identity overflow and stale
query semantics. All 15 selected CTests pass in 25.75 seconds, including all 53
metadata-tool tests. These elapsed test times are not performance measurements.

The binding payload falls by 13,039 bytes (16,492 to 3,453); combined native query
and binding data falls from 18,172 to 5,133 bytes before code and alignment.
With the identical C++ harness and compiler configuration, the component-test
EXE falls from 801,792 to 788,992 bytes, a 12,800-byte reduction. Its virtual
`.rdata` shrinks by 13,056 bytes while `.text` grows by 448 bytes; file-section
rounding is included in the EXE result. This is a component-test measurement,
not a production EXE saving or proof of faster lookup. The catalog and native
query header remain byte-identical. The temporary control EXE is removed after
comparison; its identity, sections and input hashes remain in the evidence.

The first direct Python run omitted task-local TEMP/TMP and hit PermissionError
in two existing CLI tests. The unchanged suite passed with normal Windows worker
permissions and the project validation directory; the final CTest run also passed.
Evidence is `.build/validation/frame-work/compact-bindings.json`, with the raw
CTest log and PE comparison alongside it. The release and all 58 protected files
remain unchanged. Complete metadata lifetime/restoration, caller coverage and
native object ownership remain required before production activation.

## Caller scratch-state frontier

`tools/lift/type_call_liveness.py` pins the original cast body and every call
word, then traces possible reads of its normal-return scratch registers through
all 268 literal calls and 271 declared contexts. It follows direct callees using
the shared instruction decoder but deliberately ignores the decoder's ABI call
clobber list. Reads precede writes. Indirect calls, recursion, unknown effects,
context bounds and surviving values at encoded LR returns remain explicit.
It does not certify memory/stack, exceptions, arbitrary entries or parent callers.

The raw census has 2,710 register/context checks: 250 have possible reads, 952
have barriers, 1,842 can survive a return, and 650 have no read, barrier or return
escape in known flow. These categories overlap. A closed loop is not called an
overwrite or a termination proof. There are 17 distinct read sites: 16 are
stores relative to r1. They remain observations requiring memory-lifetime proof;
variadic saves are not discarded merely because they look temporary.

The remaining read is r11 at `0x801D5AC8`, reached by two cast continuations in
the conservative census. A separate inductive first-edge check validates the
function extent `0x801D5A18..0x801D5E04`: r9 is initialized to -1 on every path to
the recording signed-byte predicate, with no intervening calls or other writes.
The read requires taking the following signed-byte >= 0 branch. Cutting that
edge disconnects the read from normal entry, so its first traversal is impossible.
The tool checks the exact predicate shape, domination and intervening effects;
mutated initializers, calls and bypasses are refused. This proof covers normal
entry and encoded direct flow only. It does not erase the raw census or claim
arbitrary computed-entry coverage.

Eighteen new audit/guard tests bring the metadata-tool gate to 71 passing cases.
The shared wait-point classifier's nine cases and all 442 current back edges
also pass. Three selected gates were exercised; the final run repeats only the
metadata gate after adding the guard checker. Runtime C++ code was unchanged,
so the preceding component validation remains separate. No game build, replay,
FPS measurement or deployment occurred. Evidence is
`.build/validation/frame-work/caller-scratch-evidence.json`.

This audit also identifies three candidate empty variadic bodies at `0x8000D374`,
`0x8001C574` and `0x8047EC78`: 941 literal call sites and 1,663 exclusive unwind
code bytes in the current release. This is a lower-bound code attribution,
not total symbol size or runtime frequency. The encoded bodies save argument
registers into their frames and return; one returns zero and another changes
scratch registers. Caller, stack-alias and retirement obligations must be proved
before eliminating them. Candidate evidence is `variadic-empty-candidates.json`.

## Integration work still required

1. Enumerate class and virtual-table metadata from proven roots and call
   arguments. Preserve constructor/destructor changes to the dynamic type;
   do not cache a type permanently on an object whose virtual table changes.
2. Preserve shared gameplay names and classify the remaining descriptor/header
   consumers. Establish metadata ownership, descriptor lifetime, snapshot-load
   behavior and deterministic name interning. Move this ownership to native
   metadata rather than adding a permanent address-based lookup wrapper.
3. Audit every caller, including source offsets, null paths, reference failures,
   exception-index ownership and any live scratch/register state. Integrate
   the tested failure-entry contract and rerun it against the emitted boundary.
4. Lower calls to the native owner, remove the original body from generation
   and linking, and verify absence in the actual executable and map.
5. Measure executable/data cost and repeated uninstrumented gameplay/resource
   effects; compare replay state, audio and native-1440p captures with the
   pinned control. No production acceptance before those gates pass.

## Generated reference caller and original exception-runtime composition

The new `native_type_unwind_call_tests` executes the unchanged reference-cast
caller prefix at `0x8007CE74`, with the cast at `0x8007CE9C` lowered through the
native query boundary. The fixture uses the full original function table and
selects that root. It covers the failing prefix, not the function's successful
post-cast virtual calls. Normal entry and cold entry after the real stack
allocation preserve the caller's 128-byte frame and the cast's 32-byte frame.
A dispatch-table observation entry stops at the actual throw address; the
reference interpreter then executes the original exception runtime unchanged.
This is an explicit handoff, not an entirely generated or native unwinder.

All 72 pairs match complete unwind instruction traces and the shared oracle's
state/effect comparisons: six failing descriptor cases (missing metadata plus
five distinct known types), with/without an outer catch, three register seeds
and both entry paths. A wrong saved cast-caller link is detected. Native cases
erase all 612 old cast instruction bytes and 209,589 class-metadata bytes and
check the 2,210 substituted native header identities remain intact. The test
executes 62,124 original instructions. Synthetic object/vtable setup remains
explicit; these results do not establish production object ownership.

The first handoff attempt used an HLE hook that the generated indirect call did
not consult without a dispatch entry. It exited correctly to `0x803A2660`, but
the test's observer never ran. The corrected observation entry supplies only
the stop; it implements no exception semantics. The next test rejected an
incorrect assumption that this caller could reach the outer cleanup handler.
The original registered fragment at `0x80008D14` covers `0x803A1258` through
`0x8043BACB`; the selected gameplay caller is outside it. Both original and
native variants therefore reach termination before that outer catch. The test
pins this fragment and requires that behavior rather than inventing unwind
coverage. All 13 literal reference-cast sites in the current catalog lie below
this fragment. This statement concerns the pinned fixture registration, not a
proof about every possible runtime fragment registration.

The pre-existing 72-pair catch/cleanup/rethrow and five typed-matcher checks
remain unchanged in coverage and still pass. Their shared oracle was extracted
into `runtime/tests/support/type_unwind_oracle.h`; it accepts an optional caller
driver and distinguishes the outer handler frame from the nested caller frame.
The old standalone oracle still executes 158,539 original instructions.

Seven relevant CTests passed across the final selected runs, including the
660,523 lookup comparisons, 8,246 generated flag-caller comparisons, 71 metadata
tool cases and generator structure checks. The first selected run rejected a
stale liveness-root file after moving the shared oracle. The guard was not
weakened: its conservative list now includes 17 additional literals; all ten
code addresses already have entries in the retained release's generated table.
The other seven literals are data or test storage. The affected four checks
passed after regeneration. Failed handoff, handler expectation and liveness
gates remain in `.build/validation/frame-work/type-unwind-call-*.log`.

No release rebuild, deployment, gameplay/performance run or new size saving is
attributed to this test integration. The retained release and all 58 protected
files are unchanged. Production type-query sites remain disabled. Complete
live metadata ownership/restoration, broader caller contracts and generated
exception-runtime validation remain outstanding. Evidence and exact source
hashes: `.build/validation/frame-work/type-unwind-call-summary.json`.
