# SE manager static map (GUPP8P main.dol), for sound phase 2

Evidence: **P** = proven from instructions, **S** = strongly supported, **U** = unknown.
r2 = 0x805FA780 (`__init_registers` 0x8000332C), r13 = 0x805EC500. "K(x)" = f32 at r2+x.
Method used: full disassembly of 0x801D4EDC-0x801D9FAB, a whole-DOL scan for bl/b sites, `lis`/`addi` pairs and data words naming every
function start in 0x801D4E00-0x801DA400, a scan of all 689 `GetSoundManager` (0x8005328C) call sites for what the caller does with the returned
pointer, and r13-slot users.

## 0. Corrections to docs/GCAX_SOUND_MAP.md section 3

| Doc says | Code says | Ev |
|---|---|---|
| entry +0x10 = live-instance count, +0x14 = limit | +0x10 counts **starts in the current frame**. 0x801D87EC (the first call of the per-frame update) zeroes +0x10 of every row of all 8 tables every frame. Nothing ever decrements it. +0x14 = maximum new starts per id per frame (0 = unlimited) | P |
| stops 0x801D8C8C / 0x801D930C | 0x801D930C is the expiry tail **inside** 0x801D8D2C, not a function | P |
| selector called from 0x801D6808 / 0x801D6A1C | Those addresses are inside 0x801D66E0 and 0x801D69E4. functions.csv's starts 0x801D6628, 0x801D6808 and 0x801D6A1C are spurious (no callers; mid-function). So are 0x801D885C..0x801D88AC (case labels of the pruned 0x801D8838) and 0x801D8A54..0x801D8AA8 (case labels of 0x801D8A20) | P |
| pause gate: mgr+0x14 set -> only 0xE000-0xE009 | mgr+0x14 is a u32 bitset with **two** gates. Bit 1 = request gate (exempt list 0x8053DF08: 10 ids E000-E009). Bit 0 = pause gate (exempt list 0x804D1590: 6 ids E000-E005). Both exemptions also require `(id>>12)&0xF == 0xE` | P |
| PlaySE(mgr,id,prio,pos) | 3rd arg = priority boost (0/1), 4th = `Vec3*` (copied by value; the pointer is never retained). PlaySE alone never starts a sound: chan+0x14 = 0 until a wrapper sets bit 0 | P |

## 1. Functions

### 1a. Module layout (P)
0x801D4EDC-0x801D4FB7 = AudioManagerTask (RTTI "AudioManagerTask" : Task, vtable 0x8053DF38). 0x801D4FB8-0x801D55EB holds the public `mgr` wrappers (SE and BGM/voice).
0x801D6CB4-0x801D86AB plus 0x801D874C = BGM (TObjBGM, vtable 0x8053E014) and voice (TObjSERIF, vtable 0x8053DFDC) on CRI ADX. These are **not SE**: they take no manager state (the wrappers shift `mgr` out of r3).
0x801D3xxx-0x801D4ED4 is a separate game-side loop-sound list (0x801D4650 calls Play3D), a **client**.
0x801D9FAC+ is unrelated code.
Sound object = class "AudioGCN" (RTTI 0x805E9F68) : IAudio (IAudio vtable 0x8053DF48: 0x801D6B74 dtor, 0x801D6CAC/0x801D6CB0/0x801D6430 = empty `blr`). Its vptr is 0x8053F42C, and vt+0x14 = 0x801D6430 = `blr`.

### 1b. Entry points called from outside (all sites: Appendix A)

