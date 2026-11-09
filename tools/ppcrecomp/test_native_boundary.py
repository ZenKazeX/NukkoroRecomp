import contextlib
import io
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch

if __package__:
    from . import check_native_boundary as boundary
else:
    import check_native_boundary as boundary


NATIVE = (
    "?run@Cpu@gekko@@QEAAXXZ",
    "?reset@Cpu@gekko@@QEAAXXZ",
    "?idle_wait@Cpu@gekko@@QEAAXXZ",
)
STD_STEP = "?step@std@@YAXXZ"
ALLOWED = (
    STD_STEP,
    "?set_verify_recompiled@Cpu@gekko@@QEAAX_N@Z",
    "?quantise_load@ppc@gekko@@YAMIII@Z",
    "?quantise_store@ppc@gekko@@YAXIMII@Z",
    "?stepper@Cpu@gekko@@QEAAXXZ",
)


def cpu_symbol(name):
    return "?" + name + "@Cpu@gekko@@AEAAXI@Z"


# Decorated names the deleted legacy GX raster contributed to an archive.
LEGACY_RASTER = (
    "??0Renderer@gekko@@QEAA@AEAVBus@1@AEAVGraphicsPipe@1@AEAVState@display@shadow@@@Z",
    "?flush@Renderer@gekko@@QEAAXXZ",
    "?create@PlumeGxRasterizer@gekko@@QEAA_N_NI@Z",
    "?kEmbeddedShaders@gxshaders@gekko@@3QBUEmbeddedShader@12@B",
)
# Members of the deleted Bus/Cpu coherence machinery.
LEGACY_COHERENCE = (
    "?observe_ram_access@Bus@gekko@@QEAAXI_K_N@Z",
    "?set_pending_dma_ranges@Bus@gekko@@QEAAXV?$vector@UPhysicalRange@Bus@gekko@@V?$allocator@UPhysicalRange@Bus@gekko@@@std@@@std@@@Z",
    "?tag_host_pages_for_physical@Cpu@gekko@@QEAAXII@Z",
)
# Current names that must NOT match the coherence pattern.
NOT_LEGACY_COHERENCE = (
    "?set_mem1_access_trace@Bus@gekko@@QEAAXV?$function@$$A6AXI_K_N@Z@std@@@Z",
    "?host_page_pointer@Cpu@gekko@@AEAAPEAEII@Z",
    "?try_install_host_page@Cpu@gekko@@AEAAXII@Z",
)
# Names that merely contain the word and are NOT the legacy raster.
NOT_LEGACY_RASTER = (
    "?start@NativeRenderer@gekko@@QEAA_NAEBUNativeRendererConfig@2@@Z",
    "??0BloomRenderer@gekko@@QEAA@XZ",
    "?kEmbeddedShaders@presentshaders@gekko@@3QBUEmbeddedShader@12@B",
    "?renderer@System@gekko@@QEAAAEAVNativeRenderer@2@XZ",
)


def dumpbin_output(symbols=NATIVE):
    return (
        "Dump of file synthetic.lib\r\n\r\nFile Type: LIBRARY\r\n\r\n"
        "Archive member name at 8: /\r\n"
        f"    {len(symbols)} public symbols\r\n\r\n"
        + "".join(f"    {0x100 + index * 0x100:8X} {symbol}\r\n" for index, symbol in enumerate(symbols))
        + "\r\n  Summary\r\n        100 .text\r\n"
    )


def completed(symbols=NATIVE, returncode=0, stdout=None, stderr=b""):
    if stdout is None:
        stdout = dumpbin_output(symbols).encode("ascii")
    return subprocess.CompletedProcess([], returncode, stdout, stderr)


def archive_bytes():
    body = struct.pack(">I", 0)
    header = (
        b"/".ljust(16) + b"0".ljust(12) + b"0".ljust(6) + b"0".ljust(6)
        + b"0".ljust(8) + str(len(body)).encode("ascii").ljust(10) + b"`\n"
    )
    return b"!<arch>\n" + header + body


