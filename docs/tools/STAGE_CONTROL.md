# Stage control: boot into a stage, restart it, finish it

`tools/stage_control.py` launches a non-shipping build (default `.build/diag`) with `--boot-stage` and `--dev-link`, and
restarts the stage through the game's own pause-menu route. It copies the card, SRAM, display.cfg and pad.cfg into its
work folder (default `.build/stage-control`), so the user's saves are never touched.

    python tools/stage_control.py --stage stg0100                                   # interactive: restart / status / quit
    python tools/stage_control.py --stage stg0100 --restart-every 20 --restarts 2 --then-run 10
    python tools/stage_control.py --stage stg0604 --headless -- --draw-distance 4  # after "--": game flags

## The route (PAL main.dol)

The pause menu (state machine 0x801E3ECC, cursor at pause object +0x38: 0 continue, 1 restart, 2 quit) maps Restart to
result 3 (0x801E364C); 0x801E4064 closes the menu (0x801E3238), unpauses (0x801E31EC) and calls
`0x801762D0(manager, 7, 1)`. The confirm-dialog path 0x801E34F8 makes the same call.

StageAction manager: a static at 0x80577038, pointer cached at r13+0x2EDC (0x805EF3DC) by its getter 0x80074598.
+0x8 current action, +0xC requested action, +0x14 lock, +0x17+n per-action "ignore requests" byte. 0x801762D0(mgr,
id, force) writes +0xC unless the ignore byte is set, or the lock is set without force. 0x80174974 consumes the
request each frame (jump table 0x8052CC68). Actions: 2 Init, 3 Play, 4 Event, 5 Pause, 6 Goal, 7 Restart,
8 Continue, 9 Dead, 10 Save, 11 TryAgain, 16 End.

RestartAction (vtable 0x80544EE0, exec 0x802062AC) fades out, tears the stage down, resets the stage record
(0x80179008 on 0x8057707C: checkpoint index 0, per-player stats, one life from record+0x748) and requests Init: the
stage restarts from its START. Dead -> Continue (0x80178F34) keeps the checkpoint instead.

The dev-link command `restart_stage` (runtime/src/shadow_main.cpp) is the menu's request with the menu's gate: it
refuses unless the manager exists and is in Play (+0x8 == 3), the Restart ignore byte is clear, the stage is
one-player (0x8057E760+0xB2 == 0) and player 0 has a life left (s32 at 0x805777C4). The tool retries while the stage is
loading.

Verified: Westopolis, two restarts at 20 s and 40 s of game time; the stage returned to Play after the first (the
second request was accepted from Play) and player 0's lives read 5 at the first request and 4 at the second: the
restart took a life, as the menu's Restart does.

## Finishing a stage: its goal and result screen (2026-10-30)

    python tools/stage_control.py --stage stg0100 --goal-after 5 --then-run 24 --capture-every 3 --work .build/result
    python tools/stage_control.py --stage stg0100 --goal-after 5 --then-run 24 --capture-every 3 -- --render-width 1707 --render-height 480 --width 1707 --height 480

Every goal trigger in the game makes the same unforced request, `0x801762D0(manager, 6, 0)` (GoalAction; 0x8011D0EC,
0x8016B43C and eleven more call sites). The dev-link command `goal_stage` makes it with 0x801762D0's own rules: the
manager in Play, the Goal ignore byte (+0x17 + 6) and the lock (+0x14) clear. The game then runs its goal sequence and
the result screen. `--capture-every` saves `capture-NNN.png` in the work folder every N game seconds after the goal.

Measured on Westopolis (one picture per 3 s): the goal flash, the stage's goal scene (from about 4 s after the goal),
the result screen ("Normal mission completed!", score, time, rings, rank) from about 9 s, then the save prompt
(action 10, about 20 s). Through the goal scene and the result screen the StageAction manager stays in Goal (6) while
the EventManager's byte r13+0x3E5D is set; a story event played through StageSequence::PlayEvent runs under action
12 instead. That is why the renderer's 4:3 cutscene framing excludes the Goal action
(runtime/scaffold/rw_driver/rw_driver.h, kStageActionGoal).
