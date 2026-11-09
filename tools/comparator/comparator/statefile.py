"""Reading a build's savestate file (version 19) as the game's memory, for the workstation's semantic readout
(docs/comparator/README.md "Snapshot semantics").

The workstation may understand both builds' worlds; the game does not describe itself. A savestate holds the whole
game - its address window (every region with its committed bytes, in the game's own big-endian byte order) - so the
player, the stage objects, the task tree and the cameras of a snapshot can be read out of its file at any time, also
long after it was made (a better probe applies to every snapshot already in the library).

Layout (runtime/src/gekko/savestate.cpp save_state): the header {u32 magic 'SKGK', u32 version, u64 game ticks,
u32 renderer kind}, the CPU's three length-prefixed blocks {u64 size, bytes} (architected state, interrupt FPR state,
the timeline), then the address window: u32 region count, per region {u32 base, u32 size, u32 committed, u8 kind,
u32 name length, name, committed bytes}. Everything after it (the audio sample store, the devices, the renderer's
block) is not read here. Host integers are little-endian; guest memory is the game's big-endian bytes.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass
from pathlib import Path

import numpy as np

MAGIC = b'SKGK'


@dataclass(frozen=True)
class Region:
    name: str
    base: int
    size: int
    committed: int
    kind: int
    offset: int      # where its bytes start in the file


class StateFileError(ValueError):
    pass


class GuestImage:
    """The game's memory at a savestate: read by guest address."""

    def __init__(self, path):
        self.path = Path(path)
        self.data = self.path.read_bytes()
        data = self.data
        if len(data) < 20 or data[:4] != MAGIC:
            raise StateFileError(f'{self.path} is not a savestate')
        self.version, self.game_ticks = struct.unpack_from('<IQ', data, 4)
        if self.version < 17:
            raise StateFileError(f'{self.path}: savestate version {self.version} has no address-window layout')
        offset = 16
        self.renderer_kind = 0
        if self.version >= 19:
            self.renderer_kind = struct.unpack_from('<I', data, 16)[0]
            offset = 20
        for _block in range(3):
            if offset + 8 > len(data):
                raise StateFileError(f'{self.path}: truncated CPU block')
            size = struct.unpack_from('<Q', data, offset)[0]
            offset += 8 + size
        count = struct.unpack_from('<I', data, offset)[0]
        offset += 4
        if count > 64:
            raise StateFileError(f'{self.path}: implausible region count {count}')
        self.regions = []
        for _region in range(count):
            base, size, committed, kind = struct.unpack_from('<IIIB', data, offset)
            offset += 13
            name_length = struct.unpack_from('<I', data, offset)[0]
            offset += 4
            name = data[offset:offset + name_length].decode('utf-8', errors='replace')
            offset += name_length
            if offset + committed > len(data):
                raise StateFileError(f'{self.path}: region {name} is truncated')
            self.regions.append(Region(name, base, size, committed, kind, offset))
            offset += committed
        self._words = {}

    # ---- reads ----
    def region_of(self, address: int):
        for region in self.regions:
            if region.base <= address < region.base + region.committed:
                return region
        return None

    def read(self, address: int, length: int):
        """The bytes at a guest address, or None when the range is not entirely inside one region."""
        region = self.region_of(address)
        if region is None or address + length > region.base + region.committed:
            return None
        start = region.offset + (address - region.base)
        return self.data[start:start + length]

    def u32(self, address: int):
        raw = self.read(address, 4)
        return struct.unpack('>I', raw)[0] if raw is not None else None

    def s32(self, address: int):
        raw = self.read(address, 4)
        return struct.unpack('>i', raw)[0] if raw is not None else None

    def u16(self, address: int):
        raw = self.read(address, 2)
        return struct.unpack('>H', raw)[0] if raw is not None else None

    def u8(self, address: int):
        raw = self.read(address, 1)
        return raw[0] if raw is not None else None

    def f32(self, address: int):
        raw = self.read(address, 4)
        return struct.unpack('>f', raw)[0] if raw is not None else None

    def vec3(self, address: int):
        raw = self.read(address, 12)
        return struct.unpack('>3f', raw) if raw is not None else None

    def cstr(self, address: int, limit: int = 96):
        raw = self.read(address, limit)
        if raw is None:
            region = self.region_of(address)
            if region is None:
                return None
            raw = self.read(address, region.base + region.committed - address)
        end = raw.find(b'\0')
        text = raw[:end if end >= 0 else len(raw)]
        try:
            return text.decode('ascii')
        except UnicodeDecodeError:
            return None

    def pointer(self, address: int):
        """The word at `address` when it points into mapped memory, else None."""
        value = self.u32(address)
        return value if value is not None and value != 0 and self.region_of(value) is not None else None

    # ---- searches ----
    def words(self, region: Region):
        if region.name not in self._words:
            count = region.committed // 4
            self._words[region.name] = np.frombuffer(self.data, dtype='>u4', count=count, offset=region.offset)
        return self._words[region.name]

    def find_u32(self, value: int, regions=None):
        """Every 4-aligned guest address holding `value`."""
        out = []
        for region in self.regions:
            if regions is not None and region.name not in regions:
                continue
            hits = np.nonzero(self.words(region) == np.uint32(value))[0]
            out.extend(int(region.base + 4 * h) for h in hits)
        return out

    def find_any_u32(self, values, regions=None):
        """{value: [addresses]} for many values at once (e.g. every class's vtable)."""
        wanted = np.array(sorted(set(int(v) for v in values)), dtype=np.uint32)
        out = {int(v): [] for v in wanted}
        for region in self.regions:
            if regions is not None and region.name not in regions:
                continue
            words = self.words(region)
            mask = np.isin(words, wanted)
            for h in np.nonzero(mask)[0]:
                out[int(words[h])].append(int(region.base + 4 * h))
        return out
