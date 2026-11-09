# Hide rules: when a live object's model is NOT drawn (static RE, 2026-10-29)

Purpose: a far stand-in (drawn for a layout entry whose object is not spawned) must not show a model the game would
keep hidden. This file lists every rule found, with addresses, whether it can be evaluated from the layout entry plus
the switch table (and other global tables) without the object existing, and what a stand-in should do.

Markers: **V** = read in the disassembly (doldis), **I** = inferred meaning (names, intent). Class attribution uses the
class vtables (vt+0x10 = update) taken from each factory constructor, not the nearest-factory heuristic (that
heuristic mis-assigns e.g. 0x802F39FC, which is CITY_BOMB_BIG's update, not HINT CONTROLLER's).

Link id = layout entry byte +0x22 (0x800CA238 u32 / 0x800CA248 u8) unless stated "misc word n" (entry misc,
read by 0x800CA1A4). `switch[x]` = 0x80169430(table, x).

---

## 1. The switch table

### 1.1 Object and layout (V)
| item | value |
|---|---|
| static holder | r13+0x329C (0x805EF79C), guard byte r13+0x3298, cached ptr r13+0x32A0; getter 0x800CBA84 (registers dtor 0x800CBB04 once via 0x803A1210) |
| array | holder+0 -> 256 entries x 8 bytes (alloc 0x810 = 0x10 header + 0x800, 0x803A12A4 new[]) |
| entry +0 | s32 value (only `== 1` is ever tested by readers) |
| entry +4 | u32 bits: bit 0 = written this checkpoint period (set by 0x80169460), bit 1 = committed at a savepoint |
| range | index < 0 or >= 0x100, or no array -> read returns 0, writes ignored |

### 1.2 API (V)
| addr | function |
|---|---|
| 0x80169684 | create: new[] 256 entries, entry ctor 0x801696F4 (value 0, bits cleared). Only caller: GadgetCommonDataStart f1 0x8015DC98 (0x8015DCB0) = **stage load: every switch is 0** |
| 0x8016963C | destroy (GadgetCommonDataEnd 0x8015DC14) |
| 0x80169430 | get(table, link) |
| 0x80169460 | set(table, link, v) **and set bit 0** (flagged write) - 13 sites |
| 0x801694B4 | set(table, link, v) raw, **no bits** - 33 sites |
| 0x801694DC | commit: every entry with bit 0 gets bit 1. Only caller SAVEPOINT 0x80119484 |
| 0x80169558 | checkpoint restart: every entry **without bit 1** -> value 0, bits 0. Caller 0x800CBA68 (in 0x800CBA3C, death -> restart at checkpoint 0x802046BC) |
| 0x801695D8 | full reset: all values 0, bits 0. Caller 0x800CBBB0 (0x800CBB80, restart stage) |

Consequences (V): raw writes (0x801694B4) are never committed, so a checkpoint restart clears them; a flagged write
survives a checkpoint restart only if a savepoint was touched after it. Nothing writes the table at stage load other
than object constructors that happen to spawn (GUN_BUILDING ctor writes 1, BA Switch ctor copies its own table, ELEC_ACCESS
ctor writes 1 when its persistent state is 3, MAGMA GUARD init writes 1 when its entry has flag 0x400000).

There is no direct access to r13+0x32A0/0x329C outside 0x800CBA84/0x800CBAB8 (r13 displacement scan).

