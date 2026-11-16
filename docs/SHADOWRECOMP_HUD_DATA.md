# ShadowRecomp's widescreen HUD table, against our automatic layout

**Source and licence.** Notes on ShadowRecomp (FoxWorkshop/ShadowRecomp, GPL-3.0), the Xbox recompilation of the same game, written from a read of its code on 2026-11-16. Mechanisms and data are described in our words; no code was taken, and none should be. Xbox addresses are hints for finding the same code in the GameCube DOL (tools/guest_classes.py for class names).

**What it means for us.**
- **Their method: a table.** ShadowRecomp positions the 2D by a hand-written table of 274 CSD paths, each mapped to an
  anchor. Ours decides automatically from bounds (`runtime/lift/ui/hud_layout.*`: group and cast anchors,
  hysteresis, split-screen regions).
- **Their table is a free test set for our policy.** For each element, our decision should match theirs, or we should
  know why it does not.
- **Agreements checked so far:**
  - The title's zoomed after-images `title2`..`title6` stay centred with their widget, in both ports.
  - Their vertical anchors only act below 4:3, because only X is squeezed on wide screens. So the Boss gauge's
    BOTTOM_CENTRE still centres horizontally, as ours does.
- **Worth checking against ours on the disc:**
  - corner decorations that hug the true screen edge (honban `Loop/ultimate`, Story_Library `Background`,
    BattleMode `Kazari`, SelectMode `title`);
  - the `Loading_*` scenes anchored right;
  - the Egg Dealer's small display and reels anchored right while the large display is centred;
  - Circus Park fly-ins squeezed in place;
  - full-width text backgrounds in SelectMode left stretched;
  - and their warning that AUTO_H widgets can tear across a third boundary. Our per-group anchoring was built to
    avoid exactly that.

## The notes

Source: /home/user/foxworkshop/shadowrecomp/ShadowRecomp (patches/aspect_ratio_patches.{h,cpp}, ui/multiplayer_hud.cpp, patches/multiplayer_mod_patches.cpp, fmv_patches.cpp, prompt_patches.cpp, bloom_patches.cpp, user/config_def.h, ui/button_guide.cpp, gpu/video.cpp ComputeViewportSize).
Only data and behaviour are summarised here. No code is reproduced.

Total CSD modifier entries in `g_csdModifiers`: **274** (counted as `HashStr(...)` rows, lines 1339-1693 of aspect_ratio_patches.cpp). One key (SelectMode/title) appears twice with different flags (see gotchas).

---------------------------------------------------------------------
### 0. How the system fundamentally works (needed to read the tables)

- The game draws ALL 2D in a fixed 640x480 virtual space that the renderer stretches over the whole viewport. On 16:9 that makes everything 1.333x too wide (X axis); below 4:3 the Y axis is too tall. ShadowRecomp does NOT re-layout; it "un-squeezes" every selected 2D quad (4-vertex CSD primitive) around an anchor point.
- Per vertex: new = anchorScreen + (vertex - anchorLayout) * unstretch. Anchors are in 640x480 layout space. Unstretch is g_hudUnstretchX (=min(1, (4/3)/aspect), so 0.75 at 16:9, 0.5625 at 21:9) and g_hudUnstretchY (=min(1, aspect/(4/3)), only <1 on screens narrower than 4:3).
- Because the layout space stays stretched across the full viewport, a LEFT-anchored widget just maps layout x=0 to screen x=0 (or to the HUD-width inset) and shrinks about that edge. Nothing is "moved to a safe zone" other than that edge mapping.
- **Lookup is per path hash (XXH3-64 of "csdFiles/<file>.xncp/<scene>[/<castnode...>/<cast>]")**, resolved while rendering: scene modifier set at Scene::Render, cast-node modifier set at cast-node render, cast modifier set at cast render. At draw time **the most specific wins and they are NOT merged**: cast entry, else cast-node entry, else scene entry. An entry with flags = NONE is therefore a deliberate "stop inheriting, leave stretched" marker.
- Cast-node paths equal the path of the node's root cast. Paths are registered by hooking the CSD loaders (direct path load, TBGRead async loader, and stage .one archives where only the file name is known, e.g. `CHECKPOINT5.XNCP/...` and `PR0302RINGANM.XNCP/...` have no csdFiles/ prefix). Shift-JIS bytes in node names are replaced with `_` (names in the pause table like `frame_edge___R_s_[` are the sanitised forms). Registered entries are freed when the CSD buffer is freed.
- An entry only acts if its flags include a "positioning" flag (UNSTRETCH, FORCE_UNSTRETCH or any ALIGN_*). Naming only an alignment is enough.
- Quads spanning the whole 640 width (x<=1 and x>=639) are treated as backgrounds / fades / letterbox and left stretched, unless FORCE_UNSTRETCH.
- Default anchor when an axis has no alignment flag: CENTRE (screen centre, so the quad squeezes toward the middle). "Centre can never break a layout since every quad moves to the same point" - this is why almost every menu is CENTRE.
- On wide screens only X is squeezed, so vertical anchors (TOP/BOTTOM/CENTRE_V) have no visible effect unless the screen is narrower than 4:3. Vertical flags in the tables are mostly there for narrow screens (5:4, portrait-ish windows).

