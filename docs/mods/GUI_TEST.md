# Automated test of the mod manager's window

`tools/mods/gui_test.ps1` starts the real `ShadowModManager.exe` window and drives it from outside, the way a user does,
then checks the results on disk. Until now the window was checked from rendered pictures and every action through the
command line; this test is the first that presses the window's own controls.

Windows PowerShell 5.1, no third-party modules, no change to the manager.

```
powershell -NoProfile -ExecutionPolicy Bypass -File tools\mods\gui_test.ps1 `
    -Exe <ShadowModManager.exe> -GameData <folder with files\> -Downloads <folder> -Work <scratch folder> `
    [-ModelFixes <a mod folder with options>] [-Only <step id prefix>]
```

* `-Downloads` must hold `cream.rar`, `playable_tails.zip`, `xbox_buttons_files.zip` and `regular_rouge.zip`.
* Every run uses a fresh `<Work>\run-<time>\mods` folder and passes it to the manager with `--mods-dir`; the game data is
  only read. Nothing else is created or deleted. The run folders stay for inspection (delete them by hand).
* Exit code = number of failed steps. A step that cannot be performed prints SKIP with the reason, never PASS.
* The test uses the real mouse and keyboard for about three minutes: leave the machine alone while it runs.
  Do not start it while the game is being played (it does not touch any game process, only the manager it started).

## Method

**Finding the controls: native UI Automation.** The managed `System.Windows.Automation` client of this machine returns
every Win32 control as a pattern-less "Pane" (no control types, no Invoke/Toggle), so the script uses the native
`IUIAutomation` COM client instead. Its interop assembly is generated from `UIAutomationCore.dll`'s type library with the
framework's `TypeLibConverter` (once per work folder, in `<Work>\_interop`); the helper classes are compiled in memory by
`Add-Type`. Controls are found by their Win32 control ids (visible as UIA automation ids: list 100, buttons 101..108,
details 109, status 111) and by their accessible names.

**Install by dragging.** A small borderless source window offers the files as `CF_HDROP`; the left mouse button goes down on
it, the cursor travels over the manager's window and the button is released there: a real OLE drag and drop, delivered by
the system as `WM_DROPFILES` to the window that called `DragAcceptFiles`, the same path as dropping from the Explorer.
Files and folders are dropped; several files in one drop work. The drop effect OLE reports is checked (`None` means nobody
took the drop). The source window is placed away from the manager and the attempt is repeated (up to 3 times) when the
button press did not reach it.

**Real input** is used where a user would use it: a real mouse click on a list check box (step 06), on `Install...` (10b),
and the keyboard (Down arrow on a focused combo box, step 10). Buttons and the "Load mods" check box are driven through UIA
patterns (Invoke, Toggle), list rows through the Toggle and SelectionItem patterns.

**Message boxes** (install result, Remove confirmation, damaged-file question, Play error) are found as the manager's
top-level `#32770` windows, their text is read from the static control and they are closed by pressing a button: Yes/No by
control id 6/7 (the labels are localised, the ids are not), a lone OK button as the only button (it has id 2 here).

**Checking.** After every action the result is read back from disk (`mods.json`, folders), from the window (list rows and
their check state, the status column, the details text, the status line, the combo box) and, for the load order and
conflicts, from a second reading of the same state by the manager's own command line (`list`, `plan`). `mods.json` is read
with a share mode that never blocks the manager's atomic rename.

## What is truly exercised

