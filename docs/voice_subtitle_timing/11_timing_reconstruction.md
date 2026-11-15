# 11 - Message system and dialogue voice layer: disassembly and native reconstruction

Phase 1 of the reconstruction (PAL main.dol, `.build/dolrecon/pal-v30/main.dol`). Disassembly read with `tools/doldis.py`, callers with
`tools/dolxref.py`. r13 = 0x805EC500, r2 = 0x805FA780. Behaviour statements are read from the instructions at the given address; section 7 is what was run.

Code: `runtime/include/gekko/native_message_system.h`, `native_dialogue_voice.h`, `runtime/src/gekko/native_message_system.cpp`,
`native_dialogue_voice.cpp`, test `runtime/tests/native_message_voice_tests.cpp`, binding and owner wiring `runtime/src/gekko/native_message_voice_owner.cpp`.

## 0. Corrections to the earlier notes

| note | claim | what the code does |
|---|---|---|
| 02 section 5A step 2, 01 "Admission rule", comment in `message_voice_hold_core.cpp:229` | a line with no record falls through to accept | **Refused.** 0x801FB288 `beq 0x801FB29C` (release, return -1). Accepted without a lookup only when the font is gone (0x801FB270). The E4 hook's decline there is harmless: it hands the case to the original. |
| 03 sections 1.5, 8 | window close / finish stops the voice | Refuted (the brief says so too). Close is only `mgr+0x14 = -1` at 0x801FB8C4; no audio call is near it. |
| 01 table, open item 2 | fn_801FAF54 has no caller | One caller: fn_802FAFD0 at 0x802FB018 (owner unnamed; it also calls rand 0x8040FAE0 itself). |
| 02 section 6 | stopping an id no longer playing is a no-op | True for ids >= 0, but -1 **matches every idle slot** (their +0x2C is -1), and the reset stops `mgr+0x20`, which is -1 before any voice: the ADXT handles of idle slots get `adxt_Stop` + `adxt_Pause(0)` (harmless to audio, but a real call sequence). Also, `mgr+0x20` is never cleared by a reset, only by the re-init. |
| 02 section 1 ("+0x2C (u8) new group, passed to the draw") | used by the draw | fn_80049BE8 overwrites r8 at entry (0x80049BF0 `lis r8`). The field is written and passed but no code reads it. |
| brief | "alloc 0x801D874C" | It allocates the four ADXT work areas, after three CRI setup calls. The TObjSERIF objects are `new`ed in fn_801D844C (0x801D8600-0x801D862C). |

New facts:
- fn_801FADD8 with a negative key answers `queue not empty` (0x801FADF4-0x801FAE00: `empty` then logical not), the reverse of what "finished" suggests. Kept.
- `G+0xA` (0x8057E76A): when 0, fn_80049BE8 draws nothing and returns 0, so the update closes every window in the update that opens it and never plays the voice (0x801FB85C-0x801FB908). Not the PC subtitle switch.
- The MessageTask name and the TObjSERIF name are real strings (0x804ECE58 "MessageTask").

## 1. Objects

### MessageManager, singleton at 0x80580B18 (0x34 bytes)
| off | field | evidence |
|---|---|---|
| +0x00 | queue capacity (`std::vector<QueueEntry>`) | 0x801FC05C (reserve 0x40 in the ctor 0x801FB4B4) |
| +0x04 | queue count | 0x801FAEA8 |
| +0x08 | queue data, entries 12 bytes `{group, number, variant}` | 0x801FAEDC, 0x801FB2BC |
| +0x0C, +0x10 | `boost::weak_ptr<Font>`: px, shared count | 0x801FB104 (lock), 0x801FB450, 0x801FB350 |
| +0x14 | current group, -1 = no window | |
| +0x18, +0x1C | current number, variant | |
| +0x20 | voice last started, -1 initially, cleared only by the re-init | 0x801FB8EC, 0x801FB42C |
| +0x24 | lock counter (0x801FADB4 -1, 0x801FADC4 +1) | enqueue refuses when nonzero |
| +0x28 | current line's priority, -1 idle | 0x801FB898, 0x801FB7DC |
| +0x2C | draw enable, u8 (MessageTask ctor sets 1) | |
| +0x2D | third layout row, u8 | |
| +0x30 | the MessageTask | |