### 1.3 Every writer (V code, I trigger meaning)
| class | site | link source | value | kind | trigger |
|---|---|---|---|---|---|
| POWER_DEVICE_CAGE | 0x800D2A94 | +0x5C (entry +0x22) | 1 | flagged | destroyed (also persistent state +4 = 1, 0x800C9C44) |
| POWER_DEVICE_NAKED | 0x800D48C4 | +0x70 (+0x22) | 1 | flagged | destroyed (state obj +8 = 1) |
| CityChaosEmerald | 0x800E0020 | +0x40 (+0x22, 0x800E00E8) | 1 | flagged | collected (also gm byte 0x8057E821 = 1) |
| CityLaser | 0x800E1298 | +0x3C (+0x22) | 1 | raw | laser strike state (0x800E1024 case 3) |
| SECRETKEY | 0x8011C3C0 | +0x4C (+0x22) | 1 | raw | collected, then entry removed |
| SETCOLLISION | 0x8011D0CC / 0x8011D184 | misc word 5 (+0x54) | 0 / 1 | raw | player touch, two variants |
| SWITCH | 0x801236BC/D0/E4, 0x801237D8..0x80123874 | +0x5C (+0x22) | toggle / 1 | raw | pressed; type misc word 0 (+0x58): 0 toggles, 1/2 set 1; 0x80123748 resets to 0 (type 1 expiry) |
| TARGETSWITCH | 0x80125044 | entry +0x22 | 1 | raw | destroyed, entry removed |
| ELEC_SECURITY | 0x80158470 | +0x58 (+0x22) | 1 | raw | condition 0x8026100C |
| CandleStick | 0x80165D04 (via 0x80165C90 from 0x8012D95C) | group id | 1 | raw | lit-candle count reaches the group's target |
| enemy groups | 0x801A2614 (0x801A250C, callers 0x8018A9CC, 0x8018ABE0, 0x801A8A20) | killed enemy's entry +0x22 | 1 | flagged | live count of that link group drops to 0 (all enemies of the group killed) |
| ELEC_ENTRANCE | 0x801F8D3C | +0x38 (+0x22) | 1 | raw | if not already 1 |
| ELEC_WALLSWITCH | 0x801FA4E4 | +0x3C (+0x22) | 1 | raw | pressed (0x801FA308) |
| DEFENSE_SYSTEM | 0x80224504 | +0x6C (+0x22) | 1 | flagged | destroyed |
| MAGMA GUARD | 0x8022B584 / 0x8022B5F4 | +0x4C (+0x22) | 1 | raw | init with entry flag 0x400000 / event |
| SUPER COMPUTER | 0x8022E634 | entry +0x22 | 1 | flagged | destroyed |
| GiantLantern | 0x80236F84 | +0x74 (+0x22) | 1 | flagged | lit (I) |
| Cream | 0x80134A90 | +0x6D (+0x22) | 1 | flagged | event (I) |
| Chao | 0x8012ECD4 | +0x55 (+0x22) | 1 | flagged | found (I) |
| GravityChangeSwitch | 0x802470D4 | +0x5C (+0x22) | 1 | raw | used |
| GravityChangeZone | 0x8024826C | +0x64 (+0x22) | 1 | raw | entered |
| ELEC_ACCESS | 0x80249698 (ctor, persistent state 3), 0x80249A84 | +0x3C (+0x22) | 1 | raw | accessed |
| ELEC_PANEL | 0x8025A524 (dtor 0x8025A4E4, if +0x30 == 1 -> 0), 0x8025B994 (panel group done -> 1) | +0x80 (+0x22) | 0 / 1 | raw | |
| SET GENERATOR | 0x8027C580 | entry +0x22 (> 0) | 1 | raw | generated set finished (0x8027CA18) |
| CITY_BOMB_BIG | 0x802F3C2C | +0x68 (+0x22, 0x802F3980) | 1 | flagged | defused/exploded |
| CITY_BOMB_SMALL | 0x802F52E8, 0x802F57BC | +0x6C (+0x22) | 1 | flagged | defused/exploded |
| BA Switch | 0x80302E5C/0x80302F08 (toggle), 0x803038F8/0x80303928 (ctor) | +0x78 (+0x22) | 0 / 1 | raw | hit; ctor copies BA table 0x80585D48 |
| SUCTION PILLAR | 0x8030471C | +0x70 (+0x22) | 1 | raw | once (+0x68 bit 0) |
| BombingWall | 0x80316388 | +0x54 (+0x22), only if >= 1 | 1 | flagged | destroyed |
| GUN_BUILDING | 0x8032CECC (ctor) / 0x8032C59C | +0x40 (+0x22) | 1 / 0 | raw | spawn writes 1; 0 when its timer runs out |

