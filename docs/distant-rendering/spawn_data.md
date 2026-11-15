# Layout (object placement) data and structures (static RE, PAL GUPP8P main.dol, 2026-10-29)

Scope: the data side of the spawn system - disc files, the in-memory "SetManager", entries, slots, flags, setid.bin,
factories, link groups and the restriction/event gates. The range math itself is in `docs/DRAW_DISTANCE.md`.
V = VERIFIED (read in the disassembly / decoded from the disc), I = INFERRED.
Decoder: `layout_decode.py`, a scratch script of the session that wrote this (not kept)
(`python layout_decode.py 0100`; `setid_map.py` there maps setid bits to stages).

## 0. Corrections to earlier docs

| Doc | Says | Is |
|---|---|---|
| DRAW_DISTANCE.md "whole cycle" | 0x1 = "dormant / may spawn" | 0x1 = **entry enabled**. It stays set while the object is live (0x1\|0x2). An object leaving range is destroyed through the base dtor 0x8015AD8C -> 0x800C9FD0, which clears only 0x2/0x4, so the entry is 0x1 again and respawns on return. Only kill/collect (0x800C9F1C) clears 0x1. V |
| object_distance.md 1.2 (0x801ED970) | sets 0x800000 / 0x20000 via 0x800CA298 / 0x800CA284 | 0x800CA284 **clears** bits: the callback sets 0x800000 (when not enabled) and **releases** the hold 0x20000. V |
| DRAW_DISTANCE.md | "dormant (0x1) with a type" | test is slot+0x10 (factory index) >= 0, set at load by 0x800CA6B4. V |

## 1. Layout files on disc

### 1.1 Which files, how found, when loaded (V)

Per stage `stg%04d/stg%04d_{cmn,nrm,hrd,ds1}.dat` (format strings 0x804B77C0 / 0x804B77A8 / 0x804B7790 / 0x804B7778,
printed by 0x800CB6BC / 0x800CB68C / 0x800CB65C / 0x800CB62C through sprintf 0x803AA248) plus the global
`setid.bin` (0x804B77E8). `stgNNNN_cam.dat` (0x804D01C8) is the camera layout, not objects.