def pe_bytes(payload=b"", pe64=True):
    raw = bytearray(1024)
    raw[:2] = b"MZ"
    struct.pack_into("<I", raw, 60, 0x80)
    raw[0x80:0x84] = b"PE\0\0"
    optional_size = 240 if pe64 else 224
    struct.pack_into("<HHIIIHH", raw, 0x84, 0x8664 if pe64 else 0x14c, 1, 0, 0, 0, optional_size, 0x22)
    optional = 0x98
    struct.pack_into("<H", raw, optional, 0x20b if pe64 else 0x10b)
    struct.pack_into("<II", raw, optional + 16, 0x1000, 0x1000)
    struct.pack_into("<Q" if pe64 else "<I", raw, optional + (24 if pe64 else 28), 0x140000000 if pe64 else 0x400000)
    struct.pack_into("<II", raw, optional + 32, 0x1000, 0x200)
    struct.pack_into("<II", raw, optional + 56, 0x2000, 0x200)
    struct.pack_into("<I", raw, optional + (108 if pe64 else 92), 16)
    section = optional + optional_size
    raw[section:section + 8] = b".text\0\0\0"
    struct.pack_into("<IIII", raw, section + 8, 0x200, 0x1000, 0x200, 0x200)
    struct.pack_into("<I", raw, section + 36, 0x60000020)
    return bytes(raw) + payload


def changed(raw, offset, data):
    return raw[:offset] + data + raw[offset + len(data):]


def map_row(symbol, origin="shadow_runtime:cpu.obj", flags="f", segment=1, offset=0x20, address=0x140001020):
    return f" {segment:04x}:{offset:08x}       {symbol} {address:016x} {flags} {origin}\n"


def map_text(extra=(), run=True):
    return (
        " ShadowTheHedgehog\n\n Timestamp is 12345678\n\n"
        " Preferred load address is 0000000140000000\n\n"
        " Start         Length     Name                   Class\n"
        " 0001:00000000 00001000H .text$mn                CODE\n"
        " 0002:00000000 00000200H .rdata                  DATA\n\n"
        "  Address         Publics by Value              Rva+Base               Lib:Object\n\n"
        + (map_row(NATIVE[0]) if run else "")
        + map_row(STD_STEP, origin="msvcprt:stl.obj", flags="f i")
        + "".join(extra)
        + "\n entry point at        0001:00000020\n\n Static symbols\n\n"
    )


class SymbolTests(unittest.TestCase):
    def test_exact_cpu_member_names(self):
        self.assertEqual([boundary.cpu_member(symbol) for symbol in NATIVE], ["run", "reset", "idle_wait"])
        for name in boundary.FORBIDDEN:
            with self.subTest(name=name):
                self.assertEqual(boundary.cpu_member(cpu_symbol(name)), name)

    def test_wrong_scopes_substrings_import_names_and_data_are_not_cpu_methods(self):
        symbols = (
            STD_STEP, "std::step", "gekko::Cpu::step", "?step@Other@gekko@@QEAAXXZ",
            "?step@Cpu@other@@QEAAXXZ", "?step@Cpu@gekko_extra@@QEAAXXZ",
            "__imp_?step@Cpu@gekko@@QEAAXXZ", "prefix?step@Cpu@gekko@@QEAAXXZ",
            "?run@Cpu@gekko@@", "?run@Cpu@gekko@@2HA",
            "??$holder@$1?step@Cpu@gekko@@QEAAXXZ@std@@YAXXZ",
        )
        for symbol in symbols:
            with self.subTest(symbol=symbol):
                self.assertIsNone(boundary.cpu_member(symbol))

    def test_native_symbols_and_allowed_helpers(self):
        count, members = boundary.parse_linker_members(dumpbin_output(NATIVE + ALLOWED))
        self.assertEqual(count, len(NATIVE + ALLOWED))
        self.assertTrue(boundary.NATIVE_REQUIRED <= members)
        self.assertIn("set_verify_recompiled", members)
        self.assertFalse(boundary.FORBIDDEN & members)

    def test_reference_symbols_supply_positive_control(self):
        symbols = NATIVE + tuple(cpu_symbol(name) for name in boundary.REFERENCE_REQUIRED)
        count, members = boundary.parse_linker_members(dumpbin_output(symbols).encode("ascii"))
        self.assertEqual(count, len(symbols))
        self.assertTrue(boundary.REFERENCE_REQUIRED <= members)

    def test_demangled_annotations_and_non_ascii_paths(self):
        output = dumpbin_output((NATIVE[0] + " (public: unsigned __int64 gekko::Cpu::run(unsigned __int64))",) + NATIVE[1:])
        output = output.replace("synthetic.lib", "caf\xe9.lib")
        self.assertEqual(boundary.parse_linker_members(output.encode("latin-1"))[1], boundary.NATIVE_REQUIRED)

    def test_malformed_empty_truncated_and_wrong_probe_outputs_fail_closed(self):
        valid = dumpbin_output()
        bad_outputs = (
            b"", "nonsense", "\n".join(NATIVE), dumpbin_output(()),
            valid.replace("LIBRARY", "EXECUTABLE"), valid.replace("3 public symbols", "three public symbols"),
            valid.replace("3 public symbols", "4 public symbols"), valid.replace("3 public symbols", "2 public symbols"),
            valid + "\x00", valid.encode("utf-16-le"), "File Type: LIBRARY\n" + valid,
            valid + "3 public symbols\n", valid + " 100 " + cpu_symbol("step") + "\n",
            "DUMPBIN : warning LNK4003: invalid library\n" + valid,
            valid.replace(NATIVE[1], ""),
        )
        for output in bad_outputs:
            with self.subTest(output=repr(output)[:120]):
                with self.assertRaises(boundary.OperationalError):
                    boundary.parse_linker_members(output)


