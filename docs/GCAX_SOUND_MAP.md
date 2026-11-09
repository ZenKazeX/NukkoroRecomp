# GCAX ("AX SNDLIB") static map: sound request to AX voice reading ARAM (GUPP8P main.dol)

## Confirmed 2026-10-26: the "pause sound out of nowhere" in Lethal Highway

User recording .build/recordings/lethal-highway-sfx (diagnostic build, --boot-stage stg0202, 284.28 s, input tape,
no Start press in the whole run: Start is control bit 0x0400, the tape holds only A/B/X). Headless replay with
GEKKO_AUDIO_AUDIT reproduces the run (same game time and frame-limiter counts). Every voice was named from the bank
files (ARAM placement from the aram-dma records: B0_SYSTEM wave data at 0x4500, see section 1): 153 bank-0 voices,
of them 120 GETRING, 8 CHECKPOINT, 8 DASHPANEL, 4 LOSTRINGS, 7 COUNTEND + 1 RANKING (result screen) and
**5 SE_SY00_WINOPEN** (the menu window-open sound) at 2.9 s (stage-start mission window) and 53.1, 85.8, 211.5,
215.0 s - with no menu open.

`--watch 0x801D5F14` (PlaySE, observe-only; PCM byte-identical with and without it) shows each of the four stray
plays is the game's own request for **0x7160 SE_Z1090_WARPHOLE** from fn_803214B0 (back chain 0x80322F0C,
0x8004ECB0, 0x801EB69C), always together with 0x714D SE_Z1077_BKTANK1_LP: a Black Arms tank warping in.
AudioDataStageCity.bin row 90 (`0x7160 SE_Z1090_WARPHOLE`) is **unfilled** in the shipped PAL data: bank 0,
program 0, every other field zero except the default duration 0x7B. GCAX resolves it exactly as the data says, to
B0 program 0 = SE_SY00_WINOPEN. The translated code is faithful; the same data and code play the same sound on the
console. It is the only unfilled row in all 23 tables (LastEggBoss row 83 is a deliberate empty "none", id 0).

CORRECTION (same day): the user checked the original in Dolphin (HLE audio, only the 16:9 Gecko code enabled): every
tank warp-in in Lethal Highway is SILENT there, at the stage start and later. So the claim above that "the same data
and code play the same sound on the console" was wrong: the original resolves this request to silence, and the port
played an audible sound where the original plays none. Why is NOT known yet (open question below).

An earlier reading of the authored intent - City rows follow row i -> bank 7 program i, and B71_CITY program 90 (one
ADPCM wave, 1.51 s) is the only program no row references - was used for a first correction and then withdrawn: it
gave the request a sound the original never plays. The user then chose silence.

### Resolved (Dolphin reference trace, 2026-10-26): the original DOES play the menu sound

tools/dolphin_dump.py read the user's Dolphin (retail PAL ISO, main.dol SHA-256 fde4fa6f... = the port's, City table
and B0_SYSTEM byte-identical) from outside the process. Dolphin ran from a private user folder
(.build/validation/sfx/dolphin-user: DSP HLE, audio dump on) with two Gecko codes reproducing --boot-stage stg0202
(C2 at 0x802B6710: the hand-off writes and phase 0; nops at 0x8030672C/0x80306738: the PAL 60 Hz screen confirms its
preselect). The original put 0x7160 on SE channel 2, exactly as the port. Snapshots of the GCAX context and the AX
parameter blocks (680 over 2.5 s): AX voice 60 runs B0 wave 0 (WINOPEN, end nibble 0xE594) from the warp-in, envelope
0x7FFF, gains 8607/3047 then 10688/7567 - identical to the port's parameter block. Dolphin's own DSP output dump,
aligned to the port's stage-start audio: the port's WINOPEN contribution is present at 0.958 of its volume (the same
measure with the template misaligned by 300..6000 samples: -0.22..+0.02). So the original plays the menu window-open
sound at every Black Arms tank warp-in - an original-game defect, as first concluded; the user's recollection of
silence did not hold up against the dump. User decision after hearing the clips: keep the warp-ins silent as a
documented deliberate fix of this original-game bug (Phase 1 below). Clips: .build/validation/sfx/dolphin_original_stage_start.wav (original),
port_prefix_stage_start.wav, port_silent_stage_start.wav. Evidence: .build/validation/sfx/dolphin-ref-1.

The section below records the reasoning before this trace.

### (Superseded) open question: why the original is silent for 0x7160

What is known in the port (pre-fix diagnostic build, .build/validation/sfx/prefix-voice): the tank's code calls
PlaySE(0x7160) at the stage start (2.8332 s game time); the lookup returns row 90 (bank 0, program 0); the SE start
caches it; the sound object's note-on 0x801F297C receives bank 0, program 0 at 2.8499 s; nothing stops the channel;
the voice runs 0.93 s at envelope 0x7FFF with positional gains 8607/3047, i.e. audible. Candidates for the original's
silence, none verified: the DSP/AX treatment of some parameter-block field the port's mixer does not implement (the
mixer ignores envelope and gain ramps, D07-F5), a GCAX path that differs under the port's timing (MIDI ring drain,
voice reuse), or the bank-0 wave data at that moment on the console. Settling it needs a reference trace of the AX
parameter blocks at that moment from the original (Dolphin), compared with the port's audit rows. Until then the row
is disabled (below), which matches what the original lets the player hear but not necessarily its internal state (the
original allocates an SE channel; the port now refuses the request).

## Native replacement (user decision 2026-10-26: phased, each phase checked against the original code)

