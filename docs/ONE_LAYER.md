# The ONE archive layer (pal-v30): specification and native status

**Native status (2026-11-04, Phase 4 batches 4a / 4b, `GEKKO_NATIVE_ONE=1`, off by default):**
- **C++ owners** (`runtime/src/gekko/native_one.cpp`, contract in `runtime/include/gekko/native_one.h`): Entry, IsOpen,
  FindName, FindSubstr, ParseDirectory, Adopt, Close, the ctor, the dtor, the sync Open with its RW stream reads,
  OpenAsync and OnRead (batch 4c). All 13 functions, with Extract owned by the expand jobs.
- **Exactness:** they reproduce guest memory exactly, their stack frames included.
- **Open's reads** continue their yields at once, and only while no thread of the same priority is ready.
- **OnRead** enters the requester's callback through the dispatcher, as its bctrl does; an owner at 0x8004CC6C runs
  its tail.
- **Proof:** `STREAMING_DIFFERENTIAL_VALIDATION.md` §10, §12 and §13.
- **Still game code on these paths:** the RenderWare heap, the RW stream open / close, the read queue's node, the
  Task ctors, and the requesters' callbacks.

The specification below is the static RE of 2026-11-03 (session note `spec_ONE_layer.md`), unchanged.

Source: dtk asm `source/.build/dolrecon/pal-v30/build/asm/auto_03_80047768_text.s` (0x8004C770-0x8004D05C) plus the
helpers it calls. Dynamic counts: `wt-gc-removal/.build/stage_loading/trace-westopolis-tape/counts.csv` (Westopolis
tape, whole run). Read-only; nothing was built or run. "unverified" = not proven from code or trace.

## 0. Function table

| Addr | Name (proposed) | Westopolis calls | Role |
|---|---|---|---|
| 0x8004CFD8 | `One::One(this, path, allocator)` ctor | 25 | init 0x58 object; sync open if path != 0 |
| 0x8004CF90 | `One::~One(this, short flags)` dtor | 24 | close; delete if flags > 0 |
| 0x8004CE1C | `One::Open(this, path)` sync | 17 | RW stream whole-file read + directory parse |
| 0x8004CC94 | `One::OpenAsync(this, path, cb, cbArg, dst, dstSize)` | 8 | queue TBGRead request |
| 0x8004CC28 | `One::OnRead(rec)` (static, TBGRead completion) | 8 | adopt buffer, parse, user callback |
| 0x8004CAC0 | `One::Adopt(this, buf, size)` | 8 | install a whole-file buffer + parse |
| 0x8004CD38 | `One::ParseDirectory(this)` | 25 | version, byte-swap, relocate offsets |
| 0x8004CB48 | `One::Close(this)` | 57 | free buffer, reset |
| 0x8004CB20 | `One::IsOpen(this)` | 166 | query |
| 0x8004CA50 | `One::Entry(this, idx)` | 43825 | entry pointer or 0 |
| 0x8004C938 | `One::FindName(this, name)` | 343 | uppercase query + strcmp scan |
| 0x8004C878 | `One::FindSubstr(this, sub, start)` | 112 | strstr scan from index |
| 0x8004C770 | `One::Extract(this, idx, dst, cb, cbArg)` | 612 | PRS / TExpandBG / memcpy (+TOneFileNotice) |

Checksum of the trace: Close 57 = Adopt 8 + OpenAsync 8 + Open 17 (each calls Close once on the reload path) + dtor
24. ParseDirectory 25 = Adopt 8 + Open 17, i.e. the OnRead failure branch (§6) never ran. Only one archive was not
deleted (the stage `stgNNNN_dat.one`, kept in 0x805F03C0 until StageManager teardown).

There is no "member size" function: every caller reads the entry's size word (entry+0x2C) directly (§11).

## 1. The archive object (0x58 bytes, no vtable, allocated by callers with `__nw__FUl(0x58)`)

