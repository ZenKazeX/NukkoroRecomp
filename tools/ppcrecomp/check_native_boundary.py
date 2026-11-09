import argparse
import io
import json
import ntpath
from pathlib import Path
import re
import stat
import struct
import subprocess
import sys


# Positive controls: the native core's dispatcher, reset and the game clock's
# wait point (2026-10-05 native time: there is no instruction clock, so the
# retirement member that used to be the third control no longer exists).
NATIVE_REQUIRED = frozenset(("run", "reset", "idle_wait"))
# run_steps is the reference interpreter's dispatch-count bound for tests;
# production has no bound but a stop, so it is forbidden there.
REFERENCE_REQUIRED = frozenset((
    "step", "execute", "verify_dispatch", "op_group4", "op_group19",
    "op_group31", "op_group59", "op_group63", "run_steps",
))
FORBIDDEN = REFERENCE_REQUIRED | frozenset((
    "op_psq_load", "op_psq_store", "quantise_load", "quantise_store",
    "unimplemented", "open_interpreter_strand", "close_interpreter_strand",
))
INTERPRETER_MARKERS = (
    b"unimplemented %s instruction 0x%08X",
    b"recompiler verification mismatch: ",
)
DUMPBIN_TIMEOUT = 120
CHUNK_SIZE = 65536
MAX_TEXT_LINE = 1048576
MAX_DETAIL = 384
MAX_ERRORS = 8
MAX_REFERENCE_NAMES = 8
CPU_MEMBER = re.compile(r"\?([A-Za-z_][A-Za-z_0-9]*)@Cpu@gekko@@[A-Z][A-Za-z_0-9?@$]*Z")
# The legacy GX register raster (Flipper EFB/TEV/copy emulation) was deleted on
# 2026-10-05 from BOTH runtime archives, with the Bus/Cpu coherence machinery
# that existed only for it. Any MSVC-decorated name scoped in, constructing, or
# typed by one of its owners - gekko::Renderer, PlumeGxRasterizer, FrameCapture,
# PresentedFrameObservation, or the gekko::gxshaders raster shader table - is
# legacy raster code. The preceding character excludes unrelated identifiers
# that merely end in the same word (NativeRenderer, BloomRenderer).
LEGACY_RASTER_OWNERS = ("Renderer", "PlumeGxRasterizer", "FrameCapture",
                        "PresentedFrameObservation", "gxshaders")
LEGACY_RASTER_SYMBOL = re.compile(
    r"(?:^|[?@0-9A-Z_$])(" + "|".join(LEGACY_RASTER_OWNERS) + r")@gekko@@")
# Members of the deleted coherence protocol (the RAM-access observer's guard
# sets and bitmaps, the per-range translation hooks and the Cpu's tagged and
# store page tables). Reported as owner "coherence:<member>".
LEGACY_COHERENCE_MEMBERS = (
    "set_ram_access_observer", "clear_ram_access_observer", "set_ram_access_observer_active",
    "observe_ram_access", "set_pending_dma_ranges", "pending_dma_overlaps", "set_source_read_ranges",
    "source_read_overlaps", "set_guard_bitmap", "set_range_invalidation", "set_guard_elision",
    "tag_host_pages_for_physical", "untag_host_pages_for_physical", "rederive_write_pages",
    "host_page_pointer_tagged", "host_page_pointer_write")
LEGACY_COHERENCE_SYMBOL = re.compile(
    r"\?(" + "|".join(LEGACY_COHERENCE_MEMBERS) + r")@(?:Bus|Cpu)@gekko@@")
MAX_LEGACY_EXAMPLES = 8
TEXT_CONTROL = re.compile(r"[\x00-\x08\x0b\x0c\x0e-\x1f\x7f]")
LIBRARY_HEADER = re.compile(r"[ \t]*File Type:[ \t]+LIBRARY[ \t]*")
SYMBOL_COUNT = re.compile(r"[ \t]*([0-9]+)[ \t]+public symbols[ \t]*")
LIBRARY_SYMBOL = re.compile(
    r"[ \t]*[0-9A-Fa-f]+[ \t]+([^ \t\r\n]+)(?:[ \t]+\([^\r\n]*\))?[ \t]*"
)
PROBE_DIAGNOSTIC = re.compile(r"\b(?:fatal error|error|warning)[ \t]+(?:[A-Z]+[0-9]+|:)", re.IGNORECASE)
MAP_HEADER = re.compile(r"[ \t]*Address[ \t]+Publics by Value[ \t]+Rva\+Base[ \t]+Lib:Object[ \t]*")
MAP_BASE = re.compile(r"[ \t]*Preferred load address is ([0-9A-Fa-f]{8}|[0-9A-Fa-f]{16})[ \t]*")
MAP_SECTION = re.compile(
    r"[ \t]*([0-9A-Fa-f]{4}):([0-9A-Fa-f]{8,16})[ \t]+([0-9A-Fa-f]+)H[ \t]+\S+[ \t]+(CODE|DATA)[ \t]*"
)
MAP_SYMBOL = re.compile(
    r"[ \t]*([0-9A-Fa-f]{4}):([0-9A-Fa-f]{8,16})[ \t]+(\S+)[ \t]+"
    r"([0-9A-Fa-f]{8}|[0-9A-Fa-f]{16})[ \t]+(\S[^\r\n]*?)[ \t]*"
)
MAP_ADDRESS = re.compile(r"[ \t]*[0-9A-Fa-f]{4}:[0-9A-Fa-f]+")