| Area | Exercised | How |
|---|---|---|
| Window and controls | all 7 buttons, "Load mods", list, columns, details, status line exist, are enabled, have Invoke/Toggle patterns | UIA (step 01) |
| Install by drop | one archive on the list; two archives in one drop on the title bar; a folder on the list | real OLE drag and drop (02, 03, 10) |
| Install by dialog | real click on `Install...`, Cancel, then typed path + Open in the common file dialog | mouse + UIA (10b) |
| Result box | text names mod, source, counts and "off until you tick it" | UIA text (02, 03, 10, 10b) |
| List rows | name, version, content, status, tick = `mods.json` = CLI | UIA vs disk vs CLI (04) |
| Tick / untick | by Toggle pattern and by a real mouse click on the check box; saved; row, status column, details follow | 05, 06 |
| Move up / down | row 3 up twice, up at the top (no change), down, restore; selection follows; list = CLI order | 07 |
| Conflicts | Cream + Playable Tails: details name `character/shadow.one` for winner and loser, status column "(1 file over others / 1 overridden)", status line, CLI plan; swap by Move up | 08 |
| Load mods | untick: `mods_enabled=false`, rows say "On (all mods switched off)", status line; tick again | 09 |
| Options | combo box of shadow-model-fixes changed with the keyboard: value, `mods.json` `options` | 10 |
| Refresh | external change by the CLI is not shown until Refresh, then is | 11 |
| Remove | confirmation text, No keeps everything, Yes removes the folder (to the Recycle Bin), the state, the row, the details | 12 |
| Errors | text file and a fake `.zip` dropped on the details pane / status line: failure boxes, nothing installed, no leftovers; damaged `mods.json` at start: No keeps the file byte-identical (also on Refresh), Yes writes a valid new list, all off | 13, 15, 16 |
| Play without a game exe | error box, nothing started | 14 (skipped if a `ShadowTheHedgehog.exe` sits beside the manager) |
| Save while the file is locked | window behaviour when the rename fails | 17 |
| Close | the window's close ends the process without a kill | 18 |

Drop targets reached by the real drop: the list, the title bar, the details pane, the status line (all took the drop).

## What is not exercised

* **Open folder** (opens an Explorer window) and **Play with a game present** (starts the game): not pressed.
* The **preview picture** (owner-drawn): its presence is not read; the layout/appearance remain a matter for the picture checks.
* **Keyboard shortcuts** (Delete = remove, Alt+Up/Down) and tab order: not driven.
* **A running install can not be interrupted** (close while busy: "An installation is still running"): not tried.
* More than one option combo box (a mod with 2..6 options), the "needs mods that are not installed" and "also enable
  dependencies?" boxes, dependency/invalid-mod rows ("Invalid: ...", the "not a usable mod" box): no downloaded mod used
  here has those; they are covered by the command-line tests only.
