# Native input architecture

```
Keyboard / mouse      raw input on the input thread (gekko/raw_input.h): level of every key and mouse button, mouse totals
Gamepad               XInput on the same thread (gekko/xinput_pad.h, pad.cfg)
   |
Action map            peripheral/action_map.h: named actions, up to 3 binds each, controls.cfg
                      gekko/controls_settings.h: snapshot per poll, atomic update, reload on change
   |
Action state          which actions are held; the actions become one controller sample
   |
Mouse look            peripheral/mouse_look.h: counts to camera input, on the game thread at the sample's entry
   |
Peripheral manager    the game's own read of port samples: held / pressed / released, stick normalisation
   |
Game logic            player controller, camera, vehicles, menus: unchanged translated game code
```

Device-specific code stops at the action map. Nothing below it knows about keys or mice.

## Threads and ownership

| Data | Owner | How others see it |
|---|---|---|
| Key and button levels | input thread | used only there, to build the published sample |
| Mouse totals | input thread writes | atomics, read by the game thread |
| `MouseLook` state | game thread | not shared |
| `ControlsConfig` | settings service | immutable snapshot, replaced whole |
| Published sample | input thread | `pad_mutex`, one short copy per latch |

No allocation on the input path except replacing a config snapshot when bindings change. The input thread already polled
once per millisecond; the additions are one snapshot read and one pass over the action table.

## Timing and determinism

The game reads its pads once per frame, at `Manager::Update`. The sample it reads is the newest published one, plus the
camera input computed from the mouse counts at that moment:

- **Live play:** the engine's read latches the newest sample (`set_live_source`), overlays the mouse and consumes the
  counts.
- **Recording:** the 5 ms latch grid stays (a recording replays exactly); the mouse is overlaid at each latch, and the
  counts are consumed only when the engine actually reads, so none are lost between latches.
- **Replay:** the tape replaces the samples. Keyboard, mouse and bindings play no part: a tape holds the samples the game
  read, so it replays the same under any binding.

Key changes wake the input thread immediately, as they did before. Many mouse events between engine reads simply sum.

Focus: when the game window leaves the foreground, the keyboard sample goes neutral, the mouse stops being accepted, and
what it had accumulated is dropped.

## What stays deliberately GameCube-shaped

`ControllerSample` is the boundary with the game's peripheral manager, because the game's player, camera and vehicle
code is still translated original logic that reads those controls. When that logic is reconstructed natively
(docs/SHADOW_PLAYER_NATIVE_RECONSTRUCTION.md), the action state above can feed it directly and the sample can go. Until
then the sample is what makes tapes, replays and every existing regression test keep working unchanged.
