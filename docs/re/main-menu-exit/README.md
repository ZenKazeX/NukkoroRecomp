# The main menu and the caution screens (PAL GUPP8P), and the EXIT entry

What this records: how the title's main menu (1P GAME / 2P GAME / OPTIONS) and the game's caution / confirmation
windows are built and drawn, as found before adding an EXIT entry (user request 2026-11-27). Marks:
**MEASURED** = seen in a run or computed from the disc files; **READ** = read from the original words (Ghidra 12.1.4
listing / decompilation of `game/sys/main.dol`, `tools/doldis.py`, `tools/dolxref.py`) or from the files with
`tools/assets/gncp_read.py` / `txd_extract.py`; **INFERRED** = follows from what was read, not shown directly;
**UNRESOLVED** = not known.

## 1. The main menu is a CSD scene with three pre-drawn text pictures

- **READ** `csdFiles/honban.gncp` (a bare NGIF project; its textures are `csdFiles/honban_<lang>.txd`) has the scenes
  `Loop`, `Menu` and `Title`. Scene `Menu` has 14 groups / 16 casts: `logo`, `title`, `head_bg`, `1pgame`, `2pgame`,
  `options`, `storymode`, `selectmode`, `hardmode`, `new`, `resume`, `select_button` + `select`, `back_button` +
  `back`, `blackcover` (group 13, a full-screen untextured quad).
- **READ** The entries are not text: each is a cell of texture 2 of the scene, the 256 x 256 C8 sheet `gamemenu` of
  `honban_<lang>.txd` (palette RGB5A3). In `honban_EN.txd` the sheet holds, one 21-pixel row each, `1P GAME`,
  `2P GAME`, `OPTIONS`, `STORY MODE`, `SELECT MODE`, `EXPERT MODE`, `NEW`, `RESUME`, `CONTINUE`, the `select` /
  `exit` prompt words and the button caps. Cells 8 / 9 / 10 (u 0.0625..0.9375, v 0..0.082 / 0.082..0.164 /
  0.164..0.246) are the three entries; casts `1pgame` / `2pgame` / `options` are 224 x 21 quads at x 208..432,
  y centres 282 / 312 / 343 on the 640 x 480 canvas (transform y 0.5875 / 0.65 / 0.7146).
- **READ** Every language has its own sheet with its own words (FR `JEU 1J`, `JEU 2J`, `OPTIONS`, `MODE HISTOIRE`,
  `MODE SELECTION`, `MODE EXPERT`, `NOUVEAU`, `REPRENDRE`; GE `1 SP SPIEL` ...; IT `PARTITA 1G` ...; SP `PARTIDA 1J` ...;
  JP katakana). The letters are a white / grey pixel face with a 3-pixel black outline that is shared between
  neighbouring letters (the gap between two letters is 2-3 columns of outline). **MEASURED** (column profile of the
  EN sheet): in `EXPERT MODE` (rows 106..126) the faces of E X P E R T are columns 50-60, 64-76, 80-91, 95-104,
  108-119, 123-134, the word's outline 47..137; in `OPTIONS` (rows 42..62) the I face is 125-127.
- **READ** Selection is colour only, done by CSD motions, one per entry: `Menu_1P`, `Menu_2P`, `Menu_Options` set the
  chosen entry's colour track to FFFFFFFF (white) and the others to 660000FF (the texture multiplied by dark red);
  scale stays 1.0; `head_bg` stays at alpha 0 in the main menu. `MenuOpen` (frames 0..15) grows the three entries
  from scale 0.5 and alpha 0 out of the centre (1P y 0.6292 -> 0.5875, 2P stays 0.65, Options 0.6729 -> 0.7146).
  `1POpen` / `HardOpen` fade 2P and Options out while the 1P sub-menu (story / select / expert, new / resume) comes in.
- **READ** The motion record layout (scene descriptor +0x34 count, +0x38 {groupCount, groupArray}, group entry
  {castCount, castArray}, cast entry {trackMask, trackArray}, track {flags, keyCount, keys}, key 24 bytes
  {frame, value, interpolation, 0, 0, 0}; +0x3C the motion names {name, index} sorted by name, +0x44 the
  {start, end} frame pair per motion index) is the one `tools/assets/gncp_read.py` documents. The pointer words of a
  project are listed in its `NOF0` chunk and relocated by the loader 0x80360224 (docs/re csd notes); the descriptor's
  pointer words are +0x18, +0x20, +0x28, +0x30, +0x38, +0x3C, +0x44 (**MEASURED**: exactly these offsets of each of the
  three descriptors are in `NOF0`).

## 2. The code that runs the menu