#### Flag meanings (CsdFlags in aspect_ratio_patches.h)
| Flag | Meaning |
|---|---|
| UNSTRETCH | squeeze quad back to square pixels |
| ALIGN_LEFT / CENTRE_H / RIGHT | horizontal anchor: layout x=0, x=320, x=640. LEFT and RIGHT map to the HUD frame edges (see MaxHUDWidth); CENTRE maps to the screen centre. Centre is tested FIRST, so adding CENTRE_H to a corner combo yields centre |
| ALIGN_TOP / CENTRE_V / BOTTOM | same for Y (y=0, 240, 480) |
| ALIGN_AUTO_H / AUTO_V | choose per quad by the quad's own bounding box centre: left third -> LEFT, right third -> RIGHT, else centre. Each quad of a widget decides independently |
| ALIGN_SELF_H | anchor = the quad's own horizontal centre (it squeezes in place, does not move) |
| FORCE_UNSTRETCH | also squeeze full-width quads (for full-width widgets that are not backgrounds, e.g. animated title logo that grows to cover the screen) |
| SKIP | do not draw this cast at all |
| ULTRAWIDE_ONLY / NARROW_ONLY | flags only apply when aspect >16:9 / <16:9 (defined, not used in the table) |
| IGNORE_MAX_WIDTH | anchor to the true screen edge even when the HUD Width option constrains the HUD |
| EXTEND_EDGE_H | restore the original x of vertices that sat on the 0/640 edge (the semi-transparent black box in the Story Library route screen must still reach the screen edges) |
| HIDE_PAUSE, PAUSE_BOSS, EXTEND_BOTTOM, MOVE_BOTTOM | pause-menu plumbing: the port adds extra pause options (each added item = 15 layout units); EXTEND_BOTTOM pushes only the bottom vertices (1 and 3) down, MOVE_BOTTOM moves the whole quad down; HIDE_PAUSE casts are hidden while the port's own options menu is open; the Boss pause variant has a larger background so one fewer item is counted when 2+ were added |
| PAUSE_DIM | work around an Xbox "black_cover" NCP bug: the dim overlay is drawn twice (CSD + game fade); the CSD's alpha is recorded and the game's separate dim is reduced to the remaining alpha so the total equals the intended dim |
| CENTRE (H+V), CENTER_UNSTRETCH | = UNSTRETCH + both centres (shorthand used throughout) |

#### Non-CSD entry point: HUD number flags and defaults appear in section 2.

---------------------------------------------------------------------
### 1. CSD table by screen

Notation: U = UNSTRETCH. "C" = CENTRE (H+V). Path root prefix `csdFiles/` omitted. "IGN" = IGNORE_MAX_WIDTH. All entries below squeeze (U) unless stated.

