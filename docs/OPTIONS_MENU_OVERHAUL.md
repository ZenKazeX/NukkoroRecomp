# Options menu overhaul - the user's directive (2026-11-05, verbatim below the notes)

## The user's notes that came with it (2026-11-05)

- **Gameplay tile icon:** use the game's own Shadow life icon (the HUD lives counter: Shadow's head in a rectangle).
  Drop the rectangle: mask out only the little Shadow head and put it on the **Gameplay** settings tile, where the
  reference image places it (bottom-left tile of the 2 x 3 grid: Display / Graphics, Audio / Controls,
  Gameplay / System).
- **Save / load:** "we're gonna have to reroute the save load system as well." The game's save / load path has to be
  rerouted too (the options persistence lives in it). Confirm its scope with the user before a large change.
- Reference image: `H:\ShadowTheHedgehogDiskGC_Extract\Options menu redesign\optinons overhaul.png` (one image).

## Directive (verbatim)

SHADOW THE HEDGEHOG — COMPLETE OPTIONS MENU OVERHAUL

PRIMARY OBJECTIVE

Completely reconstruct, modernize, and expand the existing Options menu in the native 64-bit PC port of Shadow the Hedgehog (GameCube).
The original Options menu was designed for a console game with only a handful of configurable settings. The modern PC port requires a significantly more capable menu system with extensive customization, advanced graphics configuration, native PC input support, and reliable configuration management.
Your objective is to transform the original limited Options menu into a fully functional, extensible PC configuration interface while preserving the original game's distinctive visual identity, navigation behavior, animations, and presentation.
This is a complete functional and architectural overhaul, not merely a graphical redesign.
Do not replace the original game's appearance with a generic modern interface.
Instead, make the original interface capable of supporting modern PC functionality.
The final result must look like an official PC version of Shadow the Hedgehog developed by the original team.

1. VISUAL REFERENCE — MANDATORY INVESTIGATION

Before beginning the implementation, inspect the images located in:
`H:\ShadowTheHedgehogDiskGC_Extract\Options menu redesign`
Search this directory for the available reference images and examine their layout carefully.
These images illustrate a possible expanded Options menu with:

* Six main configuration categories.
* The original tile-based menu concept.
* A significantly larger settings panel.
* Scrollable lists of configurable options.
* Multiple configuration values visible simultaneously.
* Category selection.
* Context-sensitive descriptions.
* Navigation indicators.
* Improved utilization of available screen space.

CRITICAL: THESE IMAGES ARE CONCEPTUAL REFERENCES ONLY.
They are not exact visual templates.
Do not copy their decorative elements, backgrounds, textures, fonts, icons, proportions, or artificial design details unless those elements already exist in the original game.
Use them to understand the intended functionality, information density, and general organization.
The original Shadow the Hedgehog game remains the definitive visual reference.
Your task is to combine the expanded functionality suggested by the reference images with the authentic appearance of the original game.
Reference priority

1. Original game's actual menu assets and visual behavior.
2. Original game's animation and interaction conventions.
3. Existing native PC-port functionality.
4. The reference images for structural inspiration.

If the concept images contradict the game's original visual language, follow the original game.
If the reference directory does not exist or the images cannot be opened, report the problem accurately and proceed using the original game assets and the requirements in this document.
Do not pretend to have inspected inaccessible images.

2. INVESTIGATE THE ORIGINAL OPTIONS MENU

Before changing code, thoroughly investigate the existing Options menu.
Identify and document:

* Original PowerPC menu functions.
* Their statically recompiled C++ equivalents.
* Existing native replacements, if any.
* Menu state machines.
* Menu navigation logic.
* Selection and highlighting behavior.
* Original category tiles.
* Texture and material references.
* Rendering functions.
* Menu background composition.
* Text and font rendering.
* UI animation routines.
* Confirmation and cancellation behavior.
* Menu sound effects.
* Controller input handling.
* Configuration loading and saving.
* Existing PC resolution and aspect-ratio options.
* Relevant RenderWare UI components.
* Native D3D12 renderer interactions.

Trace the actual execution path from entering the Options menu through changing a setting and returning to the previous screen.
Determine which original components can be safely reused, extended, or replaced.
Do not build an unrelated menu on top of the original menu while leaving the underlying architecture broken or duplicated.
Prefer extending the existing implementation when that provides a correct, maintainable solution.
Preserve the original menu's behavior unless a change is required to support the expanded functionality.