`Title::Phase` (AdvReal front end). The per-frame dispatcher **0x802BE51C** switches on the phase object's state word
`this+0` and, for state 3 (the main menu), calls **0x802BDF0C**, then the scene update 0x8003CE1C(this+0x10) and the
button-prompt update 0x80356D5C(this[9]) (**READ**, Ghidra listing). The menu's own globals are a block at
**0x805855A0**: +4 the main-menu cursor (0..2), +8 the 1P sub-menu cursor, +0xC the expert sub-menu cursor,
+0x10..+0x1F a snapshot of +0..+0xF, +0x20 a "refresh" byte, +0x24 the state to come back to (**READ**,
0x802BDBD0 copies the snapshot and clears +0x20; 0x802BE4E8 (this, state, flag) sets `this+0`, the sub-state
`this+8` (0 when flag, else 1) and the refresh byte).

0x802BDF0C, by sub-state `this+8` (**READ**, full listing in the session notes; addresses are the original words):

| Sub-state | What it does |
|---|---|
| 0 | `FUN_8003CE84(this+0x10)`; scene "Menu" (name pointer at r2-0xBDC = 0x805F9BA4) plays `MenuOpen` (0x805006C4) via 0x8003DA80; scene visibility 0x8003C900: "Title" (r2-0xBE0) off, "Menu" on; sub-state 1; 0x802C88F4(0x801E5FB8(), 3) |
| 1 | only when the caution manager is closed (0x801178FC -> 0x802E5D88: its alpha +0x18 == 0.0): decide (0x802B8168(1,4)): cursor 1 (2P) without two controllers (0x802BD088) -> sub-state 3 and sound 0xE004, else sub-state 2 and sound 0xE003; back (id 2): 0x802BE4E8(this, 2, 1) (back to the title) and 0xE004; up (id 3): cursor - 1 if > 0, sound 0xE002; down (id 4): cursor + 1 **if < 2**, 0xE002. Then, if the refresh byte is set or the block differs from its snapshot (memcmp 0x803A9DF8, 16 bytes): "Menu" plays the motion named by the table **0x80558A48**[cursor] = {`Menu_1P`, `Menu_2P`, `Menu_Options`}, the two visibility calls, and 0x802BDBD0 |
| 2 | cursor 0 -> 0x802BE4E8(this, 4, 1) (1P sub-menu); 1 -> block+0x24 = state, 0x802B7934(0x801E6110(), 7) (2P battle); 2 -> the same with 2 (Options) |
| 3 | 0x802E5E2C(0x801178FC(), 0x4A, 0, 0) until it returns 2 (the 2P "connect a second controller" caution), then sub-state 1 |
| >= 4 | nothing |

Input: 0x802B8168(id, 4) = `0x80357068(table 0x804FF6F8[id], 4)` when the caution manager is closed. 0x80357068 tests
the up-to-three masks `*(r13+0x59E4) + id*0xC` against the pad word `padManager + 0x30 + port*0x28`
(padManager = 0x8005185C() = *(r13+0x2D9C), port 4 = any controller); a mask matches whole, or by its 0x3C0 bits, or by
its 0x3C0000 bits. Only id 1 (decide) has a side effect: it records the deciding port at 0x805F1EE0 (**READ**). The
sounds go through 0x801D59AC(0x8005328C(), id, 0, 0): 0xE002 cursor, 0xE003 decide, 0xE004 back (**READ**).

The literal clamp `cmpwi r3,0x2` at **0x802BE0D8** and the three-word table 0x80558A48 (followed directly by the 1P
table 0x80558A54) are why a fourth entry needs code, not data (**READ**).

## 3. The caution / confirmation windows

They are not CSD. **READ**:

- One manager, created on first use by 0x801178FC (handle r13+0x3868 = **0x805EFD68**, object 0x38 bytes, ctor
  0x802E60EC on the MessageTask base 0x801FB3BC). Fields: +0 the font (an RCPtr; 0x801FB104 hands it out), +8 the
  window asked for this frame (reset to -1 every frame), +0xC the window shown, +0x10 the choice cursor, +0x14 the
  result, +0x18 the open fraction 0..1 (input of the menus is blocked while it is not 0.0), +0x2C closing, +0x30 the
  pad port whose A answers, +0x34 a flag.
- **Request = immediate mode.** A caller asks for window `id` every frame with 0x802E5E2C(handle, id, port, flag); the
  call returns 0 while it is open and unanswered and the chosen result once answered (2 at once for a window without
  choices). When nobody asks for a frame, the window fades out (0x802E5C4C, 0x802E5AE8; per-frame 0x802E61AC).
