"""Named comparison checkpoints (docs/comparator/README.md "Checkpoints").

A checkpoint is a LOGICAL game state, defined by how it is reached - its recipe - and materialised once per build in
that build's own savestate format:

    comparison_states/<name>/
        checkpoint.json            the recipe, the boundary tick, the per-build states and their reports
        input.tape                 the controller input from the recipe's root to the checkpoint (if any)
        route.uitest               a scripted-input route (boot-stage recipes, if any)
        states/<profile>.gekkostate        that build's own savestate (version 19, its renderer kind)
        states/<profile>.report.json       that build's state report at the checkpoint (probes + memory digest)
        states/<profile>.save-data/        the SRAM and memory card that build had at the checkpoint
        states/<profile>.warmup.gekkostate a state some fields before the checkpoint (recreating its frame with the
                                           renderers' frame-to-frame history rebuilt; recipe "warmup_tick")
        captures/                  captures taken at this checkpoint
        reference/                 the frame the build being played showed at a one-button snapshot (live-<profile>)
        renders/                   frames recreated from the snapshot (RECREATE EXACT FRAME / RENDER ...)
        snapshot.json              a one-button snapshot's record from the game (made by, ticks, the reference)

The recipe's root is a cold boot (optionally straight into a stage, the story or a scripted route) or another
checkpoint; the tape is replayed from the root, and the run stops at the boundary after the host service at
`stop_tick` - where the checkpoint was saved. Two builds whose game time is the game's own reach the same boundary
with the same game state from the same root and input; each saves it in its own format, and their state reports are
compared to prove it.
"""
from __future__ import annotations

import datetime
import json
import os
import re
import shutil
import threading
from pathlib import Path

FORMAT = 'shadow-comparison-checkpoint 1'
NAME_RE = re.compile(r'^[A-Za-z0-9][A-Za-z0-9_.-]{0,99}$')


def now_iso() -> str:
    return datetime.datetime.now().replace(microsecond=0).isoformat()


def valid_name(name: str) -> bool:
    return bool(NAME_RE.match(name or '')) and name not in ('seed', 'sessions') and not name.startswith('.')


class Checkpoint:
    def __init__(self, path: Path, meta: dict):
        self.path = path
        self.meta = meta

    @property
    def name(self) -> str:
        return self.meta.get('name', self.path.name)

    @property
    def title(self) -> str:
        """The readable label (a one-button snapshot's automatic one, or what the user gave it)."""
        return self.meta.get('title') or self.name

    @property
    def recipe(self) -> dict:
        return self.meta.setdefault('recipe', {})

    @property
    def states(self) -> dict:
        return self.meta.setdefault('states', {})

    @property
    def saved_tick(self):
        return self.meta.get('saved_tick')

    def file(self, relative):
        return self.path / relative if relative else None

    def state_file(self, profile: str) -> Path:
        return self.path / 'states' / f'{profile}.gekkostate'

    def report_file(self, profile: str) -> Path:
        return self.path / 'states' / f'{profile}.report.json'

    def save_data_dir(self, profile: str) -> Path:
        return self.path / 'states' / f'{profile}.save-data'

    def warmup_file(self, profile: str) -> Path:
        return self.path / 'states' / f'{profile}.warmup.gekkostate'

    def has_warmup(self, profile: str) -> bool:
        return bool((self.states.get(profile) or {}).get('warmup')) and self.warmup_file(profile).exists()

    def reference_image(self):
        """The frame the build being played showed at the snapshot (PNG), if any."""
        ref = self.meta.get('reference') or {}
        path = self.path / ref['file'] if ref.get('file') else None
        return path if path is not None and path.exists() else None

    def has_state(self, profile: str) -> bool:
        return profile in self.states and self.state_file(profile).exists()

    def report(self, profile: str):
        path = self.report_file(profile)
        if not path.exists():
            return None
        try:
            return json.loads(path.read_text(encoding='utf-8'))
        except (OSError, ValueError):
            return None

    def describe_root(self) -> str:
        root = self.recipe.get('root', {})
        kind = root.get('kind')
        if kind == 'boot-stage':
            text = f"boot into {root.get('stage')}"
        elif kind == 'boot-story':
            text = 'boot into the story'
        elif kind == 'cold-boot':
            text = 'cold boot'
        elif kind == 'checkpoint':
            text = f"checkpoint '{root.get('name')}'"
        else:
            text = 'unknown root'
        if self.recipe.get('ui_test_script'):
            text += ' + scripted input'
        if self.recipe.get('tape'):
            text += ' + recorded input'
        return text

    def save(self):
        self.meta['format'] = FORMAT
        tmp = self.path / 'checkpoint.json.tmp'
        with tmp.open('w', encoding='utf-8', newline='\n') as f:
            json.dump(self.meta, f, indent=2)
            f.write('\n')
        os.replace(tmp, self.path / 'checkpoint.json')


