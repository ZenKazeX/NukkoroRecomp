# Android build (arm64-v8a, x86_64)

The game's own build (root `CMakeLists.txt`, `CMakePresets.json`) cross-compiled for Android with the NDK. Android
renders through Vulkan only (`runtime/lift/render_vk`, docs/VULKAN_RENDERER.md). This page is the configuration and
its current state; the renderer's own Android bring-up activity is `runtime/lift/render_vk/android/README.md`.

## Presets

| Preset | What | Tree |
| --- | --- | --- |
| `host-tools` | the build machine's tools a cross build runs: `ppcrecomp` (Windows x64, Visual Studio) | `.build/host-tools` |
| `android-arm64` | the game for phones: arm64-v8a, API 33, NDK 27.2, Ninja, Release, shipping | `.build/android-arm64` |
| `android-x86_64` | the same for the emulator (this PC's emulator images are x86_64) | `.build/android-x86_64` |

`android-base` (hidden) holds the shared settings: the NDK toolchain file under `SHADOW_ANDROID_NDK` (preset
environment, default `C:/AndroidSDK/ndk/27.2.12479018`; override it in a `CMakeUserPresets.json`), the NDK's Ninja,
`ANDROID_STL=c++_static`, the DOL at `../game/sys/main.dol`, tests, plume, Tracy and the lifted renderer off.

## Steps (from `source/`)

```
cmake --preset host-tools
cmake --build --preset host-tools
cmake --preset android-arm64
cmake --build --preset android-arm64
```

The first two build only `ppcrecomp.exe`. The Android configure refuses to run without it (it names the two commands).
Never build while a `ShadowTheHedgehog*` process from `..\game` runs; builds stay at `-j 2`.

## What the configuration does (root CMakeLists.txt, additive)

* **The recompiler runs on the build machine.** When `CMAKE_CROSSCOMPILING`, `ppcrecomp` is an imported executable
  (`SHADOW_PPCRECOMP_EXECUTABLE`, the host-tools build) instead of a target compiled for Android; the generated C++
  shards are then compiled for the target like any other source. The Windows trees build `ppcrecomp` as before.
* **Floating point as on Windows.** MSVC compiles the guest code with `/fp:precise`. clang contracts `a*b+c` into a
  fused multiply-add by default on arm64, which rounds once where the guest rounded twice, so clang and GCC get
  `-ffp-contract=off` (through `shadow_host_isa`, the same usage requirement the MSVC flags travel by). Fused
  operations stay explicit in the numeric helpers.
* **The game is a shared library on Android** (`libShadowTheHedgehog.so` in `bin/`, what an activity loads), an
  executable everywhere else.
* **render_vk is built in the same tree** on Android (`add_subdirectory(runtime/lift/render_vk)`: `shadow_render_vk`
  and the bring-up activity `shadow_vk`).
* The Windows-only parts were already guarded: the D3D12 renderer and its tools (`if(WIN32)`), the resource script,
  ws2_32 / psapi, the MSVC host-ISA gate, the dumpbin / Python post-build gates (`if(MSVC)`).

## State

### First build, 2026-10-27 (android-arm64, `cmake --build --preset android-arm64 -- -k 0`)

Configure: clean (only the NDK's own CMake deprecation warnings). `ppcrecomp` from host-tools generated the 48 shards.

Every source was compiled; **6 files fail**, everything else compiles for arm64 (14 warnings, all unused variables
plus one self-comparison in `recomp_005.cpp`).

Linked: `libshadow_native_memory.a`, `libshadow_lift.a`, `libshadow_assets.a`, `libshadow_recomp.a` (all 48 recompiled
shards), `libshadow_render_vk_core.a`, `libshadow_render_vk.a`, `libshadow_render_vk_checks.a`, `libshadow_vk.so`.
Compiled but not linked (blocked by the failures): `shadow_native_gameplay`, `shadow_runtime_common` (66 of 67
objects), `shadow_runtime`. Not produced: `libShadowTheHedgehog.so`.

| File | Error | Cause / fix direction |
| --- | --- | --- |
| `runtime/src/native_execution.cpp:13` | `#error Native execution requires an x86-64 target` | an explicit x86-64 gate; the task graph above it is portable C++ - widen the gate to `__aarch64__` after checking the file for x86 assumptions |
| `runtime/src/gekko/audio_runtime.cpp:22` | `emmintrin.h`: "only meant to be used on x86" | unconditional SSE2 include (plus WASAPI under `_WIN32`); needs a NEON / scalar path, and the Android output (AAudio) is the platform layer's |
| `runtime/src/gekko/gameplay_scheduler.cpp:1292` | `-Werror,-Wformat-security`: format string not a literal | `snprintf(detail, ..., format, args...)` in a variadic helper; clang errors by default on Android - mark the helper `__attribute__((format(printf, ...)))` or pass the format as a literal |
| `runtime/src/gekko/host_window.cpp:499` | redefinition of `HostWindow::plume_active` | line 145's definition sits outside the `#ifdef _WIN32` block, so the non-Windows stub at 499 duplicates it - a defect in the POSIX branch |
| `runtime/src/gekko/cpu.cpp:763,767` | `__declspec` attributes not enabled | `__declspec(noinline)` on `translation_fault_read/write`; use `[[gnu::noinline]]` / a `SHADOW_NOINLINE` macro off MSVC |
| `runtime/src/shadow_main.cpp:2492,3571` | `GetCurrentProcessId`, `localtime_s` undeclared | the Windows launcher; expected - the Android entry point replaces it (see below) |

### Second build, 2026-10-27: the five portability fixes

| File | Fix |
| --- | --- |
| `native_execution.cpp` | the gate admits arm64 (`__aarch64__` / `_M_ARM64`); nothing else in the file was x86 |
| `audio_runtime.cpp` | SSE2 only on x86-64 (unchanged there); elsewhere the same four-lane kernel in plain C++ - same products, same per-lane sums, same `horizontal_sum` order, no contraction - and a scalar byte swap |
| `gameplay_scheduler.cpp` | `fault()` with no arguments prints the message through `"%s"` (none of those messages contains `%`) |
| `host_window.cpp` | the non-Windows duplicate of `plume_active` removed; the shared definition returns false without a presenter |
| `cpu.cpp` | `GEKKO_NOINLINE` (types.h) instead of `__declspec(noinline)` |

Result: every library links for arm64 - `shadow_native_execution`, `shadow_native_gameplay`, `shadow_runtime` and the
runtime objects included - with no warnings from the edited files. The only failure is the Windows launcher
`shadow_main.cpp` (`GetCurrentProcessId`, `localtime_s`), so `libShadowTheHedgehog.so` is not produced yet. MSVC still
compiles the same libraries (host-tools tree, Release). The arm64 audio path is compiled, not yet run; bit-equality with
the SSE2 kernel is by construction, not measured.

What remains is the platform layer (entry point, surface, audio, input) and the V7 renderer hooks.

Known before the first build, from the sources (still true):

* There is no Android entry point: `shadow_main.cpp` is the Windows launcher (window, WASAPI audio, XInput pad,
  D3D12 / plume presentation). The Android platform layer - `android_main` (NativeActivity) or GameActivity, the
  `ANativeWindow` surface, AAudio output, input, the game data path - is not written.
* The game does not draw through render_vk yet: the lifted renderer's scaffolding (`runtime/scaffold/rw_driver`)
  calls the D3D12 renderer (`shadow_render`), so `SHADOW_RENDER_LIFTED` is off here. Connecting it to render_vk is
  stage V7 (docs/VULKAN_RENDERER.md 8).
* Parts of the runtime already have POSIX branches (`guest_memory.cpp`: `mmap` beside `VirtualAlloc`); others are
  Windows-only (`audio_runtime.cpp`, `host_window.cpp`, `xinput_pad.cpp`, `dev_link.cpp`, `process_profile.cpp`) or
  carry MSVC intrinsics behind `_MSC_VER` with portable fallbacks to be checked (`ppc_ops.h`, `cpu.h`; both compiled
  cleanly for arm64 in the first build).
