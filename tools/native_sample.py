"""Observe instruction pointers in an owned x64 child process, without admin rights.

This is a diagnostic wall-time sampler, NOT a CPU-time or FPS benchmark. Threads
are briefly suspended independently; waiting threads are sampled too. Samples
are not weighted by elapsed CPU time. No code, context, priority or system
policy is changed. No existing process can be attached. Use isolated game data.

Addresses are attributed only inside actual PE unwind extents, with all folded
map aliases retained. Leaf/padding/outside-image samples remain unidentified.

--stacks (OPTIMIZATION_PLAN A3): while a sampled thread is suspended, its stack
is walked with dbghelp's StackWalk64 over the process's own unwind data (the
image's .pdata, read by dbghelp from the running process), and every sample's
frames are kept. The summary then ranks image functions INCLUSIVELY (a function
counts once per sample when it is anywhere on the stack) as well as exclusively,
by guest function for generated code (fn_XXXXXXXX), and writes folded stacks
(stacks.folded: one "outer;...;inner count" line per distinct stack) for
flame-graph tools. Walking makes each suspension longer (reported); a walk that
stops early (no unwind data, damaged frame) keeps the frames it found.
"""
from __future__ import annotations

import argparse
from bisect import bisect_right
from collections import Counter
import ctypes as ct
import json
import os
from pathlib import Path
import random
import subprocess
import time

from ppcrecomp.size_audit import fingerprint, read_map, read_pe
from stage_benchmark import clean_environment
from native_renderer_benchmark import blockers


class CodeIndex:
    def __init__(self, pe, symbols):
        self.extents = pe["unwind_extents"]
        self.starts = [start for start, _ in self.extents]
        self.symbols = symbols
        self.unwind_owners = pe.get("unwind_owners", {})

    def lookup(self, rva):
        index = bisect_right(self.starts, rva) - 1
        if index < 0 or rva >= self.extents[index][1]:
            return None
        start, end = self.extents[index]
        result = {"start_rva": start, "end_rva": end,
                  "aliases": [list(pair) for pair in sorted(self.symbols.get(start, ()))]}
        if start in self.unwind_owners:
            owner = self.unwind_owners[start]
            result["unwind_owner"] = {**owner, "aliases": [list(pair) for pair in sorted(
                self.symbols.get(owner["primary_start_rva"], ()))]}
        return result