| Addr | Size | Name / role (signature, r3 = mgr unless noted) | External refs | Ev |
|---|---|---|---|---|
| 0x801D59AC | 0x6C | **PlaySimple(mgr, u32 id, s8 vol, u32 owner)**: `c=PlaySE(mgr,id,0,0)`; flags=5; c+6=vol (dead, see 3); c+0x18=owner. Returns 1/0 | 162 bl (all pass vol=0, owner=0) | P |
| 0x801D5894 | 0x118 | **PlayFlags(mgr, id, Vec3* pos, owner, u16 flags)**: keep-alive search if owner!=0, else PlaySE(boost 0)+0x801D5628+0x801D9B20+0x801D9A28+0x801D9AB4(dur 0); c+0x18=owner | 297 bl (all flags=1, owner=0, so positional one-shots) | P |
| 0x801D5764 | 0x130 | **Play3D(mgr, id, Vec3* pos, owner, u16 flags, s8 vol, s16 dur)**: same as above with boost=(flags&2)!=0, chan+6=vol, explicit duration | 98 bl in 77 fns (flags 1/2/3 constant; 3 sites in 0x801D4650 pass a dynamic flags u16 from list entry+0x10; dur 0/2/3/5) | P |
| 0x801D56A0 | 0xC4 | **PlayRing(mgr)**: if mgr+0x18 > 0 return 0; `c=PlaySE(mgr,0xE00B,0,0)`; c+0x14=0x85; c+8=c+7=0; c+4 = byte[0x8053DF1C + T] (T=0 -> 0xC0=-64, T=1 -> +64); T = !T; mgr+0x18 = K(-0x38FC)=0.0333333351. T = *(r13+0x4518) (init 1 once, guard byte r13+0x451C) | 4 bl (0x801148D4, 0x8011A50C, 0x80238F90, 0x80358BF0) | P |
| 0x801D4FFC | 0xB0 | **StopAllSE(mgr)**: for c in 0..42: c+0xC=0, c+0x30=0, c+0x14 \|= 4, (mode!=0 -> c+6=-128), ForceStop(c); mgr+0xC=0, mgr+0xD=0x26; vt+0x34 StopAll(); mgr+0x10=0 | 4 bl (StageManager slot 8 0x80178B90, 0x801DFA40, 0x80205E3C, 0x8032D4CC) | P |
| 0x801D50AC | 0xE4 | **SetSoundMode(mgr, mode)**: StopAllSE body; vt+0x58 SoundMode(mode); then **BGM** 0x801D79DC(mode), 0x801D8314() | 1 bl (0x80306148) | P |
| 0x801D5190 | 0x30 | GateClearIfZero(mgr, x): if x==0 clear bit1 of mgr+0x14 | 1 (0x80204E84) | P |
| 0x801D51C0 | 0x30 | GateSetIfZero(mgr, x): if x==0 set bit1 | 1 (0x80204F8C) | P |
| 0x801D51F0 | 0x28 | GateClear(mgr): clear bit1 | 2 (0x80204D4C, 0x80354B24) | P |
| 0x801D5218 | 0x28 | GateSet(mgr): set bit1 | 4 (0x80178670 stage enter, 0x80204E14, 0x802057CC GoalAction, 0x80354C0C) | P |
| 0x801D535C | 0x118 | **Pause(mgr, mode)**: set bit0; mgr+0x10=mode; for active c (row!=0) with mode==2 or not pause-exempt: vt+0x5C(port,ch) | 2 (0x8020944C, 0x8034A150; both mode=1) | P |
| 0x801D5240 | 0x11C | **Resume(mgr)**: for active c with mode==2 or not exempt: vt+0x64(c+6, port, ch); clear bit0; mgr+0x10=0 | 3 (0x802093D8, 0x80349BAC ShowAction, 0x80349D88) | P |
| 0x801D4EDC | 0x34 | AudioManagerTask::Update(task, f1 dt): `UpdateFrame(GetSoundManager(), dt)` | data 0x8053DF44 (vtable) | P |
| 0x801D6434 | 0x48 | **UpdateFrame(mgr, f32 dt)**: mgr+0x18 -= dt (fsubs); if <= 0 -> 0.0; FrameSE 0x801D6270; then **BGM** 0x801D868C (CRI 0x803C5710) | from 0x801D4EDC only | P |
| 0x801D64FC | 0x1E4 | **StageExit(mgr, u8 keepBgm)**: StopAllSE loop; vt+0x34; mode=0; vt+0x54 ResetAll; InitChan(c,i) for 43; mgr+0x14=0; mode=0; mgr+0x18=0.0; if !keepBgm BGM 0x801D824C(); vt+0x20(x) for x=7,1,2,6,4,5,3 (0x801F2A2C ignores x); FreeTable(tables,7) | 2 (0x80178240, 0x801D3404) | P |
| 0x801D66E0 | 0x20C | **StageEnter(mgr, s32 tableKey, s32 bankKey, u8 keepBgm)**: same stop/reset (no mgr+0x14 clear yet); if !keepBgm 0x801D824C; if sound && tableKey!=-1: vt+0x1C SelectBank(s, bankKey) for s=3,5,4,6,2,1,7 then SelectTable(tables, tableKey, 7); then mgr+0x14=0, mode=0, mgr+0x18=0.0 | 2 (0x80178648 stage loader 0x801783D4, 0x801D3460) | P |
| 0x801D69E4 | 0x11C | **Init(mgr)**: vt+0x1C(0,0); tables = new 0x80 (0x801D8C30); SelectTable(tables,-1,s) s=0,2,3,1,6,4,5; vt+0x54; InitChan x43 | 1 (0x801E2788) | P |
| 0x801D692C | 0xB8 | **Shutdown(mgr)**: vt+0x20(0); FreeTable s=5,4,6,1,3,2,0; delete tables (0x801D8BF4); mgr+8=0 | 2 (0x801E2560, 0x8032D330) | P |
| 0x801D6BF8 | 0xB4 | **Create(mgr)**: mgr+0 = new AudioGCN (0x2C, ctor 0x801F2E58); BGM init 0x801D874C; mgr+4 = new[] 43 x 0x34 (0x803A16B4, ctor 0x801D9C9C, dtor 0x801D6BBC); vt+0x54; InitChan x43 | 1 (0x801E2708) | P |
| 0x801D6B00 | 0x74 | **Destroy(mgr)**: delete[] channels; BGM 0x801D86AC; AudioGCN dtor vt+8(1); mgr+0=0 | 2 (0x801E2568, 0x8032D338) | P |
| 0x801D64B8 / 0x801D647C | 0x44/0x3C | mgr ctor (clears +0x14,+0,+0xD,+0xC) / dtor, from GetSoundManager 0x8005328C (static at 0x80571C34, pointer r13+0x2BB8, guard r13+0x2BB0) | bl 0x800532B8 / `lis/addi` 0x800532C4 | P |

Not SE (BGM/voice wrappers, keep as they are): 0x801D4FB8, 0x801D4FD8, 0x801D5474, 0x801D5494, 0x801D54B8, 0x801D54D8, 0x801D54FC, 0x801D551C, 0x801D5540 (30 sites), 0x801D556C, 0x801D558C, 0x801D55AC, 0x801D55CC, 0x801D55EC, 0x801D68EC, 0x801D690C, 0x801D7EAC. Also AudioManagerTask ctor/dtor 0x801D4F68 / 0x801D4F10. (P)

