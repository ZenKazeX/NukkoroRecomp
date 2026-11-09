# Settings validation harness - results (Agent 5, 2026-11-05)

STATUS: NOTHING HAS BEEN BUILT OR LAUNCHED YET. The coordinator ordered "no RUN.lock, no builds, no reconfigure, no game
launches" until further notice (the voice fix has priority). Everything below is written and syntax-checked only.

| Item | Command | Result |
|---|---|---|
| options_settings_tests (unit) | `cmake --build .build/tests --config Release --target options_settings_tests` then `ctest -C Release -R "options_\|settings_registry"` in .build/tests | NOT RUN (never compiled) |
| options_input_tests, options_config_tests (other agents) | same | NOT RUN here (wired in CMakeLists.txt only) |
| run_lifecycle.py --list / --dry-run (4 scenarios) | `python -I tools/options_tests/run_lifecycle.py --dry-run master_volume rapid navigation restore_all` | ran: folders + .uitest scripts generated, no launch (simulated-only) |
| 16 lifecycle scenarios (master_volume, rumble, camera_invert, speaker, vibration, subtitles, autosave, restore_audio, restore_controls, restore_savedata, restore_all, fresh_boot, fresh_change, corrupt_cfg, rapid, navigation) | `python tools/options_tests/run_lifecycle.py --all` | NOT RUN (no game launched) |

Calibration still needed on the first real run: the script's wait lengths (VI fields) after the boot prefix, the rows'
cursor model (grid has no wrap assumed), and that B after a PC-only A returns to the grid.