#### 1a. Gameplay HUD (in-stage)
| Path | Flags | Note |
|---|---|---|
| InfoWindow.xncp/Ring, Ring/Null_0000, Ring/Null_0000/ring | U, AUTO_H, BOTTOM | ring counter icon block, bottom-left stack |
| InfoWindow.xncp/Zanki, Zanki/Null_0000, Zanki/Null_0000/key, Zanki/Null_0000/zanki | U, AUTO_H, BOTTOM | lives + key; same anchor class as ring |
| CommandWindow.xncp/CarMeter (+outline, meter_bg, meter) | U, AUTO_H, BOTTOM | vehicle meter |
| CommandWindow.xncp/Car_BCom (+Null, Null/button2, Null/icon_car) | U, AUTO_H, BOTTOM | |
| CommandWindow.xncp/Weapon (+weapon, weapon/button1, weapon/icon_gun) | U, AUTO_H, BOTTOM | weapon box, one per screen half |
| CommandWindow.xncp/Window (+window, window1, window2, info_bg, info_bg2) | U, AUTO_H, BOTTOM | info/hint window |
| HeroGauge.xncp/Scene_0000 (+bar_bg, meter_main1-3, bar, meter_RSide, meter_side) | U, TOP, CENTRE_H | Hero Chaos gauge, whole widget pinned to top centre |
| DarkGauge.xncp/Scene_0000 (+meter_bg, main1-3, meter, side1, side2) | U, TOP, CENTRE_H | |
| BossGauge.xncp/Scene_0000 (+bg, m1, m2, m3, meter, left_edge, right_edge) | U, BOTTOM, CENTRE_H | NOTE anchor is BOTTOM though the gauge sits near the top of the screen. Only matters on narrow aspect (Y squeeze) |
| Hissatu_Hero.xncp/HissatuHero/XButton, .../XButton/Eff_X | U, TOP, CENTRE_H | Chaos Blast X prompt |
| Hissatu_Dark.xncp/HissatuDark/YButton, .../YButton/Eff_Y | U, TOP, CENTRE_H | |
| TWindow.xncp/TargetWindow, TargetDark/Null_dark (+dark_icon, Cast_0005, Cast_0005/Cast_0006), TargetHero/Null_hero (+hero_icon, Cast_0003, Cast_0003/Cast_0004) | U, AUTO_H | mission target counters; per-quad left/right decision (dark counter on one side, hero on the other) |
| EggBoss.xncp/DisplaySml, 1, 2, 3 | U, RIGHT | Egg Dealer roulette small display and the three reels pinned RIGHT |
| EggBoss.xncp/DisplayLg | U, CENTRE_H | the large display is centre, the rest right |
| Mchara_{sonic,tails,knuckles,rouge,omega,vector,espio,eggman,maria}.xncp/MissionCharacters, MChara_{amy,charmy,doomeye}.xncp/MissionCharacters, MChara.xncp/MissionCharacters (13) | U, AUTO_H | partner-intro name banner |
| BChara_{BBull,EBreaker,HDog,BFalcon,SandD,BDoom,EDealer,DevDoom}.xncp/bchara (8) | U, AUTO_H | boss intro name banner |
| stageTitle/StageTitle.xncp/StageTitle | U, C | stage title card (note path root is csdFiles/stageTitle/) |
| CHECKPOINT5/6/7/8.XNCP/Box, /Icons, /Shadow (12; stage .one archives, no csdFiles prefix) | U, C | checkpoint warp menu |
| PR0302RINGANM.XNCP/Scene_0000 | U, C | Circus Park gong / fire ring / minigame reward / bomb+ring fly-in |
| PR0302RINGANM.XNCP/Scene_0000/Null_0015, Null_0015___R_s_[, bomb_ligid, bomb_parp | U, SELF_H | these fly-in nodes squeeze in place instead of toward centre |
| Synopsis.xncp/Button | U, BOTTOM, RIGHT | story synopsis skip prompt button |

Gameplay numbers drawn by code (not CSD) are in section 2.

#### 1b. Pause (51 entries: pause.xncp 47, pauseBoss.xncp 4)
- Scene-level shell, all CENTER_UNSTRETCH: pause.xncp/Pause/Pause, Pause/Mission, Pause/Mission_Board, Pause/MIconDark, MIconHero, MIconNormal, Pause/ChangeMission, Pause/SRText, pauseBoss.xncp/Pause/Pause; pause.xncp/Pause/Mission/black_cover is CENTER_UNSTRETCH + PAUSE_DIM.
- Skipped text: pause.xncp/Pause/PauseText = SKIP|HIDE_PAUSE; pauseBoss.xncp/Pause/PauseText = SKIP|HIDE_PAUSE|PAUSE_BOSS (the port re-draws pause labels itself).
- Menu background growth: Pause/Pause/Null_pause/bottom and /bg (both pause.xncp and pauseBoss.xncp) = HIDE_PAUSE|EXTEND_BOTTOM|CENTER_UNSTRETCH (boss ones add PAUSE_BOSS).
- Everything below the menu in the Mission page (Mission_Board bg1-4 + Null_missionname; Mission/Null_0190/* 14 frame/line quads; iconshadow1-3; mission; MIconHero/hero+highlight; MIconNormal/normal+highlight; MIconDark/dark+highlight; ChangeMission/change, arrow_left, arrow_right; SRText/text, button) = HIDE_PAUSE|MOVE_BOTTOM|CENTER_UNSTRETCH (31 entries).
- Pause story-route mode: pause.xncp/StoryRoute, StoryRoute/StoryRoute, StoryRoute/SRIconsBG, StoryRoute/SRIcons = CENTER_UNSTRETCH.

#### 1c. Results / game over / failed
| Path | Flags | Note |
|---|---|---|
| Result_{EN,FR,GE,SP,IT,JP}.xncp/Main | U, C | |
| Result_*/Main/result | U, LEFT | the "RESULT" heading/band is anchored LEFT inside a centred panel (child overrides parent) |
| Result_*/Rank | U, C | |
| MissionFailed_{EN,FR,GE,IT,JP,SP}.xncp/mf | U, C | |
| GameOver.xncp/GameOver | U, C | |
| GameOver.xncp/Loop | U, AUTO_H | looping decoration decides per quad |
| 2P_Result.xncp/Result, 2P_Result.xncp/TimeUp | U, C, FORCE_UNSTRETCH | 2P result and time-up squeeze even full-width quads |

#### 1d. Title / main menu / logo / loading
| Path | Flags | Note |
|---|---|---|
| Logo.xncp/XBox/sega, sonicteam, dolby | U, C | |
| honban.xncp/Title | U, C | |
| honban.xncp/Loop/ultimate | U, LEFT, IGN | "ultimate" text hugs real screen left edge even with HUD-width limit |
| honban.xncp/Title/title2 ... title6 (5) | U, C, FORCE_UNSTRETCH | the logo animation grows to full screen, which would be classed as a background; force keeps its scaled size |
| honban.xncp/Menu/logo, title, 1pgame, 2pgame, options, storymode, selectmode, hardmode, new, resume | U, C | main menu items (10) |
| honban.xncp/Menu/back_button, back_button/back, select_button, select_button/select | U, BOTTOM, CENTRE_H | the on-screen button prompts at the bottom |
| Loading_{EN,FR,GE,IT,JP,SP}.xncp/Scene_0000 | U, RIGHT, IGN | loading indicator bottom-right style, ignores width limit |

#### 1e. Stage select
| Path | Flags | Note |
|---|---|---|
| SelectMode.xncp/SelectMode | CENTER_UNSTRETCH | |
| SelectMode.xncp/SelectMode/bg, text_bg_top, text_bg_bottom, text_bg_bottom/bg2 | NONE | stay stretched (full-bleed backgrounds under a centre-squeezed scene) |
| SelectMode.xncp/SelectMode/title | listed twice: (U, LEFT, IGN) then (U, SELF_H) | duplicate key; which one wins depends on map construction (probably first). The comment on the first says LEFT |
| SelectMode.xncp/IconsBG, SelectMode.xncp/Star | CENTER_UNSTRETCH | |
| RouteDisplay.xncp/StoryRoute/IconsBG, /Pics, /Arrows | U, C | |

#### 1f. Library / story menu (Story_Library.xncp)
| Path | Flags |
|---|---|
| StoryMode/Window | U, C |
| StoryRoute/StoryRoute | U, C |
| StoryRoute/StoryRoute/blkcover | U, C, EXTEND_EDGE_H (low-opacity black box must reach screen edge like 4:3) |
| StoryRoute/SRIcons, StoryRoute/SRIconsBG | U, C |
| Buttons/Buttons | U, C |
| Library/PageNumber, Library/WindowLib | U, C |
| Background/Background | U, LEFT, IGN ("LIBRARY" text in the corner) |
(This file also hosts new game / continue / last story / expert menus.)

#### 1g. Battle mode / 2P menus (BattleMode.xncp, 21 entries)
- Kazari (the "Battle mode" corner text): U, LEFT, IGN.
- Kazari/Null_box/box_chara and box1..box6, Kazari/Null_chara/bg_1p, bg_2p, tx_1p, tx_2p, 1PCharaSelect, 2PCharaSelect, Cursor1P, Cursor2P, Arena: U, C.
- Buttons/select_button, select_button/select, back_button, back_button/back: U, BOTTOM, CENTRE_H.

#### 1h. 2P in-game CSD (split screen HUD pieces that remain CSD)
| Path | Flags |
|---|---|
| 2P_Ring.xncp/Ring1P/Null_ring/ring, Ring2P/Null_ring/ring | U, AUTO_H, TOP |
| 2P_Star.xncp/Star | U, TOP, CENTRE_H |
| 2P_Start.xncp/Start | U, FORCE_UNSTRETCH (no alignment given, so centre) |
| 2P_CommandWindow.xncp Window/window/{info_bg, info_bg2, window1, window2}, Weapon/weapon/{icon_gun, button1}, Car_BCom/Null/{icon_car, button2}, CarMeter/{outline, meter_bg, meter} (11) | U, AUTO_H, BOTTOM |
Note: the 2P_CommandWindow entries list leaf casts only (no scene/group entries), unlike the 1P CommandWindow which lists group nodes too.

#### 1i. Egg Dealer - see 1a EggBoss.
#### 1j. Mission windows - see TWindow (1a), partner/boss intro banners (1a), result (1c).
#### 1k. Staff roll - not in the table; handled in the primitive-draw hook (section 2).

#### Elements whose anchor differs from what screen position suggests
1. BossGauge (near top of screen) is BOTTOM_CENTRE while Hero/Dark gauges are TOP_CENTRE.
2. Result Main = centre, but Main/result = LEFT (child cast overrides panel).
3. EggBoss: DisplaySml and reels 1-3 = RIGHT, DisplayLg = CENTRE_H.
4. honban Loop/ultimate = LEFT + IGN, Story_Library Background = LEFT + IGN, BattleMode Kazari = LEFT + IGN, SelectMode title = LEFT + IGN (corner decorations hug the true edge, everything else in the same file squeezes to centre).
5. Loading_* = RIGHT + IGN even though the element is a small indicator (anchored to the true edge).
6. Title title2-6 are full-screen animated quads forced to squeeze (centre), versus normal backgrounds left stretched.
7. SelectMode bg / text_bg_* = NONE inside a CENTER_UNSTRETCH scene: explicit exemption.
8. Circus Park Null_0015, bomb_ligid, bomb_parp = SELF_H (squeeze in place) inside a centre-anchored scene: they fly across the screen.
9. TWindow, 2P_Ring, InfoWindow, CommandWindow, MChara/BChara, GameOver/Loop = AUTO_H: anchor chosen per quad, so two-sided widgets split to opposite edges but a widget whose quads straddle a third boundary can tear.
10. Blast prompts (Hissatu XButton/YButton) = TOP_CENTRE although they sit at the lower side of a gauge icon; Synopsis Button = BOTTOM_RIGHT.

---------------------------------------------------------------------
### 2. Non-CSD 2D

#### 2.1 Number drawing (DrawNumberHud, GetHudNumberFlags)
The game prints rings/timer/score/ammo by calling one shared digit-run function with (x, y, text, align 0=left,1=centre,2=right). One anchor per whole string so digits keep spacing. A per-call-site table keyed by exact (x, y, align) gives flags; unknown sites fall back to **UNSTRETCH + AUTO_H** (auto left/centre/right by the string's bounding box).
| Site (x, y, align) | Meaning | Flags |
|---|---|---|
| (120, 397, centre) and (520, 397, centre) | weapon ammo "%03d", one per half | U + BOTTOM_LEFT / BOTTOM_RIGHT |
| (84, 68, centre) | Ring1P count | U + TOP_LEFT |
| (560, 68, centre) | Ring2P count | U + TOP_RIGHT |
| (78, 395 / 332 / 266, centre) | ring / lives / key stack | U + BOTTOM_LEFT (share one anchor, so the stack cannot split apart) |
| (320, 41, centre) | timer 1P and 2P | U + TOP_CENTRE |
| (320, 61, centre) | main score "%08d" | U + TOP_CENTRE |
| (477, 61, left) and (54, 61, left) | two side scores "%07d" | U + TOP_CENTRE (note: left-aligned strings placed on the sides, but anchored to centre!) |
| (320, 100, centre) | timer second pass | U + TOP_CENTRE |
Math: glyph run width = (n-1)*spacing + n*glyphWidth*scaleX (read from the font object). Left edge = x, x - width/2 or x - width depending on align. Anchor is resolved from that interval, then each digit quad vertex uses the same anchor, so spacing shrinks uniformly. Hooked right when digit quads are built, no restore needed. Skipped on the main menu (to avoid breaking menu text) unless in stage-select mode.
Stage select best-time numbers: drawn by the same function but in a centred 4:3 format; they are anchored at layout x -> screen = 320 + (x-320)*unstretchX and likewise Y using the glyph's bottom (y + glyph height). This reproduces how the CSD above them was squeezed to centre.

#### 2.2 Projected / world-attached 2D (cannot be fixed at vertex level)
These are placed each frame by projecting a world point with the camera's own FOV, which the port never widens for 2D.
- Enemy HP bar position, weapon lock-on reticle position, button-prompt arrow ("VH arrow", above air saucers/checkpoints etc., also a split-screen variant): the projected screen point (x, y) is squeezed about the centre of the camera screen rectangle (rect x + w/2, y + h/2): new = centre + (p - centre) * unstretch. Done before the screen cull test so the cull stays consistent.
- HP bar scale and VH arrow scale: scale X/Y values passed to the CSD SetScale are multiplied by unstretchX/Y (game scales relative to 640x480, which the stretch then widens).
- Lock-on reticle quad: its 4 corners are squeezed about the projected centre after the original builds them (X by unstretchX, Y by unstretchY). Includes the bird targets in the Ark.
- Lock-on also takes a 3P/4P view scale (see split screen).
- Item reward icon (from checkpoint item pickups): quad rebuilt each frame at a fixed memory address; unstretch with BOTTOM_CENTRE anchor (UNSTRETCH | BOTTOM | CENTRE_H) in 1P/2P; in 3P/4P modes first re-fit into the player's viewport, then SELF_H on X and own centre-Y on Y.
- Fade-out logo (bottom-right during fade): U, BOTTOM_RIGHT, IGN.
- Lens flare: each flare quad squeezed about its own centre by unstretchX/Y (the flare is camera-projected but the sprites are not).
- Camera red filter (Gun Fortress), rain overlay (Sky Troops): full-screen quads positioned in front of the 3D camera; scale X is multiplied by aspect/(4/3) when wider than 4:3, or scale Y divided by it when narrower (this is a grow, not a squeeze).
- Quad-model cutscene pictures (flashback photos, "space" backdrop): table of 26 DFF names + file hash with per-picture calibration {X multiplier at 16:9, Y multiplier}. X multiplier is rescaled to the current aspect: max(1, calX * aspect/(16/9)); Y fixed. Vertical axis is detected as whichever of Y or Z spans in the quad (some quads lie in XZ). Bounding sphere radius is enlarged so it is not culled. Calibrations are hand-tuned per event: 2.2x1.2 (Cosmic Fall ev 6301), 1.6x1.1 (flashback pics ev 6xxx/4111), 1.3x1.1 (ADV2 pics and space epilogue 1.3x1.2), 1.2x1.1 (Prison Island intro), 1.5x1.1 (Sky Troops intro ships). Comment states this is inconsistent: some cutscenes are wide enough but not tall enough, or the opposite.
- Camera view window (3D): Hor+ (multiply X view window by aspect/(4/3)) when wider than 4:3, Vert+ otherwise. Applied to the culling/clip planes and the projection reciprocal.

#### 2.3 Text (bitmap fonts via Rt2d)
- Rt2d fonts push glyph quads through the 3D pipeline, so the Hor+ projection stretches them. Port patches the transform call: vertices are scaled about a pivot (device matrix translation / scale) by aspect/(4/3) on X (wide) or by 1/(that) on Y (narrow). This runs only during font show-bitmap / show-outline calls and is DISABLED in stage-select mode, synopsis text, library text, other gamemodes (App::s_gamemode != -1) and while an event/cutscene is playing.
- Synopsis story lines and library sequence list text: explicit flags skip the font un-stretch because the surrounding CSD handles it.
- Staff roll: each 2D quad gets CENTER_UNSTRETCH inside the 2D-primitive draw hook.

#### 2.4 Prompts / message windows (prompt_patches.cpp)
- The port replaces specific guest message boxes (save-related, pause restart/quit, unlock messages, etc.) with its own ImGui window, suppressing the guest text and mirroring the guest's layout.
- Position conversion `GuestTextToViewport(x, y)`: normalised x = x / (4/3), y = 1 - y; then if the font un-stretch is not active (stage select, synopsis, library, menus, events) the point is squeezed about screen centre by unstretchX/Y (0.5 + (p - 0.5) * unstretch); otherwise left as is. Button positions use the guest option-row table, drop and a text scale factor, text baseline offset uses the font's baseline.
- Message window is only drawn once the guest open animation reaches 1.0; closed early while opening/closing.
- Confirmation prompts can be skipped by config (ConfirmationPrompt) except when save is damaged.
- Pause dim: the black_cover bug is patched here (see PAUSE_DIM).

#### 2.5 Button guide (ui/button_guide.cpp, ImGui)
- Laid out in the 640x480 space via Scale() (viewport height / 480 on 4:3+ ), with the menu box left/width helpers, bottom y at 480; left-aligned buttons start at 10% of menu box width, right-aligned end at 88%; default side margin via SetSideMargins(); `SetLayout(left, right, top)` anchors to display centre instead. The y origin uses `g_aspectRatioOffsetY * 2` (letterbox offset for the narrow case). Achievement menu uses horizontal centre + narrow margin.

---------------------------------------------------------------------
### 3. Scaling model

#### 3.1 ComputeOffsets (runs after every viewport/size change)
- aspect = viewportW / viewportH. Viewport is already the user-chosen ratio letterboxed/pillarboxed inside the swap chain (Config AspectRatio: Auto, 4:3, 16:9, 16:10, 5:4, 21:9; Auto = window shape).
- If aspect >= 4:3: offsetX = (W - H*16/9)/2 (can be negative, i.e. a centred 16:9 reference), offsetY = 0, scale = H/480. Gameplay scale is 1.0, except for 4:3 <= aspect < 16:9: there it blends from 1.0 (the 4:3 value; ComputeScale(4/3) evaluates to exactly 1.0) toward aspect/(16/9) with factor clamp((aspect - 4/3)/(16/10 - 4/3), 0, 1). So at 16:10 it equals 0.9, between 16:10 and 16:9 it follows aspect/(16/9) up to 1.0 at 16:9, and from 16:9 up it stays 1.0 (comment: 'keep same scale above Steam Deck aspect ratio'). This is the 'narrow-aspect (Steam Deck) interpolation'; it feeds horzCentre/vertCentre for ImGui panels, not the CSD unstretch.
- If aspect < 4:3: image is a 4:3 box centred in the viewport (offsetX = (W - W*4/3)/2 which is negative, offsetY = (H - W/(4/3))/2), scale = W/640, gameplay scale = ComputeScale(4/3) = 1.0.
- Narrow scale term g_aspectRatioNarrowScale = clamp((aspect - 4/3) / (16/9 - 4/3), 0, 1); g_aspectRatioNarrowMargin = lerp(50, 0, that), a margin in layout units that shrinks to 0 at 16:9 (used by ImGui menus such as achievements).
- horzCentre = offsetX + 640*(1 - gameplayScale)*scale; vertCentre = offsetY + 360*(1 - gameplayScale)*scale (reference centres used by ImGui panels).
- g_hudUnstretchX = min(1, (4/3)/aspect); g_hudUnstretchY = min(1, aspect/(4/3)). Used by all CSD, number, projected-point, lens flare, bloom code.

#### 3.2 HUD width limiting (MaxHUDWidth)
- Presets (g_hudWidthPresets): MAX (nullptr/0.0), 4:3, 16:9, 16:10, 21:9. Any other numeric value in the config is "Custom" (matched to preset within 0.01).
- CSD frame width = 640 * clamp(maxAspect / screenAspect, unstretchX, 1); left = (640 - width)/2. LEFT/RIGHT anchors map layout 0/640 to frame left/right in layout units (so a narrower HUD insets the corners); TOP/BOTTOM map to 0/480. IGNORE_MAX_WIDTH uses the full 640 frame. The lower clamp unstretchX means the HUD is never made narrower than 4:3.
- The options menu hides the HUD during live preview except while MaxHUDWidth is highlighted (so you can see the effect).

#### 3.3 Config defaults (user/config_def.h, [Video])
| Option | Type | Default |
|---|---|---|
| AspectRatio | enum Auto, 4:3, 16:9, 16:10, 5:4, 21:9 | Auto |
| WindowSize | int | -1 |
| ResolutionScale | float (clamped 0.25-2.0) | 1.0 |
| FMVAspectRatio | enum Stretched, Fit, Fill | **Fit** |
| MaxHUDWidth | float (0 = MAX) | 0.0 (MAX) |
| BloomIntensity | float 0..1, rounded to 0.01 | 0.7 |
| Fullscreen | bool | true |
| FPS | int | 60 |
| CutsceneFramerate | Original/Unlocked | Original |
There are no per-edge safe-zone sliders or per-element alignment options; the only HUD alignment option is HUD Width (labelled "Constrain HUD to a chosen aspect ratio. Primarily for ultrawide monitors.").

#### 3.4 Bloom (bloom_patches.cpp)
- The glare draws a 4-vertex quad per tap; taps are offsets in screen-fraction of the source. On ultrawide the glare spreads too wide (and too narrow on narrow screens), "esp for jet shoes".
- Fix: each tap's X/Y offset is rescaled to (offset + 0.5) * unstretch - 0.5, i.e. scaled about the half-pixel centre with unstretchX/Y, so glare spreads like 4:3. Offset 0 (the composite quad) is left alone.
- Second fix in the same function: the original shrank the destination quad to avoid sampling outside the texture, leaving a dark blurry band along top and left. Port keeps the quad full size (offset 0) and shifts the UV window by (u1-u0)*offset/width instead.

---------------------------------------------------------------------
### 4. Split screen

Standard 2P: the game's own split (top/bottom or left/right per mode) with 2P_* CSD above, anchored AUTO_H/TOP or AUTO_H/BOTTOM, 2P Star TOP_CENTRE, 2P_Result centred with FORCE.

Extended 3P/4P (custom mod, Config::ExtendedMultiplayer, default false, hidden code):
- Viewports: 4P = four equal quadrants (w = W/2, h = H/2), FOV scale 0.75 on each; 3P = three vertical thirds (third gets the leftover pixels), P3/P4 world UI user ids set to 2/3, FOV 0.5/1.0 for the thirds layout. Rects stored at fixed memory addresses; 2D rect origins are used by item icons only.
- Per-view HUD scale `GetMultiplayerViewScale()`: min(1, viewHeight/screenHeight * fullScaleY / (viewScaleY*1.25)). Applied to: enemy HP bar scale, lock-on reticle scale, world-attached CSD (world-CSD scale argument).
- Item icon in 3P/4P: re-centred in the player's rect; scale = rectH/fullH; centreX = rect centre X, Y centre = rectY + 372*scale (372 is the original icon centre Y in layout units).
- The vanilla 1P/2P HUD CSD for extra players is hidden in 3P/4P: scenes hidden = 2P_Ring (Ring1P, Ring2P), 2P_CommandWindow (CarMeter, Car_BCom, Weapon, Window), 2P_Star/Star, HeroGauge/Scene_0000, DarkGauge/Scene_0000, TWindow TargetWindow/TargetDark + TargetHero, Hissatu_Hero/HissatuHero, Hissatu_Dark/HissatuDark, and BossGauge (additionally notifies the ImGui HUD that a boss gauge is visible).
- Replacement ImGui HUD (ui/multiplayer_hud.cpp), drawn on the background draw list over the full ImGui display size (NOT the 16:9 reference): visible only when extra-player mode and the stage action state is init/play/restart/continue/dead and the HUD toggle flag is on; alpha = 1 - game fade value.
  - Per-player counters inside that player's viewport rect (rect / screen size * display size): ring icon bottom-left (margin 20 unit), ammo icon+count bottom-right (margin 20 unit; hidden if no weapon); unit = min(rect.w/640, rect.h/480) times 1.5 in 3P.
  - Shared HUD at screen top centre: timer (hh:mm:ss format "%02d:%02d:%02d", clamps at 99:59:99), Dark gauge left and Hero gauge right of centre separated by a gap of (8 glyph widths + 2 *10 units)* unit, optional boss gauge centred below the gauges, hero/dark mission counters ("cur/target" with coloured tag) outside the gauges. unit for shared HUD = min(W/1280, H/960)*1.25 in 4P, min(W/1920, H/480)*1.5 in 3P.
  - Gauge geometry (frame widths 45, fill 156.5/155 etc.) is copied from HeroGauge/DarkGauge/BossGauge xncp layouts.
  - Digits use their own texture and a light/dark vertical gradient to match the game's number shading.
- Fallback rect if the game rect is missing: 4P quadrants, 3P thirds.

---------------------------------------------------------------------
### 5. FMV aspect options (fmv_patches.cpp)
- FMVs are 4:3 and drawn as a single quad that covers the camera raster (game uses a 640x480 and a 1024x512 raster). Movie texture power-of-two padding: maxU = frameW / nextPow2(frameW), maxV likewise.
- Applies only when the draw call is full screen (offset 0,0 and scale 1,1); smaller windowed movie draws are left alone.
- Stretched: original behaviour, UVs reset to (0,0)-(maxU,maxV).
- Fit (default): keep full texture, shrink the quad: if screen wider than 4:3, scaleX = (4/3)/aspect, else scaleY = aspect/(4/3); offsets = round(raster * (1 - scale)/2) so it is centred with black bars.
- Fill: keep quad full-screen and crop the UVs: if wider, cropV = (4/3)/aspect, else cropU = aspect/(4/3); margins = maxUV * (1 - crop)/2 on both sides.
- UVs are reset every call because the option may change mid-movie. The frame width/height are saved for a texture-clamp fix in video.cpp. The 4:3 shape is a constant, never read back from the frame size.

---------------------------------------------------------------------
### 6. Lessons / gotchas stated in comments

1. Centre is the safe default: every quad of a widget moves to the same point so a layout cannot tear. Corner anchors are opt-in per widget, and "give every quad of a widget the same flag".
2. Corner combo flags carry an edge bit; adding CENTRE_H to TOP_RIGHT would leave RIGHT set too. Centre is tested first on purpose.
3. Full-width quads are assumed to be backgrounds, fades and letterbox bars and stay stretched; animated logos that grow to full width need FORCE_UNSTRETCH.
4. The game hides elements by parking them off-screen and relying on viewport clipping. Squeezing can pull them back into view, so the code (a) skips a primitive that was hidden and would become visible, and (b) restores the original position when an element whose centre was off-screen would now land inside ("UI elements persist to draw on ultrawide when they shouldn't").
5. Quads are rebuilt every frame, so vertices are modified before the draw and restored right after for CSD (otherwise state accumulates); items rebuilt each frame (item icon, number digits, lock-on reticle) skip the restore.
6. Projection-placed UI (enemy HP bars, lock-on, prompt arrows, item icon) uses the 3D camera's own FOV, which is never widened for 2D, so it must be fixed at the projected point (before culling) plus its scale, never at vertices alone.
7. DrawNumberHud is shared by everything that prints numbers and one alignment cannot fit all, so each call site gets its own entry keyed by exact (x, y, align). Align must be part of the key since x alone does not place the digits. Stack members must share an anchor or the stack splits apart.
8. DrawNumberHud also draws stage-select records; they "don't align properly" at high resolution and each digit is a quad, so without one anchor for the string there is a "huge gap".
9. Duplicate/overriding entries: cast overrides cast node overrides scene (no merge). NONE entries are used as explicit exemptions. The table contains a duplicate key (SelectMode/title) which hides a conflict.
10. Dimension of nesting: some CSD (Result, Pause, Library) are scene-centred but a child needs a different anchor ("LIBRARY text in the corner", Result/Main/result).
11. Elements drawn by code but over CSD (stage select best times) must be moved to the same spot as the squeezed CSD.
12. Pause menu: the port adds options so the menu background grows, everything below it shifts down by 15 layout units per added item, and the boss pause variant background already supports four options. The Xbox black_cover NCP double-dim bug has to be compensated.
13. Bloom is aspect dependent (narrow/wide spreads); scale taps by the unstretch factor. Original also had a dark band at top/left from shrinking the destination quad.
14. Camera view window: Hor+ above 4:3, Vert+ below. Reciprocal view window must be recalculated manually because the set-view-window function only runs when FOV changes.
15. CSD loaded from stage archives or memory buffers lose their path; the port keys buffers by address and file name only, and registers entries (checkpoint warp menu, Circus Park) without a csdFiles prefix.
16. Half-pixel offset: the D3D "screen space offset" call is turned into a no-op to remove the half-pixel issue.
17. Shift-JIS cast names crash the path map; they are sanitised to underscores (so table keys contain `___R_s_[`).
18. Multiplayer: 3P/4P HUD is replaced by an ImGui overlay, vanilla CSD scenes are skipped by scene path; per-view scale is derived from the game's own viewport scale fields, not recomputed.
19. HUD Width never goes below 4:3 equivalents (clamp lower bound = unstretchX).
20. Picture-model calibrations are per-event hand tuning and acknowledged as inconsistent.