3. PRESERVE THE GAME'S ORIGINAL VISUAL IDENTITY

The redesigned interface must remain visually recognizable as Shadow the Hedgehog.
Reuse existing assets and rendering conventions wherever possible.
Preserve:

* Original menu backgrounds.
* Metallic interface elements.
* Existing menu textures.
* Original typography.
* Text outlines and shadows.
* Original color palette.
* Selection indicators.
* Highlighting effects.
* Navigation animations.
* Transition animations.
* Button prompts.
* Confirmation and cancellation sounds.
* Existing menu timing and responsiveness.

Additional UI components may be created when necessary, but they must be visually derived from the game's established style.
Do not introduce generic operating-system interface elements.
Avoid unrelated flat-design buttons, rounded modern cards, web-style dropdowns, external UI libraries, or newly invented decorative elements.
The finished interface must feel like a natural expansion of the original game.
Preserve the visual DNA. Expand the functionality.

4. MAIN MENU STRUCTURE

Redesign the current Options menu around six major categories:

1. Display
2. Graphics
3. Audio
4. Controls
5. Gameplay
6. System

Retain the original tile-based navigation concept.
The six categories should be arranged in a compact, visually balanced structure appropriate for the original 4:3 menu.
Selecting a category should reveal an expanded configuration panel containing that category's available settings.
The interface must no longer be constrained by the number of settings visible on the screen.
Required functionality
Implement:

* Category navigation.
* Configurable settings lists.
* Automatic scrolling.
* Selection highlighting.
* Left/right value adjustment.
* Toggle controls.
* Numerical sliders.
* Multiple-choice settings.
* Nested configuration screens.
* Context-sensitive descriptions.
* Apply and cancel operations.
* Reset-to-default functionality.
* Disabled states for unsupported features.
* Clear controller and keyboard navigation prompts.

Keep navigation consistent with the original game's interaction conventions.
Use category and settings definitions rather than manually hardcoding every individual menu item.
The interface must remain maintainable and extensible as additional PC features are implemented.

5. DISPLAY SETTINGS

Create a comprehensive Display category.
Include the following settings where supported by the current runtime:
Resolution and display

* Output resolution.
* Aspect ratio.
* Display mode.
* Fullscreen.
* Borderless fullscreen.
* Windowed mode.
* Refresh rate.
* Monitor selection.
* Vertical synchronization.
* Frame-rate limit.

Screen and HUD

* HUD scale.
* HUD positioning.
* HUD safe area.
* Screen alignment.
* Custom resolution support where feasible.

Preserve compatibility with the existing supported aspect ratios:

* 4:3
* 16:9
* 16:10
* 21:9
* 32:10

Additional aspect ratios may be supported when the renderer correctly handles them.
Do not hardcode an arbitrary list of unsupported resolutions.
Where appropriate, query the operating system and graphics backend for available display modes.
Safe display switching
Resolution and display mode changes must be reliable.
Implement:

1. Apply the requested display mode.
2. Show a confirmation dialog.
3. Start a 15-second confirmation countdown.
4. Keep the new mode if confirmed.
5. Restore the previous working mode if confirmation expires.
6. Recover gracefully if the requested mode fails.

Never allow a failed display change to leave the user permanently unable to access the menu.
Preserve existing configuration compatibility.

6. GRAPHICS SETTINGS

Create a dedicated Graphics category with meaningful native renderer controls.
Anti-aliasing
Add configurable MSAA levels:

* Off
* 2×
* 4×
* 8×

Only expose sample counts supported by the actual renderer and relevant render-target formats.
Handle renderer resource changes safely.
Texture filtering
Add configurable anisotropic filtering:

* 1×
* 2×
* 4×
* 8×
* 16×

Respect the underlying graphics hardware capabilities.
Resolution scaling
Provide an internal rendering resolution scale separate from the output resolution.
The system must clearly distinguish:

* Physical display resolution.
* Internal 3D rendering resolution.
* HUD rendering resolution.
* Final output resolution.