### Phase 1 - done: native sound-table lookup and data errata

runtime/include/gekko/native_sound_tables.h, runtime/src/gekko/native_sound_tables.cpp. The lookup 0x801D88CC
(PlaySE's id resolution) is a native owner (extract/native-owners.txt); its helper 0x801D8838 lost its only caller and
is pruned. DELIBERATE DEVIATION (user decision 2026-10-26, after the Dolphin trace above showed the original plays the
menu sound): the erratum disables the WARPHOLE row (program -1), fixing an original-game bug - the tanks warp in
silently instead of playing the pause-menu window-open sound. It matches the whole shipped row (id, bank, program,
+0xA..+0xF, live count and limit) and the table's row count, and changes only the program byte. (A first version set
bank 7 / program 90; withdrawn by the user.) Removing kErrata's row restores the original behaviour exactly.
- sound_table_contract_tests: 17,772 lookups (16,757 found) identical to 5,695,756 original instructions over the 23
  shipped tables, every id nibble, ids above 0xFFFF, disabled rows, instance limits driven past their maximum, an empty
  and a negative row count; exactly one erratum, which changes exactly byte +9 of that row and turns that one lookup
  into a refusal.
- Lethal Highway tape (headless replay, diagnostic build): the five warp-in requests are refused (report: 5
  disabled); no WINOPEN and no other sound plays for them; bank-0 voices 153 -> 148, every other bank's voice count
  unchanged; game time and frame-limiter counts identical. Shipping candidate's PCM equals the diagnostic run's.
  Evidence .build/validation/sfx/replay-silent, gate2/ (replay-1 = before; replay-fixed = the withdrawn program-90
  version).
- The rows stay in the buffer the game loaded (SCAFFOLDING for the still-translated SE manager; phase 2 deletes it).

### Phase 2 - design (2026-10-26, from the SE-manager map docs/SE_MANAGER_MAP.md)

Native code cannot call translated code synchronously (a translated callee may leave to the dispatcher and unwind
every native frame), so the native manager calls nothing translated. Its MIDI output is written straight into GCAX's
port rings, which is all the sound object's methods do for ports 0-2:
- SetBankProgram (vt+0x50): `B0|ch 20 bank` then `C0|ch prog`, prio 0, no port check. NoteOn (vt+0x24): `90|ch 30 7F`,
  prio byte 15/2/1 for priority 3/2/other. NoteOff (vt+0x30): `80|ch 30 00`. Volume (vt+0x38): CC7 =
  clamp(100 + (v > 0 ? trunc(0.2126f * v) : v), 1, 127). Pan (vt+0x3C): CC10 = clamp(trunc(fmadds(0.5, p, 64)), 0,
  127). CC62 (vt+0x44): `B0|ch 3E v`. Bend (vt+0x4C): clamp [-0x4000, 0x3FFF], +0x4000, d1 = v & 0x7F, d2 = v >> 8 (an
  original quirk, not >> 7). ResetPan (vt+0x48): CC62 0 then Pan 0 in surround mode (cfg 0x8057E760+0x78 == 2), else
  Pan 0. Ports >= 3 (sequencer) are reached only by the unused special ring: fail closed (counted, not sent).
- gcaxMidiMsg/gcaxSend pack `status<<24 | (d1&0xFF)<<16 | (d2&0xFF)<<8 | prio` (d2 = 0 for C0/F0) after checking the
  GCAX context and that the handle is an open port (ctx+0x5478+0x1530*i); gcaxPortPush 0x8043F054 stores it at
  ring[wr], count++, wr = (wr+1) mod 0x400, or drops it when count >= 0x400.
Boundary: native = the request wrappers, PlaySE, allocator, start, the per-frame channel engine 0x801D6270 with
UpdateChan and the positional math, the gates, and the internals the translated lifecycle code shares (ForceStop,
InitChan, Route). Translated until phase 3 = StopAllSE, SetSoundMode, Pause/Resume and the stage/lifecycle
functions (they need GCAX internals - StopAll runs a GCAX frame, ResetAll a GCAX reset - or the CRI music code); they
keep working on the same channel memory, which stays in guest memory until then (SCAFFOLDING).
Arithmetic goes through the port's own PowerPC FP primitives (gekko/ppc_ops.h) so results are bit-identical to the
translated code. Observation: gcaxPortPush becomes a native owner first, with a trace of every pushed message (one
ordered stream from every caller); the translated manager's stream is the baseline the native one must equal
message for message, then PCM, state and the Westopolis/Lethal Highway replays.

