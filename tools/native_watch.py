"""Attribute writes in an owned diagnostic game using x64 hardware watchpoints.

The matching PE/map and compiler class-layout report pin the observed boundary.
An execution breakpoint at the selected function obtains its Cpu argument; up
to four aligned data watchpoints then observe the specified flat-window spans.
No executable bytes or game values are patched. All debug exceptions not owned
by this tool are passed to the program. Never use this run as a benchmark.
"""
import argparse
import ctypes as ct
from contextlib import nullcontext
import gzip
import json
from pathlib import Path
import re
import struct
import subprocess
import time

from native_sample import Windows, CodeIndex
from ppcrecomp.size_audit import fingerprint, read_map, read_pe
from stage_benchmark import clean_environment
from native_renderer_benchmark import blockers
import os


def watch_spec(text):
    address, separator, size = text.partition(":")
    address, size = int(address, 0), int(size) if separator else 4
    if size not in (1, 2, 4, 8) or address < 0 or address + size > 0x100000000 or address % size:
        raise ValueError("watch spans must be 1/2/4/8 bytes, aligned and within the 32-bit window")
    return address, size


def capture_spec(text):
    address, separator, size = text.partition(":")
    address, size = int(address, 0), int(size) if separator else 16
    if address < 0 or not 0 < size <= 4096 or address + size > 0x100000000:
        raise ValueError("capture span must contain 1..4096 bytes within the 32-bit window")
    return address, size


def until_spec(text):
    index, link, count = (int(part, 0) for part in text.split(":"))
    if not 0 <= index < 4 or not 0 <= link < 0x100000000 or count < 1:
        raise ValueError("terminal watch requires INDEX:LINK:positive COUNT")
    return index, link, count


def layout_fields(path):
    text = path.read_text(encoding="utf-8-sig")
    match = re.search(r"class gekko::Cpu\s+size\((\d+)\):", text)
    if not match:
        raise ValueError("missing compiler Cpu layout")
    fields = {}
    for name in ("gpr", "fpr", "lr", "fpscr", "fm_window_"):
        matches = re.findall(r"^\s*(\d+)\s+\| " + re.escape(name) + r"\s*$", text[match.end():], re.M)
        if len(matches) != 1:
            raise ValueError(f"ambiguous compiler layout field {name}")
        fields[name] = int(matches[0])
    fields["size"] = int(match[1])
    for name, width in (("gpr", 128), ("fpr", 512), ("lr", 4), ("fpscr", 4), ("fm_window_", 8)):
        if fields[name] + width > fields["size"]:
            raise ValueError(f"compiler layout field {name} exceeds Cpu size")
    return fields


def register_values(read, cpu, fields):
    """Exact raw register values at the stopped write event, without conversion."""
    gpr = struct.unpack("<32I", read(cpu + fields["gpr"], 128))
    fpr = struct.unpack("<64Q", read(cpu + fields["fpr"], 512))
    return {"gpr": [f"{v:08x}" for v in gpr],
            "fpr_lanes": [[f"{fpr[i]:016x}", f"{fpr[i+1]:016x}"] for i in range(0, 64, 2)],
            **{name: f"{struct.unpack('<I', read(cpu + fields[name], 4))[0]:08x}" for name in ("lr", "fpscr")}}