class OperationalError(Exception):
    pass


class BoundaryArgumentParser(argparse.ArgumentParser):
    def error(self, message):
        raise OperationalError(message)


def bounded_text(value, limit=MAX_DETAIL):
    value = str(value)
    shortened = value[:limit]
    shortened = " ".join("".join(c if c.isprintable() else " " for c in shortened).split())
    return shortened + ("..." if len(value) > limit else "")


def checked_text(value, label):
    if isinstance(value, bytes):
        value = value.decode("latin-1")
    if not isinstance(value, str) or TEXT_CONTROL.search(value):
        raise OperationalError(f"{label}: invalid or binary text")
    return value


def cpu_member(symbol):
    match = CPU_MEMBER.fullmatch(symbol)
    return match.group(1) if match else None


def legacy_raster_owner(symbol):
    coherence = LEGACY_COHERENCE_SYMBOL.search(symbol)
    if coherence:
        return "coherence:" + coherence.group(1)
    match = LEGACY_RASTER_SYMBOL.search(symbol)
    return match.group(1) if match else None


def legacy_raster_census(output):
    """Legacy-raster owners among a library's linker-member symbols.

    Reads the same dumpbin /LINKERMEMBER:1 text parse_linker_members has
    already validated; returns {owner: count} plus bounded examples."""
    output = checked_text(output, "dumpbin output")
    owners = {}
    examples = []
    for line in output.split("\n"):
        match = LIBRARY_SYMBOL.fullmatch(line.rstrip("\r"))
        if not match:
            continue
        owner = legacy_raster_owner(match.group(1))
        if owner is None:
            continue
        owners[owner] = owners.get(owner, 0) + 1
        if len(examples) < MAX_LEGACY_EXAMPLES:
            examples.append(bounded_text(match.group(1), 160))
    return owners, examples


def parse_linker_members(output):
    output = checked_text(output, "dumpbin output")
    if PROBE_DIAGNOSTIC.search(output):
        raise OperationalError("dumpbin reported an error or warning")
    library_header = False
    count = None
    remaining = 0
    summary = False
    members = set()
    for line in output.split("\n"):
        line = line.rstrip("\r")
        if line.lstrip().startswith("File Type:"):
            if library_header or not LIBRARY_HEADER.fullmatch(line):
                raise OperationalError("dumpbin did not report one MSVC LIBRARY")
            library_header = True
            continue
        match = SYMBOL_COUNT.fullmatch(line)
        if match:
            if not library_header or count is not None:
                raise OperationalError("dumpbin has an unexpected linker-member table")
            count = remaining = int(match.group(1))
            continue
        if count is None or not line.strip():
            continue
        if remaining:
            match = LIBRARY_SYMBOL.fullmatch(line)
            if not match:
                raise OperationalError("dumpbin linker-member table is malformed or truncated")
            name = cpu_member(match.group(1))
            if name:
                members.add(name)
            remaining -= 1
        elif line.strip() == "Summary":
            summary = True
        elif not summary:
            raise OperationalError("dumpbin has unexpected data after its linker-member table")
        else:
            match = LIBRARY_SYMBOL.fullmatch(line)
            if match and cpu_member(match.group(1)):
                raise OperationalError("dumpbin has Cpu symbols outside its linker-member table")
    if not library_header or not count or remaining:
        raise OperationalError("dumpbin produced no complete, nonempty linker-member table")
    return count, frozenset(members)