### 1c. Internal functions (no reference outside the manager; P)

| Addr | Size | Role |
|---|---|---|
| 0x801D5F14 | 0x19C | PlaySE(mgr, id, u8 boost, Vec3* pos) -> chan* or 0 (see 3a) |
| 0x801D5A18 | 0x3EC | Alloc(mgr, row, u8 prio) -> s8 index or -1 (see 2c) |
| 0x801D5E04 | 0x110 | FindOwned(mgr, id, owner): request gate, then the first c with c+0x18==owner && c+0x12==id&0xFFFF |
| 0x801D5628 | 0x78 | RowFlags(mgr, c, u16* flags): if flags&0x40: row+0xE bit0 -> flags\|=2; row+0xE bit2 -> flags\|=1 |
| 0x801D60B0..0x801D6238 | 8 x 0x38 | thunks `mgr->sound->vt[x](sound, args...)`: 60B0 +0x50, 60E8 +0x48, 6120 +0x4C, 6158 +0x38, 6190 +0x44, 61C8 +0x3C, 6200 +0x30, 6238 +0x24 |
| 0x801D6270 | 0x1C0 | FrameSE(mgr) (see 3) |
| 0x801D87EC | 0x4C | ResetFrameCounts(tables): row+0x10 = 0 for every row of 8 slots |
| 0x801D88CC | 0xD4 | Lookup (phase 1 native) |
| 0x801D89A0 / 0x801D8AFC / 0x801D8A20 | | FreeTable(tables,s) / LoadTable(tables,path,s) / SelectTable(tables,key,s) |
| 0x801D8BF4 / 0x801D8C30 / 0x801D8C74 | | tables dtor / ctor (8 x 0x10) / slot ctor |
| 0x801D8C8C | 0xA0 | ForceStop(c): vt+0x48 ResetPan(port,ch) **unconditional**; c+4=c+5=0; vt+0x30 NoteOff(port,ch); zero +0x18,+0x12,+0xC,+0x14,+0x10,+0x30 (bend not reset) |
| 0x801D8D2C | 0x620 | UpdateChan(c) (see 3) |
| 0x801D934C | 0xBC | SendPan(c): if cfg.mode==2 && c+5!=0: vt+0x44(c+5,port,ch) else vt+0x3C(c+4,port,ch) |
| 0x801D9408 | 0x1AC | DopplerBend(c) -> s32 |
| 0x801D95B4 | 0x1E8 | PosPan(c, s8* surround) -> s32 |
| 0x801D979C | 0x170 | PosVolume(c) -> s8 |
| 0x801D9D04 | 0x268 | ListenerDistance(Vec3*) -> f32 |
| 0x801D9F6C | 0x40 | MaxDist(row) = row+0xC ? 100.0f*(float)u8 : 400.0f |
| 0x801D990C | 0x38 | KeepPos(c, pos): if c+0x14&0x10 && pos: c+0x1C = *pos |
| 0x801D9944 | 0x64 | Refresh(c, flags, s16 dur): dur==0 -> row+0xD ?: 120; flags&4: c+0xE=dur, \|=0x4000; flags&2: c+0xC=dur, \|=2 |
| 0x801D99A8 | 0x80 | OutOfRange(c): row && c+0x14&0x10 && row+0xE&0x10 && Dist(c+0x1C) > MaxDist(row) |
| 0x801D9A28 | 0x8C | SetPos(c, flags, pos): if flags&1 && pos: c+0x1C=*pos, \|=0x10; if !(row+0xE&8) \|=0x40; if flags&0x20 \|=0x100 |
| 0x801D9AB4 | 0x6C | SetDur(c, flags, s16 dur): default as Refresh; flags&4: c+0xC=c+0xE=dur, \|=0x4000; else c+0xC=dur, flags&2 -> \|=2 |
| 0x801D9B20 | 0xC | Arm(c): c+0x14 = 5 |
| 0x801D9B2C | 0xC0 | Start(c, id, row, prio) (see 2b) |
| 0x801D9BEC | 0x3C | Route(c, s8* port, u8* ch) |
| 0x801D9C28 / 0x801D9C9C | 0x74/0x68 | InitChan(c, idx) / array ctor (same without +0) |
| 0x801D6BBC | 0x3C | channel dtor (array delete) |

## 2. State

### 2a. Manager (static 0x80571C34, size 0x1C; next object at 0x80571C50) (P)

| Off | Type | Meaning |
|---|---|---|
| +0x00 | AudioGCN* | sound object (MIDI front end). **Read outside**: 0x8032D2CC, 0x8032D314, 0x8032D530, 0x8032D6E0 test `mgr->sound != 0` |
| +0x04 | chan* | 43 channels x 0x34 (array-new block of 0x8CC) |
| +0x08 | tables* | 8 slots x 0x10: +0 s32 count, +4 row*, +8 file buffer, +0xC parser (0x8040F66C, 0x28 bytes) |
| +0x0C | s8 | normal-ring cursor, 0..0x1F |
| +0x0D | s8 | special-ring cursor, 0x26..0x2A (reset value 0x26) |
| +0x10 | s32 | pause mode: 0 none, 1 = pause non-exempt (the only value callers pass), 2 = everything (no external caller sets 2: dead in shipped code, S) |
| +0x14 | u32 | bit0 pause gate (list 0x804D1590), bit1 request gate (list 0x8053DF08) |
| +0x18 | f32 | ring-sound cooldown, seconds |