Other link-driven systems (not the switch table, V from spawn_data.md 2.x): link groups 0x801ED720 (off: live ->
flag 0x4, all -> hold 0x20000), 0x801ED8B8 (on: re-enable 0x800000, release 0x20000), 0x801ED7C0 (on within radius);
SET GENERATOR 0x8027CC08 releases its link's 0x40000 target. Held entries (0x20000) never spawn.

Second, unrelated group table: **r13+0x3B68** (256 x 0x1C, field +0x14), used only by PARK_BALLOON/SHOOTING_PANEL code
0x8013BC34..0x8013F064; SHOOTING_PANEL gate 0x8013DD28 reads `[r13+0x3B68] + link*0x1C + 0x14 == 1` (V).

Third, BA table **0x80585D48**: 4 records x 12 bytes {s32 link, u8 on, u8 saved}; get 0x80302394, set 0/1
0x8030227C/0x80302308, restore saved->on 0x803023D8 (from 0x8030220C). Used by BA Switch and BA Shield (V).

---

## 2. Every switch-table reader (59 sites of 0x80169430)

### 2.1 Readers that change what is drawn
| class | rule | addresses | computable from layout + tables? | stand-in |
|---|---|---|---|---|
| **ART GADGET / ART GADGET ZONE** | entry link > 0 sets obj+0x80 bit 2 (0x800E4BFC). Each update: link > 0 and switch[link] == 1 -> hide-all + pause particles (0x800E530C, 0x80167054); else show-all (0x800E5338) | 0x800E52C4..0x800E5348 | **yes** | skip when link > 0 and switch[link] == 1 |
| **SWITCH** | two holders: switch[link] == 1 -> show +0x48, hide +0x40; else show +0x40, hide +0x48 (0x8012356C, also from ctor 0x801244A8) | 0x80123588..0x801235B4 | **yes** | draw holder +0x48's model when switch == 1, else +0x40's |
| **HINTRING** | in state 1: visible iff link == 0 or switch[link] != 1 (+0x6C bit 0); hidden (bit 2) when link > 0 and switch == 1 | 0x8010A660..0x8010A720 | **yes** | skip when link > 0 and switch[link] == 1 |
| **RING** | link > 0: group created waiting (+0x60 = 1, +0x64 bit 0, collisions off 0x801127B8); whole group skipped by the draw while +0x64 bit 0; switch[link] == 1 -> state 2, collisions on 0x80112844 | 0x80114068..0x801140DC | **yes** | skip when link > 0 and switch[link] != 1; also skip collected rings (state copied to slot+0x14 by dtor 0x801141D8 / 0x80112588) |
| **CIRCUS_BALL** | misc word 0 (+0x54) != 0: hidden at first update (0x8012FF38) until switch[link] == 1, then show + motion + state 2 (0x8012FF58) | 0x8012FF0C..0x8012FF78 | **yes** | skip when misc0 != 0 and switch[link] != 1 (after the switch it moves; position not static) |
| **BA GUNSHIP** | state 0 every update: hide-all (0x802FF098); goes to state 2 when link <= 0 or switch[link] == 1; state 2 counts +0x8C down, then show + motion + warp sound 0x792E (0x802FF12C) | 0x802FF050..0x802FF190 | partly (switch yes, warp-in delay is per spawn) | skip when link > 0 and switch != 1; it warps in on every spawn, so a stand-in shows something the player would see appear - recommend no stand-in |
| **BA Switch** | holders +0x40 (off) / +0x48 (on) swapped by 0x803025E0 (mode 1 off, mode 2 on); ctor picks from BA table 0x80585D48 and writes the switch | 0x80302E28, 0x803038DC..0x80303944 | **yes** (BA table) | on model iff BA record(link).on, else off model |
| **SET_VEHICLE** | the vehicle is created only when link == 0 or switch[link] == 1 (0x8011E624); otherwise the setter deletes its vehicle (0x8011E564) and kill-respawns its entry (0x800C9EBC) | 0x8011E638..0x8011E678 | **yes** | skip when link > 0 and switch[link] != 1 |
| **ELEC_BARRIER** | ctor: switch[link] == 1 -> object deletes itself (0x802641F0); update: switch == 1 -> state 1, colour fade (0x80155050), then entry removed (0x8015AB78) | 0x80263EA4, 0x802641DC | **yes** | skip when switch[link] == 1 |
| **ELEC_ACCESS** | ctor: switch != 1 -> every clump hidden via 0x8015FE5C(holder, 0x80248E64, 0) (rpATOMICRENDER cleared); switch == 1 / accessed -> shown (0x80248DCC, arg 1) | 0x80249740..0x80249870, 0x80249A44 | **yes** | draw only when switch[link] == 1 |
| **FallBuilding** | state 0: switch[+0x2C link] == 1 -> fall (+0x10 bit 0, 0x800E30A4), persistent state +4 = 1 (0x800E3370). Respawn with state 1 re-instances the fallen form and hides all (0x800E3F20); later show 0x800E3404 under +0x10 bit 1 (fallen-form visibility NOT fully resolved) | 0x800E3074..0x800E30A4, 0x800E3ECC..0x800E3F20 | **yes** (switch + slot+0x14 state) | draw the standing model only when switch[link] != 1 and persistent state == 0; otherwise skip |
| **BombingWall** | state 0: link >= 1 and switch[link] == 1 -> state 2 -> next update explodes: hide-all (0x80316364), switch set, killed for life (0x800C9EEC) | 0x803163A4..0x803163CC | **yes** | skip when link >= 1 and switch[link] == 1, or entry killed |
| DEFENSE_SHIELD | update: switch[+0x5C] == 1 -> +0x58 bit 1 -> motion state 3 (0x80222B1C(.., 3)); ctor: switch == 1 or persistent value -> state 3 | 0x8022321C..0x802232A8, 0x80223584 | yes | pose changes (shield lowered), model not hidden (I); stand-in default pose, or skip when switch == 1 (UNVERIFIED look) |
| FAN / FAN STAGE | switch[misc word 9 (+0x84)] (if >= 0) == 1 -> +0x98 bit 2 (blowing) else cleared. **Visibility is static**: model hidden iff misc word 8 (+0x80) == 0 (ctor 0x80104018, reparam 0x80103980) | 0x801029AC..0x801029E4 | **yes** (static) | skip when misc word 8 == 0 |
| ELEC_FAN | switch[+0x4C] == 1 or link 0 -> +0x50 bit 2 (running) | 0x80266FC8..0x80267010 | yes | no visibility change |
| ELEC_LIGHT | value stored to +0x50 each frame; rising edge in state 0 starts aiming (0x80154C60). Its part mask callback 0x80155184 (via 0x8015FE5C) hides parts 0-2 in damage state 3 (0x80154D1C) and shows part 3 in state 5 (0x80154BC8); ctor shows parts 0-2 (0x80155DB0) | 0x80154C60, 0x80154CEC, 0x80155DA0 | switch yes; damage states no | draw parts 0-2 (ctor set) |