def find_byte_markers(stream, markers=INTERPRETER_MARKERS, chunk_size=CHUNK_SIZE):
    markers = tuple(dict.fromkeys(markers))
    if not isinstance(chunk_size, int) or chunk_size <= 0:
        raise ValueError("chunk_size must be a positive integer")
    if any(not isinstance(marker, bytes) or not marker for marker in markers):
        raise ValueError("markers must be nonempty bytes")
    overlap = max((len(marker) - 1 for marker in markers), default=0)
    tail = b""
    found = set()
    while True:
        chunk = stream.read(chunk_size)
        if not isinstance(chunk, bytes):
            raise OperationalError("marker scan requires a binary stream")
        if not chunk:
            break
        window = tail + chunk
        found.update(marker for marker in markers if marker in window)
        tail = window[-overlap:] if overlap else b""
    return tuple(marker for marker in markers if marker in found)


def validate_archive(stream):
    if stream.read(8) != b"!<arch>\n":
        raise OperationalError("library is not a regular MSVC COFF archive")
    header = stream.read(60)
    if len(header) != 60 or header[:16].strip() != b"/" or header[58:] != b"`\n":
        raise OperationalError("library has no valid first linker member")
    size_field = header[48:58].strip()
    if not size_field.isdigit():
        raise OperationalError("library has an invalid linker-member size")
    member_size = int(size_field)
    stream.seek(0, io.SEEK_END)
    if member_size < 4 or 68 + member_size > stream.tell():
        raise OperationalError("library has a truncated linker member")


def validate_pe(stream):
    stream.seek(0, io.SEEK_END)
    size = stream.tell()
    stream.seek(0)
    dos = stream.read(64)
    if len(dos) != 64 or dos[:2] != b"MZ":
        raise OperationalError("executable has no valid DOS/PE header")
    pe_offset = struct.unpack_from("<I", dos, 60)[0]
    if pe_offset < 64 or pe_offset + 24 > size:
        raise OperationalError("executable has an invalid PE header offset")
    stream.seek(pe_offset)
    header = stream.read(24)
    if len(header) != 24 or header[:4] != b"PE\0\0":
        raise OperationalError("executable has no complete PE signature and file header")
    machine, sections, _, _, _, optional_size, flags = struct.unpack("<HHIIIHH", header[4:])
    if not machine or not 1 <= sections <= 96 or not flags & 2 or flags & 0x2000:
        raise OperationalError("input is not a PE executable image")
    optional = stream.read(optional_size)
    if len(optional) != optional_size or optional_size < 2:
        raise OperationalError("executable has a truncated optional header")
    magic = struct.unpack_from("<H", optional)[0]
    minimum = {0x10b: 96, 0x20b: 112}.get(magic)
    if minimum is None or optional_size < minimum:
        raise OperationalError("executable has an unsupported PE optional header")
    table_end = pe_offset + 24 + optional_size + sections * 40
    headers_size = struct.unpack_from("<I", optional, 60)[0]
    if not table_end <= headers_size <= size:
        raise OperationalError("executable has a truncated section table or invalid header size")
    executable_data = False
    for _ in range(sections):
        section = stream.read(40)
        if len(section) != 40:
            raise OperationalError("executable has a truncated section table")
        raw_size, raw_offset = struct.unpack_from("<II", section, 16)
        attributes = struct.unpack_from("<I", section, 36)[0]
        if raw_size and (raw_offset < headers_size or raw_offset + raw_size > size):
            raise OperationalError("executable has truncated or overlapping section data")
        executable_data |= bool(raw_size and attributes & 0x20000000)
    if not executable_data:
        raise OperationalError("executable has no executable section data")
    stream.seek(0)


def reference_member_pattern(reference_library=None):
    alternatives = [
        r"(?:[A-Za-z0-9_.-]+[_-])?(?:runtime[_-]reference|reference[_-]runtime)"
        r"(?:[_-][A-Za-z0-9_.-]+)?(?:\.lib)?"
    ]
    if reference_library is not None:
        name = ntpath.basename(str(reference_library))
        alternatives.extend(re.escape(alias) for alias in {name, ntpath.splitext(name)[0]})
    return re.compile(
        r"(?:^|[\\/\s\"'(])(" + "|".join(alternatives) + r")(?=:[ \t]*[^ \t\r\n]+|\([^\r\n)]+\))",
        re.IGNORECASE,
    )


def bounded_lines(stream):
    while True:
        line = stream.readline(MAX_TEXT_LINE + 1)
        if not line:
            return
        if len(line) > MAX_TEXT_LINE:
            raise OperationalError("linker map contains an overlong line")
        yield line