### Phase 2 - done 2026-10-26: native SE manager (increment 26)
Native owners: gcaxPortPush 0x8043F054 (gekko/native_gcax_midi.h), the request wrappers PlaySimple 0x801D59AC,
PlayFlags 0x801D5894, Play3D 0x801D5764, PlayRing 0x801D56A0, the request gates 0x801D5190/51C0/51F0/5218 and FrameSE
0x801D6270 (gekko/native_se_manager.h), with the positional math in gekko/native_se_math.h. Pruned from the image:
PlaySE, the allocator, FindOwned, RowFlags, Start, the per-frame counters, UpdateChan, SendPan, the request helpers,
ListenerDistance, PosVolume, PosPan, DopplerBend, MaxDist and their MSL math (31,653 instructions in 597 rows pruned in
all). Still translated, sharing the channels in guest memory (SCAFFOLDING until phase 3): StopAllSE, SetSoundMode,
Pause/Resume, the stage/lifecycle functions, ForceStop, InitChan, Route and UpdateFrame (which calls FrameSE). The
special ring (channels 0x27-0x2A -> GCAX ports 4-7) is not native: no shipped table row selects it (checked by
se_manager_contract_tests over all 23 tables), and a message for such a port would be counted, not sent.
Proof: se_math_contract_tests (205,414 calls, bit-exact incl. FPSCR; found the Doppler "> 800" test is on the
previous distance), gcax_midi_contract_tests, se_manager_contract_tests (every call compared in r3 and all 24 MiB of
MEM1 on a Dolphin image of the original in Lethal Highway, test_data/se-manager/), and both replays: the native push
stream equals the translated manager's message for message (Lethal Highway 115,365, Westopolis 37,121), PCM
byte-identical, and the only guest-memory difference is 20 bytes of dead stack below the stack pointer.
Still to do in later phases: the tables in host memory (phase 3/4 parse AudioData*.bin), the channels in host
memory, and native voices instead of the MIDI rings.

### Phase 3 - native voice engine (planned)
Replace GCAX note-on 0x8043C9A4, voice start 0x8043DACC, envelope 0x8043CEBC, mix 0x8043D2F4, pitch 0x8043D1C0, the
reaper 0x8043C64C and voice stealing (0x8043CC18/0x8043CD2C) with native voices: program -> layers -> splits -> waves
resolved from native bank data, ADPCM/PCM decoded from host memory, envelope/pitch/pan computed natively and handed
to the native mixer as VoiceFrames. Deletes: the MIDI ring per port (+0x524) and its drain 0x8043ED64, the 48 AX
voices GCAX holds, every AXVPB address/ADPCM write for sound effects, gvoice/note-object tables.

### Phase 4 - native bank store (planned)
Replace the bank load path (0x801F2CE4 -> gcaxLoadMLT 0x8044075C -> gcaxLoadMPB 0x8043C0AC) with native loading of the
.mlt wave and program units into host memory (no ARAM, no ARQ DMA, no hard-coded slot sizes), then retire
gcaxNewAramBank 0x8043C1FC, the 8 MiB ARAlloc 0x8043F7A4, the zero block and the GCAX context (0x14470 bytes). CRI's
ADX music/streaming voices are a separate owner and stay out of this plan.

Evidence: **P** = proven from instructions, **S** = strongly supported, **U** = unknown.
r13 = 0x805EC500 (`__init_registers` 0x80003334). GCAX context pointer `ctx = *(r13+0x5FAC)` = *0x805F24AC (P).
`*(r13+0x5FA8)` = raw buffer, `*(r13+0x5FB0)` = 1 if the caller supplied the buffer (P). GCAX code spans 0x8043B9F0-0x80441460.

## 0. Call graph (P)

```
game ctor 0x801F2E58: ARInit(0x8053F2B0,4); ARQInit; AIInit; AXInit 0x8038465C; MIXInit 0x8038A1F4;
   AXRegisterCallback(0x8043F3E4); buf = alloc(0xE1000); gcaxInit 0x80440B74 -> 0x8043F744(buf, 0x30, 0xE1000, 0x800000)
   for port i<8: gcaxOpenPort 0x804406FC(obj+8+4*i)   (port handle = &ctx->port[i])
AX callback 0x8043F3E4 -> frame 0x8043F4E4 (skipped if ctx+8 lock != 1) -> per open port:
   0x80440FAC seq tick, 0x8043ED64 drain MIDI ring, 0x8043E9EC/0x8043E8C0/0x8043EB00 ramps,
   0x8043EA5C per-voice update (env 0x8043CEBC, mix 0x8043D2F4, pitch 0x8043D1C0), 0x8043C64C reap finished notes
   then MIXUpdateSettings 0x8038B1F8
```

## 1. ARAM bank slots

| Item | Function / field | Evidence |
|---|---|---|
| One ARAM block for all of GCAX: `ARAlloc(0x800000)` | 0x8043F7A4; size arg r6 = 0x800000 from 0x80440B8C; stored ctx+0x52A4 | P |
| Raw base | ctx+0x52B0 = ARAlloc result. ARInit sets the AR stack pointer to 0x4000 (0x803829C4), so the base is 0x4000 **if GCAX is the first ARAlloc client**. Other ARAlloc callers exist: 0x8032E200, 0x803D069C (CRI side, also calls AXAcquireVoice at 0x803CFAB0), 0x804225B0 | P (value at runtime U) |
| Bump pointer / remaining | ctx+0x52AC = align32(base); ctx+0x52A8 = 0x800000 - align slack | P |
| Zero (silence) block | 0x500 bytes at ctx+0x52AC are zeroed and DMA'd from main RAM `align32(ctx+0x13A6B)` (0x8043F7F8..0x8043F818); ARAM address in ctx+0x52B4; then bump +0x500 | P |
| Slot sizes are **hard-coded**: 0x8043F830.. writes one table, then `0x8043C2AC` overwrites ctx+0x52B8[16] | sizes below | P |
| GCAX.conf is **not parsed**: no "AramBank"/"Reverb" keyword strings in the DOL (the only "AramBank" bytes are inside the message "gcaxNewAramBank(%d,%d) failed"). The conf values (893000, 471000...) are ignored | | S |
| Slot allocation `gcaxNewAramBank(bank, size)` 0x8043C1FC: requires 0<=bank<16 and remaining>=align32(size). Then `slot[bank].base = ctx+0x52AC` (ctx+0x52F8+8*bank), `slot[bank].size = align32(size)` (ctx+0x52FC+8*bank); bump and subtract. On failure it calls OSReport and the slot stays 0/0 | called from 0x8043F940 for each nonzero size, i = 0..15 | P |
| Remaining is printed with "aramRest=%d" (0x8043F968) | | P |