### 2.2 Readers with no effect on visibility (motion, collision, sound or logic only)
| class | site(s) | link | effect of switch == 1 |
|---|---|---|---|
| BOMB | 0x800EF5FC | +0x8C (+0x22) | state +0x88 = 5 (explode) |
| CAGE | 0x800F26C4 (setup), 0x800F2B88 | +0x58 (+0x22) | state 2: opening motion (0x80160EFC) |
| SETCOLLISION | 0x8011DD30, 0x8011DD80 | misc word 5 | collision state 2/3; no model |
| SWITCH | 0x801236A4, 0x801237C0, 0x80123818, 0x8012385C | +0x22 | toggle logic (writer) |
| WEIGHT | 0x8012B774, 0x8012B79C, 0x8012B934 | +0x90 (+0x22) | movement states |
| DOOR | 0x80144D38 | +0x7C (+0x22) | opens (vfuncs +0x20/+0x24) |
| ELEVATOR | 0x80148A80 | +0x78 (+0x22) | moves |
| MOVE_FOOTING | 0x8014B7B8 | +0xF0 (+0x22) | starts moving |
| vehicles (ride check) | 0x801C8C8C | [veh]+0x18 | vehicle with +0x10 bit 2 usable only when switch == 1 |
| ELEC_CIRCUIT | 0x801F7DAC, 0x801F7E00 | +0x3C (+0x22) | animation start (0x80162BE8 / 0x80162E4C) |
| ELEC_ENTRANCE | 0x801F8D20 | +0x38 | writer |
| ELEC_WALL | 0x801F94D0 | +0x48 (+0x22) | opens (state 2) |
| ELEC_WALLSWITCH | 0x801FA4CC | +0x3C | writer |
| HINT COLLISION | 0x80208608, 0x80208678 | +0x74 (+0x22) | trigger state; no model |
| ELEC_ACCESS | 0x80249A68 | +0x3C | writer + activation (see 2.1) |
| BASE DOOR | 0x802571E8 | +0x88 (+0x22) | state 5, opens via 0x80256AA4(.., 1) + motion (its hide/show in 0x80256AA4 is an internal open/closed part swap, not a hide of the door) |
| ELEC_PANEL | 0x802599D4, 0x80259D14, 0x80259D5C, 0x8025A274, 0x8025A3D0 | +0x80 (+0x22) | panel logic/timers |
| E_P_COLI | 0x8026064C | +0x6C (+0x22) | collision state |
| FootingRoll | 0x80261798, 0x80262D98 | +0x80 / +0xC0 (+0x22) | starts rolling |
| MAGMA | 0x8027C0D8 | +0x54 (+0x22) | state 2 (rise) |
| SET GENERATOR | 0x8027C518 | +0x3C | generator on/off |
| PLANE RAIL | 0x802AC0F8 | +0x4C (+0x22) | state 1 |
| FALL_LARGE | 0x8032AFD8 | +0x44 (+0x22) | fall motion |
| FALL_HIWAY | 0x8032BC14 | +0x44 (+0x22) | fall animation |
| SET_SE_LOOP | 0x8033E28C | +0x58 (+0x22) | sound off when switch == 1 |
| BombingWall | 0x803163B8 | see 2.1 | |

