# Player gaps named by the `H:\Unresolved Shadow functions` package

This is a read-only audit made on 2026-11-03. It lists the gaps the package states about the player, with the
package's own file:line evidence. Paths are relative to the package root. Nothing in the package was run or changed.
Stub row ids refer to `docs/player/UNRESOLVED_PACKAGE_STUBS.md` in this repository. The per-candidate table is
`package_candidates.csv`, in this folder.

"Unknown" means that the package does not say. The package's evidence labels VS / EX / SN / HY mean VERIFIED-STATIC,
EXECUTED on its own Python interpreter `skinexec/sxfp`, SNAPSHOT and HYPOTHESIS.

## 1. The 35 behaviour states (ids 0-34)

The factory is 800AE8D0. It indexes table 80522DE8 by id (`native-phys/p9_behaviour2/P9_REPORT.md:42-44`), and the
request is 800A82FC. Vtable slots: +0xC CanEnter, +0x10 Enter, +0x14 Leave, +0x18 Event, +0x1C Update
(`P9_REPORT.md:46`).

In the super form (state bit 0x25), ids 0, 2 and 3 are remapped to 34. Ground (1) is NOT remapped
(`native-phys/p20_behaviour5/P20_REPORT.md:65-69`, which corrects `P9_REPORT.md:44` "ids 0–3"). Ids 35 and 36 have no
table entry and fall back to 800A8644 (`P20_REPORT.md:68-69`).

"Orig frames" is the number of frames the composed ORIGINAL spent in each id over parity_all's 320 seeds
(`native-phys/sim/out/parity_all_report.md:27-63`).