def parse_linker_map(lines, reference_library=None):
    if isinstance(lines, (str, bytes)):
        lines = checked_text(lines, "linker map").split("\n")
    reference_pattern = reference_member_pattern(reference_library)
    preferred_base = None
    publics = False
    code_ranges = []
    symbol_count = 0
    native_run = False
    forbidden = set()
    legacy_owners = {}
    legacy_examples = []
    reference_names = set()
    reference_count = 0
    for line in lines:
        if len(line) > MAX_TEXT_LINE:
            raise OperationalError("linker map contains an overlong line")
        line = checked_text(line, "linker map").rstrip("\r\n")
        for token in line.split():
            name = cpu_member(token)
            if name in FORBIDDEN:
                forbidden.add(name)
            owner = legacy_raster_owner(token)
            if owner is not None:
                legacy_owners[owner] = legacy_owners.get(owner, 0) + 1
                if len(legacy_examples) < MAX_LEGACY_EXAMPLES:
                    legacy_examples.append(bounded_text(token, 160))
        for match in reference_pattern.finditer(line):
            reference_count += 1
            if len(reference_names) < MAX_REFERENCE_NAMES:
                reference_names.add(bounded_text(match.group(1), 160))
        match = MAP_BASE.fullmatch(line)
        if match:
            if preferred_base is not None:
                raise OperationalError("linker map has duplicate preferred load addresses")
            preferred_base = int(match.group(1), 16)
            continue
        if MAP_HEADER.fullmatch(line):
            if publics:
                raise OperationalError("linker map has duplicate public-symbol tables")
            publics = True
            continue
        match = MAP_SECTION.fullmatch(line)
        if match and not publics:
            if match.group(4) == "CODE":
                code_ranges.append(tuple(int(match.group(i), 16) for i in (1, 2, 3)))
            continue
        if not publics:
            continue
        match = MAP_SYMBOL.fullmatch(line)
        if not match:
            if MAP_ADDRESS.match(line):
                raise OperationalError("linker map has a malformed symbol row")
            continue
        symbol_count += 1
        segment, offset = int(match.group(1), 16), int(match.group(2), 16)
        address = int(match.group(4), 16)
        fields = match.group(5).split()
        flags = set()
        while fields and fields[0] in ("f", "i"):
            flags.add(fields.pop(0))
        if not fields:
            raise OperationalError("linker map symbol row has no object or archive origin")
        if cpu_member(match.group(3)) == "run" and "f" in flags and fields[0] != "<absolute>":
            native_run |= (
                preferred_base is not None and address > preferred_base and segment != 0
                and any(segment == s and start <= offset < start + length for s, start, length in code_ranges)
            )
    if preferred_base is None or not publics or not code_ranges or not symbol_count:
        raise OperationalError("linker map lacks recognized MSVC headers, code sections, or symbol rows")
    return {
        "symbol_count": symbol_count,
        "native_run_symbol": native_run,
        "forbidden_members": sorted(forbidden),
        "legacy_raster_owners": dict(sorted(legacy_owners.items())),
        "legacy_raster_examples": legacy_examples,
        "reference_archive_matches": reference_count,
        "reference_archives": sorted(reference_names),
    }


def input_path(value, label):
    try:
        path = Path(value).resolve(strict=True)
        if not stat.S_ISREG(path.stat().st_mode):
            raise OperationalError(f"{label} is not a regular file: {path}")
    except (OSError, RuntimeError, ValueError) as error:
        raise OperationalError(f"{label}: {error}") from error
    return path


def inspect_library(dumpbin, library):
    count, members, _ = inspect_library_symbols(dumpbin, library)
    return count, members


def inspect_library_symbols(dumpbin, library):
    with library.open("rb") as stream:
        validate_archive(stream)
    try:
        result = subprocess.run(
            [str(dumpbin), "/NOLOGO", "/LINKERMEMBER:1", str(library)],
            capture_output=True, check=False, timeout=DUMPBIN_TIMEOUT,
        )
    except subprocess.TimeoutExpired as error:
        raise OperationalError(f"dumpbin timed out after {DUMPBIN_TIMEOUT}s for {library}") from error
    except (OSError, subprocess.SubprocessError) as error:
        raise OperationalError(f"cannot run dumpbin for {library}: {error}") from error
    if result.returncode != 0:
        detail = result.stderr or result.stdout or b"no diagnostic output"
        if isinstance(detail, bytes):
            detail = detail[:MAX_DETAIL].decode("latin-1")
        raise OperationalError(f"dumpbin failed with exit {result.returncode} for {library}: {bounded_text(detail)}")
    if result.stderr and result.stderr.strip():
        raise OperationalError("dumpbin wrote diagnostics to stderr despite exit 0")
    count, members = parse_linker_members(result.stdout)
    return count, members, legacy_raster_census(result.stdout)