### 2b. Channel (0x34 bytes, after Start 0x801D9B2C) (P)

| Off | Type | Meaning |
|---|---|---|
| +0x00 | s8 | index. Route: idx<=0x26 -> port=idx>>4, ch=idx&15; idx>0x26 -> port=idx-0x23, ch=0. So 0x26 = port 2 ch 6 but **gets no SetBankProgram** (start tests idx<0x26) |
| +0x01 | u8 | bank = row+8 (init 0xFF) |
| +0x02 | s8 | program = row+9 (init 0xFF) |
| +0x03 | u8 | priority |
| +0x04 | s8 | pan last sent (0 at start) |
| +0x05 | s8 | surround value (CC62) |
| +0x06 | s8 | volume last computed (attenuation, 0 = nominal, -127 = silent) |
| +0x07 | s8 | volume offset (always 0: Start and Ring write 0; nothing else writes it) |
| +0x08 | s8 | volume ceiling (always 0, same) |
| +0x09 | u8 | \|= 0x80 at each note-on (from K(-0x38D0)=0x00000080); = 0x1F on retrigger; **never read anywhere** |
| +0x0A | s16 | pitch bend last computed |
| +0x0C | s16 | frames left |
| +0x0E | s16 | reload duration (retrigger) |
| +0x10 | u16 | age in frames (+1 per update incl. the start frame); ages the equal-priority victim choice |
| +0x12 | u16 | id (0 = free for the normal ring) |
| +0x14 | u32 | flags (see 3) |
| +0x18 | u32 | owner key (keep-alive match) |
| +0x1C | Vec3 | position copy |
| +0x28 | f32 | max distance = MaxDist(row); also passed as f1 to NoteOn (NoteOn never reads f1, P for port<3) |
| +0x2C | f32 | previous listener distance (Doppler), 0.0 at start |
| +0x30 | row* | AudioData row (0 = inactive for update/special ring) |

Start(c,id,row,prio) writes +0x30,+1,+2,+3; zeroes +4,+5,+7,+8,+0xA,+0x14; sets +0xC=+0xE=row+0xD ?: 120; +0x10=0; +0x12=id; +0x18=0; +0x1C..=0.0; +0x28=MaxDist; +0x2C=0.0. **+6 is not reset.**

### 2c. Allocator 0x801D5A18 (P)
- Special (row+0xE bit1; no shipped row sets it, S): 5 probes from mgr+0xD over 0x26..0x2A. Free means row==0. The victim is the **last** channel in probe order with prio <= new (r9 is never set: compiler artefact). Cursor = pick+1 (wraps to 0x26). It returns **without** NoteOff or ResetPan.
- Normal: up to 32 probes from mgr+0xC over 0..0x1F; free = id==0 stops the scan. Otherwise the victim is the **last** probed with prio < new. Failing that, the first equal-prio channel with the strictly greatest age (only while no lower-prio channel was seen); failing that it returns -1. Cursor = pick+1 (mod 0x20, also when -1 -> 0).
  Then: if mode==2: c+6=-127; if mode==1 and old id is not pause-exempt and new id is: c+6=-127. Then Route; if c+4||c+5: zero them, vt+0x48 ResetPan(port,ch). **vt+0x30 NoteOff(port,ch) always, even for a free channel.**
- Channels 0x20-0x25 are never allocated, but StopAll and the stage functions still send ResetPan/NoteOff to them.

### 2d. Other globals (read unless noted) (P)
| Address | Use |
|---|---|
| r13+0x2BB8 / +0x2BB0 | singleton pointer / guard (only 0x8005328C) |
| r13+0x4518 (s32), r13+0x451C (u8) | ring pan toggle / init guard: R/W only by 0x801D56A0 |
| 0x8057E760+0x78 (s32) | output mode: 1 mono, 2 surround (others stereo). Read by SendPan, PosPan, PosVolume and AudioGCN vt+0x48 |
| 0x8057E760+0xB2 (u8) | 2-player flag: two listeners in Distance; pan forced 0 |
| *(r13+0x3130) = 0x805EF630, *(r13-0x6AD8) = 0x805E5A28 | listener ids (player 1/2). Looked up in the actor registry 0x800742BC (static 0x80577060) via 0x800A8F5C (match obj+0x298 == id) plus the shared/weak-pointer helpers 0x800520B0/0x80051CFC/0x8005223C/0x800521AC/0x800521EC/0x8005232C. Position = Vec3 at the resolved pointer |
| camera 0x80009548 (static 0x8056FED4) | PosPan: listener = Vec3 at camera(0)+8 (0x80010244); yaw = euler.y from 0x80109F68 -> 0x8000A610 -> 0x804107B0 = atan2f(-fwd.x,-fwd.z) (radians) |
| 0x8053DF1C | ring pan bytes {0xC0, 0x40} |
| 0x8053E028 | stage-key -> AudioDataStage*.bin pairs (terminated by -1); slots 0-6 use 0x804D1810.. (System, Gadget, Weapon, Player, PlayerHero, PlayerDark, Enemy) |

## 3. Per-frame (P)

UpdateFrame (0x801D6434) -> FrameSE 0x801D6270(mgr):
1. if tables: ResetFrameCounts. 2. if mode==2: return (no channel work, no vt+0x14).
3. for i = 0..42, if c+0x30 != 0: if mode==1 and not pause-exempt(bit0, 6-list) -> skip. If bit1 set and id not request-exempt (10-list) and (c+0xC > 10 or < 0) -> c+0xC = 10. Then UpdateChan(c).
4. vt+0x14 (`blr`). Then (in UpdateFrame) BGM 0x801D868C.