| Off | Width | Meaning | Written by | Read by |
|---|---|---|---|---|
| 0x00 | char[0x2C] | path of a SYNC open (strcpy, unbounded), byte 0 = 0 when not sync-open | ctor (byte 0), Close (byte 0), Open (strcpy) | IsOpen, Entry & all "open" checks (byte 0 only), Open (strcmp) |
| 0x2C | u32 ptr | ASYNC whole-file buffer (incl. 12-byte RW chunk header); 0 otherwise | ctor/Close (memset 8), Adopt | all "open" checks, Close |
| 0x30 | u32 | ASYNC buffer size as reported by TBGRead (file size rounded up to 32, see NATIVE_THREADS) | ctor/Close (memset 8), Adopt | nobody in the layer |
| 0x34 | u32 ptr | `data` = directory base = first byte after the chunk header ("One Ver ..."). **Callers read it** | ctor (0), Close (0), Open, Adopt | ParseDirectory, Entry, FindName, FindSubstr; callers §11 |
| 0x38 | u32 ptr | SYNC: the allocation (== data). ASYNC: buf+0xC (not an allocation) | ctor (memset 8), Close (0), Open, Adopt | Close (sync branch frees it) |
| 0x3C | u32 | data size. SYNC: RwStreamRead return value. ASYNC: reported size - 0xC | ctor (memset 8), Open, Adopt | nobody found (no reader in layer or in scanned callers) |
| 0x40 | u32 ptr | allocator object (ctor arg 3). **Every caller passes 0** (9 sites) | ctor | Open (alloc), Close (free) |
| 0x44 | u8 | async-pending flag | ctor (0), OpenAsync (1 on accept), OnRead (0) | Open, OpenAsync (guard) |
| 0x45..0x47 | | padding, never written | | |
| 0x48 | u32 fn | user callback `void cb(void* arg)` | OpenAsync (always, before queueing) | OnRead |
| 0x4C | u32 | user callback arg | OpenAsync | OnRead |
| 0x50 | u8 | 0 from ctor; no other access found (unverified: purpose) | ctor | none found |
| 0x51..0x53 | | padding | | |
| 0x54 | u32 | 0 from ctor; no other access found (re_A note "per-archive count" NOT confirmed: TOneFileAsyncManager 0x8004D564 / 0x8004D424 do not touch it) | ctor | none found |

"Open" predicate used everywhere (inline in Entry, FindName, FindSubstr, Extract, Close, Open, IsOpen):
`open = (this->+0x2C != 0) ? 1 : ((s8)this->name[0] != 0)` (the `subic/subfe` pair = `x != 0`).

## 2. File / directory format as the guest sees it

Whole file = RW chunk header (12 bytes, LE: type 0, length = filesize-12, libID 0x1C020037 or 0x1C020020) + data.
`data` (object +0x34) layout, all little-endian on disc:
- data+0x00: ASCII "One Ver 0.60" / "One Ver 0.50" (+NUL); data+0x0C: u32 0; data+0x10: u32 entry count N;
  data+0x14..0x33: 0x00 then 0xCD fill (debug fill, never read).
- entry table at data+0x34, stride 0x38, slots 0..N+1; slots 0 and 1 are zero and are never visited (Entry rejects
  idx < 2). Entry i = data + 0x34 + i*0x38 (== data + 0x34 + 0x70 for the first member).
- entry (0x38): +0x00 name (NUL-terminated; 0x20 bytes in 0.50 files, up to 0x2C in 0.60), +0x2C size
  (uncompressed length), +0x30 offset relative to data, +0x34 flags (bit0 = PRS). 0.50 files keep size/offset/flags
  at +0x20/+0x24/+0x28.
- Survey of the 1244 `game/files/**/*.one`: 1231 are 0.60/0x1C020037, 11 are 0.50/0x1C020020, 2 are
  0.50/0x1C020037 (the guest decides by the version STRING only). All names are uppercase, longest 39 chars. For all
  files chunk length + 12 == file size.

## 3. Ctor 0x8004CFD8 `One* One(One* this /*r3*/, const char* path /*r4*/, void* allocator /*r5*/)`

