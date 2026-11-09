# Release validation: v0.0.1

Recorded 2026-11-09. The release's commit is the one tagged `v0.0.1`; each install records it in `install-info.json`.

## Reproducing the build

Install from a clean checkout with `Install.cmd`. Each install writes `install-info.json`, which records the commit, the
number of modified files and the settings read from the build's CMake cache.

| Setting | Value |
|---|---|
| Preset, configuration | `release`, Release |
| `SHADOW_SHIPPING` | ON |
| `SHADOW_RENDER_LIFTED` | ON |
| `SHADOW_ENABLE_PLUME` | OFF |
| `SHADOW_ENABLE_TRACY`, `SHADOW_BUILD_TESTS`, `SHADOW_BUILD_REFERENCE` | OFF |
| `SHADOW_RECOMP_SHARDS` | 48 |
| Generator | Visual Studio 18 2026 |
| Disc | GUPP8P, `main.dol` SHA-1 `6ff04994371e93d6ba1015644f4e7467713b9363` |

## Source compared with the development tree

Against the development tree this release was taken from, `rwcore` and `rpworld` are identical. The product source differs in these files:

- The Resolution row: `native_options_display.{h,cpp}`, `settings_display.cpp`, `display_settings.h`, `options_menu_texts.inc`.
- `native_vi_wait.cpp` (return addresses computed from the row's address, same values) and `unorm_calibration.cpp` (clears
  the target before its first use).
- `native_hungarian_palsel.cpp`, a stub: its generated picture is not published. `tools/hungarian/make_palsel.py` makes it.
- Comments only: six files in `runtime/lift`, six in `runtime/scaffold/rw_driver`.
- The default controller layout, in `runtime/lift/peripheral/gamepad_binding.h`: the GameCube pad's B on the left face button, its X on the right.

## Results

### Regression tests

| Build | Passed | Not passed |
|---|---|---|
| `tests` preset | 237 of 238, the other fixed below | none after the fix |
| `shipping-tests` preset | 224 of 235 | 11, listed below |

`shipping_platform_selection` failed in the `tests` run because of an empty argument in the test's own setup. It was then
fixed and passes in both trees; the other tests were not re-run for that one-line change.

These 11 cannot apply to the Shipping configuration. All pass in the `tests` preset:
`native_matrix_call_tests`, `gekko_core_tests`, `native_failure_publication_tests`, `lift_sites_reference_tests` (they
use code Shipping does not compile), `native_expand_jobs_tests` (loader modes Shipping removes), `guest_watch_tests`
(non-shipping only), `compiler_body_tests` (its helper and audio checks pass; its last check needs an idle loop Shipping
does not have), and `platform_gc_removal_plan`, `platform_runtime_coupling`, `platform_retired_readers_context`,
`platform_retired_readers_reverb` (their control rows are no longer compiled in Shipping).

`dolrecon_lift_tests` is not in either count: it needs generated code that cannot be published. Run from the development
tree it fails with last-digit floating-point differences. That code is compiled only into that test, not into the game.
**This failure is unresolved.**

### Strict comparison against the original code

Reference build, 30 s per stage:

| Stage | Segments matched | Mismatched |
|---|---|---|
| Westopolis | 10,193,336 | 0 |
| Final Haunt | 11,168,459 | 0 |
| Lethal Highway | 9,925,857 | 0 |

Each stage matched 169 more segments than before the Resolution row (reverting that commit restores 10,193,167 for
Westopolis). The menu's fonts grew with its texts (English 116,982 to 117,166 bytes), which fits a little more guest
work at boot, but that link was not proved.

### The installed Shipping executable

Installed by `installer/Install.ps1` into a folder inside this repository's build tree, and run from there with its own
game files:

| Check | Result |
|---|---|
| Replay determinism, Westopolis tape, twice | Audio `46a70a1051347069` both times |
| Run length | 3,645,000,000 ticks, 2,697 fields, 5,360 frames |
| Five rendered frames | All identical to the reference images |
| First launch, headless and in a real window | Exit 0 |
| Create a save through the game's dialog, reload it in a new process | Passed |
| PAL save | All 8,028 record bytes reach game memory |
| Converted USA and Japanese saves | Same, except the one byte PC settings override |

### Installer and uninstaller

| Check | Result |
|---|---|
| Build and install from an empty build folder | Passed |
| Update over an existing install | Save unchanged |
| `Uninstall.cmd` | Removes the install and its shortcuts; keeps saves, settings and an unrelated file |
| Reinstall over kept saves | The game loads them |
| `Uninstall.cmd -RemoveSaves` | Folder gone |
| `installer/tests/Test-Uninstall.ps1` | All checks pass |
| Setup backend (prerequisites, disc, target) | Passed |

### Controller configurator

| Check | Result |
|---|---|
| `pad_config_tests`, against the game's own parser | Passed |
| Every button chosen by clicking the real window's pictures | All 12 correct |
| Save in the installed configurator, then the installed game reads it | Passed |
| Uninstall removes it and keeps `pad.cfg` | Passed |

## Not tested

- Choosing a button by pressing it on a real controller in the configurator, and the pressed highlight.

- A person clicking through the setup window. Its own scripted run went through every page and a complete install.
- Shortcuts in the real Desktop and Start menu. Their code ran with folders inside this repository, as the work was
  limited to it.
- Another machine, or a clean Windows install.
- Add/Remove Programs: no entry exists, by design. Removal is through `Uninstall.cmd` or the Start menu shortcut.
- The new Resolution row by script in the real menu; its logic has automated tests, and the owner tried it in the game.
- How the game feels to play, beyond the owner's own sessions.

## Known limitations

- Input tapes recorded before stage loading became faster arrive late. The check tape above is unaffected.
- The Hungarian language is switched off in this release. Its code, texts and tools remain.
  Every launch prints a warning that the Hungarian 50/60 Hz picture was not applied; it is harmless.
- `rwcore` and `rpworld` hold reconstructed RenderWare code. Check its licensing before publishing this repository.
- Two floating-point test mutants were removed and two difftest fingerprints regenerated after the model was corrected:
  estimate instructions do not flush denormals.