PlaySE 0x801D5F14(mgr,id,boost,pos): request gate -> 0. tables==0 -> 0. row = Lookup (row+0x10 incremented here). row==0 -> 0. If row+0xE&0x10 && pos && Dist(pos) > MaxDist(row) -> 0 (the frame counter stays consumed). prio = row+0xF==10 ? 0 : max(row+0xF, boost)+1. idx = Alloc; <0 -> 0. Start(c,id,row,prio); return c.

Keep-alive (Play3D/PlayFlags with owner!=0): c = FindOwned. If found: (PlayFlags: RowFlags); KeepPos(c,pos); Play3D sets c+6=vol. If OutOfRange -> return 1 (no refresh, so it times out). Else Refresh(c, flags, dur), return 1. Positions are only ever updated this way: **the game's Vec3 is copied, never re-read per frame.**

Request flags (u16): 1 positional (needs pos), 2 refresh timer on keep-alive (+1 priority boost in Play3D), 4 retrigger loop, 0x20 Doppler, 0x40 take flags 1/2 from row+0xE bits 2/0.
Channel flags c+0x14: 0x1 start pending; 0x2 set, never read; 0x4 stop at expiry (always set: Arm=5, Ring=0x85); 0x10 positional; 0x20 one-shot volume from +7 (never set: dead); 0x40 positional pan; 0x80 send pan once (Ring); 0x100 Doppler; 0x200 send bend once (never set: dead); 0x400 positional only at start (never set: dead); 0x4000 retrigger at expiry.

Row fields used: +8 bank, +9 program, +0xA s16 volume % (vol += 127*rowA/100), +0xC u8 max distance /100 (0 -> 400), +0xD u8 duration frames (0 -> 120), +0xE flags (bit0/bit2 via 0x40, bit1 special ring, bit3 no pan tracking, bit4 distance cull at request + OutOfRange + stop when vol < -110), +0xF base priority (10 -> prio 0), +0x10/+0x14 per-frame start counter/limit.

**UpdateChan 0x801D8D2C(c)**. port/ch from Route; each vt call goes through GetSoundManager()->sound.
- **Start** (flags&1): c+0x2C=0.0. If idx<0x26: **vt+0x50 SetBankProgram(port, ch, c+1, c+2)**. Then: if &0x10: c+6=PosVolume(c), and if &0x400 clear 0x10|0x400. Elif &0x20: c+6=c+7, clear 0x20. Else c+6=c+7 (=0).
  v = c+6. If row+0xA != 0: v = trunc(clamp(fmadds(127.0, fmuls(0.01, (float)rowA), (float)(s8)c+6), lo -127 (`<`), hi 127 (`>=`))).
  **vt+0x38 Volume(v, port, ch)**. If &0x40: c+4=PosPan(c,&c+5); SendPan. If &0x80: clear, SendPan. If &0x100: c+0xA=DopplerBend(c). If &0x200 clear. **vt+0x4C Bend(c+0xA, port, ch)**. **vt+0x24 NoteOn(c+1, c+2, c+3, port, ch, f1=c+0x28)**. Then clear bit0; c+9 \|= 0x80; c+0x10++. Return (no timer decrement in the start frame).
- **Running** (flags&1 == 0), only if c+0xC != 0:
  - &0x10: n=PosVolume(c). If n != c+6: send Volume(f(n)) with the rowA formula, c+6=n. Then if row+0xE&0x10 && c+6 < -110: c+0xC=1.
  - &0x20 (dead): the same with c+7, clear.
  - &0x40: s=0; p=PosPan(c,&s); if p!=c+4 || s!=c+5: **SendPan(c) with the OLD c+4/c+5**, then store p, s (one-frame lag).
  - &0x80: vt+0x3C Pan(c+4) directly, clear.
  - &0x100: b=DopplerBend(c); if b != c+0xA: **Bend(OLD c+0xA)**, then c+0xA=b (one-frame lag).
  - &0x200 (dead): Bend(c+0xA), clear.
  - c+0xC--; c+0x10++.
- **Expiry** (c+0xC == 0 after the above, or on entry):
  - &0x4000: \|=1, c+0xC=c+0xE, c+9=0x1F, clear 0x4000, \|=4. The next frame retriggers (no NoteOff before the new NoteOn).
  - Elif &4: if c+4\|\|c+5: vt+0x48 ResetPan(port,ch), zero them. If c+0xA: Bend(0), c+0xA=0. **vt+0x30 NoteOff(port,ch)**. Zero +0x18,+0x12,+0xC,+0x14,+0x10,+0x30 (= ring return; no counter decrement).
  - Else nothing (unreachable in shipped flows).