class LegacyRasterSymbolTests(unittest.TestCase):
    def test_owner_recognition_is_exact(self):
        owners = [boundary.legacy_raster_owner(symbol) for symbol in LEGACY_RASTER]
        self.assertEqual(owners, ["Renderer", "Renderer", "PlumeGxRasterizer", "gxshaders"])
        for symbol in NOT_LEGACY_RASTER:
            with self.subTest(symbol=symbol):
                self.assertIsNone(boundary.legacy_raster_owner(symbol))
        # Type references inside other decorated names are legacy raster code too.
        self.assertEqual(boundary.legacy_raster_owner("??1?$unique_ptr@VRenderer@gekko@@U?$default_delete@VRenderer@gekko@@@std@@@std@@QEAA@XZ"),
                         "Renderer")
        self.assertEqual(boundary.legacy_raster_owner("??_GFrameCapture@gekko@@QEAAPEAXI@Z"), "FrameCapture")

    def test_coherence_member_recognition_is_exact(self):
        self.assertEqual([boundary.legacy_raster_owner(symbol) for symbol in LEGACY_COHERENCE],
                         ["coherence:observe_ram_access", "coherence:set_pending_dma_ranges",
                          "coherence:tag_host_pages_for_physical"])
        for symbol in NOT_LEGACY_COHERENCE:
            with self.subTest(symbol=symbol):
                self.assertIsNone(boundary.legacy_raster_owner(symbol))

    def test_library_census_counts_only_table_symbols(self):
        owners, examples = boundary.legacy_raster_census(dumpbin_output(NATIVE + LEGACY_RASTER + NOT_LEGACY_RASTER))
        self.assertEqual(owners, {"Renderer": 2, "PlumeGxRasterizer": 1, "gxshaders": 1})
        self.assertEqual(len(examples), 4)
        self.assertEqual(boundary.legacy_raster_census(dumpbin_output(NATIVE + NOT_LEGACY_RASTER)), ({}, []))

    def test_map_reports_legacy_raster_rows(self):
        evidence = boundary.parse_linker_map(map_text((map_row(LEGACY_RASTER[1], origin="shadow_runtime:renderer.obj"),)))
        self.assertEqual(evidence["legacy_raster_owners"], {"Renderer": 1})
        clean = boundary.parse_linker_map(map_text(tuple(map_row(s) for s in NOT_LEGACY_RASTER)))
        self.assertEqual(clean["legacy_raster_owners"], {})