Stage build 0x801783D4 (one function, the stage task's setup):
- 0x80178424: `new SetManagerTask(0x28)` 0x800CB820 in task group 6 (0x801EB614(sched, 6)).
- 0x8017871C: `0x800CBC54(holder = 0x800C9F6C(), stageNo = [0x8057E760+0xA8], stageIdx = [0x8057E760+0xAC])`.

0x800CBC54 (LoadLayout):

| Step | Call | Effect |
|---|---|---|
| 1 | 0x800C9B00 | r13+0x3274 = 0 (global leave flag word) |
| 2 | 0x800CB044(mgr, "setid.bin") | setid table -> mgr+0x10 / +0xC |
| 3 | 0x800CA7F0(mgr, 0x8052C1A0, stageIdx) | mark used factories (+0x1B bit 0) |
| 4 | 0x800CB1A4(mgr, cmn path) | append cmn; flags as on disc (0x1000) |
| 5 | if byte [0x8057E760+0x125] != 0 (I: hard mode): 0x800CB0EC(mgr, hrd, 0x4000, 0xB000) else 0x800CB0EC(mgr, nrm, 0x2000, 0xD000) | append, then flags,saved = (x \| set) & ~mask |
| 6 | 0x800CB0EC(mgr, ds1, 0x8000, 0x7000) | append, force 0x8000 |
| 7 | 0x800CA780(mgr, table) | preload: factory f1 (+4) for every used factory; mgr+4 = number of factories (286) |
| 8 | mgr+0x14 = 0x8052C1A0; 0x800CA6B4(mgr) | slot+0x10 = index of factory with +0x18 == entry type, else -1 (does NOT check the used bit) |
| 9 | holder+4 = 1 | update enabled |

So entry/slot indices are: cmn [0, n1), then nrm or hrd, then ds1. Westopolis normal: 504 + 71 + 536 = 1111 entries.
Layer bits 0x1000/0x2000/0x4000/0x8000 = which file the entry came from (cmn keeps its disc bits, all 0x1000 in stg0100).

File I/O: 0x80037200() -> 0x800450C4(fs, path, out) returns a heap copy of the whole file (I: archive/disc manager
load), freed with 0x80049B5C after copying. No bounds check on 0x800 entries or the 0x10000-byte misc pool (V).

### 1.2 Format (V: swap format strings + loader 0x800CB1A4; little-endian on disc, swapped in place)

Header 0xC bytes, swap format "ccccii" (0x805F4184 via ptr 0x805F4180, 0x800CB750); magic checked by memcmp
0x803AD92C against "sky2" (0x805F4178):

| Off | Type | Field |
|---|---|---|
| 0x0 | char[4] | "sky2" |
| 0x4 | u32 | entry count N |
| 0x8 | u32 | misc bytes M |

Then N entries of 0x2C (swap format "ffffffiisccii", 0x804B7768, static ptr r13+0x3288 / guard r13+0x328C), then M
bytes of misc (swapped as u32 words by 0x80043D34). File size = 0xC + 0x2C N + M (V for all 130 files).

| Off | Type | Field | Notes |
|---|---|---|---|
| 0x00 | f32 x3 | position | 0x800CA2FC returns it |
| 0x0C | f32 x3 | rotation, **degrees** | 0x800CA2BC returns it * pi/180 (0x805FBDB0 / 180.0 at 0x805F4158) |
| 0x18 | u32 | flags (live) | disc value = saved \| 0x80000000 (sometimes \| 0x200000); **overwritten** with +0x1C on load (0x800CB298) - disc value unused |
| 0x1C | u32 | saved (authored) flags | restored on respawn/restart |
| 0x20 | u16 | type = class layout id (factory +0x18) | 0x800CA10C |
| 0x22 | u8 | link id (0..255) | 0x800CA238 (u32) / 0x800CA248 (u8) |
| 0x23 | u8 | range (x100 units) | 0x800C9D20, 0x800C9D54, 0x800CA200, 0x800CA970 |
| 0x24 | u32 | misc length (bytes, multiple of 4) | |
| 0x28 | u32 | misc pointer | 0 on disc; set to pool slice at load |

Misc: the M bytes are the concatenation of every entry's misc in entry order; the loader copies them to
mgr+0x420 + used, swaps words, sets entry+0x28 = pool + cursor, cursor += len (0x800CB2A4..0x800CB344). Content is
class-defined u32/f32 words described by the factory's parameter table (+0x20, e.g. BK_SOLDIER 0x80531A88,
`object_distance.md` 1.5). Classes read it with 0x800CA1A4(slot, dst, size) (182 call sites) and write it back with
0x800CA150(slot, src, size); a size != entry+0x24 sets flags 0x20000000 and copies nothing.

### 1.3 Westopolis (stg0100) decoded (V, layout_decode.py)

| File | N | misc | flags (saved) | ranges |
|---|---|---|---|---|
| cmn | 504 | 10464 | 0x1001 x452, 0x401001 x48, 0x1001 (+disc 0x200000) x4 | 2..30; mode 5 (101), 6 (96), 4 (72), 10 (62) |
| nrm | 71 | 1716 | 0x2001 x71 | 1..15; 5 (28), 2 (15) |
| hrd | 22 | 532 | 0x4001 | 3..15 |
| ds1 | 536 | 4604 | 0x408001 x280, 0x8001 x256 | 1..26; 6 (170), 5 (162) |

Normal mode (cmn+nrm+ds1, 1111 entries) range histogram: 1:3 2:34 3:43 4:113 5:291 6:273 7:76 8:58 9:27 10:74
11:7 12:13 13:28 14:3 15:20 16:8 18:1 20:20 22:1 23:1 25:14 26:1 30:2.

Classes: cmn CityLaser 102, ART GADGET 82, Fruit 36, BRK_OBJ 34, RING 33, SETCOLLISION 27, GUN_SOLDIER 24,
BK_SOLDIER 23, BK_WINGSMALL 22, JUMP_BOARD 16, CONTAINER 16, HRD_CONTAINER 11, GUN_BEETLE 9, FIRE 9, ... (35 classes).
nrm HINTRING 22, HINT COLLISION 16, SET_CHARACTOR 7, RING 4, ... ds1 ART GADGET 391, PARTICLE 52, LIGHT_COLI 50,
STICK 39, SET_SE_ONESHOT 4 - ds1 is the always-on scenery/ambience layer (0x8000). Per-class range: CityLaser 2..25,
FallBuilding 11..30, PARTICLE up to 26, enemies 4..10 (one GUN_BEETLE 25).

