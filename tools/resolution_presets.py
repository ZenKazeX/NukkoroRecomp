"""Resolution choices for the launcher (docs/RESOLUTION_AUDIT.md).

The presets and the family rule mirror runtime/lift/render/display_modes.cpp (test_resolution_presets.py checks that
the two tables are identical). A family is the commercial name of a shape - 3440x1440 is "21:9" although it is
2.389:1 - and the game always renders from the actual width / height.

display.cfg (written by the game's Options menu) is the player's only display store (docs/DISPLAY_SETTINGS_AUDIT.md
section 6): the launcher passes no display flag at all, so the game starts exactly as the player last left it. The
tables below are kept for --list-displays reports only; nothing offers them as choices.
"""
from __future__ import annotations

import ctypes
import json
import subprocess

FAMILIES = ("Auto", "4:3", "5:4", "16:9", "16:10", "21:9", "32:10", "32:9")
GAME_SETTINGS = "Game settings (display.cfg)"
DISPLAY_MODES = (GAME_SETTINGS,)
MAX_SIDE = 16384  # Direct3D 12's texture limit; the runtime refuses more
MIN_WIDTH, MIN_HEIGHT = 160, 120

PRESETS = (
    (1280, 720, "16:9"), (1366, 768, "16:9"), (1600, 900, "16:9"), (1920, 1080, "16:9"), (2560, 1440, "16:9"),
    (3200, 1800, "16:9"), (3840, 2160, "16:9"), (5120, 2880, "16:9"), (7680, 4320, "16:9"),
    (1280, 800, "16:10"), (1440, 900, "16:10"), (1680, 1050, "16:10"), (1920, 1200, "16:10"),
    (2560, 1600, "16:10"), (2880, 1800, "16:10"), (3840, 2400, "16:10"), (5120, 3200, "16:10"),
    (2560, 1080, "21:9"), (3440, 1440, "21:9"), (3840, 1600, "21:9"), (5120, 2160, "21:9"),
    (3840, 1200, "32:10"), (5120, 1600, "32:10"), (3840, 1080, "32:9"), (5120, 1440, "32:9"),
    (7680, 2160, "32:9"),
    (640, 480, "4:3"), (800, 600, "4:3"), (1024, 768, "4:3"), (1280, 960, "4:3"), (1600, 1200, "4:3"),
    (1280, 1024, "5:4"),
)

_NOMINAL = (("4:3", 4, 3), ("5:4", 5, 4), ("16:10", 16, 10), ("16:9", 16, 9), ("21:9", 21, 9),
            ("32:10", 32, 10), ("32:9", 32, 9))


def classify(width, height):
    """(family, exact) by the size's actual ratio - the rule of display_modes.cpp aspect_family."""
    if width <= 0 or height <= 0:
        return "other", False
    ratio = width / height
    if 2.30 <= ratio <= 2.45:
        return "21:9", width * 9 == height * 21
    name, w, h = min(_NOMINAL, key=lambda f: abs(ratio / (f[1] / f[2]) - 1.0))
    if abs(ratio / (w / h) - 1.0) > 0.01:
        return "other", False
    return name, width * h == height * w


def detect_displays(exe, timeout=10):
    """The engine's own DXGI report (`--list-displays`), or None for a build that predates it."""
    try:
        done = subprocess.run([exe, "--list-displays"], capture_output=True, text=True, timeout=timeout,
                              creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        data = json.loads(done.stdout)
        return data if isinstance(data, dict) and data.get("outputs") else None
    except (OSError, ValueError, subprocess.SubprocessError):
        return None


def primary_desktop():
    """The primary display's current mode (width, height, refresh Hz) from the display driver, for old builds."""
    class DevMode(ctypes.Structure):
        _fields_ = [("name", ctypes.c_wchar * 32), ("spec", ctypes.c_ushort), ("driver", ctypes.c_ushort),
                    ("size", ctypes.c_ushort), ("extra", ctypes.c_ushort), ("fields", ctypes.c_ulong),
                    ("union", ctypes.c_byte * 16), ("colour", ctypes.c_short), ("duplex", ctypes.c_short),
                    ("y_res", ctypes.c_short), ("tt", ctypes.c_short), ("collate", ctypes.c_short),
                    ("form", ctypes.c_wchar * 32), ("log_pixels", ctypes.c_ushort), ("bits", ctypes.c_ulong),
                    ("width", ctypes.c_ulong), ("height", ctypes.c_ulong), ("flags", ctypes.c_ulong),
                    ("frequency", ctypes.c_ulong)]
    try:
        mode = DevMode()
        mode.size = ctypes.sizeof(DevMode)
        if ctypes.windll.user32.EnumDisplaySettingsW(None, -1, ctypes.byref(mode)):
            return int(mode.width), int(mode.height), int(mode.frequency)
    except (AttributeError, OSError):
        pass
    return None


def primary_output(displays):
    """The primary output of a --list-displays report (the one fullscreen uses), else the first."""
    outputs = (displays or {}).get("outputs") or []
    for output in outputs:
        if output.get("primary"):
            return output
    return outputs[0] if outputs else None


def same_shape(width, height, screen):
    """True when width x height has exactly the screen's aspect (it fills it with no bar, not even one pixel)."""
    return screen is None or width * screen[1] == height * screen[0]


def choices(family, native, monitor_modes=(), fill_screen=False):
    """The resolution list for an aspect family: (label, (width, height) or None for native/custom).

    Native first (the monitor's desktop), then the established presets of the family (all families for Auto),
    then the monitor's own modes that are not presets, then Custom. fill_screen (borderless fullscreen, the user's
    choice 2026-10-26): only sizes of exactly the monitor's shape, so fullscreen never shows bars."""
    out = []
    if native:
        out.append(("Native (%dx%d)" % native[:2], None))
    screen = native[:2] if fill_screen and native else None
    seen = set()
    for w, h, f in PRESETS:
        if family in ("Auto", f) and same_shape(w, h, screen):
            label = "%dx%d" % (w, h) if family != "Auto" else "%dx%d  (%s)" % (w, h, f)
            out.append((label, (w, h)))
            seen.add((w, h))
    for w, h in sorted({(m["width"], m["height"]) for m in monitor_modes}):
        f, _ = classify(w, h)
        if (w, h) not in seen and family in ("Auto", f) and same_shape(w, h, screen):
            out.append(("%dx%d  (monitor%s)" % (w, h, "" if family != "Auto" else ", " + f), (w, h)))
            seen.add((w, h))
    out.append(("Custom", None))
    return out


def validate(width, height):
    """None when the size is renderable, else the reason."""
    if width < MIN_WIDTH or height < MIN_HEIGHT:
        return "must be at least %dx%d" % (MIN_WIDTH, MIN_HEIGHT)
    if width > MAX_SIDE or height > MAX_SIDE:
        return "exceeds Direct3D 12's %d-pixel texture limit" % MAX_SIDE
    return None


def command_flags(display_mode, size):
    """The runtime's display flags: none. The game reads display.cfg, which its Options menu writes."""
    return []
