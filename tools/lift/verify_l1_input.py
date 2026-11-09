"""Re-derive L1's evidence from the DOL (docs/lift-20261006/L1_INPUT.md; test lift_l1_evidence).

  verify_l1_input.py --dol <main.dol> [--sites extract/lift-sites.txt] [--functions extract/functions.csv]

extract/lift-sites.txt lowers 10 call sites into the lifted peripheral layer, retires 5 data words, and lowers
the 2 calls that start the disc-error screen module (docs/DISC_SYSTEM_MAP.md cut 1). The generator checks each
line's word mechanically; this script checks the claims that make the lines valid:

  1. every bl site calls the PAD / SI entry its kind replaces; every bctrl site dispatches the manager's
     implementation object (`lwz r3,0xB4(rX)`) through the vtable slot its kind replaces;
  2. PJSPeripheral::ManagerImpGc's vtable (0x8056087C) holds the expected methods, and the retired words are
     exactly its slots +0x0C..+0x18;
  3. only ManagerImpGc's constructor and destructor form the vtable's address; the constructor is called
     once, from the peripheral bring-up (0x8034AD30), which hands the object to Manager::SetImp (0x8040E92C);
  4. no other dispatch through a `+0xB4` implementation pointer uses slots +0x0C..+0x18 - so the retired
     methods are reached through the lowered sites only;
  5. PAD's OS reset record (0x80563218) names OnReset and only PADInit forms its address;
  6. every remaining caller of a PAD / SI entry is a lowered site, a ManagerImpGc method being retired, the
     PAD / SI library itself, or the VI retrace handler the native video delivery already replaced;
  7. with --pruned (a build's recomp_pruned.csv): the PAD / SI entries, OnReset and ManagerImpGc's retired
     methods are absent from the generated code, and so are the disc-error screen and its drive check and init
     (the screen's own PADControlAllMotors call goes with it). (A test under runtime/ cannot check this: every
     literal there is a liveness root and would keep the code it names alive.)

Exit 1 with every failed claim."""
from __future__ import annotations

import argparse
import bisect
import csv
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

VTABLE = 0x8056087C
CTOR = 0x80359974
DTOR = 0x8035970C
METHODS = {0x0C: 0x80359950, 0x10: 0x80359948, 0x14: 0x80359798, 0x18: 0x80359768}  # Recalibrate..GetStickRange
BRING_UP = 0x8034AD30
SET_IMP = 0x8040E92C
RESET_RECORD = 0x80563218
ON_RESET = 0x80381C44
PAD_LIBRARY = (0x80380C94, 0x80381D00)
SI_LIBRARY = (0x803B6000, 0x803B7300)
RETRACE_HANDLER = 0x8037E290  # pruned: the retrace is a native delivery (NativeVideo)
ENTRIES = {
    0x80380C94: 'PADReset', 0x80380DA4: 'PADRecalibrate', 0x80380EB8: 'PADInit', 0x80381008: 'PADRead',
    0x80381308: 'PADControlAllMotors', 0x803813D4: 'PADControlMotor', 0x803B633C: 'SIInit',
    0x803B7138: 'SISetSamplingRate', 0x803B721C: 'SIRefreshSamplingRate',
}
BL_KIND_TARGETS = {
    'peripheral_rumble': {0x803813D4},
    'peripheral_rumble_all': {0x80381308},
    'return_true': {0x80380EB8},
    'nothing': {0x803B633C, 0x803B7138, 0x8032D444, 0x8032E254},
}
# The disc-error screen module (cut 1): game main's calls of the drive check and of the module init are lowered to
# nothing; the screen (which stops the rumble itself) then has no caller. Claim 7 checks they are pruned.
DRIVE_CHECK = 0x8032D444
DISC_ERROR_INIT = 0x8032E254
DISC_ERROR_SCREEN = 0x8032D4CC
DISC_ERROR_NAMES = {DRIVE_CHECK: 'the drive check', DISC_ERROR_INIT: 'the disc-error module init'}
BCTRL_KIND_SLOTS = {'peripheral_port_count': 0x10, 'peripheral_read': 0x14, 'peripheral_stick_range': 0x18}


class Dol:
    def __init__(self, path: Path):
        raw = path.read_bytes()
        self.raw = raw
        self.sections = []
        for i in range(18):
            off = struct.unpack_from('>I', raw, i * 4)[0]
            addr = struct.unpack_from('>I', raw, 0x48 + i * 4)[0]
            size = struct.unpack_from('>I', raw, 0x90 + i * 4)[0]
            if size:
                self.sections.append((addr, off, size, i < 7))

    def word(self, address: int) -> int | None:
        for addr, off, size, _ in self.sections:
            if addr <= address < addr + size:
                return struct.unpack_from('>I', self.raw, off + address - addr)[0]
        return None

    def text_words(self):
        for addr, off, size, text in self.sections:
            if text:
                for k in range(0, size, 4):
                    yield addr + k, struct.unpack_from('>I', self.raw, off + k)[0]


