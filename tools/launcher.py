#!/usr/bin/env python3
"""GekkoForge launcher - select a stage, click go.

Stdlib only (tkinter), no install step. Run it from anywhere:

    python tools/launcher.py

WHY IT DISCOVERS INSTEAD OF HARDCODING
The stage list comes from `ShadowTheHedgehog.exe --list-stages`, parsed at
startup, and the "bootable" flag comes from the same place. Nothing about the
stage table is baked into this file, so a change to the guest table or to which
stages `--boot-stage` accepts shows up here without editing anything. The same
applies to the executable list: every ShadowTheHedgehog*.exe next to the disc
root is offered, so archived builds can be A/B'd against the current one.
"""
import ctypes
import decimal
import glob
import json
import math
import os
import queue
import re
import shlex
import subprocess
import sys
import threading
import time
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

import resolution_presets as rp

HERE = os.path.dirname(os.path.abspath(__file__))
# tools/ lives inside source/; playable files are in the sibling game/ directory.
DISC_ROOT = os.path.abspath(os.path.join(HERE, "..", "..", "game"))
CONFIG = os.path.join(HERE, ".launcher.json")

STAGE_RE = re.compile(r"^\s*(stg\d+)\s+(\S+)\s+(bootable|not bootable)(.*)$")

# The game is a CONSOLE-subsystem program. Started from pythonw - which is how
# GekkoForge.bat runs this launcher - every console child gets a console window
# of its own: an empty one beside the game for the whole session (closing it
# kills the game) and a flash on every stage scan. Output is captured through
# pipes, so that window has nothing to show.
NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)

# The runtime's output is MIXED-encoding, line by line. Paths come from
# std::filesystem::path::string(), which MSVC converts with the ANSI code page;
# the audio device name is converted to UTF-8 before it is printed. Decoding
# everything as UTF-8 turned each non-ASCII path character into U+FFFD -
# including the one in a profile path, which then no longer matched the
# scrubber and left the rest of the account name in the log - and decoding
# everything as ANSI would mangle the device name instead. So each line is
# tried as UTF-8 first: ANSI text with accented letters is almost never valid
# UTF-8, and plain ASCII reads the same either way.
ANSI_ENCODING = "mbcs" if os.name == "nt" else "latin-1"


def decode_line(raw):
    try:
        return raw.decode("utf-8")
    except UnicodeDecodeError:
        return raw.decode(ANSI_ENCODING, errors="replace")


def decode_output(data):
    return "\n".join(decode_line(line) for line in (data or b"").splitlines())

# Where the runtime keeps per-user state. It resolves every one of these from
# the EXECUTABLE's directory, which is the disc root for every build deployed
# there - but not for the profiling build, which runs from its own tree.
ROOT_CARD = os.path.join(DISC_ROOT, "save-data", "8P-GUPP-SHADOWTHEHEDGEHOG.pc.raw")
ROOT_DISPLAY_CFG = os.path.join(DISC_ROOT, "display.cfg")
ROOT_PAD_CFG = os.path.join(DISC_ROOT, "pad.cfg")
ROOT_STATE_SLOT = os.path.join(DISC_ROOT, "save-data", "slot0.gekkostate")

# The effect layers (runtime/lift/render/effect_layers.cpp; docs/EFFECTS_INVENTORY.md "Layer switches"), in the
# game's order: --effects-off takes the names, --effects-solo one of them. Keep in step with kNames there.
EFFECT_LAYERS = (
    ("glare", "Glare"), ("lens-flare", "Lens flare"), ("motion-blur", "Motion blur"),
    ("heat-haze", "Heat haze"), ("refraction", "Refraction"), ("screen-colour", "Screen colour"),
    ("screen-fade", "Screen fade"), ("particles", "Particles"), ("particles-3d", "3D particles"),
    ("effect-models", "Effect models"), ("rays", "Light rays"), ("trails", "Trails"),
    ("shadows", "Shadows"), ("land-unfogged", "Land unfogged"), ("land-fogged", "Land fogged"),
    ("land-alpha", "Land alpha"), ("land-additive", "Land additive"),
    ("land-darken", "Land darken"), ("land-glow", "Land glow"),
    ("lighting", "Lighting"), ("baked-light", "Baked light"), ("fog", "Fog"),
    ("environment-maps", "Env maps"), ("dual-textures", "Dual textures"), ("appear-fade", "Fade-in"),
)


def parse_count(text):
    """A positive whole number from what a person types, or None.

    Accepts 15e9, 15_000_000_000 and "15 000 000 000" (any space, including
    the non-breaking ones a locale inserts). This matters because the runtime
    reads --budget with std::stoull, which stops at the first character it
    cannot use: "15e9" was a budget of FIFTEEN instructions, a run that ends
    before it starts. A comma is REJECTED rather than dropped: it is a decimal
    mark in many locales, and "1,5e10" read as a thousands separator would be
    ten times too large.
    """
    cleaned = re.sub(r"[\s_]", "", text or "")
    if not cleaned:
        return None
    try:
        value = decimal.Decimal(cleaned)
    except decimal.InvalidOperation:
        return None
    if not value.is_finite() or value <= 0 or value != value.to_integral_value():
        return None
    return int(value)


def split_flags(text):
    """Splits the free-form flags box the way a Windows command line would.

    str.split() broke any quoted argument containing a space, and shlex in POSIX
    mode would eat the backslashes of a Windows path. Non-POSIX mode keeps both
    and leaves the quotes on the token, so they are removed here.
    """
    tokens = []
    for token in shlex.split(text or "", posix=False):
        if len(token) >= 2 and token[0] == token[-1] and token[0] in "\"'":
            token = token[1:-1]
        tokens.append(token)
    return tokens


def clean_path(text):
    """A path as typed or pasted: Windows 11 "Copy as path" wraps it in quotes."""
    return (text or "").strip().strip('"').strip()




def outside_disc_root(exe):
    """True for a build that does not run from the disc root (the profiling build)."""
    here = os.path.normcase(os.path.dirname(os.path.abspath(exe)))
    return here != os.path.normcase(DISC_ROOT)

# ---- retail stage titles ---------------------------------------------------
#
# SOURCE: Sonic Retro, "Shadow the Hedgehog (game)/Technical information/Stage
# ID list". Transcribed verbatim, including the parenthetical context that
# distinguishes repeated boss encounters.
#
# This is the one table here that is NOT read off the disc, and it cannot be:
# stage titles are drawn as TEXTURES (StageTitle.gncp), not stored as strings,
# so there is no id->name mapping in main.dol or in files/ to extract. The ids
# and the internal names beside them DO come from the executable itself, via
# --list-stages.
#
# Four entries are independently corroborated by artifacts in this repository,
# which is what anchors the numbering: stg0100 (save-data/westopolis.gekkostate
# and "the Westopolis route" in the docs), stg0600 (gun-fortress.gekkostate),
# stg0602 (Lava Shelter) and stg0604 (Final Haunt). All four agree with the
# source.
#
# The original59-record table includes0618 and0710. Devil Doom0710 is absent
# from the Select Mode grids; the isolated research player adds its verified
# direct boss boot without changing the deployed executable.
STAGE_TITLES = {
    "stg0100": "Westopolis",
    "stg0200": "Digital Circuit",
    "stg0201": "Glyphic Canyon",
    "stg0202": "Lethal Highway",
    "stg0210": "Black Bull (Lethal Highway)",
    "stg0300": "Cryptic Castle",
    "stg0301": "Prison Island",
    "stg0302": "Circus Park",
    "stg0310": "Egg Breaker (Cryptic Castle)",
    "stg0400": "Central City",
    "stg0401": "The Doom",
    "stg0402": "Sky Troops",
    "stg0403": "Mad Matrix",
    "stg0404": "Death Ruins",
    "stg0410": "Heavy Dog",
    "stg0411": "Egg Breaker (Mad Matrix)",
    "stg0412": "Black Bull (Death Ruins)",
    "stg0500": "The ARK",
    "stg0501": "Air Fleet",
    "stg0502": "Iron Jungle",
    "stg0503": "Space Gadget",
    "stg0504": "Lost Impact",
    "stg0510": "Blue Falcon",
    "stg0511": "Egg Breaker (Iron Jungle)",
    "stg0600": "GUN Fortress",
    "stg0601": "Black Comet",
    "stg0602": "Lava Shelter",
    "stg0603": "Cosmic Fall",
    "stg0604": "Final Haunt",
    "stg0610": "Black Doom (GUN Fortress)",
    "stg0611": "Sonic & Diablon (GUN Fortress)",
    "stg0612": "Egg Dealer (Black Comet)",
    "stg0613": "Sonic & Diablon (Black Comet)",
    "stg0614": "Egg Dealer (Lava Shelter)",
    "stg0615": "Egg Dealer (Cosmic Fall)",
    "stg0616": "Black Doom (Cosmic Fall)",
    "stg0617": "Black Doom (Final Haunt)",
    "stg0618": "Sonic & Diablon (Final Haunt)",
    "stg0700": "The Last Way",
    "stg0710": "Devil Doom",
    # 0000-0005 are debug stages; the executable's own names (Practice, EVENT)
    # are more specific than the source's grouping, so they are left to it.
}


def find_executables(root):
    out = []
    try:
        for name in sorted(os.listdir(root)):
            low = name.lower()
            if low.startswith("shadowthehedgehog") and low.endswith(".exe"):
                out.append(os.path.join(root, name))
    except OSError:
        pass
    # The plain name first: that is the one a build deploys over.
    out.sort(key=lambda p: (os.path.basename(p).lower() != "shadowthehedgehog.exe",
                            os.path.basename(p).lower()))
    return out




_HELP_CACHE = {}


def exe_supports(exe, flag):
    """True when the build's --help names `flag` (cached per file and modification time); True when unknown."""
    try:
        key = (exe, os.path.getmtime(exe))
    except OSError:
        return True
    if key not in _HELP_CACHE:
        try:
            done = subprocess.run([exe, "--help"], capture_output=True, text=True, timeout=10,
                                  creationflags=NO_WINDOW, errors="replace")
            _HELP_CACHE[key] = done.stdout + done.stderr
        except (OSError, subprocess.SubprocessError):
            _HELP_CACHE[key] = None
    text = _HELP_CACHE[key]
    return True if text is None else flag in text


def list_stages(exe, root):
    """Ask the executable. Returns ([(id, name, bootable)], error or None)."""
    # A private TRACY_PORT keeps the scan invisible to a viewer waiting on the
    # default port. The profiling build is instrumented, so without it a
    # waiting viewer attaches to the SCAN instead of the run that follows. It is
    # worse than a wasted capture: a Tracy client with a viewer attached does
    # not exit until the viewer releases it, so the scan stays alive, the stage
    # list times out and the executable stays locked. With a port of its own the
    # client also stops searching for the default one (TracyProfiler.cpp).
    env = dict(os.environ, TRACY_PORT="8199")
    try:
        proc = subprocess.run([exe, "--game-data", root, "--list-stages"],
                              capture_output=True, timeout=180, env=env,
                              creationflags=NO_WINDOW)
    except Exception as exc:  # noqa: BLE001 - surfaced in the UI
        return [], str(exc)
    out, err = decode_output(proc.stdout), decode_output(proc.stderr)
    stages = []
    for line in out.splitlines():
        m = STAGE_RE.match(line)
        if m:
            stages.append((m.group(1), m.group(2), m.group(3) == "bootable"))
    if not stages:
        return [], out + "\n" + err
    return stages, None