| id | name | vtable | ctor | CanEnter | Enter | Leave | Event | Update | native module | orig frames | evidence |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | Idle | 805218F4 | 800871D8 | 800870D4 | 80086FE4 | 80086F1C | 800870DC | 80086D84 | P5 | 876 | native-phys/p5_behaviour/P5_REPORT.md:46 |
| 1 | Ground | 805217BC | 8008417C | 80083FFC | 80083EE8 | 80083DE4 | 80084004 | 80083A38 | P5 | 10947 | P5_REPORT.md:47 |
| 2 | Fall | 8052170C | 8008110C | 80081090 | 80080ED4 | 80080E58 | 80080E54 (nop) | 8008096C | P5 | 8134 | P5_REPORT.md:48 |
| 3 | Landing | 805219BC | 800891B4 | 80089154 | 800890AC | 80089024 | 80072548 (nop) | 80088EA0 | P5 | 936 | P5_REPORT.md:49 |
| 4 | Ottotto | 80521A7C | 8008C040 (P5 says 8008C03C) | 8008BFE0 (P15) / 80072548 (P9) | 8008BF70 | 8008BF44 | none listed | 8008BE90 | P15 | 48 | P9_REPORT.md:50; P5_REPORT.md:50; native-phys/p15_behaviour3/P15_REPORT.md:332-337 |
| 5 | Jump | 80521930 | 8008818C | 800880D0 | 80087904 | 800877E8 | 800880D8 | 8008731C | P5 | 5874 | P5_REPORT.md:51 |
| 6 | JumpDash | 80521970 | 80088D30 | 80088C74 | 80088948 | 800885D8 | 80072548 | 800883E8 | P5 | 1829 | P5_REPORT.md:52 |
| 7 | Sliding | 80521B4C | 8008E2B4 | 8008E24C | 8008E188 | 8008E108 | 8008DFFC | 8008DEC0 | P9 | 195 | P9_REPORT.md:51 |
| 8 | HomingAttack | 80521868 | 800866DC | 80086654 | 80085FE0 | 80085950 | 80072548 | 80085628 | P5 | 4057 | P5_REPORT.md:53 |
| 9 | HomingJump | 805218C0 | 80086CF4 | 80086C8C | 80086AC0 | 80086A4C | none listed | 80086928 | P15 | 303 | P9_REPORT.md:52; P15_REPORT.md:315-322 |
| 10 | Throw | 80521C24 | 80090548 | 8009045C | 800901B8 | 8008FE84 | unknown | 8008FDD8 | P14 | **0 (never entered)** | P9_REPORT.md:53; native-phys/p14_gadget2/P14_REPORT.md:449 |
| 11 | TurnOver | 80521C6C | 80091394 (creator 8009067C) | 80091278 | 80090F64 | 80090C18 | 80090B8C | 800909D4 | P19 | 1621 | native-phys/p19_behaviour4/P19_REPORT.md:49 |
| 12 | EdgeHang | 8052169C | 800800CC | 8007FF94 | 8007FBB8 | 8007F854 | 8008000C | 8007F810 | P9 | 988 | P9_REPORT.md:55 |
| 13 | WallHang | 80521CB4 | 80092668 | 800925F8 | 800924C4 | 80092418 | 80092168 | 80091C98 | P9 | 821 | P9_REPORT.md:56 |
| 14 | LineHang | 80521A4C | 8008BDB8 | 8008BAB4 | 8008B834 | 8008B6A8 | 8008B330 | 8008B274 | P15 | 2181 | P9_REPORT.md:57; P15_REPORT.md:250-257 |
| 15 | SomethingHang (TouchHang) | 80521B7C | 8008ED40 (creator 8008E310) | 8008EC88 | 8008EB1C | 8008E934 | 8008E778 | 8008E54C | P19 | 2122 | P19_REPORT.md:50 |
| 16 | WallJump | 80521D14 | 80093274 | 80093210 | 80092F04 | 80092BC0 | 80093218 | 800929FC | P9 | 460 | P9_REPORT.md:59 |
| 17 | Grind | 8052176C | 80082AD8 | 8008294C | 800825D0 | 800821E0 | 8008202C | 80081A94 | P9 | 1561 | P9_REPORT.md:60 |
| 18 | SpinDash | 80521BAC | 8008FA48 | 8008F9D8 | 8008F8D0 | 8008F7EC | 8008F728 | 8008F41C | P9 | 521 | P9_REPORT.md:61 |
| 19 | DarkSpin | 805215F4 | 8007D858 (creator 8007D168) | 8007D7DC | 8007D634 | 8007D588 | 8007D584 (blr) | 8007D3EC | P19 | 1084 | P19_REPORT.md:48 |
| 20 | LightDash | 80521A1C | 8008A790 | 8008A650 | 8008A4B0 | 8008A400 | 8008A3FC (no-op) | 80089D64 | P13 | 906 | P9_REPORT.md:63; native-phys/p13_lightdash/P13_REPORT.md:140-141 |
| 21 | PickupObject | 80521AC4 | 8008CC8C | 8008CAFC | 8008C7B0 | 8008C460 | 8008C3E8 | 8008C11C | P14 | **0 (never entered)** | P14_REPORT.md:448 |
| 22 | PickupWeapon | 80521B1C | 8008D8F0 | 8008D7F8 | 8008D450 | 8008D144 | 80072548 (blr) | 8008CEF8 | P20 | 880 | P20_REPORT.md:76 |
| 23 | Launch | 805219EC | 8008980C | 800897AC | 8008960C | 80089548 | 80089544 (blr) | 80089244 | P15 | 98 | P20_REPORT.md:77; P15_REPORT.md:332-337 |
| 24 | Stream | 80521BDC | 8008FCF8 | 8008FC98 | 8008FC2C | 8008FC00 | 8008FB80 | 8008FB04 | P15 | 1314 | P15_REPORT.md:300-307 |
| 25 | GroundAttack | 805217F8 | 80084E24 (creator 80084200) | 80084D60 | 80084A7C | 80084704 | 80084670 | 800842B4 | P19 | 52 | P19_REPORT.md:46 |
| 26 | AirAttack | 805214E4 | 8007AD4C (creator 8007A6A0) | 8007ACE4 | 8007AAF0 | 8007AA8C | 8007AA54 | 8007A774 | P19 | 487 | P19_REPORT.md:47 |
| 27 | DriveVehicle | 80521654 | 8007EFD8 | 8007EEF0 | 8007EB0C | 8007EA1C | 8007E8B8 | 8007E6D0 | P20 | 484 | P20_REPORT.md:78 |
| 28 | ChaosBlast | 80521540 | 8007B94C | 8007B88C | 8007B438 | 8007B08C | 8007B068 | 8007AE10 | P9 | 293 | P9_REPORT.md:69 |
| 29 | ChaosControl | 80521580 | 8007C8D8 | 8007C76C | 8007C468 | 8007C0AC | 8007C044 | 8007BB8C | P15 | 1267 | P9_REPORT.md:70; P15_REPORT.md:66-76 |
| 30 | Damage | 805215C0 | 8007D114 | 8007D0B0 | 8007CE74 | 8007CE10 | 8007D0B8 | 8007CCEC | P9 | 438 | P9_REPORT.md:71 |
| 31 | Dead | 80521624 | 8007DD0C | 8007DC30 | 8007DAA4 | 8007DA44 | 8007D9F0 | 8007D92C | P9 | 8922 | P9_REPORT.md:72 |
| 32 | ExternalControl | 805216D8 | 800806C8 | 80080608 | 800804A8 | 80080350 | 80080610 | 80080264 | P20 | 1178 | P20_REPORT.md:79 |
| 33 | ElectricCircuit | 8052112C | 80076D24 | 80076BFC | 80076A6C | 80076874 | 80076794 | 800765B0 | P20 | 1568 | P20_REPORT.md:80 |
| 34 | Super | 8055ABE4 | 802EBB08 | 802EBAA8 | 802EBA30 | 802EB9CC | 802EB9B0 | 802EB96C | P20 | 1555 | P20_REPORT.md:81 |

