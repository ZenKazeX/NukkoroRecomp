"""Read a running Dolphin's emulated GameCube memory from outside the process (measurement only).

Shadow the Hedgehog reference traces (docs/GCAX_SOUND_MAP.md): finds the emulated MEM1 (the 24 MiB region that starts
with the game id) and ARAM (by the B0_SYSTEM wave bytes GCAX places at ARAM + 0x4500) in the Dolphin process, waits for
a sound-effect id to appear in the game's SE channel table, then snapshots the GCAX context and the AX parameter blocks
every few milliseconds and writes full MEM1/ARAM images. It only reads: OpenProcess with query/read rights.

    python -B tools/dolphin_dump.py --pid <dolphin pid> --out <folder> --sound <game files/sound> [--id 0x7160]
"""

import argparse
import ctypes
import json
import os
import struct
import sys
import time
from ctypes import wintypes as w

PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010
MEM_COMMIT = 0x1000
MEM1_SIZE = 0x01800000
ARAM_SIZE = 0x01000000
SE_MANAGER = 0x80571C34        # PlaySE's manager (r3 of 0x801D5F14); +4 the 43 channels of 0x34 bytes
SE_CHANNELS, SE_CHANNEL_BYTES = 43, 0x34
GCAX_CONTEXT_POINTER = 0x805F24AC  # *(r13 + 0x5FAC)
GCAX_CONTEXT_BYTES = 0x14470
AX_PBS, AX_PB_BYTES, AX_VOICES = 0x8058FCA0, 244, 64


class MBI(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_void_p), ("AllocationBase", ctypes.c_void_p), ("AllocationProtect", w.DWORD),
                ("PartitionId", w.WORD), ("RegionSize", ctypes.c_size_t), ("State", w.DWORD), ("Protect", w.DWORD),
                ("Type", w.DWORD)]


k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.OpenProcess.restype = w.HANDLE
k32.VirtualQueryEx.argtypes = [w.HANDLE, ctypes.c_void_p, ctypes.POINTER(MBI), ctypes.c_size_t]
k32.VirtualQueryEx.restype = ctypes.c_size_t
k32.ReadProcessMemory.argtypes = [w.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                  ctypes.POINTER(ctypes.c_size_t)]


class Process:
    def __init__(self, pid):
        self.handle = k32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
        if not self.handle:
            raise OSError("cannot open process %d (error %d)" % (pid, ctypes.get_last_error()))

    def regions(self):
        address, mbi = 0, MBI()
        while k32.VirtualQueryEx(self.handle, ctypes.c_void_p(address), ctypes.byref(mbi), ctypes.sizeof(mbi)):
            if mbi.State == MEM_COMMIT:
                yield (mbi.BaseAddress or 0), mbi.RegionSize
            address = (mbi.BaseAddress or 0) + mbi.RegionSize
            if address >= 0x7FFFFFFFFFFF:
                break

    def read(self, address, size):
        buffer = ctypes.create_string_buffer(size)
        got = ctypes.c_size_t()
        if not k32.ReadProcessMemory(self.handle, ctypes.c_void_p(address), buffer, size, ctypes.byref(got)):
            return None
        return buffer.raw[:got.value]


def find_mem1(process, game_id):
    for base, size in process.regions():
        if size >= MEM1_SIZE and process.read(base, 6) == game_id:
            return base
    return None


def find_aram(process, signature):
    for base, size in process.regions():
        if size >= ARAM_SIZE and process.read(base + 0x4500, len(signature)) == signature:
            return base
    return None