Slot layout, assuming base 0x4000 (byte addresses; the nibble range is 2x and is what ADPCM PBs contain):

| bank | base | size | ADPCM nibble range | file(s) / MPBW bytes |
|---|---|---|---|---|
| 0 | 0x004500 | 0x0E1000 | 0x0008A00-0x01CA9FF | B0_SYSTEM 885472 |
| 1 | 0x0E5500 | 0x07D000 | 0x01CAA00-0x02C49FF | B1_COMMON 465088 |
| 2 | 0x162500 | 0x145000 | 0x02C4A00-0x054E9FF | B2_WEAPONS 1207104 |
| 3 | 0x2A7500 | 0x04B000 | 0x054EA00-0x05E49FF | B3_SHADOW_E 207872 (J 255072) |
| 4 | 0x2F2500 | 0x057800 | 0x05E4A00-0x06939FF | B4_SUBHERO_E 296608 |
| 5 | 0x349D00 | 0x023000 | 0x0693A00-0x06D99FF | B5_SUBDARK_E 62208 |
| 6 | 0x36CD00 | 0x0C8000 | 0x06D9A00-0x08699FF | B6_ENEMY 717440 |
| 7 | 0x434D00 | 0x0FA000 | 0x0869A00-0x0A5D9FF | all B7x/B8x stage banks (B71_CITY 927008, largest) |
| 8-10 | +0x200 each | 0x200 | | unused |
| 11-15 | +0x80 each | 0x80 | | unused; end 0x52F580, rest 0x2D4A80 |

**Oversize check (P):** `0x8043C0AC` compares `slot[bank].size (ctx+0x52FC+8b) >= MPBW.size`. If the bank is too large it returns **-2 before any DMA or MPBP install**. The old wave data and old MPBP stay in place, and the game gets error 0x09000101 (load fails). All shipped E/PAL banks fit the hard-coded slots (B3_SHADOW_J 255072 < 307200).

## 2. Bank load path

Game: `LoadBank(obj,name)` 0x801F2CE4 reads the whole .mlt into RAM (0x800450C4). It then calls `gcaxAllocLoadHandle` 0x80440A70 (64 handles of 0x14 bytes at ctx+0x13F6C), `gcaxSetLoad(h, buf, size, cb=-1, arg=0)` 0x804409D0, `gcaxLoadMLT(h)` 0x8044075C and `gcaxFreeHandle(h)` 0x80440914, and frees the file buffer (0x80049B5C). Bank selection 0x801F2A80 (selector 0..7, jump table 0x8053F498) picks the file names. Selector 7 resolves the stage number through the pair list at 0x8053F2C0; stages 100, 202 and 400 all map to `B71_CITY`. Language flag *(0x8057E7A0) picks _J or _E. (P)

MLT layout (P, verified on all files):

| Offset | Content |
|---|---|
| 0x00 | "gcaxMLT " , u32 0, u32 total size |
| 0x10 | "gcaxMLTM", u32 0, u32 0x20 (`count = size>>4` = 2 entries) |
| 0x20 | entry[i] (0x10 each): `type = (w0>>24)&0xF` (1 = MPB, 4 = MSB), `bank = (w1>>24)&0xF` (byte 0x24 of the file), `off = w2` relative to file+0x20; an entry with w1 == 0xFFFFFFFF is skipped |
| 0x40 | "gcaxMPB " unit (header not checked) |
| 0x50 | "gcaxMPBW", size W, then W bytes of **wave data -> ARAM** |
| 0x60+W | "gcaxMPBP", size P, then P bytes of **program data -> main RAM (GCAX heap)** |

`gcaxLoadMPB(bank, unit, cb, arg, async)` 0x8043C0AC (P):
1. It checks the "gcaxMPBW" magic and the slot size (see above).
2. It DMAs the wave data with `gcaxAramDma` 0x8043BACC(src = MPBW+0x10, dst = slot[bank].base, len = W, cb, arg, async). This calls `ARQPostRequest(&ctx->arq[i] (ctx+0x485C+0x20*i), owner = ctx+0x509C (=0xF), type 0 MRAM->ARAM, prio 1, src, dst, len, 0x8043BC9C)`. The busy flag is ctx+0x505C[i] (64 of them), with user cb/arg at ctx+0x50A0[i] and ctx+0x51A0[i]. If no request slot is free the result is OSReport("Panic: no more...") and a hang. The game passes cb = -1, which forces cb = 0 and async = 0, so the load spin-waits on the busy byte until the ARQ callback 0x8043BC9C clears it: **loads are synchronous**.
3. It checks the "gcaxMPBP" magic, frees the old copy at ctx+0x5378+8b with `gcaxFree` 0x8043BD38, then allocates P bytes with `gcaxMalloc` 0x8043BDE4. It stores the pointer and size at ctx+0x5378/+0x537C+8b and does a memcpy. The MPBP copy is **installed after the DMA** for synchronous loads.
4. MSB (sequence) units go through 0x8043BFB4 into ctx+0x53F8/+0x53FC+8b. No shipped .mlt has an MSB.

Address rebasing (P): the MPBP holds **bank-relative** byte offsets. Nothing is rebased at load time. The absolute ARAM address is built at every note-on from `slot[bank].base` (see Â§4).