Write order exactly:
1. `stw 0 -> +0x54`; 2. `stb 0 -> +0x50`; 3. `stw allocator -> +0x40`; 4. `stb 0 -> +0x00`;
5. `memset(this+0x2C, 0, 8)` (guest call to MSL memset); 6. `memset(this+0x38, 0, 8)`;
7. `stw 0 -> +0x34`; 8. `stb 0 -> +0x44`.
9. if path != 0: `Open(this, path)` (return value ignored). Returns this.
Not written: name bytes 1..0x2B, +0x45..0x47, +0x48, +0x4C, +0x51..0x53 (left as heap content).
Callers (all `allocator = 0`; object from `__nw__FUl(0x58)`, null-checked): ResourceOneFile vf1 0x80014A60 (path) /
0x80014AAC (null), LandALoadMananger::update 0x80046718 / 0x80046854 (null), fn_80046BB4 0x80046C10 (texCM, path),
ResourceOneFileReq vf1 0x8006B20C (path), TPjsEventActionScene 0x80171D28 (null), fn_80177868 0x801778D0 (null),
ref_cylinder_model_dff 0x802E14E8 (path).

## 4. Dtor 0x8004CF90 `One* ~One(One* this, s16 flags)`

If this != 0: `Close(this)`; if `(s16)flags > 0`: `__dl__FPv(this)`. Returns this (also when 0).
Callers: ResourceOneFile vf1 (x3), LandALoadMananger::update (x2) / dtor (x2), fn_80046BB4, ResourceOneFileReq vf1,
TPjsEventActionScene (x2), StageManager vf1 sub6 0x801782C0 (stage archive 0x805F03C0), ref_cylinder_model_dff.

## 5. Sync open 0x8004CE1C `bool Open(One* this, const char* path)`

Stack: 0x30 frame; `RwChunkHeaderInfo info` at sp+0x08 (type +0x08, length +0x0C, version +0x10, build +0x14,
isComplex +0x18).
1. if `this->+0x44 != 0` -> return 0 (nothing written).
2. if path == 0: if open -> return 1; else `Close(this)` (a no-op returning 0 on a closed archive) -> return 0.
3. `strcmp(this /*name*/, path)` (fn_803AD96C, case-SENSITIVE, plain C strcmp) == 0 -> return 1 (already loaded; no
   re-read; note it ignores whether the archive is actually open, and an async-opened archive has an empty name).
4. `Close(this)`.
5. `mgr = fn_80037200()` (TBGRead/file-manager singleton getter; first call constructs 0x805EF168 via fn_800452B8 and
   `__register_global_object(…, fn_8004527C, 0x80572368)`, sets 0x805EF165=1, 0x805EF064=&obj).
6. `stream = fn_80045014(mgr, path)`: `tmp = fn_80051938(path)` (-> fn_80051304: allocated, separator-normalised copy of
   the path via fn_80481580; exact transform unverified), `stream = fn_804823B8(2 /*rwSTREAMFILENAME*/, 1 /*rwSTREAMREAD*/,
   tmp)` (RwStreamOpen), `fn_80051958(tmp)` (-> fn_800512C4 free). stream == 0 -> return 0 (archive stays closed,
   name[0] = 0).
7. `fn_8047E99C(stream, &info)` = RwStreamReadChunkHeaderInfo (RwStreamRead 12 bytes + native conversion); its return
   value is IGNORED (on failure info.length is stack garbage - unverified/unreachable on disc data).
8. `len = info.length` (sp+0x0C).
9. `fn_80049B3C(0x40)` -> fn_80051BE8: `*(u32*)0x805E4A88 = 0x40` (RW allocation category global).
10. allocation: if `+0x40 != 0`: `buf = allocator->vtbl[+0x08](allocator, len)` (DEAD: no caller passes one);
    else `buf = fn_80049BB8(len, 2)` = `RwEngineInstance->+0x108(len, 2 + 0x32 = 0x34)` (RwMalloc, hint 0x34).
