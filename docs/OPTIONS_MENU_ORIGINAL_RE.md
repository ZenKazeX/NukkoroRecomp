# The original Options screen (PAL GUPP8P) - reverse-engineering notes (2026-11-05)

These notes cover the Options overhaul's Phase I (docs/OPTIONS_MENU_OVERHAUL.md). They come from read-only
disassembly (doldis / dolxref) and decoded assets. Nothing was run. [V] = verified from disassembly or data,
[I] = inferred. They correct parts of docs/OPTIONS_DISPLAY_TILE.md, listed first.

## Corrections to OPTIONS_DISPLAY_TILE.md

- **The audio items:** item 0 "Sound" is the sound test (a music player). Item 2 "System" is the speaker output:
  Stereo / Monaural / Dolby Pro Logic II. [V]
- **The vtable:** it starts at 0x805589E0: {RTTI, 0, dtor 0x802B820C, vf1 0x802B81E8, vf2 0x802B81C4}. [V]
  - vf1 calls 0x802BA004, the per-frame state machine, not setup.
  - vf2 calls 0x802B8D88, the draw.
- **The save check:** A runs it (0x802B8828) for items 1, 2 and 5. Item 3 goes through a fade state. [V]
- **The subtitle rows** are in the order JP, EN, FR, SP, GE, IT. The simulator's order (EN, FR, GE, IT, SP, JP) is
  wrong. [V]

## 1. Items

The item table is at 0x80558940, stride 0x14. Texts are from Advertise_EN.fnt: key = id * 1000 + 100 in entry
field +4, binary search in 0x80049BE8.

| # | Name | Help (message) | Rows (messages) |
|---|---|---|---|
| 0 | Sound | "Listen to the in-game music." (5003) | unlocked tracks only, 6100+i ("I AM" .. "ENDING") |
| 1 | Vibe | "Change the Controller Rumble settings." (5703) | 0x805F9A58: ON (5414), OFF (5415) |
| 2 | System | "Change the speaker output settings." (5004) | 0x804FF784: Stereo, Monaural, Dolby Pro Logic II (5417-5419) |
| 3 | Subtitle | "Change the subtitled language." (5002) | 0x804FF758: 日本語, English, Français, Español, Deutsch, Italiano (5407, 5408, 5409, 5411, 5410, 5412) |
| 4 | Load | "Load Game Data. Your progress and any changes made will be saved automatically." (5006) | 0x805F9A60: Load (5416) |
| 5 | Audio | "Change the characters spoken language." (5001) | 0x805F9A50: 日本語, English |

- **Item 0's list is built at runtime.** Its count (0x80558950) and values pointer (0x8055894C) are rebuilt by
  0x802B94A0 from the save's unlock bits (save + 0x1714). [V]
- **Settings block G = 0x8057E760** [V]:

  | Field | Content |
  |---|---|
  | G + 0x78 | speaker: 0 stereo, 1 mono, 2 Dolby PL2 |
  | G + 0x7C (byte) | vibration: 1 = on; row 0 (ON) sets 1 |
  | G + 0x58 | text language, also r13 + 0x2D20: 0 JP, 1 EN, 2 FR, 3 SP, 4 GE, 5 IT (suffixes at 0x8051E648) |
  | G + 0x5C | voice language: 0 JP, 1 EN |
  | G + 0x7D (byte) | save on exit (set by the memory check) [I] |

### What applying each item does

The apply function is 0x802B85D8.

**Speaker: `SetSoundMode(&G+0x78, m)`, 0x80306100.** [V]
1. OSSetSoundMode: mono when m = 1, otherwise stereo. This is the IPL SRAM bit, persisted by native_ipl to
   SRAM.pc.raw.
2. The SE manager's SetSoundMode 0x801D50AC: stops all SE, then AudioGCN vt + 0x58 (0x801F21AC):
   - mono: MIX mode 0 (r13 + 0x5DC8) and the GCAX mono flag ctx + 0xFDFA = 1 (note-on 0x8043D56C averages L / R),
     AXSetMode 0;
   - stereo: MIX 1, AXSetMode 0;
   - PL2: MIX 1, flag ctx + 0xFDFB, AXSetMode 2.
3. The BGM side: 0x801D79DC(mode), then 0x801D8314.

GCAX_SOUND_MAP calls 0xFDFA "surround"; it is the mono flag.

Native side: native_se_math / native_se_manager read G + 0x78. AX list op 0x07 maps to StereoFold, ops 0x13 / 0x10 to
DolbyMatrix. The host output is always 2-channel (PL2 = matrix-encoded stereo).

