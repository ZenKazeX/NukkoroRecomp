import argparse
from pathlib import Path
import subprocess
import tempfile


def configure(cmake, source, work, tests, mixed):
    root = work / ("mixed" if mixed else "reference" if tests else "native-library")
    root.mkdir()
    project = root / "source"
    project.mkdir()
    (project / "main.cpp").write_text("int main() { return 0; }\n", encoding="utf-8")
    checks = [
        "if(TARGET ShadowTheHedgehog)",
        '  message(FATAL_ERROR "An empty DOL must not produce a game executable")',
        "endif()",
        "get_target_property(recomp_links shadow_recomp INTERFACE_LINK_LIBRARIES)",
        'if(recomp_links MATCHES "shadow_runtime")',
        '  message(FATAL_ERROR "Generated code must not select a CPU execution provider")',
        "endif()",
        "get_target_property(native_mode shadow_runtime INTERFACE_SHADOW_CPU_EXECUTION)",
        'if(NOT native_mode STREQUAL "native")',
        '  message(FATAL_ERROR "Native execution provider is missing")',
        "endif()",
    ]
    if tests:
        checks += [
            "get_target_property(reference_mode shadow_runtime_reference INTERFACE_SHADOW_CPU_EXECUTION)",
            'if(NOT reference_mode STREQUAL "reference")',
            '  message(FATAL_ERROR "Reference execution provider is missing")',
            "endif()",
            "get_target_property(reference_excluded ShadowTheHedgehogReference EXCLUDE_FROM_ALL)",
            "if(NOT reference_excluded)",
            '  message(FATAL_ERROR "Reference runner must not be a default shipping target")',
            "endif()",
        ]
    else:
        checks += [
            "if(TARGET shadow_runtime_reference OR TARGET ShadowTheHedgehogReference)",
            '  message(FATAL_ERROR "Tests-disabled configuration must not create reference targets")',
            "endif()",
        ]
    if mixed:
        checks += [
            "add_executable(mixed main.cpp)",
            "target_link_libraries(mixed PRIVATE shadow_runtime shadow_runtime_reference)",
        ]
    text = ("cmake_minimum_required(VERSION 3.20)\n"
            "project(RuntimeBoundaryProbe LANGUAGES CXX)\n"
            f"add_subdirectory([==[{source.as_posix()}]==] runtime)\n" + "\n".join(checks) + "\n")
    (project / "CMakeLists.txt").write_text(text, encoding="utf-8")
    result = subprocess.run(
        [cmake, "-S", str(project), "-B", str(root / "build"),
         "-DSHADOW_RECOMP_DOL=", f"-DSHADOW_BUILD_TESTS={'ON' if tests else 'OFF'}",
         "-DSHADOW_ENABLE_PLUME=OFF", "-DSHADOW_ENABLE_AUDIO=OFF", "-DSHADOW_ENABLE_TRACY=OFF",
         "-DSHADOW_DEPLOY_DIR="], capture_output=True, text=True, errors="replace", timeout=120)
    diagnostic = result.stdout + result.stderr
    if mixed:
        if result.returncode == 0 or "SHADOW_CPU_EXECUTION" not in diagnostic:
            raise RuntimeError("Mixed CPU providers were not rejected by the execution-domain constraint:\n" + diagnostic)
    elif result.returncode != 0:
        raise RuntimeError("Runtime configuration failed:\n" + diagnostic)
    print(f"PASS runtime configuration: tests={tests}, mixed={mixed}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--work", required=True, type=Path)
    parser.add_argument("--cmake", default="cmake")
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="cpu-build-boundary-", dir=args.work.resolve()) as temporary:
        work = Path(temporary)
        for tests, mixed in ((False, False), (True, False), (True, True)):
            configure(args.cmake, args.source.resolve(), work, tests, mixed)


if __name__ == "__main__":
    main()