def sext16(v: int) -> int:
    return v - 0x10000 if v & 0x8000 else v


def bl_target(address: int, word: int) -> int | None:
    if (word >> 26) != 18 or (word & 3) != 1:
        return None
    li = word & 0x03FFFFFC
    if li & 0x02000000:
        li -= 0x04000000
    return (address + li) & 0xFFFFFFFF


def lwz(word: int):
    """(rD, rA, displacement) of an lwz, else None."""
    if (word >> 26) != 32:
        return None
    return (word >> 21) & 31, (word >> 16) & 31, sext16(word & 0xFFFF)


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument('--dol', type=Path, required=True)
    p.add_argument('--sites', type=Path, default=ROOT / 'extract/lift-sites.txt')
    p.add_argument('--functions', type=Path, default=ROOT / 'extract/functions.csv')
    p.add_argument('--pruned', type=Path, help="the generator's recomp_pruned.csv: every deleted row must be in it")
    a = p.parse_args()
    dol = Dol(a.dol)
    failures: list[str] = []

    def claim(ok: bool, what: str):
        if not ok:
            failures.append(what)

    # ---- the lines ---------------------------------------------------------------------------------------------
    sites: dict[int, tuple[int, str]] = {}
    retired: dict[int, int] = {}
    for line in a.sites.read_text(encoding='utf-8').splitlines():
        line = line.split('#')[0].split()
        if not line:
            continue
        if line[0] == 'site':
            sites[int(line[1], 16)] = (int(line[2], 16), line[3])
        elif line[0] == 'retire':
            retired[int(line[1], 16)] = int(line[2], 16)
    claim(len(sites) == 12, f'12 sites listed: 10 peripheral, 2 disc-error (found {len(sites)})')

    # ---- 1. what each site calls ------------------------------------------------------------------------------
    words = dict(dol.text_words())
    for site, (word, kind) in sorted(sites.items()):
        claim(words.get(site) == word, f'site 0x{site:08X} holds 0x{word:08X}')
        if kind in BL_KIND_TARGETS:
            target = bl_target(site, word)
            claim(target in BL_KIND_TARGETS[kind],
                  f'site 0x{site:08X} ({kind}) calls '
                  f'{ENTRIES.get(target or 0, DISC_ERROR_NAMES.get(target or 0, hex(target or 0)))}')
        elif kind in BCTRL_KIND_SLOTS:
            window = [words.get(site - 4 * k) for k in range(1, 9)]
            loads_imp = any((d := lwz(w or 0)) and d[0] == 3 and d[2] == 0xB4 for w in window)
            slot = BCTRL_KIND_SLOTS[kind]
            loads_slot = any((d := lwz(w or 0)) and d[0] == 12 and d[1] == 12 and d[2] == slot for w in window)
            loads_vptr = any((d := lwz(w or 0)) and d[0] == 12 and d[1] == 3 and d[2] == 0 for w in window)
            claim(word == 0x4E800421 and loads_imp and loads_vptr and loads_slot,
                  f'site 0x{site:08X} ({kind}) dispatches the +0xB4 implementation through slot +0x{slot:X}')
        else:
            claim(False, f'site 0x{site:08X} has an unknown kind {kind}')

    # ---- 2. the vtable and the retired words ------------------------------------------------------------------
    claim(dol.word(VTABLE + 8) == DTOR, 'vtable +0x08 is the destructor')
    for slot, method in METHODS.items():
        claim(dol.word(VTABLE + slot) == method, f'vtable +0x{slot:X} is 0x{method:08X}')
    expected_retired = {VTABLE + slot: method for slot, method in METHODS.items()}
    expected_retired[RESET_RECORD] = ON_RESET
    claim(retired == expected_retired, 'the retired words are the vtable slots +0x0C..+0x18 and PAD\'s reset record')

    # ---- 3 / 5. who forms the vtable's and the reset record's addresses (lis + addi / ori pairs) ---------------
    rows = list(csv.DictReader(open(a.functions, encoding='utf-8', errors='replace')))
    starts = sorted((int(r['addr_hex'], 16), int(r['size'] or 0)) for r in rows)
    start_keys = [s for s, _ in starts]
    # Rows may overlap; the owner is the nearest start at or below the address whose extent holds it.
    reach_end = []
    furthest = 0
    for s, size in starts:
        furthest = max(furthest, s + max(size, 4))
        reach_end.append(furthest)

    def owner(address: int) -> int | None:
        i = bisect.bisect_right(start_keys, address) - 1
        while i >= 0 and reach_end[i] > address:
            s, size = starts[i]
            if s <= address < s + max(size, 4):
                return s
            i -= 1
        return None

    def formers(value: int) -> set[int]:
        found = set()
        recent: dict[int, tuple[int, int]] = {}  # register -> (lis value, address)
        for address, word in sorted(words.items()):
            op = word >> 26
            if op == 15 and ((word >> 16) & 31) == 0:  # lis
                recent[(word >> 21) & 31] = ((word & 0xFFFF) << 16, address)
            elif op in (14, 24):  # addi / ori
                ra = (word >> 16) & 31
                if ra in recent and address - recent[ra][1] <= 64:
                    hi = recent[ra][0]
                    v = (hi + sext16(word & 0xFFFF)) & 0xFFFFFFFF if op == 14 else hi | (word & 0xFFFF)
                    if v == value:
                        found.add(owner(address))
        return found

    claim(formers(VTABLE) <= {CTOR, DTOR}, f'only the constructor / destructor form the vtable (found {formers(VTABLE)})')
    record_formers = formers(RESET_RECORD)
    claim(all(f is not None and PAD_LIBRARY[0] <= f < PAD_LIBRARY[1] for f in record_formers),
          f'only the PAD library forms its reset record (found {record_formers})')
    claim(dol.word(RESET_RECORD) == ON_RESET, 'the reset record names OnReset')
    ctor_calls = [addr for addr, w in words.items() if bl_target(addr, w) == CTOR]
    claim(len(ctor_calls) == 1 and owner(ctor_calls[0]) == BRING_UP, f'the constructor is called once, from the bring-up ({ctor_calls})')
    bring_up_size = dict(starts).get(BRING_UP, 0)
    claim(any(bl_target(addr, words.get(addr, 0)) == SET_IMP for addr in range(BRING_UP, BRING_UP + bring_up_size, 4)),
          'the bring-up hands the object to Manager::SetImp')

    # ---- 4. no other +0xB4 dispatch uses the retired slots -----------------------------------------------------
    for address, word in sorted(words.items()):
        if word != 0x4E800421 or address in sites:
            continue
        window = [words.get(address - 4 * k) for k in range(1, 9)]
        loads_imp = any((d := lwz(w or 0)) and d[2] == 0xB4 for w in window)
        slots = {d[2] for w in window if (d := lwz(w or 0)) and d[0] == 12 and d[1] == 12}
        claim(not (loads_imp and slots & set(METHODS)),
              f'bctrl 0x{address:08X} dispatches a +0xB4 object through a retired slot {sorted(slots)}')

    # ---- 6. every remaining caller of a PAD / SI entry -----------------------------------------------------------
    retired_methods = set(METHODS.values())
    for address, word in sorted(words.items()):
        target = bl_target(address, word)
        if target not in ENTRIES or address in sites:
            continue
        caller = owner(address)
        allowed = (caller in retired_methods or caller is not None and
                   (PAD_LIBRARY[0] <= caller < PAD_LIBRARY[1] or SI_LIBRARY[0] <= caller < SI_LIBRARY[1]) or
                   caller == RETRACE_HANDLER or caller == ON_RESET or caller == DISC_ERROR_SCREEN)
        claim(allowed, f'0x{address:08X} (in 0x{(caller or 0):08X}) still calls {ENTRIES[target]}')

    # ---- 7. the deleted code is gone from the generated image (with --pruned) -----------------------------------
    if a.pruned is not None:
        pruned = {int(r['address'], 16) for r in csv.DictReader(open(a.pruned, encoding='utf-8'))}
        deleted = (set(ENTRIES) | set(METHODS.values()) | {ON_RESET, 0x803599C4} |
                   {DRIVE_CHECK, DISC_ERROR_INIT, DISC_ERROR_SCREEN})
        for address in sorted(deleted):
            claim(address in pruned, f'0x{address:08X} is pruned from the generated code')
        claim(CTOR not in pruned and DTOR not in pruned,
              "ManagerImpGc's constructor and destructor stay (translated code still creates and destroys the object)")

    if failures:
        print(f'L1 evidence: {len(failures)} claim(s) failed:')
        for f in failures:
            print('  ' + f)
        return 1
    print(f'L1 evidence: {len(sites)} sites and {len(retired)} retired words verified against {a.dol.name}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