- **The descriptors** are a table at **0x805074A0**, 0x24 bytes per window id (0x00..0x4A): {message id, choice count,
  default choice, layout (1 horizontal, 2 vertical), choice message ids x3, results pointer, no-choice mode}.
  Examples: 0x4A = {6059 "Please connect second Controller...", 1 choice "OK"}; **0x2A = {6019, 2 choices, default 1,
  vertical, 6047 "Yes" / 6048 "No", results 0x8055A560 = {1, 2}}** - the format question, a Yes / No window that
  starts on No. The PS2 / Xbox questions are in the same table (0x37..0x39: horizontal Yes / No).
- **Drawing** (0x802E54B8 via 0x802E5AE8): 0x802E51B0 draws the game's **blue pop-up**: a box and its frame with
  RwIm2D (two triangle fans, colours read from 0x805ED7A0..0x805ED7AC: 000050DC, 101080DC, 000010DC, 3030C8DC, i.e.
  translucent dark and light blue), over whatever screen asks for it - the same window the main menu's own 2P caution
  (0x4A) uses; it is not the full-screen page of the boot's 50 / 60 Hz question; the question and the choices are text of the font's message table drawn by
  0x80049BE8(x, y, font, message id, 1, 0, 0, 0) (key = id * 1000 + 100, binary search in the table at font+0x5C,
  0x80049D60), the chosen line in colour 0x805ED798 and the others in 0x805ED794. Up / down (or left / right for the
  horizontal layout) move the choice with sound 0xE002; A (decide, the window's port) takes `results[choice]` with
  sound 0xE003 (0x802E5798). **B does nothing in a window with choices.**
- **The text** of the front end's windows is in `FONTS/ADVERTISE/Advertise_<lang>.fnt` (and the same messages in
  `FONTS/SAVE/Save_<lang>.fnt`): u32 count, 20-byte entries {text, key, ...}, UTF-16 texts drawn glyph by glyph from the
  language's atlas (`*.met` glyph lists). In memory an entry's first word is the text's address (0x80049D60 returns
  it and the drawer reads it as 16-bit units). **MEASURED** (glyph lists): every glyph of "Exit the game?",
  "Quitter le jeu ?", "Spiel beenden?", "Esci dal gioco?", "¿Salir del juego?" and the Japanese question exists in
  both the Advertise and the Save atlas of its language ("Uscire" does not: the Save atlas of IT has no "U").

## 4. What the EXIT entry does with this (implementation, runtime/src/gekko/main_menu_exit*.cpp)

