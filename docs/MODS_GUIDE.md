# Mods guide

This guide explains how to add mods to the game with the *Mod Manager*.

Mod support is **experimental**. Many mods work, some work only in part, and some kinds do not work at all. Read
[What works and what does not](#what-works-and-what-does-not) first, and see
[Experimental: known limitations](#experimental-known-limitations) at the end.

## Contents

- [What works and what does not](#what-works-and-what-does-not)
- [Opening the Mod Manager](#opening-the-mod-manager)
- [The window](#the-window)
- [Installing a mod](#installing-a-mod)
- [Switching mods on and off](#switching-mods-on-and-off)
- [Order, priority and conflicts](#order-priority-and-conflicts)
- [Options of a mod](#options-of-a-mod)
- [Profiles](#profiles)
- [Editing a mod's info](#editing-a-mods-info)
- [Removing a mod](#removing-a-mod)
- [Playing without mods](#playing-without-mods)
- [Texture packs](#texture-packs)
- [What the status colours mean](#what-the-status-colours-mean)
- [Region warnings](#region-warnings)
- [Troubleshooting](#troubleshooting)
- [Command line](#command-line)
- [Experimental: known limitations](#experimental-known-limitations)

## What works and what does not

**Works**

- **Mods that replace the game's files.** Models, stages, textures, text, sounds and movies. This is what most
  downloads are.
- **Voice line replacements** that come as a folder named after the game's voice archive (for example a
  `PRS_VOICE_E` folder with sound files in it).
- **Dolphin texture packs.** Picture files whose names start with `tex1_`. See [Texture packs](#texture-packs).

**Does not work**

- **Code patches.** Gecko codes, Action Replay codes, and changed copies of the game's program file (`main.dol`). The
  manager recognises them, keeps them with the mod in a `codes` folder and tells you about them. Installing a mod or
  switching it on does **not** apply them. So a mod that is mostly code (new moves, new rules, new game modes) will
  be missing those parts. See [the note on the Code... button](#the-code-button).
- **Patches for a whole disc image** (files such as `.xdelta`). They are never applied.
- **Programs.** A download that contains a program (`.exe`, `.dll`, scripts) is never run and the program is never
  installed. If a download is only a program, the manager refuses it and says so.
- **Save files.** GameCube saves (`.gci`, `.gcs`, `.sav`) and memory card files are never installed. The manager never
  writes into your saves.

## Opening the Mod Manager

Use either way:

- Open the Start menu and choose the game's *Mod Manager* shortcut.
- Open the folder where you installed the game and run `ModManager.exe`.

`ModManager.exe` must stay in the same folder as `ShadowTheHedgehog.exe`. It needs the game's files to know where each
file of a mod belongs.

Your mods are kept in the `mods` folder inside the install folder. Each mod has its own folder there.

## The window

**Top row, from left to right**

| Button | What it does | Keyboard |
|---|---|---|
| *Install...* | Choose downloaded mod files to install. | Ctrl+O |
| *Remove* | Remove the selected mod. | Del |
| *Move up* | Move the selected mod one place up. | Alt+Up |
| *Move down* | Move the selected mod one place down. | Alt+Down |
| *Open folder* | Open the selected mod's folder. With no mod selected, it opens the `mods` folder. | |
| *Refresh* | Read the `mods` folder again. Use it after you changed something there by hand. | F5 |
| *Filter (Ctrl+F)* box | Type a few letters to show only the mods that match (name, author, content or status). | Ctrl+F |
| *Code...* | Opens the *Code mods* window. See [the note below](#the-code-button). | |
| *Profiles* | Save and load sets of mods. See [Profiles](#profiles). | |
| *Load mods* | The master switch. See [Playing without mods](#playing-without-mods). | |
| *Play* | Starts the game. | Ctrl+P |

In a small window, the first six buttons show only their icons. Hold the mouse over one to see its name.

**The list** is titled "Installed mods (higher in the list wins a file both change)". Each row is one mod, with a tick
box and these columns:

- *Mod (highest priority first)*: the mod's name.
- *Version* and *Author*.
- *Content*: what the mod holds, for example "12 files, 3 textures".
- *Status*: whether the mod will load. See [What the status colours mean](#what-the-status-colours-mean).

**The card beside the list** shows the selected mod: a preview picture (if the mod has one), its options (if it has
any), and *Details*. The details tell you what the mod changes, where in the game it is used, its conflicts with other
mods, and any notes or problems.

**The bottom bar** shows a summary line, for example "5 installed, 3 enabled, 3 load", and the *Zoom* and *Theme*
lists.

**Right-click a mod** for a menu with: *Switch on* / *Switch off*, *Move to top (highest priority)*, *Move up*,
*Move down*, *Move to bottom*, *Edit name, version, author...*, *Open folder*, *Remove...*, *Switch all on*,
*Switch all off*, *Install...*.

**Zoom and theme** work the same as in the Settings program, and the two programs share the choice: Ctrl and + or - to
zoom, Ctrl and 0 for 100 %, or Ctrl and the mouse wheel. See the [Settings guide](SETTINGS_GUIDE.md#zoom-and-theme).

Everything you change in the Mod Manager is saved at once. There is no Save button.

## Installing a mod

1. Download the mod. Leave it as it is: you do not need to unpack it. `.zip`, `.7z` and `.rar` files work, and so do
   folders.
2. Open the Mod Manager.
3. Do one of these:
   - **Drop it.** Drag the downloaded file or folder from Windows Explorer onto the Mod Manager window. You can drop
     several at once.
   - **Use the button.** Click *Install...* and pick the file. You can pick several files. (*Install...* picks files
     only. To install a folder, drop it on the window.)
4. Wait while the bar at the bottom runs. Large mods can take a while.
5. Read the message. It tells you what was installed, for example:
   `Installed "Some Mod" from somemod.zip: 12 files, 0 AFS entries, 3 textures. It is off until you tick it.`
   It also lists anything the manager left out and why.
6. **Tick the mod** in the list. A new mod is always off, and it goes to the bottom of the list.
7. Click *Play*, or start the game the usual way.

The game reads the mods when it starts. If the game is already running, close it and start it again to see a change.

**What the manager does with a download.** It looks at every file and puts it where the game expects it. You do not
have to sort anything yourself. Files it cannot use are left out and named in the message. If a download holds two
versions of the same file (for example a widescreen set and a 4:3 set), the manager turns the extra set into an
[option](#options-of-a-mod) that is off at first.

**Updating a mod.** Install the new download the same way. The manager notices that the mod is installed already and
asks: "Replace the installed copy with the download? Its place in the list, its on/off state and its options are
kept." Choose *Yes* to update. If you edited the mod's name or description, your edits are kept too.

**A damaged download is refused.** The manager checks the game files in a download. If one is cut off or broken, it
says "the download is damaged" and installs nothing. Download the mod again.

## Switching mods on and off

A ticked mod is on. An unticked mod is off and the game ignores it.

To switch one mod:

- click its tick box, or
- double-click its row, or
- select it and press Space, or
- right-click it and choose *Switch on* or *Switch off*.

To switch all mods: right-click in the list and choose *Switch all on* or *Switch all off*.

**Mods that need other mods.** Some mods only work together with another mod.

- When you switch on a mod that needs another installed mod, the manager asks "Enable them too?".
- When you switch off a mod that others need, the manager asks "Switch them off too?".
- If a needed mod is not installed at all, the manager tells you. The mod stays ticked but does not load until you
  install what it needs.

## Order, priority and conflicts

Two mods can change the same file of the game. Only one of them can win. The rule is simple:

**The mod that is higher in the list wins.**

To change the order:

- select a mod and click *Move up* or *Move down*, or
- press Alt+Up or Alt+Down (Alt+Home for the top, Alt+End for the bottom), or
- drag the row to a new place with the mouse, or
- right-click it and use the *Move* entries.

The manager shows you every conflict, so you do not have to guess:

- The *Status* column says, for example, "Loads (3 files over others, 1 overridden)". The mod wins three files against
  other mods and loses one.
- The *Details* list them under "File conflicts this mod wins" and "File conflicts this mod loses", with the name of
  the other mod.
- The summary line at the bottom counts the conflicts.

A conflict is not an error. It only tells you that you see one mod's version of that file and not the other's. Put the
mod you care about more higher in the list.

One exception: a mod that another mod needs is always loaded underneath the mod that needs it, wherever it stands in
the list.

## Options of a mod

Some mods come with choices, for example a widescreen version and a 4:3 version. Select the mod. Its options appear as
drop-down lists on the card, above *Details*. Pick what you want. The choice is saved at once and is used the next
time the game starts.

Options that the manager made itself from a download (see [Installing a mod](#installing-a-mod)) are off at first.

## Profiles

A profile is a saved set: which mods are on, their order, and their options. Use profiles to switch quickly between,
say, a "plain" set and a "everything on" set.

Click *Profiles*:

- **Save current set as...** asks for a name and saves the current set. A name is 1 to 40 characters: letters,
  digits, spaces, and `-` `_` `(` `)` `.`. If the name exists, the manager asks before it replaces the profile.
- **Load "name"** switches to that set. The manager asks first, because your current choices are replaced. Save them
  as a profile first if you want to keep them.
- **Delete** shows the profiles; pick one to delete it. This does not remove any mod.

Profiles do not hold the mods themselves. If you remove a mod, a profile that used it simply loads without it.

## Editing a mod's info

Many downloads have no proper name or author. You can fix that.

1. Select the mod.
2. Press Ctrl+E, or right-click and choose *Edit name, version, author...*.
3. Change *Name*, *Version*, *Author*, *Website* or *Description*.
4. Click *OK*.

The name must not be empty, and the version must look like `1.0` or `2.3.1`. This changes only what the manager
shows. It does not change what the mod does.

## Removing a mod

1. Select the mod.
2. Click *Remove*, press Del, or right-click and choose *Remove...*.
3. Answer *Yes*.

The mod's folder is moved to the Windows **Recycle Bin**. If you removed it by mistake, restore it from the Recycle
Bin and click *Refresh*.

If you only want to stop using a mod for now, untick it. That is quicker and keeps the mod.

## Playing without mods

There are two ways. Neither removes anything.

- **The *Load mods* switch** in the Mod Manager. Untick it and the game loads no mod at all, whatever is ticked in the
  list. The mods keep their ticks, and their status reads "On (all mods switched off)". Tick *Load mods* again to get
  them back.
- **`--no-mods`**. Start the game with this word after its name to load no mod for that one run:

  ```
  ShadowTheHedgehog.exe --no-mods
  ```

  This does not change anything in the Mod Manager. The next normal start loads your mods again.

Both are useful to find out whether a problem comes from a mod or from the game.

## Texture packs

A texture pack replaces pictures of the game (the "skin" of characters, objects, menus) with new ones. The game uses
the same kind of pack as the Dolphin emulator.

**How to recognise one.** Its files are pictures whose names start with `tex1_`, for example
`tex1_128x128_0a1b2c3d4e5f6a7b_5.png`. The files end in `.png` or `.dds`. Do not rename them: the name is how the game
finds the picture it replaces.

**How to install one.** Exactly like any other mod: drop the download on the Mod Manager, then tick it. In the
*Content* column it shows as a number of textures.

**Priority.** Texture packs follow the same list. If two packs replace the same picture, the higher one wins. The
*Details* tell you: "Textures this mod wins over ..." or "Textures ... wins over this mod".

**What is supported**

- `.png` pictures.
- `.dds` pictures that are uncompressed, or compressed as BC1, BC2 or BC3 (also called DXT1, DXT3, DXT5).
- Pictures up to 8192 x 8192. A pack can be sharper than the original.

**What is not supported**

- `.dds` pictures compressed as BC7, BC4, BC5 or BC6H. Save them again as PNG or BC1 to BC3.
- Extra files for smaller copies of a picture (names ending in `_mip1` and so on). They are ignored.
- Files whose names start with `efb1_`.
- Dolphin "GraphicMods" folders. They are not read.
- Movies. Their pictures cannot be replaced by a texture pack.

**Some pictures of a pack may not show up.** A picture is only replaced when the pack was made from exactly the same
original picture that your game has. Packs made for the North American release usually match, but a few pictures
differ. Pictures that belong to another mod's changed files only show up when that mod is installed and on as well.

## What the status colours mean

The *Status* column is coloured so you can see at a glance what will happen when the game starts.

| Colour | Status text starts with | Meaning |
|---|---|---|
| Green | Loads | The mod is on and will load. |
| Orange | Loads ... NTSC-U stage layouts, see details | The mod will load, but it replaces stage layouts with those of the North American release. See [Region warnings](#region-warnings). |
| Red | Not loaded | The mod is ticked but cannot load. The text gives the reason, and the *Details* say more. |
| Red | Invalid | The folder is not a usable mod. Select it to read why. |
| Grey | Off | The mod is not ticked. |
| Grey | On (all mods switched off) | The mod is ticked, but *Load mods* is off. |

## Region warnings

This game is built from the **European (PAL)** release. Almost all mods on the internet were made for the
**North American** release, which the manager calls NTSC-U. Most files are the same in both releases, so most mods
work. A few files differ, and the manager points those out.

Where to look: select the mod and read the *Details*.

- "Made for disc: ..." names the release the mod was made for, when the mod says so.
- A section called "Region (this game reads the PAL disc)" lists each file that matters.

What the notes mean:

- **Stage layouts.** The European release changed the layout of a few stages (where enemies and objects are). A mod
  made for the North American release can bring the older layout back for those stages. The mod's status turns orange
  and says "NTSC-U stage layouts, see details". The mod still loads. Only you can decide whether you mind.
- **Text and fonts.** The European release has more languages, and many of its text files differ from the North
  American ones. The notes name the text files of the mod that follow the North American release. A mod that only
  changes English text shows the note "changes English text only: no effect while another game language is selected".

A region note is a warning, not an error. Nothing is blocked.

## Troubleshooting

**"The game's files were not found."**
`ModManager.exe` is not in the same folder as `ShadowTheHedgehog.exe`. Do not move or copy the manager somewhere else.
Start it from the install folder or from the Start menu.

**I ticked a mod but nothing changed in the game.**
Check these in order:

1. Is *Load mods* ticked?
2. Is the mod's status green ("Loads")?
3. Did you close the game and start it again? Mods are only read when the game starts.
4. Did you start the game without `--no-mods`?
5. Read the mod's *Details*. "Never opened by the game" lists files that are installed but that the game never asks
   for. If a mod was mostly code, see [What works and what does not](#what-works-and-what-does-not).
6. Is a mod higher in the list winning the same files? See [Order, priority and conflicts](#order-priority-and-conflicts).

**The status says "Not loaded".**
The text after it gives the reason. The usual ones:

- it needs another mod that is not installed: install that mod;
- it needs another mod that is switched off: tick that mod;
- it needs a newer version of another mod: update that mod;
- it does not go together with another mod that is on: untick one of the two.

**The status says "Invalid".**
The folder in `mods` is not something the manager can use. Select it and read the *Details*. Remove it and install the
original download again through the manager.

**"this download is a program, not a mod" or "this download is a save, not a mod".**
The manager never installs programs or saves. See [What works and what does not](#what-works-and-what-does-not).

**A `.7z` or `.rar` file cannot be opened.**
The manager uses a tool that is part of Windows 10 (version 1803 or later) and Windows 11 for these. If it is missing,
unpack the download yourself and drop the unpacked folder on the window.

**"This change was NOT saved".**
The manager could not write its list (`mods.json` in the `mods` folder). Another program is probably using the file.
Close that program and make the change again.

**"The mod list file could not be read ... Start a new list?"**
The file `mods.json` is damaged.

- *Yes* starts a new list. Your mods stay installed, but every mod is switched off and you have to tick them again.
- *No* leaves the file alone. The manager is then read-only: you can look, but changes are refused until you fix or
  delete `mods.json` and click *Refresh*.

While the list is damaged, the game starts without mods.

**A mod worked before and now one of its files is ignored.**
If a file of a mod gets damaged after installing, the game skips that file and uses its own file. Install the mod
again.

**The game closes with "Shadow the Hedgehog stopped because of an error in the game".**
If the message says that mods are switched on, a mod is the likely cause: one that does not fit this game, or more
mods together than the game's memory holds. Switch off some mods, or untick *Load mods*, and start again. If it also
happens without mods, please report it with the file `session-report.json` from the game's folder.
**The game misbehaves and I do not know which mod causes it.**
Untick *Load mods* and try again. If the problem is gone, tick *Load mods* again, switch off half of your mods, and
repeat until you find the one.

**The `mods` folder has folders named `.cache`, `.profiles` and `.variants`.**
They belong to the manager. `.profiles` holds your profiles. `.cache` holds rebuilt sound archives and is safe to
delete; it is made again when needed.

## Command line

You can ignore this section if you use the window.

`ModManager.exe` opens the window when you start it without anything after its name. With a command after its name it
does one thing and ends, which is useful for scripts. `ModManager.exe help` prints the list. The commands, in the words
of that help text:

| Command | What it does |
|---|---|
| `list` | installed mods in priority order (highest first) |
| `info <id>` | everything about one mod |
| `install <archive or folder>...` | install downloads (`.zip`, `.7z`, `.rar`, folders); `--replace` updates, `--name N` gives a name |
| `enable <id>`, `disable <id>` | switch a mod on or off (enable also enables disabled dependencies) |
| `remove <id>` | delete an installed mod's folder |
| `move <id> <position>` | set the priority (1 = highest) |
| `option <id> <option> <choice>` | choose an option of a mod |
| `master on` or `master off` | load mods at all |
| `enable-all`, `disable-all` | switch every installed mod on or off |
| `profile list`, `profile save <name>`, `profile load <name>`, `profile delete <name>` | named sets of enabled mods, order and options |
| `set-info <id> <field> <value>` | edit a mod's name, version, author, website or description |
| `check` | work out what will load; exit code 1 when an enabled mod cannot load |
| `validate <folder>` | check every ONE/TXD/DFF/AFS/ADX file below a folder for damage |
| `plan` | the full plan as JSON, for scripts and tests |

The `<id>` of a mod is the short name that `list` prints after the tick mark. The *Details* in the window show it as
"Id".

Two options work with every command: `--mods-dir <folder>` uses another mods folder (default: `mods` beside the
program), and `--game-data <folder>` names the game's data folder (default: the same place the game looks).

Be careful with `remove`: on the command line it deletes the mod's folder directly. It does not go to the Recycle Bin.

The help text also lists `codes`, `code`, `code-signature` and `code-plan`. They belong to the developer feature
described under [the Code... button](#the-code-button).

The game itself understands two words about mods: `--no-mods` (load no mod this run) and `--mods-dir <folder>` (use
another mods folder).

## Experimental: known limitations

Mod support is new. These limits are known.

**Code is not applied by installing a mod.**
Gecko and Action Replay codes, code libraries, and changed `main.dol` files are kept with the mod but do nothing when
you install the mod or switch it on. The game on PC is not the original program running in an emulator. It was
rebuilt for PC, so patches written for the original program do not fit by themselves.

<a id="the-code-button"></a>
**The *Code...* button is a developer feature.**
The *Code mods* window lists the code that your enabled mods offer and lets you tick items. Ticking alone changes
nothing in the game. A ticked item only takes effect after *Build...*, and that step needs things a normal install
does not have: the game's source code, Python, Visual Studio, and the program file of the North American disc. The
first build takes a long time. Even then, parts of a code that touch pieces of the game that were rewritten for PC are
reported and skipped. If you tick items and do not build, the *Play* button asks what to do, and "No" plays without
the code. Unless you build the game from source yourself, leave this window alone.

**Whole disc image patches are never applied.**
Mods that come only as a patch for a disc image (for example `.xdelta` files) cannot be used. If the same mod is also
offered as loose game files, install those instead.

**Memory for mods.**
The game was made for a console with 24 MiB of memory, and mods' models and stages are larger than the disc's. When a
mod replaces game files, the game is therefore given about ten times that (256 MiB). In the tests of this release, 15
community mods switched on together first stopped the game about 50 seconds into a stage, because its memory ran out;
with the larger memory the same 15 play the whole stage. A game without mods, or with texture packs only, runs exactly
as before. A very large set of mods can still be too much; the game then closes and tells you: "Shadow the Hedgehog
stopped because of an error in the game", with the number of mods that are on. Switch off some mods and start again.
**Big "overhaul" mods load only in part.**
*Shadow the Hedgehog: Reloaded* and similar projects are a mix of changed files and changed code. Their files load
(stage changes, models, textures, text, music, voice lines). Their code features do not (for Reloaded: S rank, Red
Rings, skippable cutscenes, new moves, and others). Stages that expect the changed code behave like the original game.
The *Details* of such a mod list "Reloaded features this build does not have".

**The game is the European release.**
Mods made for the North American release can change stage layouts and text wording in a few places. See
[Region warnings](#region-warnings).

**Saves of modded games cannot be used.**
The game only reads its own save. A save made with a modded version of the game on a console or emulator belongs to
that version.

**Movies.**
Replacement movies play. The game draws every movie in a 4:3 box, so a "wider" movie mod is squeezed into that box and
does not show wider. Movies made for another version of the game with a different picture size are left out when you
install.

**Texture packs.**
See the list under [Texture packs](#texture-packs). In addition, replacement pictures are loaded when the game first
needs them, so very large packs can make a stage load a little longer.

**Text mods in one language.**
A mod that changes only the English text has no effect while the game is set to another language.

**The window shows at most six options per mod.**
If a mod has more than six options, the rest can only be set from the command line (`option`).

**A change needs a restart of the game.**
The game reads the mods once, when it starts. Nothing changes in a game that is already running.

**`.7z` and `.rar` need Windows 10 version 1803 or later.**
On older systems, unpack the download yourself and install the folder.

**Not everything has been tested.**
For this release, 15 downloads from the community were installed with the manager and each played through one stage
(a fixed route through Westopolis, about five minutes) on the installed game: a character model, stage layouts,
weapons, text, button pictures, voices, movies and three texture packs. Other stages, the game's menus with mods, and
long play were not covered. If a mod causes trouble, switch it off and see [Troubleshooting](#troubleshooting).
For the technical side (folder layout, the rules, how files are placed) see [mods/DESIGN.md](mods/DESIGN.md) and
[mods/TEXTURE_PACKS.md](mods/TEXTURE_PACKS.md).
