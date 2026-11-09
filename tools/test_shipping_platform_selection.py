"""Exercise the executable boundary; a crash is never a passing rejection."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def invoke(exe, arguments, cwd, environment=None):
    env = dict(os.environ)
    env.update(environment or {})
    return subprocess.run([str(exe), *arguments], cwd=cwd, env=env,
                          capture_output=True, text=True, timeout=10)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--shipping", type=Path, required=True)
    parser.add_argument("--reference", type=Path, required=True)
    # A SHADOW_SHIPPING tree: development controls are refused at parse time, in both executables.
    parser.add_argument("--shipping-build", type=int, default=0)
    args = parser.parse_args()
    # Oracle controls the reference executable still has; shipping refuses them
    # before constructing any owner.
    reference_only = [
        ["--legacy-scheduler"], ["--legacy-health-bar"], ["--native-no-gpu"], ["--verify-plume"],
        ["--native-xfb-present"], ["--no-native-xfb-present"],
    ]
    # Controls of the legacy GX register raster, deleted from BOTH executables
    # on 2026-10-05 together with the raster: neither accepts them any more.
    removed = [
        ["--legacy-renderer"], ["--legacy-raster-mode", "modern"],
        ["--screenshot", "old.ppm"], ["--screenshot-efb", "old.ppm"], ["--screenshot-efb-native", "old.ppm"],
        ["--filmstrip", "old"], ["--present-hash"], ["--dump-textures", "old"],
        ["--hud-geometry", "old"], ["--effect-geometry", "old"],
        ["--border-probe", "old"], ["--hud-report", "old"], ["--validate-dx12-raster"],
        ["--validate-dx12-gpu-clip"], ["--no-dx12-raster"], ["--diag-coverage"],
        ["--dx12-raster"], ["--blur-history-average"], ["--effect-footprint-filter"],
        ["--no-effect-footprint-filter"], ["--xfb-filter-spacing", "native"],
        ["--no-effect-depth-test"], ["--constant-depth-snap"], ["--no-constant-depth-snap"],
        ["--vertex-decode-fanout", "64"], ["--no-lazy-copies"], ["--no-lazy-xfb"],
        ["--pe-token-drain"], ["--no-pe-token-drain"], ["--no-guard-publish-elision"],
        ["--diag-clear-efb"], ["--diag-no-raster"], ["--diag-no-gpu-submit"],
        ["--separate-present-device"], ["--no-zcomploc"], ["--no-opaque-copy-alpha"],
        ["--no-one-stage-shader"], ["--pixel-probe", "1,1"], ["--no-strip-extend"],
        ["--flipper-thread"], ["--no-flipper-thread"], ["--flipper-depth", "2"], ["--ui-overlay"],
        ["--layer-isolate", "2d"], ["--copy-filter"], ["--no-copy-filter"], ["--frame-capture"],
        ["--motion-history-filter", "box"], ["--no-smooth-motion-history"],
        ["--hud-geometry-frames", "1-2"], ["--native-efb-frame", "1"], ["--filmstrip-stride", "1"],
        ["--verify-data"],
    ]
    # Call-trace diagnostics, deleted from BOTH executables on 2026-10-05 with the
    # per-call note_call hooks that fed them (platform-retirement batch 3, change N).
    removed_diagnostics = [["--call-trace"], ["--no-call-trace"], ["--reach-log", "old.txt"]]
    with tempfile.TemporaryDirectory(prefix="shadow-platform-selection-") as work:
        for control in reference_only:
            result = invoke(args.shipping, [*control, "--help"], work)
            assert result.returncode == 2, (control, result.returncode, result.stderr)
            assert "reference-only" in result.stderr and "ShadowTheHedgehogReference" in result.stderr
            assert "Guest retired" not in result.stdout
            # Real reference acceptance: no boot/assets are invoked, but this
            # checks the separately compiled launcher rather than a mock parser.
            result = invoke(args.reference, [*control, "--help"], work)
            assert result.returncode == 0, (control, result.returncode, result.stderr)
        for executable in (args.shipping, args.reference):
            for control in removed + removed_diagnostics:
                result = invoke(executable, [*control, "--help"], work)
                assert result.returncode == 1, (executable.name, control, result.returncode, result.stderr)
                assert "Unknown or incomplete argument: " + control[0] in result.stderr, result.stderr
                assert "Guest retired" not in result.stdout
            for value in ("legacy", "modern", "exact"):
                result = invoke(executable, ["--renderer", value, "--help"], work)
                assert result.returncode == 2 and "the only renderer" in result.stderr, (value, result)
            for control in (["--renderer", "native"], ["--native-renderer"],
                            ["--native-health-bar"], ["--ui-test", "all"]):
                result = invoke(executable, [*control, "--help"], work)
                if args.shipping_build and control[0] == "--ui-test":
                    assert result.returncode == 2 and "not part of this build" in result.stderr, \
                        (executable.name, control, result.returncode, result.stderr)
                    continue
                assert result.returncode == 0, (executable.name, control, result.returncode, result.stderr)
        assert not list(Path(work).iterdir()), "selection must not construct persistent owners"
    print(f"shipping selection: {len(reference_only)} reference-only controls refused by shipping and "
          f"accepted by the reference; {len(removed)} removed raster controls refused by both; "
          f"{len(removed_diagnostics)} removed call-trace controls refused by both")


if __name__ == "__main__":
    main()