* Dragging from a real Explorer window (the source here is a small test window; the shell data object is the same
  `CF_HDROP`, but Explorer's own drag image/feedback code is not involved).
* High-DPI / multi-monitor layouts: the test runs at the primary display's scale and computes click points from the
  UIA rectangles (the check-box click assumes the list's check box sits 12 px, scaled, from the row's left edge).
* The Recycle Bin content is not inspected: only that the folder left the mods dir. Each run sends one small
  folder (`playable-tails`) to the Recycle Bin; the test does not empty it.

## Results of the runs

(Windows 11, 1920x1080 at 100 %, manager `mgr-52e3068e` built from `mods/loader` c9d7e952.)

Runs 13 and 14 of the development series, on the final script (they were started one after the other; the only edit made
after them is the wording of one finding string that prints only when step 09 fails):

| Step | Run 1 | Run 2 |
|---|---|---|
| 01-start | PASS | PASS |
| 02-drop-one | PASS | PASS |
| 03-drop-two | PASS | PASS |
| 04-rows | PASS | PASS |
| 05-tick-uia | PASS | PASS |
| 06-tick-click | PASS | PASS |
| 07-move | PASS | PASS |
| 08-conflict | PASS | PASS |
| 09-master | PASS | PASS |
| 10-options | PASS | PASS |
| 10b-install-dialog | PASS | PASS |
| 11-refresh | PASS | PASS |
| 12-remove | PASS | PASS |
| 13-nonarchive | PASS | PASS |
| 14-play-missing | PASS | PASS |
| 15-damaged-no | PASS | PASS |
| 16-damaged-yes | PASS | PASS |
| 17-save-locked | PASS | PASS |
| 18-close | PASS | PASS |
| **Result** | 19 passed, 0 failed, 0 skipped | 19 passed, 0 failed, 0 skipped |

Stability, honestly. The script needed several rounds before it was green; most early failures were mine (a wrong
message-box button id, arrays unrolled by PowerShell, a drag source window that sometimes was not under the cursor:
one run where all 3 attempts failed, after which the source was moved to the bottom of the screen and the retry names
the window under the cursor). From the first run with the final reader onwards there were 11 full runs: 9 green and 2
red. The red ones: the drag-start problem just described (before the fix) and one run in which step 09's second tick
("Load mods" on again) showed in the window but never reached `mods.json` although the test held no lock on it (the
same symptom was seen in an early run while the test's own reader held the file); this is finding 1 below, and the
script now prints evidence when it happens. So the suite is stable except for that real manager behaviour, which shows
up about once in ten runs here. The checks read real state: the red runs are the proof they can fail, and step 11
shows the window really differs from the disk until Refresh.

## Findings about the manager (not fixed here: wt-mods is untouched)

1. **A failed save is silent.** `write_atomically` writes `mods.json.tmp` and renames it over `mods.json`; the rename fails
   when another program has `mods.json` open without delete sharing (virus scanner, indexer, backup, a `Get-Content`).
   `Model::save()` returns the failure but `changed()`, `refresh()` and the CLI-side callers ignore it, so the window shows
   the new state ("Load mods" unticked) while the file keeps the old one and no message appears. Reproduced by step 17
   (file held open with `FileShare.ReadWrite`: window changed, file byte-identical, 0 message boxes, no stray `.tmp`). It
   also happened by itself in 1 of 11 runs (step 09) and in an early run while the test's own reader held the file. Whether the choice is lost on close
   is from the code (the state lives only in memory), not run.
2. **After answering No to "Start a new list?" the window still lets you edit** (step 15): rows are built in memory from the
   mods folder, "Load mods" and ticks change, but `save()` returns false while `state_error` is set; no message says so
   (file untouched, which is the intended safety, but the user is misled).
3. **Accessible names.** The mod list (`SysListView32`) and the details pane (read-only `Edit`) have an empty UIA name; the
   screen reader announces unnamed controls. Buttons, "Load mods", the status line, option combo boxes (named by their label)
   and list rows/cells are named.
4. **Observation, not a bug:** the managed .NET UIA client on this machine cannot see the controls' patterns at all (every
   control a "Pane"), so tools written against `System.Windows.Automation` find nothing to press; the native client works.
   The lone OK button of a message box has control id 2, not 1.
5. **Observation:** the Install result box says "1 files" / "1 textures" (no singular), while the list says "1 file".

### Proposed fixes (diff sketches, not applied, not compiled)

Finding 1 (retry the rename, say so when it still fails):

```diff
--- a/runtime/mods/mods.cpp
+++ b/runtime/mods/mods.cpp
@@ bool write_atomically(...)
-    fs::rename(temp, file, ec);
-    if (ec) {
+    for (int attempt = 0; attempt < 8; ++attempt) {   // a scanner may hold the file for a moment
+        fs::rename(temp, file, ec);
+        if (!ec) break;
+        std::this_thread::sleep_for(std::chrono::milliseconds(75));
+    }
+    if (ec) {
--- a/tools/mod_manager/mod_manager.cpp
+++ b/tools/mod_manager/mod_manager.cpp
@@ struct Model
+    std::string save_error;  // why the last save did not reach the disk (empty = it did)
-    bool save() {
-        if (!state_error.empty()) return false;
-        std::string error;
-        return mods::save_state(state_file(), state, error);
-    }
+    bool save() {
+        save_error.clear();
+        if (!state_error.empty()) { save_error = "mods.json is damaged and was left as it is"; return false; }
+        return mods::save_state(state_file(), state, save_error);
+    }
@@ void changed()
-    g->model.save();
+    const bool saved = g->model.save();
     g->model.replan();
     fill_list();
+    if (!saved)
+        MessageBoxW(g->window, wide("This change could not be saved and will be lost when the window closes:\n\n" +
+                                    g->model.save_error).c_str(), L"Shadow Mod Manager", MB_OK | MB_ICONWARNING);
```

Finding 2 (read-only window while the file is damaged and declined):

```diff
@@ void refresh_all()
-        if (MessageBoxW(...) == IDYES)
-            g->model.reset_state();
+        if (MessageBoxW(...) == IDYES) g->model.reset_state();
+        else g->read_only = true;               // new flag: Gui::read_only
+    ...
+    // after fill_list(): while read_only, disable the list check boxes' effect, Move up/down, Install, Remove and
+    // "Load mods" (EnableWindow false) and show in the status line:
+    //   "mods.json is damaged - read only. Press Refresh to be asked again."
```
(`on_check_changed` and the `WM_COMMAND` cases would also return early when `g->read_only`; Refresh stays enabled and clears
the flag when the user answers Yes.)

Finding 3 (accessible names; untested): give the list a window text (`CreateWindowExW(..., L"Installed mods", ...)`: the
Win32 proxy names list views from it) and put a small static label "Details" before the details edit in the tab order (UIA
names an edit from the static that precedes it).

## Re-test against the fixed manager (mods/loader f5960e4e, exe `mgr-70cd3039`)

Steps 01, 02, 15 and 17 were changed to expect the new behaviour; four full runs in a row: 19 passed, 0 failed, 0 skipped
each (the first attempt had 2 failing steps, both my wrong expectations: the list name below, and what step 17 should
see after a refused save). The step-09 lost save did not appear in these 4 runs (nor in step 17's lock, which now
reports it); the retry could only be confirmed against the deliberate lock, not against the rare transient one.

| Finding | Status | Verified by |
|---|---|---|
| 1 failed save is silent | **fixed** | step 17: with `mods.json` held open an error box "This change was NOT saved: cannot replace ... (is another program using it?)" appears, `mods.json` is byte-identical, no `.tmp` left; the window keeps the choice in memory ("Load mods" unticked) and the next successful save writes it together with a later tick. (The retry does not rescue a lock that lasts longer than ~0.6 s, as intended.) |
| 2 edits after No on the damaged file | **fixed** | step 15: status line "mods.json is damaged - read only...", a tick, Move down and "Load mods" each give the warning box ("The mod list file is damaged, so changes cannot be saved") and the control is put back; `mods.json` byte-identical afterwards (real mouse clicks, because a UIA call would block behind the box) |
| 3 accessible names | **half**: details pane fixed, list not | step 01: the details edit is named "Details"; the list view is still unnamed (UIA name empty): the Win32 list-view proxy does not use the window text "Installed mods". Reported as a FINDING (no failure). Idea: `LVM`-level name is not supported; a preceding static label like the one used for the details, or a `WM_GETOBJECT` accessible, would be needed (not tried) |
| 4 plurals | **fixed** | step 02: the install box says "1 file, 0 AFS entries, 0 textures" |

Note the observation in step 17's text: the box shows the full path of `mods.json` in forward slashes.

## Update (coordinator, manager f51f9f53)

Finding 3 is now fully fixed: a visible label precedes the list ("Installed mods (higher in the list wins a file both
change)"), and UI Automation reports it as the list's name; step 01 checks it. Full run: 19 passed, 0 failed, 0
skipped. The save retry (finding 1) is proven separately by `mod_loader_tests` (a 200 ms lock is waited out).

## Round 4: ergonomic features (steps 19 to 30, exe `mgr-79588cdd`, mods/loader 5f214e59)

Steps 19 to 30 run on a second, fresh mods dir (`mods2`: Cream, Playable Tails, Regular Rouge) after the original 19 steps.
New input helpers: real right-click, double-click, a real mouse drag inside the window, key chords (Ctrl, Alt with
extended keys), typed text, and `GetPixel` on the screen. Menus (`#32768`) and the edit form (`ShadowModManagerForm`,
its own message loop) are found as top-level windows of the manager process and driven by UIA plus real clicks/keys.
`-Only` is now a regex of step ids (`'19|24'`), `-BigFolder` gives a big folder for the progress step.

| Step | What is driven | Result on the fixed exe (runs 1 and 2) |
|---|---|---|
| 19-setup | CLI installs into a fresh dir, window started | PASS / PASS |
| 20-context-menu | real right-click selects the row; 11 items present; Move to top (Invoke), Move to bottom (arrow keys + Enter), Move down (real click on the item), Switch on, Switch all on/off, verified in mods.json | PASS / PASS |
| 21-drag-row | real mouse drag: row 1 onto the lower half of row 3, row 3 onto the upper half of row 1, into the empty area below the list, row 3 onto row 2, drop on itself | PASS / PASS (see bug 1) |
| 22-filter | Ctrl+F then typing "tail": 1 row, status "Filter: 1 of 3 mods shown", Ctrl+F + Del restores, a no-match filter gives 0 rows | PASS / PASS |
| 23-keyboard | Space and double-click toggle; Alt+End / Alt+Home; F5 after an external CLI change; Ctrl+O opens the file dialog (Cancel); Ctrl+E opens the form (Esc); Del asks, No keeps it | PASS / PASS |
| 24-edit-info | name and author written to `mod.json` and shown in the row; "x.y" warns and keeps the form with the typed text; Cancel leaves `mod.json` byte-identical | PASS / PASS |
| 25-profiles | save "gui-a" (file in `.profiles`), change, Load + Yes restores mods.json, Load + No keeps, Delete submenu + Yes removes the file | PASS / PASS |
| 26-update-prompt | drop cream.rar again: "Update mods" box; No leaves everything (no `cream-2`); Yes replaces in place (folder `cream`, flag and position kept; the edited name reverts to the download's) | PASS / PASS |
| 27-dependencies | hand-made folder mod `needs-cream` dropped; unticking Cream asks "Switch them off too?" (No keeps, Yes switches both off); enabling Needs Cream with Cream off asks "Enable them too?" (Yes) | PASS / PASS |
| 28-status-colours | darkest text pixel of the status cell: "Loads" RGB(30,120,50) (green), "Off" RGB(109,109,109) (grey); needs the window in front, otherwise the step is SKIP | PASS / PASS |
| 29-progress | `msctls_progress32` hidden when idle, visible while hq_gun_soldiers.zip installs (Install..., Remove, Refresh disabled), hidden after the result box | PASS / PASS |
| 30-placement | real drag of the title bar and the corner; close ends the process; `.manager.json` holds the rect; restart opens at the same rect; left=-30000 is ignored (default place and size) | PASS / PASS |

Whole script on the fixed exe, two runs: 31 passed, 0 failed, 0 skipped, each.

Bugs found in this round:
1. **Drag-to-reorder did nothing** (exe `mgr-856a1135`): all drops were no-ops and the selection was cleared. Cause (from the
   code, confirmed by the fix): `WM_LBUTTONUP` called `ReleaseCapture()` before `drag_move(true)`, and `ReleaseCapture`
   sends `WM_CAPTURECHANGED` synchronously, whose handler cleared `g->drag_key`. Fixed in mods/loader 5f214e59 (the key is
   held aside across `ReleaseCapture`); step 21 failed 5 checks before and passes after.
2. The step-06 test bug found on the way: two real clicks within the double-click time are a double-click, which the new
   double-click-toggles feature handles as a second toggle (the test now waits between clicks; not a manager bug).

Not driven in round 4: the menu items "Edit..." and "Remove..." from the context menu (same actions as Ctrl+E and Del,
which are driven), "Open folder", "Install..." from the menu, the maximised state of the saved window placement, a "Not loaded"
status colour (only green and grey sampled), texture-pack conflicts.