All 59 sites are covered by 2.1 + 2.2.

---

## 3. Every caller of hide-all 0x80162B74 / show-all 0x80162B00 / blink 0x80162A70 not covered above

| class | addresses | rule | computable? | stand-in |
|---|---|---|---|---|
| EggFleet (sub-objects 0x800CC8AC, 0x800CD318, 0x800CF0B4) | hide 0x800CC938, 0x800CD770, 0x800CF1E8/F0; show 0x800CD738/77C, 0x800CF1FC..230 | timer show (+0x38 bit 3); else hidden while game word 0x8057E7F0 (gm+0x90) bit 1, shown otherwise; one state hides + plays motion | gm bit: yes (global) | skip while gm+0x90 bit 1 |
| POWER_DEVICE_NAKED | 0x800D49A8 / 0x800D497C, 0x800D49B4; destroyed 0x800D44C4 (0x800D425C state 1) | same gm+0x90 bit 1 rule; destroyed state (persistent, 0x800D4D00) hides the device | yes (gm word + slot+0x14) | skip while gm bit 1 or persistent destroyed |
| POWER_DEVICE_CAGE | 0x800D44C4 path via 0x800D425C; persistent 0x800D3474 | destroyed -> hidden on respawn | yes (slot+0x14) | skip when destroyed |
| RainEffect | hide in ctor 0x800D578C; show 0x800D5450 when the player is inside its volume (0x800C5C60); hide 0x800D54F4 on exit | player-position driven | no | never draw |
| STONE_POLE | 0x800D8380, 0x800D8400, 0x800D857C | sub-object pieces hidden on break | no (damage) | draw intact |
| FallBuilding | 0x800E3F20 / 0x800E3404 | see 2.1 | | |
| FAN | 0x80103980 / 0x80103974, 0x80104018 | see 2.1 (misc word 8) | | |
| HINTRING, SWITCH, CIRCUS_BALL, ART GADGET, BA GUNSHIP, BA Switch, BombingWall | see 2.1 | | | |
| Coaster | 0x80132BFC, 0x80132DE0 (0x80132A50) | hidden in ride states, manager [r13+0x3A68] | no | draw idle |
| GONG / Monster | show 0x80138E18 (0x80138DB4, message 0) | shown on message (I: appears on gong hit) | no | UNRESOLVED initial visibility |
| ThreatObj | hide 0x801403E4 (sub-part ctor 0x80140318); show 0x8013FE44 when 0x8013F930() | sub-part hidden until condition 0x8013F930 | no (condition unresolved) | skip that sub-part |
| FIRE_VINE | second holder +0x4C hidden in ctor 0x8015080C; shown 0x8014F7FC (state 0 -> 1), hidden 0x8014F6AC | alternate (burning) model | no | draw primary holder +0x44 only |
| METEOR_L / METEOR_S | 0x8021D3BC / 0x8021EF5C | destroyed -> hidden, then removed | entry flag | skip if entry not enabled |
| ARKCANNON | 0x80227D30 (+0x68 bit 1) | destroyed | entry/state | skip if not enabled |
| BASE DOOR | 0x80256ADC/AFC/B24/B44 (0x80256AA4 mode 0/1) | open/closed part swap | switch (2.2) | closed form when switch != 1 (I) |
| WATCH CAMERA | wrappers 0x80268608/0x80268634; hide 0x80269414 (hit, 0x802693C8), show 0x802697E4 (re-appear 0x802696EC) | damage, timed return | no | draw |
| SPACEGADGET | 0x8026B3E4 | destroyed | entry flag | skip if not enabled |
| CITY_BOMB_BIG / SMALL | 0x802F3AE0 / 0x802F56F8 | exploded/defused -> hidden, switch set, removed | entry flag | skip if not enabled |
| CITYGADGET | 0x802F73B0 | destroyed (record +8 == -1) -> hidden, killed | entry flag | skip if not enabled |
| Helicopter | 0x802F8804 (+0xA4 bit 0) | destroyed | entry flag | skip if not enabled |
| blink 0x80162A70 | ART GADGET 0x800E4A90 (hit, health > 0), CAPTURE_CAGE 0x800F4048, TURNOVER/TANK 0x80126CBC, FIRE_RING 0x80135D58, SHOOTING_PANEL 0x8013E95C/0x8013EAD4/0x8013EB7C, DEFENSE WALL 0x8022A3F8, kind-4 item objects 0x800E9D50/0x800EBE20 | timed or endless toggle (holder +0x3C bits 4/5) | no (transient, near the player) | draw steady |
| holder internals | 0x80162A58 (blink end), 0x801635F0..0x8016366C (0x80163394 blink tick) | | | |