11. `fn_80049B1C()` -> fn_80051BDC: `*(u32*)0x805E4A88 = 0x20` (unconditionally 0x20, NOT the previous value).
12. buf == 0 -> return 0. **Leak: the stream is not closed**; name not set.
13. `n = RwStreamRead(stream, buf, len)` (0x80481D9C); n stored to sp+0x0C.
14. `RwStreamClose(stream, 0)` (0x8048229C).
15. `strcpy(this->name, path)` (fn_803ADB50; unbounded - a path >= 44 chars would overwrite +0x2C...; longest real path
    is far shorter, unverified for all callers).
16. `+0x38 = buf`; `+0x3C = n`; `+0x34 = +0x38` (re-read from memory then stored).
17. `ParseDirectory(this)`; return 1.
Guest-visible heap effects: path copy alloc+free (step 6), RW stream object open/close (RW freelist), ONE RwMalloc of
`len` (= filesize-12) with hint 0x34 under category 0x40. RtGCNFS reads inside RwStreamRead yield the thread
(OSYieldThread, see re_A §5) - a scheduling point visible to equal-priority threads.
Callers: ctor (with path), fn_80177868 0x80177940 (stage `_dat.one` in sync mode, then calls fn_80177964 directly).

## 6. Async open 0x8004CC94 `bool OpenAsync(One* this, const char* path, void (*cb)(void*), void* cbArg, void* dst, u32 dstSize)`

Frame 0x40; request block at sp+0x08 (6 words): `[0]=path, [1]=dst (r7), [2]=dstSize (r8), [3]=this, [4]=0x8004CC28,
[5]=UNINITIALISED stack word (sp+0x1C; never written by this function).`
1. if `+0x44 != 0` -> return 0 (nothing written).
2. `Close(this)`.
3. stores (in this order): `sp[0]=path, sp[1]=dst, sp[2]=dstSize, sp[4]=0x8004CC28, sp[3]=this`, `+0x48 = cb`,
   `+0x4C = cbArg`.
4. `mgr = fn_80037200()`; `ok = fn_8004506C(mgr, &req, 1)`:
   - `exists = (*(fn*)(mgr+0x04))(req[0])` (RwFexist per re_A); false -> return 0.
   - else `fn_80045520(&req, 1)`: `node = fn_80049BB8(0x60, 2)` (RwMalloc hint 0x34; NO category push here);
     `node->+0x5C = 1`, `node->+0x00 = 0`, appended to the TAIL of FIFO list `0x805742A0[1]` (linked BEFORE the null
     check: a null node would fault at the +0x5C store); words 0..5 of req copied to node+0x04..0x18 (word 5 = the
     uninitialised stack word); `strcpy(node+0x1C, path)` (0x40-byte field, unbounded); `node+0x04 = node+0x1C`;
     `node+0x5C = 1` again; return 1.
5. accepted (low byte != 0): `+0x44 = 1`, return 1. Refused: return 0 with the archive CLOSED and +0x48/+0x4C already
   overwritten.