Math (all f32 unless noted; constants from K()):
- Dist(p) (0x801D9D04): single player: listener found -> |L - p|, else 0.0 (K(-0x3884)). 2P (cfg+0xB2): min(d1, d2), with 9999999.0 for a missing player. |v| = 0x8040DFA0: x*x, y*y, z*z (fmuls), (x2+y2), z2+that (fadds); sqrt 0x8000FF04 = `frsqrte` + 3 double Newton steps, x*r, `frsp` (x<=0 handled separately).
- PosVolume (0x801D979C): d=Dist(c+0x1C); d >= c+0x28 -> -127. r7 = (mode==2) ? 30 : 0. d < 60 -> a=0; else t=max(0, (d-60)/(max-60)) (fsubs, fdivs); a=trunc((float)r7 - t*(float)(r7+127)). a += c+7, floor -127. Mode 2: a=min(a, c+8+30), min 127; else a=min(a, c+8). Returns s8.
- PosPan (0x801D95B4): mode 1 or 2P -> 0 (*out untouched). D = c+0x1C - camPos; D.y=0; r=|D|. **Mode 2 -> *out=0, return 0** (an atan2f is computed and discarded), so there is no positional pan in surround. Stereo: ang = (atan2f(D.z, D.x) + yaw) + 16384.0f (two fadds; 16384 added to a **radian** angle, an original quirk); s = sinf(ang) (MSL double sin + frsp); p = 0.7f*s; p = (r < 50) ? 100*(p*(r/50)) : 100*p; clamp [-127,127]; trunc.
- DopplerBend (0x801D9408): single listener only; d=|c+0x1C - L| (L=(0,0,0) if missing). If c+0x2C==0.0 && d!=0.0 (fcmpu): c+0x2C=d, return 0. If the PREVIOUS distance c+0x2C > 800 (`fcmpo f2=prev, f0` at 0x801D9524; corrected 2026-10-26 by se_math_contract_tests): c+0x2C=d, return c+0xA. Else b=20*(c+0x2C - d), clamp [-6144, 6144], c+0x2C=d, return trunc(b). (AudioGCN Bend then clamps to [-0x4000, 0x3FFF].)
- Cooldown: mgr+0x18 = fsubs(mgr+0x18, dt); <= 0 -> 0.0.

## 4. Outside access to manager/channel state

| What | Who | Ev |
|---|---|---|
| mgr+0 (non-null check) | 0x8032D1D0 (x2), 0x8032D4CC (x2) | P |
| any other mgr field, channel, table slot or row | none: all 689 GetSoundManager results go straight into a wrapper call (only the +0 reads above; no `mr` copies); 0x80571C34 is formed only in 0x8005328C; r13+0x2BB8 is read only there | S (window-based scan) |
| handles | none returned. The wrappers return 1/0. The only "handle" is the caller-chosen `owner` word (usually `this`) matched with the id by FindOwned for keep-alive | P |
| channel +7/+8/+0x20 flag writers | none outside, so the +0x20/+0x200/+0x400 branches are dead | S |

## 5. Suggested phase-2 boundary

**Native owners** (replace whole guest functions; afterwards no translated SE code remains reachable):
- Requests: 0x801D59AC, 0x801D5894, 0x801D5764, 0x801D56A0.
- Control: 0x801D4FFC, 0x801D50AC (must still call guest BGM 0x801D79DC(mode) then 0x801D8314 after vt+0x58), 0x801D5190, 0x801D51C0, 0x801D51F0, 0x801D5218, 0x801D535C, 0x801D5240.
- Frame: 0x801D6434 (cooldown + SE frame; must still call guest 0x801D868C last). AudioManagerTask 0x801D4EDC can stay translated.
- Lifecycle/stage: 0x801D64FC, 0x801D66E0 (keep the BGM 0x801D824C call, vt+0x20/vt+0x1C call orders and the table load), 0x801D69E4, 0x801D692C, 0x801D6BF8 (keep guest `new` + AudioGCN ctor 0x801F2E58 + BGM 0x801D874C; the channel array moves to host), 0x801D6B00 (keep BGM 0x801D86AC + AudioGCN dtor).
- Then every function in table 1c is unreachable and can be pruned (0x801D88CC phase-1 owner included; tables parsed natively from AudioData*.bin).

**Must stay in guest memory:** mgr+0 (AudioGCN*; read by 0x8032D1D0/0x8032D4CC and the vtable dispatch target), the singleton storage 0x80571C34 and r13+0x2BB8/0x2BB0 (0x8005328C stays translated; its result is just the r3 handle), the AudioGCN object and the GCAX state behind it. mgr+4..+0x18, the channels, the table slots/rows/file buffers and r13+0x4518/0x451C can move to host (S: no outside readers found).

**Guest inputs the native owner must read each use:** cfg 0x8057E760 +0x78/+0xB2; listener ids r13+0x3130 / r13-0x6AD8 through registry 0x80577060; camera 0 pose; the caller's Vec3* at request time. (Decision: call the guest lookup helpers, which bump/drop shared-pointer counts, or read the same pointers natively.)

**Oracle:** the ordered list of AudioGCN vt calls (offset + args) per request and per frame, plus host-side channel state, compared against the translated manager on recorded request sequences.

**Riskiest parts for exact equivalence:**
1. **sqrt** = `frsqrte` (Gekko estimate table) + 3 double Newton steps + `frsp`. It feeds the cull (`>`), PosVolume (`>=`, t), pan (r/50) and Doppler. Use Dolphin's frsqrte table or prove sqrtf-equality over the input range.
2. **PosPan**: MSL atan2/sin (double, then frsp), the radian+16384.0f quirk (absorbs ~0.002 rad of precision), fadds order, trunc at integer boundaries. Same for the camera yaw (0x804107B0: sqrt + atan2f).
3. **fmadds** in the rowA volume: use fmaf(127, 0.01f*(float)rowA, (float)vol), not a separate mul+add.
4. **MIDI message count/order**: the GCAX ring drops messages at 0x400. Cases to reproduce:
   - NoteOff on every normal-ring allocation, even when the channel is free.
   - ResetPan only when +4/+5 != 0, except ForceStop (always).
   - Pan and Bend resend the OLD value (one-frame lag).
   - Play3D keep-alive writes c+6=vol, which re-sends Volume whenever PosVolume differs.
   - Retrigger sends a NoteOn without a NoteOff.
   - SetBankProgram is skipped for idx 0x26.
   - StopAll walks all 43 channels, including the never-used 0x20-0x25.