class Debugger(Windows):
    def __init__(self):
        super().__init__()
        D, P, Q = ct.c_uint32, ct.c_void_p, ct.c_uint64

        class ExceptionRecord(ct.Structure):
            _fields_ = [("code", D), ("flags", D), ("record", P), ("address", P),
                        ("count", D), ("information", Q * 15)]

        class ExceptionInfo(ct.Structure):
            _fields_ = [("record", ExceptionRecord), ("first_chance", D)]

        class CreateProcess(ct.Structure):
            _fields_ = [("file", P), ("process", P), ("thread", P), ("base", P),
                        ("debug_offset", D), ("debug_size", D), ("tls", P),
                        ("start", P), ("name", P), ("unicode", ct.c_uint16)]

        class CreateThread(ct.Structure):
            _fields_ = [("thread", P), ("tls", P), ("start", P)]

        class LoadDll(ct.Structure):
            _fields_ = [("file", P), ("base", P), ("debug_offset", D), ("debug_size", D),
                        ("name", P), ("unicode", ct.c_uint16)]

        class Data(ct.Union):
            _fields_ = [("exception", ExceptionInfo), ("create_process", CreateProcess),
                        ("create_thread", CreateThread), ("load_dll", LoadDll), ("exit_code", D)]

        class Event(ct.Structure):
            _fields_ = [("code", D), ("pid", D), ("tid", D), ("data", Data)]

        self.Event = Event
        if ct.sizeof(Event) != 176:
            raise ValueError("unexpected x64 DEBUG_EVENT layout")
        declarations = {
            "WaitForDebugEvent": (ct.c_int32, [ct.POINTER(Event), D]),
            "ContinueDebugEvent": (ct.c_int32, [D, D, D]),
            "SetThreadContext": (ct.c_int32, [P, P]),
            "ReadProcessMemory": (ct.c_int32, [P, P, P, ct.c_size_t, ct.POINTER(ct.c_size_t)]),
        }
        for name, (result, arguments) in declarations.items():
            function = getattr(self.kernel, name)
            function.restype, function.argtypes = result, arguments

    def read(self, process, address, size):
        buffer, read = ct.create_string_buffer(size), ct.c_size_t()
        if not self.kernel.ReadProcessMemory(process, address, buffer, size, ct.byref(read)) or read.value != size:
            raise ct.WinError(ct.get_last_error())
        return buffer.raw

    def context_for(self, thread, flags=0x100013):
        ct.c_uint32.from_address(self.context + 48).value = flags
        if not self.kernel.GetThreadContext(thread, self.context):
            raise ct.WinError(ct.get_last_error())
        return {"rcx": ct.c_uint64.from_address(self.context + 128).value,
                "rip": ct.c_uint64.from_address(self.context + 248).value,
                "dr6": ct.c_uint64.from_address(self.context + 104).value}

    def arm(self, thread, locations, writing):
        # Debug events already suspend the process; no SuspendThread here.
        self.context_for(thread, 0x100010)
        dr7 = 0
        for i in range(4):
            address, size = locations[i] if i < len(locations) else (0, 1)
            ct.c_uint64.from_address(self.context + 72 + i * 8).value = address
            if i < len(locations) and address:
                dr7 |= 1 << (2 * i)
                if writing:
                    dr7 |= (1 | ({1: 0, 2: 1, 4: 3, 8: 2}[size] << 2)) << (16 + 4 * i)
        ct.c_uint64.from_address(self.context + 104).value = 0
        ct.c_uint64.from_address(self.context + 112).value = dr7
        if not self.kernel.SetThreadContext(thread, self.context):
            raise ct.WinError(ct.get_last_error())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("exe", "map", "layout", "workdir", "out"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--entry-symbol", required=True, help="unique substring in a code symbol")
    parser.add_argument("--expected-link", type=lambda value: int(value, 0), required=True)
    parser.add_argument("--watch", action="append", type=watch_spec, required=True)
    parser.add_argument("--timeout", type=float, default=300)
    parser.add_argument("--registers", action="store_true", help="record raw Cpu register values at each write event")
    parser.add_argument("--cpu-field", choices=("fpscr", "lr"), action="append", default=[],
                        help="also watch a compiler-layout-pinned four-byte Cpu field")
    parser.add_argument("--cpu-mask", type=lambda value: int(value, 0), default=0xffffffff,
                        help="record Cpu-field events only when these bits change; all hardware events remain counted")
    parser.add_argument("--cpu-once", action="store_true", help="disarm each Cpu-field watch after its first recorded change")
    parser.add_argument("--capture-span", type=capture_spec, action="append", default=[],
                        help="read this guest span at CPU or terminal events; diagnostic evidence only")
    parser.add_argument("--until-watch-link", type=until_spec,
                        help="disarm all watches after INDEX:LINK:COUNT matching guest writes; game continues")
    storage = parser.add_mutually_exclusive_group()
    storage.add_argument("--compress", action="store_true", help="write a lossless gzip event stream")
    storage.add_argument("--summary-only", action="store_true",
                         help="retain exact counts and last writes only; no chronological event stream")
    parser.add_argument("arguments", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if len(args.watch) + len(args.cpu_field) > 4 or not 0 < args.cpu_mask <= 0xffffffff or args.out.exists() or not args.workdir.is_dir():
        parser.error("requires 1..4 watchpoints, a new output directory and an existing isolated working directory")
    if args.until_watch_link and args.until_watch_link[0] >= len(args.watch):
        parser.error("terminal watch index must refer to a guest watch")
    exe = args.exe.resolve()
    pins = {name: fingerprint(getattr(args, name)) for name in ("exe", "map", "layout")}
    fields = layout_fields(args.layout)
    watches = [("guest", address, size, None) for address, size in args.watch]
    watches += [("cpu", fields[name], 4, name) for name in args.cpu_field]
    pe = read_pe(exe.read_bytes())
    with args.map.open(encoding="latin-1") as stream:
        symbols = read_map(stream, pe)
    matches = [rva for rva, aliases in symbols.items() if any(args.entry_symbol in name for name, _ in aliases)]
    if len(matches) != 1:
        parser.error(f"entry symbol must identify one address, found {len(matches)}")
    code, debugger = CodeIndex(pe, symbols), Debugger()
    if blockers():
        parser.error("another game/build/test is active")
    args.out.mkdir(parents=True)
    arguments = args.arguments[1:] if args.arguments[:1] == ["--"] else args.arguments
    argv = [str(exe)] + arguments
    provenance = {"pins": pins, "argv": argv, "working_directory": str(args.workdir.resolve()),
                  "fields": fields, "entry_rva": matches[0], "entry_symbol": args.entry_symbol, "watch": args.watch,
                  "record_registers": args.registers,
                  "cpu_fields": args.cpu_field, "cpu_change_mask": args.cpu_mask,
                  "cpu_once": args.cpu_once, "capture_spans": args.capture_span,
                  "until_watch_link": args.until_watch_link, "compressed_events": args.compress,
                  "summary_only": args.summary_only,
                  "scope": "hardware write watchpoints in an owned process; instrumentation, never a benchmark"}
    def save(name, value):
        (args.out / name).write_text(json.dumps(value, indent=2) + "\n")
    errors, threads, last = [], {}, {}
    base = window = cpu = None
    writes, events, filtered = 0, 0, 0
    prior_cpu_values = {}
    disabled = set()
    terminal_matches = 0
    process = pending = None
    initial_break = False
    start = time.monotonic()
    def locations():
        return [(0, 1) if i in disabled else ((window if space == "guest" else cpu) + address, size)
                for i, (space, address, size, _) in enumerate(watches)]
    trace_path = args.out / ("writes.jsonl.gz" if args.compress else "writes.jsonl")
    trace_stream = (nullcontext(None) if args.summary_only else
                    gzip.open(trace_path, "xt", encoding="utf-8", compresslevel=1) if args.compress else trace_path.open("x"))
    with (args.out / "stdout.log").open("xb") as stdout, (args.out / "stderr.log").open("xb") as stderr, \
            trace_stream as trace:
        try:
            process = subprocess.Popen(argv, cwd=args.workdir, env=clean_environment(os.environ),
                                       stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr, creationflags=2)
            provenance["pid"] = process.pid
            save("provenance.json", provenance)
            handle, next_check = None, 0
            while True:
                elapsed = time.monotonic() - start
                if elapsed > args.timeout:
                    raise TimeoutError("owned debugger run timed out")
                if elapsed > next_check:
                    if any(pid != process.pid for pid, *_ in blockers()):
                        raise RuntimeError("another game/build/test became active")
                    next_check = elapsed + 1
                event = debugger.Event()
                if not debugger.kernel.WaitForDebugEvent(ct.byref(event), 100):
                    if ct.get_last_error() == 121:
                        continue
                    raise ct.WinError(ct.get_last_error())
                pending = event
                events += 1
                status, finished = 0x10002, False  # DBG_CONTINUE
                if event.pid != process.pid:
                    raise RuntimeError("unexpected debugged process")
                if event.code == 3:  # CREATE_PROCESS_DEBUG_EVENT
                    info = event.data.create_process
                    handle, base = info.process, int(info.base)
                    threads[event.tid] = info.thread
                    if info.file:
                        debugger.kernel.CloseHandle(info.file)
                    debugger.arm(info.thread, [(base + matches[0], 1)], False)
                elif event.code == 2:  # CREATE_THREAD_DEBUG_EVENT
                    thread = event.data.create_thread.thread
                    threads[event.tid] = thread
                    debugger.arm(thread, locations() if window else [(base + matches[0], 1)], bool(window))
                elif event.code == 4:  # Windows closes the debug thread handle after ContinueDebugEvent.
                    threads.pop(event.tid, None)
                elif event.code == 5:
                    provenance["exit_code"] = int(event.data.exit_code)
                    finished = True
                elif event.code == 6 and event.data.load_dll.file:
                    debugger.kernel.CloseHandle(event.data.load_dll.file)
                elif event.code == 1:
                    exception = event.data.exception.record.code
                    status = 0x80010001  # Let the program handle unowned exceptions, including its memory VEH.
                    if exception == 0x80000003 and not initial_break:
                        initial_break, status = True, 0x10002
                    elif exception == 0x80000004:
                        context = debugger.context_for(threads[event.tid])
                        if context["dr6"] & 15:
                            status = 0x10002
                            if window is None:
                                if context["rip"] != base + matches[0]:
                                    raise RuntimeError("unexpected execution breakpoint")
                                cpu = context["rcx"]
                                link, = struct.unpack("<I", debugger.read(handle, cpu + fields["lr"], 4))
                                if link != args.expected_link:
                                    raise RuntimeError("Cpu ABI/link validation failed")
                                window, = struct.unpack("<Q", debugger.read(handle, cpu + fields["fm_window_"], 8))
                                provenance.update(cpu=cpu, window=window, image_base=base, armed_seconds=elapsed)
                                if args.registers:
                                    provenance["entry_registers"] = register_values(
                                        lambda at, size: debugger.read(handle, at, size), cpu, fields)
                                    entry_stack = int(provenance["entry_registers"]["gpr"][1], 16)
                                    provenance["entry_stack"] = {"address": entry_stack,
                                        "hex": debugger.read(handle, window + entry_stack, 32).hex()}
                                save("provenance.json", provenance)
                                for i, (space, address, size, _) in enumerate(watches):
                                    if space == "cpu":
                                        prior_cpu_values[i] = int.from_bytes(debugger.read(handle, cpu + address, size), "little")
                                provenance["initial_cpu_values"] = prior_cpu_values.copy()
                                save("provenance.json", provenance)
                                for thread in threads.values():
                                    debugger.arm(thread, locations(), True)
                            else:
                                stack, = struct.unpack("<I", debugger.read(handle, cpu + fields["gpr"] + 4, 4))
                                rva = context["rip"] - base
                                for i, (space, address, size, field) in enumerate(watches):
                                    if not context["dr6"] & (1 << i):
                                        continue
                                    writes += 1
                                    raw = debugger.read(handle, (window if space == "guest" else cpu) + address, size)
                                    if space == "cpu":
                                        value = int.from_bytes(raw, "little")
                                        changed = (value ^ prior_cpu_values[i]) & args.cpu_mask
                                        prior_cpu_values[i] = value
                                        if not changed:
                                            filtered += 1
                                            continue
                                    row = {"seconds": elapsed, "thread": event.tid, "watch_index": i,
                                           "address": address, "address_space": space, "field": field, "after_hex": raw.hex(),
                                           "stack_pointer": stack,
                                           "link_register": struct.unpack("<I", debugger.read(handle, cpu + fields["lr"], 4))[0],
                                           "rip_after_instruction": context["rip"],
                                           "rva_after_instruction": rva, "range": code.lookup(rva - 1)}
                                    if args.registers:
                                        row["registers"] = register_values(lambda at, size: debugger.read(handle, at, size), cpu, fields)
                                    terminal = False
                                    if args.until_watch_link and space == "guest" and i == args.until_watch_link[0]:
                                        link, = struct.unpack("<I", debugger.read(handle, cpu + fields["lr"], 4))
                                        if link == args.until_watch_link[1]:
                                            terminal_matches += 1
                                            terminal = terminal_matches == args.until_watch_link[2]
                                    if space == "cpu" or terminal:
                                        row["captured_spans"] = [{"address": at, "hex": debugger.read(handle, window + at, count).hex()}
                                                                 for at, count in args.capture_span]
                                        if terminal or (space == "cpu" and args.cpu_once):
                                            disabled.update(range(len(watches)) if terminal else (i,))
                                            for thread in threads.values():
                                                debugger.arm(thread, locations(), True)
                                    if trace is not None:
                                        trace.write(json.dumps(row, separators=(",", ":")) + "\n")
                                    last[i] = row
                                debugger.arm(threads[event.tid], locations(), True)
                if not debugger.kernel.ContinueDebugEvent(event.pid, event.tid, status):
                    raise ct.WinError(ct.get_last_error())
                pending = None
                if finished:
                    process.wait(timeout=5)
                    break
        except (Exception, KeyboardInterrupt) as error:
            errors.append(f"{type(error).__name__}: {error}")
        finally:
            if pending is not None:
                debugger.kernel.ContinueDebugEvent(pending.pid, pending.tid, 0x80010001 if pending.code == 1 else 0x10002)
            if process is not None and process.poll() is None:
                process.kill()  # Only the process launched here.
                # Termination can require draining its debug exit events.
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline and process.poll() is None:
                    event = debugger.Event()
                    if debugger.kernel.WaitForDebugEvent(ct.byref(event), 100):
                        debugger.kernel.ContinueDebugEvent(event.pid, event.tid, 0x10002)
    if window is None:
        errors.append("entry breakpoint never armed the memory watches")
    if args.until_watch_link and terminal_matches < args.until_watch_link[2]:
        errors.append("requested terminal watch occurrence was not reached")
    if process is None or process.poll() != 0:
        errors.append(f"game exit {process.poll() if process else None}")
    if any(fingerprint(getattr(args, name))["sha256"] != pin["sha256"] for name, pin in pins.items()):
        errors.append("a pinned input changed")
    save("summary.json", {"errors": errors, "events": events, "writes": writes, "filtered_cpu_events": filtered,
                         "disarmed_watch_indices": sorted(disabled), "terminal_matches": terminal_matches,
                         "recorded_writes": 0 if args.summary_only else writes - filtered,
                         "matching_writes": writes - filtered, "last_writes": last,
                         "limitations": ["Only these virtual-address spans and hardware write events are observed.",
                         "Writes through a separate alias mapping are outside this watch scope.",
                         "The recorded RIP is after the triggering instruction; ranges use RIP-1.",
                         "Cpu-field records cover changes in the explicit mask only; total events include filtered writes.",
                         "Summary-only mode retains final writes and counts, not a chronological event stream.",
                         "This attributes writes, not all subsequent reads or complete behavioral equivalence."]})
    print(f"Observed {writes} writes; errors={errors}")
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