class Windows:
    """Documented Toolhelp and thread-debug APIs; AMD64 CONTEXT from WinNT.h."""
    def __init__(self):
        if os.name != "nt" or ct.sizeof(ct.c_void_p) != 8:
            raise OSError("requires 64-bit Python on Windows")
        self.kernel = ct.WinDLL("kernel32", use_last_error=True)
        self.invalid = ct.c_void_p(-1).value
        DWORD, HANDLE, BOOL = ct.c_uint32, ct.c_void_p, ct.c_int32

        class Thread(ct.Structure):
            _fields_ = [("size", DWORD), ("usage", DWORD), ("tid", DWORD), ("pid", DWORD),
                        ("priority", ct.c_int32), ("delta", ct.c_int32), ("flags", DWORD)]

        class Module(ct.Structure):
            _fields_ = [("size", DWORD), ("id", DWORD), ("pid", DWORD), ("global_usage", DWORD),
                        ("process_usage", DWORD), ("base", HANDLE), ("bytes", DWORD), ("module", HANDLE),
                        ("name", ct.c_wchar * 256), ("path", ct.c_wchar * 260)]

        self.Thread, self.Module = Thread, Module
        declarations = {
            "CreateToolhelp32Snapshot": (HANDLE, [DWORD, DWORD]),
            "Thread32First": (BOOL, [HANDLE, ct.POINTER(Thread)]),
            "Thread32Next": (BOOL, [HANDLE, ct.POINTER(Thread)]),
            "Module32FirstW": (BOOL, [HANDLE, ct.POINTER(Module)]),
            "Module32NextW": (BOOL, [HANDLE, ct.POINTER(Module)]),
            "OpenThread": (HANDLE, [DWORD, BOOL, DWORD]),
            "GetProcessIdOfThread": (DWORD, [HANDLE]),
            "SuspendThread": (DWORD, [HANDLE]), "ResumeThread": (DWORD, [HANDLE]),
            "GetThreadContext": (BOOL, [HANDLE, ct.c_void_p]),
            "GetThreadTimes": (BOOL, [HANDLE] + [ct.POINTER(ct.c_uint64)] * 4),
            "GetThreadDescription": (ct.c_int32, [HANDLE, ct.POINTER(ct.c_void_p)]),
            "LocalFree": (HANDLE, [HANDLE]),
            "WaitForSingleObject": (DWORD, [HANDLE, DWORD]),
            "CloseHandle": (BOOL, [HANDLE]),
        }
        for name, (result, arguments) in declarations.items():
            function = getattr(self.kernel, name)
            function.restype, function.argtypes = result, arguments
        # AMD64 CONTEXT is 1232 bytes, aligned to 16, with ContextFlags at 48
        # and Rip at 248. Request CONTROL only, not optional extended state.
        self.storage = ct.create_string_buffer(1232 + 15)
        self.context = (ct.addressof(self.storage) + 15) & ~15

    def threads(self, pid):
        snapshot = self.kernel.CreateToolhelp32Snapshot(4, 0)
        if snapshot == self.invalid:
            raise ct.WinError(ct.get_last_error())
        try:
            entry = self.Thread()
            entry.size = ct.sizeof(entry)
            ok = self.kernel.Thread32First(snapshot, ct.byref(entry))
            found = []
            while ok:
                if entry.pid == pid:
                    found.append(entry.tid)
                ok = self.kernel.Thread32Next(snapshot, ct.byref(entry))
            return found
        finally:
            self.kernel.CloseHandle(snapshot)

    def image(self, pid, executable):
        snapshot = self.kernel.CreateToolhelp32Snapshot(0x18, pid)
        if snapshot == self.invalid:
            return None  # The loader may not have initialized yet.
        try:
            entry = self.Module()
            entry.size = ct.sizeof(entry)
            ok = self.kernel.Module32FirstW(snapshot, ct.byref(entry))
            while ok:
                if Path(entry.path).resolve() == executable:
                    return int(entry.base), int(entry.bytes)
                ok = self.kernel.Module32NextW(snapshot, ct.byref(entry))
            return None
        finally:
            self.kernel.CloseHandle(snapshot)

    def modules(self, pid):
        """Every loaded module as sorted (base, end, name); empty before the loader is ready."""
        snapshot = self.kernel.CreateToolhelp32Snapshot(0x18, pid)
        if snapshot == self.invalid:
            return []
        try:
            entry = self.Module()
            entry.size = ct.sizeof(entry)
            found = []
            ok = self.kernel.Module32FirstW(snapshot, ct.byref(entry))
            while ok:
                found.append((int(entry.base), int(entry.base) + int(entry.bytes), entry.name))
                ok = self.kernel.Module32NextW(snapshot, ct.byref(entry))
            return sorted(found)
        finally:
            self.kernel.CloseHandle(snapshot)

    def open_thread(self, pid, tid):
        handle = self.kernel.OpenThread(0x0002 | 0x0008 | 0x0040 | 0x100000, False, tid)
        if handle and self.kernel.GetProcessIdOfThread(handle) != pid:
            self.kernel.CloseHandle(handle)
            return None
        return handle

    def times(self, handle):
        values = [ct.c_uint64() for _ in range(4)]
        if self.kernel.GetThreadTimes(handle, *(ct.byref(value) for value in values)):
            return {"created_filetime": values[0].value, "exited_filetime": values[1].value,
                    "kernel_seconds": values[2].value / 1e7, "user_seconds": values[3].value / 1e7}
        return None

    def name(self, handle):
        text = ct.c_void_p()
        if self.kernel.GetThreadDescription(handle, ct.byref(text)) < 0:
            return ""
        try:
            return ct.wstring_at(text) if text.value else ""
        finally:
            if text.value:
                self.kernel.LocalFree(text)

    def sample(self, handle, walker=None):
        """The thread's RIP (or, with a StackWalker, its list of frame addresses, innermost first)."""
        start = time.perf_counter_ns()
        previous = self.kernel.SuspendThread(handle)
        if previous == 0xffffffff:
            return None, "suspend_failed", 0
        try:
            if previous:
                return None, "already_suspended", 0
            # CONTROL only for an IP sample; CONTROL | INTEGER for a stack walk.
            ct.c_uint32.from_address(self.context + 48).value = 0x100003 if walker else 0x100001
            if not self.kernel.GetThreadContext(handle, self.context):
                return None, "context_failed", 0
            rip = ct.c_uint64.from_address(self.context + 248).value
            if walker:
                rip = walker.walk(handle, self.context)
        finally:
            if self.kernel.ResumeThread(handle) == 0xffffffff:
                raise OSError("could not undo this sampler's suspension")
        return rip, None, time.perf_counter_ns() - start