MPBP body (`B` = pointer stored at ctx+0x5378+8b) (P, cross-checked against B0 and B71):

| Field | Meaning |
|---|---|
| B+0 | `(nWaves<<24) | waveTblOff` (wave entries 0x50 bytes) |
| B+4 | `(nVelCurves<<24) | velCurveOff` (128-byte curves) |
| B+8 | `(nPrograms<<24) | progTblOff`; program[p] u32 = `(nLayers<<24) | layerOff` |
| layer (0x10) | w0 = `(nSplits<<24) | splitOff`; +4 bend-up, +5 bend-down |
| split (0x40) | +0 wave index, +5 velocity-curve index, +6/+7 vel clamp, +8/+9 key lo/hi, +0xA pan, +0xB (&0x1F, table), +0xC root key, +0xD fine, +0xE/+0xF, +0x10..+0x15 envelope rates/scale, +0x16 attenuation |
| wave (0x50) | DSPADPCM header: +0 num_samples, +4 num_nibbles, +8 sample rate, +0xC u16 loop flag, +0x10 loop start, +0x14 end, +0x18 current (sa/ea/ca, **nibbles for ADPCM**, samples for PCM), +0x1C coef[16], +0x3C gain, ps, yn1, yn2, +0x44 loop ps/yn1/yn2, **+0x4A u8 format (0 ADPCM, 0x0A PCM16, 0x19 PCM8)**, **+0x4C u32 byte offset of the wave inside the bank** |

## 3. Sound-effect request API

### Game side (P)

- **Entry point:** `PlaySE(mgr, id, prio, pos)` 0x801D5F14 (called from 0x801D56A0, 0x801D5764, 0x801D5894, 0x801D59AC).
  - Pause gate: if mgr+0x14 is set, only ids 0xE000-0xE009 (list at 0x8053DF08) may play.
  - Table slot = `(id>>12)&0xF` through jump table 0x8053E188: 0xE->slot0 System, 1 Gadget, 2 Weapon, 3 Player, 4 PlayerHero, 5 PlayerDark, 6 Enemy, 7/8/9->slot7 Stage, everything else is rejected.
  - Lookup 0x801D88CC: linear search of `entries` for `entry+4 == id&0xFFFF`. It rejects an entry when `(s8)program < 0`, and it applies the per-entry instance limit at entry+0x10/+0x14.
- **Table manager:** 8 slots of 0x10 bytes (count, entries, file buffer, parser). Loader 0x801D8AFC(mgr, path, slot). Selector 0x801D8A20(mgr, key, slot): slots 0..6 take the fixed files at 0x804D1810+, and slot 7 looks up the stage key in the pair list 0x8053E028. Keys 5, 8 and 12 map to AudioDataStageCity (Westopolis, Lethal Highway, Central City). It is called from 0x801D6808 and 0x801D6A1C.
- **AudioData*.bin format:**
  - Header: 0x20 bytes, then +0x20 = 8 and +0x24 = count; entries start at 0x28.
  - Entry (0x18 bytes): +0 name offset (relative to 0x20), +4 id, **+8 GCAX bank**, **+9 program** (s8; negative = disabled), +0xA s16 pitch/vol offset, +0xC, +0xD (duration, default 0x78), +0xE flags (bit1 = "special pool"; no shipped entry sets it), +0xF, +0x10 live-instance count, +0x14 limit.
- **SE channel pool:** 43 channel objects of 0x34 bytes at mgr+4. The channel byte at +0 equals the index (init 0x801D64FC/0x801D9C28).
  - Decode (0x801D8CA0/0x801D8D40): byte <= 0x26 gives MIDI `port = byte>>4`, `ch = byte&0xF`; byte > 0x26 gives sequence port `byte-0x23`.
  - Allocator 0x801D5A18: normal entries use channels 0x00-0x1F (ring mgr+0xC); "special" entries use 0x26-0x2A (ring mgr+0xD).
  - Start 0x801D9B2C caches `bank=entry+8 -> chan+1`, `program=entry+9 -> chan+2`, `prio -> chan+3`, `id -> chan+0x12`, `entry -> chan+0x30`.
- **Per-frame channel state machine** 0x801D8D2C. On start (chan+0x14 bit0) it sends:
  - `SetBankProgram` (vtable+0x50 = 0x801F2270: MIDI `B0|ch 20 bank`, then `C0|ch program`). **This is sent only if chan byte < 0x26** (0x801D8D8C).
  - Volume CC7 (vtable+0x38 0x801F27B4), pan CC10, CC62/63 (positional), pitch bend.
  - `NoteOn` (vtable+0x24 = 0x801F297C): for port < 3 this is MIDI `90|ch 0x30 0x7F`, with the message priority byte = 15/2/1 from chan+3; for port >= 3 it calls `gcaxStartSeq` 0x80440040.
  - Stop: 0x801D8C8C / 0x801D930C send NoteOff (vtable+0x30, `80|ch 30 00`) and vtable+0x48.
- **Sound object vtable** 0x8053F42C:

| Offset | Function |
|---|---|
| +0x18 | LoadBank 0x801F2CE4 |
| +0x1C | SelectBank 0x801F2A80 |
| +0x20 | 0x801F2A2C (effect off) |
| +0x24 | NoteOn 0x801F297C |
| +0x30 | NoteOff 0x801F28F8 |
| +0x34 | StopAll 0x801F28A4 |
| +0x38 | Volume 0x801F27B4 |
| +0x3C | Pan 0x801F26D4 |
| +0x40 | CC63 0x801F2610 |
| +0x44 | CC62 0x801F25A8 |
| +0x48 | positional 0x801F2508 |
| +0x4C | Bend 0x801F245C |
| +0x50 | SetBankProgram 0x801F2270 |
| +0x54 | ResetAllControllers (CC121 to all 8x16) 0x801F2304 |
| +0x58 | SoundMode 0x801F21AC |
| +0x5C / +0x64 | port pause / resume (ports >= 3 only) |