5. **Per-frame counters** (+0x10 reset every frame, consumed even when the request is culled or the allocation fails), the allocator's "last lower-prio" and "oldest equal-prio, first on tie" rules, and the special-ring compiler artefact.
6. Gate/pause semantics: two lists (10 vs 6 ids); clamping timers to 10 under the request gate; mode-1 pause freezing timers; Resume restoring c+6 via vt+0x64.

## Appendix A: every external reference (bl sites unless noted)

**0x801D4EDC** (1 sites, 1 functions): data:8053DF44

**0x801D4F10** (1 sites, 1 functions): data:8053DF40

**0x801D4F68** (1 sites, 1 functions): 801E2730

**0x801D4FFC** (4 sites, 4 functions): 80178BD4 801DFA64 80205EAC 8032D578

**0x801D50AC** (1 sites, 1 functions): 80306148

**0x801D5190** (1 sites, 1 functions): 80204E84

**0x801D51C0** (1 sites, 1 functions): 80204F8C

**0x801D51F0** (2 sites, 2 functions): 80204D4C 80354B24

**0x801D5218** (4 sites, 4 functions): 80178670 80204E14 802057CC 80354C0C

**0x801D5240** (3 sites, 3 functions): 802093D8 80349BAC 80349D88

**0x801D535C** (2 sites, 2 functions): 8020944C 8034A150

**0x801D56A0** (4 sites, 4 functions): 801148D4 8011A50C 80238F90 80358BF0

**0x801D5764** (98 sites, 77 functions): 8005C42C 8005C8AC 8005CD4C 8005D494 8005D740 8005D9B8 8005E824 8005EC50 8005EEA0 8005F6FC 80064A08 80068C8C 80069194 80069940 80076648 8008B07C 800EA22C 800EC20C 80102B14 80102B38 8011BBE8 80135B7C 80148B7C 80148C00 80148C94 80148D10 8014C5AC 801542A4 801571DC 801BDE6C 801BDED4 801BDF9C 801BDFC0 801BDFE4 801BE008 801BE0A0 801BE0C4 801BE0E8 801BE10C 801D46A8 801D46CC 801D46F0 801F4100 801F4350 802219E8 802257E8 80225CD4 80229164 8022A734 8022B6D0 8022C3E8 8022CF34 8022E49C 8022E4D0 8022FC8C 8023F9AC 802400E8 80240F6C 802491F8 8024ABA0 80255F64 802570C0 80258210 8025A30C 802600E4 80266D44 80266DEC 8026C674 8026C8F0 802744D0 802773A4 8027F620 80280074 802807F8 80281258 80285FC4 8028634C 8029317C 802AA734 802AB0A4 802C049C 802C07E8 802CD370 802F1ECC 802FF028 80301F64 80301FC0 80303F9C 80304AD0 8030A714 80321720 80321804 80325860 803265B8 80328F5C 8033DB84 8033DBF8 8033DC20

