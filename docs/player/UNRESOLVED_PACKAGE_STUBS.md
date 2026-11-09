# Original-side stubs in the `H:\Unresolved Shadow functions` harnesses

Batch 00 audit (2026-11-03, read-only; directive item 4). The package's player modules were validated by running the
original words in a Python interpreter (`skinexec/sxfp.py`, `a1fp.py`) against native DLLs. Its one substitution
mechanism is `sxfp.CPU.stubs` (guest address -> Python callable, return via LR; `sxfp.py:313-314`). Every row below is
original behaviour that the package's "bit-exact" results did **not** check. Paths are relative to the package root.

**Rule applied here:** no behaviour that passes through a row below counts as verified coverage. The package's parity
numbers (64,000 / 223,200 frames etc.) are candidate-selection evidence only; each batch re-proves its scope with
`--verify-player` and the original-word oracle of this repository.

| # | Original VA(s) | Substitute | Where | Affects |
|---|---|---|---|---|
| 1 | world queries 80021934 / 80021E24 / 80021CA4 / 80020D40, result-list bookkeeping 80026FB4 800265A4 80026A40 800270CC 80334280 80334224 803340F0 800981A4 800981E4 | Python `MeshQueries` (SUBSTITUTE) or `GameQueries` = the **native** `sphys_world_*` DLL answering for the original | p3_move/tests/orig_collide.py:25-32, sim/parity_all_harness.py:308-321, tests/world_p3q.py:18-22 | every P3-based module and parity_all: the narrow phase is never oracle-checked; only I1 orig_loop (KEEP_REAL) and P16 test_world_dyn run the game's queries |
| 2 | state bit 0x37 forced: 800971EC edge catch, 80078F34 platform attach | skipped | p5exec.py:22-23,160; orig_loop.py:102 | platform carry, edge catch in the frame loop |
| 3 | ~41 VAs returning 0: incl. **8009827C ledge probe**, **800859F4 contact clear**, 80079994, **80078E68 pose history**, **800A76CC idle-voice RNG timer**, 80095DC0/DE4/D78, 800AC184, 80076980, 8008636C | r3 = 0, f1 = 0.0 | p5exec.py:50-61 | P5 and everything on it (P9..P22, parity_all, I1, IT, IN, F1, F3); 8009827C lifted only in parity_p5 / F1, 80078E68 still a debt (HANDOFF_COMPACT:241) |
| 4 | 800765A0 path follower | returns 0 | p5exec:62 | P5 (popped in p9exec:76) |
| 5 | 80052EFC motion-handle check | returns 1 | p5exec:63; F3 f3exec:80 | P5, F3 |
| 6 | 803A1AFC dynamic_cast | returns its argument | p5exec:64 | P5 and up |
| 7 | shared_ptr / weak handles 8007259C 8006D83C 8007254C 8006C380 800578A0 80057810 80057850 8005FEDC 8006119C 8008A310 80089D24 8008A350 8020691C 8005223C 800521AC 800521EC 80051CFC 80057990 | copies / no-ops, no reference counts | p5exec:65-72, p13exec:41-45, p15exec:127, p20exec:139 | handle lifetime: homing, lock-on, LineHang, vehicle |
| 8 | **800A82FC behaviour change** | Python re-implementation; unregistered ids refused silently, `refused` not compared | p5exec:67,103-117 | P5/P9/P13/P15/P19 and the I1 / IT / IN / F1 closed loops (original only in p20exec / parity_all) |
| 9 | homing target (weak handle, vt+8 position, expiry) | Python object; position / alive are inputs | p5exec:68-101; parity_all:159-172 | HomingAttack |
| 10 | motion system: +0xB4 bit 3 motion end, motion events | harness input; **random 6 % per frame** in parity_all | p5exec:216; parity_all_scenarios.py:77,142 | every motion-gated transition outside I1 |
| 11 | sub-objects +0x230..+0x254, +0x278, executer +0x38 | dummy vtable, every slot 0 | p5exec:152-159 | P5 and up |
| 12 | 80097658 collide inside carry | no-op | p5exec:274 | carry tests |
| 13 | 8034ED50 80076930 80334180 800A3058/34/10 **8016F4B0 gauge add** **80009548 camera singleton** 8000D664 8000D534 801D3C28 801D3BD8 8034EC7C 80079060 800A3164 80077A3C | return 0 (8016F4B0 re-stubbed in p22exec:62) | p9exec:49-66 | P9 and up, orig_loop |
| 14 | contact-event slot 8008636C / 800859F4 / 80099418 / 80099358 | Python model of the boost::function | p9exec:69-70 | WallJump, WallHang, JumpDash wall |
| 15 | 80074598 game manager, 800A2B7C/BBC effects, 8001B494 path singleton, **80078E20 bone ref**, **800818CC rail switch**, 800B7FB4, 803A1380 allocator | Python stand-ins | p9exec:71-80 | Dead, Grind, rails, ChaosBlast |
| 16 | 801DA024, 801DB7BC, lock-on handle (vt+0x28), 8007D768 801D48FC 8007D8B4 | Python; in parity_all the lock-on pick is the **native** sphys_p13_lockon fed to the original | p13exec:41-90; parity_all:383-392 | LightDash, homing lock-on |
| 17 | 8009741C zone overlap, 80049BB8/80049B5C, effects/sound/HUD, command ctors 801DB8F8/9D8/C224 | stand-ins | p15exec:45-129 | ChaosControl, LineHang |
| 18 | SetMotion, 800AABA0, effects, sound, command ctors 801DB704..801DC224, listener 8007B5A4/8007B11C, 8009CFFC | recorders, injected events | p19exec:62-81 | motion-handler-driven behaviours |
| 19 | HUD 80178F10, vehicle slots, 801B6B6C, weapon category/count, 80078DBC/8027AE98 super, 80095D00, ring drop 800B8130 | **canned values**; vehicle traits from the **native** table | p20exec:68-139; parity_all:494,510 | DriveVehicle, Super, PickupWeapon, ExternalControl |
| 20 | gadget side calls 800F35BC 800CDAF4 80169F94 8005328C .. 80169BB0 | 0 | p20exec:350-378; p20run:265 | Launch |
| 21 | weapon model 80063550.., hand node 800AAB48, aim 800AAA28, bullet spawn 80051D5C (recorded only), **target system 80060180 801E7C40 801E83D0 801E82C4 801E8528 801E879C**, **melee query 800C615C**, **RNG 8040FEE0 -> 1.0**, collider 800C7D84, heap 803A12A4/803A3DA4 | Python | p21exec:91-278; p20run:80-86 | weapon lock-on, melee, bullets |
| 22 | weapon-table availability bytes (0x48) | forced to 1 | p20run:95 | weapon availability |
| 23 | 801DA760 80079BA4 80079B54 801A02B4 80074758 80050C40/BF8 800AAA20 8016F99C 8016F4B0 802F3014/2F14 | stand-ins | p22exec:50-66 | command routing |
| 24 | sound loop / stop records, setmot +0x42C | Python | parity_all:141-158 | sound handles |
| 25 | IRQ 8037611C/30/44, CharaColli dynamic set 800C51C8/800C5860/800C5D20 -> empty, 800A2C3C/800A2BFC, **lock-on update (player+0x24C vt+0xC) skipped** | stubs | i1_player/tests/orig_loop.py:71-101 | I1, IT, IN, F1 |
| 26 | 80079994, 80357230, 80053014, sound / effect / voice, 80077AA8/80077A60, 800AB090, malloc/free 80412184/8041214C, **80414588 ARAM convert -> pass-through**, OSReport | recorders, Python allocator | a8_player/tests/orig_player.py:115-141; a5_states/tests/orig_inject.py:39-42 | A8 animation |
| 27 | Effect children, aura model 800122D4/80078DBC/8027AF10/80012700, music, rumble, 800A35D8, new/delete, OS lock | stubs | F3 f3exec:71-95 | gauge-full, time stop |
| 28 | Effect children, shields 803411CC/80341638, 8009BA6C, 80345C80, ring scatter 8011B614 (recorded only), 801B6888 | stubs | IT it_orig:58-76 | barrier, item economy |
| 29 | **80379628 OSGetTime -> host clock**, IRQ helpers, 80079B04 post queue, forced query answers | Python | IN in_orig:60,134; test_loop:74-89 | NPC clock, controller layer |
| 30 | 80096724, **80094F84 rider callback -> scripted**, 8009CFFC, 800CBA84 80074598 8015AB78 8015D7CC, 8001FF00 | stubs | p16 test_ride / test_setcoll / test_world_dyn | ride update, SetCollision |
| 31 | P16 walkers: original queries answered by **native** sphys_p16 | native-backed | run_walkers.py:5-8 | Walker |
| 32 | 8040FEE0 RNG scripted, **8011A338 floor -> Python**, 8011A434, 8011ADC0 | stubs | p13 test_scatter:30-40 | ring scatter |
| 33 | 80009548 / 80094264 camera, path allocators | stubs | p15 test_kernels:89-92; p15paths_orig:64-66 | camera forward |
| 34 | laser hit test 8005AAF8 answered by **native on both sides**, 8005B10C, world / list stubs, ctor / handle stubs | stubs | p14 tests / tools | P14 bullets |
| 35 | interpreter: fres / frsqrte / ps_res / ps_rsqrte exact (hardware: estimates), dcbz ignored, denormals / FPSCR not modelled, fctiw(+-inf) raises | interpreter deviation | sxfp.py:9-11,42-44,606,640 | any code using them |

Comparisons that were not against the original: TM against a Python model (TM/tools/tm_capsule.py:44,284-294);
sim/parity.py's models share P3's Python layer; motion events from a model / random source; native answers standing in
for original code in rows 1, 16, 19, 31, 34. No numeric tolerance was found; fields outside each harness's semantic view
were never compared (most of player +0x60..+0xA0, +0x228..+0x27C, +0x290; A8 pad words; rejected behaviour requests).