### GCAX side (P)

- `gcaxMidiMsg(port, buf, status, d1[, d2])` 0x8044046C (varargs through `__va_arg` 0x803A1100) packs 3 bytes. `gcaxSend(port, buf, prio)` 0x804403AC builds `msg = status<<24|d1<<16|d2<<8|prio` and calls `gcaxPortPush` 0x8043F054.
- **MIDI ring per port:**

| Field | Meaning |
|---|---|
| port+0x524 | u32[0x400] ring |
| port+0x1524 | read index |
| port+0x1528 | write index |
| port+0x152C | count |

  The push is guarded by OSDisableInterrupts. **When count >= 0x400 the message is dropped silently.**
- Drain 0x8043ED64 (inside the AX callback):

| Status | Action |
|---|---|
| 0x80 | note off |
| 0x90 | vel 0 = note off; otherwise **note-on `0x8043C9A4(port, ch, note, vel, prio)`** |
| 0xB0 | CC 0x8043EB58 |
| 0xC0 | `ch.program (port+0xB0+0x48*ch) = d1&0x7F` |
| 0xE0 | bend 0x8043EB34 |

- CC handler 0x8043EB58:

| CC | Action |
|---|---|
| CC0 **and** CC32 | `ch.bank (port+0xAC+0x48*ch) = v&0x7F` |
| CC7 | +0xB4 |
| CC10 | +0xBC (+0xC8 = -1) |
| CC11 | +0xB8 |
| CC62 | +0xC4/+0xC8 |
| CC63 | +0xC4 |
| CC64 | +0xA4 bit0 |
| CC121 | resets the channel **but does NOT reset bank (+0xAC) or program (+0xB0)** |

- Port open 0x8043F138 zeroes the whole port, so every channel starts at **bank 0 / program 0** (P).

## 4. Voice allocation and AXVPB setup

- **AX calls** (S, identified by body and use):

| Address | Function |
|---|---|
| 0x80384A54 | AXAcquireVoice(prio 0x1B, dropCb 0x8043E74C, gvoice) |
| 0x80387154 | AXSetVoiceState (PB state at vpb+0x146; pb at vpb+0x138; P) |
| 0x80387404 | AXSetVoiceAddr (copies 16 bytes to vpb+0x1A6, sets sync 0x21000; P) |
| 0x80387090 | SetVoiceSrcType |
| 0x803875B8 | SetVoiceSrc |
| 0x80387514 | SetVoiceAdpcm |
| 0x803876E8 | SetVoiceAdpcmLoop |
| 0x803871B0 | SetVoiceMix |
| 0x80387650 | SetVoiceSrcRatio |
| 0x8038A2C8 | MIXInitChannel(vpb,0,0,-60,-60,64,0,0) |
| 0x8038B144 | MIXReleaseChannel |
| 0x80385784 | AXSetMode |
| 0x80385050 / 0x8038505C | AXRegisterAux A/B |

  GCAX never calls AXFreeVoice.
- **Voice pool:** init 0x8043E66C memsets 64 gvoices (ctx+0x54, stride 0x10C) and permanently acquires `min(ctx+0x38 = 0x30, 64) = 48` AX voices. It stores each at gvoice+0x34 and counts them in ctx+0x3C (P).
- **Note-on** 0x8043C9A4 (P):
  1. Reads `bank = ch+0xAC` and `prog = ch+0xB0` **at drain time**.
  2. `n = gcaxCountSplits(bank, prog, note)` 0x8043DA20. It returns 0 if the MPBP for that bank is null or prog >= nPrograms, so **an unloaded bank or a bad program gives silence, not bank 0**.
  3. If `n > 48 - activeVoices` (0x8043E4EC) it steals note objects with `prio <= new` (0x8043CC18 / 0x8043CD2C, round robin from ctx+0x4C). Otherwise it takes a free note object (64 at ctx+0x4354, 0x14 each).
  4. For each split it calls `gcaxGetVoice` 0x8043E528 (round robin ctx+0x50; takes a gvoice with bit31 clear and a non-null vpb, or re-acquires a dropped one with bit30). It sets bit31, clears bit0 and bit1, and sets +0x108 = note object.
  5. It calls `0x8043D8EC(list, bank, prog, note, vel)`, which calls `0x8043DACC` for each matching split.
- **`gcaxVoiceStart` 0x8043DACC(bank, gv, layer, B, split, note, velTbl, vel)** saves its arguments in gv+0xE4..+0xFC (bank, layer, B, split, note, velTbl, vel). The wave is `W = B + (B[0]&0xFFFFFF) + split.wave*0x50`, and `base = *(ctx+0x52F8+8*bank)` (P). Addresses per format:

| Format | loop | end | current | format word | If loopFlag == 0 |
|---|---|---|---|---|---|
| 0 ADPCM (0x8043DF78) | `W.sa + 2*base + 2*W.off` (nibble units) | `W.ea + 2*base + 2*W.off` | `W.ca + 2*base + 2*W.off` | 0 | loop address is still W.sa |
| 0x0A PCM16 (0x8043E0A4) | `(2*W.x + base + W.off) >> 1` (16-bit sample units) | same formula | same formula | 0x0A | loop = zero block ctx+0x52B4 |
| 0x19 PCM8 (0x8043E180) | `W.x + base + W.off` (bytes) | same formula | same formula | 0x19 | loop = zero block |

  For ADPCM, the coefficients, gain, ps, yn1 and yn2 are copied from W+0x1C..0x43 to gv+0x56, and loop ps/yn from W+0x44 to gv+0x7E. The AXPBADDR image is built at gv+0x38 (loopFlag, format, loopHi/Lo +0x3C/3E, endHi/Lo +0x40/42, curHi/Lo +0x44/46), with the source image at gv+0x48. Call order: SetVoiceAddr, SrcType(1), Src, Adpcm, AdpcmLoop, mix 0x8043D2F4, then `SetVoiceState(1)`. For ADPCM the state is set only if W.num_samples >= 3. Finally gv bit1 (started) is set.
- **Bank-0 aliasing check (P):** the ARAM address, the MPBP and the wave entry all come from the same `bank` value. There is **no fallback to slot 0** in GCAX. A bank-0 sound needs `ch.bank == 0` at drain time, or a PB whose addresses never received this update. With base == 0 (an unallocated slot) the addresses would point into the DSP-reserved 0..0x4000 area or the zero block, not into bank 0.
- **Release:**
  - Normal: 0x8043C64C reaps note objects whose voices have stopped (vpb state 0, or envelope state 5 below threshold). It then frees the voices through 0x8043D0D4: SetVoiceState(0), MIXReleaseChannel, clear bit31/bit0/bit1, gv+8 = 0.
  - Steal: 0x8043CBA4.
  - Drop callback 0x8043E74C: stop, MIX release, bit30 set, re-acquire.
  - A reused gvoice keeps its vpb, but every start rewrites the full address set, so **GCAX itself does not leave a stale address** (P).
  - Pause/resume of ports (0x80440CE8 / 0x80440BA8 -> 0x8043C720 / 0x8043C7E4 -> 0x8043CDD0) **restarts voices from the saved raw pointers gv+0xE8..0xF8**. These pointers dangle if the bank's MPBP was reloaded in between. This is reachable only on ports >= 3 (sequence ports, unused) (P; impact U).
- **Latent game off-by-one (P):** channel 0x26 decodes as MIDI port 2 ch 6, but its SetBankProgram is skipped (`cmpwi 0x26; bge` at 0x801D8D8C). A note started on channel 0x26 plays that MIDI channel's default **bank 0 / program 0 = B0 "SE_SY00_WINOPEN"**. It is reachable only by special-pool entries (AudioData flag 0xE bit1), and no shipped table has one.
- **Data facts (P): AudioData entries that deliberately target B0 during gameplay:**

| Table | Entry | Plays |
|---|---|---|
| City | 0x7160 SE_Z1090_WARPHOLE | **B0 prog 0 (SY00_WINOPEN)** |
| Gadget | 0x1020 WARPIN | B0/20 |
| Gadget | 0x1021 WARPOUT | B0/21 |
| Gadget | 0x1023 ENERGYCORE | B0/32 (SY32_REGAIN) |
| Enemy | 0x6079 | B0/121 (no such program, so silent) |
| Jungle | 0x7558-0x755A | B0/88-90 (silent; B0 has 49 programs) |
| FinalBoss | 0x813D BALLOON | B0/29 |
| City | 0x7152 | B1/6 |

## 5. GCAX context layout (ctx = *(0x805F24AC), size 0x14470, cleared by 0x8043FA14)