class StackWalker:
    """dbghelp StackWalk64 over a live process (documented DbgHelp API; AMD64 STACKFRAME64 from DbgHelp.h)."""
    MAX_FRAMES = 256

    def __init__(self, process_handle):
        self.process = process_handle
        self.dbghelp = ct.WinDLL("dbghelp", use_last_error=True)
        BOOL, HANDLE, DWORD64 = ct.c_int32, ct.c_void_p, ct.c_uint64

        class Address(ct.Structure):
            _fields_ = [("offset", DWORD64), ("segment", ct.c_uint16), ("mode", ct.c_uint32)]

        class KdHelp(ct.Structure):
            _fields_ = [("thread", DWORD64), ("callback_stack", ct.c_uint32), ("callback_bstore", ct.c_uint32),
                        ("next_callback", ct.c_uint32), ("frame_pointer", ct.c_uint32),
                        ("ki_call_user_mode", DWORD64), ("ke_user_callback_dispatcher", DWORD64),
                        ("system_range_start", DWORD64), ("ki_user_exception_dispatcher", DWORD64),
                        ("stack_base", DWORD64), ("stack_limit", DWORD64), ("build_version", ct.c_uint32),
                        ("retpoline_size", ct.c_uint32), ("retpoline_table", DWORD64),
                        ("retpoline_offset", ct.c_uint32), ("retpoline_stub_size", ct.c_uint32),
                        ("reserved", DWORD64 * 2)]

        class Frame(ct.Structure):
            _fields_ = [("pc", Address), ("ret", Address), ("frame", Address), ("stack", Address),
                        ("bstore", Address), ("function_table_entry", ct.c_void_p), ("params", DWORD64 * 4),
                        ("far", BOOL), ("virtual", BOOL), ("reserved", DWORD64 * 3), ("kd_help", KdHelp)]
        if ct.sizeof(Frame) != 264:
            raise OSError(f"STACKFRAME64 layout is {ct.sizeof(Frame)} bytes, expected 264")
        self.Frame = Frame
        d = self.dbghelp
        d.SymSetOptions.restype, d.SymSetOptions.argtypes = ct.c_uint32, [ct.c_uint32]
        d.SymInitializeW.restype, d.SymInitializeW.argtypes = BOOL, [HANDLE, ct.c_wchar_p, BOOL]
        d.SymRefreshModuleList.restype, d.SymRefreshModuleList.argtypes = BOOL, [HANDLE]
        d.SymCleanup.restype, d.SymCleanup.argtypes = BOOL, [HANDLE]
        d.StackWalk64.restype = BOOL
        d.StackWalk64.argtypes = [ct.c_uint32, HANDLE, HANDLE, ct.POINTER(Frame), ct.c_void_p,
                                  ct.c_void_p, ct.c_void_p, ct.c_void_p, ct.c_void_p]
        # Unwind data only: no symbol files, no prompts (SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS |
        # SYMOPT_NO_PROMPTS); the image's names come from its linker map.
        d.SymSetOptions(0x4 | 0x200 | 0x80000)
        if not d.SymInitializeW(self.process, None, False):
            raise ct.WinError(ct.get_last_error())
        self.table_access = ct.cast(d.SymFunctionTableAccess64, ct.c_void_p).value
        self.module_base = ct.cast(d.SymGetModuleBase64, ct.c_void_p).value

    def refresh(self):
        self.dbghelp.SymRefreshModuleList(self.process)  # modules load after start (drivers, runtimes)

    def walk(self, thread, context):
        frame = self.Frame()
        frame.pc.offset = ct.c_uint64.from_address(context + 248).value   # Rip
        frame.frame.offset = ct.c_uint64.from_address(context + 160).value  # Rbp
        frame.stack.offset = ct.c_uint64.from_address(context + 152).value  # Rsp
        frame.pc.mode = frame.frame.mode = frame.stack.mode = 3            # AddrModeFlat
        frames = []
        while len(frames) < self.MAX_FRAMES:
            if not self.dbghelp.StackWalk64(0x8664, self.process, thread, ct.byref(frame), context, None,
                                            self.table_access, self.module_base, None):
                break
            if frame.pc.offset == 0 or (frames and frame.pc.offset == frames[-1] and len(frames) > 1):
                break
            frames.append(frame.pc.offset)
        return frames

    def close(self):
        self.dbghelp.SymCleanup(self.process)