Links (non-zero) stg0100: 1/3/11/13 CAGE + BK_SOLDIER/HINTRING/SWITCH groups; 10 HINT COLLISION 12 + SETCOLLISION 10;
60..64 SECRETKEY one each; 100..109 CityLaser (+FallBuilding / ART GADGET); 12 GUN_BEETLE x3.

Census of saved flags over all 130 layout files (26,567 entries): 0x1 all; 0x200 SAVEPOINT 27; 0x400 2; 0x800 137
(BK_TANK_COLI, E_P_COLI, E_B_COLI, HINT CONTROLLER, MOVE_FOOTING, EGG_LASTMECH, bosses); 0x10000 187 (WEAPON_CONTAINER,
STICK, BK_WINGSMALL, BREAKWALL, ...); 0x20000 506 (EGG_PAWN 328, WEAPON_CONTAINER 75, enemies); 0x40000 78 (enemies);
0x400000 4015 (ART GADGET 3277, ART GADGET ZONE 646, BK_WORM 35, BRK_OBJ 29).

## 2. The layout manager ("SetManager", 0x42C bytes)

### 2.1 Where it lives (V)

| Item | Address |
|---|---|
| holder (static) | r13+0x327C = { +0 SetManager*, +4 u8 update enabled } |
| getter | 0x800C9F6C: lazy-constructs via 0x800CBDF0 (guard byte r13+0x3278), atexit 0x803A1210(holder, 0x800CBD88), returns r13+0x3284 (= &holder) |
| ctor | 0x800CB474 (new 0x42C in 0x800CBDF0) |
| dtor | 0x800CB378 (frees pool, entries, slots); holder dtor 0x800CBD88 |
| task | SetManagerTask, vtable 0x80523D3C {+8 dtor 0x800CB7C8, +0xC update 0x800CB794}; ctor 0x800CB820 (base task 0x8004F014); created at 0x80178424 in group 6 |
| per-frame | 0x800CB794(task, dt) -> 0x800CB97C(holder): if holder+4 && mgr && [[r13+0x3EC8]+0x14] (0x801780AC, I: stage running) && [[r13+0x2B7C]+0x10] == 0 (0x800122D4, I: not paused) -> two players ([0x8057E760+0xB2]) 0x800CAA2C(mgr, p0, p1) else 0x800CAC08(mgr, p0); p = 0x80010244(0x80009548(), i) = camera-list element i object +8 (camera list 0x8056FED4 at r13+0x2B34) |
| stage unload | 0x801781B8 -> 0x800CBC08: factory f2 (+8) for used factories (0x800CA720), clear (0x800CAF38), free setid (0x800CAEF0), r13+0x3274 = 0 (0x800C9ADC) |

### 2.2 Fields (V)

| Off | Type | Meaning | Written by |
|---|---|---|---|
| 0x000 | s32 | entry count (next free entry) | 0x800CB1A4, 0x800CAF38 |
| 0x004 | s32 | factory count (all, 286) | 0x800CA780 |
| 0x008 | s32 | live count this frame (entries with 0x2) | 0x800CAC08 / 0x800CAA2C |
| 0x00C | s32 | setid record count (302) | 0x800CB044 |
| 0x010 | rec* | setid records (count x 0xC, heap 0x80049BB8(size,2)) | 0x800CB044 |
| 0x014 | Factory** | factory table 0x8052C1A0 (null-terminated) | 0x800CBD60 |
| 0x018 | Slot* | slots[0x800] x 0x20 (new[] 0x803A16B4, ctor 0x800CB5D8, dtor 0x800CB424) | ctor |
| 0x01C | Slot*[0x100] | link-list heads, one per link id (0x800CA68C / 0x800CB948 = get head) | 0x800CAE24 |
| 0x41C | Entry* | entries[0x800] x 0x2C = 0x16000 bytes (ctor 0x800CB598 zeroes) | ctor |
| 0x420 | u8* | misc pool, 0x10000 bytes (0x80049BB8(0x10000, 2)) | ctor |
| 0x424 | s32 | misc bytes used | 0x800CB1A4 |
| 0x428 | u32 | bit 0 = restricted (event): only 0x8000 entries are processed | set 0x800CA458 / clear 0x800CA430 |