Singleton: pointer at r13+0x3740 = 0x805EFC40, guard byte r13+0x373A = 0x805EFC3A. First use: fn_801FB490(mgr) then 0x803A1210(mgr, dtor 0x8010A1B8, block 0x80580AFC), guard = 1. fn_801FB490 stays translated (vector reserve through the game's allocator).

### MessageTask (0x40 bytes), vtable 0x805437D4 (vf0 0x801FB938, vf1 0x801FB5AC), base `Task` (ctor 0x8004F014, dtor 0x8004EF18; +0x00 name word, +0x04 u16 flags, +0x10 parent, +0x18 vtable)
+0x28 f32 elapsed seconds; +0x2C u8 new-group (dead); +0x30 current record; +0x34 f32 x; +0x38 f32 y; +0x3C f32 scale. Flags gets 0x100 (updated while paused, walker 0x8004ED6C). Kill bit 0.

### Record (20 bytes in the .fnt table at font+0x5C: `u32 count; Record[]`, sorted by key)
+0 text (runtime pointer), +4 key = group*1000 + number*100 + variant, +8 priority, +0xC frames (0 = 180), +0x10 voice (-1 none).

### Font weak pointer handling
Boost counted base: `{ vtable, use, weak }` (use at +4, weak at +8). Lock: `use == 0` or null count = empty pointer (fn_801FB1E4). Otherwise use is incremented twice (0x80016914 lock, 0x800097E0 copy) and the temporary released once (0x8000945C): net +1 for the call. Final release 0x8000945C: `use-1`; at 0 call dispose vtable+0xC, then `weak-1`, at 0 destroy vtable+0x10. The native code does the net +1 and the matching release, and hands a release reaching 0 to the game's 0x8000945C unchanged.

### Layout table 0x80543798 (f32 x, y, scale, scale-language-0)
+0x00 0.56 / 0.18 / 0.038 / 0.04 (`3F0F5C29 3E3851EC 3D1BA5E3 3D23D70A`); +0x10 (G+0xB1 set) 0.55 / 0.22 / 0.038 / 0.04 (`3F0CCCCD 3E6147AE 3D1BA5E3 3D23D70A`); +0x20 (mgr+0x2D set) 0.61 / 0.18 / 0.04 / 0.04 (`3F1C28F6 3E3851EC 3D23D70A 3D23D70A`). Scale: G+0x58 == 0 picks the fourth column, else the third.

### TObjSERIF (0x48 bytes), array r13-0x2930 = 0x805E9BD0 [2], vtable 0x8053DFDC (vf0 0x801D6EA4, vf1 0x801D6E14)
+0x28 s8 index; +0x29 s8 state (0 idle, 1 playing); +0x2A s8 priority (3 never replaced; <=0 replaceable; else only by a strictly higher one); +0x2B bit 0 paused; +0x2C AFS index (-1); +0x30 kind (-1); +0x34 last status; +0x38 volume; +0x3C ADXT; +0x40 work area; +0x44 ctor arg (unused, 0 at the one site).

Statics: BGM objects r13-0x2940 = 0x805E9BC0 [2]; BGM work r13-0x2938 = 0x805E9BC8 [2]; SERIF work r13-0x2928 = 0x805E9BD8 [2]; r13-0x2920 = 0x805E9BE0 (loaded voice language, 6 = none).

## 2. Message system, function by function

**fn_8010A14C** accessor: section 1.

**fn_80049E44(font, group, number, variant)** table null = 0; key = group*1000 + (number*100 + variant) (wrapping). `lo = 0, hi = count-1; while (hi - lo > 1) { mid = (lo+hi)/2 toward zero; key[mid] < key: lo = mid; == : return; else hi = mid }`, then test lo, then hi. Count 0 reads index 0 and -1: the original has no guard (0x80049E78-0x80049EE0).

**fn_80049D60(font, group, number, variant, outFrames, outVoice)** the same search; null record = 0; else `*outFrames = +0xC` if non-null, `*outVoice = +0x10` if non-null; returns record +0 (text pointer). Only caller: fn_80049BE8.

**fn_80049BE8(font, group, number, variant, outVoice, f1 x, f2 y)**: `G+0xA == 0` returns 0 at once. r8 is clobbered at entry: the caller's flag is unused. `text = D60(number)`; if 0 and number != 0, `number = 0`, retry. If 0, return 0. fn_8004A09C(font, text, x, y). Table null returns 0. Search for (group, number (0 if retried), variant) and return the record. Other callers (all outside the message system): 0x800BA004, 0x800BA038, 0x802BF35C, 0x802BF39C, 0x802BF48C, 0x802BF4C4, 0x802E55FC, 0x802E5720.

**fn_801FAEF4 reset** queue count = 0 (fn_8010B670; the element dtors are empty); fn_8005328C; fn_801D551C(mgr+0x20); `mgr+0x14 = -1`.

**fn_801FB210 enqueue(mgr, group, number, r6 unused, first_variant)** `+0x24 != 0` returns -1. Lock font. Font alive: record(group, number, 0): null or `priority < mgr+0x28` returns -1. Reset. Push `{group, number, v}` for v = first_variant .. 35. Release, return `group*1000 + number*100`.

**fn_801FAF54 random enqueue(mgr, group)** lock test. Font null returns -1. `lines` = first variant 0..35 with no record (of number 0); 36 returns -1. Record(group,0,0): null or lower priority returns -1. `rand(lines)` (0x8040FAE0); push `{group, 0, r}` (no reset); return `group*1000`.

**fn_801FBA18 queue pop(mgr, entry)** forward copy of the tail by 12s (0x801FBAAC), count-1. Returns entry.

**fn_801FADD8 finished(mgr, key)** key < 0: `count != 0`. Else 0 if key == current group*1000 + number*100; else 0 if any queue entry matches; else 1.

**fn_801FB3CC reinit(mgr, shared_ptr*)** old task flags |= 1, `mgr+0x30 = 0` (fn_801FB30C). Weak pointer cleared, old count weak-released (fn_801FB350). Weak assigned from the shared_ptr (px, then weak+1 on the new count, 0x80017390). `new 0x40`; if non-null parent = fn_800461A0()+0x54, MessageTask ctor. `mgr+0x30 = task`; +0x14 = -1, +0x20 = -1, +0x24 = 0, +0x2D = 0 (byte), +0x28 = -1. The old task's destructor runs later, from the task walker, and calls the reset on this manager.

**fn_801FB99C MessageTask ctor(this, parent)** Task ctor; vtable; elapsed 0.0f; +0x2C 0; +0x30 0; +0 name 0x804ECE58; manager reset; `mgr+0x2C = 1`; flags |= 0x100.

**fn_801FB938 vf0 dtor(this, s16 flag)** null test; vtable; manager reset; Task dtor(this, 0); `flag > 0` delete.

**fn_801FB5AC vf1 update(this, f1 dt)**
1. Gate fn_80203CF8(0x8057E808): bytes +0x21 or +0x23 nonzero: return immediately, nothing updated, timer frozen.
2. Layout row (above) to +0x34/+0x38/+0x3C.
3. Lock font; empty: return.
4. `count == 0 && cur < 0`: `mgr+0x28 = -1`, return.
5. Queue head (when begin != end): no record: pop. Else if `cur == -1` or no current record or `prio > curPrio`, or `head.group != cur && prio == curPrio`: open: `+0x2C = (cur != head.group)`, `mgr+0x14/18/1C = head`, opened = 1, `elapsed = 0.0f`, pop. Else lower priority: pop; equal priority and same group: stays queued.
6. `cur < 0`: `mgr+0x28 = -1`, return.
7. `font+0x64 = +0x3C` (bits).
8. Draw flag set: `+0x30 = fn_80049BE8(... , &voice, x, y)`. Else `+0x30 = lookup`, voice = record+0x10 (0x801FB850 reads it unchecked: the native code fails closed on a null record instead of faulting; the record exists because the queue lookup found the same key and no re-init intervenes).
9. Null record: `cur = -1`, return (no priority write).
10. `frames = rec+0xC` or 180. `mgr+0x28 = rec+8`.
11. `limit = f32(frames) / 60.0f` (fsubs of the 0x43300000 biased int, then fdivs; 60.0 at 0x805F7794).
12. `elapsed = elapsed + dt` (fadds, f31 = dt), stored at +0x28. `elapsed >= limit` (fcmpo + cror, false on NaN): `cur = -1`. No audio here.
13. opened and `voice >= 0`: fn_8005328C; stop by id (mgr+0x20); `mgr+0x20 = voice`; fn_8005328C; PlayVoice(voice, -1, -1). The stack slot holding voice is not initialised, but steps 8-9 guarantee it was written whenever a record exists.
14. Font release.

## 3. Dialogue voice layer

Slot pointers are re-read through the array where the original reloads them.

**0x801D7C64 choose(kind, prio)** kind >= 0: first slot with +0x30 == kind decides: `canReplace(+0x2A, prio) ? index : -1`. Otherwise: first non-null slot with `+0x29 != 1` (byte) is returned at once; then candidates with `canReplace(old, prio)`: first becomes best; later ones replace best if `canReplace(old, bestPrio)`. `canReplace(old, n) = old != 3 && (old <= 0 || old < n)`; the sign-bit idiom `((a^b)>>1) - ((a^b)&b)` is `a < b` for s8 inputs (exhaustive test).

**0x801D7DF0 play(afs, prio, kind)**, wrapper 0x801D5540 (`r3 = mgr` ignored, `extsb` prio). Choose (sign-extended prio); -1 returns -1. Stop + pause(0) + clear on the chosen slot; `+0x2C = afs`, `+0x2A = prio`, `+0x30 = kind`; `adxt_StartAfs(h, 0, +0x2C)`; `+0x29 = 1`; return slot.

**0x801D7BD0 stop by id**, wrapper 0x801D551C: every non-null slot with `+0x2C == id` (both slots can match). **0x801D7B4C stop all**, wrapper 0x801D54FC. **Clear** in store order: `+2C=-1, +30=-1, +2A=-1, +29=0, +2B &= ~1`.

**0x801D8058 / 0x801D7F80 pause / resume**, wrappers 0x801D55AC / 0x801D558C. Entangled with BGM: each has a first loop over the two BGM objects. Pause: state 1 sets the flag only, 2..6 also `adxt_Pause(+0x44, 1)`, other states nothing. Resume: 2..6 `adxt_Pause(+0x44, 0)`; flag cleared for any non-null object. Only the fields needed for that (+0x29, +0x2B, +0x44) are named; TObjBGM itself is out of scope. Then SERIF: state 1 only, pause(1/0) + flag.

**0x801D6F60 reset** stop, pause(0), clear; fields written again: state 0, prio -1, voice -1, kind -1, status -1, volume -0x28, flags 0; `adxt_SetOutVol(h, -0x28)`.

**0x801D6FEC ctor(this, parent, index, work, extra)** Task ctor; vtable; name word from r13-0x2910; flags |= 0x100; +0x40 work; +0x44 extra; `adxt_Create(1, work, 0x17124)` to +0x3C; null: flags |= 1, return. `+0x28 = index`; reset.

**0x801D6E14 update** sign-extended state: 1: status = GetStat to +0x34, reloaded; 0 or 5: `+2C=-1, +30=-1, +2A=-1, +29=0` (flags untouched). 0: status only. Others: nothing.

**0x801D874C allocate** 0x803C5750(0), 0x803C5118(0), 0x803B72D8(); `0x389E4` twice to r13-0x2938; `0x17124` twice to r13-0x2928; each via 0x80049BB8(size, 2); r13-0x2920 = 6.

## 4. Boundaries kept (interfaces only)

ADXT, unchanged: 0x803C3D34 Create, 0x803C2F90 Stop, 0x803C21C4 Pause, 0x803C3E40 StartAfs, 0x803C2F38 GetStat (-1 for a null handle), 0x803C25BC SetOutVol. CRI setup 0x803C5750, 0x803C5118, 0x803B72D8.
Task framework: 0x8004F014 ctor, 0x8004EF18 dtor, 0x800461A0 accessor (+0x54 is the MessageTask's parent).
Heap: 0x803A1380 new, 0x803A1334 delete, 0x80049BB8 sound-heap alloc. Static destructor registration 0x803A1210.
Lazy constructors: fn_801FB490 (message manager), fn_8005328C (sound manager, called only while its pointer r13+0x2BB8 is 0).
Glyph draw: fn_8004A09C, translated. The PC "Show subtitles" switch in `system.cpp:2660` recognises the MessageTask draw by the saved LR at r1+0x34 of fn_80049BE8 (0x801FB82C) and its own LR 0x80049C98; the binding writes those so the existing hook keeps working, and it was exercised (section 7).
Boost control blocks: 0x8000945C last release, weak destroy via vtable+0x10.
Rand 0x8040FAE0. Mission / producer callers: unchanged.

Out of scope, not reconstructed: TObjBGM (0x805E9BC0, playback, fades, `sng_EV*` streams), SE manager, cutscene streaming, the SERIF destructor 0x801D6EA4 and creation fn_801D844C (the latter also loads the voice archive).

Left translated in the message system: fn_801FBAF4's reallocation path (fails closed, section 5), fn_801FB490 and the vector reserve, the 0x801FAE88..0x801FAEE4 iterators and 0x801FB0A0/0x801FB0C0 push wrappers (no native users are left, but still in the link; not pruned, verify mode needs the bodies), glyph layout.

## 5. Fail-closed paths

`dialogue_voice::UnportedPath` (a thrown value the phase-2 binding turns into `ExecutionFault`):
- `queue_push` at `count >= capacity` (the growth branch 0x801FBC88 of fn_801FBAF4). Enqueue resets first, so only random enqueues with no updates running could reach it.
- The no-draw update path with a null record (the original reads address 0x10).

## 6. Float rules

`limit` is computed `float(int) / 60.0f`. `elapsed` is `float(double(elapsed) + dt)` with dt as the caller passed it in the FPR (a double holding a float value): the fadds of two floats, rounded once. The reset stores 0.0f bit pattern 0. The elapsed test is `>=`, false on NaN, as fcmpo + cror eq,gt,eq.

## 7. Phase 2 results

Wiring: `runtime/src/gekko/native_message_voice_owner.cpp` (`SHADOW_NATIVE_MESSAGE` = off (default), `verify`, `owner`), HLE entries installed after every other entry hook, before the voice trace. Owner mode refuses to start while erratum E4's hooks are installed (they are not ported; SHADOW_ERRATA_E4=0).

The binding of each translated-out-of-scope call uses guest_call with the original's return address and rebuilds the return-address chain on the guest stack, so the voice trace and the Show subtitles hook still see the original call shape. Trace lines the native code makes without going through a function entry come from `voice_trace::note_voice_stop/note_voice_request/note_queue_pop`.

Verify mode runs the translated body, recording every boundary call and its result, then replays the native code over a snapshot and compares final bytes, the boundary sequence and the result. A boundary's effects inside the snapshot are captured (Task header after its ctor/dtor, manager after its ctor); Task tree links are excluded (the task framework's).

Unit tests: `native_message_voice_tests` (65,984 checks), `message_voice_hold_tests` (125): 0 failed.

Differential on real execution, E4 off, headless, voice English unless marked, total of 6 runs (Devil Doom tape, Westopolis 720p tape en+ja, Final Haunt, Lethal Highway, Westopolis CPU spike): **0 differences**. Checkpoints in all those runs matched. Per-function totals (`calls` = reached from translated code, `nested` = reached from translated bodies inside a checked call): manager 2098 / 283,269; find_record 2 / 16,091; record_text 0 / 55,358; draw_message 18,872 / 36,486; reset 14 / 147; enqueue 174 / 0; enqueue_random 77 / 0; queue_erase 0 / 4,253; is_finished 1,732 / 0; reinit 15 / 0; task_ctor 0 / 15; task_dtor 9 / 0; task_update 94,806 / 0; choose_slot 0 / 249; play 0 / 249; request_play 87 / 162; stop_by_id 0 / 323; request_stop 0 / 323; stop_all 0 / 13; request_stop_all 13 / 0; pause_all 0 / 13; request_pause_all 13 / 0; resume_all 0 / 43; request_resume_all 43 / 0; serif_reset 0 / 12; serif_ctor 12 / 0; serif_update 194,500 / 0; allocate_work_areas 6 / 0. Bytes compared: about 317 million. Each row is nonzero: every owned function ran.

Owner mode, E4 off, against the baselines: see the report. Traces differ on no line (minus the wall-clock column) and checkpoints all match, for Devil Doom en and ja against `e4_r2/dd_*_e40`, Westopolis, Final Haunt, Lethal Highway, Westopolis CPU spike, pause then restart, pause then resume and the event1001 cutscene against same-exe off-mode controls. Show subtitles off in owner mode: 8,737 of 8,737 glyph draws skipped, trace and checkpoints unchanged.

What was not proved: headless stage restart (not exercised beyond the pause-menu restart tape), Japanese on tapes other than Devil Doom and Westopolis in verify mode, the bodies are still in the link (no pruning or ratchet change): verify mode needs them.

E4 is not ported onto the native code (user decision 2026-11-10: no held requests, no pinned timer).

## 8. The window lifetime (erratum E4, English voice only)

See `docs/GAME_ERRATA.md` E4 for the rule. Added to `task_update` (native_message_system.cpp) as `WindowLifetime`:
- Linger begins at the expiry (the `expired` test) when the manager's voice is really playing. `linger_tick` runs on the original idle exits and on the gate.
- The gate sits at the `open` decision and is taken only when `current == -1`. The two other reasons to open (`queued_priority > current_priority` and equal priority of another group) can only be true with a window showing and stay untouched; the drop of a lower-priority part is only reached with `current != -1`.
- With `window_lifetime()` null (Japanese, switch off, verify mode) the path is the original one, proven in section 7.

### 8.0 Held requests in a voice's overrun (user decision 2026-11-10, "Finish, then Eggman")

`message_system::enqueue` (fn_801FB210 path only): after the original's lock and record tests, if the state shows a voice in overrun (linger active or gate holding, and it is really playing), a request of priority not strictly above the line owning the voice (`WindowLifetime::voice_priority`, remembered when the update starts the voice) is held instead of resetting. The call returns the accepted value. One held at a time; a newer request of priority >= the held one replaces it, a lower one returns -1. Release runs in `task_update` before the open decision, in the update where the voice is seen to have ended (or the failsafe): the lock word is saved and treated as free, the priority word idle, and the real `enqueue` runs with the guard bypassed; a refusal is counted as such. Every reset (including a strictly higher request) drops it. Japanese, the switch off and verify mode: `window_lifetime()` is null and the original path runs.

### 8.1 Validation (2026-11-10, final executable)

Unit: native_message_voice_tests 67,921 checks, 0 failed (linger and its ends, manager words identical through a linger, gate wait and open with the original timer value, gate priority word, gate reaching its own limit while waiting, voiceless part, interruption path not gated, lower-priority enqueue during a linger, Japanese identical, failsafe).

Runs, headless, same setups: (a) E4 off owner: trace minus wall clock identical to e4_r2 (Devil Doom en, ja) and to the off-mode controls (Westopolis, Final Haunt, Lethal Highway, Westopolis CPU spike, pause-restart, pause-resume, event1001), checkpoints equal. Verify mode on Devil Doom: 0 differences. (b) Japanese E4 on vs off identical on 5 tapes, 0 lingers, 0 gates. (c) English on vs off on all 8 setups: every checkpoint equal, every msg_enqueue at the same frame and order, accepted enqueues (those that reset the manager) identical per frame, every win_close identical in frame and identity (40/24/41/18 lines, win_close by timer included). Voice 3271 at frame 3142 and the cut of 3290 there are as at E4 off; all cuts in the on-run are at the off-run's frames; the only cuts removed are the three gated next-part cuts (below). Subtitles closed with their voice still playing: 0 on every tape (19/12/11/10/11 at E4 off). Failsafe hits 0. Finished-poll end frames equal on Westopolis, Lethal Highway, CPU spike. Show subtitles off: 9,346 of 9,346 draws skipped on the English E4-on run, 8,737 on the Japanese, trace unchanged.

Gates (Devil Doom only): frames 1166 (414 part 2, its voice starts 1 frame later), 2999 (416 part 2, 12 frames later), 3988 (417 part 1, 5 frames later). The window state and all admissions keep the original frames (timer carried through the gate); only those three voices start later, by the gate. Gate expiry while waiting: 0 on every tape, so that path (and the following part it can delay, whose timer then starts at its own gate) is unit-tested but not exercised in a run.

Next-part stops that remain (Westopolis 4, Lethal Highway 1, identical at E4 off): a different group at equal priority replacing a still-showing window, the original interruption path.

### 8.2 Validation of the held requests (2026-11-10, final executable nh.exe = .build/watch)

Unit: native_message_voice_tests 68,012 checks, 0 failed. E4 off and Japanese E4 on/off identical to the existing controls on every tape; verify mode 0 differences; checkpoints equal on every tape.

Holds occur only in Devil Doom (5 requests held: 414, 430, 430, 430, 430; 3 replaced by 426, 427, 426; 1 refused: 421 at frame 693; 5 released, 0 refused at release, 0 dropped). Voice 3260 plays its full length (requested 51, natural end 699, 10.8 s); line 414 opens at 700 (E4 off: 592).

Cost in Devil Doom (all from the rule, not fixed): lines moved: 414-0 and 414-1 +108 frames, 416-2 +12 and 417-1 +5 (gates), 430 at 3143 +30, 426 at 9200 and 10792 +14, 427 at 10019 +74. Lines that do not open: 414-2 at 1167 (voice 3284, dropped when 415's accept at 1253 resets the queue; the original played it as a 1.35 s part cut by 415 itself) and 430 at 9139 and 10731 (replaced by 426 while held). Voice plays lost: 3284 once, 3271 three times. Voices newly cut: 3283 at 1253 (by 415, table window of part 414-1 still open), not cut at E4 off where next-part cut it at 1166. No other tape differs.