Callers: ResourceOneFile vf1 0x80014AEC (cb fn_80014DAC, arg this+0x20, dst 0), LandALoadMananger::update
0x80046744 / 0x800468A0 (cb 0, dst 0 - completion only parses; the manager polls IsOpen), TPjsEventActionScene
0x80171D50 (cb fn_80171AC4, **caller-provided dst = r30, dstSize = +0x6C** - Close later frees that buffer with RwFree;
untraced, unverified whether it was RwMalloc'd), fn_80177868 0x80177920 (stage `_dat.one`, cb fn_80177964, dst 0).

## 7. Completion 0x8004CC28 `void OnRead(u32* rec)`

Called by TFileControlAsycManager::update 0x8004580C on the main thread as `rec->[4](task+0x28)`, where task+0x28..0x3C is
the 6-word block (TBGRead has replaced [1] with the buffer it filled - RwMalloc'd `fn_80049BB8(roundup32(filesize),2)`
when dst was 0 - and [2] with the reported size = roundup32(filesize)). Fields used: `rec[1]` buf, `rec[2]` size,
`rec[3]` archive.
- buf != 0: `Adopt(archive, buf, size)`; then if `+0x48 != 0`: `(+0x48)(+0x4C)` (single argument r3 = cbArg).
- buf == 0: `ParseDirectory(archive)` on a CLOSED archive (+0x34 == 0 -> sscanf of address 0 / load from 0x10; a
  crash on hardware). Not reached in the trace; TBGRead drops failed opens silently (NATIVE_THREADS), so
  unverified/unreachable. The user callback is NOT called on this path.
- Always: `+0x44 = 0` (after the callback returns).

## 8. Adopt 0x8004CAC0 `bool Adopt(One* this, u8* buf, u32 size)`

`Close(this)`; `+0x2C = buf`; `+0x30 = size`; `+0x38 = buf+0xC`; `+0x3C = size-0xC`; `+0x34 = +0x38` (re-read);
`ParseDirectory(this)`; return 1. Only caller: OnRead. (Close here is normally a no-op: OpenAsync already closed.)

## 9. ParseDirectory 0x8004CD38 `void ParseDirectory(One* this)`

Frame 0x20; `float ver` at sp+0x08.
1. `sscanf(this->+0x34, "One Ver %f", &ver)` (fn_803AC608, MSL sscanf; `crclr cr1eq` = no FP varargs). Return value
   ignored; if the match fails `ver` is stack garbage (unverified; every disc file matches).
2. `fn_80043D34(data+0x10, 4)`: in-place byte swap of the count (loop `ctr = (n+3)>>2` words, `lwz; stwbrx`).
3. `for (i = 0; i < (s32)(*(u32*)(data+0x10)) + 2; ++i)` (bound re-read from memory each iteration; signed compare):
   `e = Entry(this, i)`; e == 0 (i < 2) -> continue. Otherwise:
   - if `ver <= 0.59f` (`fcmpo` with `cror eq,lt,eq`; constant kF32_0_59; NaN -> not legacy): copy raw words
     `e+0x34 = e+0x28; e+0x30 = e+0x24; e+0x2C = e+0x20` (this store order).
   - swap `e+0x2C`, then `e+0x30`, then `e+0x34` (each a 4-byte stwbrx in place).
   - `e+0x30 = this->+0x34 + e+0x30` (offset -> absolute guest pointer).
Guest-visible writes: data+0x10 and, per member, entry +0x2C/+0x30/+0x34 (and the legacy copies). Names untouched.
The function is not idempotent; it runs exactly once per loaded buffer.

## 10. Close 0x8004CB48 `bool Close(One* this)`

- not open -> return 0, nothing written.
- `+0x2C != 0` (async buffer): free `+0x2C` via `allocator->vtbl[+0x18](allocator, ptr)` if `+0x40 != 0` (dead) else
  `fn_80049B5C(ptr)` = `RwEngineInstance->+0x10C(ptr)` (RwFree); then `memset(this+0x2C, 0, 8)`.
- else (sync): free `+0x38` the same way (no memset).
- then `+0x34 = 0`, `+0x38 = 0`, `+0x00 (byte) = 0`; return 1. `+0x3C` is NOT cleared.

## 11. Queries

**Entry 0x8004CA50** `u8* Entry(One* this, s32 idx)` (leaf, no frame): not open -> 0; `idx < 2` (signed) -> 0;
`idx >= count+2` (signed, count = data+0x10 after swap) -> 0; else `data + 0x34 + idx*0x38` (`mulli; addi 0x34; add`).

**IsOpen 0x8004CB20**: the open predicate (returns 0/1).

**FindName 0x8004C938** `s32 FindName(One* this, const char* name)`: not open -> -1; name == 0 -> -1.
Uppercase copy into a 0x2C-byte stack buffer at sp+0x08: up to 44 iterations (`ctr = 0x2C`); for each byte c
(signed): if 'a' <= c <= 'z' store c-0x20 else store c; stop after storing a NUL. A name of >= 44 chars leaves the
buffer unterminated (strcmp then runs into the frame - not reached by any real query). Then
`for (i = 2; i < count+2; ++i) { e = Entry(this,i); if (!e) break; if (strcmp(e, buf) == 0) return i; }` -> -1.
Forward scan, FIRST match wins. The query is uppercased, the entry name is compared as stored (case-sensitive
strcmp; all disc names are uppercase so the lookup is effectively case-insensitive on the query side only).
NOTE: the host `OneArchive::find` scans backwards ("later entries win") - that matches FindResourceByName 0x800145A4,
not this function.

**FindSubstr 0x8004C878** `s32 FindSubstr(One* this, const char* sub, s32 start)`: not open -> -1; sub == 0 -> -1;
`for (i = start; i < count+2; ++i) { e = Entry(this,i); if (!e) return -1; if (strstr(e, sub)) return i; }` -> -1
(fn_803AD710 = MSL strstr, case-sensitive; a start < 2 returns -1 immediately). Callers: LandALoadMananger::update
(`"COLI"`, start 2), sub11 (table 0x8051E328 of substrings, start 2).

**Member size**: no function. Callers read `Entry(...)->+0x2C` (u32, uncompressed size, already swapped).

## 12. Extract 0x8004C770 `u32 Extract(One* this, s32 idx, u8* dst, cbfn cb, void* cbArg)`

1. not open -> return 0. `e = Entry(this, idx)`; e == 0 -> return 0.
2. `e+0x34 & 1` (PRS):
   - cb != 0: `fn_8035A018()` (empty `blr` stub), `p = __nw__FUl(0x348)`; if p:
     `TExpandBG__ctor(p, src = e+0x30, dst, cb, cbArg)` = Thread ctor(p,0); `p+0x328 = TExpandBG vtable`; `+0x330 =
     src`; `+0x334 = dst`; `+0x338 = cb`; `+0x340 = cbArg`; `+0x33C = 0`; `fn_8025D580(fn_80014B90(), p, 1,
     "Expanding")` (busy registry). `fn_8035A014(p)` (empty stub). Return 0. Later: worker PRS, then a 0x38
     TExpandNotice Task calls `cb(size, cbArg)` (NATIVE_THREADS). Westopolis: 21 TExpandBG ctors.
   - cb == 0: `return fn_80043FE0(src = e+0x30, dst)` (PRS, no size bound on either side; returns the decoded length;
     already a native owner).
3. not compressed: `memcpy(dst, e+0x30, e+0x2C)`; if cb != 0: `fn_8004AD84(cb, size = e+0x2C, cbArg)` =
   `p = __nw__FUl(0x38)`; if p: `TOneFileNotice__ctor(p, parent = *(u32*)0x805EF230, cb, size, cbArg)` (Task ctor,
   vtable, +0x28 cb, +0x2C size, +0x30 arg, name "TOneFileNoice", flags |= 0x100); its update calls `cb(size, arg)`
   once next tree pass. Return `e+0x2C`.
Return values: sync PRS -> decoded length; memcpy -> size; async PRS -> 0; failure -> 0.
Callers: the six TOneFileAsync<T> bodies (0x8004AE60, B3A0, B7BC, BBD4, BFF0, C408: with cb fn_8004C6B4-style
done-callbacks when the Task has a callback, else cb 0), ResourceOneFileReq sub6 0x8006A32C (.BIN, cb 0),
fn_80177964 0x80177A00 (PATH.PTP, cb fn_80177B30, dst = RwMalloc(size,2) stored in 0x805F03C4).

## 13. Callers that read archive memory directly (must keep working byte-for-byte)

- `archive+0x34` then `data+0x10` (swapped count) as a loop bound: ResourceOneFile vf1 sub4 0x80013C4C/0x80013C64,
  fn_80177964 0x80177998/0x80177B0C, ResourceOneFileReq vf1 0x8006B22C (allocates count x 0x3C records),
  LandALoadMananger::update 0x800467A0/0x800468F0, ref_ASOBI 0x801717A4.
- **fn_80177964 bypasses Entry**: `e = data + 0x34 + 0x70 + k*0x38` directly, strcmp(e, "PATH.PTP") (case-sensitive),
  strstr(e,"BDT"), reads `e+0x2C`.
- Entry name `e+0x00`: strchr('.'), strstr("TXD"/"_TEX"/"COLI"), strcpy (fn_80170924 0x801709B4 copies the name).
- Entry size `e+0x2C`: every TOneFileAsync<T> state 0 (alloc `(size+0x401F)&~0x1F` then `(size+0x1F)&~0x1F`, heaps
  2 then 1), ResourceOneFile sub4, ref_XNCP, ResourceOneFileReq sub6.
- No caller found reading +0x30 (pointer), +0x34 (flags), +0x3C, +0x50, +0x54 of the entry/object outside the layer
  (scan limited to the listed caller functions; unverified for the event/ASOBI paths).
- TOneFileAsync<T> Tasks keep the archive pointer and index (`task+0x40`, `task+0x34`) and call Entry/Extract later:
  the archive must stay valid and unchanged between frames.

## 14. Proposed native boundary

Natively ownable with identical guest state (pure functions of guest memory, no allocation, no guest calls):
- Entry, IsOpen, FindName, FindSubstr: pure reads; native versions must reproduce the exact match rules (uppercase
  query only, first match, Entry's signed bounds, strstr). FindName's stack buffer is a frame-local; nothing else
  reads it.
- ParseDirectory: in-place swaps + relocation; native must perform exactly the writes of §9 (including the legacy copy
  and the absolute +0x30 pointer). The sscanf/strcmp/memset are MSL calls without side effects beyond the buffers.
- Adopt, Close's field writes, the ctor's field writes. Close's free must still go through the guest RW free hook
  (RwEngineInstance+0x10C) - or a native owner of that allocator - with the same pointer.
- Extract sync paths: PRS (already native) and memcpy, returning the same length.
- Open (sync): the data path (chunk header, read, close) can be served from the host file IF the guest-visible
  effects are reproduced: global 0x805E4A88 = 0x40 around the alloc then 0x20; ONE `RwMalloc(filesize-12, 0x34)`
  through the guest allocator in the same order relative to other allocations; buffer bytes = file[12..]; strcpy name;
  +0x38/+0x3C/+0x34; parse. What is dropped: the path-copy alloc/free pair and the RW stream object open/close
  (net-zero only if the heap/freelist end state is identical - unverified for the path-copy heap), RW error state
  on short reads, and the RtGCNFS OSYieldThread points (a scheduling change: TBGRead/TExpandBG workers could
  previously run there). Keep the failure semantics: missing file -> 0 with the archive closed; alloc failure -> 0.

Must stay game code (or be reproduced as the same guest calls in the same order):
- OpenAsync's queueing: fn_80037200 (singleton construction on first use), the exists check (mgr+4), fn_80045520's
  0x60 RwMalloc node and list append, the +0x48/+0x4C writes before the call, the +0x44 flag. The TBGRead read
  itself belongs to the async-loader spec (NATIVE_THREADS), not this layer.
- OnRead's call of the user callback (fn_80177964, fn_80014DAC, fn_80171AC4: game logic) and the +0x44 clear after it.
- Extract's TExpandBG creation (0x348 operator-new, Thread ctor, "Expanding" busy registration) and TOneFileNotice
  (0x38 Task under 0x805EF230): Task-tree / thread objects and deferred callbacks are game-visible; only the PRS
  work inside the TExpandBG worker may be native.
- Callers (ResourceOneFile*, LandALoadMananger, TOneFileAsync<T>, fn_80177964, events) stay; they read +0x34,
  data+0x10, entry +0x00/+0x2C directly, so a native archive must keep the guest buffer as the source of truth
  (no host-side shadow directory without the guest bytes).

Open / unverified items: purpose of +0x50/+0x54; OnRead's buf==0 path (crash, believed unreachable); the
TPjsEvent caller-provided buffer later freed by Close; exact transform and heap of fn_80051304; MSL identities of
fn_803AD96C (strcmp), fn_803AD710 (strstr), fn_803ADB50 (strcpy), fn_803AC608 (sscanf) inferred from use and size,
bodies not read; the uninitialised request word 5 (copied into the TBGRead node +0x18 and the completion Task +0x3C;
no reader found).