Slot i is bound to entry i once (ctor 0x800CB530 loop and 0x800CAF38): slot.entry = &entries[i]. Reset of a stage
(0x800CAF38): free misc of entries whose saved has 0x200000, memset entries, reset slots (0x800C9DD4), count = used = 0.

Other statics: r13+0x3270 f32 leave margin = 50.0 (0x805F415C), r13+0x3274 u32 global leave flags, both set by
static init 0x800CA324 (no direct caller - I: static-ctor table). Constants 100.0 0x805F4148, 10000.0 0x805F4160.

### 2.3 Manager-wide operations (V code, I trigger)

| Func | Wrapper (holder) | Effect | Caller |
|---|---|---|---|
| 0x800CA57C / 0x800CA640 | 0x800CBB40 / 0x800CBBC8 (holder+4 = 0) | every entry flags \|= 0x4 (all leave), update off, then 0x8001C4F8 / 0x801EB5D4 | 0x802046BC / 0x802062AC |
| 0x800CA4B4 | 0x800CBA3C (holder+4 = 1) | checkpoint restart: per entry, skip if saved or live has 0x200; saved 0x400 -> flags = saved & ~1; live 0x2000000 -> flags = saved \| 0x2000000; else flags = saved and delete slot+0x14; rebuild links | 0x802046BC (I: death -> restart at checkpoint) |
| 0x800CA5C8 | 0x800CBB80 (holder+4 = 1) | full reset: flags = saved for all, delete all slot+0x14, rebuild links | 0x802062AC (I: restart stage) |
| 0x800CA480 | 0x800CB8F0 | every entry with 0x8 gets 0x200 (commit kills) | 0x80119474 in 0x80119350 (I: SAVEPOINT pass) |
| 0x800CA430 + 0x800C9A8C | 0x800CB890 | clear mgr+0x428 bit 0 and r13+0x3274 bit 0 | 0x80173E40 (event task dtor 0x80173DC0) |
| 0x800CA458 + 0x800C9AB4 | 0x800CB8C0 | set both bits | 0x80173F38 (EventManager task ctor 0x80173EF4) |
| 0x800CA350(mgr, cb, int* link) | 0x800CB864 | for every slot whose saved has 0x1 and (link < 0 or entry link == link&0xFF): call cb(slot, link*) until it returns 0 | 0x801ED758/0x801ED810/0x801ED8F0, 0x8027CC28 (SET GENERATOR), 0x80332254 (I: GUN_BUILDING) |
| 0x800CAE24(mgr, link) | - | rebuild list head[link] from slots with flags 0x1 and entry link == link | spawn loop (respawn), 0x800CAEAC (all 256) |
| 0x800CADB0(mgr, link, slot) | 0x800CB91C | unlink slot | 0x800C9F1C |

## 3. Entry and slot in memory

### 3.1 Entry (0x2C, same layout as disc after swap; V)
See 1.2. In memory +0x18 is the live flags, +0x1C the saved flags, +0x28 a pointer into mgr+0x420 (or a heap
block when saved 0x200000 - freed by 0x800C9FD0 / 0x800CAF38; I: owned misc).

### 3.2 Flags word (+0x18; +0x1C holds the authored copy)