class MarkerTests(unittest.TestCase):
    def test_every_internal_marker_split(self):
        for marker in boundary.INTERPRETER_MARKERS:
            chunk_size = len(marker) + 5
            for split in range(1, len(marker)):
                with self.subTest(marker=marker, split=split):
                    payload = b"x" * (chunk_size - split) + marker + b"y" * chunk_size
                    self.assertEqual(boundary.find_byte_markers(io.BytesIO(payload), chunk_size=chunk_size), (marker,))

    def test_markers_spanning_many_chunks_are_unique(self):
        payload = b"\0".join(boundary.INTERPRETER_MARKERS * 3)
        for size in (1, 2, 7, 64, 65536):
            with self.subTest(size=size):
                self.assertEqual(boundary.find_byte_markers(io.BytesIO(payload), chunk_size=size), boundary.INTERPRETER_MARKERS)

    def test_near_matches_case_and_trailing_space_are_not_markers(self):
        payload = (
            b"unimplemented %s instruction 0x%08x\0"
            b"recompiler verification mismatch:\0"
            b"set_verify_recompiled quantise_load quantise_store std::step"
        )
        self.assertEqual(boundary.find_byte_markers(io.BytesIO(payload), chunk_size=1), ())
        self.assertEqual(boundary.find_byte_markers(io.BytesIO(b"")), ())

    def test_single_byte_and_empty_marker_sets(self):
        self.assertEqual(boundary.find_byte_markers(io.BytesIO(b"axb"), (b"x", b"x"), 1), (b"x",))
        self.assertEqual(boundary.find_byte_markers(io.BytesIO(b"anything"), (), 1), ())

    def test_invalid_marker_parameters_and_text_stream(self):
        for size in (0, -1, "1"):
            with self.subTest(size=size), self.assertRaises(ValueError):
                boundary.find_byte_markers(io.BytesIO(), chunk_size=size)
        for markers in ((b"",), ("text",)):
            with self.subTest(markers=markers), self.assertRaises(ValueError):
                boundary.find_byte_markers(io.BytesIO(), markers)
        with self.assertRaises(boundary.OperationalError):
            boundary.find_byte_markers(io.StringIO("text"))

    def test_read_failure_is_not_treated_as_eof(self):
        class FailingStream:
            def read(self, size):
                raise OSError("synthetic read failure")
        with self.assertRaises(OSError):
            boundary.find_byte_markers(FailingStream())


class FormatTests(unittest.TestCase):
    def test_archive_header(self):
        boundary.validate_archive(io.BytesIO(archive_bytes()))
        valid = archive_bytes()
        invalid = (
            b"", b"not an archive", b"!<thin>\n", valid[:8], valid[:-1],
            changed(valid, 8, b"not-linker"), changed(valid, 66, b"xx"), changed(valid, 56, b"bad"),
            changed(valid, 56, b"9999999999"),
        )
        for raw in invalid:
            with self.subTest(raw=raw[:24]), self.assertRaises(boundary.OperationalError):
                boundary.validate_archive(io.BytesIO(raw))

    def test_pe32_and_pe32_plus(self):
        for pe64 in (False, True):
            with self.subTest(pe64=pe64):
                stream = io.BytesIO(pe_bytes(pe64=pe64))
                boundary.validate_pe(stream)
                self.assertEqual(stream.tell(), 0)

    def test_invalid_and_truncated_pe_images(self):
        valid = pe_bytes()
        section = 0x98 + 240
        invalid = (
            b"", b"MZ", b"MZ" + b"\0" * 62, valid[:80], valid[:-1],
            changed(valid, 60, struct.pack("<I", 0xfffffff0)), changed(valid, 0x80, b"NOPE"),
            changed(valid, 0x84, b"\0\0"), changed(valid, 0x86, b"\0\0"),
            changed(valid, 0x96, struct.pack("<H", 0x2002)), changed(valid, 0x96, b"\0\0"),
            changed(valid, 0x94, struct.pack("<H", 1)), changed(valid, 0x98, b"\0\0"),
            changed(valid, 0x98 + 60, struct.pack("<I", 128)),
            changed(valid, section + 20, struct.pack("<I", 128)),
            changed(valid, section + 36, struct.pack("<I", 0x40000040)),
        )
        for raw in invalid:
            with self.subTest(size=len(raw), header=raw[0x80:0xa0]), self.assertRaises(boundary.OperationalError):
                boundary.validate_pe(io.BytesIO(raw))