# ---- host controller, read directly so binding can be press-to-bind --------
#
# XInput lives in one of three DLLs depending on what is installed. The runtime
# resolves the same way (runtime/src/gekko/xinput_pad.cpp), so the launcher sees
# exactly the controller the game will see.
XINPUT_BUTTONS = [
    ("DPAD_UP", 0x0001), ("DPAD_DOWN", 0x0002), ("DPAD_LEFT", 0x0004),
    ("DPAD_RIGHT", 0x0008), ("START", 0x0010), ("BACK", 0x0020),
    ("LEFT_THUMB", 0x0040), ("RIGHT_THUMB", 0x0080),
    ("LEFT_SHOULDER", 0x0100), ("RIGHT_SHOULDER", 0x0200),
    ("A", 0x1000), ("B", 0x2000), ("X", 0x4000), ("Y", 0x8000),
]
MASK_TO_NAME = {m: n for n, m in XINPUT_BUTTONS}
NAME_TO_MASK = {n: m for n, m in XINPUT_BUTTONS}

# The recovered GameCube pad, in the order a player looks for them.
GC_BUTTONS = [
    ("a", "A"), ("b", "B"), ("x", "X"), ("y", "Y"), ("start", "Start"),
    ("l", "L"), ("r", "R"), ("z", "Z"),
    ("dpad_up", "D-Pad Up"), ("dpad_down", "D-Pad Down"),
    ("dpad_left", "D-Pad Left"), ("dpad_right", "D-Pad Right"),
]
# The buttons the game's prompts show (runtime gekko/button_glyphs.h; pad.cfg button_style).
BUTTON_STYLES = ("auto", "original", "xbox", "playstation", "switch")
PAD_DEFAULTS = {
    "a": 0x1000, "b": 0x2000, "x": 0x4000, "y": 0x8000, "start": 0x0010,
    "l": 0x0100, "r": 0x0000, "z": 0x0200,
    "dpad_up": 0x0001, "dpad_down": 0x0002, "dpad_left": 0x0004, "dpad_right": 0x0008,
}


class _XInputGamepad(ctypes.Structure):
    _fields_ = [("wButtons", ctypes.c_ushort), ("bLeftTrigger", ctypes.c_ubyte),
                ("bRightTrigger", ctypes.c_ubyte), ("sThumbLX", ctypes.c_short),
                ("sThumbLY", ctypes.c_short), ("sThumbRX", ctypes.c_short),
                ("sThumbRY", ctypes.c_short)]


class _XInputState(ctypes.Structure):
    _fields_ = [("dwPacketNumber", ctypes.c_uint), ("Gamepad", _XInputGamepad)]


def _load_xinput():
    for name in ("xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"):
        try:
            return ctypes.WinDLL(name)
        except Exception:  # noqa: BLE001
            continue
    return None


_XINPUT = _load_xinput()


# A path under a user profile identifies the person running this, and the log
# is the part of the launcher meant to be shown to other people. Everything
# printed goes through log_line, which calls this, so one rule covers the
# command echo, the runtime's own output and the live-stat lines alike.
_USER_PATH = re.compile(r"(?i)\b[a-z]:[\\/]users[\\/][^\\/\s]+")
_HOME_PATH = re.compile(r"(?i)/(?:home|users)/[^/\s]+")


# The regexes below stop at whitespace, so an account name containing a space
# would leave its second word behind. The exact profile directory is known, so
# it is replaced literally FIRST and that case cannot arise. Every spelling of
# it that can reach the log is covered: the long form and the 8.3 short form
# (the runtime and the temp directory use either), each with backslashes and
# with forward slashes (a Tk file dialog returns "C:/Users/...").
def _short_path(path):
    if os.name != "nt":
        return path
    try:
        buf = ctypes.create_unicode_buffer(1024)
        if ctypes.windll.kernel32.GetShortPathNameW(path, buf, len(buf)):
            return buf.value
    except Exception:  # noqa: BLE001
        pass
    return path


def _profile_directories():
    seen = []
    for base in (os.path.expanduser("~"), os.environ.get("USERPROFILE", "")):
        base = (base or "").rstrip("\\/")
        if not base:
            continue
        for form in (base, _short_path(base)):
            for spelling in (form, form.replace("\\", "/")):
                if spelling.lower() not in [p.lower() for p in seen]:
                    seen.append(spelling)
    # Longest first: a prefix must never be substituted before the longer path
    # that contains it.
    return sorted(seen, key=len, reverse=True)


_PROFILE_DIRS = _profile_directories()


def scrub_paths(text):
    """Replaces a user-profile path prefix with <home>, keeping the tail."""
    for directory in _PROFILE_DIRS:
        if directory.lower() in text.lower():
            # Case-insensitive replace without a regex, so a directory
            # containing regex metacharacters is still handled literally.
            low, out, at = text.lower(), [], 0
            needle = directory.lower()
            while True:
                hit = low.find(needle, at)
                if hit < 0:
                    out.append(text[at:])
                    break
                out.append(text[at:hit])
                out.append("<home>")
                at = hit + len(needle)
            text = "".join(out)
    text = _USER_PATH.sub("<home>", text)
    return _HOME_PATH.sub("<home>", text)


# HOST WINDOW PLACEMENT.
#
# The game creates its own top-level window and lets Windows choose where, so
# it lands centred - directly on top of this launcher and its log. The window
# is a host artefact, not game state: nothing the guest does depends on where
# its frames are composited, so moving it changes nothing that can be observed
# from inside the game. It is therefore the launcher's business, and doing it
# here means it also works with archived builds compiled before this existed.
_GAME_WINDOW_CLASS = "ShadowTheHedgehogNative"
_ENUM_PROC = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
_USER32 = None


class _RECT(ctypes.Structure):
    _fields_ = [("left", ctypes.c_long), ("top", ctypes.c_long),
                ("right", ctypes.c_long), ("bottom", ctypes.c_long)]


class _POINT(ctypes.Structure):
    _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]


class _MONITORINFO(ctypes.Structure):
    _fields_ = [("cbSize", ctypes.c_ulong), ("rcMonitor", _RECT),
                ("rcWork", _RECT), ("dwFlags", ctypes.c_ulong)]


def _user32():
    """user32 with every entry point below declared.

    HWND is a pointer, and ctypes defaults an undeclared integer argument to
    c_int - which silently truncates a 64-bit handle. Declaring the signatures
    is what makes this correct rather than merely usually correct.
    """
    global _USER32
    if _USER32 is not None or os.name != "nt":
        return _USER32
    lib = ctypes.windll.user32
    c_p, c_i, c_u = ctypes.c_void_p, ctypes.c_int, ctypes.c_uint
    lib.EnumWindows.argtypes = [_ENUM_PROC, c_p]
    lib.IsWindowVisible.argtypes = [c_p]
    lib.GetWindowThreadProcessId.argtypes = [c_p, ctypes.POINTER(ctypes.c_ulong)]
    lib.GetClassNameW.argtypes = [c_p, ctypes.c_wchar_p, c_i]
    lib.GetWindowRect.argtypes = [c_p, ctypes.POINTER(_RECT)]
    lib.GetWindowRect.restype = ctypes.c_bool
    lib.SetWindowPos.argtypes = [c_p, c_p, c_i, c_i, c_i, c_i, c_u]
    lib.SetWindowPos.restype = ctypes.c_bool
    lib.SystemParametersInfoW.argtypes = [c_u, c_u, ctypes.POINTER(_RECT), c_u]
    lib.GetWindowTextW.argtypes = [c_p, ctypes.c_wchar_p, c_i]
    lib.PostMessageW.argtypes = [c_p, c_u, ctypes.c_size_t, ctypes.c_ssize_t]
    lib.PostMessageW.restype = ctypes.c_bool
    lib.MonitorFromPoint.argtypes = [_POINT, c_u]
    lib.MonitorFromPoint.restype = c_p
    lib.GetMonitorInfoW.argtypes = [c_p, ctypes.POINTER(_MONITORINFO)]
    lib.GetMonitorInfoW.restype = ctypes.c_bool
    _USER32 = lib
    return lib


def find_game_window(pid):
    """The visible top-level window of class _GAME_WINDOW_CLASS owned by pid."""
    user32 = _user32()
    if user32 is None:
        return None
    found = []
    buf = ctypes.create_unicode_buffer(128)
    owner = ctypes.c_ulong()

    def visit(hwnd, _lparam):
        if not user32.IsWindowVisible(hwnd):
            return True
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value != pid:
            return True
        user32.GetClassNameW(hwnd, buf, len(buf))
        if buf.value == _GAME_WINDOW_CLASS:
            found.append(hwnd)
            return False
        return True

    try:
        user32.EnumWindows(_ENUM_PROC(visit), None)
    except Exception:  # noqa: BLE001
        return None
    return found[0] if found else None


def work_area(x=None, y=None):
    """The work area (desktop minus taskbar) of the monitor nearest (x, y).

    SPI_GETWORKAREA describes the PRIMARY monitor only, so a position
    remembered on a second monitor was clamped back onto the first one. With no
    point, the primary monitor's work area is returned.
    """
    user32 = _user32()
    if user32 is not None and x is not None and y is not None:
        info = _MONITORINFO()
        info.cbSize = ctypes.sizeof(info)
        # MONITOR_DEFAULTTONEAREST = 2: a point on no monitor at all - one that
        # has since been unplugged - resolves to the closest attached monitor.
        monitor = user32.MonitorFromPoint(_POINT(int(x), int(y)), 2)
        if monitor and user32.GetMonitorInfoW(monitor, ctypes.byref(info)):
            work = info.rcWork
            return work.left, work.top, work.right, work.bottom
    rect = _RECT()
    # SPI_GETWORKAREA = 0x0030
    if user32 is not None and user32.SystemParametersInfoW(0x0030, 0, ctypes.byref(rect), 0):
        return rect.left, rect.top, rect.right, rect.bottom
    return 0, 0, 1920, 1080


def window_rect(hwnd):
    user32 = _user32()
    rect = _RECT()
    if user32 is not None and user32.GetWindowRect(hwnd, ctypes.byref(rect)):
        return rect.left, rect.top, rect.right, rect.bottom
    return None


def move_window(hwnd, x, y):
    # SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE - position only. NOACTIVATE
    # matters: the game has just taken the foreground for input, and stealing
    # it back here would send the player's first keypresses to the launcher.
    user32 = _user32()
    if user32 is not None:
        user32.SetWindowPos(hwnd, None, int(x), int(y), 0, 0, 0x0001 | 0x0004 | 0x0010)


def post_close(hwnd):
    """Asks a window to close, exactly as its close button does (WM_CLOSE)."""
    user32 = _user32()
    return bool(user32 is not None and user32.PostMessageW(hwnd, 0x0010, 0, 0))




def read_pad(index=0):
    """(buttons, lt, rt, lx, ly, rx, ry) or None when nothing is connected."""
    if _XINPUT is None:
        return None
    state = _XInputState()
    if _XINPUT.XInputGetState(index, ctypes.byref(state)) != 0:
        return None
    g = state.Gamepad
    return (g.wButtons, g.bLeftTrigger, g.bRightTrigger,
            g.sThumbLX, g.sThumbLY, g.sThumbRX, g.sThumbRY)