| Bit | Meaning | Set by | Cleared / tested by | V/I |
|---|---|---|---|---|
| 0x1 | enabled: may spawn / exists | load (saved), respawn 0x800CAC98.., 0x800CAD20 | release 0x800C9F1C (kill/collect); restart with saved 0x400; tested by spawn loop, link list 0x800CAE24, enumerate 0x800CA350 (saved), 0x801ED9F8 | V |
| 0x2 | live (object exists) | 0x800CA0B8 from base ctor 0x8015AE00 | 0x800C9FD0 (base dtor 0x8015AD8C); spawn 0x800CA8D8 refuses; loop counts at +8 | V |
| 0x4 | asked to leave | 0x800CA57C/0x800CA640 (all), 0x801ED948 (link off, if live) | 0x800CA0B8, 0x800C9FD0; leave test 0x800C9E4C | V |
| 0x8 | killed for this life | 0x800C9C44 (10 sites), 0x800C9EEC (= \|0x8 + release; base method 0x8015AB4C) | promoted to 0x200 by 0x800CA480 | V |
| 0x40 | out of range while destroyed (respawn armed) | loop (0x10000 path, not in range), 0x8027CCE8 (SET GENERATOR) | respawn path clears; spawn 0x800CA8D8 clears (& 0xFFF7FFBF) | V |
| 0x100 | editor "entry changed" (reload pos/misc) | - | 0x800CA0FC test / 0x800CA0E0 clear, only after 0x800CA0F4 which always returns 0 | I (dead editor path) |
| 0x200 | killed permanently (past a checkpoint); also authored on SAVEPOINT | 0x800CA480, SAVEPOINT 0x80117E0C, SECRETDOOR 0x8011BDA4 | BK_LARVA 0x80189F00; restart 0x800CA4B4 skips; respawn carries 0x208 | V |
| 0x400 | (saved) do not come back after restart | disc | 0x800CA4B4 | V |
| 0x800 | never leaves / always in range | disc (factory +0x14 default for coli/boss classes) | 0x800C9D54 -> 0; 0x800CA970 (with 0x80000) -> 1 | V |
| 0x1000/0x2000/0x4000/0x8000 | layer cmn/nrm/hrd/ds1; 0x8000 = processed while restricted and ignores the global leave flag | 0x800CB0EC | 0x800CAC08, 0x800C9E4C | V |
| 0x10000 | respawn when player leaves and returns | disc; 0x800C9EBC (= \|0x10000 + release; base 0x8015ABA4) | respawn path 0x800CAD2C clears (& ~0x10040); 0x800C9C58 | V |
| 0x20000 | held: may not spawn | disc; 0x801ED954 (link off) | 0x801EDA14/0x801EDA74 (link on), 0x8027CD14 (SET GENERATOR); 0x800CA8D8 refuses | V |
| 0x40000 | generator target | disc (enemies) | 0x8027CFD4: SET GENERATOR picks the first slot of its link with it | V code, I name |
| 0x80000 | force in range (one shot) | (no setter found) | 0x800CA970 -> 1; cleared on spawn by 0x800CA8D8 | V clear, I setter |
| 0x200000 | misc heap-owned (saved) | disc +0x18 only on some entries (overwritten on load) | 0x800C9FD0, 0x800CAF38 free +0x28 | V code, I name |
| 0x400000 | class state bit (saved for ART GADGET: copied to obj+0x80 bit 1, 0x800E4BB8) | disc; SAVEPOINT 0x80117C24, SECRETDOOR 0x8011BD98, 0x8022B684 | SECRETDOOR 0x8011BC60, BK_WORM 0x801925F0 | V sites, I meaning |
| 0x800000 | re-enable request (respawn now) | 0x801EDA08/0x801EDA68 (link on, entry not enabled) | respawn path: flags = saved \| 1, & ~0x10040 | V |
| 0x1000000 | class bit (GUN_SOLDIER) | 0x8019E5A4 | 0x8019E590 | V site |
| 0x2000000 | survives restart (flags = saved \| it) | BK_LARVA 0x80189F0C | 0x800CA4B4 | V |
| 0x20000000 | data error (misc size mismatch, or base method 0x8015ABD0 -> 0x800C9A78) | 0x800CA150/0x800CA1A4, 0x800C9A78 | 0x800CA150/0x800CA1A4 on success | V code, I name |
| 0x40000000 | detach frees slot+0x14 and owned misc | (no setter found) | 0x800C9FD0 | V test, I setter |
| 0x80000000 | disc only (all entries) | - | overwritten | V |

Spawn loop per slot (0x800CAC08; 0x800CAA2C identical with two points, either may spawn) V:
restricted && !(0x8000) -> skip; 0x2 -> count; 0x1 && slot+0x10 >= 0 -> 0x800CA8D8; else if saved 0x1:
0x40 -> flags = saved \| (flags & 0x208) \| 1, clear 0x40, relink; elif 0x800000 -> flags = saved \| 1, clear 0x10040,
relink; elif 0x10000 && !in_range -> \|= 0x40.

### 3.3 Slot (0x20; ctor 0x800CB5D8, reset 0x800C9DD4) V