class MapTests(unittest.TestCase):
    def test_real_run_and_allowed_helpers(self):
        text = map_text(tuple(map_row(symbol, flags="f i") for symbol in ALLOWED))
        evidence = boundary.parse_linker_map(text)
        self.assertTrue(evidence["native_run_symbol"])
        self.assertFalse(evidence["forbidden_members"])
        self.assertFalse(evidence["reference_archive_matches"])

    def test_every_forbidden_method_is_reported(self):
        for name in boundary.FORBIDDEN:
            with self.subTest(name=name):
                evidence = boundary.parse_linker_map(map_text((map_row(cpu_symbol(name)),)))
                self.assertEqual(evidence["forbidden_members"], [name])

    def test_reference_archive_member_names(self):
        origins = (
            "shadow_runtime_reference:cpu.obj", "shadow_runtime_reference.lib:cpu.obj",
            "SHADOW_RUNTIME_REFERENCE.LIB:other.obj", "shadow_reference_runtime:cpu.obj",
            "reference-runtime.lib:cpu.obj", "shadow_runtime_reference_debug:cpu.obj",
            r'"H:\build path\shadow_runtime_reference.lib:cpu.obj"',
            "shadow_runtime_reference.lib(cpu.obj)",
        )
        for origin in origins:
            with self.subTest(origin=origin):
                evidence = boundary.parse_linker_map(map_text((map_row("harmless", origin=origin),)))
                self.assertEqual(evidence["reference_archive_matches"], 1)

    def test_custom_reference_basename_and_stem(self):
        for origin in ("oracle.lib:cpu.obj", "oracle:cpu.obj", "oracle.lib(cpu.obj)"):
            with self.subTest(origin=origin):
                text = map_text((map_row("harmless", origin=origin),))
                evidence = boundary.parse_linker_map(text, r"H:\build path\oracle.lib")
                self.assertEqual(evidence["reference_archive_matches"], 1)

    def test_unrelated_archive_and_reference_mentions_are_not_members(self):
        text = map_text((map_row("harmless", origin="reference_geometry.lib:mesh.obj"),))
        text += " shadow_runtime_reference.lib\n oracle.lib\n shadow_runtime_reference_count\n"
        self.assertEqual(boundary.parse_linker_map(text)["reference_archive_matches"], 0)

    def test_run_mentions_data_absolute_imports_and_out_of_range_rows_do_not_count(self):
        rows = (
            NATIVE[0] + "\n", map_row(NATIVE[0], flags=""), map_row(NATIVE[0], segment=0),
            map_row(NATIVE[0], segment=2), map_row(NATIVE[0], address=0),
            map_row(NATIVE[0], offset=0x1000), map_row("__imp_" + NATIVE[0]),
            map_row("?run@Cpu@other@@QEAAXXZ"), map_row("?run@Cpu@gekko@@"),
        )
        for row in rows:
            with self.subTest(row=row):
                self.assertFalse(boundary.parse_linker_map(map_text((row,), run=False))["native_run_symbol"])

    def test_malformed_maps_fail_closed(self):
        valid = map_text()
        invalid = (
            "", NATIVE[0], "MZ\0\0", valid.encode("utf-16-le"),
            valid.replace("Publics by Value", "not a symbol table"), valid.replace(" CODE\n", " DATA\n"),
            valid.replace("Preferred load address is", "image base"),
            valid + " 0001:00000030 invalid row\n", valid + " Preferred load address is 0000000140000000\n",
            valid + "x" * (boundary.MAX_TEXT_LINE + 1),
            map_text((map_row(NATIVE[0], flags="", origin=""),), run=False),
        )
        for text in invalid:
            with self.subTest(text=repr(text)[:100]), self.assertRaises(boundary.OperationalError):
                boundary.parse_linker_map(text)

    def test_reference_examples_and_line_reads_are_bounded(self):
        rows = tuple(map_row("harmless", origin=f"shadow_runtime_reference_{index}.lib:cpu.obj") for index in range(20))
        evidence = boundary.parse_linker_map(boundary.bounded_lines(io.BytesIO(map_text(rows).encode("ascii"))))
        self.assertEqual(evidence["reference_archive_matches"], 20)
        self.assertLessEqual(len(evidence["reference_archives"]), boundary.MAX_REFERENCE_NAMES)
        with self.assertRaises(boundary.OperationalError):
            list(boundary.bounded_lines(io.BytesIO(b"x" * (boundary.MAX_TEXT_LINE + 1))))


class CliTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="native-boundary-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.dumpbin = self.root / "MSVC tools" / "dumpbin.exe"
        self.dumpbin.parent.mkdir()
        self.dumpbin.write_bytes(b"synthetic tool; never executed")
        self.native = self.root / "native runtime.lib"
        self.reference = self.root / "shadow_runtime_reference.lib"
        self.executable = self.root / "game.exe"
        self.linker_map = self.root / "game.map"
        self.native.write_bytes(archive_bytes())
        self.reference.write_bytes(archive_bytes())
        self.executable.write_bytes(pe_bytes())
        self.linker_map.write_bytes(map_text().encode("ascii"))

    def arguments(self, *extra):
        return ["--dumpbin", str(self.dumpbin), "--native-library", str(self.native), *map(str, extra)]

    def invoke(self, arguments=None, results=None):
        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            with patch.object(boundary.subprocess, "run", side_effect=results if results is not None else [completed()]) as run:
                code = boundary.main(self.arguments() if arguments is None else arguments)
        raw = stdout.getvalue()
        self.assertEqual(len(raw.splitlines()), 1)
        self.assertLess(len(raw.encode("utf-8")), 65536)
        report = json.loads(raw)
        self.assertEqual(report["exit_code"], code)
        return code, report, stderr.getvalue(), run

    def test_native_only_optional_checks_are_explicitly_absent(self):
        code, report, stderr, run = self.invoke(results=[completed(NATIVE + ALLOWED)])
        self.assertEqual(code, 0)
        self.assertEqual(report["status"], "passed")
        self.assertEqual(report["native_library"]["required_members"], sorted(boundary.NATIVE_REQUIRED))
        for name in ("reference_library", "executable", "map"):
            self.assertIsNone(report[name])
        self.assertEqual(stderr, "")
        run.assert_called_once_with(
            [str(self.dumpbin.resolve()), "/NOLOGO", "/LINKERMEMBER:1", str(self.native.resolve())],
            capture_output=True, check=False, timeout=boundary.DUMPBIN_TIMEOUT,
        )

    def test_all_checks_pass_and_inputs_are_not_changed(self):
        files = tuple(path for path in self.root.rglob("*") if path.is_file())
        before = {path: (path.read_bytes(), path.stat().st_mtime_ns) for path in files}
        results = [completed(), completed(NATIVE + tuple(cpu_symbol(name) for name in boundary.FORBIDDEN))]
        args = self.arguments("--reference-library", self.reference, "--executable", self.executable, "--map", self.linker_map)
        code, report, stderr, run = self.invoke(args, results)
        self.assertEqual(code, 0)
        self.assertEqual(run.call_count, 2)
        self.assertEqual(report["reference_library"]["missing_required_members"], [])
        self.assertTrue(report["map"]["native_run_symbol"])
        self.assertIn("does not prove code absence", report["executable"]["evidence"])
        self.assertEqual(stderr, "")
        self.assertEqual(before, {path: (path.read_bytes(), path.stat().st_mtime_ns) for path in files})
        self.assertEqual(set(files), {path for path in self.root.rglob("*") if path.is_file()})

    def test_native_forbidden_members_are_boundary_violations(self):
        for name in boundary.FORBIDDEN:
            with self.subTest(name=name):
                code, report, stderr, _ = self.invoke(results=[completed(NATIVE + (cpu_symbol(name),))])
                self.assertEqual(code, 1)
                self.assertEqual(report["status"], "boundary_violation")
                self.assertEqual(report["native_library"]["forbidden_members"], [name])
                self.assertIn(name, stderr)

    def test_native_legacy_raster_symbols_are_boundary_violations(self):
        for symbol in LEGACY_RASTER:
            with self.subTest(symbol=symbol):
                code, report, stderr, _ = self.invoke(results=[completed(NATIVE + (symbol,))])
                self.assertEqual(code, 1)
                self.assertEqual(report["status"], "boundary_violation")
                self.assertEqual(list(report["native_library"]["legacy_raster_owners"]),
                                 [boundary.legacy_raster_owner(symbol)])
                self.assertIn("legacy GX raster", stderr)
        code, report, _, _ = self.invoke(results=[completed(NATIVE + NOT_LEGACY_RASTER)])
        self.assertEqual(code, 0)
        self.assertEqual(report["native_library"]["legacy_raster_owners"], {})

    def test_reference_legacy_raster_symbols_are_boundary_violations(self):
        reference = NATIVE + tuple(cpu_symbol(name) for name in boundary.REFERENCE_REQUIRED)
        code, report, stderr, _ = self.invoke(self.arguments("--reference-library", self.reference),
                                              [completed(), completed(reference)])
        self.assertEqual(code, 0, stderr)
        self.assertEqual(report["reference_library"]["legacy_raster_owners"], {})
        for symbol in LEGACY_RASTER + LEGACY_COHERENCE:
            with self.subTest(symbol=symbol):
                code, report, stderr, _ = self.invoke(self.arguments("--reference-library", self.reference),
                                                      [completed(), completed(reference + (symbol,))])
                self.assertEqual(code, 1)
                self.assertEqual(list(report["reference_library"]["legacy_raster_owners"]),
                                 [boundary.legacy_raster_owner(symbol)])
                self.assertIn("reference-library contains legacy GX raster symbols", stderr)

    def test_native_legacy_coherence_symbols_are_boundary_violations(self):
        for symbol in LEGACY_COHERENCE:
            with self.subTest(symbol=symbol):
                code, report, stderr, _ = self.invoke(results=[completed(NATIVE + (symbol,))])
                self.assertEqual(code, 1)
                self.assertIn("legacy GX raster", stderr)
        code, report, _, _ = self.invoke(results=[completed(NATIVE + NOT_LEGACY_COHERENCE)])
        self.assertEqual(code, 0)

    def test_map_legacy_raster_symbols_are_boundary_violations(self):
        self.linker_map.write_bytes(map_text((map_row(LEGACY_RASTER[0], origin="shadow_runtime:renderer.obj"),)).encode("ascii"))
        code, report, stderr, _ = self.invoke(self.arguments("--map", self.linker_map))
        self.assertEqual(code, 1)
        self.assertEqual(report["map"]["legacy_raster_owners"], {"Renderer": 1})
        self.assertIn("map contains legacy GX raster symbols", stderr)

    def test_missing_native_members_do_not_pass(self):
        for missing in NATIVE:
            with self.subTest(missing=missing):
                symbols = tuple(symbol for symbol in NATIVE if symbol != missing)
                code, report, stderr, _ = self.invoke(results=[completed(symbols)])
                self.assertEqual(code, 1)
                self.assertIn(boundary.cpu_member(missing), report["native_library"]["missing_required_members"])
                self.assertIn("lacks required", stderr)
        self.assertEqual(self.invoke(results=[completed(ALLOWED)])[0], 1)

    def test_reference_positive_control_requires_every_expected_member(self):
        for missing in boundary.REFERENCE_REQUIRED:
            with self.subTest(missing=missing):
                reference = NATIVE + tuple(cpu_symbol(name) for name in boundary.REFERENCE_REQUIRED if name != missing)
                code, report, stderr, _ = self.invoke(self.arguments("--reference-library", self.reference), [completed(), completed(reference)])
                self.assertEqual(code, 1)
                self.assertIn(missing, report["reference_library"]["missing_required_members"])
                self.assertIn("positive control", stderr)
        self.assertEqual(self.invoke(self.arguments("--reference-library", self.reference), [completed(), completed()])[0], 1)

    def test_command_failure_is_operational_even_with_valid_looking_stdout(self):
        code, report, stderr, _ = self.invoke(results=[completed(returncode=7, stderr=b"synthetic dumpbin failure")])
        self.assertEqual(code, 2)
        self.assertEqual(report["status"], "error")
        self.assertIn("exit 7", stderr)
        self.assertIn("synthetic dumpbin failure", stderr)

    def test_timeout_launch_failure_empty_probe_and_stderr_fail_closed(self):
        results = (
            subprocess.TimeoutExpired("dumpbin", boundary.DUMPBIN_TIMEOUT), OSError("cannot launch tool"),
            completed(stdout=b""), completed(stderr=b"warning: synthetic probe problem"),
            completed(stdout=b"File Type: LIBRARY\n\n?run@Cpu@gekko@@QEAAXXZ\n"),
        )
        for result in results:
            with self.subTest(result=result):
                code, report, stderr, _ = self.invoke(results=[result])
                self.assertEqual(code, 2)
                self.assertEqual(report["status"], "error")
                self.assertTrue(stderr)

    def test_executable_markers_are_additional_boundary_evidence(self):
        for marker in boundary.INTERPRETER_MARKERS:
            with self.subTest(marker=marker):
                self.executable.write_bytes(pe_bytes(b"x" * (boundary.CHUNK_SIZE - 1025) + marker))
                code, report, stderr, _ = self.invoke(self.arguments("--executable", self.executable))
                self.assertEqual(code, 1)
                self.assertEqual(report["executable"]["diagnostic_markers"], [marker.decode("ascii")])
                self.assertIn("diagnostic markers", stderr)
        self.executable.write_bytes(pe_bytes())
        self.assertEqual(self.invoke(self.arguments("--executable", self.executable), [completed(stdout=b"")])[0], 2)

    def test_map_violations_and_missing_real_run(self):
        texts = (
            map_text((map_row(cpu_symbol("step")),)),
            map_text((map_row("harmless", origin="shadow_runtime_reference:other.obj"),)),
            map_text(run=False),
        )
        for text in texts:
            with self.subTest(text=text[-300:]):
                self.linker_map.write_bytes(text.encode("ascii"))
                code, report, stderr, _ = self.invoke(self.arguments("--map", self.linker_map))
                self.assertEqual(code, 1)
                self.assertEqual(report["status"], "boundary_violation")
                self.assertIn("map", stderr)

    def test_invalid_arguments_paths_and_same_reference_are_operational(self):
        arguments = (
            [], ["--dumpbin", str(self.dumpbin)], self.arguments("--unknown", "value"),
            ["--dumpb", str(self.dumpbin), "--native-library", str(self.native)],
            ["--dumpbin", str(self.root / "absent.exe"), "--native-library", str(self.native)],
            ["--dumpbin", str(self.dumpbin), "--native-library", str(self.root)],
            self.arguments("--reference-library", self.root / "absent.lib"),
            self.arguments("--executable", self.root / "absent.exe"), self.arguments("--map", self.root / "absent.map"),
            self.arguments("--reference-library", self.native),
        )
        for args in arguments:
            with self.subTest(args=args):
                code, report, stderr, run = self.invoke(args)
                self.assertEqual(code, 2)
                self.assertEqual(report["status"], "error")
                self.assertTrue(stderr)
                run.assert_not_called()

    def test_invalid_artifact_formats_fail_closed(self):
        self.native.write_bytes(b"not an archive")
        code, _, _, run = self.invoke()
        self.assertEqual(code, 2)
        run.assert_not_called()
        self.native.write_bytes(archive_bytes())
        for path, option in ((self.reference, "--reference-library"), (self.executable, "--executable"), (self.linker_map, "--map")):
            with self.subTest(option=option):
                path.write_bytes(b"not the required format")
                code, report, stderr, _ = self.invoke(self.arguments(option, path))
                self.assertEqual(code, 2)
                self.assertEqual(report["status"], "error")
                self.assertTrue(stderr)

    def test_read_errors_are_operational(self):
        with patch.object(boundary, "find_byte_markers", side_effect=OSError("synthetic read denial")):
            code, report, stderr, _ = self.invoke(self.arguments("--executable", self.executable))
        self.assertEqual(code, 2)
        self.assertEqual(report["status"], "error")
        self.assertIn("read denial", stderr)
        with patch.object(Path, "open", side_effect=PermissionError("synthetic access denial")):
            code, _, stderr, run = self.invoke()
        self.assertEqual(code, 2)
        self.assertIn("access denial", stderr)
        run.assert_not_called()

    def test_diagnostics_and_json_remain_bounded(self):
        code, report, stderr, _ = self.invoke(results=[completed(returncode=1, stderr=b"x" * 1000000)])
        self.assertEqual(code, 2)
        self.assertLess(len(stderr), 4096)
        self.assertLessEqual(len(report["errors"][0]), boundary.MAX_DETAIL + 3)
        code, _, stderr, _ = self.invoke(self.arguments("--invalid-" + "x" * 1000000))
        self.assertEqual(code, 2)
        self.assertLess(len(stderr), 4096)


if __name__ == "__main__":
    unittest.main()