The original 3D rendering resolution should not unnecessarily constrain the HUD's sharpness.
Likewise, changing the 3D render scale must not introduce unintended HUD blur or distortion.
Additional graphics controls
Investigate whether the current native renderer supports configurable:

* Texture filtering.
* Texture quality.
* Shadow quality.
* Shadow resolution.
* Visual effects quality.
* Level of detail.
* Particle quality.
* Other relevant rendering features.

Implement settings only when a real, verified engine capability exists.
Do not introduce fake configuration entries.
Do not simulate functionality by silently reducing unrelated quality settings.
Do not invent implementation support simply because an option appears in the reference image.
The default configuration must preserve the original game's validated rendering behavior.
Any intentional graphical changes must be user-controlled, documented, and independently testable.

7. AUDIO SETTINGS

Expand the Audio category using the existing native audio system.
Include applicable controls for:

* Master volume.
* Music volume.
* Sound effects volume.
* Voice volume.
* Audio output device.
* Mute functionality.
* Audio output configuration.
* Other supported audio features.

Preserve original audio behavior at the equivalent default settings.
Avoid changes that introduce audio timing errors, synchronization problems, or unexpected differences in PCM output.
Do not reintroduce obsolete audio middleware.
Use the existing native audio architecture.
Settings must apply correctly during gameplay and across application restarts.

8. CONTROLS AND INPUT SETTINGS

Create a full input configuration system for the PC port.
Include:

* Keyboard configuration.
* Controller configuration.
* Controller selection.
* Controller button rebinding.
* Keyboard key rebinding.
* Analog stick dead zones.
* Analog sensitivity.
* Camera sensitivity.
* Camera inversion.
* Vibration enable/disable.
* Vibration intensity where supported.
* Restore default bindings.

Implement a dedicated binding interface using the original game's visual language.
Input requirements

* Correctly distinguish controller and keyboard inputs.
* Prevent accidental loss of essential navigation bindings.
* Detect binding conflicts.
* Support cancellation during rebinding.
* Preserve original default control behavior.
* Save input configuration persistently.
* Support navigation without requiring a mouse.

A mouse may be supported as an additional input method, but the menu must remain fully usable with a gamepad.
Do not break the original controller behavior.

9. GAMEPLAY SETTINGS

Investigate the game's actual gameplay configuration capabilities.
Expose supported options such as:

* Camera preferences.
* Camera inversion.
* Camera sensitivity.
* Subtitle settings where available.
* Additional gameplay-related preferences supported by the engine.

Preserve the original gameplay behavior by default.
Do not create features merely to fill empty menu space.
Do not introduce settings that unintentionally change:

* Player physics.
* Movement timing.
* Animation behavior.
* Mission logic.
* Collision detection.
* Enemy behavior.
* Game simulation timing.

Only expose settings with a correctly implemented runtime effect.

10. SYSTEM SETTINGS

Create a dedicated System category.
Include appropriate options for:

* Language selection where supported.
* Restore all defaults.
* Restore individual category defaults.
* Configuration management.
* Build information.
* System information.
* Other relevant native PC-port functionality.

Keep developer-only debugging features separate from ordinary user configuration unless explicitly intended.
The menu should remain understandable to regular players.
Avoid unnecessary technical terminology in visible menu labels.

11. 4:3 LAYOUT — ABSOLUTE REQUIREMENT

The primary UI design reference is 4:3.
Design and validate the complete Options menu using a native 4:3 reference layout.
Do not create a 16:9 layout and compress it horizontally.
Do not stretch UI textures.
Do not distort icons.
Do not reduce font sizes excessively to fit additional controls.
The menu should remain readable and navigable at traditional GameCube resolutions.
Mandatory layout tests

* 640×480 — 4:3
* 1024×768 — 4:3
* 1920×1080 — 16:9
* 2560×1600 — 16:10
* 3440×1440 — 21:9
* A valid 32:10 resolution

The user must be able to navigate and configure the game at every supported aspect ratio.
Where necessary, adapt the width and positioning of interface panels.
Keep the original visual proportions intact.
Do not assume that available screen width can always be used for additional settings.
The 4:3 layout is the baseline, not an afterthought.

12. SCROLLABLE SETTINGS PANEL

Implement a reusable scrolling system capable of managing large numbers of settings.
Required features:

* Automatic scrolling to the highlighted item.
* Correct navigation at list boundaries.
* Clear indication of the current scroll position.
* Support for variable-length setting names.
* Support for variable-length values.
* Context-sensitive description panel.
* Proper handling of disabled options.
* Consistent selection appearance.
* Stable selection during category changes.
* Responsive navigation with keyboard and controller.

The list should display only as many rows as comfortably fit within the selected layout.
Do not shrink all entries to make the entire category fit simultaneously.
The menu must support additional settings without redesigning the entire interface.
Optimize rendering to avoid unnecessary per-frame work.

13. CONTEXT-SENSITIVE HELP

Introduce a description panel that explains the currently highlighted setting.
Examples:
Resolution
"Change the screen resolution. Higher resolutions provide a sharper image."
Aspect Ratio
"Choose the display proportions used by the game."
MSAA
"Reduce geometric edge aliasing using multisample anti-aliasing."
Anisotropic Filtering
"Improve texture clarity when surfaces are viewed at oblique angles."
Render Scale
"Adjust the internal 3D rendering resolution without changing the display resolution."
HUD Scale
"Adjust the size of interface elements independently from the 3D rendering resolution."
The wording must be accurate and concise.
Do not describe a setting as providing an effect it does not actually implement.
Keep help text legible within the original menu's style.

14. CENTRALIZED CONFIGURATION ARCHITECTURE

Refactor or extend the configuration backend to support the expanded menu cleanly.
Separate the following responsibilities:

1. Setting definitions.
2. Available values.
3. Hardware capability queries.
4. Current configuration.
5. Pending configuration changes.
6. Runtime application.
7. Persistent storage.
8. Menu presentation.
9. Validation and rollback.

Use the existing configuration infrastructure wherever practical.
Do not duplicate functionality that is already implemented correctly.
Configuration requirements

* Load settings at startup.
* Save modified settings.
* Validate stored values.
* Handle missing fields.
* Recover from corrupted configuration.
* Preserve compatibility with existing user configuration files.
* Restore defaults safely.
* Apply changes without restarting where technically possible.
* Clearly indicate when a restart is required.
* Recreate graphics or audio resources only when necessary.

Use typed settings definitions and explicit validation.
Avoid scattering configuration parsing or renderer state changes throughout UI event handlers.
Every visible setting must be connected to a real engine function.

15. ORIGINAL GAME INTEGRATION

This menu must be integrated directly with the existing native PC-port architecture.
Inspect and reuse:

* Static recompilation runtime.
* Original menu execution paths.
* Native rendering backend.
* Native audio system.
* Input subsystem.
* Configuration management.
* Existing OS replacement services.
* Existing menu asset loading.

Avoid introducing unnecessary dependencies.
Avoid recreating entire systems that already exist and work correctly.
Do not reintroduce removed GameCube hardware emulation merely to support the menu.
The menu should operate through the existing native runtime and the appropriate recompiled game interfaces.
Preserve the original menu transitions and surrounding gameplay systems.

16. PERFORMANCE REQUIREMENTS

The expanded Options menu must remain lightweight.
Avoid:

* Per-frame heap allocations where unnecessary.
* Repeated loading of the same textures.
* Repeated parsing of configuration files.
* Excessive draw calls.
* Unnecessary state synchronization.
* Recreating graphics resources when values are unchanged.
* Recalculating static layouts every frame.
* Blocking file operations during ordinary navigation.
* Unnecessary runtime dependencies.

Prefer cached layout information and reusable UI components.
Menu navigation must remain responsive.
Do not introduce stuttering when switching categories or adjusting ordinary settings.
Do not claim a performance improvement without measurements.

17. IMPLEMENTATION PHASES

Execute the work in the following sequence.
Phase I — Discovery
Investigate the existing Options menu, original assets, relevant translated functions, native runtime, and configuration system.
Inspect the reference images at the provided directory.
Document the original implementation and identify safe extension points.
Phase II — Architecture
Design and implement a reusable, data-driven configuration menu architecture.
Preserve existing working functionality.
Keep the menu presentation separate from the configuration backend.
Phase III — Layout Reconstruction
Rebuild the Options menu layout around six categories and scrollable settings panels.
Use original game textures, fonts, animations, and visual conventions.
Implement the 4:3 layout first.
Then validate widescreen and ultrawide behavior.
Phase IV — Functional Integration
Connect every implemented setting to its actual engine functionality.
Prioritize already-supported features before adding new runtime capabilities.
Do not create placeholder controls.
Phase V — Persistence and Recovery
Implement reliable configuration loading, saving, validation, resetting, and rollback behavior.
Verify safe display mode changes.
Phase VI — Testing
Build and run the game.
Test menu navigation, visual consistency, settings application, persistence, and aspect-ratio behavior.
Perform actual runtime validation wherever possible.
Do not stop after writing an implementation plan unless a genuine external blocker prevents further work.