| Offset | Size | Meaning |
|---|---|---|
| 0x0000 | 4 | initialised (1 after init; 0 on quit 0x8043F674) |
| 0x0004 | 4 | 0x14470 (context size) |
| 0x0008 | 4 | frame lock / nesting counter (frame runs only when == 1) |
| 0x000C | 4 | active gvoice count (0x8043E4EC) |
| 0x0010 | 4 | active note-object count (0x8043CB68) |
| 0x0018/0x001C | 8 | OSGetTime now; 0x20/0x24 previous; 0x28 delta |
| 0x002C | 4f | master attenuation added in mix |
| 0x0038 | 4 | max voices (0x30) |
| 0x003C | 4 | AX voices held |
| 0x0040 | 4 | frame counter |
| 0x0044 | 4 | pause mode (1) |
| 0x004C | 4 | note-object round robin |
| 0x0050 | 4 | gvoice round robin |
| 0x0054 | 64x0x10C | gvoice: +0 flags (b31 used, b30 AX voice dropped, b1 started, b0 restart pending), +4 index, +8, +0xC, +0x10 f gain, +0x14 f pitch, +0x18/1C bend range, +0x20 pan, +0x24, +0x28 atten, +0x2C/30 L/R, **+0x34 AXVPB\***, +0x38 AXPBADDR image, +0x48 SRC image, +0x56 ADPCM image, +0x7E ADPCM-loop image, +0x84 MIX image, +0xA8..+0xD8 envelope (level +0xAC, dB +0xB0, state +0xD8 0..5), +0xDC/+0xE0, **+0xE4 bank, +0xE8 layer\*, +0xEC MPBP\*, +0xF0 split\*, +0xF4 note, +0xF8 velTbl\*, +0xFC vel**, +0x100 vel, +0x104, +0x108 note-object\* |
| 0x4354 | 64x0x14 | note object: +0 used, +1 voice count, +2 ch, +3 note, +4 vel, +8 prio, +0xC port\*, +0x10 voice list |
| 0x4854 | 4 | GCAX heap start ('MF'=0x4D46 free / 'MB'=0x4D42 used, 0x20-byte headers) |
| 0x4858 | 4 | heap size |
| 0x485C | 64x0x20 | ARQRequest |
| 0x505C | 64x1 | ARQ busy flags |
| 0x509C | 4 | ARQ owner (0xF) |
| 0x50A0 | 64x4 | DMA user cb |
| 0x51A0 | 64x4 | DMA user arg |
| 0x52A4 | 4 | ARAM bytes requested (0x800000) |
| 0x52A8 | 4 | ARAM remaining |
| 0x52AC | 4 | ARAM bump |
| 0x52B0 | 4 | ARAlloc base |
| 0x52B4 | 4 | zero-block ARAM address |
| 0x52B8 | 16x4 | slot sizes (hard-coded by 0x8043C2AC) |
| 0x52F8 | 16x8 | **slot {ARAM base, size}** |
| 0x5378 | 16x8 | **MPBP {ptr, size}** |
| 0x53F8 | 16x8 | MSB {ptr, size} |
| 0x5478 | 8x0x1530 | port: +0 b31 open, b30; +4 seq flags; +8..+0x38 fade floats; +0x40..+0x9C sequencer state; +0xA0 note list; **+0xA4 + 0x48\*ch: channel {+0xA4 flags, +0xA8 idx, +0xAC bank, +0xB0 prog, +0xB4 vol, +0xB8 expr, +0xBC pan, +0xC0 bend, +0xC4/+0xC8 surround, +0xD0 bend-0x2000, +0xD4/+0xD8 L/R}**; +0x524 ring[0x400]; +0x1524 rd; +0x1528 wr; +0x152C count |
| 0xFDFA / 0xFDFB | 1/1 | surround flag (0x8043F4CC) / AX mode flag (0x8043F480) |
| 0xFDFC | 4 | note object being freed (temp) |
| 0xFE04 | | AXFX object type 1 (init 0x80388FF8) |
| 0xFF58 | | **AXFX ReverbHi** (0x803884E4; type 2 via 0x8043F304, enabled by game 0x80440290) |
| 0x10138 / 0x101D4 | | AXFX types 3 / 4 |
| 0x1011C-0x10230 | | effect parameters |
| 0x13A38 | 1 | frame-enable flag |
| 0x13A39 / 0x13A3B | 1/1 | callback / skip counters |
| 0x13A44 | 4f | sequence tempo factor |
| ~0x13A80 | 0x500 | zero buffer (main RAM, init only; overlaps load handle 0) |
| 0x13F6C | 64x0x14 | load handle {+0 used, +1 s8 DMA idx, +4 MLT\*, +8 size, +0xC cb, +0x10 arg} |
| 0x1446C | 1 | async-load flag |

## 6. How a bank-0 (menu) sound can reach gameplay, ranked, with probes

| # | Mechanism | Evidence | Probe |
|---|---|---|---|
| A | Designed data: City 0x7160 WARPHOLE->B0/0 WINOPEN; Gadget 0x1020/0x1021/0x1023 -> B0 WARPIN/WARPOUT/REGAIN | P (data) | Log `PlaySE` 0x801D5F14 r4 (id) and the looked-up entry (0x801D5FC4 r3: +8 bank, +9 prog). If menu sounds line up with these ids, this is original behaviour; check against Dolphin. |
| B | MIDI ring desync: push 0x8043F054 vs drain 0x8043ED7C rely on OSDisableInterrupts. If the AX callback runs concurrently with the game thread in the port, count (+0x152C) drifts and the drain reads **stale slots**. Those slots can hold old pause-menu/menu `B0 20 00 / C0 p / 90 30 7F` triples, which replay as menu sounds. | U (depends on host threading) | At 0x8043F054 and 0x8043ED7C assert `count == (wr - rd) & 0x3FF`. Log the thread id of both sides. |
| C | Native AX/mixer: on voice (re)start the PB addresses are not refreshed (lost sync 0x21000, or the writeback 0x80386474 overwrites the user's address), so a reused vpb resumes its old sample (often a menu sound from the menu). | U | At 0x8043E03C (ADPCM SetVoiceAddr) record gv+0x3C..+0x47 and bank. In the native mixer, on state 0->1, compare the DSP PB loop/end/current with that record, and flag any current address inside [0x8A00, 0x1CAA00) (bank-0 nibble range) when bank != 0. |
| D | Channel bank still 0: a note-on on a MIDI channel that never received CC0/CC32. Port open zeroes the bank and CC121 keeps it. The game always sends CC32+PC before NoteOn except on channel 0x26 (latent). | P (code), unlikely in data | At 0x8043C9A4 log port, ch, `*(r3+0xAC+0x48*r4)`, `*(r3+0xB0+0x48*r4)`. Also log the CC store at 0x8043EBE8 (r4 value). |
| E | ARAM overwrite / overlap by other ARAM clients (0x8032E200, 0x803D069C, 0x804225B0 ARAlloc; direct ARStartDMA 0x8039FA0C/0x8039FBF8) or a different ARAlloc order | U; would corrupt rather than replay | Read ctx+0x52B0 and ctx+0x52F8.. after init, and log every ARQ/ARStartDMA destination range. |
| F | Bank load refused (-2 oversize) leaves an old bank in place | P mechanism; no shipped bank oversize | Check the return of 0x8043C0AC (r3 at 0x8043C1E8) and 0x8044075C. |