Inconsistencies inside the package:
- **Ottotto (4):** the ctor is 8008C03C in `P5_REPORT.md:50` but 8008C040 in `P9_REPORT.md:50` and `P15_REPORT.md:334`.
  CanEnter is 80072548 in `P9_REPORT.md:50` but 8008BFE0 in `P15_REPORT.md:335`.
- **GroundAttack / AirAttack:** `P19_REPORT.md:52` says that P15_REPORT §7 has ids 25 and 26 swapped.
- **Throw / PickupObject ids:** `integration/u1_dispatch/U1_REPORT.md:320` writes "PickupObject 10 / Throw 21". The same
  report's line 223, `P14_REPORT.md:448-449` and `parity_all_report.md:39,50` give Throw = 10 and PickupObject = 21.
- **The 8007A6EC turn:** SG says that 8007A6EC / 8007A774 are not ported (`native-phys/sg_gaps/SG_REPORT.md:493`).
  `P19_REPORT.md:47` lists 8007A6EC as AirAttack's turn helper, and `P19_REPORT.md:309` says it is ported.

Coverage claim: the package says "33 of the 35 ids are entered on the ORIGINAL side" (`U1_REPORT.md:223`). Ids 10 and
21 are never entered because P14's object executer is not composed into the original
(`native-phys/sim/out/parity_all_report.md:39,50`; `merge-handoffs/MERGE_PLAN.md:218`). Those two are tested only by
P14's function-level suites.

## 2. The named gaps

### Vacuum weapons (ids 24 VacuumPod, 61 / 62 VacuumEgg) — not ported
- Category 4 has controller ControlVacuum, vtable 80548DF4, and base WeaponVacuumBase. The report marks them **not
  ported** (`native-phys/p21_weapon/P21_REPORT.md:60`). ControlVacuum 8023DBA4 is "decoded, not ported"
  (`P21_REPORT.md:105`).