18. TESTING AND ACCEPTANCE CRITERIA

The implementation is considered complete only when the following conditions have been verified.
Visual integrity

* The menu retains the original game's visual style.
* Original menu assets are reused appropriately.
* The layout works correctly in 4:3.
* Text remains readable.
* No elements overlap.
* No controls are clipped.
* Widescreen and ultrawide modes remain usable.
* The menu does not resemble an unrelated modern UI.

Functionality

* Every visible, enabled setting works.
* Settings affect the intended engine subsystem.
* Configuration changes persist across restarts.
* Defaults can be restored.
* Invalid configurations are handled safely.
* Category navigation works.
* Scrolling works.
* Controller navigation works.
* Keyboard navigation works.
* Resolution rollback works.

Regression safety

* Original gameplay remains functional.
* Original menu behavior is preserved where intended.
* No input regressions.
* No audio regressions.
* No renderer initialization regressions.
* No unexpected changes to game timing.
* No unexpected loss of graphical quality.

Validation
Perform real builds and runtime tests.
Record:

* Tests performed.
* Tests passed.
* Tests failed.
* Features not yet verified.
* Remaining limitations.

Do not fabricate benchmarks, screenshots, or test results.
If the game cannot be launched in the current environment, explicitly state which tests could not be performed.

19. FINAL DELIVERABLES

Provide the following:

1. Fully implemented Options menu overhaul.
2. Description of the original menu architecture.
3. Description of the new menu architecture.
4. Complete inventory of implemented settings.
5. Runtime integration details for each setting.
6. List of unsupported or deferred configuration options.
7. Summary of modified files.
8. Build results.
9. Runtime test results.
10. Screenshots captured from the actual game at 4:3 and 16:9, if available.
11. Known limitations and remaining work.

Clearly distinguish between implemented, tested, partially implemented, and unsupported functionality.
Do not present conceptual mockups as screenshots of the working game.
Do not claim completion merely because the menu appears visually correct.
The configuration controls must be functionally connected to the engine.

20. NON-NEGOTIABLE RULES

* Preserve the original Shadow the Hedgehog visual identity.
* Inspect the provided reference directory.
* Treat the reference images as inspiration, not templates.
* Use 4:3 as the primary layout.
* Support the existing widescreen and ultrawide modes.
* Retain existing working PC-port settings.
* Do not introduce non-functional options.
* Do not degrade graphics quality silently.
* Do not blur or distort the HUD when changing 3D rendering resolution.
* Do not introduce unnecessary external UI frameworks.
* Do not reintroduce obsolete console hardware dependencies.
* Do not break existing gameplay, rendering, audio, or input behavior.
* Do not fabricate performance or validation results.
* Do not stop at a proposal when implementation is feasible.
* Do not make unrelated modifications to the project.

FINAL DIRECTIVE

The original Shadow the Hedgehog Options menu was built for a console with a small number of settings.
We are transforming it into the configuration interface of a complete modern native PC game.
The functionality must grow dramatically, but the game's identity must remain intact.
The reference images show what a larger configuration system could look like structurally.
The original game defines how that system must look, sound, animate, and behave.
Build the expanded Options menu as though the original developers had designed it for a fully featured PC release from the beginning.
Maintain the original atmosphere, the recognizable tile-based design, the existing navigation conventions, and the authenticity of the game's assets.
Implement a scalable architecture that can accommodate current and future PC-port features without another complete redesign.
KEEP THE ORIGINAL SOUL. REMOVE THE ORIGINAL LIMITATIONS.
Investigate the existing implementation, inspect the provided reference images, reconstruct the menu architecture, integrate the actual settings, build the project, and validate the finished result.
