# Settings guide

This guide explains the *Settings* program that comes with the game. It is where you set the screen, the frame rate,
the sound, the language, your controller, the keyboard and mouse, and autosave.

You do not need to edit any file by hand. The file and key names in this guide are only there for people who want
them.

## Contents

- [Opening the program](#opening-the-program)
- [How the window works](#how-the-window-works)
- [Save, Close and Restore defaults](#save-close-and-restore-defaults)
- [Display](#display)
- [Frame rate](#frame-rate)
- [Sound](#sound)
- [Language](#language)
- [Controller buttons](#controller-buttons)
- [Controller options](#controller-options)
- [Keyboard and mouse](#keyboard-and-mouse)
- [Saving](#saving)
- [Zoom and theme](#zoom-and-theme)
- [If something looks wrong](#if-something-looks-wrong)

## Opening the program

Use either way:

- Open the Start menu and choose the game's *Settings* shortcut.
- Open the folder where you installed the game and run `Settings.exe`.

The window is called *Settings - Shadow the Hedgehog*.

## How the window works

- The list on the left shows the pages: *Display*, *Frame rate*, *Sound*, *Language*, *Controller buttons*,
  *Controller options*, *Keyboard and mouse*, *Saving*. Click a page to open it.
- Under that list are two small lists: *Zoom* and *Theme*. They change how the program itself looks.
- The bar at the bottom shows a status line and three buttons: *Restore defaults*, *Save* and *Close*.
- If the window is small or the zoom is large, the page list shows only icons, and the *Zoom* and *Theme* lists move to
  the bottom bar. The page scrolls up and down with the mouse wheel.

**When do changes take effect?** The game reads its settings when it starts. Change a setting, click *Save*, then start
the game (or close it and start it again). There is one exception: keyboard and mouse changes are also picked up when
you switch back to a game that is already running.

You can leave the game running while you use the program. The bottom bar then says: "The game is running. You can still
save: it uses the new settings the next time it starts."

**Where are the settings kept?** In small text files in the install folder, next to `Settings.exe`:

| File | What it holds |
|---|---|
| `display.cfg` | the *Display* and *Frame rate* pages |
| `options.cfg` | the *Sound*, *Language*, *Controller options* and *Saving* pages |
| `pad.cfg` | the *Controller buttons* page |
| `controls.cfg` | the *Keyboard and mouse* page |
| `appearance.cfg` | the zoom and theme of the Settings program and the Mod Manager |

## Save, Close and Restore defaults

**Save**

- Nothing is written until you click *Save*. The status line says "You have unsaved changes. Click Save to keep them."
  while there is something to save.
- *Save* saves the changes of all pages at once, not only the page you are looking at.
- Only the settings you changed are written. Everything else in the files stays as it was. A file is only created when
  you save something that belongs in it.
- After a save you see "Saved. The game uses the new settings the next time it starts."
- If a value is not allowed (for example a window size that is too small), the status line says what is wrong and ends
  with "Nothing was saved." Fix the value and click *Save* again.

**Close**

- If you have unsaved changes, the program asks "Save your changes before closing?"
  - *Yes* saves and closes.
  - *No* closes and throws the changes away.
  - *Cancel* keeps the window open.
- There is no separate "undo" button. To throw away your changes, close the program and answer *No*.

**Restore defaults**

- *Restore defaults* only resets the page you are looking at. The other pages keep their values.
- It does not save. The status line says "This page is back to the game's defaults. Click Save to keep them." Click
  *Save* to keep the defaults, or close and answer *No* to keep what you had.

## Display

"How the game's window and picture look." Everything on this page is kept in `display.cfg`.

| Setting | What it does | Choices | Default | Key in `display.cfg` |
|---|---|---|---|---|
| **Screen mode** | *Window* shows the game in a window. *Full screen* covers the whole screen at your desktop's resolution. *Exclusive full screen* switches the monitor to the resolution you pick under *Full screen resolution*. | Window, Full screen, Exclusive full screen | Window | `fullscreen`, `exclusive` |
| **Window size** | The size of the game's window, as width x height. Pick one from the list or type your own, such as `1600 x 900`. | 1280 x 720, 1366 x 768, 1600 x 900, 1920 x 1080, 2560 x 1440, 3840 x 2160, 1024 x 768, 1440 x 1080, or your own (at least 320 x 240, at most 16384 x 16384) | 1280 x 720 | `width`, `height` |
| **Full screen resolution** | The resolution used by *Exclusive full screen*. It is greyed out for the other two screen modes. | "The same as the desktop", or one of the resolutions your monitor offers | The same as the desktop | `fullscreen_width`, `fullscreen_height` |
| **Picture shape** | *Automatic* fills the whole window or screen. A fixed shape keeps that shape and adds black bars where it does not fit. | Automatic (fills the window or screen), 4:3 (the original game), 5:4, 16:10, 16:9, 21:9, 32:10, 32:9 | Automatic | `aspect` |
| **Resolution scale** | How sharp the 3D picture is drawn. Below 100 % is faster but blurrier. Above 100 % is sharper but needs a stronger graphics card. | Slider, 25 % to 200 % | 100 % | `render_scale` |
| **Edge smoothing (anti-aliasing)** | Smooths jagged edges of 3D objects. Higher is smoother but slower. If your graphics card cannot do the chosen level, the game uses the nearest one it can. | Off, 2x, 4x, 8x, 16x, 32x | 4x | `msaa` |
| **Texture sharpness (anisotropic filtering)** | Keeps ground and wall textures sharp when you see them at an angle. *Off* looks like the original console. | Off, 2x, 4x, 8x, 16x | 16x | `anisotropy` |
| **Draw distance** | How far away objects are still shown. 1 is the original game. Higher shows more but needs more power. | Slider, 1 to 8 | 1 (original) | `draw_distance` |

Good to know:

- In the game, Alt+Enter switches between window and full screen.
- You can also drag the edge of the game's window to resize it.
- If *Exclusive full screen* picks a resolution your monitor cannot show, close the game and choose another one here.

## Frame rate

"How smoothly the picture is shown. The game itself always runs at its original speed." Everything on this page is kept
in `display.cfg`.

| Setting | What it does | Choices | Default | Key in `display.cfg` |
|---|---|---|---|---|
| **V-Sync** | Waits for your monitor before showing each picture, so the picture never tears. Turn it off only if you want the lowest delay or your monitor has G-Sync or FreeSync. | On, Off | On | `vsync` |
| **Frame rate** | How many pictures per second are shown. A higher rate only looks smoother when *Smooth motion* is on. | 30, 60, 75, 90, 120, 144, 165, 240 or 360 per second, or Unlimited | 60 per second | `render_rate` |
| **Smooth motion (frame interpolation)** | Blends the game's own frames to fill the extra pictures when the frame rate is above 60. It has no effect at 60 per second or less. | On, Off | Off | `frame_interpolation` |
| **Frame counter** | Shows the frames per second in the top right corner of the game. | On, Off | Off | `fps_counter` |

Good to know:

- In the game, the `*` key on the number pad switches the frame counter on and off.
- For smoother motion on a fast monitor, pick a frame rate above 60 **and** turn *Smooth motion* on. One without the
  other does not make the motion smoother.

## Sound

"The volume of each kind of sound." Everything on this page is kept in `options.cfg`.

| Setting | What it does | Choices | Default | Key in `options.cfg` |
|---|---|---|---|---|
| **Master volume** | The volume of everything the game plays. | Slider, 0 % to 100 % | 100 % | `master_volume` |
| **Music** | The music, below the master volume. | Slider, 0 % to 100 % | 100 % | `music_volume` |
| **Sound effects** | Jumps, rings, explosions and other effects, below the master volume. | Slider, 0 % to 100 % | 100 % | `sfx_volume` |
| **Voices** | Spoken lines in cutscenes and during play, below the master volume. | Slider, 0 % to 100 % | 100 % | `voice_volume` |
| **Speaker output** | The same setting as the game's own *Options > System*. Choose *Dolby Pro Logic II* only if you have a surround receiver that decodes it. | Not chosen yet (the game decides), Stereo, Mono, Dolby Pro Logic II | Not chosen yet (the game decides) | `speaker` |

"Not chosen yet (the game decides)" means the file does not say anything about this setting, so the game uses its own
value. You will see the same choice on a few other pages. If you pick it and save, the line is removed from the file.

## Language

"Voices, subtitles and the language of the game's text." Everything on this page is kept in `options.cfg`.

| Setting | What it does | Choices | Default | Key in `options.cfg` |
|---|---|---|---|---|
| **Spoken language** | The language of the voices. | Not chosen yet (the game decides), English, Japanese | Not chosen yet (the game decides) | `voice` |
| **Subtitles** | Shows the text of spoken lines. *Off* hides only the text. The voices still play. | On, Off | On | `subtitles` |
| **Text language** | The language of menus and subtitles. The same setting as the game's own *Options > Subtitle*. | Not chosen yet (the game decides), English, French, German, Italian, Spanish, Japanese | Not chosen yet (the game decides) | `language` |

Japanese voices need the voice files of the Japanese disc in a `voice_jp` folder of the game. Without them the voices
stay English. The README explains how the setup copies them.

## Controller buttons

This page decides which button on your controller presses which button of the game. It is kept in `pad.cfg`.

The game was made for the GameCube controller, so the page talks about the GameCube's buttons: A, B, X, Y, Start, L, R,
Z and the D-Pad.

### What you see

- **A list** with two columns: *Game button* and *Your controller (click to change)*. There is one row for each of the
  twelve game buttons.
- **A picture** of the game's controller, drawn twice (from above and tilted, so you can see the shoulder buttons).
  Each button on the picture has a label with the button of *your* controller that presses it. The picture also says
  "Click a button on the picture to change it".
- Under the list: *Trigger press needed*, *Button prompts*, and *Move with the right stick, camera with the left*.

### How the picture works

- Hold a button on your controller. A dot appears beside its row in the list, and the picture reacts. This is a quick
  way to check that a button does what you expect.
- Move a stick, and the stick on the picture moves with it.
- Click a button on the picture to change it. This does the same as clicking its row in the list.
- The labels next to the two sticks say *Left stick* and *Right stick*. They swap when you tick *Move with the right
  stick, camera with the left*.

### Change a button, step by step

1. Connect your controller. The status line at the bottom says "Controller connected. Click a button on the picture or
   in the list to change it."
2. Click the game button you want to change, either on the picture or in the list. Its row now says "Press a
   button...".
3. Press the button you want on your controller. You have about six seconds. If you wait longer, nothing changes.
4. The row shows the new button.
5. Click *Save*.
6. Start the game (or restart it). It uses the new buttons.

While the row says "Press a button...":

- **Esc** cancels. Nothing changes.
- **Delete** (or Backspace) clears the game button, so no button presses it. The row then says "Not set".

Rules the program follows:

- You can choose A, B, X, Y, LB, RB, Start, Back, the two stick clicks (shown as L3 and R3) and the four D-Pad
  directions.
- The two triggers (LT and RT) cannot be chosen. They always work as the game's L and R. That is why the L row says,
  for example, "LB or LT", and the R row says "RT".
- One controller button does one job. If the button you press was already used by another game button, that other game
  button gets the button you just replaced. In other words, the two swap, and no game button is left empty by accident.

### The buttons at the start

| Game button | Your controller | Key in `pad.cfg` |
|---|---|---|
| A | A | `a` |
| B | X | `b` |
| X | B | `x` |
| Y | Y | `y` |
| Start | Start | `start` |
| L | LB or LT | `l` |
| R | RT | `r` |
| Z | RB | `z` |
| D-Pad Up, Down, Left, Right | D-Pad Up, Down, Left, Right | `dpad_up`, `dpad_down`, `dpad_left`, `dpad_right` |

The names in the middle column are those of an Xbox controller. B and X look swapped on purpose. On the GameCube
controller, B sits to the left of A and X sits to the right. The default keeps the buttons in the same *places* on a
modern controller.

### The three settings under the list

| Setting | What it does | Choices | Default | Key in `pad.cfg` |
|---|---|---|---|---|
| **Trigger press needed** | How far you have to press a trigger (LT or RT) before it counts as L or R. A low number reacts to a light press. A high number needs a firm press. | Slider, 0 to 255 | 40 | `trigger_threshold` |
| **Button prompts** | Which controller's button pictures the game shows in its prompts. *Automatic* follows the controller that is connected. *Original (GameCube)* shows the game's own pictures. | Automatic, Original (GameCube), Xbox, PlayStation, Nintendo Switch | Automatic | `button_style` |
| **Move with the right stick, camera with the left** | Swaps the two sticks. | Ticked or not | Not ticked | `swap_sticks` |

*Restore defaults* on this page resets all twelve buttons and these three settings.

## Controller options

"Rumble and the camera stick." Everything on this page is kept in `options.cfg`.

| Setting | What it does | Choices | Default | Key in `options.cfg` |
|---|---|---|---|---|
| **Vibration** | Whether the controller rumbles. The same setting as the game's own *Options > Vibration*. | Not chosen yet (the game decides), On, Off | Not chosen yet (the game decides) | `vibration` |
| **Rumble strength** | How hard the controller rumbles when vibration is on. 100 % is the original strength. | Slider, 25 % to 100 % | 100 % | `rumble_strength` |
| **Invert camera left and right** | Pushing the camera stick right turns the camera the other way. | On, Off | Off | `invert_camera_x` |
| **Invert camera up and down** | Pushing the camera stick up moves the camera the other way. | On, Off | Off | `invert_camera_y` |

These two "invert" switches are for the controller's camera stick. The mouse has its own settings on the *Keyboard and
mouse* page.

## Keyboard and mouse

The game can be played with keyboard and mouse. This page is kept in `controls.cfg`.

The page has two parts: a table of actions with their keys, and a group of mouse settings.

### The table of actions

Each row is one action. Each action has **three boxes**, so you can give it up to three keys or mouse buttons. A box
that shows "-" is empty.

**Change a key, step by step**

1. Click a box in the row of the action. The box says "Press a key...".
2. Press the key or the mouse button you want. You have about six seconds.
3. The box shows the new key.
4. Click *Save*.

While the box says "Press a key...":

- **Delete** empties the box. (Because of this, the Delete key itself cannot be chosen here.)
- **Waiting a few seconds** cancels. Nothing changes.
- Every other key is taken as your choice. That includes Esc, so do not press Esc to cancel.
- You can use the left, right and middle mouse buttons and the back and forward buttons on the side of the mouse.
- Left and right Shift, Ctrl and Alt count as different keys.

If you pick a key that another action already uses, the program does not stop you. The status line tells you, for
example: "E is also used for: Action (X). Both will happen when you press it." Empty one of the two boxes if you do not
want that.

In the game's menus, mouse buttons do nothing. Use the keyboard there.

**The actions and their keys at the start**

| Action on the page | Key at the start | Key in `controls.cfg` |
|---|---|---|
| Move forward | W | `move_forward` |
| Move back | S | `move_back` |
| Move left | A | `move_left` |
| Move right | D | `move_right` |
| Walk (hold) | Left Ctrl | `walk` |
| Camera left | none | `camera_left` |
| Camera right | none | `camera_right` |
| Camera up | none | `camera_up` |
| Camera down | none | `camera_down` |
| Jump (A) | Space | `jump` |
| Attack (B) | Left mouse button | `attack` |
| Action (X) | E | `action` |
| Chaos power (Y) | F, Right mouse button | `chaos` |
| L button | Q | `shoulder_l` |
| Move sideways (R, hold) | Left Shift | `parallel_move` |
| Z button | C | `trigger_z` |
| Pause (Start) | Enter | `pause` |
| Back / pause | Esc | `back` |
| Menu up (D-pad) | Up arrow | `menu_up` |
| Menu down (D-pad) | Down arrow | `menu_down` |
| Menu left (D-pad) | Left arrow | `menu_left` |
| Menu right (D-pad) | Right arrow | `menu_right` |

The letters in brackets are the game's own buttons, so you can match them with the button prompts the game shows.

The four *Camera* actions have no key at the start, because the mouse turns the camera. Give them keys if you want to
turn the camera from the keyboard. The first box of each *Camera* row is a list: the mouse movement that action follows
(*Mouse left*, *Mouse right*, *Mouse up*, *Mouse down* or *No mouse*; see [The four camera mouse lists](#the-four-camera-mouse-lists)),
and its other two boxes are its keys.

### The mouse settings

| Setting | What it does | Choices | Default | Key in `controls.cfg` |
|---|---|---|---|---|
| **Mouse speed, left and right** | How fast the camera turns sideways when you move the mouse. 1 is the normal speed. 2 turns twice as fast. | A number from 0.01 to 50, for example 1.5 | 1 | `mouse_sensitivity_x` |
| **Mouse speed, up and down** | The same for moving the mouse up and down. | A number from 0.01 to 50 | 1 | `mouse_sensitivity_y` |
| **Keep the pointer in the game** | Hides the mouse pointer and keeps it inside the game while you play. | On, Off | On | `mouse_capture` |
| **Walk speed** | How fast you move while you hold the *Walk* key. | Slider, 1 to 100 | 36 | `walk_deflection` |

### The four camera mouse lists

The list in the first box of each *Camera* row in the table of actions decides which mouse movement does that action:

| Row | Choices | Default | Key in `controls.cfg` |
|---|---|---|---|
| **Camera left** | Mouse left, Mouse right, Mouse up, Mouse down, No mouse | Mouse right | `camera_left_mouse` |
| **Camera right** | the same five choices | Mouse left | `camera_right_mouse` |
| **Camera up** | the same five choices | Mouse up | `camera_up_mouse` |
| **Camera down** | the same five choices | Mouse down | `camera_down_mouse` |

The defaults for left and right look the wrong way round, but they are right. In this game, "camera left" swings the
camera around to the left side of Shadow, and that makes the *view* turn to the right. So at the start, *Camera left*
follows the mouse moving right, and the view follows your hand.

What you will usually want:

- **The camera turns the wrong way sideways.** Choose *Mouse left* in the *Camera left* row and *Mouse right* in the
  *Camera right* row.
- **The camera goes the wrong way up and down.** Choose *Mouse down* in the *Camera up* row and *Mouse up* in the
  *Camera down* row.
- **You do not want the mouse to move the camera in one direction.** Choose *No mouse* in that row.

If your `controls.cfg` comes from an older version and uses its "invert mouse" lines (`mouse_invert_x`,
`mouse_invert_y`), the four lists already show what really happens in the game. When you choose something in one of the
lists and save, the program writes your choices into the four `camera_..._mouse` lines and switches the old invert lines
off.

*Restore defaults* on this page resets all the keys and all the mouse settings.

## Saving

"Whether the game saves your progress by itself." This page is kept in `options.cfg`.

| Setting | What it does | Choices | Default | Key in `options.cfg` |
|---|---|---|---|---|
| **Autosave** | *On*: the game saves by itself after stages and when you leave its Options screen. *Off*: it saves only when you choose to. | Not chosen yet (the game decides), On, Off | Not chosen yet (the game decides) | `autosave` |

## Zoom and theme

These two lists change how the Settings program looks. They do not change the game.

**Zoom** makes everything in the program larger.

- Choices: 100 %, 125 %, 150 %, 175 %, 200 %. The default is 100 %.
- Keyboard: hold **Ctrl** and press **+** to zoom in or **-** to zoom out. **Ctrl and 0** goes back to 100 %.
- Mouse: hold **Ctrl** and turn the mouse wheel.
- Zoom never changes the size of the window. The program rearranges the page to fit, and you can scroll.

**Theme** changes the colours.

- *Theme: Same as Windows* follows the light or dark mode of Windows. This is the default.
- *Theme: Light*
- *Theme: Dark*
- *Theme: Shadow* is black with red.

Both are used right away. You do not need to click *Save* for them. They are kept in `appearance.cfg` (keys `zoom` and
`theme`). The Mod Manager uses the same file, so it shows the same zoom and theme the next time you open it.

## If something looks wrong

**A setting did not change in the game.**
Check that you clicked *Save*, then close the game and start it again. The game reads its settings only when it starts.

**I want everything back the way it was after installing.**
Close the game and the Settings program. Then delete the file of the page you want to reset: `display.cfg`,
`options.cfg`, `pad.cfg` or `controls.cfg` in the install folder. The game and the Settings program both use the
defaults when a file is missing. Deleting a file does not touch your saved game. To reset only one page, you can also
open that page, click *Restore defaults*, then *Save*.

**The program opens with a message about `display.cfg`.**
The message says that `display.cfg` has a line the game cannot read, and that the game will not start with it. This
happens when the file was edited by hand and contains a mistake. The program shows the defaults and refuses to save
display settings until the file is fixed. The easy fix is to close the program, delete `display.cfg`, and open the
program again.

**The program opens with a message about `pad.cfg`.**
The same thing for the controller file: "pad.cfg has a problem, so the default buttons are shown". The game does not
start with a broken `pad.cfg` either. Delete `pad.cfg`, or change a button on the *Controller buttons* page and click
*Save*. Saving writes a fresh, correct file.

**"No controller found. Connect one to choose buttons."**
The program does not see a controller. Connect it, or turn it on, and wait a moment. The program looks for controllers
that Windows handles as Xbox-style controllers (XInput).

**A list shows a value with "(set in the file)".**
The file holds a value that is not one of the normal choices, for example a frame rate of 100. The program keeps it
until you pick something else.

**"Full screen resolution" is greyed out.**
It is only used by *Exclusive full screen*. Choose that under *Screen mode* first.

**My monitor cannot show the resolution I chose for exclusive full screen.**
Close the game (Alt+F4), open the Settings program, and pick another *Full screen resolution* or another *Screen mode*.

**The status line says a file "could not be written" or "could not be replaced".**
The install folder may be read-only, or the file may be open in another program. Close the other program and try again.

**The text is too small or too large.**
Use the *Zoom* list, or Ctrl and +, Ctrl and -, Ctrl and 0.

For every line of every file, see [PC_SETTINGS_FILES.md](PC_SETTINGS_FILES.md).