- The decoded details (VS) are in `P21_REPORT.md:192-198`:
  - ControlVacuum 8023DBA4: states 0 / 1 / 0x64 / 0x65 / 0x67 / 0xC8, 0.3 s.
  - WeaponVacuumBase 80239988: a cone query of cos 30° or 60° chosen by bit 1, mask 0x80, plus a range; the held-object
    list is at +0x40.
  - 80239DB8: shoots the held object from the muzzle + R·(0,0,[805F0FB8] = 300).
  - VacuumPod update 802264FC, per object: Damage 801DBBA4 when closer than 4, Accelerate 8006E47C with 500·f + 800, and
    VacuumCommand 801DB478 into the list.
  - VacuumEgg 80277D6C / +0x3C 80277A5C.
- The native code returns `env.unported` for category 4 (`P21_REPORT.md:198`). The weapon task must refuse loudly.
  Stages with a vacuum weapon cannot run the player unit (`merge-handoffs/MH01_player_behaviours.md:145-146`).
- Command 0x14 VacuumCommand (Execute 801DA09C = blr) is a P22 no-op (`native-phys/p22_control/P22_REPORT.md:121`).
- The gap is also listed in `integration/u1_dispatch/U1_REPORT.md:323`, `HANDOFF_COMPACT.md:247`,
  `HANDOFF_20261014_WAVE8.md:64` and `merge-handoffs/MERGE_PLAN.md:220`.

### SatelliteLaser (ids 59 / 60)
- The lock-on controller part is ported: "SatelliteLaser needs a lock" (`P21_REPORT.md:98`).
- The beam object is not ported. The spawn path is 8027B2D4 / 8027B290 → 8027B4A0. The request is ported and validated,
  but the birth is not: there is no P7 / P14 kernel, and `sphys_p21_shot_birth` returns 0 (`P21_REPORT.md:199-200`).
- The gap is also listed in `HANDOFF_COMPACT.md:248`, `MERGE_PLAN.md:221` and `merge-handoffs/MH09_weapons.md:54`.

### Homing-target search 80206B98 — not ported, by design
- **What it is:** the PlayerTarget's (+0x24C) homing-target search. It is called from the player input code 800A0130 at
  800A06C8, and its result goes to 800A1A30 (`native-phys/p5_behaviour/SJ_NOTE.md:297-301`).
- **Pipeline** (`SJ_NOTE.md:302-319`): 801E7C40, mask 0x400 (801E83D0), exclude own target (801E8158), cone 801E82C4,
  radius 801E8528 at [r13+0x47E0], directional 801E84C0, nearest 801E879C.
- **The bit-0x25 branch:** outside the super form, the search keeps only targets below the player (cone apex = player,
  dir = −up, cos 0.0). The super form skips that filter. With |stick| > 0.09 it uses a stick cone, then cos 45°;
  otherwise it uses the facing cone cos([805F79D8] deg).
- **Parameters:** range 150, angles 40° / 200°, set by init 80207274 (sbss). The range is read at 80206C98 / 80206DDC
  and the angles at 802070A8 / 802070EC (`findings-20261019-GP/xcorr/XCORR.md:120,315-320`;
  `findings-20261019-GP/GP_REPORT.md:45`).
- **Status:** unported by design (`SJ_NOTE.md:373`). 801E8528 is not among P13's native filters. In the native
  architecture the host passes the target with 0x10A. The input layer P25 runs it only as a boundary answer:
  "EXECUTED only at the boundary", with no native owner (`findings-20261019-IN/IN_REPORT.md:332-333`). The trigger is
  the character-table byte +8 for ids 1 and 13..19 (`IN_REPORT.md:130`).
- MH01 keeps it translated: "stays translated: it posts HomingAttack with the target as values"
  (`MH01_player_behaviours.md:147-148`). The SJ scan of bit-0x25 sites marks 80206C14 as "not checked by SJ (open)"
  (`SJ_NOTE.md:58`).

