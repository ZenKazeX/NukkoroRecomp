# Camera control: what the game does, and the mouse

The full reverse engineering is in `docs/camera-re-20261102/`. This records the parts that decide how a mouse can drive
the camera, and the design that follows.

## The camera's input

`PostManageTask` (`0x80297B8C`) copies the player's right stick into the camera target (TargetInfo) with no scale and no
sign change: `TI+0x4C` is X, `TI+0x50` is Y (`y` positive = down). The copy is gated by a global input lock (`G+0xC8`)
and a player state bit (`0x2A`).

- **Yaw.** `CameraModeGeneral::AnalogMove` (`0x801C3A9C`) rotates the eye about the up axis through the player by
  `dt * 150 degrees * input` (`0x805F6C64`). The rotation is a rate. Input is the normalised stick, -1 to 1.
- **Height and distance.** The same step moves the eye nearer or farther by `100 * input * dt`, and up or down by up to
  `50 * input * dt`. The game has no separate pitch: a stick moved up brings the camera in and lower, moved down takes it
  out and higher, which also tilts the view. Limits: distance 2.5 to 120, height 0 to 50.
- **Permission.** `StageCameraManager` (`0x801CAF8C`) reads the same pair: `x^2 + y^2 >= 1e-4` (the test at `0x801CB86C`) counts as the player
  steering, and only then is the manual override set up. Volume record flag bit 3, external scripted cameras without flag
  1, and scripted modes with their extra bit forbid it, so no camera input does anything there. After 2 s without input
  the camera may return to the scripted view.

## What the mouse is

Mouse motion is fed into that same input, one engine read at a time. Every constraint the game puts on the camera stick
therefore applies to the mouse without extra code: cinematic and scripted cameras stay fixed, volumes that forbid manual
control keep forbidding it, collision still pushes the eye, and the return to the scripted camera works.

No camera state is written directly. There is no separate free-look mode: nothing in the original forbids what the stick
can do, and nothing outside what the stick can do is attempted.

### The arithmetic (`peripheral/mouse_look.h`)

For each engine read: counts since the last read become `degrees = counts * 0.06 * sensitivity`, then
`input = degrees / (150 * 1/60)`. Since yaw is `150 * input * dt` and the game frame is 1/60 s, the camera turns by exactly
`degrees`: rotation depends on counts alone, not on rendered frames, display size, or cursor position.

- The stick written is the value the engine normalises back to that input (dead zone 25, range 39), so the error is at
  most half a stick step (1/78 of full input; about 0.03 degrees per frame).
- Motion below the game's steering threshold waits in the accumulator rather than vanishing.
- Motion above full input is limited to it, and the excess is dropped rather than owed, so the camera stops when the
  hand stops.

### Limits of this design

- **Turn rate is capped at the game's 150 degrees per second.** A flick faster than that plays at 150 degrees per second.
  This is the one place a mouse can outrun the original camera input. Going past it needs a generator change at the bl to
  `0x8006F9F8` (`0x80297D4C`) that supplies an unclamped value; not done, see INPUT_IMPLEMENTATION_STATUS.md.
- **Vertical is the game's height and distance input, not a pitch angle.** That is the only vertical camera input the
  game has.

### Input capture

The mouse belongs to the camera only while the game is playing and its window is in front (`gameplay_camera_active`: pause
byte `0x805EF27C` clear and camera unit 0 has a request). Otherwise the cursor is free and motion is ignored. Counts
gathered while the mouse is not accepted are never added, and a reset drops any waiting, so returning to the game cannot
jump the camera.

## Measured on the real game

`tools/controls/camera_parity.py`, driving the game's own camera with scripted camera-stick samples at a Westopolis
position where the player had already steered the camera (results in INPUT_PARITY_VALIDATION.md): yaw is `150 * input`
degrees per second at every level tried, in both directions, to within measurement noise.

A positive stick orbits the eye to the right, which turns the view left, so the mouse moving right is a negative stick.
