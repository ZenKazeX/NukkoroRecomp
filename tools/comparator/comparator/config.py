"""The workstation's configuration: one user-editable JSON file (tools/comparator/comparator.json).

Nothing about a local machine is hard-coded here: the defaults are derived from where this file lives (the
GekkoForge tree and its parent, the disc root), and every value can be edited in the file or in the Settings
dialog. A missing file is created from the defaults on the first start; unknown keys are kept.
"""
from __future__ import annotations

import copy
import json
import os
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOOL_DIR = HERE.parent                      # tools/comparator
GEKKOFORGE = TOOL_DIR.parents[1]            # GekkoForge
DISC_ROOT = GEKKOFORGE.parent / 'game'               # the extracted disc (sys/, files/, GC/)
CONFIG_PATH = TOOL_DIR / 'comparator.json'

FORMAT = 'shadow-comparator-config 1'


def default_config() -> dict:
    """The defaults: OLD = the retained reference, NEW = the deployed game."""
    return {
        'format': FORMAT,
        # Where the game's data is (the folder holding files/ and GC/); passed as --game-data.
        'game_data': str(DISC_ROOT),
        # The checkpoint store and the per-launch session folders.
        'states_dir': str(DISC_ROOT / 'comparison_states'),
        # Which build profile is BEFORE (left) and AFTER (right).
        'before': 'old',
        'after': 'new',
        'profiles': {
            'old': {
                'label': 'Reference - legacy renderer',
                'exe': str(DISC_ROOT / 'ShadowTheHedgehog.reference.exe'),
                # Extra arguments for every run of this profile (windowed and headless).
                'args': [],
                # Extra arguments for windowed runs only / headless (replay) runs only.
                'window_args': [],
                'headless_args': [],
                # Environment variables added to the process environment.
                'env': {},
                # False for an executable without the development link (an archived build): launch only.
                'dev_link': True,
            },
            'new': {
                'label': 'Deployed game - native renderer',
                'exe': str(DISC_ROOT / 'ShadowTheHedgehog.exe'),
                'args': [],
                'window_args': [],
                'headless_args': [],
                'env': {},
                'dev_link': True,
            },
        },
        # Both windows get the same client size (world visibility follows the presented aspect, so a checkpoint's
        # runs must all use one size - it is recorded with the checkpoint).
        # audio: which side plays sound (before / after / both / none); input: which side reads the gamepad
        # (XInput; the keyboard always goes to the focused window); hold: launches and reloads stop on their first
        # frame in lockstep instead of running free.
        'window': {'width': 928, 'height': 522, 'side_by_side': True, 'audio': 'after', 'input': 'both',
                   'hold': True},
        # The save-data a run starts from when its recipe names none (SRAM with the 50/60 Hz choice made, the memory
        # card); empty = the game's save-data directory.
        'seed_save_data': '',
        # Headless replays (materialising a checkpoint for a build).
        'headless': {'job_workers': 4, 'timeout_s': 1800},
        # The pad bindings copied into each session (keyboard play); empty = none.
        'pad_cfg': str(DISC_ROOT / 'pad.cfg'),
        # A memory card image for runs that start without a checkpoint card (.gci or .raw); empty = the game's
        # default card (GC/EUR/Card A beside --game-data), imported into the session's own save-data.
        'memory_card': '',
        'capture': {'diff_threshold': 8, 'amplify': 4, 'fit_before_to_after': True},
        # One-button snapshots (docs/comparator/README.md "One-button snapshots"): the key that snapshots the moment
        # in a game window (F1..F12, a letter or a digit; "none" disables it).
        'snapshot_hotkey': 'F5',
        # Recreating a snapshot's frame (RECREATE EXACT FRAME): each build replays from a state this many display
        # fields (1/59.94 s) before the snapshot, so the renderers' frame-to-frame history (motion trails, the
        # radial blur's saved screen) is rebuilt exactly as it was; 0 = from the snapshot's own state. `auto`:
        # recreate and compare as soon as a new snapshot has both builds' states.
        'recreate': {'warmup_fields': 60, 'auto': True, 'stop_margin_fields': 40},
        # Frames for "Run N frames".
        'run_frames': 300,
        # Remove session folders older than this many days at start (0 = keep).
        'keep_sessions_days': 3,
    }


def _merge(defaults: dict, loaded: dict) -> dict:
    out = copy.deepcopy(defaults)
    for key, value in loaded.items():
        if isinstance(value, dict) and isinstance(out.get(key), dict) and key != 'profiles':
            out[key] = _merge(out[key], value)
        else:
            out[key] = value
    if 'profiles' in loaded:
        profiles = {}
        for pid, prof in loaded['profiles'].items():
            base = defaults['profiles'].get(pid, defaults['profiles']['new'])
            merged = copy.deepcopy(base)
            merged.update(prof)
            profiles[pid] = merged
        out['profiles'] = profiles
    return out


def load_config(path: Path = CONFIG_PATH) -> dict:
    defaults = default_config()
    if not path.exists():
        save_config(defaults, path)
        return defaults
    with path.open('r', encoding='utf-8') as f:
        loaded = json.load(f)
    return _merge(defaults, loaded)


def save_config(config: dict, path: Path = CONFIG_PATH) -> None:
    tmp = path.with_suffix('.json.tmp')
    with tmp.open('w', encoding='utf-8', newline='\n') as f:
        json.dump(config, f, indent=2)
        f.write('\n')
    os.replace(tmp, path)


def profile_ids(config: dict) -> list:
    return list(config.get('profiles', {}).keys())