### Ground's 0x10C loop path
- Command 0x10C is Player::SetLoopPathCommand, with Execute 8009C8B8 (handled only). Ground's Event 80084004 sends the
  loop path to **80082EE0**. The Execute is ported; the P5 slot is open (`P22_REPORT.md:142,306-307`).
- The native Ground Event at `native-phys/p5_behaviour/src/p5_behaviour.cpp:747-751` returns 1. It sets `c.unported`
  when contact bit 0 and caps bit 0 are both set; that path needs 80082EE0 (state bit 0x31 + the Ground path follower
  +0x38), which is not ported.
- U1 states "The 0x10C loop variant still returns unported" (`U1_REPORT.md:79,324`). The gap is listed in
  `HANDOFF_COMPACT.md:250`, `MERGE_PLAN.md:221` and `MH01_player_behaviours.md:107`.
- The controller emits 0x10C when its path search returns kind 1 (`IN_REPORT.md:64`).
- The Ground path follower (+0x38) and its release 80082E90 are unported (`P5_REPORT.md:699-707`;
  `P15_REPORT.md:564`).

### Unhandled commands 0x11C / 0x12A / 0x133 (FX open items O-2 / O-3 / O-4)
The receive logs these as `SPHYS_P22_EV_UNOWNED` and returns them as unported (`U1_REPORT.md:74,330-331`). Their
status after FX is "0x11B/0x136 stay unowned, 0x11C/0x12A/0x133 open"
(`findings-20261013-U11/CONTINUATION_DIRECTIVE.md:167`).