class Launcher(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("GekkoForge - Shadow the Hedgehog")
        self.geometry("1500x780")
        self.minsize(1150, 640)

        self.cfg = self._load_cfg()
        self.exes = find_executables(DISC_ROOT)
        self.stages = []
        self.selected_stage = tk.StringVar(value="")
        self.proc = None
        # Worker threads never touch Tk. They post (callable, args) here and the
        # UI thread runs them - see _pump.
        self._inbox = queue.SimpleQueue()
        self._scan_generation = 0
        self._run_serial = 0

        self._sweep_telemetry_files()
        self._build()
        self.protocol("WM_DELETE_WINDOW", self._on_close)
        self.after(100, self._pump)
        self.after(100, self.refresh_stages)

    # ---- the one UI pump ---------------------------------------------------
    #
    # Every thread that has something for the window posts it here, and this
    # loop - started once, alive for the life of the window - runs it on the Tk
    # thread. It replaces a log pump that was started per launch and stopped by
    # a grace timer (a quick relaunch left two running), a list swap that could
    # drop a line appended at the wrong instant, and after() calls made from
    # worker threads, which tkinter only tolerates while its main loop runs.
    def post(self, fn, *args):
        """Runs fn(*args) on the Tk thread. Safe to call from any thread."""
        self._inbox.put((fn, args))

    def post_log(self, text, tag=None):
        self._inbox.put((self.log_line, (text, tag)))

    def _pump(self):
        # Bounded per tick, so a burst of output cannot freeze the window.
        for _ in range(2000):
            try:
                fn, args = self._inbox.get_nowait()
            except queue.Empty:
                break
            try:
                fn(*args)
            except Exception as exc:  # noqa: BLE001 - reported, never swallowed
                try:
                    self.log_line("launcher: %s failed: %s"
                                  % (getattr(fn, "__name__", "callback"), exc), "error")
                except Exception:  # noqa: BLE001
                    pass
        self.after(100, self._pump)

    def _on_close(self):
        """Closing the window must not silently orphan a running game."""
        proc = self.proc
        if proc is not None and proc.poll() is None:
            answer = messagebox.askyesnocancel(
                "A run is still going",
                "Stop the running game before closing the launcher?\n\n"
                "Yes - stop it (it saves its exit report), then close\n"
                "No - leave it running and close\n"
                "Cancel - keep the launcher open")
            if answer is None:
                return
            if answer:
                self._close_run(proc, wait=8.0)
        self._remove_file(self._telemetry_path_for(self._run_serial))
        self.destroy()

    # ---- persistence -------------------------------------------------------
    def _load_cfg(self):
        try:
            with open(CONFIG, encoding="utf-8") as fh:
                data = json.load(fh)
        except Exception:  # noqa: BLE001 - absent or corrupt: start from defaults
            return {}
        return data if isinstance(data, dict) else {}

    def _save_cfg(self):
        data = {
            "exe": self.exe_var.get(),
            "display_mode": self.mode_var.get(),
            "aspect_family": self.family_var.get(),
            "resolution": self.resolution_value,
            "custom_width": self.width_var.get(),
            "custom_height": self.height_var.get(),
            "renderer": self.renderer_var.get(),
            "draw_distance": self.draw_distance_var.get(),
            "effects_off": [name for name, var in self.effect_vars.items() if not var.get()],
            "effects_solo": self.effect_solo_var.get(),
            "effects_isolate": self.effect_isolate_var.get(),
            "audio": self.audio_var.get(),
            "xinput": self.xinput_var.get(),
            "headless": self.headless_var.get(),
            "fps_counter": self.fps_var.get(),
            "budget": self.budget_var.get(),
            "state_file": self.state_var.get(),
            "load_state": self.load_state_var.get(),
            "save_state": self.save_state_var.get(),
            "ui_test": self.uitest_var.get(),
            "extra": self.extra_var.get(),
            "telemetry": self.live_telemetry.get(),
            "placement": self.placement_var.get(),
            "place_x": self.place_x.get(),
            "place_y": self.place_y.get(),
        }
        try:
            with open(CONFIG, "w", encoding="utf-8") as fh:
                json.dump(data, fh, indent=1)
        except Exception:  # noqa: BLE001
            pass

    def _effects_all_on(self):
        for var in self.effect_vars.values():
            var.set(True)
        self.effect_solo_var.set("")
        self.effect_isolate_var.set(False)

    def _snap_draw_distance(self):
        # Half steps: 1, 1.5 ... 8.
        value = min(8.0, max(1.0, round(float(self.draw_distance_var.get()) * 2.0) / 2.0))
        if value != self.draw_distance_var.get():
            self.draw_distance_var.set(value)
        self.draw_distance_label.configure(text="x%g" % value)

    def _get(self, key, default):
        v = self.cfg.get(key, default)
        return default if v is None else v

    # ---- layout ------------------------------------------------------------
    def _build(self):
        root = ttk.Frame(self, padding=8)
        root.pack(fill="both", expand=True)

        # --- top: which executable ---
        top = ttk.LabelFrame(root, text="Build", padding=6)
        top.pack(fill="x")
        # A remembered build that has since been deleted or renamed would leave
        # the box showing a path that cannot run; fall back to one that can.
        saved = self._get("exe", "")
        initial = saved if saved and os.path.exists(saved) else (self.exes[0] if self.exes else "")
        self.exe_var = tk.StringVar(value=initial)
        self.exe_box = ttk.Combobox(top, textvariable=self.exe_var, values=self.exes,
                                    state="readonly", width=90)
        self.exe_box.pack(side="left", padx=(0, 6))
        self.exe_box.bind("<<ComboboxSelected>>", lambda _e: self.refresh_stages())
        ttk.Button(top, text="Rescan", command=self.rescan).pack(side="left")
        self.status = ttk.Label(top, text="")
        self.status.pack(side="left", padx=8)

        self.tabs = ttk.Notebook(root)
        self.tabs.pack(fill="both", expand=True, pady=6)

        mid = ttk.Frame(self.tabs, padding=4)
        self.tabs.add(mid, text="  Play  ")
        pad_tab = ttk.Frame(self.tabs, padding=4)
        self.tabs.add(pad_tab, text="  Controller  ")
        self._build_pad_tab(pad_tab)

        # --- left: stages ---
        left = ttk.LabelFrame(mid, text="Stage  (double-click to launch)", padding=6)
        left.pack(side="left", fill="both", expand=True)

        filt = ttk.Frame(left)
        filt.pack(fill="x", pady=(0, 4))
        ttk.Label(filt, text="Filter").pack(side="left")
        self.filter_var = tk.StringVar()
        e = ttk.Entry(filt, textvariable=self.filter_var)
        e.pack(side="left", fill="x", expand=True, padx=4)
        e.bind("<KeyRelease>", lambda _e: self.apply_filter())
        self.only_bootable = tk.BooleanVar(value=True)
        ttk.Checkbutton(filt, text="bootable only", variable=self.only_bootable,
                        command=self.apply_filter).pack(side="left")

        cols = ("id", "title", "name", "boot")
        self.tree = ttk.Treeview(left, columns=cols, show="headings", selectmode="browse")
        for c, w, t in (("id", 105, "ID"), ("title", 170, "Title"),
                        ("name", 190, "Asset name"), ("boot", 80, "Bootable")):
            self.tree.heading(c, text=t)
            self.tree.column(c, width=w, anchor="w")
        self.tree.pack(fill="both", expand=True)
        self.tree.bind("<Double-1>", lambda _e: self.launch())
        self.tree.bind("<<TreeviewSelect>>", self._on_select)
        # Escape deselects: there was no way back to "no stage" once one was chosen.
        self.tree.bind("<Escape>", self._clear_selection)
        sb = ttk.Scrollbar(left, orient="vertical", command=self.tree.yview)
        self.tree.configure(yscrollcommand=sb.set)
        sb.place(relx=1.0, rely=0, relheight=1.0, anchor="ne")

        # --- right: options ---
        right = ttk.Frame(mid)
        right.pack(side="left", fill="y", padx=(8, 0))

        # Display (docs/RESOLUTION_AUDIT.md): a display mode, an aspect family that filters the resolutions, and a
        # resolution - the monitor's native desktop, an established preset, one of the monitor's own modes or a
        # custom size. The chosen size is both the window/fullscreen picture and the exact render size, so the
        # game rasterizes its 3D scene at exactly those pixels; a family is only a filter (the actual width /
        # height is what the game renders with). No multiplier exists.
        disp = ttk.LabelFrame(right, text="Display", padding=6)
        disp.pack(fill="x")
        self.displays = None  # the selected build's --list-displays report (None: an older build)
        old_mode = "Borderless fullscreen" if self._get("fullscreen", False) else "Windowed"
        self.mode_var = tk.StringVar(value=self._get("display_mode", old_mode))
        if self.mode_var.get() not in rp.DISPLAY_MODES:
            self.mode_var.set("Windowed")
        ttk.Label(disp, text="Display mode").grid(row=0, column=0, sticky="w")
        mode_box = ttk.Combobox(disp, textvariable=self.mode_var, width=24, state="readonly",
                                values=rp.DISPLAY_MODES)
        mode_box.grid(row=0, column=1, sticky="w")
        mode_box.bind("<<ComboboxSelected>>", lambda _e: self._fill_resolutions())
        self.family_var = tk.StringVar(value=self._get("aspect_family", "Auto"))
        if self.family_var.get() not in rp.FAMILIES:
            self.family_var.set("Auto")
        ttk.Label(disp, text="Aspect ratio").grid(row=1, column=0, sticky="w")
        self.family_box = ttk.Combobox(disp, textvariable=self.family_var, width=24, state="readonly",
                                       values=rp.FAMILIES)
        self.family_box.grid(row=1, column=1, sticky="w")
        self.family_box.bind("<<ComboboxSelected>>", lambda _e: self._fill_resolutions())
        # Stored as "native", "custom" or "WxH"; an old configuration's width x height carries over.
        old = "%sx%s" % (self._get("width", ""), self._get("height", "")) if self._get("width", "") else "native"
        self.resolution_value = self._get("resolution", old)
        self.resolution_var = tk.StringVar()
        ttk.Label(disp, text="Resolution").grid(row=2, column=0, sticky="w")
        self.resolution_box = ttk.Combobox(disp, textvariable=self.resolution_var, width=24, state="readonly")
        self.resolution_box.grid(row=2, column=1, sticky="w")
        self.resolution_box.bind("<<ComboboxSelected>>", lambda _e: self._resolution_picked())
        custom = ttk.Frame(disp)
        custom.grid(row=3, column=1, sticky="w")
        self.width_var = tk.StringVar(value=str(self._get("custom_width", "1920")))
        self.height_var = tk.StringVar(value=str(self._get("custom_height", "1080")))
        self.custom_w = ttk.Entry(custom, textvariable=self.width_var, width=7)
        self.custom_w.pack(side="left")
        ttk.Label(custom, text="x").pack(side="left", padx=2)
        self.custom_h = ttk.Entry(custom, textvariable=self.height_var, width=7)
        self.custom_h.pack(side="left")
        self.monitor_label = ttk.Label(disp, text="", wraplength=280, foreground="#555")
        self.monitor_label.grid(row=4, column=0, columnspan=2, sticky="w", pady=(4, 0))
        self._fill_resolutions()
        # "auto" passes no flag, so each build uses its own default renderer.
        # Always emitting --renderer made every build from before 2026-09-21 -
        # which does not know the flag - impossible to launch, although its
        # stage list loaded fine. A saved "native" was only ever the old
        # default, and is exactly what "auto" gives on every build that has a
        # native renderer.
        renderer = self._get("renderer", "auto")
        self.renderer_var = tk.StringVar(value="auto" if renderer == "native" else renderer)
        ttk.Label(disp, text="Renderer").grid(row=5, column=0, sticky="w")
        ttk.Combobox(disp, textvariable=self.renderer_var, width=12, state="readonly",
                     values=["auto", "native", "legacy"]).grid(row=5, column=1, sticky="w")
        # Object draw distance (docs/DRAW_DISTANCE.md): the native far-object proxies, render only. 1 = the original
        # game, 8 = the default (to a pixel). Passed only when it is not 8, so builds without the flag still launch.
        self.draw_distance_var = tk.DoubleVar(value=float(self._get("draw_distance", 8.0)))
        ttk.Label(disp, text="Draw distance").grid(row=6, column=0, sticky="w")
        ddf = ttk.Frame(disp)
        ddf.grid(row=6, column=1, sticky="w")
        self.draw_distance_label = ttk.Label(ddf, width=5)
        ttk.Scale(ddf, from_=1.0, to=8.0, length=150, variable=self.draw_distance_var,
                  command=lambda _v: self._snap_draw_distance()).pack(side="left")
        self.draw_distance_label.pack(side="left", padx=4)
        self._snap_draw_distance()

        self._build_placement(right)

        runf = ttk.LabelFrame(right, text="Run", padding=6)
        runf.pack(fill="x", pady=(6, 0))
        self.audio_var = tk.BooleanVar(value=bool(self._get("audio", True)))
        ttk.Checkbutton(runf, text="Audio", variable=self.audio_var).pack(anchor="w")
        self.xinput_var = tk.BooleanVar(value=bool(self._get("xinput", True)))
        ttk.Checkbutton(runf, text="Controller (XInput)", variable=self.xinput_var).pack(anchor="w")
        self.headless_var = tk.BooleanVar(value=bool(self._get("headless", False)))
        ttk.Checkbutton(runf, text="Headless (no window)", variable=self.headless_var).pack(anchor="w")
        self.fps_var = tk.BooleanVar(value=bool(self._get("fps_counter", False)))
        ttk.Checkbutton(runf, text="FPS counter (Num * in game)", variable=self.fps_var).pack(anchor="w")
        bf = ttk.Frame(runf)
        bf.pack(fill="x", pady=(4, 0))
        ttk.Label(bf, text="Budget").pack(side="left")
        self.budget_var = tk.StringVar(value=self._get("budget", ""))
        ttk.Entry(bf, textvariable=self.budget_var, width=14).pack(side="left", padx=4)
        ttk.Label(bf, text="(blank = unlimited)").pack(side="left")

        statef = ttk.LabelFrame(right, text="Savestate", padding=6)
        statef.pack(fill="x", pady=(6, 0))
        self.state_var = tk.StringVar(value=self._get("state_file", ""))
        sfr = ttk.Frame(statef)
        sfr.pack(fill="x")
        ttk.Entry(sfr, textvariable=self.state_var, width=26).pack(side="left")
        ttk.Button(sfr, text="...", width=3, command=self.pick_state).pack(side="left", padx=2)
        self.load_state_var = tk.BooleanVar(value=bool(self._get("load_state", False)))
        ttk.Checkbutton(statef, text="Load it at start", variable=self.load_state_var).pack(anchor="w")
        self.save_state_var = tk.BooleanVar(value=bool(self._get("save_state", False)))
        ttk.Checkbutton(statef, text="Save one at exit", variable=self.save_state_var).pack(anchor="w")
        ttk.Label(statef, text="Stage selection needs a cold boot:\nit cannot combine with load-state.",
                  foreground="#804000").pack(anchor="w", pady=(4, 0))

        uif = ttk.LabelFrame(right, text="Scripted input (needs a savestate)", padding=6)
        uif.pack(fill="x", pady=(6, 0))
        self.uitest_var = tk.StringVar(value=self._get("ui_test", ""))
        ttk.Combobox(uif, textvariable=self.uitest_var, width=16, state="readonly",
                     values=["", "boot", "fade", "loading", "select", "hud"]).pack(anchor="w")

        extraf = ttk.LabelFrame(right, text="Extra flags", padding=6)
        extraf.pack(fill="x", pady=(6, 0))
        self.extra_var = tk.StringVar(value=self._get("extra", ""))
        ttk.Entry(extraf, textvariable=self.extra_var, width=30).pack(fill="x")

        efff = ttk.LabelFrame(right, text="Effects (in game: Num / or F6 opens the layer list)", padding=6)
        efff.pack(fill="x", pady=(6, 0))
        saved_off = set(self._get("effects_off", []))
        self.effect_vars = {}
        egrid = ttk.Frame(efff)
        egrid.pack(fill="x")
        for i, (name, title) in enumerate(EFFECT_LAYERS):
            var = tk.BooleanVar(value=name not in saved_off)
            ttk.Checkbutton(egrid, text=title, variable=var).grid(row=i // 3, column=i % 3, sticky="w")
            var.trace_add("write", lambda *_a: self.update_preview())
            self.effect_vars[name] = var
        esolo = ttk.Frame(efff)
        esolo.pack(fill="x", pady=(4, 0))
        ttk.Label(esolo, text="Solo").pack(side="left")
        self.effect_solo_var = tk.StringVar(value=self._get("effects_solo", ""))
        ttk.Combobox(esolo, textvariable=self.effect_solo_var, width=15, state="readonly",
                     values=[""] + [name for name, _ in EFFECT_LAYERS]).pack(side="left", padx=4)
        self.effect_isolate_var = tk.BooleanVar(value=bool(self._get("effects_isolate", False)))
        ttk.Checkbutton(esolo, text="without the world", variable=self.effect_isolate_var).pack(side="left")
        ttk.Button(esolo, text="All on", command=self._effects_all_on).pack(side="right")
        for var in (self.effect_solo_var, self.effect_isolate_var):
            var.trace_add("write", lambda *_a: self.update_preview())

        # --- far right: the run's own output ---
        logf = ttk.LabelFrame(mid, text="Runtime log", padding=6)
        logf.pack(side="left", fill="both", expand=True, padx=(8, 0))
        self._build_log(logf)

        # --- bottom: command preview + launch ---
        bot = ttk.Frame(root)
        bot.pack(fill="x")
        self.cmd_text = tk.Text(bot, height=4, wrap="word")
        self.cmd_text.pack(fill="x")
        btns = ttk.Frame(bot)
        btns.pack(fill="x", pady=4)
        self.launch_btn = ttk.Button(btns, text="Launch", command=self.launch)
        self.launch_btn.pack(side="left")
        ttk.Button(btns, text="Stop", command=self.stop).pack(side="left", padx=4)
        ttk.Button(btns, text="Copy command", command=self.copy_cmd).pack(side="left")
        self.run_status = ttk.Label(btns, text="idle")
        self.run_status.pack(side="left", padx=10)

        for var in (self.width_var, self.height_var, self.mode_var, self.family_var, self.resolution_var,
                    self.renderer_var, self.budget_var, self.state_var, self.uitest_var,
                    self.extra_var):
            var.trace_add("write", lambda *_a: self.update_preview())
        for var in (self.audio_var, self.xinput_var, self.headless_var, self.fps_var,
                    self.load_state_var, self.save_state_var, self.live_telemetry):
            var.trace_add("write", lambda *_a: self.update_preview())
        self.exe_var.trace_add("write", lambda *_a: self.update_preview())

    # ---- game window placement --------------------------------------------
    #
    # Where the game window sits is a HOST concern. The guest composites into an
    # XFB and never learns where that ends up on a desktop, so moving the window
    # cannot change anything observable from inside the game - which is why this
    # belongs in the launcher and works with builds compiled before it existed.
    #
    # "Remembered" is the useful one: launch, drag the window wherever you want
    # it, press Grab, and every later run starts there.
    PLACEMENTS = ("Windows decides", "Top left", "Top right", "Bottom left",
                  "Bottom right", "Centre", "Left of this launcher", "Remembered")

    def _build_placement(self, parent):
        frame = ttk.LabelFrame(parent, text="Game window position", padding=6)
        frame.pack(fill="x", pady=(6, 0))
        self.placement_var = tk.StringVar(
            value=self._get("placement", "Left of this launcher"))
        if self.placement_var.get() not in self.PLACEMENTS:
            self.placement_var.set("Left of this launcher")
        box = ttk.Combobox(frame, textvariable=self.placement_var, width=22,
                           state="readonly", values=list(self.PLACEMENTS))
        box.pack(anchor="w")

        row = ttk.Frame(frame)
        row.pack(fill="x", pady=(4, 0))
        ttk.Label(row, text="X").pack(side="left")
        self.place_x = tk.StringVar(value=str(self._get("place_x", "0")))
        ttk.Entry(row, textvariable=self.place_x, width=6).pack(side="left", padx=(2, 6))
        ttk.Label(row, text="Y").pack(side="left")
        self.place_y = tk.StringVar(value=str(self._get("place_y", "0")))
        ttk.Entry(row, textvariable=self.place_y, width=6).pack(side="left", padx=2)
        self.grab_btn = ttk.Button(row, text="Grab", width=6, command=self.grab_position)
        self.grab_btn.pack(side="left", padx=(6, 0))

        self.place_hint = ttk.Label(frame, text="", foreground="#606070")
        self.place_hint.pack(anchor="w")
        self.placement_var.trace_add("write", lambda *_a: self._placement_changed())
        self._placement_changed()

        ttk.Button(frame, text="Apply to the running game", command=self.apply_placement).pack(
            anchor="w", pady=(4, 0))

    def _placement_changed(self):
        mode = self.placement_var.get()
        if mode == "Remembered":
            self.place_hint.config(
                text="Drag the window where you want it,\nthen press Grab.")
        elif mode == "Windows decides":
            self.place_hint.config(text="The launcher will not move it.")
        else:
            self.place_hint.config(text="X and Y are used by Remembered only.")

    def grab_position(self):
        """Reads the running game window's position into X/Y and remembers it."""
        if not (self.proc and self.proc.poll() is None):
            messagebox.showinfo("Nothing running", "Launch a stage first, put its window "
                                                   "where you want it, then press Grab.")
            return
        hwnd = find_game_window(self.proc.pid)
        rect = window_rect(hwnd) if hwnd else None
        if rect is None:
            messagebox.showinfo("No window", "The run has no window to read a position from.")
            return
        self.place_x.set(str(rect[0]))
        self.place_y.set(str(rect[1]))
        self.placement_var.set("Remembered")
        self._save_cfg()
        self.log_line("remembered game window position %d,%d" % (rect[0], rect[1]), "event")

    def apply_placement(self):
        """Moves the window that is running right now, without relaunching."""
        if not (self.proc and self.proc.poll() is None):
            messagebox.showinfo("Nothing running", "There is no run to move.")
            return
        hwnd = find_game_window(self.proc.pid)
        if not hwnd:
            messagebox.showinfo("No window", "The run has no window.")
            return
        self._apply_placement_to(hwnd)

    def _placement_target(self, width, height):
        """The (x, y) the chosen mode asks for, or None to leave the window alone.

        Every result is clamped into the work area of ONE monitor, so a
        remembered position from a monitor that is no longer attached cannot
        put the window somewhere it can never be reached. Which monitor matters:
        clamping to the primary one pulled a window remembered on a second
        monitor back onto the first.
        """
        mode = self.placement_var.get()
        if mode == "Windows decides":
            return None
        margin = 8
        if mode == "Remembered":
            try:
                x, y = int(self.place_x.get()), int(self.place_y.get())
            except ValueError:
                return None
            left, top, right, bottom = work_area(x, y)
        else:
            # Every other mode is relative to the monitor this launcher is on.
            left, top, right, bottom = work_area(self.winfo_rootx() + self.winfo_width() // 2,
                                                 self.winfo_rooty() + self.winfo_height() // 2)
            if mode == "Top left":
                x, y = left + margin, top + margin
            elif mode == "Top right":
                x, y = right - width - margin, top + margin
            elif mode == "Bottom left":
                x, y = left + margin, bottom - height - margin
            elif mode == "Bottom right":
                x, y = right - width - margin, bottom - height - margin
            elif mode == "Centre":
                x, y = left + (right - left - width) // 2, top + (bottom - top - height) // 2
            elif mode == "Left of this launcher":
                # Far enough left that the launcher's log column stays readable.
                x = max(left, self.winfo_rootx() - width - margin)
                y = top + max(margin, (bottom - top - height) // 3)
            else:
                return None
        x = max(left, min(x, max(left, right - width)))
        y = max(top, min(y, max(top, bottom - height)))
        return x, y

    def _apply_placement_to(self, hwnd):
        rect = window_rect(hwnd)
        if rect is None:
            return
        width, height = rect[2] - rect[0], rect[3] - rect[1]
        target = self._placement_target(width, height)
        if target is None:
            return
        move_window(hwnd, target[0], target[1])
        self.log_line("game window placed at %d,%d (%dx%d, %s)"
                      % (target[0], target[1], width, height,
                         self.placement_var.get().lower()), "event")
        if self.placement_var.get() == "Left of this launcher":
            self._dodge_right(target[0] + width)

    # ---- controller remapper ----------------------------------------------
    def _build_pad_tab(self, parent):
        self.pad_map = dict(PAD_DEFAULTS)
        self.pad_threshold = tk.StringVar(value="40")
        self.pad_swap = tk.BooleanVar(value=False)
        self.pad_style = tk.StringVar(value="auto")
        self.binding_for = None          # GC key currently waiting for a press
        self._bind_seen_release = False
        self.pad_rows = {}

        head = ttk.Frame(parent)
        head.pack(fill="x")
        self.pad_status = ttk.Label(head, text="")
        self.pad_status.pack(side="left")
        ttk.Button(head, text="Defaults", command=self.pad_defaults).pack(side="right")
        ttk.Button(head, text="Save to pad.cfg", command=self.pad_save).pack(side="right", padx=4)
        ttk.Button(head, text="Reload", command=self.pad_load).pack(side="right")

        body = ttk.Frame(parent)
        body.pack(fill="both", expand=True, pady=6)

        grid = ttk.LabelFrame(body, text="Bindings  (click Bind, then press the button)", padding=8)
        grid.pack(side="left", fill="both", expand=True)
        for i, (key, label) in enumerate(GC_BUTTONS):
            r, c = i % 6, (i // 6) * 3
            ttk.Label(grid, text=label, width=12).grid(row=r, column=c, sticky="w", pady=2)
            val = ttk.Label(grid, text="", width=16, relief="sunken", anchor="w")
            val.grid(row=r, column=c + 1, sticky="w", padx=4)
            btn = ttk.Button(grid, text="Bind", width=7,
                             command=lambda k=key: self.pad_begin_bind(k))
            btn.grid(row=r, column=c + 2, padx=(0, 12))
            self.pad_rows[key] = (val, btn)

        side = ttk.Frame(body)
        side.pack(side="left", fill="y", padx=(8, 0))

        opts = ttk.LabelFrame(side, text="Options", padding=8)
        opts.pack(fill="x")
        tf = ttk.Frame(opts)
        tf.pack(anchor="w")
        ttk.Label(tf, text="Trigger threshold").pack(side="left")
        ttk.Entry(tf, textvariable=self.pad_threshold, width=5).pack(side="left", padx=4)
        ttk.Label(opts, text="analog travel that also latches\nthe digital L/R bit",
                  foreground="#555").pack(anchor="w")
        ttk.Checkbutton(opts, text="Swap analog sticks", variable=self.pad_swap).pack(
            anchor="w", pady=(6, 0))
        sf = ttk.Frame(opts)
        sf.pack(anchor="w", pady=(6, 0))
        ttk.Label(sf, text="Button icons").pack(side="left")
        ttk.Combobox(sf, textvariable=self.pad_style, values=BUTTON_STYLES, width=11,
                     state="readonly").pack(side="left", padx=4)
        ttk.Label(opts, text="auto = the connected controller;\noriginal = the game's own art",
                  foreground="#555").pack(anchor="w")

        live = ttk.LabelFrame(side, text="Live controller", padding=8)
        live.pack(fill="x", pady=(8, 0))
        self.pad_live = ttk.Label(live, text="", font=("Consolas", 9), justify="left")
        self.pad_live.pack(anchor="w")

        note = ttk.LabelFrame(side, text="Not configurable, and why", padding=8)
        note.pack(fill="x", pady=(8, 0))
        ttk.Label(note, justify="left", foreground="#804000", text=(
            "Stick range, the guest's 8000 deadzone\n"
            "and analog A/B as 0-or-255 are read by\n"
            "the game's own movement and camera code\n"
            "against fixed thresholds. Changing them\n"
            "changes how far Shadow moves, so they\n"
            "are game data, not preferences.")).pack(anchor="w")

        self.pad_load()
        self._pad_poll()

    def pad_defaults(self):
        self.pad_map = dict(PAD_DEFAULTS)
        self.pad_threshold.set("40")
        self.pad_swap.set(False)
        self.pad_style.set("auto")
        self._pad_refresh()
        self.pad_status.config(text="built-in defaults restored - not saved yet")

    def _pad_cfg_path(self):
        # Always the disc root's pad.cfg: the one every build deployed there
        # reads, and the one --pad-cfg points the profiling build at. It used to
        # follow the selected executable, so after switching builds the tab
        # showed one file's bindings and Save wrote them over another's.
        return ROOT_PAD_CFG

    def pad_load(self):
        path = self._pad_cfg_path()
        # A full replace: start from the defaults, then apply the file. Parsing
        # on top of the current state left any binding the file omits - and the
        # threshold and stick swap - at whatever the tab held before.
        self.pad_map = dict(PAD_DEFAULTS)
        self.pad_threshold.set("40")
        self.pad_swap.set(False)
        self.pad_style.set("auto")
        if not os.path.exists(path):
            self.pad_status.config(text="no pad.cfg yet - showing built-in defaults")
            self._pad_refresh()
            return
        try:
            with open(path, encoding="utf-8") as fh:
                for line in fh:
                    line = line.split("#", 1)[0].strip()
                    if "=" not in line:
                        continue
                    k, v = (x.strip() for x in line.split("=", 1))
                    if k == "trigger_threshold":
                        self.pad_threshold.set(v)
                    elif k == "swap_sticks":
                        self.pad_swap.set(int(v, 0) != 0)
                    elif k == "button_style" and v in BUTTON_STYLES:
                        self.pad_style.set(v)
                    elif k in self.pad_map:
                        self.pad_map[k] = int(v, 0)
            self.pad_status.config(text="loaded " + path)
        except Exception as exc:  # noqa: BLE001
            self.pad_status.config(text="could not read pad.cfg: %s" % exc)
        self._pad_refresh()

    def pad_save(self):
        path = self._pad_cfg_path()
        try:
            thr = int(self.pad_threshold.get(), 0)
        except ValueError:
            thr = -1
        if not 0 <= thr <= 255:
            # The message always promised this range; now it is enforced. The
            # runtime rejects anything outside it rather than wrapping it.
            messagebox.showerror("Trigger threshold", "must be a number from 0 to 255")
            return
        lines = ["# GekkoForge host input binding. Written by tools/launcher.py.",
                 "# Values are XInput button masks; 0 means unbound.", ""]
        for key, label in GC_BUTTONS:
            mask = self.pad_map.get(key, 0)
            name = MASK_TO_NAME.get(mask, "unbound" if mask == 0 else "0x%04X" % mask)
            lines.append("%-12s = 0x%04X   # %s -> GC %s" % (key, mask, name, label))
        lines.append("")
        lines.append("trigger_threshold = %d" % thr)
        lines.append("swap_sticks       = %d" % (1 if self.pad_swap.get() else 0))
        lines.append("button_style      = %s   # auto, gamecube, xbox, playstation, switch"
                     % self.pad_style.get())
        lines.append("")
        try:
            with open(path, "w", encoding="utf-8") as fh:
                fh.write("\n".join(lines))
        except Exception as exc:  # noqa: BLE001
            messagebox.showerror("Could not write pad.cfg", str(exc))
            return
        self.pad_status.config(text="saved " + path + " - applies on the next launch")

    def pad_begin_bind(self, key):
        self.binding_for = key
        self._bind_seen_release = False
        for k, (_v, btn) in self.pad_rows.items():
            btn.config(text="press..." if k == key else "Bind")
        self.pad_status.config(text="press a button on the controller, or Esc to cancel")
        self.bind_all("<Escape>", self._pad_cancel_bind)

    def _pad_cancel_bind(self, _e=None):
        self.binding_for = None
        self.unbind_all("<Escape>")
        self._pad_refresh()
        self.pad_status.config(text="binding cancelled")

    def _pad_refresh(self):
        for key, (val, btn) in self.pad_rows.items():
            mask = self.pad_map.get(key, 0)
            val.config(text=MASK_TO_NAME.get(mask, "unbound" if mask == 0 else "0x%04X" % mask))
            btn.config(text="Bind")

    def _pad_poll(self):
        sample = read_pad(0)
        if sample is None:
            self.pad_live.config(
                text="no controller on port 1" if _XINPUT is not None else "XInput not available")
        else:
            buttons, lt, rt, lx, ly, rx, ry = sample
            names = [n for n, m in XINPUT_BUTTONS if buttons & m]
            self.pad_live.config(
                text="buttons %s\nLT %3d  RT %3d\nL %6d,%6d\nR %6d,%6d"
                     % (",".join(names) if names else "-", lt, rt, lx, ly, rx, ry))
            if self.binding_for is not None:
                # A clean release is required first, so the button still held
                # from clicking "Bind" on a controller cannot bind itself.
                if buttons == 0:
                    self._bind_seen_release = True
                elif self._bind_seen_release:
                    for _n, m in XINPUT_BUTTONS:
                        if buttons & m:
                            self.pad_map[self.binding_for] = m
                            break
                    self.binding_for = None
                    self.unbind_all("<Escape>")
                    self._pad_refresh()
                    self.pad_status.config(text="bound - press Save to write pad.cfg")
        self.after(60, self._pad_poll)

    def _row(self, parent, label, var, row):
        ttk.Label(parent, text=label).grid(row=row, column=0, sticky="w")
        ttk.Entry(parent, textvariable=var, width=12).grid(row=row, column=1, sticky="w")

    # ---- behaviour ---------------------------------------------------------
    def rescan(self):
        self.exes = find_executables(DISC_ROOT)
        self.exe_box["values"] = self.exes
        if self.exes and self.exe_var.get() not in self.exes:
            self.exe_var.set(self.exes[0])
        self.refresh_stages()

    def refresh_stages(self):
        self._scan_generation += 1
        generation = self._scan_generation
        exe = self.exe_var.get()
        self.status.config(text="reading stages...", foreground="")
        def work():
            stages, err = list_stages(exe, DISC_ROOT) if exe and os.path.isfile(exe) else ([], "No stage executable found")
            self.post(self._stages_ready, generation, stages, err)
            # The build's own DXGI report of the displays (None for a build that predates --list-displays).
            displays = rp.detect_displays(exe) if exe and os.path.isfile(exe) else None
            self.post(self._displays_ready, generation, displays, exe_supports(exe, "--render-width"))
        threading.Thread(target=work, daemon=True).start()

    # ---- display choices ---------------------------------------------------
    def _native_size(self):
        """(width, height, refresh Hz, monitor name) of the primary display's desktop."""
        output = rp.primary_output(self.displays)
        if output:
            d = output["desktop"]
            return d["width"], d["height"], d.get("refresh_hz", 0), output.get("monitor") or output.get("device", "")
        desktop = rp.primary_desktop()
        return (desktop + ("primary display",)) if desktop else None

    def _displays_ready(self, generation, displays, render_size_flags):
        if generation != self._scan_generation:
            return
        self.displays = displays
        self.render_size_flags = render_size_flags
        self._fill_resolutions()

    def _fill_resolutions(self):
        native = self._native_size()
        output = rp.primary_output(self.displays)
        modes = output.get("modes", []) if output else []
        # Borderless fullscreen offers only sizes of the screen's own shape, so it never shows bars (the user's
        # choice, 2026-10-26); other shapes stay available windowed, where the window takes their shape.
        fill = self.mode_var.get() == "Borderless fullscreen" and bool(native)
        if fill:
            self.family_var.set(rp.classify(native[0], native[1])[0])
        self.family_box.configure(state="disabled" if fill else "readonly")
        self._resolution_items = rp.choices(self.family_var.get(), native, modes, fill_screen=fill)
        self.resolution_box.configure(values=[label for label, _ in self._resolution_items])
        # Keep the stored choice when this family lists it; else the native desktop.
        label = None
        for item_label, size in self._resolution_items:
            value = "%dx%d" % size if size else ("custom" if item_label == "Custom" else "native")
            if value == self.resolution_value:
                label = item_label
        if label is None:
            label = self._resolution_items[0][0]
            self.resolution_value = "native" if native else "custom"
        self.resolution_var.set(label)
        self._resolution_picked()
        if native:
            family, exact = rp.classify(native[0], native[1])
            note = "" if exact else " (near)"
            self.monitor_label.config(text="Monitor: %s, native %dx%d (%s%s), %s Hz. Borderless fullscreen uses the "
                                           "desktop's refresh rate; exclusive fullscreen is not implemented."
                                           % (native[3], native[0], native[1], family, note, native[2] or "?"))
        else:
            self.monitor_label.config(text="Monitor: not detected.")

    def _resolution_picked(self):
        label = self.resolution_var.get()
        for item_label, size in getattr(self, "_resolution_items", []):
            if item_label == label:
                self.resolution_value = "%dx%d" % size if size else ("custom" if label == "Custom" else "native")
        state = "normal" if self.resolution_value == "custom" else "disabled"
        self.custom_w.configure(state=state)
        self.custom_h.configure(state=state)
        if hasattr(self, "cmd_text"):
            self.update_preview()

    def _chosen_size(self):
        """((width, height), None), (None, reason) or (None, None) when nothing is decided yet."""
        value = self.resolution_value
        if value == "native":
            native = self._native_size()
            return ((native[0], native[1]), None) if native else (None, None)
        if value == "custom":
            text_w, text_h = self.width_var.get().strip(), self.height_var.get().strip()
            if not (text_w.isdigit() and text_h.isdigit()):
                return None, "Custom resolution must be whole numbers of pixels, not %r x %r." % (text_w, text_h)
            size = (int(text_w), int(text_h))
        else:
            w, _, h = value.partition("x")
            if not (w.isdigit() and h.isdigit()):
                return None, "Unknown resolution %r." % value
            size = (int(w), int(h))
        reason = rp.validate(*size)
        native = self._native_size()
        if not reason and self.mode_var.get() == "Borderless fullscreen" and native and \
                not rp.same_shape(size[0], size[1], native[:2]):
            reason = "does not have this screen's shape (%dx%d); borderless fullscreen takes only sizes that fill it" % \
                native[:2]
        return (None, "Resolution %dx%d %s." % (size[0], size[1], reason)) if reason else (size, None)

    def _stages_ready(self, generation, stages, err):
        if generation != self._scan_generation:
            return
        self.stages = [] if err else stages
        self.apply_filter()
        if err:
            self.status.config(text="could not read stages", foreground="red")
            messagebox.showerror("--list-stages failed", err[:2000])
            return
        count = sum(1 for _sid, _name, bootable in stages if bootable)
        self.status.config(text="%d stages, %d bootable" % (len(stages), count), foreground="")

    def apply_filter(self):
        needle = self.filter_var.get().strip().lower()
        self.tree.delete(*self.tree.get_children())
        selected = self.selected_stage.get()
        keep = None
        entries = self.stages
        for sid, name, boot in entries:
            if self.only_bootable.get() and not boot:
                continue
            title = STAGE_TITLES.get(sid, "") if sid.startswith("stg") else ("Opening movie" if sid == "Op.sfd" else sid)
            # Search the retail title too, so typing "lava" or "haunt" finds it.
            hay = (sid + " " + name + " " + title).lower()
            if needle and needle not in hay:
                continue
            item = self.tree.insert("", "end", values=(sid, title, name, "yes" if boot else "no"))
            if sid == selected:
                keep = item
        # What is highlighted is what launches. A selection that the filter or
        # a new list has hidden used to stay live, invisibly, and was passed to
        # --boot-stage; now it is shown again or it is dropped.
        if keep is not None:
            self.tree.selection_set(keep)
            self.tree.see(keep)
        elif selected:
            self.selected_stage.set("")
        self.update_preview()

    def _on_select(self, _event=None):
        sel = self.tree.selection()
        self.selected_stage.set(self.tree.item(sel[0])["values"][0] if sel else "")
        self.update_preview()

    def _clear_selection(self, _event=None):
        self.tree.selection_remove(*self.tree.selection())
        self.selected_stage.set("")
        self.update_preview()

    def _launchable_stage(self):
        """The selected stage when --boot-stage can take it, else ''."""
        sid = self.selected_stage.get()
        entries = self.stages
        for stage_id, _name, bootable in entries:
            if stage_id == sid:
                return sid if bootable else ""
        return ""

    def pick_state(self):
        initial = os.path.join(DISC_ROOT, "save-data")
        p = filedialog.askopenfilename(initialdir=initial if os.path.isdir(initial) else DISC_ROOT,
                                       filetypes=[("Savestate", "*.gekkostate"), ("All", "*.*")])
        if p:
            self.state_var.set(os.path.normpath(p))

    def build_command(self):
        """Returns (command, notes, errors).

        notes explain anything that was dropped or adjusted; errors are values
        that cannot be passed as typed, and Launch refuses while there are any.
        The preview, Copy command and Launch all come from this one function, so
        they cannot disagree.
        """
        exe = self.exe_var.get()
        cmd = [exe, "--game-data", DISC_ROOT]
        notes, errors = [], []
        outside = bool(exe) and outside_disc_root(exe)

        load = self.load_state_var.get()
        save = self.save_state_var.get()
        uitest = self.uitest_var.get()
        state_file = clean_path(self.state_var.get())
        if outside and not state_file and (load or save):
            # The default slot is resolved beside the executable, which for the
            # profiling build is its own tree, not save-data in the disc root.
            state_file = ROOT_STATE_SLOT

        stage = self._launchable_stage()
        if self.selected_stage.get() and not stage:
            notes.append("%s is not bootable, so no --boot-stage was added." % self.selected_stage.get())
        if stage and (load or uitest):
            notes.append("--boot-stage was dropped: it needs a cold boot and cannot be combined with --load-state or --ui-test.")
        elif stage:
            cmd += ["--boot-stage", stage]

        if outside:
            # The runtime resolves the memory card - and the SRAM beside its
            # working image, which holds the 50/60 Hz choice - display.cfg and
            # pad.cfg from the EXECUTABLE's directory. For the profiling build
            # that is its own tree: a fresh card that preselects 50 Hz (so
            # --boot-stage stops after half a second), default display
            # settings and default bindings. Point all three back here, so a
            # profiled run plays with the same card, settings and controls as a
            # normal one. Builds in the disc root never get these flags, so an
            # archived build that predates them is unaffected.
            card = ROOT_CARD
            display = ROOT_DISPLAY_CFG
            pad = ROOT_PAD_CFG
            if os.path.exists(card):
                cmd += ["--memory-card", card]
            if os.path.exists(display):
                cmd += ["--display-cfg", display]
            if os.path.exists(pad):
                cmd += ["--pad-cfg", pad]

        if state_file:
            cmd += ["--state-file", state_file]
        if load:
            # Always, even with the path blank: the runtime has a default slot,
            # and dropping the flag silently turned the run into a cold boot.
            cmd += ["--load-state"]
        if save:
            cmd += ["--save-state"]
        if uitest:
            cmd += ["--ui-test", uitest]

        # The display: one exact size, used for the window or the fullscreen picture AND as the render size.
        size, size_error = self._chosen_size()
        if size_error:
            errors.append(size_error)
        elif size:
            flags = rp.command_flags(self.mode_var.get(), size)
            if not getattr(self, "render_size_flags", True):
                # A build from before the exact render size: the window / fullscreen choice only.
                flags = [f for i, f in enumerate(flags) if not (f.startswith("--render-") or
                                                              (i > 0 and flags[i - 1].startswith("--render-")))]
                notes.append("This build predates --render-width: it renders at the window's size.")
            cmd += flags
        if self.renderer_var.get() in ("native", "legacy"):
            cmd += ["--renderer", self.renderer_var.get()]
        if self.draw_distance_var.get() != 8.0:
            cmd += ["--draw-distance", "%g" % self.draw_distance_var.get()]
        # The effect layer switches: only what differs from "everything drawn" (older builds lack the flags).
        effects_off = [name for name, var in self.effect_vars.items() if not var.get()]
        if effects_off:
            cmd += ["--effects-off", ",".join(effects_off)]
        solo = self.effect_solo_var.get()
        if solo:
            cmd += ["--effects-solo", solo]
            if self.effect_isolate_var.get():
                cmd += ["--effects-isolate"]
        elif self.effect_isolate_var.get():
            notes.append("\"without the world\" applies to a solo only: pick a Solo layer.")
        if self.fps_var.get():
            cmd += ["--fps-counter"]
        if self.headless_var.get():
            cmd += ["--headless"]
        if not self.audio_var.get():
            cmd += ["--no-audio"]
        if not self.xinput_var.get():
            cmd += ["--no-xinput"]

        budget_text = self.budget_var.get().strip()
        if budget_text:
            budget = parse_count(budget_text)
            if budget is None:
                errors.append("Budget must be a positive whole number - 15e9, "
                              "15_000_000_000 or 15 000 000 000, no commas - not %r."
                              % budget_text)
            else:
                cmd += ["--budget", str(budget)]
        if save and self.headless_var.get() and not budget_text:
            notes.append("A headless run with no budget never ends by itself, and Stop "
                         "on a run with no window is a hard kill: the savestate at exit "
                         "will not be written. Set a budget.")

        # Live stats are a host-side observation channel: the runtime writes a
        # row per wall second and this launcher tails it. Appended before the
        # free-form box so an explicit --telemetry there still wins.
        if self.live_telemetry.get() and not self.headless_var.get():
            cmd += ["--telemetry", self._telemetry_path_for(self._run_serial + 1)]
        try:
            cmd += split_flags(self.extra_var.get())
        except ValueError as exc:
            errors.append("Extra flags could not be read: %s." % exc)
        return cmd, notes, errors

    # The live-stats file. One per RUN, not one fixed name: a fixed name was
    # shared by every launcher instance and every run, and a copy a previous
    # run left behind (its delete fails while a reader still holds it) was then
    # replayed as the new run's first seconds. It sits next to the launcher,
    # deliberately not in the temp directory, which is inside the user profile
    # and would put the account name into the preview and the log.
    def _telemetry_path_for(self, serial):
        return os.path.join(HERE, ".launcher-live-%d-%d.csv" % (os.getpid(), serial))

    @staticmethod
    def _remove_file(path):
        try:
            os.remove(path)
        except OSError:
            pass

    def _sweep_telemetry_files(self):
        """Removes live-stats files left by launchers that are no longer open."""
        for path in glob.glob(os.path.join(HERE, ".launcher-live*.csv")):
            try:
                if time.time() - os.path.getmtime(path) > 3600:
                    os.remove(path)
            except OSError:
                pass

    def update_preview(self, *_a):
        if not hasattr(self, "cmd_text"):
            return  # a variable changed while the window is still being built
        cmd, notes, errors = self.build_command()
        text = subprocess.list2cmdline(cmd)
        for line in errors:
            text += "\nERROR: " + line
        for line in notes:
            text += "\nNOTE: " + line
        self.cmd_text.delete("1.0", "end")
        self.cmd_text.insert("1.0", text)

    def copy_cmd(self):
        # The command alone, quoted the way Windows parses it - never the notes
        # shown under it in the preview, which made the copy unrunnable.
        cmd, _notes, errors = self.build_command()
        self.clipboard_clear()
        self.clipboard_append(subprocess.list2cmdline(cmd))
        self.run_status.config(text="command copied" + (" (with errors!)" if errors else ""))

    def launch(self):
        if self.proc and self.proc.poll() is None:
            messagebox.showinfo("Already running", "Stop the current run first.")
            return
        cmd, notes, errors = self.build_command()
        if errors:
            messagebox.showerror("Cannot launch", "\n".join(errors))
            return
        if not os.path.exists(cmd[0]):
            messagebox.showerror("Missing executable", cmd[0])
            return
        self._save_cfg()
        self.log_clear()
        self.log_line("$ " + subprocess.list2cmdline(cmd), "cmd")
        for line in notes:
            self.log_line("NOTE: " + line, "warn")
        serial = self._run_serial + 1
        telemetry = cmd[cmd.index("--telemetry") + 1] if "--telemetry" in cmd else None
        if telemetry:
            # Normally absent already; this only matters if a file of the same
            # name survived a crashed launcher whose process id was reused.
            self._remove_file(telemetry)
        try:
            # The run's own output is the log: piped as bytes and decoded line
            # by line (see decode_line), drained on a thread - a pipe nobody
            # reads fills and then blocks the game - and with no console window.
            proc = subprocess.Popen(
                cmd, cwd=DISC_ROOT,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                stdin=subprocess.DEVNULL, creationflags=NO_WINDOW)
        except Exception as exc:  # noqa: BLE001
            messagebox.showerror("Launch failed", str(exc))
            return
        self._run_serial = serial
        self.proc = proc
        self.run_status.config(text="running (pid %d)" % proc.pid)
        threading.Thread(target=self._drain_output, args=(proc,), daemon=True).start()
        if telemetry:
            threading.Thread(target=self._tail_telemetry, args=(telemetry, proc),
                             daemon=True).start()
        placing = (self.placement_var.get() != "Windows decides"
                   and not self.headless_var.get()
                   and self.mode_var.get() != "Borderless fullscreen")
        if placing:
            threading.Thread(target=self._place_game_window, args=(proc,), daemon=True).start()

    def _drain_output(self, proc):
        """Reads the run's output to the end, THEN reports its exit.

        Both go through the same queue in that order, so the exit line can never
        appear above output the run printed before it ended.
        """
        try:
            for raw in iter(proc.stdout.readline, b""):
                self.post_log(decode_line(raw).rstrip("\r\n"))
        except Exception as exc:  # noqa: BLE001
            self.post_log("launcher: reading the run's output failed: %s" % exc, "error")
        self.post(self._run_finished, proc, proc.wait())

    def _run_finished(self, proc, rc):
        self.log_line("-- process exited with %d --" % rc, "cmd")
        if self.proc is proc:
            self.proc = None
            self.run_status.config(text="exited with %d" % rc)

    def stop(self):
        proc = self.proc
        if proc is None or proc.poll() is not None:
            return
        if self._close_run(proc, wait=0.0):
            self.run_status.config(text="stopping...")
            self.after(8000, self._escalate_stop, proc)
        else:
            self.run_status.config(text="stopping (no window: terminated)")

    def _close_run(self, proc, wait):
        """Ends a run the way its own close button would, if it has a window.

        WM_CLOSE lets the game finish through its normal path: the exit report
        is printed and "Save one at exit" is honoured. TerminateProcess loses
        both, so it is only the fallback - for a run with no window, or one that
        has not gone after the grace period. Returns True if WM_CLOSE was sent.
        """
        hwnd = find_game_window(proc.pid)
        if hwnd and post_close(hwnd):
            if wait > 0.0:
                try:
                    proc.wait(timeout=wait)
                except subprocess.TimeoutExpired:
                    proc.terminate()
            return True
        proc.terminate()
        return False

    def _escalate_stop(self, proc):
        if proc.poll() is None:
            proc.terminate()
            self.log_line("the run did not close within 8 s of Stop - terminated", "warn")

    # ---- run log -----------------------------------------------------------
    #
    # The point of showing this is that the runtime says, in its own words,
    # what it is doing: the filesystem it mounts, the assets it serves, and -
    # the line that actually settles the question - how many instructions were
    # retired through NATIVE frames versus interpreted. On a clean run the
    # interpreter line reads 0 instructions, which is what "recompiled, not
    # emulated" means concretely.
    LOG_RULES = [
        # (substring, tag). First match wins, so order matters.
        ("Interpreter cost", "native"),
        ("Native execution", "native"),
        ("Counter identity", "native"),
        ("Throughput split", "native"),
        ("native frames", "native"),
        ("Assets:", "asset"),
        ("[file-api]", "asset"),
        ("[native-assets]", "asset"),
        ("Filesystem mounted", "asset"),
        ("DOL loaded", "asset"),
        ("Files:", "asset"),
        ("Memory card", "asset"),
        ("Display config", "info"),
        ("Pad mapping", "info"),
        ("Internal EFB", "info"),
        ("Gameplay:", "info"),
        ("Stages in the guest", "info"),
        ("[native-gpu]", "native"),
        # Why the run ended. A boot-stage refusal ("the PAL screen preselects
        # 50 Hz ...") is explained ONLY on the Detail line; unclassified, both
        # were hidden by default and a run that died after half a second said
        # nothing about why.
        ("Stop reason:", "info"),
        ("Detail:", "warn"),
    ]

    # A COUNT OF ZERO IS NOT A PROBLEM. The runtime reports its failure counters
    # whether or not they fired - "FAILED 0", "0 failed", "refused 0" - and a
    # plain substring rule painted every one of those lines red, which is worse
    # than no colour at all: a log where the healthy case is red cannot be read
    # at a glance. These are stripped before the line is judged.
    _ZERO_COUNTS = re.compile(
        r"(?i)\b(?:errors?|failed|failures?|refused|unmapped|out[- ]of[- ]range|"
        r"drops?|evictions?|misses|stalls?)(?:\s+|\s*[=:]\s*)0\b|\b0\s+"
        r"(?:errors?|failed|failures?|refused|unmapped|out[- ]of[- ]range|"
        r"drops?|evictions?)\b")
    # Plurals included: "render errors 12" and "failures 7" did not match the
    # singular forms, fell through to "plain" and were hidden by default.
    _PROBLEM = re.compile(
        r"(?i)\b(?:errors?|failed|failures?|refused|unmapped|out[- ]of[- ]range|"
        r"cannot|could not|refusing)\b")

    def _classify(self, line):
        # Failures FIRST. The subsystem rules below colour a line by where it
        # comes from, and used to be checked first - so "Assets: ... 3 failed"
        # or a [native-gpu] line with a non-zero failure count was painted in
        # the healthy colour. Zero counts are stripped before judging.
        residue = self._ZERO_COUNTS.sub("", line)
        # A line that labels itself a warning is a warning, whatever wording
        # follows it: WARNING lines routinely contain "could not".
        if "WARNING" in line or "warning" in line:
            return "warn"
        if self._PROBLEM.search(residue):
            return "error"
        for needle, tag in self.LOG_RULES:
            if needle in line:
                return tag
        return "plain"

    def _build_log(self, parent):
        self.log_only_interesting = tk.BooleanVar(value=True)
        # The runtime prints at startup and at exit and is silent in between,
        # so stdout alone shows nothing while the game is actually running.
        # --telemetry emits one row per wall SECOND covering everything that
        # can grow as a level is traversed; tailing it is what makes this panel
        # show behaviour rather than just a boot banner.
        self.live_telemetry = tk.BooleanVar(value=bool(self._get("telemetry", True)))

        head = ttk.Frame(parent)
        head.pack(fill="x")
        ttk.Checkbutton(head, text="Live stats (1 Hz)",
                        variable=self.live_telemetry).pack(side="left")
        ttk.Checkbutton(head, text="Hide unclassified output",
                        variable=self.log_only_interesting).pack(side="left", padx=(8, 0))
        ttk.Button(head, text="Clear", width=7, command=self.log_clear).pack(side="right")
        ttk.Button(head, text="Save", width=7, command=self.log_save).pack(side="right", padx=4)

        self.log_text = tk.Text(parent, wrap="none", height=10, width=60,
                                font=("Consolas", 9), background="#101014",
                                foreground="#c8c8d0", insertbackground="#c8c8d0")
        ysb = ttk.Scrollbar(parent, orient="vertical", command=self.log_text.yview)
        self.log_text.configure(yscrollcommand=ysb.set)
        ysb.pack(side="right", fill="y")
        self.log_text.pack(fill="both", expand=True)
        self.log_text.tag_configure("asset", foreground="#6fd66f")
        self.log_text.tag_configure("native", foreground="#66b8ff")
        self.log_text.tag_configure("info", foreground="#c8c8d0")
        self.log_text.tag_configure("warn", foreground="#e0c060")
        self.log_text.tag_configure("error", foreground="#ff6b6b")
        self.log_text.tag_configure("cmd", foreground="#8a8a96")
        self.log_text.tag_configure("plain", foreground="#70707c")
        self.log_text.tag_configure("stat", foreground="#9ad5e0")
        self.log_text.tag_configure("event", foreground="#d0a0ff")
        self.log_line("Launch a stage and the runtime reports itself here.", "cmd")
        self.log_line("green assets   blue native   cyan live stats   purple events", "cmd")

    def log_line(self, line, tag=None):
        line = scrub_paths(line)
        tag = tag or self._classify(line)
        if self.log_only_interesting.get() and tag == "plain":
            return
        self.log_text.insert("end", line + "\n", tag)
        # Keep the buffer bounded: a long run prints a great deal and an
        # unbounded Text widget is how a launcher ends up using more memory
        # than the game.
        if int(self.log_text.index("end-1c").split(".")[0]) > 4000:
            self.log_text.delete("1.0", "1000.0")
        self.log_text.see("end")

    # ---- live stats --------------------------------------------------------
    #
    # The runtime prints a banner at startup and a summary at exit and says
    # nothing in between, so stdout alone cannot show a game that is running.
    # --telemetry does: one CSV row per wall SECOND, sampled on the game thread,
    # covering frame rate, guest throughput, what the renderer submitted and
    # every resource pool that can grow while a level is traversed.
    #
    # Only a reading of that row goes in the log. The raw row has 49 columns;
    # what a person watching wants is the handful that say whether the game is
    # keeping up, plus a line whenever something actually HAPPENS - a texture
    # set streaming in, an eviction, a host stall, or the one number that would
    # falsify the whole project: guest instructions that went to the interpreter.

    def _tail_telemetry(self, path, proc):
        """Follows the CSV the run is appending. Never blocks the game.

        The writer flushes after each complete row, so a readline() either
        returns a whole row or nothing; a partial line cannot be observed. The
        file is this run's alone (see _telemetry_path_for), so nothing it holds
        can belong to an earlier run, and it is removed when the run is over.
        """
        handle = None
        header = None
        previous = None
        try:
            while True:
                if handle is None:
                    if os.path.exists(path):
                        handle = open(path, "r", encoding="utf-8", errors="replace")
                    elif proc.poll() is not None:
                        return  # the run ended before it ever wrote a row
                    else:
                        time.sleep(0.25)
                        continue
                line = handle.readline()
                if not line:
                    if proc.poll() is not None:
                        return
                    time.sleep(0.2)
                    continue
                fields = line.strip().split(",")
                if header is None:
                    header = fields
                    note = "live stats on: %d columns, one row per second" % len(header)
                    if "renderer" not in header:
                        note += "  (build predates the renderer column: legacy names)"
                    self.post_log(note, "event")
                    continue
                if len(fields) != len(header):
                    continue
                row = dict(zip(header, fields))
                if previous is None and "renderer" in row:
                    self.post_log("rendering with the %s renderer" % row["renderer"], "native")
                for tag, text in self._read_telemetry(row, previous):
                    self.post_log(text, tag)
                previous = row
        except Exception as exc:  # noqa: BLE001 - reported, never swallowed
            self.post_log("launcher: live stats stopped: %s" % exc, "error")
        finally:
            if handle is not None:
                handle.close()
            if proc.poll() is not None:
                self._remove_file(path)

    @staticmethod
    def _num(row, key, default=0.0):
        try:
            return float(row.get(key, default))
        except (TypeError, ValueError):
            return default

    def _read_telemetry(self, row, previous):
        """One row -> the lines a person should read. Returns [(tag, text)].

        The runtime writes a `renderer` column because most of the row is
        legacy-Flipper-only and reads zero under the native renderer. Rows from
        a build that predates that column are still readable: the legacy names
        are used and the reading says so.
        """
        n = self._num
        native = row.get("renderer") == "native"

        def total(key, legacy_key=None):
            """Per-second delta of a running total, 0.0 on the first row."""
            if previous is None:
                return 0.0
            k = key if key in row else legacy_key
            if k is None:
                return 0.0
            return max(0.0, n(row, k) - n(previous, k))

        def since_start(key):
            """A running total's growth this interval - or, on the first row,
            everything it counted before the first row. A plain delta dropped
            that first interval, so the boot was never reported at all."""
            return n(row, key) if previous is None else total(key)

        if row.get("renderer") == "lifted":
            def measured(key, digits=1):
                value = self._num(row, key, float("nan"))
                return f"{value:.{digits}f}" if math.isfinite(value) else "n/a"

            # Render/present includes CPU-side waits. It is not GPU execution
            # or process CPU usage, and no guest-instruction/MIPS rate applies.
            out = [("stat", f"{measured('wall_s', 0):>4}s {measured('fps'):>5} render FPS  "
                    f"render/present {measured('render_present_ms')} ms  GPU n/a  "
                    f"{measured('draws_last', 0)} draws (last)  {measured('textures_live', 0)} tex  "
                    f"RAM {measured('ram_working_mib', 0)}M")]
            for key, label in (("dropped", "unrendered frame packets dropped"),
                               ("skipped_draws", "draws skipped"),
                               ("resource_failures", "resource failures"),
                               ("host_stalls", "host stalls"),
                               ("interp_instr", "interpreted instructions")):
                delta = since_start(key)
                if delta > 0:
                    out.append(("warn", f"      {delta:.0f} {label}"))
            return out

        if native:
            tris = total("nat_triangles")
            draws = total("nat_draws")
            textures = n(row, "nat_textures")
            texture_mib = n(row, "nat_texture_mib")
        else:
            tris = total("triangles")
            draws = total("draw_batches")
            textures = n(row, "tex_cache_entries")
            texture_mib = n(row, "tex_cache_mib")

        uploaded = total("nat_upload_mib") if native else total("upload_mib")
        out = []
        fps = n(row, "fps")
        # Upload traffic belongs HERE and not in the event list: constant and
        # texture data are re-sent every frame, so it is a steady rate. Reported
        # as an event it printed an identical line every second and buried the
        # things that were genuinely happening.
        stat = ("%4ds %5.1f fps  cpu %4.1f gpu %4.1f ms  %4.0f MIPS  "
                "%6s tris %5s draws  %.0f tex  %5.1f MiB/s  RAM %.0fM"
                % (n(row, "wall_s"), fps, n(row, "cpu_frame_ms"), n(row, "gpu_frame_ms"),
                   n(row, "mips"), self._compact(tris), self._compact(draws),
                   textures, uploaded, n(row, "ram_working_mib")))
        # Falling behind is the one thing a reader should not have to spot by
        # comparing numbers. The target is the game's OWN rate - frames per
        # guest second, 60 or 50 depending on the video mode - not a fixed 50,
        # which flagged every PAL 50 Hz run as slow whenever it read 49.x.
        target = n(row, "xfb_per_guest_s")
        behind = fps < 0.95 * target if target > 0.0 else 0.0 < fps < 50.0
        out.append(("warn" if behind else "stat", stat))

        # Events. Each is a delta against the previous second, so a line only
        # appears when the thing it describes actually occurred.
        interp = since_start("interp_instr")
        if interp > 0.0:
            out.append(("error",
                        "      interpreter executed %s guest instructions" % self._compact(interp)))
        stalls = since_start("host_stalls")
        if stalls > 0.0:
            out.append(("warn", "      %d host stalls" % stalls))
        if previous is None:
            return out

        if native:
            new_textures = n(row, "nat_textures") - n(previous, "nat_textures")
            geometry = n(row, "nat_geometry") - n(previous, "nat_geometry")
            lost = total("nat_missing_geometry")
            refused = total("nat_rejections")
            if geometry >= 4.0:
                out.append(("asset", "      +%d meshes resident" % geometry))
            elif geometry <= -4.0:
                out.append(("event", "      %d meshes released" % -geometry))
            if lost > 0.0:
                out.append(("warn", "      %s draws had no geometry" % self._compact(lost)))
            if refused > 0.0:
                out.append(("warn", "      %s resources refused and re-queued" % self._compact(refused)))
        else:
            new_textures = n(row, "tex_cache_entries") - n(previous, "tex_cache_entries")
            decoded = total("decoded_maps")
            if decoded >= 4.0:
                out.append(("asset", "      +%d assets decoded" % decoded))
            evicted = total("tex_evictions")
            if evicted > 0.0:
                out.append(("event", "      %d textures evicted" % evicted))

        if new_textures >= 4.0:
            out.append(("asset", "      +%d textures resident, %.1f MiB total (streaming in)"
                                 % (new_textures, texture_mib)))
        elif new_textures <= -4.0:
            out.append(("event", "      %d textures released" % -new_textures))
        return out

    @staticmethod
    def _compact(value):
        if value >= 1.0e6:
            return "%.1fM" % (value / 1.0e6)
        if value >= 1.0e3:
            return "%.0fk" % (value / 1.0e3)
        return "%.0f" % value

    # ---- host window placement ---------------------------------------------
    def _place_game_window(self, proc):
        """Moves the run's window off this launcher's log column.

        The window does not exist yet when Popen returns - the runtime mounts
        the filesystem and loads the DOL first - so this waits for it rather
        than guessing a delay, and gives up if the run never shows one.
        """
        deadline = time.time() + 45.0
        hwnd = None
        while time.time() < deadline and proc.poll() is None:
            hwnd = find_game_window(proc.pid)
            if hwnd:
                break
            time.sleep(0.2)
        if not hwnd:
            return
        # The window raises itself immediately after creation; a move issued
        # during that sequence is undone by it.
        time.sleep(0.5)
        self.post(self._apply_placement_to, hwnd)

    def _dodge_right(self, game_right):
        if self.winfo_rootx() >= game_right:
            return
        left, top, right, _bottom = work_area(self.winfo_rootx(), self.winfo_rooty())
        target = max(left, right - self.winfo_width())
        if target > self.winfo_rootx():
            self.geometry("+%d+%d" % (target, max(top, self.winfo_rooty())))

    def log_clear(self):
        self.log_text.delete("1.0", "end")

    def log_save(self):
        path = filedialog.asksaveasfilename(defaultextension=".txt",
                                            filetypes=[("Text", "*.txt")])
        if not path:
            return
        try:
            with open(path, "w", encoding="utf-8") as fh:
                fh.write(self.log_text.get("1.0", "end"))
        except Exception as exc:  # noqa: BLE001
            messagebox.showerror("Could not save log", str(exc))


if __name__ == "__main__":
    if not os.path.isdir(os.path.join(DISC_ROOT, "files")):
        print("This does not look like the extracted-disc root: %s" % DISC_ROOT, file=sys.stderr)
    Launcher().mainloop()