---

## 4. Other ways a live object's model is not drawn

| mechanism | addresses | effect | stand-in |
|---|---|---|---|
| entry not enabled (flag 0x1 clear) | 0x800C9F1C (remove), 0x800C9EEC (\|0x8 killed for life), 0x800C9EBC (\|0x10000 kill-respawn) | no object exists: collected, destroyed, removed (TARGETSWITCH, SECRETKEY, ELEC_BARRIER, BombingWall, CITY_*...) | **skip entries whose live flags lack 0x1** |
| held (0x20000) | disc flag; released by 0x801ED8B8 / SET GENERATOR 0x8027CD14; set by 0x801ED720 | spawn refused (0x800CA8D8) | skip held entries |
| asked to leave / event mode | flag 0x4; r13+0x3274 bit 0 during events (EventManager 0x80173EF4): every non-0x8000 object leaves (0x800C9E4C) | | skip non-0x8000 entries while r13+0x3274 bit 0 |
| holder hide-all | holder +0x3C bit 2, 0x80162B74 / 0x80162B00 (clears / sets rpATOMICRENDER per clump via 0x80038FE8 / 0x80038FBC; test 0x80162AD8) | | see sections 2-3 |
| holder blink | +0x3C bits 4/5, 0x80162A70, tick in 0x80163394 | | draw steady |
| holder fade out | +0x3C bits 7-10 (0x80163394; faded out = bit 10, test 0x8015FFDC) | alpha 0 | class-specific; none found tied to switches |
| per-clump rpATOMICRENDER through 0x8015FE5C(holder, cb, arg) | ELEC_ACCESS 0x80248E64 (all clumps), ELEC_LIGHT 0x80155184 and ELEC_BOMB 0x8026432C (part mask: bit n = part n, bit 15 = show) | | ELEC_ACCESS see 2.1; ELEC_LIGHT/BOMB draw ctor parts |
| non-holder model sets | 0x8022FFA8 (SUPER COMPUTER wrapper 0x8022F014), 0x800641A4 (same code, weapon area) | all clumps hidden | skip SUPER COMPUTER when destroyed (entry flag) |
| persistent per-entry state (slot+0x14, set by 0x800C9D18) | EggFleet, POWER_DEVICE_CAGE/NAKED, RainEffect, STONE_STATUE, FallBuilding, RING, CandleStick, Chao, Cream, BK_LARVA, GUN_SOLDIER, DEFENSE_SHIELD, DEFENSE_SYSTEM, GiantLantern, ELEC_ACCESS, ELEC_PANEL, ELEC_BOMB, SET GENERATOR, CITYGADGET | object respawns in its saved state (fallen, destroyed, collected rings...) | read the state object where a rule above names it |
| global progress word | 0x8057E7F0 (gm+0x90) bit 1, restored at checkpoint by 0x80178F70 | EggFleet, POWER_DEVICE_NAKED hidden | skip while set |
| BA table | 0x80585D48 | BA Switch model, BA Shield open/close motion | read the table |
| shooting group table | [r13+0x3B68] + link*0x1C + 0x14 | SHOOTING_PANEL gate | n/a (near-only) |
| hidden until near (enemies) | draw_paths_enemies.md | | known |
| created hidden until a trigger | RING (link), CIRCUS_BALL (misc0), BA GUNSHIP, ELEC_ACCESS, RainEffect (volume), ThreatObj sub-part, FIRE_VINE alternate, FallBuilding fallen form | | per rows above |