**0x801D5894** (297 sites, 218 functions): 800531BC 800531EC 800553F4 800572F8 8006504C 80067A1C 800CCC50 800CF31C 800D25CC 800D47F0 800D66B4 800D8490 800D86AC 800D8FF4 800D97C4 800DAD98 800DC8A0 800DCFC0 800E11A8 800E11C0 800E135C 800E1374 800E3338 800E3590 800E4904 800E5504 800E5600 800E63CC 800EE57C 800F1864 800F1880 800F2C88 800F2CA4 800F955C 80105C80 8012225C 80122B44 80123808 8012384C 80123890 80124BB4 80124BD8 80126C44 8012B9C4 8012D92C 8012E45C 8012EBE8 8012ED2C 8012EDD8 8012FFB0 80131838 80132B68 80132D68 8013465C 80138D6C 80138E44 80138E8C 80138EB0 801395CC 8013C688 8013C6AC 8013C6C8 8013C6E4 8013C6FC 8013F8B8 8013FFE8 801400CC 801407F4 80140A48 80142240 801445C4 8014466C 80144718 801447C4 80146B58 80146BA0 80146BF4 80146C24 80146D58 80146D80 80148B58 80148BDC 80148C70 80148CEC 8014E1F0 8014F670 8014FD40 80151334 80157260 80157C84 80158FD8 8015A8E4 80181648 8018A8E8 8018A904 8018B3BC 8018D804 80190F98 80192DB0 80192EB0 80192ED8 80192FFC 80194FB0 8019AD68 8019ADCC 8019AE30 801A0A98 801A0ABC 801A0AE8 801A0B30 801A0C38 801A0C64 801A0CD0 801A0CFC 801A0D68 801A0D94 801A0E00 801A0E2C 801AFE5C 801B1AE8 801B1B4C 801B2008 801F48DC 801F4900 801F70AC 801F91A4 801F91C8 801F9580 8020A1A4 8020C250 8021D3D4 8021F000 80222820 8022328C 802238D0 802245A0 80224670 802261F8 802263AC 8022A710 8022A750 8022B248 8022DF7C 8022DFB8 8022DFF4 8022E0C4 8022E284 8022FBBC 80236ECC 80237E4C 8023F610 8023F630 8023F650 8023F670 8023F690 8023F6B0 8023F6D0 8023F6F0 8023F710 8023FAEC 80240180 802407C0 802408E4 80240954 802468A0 8024709C 80248284 8025066C 80255878 802558B4 802558F0 8025592C 80255AE0 8025725C 8025731C 80257454 80257F48 802581D0 80259940 8025995C 80259978 80259994 8026009C 80262E0C 80262EC0 80263048 802699FC 8026B404 8026CD90 8026E3BC 80275734 80275E18 80276370 80276838 80277A90 80277B98 8027B3A0 8027B684 8027FD48 8027FE50 8027FFB0 802800E0 802809BC 802811F0 802812D0 802816EC 802848D8 80285194 802857F4 80285858 802858BC 80287E84 8028CC40 8029EB34 802A43B8 802A5E90 802A8BD8 802A8EEC 802A92B4 802AAAF0 802AAC08 802AAEC4 802AB074 802AB368 802AB40C 802B1DC8 802C0C40 802CB21C 802CB910 802D68F8 802D76A4 802D76DC 802D7718 802D7840 802D786C 802D7AC0 802D837C 802D8D50 802D8E1C 802E8FC4 802F3A80 802F3A98 802F3B94 802F56B0 802F57AC 802F7398 802F88C4 802FB314 802FE9A4 802FEC74 802FECF4 802FF17C 80300828 80300B58 80301BD4 80301C54 80302FC4 80303298 80303F78 80304AAC 803077F4 80308CC0 80308CE0 80309134 8030C0C0 8031635C 80316D24 80316DC4 80316E14 80316E5C 80317F14 80317F80 80318088 803182AC 8031897C 80319F68 8031DE78 8031EB84 8031F7F0 80320AA4 80320C3C 80320C90 80320CD8 80320F00 803217D8 80324A18 80324C28 80324D28 80327070 8032B15C 80337960 8033E36C 8033E3A8 8033F5D0 80350EDC 803511B0 803586F0

**0x801D59AC** (162 sites, 98 functions): 8005C938 800650A0 8007660C 8009B9BC 8009B9E8 800A3458 800A5484 800D54CC 800E0000 800E88E0 801003E4 801013E0 8010A120 8010E328 8010F798 80110A4C 80116560 80119450 8011B7CC 8011C150 8011C688 80121BEC 80125AA8 80125AF8 80129ABC 80129C60 80130A0C 80137094 801370EC 80138630 80138A48 8013DB64 8013DBE8 80142314 8014C660 8014C694 80154074 8016B41C 80174194 8019AD80 8019ADE4 8019AE48 801ADC20 801AE804 801AECF0 801BB9F4 801BC308 801BDEF4 801BE5F4 801BE60C 801BEE38 801E3278 801E3834 801E3868 801E3914 801E3948 801E3C74 801E3CB8 801E3CE0 801E3F20 801F67BC 801F6870 801F6A18 802015D0 80202E98 802039A0 8023B20C 8023B664 8025B9A8 80263ED8 8026467C 8026757C 80268B90 80275764 80276FC4 80287B60 80293A5C 802A5EA8 802B9720 802B97B8 802B9848 802B9910 802B996C 802B99D8 802B9B74 802B9C4C 802B9D20 802B9DF4 802B9E34 802B9E74 802BC48C 802BC510 802BC590 802BC5E8 802BC684 802BC79C 802BCAF8 802BCB6C 802BCBA0 802BCBDC 802BCC10 802BCC50 802BD9E8 802BDA84 802BDAC8 802BDB18 802BDCDC 802BDD40 802BDD84 802BDDCC 802BE010 802BE034 802BE074 802BE0B8 802BE0FC 802BE3D4 802C24F4 802C2578 802C2724 802C277C 802C2C58 802C2DA0 802C2DEC 802C3204 802C3368 802C364C 802C7BFC 802C7C18 802C7C5C 802C7D50 802C7D98 802C7E50 802CD3A0 802DBE74 802DC1AC 802E583C 802E5934 802E59F0 802E5A44 802E5A8C 802E5AD0 802E658C 802E65C8 802E65EC 802F13CC 802FA6CC 802FB5B0 8030E80C 8030EAB4 8030EC24 8030F490 80310410 8032BCC8 80332948 80332970 803329AC 80332CCC 80333B88 8033791C 8033F0EC 8034288C 803550F0

**0x801D6430** (2 sites, 1 functions): data:8053DF5C data:8053F440

**0x801D6434** (0 sites, 0 functions): 

**0x801D647C** (1 sites, 1 functions): lis/addi:800532C4

**0x801D64B8** (1 sites, 1 functions): 800532B8

**0x801D64FC** (2 sites, 2 functions): 80178240 801D3404

**0x801D66E0** (2 sites, 2 functions): 80178648 801D3460

**0x801D692C** (2 sites, 2 functions): 801E2560 8032D330

**0x801D69E4** (1 sites, 1 functions): 801E2788

**0x801D6B00** (2 sites, 2 functions): 801E2568 8032D338

**0x801D6B74** (1 sites, 1 functions): data:8053DF50

**0x801D6BF8** (1 sites, 1 functions): 801E2708