GUEST = __import__("re").compile(r"\?fn_([0-9A-F]{8})(?:_[A-Za-z0-9]+)?@recomp@gekko@@")


def frame_name(code, image, address, innermost):
    """A frame's label: guest function, image function, or module."""
    if not image or not image[0] <= address < image[0] + image[1]:
        return None
    # A return address points after the call; look up the call itself.
    match = code.lookup(address - image[0] - (0 if innermost else 1))
    if not match:
        return "image:unattributed"
    names = [alias[0] for alias in match["aliases"]]
    if not names and "unwind_owner" in match:
        names = [alias[0] for alias in match["unwind_owner"]["aliases"]]
    for name in names:
        guest = GUEST.match(name)
        if guest:
            return f"fn_{guest[1]}"
    return names[0] if names else f"rva:{match['start_rva']:x}"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--map", type=Path, required=True)
    parser.add_argument("--workdir", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--interval-ms", type=float, default=4)
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("--seed", type=int, default=64351)
    parser.add_argument("--thread-name", help="sample only this exact OS thread description")
    parser.add_argument("--stacks", action="store_true", help="walk each sampled stack (inclusive attribution)")
    parser.add_argument("--env", action="append", default=[], metavar="NAME=VALUE",
                        help="set one variable in the child's otherwise cleaned environment (repeatable)")
    parser.add_argument("arguments", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if not 1 <= args.interval_ms <= 100 or not 1 <= args.timeout <= 600:
        parser.error("interval must be 1..100 ms and timeout 1..600 seconds")
    exe, linker_map = args.exe.resolve(), args.map.resolve()
    if not args.workdir.is_dir() or args.out.exists():
        parser.error("requires an existing isolated working directory and a new output directory")
    pins = {"exe": fingerprint(exe), "map": fingerprint(linker_map)}
    pe = read_pe(exe.read_bytes(), include_unwind_owners=True)
    with linker_map.open(encoding="latin-1") as stream:
        code = CodeIndex(pe, read_map(stream, pe))
    windows = Windows()
    if blockers():
        parser.error("another game/build/test is active")
    args.out.mkdir(parents=True)
    arguments = args.arguments[1:] if args.arguments[:1] == ["--"] else args.arguments
    argv = [str(exe)] + arguments
    provenance = {"pins": pins, "argv": argv, "working_directory": str(args.workdir.resolve()),
                  "interval_ms": args.interval_ms, "seed": args.seed,
                  "thread_name": args.thread_name,
                  "scope": "wall-time instruction-pointer samples; not CPU percentages or benchmark timings"}
    def save(name, value):
        (args.out / name).write_text(json.dumps(value, indent=2) + "\n")
    save("provenance.json", provenance)
    handles, all_handles, names, counts, thread_counts = {}, [], {}, Counter(), Counter()
    modules, module_starts, module_counts, next_modules = [], [], Counter(), 0
    errors, failures = [], Counter()
    elapsed, suspended_ns, maximum_suspend_ns = 0.0, 0, 0
    randomizer, image, process = random.Random(args.seed), None, None
    walker, folded, inclusive, exclusive, depths = None, Counter(), Counter(), Counter(), Counter()
    stack_samples = 0

    def label(address, innermost):
        name = frame_name(code, image, address, innermost)
        if name:
            return name
        at = bisect_right(module_starts, address) - 1
        return modules[at][2] if at >= 0 and address < modules[at][1] else "no_module"
    with (args.out / "stdout.log").open("xb") as stdout, (args.out / "stderr.log").open("xb") as stderr, \
            (args.out / "samples.jsonl").open("x") as samples:
        try:
            environment = clean_environment(os.environ)
            environment.update(item.split("=", 1) for item in args.env)
            process = subprocess.Popen(argv, cwd=args.workdir, env=environment,
                                       stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr)
            provenance["pid"] = process.pid
            provenance["stacks"] = args.stacks
            save("provenance.json", provenance)
            if args.stacks:
                walker = StackWalker(int(process._handle))  # the child's own handle: full access, no attach
            start = time.perf_counter()
            next_refresh = 0
            while process.poll() is None:
                elapsed = time.perf_counter() - start
                if elapsed > args.timeout:
                    raise TimeoutError("owned diagnostic process exceeded its timeout")
                if elapsed >= next_refresh:
                    if any(pid != process.pid for pid, *_ in blockers()):
                        raise RuntimeError("another game/build/test became active")
                    image = image or windows.image(process.pid, exe)
                    for tid in windows.threads(process.pid):
                        if tid in handles and windows.kernel.WaitForSingleObject(handles[tid], 0) == 0:
                            del handles[tid]
                        if tid not in handles:
                            handle = windows.open_thread(process.pid, tid)
                            if handle:
                                handles[tid] = handle
                                all_handles.append((tid, handle))
                    for handle in handles.values():
                        names[handle] = windows.name(handle)
                    next_refresh = elapsed + 0.25
                if elapsed >= next_modules:  # drivers and runtimes load after start
                    modules = windows.modules(process.pid)
                    module_starts = [base for base, _, _ in modules]
                    if walker:
                        walker.refresh()
                    next_modules = elapsed + 2.0
                pending = []
                for tid, handle in list(handles.items()):
                    if windows.kernel.WaitForSingleObject(handle, 0) == 0:
                        del handles[tid]
                        continue
                    if args.thread_name is not None and names.get(handle) != args.thread_name:
                        continue
                    rip, error, duration = windows.sample(handle, walker)
                    if error:
                        failures[error] += 1
                        continue
                    suspended_ns += duration
                    maximum_suspend_ns = max(maximum_suspend_ns, duration)
                    frames = None
                    if walker:
                        frames = rip
                        if not frames:
                            failures["walk_empty"] += 1
                            continue
                        rip = frames[0]
                        frame_names = [label(address, i == 0) for i, address in enumerate(frames)]
                        stack_samples += 1
                        depths[min(len(frames), StackWalker.MAX_FRAMES)] += 1
                        exclusive[frame_names[0]] += 1
                        for name in set(frame_names):
                            inclusive[name] += 1
                        folded[(tid, ";".join(reversed(frame_names)))] += 1
                    rva = rip - image[0] if image and image[0] <= rip < image[0] + image[1] else None
                    match = code.lookup(rva) if rva is not None else None
                    key = match["start_rva"] if match else ("unattributed_image" if rva is not None else "outside_image")
                    counts[key] += 1
                    if rva is None:
                        at = bisect_right(module_starts, rip) - 1
                        owner = modules[at][2] if at >= 0 and rip < modules[at][1] else "no_module"
                        module_counts[(tid, owner)] += 1
                    thread_counts[(tid, key)] += 1
                    record = {"seconds": time.perf_counter() - start, "ns": time.perf_counter_ns(), "thread": tid, "rip": rip,
                              "rva": rva}
                    if frames:
                        record["frames"] = frames
                        record["names"] = frame_names
                    pending.append(record)
                for sample in pending:
                    samples.write(json.dumps(sample, separators=(",", ":")) + "\n")
                time.sleep(args.interval_ms / 1000 * randomizer.uniform(0.7, 1.3))
            if process.returncode:
                errors.append(f"process exit {process.returncode}")
        except (Exception, KeyboardInterrupt) as error:
            errors.append(f"{type(error).__name__}: {error}")
            if process is not None and process.poll() is None:
                process.kill()  # Only the child created above; never an attached game.
                process.wait()
        finally:
            thread_times = [{"thread": tid, "name": names.get(handle), "times": windows.times(handle)}
                            for tid, handle in all_handles]
            if walker:
                walker.close()
            for _, handle in all_handles:
                windows.kernel.CloseHandle(handle)
    if not image or not sum(counts.values()):
        errors.append("no valid image/samples collected")
    if any(fingerprint(path)["sha256"] != pins[key]["sha256"] for key, path in (("exe", exe), ("map", linker_map))):
        errors.append("input binary/map changed")
    rows = []
    for key, count in counts.most_common():
        row = {"samples": count, "range": code.lookup(key) if isinstance(key, int) else key,
               "by_thread": {str(tid): amount for (tid, candidate), amount in thread_counts.items() if candidate == key}}
        rows.append(row)
    result = {"errors": errors, "image": image, "exit_code": process.returncode if process else None,
              "samples": sum(counts.values()), "sampling_failures": dict(failures), "threads": thread_times,
              "suspend_total_ms": suspended_ns / 1e6, "suspend_max_ms": maximum_suspend_ns / 1e6,
              "rows": rows,
              "outside_image_by_module": [{"thread": tid, "module": owner, "samples": amount}
                                          for (tid, owner), amount in module_counts.most_common()],
              "limitations": ["Waits and runnable code both appear in wall-time sampling.",
              "Sampling pauses perturb execution; these timings are never benchmark results.",
              ("Stacks are walked with dbghelp over the process's unwind data; inclusive counts are per sample "
               "(recursion counted once)." if walker else
               "No call stacks are unwound; attribution is exclusive current-IP only."),
              "Threads shorter than the discovery interval can be missed; failures and unknown samples are retained."]}
    if walker:
        result["stacks"] = {
            "samples": stack_samples, "depths": dict(sorted(depths.items())),
            "inclusive": [{"name": name, "samples": count, "share": count / stack_samples}
                          for name, count in inclusive.most_common(400)],
            "exclusive": [{"name": name, "samples": count, "share": count / stack_samples}
                          for name, count in exclusive.most_common(200)]}
        with (args.out / "stacks.folded").open("x") as stream:
            for (tid, stack), count in folded.most_common():
                stream.write(f"thread_{tid};{stack} {count}\n")
    save("summary.json", result)
    print(f"Collected {result['samples']} wall-time IP samples; errors={errors}")
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