| type | name | Execute | what it does | why it is not routed | evidence |
|---|---|---|---|---|---|
| 0x11C | ApproachControl | 8009B72C | args 8009B83C (kind +0x14, handle +0x18) → Controller +0x224 → 8009EF70 → control manager +0x28 → **8009F764**: a Player::Control change to Approach (8009E124, P14's kernel) | needs a native Player::Control state machine (8009F764); PickupObject 21 is also unreachable in parity_all | `integration/u1_dispatch/FX_NOTE.md:47,148` |
| 0x12A | SetExternalMotion | 8009AA28 | +0x230 vt+0x1C = **800B5380**: SetMotion(0x2F, fade +0x20), component +0x430 = fade, +0x420 = *(cmd+0x14), hand +0x7E0 800B050C | a8 has no external-motion entry | `FX_NOTE.md:51,149` |
| 0x133 | SetLookAtPosition | 8009A02C | PlayerTarget +0x24C vt+0x18 = 800B6C4C → **800B7670**: +8 position, +4 weight | no native owner of the look-at target | `FX_NOTE.md:53,150` |

Also UNOWNED: 0x11B GetItem (later ported by IT, `findings-20261019-IT/IT_REPORT.md:60`) and 0x136 ResetCamera (camera)
(`FX_NOTE.md:46,54`). 0x11C also appears as a TurnOver button event (`P15_REPORT.md:570`).

### Throw (id 10) and PickupObject (id 21)
- **Native:** `native-phys/p14_gadget2/src/p14_player.cpp` and `p14_pickup.cpp` (`P14_REPORT.md:434-457`).
- **Tested only function-level** by `tests/test_pickup.py`: 400 / 401 / 400 / 400 cases (`P14_REPORT.md:512`).
  - The harness `tools/pickup_orig.py` only logs requests to 800A82FC (line 103).
  - Shared/weak handles are plain copies (`pickup_orig.py:6`).
  - 803A1AFC returns its argument (`pickup_orig.py:223`).
- **Not in parity_all:** the original enters neither id. Throw has 0 frames with 0/7 requests accepted; PickupObject has
  0 frames with 0/3 (`parity_all_report.md:39,50`). The same gap is stated in `U1_REPORT.md:320-322`,
  `F1_REPORT.md:159`, `MERGE_PLAN.md:218` and `HANDOFF_20261014_WAVE8.md:72`.
- **Deviations** (`P14_REPORT.md:560-568`):
  - D1: the slide step 801BFFC8 is not ported; drops are proven only by replaying the original's answers.
  - D2: the hand point is an input (HY).
  - D3: the thrown pitch / yaw start at 0.
  - D4: 70 NaN-payload-only cases.
  - HY: the frame order, and the meaning of state bits 0x2A / 0x2E.
- **Carrying (bit 0x1E):** no native code set bit 0x1E before F2. Nothing caught that, because "parity_all never reaches
  PickupObject 21 or Throw 10" (`findings-20261019-F1/F1_REPORT.md:103-106`).
- **The Throw animation command 24** (handler 800B3210, pick_throw / jump_throw_b) is ported in A13
  (`native-anim/a13_animgaps/A13_REPORT.md:58`).

### Unresolved animation dependencies
- **D1 — 8041BB8C**, the paired-channel sync against a PENDING clip.
  - GekkoForge `native_motion_v7` refuses it: "v7 throws FAIL_CLOSED at `pose.cpp:1588-1590`". The player refuses at
    frames 72/31/38, NPCs at 64/55/43, and the a5 full frame at 195 (`merge-handoffs/MERGE_PLAN.md:179`).
  - The package ports it in `native-anim/a8_player/src/pair_sync.cpp`, spliced at build time into a COPY of v7
    (`native-anim/a8_player/A8_REPORT.md:23,85-100`). The splice fired 38 times on 21 frames (`MH10_player_anim.md:32-33`).
  - Two parts are not executed: the id-1 legacy branch fails closed (`A8_REPORT.md:91`), and the NaN-span rule is
    VERIFIED-STATIC only (`A8_REPORT.md:95`).
  - The player unit is "**Blocked by D1**" (`MH01_player_behaviours.md:5`): "Without it the player cannot be lifted at
    all" (`MERGE_PLAN.md:179`). The recommended path is a new `native_motion_v8` (`MH10_player_anim.md:63-75`).
  - The a5 full frame ran only 195 of 230 frames of its default script (`native-anim/a5_states/A5_REPORT.md:248,256`).
- **D12:** the Super Shadow bank (SUPERSHADOW.MTP) is not registered, so commands 43-45 fail closed
  (`MERGE_PLAN.md:190`; `A13_REPORT.md:373`; `U1_REPORT.md:160-162,326`).
- **D6:** the owner of the attachment table, GekkoForge native_attach vs the package's a8 / I1 port
  (`MERGE_PLAN.md:184`).
- **D10:** the BON / MTP importer is in the product link (`MERGE_PLAN.md:188`).
- **Not executed in the A8 tests** (`A8_REPORT.md:238-243`; `MH10_player_anim.md:102`): the weapon fire path (8005BA2C),
  13 handler sets (15 of them were added later by A13), listener forwarding and the weapon window.
- **Motion core** (`native-anim/DEVIATIONS.md:9-41`): the package's own motion port omits root motion and blend
  scheduling. The evaluation is v7's.
- **ARAM conversion 80414588** is a pass-through in the a8 / a5 original harnesses (stub row 26).
- **UNRESOLVED_INDEX rows touching the player rig** (`UNRESOLVED_INDEX.csv`):
  - U03 (line 4): 800B1C20, 800B1F04, 80076E58, 800B6BEC and 800B6AD0. "Second handle call 800B1E04 remains unreached
    after failed handle validation". Neither full callback has independent pose equality.
  - U05 (line 6): the static-only family 800B1AA0 … 800B2798, 8018F9E4.
  - U14 (line 15): oracle limits. "Independent hardware/CPU agreement missing; exceptional floats/FPSCR not covered".

### Unresolved indirect dispatch
- `README.md:7`: "Unknown indirect destinations are not filled with guesses".
- `unresolved-code-regions/regions.json` records each `indirect_ctr_call` with `target: null`. In the player / ShadowMotion
  regions they are:

| region | `bctrl` sites with no resolved target |
|---|---|
| 800B1C20 (grab frame, ported by P14) | 800B1C7C, 800B1E04 |
| 800B1F04 | 800B1FA8, 800B2010 |
| 800B16F0 | 800B1764, 800B1788 |
| 800B2398 | 800B2434 |
| 800B2590 | 800B2638 |
| 800B2798 | 800B2A1C |

- Outside the player: 802ADEFC → 802ADF9C (U01: "slot+1C at 802ADF9C has no concrete constructor/captured target
  proof", `UNRESOLVED_INDEX.csv:2`) and 80196784 → 80196824 (U02).
- **Indirect calls inside player code that the harnesses resolve with stand-ins** (stub list):
  - the lock-on target update, player+0x24C vt+0xC, skipped in I1 (row 25);
  - the PlayerTarget lock-on handle vt+0x28 (row 16);
  - the contact-event boost::function slot player+0x68 (row 14);
  - the sub-objects +0x230..+0x254 with every vtable slot 0 (row 11);
  - the weapon refill, vt+0x44, not ported (`IN_REPORT.md:340`).

### Other gaps the package names (player scope)
- **Bird controller 802C959C, the Egg Dealer non-clip camera, and the P12 placements found by GP2** are not ported
  (`HANDOFF_COMPACT.md:251-253`; `HANDOFF_20261014_WAVE8.md:67-68`).
- **Laser redirect callbacks** 8023C5E4 (reflect) and 8023C2B0 (seek) are only recorded as an enum. The **drop wall
  adjust** 80221FA4 belongs to the item. `bullet_type` is HY. The SET parameter that holds the weapon id is HY
  (`P21_REPORT.md:201-210`).
- **Target system and melee query:** TargetManageTask 801E7C40..801E879C and the CharaColli line attack 800C615C are input
  callbacks in P21 (`P21_REPORT.md:206-207`; stub row 21).
- **Controller's stick InputCommand:** its class, and the bit-0x2E suppression via 800A1C7C, are not decoded. All P22
  scenarios use a zero pad stick (`P22_REPORT.md:313-314`).
- **DarkSpin:** the trigger search 800AC2B4 and the junction stick choice 800AC8DC are not ported
  (`P19_REPORT.md:321-323`; `P20_REPORT.md:379-380`).
- **Grind:** reversed cursors, junction links, rail switching 800811C4 / 8008151C, the special-surface sub 5 and event 3
  (80018150) are not ported (`P9_REPORT.md:490`).
- **NaN-branch ports:** WallHang 800917C4 plus one more in 80091C98's path, and LightDash 80089F44 / 8008A270. FX found 6
  of 396 cases diverging (`FX_NOTE.md:97-109,146-147`). F1 promoted fixes for WallHang 80091DAC / 80091F80 / 800917C4
  (`F1_REPORT.md:18`). The package does not say whether the LightDash ones were fixed.
- **LightDash:** the line-of-sight object half is a callback (HY), and the RegularRing handler 801128D0 is not executed
  (`P13_REPORT.md:291-295`).
- **Executer pre-steps:** 800AF868 was ported in F2 and 800AF7B8 / 800AF60C in F3. The I1 loop does not call the F3
  functions yet (`F3_REPORT.md:272-276`).
  - 80079060 counter (player +0x280): unported, meaning UNRESOLVED (`F3_REPORT.md:281-284`).
  - +0x84 has shared owners (`F3_REPORT.md:289-291`).
  - Who writes bits 0x22 / 0x23: `F1_REPORT.md:158` still says unknown, while `F1_REPORT.md:83-88` and F3 identify
    800A51F0.
- **Idle voice timer** 800A76CC (RNG) is stubbed in every harness (stub row 3; `MH01_player_behaviours.md:107`).
- **Pose history 80078E68:** "every closed loop before NP ran with Shadow's history frozen on the ORIGINAL side"
  (`HANDOFF_COMPACT.md:241`).
- **Module-level harness debts:**
  - Originals still stub the contact slot and the ledge probe (`HANDOFF_COMPACT.md:242`).
  - orig_loop inherits the gauge-add stub 8016F4B0 (`HANDOFF_COMPACT.md:243`).
  - The queued `p9exec jumpdash_wall` flip (`SJ_NOTE.md:367-370`; `HANDOFF_20261014_WAVE8.md:73`).
  - T2: "86 of its 269 stubs replace routines a port cites", and 54 of 102 static-initialised tunables are 0 in the
    composed harness. Original-side branch coverage is 76.9 %, with 3,771 outcomes OPEN
    (`findings-20261014-verify/t2_coverage/T2_REPORT.md:21-26`).
- **IT open items:** kind 5 vehicle repair, the writer of Barrier flag bit 1, the ElectricCircuit route (VS only),
  MagnetEffect 80341638, HeatEffect 80096110 and NPC GetItem forwarding 800721A8 (`IT_REPORT.md:421-433`).
- **Input layer:** the mapping from physical button to device bit is not decoded, and owner answers are proven only at
  the query boundary (`IN_REPORT.md:331-345`).
- **Order hypotheses:**
  - command order within a frame (`MH01_player_behaviours.md:149`; `I1_REPORT.md:293-294`);
  - car / vehicle before the player (`P20_REPORT.md:395-398`; `P22_REPORT.md:315-316`);
  - grab before the container update (`P14_REPORT.md:567`).
- **Weak controls:** P21_CTRL=16 is caught on only 2 seeds (`parity_all_report.md:20`; `MH01_player_behaviours.md:127`).
- **Super:** the motion bank (D12, above). The 80079060 counter is also touched by ChaosControl Enter / Leave
  (`F3_REPORT.md:283-284`).
- **Presentation calls** (80077A84 voices, effects) are logged, not modelled (`U1_REPORT.md:325`).

## 3. How the package's "bit-exact" claims were produced (summary for the CSV columns)
- **The reference side** is always the original PPC words run on the package's own Python interpreter `skinexec/sxfp`.
  No result in these reports compares against hardware or Dolphin. U14 says independent CPU agreement is missing
  (`UNRESOLVED_INDEX.csv:15`).
- **Comparisons are bit-for-bit**, with NaN compared as a class: x86 0xFFC00000 vs Gekko 0x7FC00000 (for example
  `FX_NOTE.md:62`; `MERGE_PLAN.md:189` D11). No numeric tolerance was found.
- **The interpreter** computes fres / frsqrte exactly (stub row 35). The native frsqrte seed is the correctly rounded
  1/sqrt model. For `normalize`, "The interpreter uses the same model, so the EXECUTED tests do not exercise it"
  (`native-phys/p3_move/P3_MOVE_REPORT.md:479`).
- **Circular tests:** every test whose world queries go through `world_p3q.GameQueries` has its original-side queries
  answered by the package's own `sphys_world_*` DLL. That covers parity_p5, parity.py, P19 fuzz and courses, the P20 and
  P22 stage runs, P21 fuzz and courses, T3 and the JumpDash wall test (`native-phys/sim/parity_p5.py:5`;
  `T3_REPORT.md:41`).
  - parity_all itself uses the Python `MeshQueries` substitute (`native-phys/sim/parity_all_harness.py:310`). It feeds
    the native sphys_p13 lock-on pick to the original (`parity_all_harness.py:14-16`; stub row 16) and uses random 6 %
    motion events (`parity_all_scenarios.py:77`; stub row 10).
- **Tests that are not circular and not stub-only on world queries** run the game's own queries on a snapshot RpWorld:
  I1 / I2, T3's animation tier, IT, F3, W's stage tests, P14's stage throws and P16's dynamic queries.
