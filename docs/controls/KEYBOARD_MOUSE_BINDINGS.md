# Keyboard and mouse bindings

The default PC layout. Every line is configurable in `controls.cfg`, beside the executable. Actions are named for what
they do in the game; the game control each one raises is in the second column. The behaviour of each control is the
original game's (ORIGINAL_GAMECUBE_CONTROL_MAP.md).

| Action (config key) | Control | Default | What the game does with it |
|---|---|---|---|
| `move_forward` / `back` / `left` / `right` | left stick | W S A D | movement, camera-relative |
| `walk` | left stick, partial | Left Ctrl | holds the stick at the walking deflection |
| `jump` | A | Space | jump; jump dash and homing attack in the air |
| `attack` | B | Left mouse | attack; use the held weapon; slide |
| `action` | X | E | spin dash, light dash, dark spin; pick up a weapon, enter a vehicle, throw; context objects |
| `chaos` | Y | F, Right mouse | Chaos Blast, Chaos Control, cast away the held weapon |
| `parallel_move` | R (held) | Left Shift | parallel movement |
| `shoulder_l` | L | Q | menus; no on-foot use found |
| `trigger_z` | Z | C | menus; no on-foot use found |
| `pause` | Start | Enter | pause; confirm in menus |
| `back` | Start in play, B in menus | Esc | pause during play, back in a menu |
| `menu_up` / `down` / `left` / `right` | d-pad | arrow keys | menus and the d-pad's mission picks |
| `camera_left` / `right` / `up` / `down` | right stick | unbound | camera by keys, for players without a mouse |

**Mouse:** moving it right turns the view right; forward moves the camera in and lower, the same input as the game's
stick. Mouse buttons do nothing in menus.

**Which way the mouse steers the camera** is set per camera action: `camera_left_mouse`, `camera_right_mouse`,
`camera_up_mouse` and `camera_down_mouse` each name the mouse movement that action follows: `left`, `right`, `up`, `down`
or `none` (the Settings program's Keyboard and mouse page has a list for each). Moving the mouse that way steers the camera
as the action does, in proportion to the motion. The game's camera left orbits the camera to the left, which swings the
view right, so by default `camera_left_mouse = right` and `camera_right_mouse = left` (the view follows the hand), and
`camera_up_mouse = up`, `camera_down_mouse = down`. `mouse_invert_x` / `mouse_invert_y` still swap the two directions of
their axis on top (older files); the Settings program writes the movements instead and turns the inverts off.

**Not rebinding on purpose:** Alt+Enter, Alt+F4. Esc quits only if the player binds no action to it. N and M are still
the development savestate keys in non-shipping builds.

## controls.cfg

One `key = value` per line, `#` for comments. An action takes up to 3 comma-separated binds, or `none`.

```
jump = key:Space, mouse:X1
attack = mouse:Left
mouse_sensitivity_x = 1.0     # 0.01 to 50: 1.0 = 0.06 degrees of yaw per count
mouse_sensitivity_y = 1.0
mouse_invert_x = 0
mouse_invert_y = 0
camera_left_mouse = right     # left, right, up, down or none
camera_right_mouse = left
camera_up_mouse = up
camera_down_mouse = down
mouse_capture = 1             # hide and confine the cursor in play
walk_deflection = 36          # 1 to 100 stick units
```

Key names: letters, digits, `F1`..`F24`, `Num0`..`Num9`, `Space`, `Enter`, `Esc`, `Tab`, `Backspace`, `LShift`, `RShift`,
`LCtrl`, `RCtrl`, `LAlt`, `RAlt`, `Up`, `Down`, `Left`, `Right`, `Insert`, `Delete`, `Home`, `End`, `PageUp`, `PageDown`,
`CapsLock`, `Pause`, or `0xNN` for any other virtual key. Mouse buttons: `Left`, `Right`, `Middle`, `X1`, `X2`.

A line that cannot be read is reported on the console and skipped. The rest of the file still applies, so one typo never
removes the other controls. A missing file is the default layout. The file is re-read when the game window returns to the
front, so a rebinding made in another program takes effect on Alt+Tab.

Gamepad bindings stay in `pad.cfg` (unchanged). The gamepad and the keyboard and mouse are combined on port 1: buttons
from either, the gamepad's sticks where deflected.