**Vibration: `SetVibration(&G+0x7C, on)`, 0x80306068.** [V]
- Off stops every motor (0x801782EC, then 0x8034C198).
- The apply then asks for rumble pattern 4 on every port (0x8034ED50(buf, 4, -2)). The gate 0x8034C01C passes only
  when G + 0x7C != 0, so ON gives a test rumble.
- Readers of the flag:
  - the gate (from 0x8034BD74 / 0x8034BEF4);
  - the save-data init 0x801E54F4;
  - the loaders 0x8035AA28 / 0x8035B414.
- On PC: PADControlMotor goes to `ManagerImpPc::set_rumble`, then the shadow_main listener, then XInput at 0xC000
  on both motors.

**Subtitle = the whole front-end text language.** [V]
- It writes G + 0x58 and r13 + 0x2D20.
- It reloads the text and fonts (0x801E621C, 0x802B5B6C) and the per-language Loading CSD (0x802BE860:
  csdFiles/Loading + suffix).
- On PAL impl + 0x25 = 0, so row index = language.

**Load.** [V]
- It runs SaveCycle::CycleTask (0x802E4314) in mode 1 (Load).
- Then it re-reads every value from G (0x802B94A0). A changed language goes through the fade / reload state 3.

**Audio (voice language).** [V]
- It calls 0x801DFA40: StopAllSE, a voice / stream stop (0x801D68EC), G + 0x5C, then a reload (0x801D690C).
- 0x801D844C reopens PRS_VOICE_J/E.afs, but picks the file by G + 0x40 (the region) [I], not by G + 0x5C.
- G + 0x5C picks the event voices `sng_EV%04d_J/_E.adx` (0x80173770; format strings 0x804C4E2C / 0x804C4E40).
- **The PAL disc has no `*_J.adx` and no PRS_VOICE_J.afs.** Only sound/*_J.mlt exist. Japanese voice would lose
  the event voices [I], so it is not offered.
- The constructor 0x802BA13C sets impl + 0x26 = 1 unconditionally (the tile is hidden). impl + 0x24 and impl + 0x25
  stay 0. [V]

## 2. Runtime state and flow [V]

**The cursor block 0x80585528:**

| Offset | Content |
|---|---|
| + 0x00 | row |
| + 0x04 | column |
| + 0x08 + 4i | the first visible row of item i |
| + 0x20 + 4i | the value of item i |
| + 0x38 .. + 0x6C | a copy of + 0x00 .. + 0x34 |
| + 0x70 | the refresh byte |

**impl:**

| Offset | Content |
|---|---|
| + 0 | state |
| + 8 | substate |
| + 0x1C | motion busy |
| + 0x1E | draw text |
| + 0x20 | card task |
| + 0x30 | a sound-test track is playing |
| + 0x31 | "applied" (no reader) |
| + 0x34 | move delay |

**States (0x802BA004):**

| State | Function | What it does |
|---|---|---|
| 0 | 0x802B9F2C | load, setup 0x802B833C, BGOpen / MenuOpen |
| 1 | 0x802B9A54 | grid |
| 2 | 0x802B964C | page |
| 3 | 0x802B9230 | language: fade out, apply, fade in |
| 4 | 0x802B9394 | load from card |
| 5 | 0x802B90B0 | exit |

**The copy.** 0x802B9330 takes it at construction, on every grid cursor change, and when the refresh byte is set.
In effect it is the snapshot when the page opens.

**The page:**
- Up / down move only the yellow value; nothing applies.
- A:
  - item 0 plays the track (0x801E52F0, then 0x801D55EC);
  - item 4 goes to state 4;
  - items 1, 2 and 5: if 0x802B8828 sees a change, apply (0x802B85D8), the decide sound, back to the grid.
    **No change: A does nothing.**
  - item 3: if changed, state 3 with the fade 0x80173C5C.
- B stops a test track, restores the copy (0x802B99F0), returns to the grid and plays the back sound.

**The card save.** 0x802B85D8 does not write the card. It copies G + 0x58 / + 0x5C / + 0x78 / + 0x7C into the save
object (0x800BBB34(): + 0x18, + 0x1C, + 0x20, + 0x24 byte) and re-snapshots. **On leaving** (B on the grid,
state 5, after MenuClose / BGClose), the game runs CycleTask mode 2 (Save) when G + 0x7D is set. It does so
unconditionally, changed or not, and then returns to the Title phase.

## 3. Visuals

The source is Options.gncp. Positions are on the 640 x 480 canvas, after the motions.

**Options_<lang>.txd** (slot = 3 - entry):

| Slot | Texture | Format |
|---|---|---|
| 0 | TitleBg 512 | C8 + RGB565 |
| 1 | Options 512 | C8 + RGB5A3: the vertical "OPTIONS", the arrow, the fire frames |
| 2 | Options2 512 | RGB5A3 |
| 3 | gamemenu 256 | C8: button glyphs, "select" / "exit" |

**Options2:**
- Tile cells, 82 x 82, normal / selected (red):

  | Tile | Normal | Selected |
  |---|---|---|
  | Sound | 41 (0,0) | 47 (166,0) |
  | Vibe | 42 (83,0) | 48 (249,0) |
  | System | 43 (0,82) | 49 (166,82) |
  | Subtitle | 44 (83,82) | 50 (249,82) |
  | Load | 45 (0,164) | 51 (166,164) |
  | Audio | 46 (83,164) | 52 (249,164) |

- Panel `menu_bg`: cell 54 at (0,247), 274 x 267.
- Lines: cells 53, 56.
- Highlight: cell 40 at (491,0), 21 x 225 (no code use found).

**Layout:**
- **The grid.** Scene "menu"; table 0x804FF7A8 = [[Sound, Vibe], [System, Subtitle], [Load, Audio]].
  - Tile left edges at x 140 / 255; centres at y 170 / 262 / 354, after MenuOpen slides them down.
  - Each item's pose motion sets its tile to cell 1 (the selected art).
- **The help line:** the `Null_box` anchor at (143,68), colour 0.
- **The panel:** `menu_bg` (scene "backgound") slides from x 641 to 366 at y 104.
- **The title:** cast "Options" at (73,10), alpha fading to 0x98.
- **The rows:** scene "fontnull", text1-7 at (399, 140 + 40n); text8 at y 420 is unused.
  - Drawn by 0x802B8BC4. The current value is colour 2 (yellow), the others colour 0 (white), from table 0x80558AE8
    {white, black, yellow, red, green, blue}.
  - Brightness x1.0 in the page, x0.5 while the cursor is in the grid.
- **The prompts:**
  - `exit_btn` (65,417): pattern 1 = the GC Y glyph (gamemenu cell 64 at (184,203), 28 x 28) + "exit" (cell 58);
  - `select_btn` (495,417): the A glyph (cell 63) + "select" (cell 57).
  - Pattern 3 (the alternate layout) is gated by 0x80356EC0, which is 0 on GC.

**The HUD lives icon (for the Gameplay tile):**
- `game/files/csdFiles/InfoWindow.txd`, the same as `game/files/InfoWindow.txd`.
- Texture `000_windowitem`: 128 x 128, C8 (GX format 9), RGB5A3 palette. Shadow's head tile is about
  x 43-84, y 85-128 (42 px tiles with transparent gutters at 42 / 84). [V, pixel scan]
- CSD: InfoWindow.gncp, scene "Zanki", cast `zanki`, cell 1. It is a child of Null_0000, which slides from
  x -50 to +49 at y 401; on screen the box is about (52,298)-(99,335).
- **Open:** the gncp declares the texture as 128 x 64, and cell 1 (u 0.3125-0.680, v 0-0.578) does not land on the
  head tile of the 128 x 128 file. Check it in a capture.

**Dolphin reference:** none exists under H:\ShadowTheHedgehogDiskGC_Extract (source/.build/validation/menu-ref is
empty).

## 4. Input and phase flow [V]

**Menu buttons.** Table 0x80560788, built by 0x80357220, at *(r13 + 0x59E4); up to 3 masks per id:

| Id | Button |
|---|---|
| 0 | Start 0x400 |
| 1 | decide: Start / X / A |
| 2 | back: Y / B |
| 3 | up 0x40040 |
| 4 | down 0x80080 |
| 5 | left 0x100100 |
| 6 | right 0x200200 |
| 7 | 0x800 (never reported on PC) |
| 8 | R 0x2000 |
| 9 | L 0x1000 |

- L / R exist (ids 9 / 8), but Options never asks for them. They are free for category switching.
- Z is not in the table. X counts as decide and Y as back.
- The grid uses edge queries for ids 1-6. The page uses the repeat query (0x802B810C) for up / down and the edge query
  for A / B.

**Entering and leaving:**
- Title::Phase main menu (honban.gncp "Menu": 1P game, 2P game, Options). A on entry 2 calls RequestPhase(ctrl, 2)
  (0x802BE1F4).
- The switch 0x802B687C (jump table 0x805587D0): 1 = Title (0x802BD030), 2 = Option (0x802B690C -> 0x802B82E4 ->
  constructor 0x802BA13C).
- State 5 calls RequestPhase(1) back to the Title (0x802B91F4).