---

## 5. Summary table (class -> rule -> computable -> stand-in)

| class | rule | computable | stand-in does |
|---|---|---|---|
| ART GADGET (+ZONE) | hidden iff link>0 && switch[link]==1 | yes | same test |
| HINTRING | hidden iff link>0 && switch[link]==1 | yes | same test |
| RING | hidden iff link>0 && switch[link]!=1; collected rings in slot+0x14 | yes | same test + per-ring state |
| SWITCH | model +0x48 iff switch==1 else +0x40 | yes | pick model |
| CIRCUS_BALL | hidden iff misc0!=0 && switch!=1 | yes | same test |
| SET_VEHICLE | vehicle exists iff link==0 \|\| switch==1 | yes | same test |
| ELEC_BARRIER | gone iff switch==1 | yes | same test |
| ELEC_ACCESS | visible iff switch==1 | yes | same test |
| BombingWall | gone iff (link>=1 && switch==1) or killed | yes | same test + entry flag |
| FallBuilding | standing iff switch!=1 && state==0 | yes | same test |
| BA Switch | on/off model from BA table | yes | pick model |
| BA GUNSHIP | hidden until switch (link>0) + warp-in delay per spawn | partly | no stand-in (or switch test) |
| FAN / FAN STAGE | hidden iff misc word 8 == 0 (static) | yes | same test |
| DEFENSE_SHIELD | pose 3 when switch==1 | yes | default pose / skip when switch==1 |
| EggFleet, POWER_DEVICE_NAKED | hidden while gm+0x90 bit 1 | yes | same test |
| POWER_DEVICE_CAGE/NAKED | destroyed (slot+0x14) | yes | skip destroyed |
| RainEffect | visible only with the player inside | no | never |
| destroyed/collected classes | entry flag 0x1 clear | yes | skip |
| everything | held 0x20000, event mode leave | yes | skip |

Westopolis (stg0100): links 100..109 tie CityLaser (writer, raw 1 on strike) to FallBuilding (falls) and ART GADGET
(hidden) - this is the source of the observed blinks. Links 1/3/11/13 tie BK_SOLDIER groups (enemy-group writer sets 1
when the group is wiped) to CAGE (opens), HINTRING (hidden) and SWITCH. All are raw or flagged writes reset by restarts
as in 1.2.