class Guest:
    def __init__(self, process, mem1):
        self.process, self.mem1 = process, mem1

    def read(self, address, size):
        return self.process.read(self.mem1 + (address & 0x01FFFFFF), size)

    def u32(self, address):
        data = self.read(address, 4)
        return struct.unpack(">I", data)[0] if data else None


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--sound", required=True, help="the game's files/sound folder (B0_SYSTEM.mlt)")
    parser.add_argument("--id", type=lambda v: int(v, 0), default=0x7160)
    parser.add_argument("--game-id", default="GUPP8P")
    parser.add_argument("--wait", type=float, default=300.0, help="seconds to wait for the id")
    parser.add_argument("--window", type=float, default=2.0, help="seconds of snapshots after the id appears")
    args = parser.parse_args(argv)
    os.makedirs(args.out, exist_ok=True)

    with open(os.path.join(args.sound, "B0_SYSTEM.mlt"), "rb") as f:
        bank = f.read()
    signature = bank[0x60:0xA0]  # the first wave bytes: GCAX DMAs them to ARAM slot 0 (0x4500)

    process = Process(args.pid)
    mem1 = aram = None
    deadline = time.time() + args.wait
    while time.time() < deadline and (mem1 is None or aram is None):
        mem1 = mem1 or find_mem1(process, args.game_id.encode())
        aram = aram or find_aram(process, signature)
        if mem1 is None or aram is None:
            time.sleep(0.5)
    if mem1 is None:
        raise SystemExit("MEM1 not found")
    print("MEM1 at 0x%X, ARAM at %s" % (mem1, "0x%X" % aram if aram else "not found"), flush=True)
    guest = Guest(process, mem1)

    # Wait for the id in the SE channel table (channel +0x12: the id it plays).
    start = time.time()
    trigger = None
    while time.time() < deadline:
        channels = guest.u32(SE_MANAGER + 4)
        if channels and 0x80000000 <= channels < 0x81800000:
            table = guest.read(channels, SE_CHANNELS * SE_CHANNEL_BYTES) or b""
            for c in range(SE_CHANNELS):
                row = table[c * SE_CHANNEL_BYTES:(c + 1) * SE_CHANNEL_BYTES]
                if len(row) == SE_CHANNEL_BYTES and struct.unpack(">H", row[0x12:0x14])[0] == args.id:
                    trigger = {"channel": c, "channel_address": channels + c * SE_CHANNEL_BYTES,
                               "wall_s": time.time() - start, "row": row.hex()}
                    break
        if trigger:
            break
        time.sleep(0.002)
    if not trigger:
        raise SystemExit("id 0x%04X never appeared in the SE channel table" % args.id)
    print("id 0x%04X on SE channel %d after %.3f s" % (args.id, trigger["channel"], trigger["wall_s"]), flush=True)

    # Snapshots: the GCAX context and the AX parameter blocks, as fast as reading allows.
    snapshots = open(os.path.join(args.out, "snapshots.bin"), "wb")
    index = []
    t0 = time.time()
    while time.time() - t0 < args.window:
        context = guest.u32(GCAX_CONTEXT_POINTER)
        ctx = guest.read(context, GCAX_CONTEXT_BYTES) if context else None
        pbs = guest.read(AX_PBS, AX_VOICES * AX_PB_BYTES)
        if ctx and pbs:
            index.append({"t": time.time() - t0, "offset": snapshots.tell(), "context": context})
            snapshots.write(ctx + pbs)
        time.sleep(0.003)
    snapshots.close()
    with open(os.path.join(args.out, "mem1.raw"), "wb") as f:
        f.write(guest.read(0x80000000, MEM1_SIZE))
    if aram:
        with open(os.path.join(args.out, "aram.raw"), "wb") as f:
            f.write(process.read(aram, ARAM_SIZE))
    with open(os.path.join(args.out, "dump.json"), "w") as f:
        json.dump({"mem1_host": mem1, "aram_host": aram, "trigger": trigger, "snapshots": index,
                   "snapshot_layout": {"gcax_context_bytes": GCAX_CONTEXT_BYTES, "ax_pbs": AX_PBS,
                                       "ax_pb_bytes": AX_PB_BYTES, "voices": AX_VOICES}}, f, indent=1)
    print("%d snapshots, MEM1%s written to %s" % (len(index), " + ARAM" if aram else "", args.out), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