class CheckpointStore:
    def __init__(self, root: Path):
        self._seed_lock = threading.Lock()
        self.root = Path(root)
        self.root.mkdir(parents=True, exist_ok=True)
        (self.root / '.sessions').mkdir(exist_ok=True)
        (self.root / '.trash').mkdir(exist_ok=True)

    @property
    def sessions_dir(self) -> Path:
        return self.root / '.sessions'

    @property
    def seed_dir(self) -> Path:
        return self.root / '.seed' / 'save-data'

    def ensure_seed(self, candidates) -> Path:
        """The save-data every run starts from when its recipe names no other (SRAM with the 60 Hz choice made, the
        memory card): copied once from the first candidate that exists (read only). Thread-safe: replays of both
        builds start together."""
        with self._seed_lock:
            return self._ensure_seed(candidates)

    def _ensure_seed(self, candidates) -> Path:
        if self.seed_dir.exists() and any(self.seed_dir.iterdir()):
            return self.seed_dir
        for candidate in candidates:
            candidate = Path(candidate)
            if candidate.is_dir() and any(p.name.endswith('.pc.raw') for p in candidate.iterdir()):
                self.seed_dir.mkdir(parents=True, exist_ok=True)
                for item in candidate.iterdir():
                    # The SRAM settings image and memory card working images (and their source stamps) only.
                    if item.is_file() and (item.name.endswith('.pc.raw') or item.name.endswith('.pc.raw.source')):
                        shutil.copy2(item, self.seed_dir / item.name)
                return self.seed_dir
        self.seed_dir.mkdir(parents=True, exist_ok=True)
        return self.seed_dir

    def list(self):
        out = []
        for path in sorted(self.root.iterdir(), key=lambda p: p.name.lower()):
            if not path.is_dir() or path.name.startswith('.'):
                continue
            meta_path = path / 'checkpoint.json'
            if not meta_path.exists():
                continue
            try:
                meta = json.loads(meta_path.read_text(encoding='utf-8'))
            except (OSError, ValueError):
                continue
            out.append(Checkpoint(path, meta))
        return out

    def get(self, name: str):
        path = self.root / name
        if not (path / 'checkpoint.json').exists():
            return None
        return Checkpoint(path, json.loads((path / 'checkpoint.json').read_text(encoding='utf-8')))

    def create(self, name: str, recipe: dict, description: str = '') -> Checkpoint:
        if not valid_name(name):
            raise ValueError(f"'{name}' is not a valid checkpoint name (letters, digits, _ - .)")
        path = self.root / name
        if path.exists():
            raise ValueError(f"a checkpoint named '{name}' exists")
        (path / 'states').mkdir(parents=True)
        (path / 'captures').mkdir()
        cp = Checkpoint(path, {'format': FORMAT, 'name': name, 'description': description, 'created': now_iso(),
                               'recipe': recipe, 'saved_tick': None, 'states': {}})
        cp.save()
        return cp

    def rename(self, old: str, new: str) -> Checkpoint:
        if not valid_name(new):
            raise ValueError(f"'{new}' is not a valid checkpoint name")
        src, dst = self.root / old, self.root / new
        if dst.exists():
            raise ValueError(f"a checkpoint named '{new}' exists")
        os.replace(src, dst)
        cp = self.get(new)
        cp.meta['name'] = new
        cp.save()
        # Children name their root by name.
        for other in self.list():
            root = other.recipe.get('root', {})
            if root.get('kind') == 'checkpoint' and root.get('name') == old:
                root['name'] = new
                other.save()
        return cp

    def delete(self, name: str) -> Path:
        """Moves the checkpoint into .trash (recoverable); returns where it went."""
        src = self.root / name
        stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
        dst = self.root / '.trash' / f'{name}-{stamp}'
        os.replace(src, dst)
        return dst

    def children(self, name: str):
        return [cp for cp in self.list()
                if cp.recipe.get('root', {}).get('kind') == 'checkpoint' and cp.recipe['root'].get('name') == name]
