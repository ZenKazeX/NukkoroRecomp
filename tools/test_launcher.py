#!/usr/bin/env python3
"""Unit checks for tools/launcher.py: the pure helpers and the log reading.

    python tools/test_launcher.py

No window is opened and no game is started, so it runs in a second. Each check
pins one defect that was found in review and fixed; the comment says which.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import launcher  # noqa: E402

failures = []


def check(condition, what):
    if not condition:
        failures.append(what)
        print("FAIL: " + what)


# --- budget: std::stoull read "15e9" as FIFTEEN instructions -----------------
for text, want in (("15e9", 15000000000), ("15_000_000_000", 15000000000),
                   ("15 000 000 000", 15000000000), ("15\u00a0000", 15000),
                   ("3000000000", 3000000000), ("1.5e10", 15000000000)):
    check(launcher.parse_count(text) == want, "parse_count(%r) == %d" % (text, want))
# A comma is a decimal mark in many locales: "1,5e10" must not become 15e10.
for text in ("1,5e10", "15,000", "abc", "0", "-5", "1.5", "inf", "nan", "", "   "):
    check(launcher.parse_count(text) is None, "parse_count(%r) is rejected" % text)

# --- extra flags: str.split() broke quoted paths; POSIX shlex eats backslashes
check(launcher.split_flags(r'--state-file "C:\a b\x.gekkostate" --foo')
      == ["--state-file", r"C:\a b\x.gekkostate", "--foo"], "split_flags keeps a quoted path")
check(launcher.split_flags(r"--x C:\dir\file") == ["--x", r"C:\dir\file"],
      "split_flags keeps backslashes")
check(launcher.split_flags("") == [], "split_flags of nothing")

# --- pasted paths: Windows 11 "Copy as path" adds quotes ---------------------
check(launcher.clean_path(' "C:\\a b\\x.gekkostate" ') == "C:\\a b\\x.gekkostate",
      "clean_path strips quotes and spaces")

# --- decoding: the runtime's output is mixed UTF-8 / ANSI, line by line ------
ansi = "C:\\Users\\T\u00f3th Anna\\save-data".encode("cp1250")
utf8 = "Audio device: Hangsz\u00f3r\u00f3k".encode("utf-8")
check(launcher.decode_line(ansi) == "C:\\Users\\T\u00f3th Anna\\save-data",
      "decode_line reads an ANSI path")
check(launcher.decode_line(utf8) == "Audio device: Hangsz\u00f3r\u00f3k",
      "decode_line reads a UTF-8 device name")
check(launcher.decode_line(b"plain ascii") == "plain ascii", "decode_line reads ASCII")

# --- the scrubber: no user-profile path may reach the log ----------------------
home = os.path.expanduser("~")
for sample in (home + "\\x.cfg", home.replace("\\", "/") + "/x.cfg",
               launcher._short_path(home) + "\\x.cfg",
               launcher.decode_line((home + "\\x").encode(launcher.ANSI_ENCODING, "replace"))):
    scrubbed = launcher.scrub_paths("path " + sample)
    check("users" not in scrubbed.lower() and "<home>" in scrubbed,
          "scrub_paths removes the profile from %r -> %r" % (sample, scrubbed))

# --- classifier: failures first, plurals, zero counts in every form ----------
L = launcher.Launcher
for line, want in (
        ("Assets: 2174 reads, 0 unmapped, 0 out of range, 0 failed", "asset"),
        ("Assets: 2174 reads, 3 unmapped, 0 out of range, 0 failed", "error"),
        ("[native-gpu] fence waits 1 (FAILED 0) target allocation failures 0", "native"),
        ("[native-gpu] fence waits 1 (FAILED 2)", "error"),
        ("[file-api] opens 110 (missed 2, directories 0) reads 2175 failed 0 refused 0", "asset"),
        ("[file-api] opens 110 reads 2175 failed 0 refused 0 unmapped 3 out-of-range 0", "error"),
        ("render errors 12", "error"),
        ("failures 7", "error"),
        ("WARNING: could not open telemetry log", "warn"),
        ("ERROR: something broke", "error"),
        ("Stop reason: host request", "info"),
        ("Detail: boot-stage: the PAL screen preselects 50 Hz", "warn"),
        ("Interpreter cost: 0 instructions (0.000% of retired)", "native"),
        ("some unremarkable line", "plain")):
    got = L._classify(L, line)
    check(got == want, "classify %r -> %s (got %s)" % (line, want, got))


# --- live stats: first interval, and the game's own frame rate ---------------
def reading(row, previous=None):
    return L._read_telemetry(L, row, previous)


first = {"wall_s": "1", "fps": "60", "interp_instr": "1250", "host_stalls": "2",
         "renderer": "native"}
tags = [t for t, _ in reading(first)]
check("error" in tags, "interpreter use before the first row is reported")
check(any("host stalls" in text for _t, text in reading(first)),
      "host stalls before the first row are reported")

pal50 = {"wall_s": "5", "fps": "49.6", "xfb_per_guest_s": "50", "renderer": "native"}
ntsc = {"wall_s": "5", "fps": "49.6", "xfb_per_guest_s": "60", "renderer": "native"}
check(reading(pal50)[0][0] == "stat", "a PAL 50 Hz run at full speed is not flagged")
check(reading(ntsc)[0][0] == "warn", "a 60 Hz run at 49.6 fps is flagged")
old = {"wall_s": "5", "fps": "40"}
check(reading(old)[0][0] == "warn", "a build without xfb_per_guest_s keeps the old test")

print("test_launcher: %s" % ("all passed" if not failures else "%d failure(s)" % len(failures)))
if __name__ == "__main__":
    sys.exit(1 if failures else 0)
elif failures:
    raise AssertionError("Launcher checks failed: " + "; ".join(failures))