| Off | Type | Meaning |
|---|---|---|
| 0x00 | Entry* | bound entry (fixed) |
| 0x04 | Slot* | link list prev (head's +4 = tail) |
| 0x08 | Slot* | link list next (0 = end) |
| 0x0C | Object* | live object (0x800CA0B8 sets, 0x800C9FD0 clears) |
| 0x10 | s32 | factory index into mgr+0x14, -1 = no class |
| 0x14 | obj* | persistent per-entry state object (virtual dtor +8), set by 0x800C9D18 (20 sites), survives the object; deleted by restart/reset (0x800C9C7C) or detach with 0x40000000; enemy group code dynamic_casts it (0x801A26B8, types 100 GUN_SOLDIER, 0x91) |
| 0x18 | 8 bytes | weak handle to the object (init 0x80051D4C, reset 0x8008D78C, read 0x8005223C/0x800521AC in 0x800C9B24 to hand SET GENERATOR's effect to the live object) |

### 3.4 Release paths (who ends an object) V

| Func | Does | Base method (vtable 0x8052BFFC class) |
|---|---|---|
| 0x800C9FD0(slot) | clear 0x2, 0x4; if 0x40000000: delete +0x14, free owned misc, clear it; slot+0xC = 0; reset handle | base dtor 0x8015AD8C (object leaves range or is deleted) |
| 0x800C9F1C(slot) | clear 0x1, 0x800C9FD0, unlink from its link list | 0x8015AB78 |
| 0x800C9EBC(slot) | \|= 0x10000, then 0x800C9F1C | 0x8015ABA4 |
| 0x800C9EEC(slot) | \|= 0x8, then 0x800C9F1C | 0x8015AB4C |
| 0x800C9E4C(slot) | leave test: (r13+0x3274 bit 0 && !0x8000) \|\| 0x4 | 0x8015AC3C, base gate 0x8015AC7C |

## 4. setid.bin (V format; V filter; I stage index mapping)

`files/setid.bin` 3632 bytes, little-endian: u32 0, u32 count = 302, then count x {u32 class id, u32 w0, u32 w1}
(0x800CB044: swap 0x800CB728 / 0x800CB6EC, copy to mgr+0x10). The 64 bits are one per **stage index**, MSB first:
bit b is w[b/32] & (1 << (31 - b%32)) (0x800CA828..0x800CA858). 0x800CA7F0(mgr, table, stageIdx): clear every
factory's +0x1B bit 0, then set it for each record with the stage's bit whose id matches factory +0x18. Only the
used factories get their preload f1 (0x800CA780) and release f2 (0x800CA720). Not per mission: nrm/hrd is chosen by
[0x8057E760+0x125]; missions do not filter entries.

stageIdx = [0x8057E760+0xAC]. Mapping by exact class coverage (setid_map.py): 5 stg0100, 6 0200, 7 0201, 8 0202,
9 0300, 10 0301, 11 0302, 12..16 0400..0404, 17..21 0500..0504, 22..26 0600..0604, 27 0700, 28 0210, 29 0310,
30..32 0410..0412, 33 0510, 34 0511, 35..43 0610..0618, 44 0710, 45..47 0800..0802; 0..4 presumably stg0000..0005
(no layouts). Gaps: stg0300 places FIRE and stg0511 SET_VEHICLE without the setid bit - they still get a factory
index (0x800CA6B4 ignores +0x1B) but no preload (original data quirk). Markers (0x2580..0x2583 etc.) have all bits.
17 ids in setid have no factory (e.g. 0x32, 0x52..0x55, 0x258B) - ignored.

## 5. Factories (table 0x8052C1A0, 286 pointers + 0)

Struct (V by offsets used; I where marked):

| Off | Field | Use |
|---|---|---|
| 0x00 | char* name | V |
| 0x04 | f1 preload() | 0x800CA780 at load if used (e.g. RING 0x80112790 -> 0x8006B634(0xDA, 1)) V |
| 0x08 | f2 release() | 0x800CA720 at unload if used V |
| 0x0C | create(slot) | 0x800CA8D8 (r3 = slot) V |
| 0x10 | 0 | - |
| 0x14 | default flags (0x80 markers/unnamed, 0x200 SAVEPOINT, 0x800 coli/boss) | I editor default; no retail reader via the table |
| 0x18 | u16 layout id (= entry type) | V (0x800CA6B4, 0x800CA7F0) |
| 0x1A | u8 kind 1 gadget (186) / 2 enemy (28) / 4 other (72) | I; no reader via the table |
| 0x1B | u8 bit 0 = used in this stage | V |
| 0x1C | u8 default range (10 gadgets, 5 or 10 enemies, 20/30 a few) | I |
| 0x20 | param descriptor table (0x28-byte rows) | V: 0x800CA118 returns misc only if non-null |
| 0x24 | type-info pointer (marker structs: end-marker name, +0x28 a function) | I |

The table is referenced only by 0x800CA118, 0x800CBC08, 0x800CBC54 (V, address scan). Duplicate id: 0x258E =
EnemyEntryStart and EnemyEntryEnd (first wins in lookups).

create (RING 0x80112738, V): `obj = new(0x68)`; parent = 0x801EB614(0x8001C4F8(), 0xB); class ctor
0x80114564(obj, parent, slot) -> ... -> base SetObject ctor 0x8015AE00(obj, parent, slot): base task 0x8004F014,
obj+0x18 = vtable 0x8052BFFC, obj+0x28 = slot, 0x800CA0B8(slot, obj) (flags \|= 2, &= ~4, slot+0xC = obj).

Base SetObject fields used by the manager (V): +0x04 u16 bit 0 = done (task removed), +0x18 vtable
{+8 dtor 0x8015AD8C, +0xC exec 0x8015AD28 (gate +0x1C, then update +0x10), +0x14 0x800CC004, +0x1C gate 0x8015AC7C},
+0x28 Slot*. Every slot accessor takes the slot: 0x800CA2FC pos, 0x800CA2BC rot (rad), 0x800CA200 range*100,
0x800C9D20 range*100+100, 0x800CA10C type, 0x800CA238/48 link, 0x800CA2AC flags, 0x800CA258 saved, 0x800CA268
(flags & m) == m, 0x800CA298 set, 0x800CA284 clear.

## 6. What gates whether an entry may exist

| Gate | Mechanism | V/I |
|---|---|---|
| stage | setid bit -> preload; layout file set chosen by stageNo and [0x8057E760+0x125] | V |
| range | in_range 0x800CA970 vs camera(s); leave 0x800C9D54 (+50) | V |
| event | EventManager ctor 0x80173EF4 sets mgr+0x428 bit 0 (only 0x8000 entries processed) and r13+0x3274 bit 0 (every object without 0x8000 leaves via 0x800C9E4C); dtor 0x80173DC0 clears both | V code, I class name |
| hold 0x20000 | entry may not spawn until released | V |
| link groups | link id +0x22 (0..255) with one intrusive list per id (mgr+0x1C), rebuilt from enabled entries. API: 0x801ED720(link) off: live -> 0x4, all -> 0x20000; 0x801ED8B8(link) on: not enabled -> 0x800000, release 0x20000; 0x801ED7C0(link, point, r) on within radius (y ignored) and in range. Callers: BLACK_BULL_0210 area 0x80234B9C/0x80234E5C/0x802352EC/0x80235310, DEVIL_DOOM area 0x80313BF0/0x803118CC | V code, I class attribution (nearest factory) |
| SET GENERATOR | 0x8027CC08: finds the link's 0x40000 slot (0x8027CFB8), releases 0x20000 or arms respawn 0x40, spawns an effect handed to the object (0x800C9B24) | V |
| link-head users | 0x800CB948 from WEIGHT/WARP_HOLE 0x8012ADD8, enemy group 0x801A26FC.. (walks all 256 lists, dynamic_cast of slot+0x14), 0x801DA670, ELEC_PANEL 0x8025C1F8, E_P_COLI 0x80261170 | V sites |
| checkpoint | SAVEPOINT commits 0x8 -> 0x200 (0x800CA480); restart 0x800CA4B4 keeps 0x200 entries dead and 0x400 entries off | V |
| ART GADGET switch | link > 0 -> obj+0x80 bit 2 (hidden until switched, 0x800E52C4) | V (see object_distance.md 7) |

Open: who sets 0x80000 and 0x40000000; reader of factory +0x14/+0x1A/+0x1C; exact semantics of
[0x8057E760+0x125] (hard mode) and how +0xAC is derived from the stage number; the ART GADGET obj+0x80 bit 1 consumer.