def library_report(path, count, members, required, legacy=({}, [])):
    return {
        "path": bounded_text(path, 240),
        "symbol_count": count,
        "required_members": sorted(required & members),
        "missing_required_members": sorted(required - members),
        "forbidden_members": sorted(FORBIDDEN & members),
        "legacy_raster_owners": dict(sorted(legacy[0].items())),
        "legacy_raster_examples": list(legacy[1]),
    }


def main(argv=None):
    parser = BoundaryArgumentParser(
        description="Check MSVC native/reference archive boundaries; executable strings are supplemental evidence.",
        allow_abbrev=False,
    )
    parser.add_argument("--dumpbin", required=True, type=Path)
    parser.add_argument("--native-library", required=True, type=Path)
    parser.add_argument("--reference-library", type=Path)
    parser.add_argument("--executable", type=Path)
    parser.add_argument("--map", dest="linker_map", type=Path)
    report = {"status": "error", "native_library": None, "reference_library": None, "executable": None, "map": None}
    errors = []
    exit_code = 2
    try:
        args = parser.parse_args(argv)
        dumpbin = input_path(args.dumpbin, "dumpbin")
        native = input_path(args.native_library, "native-library")
        reference = input_path(args.reference_library, "reference-library") if args.reference_library is not None else None
        executable = input_path(args.executable, "executable") if args.executable is not None else None
        linker_map = input_path(args.linker_map, "map") if args.linker_map is not None else None
        if reference is not None and reference.samefile(native):
            raise OperationalError("reference-library must be separate from native-library")
        count, members, legacy = inspect_library_symbols(dumpbin, native)
        report["native_library"] = library_report(native, count, members, NATIVE_REQUIRED, legacy)
        if FORBIDDEN & members:
            errors.append("native-library contains forbidden Cpu members: " + ", ".join(sorted(FORBIDDEN & members)))
        if legacy[0]:
            errors.append("native-library contains legacy GX raster symbols: " + ", ".join(sorted(legacy[0])))
        if NATIVE_REQUIRED - members:
            errors.append("native-library lacks required Cpu members: " + ", ".join(sorted(NATIVE_REQUIRED - members)))
        if reference is not None:
            count, members, legacy = inspect_library_symbols(dumpbin, reference)
            report["reference_library"] = library_report(reference, count, members, REFERENCE_REQUIRED, legacy)
            if REFERENCE_REQUIRED - members:
                errors.append("reference-library positive control lacks Cpu members: " + ", ".join(sorted(REFERENCE_REQUIRED - members)))
            if legacy[0]:
                errors.append("reference-library contains legacy GX raster symbols: " + ", ".join(sorted(legacy[0])))
        if executable is not None:
            with executable.open("rb") as stream:
                validate_pe(stream)
                markers = find_byte_markers(stream)
            report["executable"] = {
                "path": bounded_text(executable, 240),
                "diagnostic_markers": [marker.decode("ascii") for marker in markers],
                "evidence": "ASCII diagnostic markers only; their absence does not prove code absence",
            }
            if markers:
                errors.append("executable contains interpreter-only diagnostic markers: " + ", ".join(marker.decode("ascii") for marker in markers))
        if linker_map is not None:
            with linker_map.open("rb") as stream:
                evidence = parse_linker_map(bounded_lines(stream), reference)
            evidence["path"] = bounded_text(linker_map, 240)
            report["map"] = evidence
            if evidence["forbidden_members"]:
                errors.append("map contains forbidden Cpu members: " + ", ".join(evidence["forbidden_members"]))
            if evidence["legacy_raster_owners"]:
                errors.append("map contains legacy GX raster symbols: " + ", ".join(evidence["legacy_raster_owners"]))
            if evidence["reference_archive_matches"]:
                errors.append("map contains reference-runtime archive members: " + ", ".join(evidence["reference_archives"]))
            if not evidence["native_run_symbol"]:
                errors.append("map lacks a real Cpu::run function symbol in a CODE section")
        exit_code = 1 if errors else 0
        report["status"] = "boundary_violation" if errors else "passed"
    except (OperationalError, OSError, RuntimeError, ValueError) as error:
        errors.append(str(error))
    report["errors"] = [bounded_text(error) for error in errors[:MAX_ERRORS]]
    report["exit_code"] = exit_code
    print(json.dumps(report, sort_keys=True, ensure_ascii=True, separators=(",", ":")))
    for error in report["errors"]:
        print("native boundary: " + error, file=sys.stderr)
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