- **The picture (INFERRED approach, MEASURED result in the runs):** built at mount from the player's disc, per
  language from that language's own `gamemenu` sheet. **MEASURED** (column profile of each sheet's letter faces): every
  sheet draws its letters with the face five rows into the cell and a 3-column outline; so a word is made of letter
  faces (all 21 rows from five above the face) with 3 columns of outline cut from the same sheet between them (a place
  where two straight-sided letters meet) and at its ends (a word's start / end). The 16-bit RGB5A3 palette values are
  copied as they are into an empty area of the same file's `TitleParts` texture (texture 0 of the scene, 512 x 512
  RGB5A3, rows 400..511 unused in all six languages, **MEASURED**), cell x 4..115, y 452..472, the word centred in it
  (the cell is as wide as the longest word plus margins; the menu's own cells are 224 wide and centred the
  same way). `gamemenu` (which the button-glyph patch rewrites) is not touched and the file keeps its size. The repo
  holds only column numbers and each sheet's FNV-1a 64 (a sheet that differs - another region, a mod - is never cut:
  the English word is used).

  | Text language | Word | Letters from (sheet rows) | Letters made |
  |---|---|---|---|
  | EN | EXIT | EXPERT MODE (E, X, T), OPTIONS (I) | none |
  | FR | QUITTER | JEU (U), MODE HISTOIRE (O, I, T), MODE EXPERT (E, R) | **Q derived**: HISTOIRE's O with a tail drawn over its lower right corner and one row below the baseline, in the sheet's own face / edge / outline colours (no sheet of the disc has a Q) |
  | GE | BEENDEN | EXPERTEN-MODUS (P, E, N), STORY-MODUS (D), OPTIONEN (E, N) | **B derived**: EXPERTEN's P with its bowl rows (rows 2..4 and the middle bar) repeated under the middle bar and its stem's foot closed by outline (no sheet of the disc has a B) |
  | IT | ESCI | MODALITA ESPERTO (E, S, I) | **C taken** from the English sheet (SELECT MODE's C, a real glyph of the same font), its outline trimmed by one row above and below to the Italian rows |
  | SP | SALIR | MODO SELECCIONAR (S, L, R), PARTIDA (A), MODO HISTORIA (I) | none |
  | JP | U+7D42 U+4E86 (shuuryou, "end") | - | **both kanji made at run time** from the game's message font (Advertise_JP.txd pages JP01 / JP06, the glyphs the blue pop-ups draw), scaled to the menu's 13-row face, brightened and cut at a coverage threshold, with the menu's shading (a lighter row under each stroke, a darker one under the glyph) and its black outline; the menu sheet's kanji (the ones in ZENKAI NO TSUZUKI KARA) were the comparison. shuuryou is what Japanese game menus write for quitting; GEEMU SHUURYOU would need katakana the sheet does not have either |

  Joins: two black columns between letters (the sheet's own gap between straight-sided letters), three outline columns
  at a word's ends, all cut from the same sheet. The cell is 112 columns wide (the longest word, BEENDEN, is 107).
  Comparison strips (word above the language's three original entries, x5): `.build/exit/strips/strip_<lang>.png`
  (made by a scratch script from the built files; not in the repo).
- **The entry:** `honban.gncp`'s scene `Menu` gets a cast `exit` (a copy of `options`, on the scene's unused cell 14 - the sheet's CONTINUE row - pointed at that area,
  54 + 8 pixels wide, y 0.7792 = the 31-pixel step below Options) in a new group 13 before `blackcover`, and in every
  motion a track set copied from `options`'s with y mapped as y' = 2y - 0.65 (so `MenuOpen` grows it out of the same
  centre, the sub-menus fade it out with Options); a new motion `Menu_Exit` is `Menu_Options` with Options unselected
  and EXIT selected. New data is appended to the data area and its pointer words are added to `NOF0`. **MEASURED** (first build): a project that grew past its 64 KiB page shifted the guest tokens (file start addresses) of every later file and so changed two words of a stage run's memory; the project is therefore kept within its page (track keys that do not change are shared, the cell is reused), and the builder refuses to grow it past the page.
- **The code:** a native owner of 0x802BDF0C that declines (the original words run, nothing is called) unless the
  frame involves EXIT: sub-state 1 with the cursor on EXIT (3), sub-state 1 on Options with only Down pressed (read
  from the same pad word and masks as 0x80357068, no guest call), or its own sub-states 4 (the question) and 5
  (leaving). Then it does the original frame's work through the original functions (guest_call), with a fourth
  entry. Decide on EXIT asks for caution window 0x2A every frame with the question's text written over message
  6019's in the loaded table (saved and written back once the window has closed); Yes stops the game the way closing
  the window does; No, or B (added: the game's windows ignore B), returns to the menu on EXIT.

## 5. Results (MEASURED, release preset, 2026-11-27)

- **Runs from the title** (headless, scripted tape: START every 2 s until the menu, then the moves; the Westopolis
  tape's card / SRAM / cfg copied into each run folder, `options.cfg` `language = N` for the text language): the menu
  shows the fourth entry in all six languages (EN EXIT, FR QUITTER, GE BEENDEN, IT ESCI, SP SALIR, JP shuuryou), selected
  white and the others dark red like the original entries; Down from OPTIONS reaches it, Down on it stays, Up leaves it.
  Decide opens the game's blue pop-up with the question in the text language and the game's own Yes / No (6047 / 6048:
  Oui / Non, Ja / Nein, Si / No, Si / No, hai / iie) on No. No and B return to the menu on EXIT; Yes ends the run with
  exit code 0, stop reason "host request", detail "the player chose Exit in the main menu". The 2P "connect a second
  controller" window (0x4A) of the same runs and of the base build is the same blue box.
- **The boot's 50 / 60 Hz question** is a different, full-screen page (red text, Yes / No / Test); the EXIT question
  is not that page.
- **Tape gate** (`tools/native/gate.ps1`, Westopolis + Final Haunt with `--boot-stage`): every value equals the base
  build's (fd27fb4): Westopolis state e0a60c4f3296, audio ac085fd173059dd6, behaviour 4d900f221003, screenshots
  550fcb3c9e86, Final Haunt state f95128aae2d3, audio 09b284bc876c36ed. (Both builds differ from the gate's stored
  dev-line references the same way.) The first gate run of a freshly built exe gave screenshots 5594d30591cb twice (one frame,
  234 pixels one blue level apart); every repeat, of the base build and of this one, gives 550fcb3c9e86.
- **CPU** (Westopolis tape, exclusive, 2 x 3 runs each, process CPU): base 19.484 / 19.500 s median, EXIT build
  19.391 / 19.516 s; cycles 58.41 / 58.40 G vs 58.32 / 58.38 G. No rise.

## 6. Unresolved

- **UNRESOLVED** Whether any other code reads the phase object's sub-state `this+8` while the menu is in state 3
  (the owner uses the unused values 4 and 5); none was found among the callers of the block's functions.
- **UNRESOLVED** The meaning of the cast record words +0x34 (0xFFF), +0x38 (0x51000) and the transform flags; they are
  copied from `options` unchanged.
