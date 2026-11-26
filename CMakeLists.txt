cmake_minimum_required(VERSION 3.20)

project(ShadowTheHedgehogNative LANGUAGES CXX)

# FM (2026-10-20): no volatile-access metadata. MSVC records every volatile access in .rdata$voltmd for
# ARM64's x64 emulation; with FM's volatile guest accesses that table is 3 MB of image never read on an x64
# host. Size only: the generated code is unchanged (checked by .text byte comparison).
if(MSVC AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")  # clang-cl has no such option (and emits no such table)
    add_compile_options($<$<COMPILE_LANGUAGE:CXX,C>:/volatileMetadata->)
endif()

if(WIN32)
    enable_language(RC)
endif()

# cpu/opt (2026-11-15): whole-program optimization of the Release image (MSVC /GL + /LTCG). The generated shards call
# the runtime's small helpers (ppc::fp_compare, psq_load/psq_store, update_cr0, the Cpu memory accessors, the owner and
# indirect-call dispatch) across translation units; without link-time code generation each is a real call. Only the
# optimizer's view changes: /fp:precise semantics hold across inlining, so guest results stay bit-identical (gated).
option(SHADOW_LTO "Link-time code generation (/GL + /LTCG) for the Release image" OFF)
# The 48 generated shards are most of the image; with them under /GL a profile-guided link runs for hours. OFF keeps
# them ordinary objects (compiled as without SHADOW_LTO) and applies LTCG / PGO to the runtime and native owners only.
option(SHADOW_LTO_SHARDS "With SHADOW_LTO: the generated recomp shards are /GL objects too" ON)
if(SHADOW_LTO)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
endif()

option(SHADOW_BUILD_TESTS "Build native runtime tests" ON)
set(SHADOW_PRIVATE_TEST_DATA "${CMAKE_CURRENT_SOURCE_DIR}/test_data" CACHE PATH
    "Folder with the test inputs that cannot be published (RAM images of the game); docs/BUILDING.md")

# The --verify-recomp oracle's runner (ShadowTheHedgehogReference, built by name) and the reference CPU runtime it
# links, in a tree that builds no tests - the lifted renderer's (build-lifted), whose generated code differs from the
# default tree's by R2's lowered sites. SHADOW_BUILD_TESTS builds both anyway.
option(SHADOW_BUILD_REFERENCE "Build the oracle runner (ShadowTheHedgehogReference) without the tests" OFF)

# Present through plume (Direct3D 12) instead of GDI. OFF keeps the default
# build byte-identical: plume_present.cpp compiles to no-ops and HostWindow uses
# GDI. See PROJECT_STATE.md section 16.
# s1/core: serve the layout manager scan 0x800CAC08 natively by default (gekko/native_s1_core.h); GEKKO_S1_CORE_SCAN=0/1 overrides at run time.
option(SHADOW_S1_CORE_SCAN "Layout manager scan owner on by default (quiet window 2026-11-12: -1.5% game-thread CPU)" ON)
# s1/core: serve the counted-handle (shared_ptr / weak_ptr) family natively by default (gekko/native_s1_shptr.h); GEKKO_S1_SHPTR=0/1 overrides at run time.
option(SHADOW_S1_SHPTR "Counted-handle family owner on by default (quiet window 2026-11-13: -3.7% game-thread CPU, 5/5)" ON)
# s1/core: serve the character sweep query 0x80021934 natively by default (gekko/native_s1_chars.h); GEKKO_S1_CHARS=0/1 overrides at run time.
option(SHADOW_RAM_PROBE "cs2/ram measurement build: per-call-stack live heap bytes (GEKKO_RAM_PROBE=<file>); never for shipped or gate exes" OFF)
option(SHADOW_S1_CHARS "Character sweep query owner on by default (quiet window 2026-11-13: -1.7% game-thread CPU, 4/5)" ON)
# cpu/opt5 (revived s2/sound, op3/engine owner): serve the GCAX dB -> amplitude search 0x8043C3F0 natively by default (gekko/native_sound_ampdb.h); GEKKO_OP3_AMPDB=0/1 overrides at run time.
option(SHADOW_OP3_AMPDB "GCAX dB to amplitude search owner on by default (quiet machine 2026-11-17: -1.5% game-thread CPU, -1.0% cycles, 5/5)" ON)
# cpu/opt6: the CharaColli manager's per-frame pair pass 0x800C7068 natively by default (gekko/chara_colli_intrinsic.h, run_pass); GEKKO_CHARA_COLLI_PASS=0/1 and --[no-]chara-colli-pass override at run time.
option(SHADOW_CHARA_COLLI_PASS "CharaColli manager pair pass native by default (quiet machine 2026-11-17: -1.8% game-thread cycles Westopolis, -1.6% Final Haunt, 5/5 each)" ON)
# native/hal-alloc: RwMalloc / RwFree, RenderWare's free-list pair and the game's two wrappers served as one native call each over the OS heap owner (gekko/native_rw_alloc.h); GEKKO_RW_ALLOC=0/1 overrides at run time.
option(SHADOW_RW_ALLOC "RwMalloc / RwFree chain native by default (hooks with a hand-back; the bodies stay)" ON)
# s1/target: serve the target registry query layer natively when ON (gekko/native_s1_target2.h); GEKKO_S1_TARGET2=0/1 overrides at run time. Default OFF.
option(SHADOW_S1_TARGET2 "Target registry query layer owner on by default (quiet window 2026-11-14: -0.5% game-thread CPU, 5/5)" ON)
option(SHADOW_ENABLE_PLUME "Present through plume (Direct3D 12)" OFF)

# Host audio is AudioRuntime (runtime/src/gekko/audio_runtime.cpp): WASAPI,
# always built on Windows, no third-party audio library. The SDL3 backend and
# SHADOW_ENABLE_AUDIO were removed with it (docs/native-runtime-2026-09-16).
option(SHADOW_ENABLE_TRACY "Host CPU/GPU performance tracing through Tracy" OFF)
# The user's exe (ES 2026-10-20, waste census / GC environment row 15): development and verification-only paths
# compiled out under GEKKO_SHIPPING (dev link, traces, probes, stall injector, Tracy, ...). OFF = the dev / test
# build the gates run (same source). The user's own tooling stays: input tapes, savestates, --json-report,
# --boot-stage, --memory-card, --budget-ms.
option(SHADOW_SHIPPING "Compile out development / verification-only paths (GEKKO_SHIPPING)" OFF)
if(SHADOW_SHIPPING)
    set(SHADOW_ENABLE_TRACY OFF CACHE BOOL "" FORCE)
    add_compile_definitions(GEKKO_SHIPPING=1)
endif()
# native/win N1 (docs/NATIVE_WINDOWS_MIGRATION.md): the GameCube memory-card block-device model (NativeCard) is the
# contract ORACLE of the save store in the reference tests; the game itself always saves through the store. It is
# compiled only where the oracle is needed (the tests tree by default). OFF = no card model in the image: System has no
# card, no card tick on the game clock, no card owners.
option(SHADOW_CARD_MODEL "Compile the memory-card block-device model (the save store's test oracle)" ${SHADOW_BUILD_TESTS})
if(SHADOW_CARD_MODEL)
    add_compile_definitions(GEKKO_CARD_MODEL=1)
endif()
# s1/mtn round 4: the five Mtn NodeWalk callbacks folded into the owned walk (docs/agents/s1-mtn.md). OFF = the guest bodies run as
# before; ON = folded by default (GEKKO_MTN_FOLD=0 still disables at run time, GEKKO_MTN_FOLD=1 enables when the option is OFF).
option(SHADOW_MTN_FOLD "Fold the Mtn NodeWalk callbacks into the owned walk by default (quiet window 2026-11-12: -1.3% game-thread CPU)" ON)
if(SHADOW_MTN_FOLD)
    set_source_files_properties(runtime/src/gekko/native_mtn_pose.cpp PROPERTIES COMPILE_DEFINITIONS GEKKO_MTN_FOLD_DEFAULT=1)
endif()
# s1/mtn round 5: the MotionUpdate tree (0x80416EF4: sampler pick + bone pass native) as an HLE hook (docs/agents/s1-mtn.md). OFF = the guest
# body runs; ON = native by default (GEKKO_MTN_TREE=0 disables at run time, GEKKO_MTN_TREE=1 enables when the option is OFF).
option(SHADOW_MTN_TREE "Run the Mtn MotionUpdate tree natively by default (quiet window 2026-11-13: -0.4% game-thread CPU, 5/5)" ON)
if(SHADOW_MTN_TREE)
    set_property(SOURCE runtime/src/gekko/native_mtn_pose.cpp APPEND PROPERTY COMPILE_DEFINITIONS GEKKO_MTN_TREE_DEFAULT=1)
endif()
# s1/mtn round 6, candidate D: the pose rebuild phase 0x80418618 native (HLE hook with hand-back). OFF = the guest body runs.
option(SHADOW_MTN_PHASE_D "Run the Mtn pose rebuild phase (0x80418618) natively by default (quiet window 2026-11-13, with C: -0.8% game-thread CPU, 4/5)" ON)
if(SHADOW_MTN_PHASE_D)
    set_property(SOURCE runtime/src/gekko/native_mtn_pose.cpp APPEND PROPERTY COMPILE_DEFINITIONS GEKKO_MTN_PHASE_D_DEFAULT=1)
endif()
# s1/mtn round 6, candidate C: the time advance phase 0x80419A9C native (HLE hook with hand-back). OFF = the guest body runs.
option(SHADOW_MTN_PHASE_C "Run the Mtn time advance phase (0x80419A9C) natively by default (quiet window 2026-11-13, with D: -0.8% game-thread CPU, 4/5)" ON)
if(SHADOW_MTN_PHASE_C)
    set_property(SOURCE runtime/src/gekko/native_mtn_pose.cpp APPEND PROPERTY COMPILE_DEFINITIONS GEKKO_MTN_PHASE_C_DEFAULT=1)
endif()
# s1/mtn round 7, candidate B: the clip start phase 0x8041A33C and its bind tree native (HLE hook with hand-back). OFF = the guest body runs.
option(SHADOW_MTN_PHASE_B "Run the Mtn clip start phase (0x8041A33C) natively by default (quiet window 2026-11-13: -2.5% game-thread CPU, 4/5)" ON)
if(SHADOW_MTN_PHASE_B)
    set_property(SOURCE runtime/src/gekko/native_mtn_pose.cpp APPEND PROPERTY COMPILE_DEFINITIONS GEKKO_MTN_PHASE_B_DEFAULT=1)
endif()
# Symbols for every object and program (a PDB): --watch-guest (include/gekko/guest_watch.h) then attributes accesses
# made by leaf translated rows, which have no unwind record. Diagnostic trees only; code generation is unchanged.
option(SHADOW_WATCH_SYMBOLS "Build with a PDB so the guest watch attributes leaf rows exactly" OFF)
if(SHADOW_WATCH_SYMBOLS AND MSVC)
    if(SHADOW_SHIPPING)
        message(FATAL_ERROR "SHADOW_WATCH_SYMBOLS is for diagnostic trees (the guest watch is compiled out of shipping)")
    endif()
    add_compile_options(/Zi)
    add_link_options(/DEBUG:FULL /OPT:REF /OPT:NOICF)  # no folding: identical rows keep distinct host code
endif()

# R2e (docs/lift-20261006/R2_DRIVER.md 4.3): the generator also lowers the RenderWare driver's draw-facing call
# sites (extract/lift-sites-r2.txt) into the scaffolding (runtime/scaffold/rw_driver), and the game renders through
# the native renderer (runtime/lift/render) on the UI thread. OFF builds today's game; configure ON in a build tree of
# its own (it generates and compiles its own translation). The default flips when R2f deletes the old path.
option(SHADOW_RENDER_LIFTED "Lower the RenderWare driver into the native renderer (R2e, own build tree)" OFF)

# ---------------------------------------------------------------------------
# Static recompilation
#
# Point SHADOW_RECOMP_DOL at the main.dol of your own extracted disc. The build
# then runs tools/ppcrecomp over it and compiles the resulting C++ into the
# executable, so those guest functions execute as native x86-64 instead of being
# interpreted. With the variable unset the runtime builds as a pure interpreter
# and behaves exactly as before.
#
# No game code is stored in this repository; the generated sources land in the
# build directory only.
# ---------------------------------------------------------------------------
set(SHADOW_RECOMP_DOL "" CACHE FILEPATH "main.dol to statically recompile (optional)")
set(SHADOW_RECOMP_FUNCTIONS "${CMAKE_CURRENT_SOURCE_DIR}/extract/functions.csv"
    CACHE FILEPATH "Function address/size table driving the recompiler")
# Code-mod variant (docs/mods/codemods/RECOMPILE.md): the retail PAL main.dol that SHADOW_RECOMP_DOL is an in-place
# patched copy of. When set, the SHA-1 gate is replaced by "same size and section header as that retail DOL".
set(SHADOW_RECOMP_VARIANT_OF "" CACHE FILEPATH "retail PAL main.dol that SHADOW_RECOMP_DOL is a patched copy of (optional)")
set(SHADOW_CODE_VARIANT "" CACHE STRING "code-mod variant: the signature of the chosen codes it is built for (build_variant.py)")
if(NOT SHADOW_CODE_VARIANT STREQUAL "")
    set_property(SOURCE runtime/src/shadow_main.cpp APPEND PROPERTY COMPILE_DEFINITIONS
                 "SHADOW_CODE_VARIANT=\"${SHADOW_CODE_VARIANT}\"")
endif()
set(SHADOW_RECOMP_ONLY "" CACHE FILEPATH "Optional file of guest addresses to restrict to")
set(SHADOW_RECOMP_SHARDS "48" CACHE STRING "Number of generated translation units")

# ---- which release the DOL is (docs/REGION_FREE_BUILDER.md) ----------------
#
# Every guest address in this tree is a canonical (PAL GUPP8P) address. The DOL
# is named by its SHA-1 against extract/discs.json, so an unknown or modified
# main.dol is refused here instead of producing a game whose hard-coded
# addresses point into the wrong code. Another known release (NTSC-U GUPE8P,
# NTSC-J GUPJ8P) has a verified address map (extract/address-maps), but its
# executable also needs the runtime's own addresses mapped (step 2 of the
# design), so configuring one is refused until that step lands.
set(SHADOW_DISC_ID "")
if(NOT SHADOW_RECOMP_DOL STREQUAL "")
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(_shadow_variant_args "")
    if(NOT SHADOW_RECOMP_VARIANT_OF STREQUAL "")
        set(_shadow_variant_args --variant-of "${SHADOW_RECOMP_VARIANT_OF}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SHADOW_RECOMP_VARIANT_OF}")
        message(STATUS "SHADOW_RECOMP_DOL is a code-mod variant of ${SHADOW_RECOMP_VARIANT_OF}")
    endif()
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tools/ppcrecomp/disc_map.py" identify
                --catalog "${CMAKE_CURRENT_SOURCE_DIR}/extract/discs.json" --dol "${SHADOW_RECOMP_DOL}"
                --field game_id ${_shadow_variant_args}
        OUTPUT_VARIABLE SHADOW_DISC_ID
        ERROR_VARIABLE _shadow_disc_error
        RESULT_VARIABLE _shadow_disc_result
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _shadow_disc_result EQUAL 0)
        message(FATAL_ERROR "SHADOW_RECOMP_DOL: ${_shadow_disc_error}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                 "${SHADOW_RECOMP_DOL}" "${CMAKE_CURRENT_SOURCE_DIR}/extract/discs.json")
    if(NOT SHADOW_DISC_ID STREQUAL "GUPP8P")
        message(FATAL_ERROR "SHADOW_RECOMP_DOL is ${SHADOW_DISC_ID}, a known release with a verified address map "
                            "(extract/address-maps/${SHADOW_DISC_ID}.map); building it needs step 2 of "
                            "docs/REGION_FREE_BUILDER.md (the runtime's addresses mapped). Use the PAL GUPP8P "
                            "main.dol until then.")
    endif()
    message(STATUS "Recompiling ${SHADOW_DISC_ID} (canonical address space)")
endif()

# ---- host instruction set for the translated guest code (CPU-08) -----------
#
# MSVC's x64 default is /arch:SSE2 - see the /arch (x64) documentation: "If you
# don't specify a /arch option, the default instruction set is SSE2". That left
# the 64 generated recomp shards, and the runtime that serves them, compiled for
# a 2003 baseline.
#
# This recompilation is PowerPC-to-x86-64 for a big-endian machine, so the code
# is dominated by rotate-and-mask, variable shifts and byte swaps - precisely
# BMI2 (SHLX/SHRX/SARX/RORX), BMI1 (ANDN/BZHI), LZCNT/TZCNT and MOVBE. Scalar
# floating point additionally gains three-operand VEX forms.
#
# /fp:precise is explicit: operations that require separate rounding must not
# be implicitly contracted. Operations that require fused rounding use the
# numeric helpers or FMA intrinsics explicitly, including native matrix math.
#
# Lower it if the binary must run on an older CPU and validate that build.
set(SHADOW_X64_ARCH "AVX2" CACHE STRING
    "MSVC /arch: level for the runtime and the generated recomp shards")
set_property(CACHE SHADOW_X64_ARCH PROPERTY STRINGS SSE2 SSE4.2 AVX AVX2)
if(MSVC)
    if(SHADOW_X64_ARCH STREQUAL "SSE2")
        # MSVC rejects an explicit /arch:SSE2 on x64; the default already is it.
        set(SHADOW_ARCH_FLAGS /fp:precise)
    else()
        set(SHADOW_ARCH_FLAGS /arch:${SHADOW_X64_ARCH} /fp:precise)
    endif()
    message(STATUS "Host ISA for guest code: /arch:${SHADOW_X64_ARCH} /fp:precise")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    # The /fp:precise counterpart (Android, docs/ANDROID_BUILD.md): no implicit contraction of a*b+c into a fused
    # multiply-add - clang contracts by default on arm64, which would round the guest's separately rounded operations
    # once instead of twice. Fused rounding stays explicit (the numeric helpers), as on MSVC.
    set(SHADOW_ARCH_FLAGS -ffp-contract=off)
    message(STATUS "Host floating point for guest code: -ffp-contract=off")
else()
    set(SHADOW_ARCH_FLAGS)
endif()

# L0r0-01: the host ISA is a USAGE REQUIREMENT of runtime/include/gekko, not a private
# flag of the runtime and shard targets. gekko/ppc_ops.h selects the definition of inline
# guest-semantics helpers by __AVX2__/__FMA__ (cntlzw: LZCNT or BSR; fused_multiply_add:
# FMA3 or the C library fma), and every TU that uses a GEKKO_NOINLINE Cpu accessor
# (cpu.h Cpu::mem_read32 ...) emits its own COMDAT for it. Any TU that includes those
# headers and is linked into the same image as the shards must therefore be compiled for
# the same model, or the image is ill-formed (ODR) and the linker keeps whichever copy it
# sees first - which, until this, was shadow_main.obj's baseline-SSE2 Cpu::mem_read32 for
# every shard call site in both executables. Everything that links the runtime or the
# generated code inherits the flags from here; gekko/types.h makes the linker reject a
# TU that still differs (LNK2038 on the gekko_host_* keys), and ctest
# native_host_isa_consistency checks the whole tree.
add_library(shadow_host_isa INTERFACE)
target_compile_options(shadow_host_isa INTERFACE ${SHADOW_ARCH_FLAGS})

# L0r0-02: an image compiled for the host ISA also CHECKS it before relying on it. Without this the
# /arch:AVX2 images executed LZCNT, FMA3, BMI1/BMI2 and AVX2 with no CPUID test anywhere and died with an
# unexplained STATUS_ILLEGAL_INSTRUCTION on an older processor (or, for LZCNT, silently computed BSR).
# runtime/src/host_isa/host_isa_gate.cpp is a C initializer (.CRT$XIU) that runs before every C++
# initializer and main, reads CPUID/XGETBV and refuses with a diagnostic (exit code 29) when the level is not
# available. It is an OBJECT library that does NOT link shadow_host_isa: it must be baseline x86-64 code, and
# it includes no gekko header. Its objects are a link item of shadow_host_isa itself, so exactly the images
# that inherit the /arch flags (everything that links the runtime or the generated code) get the gate - the
# same usage-requirement model as L0r0-01. ctest native_host_isa_gate and host_isa_gate_tests check it.
set(SHADOW_HOST_ISA_LEVELS SSE2 SSE4.2 AVX AVX2 AVX512)
if(NOT SHADOW_X64_ARCH IN_LIST SHADOW_HOST_ISA_LEVELS)
    message(FATAL_ERROR "SHADOW_X64_ARCH '${SHADOW_X64_ARCH}' has no host ISA gate requirement set; "
                        "supported: ${SHADOW_HOST_ISA_LEVELS} (runtime/src/host_isa/host_isa_gate.h)")
endif()
if(MSVC)
    string(REPLACE "." "_" SHADOW_HOST_ISA_TOKEN "${SHADOW_X64_ARCH}")
    add_library(shadow_host_isa_gate OBJECT runtime/src/host_isa/host_isa_gate.cpp)
    target_include_directories(shadow_host_isa_gate PUBLIC runtime/src/host_isa)
    target_compile_definitions(shadow_host_isa_gate PRIVATE SHADOW_HOST_ISA_REQUIRE_${SHADOW_HOST_ISA_TOKEN}=1)
    target_compile_features(shadow_host_isa_gate PRIVATE cxx_std_20)
    target_compile_options(shadow_host_isa_gate PRIVATE /W4 /permissive-)
    set_property(TARGET shadow_host_isa_gate PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_link_libraries(shadow_host_isa INTERFACE $<TARGET_OBJECTS:shadow_host_isa_gate>)
endif()

# ---- prebuilt game-code-free parts (docs/BUILDING.md "Prebuilt parts") ---------------------------------------------
# The release package may carry the parts of the build that contain no game code, compiled once by the release step
# (dev/Package-Release.ps1) in a tree configured WITHOUT a main.dol (SHADOW_PREBUILT_EXPORT): the runtime libraries,
# the recompiler, the controller configurator and the mod manager. A player's tree uses them only when everything that shapes their
# compiled bytes matches exactly (shadow_prebuilt_key: compiler version, Windows SDK, generator, flags, options, a hash
# of every source file); otherwise it builds them, as before. The recompiled game code, everything generated from the
# player's main.dol and the executable that embeds it are always built on the player's PC. The two lists below are the
# only place the prebuilt parts are named: the release step exports exactly these targets.
# (shadow_msl is not one: fdlibm's constant tables, the digits of 2/pi among them, are byte for byte the game's.)
set(SHADOW_PREBUILT_LIBRARIES shadow_native_execution shadow_native_memory shadow_native_gameplay shadow_lift
    shadow_assets shadow_mods shadow_motion shadow_d3d12ma shadow_render shadow_rwim shadow_render_lifted shadow_runtime)
# OBJECT libraries whose objects the static libraries above already hold (shadow_runtime holds shadow_runtime_common's).
set(SHADOW_PREBUILT_FOLDED_OBJECT_LIBRARIES shadow_runtime_common)
set(SHADOW_PREBUILT_PROGRAMS ppcrecomp ShadowPadConfig ShadowModManager)
# Sources of those libraries that are never prebuilt: they hold data taken from the game (dev/scan_game_code.py found
# their objects sharing bytes with the disc), so they are compiled on the player's PC from this package's source, as
# before. The prebuilt archives lack their objects; the player's tree compiles them into the same targets.
#   native_audio_mixer.cpp: the AX compressor's gain table (main.dol 0x80563420..0x80564E5F).
#   button_glyphs.cpp (button_glyph_data.inc): palettes and texel blocks that match the game's button sheets.
#   native_boot.cpp, os_exception_entry.cpp: the SDK's exception vector, 38 PowerPC words (gekko/os_exception_vector.h)
#   the boot writes into guest memory and the exception entry checks; native_boot.cpp also has a table of the OS's.
#   runtime/mods/afs.cpp: the names of the memory-card banner and icon files it checks, which the compiler pools next
#   to each other in the disc's own order (a 70-byte run of the disc's file table). ModManager.exe keeps a
#   48-byte part of it, under the scan's limits.
set(SHADOW_PREBUILT_LOCAL_SOURCES
    runtime/mods/afs.cpp
    runtime/src/native_audio_mixer.cpp
    runtime/src/gekko/button_glyphs.cpp
    runtime/src/gekko/native_boot.cpp
    runtime/src/gekko/os_exception_entry.cpp)
set(SHADOW_PREBUILT_DIR "" CACHE PATH "Folder of prebuilt game-code-free parts (the release package's prebuilt/)")
option(SHADOW_PREBUILT_EXPORT "Release step: build the prebuilt parts (requires an empty SHADOW_RECOMP_DOL)" OFF)

# Everything that decides the bytes of the prebuilt parts, one "name=value" line each.
function(shadow_prebuilt_key out_var)
    file(GLOB_RECURSE _sources LIST_DIRECTORIES false RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
         "${CMAKE_CURRENT_SOURCE_DIR}/runtime/*" "${CMAKE_CURRENT_SOURCE_DIR}/rwcore/*"
         "${CMAKE_CURRENT_SOURCE_DIR}/rpworld/*" "${CMAKE_CURRENT_SOURCE_DIR}/tools/*"
         "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/*")
    list(FILTER _sources EXCLUDE REGEX "(^|/)__pycache__/|\\.pyc$")
    list(APPEND _sources CMakeLists.txt CMakePresets.json)
    list(SORT _sources)
    set(_listing "")
    foreach(_file IN LISTS _sources)
        file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/${_file}" _hash)
        string(APPEND _listing "${_file} ${_hash}\n")
    endforeach()
    string(SHA256 _source_hash "${_listing}")
    list(LENGTH _sources _source_count)
    set(_key
        "compiler=${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
        "toolset=${CMAKE_VS_PLATFORM_TOOLSET} ${CMAKE_VS_PLATFORM_TOOLSET_VERSION}"
        "windows_sdk=${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}"
        "generator=${CMAKE_GENERATOR} ${CMAKE_VS_PLATFORM_NAME}"
        "cxx_flags=${CMAKE_CXX_FLAGS}"
        "cxx_flags_release=${CMAKE_CXX_FLAGS_RELEASE}"
        "rc_flags=${CMAKE_RC_FLAGS}"
        "options=X64_ARCH:${SHADOW_X64_ARCH} SHIPPING:${SHADOW_SHIPPING} RENDER_LIFTED:${SHADOW_RENDER_LIFTED} PLUME:${SHADOW_ENABLE_PLUME} TRACY:${SHADOW_ENABLE_TRACY} WATCH_SYMBOLS:${SHADOW_WATCH_SYMBOLS} TESTS:${SHADOW_BUILD_TESTS} REFERENCE:${SHADOW_BUILD_REFERENCE}"
        "sources=${_source_count} files ${_source_hash}")
    set(${out_var} "${_key}" PARENT_SCOPE)
endfunction()

set(SHADOW_PREBUILT_USED "" CACHE INTERNAL "The prebuilt set this tree links (empty: everything is built here)")
set(SHADOW_PREBUILT_USED "")
if(SHADOW_PREBUILT_EXPORT)
    if(NOT SHADOW_RECOMP_DOL STREQUAL "" OR SHADOW_PREBUILT_DIR)
        message(FATAL_ERROR "SHADOW_PREBUILT_EXPORT builds the prebuilt parts with no game input: configure it with "
                            "-DSHADOW_RECOMP_DOL= (empty) and no SHADOW_PREBUILT_DIR")
    endif()
    shadow_prebuilt_key(_shadow_key)
    string(REPLACE ";" "\n" _shadow_key "${_shadow_key}")
    file(WRITE "${CMAKE_BINARY_DIR}/prebuilt-export/key.txt" "${_shadow_key}\n")
elseif(SHADOW_PREBUILT_DIR)
    shadow_prebuilt_key(_shadow_key)
    file(GLOB _shadow_manifests "${SHADOW_PREBUILT_DIR}/*/prebuilt.json")
    set(_shadow_reasons "")
    foreach(_manifest IN LISTS _shadow_manifests)
        get_filename_component(_set_dir "${_manifest}" DIRECTORY)
        get_filename_component(_set_name "${_set_dir}" NAME)
        file(READ "${_manifest}" _json)
        string(JSON _count ERROR_VARIABLE _json_error LENGTH "${_json}" key)
        if(_json_error)
            list(APPEND _shadow_reasons "${_set_name}: unreadable prebuilt.json")
            continue()
        endif()
        set(_theirs "")
        if(_count GREATER 0)
            math(EXPR _last "${_count} - 1")
            foreach(_i RANGE ${_last})
                string(JSON _line GET "${_json}" key ${_i})
                list(APPEND _theirs "${_line}")
            endforeach()
        endif()
        set(_mismatch "")
        foreach(_line IN LISTS _shadow_key)
            if(NOT "${_line}" IN_LIST _theirs)
                string(REGEX REPLACE "=.*" "" _name "${_line}")
                list(APPEND _mismatch "${_name}")
            endif()
        endforeach()
        list(LENGTH _shadow_key _ours)
        if(NOT _mismatch STREQUAL "" OR NOT _count EQUAL _ours)
            list(APPEND _shadow_reasons "${_set_name}: differs in ${_mismatch}")
            continue()
        endif()
        # Every file must be the one the release step recorded.
        string(JSON _files LENGTH "${_json}" files)
        math(EXPR _last "${_files} - 1")
        set(_intact TRUE)
        foreach(_i RANGE ${_last})
            string(JSON _path GET "${_json}" files ${_i} path)
            string(JSON _want GET "${_json}" files ${_i} sha256)
            if(NOT EXISTS "${_set_dir}/${_path}")
                set(_intact FALSE)
                break()
            endif()
            file(SHA256 "${_set_dir}/${_path}" _have)
            if(NOT _have STREQUAL _want)
                set(_intact FALSE)
                break()
            endif()
        endforeach()
        if(NOT _intact)
            list(APPEND _shadow_reasons "${_set_name}: a file is missing or changed")
            continue()
        endif()
        set(SHADOW_PREBUILT_USED "${_set_dir}")
        break()
    endforeach()
    set(SHADOW_PREBUILT_USED "${SHADOW_PREBUILT_USED}" CACHE INTERNAL "" FORCE)
    if(SHADOW_PREBUILT_USED)
        message(STATUS "Prebuilt game-code-free parts: ${SHADOW_PREBUILT_USED}")
    else()
        message(STATUS "Prebuilt game-code-free parts not used, everything is built here (${_shadow_reasons})")
        message(STATUS "This tree: ${_shadow_key}")
    endif()
endif()

find_package(Threads REQUIRED)
add_library(shadow_native_execution STATIC runtime/src/native_execution.cpp)
target_compile_features(shadow_native_execution PUBLIC cxx_std_20)
target_link_libraries(shadow_native_execution PUBLIC Threads::Threads shadow_host_isa)
if(MSVC)
    target_compile_options(shadow_native_execution PRIVATE /W4 /permissive-)
    set_property(TARGET shadow_native_execution PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    target_compile_options(shadow_native_execution PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(shadow_native_memory STATIC runtime/src/native_memory.cpp)
target_compile_features(shadow_native_memory PUBLIC cxx_std_20)
target_link_libraries(shadow_native_memory PUBLIC shadow_host_isa)
if(WIN32)
    target_link_libraries(shadow_native_memory PRIVATE psapi)
endif()
if(MSVC)
    target_compile_options(shadow_native_memory PRIVATE /W4 /permissive-)
    set_property(TARGET shadow_native_memory PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    target_compile_options(shadow_native_memory PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(shadow_native_gameplay STATIC runtime/src/native_gameplay.cpp)
target_compile_features(shadow_native_gameplay PUBLIC cxx_std_20)
target_link_libraries(shadow_native_gameplay PUBLIC shadow_native_execution shadow_native_memory)
if(MSVC)
    target_compile_options(shadow_native_gameplay PRIVATE /W4 /permissive-)
    set_property(TARGET shadow_native_gameplay PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    target_compile_options(shadow_native_gameplay PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---- the game's MSL math library, native (docs/NATIVE_MSL_MATH.md) ------------
# fdlibm as the game compiled it: pure C++, no CPU state. Shared by the lifted subsystems and the native RenderWare
# core (RwMatrixRotate's sin / cos).
add_library(shadow_msl STATIC
    runtime/lift/math/msl.cpp)
target_include_directories(shadow_msl PUBLIC runtime/lift)
target_compile_features(shadow_msl PUBLIC cxx_std_20)
target_link_libraries(shadow_msl PUBLIC shadow_host_isa)
if(MSVC)
    target_compile_options(shadow_msl PRIVATE /W4 /permissive-)
    set_property(TARGET shadow_msl PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    target_compile_options(shadow_msl PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---- native RenderWare core (rwcore/README.md) --------------------------------
# RenderWare Graphics 3.7 rebuilt from the game's own RenderWare functions as plain x64 C++20: no GameCube code, no
# guest memory, no dependency on the emulator runtime (only on the game's native MSL math). Each module is proven
# against the original instructions by a reference-interpreter test (runtime/tests/rwcore_*_tests.cpp).
add_library(rwcore STATIC
    rwcore/src/binary.cpp
    rwcore/src/camera.cpp
    rwcore/src/device.cpp
    rwcore/src/engine.cpp
    rwcore/src/error.cpp
    rwcore/src/frame.cpp
    rwcore/src/image.cpp
    rwcore/src/list.cpp
    rwcore/src/plugin.cpp
    rwcore/src/pluginstream.cpp
    rwcore/src/palquant.cpp
    rwcore/src/raster.cpp
    rwcore/src/resample.cpp
    rwcore/src/resources.cpp
    rwcore/src/stream.cpp
    rwcore/src/string.cpp
    rwcore/src/texture.cpp
    rwcore/src/matrix.cpp
    rwcore/src/memory.cpp
    rwcore/src/vector.cpp)
target_include_directories(rwcore PUBLIC rwcore/include)
target_compile_features(rwcore PUBLIC cxx_std_20)
target_link_libraries(rwcore PRIVATE shadow_msl)
if(MSVC)
    target_compile_options(rwcore PRIVATE /W4 /permissive- /fp:precise)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")   # clang-cl: /fp:precise still contracts a*b+c within an expression
        target_compile_options(rwcore PRIVATE /clang:-ffp-contract=off)
    endif()
    set_property(TARGET rwcore PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    # bit-identical results need every a*b+c rounded twice unless the source fuses it (std::fmaf)
    target_compile_options(rwcore PRIVATE -Wall -Wextra -Wpedantic -ffp-contract=off)
endif()

# The native RenderWare world plugin (rpworld/include/rw/world, rwcore/README.md "rpworld"): same build rules as
# rwcore (bit-identical floating point: no contraction).
add_library(rpworld STATIC
    rpworld/src/light.cpp
    rpworld/src/material.cpp
    rpworld/src/world.cpp)
target_include_directories(rpworld PUBLIC rpworld/include)
target_compile_features(rpworld PUBLIC cxx_std_20)
target_link_libraries(rpworld PUBLIC rwcore PRIVATE shadow_msl)
if(MSVC)
    target_compile_options(rpworld PRIVATE /W4 /permissive- /fp:precise)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        target_compile_options(rpworld PRIVATE /clang:-ffp-contract=off)
    endif()
    set_property(TARGET rpworld PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    target_compile_options(rpworld PRIVATE -Wall -Wextra -Wpedantic -ffp-contract=off)
endif()

# ---- lifted subsystems (docs/lift-20261006) ----------------------------------
# The game's code rewritten as native C++ that knows nothing of the translated
# machine (tools/lift/check_purity.py, test lift_purity). Translated code reaches
# it only through the generator's lowered call sites (extract/lift-sites.txt).
add_library(shadow_lift STATIC
    runtime/lift/audio/reverb.cpp
    runtime/lift/audio/output_stage.cpp
    runtime/lift/audio/effect_stage.cpp
    runtime/lift/math/scalar.cpp
    runtime/lift/math/matrix.cpp
    runtime/lift/math/hermite.cpp
    runtime/lift/object/type_cast.cpp
    runtime/lift/peripheral/controls.cpp
    runtime/lift/peripheral/gamepad_binding.cpp
    runtime/lift/peripheral/keyboard_layout.cpp
    runtime/lift/peripheral/action_map.cpp
    runtime/lift/peripheral/mouse_look.cpp
    runtime/lift/peripheral/manager_imp_pc.cpp
    # NE-RW2c (HW_REPORT Y): the game's 2D layout policy, plain values (it left gekko/native_hud + display).
    runtime/lift/ui/hud_layout.cpp)
target_include_directories(shadow_lift PUBLIC runtime/lift)
target_compile_features(shadow_lift PUBLIC cxx_std_20)
target_link_libraries(shadow_lift PUBLIC shadow_host_isa shadow_msl)
if(MSVC)
    target_compile_options(shadow_lift PRIVATE /W4 /permissive-)
    set_property(TARGET shadow_lift PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    target_compile_options(shadow_lift PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---- the asset importers (docs/lift-20261006/R2_DRIVER.md 2.3) ----------------
# Disc bytes in, native data out: PRS, ONE archives, RenderWare chunk streams,
# texture dictionaries, texel decoding, mip generation. Pure C++, no device.
add_library(shadow_assets STATIC
    runtime/lift/assets/archive.cpp
    runtime/lift/assets/chunk_stream.cpp
    runtime/lift/assets/geometry_import.cpp
    runtime/lift/assets/light_slots.cpp
    runtime/lift/assets/material_import.cpp
    runtime/lift/assets/texel_decode.cpp
    runtime/lift/assets/texture_dictionary.cpp
    runtime/lift/assets/texture_import.cpp)
target_include_directories(shadow_assets PUBLIC runtime/lift)
target_compile_features(shadow_assets PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(shadow_assets PRIVATE /W4 /permissive-)
    set_property(TARGET shadow_assets PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    target_compile_options(shadow_assets PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---- mods: the mod loader's model, installer and plan (runtime/mods/include/shadow/mods/*.h), shared by the game and the
# mod manager. Its own include root: it uses no gekko/ header (native_host_isa_consistency keys on runtime/include).
# Pure C++20 plus Win32 process start for archive types the native zip reader does not take; no game code.
add_library(shadow_mods STATIC
    runtime/mods/json.cpp
    runtime/mods/mods.cpp
    runtime/mods/afs.cpp
    runtime/mods/install.cpp
    runtime/mods/insight.cpp)
target_include_directories(shadow_mods PUBLIC runtime/mods/include PRIVATE thirdparty/stb)
target_compile_features(shadow_mods PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(shadow_mods PRIVATE /W4 /permissive- /utf-8)
    set_property(TARGET shadow_mods PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    target_compile_options(shadow_mods PRIVATE -Wall -Wextra)
endif()

# The mod library's tests: JSON, versions, manifests, state, resolution, plan, AFS rebuild, zip reader, installer.
# Defined in every tree (cheap, no game code) so `--target mod_loader_tests` works in a watch tree too.
add_executable(mod_loader_tests runtime/tests/mod_loader_tests.cpp)
target_link_libraries(mod_loader_tests PRIVATE shadow_mods)
target_include_directories(mod_loader_tests PRIVATE thirdparty/stb)
if(MSVC)
    target_compile_options(mod_loader_tests PRIVATE /W4 /permissive- /utf-8)
    set_property(TARGET mod_loader_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
if(NOT SHADOW_BUILD_TESTS)
    set_target_properties(mod_loader_tests PROPERTIES EXCLUDE_FROM_ALL ON)
endif()

# The data-code engine's tests (Gecko code types against a fake memory; no game code).
add_executable(code_engine_tests runtime/tests/code_engine_tests.cpp runtime/src/gekko/code_engine.cpp)
target_include_directories(code_engine_tests PRIVATE runtime/include)
target_compile_features(code_engine_tests PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(code_engine_tests PRIVATE /W4 /permissive- /utf-8)
    set_property(TARGET code_engine_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
if(NOT SHADOW_BUILD_TESTS)
    set_target_properties(code_engine_tests PROPERTIES EXCLUDE_FROM_ALL ON)
endif()

# ModManager.exe (target ShadowModManager): the mod manager (window + command line), beside the game executable.
# Windows only. The prebuilt file keeps the target's name (prebuilt/ShadowModManager.exe); the installed one is ModManager.exe.
# The manager and the game both use the mods/ folder beside their own executable, so they share bin/<config>.
set(SHADOW_MOD_MANAGER_DATA region_diff.json reloaded_features.json file_contexts.json)
if(WIN32 AND SHADOW_PREBUILT_USED)
    # The prebuilt manager, put where the build would have written it, with its maps of the game.
    set(_shadow_mod_data_copy "")
    foreach(_data IN LISTS SHADOW_MOD_MANAGER_DATA)
        list(APPEND _shadow_mod_data_copy COMMAND ${CMAKE_COMMAND} -E copy_if_different
             "${CMAKE_CURRENT_SOURCE_DIR}/test_data/mods/${_data}" "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/mods-data/${_data}")
    endforeach()
    add_custom_target(ShadowModManager ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/mods-data"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SHADOW_PREBUILT_USED}/ShadowModManager.exe"
                "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/ModManager.exe"
        ${_shadow_mod_data_copy}
        DEPENDS "${SHADOW_PREBUILT_USED}/ShadowModManager.exe"
        COMMENT "Using the prebuilt mod manager (ModManager.exe)"
        VERBATIM)
elseif(WIN32)
    # The text size and colours are shared with the settings program (tools/ui, appearance.cfg).
    add_executable(ShadowModManager WIN32 tools/mod_manager/mod_manager.cpp tools/mod_manager/mod_manager.rc tools/ui/appearance.cpp
                   tools/ui/theme.cpp)
    target_include_directories(ShadowModManager PRIVATE tools/ui)
    set_target_properties(ShadowModManager PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin" OUTPUT_NAME ModManager)
    target_link_libraries(ShadowModManager PRIVATE shadow_mods comctl32 shell32 ole32 windowscodecs dwmapi uxtheme
                          msimg32 comdlg32 advapi32)
    target_compile_definitions(ShadowModManager PRIVATE UNICODE _UNICODE)
    if(MSVC)
        target_compile_options(ShadowModManager PRIVATE /W4 /permissive- /utf-8)
        set_property(TARGET ShadowModManager PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    # The manager's maps of the game (docs/mods/map/), beside it as mods-data/.
    add_custom_command(TARGET ShadowModManager POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:ShadowModManager>/mods-data"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/test_data/mods/region_diff.json"
                "$<TARGET_FILE_DIR:ShadowModManager>/mods-data/region_diff.json"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/test_data/mods/reloaded_features.json"
                "$<TARGET_FILE_DIR:ShadowModManager>/mods-data/reloaded_features.json"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/test_data/mods/file_contexts.json"
                "$<TARGET_FILE_DIR:ShadowModManager>/mods-data/file_contexts.json"
        VERBATIM)
endif()

# The PC save wording on save tables of both discs and of mods (docs/mods/map/REGION_DIFF.md). Takes the game's
# files/ directories as arguments; without them it reports SKIP.
add_executable(save_text_region_tests runtime/tests/save_text_region_tests.cpp runtime/src/gekko/native_save_text.cpp)
target_include_directories(save_text_region_tests PRIVATE runtime/include)
target_compile_features(save_text_region_tests PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(save_text_region_tests PRIVATE /W4 /permissive- /utf-8)
    set_property(TARGET save_text_region_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
if(NOT SHADOW_BUILD_TESTS)
    set_target_properties(save_text_region_tests PROPERTIES EXCLUDE_FROM_ALL ON)
endif()

# ---- motion: the game's skeleton (.BON) posed by a motion clip (.MTP), evaluated as the game does ----
# Pure C++ (no guest memory): the far-object proxies pose enemy stand-ins with it (docs/DRAW_DISTANCE.md).
add_library(shadow_motion STATIC
    runtime/lift/motion/curves.cpp
    runtime/lift/motion/pose_math.cpp
    runtime/lift/motion/skeleton_pose.cpp)
target_include_directories(shadow_motion PUBLIC runtime/lift)
target_compile_features(shadow_motion PUBLIC cxx_std_20)
target_link_libraries(shadow_motion PUBLIC shadow_lift)  # math/msl: the game's sin / cos / acos
set_property(TARGET shadow_motion PROPERTY CXX_STANDARD 20)
if(MSVC)
    target_compile_options(shadow_motion PRIVATE /W4 /permissive- /fp:precise)
    set_property(TARGET shadow_motion PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    target_compile_options(shadow_motion PRIVATE -Wall -Wextra -Wpedantic -ffp-contract=off)
endif()
add_executable(motion_pose_check tools/motion/motion_pose_check.cpp)
target_link_libraries(motion_pose_check PRIVATE shadow_motion)
if(MSVC)
    set_property(TARGET motion_pose_check PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# ---- the native renderer (docs/lift-20261006/R_DESIGN.md) ----------------------
# Direct3D 12 owned directly, Shader Model 6.6, bindless. It knows neither the
# translated machine nor the console's graphics pipeline nor RenderWare
# (tools/lift/check_purity.py). Stage R2a: device, descriptors, memory, frame
# graph, swap chain, frame loop. D3D12 Memory Allocator is the renderer's own
# pinned copy (thirdparty/PINS.md); it is not linked into the game until R2e.
if(WIN32)
    # Renderer and presentation share one dependency snapshot.
    set(SHADOW_D3D12MA_DIR thirdparty/plume/contrib/D3D12MemoryAllocator)
    add_library(shadow_d3d12ma STATIC ${SHADOW_D3D12MA_DIR}/src/D3D12MemAlloc.cpp)
    target_include_directories(shadow_d3d12ma PUBLIC ${SHADOW_D3D12MA_DIR}/include)
    target_link_libraries(shadow_d3d12ma PUBLIC d3d12 dxgi dxguid)
    if(MSVC)
        set_property(TARGET shadow_d3d12ma PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()

    # Shaders: compiled by DXC at build time (SM 6.6) and embedded; a compile
    # error or warning fails the build. IEEE strictness (-Gis) keeps DXC from
    # fusing or reordering; the shaders' results are also `precise`, because
    # D3D lets the driver fuse a multiply and an add or not, and two processes'
    # compilations of one shader did differ in the last bit (R2_DRIVER.md 4.9).
    # So every compilation computes the same results (a pass that lands on
    # another pass's depth, captures equal run to run).
    find_program(SHADOW_DXC dxc
        HINTS "$ENV{WindowsSdkVerBinPath}/x64"
              "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64"
        REQUIRED)
    set(SHADOW_RENDER_SHADER_DIR "${CMAKE_BINARY_DIR}/render_shaders")
    set(SHADOW_RENDER_SHADER_HEADERS)
    foreach(entry IN ITEMS "utility|VSFullscreen|vs_6_6|render_vs_fullscreen.h"
                           "utility|VSTriangle|vs_6_6|render_vs_triangle.h"
                           "utility|PSSolid|ps_6_6|render_ps_solid.h"
                           "utility|PSTexture|ps_6_6|render_ps_texture.h"
                           "utility|PSResolve|ps_6_6|render_ps_resolve.h"
                           "utility|PSScale|ps_6_6|render_ps_scale.h"
                           "mesh|VSMesh|vs_6_6|render_vs_mesh.h"
                           "mesh|PSBase|ps_6_6|render_ps_base.h"
                           "mesh|PSEnvironment|ps_6_6|render_ps_environment.h"
                           "mesh|PSDual|ps_6_6|render_ps_dual.h"
                           "particles|VSParticle|vs_6_6|render_vs_particle.h"
                           "particles|PSParticle|ps_6_6|render_ps_particle.h"
                           "immediate|VSImmediate|vs_6_6|render_vs_immediate.h"
                           "immediate|PSImmediate|ps_6_6|render_ps_immediate.h"
                           "glare|PSGlareCapture|ps_6_6|render_ps_glare_capture.h"
                           "glare|PSGlareBlur|ps_6_6|render_ps_glare_blur.h"
                           "glare|PSGlareBox|ps_6_6|render_ps_glare_box.h"
                           "glare|PSGlareMerge|ps_6_6|render_ps_glare_merge.h"
                           "glare|PSGlareComposite|ps_6_6|render_ps_glare_composite.h"
                           "glare|PSResolveGlare|ps_6_6|render_ps_resolve_glare.h"
                           "utility|PSDepthBits|ps_6_6|render_ps_depth_bits.h"
                           "glare|CSGlareRows|cs_6_6|render_cs_glare_rows.h"
                           "glare|CSGlareColumns|cs_6_6|render_cs_glare_columns.h"
                           "glare|VSGlareTiles|vs_6_6|render_vs_glare_tiles.h"
                           "glare|CSGlareTilesClear|cs_6_6|render_cs_glare_tiles_clear.h"
                           # The glare-sources pass's variants: they mark the tiles they write (glare_tiles.hlsli).
                           "mesh|PSBase|ps_6_6|render_ps_base_tiles.h|GLARE_TILES"
                           "mesh|PSEnvironment|ps_6_6|render_ps_environment_tiles.h|GLARE_TILES"
                           "mesh|PSDual|ps_6_6|render_ps_dual_tiles.h|GLARE_TILES"
                           "particles|PSParticle|ps_6_6|render_ps_particle_tiles.h|GLARE_TILES"
                           "immediate|PSImmediate|ps_6_6|render_ps_immediate_tiles.h|GLARE_TILES"
                           "skinning|CSSkin|cs_6_6|render_cs_skin.h")
        string(REPLACE "|" ";" parts "${entry}")
        list(GET parts 0 source_name)
        list(GET parts 1 entry_point)
        list(GET parts 2 profile)
        list(GET parts 3 header)
        # An optional fifth field: a define the variant is compiled with; its array is named ...Tiles.
        set(variant_flags)
        set(variant_name "")
        list(LENGTH parts part_count)
        if(part_count GREATER 4)
            list(GET parts 4 variant_define)
            set(variant_flags -D ${variant_define}=1)
            set(variant_name "Tiles")
        endif()
        set(source "${CMAKE_CURRENT_SOURCE_DIR}/runtime/lift/render/shaders/${source_name}.hlsl")
        add_custom_command(
            OUTPUT "${SHADOW_RENDER_SHADER_DIR}/${header}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${SHADOW_RENDER_SHADER_DIR}"
            COMMAND "${SHADOW_DXC}" -nologo -T ${profile} -E ${entry_point} -O3 -WX -HV 2021 -Gis ${variant_flags}
                    -Vn kRender${entry_point}${variant_name} -Fh "${SHADOW_RENDER_SHADER_DIR}/${header}" "${source}"
            DEPENDS "${source}" "${CMAKE_CURRENT_SOURCE_DIR}/runtime/lift/render/shaders/glare_tiles.hlsli"
            COMMENT "Compiling renderer shader ${entry_point} (DXC ${profile})"
            VERBATIM)
        list(APPEND SHADOW_RENDER_SHADER_HEADERS "${SHADOW_RENDER_SHADER_DIR}/${header}")
    endforeach()
    add_custom_target(shadow_render_shaders DEPENDS ${SHADOW_RENDER_SHADER_HEADERS})

    add_library(shadow_render STATIC
        runtime/lift/render/descriptors.cpp
        runtime/lift/render/display_modes.cpp
        runtime/lift/render/effect_layers.cpp
        runtime/lift/render/frame.cpp
        runtime/lift/render/frame_interp.cpp
        runtime/lift/render/packet_stream.cpp
        runtime/lift/render/frame_graph.cpp
        runtime/lift/render/frame_loop.cpp
        runtime/lift/render/gpu_device.cpp
        runtime/lift/render/gpu_memory.cpp
        runtime/lift/render/gpu_timer.cpp
        runtime/lift/render/jit_pacer.cpp
        runtime/lift/render/pipelines.cpp
        runtime/lift/render/renderer.cpp
        runtime/lift/render/unorm_calibration.cpp
        runtime/lift/render/resource_worker.cpp
        runtime/lift/render/resources.cpp
        runtime/lift/render/skin_cache.cpp
        runtime/lift/render/swap_chain.cpp)
    if(NOT SHADOW_PREBUILT_USED)  # a prebuilt shadow_render.lib already holds the compiled shaders
        add_dependencies(shadow_render shadow_render_shaders)
    endif()
    target_include_directories(shadow_render PUBLIC runtime/lift "${SHADOW_RENDER_SHADER_DIR}")
    target_compile_features(shadow_render PUBLIC cxx_std_20)
    target_compile_definitions(shadow_render PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(shadow_render PUBLIC shadow_d3d12ma shadow_assets d3d12 dxgi dxguid)
    if(MSVC)
        target_compile_options(shadow_render PRIVATE /W4 /permissive-)
        set_property(TARGET shadow_render PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()

    # NE-RW2 (HW_REPORT Y.3): RenderWare's immediate modes as a native library over plain values (no guest memory,
    # no console layouts); the RW GameCube driver scaffold is its adapter until R4 / R6.
    add_library(shadow_rwim STATIC runtime/lift/rwim/immediate.cpp)
    target_include_directories(shadow_rwim PUBLIC runtime/lift)
    target_compile_features(shadow_rwim PUBLIC cxx_std_20)
    target_link_libraries(shadow_rwim PUBLIC shadow_render)
    if(MSVC)
        target_compile_options(shadow_rwim PRIVATE /W4 /permissive-)
        set_property(TARGET shadow_rwim PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()

    # A stage's land drawn by the new renderer straight from the disc (R2b + R2a,
    # no game code): tools/stage_preview.
    add_executable(stage_preview tools/stage_preview/stage_preview.cpp tools/stage_preview/stage_scene.cpp)
    target_link_libraries(stage_preview PRIVATE shadow_render shadow_assets)
    target_include_directories(stage_preview PRIVATE thirdparty/stb)
    target_compile_definitions(stage_preview PRIVATE _CRT_SECURE_NO_WARNINGS)
    # The offline converter of a stage's GameCube data to the native PC formats the native RenderWare core loads
    # (tools/stage_convert; rwcore/README.md "Stage conversion").
    add_executable(stage_convert tools/stage_convert/stage_convert.cpp)
    target_link_libraries(stage_convert PRIVATE shadow_assets)
    target_compile_features(stage_convert PRIVATE cxx_std_20)
    # A stage drawn entirely by the native path along a recorded game camera, into a video
    # (tools/native_stage_clip/native_stage_clip.cpp).
    add_executable(native_stage_clip tools/native_stage_clip/native_stage_clip.cpp)
    target_link_libraries(native_stage_clip PRIVATE shadow_render shadow_assets rpworld)
    target_include_directories(native_stage_clip PRIVATE thirdparty/stb)
    target_compile_definitions(native_stage_clip PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_features(native_stage_clip PRIVATE cxx_std_20)
    if(MSVC)
        set_property(TARGET native_stage_clip PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_executable(stage_load_check tools/stage_convert/stage_load_check.cpp)
    target_link_libraries(stage_load_check PRIVATE shadow_assets rpworld)
    if(MSVC)
        set_property(TARGET stage_load_check PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    if(MSVC)
        set_property(TARGET stage_convert PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    # The same, live in a window: fly through a stage (tools/stage_preview/stage_viewer.cpp).
    add_executable(stage_viewer tools/stage_preview/stage_viewer.cpp tools/stage_preview/stage_scene.cpp)
    target_link_libraries(stage_viewer PRIVATE shadow_render shadow_assets)
    # Every world of a stage as text, for measuring the levels' geometry (tools/stage_preview/world_dump.cpp).
    add_executable(world_dump tools/stage_preview/world_dump.cpp)
    target_link_libraries(world_dump PRIVATE shadow_assets)
    if(MSVC)
        set_property(TARGET world_dump PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    # A model drawn through the material program (decision table, effects, lights).
    add_executable(model_preview tools/stage_preview/model_preview.cpp)
    target_link_libraries(model_preview PRIVATE shadow_render shadow_assets)
    target_include_directories(model_preview PRIVATE thirdparty/stb)
    target_compile_definitions(model_preview PRIVATE _CRT_SECURE_NO_WARNINGS)
    if(MSVC)
        set_property(TARGET model_preview PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    if(MSVC)
        set_property(TARGET stage_preview PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        set_property(TARGET stage_viewer PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endif()

add_library(shadow_runtime_common OBJECT
    runtime/src/native_bootstrap.cpp
    runtime/src/native_display.cpp
    runtime/src/native_audio_mixer.cpp
    runtime/src/embedded_game_data.cpp
    runtime/src/gekko/guest_memory.cpp
    runtime/src/gekko/guest_watch.cpp
    runtime/src/gekko/os_exception_entry.cpp
    runtime/src/gekko/asset_trace.cpp
    runtime/src/gekko/display.cpp
    runtime/src/gekko/display_settings.cpp
    runtime/src/gekko/save_file.cpp
    runtime/src/gekko/save_import.cpp
    runtime/src/gekko/save_store.cpp
    runtime/src/gekko/gx.cpp
    runtime/src/gekko/audio_runtime.cpp
    runtime/src/gekko/timeline.cpp
    runtime/src/gekko/audio_audit.cpp
    runtime/src/gekko/host_timing.cpp
    runtime/src/gekko/process_profile.cpp
    runtime/src/gekko/host_jobs.cpp
    runtime/src/gekko/xinput_pad.cpp
    runtime/src/gekko/raw_input.cpp
    runtime/src/gekko/controls_settings.cpp
    runtime/src/gekko/gameplay_camera.cpp
    runtime/src/gekko/heap_census.cpp
    runtime/src/gekko/sonicteam_heap.cpp
    runtime/src/gekko/camera.cpp
    runtime/src/gekko/world_anchor.cpp
    runtime/src/gekko/health_bar.cpp
    runtime/src/gekko/prs.cpp
    runtime/src/gekko/asset_runtime.cpp
    runtime/src/gekko/asset_providers.cpp
    runtime/src/gekko/asset_namespace.cpp
    runtime/src/gekko/asset_mount.cpp
    runtime/backends/storage/storage_backends.cpp
    runtime/src/gekko/mod_overlay.cpp
    runtime/src/gekko/code_engine.cpp
    runtime/src/gekko/native_frame.cpp
    runtime/src/gekko/rw_capture.cpp
    runtime/src/gekko/native_renderer.cpp
    runtime/src/gekko/native_gpu.cpp
    runtime/src/gekko/render/gpu_transfer.cpp
    runtime/src/gekko/render/upload_arena.cpp
    runtime/src/gekko/native_hud.cpp
    runtime/src/gekko/bloom.cpp
    runtime/src/gekko/bloom_gpu.cpp
    runtime/src/gekko/gx_texture_decode.cpp
    runtime/src/gekko/native_assets.cpp
    runtime/src/gekko/native_file_api.cpp
    runtime/src/gekko/game_clock.cpp
    runtime/src/gekko/native_interrupts.cpp
    runtime/src/gekko/native_video.cpp
    runtime/src/gekko/native_rw_world.cpp
    runtime/src/gekko/native_rw_device_state.cpp
    runtime/src/gekko/native_rw_registry.cpp
    runtime/src/gekko/native_rw_world_decode.cpp
    runtime/src/gekko/native_rw_vector.cpp
    runtime/src/gekko/native_rw_matrix.cpp
    runtime/src/gekko/native_rw_view_cache.cpp
    runtime/src/gekko/native_audio.cpp
    runtime/src/gekko/native_reverb_owner.cpp
    runtime/src/gekko/native_axfx_effects.cpp
    runtime/src/gekko/native_os_arena_clock.cpp
    runtime/src/gekko/native_os_debug.cpp
    runtime/src/gekko/native_os_report.cpp
    runtime/src/gekko/native_trk.cpp
    runtime/src/gekko/native_os_font.cpp
    runtime/src/gekko/native_vec_math.cpp
    runtime/src/gekko/native_mtn_track.cpp
    runtime/src/gekko/native_mtn_pose.cpp
    runtime/src/gekko/native_effect_update.cpp
    runtime/src/gekko/native_mix_entries.cpp
    runtime/src/gekko/native_sound_tables.cpp
    runtime/src/gekko/native_gcax_midi.cpp
    runtime/src/gekko/native_se_manager.cpp
    runtime/src/gekko/native_se_math.cpp
    runtime/src/gekko/native_frame_limiter.cpp
    runtime/src/gekko/self_sampler.cpp
    runtime/src/gekko/native_motion_store.cpp
    # SubQ batch B003: the motion bank registry, native (docs/NATIVE_WINDOWS_MIGRATION.md section 8).
    runtime/src/gekko/native_motion_bank_store.cpp
    runtime/src/gekko/native_sound_banks.cpp
    runtime/src/gekko/native_stream_ring.cpp
    runtime/src/gekko/sound_arena.cpp
    runtime/src/gekko/native_object_range.cpp
    runtime/src/gekko/native_attached_effects.cpp
    runtime/src/gekko/native_shoe_jets.cpp
    runtime/src/gekko/native_gx_state.cpp
    runtime/src/gekko/native_gx_display.cpp
    runtime/src/gekko/native_gx_texobj.cpp
    runtime/src/gekko/native_rw_node_stops.cpp
    runtime/src/gekko/native_rw_texbind.cpp
    runtime/src/gekko/native_driver_callbacks.cpp
    runtime/src/gekko/native_rw_raster.cpp
    runtime/src/gekko/native_rw_raster_image.cpp
    runtime/src/gekko/native_rw_raster_size.cpp
    runtime/src/gekko/native_rw_display.cpp
    runtime/src/gekko/native_gx_sync.cpp
    runtime/src/gekko/native_immediate.cpp
    runtime/src/gekko/host_files.cpp
    runtime/src/gekko/native_card.cpp
    runtime/src/gekko/native_save_text.cpp
    runtime/src/gekko/voice_language.cpp
    runtime/src/gekko/voice_trace.cpp
    runtime/src/gekko/pc_settings.cpp
    runtime/src/gekko/config_file.cpp
    runtime/src/gekko/menu_backgrounds.cpp
    runtime/src/gekko/button_glyphs.cpp
    # Dolphin-named custom texture packs (--texture-pack; docs/mods/TEXTURE_PACKS.md).
    runtime/src/gekko/texture_packs.cpp
    runtime/src/gekko/native_ipl.cpp
    runtime/src/gekko/native_os_settings.cpp
    runtime/src/gekko/native_os_heap.cpp
    runtime/src/gekko/native_os_panic.cpp
    runtime/src/gekko/native_os_misc.cpp
    runtime/src/gekko/native_os_machine_check.cpp
    runtime/src/gekko/native_ax_mix_alloc.cpp
    runtime/src/gekko/native_math_affine.cpp
    runtime/src/gekko/native_math_mtx.cpp
    runtime/src/gekko/native_adx_decode.cpp
    runtime/src/gekko/native_rw_pixel.cpp
    runtime/src/gekko/native_rw_image.cpp
    runtime/src/gekko/native_os_threads.cpp
    runtime/src/gekko/native_vi_wait.cpp
    runtime/src/gekko/native_guest_scheduler.cpp  # the reference build's guest SelectThread / OSLoadContext (not in the lifted tree)
    runtime/src/gekko/native_reset.cpp
    runtime/src/gekko/native_boot.cpp
    runtime/src/gekko/native_disc_error.cpp
    runtime/src/gekko/native_link_groups.cpp
    runtime/src/gekko/native_world_collision.cpp
    runtime/src/gekko/thread_sampler.cpp
    runtime/src/gekko/native_world_line.cpp
    runtime/src/gekko/prs_ahead.cpp
    runtime/src/gekko/native_directory_scan.cpp
    runtime/src/gekko/native_cri_file_device.cpp
    runtime/src/gekko/native_rtgcnfs.cpp
    runtime/src/gekko/native_jobs.cpp
    runtime/src/gekko/native_expand_jobs.cpp
    runtime/src/gekko/native_one.cpp
    runtime/src/gekko/native_cri.cpp
    runtime/src/gekko/game_errata.cpp
    runtime/src/gekko/native_message_system.cpp
    runtime/src/gekko/native_dialogue_voice.cpp
    runtime/src/gekko/native_message_voice_owner.cpp
    runtime/src/gekko/native_svm.cpp
    runtime/src/gekko/native_sj.cpp
    runtime/src/gekko/native_adx_output.cpp
    runtime/src/gekko/game_memory.cpp
    runtime/src/gekko/gameplay_scheduler.cpp
    runtime/src/gekko/rw_geometry_convert.cpp
    runtime/src/gekko/host_window.cpp
    runtime/src/gekko/plume_present.cpp
    runtime/src/gekko/savestate.cpp
    runtime/src/gekko/audio_state.cpp
    runtime/src/gekko/dynamic_cast_intrinsic.cpp
    runtime/src/gekko/matrix_multiply_intrinsic.cpp
    runtime/src/gekko/chara_colli_intrinsic.cpp
    runtime/src/gekko/chara_colli_bone.cpp
    runtime/src/gekko/system.cpp
    runtime/src/gekko/measure_probes.cpp
    runtime/src/gekko/ui_test.cpp
    runtime/src/gekko/dev_link.cpp
    runtime/src/gekko/input_tape.cpp
    # s1/core
    runtime/src/gekko/native_s1_core.cpp
    # s1/target
    runtime/src/gekko/native_s1_target2.cpp
    runtime/src/gekko/native_s1_shptr.cpp
    runtime/src/gekko/native_s1_chars.cpp
    # cpu/opt5 (s2/sound owner)
    runtime/src/gekko/native_sound_ampdb.cpp
    # native/hal-alloc: the RwMalloc / RwFree chain above the OS heap
    runtime/src/gekko/native_rw_alloc.cpp
)
# The runtime sources the lifted (shipping) tree does not compile. One list: it removes them below, and the generator's
# liveness roots for that tree (extract/liveness-roots-lifted.txt) are generated without their literals, so guest code
# only they enter is not kept (tools/ppcrecomp/liveness_roots.py --without; the native_liveness_roots_lifted test
# refuses a roots file made for a different list).
set(SHADOW_LIFTED_UNCOMPILED_SOURCES
    runtime/src/gekko/rw_capture.cpp
    runtime/src/gekko/native_renderer.cpp
    runtime/src/gekko/native_gpu.cpp
    runtime/src/gekko/render/gpu_transfer.cpp
    runtime/src/gekko/render/upload_arena.cpp
    runtime/src/gekko/bloom.cpp
    runtime/src/gekko/bloom_gpu.cpp
    runtime/src/gekko/native_immediate.cpp
    # NE-RW2c: the legacy renderer's view of the 2D layout policy (its canvas affine); the policy is shadow_lift's.
    runtime/src/gekko/native_hud.cpp
    # The guest scheduler entry of the modes that create guest threads (the lifted build refuses them).
    runtime/src/gekko/native_guest_scheduler.cpp
    # A test of the generated body of SelectThread's idle spin (0x803788FC): it runs against the reference tree, not the lifted one.
    runtime/tests/compiler_body_tests.cpp)
if(SHADOW_RENDER_LIFTED)
    # G5 (HW_REPORT T.1): the lifted (shipping) build carries no legacy renderer: no RenderWare capture, no legacy
    # GPU / presenter / bloom, no GX immediate builder. They stay in the native tree (SHADOW_RENDER_LIFTED=OFF), the
    # R2 gate's baseline. The sources that remain read SHADOW_RENDER_LIFTED (PRIVATE: the recomp shards do not see it).
    get_target_property(SHADOW_COMMON_SOURCES shadow_runtime_common SOURCES)
    list(REMOVE_ITEM SHADOW_COMMON_SOURCES ${SHADOW_LIFTED_UNCOMPILED_SOURCES})
    set_target_properties(shadow_runtime_common PROPERTIES SOURCES "${SHADOW_COMMON_SOURCES}")
    target_compile_definitions(shadow_runtime_common PRIVATE SHADOW_RENDER_LIFTED=1)
endif()

target_include_directories(shadow_runtime_common PUBLIC runtime/include)
# stb_image (PNG) for the texture packs' decoder, compiled into texture_packs.cpp only (STB_IMAGE_STATIC).
target_include_directories(shadow_runtime_common PRIVATE thirdparty/stb)
if(SHADOW_S1_CORE_SCAN)
    target_compile_definitions(shadow_runtime_common PRIVATE SHADOW_S1_CORE_SCAN_DEFAULT=1)
endif()
if(SHADOW_S1_TARGET2)
    target_compile_definitions(shadow_runtime_common PRIVATE SHADOW_S1_TARGET2_DEFAULT=1)
endif()
if(SHADOW_S1_SHPTR)
    target_compile_definitions(shadow_runtime_common PRIVATE SHADOW_S1_SHPTR_DEFAULT=1)
endif()
if(SHADOW_S1_CHARS)
    target_compile_definitions(shadow_runtime_common PRIVATE SHADOW_S1_CHARS_DEFAULT=1)
endif()
if(SHADOW_OP3_AMPDB)
    target_compile_definitions(shadow_runtime_common PRIVATE SHADOW_OP3_AMPDB_DEFAULT=1)
endif()
if(SHADOW_CHARA_COLLI_PASS)
    target_compile_definitions(shadow_runtime_common PRIVATE SHADOW_CHARA_COLLI_PASS_DEFAULT=1)
endif()
if(SHADOW_RW_ALLOC)
    target_compile_definitions(shadow_runtime_common PRIVATE SHADOW_RW_ALLOC_DEFAULT=1)
endif()
target_compile_features(shadow_runtime_common PUBLIC cxx_std_20)
target_link_libraries(shadow_runtime_common PUBLIC shadow_host_isa shadow_native_execution shadow_native_memory shadow_native_gameplay shadow_lift shadow_assets shadow_mods)
# The development link (gekko/dev_link.h) connects to the comparison workstation over loopback TCP.
if(WIN32 AND NOT SHADOW_SHIPPING)  # ES: the dev link is compiled out of the shipping exe
    target_link_libraries(shadow_runtime_common PUBLIC ws2_32)
endif()

# The legacy GX register raster (Flipper EFB/TEV/copy emulation) was deleted
# on 2026-10-05, from both executables, together with the Bus/Cpu coherence
# machinery that existed only for it. tools/ppcrecomp/check_native_boundary.py
# refuses any of its symbols in either runtime archive and in the executable.

add_library(shadow_runtime STATIC runtime/src/gekko/cpu.cpp runtime/src/gekko/flat_memory.cpp runtime/src/gekko/indirect_call_trace.cpp runtime/src/gekko/native_guest_call.cpp runtime/src/gekko/native_s1_call.cpp runtime/src/gekko/native_fast_call.cpp)
set(SHADOW_CPU_TARGETS shadow_runtime)
set_property(TARGET shadow_runtime PROPERTY INTERFACE_SHADOW_CPU_EXECUTION native)
if(SHADOW_BUILD_TESTS OR SHADOW_BUILD_REFERENCE)
    add_library(shadow_runtime_reference STATIC runtime/src/gekko/cpu.cpp runtime/src/gekko/flat_memory.cpp runtime/src/gekko/indirect_call_trace.cpp runtime/src/gekko/native_guest_call.cpp runtime/src/gekko/native_s1_call.cpp runtime/src/gekko/native_fast_call.cpp)
    target_compile_definitions(shadow_runtime_reference PRIVATE SHADOW_CPU_REFERENCE_BUILD=1)
    set_property(TARGET shadow_runtime_reference PROPERTY INTERFACE_SHADOW_CPU_EXECUTION reference)
    list(APPEND SHADOW_CPU_TARGETS shadow_runtime_reference)
endif()
foreach(cpu_runtime IN LISTS SHADOW_CPU_TARGETS)
    target_link_libraries(${cpu_runtime} PUBLIC shadow_runtime_common)
    set_property(TARGET ${cpu_runtime} APPEND PROPERTY COMPATIBLE_INTERFACE_STRING SHADOW_CPU_EXECUTION)
endforeach()
if(SHADOW_ENABLE_TRACY)
    set(TRACY_ENABLE ON CACHE BOOL "" FORCE)
    set(TRACY_ON_DEMAND ON CACHE BOOL "" FORCE)
    # Localhost only: the client's listen socket never leaves this machine, so a
    # profiling build cannot be connected to from the network.
    set(TRACY_ONLY_LOCALHOST ON CACHE BOOL "" FORCE)
    # Broadcast ON so the running game appears in the profiler's connection list
    # by itself. It is an announcement, not an opening: ONLY_LOCALHOST above is
    # what decides who may connect, and it still refuses everything but this
    # machine. Without this the profiler shows an empty list and the address has
    # to be typed in by hand every time.
    set(TRACY_NO_BROADCAST OFF CACHE BOOL "" FORCE)
    set(TRACY_NO_FRAME_IMAGE ON CACHE BOOL "" FORCE)
    # THE GAME ONLY. A profile of this runtime must not be mixed with whatever
    # else the machine is doing. Tracy's Windows system tracing is an ETW
    # kernel-logger session, and two of its streams are machine-wide:
    #  - context switches and thread wake-ups are recorded for EVERY thread on
    #    the machine, which fills the CPU-core rows with other processes;
    #  - vsync comes from the desktop compositor, not from this process.
    # Both are off. Call-stack SAMPLING stays on: the event callback keeps only
    # samples whose stack belongs to this process (stackProcess == s_pid), so
    # it is the game's own data.
    set(TRACY_NO_CONTEXT_SWITCH ON CACHE BOOL "" FORCE)
    set(TRACY_NO_VSYNC_CAPTURE ON CACHE BOOL "" FORCE)
    add_subdirectory(thirdparty/tracy EXCLUDE_FROM_ALL)
    # The viewer's "CPU usage" plot is the third machine-wide source, and it
    # has no option: upstream reads GetSystemTimes(). A local patch in
    # client/TracySysTime.cpp reads this process's own CPU time instead when
    # this is defined. docs/tracy-local-patches/ holds the patch; re-apply it
    # after any Tracy upgrade.
    target_compile_definitions(TracyClient PRIVATE TRACY_SYSTIME_PROCESS_ONLY)
    target_link_libraries(shadow_runtime_common PUBLIC Tracy::TracyClient)
    target_compile_definitions(shadow_runtime_common PUBLIC SHADOW_TRACY=1)
    if(MSVC)
        set_property(TARGET TracyClient PROPERTY MSVC_RUNTIME_LIBRARY
                     "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endif()

foreach(runtime_target IN ITEMS shadow_runtime_common ${SHADOW_CPU_TARGETS})
    if(MSVC)
        target_compile_options(${runtime_target} PRIVATE /W4 /permissive- ${SHADOW_ARCH_FLAGS})
        set_property(TARGET ${runtime_target} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    else()
        target_compile_options(${runtime_target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endforeach()

# ---- Android: the Vulkan backend (docs/VULKAN_RENDERER.md, docs/ANDROID_BUILD.md) ----------------------------------
# render_vk is the renderer on Android (Vulkan only). It is built in this tree beside the game; the game draws through
# it once the integration hooks land (VULKAN_RENDERER.md 8, stage V7).
if(ANDROID)
    add_subdirectory(runtime/lift/render_vk)
endif()

# ---- R2e: the lifted rendering (SHADOW_RENDER_LIFTED; docs/lift-20261006/R2_DRIVER.md 4.2-4.3) ----------------------
# The scaffolding the lowered RenderWare driver sites call (every file marked; tools/lift/check_scaffold.py) and the
# launcher's handle on the native renderer (gekko/lifted_render.h, no graphics-API type in its interface). Its own
# library: the renderer's Windows definitions stay out of the runtime.
if(SHADOW_RENDER_LIFTED)
    if(NOT TARGET shadow_render)
        message(FATAL_ERROR "SHADOW_RENDER_LIFTED needs the native renderer (Windows)")
    endif()
    add_library(shadow_render_lifted STATIC
        runtime/scaffold/rw_driver/rw_driver.cpp
        runtime/scaffold/rw_driver/render_states.cpp
        runtime/scaffold/rw_driver/textures.cpp
        runtime/scaffold/rw_driver/frames.cpp
        runtime/scaffold/rw_driver/pipelines.cpp
        runtime/scaffold/rw_driver/glare.cpp
        runtime/scaffold/rw_driver/immediate.cpp
        runtime/scaffold/rw_driver/im2d_prims.cpp
        runtime/scaffold/rw_driver/im2d_prims_install.cpp
        runtime/scaffold/rw_driver/raster_render.cpp
        runtime/scaffold/rw_driver/raster_render_install.cpp
        runtime/scaffold/rw_driver/screens.cpp
        runtime/scaffold/rw_driver/projection.cpp
        runtime/scaffold/rw_driver/far_objects.cpp
        runtime/scaffold/rw_driver/material_probe.cpp
        runtime/scaffold/rw_driver/layers.cpp
        runtime/game/game_owners.cpp
        runtime/src/gekko/effect_layer_overlay.cpp
        runtime/src/gekko/lifted_render.cpp)
    # runtime/game: the game-level owners (NE-RW1) - functions of the game itself, outside the RenderWare scaffold.
    target_include_directories(shadow_render_lifted PRIVATE runtime/include runtime/scaffold runtime thirdparty/stb)
    target_compile_definitions(shadow_render_lifted PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_features(shadow_render_lifted PRIVATE cxx_std_20)
    target_link_libraries(shadow_render_lifted PRIVATE shadow_render shadow_rwim shadow_assets shadow_motion)
    if(MSVC)
        target_compile_options(shadow_render_lifted PRIVATE /W4 /permissive- ${SHADOW_ARCH_FLAGS})
        set_property(TARGET shadow_render_lifted PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    # The pixel harness for what is drawn through RwDriver (WARP adapter, test guest memory): built on request only
    # (cmake --build <lifted tree> --target rw_im2d_pixel_tests), run by hand or by ctest in a tree with testing on.
    if(WIN32)
        if(SHADOW_BUILD_TESTS)
            set(rw_im2d_pixel_tests_scope "")
        else()
            set(rw_im2d_pixel_tests_scope EXCLUDE_FROM_ALL)
        endif()
        add_executable(rw_im2d_pixel_tests ${rw_im2d_pixel_tests_scope} runtime/tests/rw_im2d_pixel_tests.cpp)
        target_include_directories(rw_im2d_pixel_tests PRIVATE runtime/include runtime/scaffold runtime thirdparty/stb)
        target_link_libraries(rw_im2d_pixel_tests PRIVATE shadow_render_lifted shadow_render shadow_rwim shadow_assets shadow_motion shadow_runtime shadow_lift)
        target_compile_features(rw_im2d_pixel_tests PRIVATE cxx_std_20)
        if(MSVC)
            target_compile_options(rw_im2d_pixel_tests PRIVATE /W4 /permissive- ${SHADOW_ARCH_FLAGS})
            set_property(TARGET rw_im2d_pixel_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME rw_im2d_pixel_tests COMMAND rw_im2d_pixel_tests)
        set_tests_properties(rw_im2d_pixel_tests PROPERTIES TIMEOUT 300 LABELS "lifted;render")
    endif()
endif()
# The native renderer's Tracy zones (runtime/lift/render/profile.h, HW 2026-10-20): only a Tracy build gives its
# libraries the define, the client and profiling.h's directory; a build without Tracy compiles them unchanged.
if(SHADOW_ENABLE_TRACY)
    foreach(render_target IN ITEMS shadow_render shadow_render_lifted)
        if(TARGET ${render_target})
            target_link_libraries(${render_target} PRIVATE Tracy::TracyClient)
            target_compile_definitions(${render_target} PRIVATE SHADOW_TRACY=1)
            target_include_directories(${render_target} PRIVATE runtime/include)
        endif()
    endforeach()
endif()

# ---- the recompiler, a host tool -------------------------------------------
# A cross build (Android, docs/ANDROID_BUILD.md) runs the generator on the build machine: it is the host-tools
# preset's ppcrecomp, imported, not compiled for the target.
if(CMAKE_CROSSCOMPILING)
    set(SHADOW_PPCRECOMP_EXECUTABLE "" CACHE FILEPATH
        "Cross builds: the build machine's ppcrecomp (cmake --build --preset host-tools)")
    if(NOT SHADOW_RECOMP_DOL STREQUAL "" AND NOT EXISTS "${SHADOW_PPCRECOMP_EXECUTABLE}")
        message(FATAL_ERROR "A cross build runs the host's ppcrecomp, and SHADOW_PPCRECOMP_EXECUTABLE "
                            "('${SHADOW_PPCRECOMP_EXECUTABLE}') does not exist: build it first with "
                            "cmake --preset host-tools && cmake --build --preset host-tools")
    endif()
    add_executable(ppcrecomp IMPORTED)
    set_property(TARGET ppcrecomp PROPERTY IMPORTED_LOCATION "${SHADOW_PPCRECOMP_EXECUTABLE}")
    set(SHADOW_PPCRECOMP_DEPENDS "${SHADOW_PPCRECOMP_EXECUTABLE}")
elseif(SHADOW_PREBUILT_USED)
    # The release step's recompiler (built from this package's own tools/ppcrecomp by the same compiler).
    add_executable(ppcrecomp IMPORTED)
    set_property(TARGET ppcrecomp PROPERTY IMPORTED_LOCATION "${SHADOW_PREBUILT_USED}/ppcrecomp.exe")
    set(SHADOW_PPCRECOMP_DEPENDS "${SHADOW_PREBUILT_USED}/ppcrecomp.exe")
else()
    add_executable(ppcrecomp tools/ppcrecomp/main.cpp)
    target_include_directories(ppcrecomp PRIVATE runtime/include)
    target_compile_features(ppcrecomp PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(ppcrecomp PRIVATE /W4 /permissive-)
        set_property(TARGET ppcrecomp PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    else()
        target_compile_options(ppcrecomp PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    set(SHADOW_PPCRECOMP_DEPENDS ppcrecomp)
endif()

# ---- the recompiled game code ----------------------------------------------
if(SHADOW_RECOMP_DOL STREQUAL "")
    add_library(shadow_recomp STATIC runtime/src/gekko/recomp_table_empty.cpp)
else()
    set(SHADOW_RECOMP_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
    file(MAKE_DIRECTORY "${SHADOW_RECOMP_DIR}")

    set(SHADOW_RECOMP_OUTPUTS "${SHADOW_RECOMP_DIR}/recomp_table.cpp")
    math(EXPR SHADOW_RECOMP_LAST "${SHADOW_RECOMP_SHARDS} - 1")
    foreach(index RANGE ${SHADOW_RECOMP_LAST})
        string(LENGTH "${index}" _len)
        if(_len EQUAL 1)
            set(_name "recomp_00${index}.cpp")
        elseif(_len EQUAL 2)
            set(_name "recomp_0${index}.cpp")
        else()
            set(_name "recomp_${index}.cpp")
        endif()
        list(APPEND SHADOW_RECOMP_OUTPUTS "${SHADOW_RECOMP_DIR}/${_name}")
    endforeach()
    # The compilers take the shards in this order. Three shards of the PAL image hold one very large function each
    # (fn_803E1D58, fn_803EA420, fn_803E61F0) and take far longer than the others (measured alone, MSVC 19.52: 196 s,
    # 56 s, 56 s; a usual shard about 30 s): started last, they finish minutes after everything else and set the
    # build's length. They go first. The order changes nothing in what is compiled.
    if(SHADOW_RECOMP_SHARDS EQUAL 48)
        foreach(_heavy IN ITEMS recomp_016.cpp recomp_007.cpp recomp_025.cpp)
            list(REMOVE_ITEM SHADOW_RECOMP_OUTPUTS "${SHADOW_RECOMP_DIR}/${_heavy}")
            list(PREPEND SHADOW_RECOMP_OUTPUTS "${SHADOW_RECOMP_DIR}/${_heavy}")
        endforeach()
    endif()

    # The exception vector page the game's OS assembles at runtime is never
    # executed: the OS exception entry is native (runtime/src/gekko/
    # os_exception_entry.cpp; extract/exception-vectors.csv documents the
    # construction it checks). No guest code runs in real mode, so no vector
    # and no real-mode alias is translated
    # (docs/memory-architecture-20261006/README.md 2.2).
    set(SHADOW_RECOMP_ARGS
        --dol "${SHADOW_RECOMP_DOL}"
        --functions "${SHADOW_RECOMP_FUNCTIONS}"
        --out "${SHADOW_RECOMP_DIR}"
        --shards "${SHADOW_RECOMP_SHARDS}"
        --report "${SHADOW_RECOMP_DIR}/recomp_report.txt")
    # Translations nothing can reach are not emitted (ppcrecomp's pruning pass).
    # The DOL supplies the entry, the vector sections and every data word; this
    # file adds the guest code the native runtime names (HLE hooks, native
    # callers) and the address census's augmented roots. The native_liveness_roots
    # test refuses it when a runtime literal is missing.
    # The lifted tree compiles none of SHADOW_LIFTED_UNCOMPILED_SOURCES, so it is rooted by the file made without
    # their literals; the other trees (the R2 baseline, the tests tree) compile them and keep the full file.
    # native/win N6: a lifted tree that compiles no test (the game image) is entered by no test, so it is rooted by the
    # file made without the literals of runtime/tests/ as well (liveness_roots.py --without-tests). A test that names
    # the address of a retired owner then no longer puts the original body back into the game.
    if(SHADOW_RENDER_LIFTED AND NOT SHADOW_BUILD_TESTS)
        set(SHADOW_RECOMP_LIVENESS_ROOTS "${CMAKE_CURRENT_SOURCE_DIR}/extract/liveness-roots-game.txt")
    elseif(SHADOW_RENDER_LIFTED)
        set(SHADOW_RECOMP_LIVENESS_ROOTS "${CMAKE_CURRENT_SOURCE_DIR}/extract/liveness-roots-lifted.txt")
    else()
        set(SHADOW_RECOMP_LIVENESS_ROOTS "${CMAKE_CURRENT_SOURCE_DIR}/extract/liveness-roots.txt")
    endif()
    list(APPEND SHADOW_RECOMP_ARGS --prune-roots "${SHADOW_RECOMP_LIVENESS_ROOTS}")
    # Recognized switch tables: their words are reached from the rows that can
    # read them (the dispatcher and any row forming an address inside the
    # table) instead of seeding liveness as data words. The native_jump_tables
    # test refuses a file the recognizer would not produce.
    set(SHADOW_RECOMP_JUMP_TABLES "${CMAKE_CURRENT_SOURCE_DIR}/extract/jump-tables.txt")
    list(APPEND SHADOW_RECOMP_ARGS --jump-tables "${SHADOW_RECOMP_JUMP_TABLES}")
    # Guest functions a runtime handler always serves: the generator emits only
    # a thunk to the owner (Cpu::serve_native_owner); startup checks each one.
    set(SHADOW_RECOMP_NATIVE_OWNERS "${CMAKE_CURRENT_SOURCE_DIR}/extract/native-owners.txt")
    list(APPEND SHADOW_RECOMP_ARGS --native-owners "${SHADOW_RECOMP_NATIVE_OWNERS}")
    # Side-effect-free polling loops: their taken back edge waits for an event,
    # so the generator emits the game clock's wait there (Cpu::idle_wait). The
    # native_wait_points test refuses a file the classifier would not produce.
    set(SHADOW_RECOMP_WAIT_POINTS "${CMAKE_CURRENT_SOURCE_DIR}/extract/wait-points.txt")
    if(SHADOW_RENDER_LIFTED)
        # Back edges of functions only the lifted build owns (extract/native-owners-r2.txt) are left out of its list: the generator
        # refuses a site whose function it prunes. The reference build keeps the guest body, so it keeps the wait point; a spin
        # without one never advances the game clock (strict mode B hung in GcnSystem's request-3 wait, 2026-11-09).
        set(_wp_lifted_retired "${CMAKE_CURRENT_SOURCE_DIR}/extract/wait-points-retired-lifted.txt")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SHADOW_RECOMP_WAIT_POINTS}" "${_wp_lifted_retired}")
        file(STRINGS "${_wp_lifted_retired}" _wp_skip_lines REGEX "^0x[0-9A-Fa-f]+")
        set(_wp_skip "")
        foreach(_line IN LISTS _wp_skip_lines)
            string(REGEX MATCH "^0x[0-9A-Fa-f]+" _site "${_line}")
            string(TOUPPER "${_site}" _site)
            string(REPLACE "0X" "0x" _site "${_site}")
            list(APPEND _wp_skip "${_site}")
        endforeach()
        file(STRINGS "${SHADOW_RECOMP_WAIT_POINTS}" _wp_lines)
        set(_wp_text "")
        set(_wp_removed 0)
        foreach(_line IN LISTS _wp_lines)
            string(REGEX MATCH "^0x[0-9A-Fa-f]+" _site "${_line}")
            if(_site AND "${_site}" IN_LIST _wp_skip)
                math(EXPR _wp_removed "${_wp_removed} + 1")
            else()
                string(APPEND _wp_text "${_line}
")
            endif()
        endforeach()
        list(LENGTH _wp_skip _wp_skip_count)
        if(NOT _wp_removed EQUAL _wp_skip_count)
            message(FATAL_ERROR "extract/wait-points-retired-lifted.txt lists ${_wp_skip_count} sites, ${_wp_removed} are in extract/wait-points.txt")
        endif()
        set(SHADOW_RECOMP_WAIT_POINTS "${CMAKE_CURRENT_BINARY_DIR}/wait-points-lifted.txt")
        file(CONFIGURE OUTPUT "${SHADOW_RECOMP_WAIT_POINTS}" CONTENT "${_wp_text}" @ONLY NEWLINE_STYLE UNIX)
    endif()
    list(APPEND SHADOW_RECOMP_ARGS --wait-points "${SHADOW_RECOMP_WAIT_POINTS}")
    # Platform sites: the listed VI register accesses of non-VI code (OS fonts,
    # CARD, SI, OS reset; docs/native-time-20261005/STAGE2_VI.md D5). Each is
    # emitted as a native call (Cpu::platform_read16/platform_write16) instead of
    # the bus access; the generator refuses a site whose DOL word differs or that
    # it does not emit.
    set(SHADOW_RECOMP_PLATFORM_SITES "${CMAKE_CURRENT_SOURCE_DIR}/extract/platform-sites.txt")
    list(APPEND SHADOW_RECOMP_ARGS --platform-sites "${SHADOW_RECOMP_PLATFORM_SITES}")
    # GX write-gather pipe sites (docs/native-time-20261005/GX_DATA_PLANE.md):
    # every store of the image to the pipe, lowered by family - dead transport
    # emits nothing, the live families become Cpu::gx_* calls. The
    # native_gx_sites test refuses a list the inventory would not produce.
    set(SHADOW_RECOMP_GX_SITES "${CMAKE_CURRENT_SOURCE_DIR}/extract/gx-sites.txt")
    list(APPEND SHADOW_RECOMP_ARGS --gx-sites "${SHADOW_RECOMP_GX_SITES}")
    # Lowered call sites and retired roots (docs/lift-20261006/LIFT_PROGRAMME.md
    # 2.1): a translated call of a lifted subsystem becomes its generated
    # lift_site function; a retired data word no longer seeds liveness. The
    # generator refuses a stale line; lift_l1_evidence re-derives L1's.
    set(SHADOW_RECOMP_LIFT_SITES "${CMAKE_CURRENT_SOURCE_DIR}/extract/lift-sites.txt")
    list(APPEND SHADOW_RECOMP_ARGS --lift-sites "${SHADOW_RECOMP_LIFT_SITES}")
    list(APPEND SHADOW_RECOMP_LIFT_SITES "${CMAKE_CURRENT_SOURCE_DIR}/extract/lift-sites-math.txt")
    list(APPEND SHADOW_RECOMP_ARGS --lift-sites "${CMAKE_CURRENT_SOURCE_DIR}/extract/lift-sites-math.txt")
    list(APPEND SHADOW_RECOMP_LIFT_SITES "${CMAKE_CURRENT_SOURCE_DIR}/extract/lift-sites-native.txt")
    list(APPEND SHADOW_RECOMP_ARGS --lift-sites "${CMAKE_CURRENT_SOURCE_DIR}/extract/lift-sites-native.txt")
    set(SHADOW_RECOMP_NATIVE_OWNERS_LIFTED "")
    set(SHADOW_RECOMP_GX_FAIL_SITES "")
    set(SHADOW_EMPTY_VARIADIC_LEAVES "${CMAKE_CURRENT_SOURCE_DIR}/extract/empty-variadic-leaves.txt")
    list(APPEND SHADOW_RECOMP_ARGS --empty-variadic-leaves "${SHADOW_EMPTY_VARIADIC_LEAVES}")
    set(SHADOW_EMPTY_VARIADIC_SHIPPING_CHECK)
    if(SHADOW_SHIPPING)
        set(SHADOW_EMPTY_VARIADIC_SHIPPING_CHECK --shipping)
    endif()
    if(NOT SHADOW_RECOMP_VARIANT_OF STREQUAL "")
        list(APPEND SHADOW_EMPTY_VARIADIC_SHIPPING_CHECK --variant)
    endif()
    if(SHADOW_RENDER_LIFTED)
        # R2's lowered RenderWare driver sites (R2_DRIVER.md 4.3).
        list(APPEND SHADOW_RECOMP_LIFT_SITES "${CMAKE_CURRENT_SOURCE_DIR}/extract/lift-sites-r2.txt")
        list(APPEND SHADOW_RECOMP_ARGS --lift-sites "${CMAKE_CURRENT_SOURCE_DIR}/extract/lift-sites-r2.txt")
        # ... and the functions its scaffolding owns (docs/boundary-20261007: the lock-on mark's draw).
        set(SHADOW_RECOMP_NATIVE_OWNERS_LIFTED "${CMAKE_CURRENT_SOURCE_DIR}/extract/native-owners-r2.txt")
        list(APPEND SHADOW_RECOMP_ARGS --native-owners "${SHADOW_RECOMP_NATIVE_OWNERS_LIFTED}")
        # GXD (2026-10-20): the GX pipe sites dead under the lifted renderer stop the machine
        # naming the site instead of feeding NativeImmediate (GC_ENVIRONMENT row 7; HW's list).
        set(SHADOW_RECOMP_GX_FAIL_SITES "${CMAKE_CURRENT_SOURCE_DIR}/extract/gx-fail-sites-lifted.txt")
        list(APPEND SHADOW_RECOMP_ARGS --gx-fail-sites "${SHADOW_RECOMP_GX_FAIL_SITES}")
        # RC-gen-2: the shipping image has no original body for a native owner, so no surviving row may translate an
        # owner's instructions (an overlapping or widened row would run the owner's original words natively). The
        # generator refuses such a tree here; the reference tree, which keeps the bodies, gets the same list as notes.
        list(APPEND SHADOW_RECOMP_ARGS --strict-owner-extents)
    else()
        # The native MSL sites in code only the unlifted renderer keeps (the projection fn_8000F654).
        list(APPEND SHADOW_RECOMP_LIFT_SITES "${CMAKE_CURRENT_SOURCE_DIR}/extract/lift-sites-math-unlifted.txt")
        list(APPEND SHADOW_RECOMP_ARGS --lift-sites "${CMAKE_CURRENT_SOURCE_DIR}/extract/lift-sites-math-unlifted.txt")
    endif()
    if(NOT SHADOW_RECOMP_ONLY STREQUAL "")
        list(APPEND SHADOW_RECOMP_ARGS --only "${SHADOW_RECOMP_ONLY}")
    endif()
    # R13 (2026-10-20): the profile-chosen hot set. Only the hot bodies of the
    # listed rows call the force-inlined accessors (Cpu::mem_*_hot); the rest of
    # the image is unchanged. The generator refuses a list whose header or row
    # starts do not match. Empty (-DSHADOW_RECOMP_HOT_SET=) is the A/B control:
    # the generated sources are then byte-identical to a build without R13.
    # FM step 2: every accessor is inline (the flat map), so R13's hot-set retargeting has nothing left to do
    # and Cpu's *_hot accessors are gone; the list stays in extract/ for RP (register promotion).
    set(SHADOW_RECOMP_HOT_SET "" CACHE STRING
        "ppcrecomp --hot-set list (empty: no hot-set inlining)")
    if(NOT SHADOW_RECOMP_HOT_SET STREQUAL "")
        list(APPEND SHADOW_RECOMP_ARGS --hot-set "${SHADOW_RECOMP_HOT_SET}")
    endif()
    # cs2/cpu: leaf inlining. A direct bl to a small straight-line leaf (at most N guest instructions, no calls, no
    # loops, no runtime-named address) gets the leaf's hot text spliced in place of the call (ppcrecomp
    # --inline-leaves N; the generator's comment above build_leaf_inline has the rules). Empty = off: the generated
    # sources are then byte-identical to a build without the option.
    set(SHADOW_RECOMP_INLINE_LEAVES "24" CACHE STRING
        "ppcrecomp --inline-leaves N (largest leaf, in guest instructions, spliced into its direct callers; empty: off)")
    if(NOT SHADOW_RECOMP_INLINE_LEAVES STREQUAL "")
        list(APPEND SHADOW_RECOMP_ARGS --inline-leaves "${SHADOW_RECOMP_INLINE_LEAVES}")
    endif()
    # cpu/opt2: the hot-body value pass (ppcrecomp --hot-value MASK; the generator's comment above HotValuePass has
    # the rules). Bits: 1 lfs -> stfs word forwarding, 2 dead CR field writes, 4 dead FPR half writes, 8 / 16 / 32
    # force-inlined update_cr0 / fp_compare / psq_load + psq_store in the hot bodies. Empty = off: the generated
    # sources are then byte-identical to a build without the option.
    set(SHADOW_RECOMP_HOT_VALUE "63" CACHE STRING
        "ppcrecomp --hot-value MASK (hot-body single forwarding, dead CR / FPR writes, inlined helpers; empty: off)")
    if(NOT SHADOW_RECOMP_HOT_VALUE STREQUAL "")
        list(APPEND SHADOW_RECOMP_ARGS --hot-value "${SHADOW_RECOMP_HOT_VALUE}")
    endif()
    # cpu/opt2: hot-body block layout (ppcrecomp --hot-layout): rare checks [[unlikely]], direct calls enter the
    # callee's hot body (entry 0 = the hook filter, then the body). OFF = the A/B control: the generated sources are
    # then byte-identical to a build without the option.
    # native/win: translation coverage (ppcrecomp --coverage): every emitted body records that it ran, and the image
    # writes the set at exit (GEKKO_COVERAGE=<file>, accumulating). A measurement build in its own tree; never for
    # shipped, gate or timing exes. OFF = the generated sources are byte-identical to a build without the option.
    option(SHADOW_RECOMP_COVERAGE "ppcrecomp --coverage (which translated rows ran; measurement build only)" OFF)
    if(SHADOW_RECOMP_COVERAGE)
        list(APPEND SHADOW_RECOMP_ARGS --coverage)
    endif()
    option(SHADOW_RECOMP_HOT_LAYOUT "ppcrecomp --hot-layout (usual path of calls and guards falls through)" ON)
    if(SHADOW_RECOMP_HOT_LAYOUT)
        list(APPEND SHADOW_RECOMP_ARGS --hot-layout)
    endif()
    # cpu/opt4: which rare checks take the structural spelling (`if (usual) {} else { ...; return; }`, the shape MSVC
    # 19.52 places out of line; it ignores [[unlikely]]). Bits: 1 the call's exit check, 2 the entry block and the
    # hook filter, 4 the guards that leave. Empty = the generator's default (all). A/B and bisection only.
    set(SHADOW_RECOMP_HOT_LAYOUT_PARTS "" CACHE STRING
        "ppcrecomp --hot-layout-parts MASK (1 call checks, 2 entry + hook filter, 4 guards; empty: all)")
    if(SHADOW_RECOMP_HOT_LAYOUT AND NOT SHADOW_RECOMP_HOT_LAYOUT_PARTS STREQUAL "")
        list(APPEND SHADOW_RECOMP_ARGS --hot-layout-parts "${SHADOW_RECOMP_HOT_LAYOUT_PARTS}")
    endif()
    # R16 (2026-10-20): paired-single sites specialised for one GQR value behind an
    # exact runtime guard (ppcrecomp --psq-sites; ppc::psq_load_k / psq_store_k).
    # Empty (-DSHADOW_RECOMP_PSQ_SITES=) is the A/B control.
    set(SHADOW_RECOMP_PSQ_SITES "${CMAKE_CURRENT_SOURCE_DIR}/extract/psq-sites.txt" CACHE STRING
        "ppcrecomp --psq-sites list (empty: no paired-single specialisation)")
    if(NOT SHADOW_RECOMP_PSQ_SITES STREQUAL "")
        list(APPEND SHADOW_RECOMP_ARGS --psq-sites "${SHADOW_RECOMP_PSQ_SITES}")
    endif()

    # RC-gen-1: the generator prunes with the roots file AS GIVEN, and that file is derived from the runtime's sources
    # (every guest address they name). A runtime edit that adds a guest entry without regenerating the file used to
    # build cleanly and prune the newly entered row; only the native_liveness_roots ctest noticed, after the fact. The
    # check the test runs is now a build step: it reruns when any runtime source or the roots file changes (seconds),
    # fails the build on a stale file, and does not rerun the generator by itself.
    file(GLOB_RECURSE SHADOW_LIVENESS_ROOT_SOURCES CONFIGURE_DEPENDS
         "${CMAKE_CURRENT_SOURCE_DIR}/runtime/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/runtime/*.h"
         "${CMAKE_CURRENT_SOURCE_DIR}/runtime/*.hpp" "${CMAKE_CURRENT_SOURCE_DIR}/runtime/*.inl"
         "${CMAKE_CURRENT_SOURCE_DIR}/runtime/*.cc")
    set(SHADOW_LIVENESS_ROOTS_CENSUS "${CMAKE_CURRENT_SOURCE_DIR}/test_data/address-census-20261005-v1/census-v2.json")
    set(SHADOW_LIVENESS_ROOTS_CHECK_ARGS
        --runtime "${CMAKE_CURRENT_SOURCE_DIR}/runtime" --census "${SHADOW_LIVENESS_ROOTS_CENSUS}")
    set(SHADOW_LIVENESS_ROOTS_CHECK_DEPENDS "${SHADOW_RECOMP_LIVENESS_ROOTS}")
    if(SHADOW_RENDER_LIFTED)
        foreach(_source ${SHADOW_LIFTED_UNCOMPILED_SOURCES})
            list(APPEND SHADOW_LIVENESS_ROOTS_CHECK_ARGS --without "${_source}")
        endforeach()
        list(APPEND SHADOW_LIVENESS_ROOTS_CHECK_ARGS --base "${CMAKE_CURRENT_SOURCE_DIR}/extract/liveness-roots.txt")
        list(APPEND SHADOW_LIVENESS_ROOTS_CHECK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/extract/liveness-roots.txt")
        if(NOT SHADOW_BUILD_TESTS)
            list(APPEND SHADOW_LIVENESS_ROOTS_CHECK_ARGS --without-tests)  # extract/liveness-roots-game.txt
        endif()
    endif()
    set(SHADOW_LIVENESS_ROOTS_STAMP "${SHADOW_RECOMP_DIR}/liveness_roots.stamp")
    add_custom_command(
        OUTPUT "${SHADOW_LIVENESS_ROOTS_STAMP}"
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tools/ppcrecomp/liveness_roots.py"
                ${SHADOW_LIVENESS_ROOTS_CHECK_ARGS} --check "${SHADOW_RECOMP_LIVENESS_ROOTS}"
        COMMAND ${CMAKE_COMMAND} -E touch "${SHADOW_LIVENESS_ROOTS_STAMP}"
        DEPENDS ${SHADOW_LIVENESS_ROOT_SOURCES} ${SHADOW_LIVENESS_ROOTS_CHECK_DEPENDS} "${SHADOW_LIVENESS_ROOTS_CENSUS}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/ppcrecomp/liveness_roots.py"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Checking the liveness roots against the runtime sources (stale file: regenerate with liveness_roots.py --out)"
        VERBATIM)
    add_custom_target(shadow_liveness_roots_check DEPENDS "${SHADOW_LIVENESS_ROOTS_STAMP}")

    # The stamp acknowledges a successful generator run even when every
    # source is byte-identical and deliberately keeps its old timestamp.
    set(SHADOW_RECOMP_STAMP "${SHADOW_RECOMP_DIR}/recomp.stamp")
    add_custom_command(
        OUTPUT "${SHADOW_RECOMP_STAMP}"
        BYPRODUCTS ${SHADOW_RECOMP_OUTPUTS} "${SHADOW_RECOMP_DIR}/recomp_decls.h"
                   "${SHADOW_RECOMP_DIR}/recomp_report.txt" "${SHADOW_RECOMP_DIR}/recomp_layout.csv"
        COMMAND ppcrecomp ${SHADOW_RECOMP_ARGS}
        COMMAND ${CMAKE_COMMAND} -E touch "${SHADOW_RECOMP_STAMP}"
        DEPENDS ${SHADOW_PPCRECOMP_DEPENDS} "${SHADOW_RECOMP_DOL}" "${SHADOW_RECOMP_FUNCTIONS}"
                "${CMAKE_CURRENT_SOURCE_DIR}/runtime/include/gekko/ppc_instruction_traits.h"
                "${SHADOW_RECOMP_LIVENESS_ROOTS}"
                "${SHADOW_RECOMP_JUMP_TABLES}"
                ${SHADOW_RECOMP_NATIVE_OWNERS}
                ${SHADOW_RECOMP_NATIVE_OWNERS_LIFTED}
                ${SHADOW_RECOMP_GX_FAIL_SITES}
                "${SHADOW_RECOMP_WAIT_POINTS}"
                "${SHADOW_RECOMP_PLATFORM_SITES}"
                "${SHADOW_RECOMP_GX_SITES}"
                ${SHADOW_RECOMP_LIFT_SITES}
                "${SHADOW_EMPTY_VARIADIC_LEAVES}"
                $<$<NOT:$<STREQUAL:${SHADOW_RECOMP_ONLY},>>:${SHADOW_RECOMP_ONLY}>
                $<$<NOT:$<STREQUAL:${SHADOW_RECOMP_HOT_SET},>>:${SHADOW_RECOMP_HOT_SET}>
                $<$<NOT:$<STREQUAL:${SHADOW_RECOMP_PSQ_SITES},>>:${SHADOW_RECOMP_PSQ_SITES}>
        COMMENT "Recompiling PowerPC functions to native x86-64"
        VERBATIM)

    set(SHADOW_RECOMP_ALL_OUTPUTS ${SHADOW_RECOMP_OUTPUTS}
        "${SHADOW_RECOMP_DIR}/recomp_decls.h"
        "${SHADOW_RECOMP_DIR}/recomp_report.txt"
        "${SHADOW_RECOMP_DIR}/recomp_layout.csv")
    set(SHADOW_RECOMP_SCRIPT_ARGS "")
    foreach(argument IN LISTS SHADOW_RECOMP_ARGS)
        string(APPEND SHADOW_RECOMP_SCRIPT_ARGS " [====[${argument}]====]")
    endforeach()
    configure_file(tools/ppcrecomp/ensure_outputs.cmake.in
                   "${SHADOW_RECOMP_DIR}/ensure_outputs.cmake.in" @ONLY)
    file(GENERATE OUTPUT "${SHADOW_RECOMP_DIR}/ensure_outputs_$<CONFIG>.cmake"
         INPUT "${SHADOW_RECOMP_DIR}/ensure_outputs.cmake.in")
    add_custom_target(shadow_recomp_generate
        COMMAND ${CMAKE_COMMAND} -P "${SHADOW_RECOMP_DIR}/ensure_outputs_$<CONFIG>.cmake"
        DEPENDS "${SHADOW_RECOMP_STAMP}"
        VERBATIM)
    add_library(shadow_recomp STATIC ${SHADOW_RECOMP_OUTPUTS})
    if(SHADOW_LTO AND NOT SHADOW_LTO_SHARDS)
        set_property(TARGET shadow_recomp PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE OFF)
    endif()
    # The profiled translated functions in shards of their own under /GL (2026-11-18): tools/perf/hot_shards.py writes a
    # recomp_layout.csv that gathers them in the first N shards (the generator carries the layout it finds), and those
    # N shards become /GL objects so the profile-guided link sees them; the other shards stay ordinary objects. Which
    # shard a function is in changes no generated statement. 0 = off.
    set(SHADOW_LTO_HOT_SHARDS "0" CACHE STRING "With SHADOW_LTO and SHADOW_LTO_SHARDS=OFF: the first N generated shards are /GL objects")
    if(MSVC AND SHADOW_LTO AND NOT SHADOW_LTO_SHARDS AND SHADOW_LTO_HOT_SHARDS GREATER 0)
        math(EXPR _hot_last "${SHADOW_LTO_HOT_SHARDS} - 1")
        foreach(_hot RANGE 0 ${_hot_last})
            if(_hot LESS 10)
                set(_hot_name "recomp_00${_hot}.cpp")
            else()
                set(_hot_name "recomp_0${_hot}.cpp")
            endif()
            set_property(SOURCE "${SHADOW_RECOMP_DIR}/${_hot_name}" APPEND PROPERTY COMPILE_OPTIONS "$<$<CONFIG:Release>:/GL>")
        endforeach()
        message(STATUS "Generated shards 0..${_hot_last} are /GL objects (hot translated functions under LTCG / PGO)")
    endif()
    add_dependencies(shadow_recomp shadow_recomp_generate)
    add_dependencies(shadow_recomp_generate shadow_liveness_roots_check)
    target_include_directories(shadow_recomp PRIVATE "${SHADOW_RECOMP_DIR}")
    if(MSVC)
        # Generated code: a terminating branch makes the tail of a function
        # unreachable by construction, which is not a defect worth a warning.
        # C4702: a terminating branch makes the tail unreachable by construction.
        # C4102: each function's own start label is emitted for the dispatch
        #        switch but reached by fallthrough, so it is never referenced.
        # C4065: a function with a single reachable instruction has a dispatch
        #        switch with only a default.
        # /MP with no count spawns one cl per logical CPU. The recomp shards are
        # very large generated translation units and each compiler instance can
        # peak in the multi-GB range, so on a high-core-count host the bare form
        # can exhaust commit and take the whole build down with no diagnostic.
        # Cap it, and let a machine with headroom raise it explicitly.
        set(SHADOW_RECOMP_COMPILE_JOBS "8" CACHE STRING
            "Parallel cl.exe instances used for the generated recomp shards (MSVC /MP)")
        # MSVC otherwise silently suppresses optimization for large generated
        # functions even at /O2 (C4883). Keep the arithmetic unchanged and
        # let the optimizer finish; make any future suppression a build error.
        # Measured evidence and the Microsoft reference: docs/OPTIMIZATION_RESULTS.md.
        if(SHADOW_RECOMP_COVERAGE)
            target_compile_definitions(shadow_recomp PRIVATE SHADOW_RECOMP_COVERAGE=1)
        endif()
        option(SHADOW_RECOMP_RESTRICT "Generated hot bodies take the Cpu as __restrict (registers stay in host registers across guest memory accesses)" OFF)
        if(SHADOW_RECOMP_RESTRICT)
            target_compile_definitions(shadow_recomp PRIVATE SHADOW_RECOMP_RESTRICT=1)
        endif()
        # Extra compiler options for the generated shards only (measurement builds; empty = none).
        set(SHADOW_RECOMP_EXTRA_FLAGS "" CACHE STRING "Extra compile options for the generated recomp shards only")
        if(NOT SHADOW_RECOMP_EXTRA_FLAGS STREQUAL "")
            separate_arguments(_shadow_recomp_extra NATIVE_COMMAND "${SHADOW_RECOMP_EXTRA_FLAGS}")
            target_compile_options(shadow_recomp PRIVATE ${_shadow_recomp_extra})
        endif()
        target_compile_options(shadow_recomp PRIVATE /W4 /permissive- /wd4702 /wd4102 /wd4065
                                                     /d2OptimizeHugeFunctions /we4883
                                                     /bigobj /MP${SHADOW_RECOMP_COMPILE_JOBS}
                                                     ${SHADOW_ARCH_FLAGS})
        # The runtime's 143 files are the other long stage, and nothing else can start until they finish. (/MP runs
        # several compilers at once; it changes no compiled byte.)
        target_compile_options(shadow_runtime_common PRIVATE /MP${SHADOW_RECOMP_COMPILE_JOBS})
        # Every shard is compiled with the same flags. (A 2026-09-20 workaround
        # compiled recomp_034.cpp at /O1 for an ICE seen on MSVC 14.51.36231;
        # keyed on a shard index, it slowed whatever functions the layout put
        # there - and the layout differs between build trees. Rechecked
        # 2026-10-19 on cl 19.50.35730: both layouts' shard 34 compile at /O2.
        # An ICE fails the build loudly; it must never be worked around by a
        # shard index.)
    endif()
endif()

target_include_directories(shadow_recomp PUBLIC runtime/include)
target_compile_features(shadow_recomp PUBLIC cxx_std_20)
target_link_libraries(shadow_recomp PUBLIC shadow_host_isa shadow_lift)
if(SHADOW_RENDER_LIFTED)
    # The generated code calls the scaffolding at R2's lowered sites (gekko::scaffold).
    target_link_libraries(shadow_recomp PUBLIC shadow_render_lifted)
endif()
if(MSVC)
    set_property(TARGET shadow_recomp PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(SHADOW_ENABLE_PLUME)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/plume/CMakeLists.txt")
        message(FATAL_ERROR
            "SHADOW_ENABLE_PLUME is ON but thirdparty/plume is empty. "
            "Run clone-thirdparty.bat; see thirdparty/PINS.md for the pinned revision.")
    endif()
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/plume/contrib/D3D12MemoryAllocator/src/D3D12MemAlloc.cpp")
        message(FATAL_ERROR
            "thirdparty/plume is present but its submodules are not checked out. "
            "Run: git -C thirdparty/plume submodule update --init --recursive --depth 1")
    endif()

    add_subdirectory(thirdparty/plume EXCLUDE_FROM_ALL)

    # The allocator is compiled once, by shadow_d3d12ma (the native renderer's target), from plume's own pinned
    # copy: two copies in one image would collide at link time.
    get_target_property(SHADOW_PLUME_SOURCES plume SOURCES)
    list(FILTER SHADOW_PLUME_SOURCES EXCLUDE REGEX "D3D12MemAlloc\\.cpp$")
    set_target_properties(plume PROPERTIES SOURCES "${SHADOW_PLUME_SOURCES}")
    target_link_libraries(plume PUBLIC shadow_d3d12ma)

    # plume does not pick a CRT or link the D3D12 libraries itself.
    set_property(TARGET plume PROPERTY MSVC_RUNTIME_LIBRARY
                 "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    if(MSVC)
        # Third-party code: its warnings are not this project's to fix, and
        # /W4 /permissive- is imposed on our own targets only.
        target_compile_options(plume PRIVATE /W0)
    endif()

    # PRIVATE, deliberately. shadow_runtime is a static library, so CMake still
    # propagates these to the final link as $<LINK_ONLY:...>, but plume's include
    # directories and this definition stay off the compile line of shadow_recomp -
    # whose 64 generated shards would otherwise all rebuild whenever the option
    # is toggled. Only plume_present.cpp reads SHADOW_ENABLE_PLUME.
    # G5: the lifted build presents through its own swap chain (the presenter is never asked for): plume_present.cpp
    # compiles to its no-op branch there, and the legacy shaders, the presenter's baked shaders and plume stay out.
    if(NOT SHADOW_RENDER_LIFTED)
    target_compile_definitions(shadow_runtime_common PRIVATE SHADOW_ENABLE_PLUME)

    # ---- native renderer shaders (DXC, SM 6.0) -----------------------------
    # docs/native-runtime-2026-09-16/ARCHITECTURE.md. Compiled at build time and
    # embedded like the legacy bake; a compile error fails the build.
    find_program(SHADOW_DXC dxc
        HINTS "$ENV{WindowsSdkVerBinPath}/x64"
              "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64"
        REQUIRED)
    set(SHADOW_NATIVE_SHADER_DIR "${CMAKE_BINARY_DIR}/native_shaders")
    set(SHADOW_NATIVE_SHADER_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/runtime/src/gekko/native_shaders.hlsl")
    set(SHADOW_NATIVE_SHADER_HEADERS)
    foreach(entry IN ITEMS "VSMain|vs_6_0|native_shader_vs.h"
                           "PSMain|ps_6_0|native_shader_ps.h"
                           "PSMainAlphaTest|ps_6_0|native_shader_ps_alpha.h"
                           "PSMainFootprint|ps_6_0|native_shader_ps_footprint.h"
                           "PSMainAlphaTestFootprint|ps_6_0|native_shader_ps_alpha_footprint.h")
        string(REPLACE "|" ";" parts "${entry}")
        list(GET parts 0 entry_point)
        list(GET parts 1 profile)
        list(GET parts 2 header)
        add_custom_command(
            OUTPUT "${SHADOW_NATIVE_SHADER_DIR}/${header}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${SHADOW_NATIVE_SHADER_DIR}"
            COMMAND "${SHADOW_DXC}" -nologo -T ${profile} -E ${entry_point} -O3 -Qstrip_reflect
                    -Vn kNative${entry_point} -Fh "${SHADOW_NATIVE_SHADER_DIR}/${header}"
                    "${SHADOW_NATIVE_SHADER_SOURCE}"
            DEPENDS "${SHADOW_NATIVE_SHADER_SOURCE}"
            COMMENT "Compiling native renderer shader ${entry_point} (DXC ${profile})"
            VERBATIM)
        list(APPEND SHADOW_NATIVE_SHADER_HEADERS "${SHADOW_NATIVE_SHADER_DIR}/${header}")
    endforeach()
    # ---- the new native bloom system's shaders -----------------------------
    # A separate source file with its own root signature: the bloom passes share
    # nothing with the material pipeline, and keeping them apart is what stops a
    # change here from perturbing PSMain (the FXC `precise` hazard recorded in
    # docs/). Specification: docs/effects-2026-09-21/BLOOM_SPEC.md.
    set(SHADOW_BLOOM_SHADER_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/runtime/src/gekko/bloom_shaders.hlsl")
    foreach(entry IN ITEMS "VSBloom|vs_6_0|bloom_shader_vs.h"
                           "PSBloomCapture|ps_6_0|bloom_shader_ps_capture.h"
                           "PSBloomBlur|ps_6_0|bloom_shader_ps_blur.h"
                           "PSBloomMerge|ps_6_0|bloom_shader_ps_merge.h"
                           "PSBloomComposite|ps_6_0|bloom_shader_ps_composite.h")
        string(REPLACE "|" ";" parts "${entry}")
        list(GET parts 0 entry_point)
        list(GET parts 1 profile)
        list(GET parts 2 header)
        add_custom_command(
            OUTPUT "${SHADOW_NATIVE_SHADER_DIR}/${header}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${SHADOW_NATIVE_SHADER_DIR}"
            COMMAND "${SHADOW_DXC}" -nologo -T ${profile} -E ${entry_point} -O3 -Qstrip_reflect
                    -Vn kBloom${entry_point} -Fh "${SHADOW_NATIVE_SHADER_DIR}/${header}"
                    "${SHADOW_BLOOM_SHADER_SOURCE}"
            DEPENDS "${SHADOW_BLOOM_SHADER_SOURCE}"
            COMMENT "Compiling bloom shader ${entry_point} (DXC ${profile})"
            VERBATIM)
        list(APPEND SHADOW_NATIVE_SHADER_HEADERS "${SHADOW_NATIVE_SHADER_DIR}/${header}")
    endforeach()

    add_custom_target(shadow_native_shaders DEPENDS ${SHADOW_NATIVE_SHADER_HEADERS})
    add_dependencies(shadow_runtime_common shadow_native_shaders)
    target_include_directories(shadow_runtime_common PRIVATE "${SHADOW_NATIVE_SHADER_DIR}")
    # d3dcompiler is deliberately absent: every shader program is compiled at
    # build time by tools/shaderbake and embedded, so the executable contains
    # no shader compiler and cannot recompile anything at runtime.
    target_link_libraries(shadow_runtime_common PRIVATE plume)
    foreach(cpu_runtime IN LISTS SHADOW_CPU_TARGETS)
        target_link_libraries(${cpu_runtime} PRIVATE plume d3d12 dxgi dxguid)
    endforeach()

    # ---- build-time shader baking -----------------------------------------
    # shaderbake compiles the presenter blit programs to DXBC during the build
    # and generates their embedded table. A shader compile error fails the
    # build; there is no runtime fallback.
    add_executable(shaderbake tools/shaderbake/main.cpp)
    target_include_directories(shaderbake PRIVATE runtime/src/gekko runtime/include)
    target_compile_features(shaderbake PRIVATE cxx_std_20)
    target_link_libraries(shaderbake PRIVATE d3dcompiler)
    target_compile_definitions(shaderbake PRIVATE _CRT_SECURE_NO_WARNINGS)
    if(MSVC)
        target_compile_options(shaderbake PRIVATE /W4 /permissive-)
        set_property(TARGET shaderbake PROPERTY MSVC_RUNTIME_LIBRARY
                     "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()

    set(SHADOW_PRESENT_SHADER_CACHE "${CMAKE_BINARY_DIR}/generated/present_shader_cache.cpp")
    add_custom_command(
        OUTPUT "${SHADOW_PRESENT_SHADER_CACHE}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/generated"
        COMMAND shaderbake present "${SHADOW_PRESENT_SHADER_CACHE}"
        DEPENDS shaderbake
                "${CMAKE_CURRENT_SOURCE_DIR}/runtime/src/gekko/present_shader.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/runtime/include/gekko/present_shader_cache.h"
        COMMENT "Baking presenter shaders to embedded DXBC"
        VERBATIM)
    target_sources(shadow_runtime_common PRIVATE "${SHADOW_PRESENT_SHADER_CACHE}")
    endif()  # G5: NOT SHADOW_RENDER_LIFTED
endif()


find_package(Python3 COMPONENTS Interpreter QUIET)
if(MSVC AND Python3_Interpreter_FOUND)
    get_filename_component(SHADOW_MSVC_TOOLS "${CMAKE_LINKER}" DIRECTORY)
    find_program(SHADOW_DUMPBIN NAMES dumpbin.exe dumpbin HINTS "${SHADOW_MSVC_TOOLS}" REQUIRED)
endif()

set(SHADOW_PROGRAM_TARGETS)
if(NOT SHADOW_RECOMP_DOL STREQUAL "")
    if(ANDROID)
        # Android: the game is the shared library an activity loads (lib/<abi>/libShadowTheHedgehog.so in the APK;
        # docs/ANDROID_BUILD.md - its entry point is part of the Android platform layer, not written yet).
        add_library(ShadowTheHedgehog SHARED runtime/src/shadow_main.cpp)
    else()
        add_executable(ShadowTheHedgehog runtime/src/shadow_main.cpp)
    endif()
    target_link_libraries(ShadowTheHedgehog PRIVATE shadow_recomp shadow_runtime)
    # cpu/opt (2026-11-15): profile-guided optimization on top of SHADOW_LTO. GEN links an instrumented image that
    # writes <pgd>!N.pgc counts; USE links the optimized image from the merged <pgd> (tools/perf/pgo_train.sh). Only
    # the link changes - the /GL objects are the same for both - and code layout, inlining and branch order are the
    # only things the profile decides: /fp:precise semantics are unchanged, so guest results stay bit-identical.
    set(SHADOW_PGO "OFF" CACHE STRING "Profile-guided optimization of the game image: OFF, GEN or USE (needs SHADOW_LTO)")
    set_property(CACHE SHADOW_PGO PROPERTY STRINGS OFF GEN USE)
    set(SHADOW_PGO_PGD "${CMAKE_CURRENT_BINARY_DIR}/pgo/ShadowTheHedgehog.pgd" CACHE FILEPATH "PGO database path")
    if(MSVC AND NOT SHADOW_PGO STREQUAL "OFF")
        if(NOT SHADOW_LTO)
            message(FATAL_ERROR "SHADOW_PGO=${SHADOW_PGO} needs SHADOW_LTO=ON")
        endif()
        if(SHADOW_PGO STREQUAL "GEN")
            target_link_options(ShadowTheHedgehog PRIVATE "$<$<CONFIG:Release>:/GENPROFILE:PGD=${SHADOW_PGO_PGD}>")
        elseif(SHADOW_PGO STREQUAL "USE")
            if(NOT EXISTS "${SHADOW_PGO_PGD}")
                message(FATAL_ERROR "SHADOW_PGO=USE: no profile database at ${SHADOW_PGO_PGD}")
            endif()
            target_link_options(ShadowTheHedgehog PRIVATE "$<$<CONFIG:Release>:/USEPROFILE:PGD=${SHADOW_PGO_PGD}>")
        else()
            message(FATAL_ERROR "SHADOW_PGO must be OFF, GEN or USE")
        endif()
    endif()
    if(SHADOW_RAM_PROBE)
        # cs2/ram measurement build: per-call-stack live bytes of the executable's operator new (runtime/src/ram_probe.cpp)
        target_sources(ShadowTheHedgehog PRIVATE runtime/src/ram_probe.cpp)
        target_compile_definitions(ShadowTheHedgehog PRIVATE SHADOW_RAM_PROBE=1)
    endif()
    if(SHADOW_RENDER_LIFTED)
        target_compile_definitions(ShadowTheHedgehog PRIVATE SHADOW_RENDER_LIFTED=1)
        target_link_libraries(ShadowTheHedgehog PRIVATE shadow_render_lifted)
    endif()
    list(APPEND SHADOW_PROGRAM_TARGETS ShadowTheHedgehog)
    if(MSVC)
        if(NOT Python3_Interpreter_FOUND)
            message(FATAL_ERROR "The native executable requires Python3 for its interpreter-absence build gate")
        endif()
        add_custom_command(TARGET ShadowTheHedgehog POST_BUILD
            COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tools/ppcrecomp/check_native_boundary.py"
                    --dumpbin "${SHADOW_DUMPBIN}"
                    --native-library "$<TARGET_FILE:shadow_runtime>"
                    --executable "$<TARGET_FILE:ShadowTheHedgehog>"
                    --map "$<TARGET_FILE_DIR:ShadowTheHedgehog>/ShadowTheHedgehog.map"
            COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tools/lift/verify_native_math.py"
                    --dol "${SHADOW_RECOMP_DOL}" --generated "${SHADOW_RECOMP_DIR}"
                    --map "$<TARGET_FILE_DIR:ShadowTheHedgehog>/ShadowTheHedgehog.map"
            COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tools/ppcrecomp/make_empty_variadic_fixture.py"
                    --dol "${SHADOW_RECOMP_DOL}" --functions "${SHADOW_RECOMP_FUNCTIONS}"
                    --leaves "${SHADOW_EMPTY_VARIADIC_LEAVES}" --jump-tables "${SHADOW_RECOMP_JUMP_TABLES}"
                    --generated "${SHADOW_RECOMP_DIR}"
                    --map "$<TARGET_FILE_DIR:ShadowTheHedgehog>/ShadowTheHedgehog.map"
                    --out "${CMAKE_CURRENT_BINARY_DIR}/validation/empty-variadic"
                    ${SHADOW_EMPTY_VARIADIC_SHIPPING_CHECK}
            VERBATIM)
        set_property(TARGET ShadowTheHedgehog APPEND PROPERTY LINK_DEPENDS
                     "${CMAKE_CURRENT_SOURCE_DIR}/tools/ppcrecomp/check_native_boundary.py"
                     "${CMAKE_CURRENT_SOURCE_DIR}/tools/lift/verify_native_math.py"
                     "${CMAKE_CURRENT_SOURCE_DIR}/tools/ppcrecomp/make_empty_variadic_fixture.py"
                     "${CMAKE_CURRENT_SOURCE_DIR}/tools/lift/type_call_liveness.py")
    endif()
else()
    message(STATUS "SHADOW_RECOMP_DOL is empty: no native game executable will be produced")
endif()
if(SHADOW_BUILD_TESTS OR SHADOW_BUILD_REFERENCE)
    add_executable(ShadowTheHedgehogReference EXCLUDE_FROM_ALL runtime/src/shadow_main.cpp)
    # Launcher-only selection of oracle paths. Shared Cpu/runtime definitions
    # remain identical; this must not propagate into the common object library.
    target_compile_definitions(ShadowTheHedgehogReference PRIVATE SHADOW_REFERENCE_RUNNER=1)
    target_link_libraries(ShadowTheHedgehogReference PRIVATE shadow_recomp shadow_runtime_reference)
    if(SHADOW_RENDER_LIFTED)
        target_compile_definitions(ShadowTheHedgehogReference PRIVATE SHADOW_RENDER_LIFTED=1)
        target_link_libraries(ShadowTheHedgehogReference PRIVATE shadow_render_lifted)
    endif()
    list(APPEND SHADOW_PROGRAM_TARGETS ShadowTheHedgehogReference)
endif()
foreach(program_target IN LISTS SHADOW_PROGRAM_TARGETS)
    set_target_properties(${program_target} PROPERTIES
        OUTPUT_NAME "${program_target}"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endforeach()

# The playable executable resolves settings and persistence beside itself.
# Keep builds separate; deployment into ../game is an explicit operation.
set(SHADOW_DEPLOY_DIR "" CACHE PATH
    "Optional game directory for explicitly requested deployment; empty keeps the executable in the build tree")
if(TARGET ShadowTheHedgehog)
    if(SHADOW_RENDER_LIFTED)
        # The lifted renderer is a development build until R2f: it never replaces the deployed game. Test copies go
        # beside the data under their own name, by hand.
        message(STATUS "SHADOW_RENDER_LIFTED: no deployment; the executable stays in the build tree")
    elseif(SHADOW_DEPLOY_DIR AND IS_DIRECTORY "${SHADOW_DEPLOY_DIR}")
        add_custom_command(TARGET ShadowTheHedgehog POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:ShadowTheHedgehog>"
                    "${SHADOW_DEPLOY_DIR}/ShadowTheHedgehog.exe"
            COMMENT "Deploying ShadowTheHedgehog.exe to ${SHADOW_DEPLOY_DIR}"
            VERBATIM)
    else()
        message(STATUS "No deployment requested; the executable stays in the build tree")
    endif()
endif()

# The shipping Windows executable owns an exact copy of the original system
# area used to build its recompiled code. At runtime only the adjacent files/
# asset tree is required. Interpreter-only development builds retain the
# external sys/ path.
if(WIN32 AND NOT SHADOW_RECOMP_DOL STREQUAL "")
    # The title block and the executable, plus BI2 when present. No directory
    # table (fst.bin) and no apploader: the file tree is the native namespace
    # built from files/, and the native boot runs no disc loader.
    get_filename_component(SHADOW_EMBED_SYS_DIR "${SHADOW_RECOMP_DOL}" DIRECTORY)
    set(SHADOW_EMBED_BOOT "${SHADOW_EMBED_SYS_DIR}/boot.bin")
    set(SHADOW_EMBED_DOL "${SHADOW_RECOMP_DOL}")
    foreach(required IN ITEMS SHADOW_EMBED_BOOT SHADOW_EMBED_DOL)
        if(NOT EXISTS "${${required}}")
            message(FATAL_ERROR "Cannot embed missing game-system input: ${${required}}")
        endif()
    endforeach()
    # The resource compiler reads its script in the system's ANSI code page: a disc folder with letters outside it
    # (or outside ASCII, the script being UTF-8) ends in "RC2135: file not found". The inputs are copied into the
    # build tree, whose path the setup keeps plain, and embedded from there; the bytes are the same.
    set(SHADOW_EMBED_COPY_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/embed")
    file(MAKE_DIRECTORY "${SHADOW_EMBED_COPY_DIR}")
    foreach(required IN ITEMS SHADOW_EMBED_BOOT SHADOW_EMBED_DOL)
        get_filename_component(embed_name "${${required}}" NAME)
        file(COPY_FILE "${${required}}" "${SHADOW_EMBED_COPY_DIR}/${embed_name}" ONLY_IF_DIFFERENT)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${${required}}")
        file(TO_CMAKE_PATH "${SHADOW_EMBED_COPY_DIR}/${embed_name}" ${required})
    endforeach()

    set(SHADOW_EMBED_BI2_LINE "")
    if(EXISTS "${SHADOW_EMBED_SYS_DIR}/bi2.bin")
        file(COPY_FILE "${SHADOW_EMBED_SYS_DIR}/bi2.bin" "${SHADOW_EMBED_COPY_DIR}/bi2.bin" ONLY_IF_DIFFERENT)
        file(TO_CMAKE_PATH "${SHADOW_EMBED_COPY_DIR}/bi2.bin" SHADOW_EMBED_BI2)
        set(SHADOW_EMBED_BI2_LINE "102 RCDATA \"${SHADOW_EMBED_BI2}\"")
    endif()

    set(SHADOW_EMBED_RC "${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_game_data.rc")
    configure_file(runtime/src/embedded_game_data.rc.in "${SHADOW_EMBED_RC}" @ONLY)
    foreach(program_target IN LISTS SHADOW_PROGRAM_TARGETS)
        target_sources(${program_target} PRIVATE "${SHADOW_EMBED_RC}")
    endforeach()
endif()

# The executable's icon: the setup's multi-size logo (16-256 px) from the art pack
# (installer/ART_PACK.md). Resource 1 is the icon Explorer shows and host_window.cpp gives the window.
set(SHADOW_GAME_ICON "${CMAKE_CURRENT_SOURCE_DIR}/installer/art/app.ico" CACHE FILEPATH
    "Icon (.ico) linked into the game executable; skipped when the file is missing")
if(WIN32 AND EXISTS "${SHADOW_GAME_ICON}")
    file(TO_CMAKE_PATH "${SHADOW_GAME_ICON}" SHADOW_GAME_ICON_PATH)
    set(SHADOW_ICON_RC "${CMAKE_CURRENT_BINARY_DIR}/generated/game_icon.rc")
    file(CONFIGURE OUTPUT "${SHADOW_ICON_RC}" CONTENT "1 ICON \"${SHADOW_GAME_ICON_PATH}\"\n")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SHADOW_GAME_ICON}")
    foreach(program_target IN LISTS SHADOW_PROGRAM_TARGETS)
        target_sources(${program_target} PRIVATE "${SHADOW_ICON_RC}")
    endforeach()
    message(STATUS "Game icon: ${SHADOW_GAME_ICON_PATH}")
endif()

if(MSVC)
    foreach(program_target IN LISTS SHADOW_PROGRAM_TARGETS)
        target_compile_options(${program_target} PRIVATE /W4 /permissive-)
        set_property(TARGET ${program_target} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        # Recompiled guest code turns guest call depth into native call depth.
        target_link_options(${program_target} PRIVATE /STACK:16777216)
        # 2026-09-05 interpreter-eradication audit: a linker map binds every
        # generated fn_* symbol and the interpreter's Cpu::execute/step to their
        # x86-64 address ranges. Diagnostic only; the executable's bytes are unchanged.
        target_link_options(${program_target} PRIVATE "/MAP:$<TARGET_FILE_DIR:${program_target}>/${program_target}.map")
    endforeach()
endif()

if(SHADOW_BUILD_TESTS)
    include(CTest)
    # The new native camera and visibility system: projection construction,
    # the single Hor+ implementation, frustum derivation, and the visibility
    # classification with its per-plane rejection accounting. Pure arithmetic,
    # no device, so it runs everywhere and runs fast.
    add_executable(camera_tests runtime/tests/camera_tests.cpp runtime/src/gekko/camera.cpp)
    target_include_directories(camera_tests PRIVATE runtime/include)
    target_compile_features(camera_tests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(camera_tests PRIVATE /W4 /permissive-)
        set_property(TARGET camera_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME code_engine_tests COMMAND code_engine_tests)
    set_tests_properties(code_engine_tests PROPERTIES TIMEOUT 60 LABELS "native-runtime")
    add_test(NAME mod_loader_tests COMMAND mod_loader_tests)
    set_tests_properties(mod_loader_tests PROPERTIES TIMEOUT 120 LABELS "native-runtime")
    add_test(NAME camera_tests COMMAND camera_tests)
    set_tests_properties(camera_tests PROPERTIES TIMEOUT 60 LABELS "native-runtime")

    # The 2D <-> 3D boundary (runtime/lift/render/view.h, docs/boundary-20261007/DESIGN.md): world-anchored 2D lands on
    # its world point's pixel on every output, canvas and framing. Pure arithmetic.
    add_executable(view_boundary_tests runtime/tests/view_boundary_tests.cpp)
    target_include_directories(view_boundary_tests PRIVATE runtime/lift)
    target_compile_features(view_boundary_tests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(view_boundary_tests PRIVATE /W4 /permissive-)
        set_property(TARGET view_boundary_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME view_boundary_tests COMMAND view_boundary_tests)
    set_tests_properties(view_boundary_tests PROPERTIES TIMEOUT 60 LABELS "lift;render")

    # The lens flare's native layout (runtime/lift/effects/lens_flare.h, docs/boundary-20261007/FLARE.md) against the
    # console's builder reconstructed from its disassembly: identical at the raster's aspect except the glow's
    # documented edge bands; the view the output shows on wider / narrower / split-screen views. Pure arithmetic.
    add_executable(lens_flare_tests runtime/tests/lens_flare_tests.cpp)
    target_include_directories(lens_flare_tests PRIVATE runtime/lift)
    target_compile_features(lens_flare_tests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(lens_flare_tests PRIVATE /W4 /permissive- ${SHADOW_ARCH_FLAGS})
        set_property(TARGET lens_flare_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME lens_flare_tests COMMAND lens_flare_tests)
    set_tests_properties(lens_flare_tests PROPERTIES TIMEOUT 60 LABELS "lift;render")

    # The 2D layout's tiled strips (runtime/lift/ui/tiled_strip.h, docs/LOADING_SCREEN_DECOMPOSITION.md 3): the loading
    # screen's rotating line chains recognised and continued to the visible edge on every aspect, nothing added at 4:3,
    # the false positives rejected. Pure arithmetic.
    add_executable(tiled_strip_tests runtime/tests/tiled_strip_tests.cpp)
    target_include_directories(tiled_strip_tests PRIVATE runtime/lift)
    target_compile_features(tiled_strip_tests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(tiled_strip_tests PRIVATE /W4 /permissive-)
        set_property(TARGET tiled_strip_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME tiled_strip_tests COMMAND tiled_strip_tests)
    set_tests_properties(tiled_strip_tests PROPERTIES TIMEOUT 60 LABELS "lift;render")

    # The pad.cfg contract between tools/launcher.py (writer) and the runtime
    # (reader): the launcher's exact output loads, and a partial number, an
    # out-of-range value or an unknown key is an error rather than a setting
    # that silently loads as something else.
    add_executable(pad_mapping_tests runtime/tests/pad_mapping_tests.cpp)
    target_link_libraries(pad_mapping_tests PRIVATE shadow_lift)
    target_compile_features(pad_mapping_tests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(pad_mapping_tests PRIVATE /W4 /permissive-)
        set_property(TARGET pad_mapping_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME pad_mapping_tests COMMAND pad_mapping_tests)
    set_tests_properties(pad_mapping_tests PROPERTIES TIMEOUT 60 LABELS "native-runtime")

    # The lifted peripheral layer (docs/lift-20261006/L1_INPUT.md): the PC
    # implementation of the engine's peripheral interface, the gamepad and
    # keyboard mappings, persistence. Pure native code, no runtime.
    add_executable(lift_peripheral_tests runtime/tests/lift_peripheral_tests.cpp)
    target_link_libraries(lift_peripheral_tests PRIVATE shadow_lift)
    if(MSVC)
        target_compile_options(lift_peripheral_tests PRIVATE /W4 /permissive-)
        set_property(TARGET lift_peripheral_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME lift_peripheral_tests COMMAND lift_peripheral_tests)
    set_tests_properties(lift_peripheral_tests PROPERTIES TIMEOUT 60 LABELS "lift")

    # The renderer core (docs/lift-20261006/R_DESIGN.md section 6, R2a) on WARP
    # with the D3D12 debug layer: device floor, bindless descriptors, upload
    # ring, copy-queue batches, frame graph, 4x resolve, determinism, swap chain.
    if(WIN32)
        add_executable(packet_queue_tests runtime/tests/packet_queue_tests.cpp)
        target_include_directories(packet_queue_tests PRIVATE runtime/lift)
        target_compile_features(packet_queue_tests PRIVATE cxx_std_20)
        target_compile_definitions(packet_queue_tests PRIVATE GEKKO_SHIPPING=1)
        if(MSVC)
            target_compile_options(packet_queue_tests PRIVATE /W4 /permissive-)
            set_property(TARGET packet_queue_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME packet_queue_tests COMMAND packet_queue_tests)
        set_tests_properties(packet_queue_tests PROPERTIES TIMEOUT 30 LABELS "lift;render;ownership")

        add_executable(render_core_tests runtime/tests/render_core_tests.cpp)
        target_link_libraries(render_core_tests PRIVATE shadow_render)
        if(MSVC)
            target_compile_options(render_core_tests PRIVATE /W4 /permissive-)
            set_property(TARGET render_core_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME render_core_tests COMMAND render_core_tests)
        set_tests_properties(render_core_tests PROPERTIES TIMEOUT 180 LABELS "lift;render")
        # NE-RW2: the native immediate modes (no device: frame packets only).
        add_executable(rwim_tests runtime/tests/rwim_tests.cpp)
        target_link_libraries(rwim_tests PRIVATE shadow_rwim)
        if(MSVC)
            target_compile_options(rwim_tests PRIVATE /W4 /permissive-)
            set_property(TARGET rwim_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME rwim_tests COMMAND rwim_tests)
        set_tests_properties(rwim_tests PROPERTIES TIMEOUT 60 LABELS "lift;render")
        # The effect layer switches (docs/EFFECTS_INVENTORY.md "Layer switches"; frame packets only).
        add_executable(effect_layer_tests runtime/tests/effect_layer_tests.cpp)
        target_include_directories(effect_layer_tests PRIVATE runtime/lift)
        target_link_libraries(effect_layer_tests PRIVATE shadow_render)
        if(MSVC)
            target_compile_options(effect_layer_tests PRIVATE /W4 /permissive-)
            set_property(TARGET effect_layer_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME effect_layer_tests COMMAND effect_layer_tests)
        set_tests_properties(effect_layer_tests PROPERTIES TIMEOUT 30 LABELS "lift;render")
        # Independent frame (docs/re/independent-frame/): a packet between two packets and the render timeline, on
        # synthetic clocks (frame packets only: no device, no game).
        add_executable(frame_interp_tests runtime/tests/frame_interp_tests.cpp
            runtime/lift/render/frame.cpp runtime/lift/render/frame_interp.cpp)
        target_include_directories(frame_interp_tests PRIVATE runtime/lift)
        target_compile_features(frame_interp_tests PRIVATE cxx_std_20)
        if(MSVC)
            target_compile_options(frame_interp_tests PRIVATE /W4 /permissive-)
            set_property(TARGET frame_interp_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME frame_interp_tests COMMAND frame_interp_tests)
        set_tests_properties(frame_interp_tests PROPERTIES TIMEOUT 60 LABELS "lift;render")
    endif()

    # The asset importers; with the game data present every disc file is
    # walked and every texture decoded against the current decoder (the
    # oracle, retired together with the old renderer in R2f).
    add_executable(asset_import_tests runtime/tests/asset_import_tests.cpp
                   runtime/src/gekko/gx_texture_decode.cpp runtime/src/gekko/prs.cpp
                   runtime/src/native_memory.cpp)
    target_link_libraries(asset_import_tests PRIVATE shadow_assets)
    target_include_directories(asset_import_tests PRIVATE runtime/include)
    if(MSVC)
        target_compile_options(asset_import_tests PRIVATE /W4 /permissive-)
        set_property(TARGET asset_import_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    if(NOT SHADOW_RECOMP_DOL STREQUAL "")
        add_test(NAME asset_import_tests COMMAND asset_import_tests "${SHADOW_RECOMP_DOL}")
    else()
        add_test(NAME asset_import_tests COMMAND asset_import_tests)
    endif()
    set_tests_properties(asset_import_tests PROPERTIES TIMEOUT 600 LABELS "lift;assets")

    # The Options > Audio category volumes (gekko/audio_categories.h, native_audio_mixer.h VoiceFrame::gain): the
    # guest-table voice classifier and the mixer's per-voice gain. Standalone (no guest, no device).
    add_executable(options_audio_mix_tests runtime/tests/options_audio_mix_tests.cpp
                   runtime/src/native_audio_mixer.cpp runtime/lift/audio/reverb.cpp
                   runtime/lift/audio/output_stage.cpp runtime/lift/audio/effect_stage.cpp)
    target_include_directories(options_audio_mix_tests PRIVATE runtime/include runtime/lift)
    target_compile_features(options_audio_mix_tests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(options_audio_mix_tests PRIVATE /W4 /permissive-)
        set_property(TARGET options_audio_mix_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    # The mixer is compiled for the build's host ISA model: it links shadow_host_isa (the model's flags AND the CPUID gate), instead of
    # a hand-set /arch:AVX2 that left this image without the gate (native_host_isa_gate lane E).
    target_link_libraries(options_audio_mix_tests PRIVATE shadow_host_isa)
    add_test(NAME options_audio_mix_tests COMMAND options_audio_mix_tests)
    set_tests_properties(options_audio_mix_tests PROPERTIES TIMEOUT 60 LABELS "audio;menu")
    # The game's save file on the host (gekko/save_file.h, docs/SAVE_REROUTE_PLAN.md): DES / MD5 vectors, the GCI
    # round trip, extraction from a memory-card image, the record check. Real saves can be passed as arguments.
    add_executable(save_file_tests runtime/tests/save_file_tests.cpp runtime/src/gekko/save_file.cpp
                   runtime/src/gekko/save_import.cpp)
    target_include_directories(save_file_tests PRIVATE runtime/include)
    target_compile_features(save_file_tests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(save_file_tests PRIVATE /W4 /permissive-)
        set_property(TARGET save_file_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME save_file_tests COMMAND save_file_tests)
    set_tests_properties(save_file_tests PROPERTIES TIMEOUT 60 LABELS "saves")
    # The universal save recognizer (gekko/save_recognize.h, docs/SAVE_FORMATS.md): CRC-32 / deflate / LZARI, every
    # container built here (GC GCI and card image in three regions, AR MAX, SharkPort, PSV, CodeBreaker, Xbox
    # Save.dat, ZIP), the GC conversion to the PC GCI, scan(). The user's sample saves can be passed as arguments.
    add_executable(save_recognize_tests runtime/tests/save_recognize_tests.cpp runtime/src/gekko/save_recognize.cpp
                   runtime/src/gekko/save_file.cpp)
    target_include_directories(save_recognize_tests PRIVATE runtime/include)
    target_compile_features(save_recognize_tests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(save_recognize_tests PRIVATE /W4 /permissive-)
        set_property(TARGET save_recognize_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME save_recognize_tests COMMAND save_recognize_tests)
    set_tests_properties(save_recognize_tests PROPERTIES TIMEOUT 120 LABELS "saves")
    # Settings validation: config_file and PcSettingsService (options.cfg: defaults, bad values, the retired
    # language 6, line-preserving saves, threads). The audio gain and XInput owners are recording stubs inside the test.
    add_executable(options_settings_tests runtime/tests/options_settings_tests.cpp
                   runtime/src/gekko/config_file.cpp runtime/src/gekko/pc_settings.cpp)
    target_include_directories(options_settings_tests PRIVATE runtime/include runtime/lift)
    target_compile_features(options_settings_tests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(options_settings_tests PRIVATE /W4 /permissive-)
        set_property(TARGET options_settings_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME options_settings_tests COMMAND options_settings_tests)
    set_tests_properties(options_settings_tests PROPERTIES TIMEOUT 120 LABELS "menu")
    # Input settings (Agent 4): the XInput pad's preferences, bindings and options.cfg. The test defines the audio gain
    # functions itself, so audio_runtime.cpp is not linked.
    add_executable(options_input_tests runtime/tests/options_input_tests.cpp
                   runtime/src/gekko/xinput_pad.cpp runtime/src/gekko/pc_settings.cpp runtime/src/gekko/config_file.cpp
                   runtime/lift/peripheral/gamepad_binding.cpp runtime/lift/peripheral/keyboard_layout.cpp
                   runtime/lift/peripheral/controls.cpp)
    target_include_directories(options_input_tests PRIVATE runtime/include runtime/lift)
    target_compile_features(options_input_tests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(options_input_tests PRIVATE /W4 /permissive-)
        set_property(TARGET options_input_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME options_input_tests COMMAND options_input_tests)
    set_tests_properties(options_input_tests PROPERTIES TIMEOUT 60 LABELS "menu")
    # Configuration / persistence: options.cfg and display.cfg's Alt+Enter toggle (gekko/display_settings.h
    # FullscreenToggle), with the audio gain and the XInput preferences stubbed inside the test.
    add_executable(options_config_tests runtime/tests/options_config_tests.cpp
                   runtime/src/gekko/config_file.cpp runtime/src/gekko/pc_settings.cpp
                   runtime/src/gekko/display_settings.cpp)
    target_include_directories(options_config_tests PRIVATE runtime/include runtime/lift)
    target_compile_features(options_config_tests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(options_config_tests PRIVATE /W4 /permissive-)
        set_property(TARGET options_config_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME options_config_tests COMMAND options_config_tests)
    set_tests_properties(options_config_tests PROPERTIES TIMEOUT 60 LABELS "menu")
    # The native keyboard and mouse layer (docs/controls/NATIVE_INPUT_ARCHITECTURE.md): actions, controls.cfg, the mouse's
    # camera arithmetic. Pure native code; the settings service uses the shared config file writer.
    add_executable(input_actions_tests runtime/tests/input_actions_tests.cpp
                   runtime/src/gekko/controls_settings.cpp runtime/src/gekko/config_file.cpp
                   runtime/lift/peripheral/action_map.cpp runtime/lift/peripheral/mouse_look.cpp
                   runtime/lift/peripheral/controls.cpp runtime/lift/peripheral/manager_imp_pc.cpp)
    target_include_directories(input_actions_tests PRIVATE runtime/include runtime/lift)
    target_compile_features(input_actions_tests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(input_actions_tests PRIVATE /W4 /permissive-)
        set_property(TARGET input_actions_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME input_actions_tests COMMAND input_actions_tests)
    set_tests_properties(input_actions_tests PROPERTIES TIMEOUT 60 LABELS "native-runtime")
    # The gameplay recorder's tape (gekko/input_tape.h, docs/tools/RECORDER.md): version 3 round trip, versions 1 / 2
    # read as before (a version 2 output is its logic aspect), checkpoint comparison (gekko/replay_check.h).
    add_executable(input_tape_tests runtime/tests/input_tape_tests.cpp runtime/src/gekko/input_tape.cpp)
    target_include_directories(input_tape_tests PRIVATE runtime/include runtime/lift)
    target_compile_features(input_tape_tests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(input_tape_tests PRIVATE /W4 /permissive-)
        set_property(TARGET input_tape_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME input_tape_tests COMMAND input_tape_tests)
    set_tests_properties(input_tape_tests PROPERTIES TIMEOUT 60 LABELS "recorder")
    # Erratum E4, the dialogue window / voice hold (docs/GAME_ERRATA.md): the decision logic on a fake guest.
    add_executable(native_message_voice_tests runtime/tests/native_message_voice_tests.cpp
                   runtime/src/gekko/native_message_system.cpp runtime/src/gekko/native_dialogue_voice.cpp)
    target_include_directories(native_message_voice_tests PRIVATE runtime/include runtime/lift)
    target_compile_features(native_message_voice_tests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(native_message_voice_tests PRIVATE /W4 /permissive-)
        set_property(TARGET native_message_voice_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME native_message_voice_tests COMMAND native_message_voice_tests)
    set_tests_properties(native_message_voice_tests PROPERTIES TIMEOUT 60 LABELS "native-runtime")

    # Full-height menu backgrounds (gekko/menu_backgrounds.h): with the game data, the five retail layouts are
    # rewritten in exactly their background's scaleY word; anything else is refused.
    add_executable(menu_backgrounds_tests runtime/tests/menu_backgrounds_tests.cpp
                   runtime/src/gekko/menu_backgrounds.cpp)
    target_include_directories(menu_backgrounds_tests PRIVATE runtime/include)
    target_compile_features(menu_backgrounds_tests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(menu_backgrounds_tests PRIVATE /W4 /permissive-)
        set_property(TARGET menu_backgrounds_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    if(SHADOW_RECOMP_DOL)
        add_test(NAME menu_backgrounds_tests COMMAND menu_backgrounds_tests "${SHADOW_RECOMP_DOL}")
    else()
        add_test(NAME menu_backgrounds_tests COMMAND menu_backgrounds_tests)
    endif()
    set_tests_properties(menu_backgrounds_tests PROPERTIES TIMEOUT 60 LABELS "assets;menu")

    # tools/launcher.py's pure helpers and log reading: budget parsing, flag
    # splitting, per-line output decoding, the path scrubber, the classifier
    # and the live-stats reading. No window and no game, so it runs anywhere
    # Python does; skipped where it does not.
    find_package(Python3 COMPONENTS Interpreter QUIET)
    add_executable(native_display_tests runtime/tests/native_display_tests.cpp
                   runtime/src/native_display.cpp runtime/src/gekko/display.cpp
                   runtime/src/gekko/camera.cpp runtime/src/gekko/host_jobs.cpp)
    target_include_directories(native_display_tests PRIVATE runtime/include)
    # NE-RW2c: display.h's 2D anchors / classifiers are the 2D layout policy's (shadow_lift, runtime/lift/ui).
    target_link_libraries(native_display_tests PRIVATE shadow_lift)
    target_compile_features(native_display_tests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(native_display_tests PRIVATE /W4 /permissive-)
        set_property(TARGET native_display_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME native_display_tests COMMAND native_display_tests)
    set_tests_properties(native_display_tests PROPERTIES TIMEOUT 60 LABELS "native-runtime")
    if(Python3_Interpreter_FOUND)
        if(TARGET ShadowTheHedgehog)
            add_test(NAME shipping_platform_selection
                     COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_SOURCE_DIR}/tools/test_shipping_platform_selection.py
                             --shipping $<TARGET_FILE:ShadowTheHedgehog>
                             --reference $<TARGET_FILE:ShadowTheHedgehogReference>
                             --shipping-build=$<BOOL:${SHADOW_SHIPPING}>)
            set_tests_properties(shipping_platform_selection PROPERTIES TIMEOUT 60 LABELS "native;reference;build")
        endif()
        add_test(NAME launcher_tests
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/test_launcher.py)
        set_tests_properties(launcher_tests PROPERTIES TIMEOUT 60 LABELS "tools")
        # naming-evidence gates of tools/dolrecon (string pools, callback registers): cases that once passed wrongly
        add_test(NAME dolrecon_evidence_tests
                 COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_SOURCE_DIR}/tools/test_dolrecon_evidence.py
                 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/tools)
        set_tests_properties(dolrecon_evidence_tests PROPERTIES TIMEOUT 60 LABELS "tools")
        # tools/platform/structure_match.py: instruction tokens and owner templates on fixed words
        add_test(NAME structure_match_tests
                 COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_SOURCE_DIR}/tools/test_structure_match.py)
        set_tests_properties(structure_match_tests PROPERTIES TIMEOUT 60 LABELS "tools;platform")
    endif()

    # Adversarial mathematical audit of the camera/visibility system: AABB
    # straddle cases on every plane, off-centre projections that expose
    # row/column-major confusion, and the proof that frustum membership is the
    # same predicate as clip-space membership.
    add_executable(camera_frustum_tests runtime/tests/camera_frustum_tests.cpp runtime/src/gekko/camera.cpp)
    target_include_directories(camera_frustum_tests PRIVATE runtime/include)
    target_compile_features(camera_frustum_tests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(camera_frustum_tests PRIVATE /W4 /permissive-)
        set_property(TARGET camera_frustum_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME camera_frustum_tests COMMAND camera_frustum_tests)
    set_tests_properties(camera_frustum_tests PROPERTIES TIMEOUT 120 LABELS "native-runtime")

    # NativeWorldAnchorProjection: world anchor -> canonical camera -> NDC ->
    # presentation pixels, with the captured gauge transitions as fixtures.
    add_executable(world_anchor_tests runtime/tests/world_anchor_tests.cpp
                   runtime/src/gekko/world_anchor.cpp
    runtime/src/gekko/health_bar.cpp runtime/src/gekko/camera.cpp)
    target_include_directories(world_anchor_tests PRIVATE runtime/include)
    target_compile_features(world_anchor_tests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(world_anchor_tests PRIVATE /W4 /permissive-)
        set_property(TARGET world_anchor_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME world_anchor_tests COMMAND world_anchor_tests)
    set_tests_properties(world_anchor_tests PROPERTIES TIMEOUT 60 LABELS "native-runtime")

    # Texture packs (docs/mods/TEXTURE_PACKS.md): XXH64 reference values, the Dolphin name rule (base-level hash,
    # used palette range), the name parser, the DDS / PNG decoders and the prioritised index.
    add_executable(texture_packs_tests runtime/tests/texture_packs_tests.cpp runtime/src/gekko/texture_packs.cpp)
    target_include_directories(texture_packs_tests PRIVATE runtime/include thirdparty/stb)
    target_compile_features(texture_packs_tests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(texture_packs_tests PRIVATE /W4 /permissive-)
        set_property(TARGET texture_packs_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME texture_packs_tests COMMAND texture_packs_tests)
    set_tests_properties(texture_packs_tests PROPERTIES TIMEOUT 60 LABELS "native-runtime;mods")

    # The rebuilt enemy health bar: the authored depth window, the scale law,
    # the width law and the instantaneous fill, plus the exact world-to-view
    # camera decode that replaces frustum-plane archaeology. Every camera in
    # the suite is deliberately non-identity - a suite built on Transform{}
    # cannot tell a correct camera from a default-constructed one, because
    # both compute depth = -world_z, which is exactly the defect that shipped.
    add_executable(health_bar_tests runtime/tests/health_bar_tests.cpp
                   runtime/src/gekko/health_bar.cpp runtime/src/gekko/world_anchor.cpp
                   runtime/src/gekko/camera.cpp)
    target_include_directories(health_bar_tests PRIVATE runtime/include)
    target_compile_features(health_bar_tests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(health_bar_tests PRIVATE /W4 /permissive-)
        set_property(TARGET health_bar_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME health_bar_tests COMMAND health_bar_tests)
    set_tests_properties(health_bar_tests PROPERTIES TIMEOUT 60 LABELS "native-runtime")

    # The Native Asset Runtime: AssetId identity, registry, providers, handle
    # lifetime and the streaming scheduler. Defends the contracts recovered
    # from the guest loader - reject-at-submit, priority-then-FIFO, no partial
    # completion, and reads past EOF that succeed and zero-fill.
    add_executable(asset_runtime_tests runtime/tests/asset_runtime_tests.cpp
                   runtime/tests/support/fst_oracle.cpp
                   runtime/src/gekko/asset_runtime.cpp runtime/src/gekko/asset_providers.cpp
                   runtime/src/gekko/asset_namespace.cpp runtime/src/gekko/asset_mount.cpp
                   runtime/backends/storage/storage_backends.cpp
                   runtime/src/gekko/prs.cpp runtime/src/native_memory.cpp)
    target_include_directories(asset_runtime_tests PRIVATE runtime/include runtime/tests)
    target_compile_features(asset_runtime_tests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(asset_runtime_tests PRIVATE /W4 /permissive-)
        set_property(TARGET asset_runtime_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME asset_runtime_tests COMMAND asset_runtime_tests)
    set_tests_properties(asset_runtime_tests PROPERTIES TIMEOUT 60 LABELS "native-runtime")
    # cs2/ram: the same suite with every cs2/ram saving off (GEKKO_CS2_RAM=0), as the previous behaviour.
    add_test(NAME asset_runtime_tests_cs2_ram_off COMMAND asset_runtime_tests)
    set_tests_properties(asset_runtime_tests_cs2_ram_off PROPERTIES TIMEOUT 60 LABELS "native-runtime"
                         ENVIRONMENT "GEKKO_CS2_RAM=0")

    # The native asset namespace: ordering, the path grammar, refusals, and -
    # with the game data present - the host tree against the disc's own FST as
    # an oracle, entry for entry (docs/native-asset-pipeline-20261005/). The
    # table format lives only in the test-only oracle (runtime/tests/support).
    add_executable(asset_namespace_tests runtime/tests/asset_namespace_tests.cpp
                   runtime/tests/support/fst_oracle.cpp
                   runtime/src/gekko/native_save_text.cpp
                   runtime/src/gekko/asset_namespace.cpp runtime/src/gekko/asset_mount.cpp
                   runtime/backends/storage/storage_backends.cpp
                   runtime/src/gekko/asset_runtime.cpp runtime/src/gekko/asset_providers.cpp
                   runtime/src/gekko/prs.cpp runtime/src/native_memory.cpp)
    target_include_directories(asset_namespace_tests PRIVATE runtime/include runtime/tests)
    target_compile_features(asset_namespace_tests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(asset_namespace_tests PRIVATE /W4 /permissive-)
        set_property(TARGET asset_namespace_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    if(NOT SHADOW_RECOMP_DOL STREQUAL "")
        add_test(NAME asset_namespace_tests COMMAND asset_namespace_tests "${SHADOW_RECOMP_DOL}")
    else()
        add_test(NAME asset_namespace_tests COMMAND asset_namespace_tests)
    endif()
    set_tests_properties(asset_namespace_tests PROPERTIES TIMEOUT 120 LABELS "native-runtime")

    # The runtime contract's storage service (shadow/runtime/v1/runtime.h) against both backends: host-directory on
    # ../game/files and gamecube-disc on the disc image tools/platform/gcm.py rebuilds into .build/platform. Every
    # file byte for byte, the read rule, concurrency, and the same guest tokens through AssetSystem. Registered only
    # when both media exist (build the image first: python -B tools/platform/gcm.py build ../game .build/platform/GUPP8P.gcm).
    add_executable(runtime_storage_contract_tests runtime/tests/runtime_storage_contract_tests.cpp
                   runtime/src/gekko/asset_runtime.cpp runtime/src/gekko/asset_providers.cpp
                   runtime/src/gekko/asset_namespace.cpp runtime/src/gekko/asset_mount.cpp
                   runtime/backends/storage/storage_backends.cpp
                   runtime/src/gekko/prs.cpp runtime/src/native_memory.cpp)
    target_include_directories(runtime_storage_contract_tests PRIVATE runtime/include runtime/tests)
    target_compile_features(runtime_storage_contract_tests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(runtime_storage_contract_tests PRIVATE /W4 /permissive-)
        set_property(TARGET runtime_storage_contract_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    set(SHADOW_DISC_IMAGE "${CMAKE_CURRENT_SOURCE_DIR}/.build/platform/GUPP8P.gcm" CACHE FILEPATH
        "The GameCube disc image the gamecube-disc storage backend tests read")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../game/files" AND EXISTS "${SHADOW_DISC_IMAGE}")
        add_test(NAME runtime_storage_contract_tests COMMAND runtime_storage_contract_tests
                 "${CMAKE_CURRENT_SOURCE_DIR}/../game" "${SHADOW_DISC_IMAGE}")
        set_tests_properties(runtime_storage_contract_tests PROPERTIES TIMEOUT 600 LABELS "native-runtime;runtime-contract")
    endif()

    # The prompts' modern controller buttons (gekko/button_glyphs.h): the style key, the family choice and, with the
    # game data, every button sheet patched as the game reads it.
    add_executable(button_glyphs_tests runtime/tests/button_glyphs_tests.cpp
                   runtime/src/gekko/button_glyphs.cpp runtime/src/gekko/gx_texture_decode.cpp
                   runtime/src/gekko/native_save_text.cpp
                   runtime/src/gekko/asset_namespace.cpp runtime/src/gekko/asset_mount.cpp
                   runtime/backends/storage/storage_backends.cpp
                   runtime/src/gekko/asset_runtime.cpp runtime/src/gekko/asset_providers.cpp
                   runtime/src/gekko/prs.cpp runtime/src/native_memory.cpp)
    target_link_libraries(button_glyphs_tests PRIVATE shadow_lift)
    target_include_directories(button_glyphs_tests PRIVATE runtime/include runtime/tests)
    target_compile_features(button_glyphs_tests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(button_glyphs_tests PRIVATE /W4 /permissive-)
        set_property(TARGET button_glyphs_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    if(NOT SHADOW_RECOMP_DOL STREQUAL "")
        add_test(NAME button_glyphs_tests COMMAND button_glyphs_tests "${SHADOW_RECOMP_DOL}")
    else()
        add_test(NAME button_glyphs_tests COMMAND button_glyphs_tests)
    endif()
    set_tests_properties(button_glyphs_tests PROPERTIES TIMEOUT 120 LABELS "assets;menu")
    # cs2/ram: the same suite with GEKKO_CS2_RAM=0 (scaled sheets made at attach by a worker, as before); the
    # "sharp-sheet digest" line of both runs must be the same.
    if(NOT SHADOW_RECOMP_DOL STREQUAL "")
        add_test(NAME button_glyphs_tests_cs2_ram_off COMMAND button_glyphs_tests "${SHADOW_RECOMP_DOL}")
    else()
        add_test(NAME button_glyphs_tests_cs2_ram_off COMMAND button_glyphs_tests)
    endif()
    set_tests_properties(button_glyphs_tests_cs2_ram_off PROPERTIES TIMEOUT 120 LABELS "assets;menu"
                         ENVIRONMENT "GEKKO_CS2_RAM=0")

    # The new native bloom system's mathematics: the sizing rule, the gain
    # chain, the tap-set generator and the CPU reference of the pass chain.
    # Pure arithmetic, no device, so it runs everywhere and runs fast.
    add_executable(bloom_tests runtime/tests/bloom_tests.cpp runtime/src/gekko/bloom.cpp)
    target_include_directories(bloom_tests PRIVATE runtime/include)
    target_compile_features(bloom_tests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(bloom_tests PRIVATE /W4 /permissive-)
        set_property(TARGET bloom_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME bloom_tests COMMAND bloom_tests)
    set_tests_properties(bloom_tests PROPERTIES TIMEOUT 60 LABELS "native-runtime")

    add_executable(reverb_block_hash_tests runtime/tests/reverb_block_hash_tests.cpp)
    target_link_libraries(reverb_block_hash_tests PRIVATE shadow_lift)
    if(MSVC)
        target_compile_options(reverb_block_hash_tests PRIVATE /W4 /permissive-)
        set_property(TARGET reverb_block_hash_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME reverb_block_hash_tests COMMAND reverb_block_hash_tests)
    set_tests_properties(reverb_block_hash_tests PROPERTIES TIMEOUT 120 LABELS "native-runtime")

    # cs2 audio: contract tests of header-only audio kernels (reverb clean vector ops, linear SRC, decode-ahead claim).
    foreach(cs2_audio_test reverb_ops_tests audio_src_tests claimable_task_tests)
        add_executable(${cs2_audio_test} runtime/tests/${cs2_audio_test}.cpp)
        target_include_directories(${cs2_audio_test} PRIVATE runtime/include runtime/lift)
        target_compile_features(${cs2_audio_test} PRIVATE cxx_std_20)
        target_link_libraries(${cs2_audio_test} PRIVATE shadow_host_isa)
        if(MSVC)
            target_compile_options(${cs2_audio_test} PRIVATE /W4 /permissive-)
            set_property(TARGET ${cs2_audio_test} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME ${cs2_audio_test} COMMAND ${cs2_audio_test})
        set_tests_properties(${cs2_audio_test} PROPERTIES TIMEOUT 120 LABELS "native-runtime;audio")
    endforeach()

    add_executable(flat_word_map_tests runtime/tests/flat_word_map_tests.cpp)
    target_include_directories(flat_word_map_tests PRIVATE runtime/include)
    target_compile_features(flat_word_map_tests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(flat_word_map_tests PRIVATE /W4 /permissive-)
        set_property(TARGET flat_word_map_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME flat_word_map_tests COMMAND flat_word_map_tests)
    set_tests_properties(flat_word_map_tests PROPERTIES TIMEOUT 60 LABELS "native-runtime")

    # cpu/opt2: stfs(lfs(w)) == w for all 2^32 words: the identity behind ppcrecomp --hot-value bit 1. Compiled with
    # the flags of the generated code (the host conversions it uses are the ones under test).
    add_executable(fp_single_round_trip_tests runtime/tests/fp_single_round_trip_tests.cpp)
    target_include_directories(fp_single_round_trip_tests PRIVATE runtime/include)
    target_compile_features(fp_single_round_trip_tests PRIVATE cxx_std_20)
    # The generated code's host instruction level, with the gate every image compiled for it carries.
    target_link_libraries(fp_single_round_trip_tests PRIVATE shadow_host_isa)
    if(MSVC)
        target_compile_options(fp_single_round_trip_tests PRIVATE /W4 /permissive-)
        set_property(TARGET fp_single_round_trip_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME fp_single_round_trip_tests COMMAND fp_single_round_trip_tests)
    set_tests_properties(fp_single_round_trip_tests PROPERTIES TIMEOUT 600 LABELS "native-runtime")

    # opt-intrinsics (round 2): the plain-float routes of the host intrinsics against the original sequences (bit-exact).
    if(TARGET shadow_runtime_reference)
        add_executable(opt_intrinsics_diff_tests runtime/tests/opt_intrinsics_diff_tests.cpp)
        target_link_libraries(opt_intrinsics_diff_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(opt_intrinsics_diff_tests PRIVATE /W4 /permissive-)
            set_property(TARGET opt_intrinsics_diff_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME opt_intrinsics_diff_tests COMMAND opt_intrinsics_diff_tests)
        set_tests_properties(opt_intrinsics_diff_tests PROPERTIES TIMEOUT 1200 LABELS "native-runtime")
    endif()

    add_executable(native_gameplay_tests runtime/tests/native_gameplay_tests.cpp)
    target_link_libraries(native_gameplay_tests PRIVATE shadow_native_gameplay)
    if(MSVC)
        target_compile_options(native_gameplay_tests PRIVATE /W4 /permissive-)
        set_property(TARGET native_gameplay_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME native_gameplay_tests COMMAND native_gameplay_tests)
    set_tests_properties(native_gameplay_tests PROPERTIES TIMEOUT 60 LABELS "native-gameplay")
    if(NOT SHADOW_RECOMP_DOL STREQUAL "")
        add_executable(audio_output_contract_tests runtime/tests/audio_output_contract_tests.cpp)
        target_link_libraries(audio_output_contract_tests PRIVATE shadow_lift)
        if(MSVC)
            target_compile_options(audio_output_contract_tests PRIVATE /W4 /permissive-)
            set_property(TARGET audio_output_contract_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME audio_output_contract_tests COMMAND audio_output_contract_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(audio_output_contract_tests PROPERTIES TIMEOUT 120 LABELS "reference;native-contract")
        add_executable(axfx_reverb_contract_tests runtime/tests/axfx_reverb_contract_tests.cpp)
        target_link_libraries(axfx_reverb_contract_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(axfx_reverb_contract_tests PRIVATE /W4 /permissive-)
            set_property(TARGET axfx_reverb_contract_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME axfx_reverb_contract_tests COMMAND axfx_reverb_contract_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(axfx_reverb_contract_tests PROPERTIES TIMEOUT 120 LABELS "reference;native-contract")
        add_executable(axfx_effects_contract_tests runtime/tests/axfx_effects_contract_tests.cpp)
        target_link_libraries(axfx_effects_contract_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(axfx_effects_contract_tests PRIVATE /W4 /permissive-)
            set_property(TARGET axfx_effects_contract_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME axfx_effects_contract_tests COMMAND axfx_effects_contract_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(axfx_effects_contract_tests PROPERTIES TIMEOUT 600 LABELS "reference;native-contract")
        # os.memory + os.time (docs/agents/axfx-effects.md, Step B): the arena accessors and the time-base readers.
        add_executable(os_arena_clock_contract_tests runtime/tests/os_arena_clock_contract_tests.cpp)
        target_link_libraries(os_arena_clock_contract_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(os_arena_clock_contract_tests PRIVATE /W4 /permissive-)
            set_property(TARGET os_arena_clock_contract_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME os_arena_clock_contract_tests COMMAND os_arena_clock_contract_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(os_arena_clock_contract_tests PROPERTIES TIMEOUT 120 LABELS "reference;native-contract")
        # os.debug (docs/agents/axfx-effects.md, Step C): InitializeUART.
        add_executable(os_debug_contract_tests runtime/tests/os_debug_contract_tests.cpp)
        target_link_libraries(os_debug_contract_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(os_debug_contract_tests PRIVATE /W4 /permissive-)
            set_property(TARGET os_debug_contract_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME os_debug_contract_tests COMMAND os_debug_contract_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(os_debug_contract_tests PROPERTIES TIMEOUT 120 LABELS "reference;native-contract")
        # os.debug (docs/agents/axfx-effects.md, Step D): OSReport's native formatter against the original vprintf.
        add_executable(os_report_contract_tests runtime/tests/os_report_contract_tests.cpp)
        target_link_libraries(os_report_contract_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(os_report_contract_tests PRIVATE /W4 /permissive-)
            set_property(TARGET os_report_contract_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME os_report_contract_tests COMMAND os_report_contract_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(os_report_contract_tests PROPERTIES TIMEOUT 900 LABELS "reference;native-contract")
        # debug.trk (docs/agents/axfx-effects.md, Step G): DBInit and the TRK console procedures.
        add_executable(trk_contract_tests runtime/tests/trk_contract_tests.cpp)
        target_link_libraries(trk_contract_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(trk_contract_tests PRIVATE /W4 /permissive-)
            set_property(TARGET trk_contract_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME trk_contract_tests COMMAND trk_contract_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(trk_contract_tests PROPERTIES TIMEOUT 120 LABELS "reference;native-contract")
        # saves.card (docs/agents/card-saves.md): the save store's owners against the original DOL words.
        add_executable(save_store_contract_tests runtime/tests/save_store_contract_tests.cpp)
        target_link_libraries(save_store_contract_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(save_store_contract_tests PRIVATE /W4 /permissive-)
            set_property(TARGET save_store_contract_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME save_store_contract_tests COMMAND save_store_contract_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(save_store_contract_tests PROPERTIES TIMEOUT 600 LABELS "reference;native-contract;saves")
        # os.interrupts (docs/agents/card-saves.md): the interrupt gate's owners and the exception-vector table
        # against the original DOL words.
        add_executable(gx_state_contract_tests runtime/tests/gx_state_contract_tests.cpp)
        target_link_libraries(gx_state_contract_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(gx_state_contract_tests PRIVATE /W4 /permissive-)
            set_property(TARGET gx_state_contract_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME gx_state_contract_tests COMMAND gx_state_contract_tests "${SHADOW_RECOMP_DOL}" "${CMAKE_CURRENT_SOURCE_DIR}/extract/gx-sites.txt")
        set_tests_properties(gx_state_contract_tests PROPERTIES TIMEOUT 600 LABELS "reference;native-contract")
        # The GX display-copy and FIFO-setup leaves (docs/agents/axfx-effects.md, Step H).
        add_executable(gx_display_contract_tests runtime/tests/gx_display_contract_tests.cpp)
        target_link_libraries(gx_display_contract_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(gx_display_contract_tests PRIVATE /W4 /permissive-)
            set_property(TARGET gx_display_contract_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME gx_display_contract_tests COMMAND gx_display_contract_tests "${SHADOW_RECOMP_DOL}" "${CMAKE_CURRENT_SOURCE_DIR}/extract/gx-sites.txt")
        set_tests_properties(gx_display_contract_tests PROPERTIES TIMEOUT 600 LABELS "reference;native-contract")
        # The GX texture-object functions (docs/agents/axfx-effects.md, Step J1).
        add_executable(gx_texobj_contract_tests runtime/tests/gx_texobj_contract_tests.cpp)
        target_link_libraries(gx_texobj_contract_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(gx_texobj_contract_tests PRIVATE /W4 /permissive-)
            set_property(TARGET gx_texobj_contract_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME gx_texobj_contract_tests COMMAND gx_texobj_contract_tests "${SHADOW_RECOMP_DOL}" "${CMAKE_CURRENT_SOURCE_DIR}/extract/gx-sites.txt" "${CMAKE_CURRENT_SOURCE_DIR}/test_data/gx-texobj-corpus")
        set_tests_properties(gx_texobj_contract_tests PROPERTIES TIMEOUT 600 LABELS "reference;native-contract")
        # The RenderWare texture bind 0x8049AC88 (docs/agents/axfx-effects.md, Step L).
        add_executable(rw_texbind_contract_tests runtime/tests/rw_texbind_contract_tests.cpp)
        target_link_libraries(rw_texbind_contract_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(rw_texbind_contract_tests PRIVATE /W4 /permissive-)
            set_property(TARGET rw_texbind_contract_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME rw_texbind_contract_tests COMMAND rw_texbind_contract_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(rw_texbind_contract_tests PROPERTIES TIMEOUT 600 LABELS "reference;native-contract")
        # The GX default region callbacks and the RenderWare raster-render wrappers (docs/agents/sdk-rwdrv.md).
        add_executable(driver_callbacks_contract_tests runtime/tests/driver_callbacks_contract_tests.cpp)
        target_link_libraries(driver_callbacks_contract_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(driver_callbacks_contract_tests PRIVATE /W4 /permissive-)
            set_property(TARGET driver_callbacks_contract_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME driver_callbacks_contract_tests COMMAND driver_callbacks_contract_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(driver_callbacks_contract_tests PROPERTIES TIMEOUT 600 LABELS "reference;native-contract")
        # rwR2 (docs/agents/card-saves.md): the RenderWare raster life cycle's owners against the original DOL words.
        add_executable(rw_raster_contract_tests runtime/tests/rw_raster_contract_tests.cpp)
        target_link_libraries(rw_raster_contract_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(rw_raster_contract_tests PRIVATE /W4 /permissive-)
            set_property(TARGET rw_raster_contract_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME rw_raster_contract_tests COMMAND rw_raster_contract_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(rw_raster_contract_tests PROPERTIES TIMEOUT 1500 LABELS "reference;native-contract")
        add_executable(os_interrupts_contract_tests runtime/tests/os_interrupts_contract_tests.cpp)
        target_link_libraries(os_interrupts_contract_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(os_interrupts_contract_tests PRIVATE /W4 /permissive-)
            set_property(TARGET os_interrupts_contract_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME os_interrupts_contract_tests COMMAND os_interrupts_contract_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(os_interrupts_contract_tests PROPERTIES TIMEOUT 600 LABELS "reference;native-contract")
        add_executable(os_threads_contract_tests runtime/tests/os_threads_contract_tests.cpp)
        target_link_libraries(os_threads_contract_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(os_threads_contract_tests PRIVATE /W4 /permissive-)
            set_property(TARGET os_threads_contract_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME os_threads_contract_tests COMMAND os_threads_contract_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(os_threads_contract_tests PROPERTIES TIMEOUT 120 LABELS "reference;native-contract")
        add_executable(guest_call_tests runtime/tests/guest_call_tests.cpp)
        target_link_libraries(guest_call_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(guest_call_tests PRIVATE /W4 /permissive-)
            set_property(TARGET guest_call_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME guest_call_tests COMMAND guest_call_tests)
        set_tests_properties(guest_call_tests PROPERTIES TIMEOUT 120 LABELS "reference;native-contract")
        add_executable(shoe_jets_tests runtime/tests/shoe_jets_tests.cpp)
        target_link_libraries(shoe_jets_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(shoe_jets_tests PRIVATE /W4 /permissive-)
            set_property(TARGET shoe_jets_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME shoe_jets_tests COMMAND shoe_jets_tests)
        set_tests_properties(shoe_jets_tests PROPERTIES TIMEOUT 120 LABELS "reference;native-contract")
        add_executable(os_settings_contract_tests runtime/tests/os_settings_contract_tests.cpp)
        target_link_libraries(os_settings_contract_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(os_settings_contract_tests PRIVATE /W4 /permissive-)
            set_property(TARGET os_settings_contract_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME os_settings_contract_tests COMMAND os_settings_contract_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(os_settings_contract_tests PROPERTIES TIMEOUT 120 LABELS "reference;native-contract")
        add_executable(os_heap_contract_tests runtime/tests/os_heap_contract_tests.cpp)
        target_link_libraries(os_heap_contract_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(os_heap_contract_tests PRIVATE /W4 /permissive-)
            set_property(TARGET os_heap_contract_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME os_heap_contract_tests COMMAND os_heap_contract_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(os_heap_contract_tests PROPERTIES TIMEOUT 600 LABELS "reference;native-contract")
        add_executable(os_panic_contract_tests runtime/tests/os_panic_contract_tests.cpp)
        target_link_libraries(os_panic_contract_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(os_panic_contract_tests PRIVATE /W4 /permissive-)
            set_property(TARGET os_panic_contract_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME os_panic_contract_tests COMMAND os_panic_contract_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(os_panic_contract_tests PROPERTIES TIMEOUT 600 LABELS "reference;native-contract")
        add_executable(os_misc_contract_tests runtime/tests/os_misc_contract_tests.cpp)
        target_link_libraries(os_misc_contract_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(os_misc_contract_tests PRIVATE /W4 /permissive-)
            set_property(TARGET os_misc_contract_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME os_misc_contract_tests COMMAND os_misc_contract_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(os_misc_contract_tests PROPERTIES TIMEOUT 600 LABELS "reference;native-contract")
        add_executable(os_machine_check_contract_tests runtime/tests/os_machine_check_contract_tests.cpp)
        target_link_libraries(os_machine_check_contract_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(os_machine_check_contract_tests PRIVATE /W4 /permissive-)
            set_property(TARGET os_machine_check_contract_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME os_machine_check_contract_tests COMMAND os_machine_check_contract_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(os_machine_check_contract_tests PROPERTIES TIMEOUT 600 LABELS "reference;native-contract")
        add_executable(os_halt_contract_tests runtime/tests/os_halt_contract_tests.cpp)
        target_link_libraries(os_halt_contract_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(os_halt_contract_tests PRIVATE /W4 /permissive-)
            set_property(TARGET os_halt_contract_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME os_halt_contract_tests COMMAND os_halt_contract_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(os_halt_contract_tests PROPERTIES TIMEOUT 600 LABELS "reference;native-contract")
        add_executable(ax_mix_alloc_contract_tests runtime/tests/ax_mix_alloc_contract_tests.cpp)
        target_link_libraries(ax_mix_alloc_contract_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(ax_mix_alloc_contract_tests PRIVATE /W4 /permissive-)
            set_property(TARGET ax_mix_alloc_contract_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME ax_mix_alloc_contract_tests COMMAND ax_mix_alloc_contract_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(ax_mix_alloc_contract_tests PROPERTIES TIMEOUT 600 LABELS "reference;native-contract")
        add_executable(math_affine_contract_tests runtime/tests/math_affine_contract_tests.cpp)
        target_link_libraries(math_affine_contract_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(math_affine_contract_tests PRIVATE /W4 /permissive-)
            set_property(TARGET math_affine_contract_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME math_affine_contract_tests COMMAND math_affine_contract_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(math_affine_contract_tests PROPERTIES TIMEOUT 600 LABELS "reference;native-contract")
        # The disc's files/ beside the main.dol the tree is configured with (the development tree's ../game is not
        # where a copy of this repository finds it).
        get_filename_component(adx_disc_sys "${SHADOW_RECOMP_DOL}" DIRECTORY)
        get_filename_component(adx_disc "${adx_disc_sys}" DIRECTORY)
        foreach(adx_control "" "--control-shift" "--control-keymask" "--control-table" "--control-frame")
            set(adx_name adx_decode_contract_tests${adx_control})
            if(adx_control STREQUAL "")
                add_executable(adx_decode_contract_tests runtime/tests/adx_decode_contract_tests.cpp)
                target_link_libraries(adx_decode_contract_tests PRIVATE shadow_lift shadow_runtime_reference)
                if(MSVC)
                    target_compile_options(adx_decode_contract_tests PRIVATE /W4 /permissive-)
                    set_property(TARGET adx_decode_contract_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
                endif()
            endif()
            add_test(NAME ${adx_name} COMMAND adx_decode_contract_tests "${SHADOW_RECOMP_DOL}" "${adx_disc}/files" ${adx_control})
            set_tests_properties(${adx_name} PROPERTIES TIMEOUT 900 LABELS "reference;native-contract")
            if(NOT adx_control STREQUAL "")
                set_tests_properties(${adx_name} PROPERTIES WILL_FAIL TRUE)
            endif()
        endforeach()
        # opt-matrix (docs/agents/opt-matrix.md): the 3x4 matrix / vector library owners against the original words, with controls.
        foreach(matrix_control "" "--control-concat" "--control-invert" "--control-copy" "--control-point" "--control-rotate"
                "--control-length" "--control-translate-floats" "--control-scale-floats" "--control-frame" "--control-ps1")
            set(matrix_name math_matrix_contract_tests${matrix_control})
            if(matrix_control STREQUAL "")
                add_executable(math_matrix_contract_tests runtime/tests/math_matrix_contract_tests.cpp)
                target_link_libraries(math_matrix_contract_tests PRIVATE shadow_lift shadow_runtime_reference)
                if(MSVC)
                    target_compile_options(math_matrix_contract_tests PRIVATE /W4 /permissive-)
                    set_property(TARGET math_matrix_contract_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
                endif()
            endif()
            add_test(NAME ${matrix_name} COMMAND math_matrix_contract_tests "${SHADOW_RECOMP_DOL}" ${matrix_control})
            set_tests_properties(${matrix_name} PROPERTIES TIMEOUT 900 LABELS "reference;native-contract")
            if(NOT matrix_control STREQUAL "")
                set_tests_properties(${matrix_name} PROPERTIES WILL_FAIL TRUE)
            endif()
        endforeach()
        # docs/hal/HAL_DETACHMENT.md section 4 step 1: the SDK's PSMTXConcat owner against the original words, with controls.
        foreach(mtx_control "" "--control-quirk" "--control-order" "--control-frame")
            set(mtx_name math_mtx_contract_tests${mtx_control})
            if(mtx_control STREQUAL "")
                add_executable(math_mtx_contract_tests runtime/tests/math_mtx_contract_tests.cpp)
                target_link_libraries(math_mtx_contract_tests PRIVATE shadow_lift shadow_runtime_reference)
                if(MSVC)
                    target_compile_options(math_mtx_contract_tests PRIVATE /W4 /permissive-)
                    set_property(TARGET math_mtx_contract_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
                endif()
            endif()
            add_test(NAME ${mtx_name} COMMAND math_mtx_contract_tests "${SHADOW_RECOMP_DOL}" ${mtx_control})
            set_tests_properties(${mtx_name} PROPERTIES TIMEOUT 900 LABELS "reference;native-contract")
            if(NOT mtx_control STREQUAL "")
                set_tests_properties(${mtx_name} PROPERTIES WILL_FAIL TRUE)
            endif()
        endforeach()
        # native/hal-alloc: the native RwMalloc / RwFree chain against the original words over the image's owners, with controls.
        foreach(rw_alloc_control "" "--control-size" "--control-stack" "--control-result")
            set(rw_alloc_name rw_alloc_contract_tests${rw_alloc_control})
            if(rw_alloc_control STREQUAL "")
                add_executable(rw_alloc_contract_tests runtime/tests/rw_alloc_contract_tests.cpp)
                target_link_libraries(rw_alloc_contract_tests PRIVATE shadow_lift shadow_runtime_reference)
                if(MSVC)
                    target_compile_options(rw_alloc_contract_tests PRIVATE /W4 /permissive-)
                    set_property(TARGET rw_alloc_contract_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
                endif()
            endif()
            add_test(NAME ${rw_alloc_name} COMMAND rw_alloc_contract_tests "${SHADOW_RECOMP_DOL}" ${rw_alloc_control})
            set_tests_properties(${rw_alloc_name} PROPERTIES TIMEOUT 900 LABELS "reference;native-contract")
            if(NOT rw_alloc_control STREQUAL "")
                set_tests_properties(${rw_alloc_name} PROPERTIES WILL_FAIL TRUE)
            endif()
        endforeach()
        # SubQ batch B003: the native motion bank store against the original words of its 15 functions, with controls.
        foreach(motion_banks_control "" "--control-quirk" "--control-order")
            set(motion_banks_name motion_bank_store_contract_tests${motion_banks_control})
            if(motion_banks_control STREQUAL "")
                add_executable(motion_bank_store_contract_tests runtime/tests/motion_bank_store_contract_tests.cpp)
                target_link_libraries(motion_bank_store_contract_tests PRIVATE shadow_lift shadow_runtime_reference)
                if(MSVC)
                    target_compile_options(motion_bank_store_contract_tests PRIVATE /W4 /permissive-)
                    set_property(TARGET motion_bank_store_contract_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
                endif()
            endif()
            add_test(NAME ${motion_banks_name} COMMAND motion_bank_store_contract_tests "${SHADOW_RECOMP_DOL}" ${motion_banks_control})
            set_tests_properties(${motion_banks_name} PROPERTIES TIMEOUT 1800 LABELS "reference;native-contract")
            if(NOT motion_banks_control STREQUAL "")
                set_tests_properties(${motion_banks_name} PROPERTIES WILL_FAIL TRUE)
            endif()
        endforeach()
        add_executable(im2d_prims_tests runtime/tests/im2d_prims_tests.cpp runtime/scaffold/rw_driver/im2d_prims.cpp)
        target_include_directories(im2d_prims_tests PRIVATE runtime/scaffold)
        target_link_libraries(im2d_prims_tests PRIVATE shadow_lift shadow_runtime_reference shadow_rwim)
        if(MSVC)
            target_compile_options(im2d_prims_tests PRIVATE /W4 /permissive-)
            set_property(TARGET im2d_prims_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME im2d_prims_tests COMMAND im2d_prims_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(im2d_prims_tests PROPERTIES TIMEOUT 120 LABELS "reference;native-contract")
        add_executable(raster_render_contract_tests runtime/tests/raster_render_contract_tests.cpp runtime/scaffold/rw_driver/raster_render.cpp)
        target_include_directories(raster_render_contract_tests PRIVATE runtime/scaffold)
        target_link_libraries(raster_render_contract_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(raster_render_contract_tests PRIVATE /W4 /permissive-)
            set_property(TARGET raster_render_contract_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME raster_render_contract_tests COMMAND raster_render_contract_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(raster_render_contract_tests PROPERTIES TIMEOUT 120 LABELS "reference;native-contract")
        add_executable(rw_pixel_contract_tests runtime/tests/rw_pixel_contract_tests.cpp)
        target_link_libraries(rw_pixel_contract_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(rw_pixel_contract_tests PRIVATE /W4 /permissive-)
            set_property(TARGET rw_pixel_contract_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME rw_pixel_contract_tests COMMAND rw_pixel_contract_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(rw_pixel_contract_tests PROPERTIES TIMEOUT 120 LABELS "reference;native-contract")
        add_executable(rw_image_contract_tests runtime/tests/rw_image_contract_tests.cpp)
        target_link_libraries(rw_image_contract_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(rw_image_contract_tests PRIVATE /W4 /permissive-)
            set_property(TARGET rw_image_contract_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME rw_image_contract_tests COMMAND rw_image_contract_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(rw_image_contract_tests PROPERTIES TIMEOUT 300 LABELS "reference;native-contract")
        # audio-ax step 1 (docs/agents/audio-ax.md): the AX registration owners against the original DOL
        add_executable(ax_registration_contract_tests runtime/tests/ax_registration_contract_tests.cpp)
        target_link_libraries(ax_registration_contract_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(ax_registration_contract_tests PRIVATE /W4 /permissive-)
            set_property(TARGET ax_registration_contract_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME ax_registration_contract_tests COMMAND ax_registration_contract_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(ax_registration_contract_tests PROPERTIES TIMEOUT 120 LABELS "reference;native-contract")
        # audio-ax stage c: the AX voice setter owners against the original DOL
        add_executable(ax_voice_contract_tests runtime/tests/ax_voice_contract_tests.cpp)
        target_link_libraries(ax_voice_contract_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(ax_voice_contract_tests PRIVATE /W4 /permissive-)
            set_property(TARGET ax_voice_contract_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME ax_voice_contract_tests COMMAND ax_voice_contract_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(ax_voice_contract_tests PROPERTIES TIMEOUT 120 LABELS "reference;native-contract")
        # audio-ax stage e step 1: the native AX sync against the original DOL
        add_executable(ax_sync_contract_tests runtime/tests/ax_sync_contract_tests.cpp)
        target_link_libraries(ax_sync_contract_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(ax_sync_contract_tests PRIVATE /W4 /permissive-)
            set_property(TARGET ax_sync_contract_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME ax_sync_contract_tests COMMAND ax_sync_contract_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(ax_sync_contract_tests PROPERTIES TIMEOUT 600 LABELS "reference;native-contract")
        add_executable(ax_frame_contract_tests runtime/tests/ax_frame_contract_tests.cpp)
        target_link_libraries(ax_frame_contract_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(ax_frame_contract_tests PRIVATE /W4 /permissive-)
            set_property(TARGET ax_frame_contract_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME ax_frame_contract_tests COMMAND ax_frame_contract_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(ax_frame_contract_tests PROPERTIES TIMEOUT 300 LABELS "reference;native-contract")
        add_executable(ax_out_contract_tests runtime/tests/ax_out_contract_tests.cpp)
        target_link_libraries(ax_out_contract_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(ax_out_contract_tests PRIVATE /W4 /permissive-)
            set_property(TARGET ax_out_contract_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME ax_out_contract_tests COMMAND ax_out_contract_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(ax_out_contract_tests PROPERTIES TIMEOUT 600 LABELS "reference;native-contract")
        add_executable(os_font_contract_tests runtime/tests/os_font_contract_tests.cpp)
        target_link_libraries(os_font_contract_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(os_font_contract_tests PRIVATE /W4 /permissive-)
            set_property(TARGET os_font_contract_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME os_font_contract_tests COMMAND os_font_contract_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(os_font_contract_tests PROPERTIES TIMEOUT 120 LABELS "reference;native-contract")
        add_executable(vec_math_contract_tests runtime/tests/vec_math_contract_tests.cpp)
        target_link_libraries(vec_math_contract_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(vec_math_contract_tests PRIVATE /W4 /permissive-)
            set_property(TARGET vec_math_contract_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME vec_math_contract_tests COMMAND vec_math_contract_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(vec_math_contract_tests PROPERTIES TIMEOUT 600 LABELS "reference;native-contract")
        add_executable(mtn_track_contract_tests runtime/tests/mtn_track_contract_tests.cpp)
        target_link_libraries(mtn_track_contract_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(mtn_track_contract_tests PRIVATE /W4 /permissive-)
            set_property(TARGET mtn_track_contract_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME mtn_track_contract_tests COMMAND mtn_track_contract_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(mtn_track_contract_tests PROPERTIES TIMEOUT 1800 LABELS "reference;native-contract")
        # The test compiles the owner file itself with the test hook (a loop-iteration cap in phase C's wrap loops); the game's copy in the runtime
        # library is built without it, and the linker takes the executable's definitions first, so the hook never reaches the game target.
        add_executable(mtn_pose_contract_tests runtime/tests/mtn_pose_contract_tests.cpp runtime/src/gekko/native_mtn_pose.cpp)
        target_compile_definitions(mtn_pose_contract_tests PRIVATE GEKKO_MTN_TEST_HOOKS=1)
        target_link_libraries(mtn_pose_contract_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(mtn_pose_contract_tests PRIVATE /W4 /permissive-)
            set_property(TARGET mtn_pose_contract_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME mtn_pose_contract_tests COMMAND mtn_pose_contract_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(mtn_pose_contract_tests PROPERTIES TIMEOUT 7200 LABELS "reference;native-contract")
        # s1/core
        # cpu/opt5 (revived s2/sound owner): the GCAX dB -> amplitude search against the original words, with negative controls
        foreach(ampdb_control "" "--control-offbyone" "--control-nanmatch" "--control-table" "--control-model")
            set(ampdb_name sound_ampdb_contract_tests${ampdb_control})
            if(ampdb_control STREQUAL "")
                add_executable(sound_ampdb_contract_tests runtime/tests/sound_ampdb_contract_tests.cpp)
                target_link_libraries(sound_ampdb_contract_tests PRIVATE shadow_lift shadow_runtime_reference)
                if(MSVC)
                    target_compile_options(sound_ampdb_contract_tests PRIVATE /W4 /permissive-)
                    set_property(TARGET sound_ampdb_contract_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
                endif()
            endif()
            add_test(NAME ${ampdb_name} COMMAND sound_ampdb_contract_tests "${SHADOW_RECOMP_DOL}" ${ampdb_control})
            set_tests_properties(${ampdb_name} PROPERTIES TIMEOUT 900 LABELS "reference;native-contract")
            if(NOT ampdb_control STREQUAL "")
                set_tests_properties(${ampdb_name} PROPERTIES WILL_FAIL TRUE)
            endif()
        endforeach()
        add_executable(s1_core_layout_scan_tests runtime/tests/s1_core_layout_scan_tests.cpp)
        target_link_libraries(s1_core_layout_scan_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(s1_core_layout_scan_tests PRIVATE /W4 /permissive-)
            set_property(TARGET s1_core_layout_scan_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME s1_core_layout_scan_tests COMMAND s1_core_layout_scan_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(s1_core_layout_scan_tests PROPERTIES TIMEOUT 1800 LABELS "reference;native-contract")
        # s1/target
        add_executable(s1_target2_contract_tests runtime/tests/s1_target2_contract_tests.cpp)
        target_link_libraries(s1_target2_contract_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(s1_target2_contract_tests PRIVATE /W4 /permissive-)
            set_property(TARGET s1_target2_contract_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME s1_target2_contract_tests COMMAND s1_target2_contract_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(s1_target2_contract_tests PROPERTIES TIMEOUT 1800 LABELS "reference;native-contract")
        add_executable(s1_core_shptr_tests runtime/tests/s1_core_shptr_tests.cpp)
        target_link_libraries(s1_core_shptr_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(s1_core_shptr_tests PRIVATE /W4 /permissive-)
            set_property(TARGET s1_core_shptr_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME s1_core_shptr_tests COMMAND s1_core_shptr_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(s1_core_shptr_tests PROPERTIES TIMEOUT 1800 LABELS "reference;native-contract")
        add_executable(s1_core_chars_tests runtime/tests/s1_core_chars_tests.cpp)
        target_link_libraries(s1_core_chars_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(s1_core_chars_tests PRIVATE /W4 /permissive-)
            set_property(TARGET s1_core_chars_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME s1_core_chars_tests COMMAND s1_core_chars_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(s1_core_chars_tests PROPERTIES TIMEOUT 1800 LABELS "reference;native-contract")
        add_executable(effect_update_contract_tests runtime/tests/effect_update_contract_tests.cpp)
        target_link_libraries(effect_update_contract_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(effect_update_contract_tests PRIVATE /W4 /permissive-)
            set_property(TARGET effect_update_contract_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME effect_update_contract_tests COMMAND effect_update_contract_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(effect_update_contract_tests PROPERTIES TIMEOUT 1800 LABELS "reference;native-contract")
        add_executable(mix_entries_contract_tests runtime/tests/mix_entries_contract_tests.cpp)
        target_link_libraries(mix_entries_contract_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(mix_entries_contract_tests PRIVATE /W4 /permissive-)
            set_property(TARGET mix_entries_contract_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME mix_entries_contract_tests COMMAND mix_entries_contract_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(mix_entries_contract_tests PROPERTIES TIMEOUT 120 LABELS "reference;native-contract")
        add_executable(sound_table_contract_tests runtime/tests/sound_table_contract_tests.cpp)
        target_link_libraries(sound_table_contract_tests PRIVATE shadow_runtime_reference)
        if(MSVC)
            target_compile_options(sound_table_contract_tests PRIVATE /W4 /permissive-)
            set_property(TARGET sound_table_contract_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        get_filename_component(sound_table_sys "${SHADOW_RECOMP_DOL}" DIRECTORY)
        get_filename_component(sound_table_game "${sound_table_sys}" DIRECTORY)
        add_test(NAME sound_table_contract_tests
                 COMMAND sound_table_contract_tests "${SHADOW_RECOMP_DOL}" "${sound_table_game}/files/sound")
        set_tests_properties(sound_table_contract_tests PROPERTIES TIMEOUT 120 LABELS "reference;native-contract")
        add_executable(gcax_midi_contract_tests runtime/tests/gcax_midi_contract_tests.cpp)
        target_link_libraries(gcax_midi_contract_tests PRIVATE shadow_runtime_reference)
        if(MSVC)
            target_compile_options(gcax_midi_contract_tests PRIVATE /W4 /permissive-)
            set_property(TARGET gcax_midi_contract_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME gcax_midi_contract_tests COMMAND gcax_midi_contract_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(gcax_midi_contract_tests PROPERTIES TIMEOUT 300 LABELS "reference;native-contract")
        add_executable(se_math_contract_tests runtime/tests/se_math_contract_tests.cpp)
        target_link_libraries(se_math_contract_tests PRIVATE shadow_runtime_reference)
        if(MSVC)
            target_compile_options(se_math_contract_tests PRIVATE /W4 /permissive-)
            set_property(TARGET se_math_contract_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME se_math_contract_tests COMMAND se_math_contract_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(se_math_contract_tests PROPERTIES TIMEOUT 300 LABELS "reference;native-contract")
        add_executable(se_manager_contract_tests runtime/tests/se_manager_contract_tests.cpp)
        target_link_libraries(se_manager_contract_tests PRIVATE shadow_runtime_reference)
        if(MSVC)
            target_compile_options(se_manager_contract_tests PRIVATE /W4 /permissive-)
            set_property(TARGET se_manager_contract_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        # A MEM1 image of the original game at the start of Lethal Highway (Dolphin, retail disc).
        set(se_manager_image "${SHADOW_PRIVATE_TEST_DATA}/se-manager/lethal-highway-mem1.raw")
        add_test(NAME se_manager_contract_tests
                 COMMAND se_manager_contract_tests "${SHADOW_RECOMP_DOL}" "${sound_table_game}/files/sound"
                         "${se_manager_image}")
        set_tests_properties(se_manager_contract_tests PROPERTIES TIMEOUT 600 LABELS "reference;native-contract")
        # C++ reconstruction of leaf functions (tools/dolrecon/lift_cpp.py) against the original words
        # (docs/DOLRECON.md "C++ reconstruction").
        file(GLOB dolrecon_lifted_sources CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_SOURCE_DIR}/runtime/tests/generated/dolrecon_lifted_*.cpp")
        if(dolrecon_lifted_sources)
        add_executable(dolrecon_lift_tests runtime/tests/dolrecon_lift_tests.cpp ${dolrecon_lifted_sources})
        target_include_directories(dolrecon_lift_tests PRIVATE runtime/tests runtime/tests/generated)
        target_link_libraries(dolrecon_lift_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(dolrecon_lift_tests PRIVATE /W4 /permissive- /bigobj)
            set_property(TARGET dolrecon_lift_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME dolrecon_lift_tests COMMAND dolrecon_lift_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(dolrecon_lift_tests PROPERTIES TIMEOUT 1800 LABELS "reference;native-contract")
        endif()
        # The native MSL math library against the original words (docs/NATIVE_MSL_MATH.md).
        add_executable(native_msl_tests runtime/tests/native_msl_tests.cpp)
        target_link_libraries(native_msl_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(native_msl_tests PRIVATE /W4 /permissive-)
            set_property(TARGET native_msl_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME native_msl_tests COMMAND native_msl_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(native_msl_tests PROPERTIES TIMEOUT 600 LABELS "reference;native-contract")
        add_executable(native_matrix_copy_tests runtime/tests/native_matrix_copy_tests.cpp)
        target_link_libraries(native_matrix_copy_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(native_matrix_copy_tests PRIVATE /W4 /permissive-)
            set_property(TARGET native_matrix_copy_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME native_matrix_copy_tests COMMAND native_matrix_copy_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(native_matrix_copy_tests PROPERTIES TIMEOUT 120 LABELS "reference;native-contract")
        add_executable(native_hermite_tests runtime/tests/native_hermite_tests.cpp)
        target_link_libraries(native_hermite_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(native_hermite_tests PRIVATE /W4 /permissive- ${SHADOW_ARCH_FLAGS})
            set_property(TARGET native_hermite_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME native_hermite_tests COMMAND native_hermite_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(native_hermite_tests PROPERTIES TIMEOUT 120 LABELS "reference;native-contract")
        # The native RenderWare core against the original RenderWare instructions (rwcore/README.md).
        add_executable(rwcore_matrix_tests runtime/tests/rwcore_matrix_tests.cpp)
        target_link_libraries(rwcore_matrix_tests PRIVATE rwcore shadow_runtime_reference)
        if(MSVC)
            target_compile_options(rwcore_matrix_tests PRIVATE /W4 /permissive-)
            set_property(TARGET rwcore_matrix_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME rwcore_matrix_tests COMMAND rwcore_matrix_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(rwcore_matrix_tests PROPERTIES TIMEOUT 300 LABELS "reference;native-contract")
        add_executable(rwcore_vector_tests runtime/tests/rwcore_vector_tests.cpp)
        target_link_libraries(rwcore_vector_tests PRIVATE rwcore shadow_runtime_reference)
        if(MSVC)
            target_compile_options(rwcore_vector_tests PRIVATE /W4 /permissive-)
            set_property(TARGET rwcore_vector_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME rwcore_vector_tests COMMAND rwcore_vector_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(rwcore_vector_tests PROPERTIES TIMEOUT 600 LABELS "reference;native-contract")
        add_executable(rwcore_memory_tests runtime/tests/rwcore_memory_tests.cpp)
        target_link_libraries(rwcore_memory_tests PRIVATE rwcore shadow_runtime_reference)
        if(MSVC)
            target_compile_options(rwcore_memory_tests PRIVATE /W4 /permissive-)
            set_property(TARGET rwcore_memory_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME rwcore_memory_tests COMMAND rwcore_memory_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(rwcore_memory_tests PROPERTIES TIMEOUT 600 LABELS "reference;native-contract")
        add_executable(rwcore_plugin_tests runtime/tests/rwcore_plugin_tests.cpp)
        target_link_libraries(rwcore_plugin_tests PRIVATE rwcore shadow_runtime_reference)
        if(MSVC)
            target_compile_options(rwcore_plugin_tests PRIVATE /W4 /permissive-)
            set_property(TARGET rwcore_plugin_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME rwcore_plugin_tests COMMAND rwcore_plugin_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(rwcore_plugin_tests PROPERTIES TIMEOUT 600 LABELS "reference;native-contract")
        add_executable(rwcore_stream_tests runtime/tests/rwcore_stream_tests.cpp)
        target_link_libraries(rwcore_stream_tests PRIVATE rwcore shadow_runtime_reference)
        if(MSVC)
            target_compile_options(rwcore_stream_tests PRIVATE /W4 /permissive-)
            set_property(TARGET rwcore_stream_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME rwcore_stream_tests COMMAND rwcore_stream_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(rwcore_stream_tests PROPERTIES TIMEOUT 600 LABELS "reference;native-contract")
        add_executable(rwcore_binary_tests runtime/tests/rwcore_binary_tests.cpp)
        target_link_libraries(rwcore_binary_tests PRIVATE rwcore shadow_runtime_reference)
        if(MSVC)
            target_compile_options(rwcore_binary_tests PRIVATE /W4 /permissive-)
            set_property(TARGET rwcore_binary_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME rwcore_binary_tests COMMAND rwcore_binary_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(rwcore_binary_tests PROPERTIES TIMEOUT 600 LABELS "reference;native-contract")
        add_executable(rwcore_frame_tests runtime/tests/rwcore_frame_tests.cpp)
        target_link_libraries(rwcore_frame_tests PRIVATE rwcore shadow_runtime_reference)
        if(MSVC)
            target_compile_options(rwcore_frame_tests PRIVATE /W4 /permissive-)
            set_property(TARGET rwcore_frame_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME rwcore_frame_tests COMMAND rwcore_frame_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(rwcore_frame_tests PROPERTIES TIMEOUT 600 LABELS "reference;native-contract")
        add_executable(rwcore_texture_tests runtime/tests/rwcore_texture_tests.cpp)
        target_link_libraries(rwcore_texture_tests PRIVATE rwcore shadow_runtime_reference)
        if(MSVC)
            target_compile_options(rwcore_texture_tests PRIVATE /W4 /permissive-)
            set_property(TARGET rwcore_texture_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME rwcore_texture_tests COMMAND rwcore_texture_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(rwcore_texture_tests PROPERTIES TIMEOUT 600 LABELS "reference;native-contract")
        add_executable(rwcore_image_tests runtime/tests/rwcore_image_tests.cpp)
        target_link_libraries(rwcore_image_tests PRIVATE rwcore shadow_runtime_reference)
        if(MSVC)
            target_compile_options(rwcore_image_tests PRIVATE /W4 /permissive-)
            set_property(TARGET rwcore_image_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME rwcore_image_tests COMMAND rwcore_image_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(rwcore_image_tests PROPERTIES TIMEOUT 600 LABELS "reference;native-contract")
        add_executable(rwcore_resources_tests runtime/tests/rwcore_resources_tests.cpp)
        target_link_libraries(rwcore_resources_tests PRIVATE rwcore shadow_runtime_reference)
        if(MSVC)
            target_compile_options(rwcore_resources_tests PRIVATE /W4 /permissive-)
            set_property(TARGET rwcore_resources_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME rwcore_resources_tests COMMAND rwcore_resources_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(rwcore_resources_tests PROPERTIES TIMEOUT 600 LABELS "reference;native-contract")
        add_executable(rwcore_resample_tests runtime/tests/rwcore_resample_tests.cpp)
        target_link_libraries(rwcore_resample_tests PRIVATE rwcore shadow_runtime_reference)
        if(MSVC)
            target_compile_options(rwcore_resample_tests PRIVATE /W4 /permissive-)
            set_property(TARGET rwcore_resample_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME rwcore_resample_tests COMMAND rwcore_resample_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(rwcore_resample_tests PROPERTIES TIMEOUT 600 LABELS "reference;native-contract")
        add_executable(rwcore_palquant_tests runtime/tests/rwcore_palquant_tests.cpp)
        target_link_libraries(rwcore_palquant_tests PRIVATE rwcore shadow_runtime_reference)
        if(MSVC)
            target_compile_options(rwcore_palquant_tests PRIVATE /W4 /permissive-)
            set_property(TARGET rwcore_palquant_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME rwcore_palquant_tests COMMAND rwcore_palquant_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(rwcore_palquant_tests PROPERTIES TIMEOUT 900 LABELS "reference;native-contract")
        add_executable(rpworld_material_tests runtime/tests/rpworld_material_tests.cpp)
        target_link_libraries(rpworld_material_tests PRIVATE rpworld shadow_runtime_reference)
        if(MSVC)
            target_compile_options(rpworld_material_tests PRIVATE /W4 /permissive-)
            set_property(TARGET rpworld_material_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME rpworld_material_tests COMMAND rpworld_material_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(rpworld_material_tests PROPERTIES TIMEOUT 600 LABELS "reference;native-contract")
        add_executable(rpworld_light_tests runtime/tests/rpworld_light_tests.cpp)
        target_link_libraries(rpworld_light_tests PRIVATE rpworld shadow_runtime_reference)
        if(MSVC)
            target_compile_options(rpworld_light_tests PRIVATE /W4 /permissive-)
            set_property(TARGET rpworld_light_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME rpworld_light_tests COMMAND rpworld_light_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(rpworld_light_tests PROPERTIES TIMEOUT 600 LABELS "reference;native-contract")
        add_executable(rpworld_world_tests runtime/tests/rpworld_world_tests.cpp)
        target_link_libraries(rpworld_world_tests PRIVATE rpworld shadow_runtime_reference)
        if(MSVC)
            target_compile_options(rpworld_world_tests PRIVATE /W4 /permissive-)
            set_property(TARGET rpworld_world_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME rpworld_world_tests COMMAND rpworld_world_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(rpworld_world_tests PROPERTIES TIMEOUT 600 LABELS "reference;native-contract")
        add_executable(rwcore_camera_tests runtime/tests/rwcore_camera_tests.cpp)
        target_link_libraries(rwcore_camera_tests PRIVATE rwcore shadow_runtime_reference)
        if(MSVC)
            target_compile_options(rwcore_camera_tests PRIVATE /W4 /permissive-)
            set_property(TARGET rwcore_camera_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME rwcore_camera_tests COMMAND rwcore_camera_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(rwcore_camera_tests PROPERTIES TIMEOUT 600 LABELS "reference;native-contract")
        add_executable(rwcore_pluginstream_tests runtime/tests/rwcore_pluginstream_tests.cpp)
        target_link_libraries(rwcore_pluginstream_tests PRIVATE rwcore shadow_runtime_reference)
        if(MSVC)
            target_compile_options(rwcore_pluginstream_tests PRIVATE /W4 /permissive-)
            set_property(TARGET rwcore_pluginstream_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME rwcore_pluginstream_tests COMMAND rwcore_pluginstream_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(rwcore_pluginstream_tests PROPERTIES TIMEOUT 600 LABELS "reference;native-contract")
        add_executable(rwcore_list_tests runtime/tests/rwcore_list_tests.cpp)
        target_link_libraries(rwcore_list_tests PRIVATE rwcore shadow_runtime_reference)
        if(MSVC)
            target_compile_options(rwcore_list_tests PRIVATE /W4 /permissive-)
            set_property(TARGET rwcore_list_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME rwcore_list_tests COMMAND rwcore_list_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(rwcore_list_tests PROPERTIES TIMEOUT 600 LABELS "reference;native-contract")
        add_executable(rwcore_lifecycle_tests runtime/tests/rwcore_lifecycle_tests.cpp)
        target_link_libraries(rwcore_lifecycle_tests PRIVATE rwcore)
        if(MSVC)
            target_compile_options(rwcore_lifecycle_tests PRIVATE /W4 /permissive-)
            set_property(TARGET rwcore_lifecycle_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME rwcore_lifecycle_tests COMMAND rwcore_lifecycle_tests)
        set_tests_properties(rwcore_lifecycle_tests PROPERTIES TIMEOUT 600 LABELS "native-contract")
        add_executable(rwcore_frame_bench runtime/tests/rwcore_frame_bench.cpp)
        target_link_libraries(rwcore_frame_bench PRIVATE rwcore)
        if(MSVC)
            target_compile_options(rwcore_frame_bench PRIVATE /W4 /permissive-)
            set_property(TARGET rwcore_frame_bench PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_executable(rwcore_camera_bench runtime/tests/rwcore_camera_bench.cpp)
        target_link_libraries(rwcore_camera_bench PRIVATE rwcore)
        if(MSVC)
            target_compile_options(rwcore_camera_bench PRIVATE /W4 /permissive-)
            set_property(TARGET rwcore_camera_bench PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_executable(rwcore_frame_native_tests runtime/tests/rwcore_frame_native_tests.cpp)
        target_link_libraries(rwcore_frame_native_tests PRIVATE rwcore)
        if(MSVC)
            target_compile_options(rwcore_frame_native_tests PRIVATE /W4 /permissive-)
            set_property(TARGET rwcore_frame_native_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME rwcore_frame_native_tests COMMAND rwcore_frame_native_tests)
        set_tests_properties(rwcore_frame_native_tests PROPERTIES TIMEOUT 600 LABELS "native-contract")
        add_executable(rpworld_material_native_tests runtime/tests/rpworld_material_native_tests.cpp)
        target_link_libraries(rpworld_material_native_tests PRIVATE rpworld)
        if(MSVC)
            target_compile_options(rpworld_material_native_tests PRIVATE /W4 /permissive-)
            set_property(TARGET rpworld_material_native_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME rpworld_material_native_tests COMMAND rpworld_material_native_tests)
        set_tests_properties(rpworld_material_native_tests PROPERTIES TIMEOUT 600 LABELS "native-contract")
        add_executable(rpworld_light_native_tests runtime/tests/rpworld_light_native_tests.cpp)
        target_link_libraries(rpworld_light_native_tests PRIVATE rpworld)
        if(MSVC)
            target_compile_options(rpworld_light_native_tests PRIVATE /W4 /permissive-)
            set_property(TARGET rpworld_light_native_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME rpworld_light_native_tests COMMAND rpworld_light_native_tests)
        set_tests_properties(rpworld_light_native_tests PROPERTIES TIMEOUT 600 LABELS "native-contract")
        add_executable(rpworld_world_native_tests runtime/tests/rpworld_world_native_tests.cpp)
        target_link_libraries(rpworld_world_native_tests PRIVATE rpworld)
        if(MSVC)
            target_compile_options(rpworld_world_native_tests PRIVATE /W4 /permissive-)
            set_property(TARGET rpworld_world_native_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME rpworld_world_native_tests COMMAND rpworld_world_native_tests)
        set_tests_properties(rpworld_world_native_tests PROPERTIES TIMEOUT 600 LABELS "native-contract")
        add_executable(rwcore_texture_native_tests runtime/tests/rwcore_texture_native_tests.cpp)
        target_link_libraries(rwcore_texture_native_tests PRIVATE rwcore)
        if(MSVC)
            target_compile_options(rwcore_texture_native_tests PRIVATE /W4 /permissive-)
            set_property(TARGET rwcore_texture_native_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME rwcore_texture_native_tests COMMAND rwcore_texture_native_tests)
        set_tests_properties(rwcore_texture_native_tests PROPERTIES TIMEOUT 600 LABELS "native-contract")
        add_executable(rwcore_image_native_tests runtime/tests/rwcore_image_native_tests.cpp)
        target_link_libraries(rwcore_image_native_tests PRIVATE rwcore)
        if(MSVC)
            target_compile_options(rwcore_image_native_tests PRIVATE /W4 /permissive-)
            set_property(TARGET rwcore_image_native_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME rwcore_image_native_tests COMMAND rwcore_image_native_tests)
        set_tests_properties(rwcore_image_native_tests PROPERTIES TIMEOUT 600 LABELS "native-contract")
        add_executable(rwcore_resources_native_tests runtime/tests/rwcore_resources_native_tests.cpp)
        target_link_libraries(rwcore_resources_native_tests PRIVATE rwcore)
        if(MSVC)
            target_compile_options(rwcore_resources_native_tests PRIVATE /W4 /permissive-)
            set_property(TARGET rwcore_resources_native_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME rwcore_resources_native_tests COMMAND rwcore_resources_native_tests)
        set_tests_properties(rwcore_resources_native_tests PROPERTIES TIMEOUT 600 LABELS "native-contract")
        add_executable(rwcore_resample_native_tests runtime/tests/rwcore_resample_native_tests.cpp)
        target_link_libraries(rwcore_resample_native_tests PRIVATE rwcore)
        if(MSVC)
            target_compile_options(rwcore_resample_native_tests PRIVATE /W4 /permissive-)
            set_property(TARGET rwcore_resample_native_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME rwcore_resample_native_tests COMMAND rwcore_resample_native_tests)
        set_tests_properties(rwcore_resample_native_tests PROPERTIES TIMEOUT 600 LABELS "native-contract")
        add_executable(rwcore_palquant_native_tests runtime/tests/rwcore_palquant_native_tests.cpp)
        target_link_libraries(rwcore_palquant_native_tests PRIVATE rwcore)
        if(MSVC)
            target_compile_options(rwcore_palquant_native_tests PRIVATE /W4 /permissive-)
            set_property(TARGET rwcore_palquant_native_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME rwcore_palquant_native_tests COMMAND rwcore_palquant_native_tests)
        set_tests_properties(rwcore_palquant_native_tests PROPERTIES TIMEOUT 600 LABELS "native-contract")
        add_executable(rwcore_camera_native_tests runtime/tests/rwcore_camera_native_tests.cpp)
        target_link_libraries(rwcore_camera_native_tests PRIVATE rwcore)
        if(MSVC)
            target_compile_options(rwcore_camera_native_tests PRIVATE /W4 /permissive-)
            set_property(TARGET rwcore_camera_native_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME rwcore_camera_native_tests COMMAND rwcore_camera_native_tests)
        set_tests_properties(rwcore_camera_native_tests PROPERTIES TIMEOUT 600 LABELS "native-contract")
        add_executable(rwcore_limits_tests runtime/tests/rwcore_limits_tests.cpp)
        target_link_libraries(rwcore_limits_tests PRIVATE rwcore)
        if(MSVC)
            target_compile_options(rwcore_limits_tests PRIVATE /W4 /permissive-)
            set_property(TARGET rwcore_limits_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME rwcore_limits_tests COMMAND rwcore_limits_tests "${CMAKE_BINARY_DIR}")
        set_tests_properties(rwcore_limits_tests PROPERTIES TIMEOUT 600 LABELS "native-contract")
        add_executable(rwcore_math_tests runtime/tests/rwcore_math_tests.cpp)
        target_link_libraries(rwcore_math_tests PRIVATE rwcore)
        if(MSVC)
            target_compile_options(rwcore_math_tests PRIVATE /W4 /permissive-)
            set_property(TARGET rwcore_math_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME rwcore_math_tests COMMAND rwcore_math_tests)
        set_tests_properties(rwcore_math_tests PROPERTIES TIMEOUT 600 LABELS "native-contract")
        add_executable(rwcore_fp_digest_tests runtime/tests/rwcore_fp_digest_tests.cpp)
        target_link_libraries(rwcore_fp_digest_tests PRIVATE rwcore)
        if(MSVC)
            target_compile_options(rwcore_fp_digest_tests PRIVATE /W4 /permissive-)
            set_property(TARGET rwcore_fp_digest_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME rwcore_fp_digest_tests COMMAND rwcore_fp_digest_tests)
        set_tests_properties(rwcore_fp_digest_tests PROPERTIES TIMEOUT 600 LABELS "native-contract")
        add_executable(native_matrix_compose_tests runtime/tests/native_matrix_compose_tests.cpp)
        target_link_libraries(native_matrix_compose_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(native_matrix_compose_tests PRIVATE /W4 /permissive-)
            set_property(TARGET native_matrix_compose_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME native_matrix_compose_tests COMMAND native_matrix_compose_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(native_matrix_compose_tests PROPERTIES TIMEOUT 120 LABELS "reference;native-contract")
        add_executable(native_matrix_call_tests runtime/tests/native_matrix_call_tests.cpp)
        target_include_directories(native_matrix_call_tests PRIVATE "${SHADOW_RECOMP_DIR}")
        target_link_libraries(native_matrix_call_tests PRIVATE shadow_lift shadow_recomp shadow_runtime)
        if(MSVC)
            target_compile_options(native_matrix_call_tests PRIVATE /W4 /permissive-)
            set_property(TARGET native_matrix_call_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME native_matrix_call_tests COMMAND native_matrix_call_tests "${CMAKE_CURRENT_SOURCE_DIR}/extract/lift-sites-math.txt")
        set_tests_properties(native_matrix_call_tests PROPERTIES TIMEOUT 60 LABELS "native-contract")
        add_executable(guest_call_translated_tests runtime/tests/guest_call_translated_tests.cpp)
        target_include_directories(guest_call_translated_tests PRIVATE "${SHADOW_RECOMP_DIR}")
        target_link_libraries(guest_call_translated_tests PRIVATE shadow_lift shadow_recomp shadow_runtime)
        if(MSVC)
            target_compile_options(guest_call_translated_tests PRIVATE /W4 /permissive-)
            set_property(TARGET guest_call_translated_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME guest_call_translated_tests COMMAND guest_call_translated_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(guest_call_translated_tests PROPERTIES TIMEOUT 60 LABELS "native-contract")
        set(SHADOW_TYPE_CATALOG_DIR "${CMAKE_CURRENT_BINARY_DIR}/Testing/type-catalog")
        add_custom_command(
            OUTPUT "${SHADOW_TYPE_CATALOG_DIR}/native_type_catalog.inc" "${SHADOW_TYPE_CATALOG_DIR}/catalog.json"
                   "${SHADOW_TYPE_CATALOG_DIR}/metadata-uses.json" "${SHADOW_TYPE_CATALOG_DIR}/native_type_queries.h"
                   "${SHADOW_TYPE_CATALOG_DIR}/native_type_boundary.h"
            COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tools/lift/type_catalog.py"
                    --dol "${SHADOW_RECOMP_DOL}" --descriptor 0x805E5AF0 --descriptor 0x805E9D98
                    --functions "${SHADOW_RECOMP_FUNCTIONS}" --jump-tables "${CMAKE_CURRENT_SOURCE_DIR}/extract/jump-tables.txt"
                    --cast-entry 0x803A1AFC --discover-vtables --discover-stores
                    --metadata-uses-out "${SHADOW_TYPE_CATALOG_DIR}/metadata-uses.json"
                    --out "${SHADOW_TYPE_CATALOG_DIR}/catalog.json"
                    --oracle-header "${SHADOW_TYPE_CATALOG_DIR}/native_type_catalog.inc"
                    --native-query-header "${SHADOW_TYPE_CATALOG_DIR}/native_type_queries.h"
                    --call-boundary-header "${SHADOW_TYPE_CATALOG_DIR}/native_type_boundary.h"
            DEPENDS "${SHADOW_RECOMP_DOL}" "${SHADOW_RECOMP_FUNCTIONS}"
                    extract/jump-tables.txt tools/lift/type_catalog.py tools/lift/type_metadata_uses.py tools/graphics_lift_inventory.py
                    tools/gx_site_inventory.py tools/gqr_format_dataflow.py
            COMMENT "Recovering immutable native type-query oracle fixture" VERBATIM)
        add_custom_target(shadow_type_catalog
            DEPENDS "${SHADOW_TYPE_CATALOG_DIR}/native_type_catalog.inc"
                    "${SHADOW_TYPE_CATALOG_DIR}/native_type_queries.h"
                    "${SHADOW_TYPE_CATALOG_DIR}/native_type_boundary.h"
                    "${SHADOW_TYPE_CATALOG_DIR}/catalog.json" "${SHADOW_TYPE_CATALOG_DIR}/metadata-uses.json")
        add_executable(native_type_cast_tests runtime/tests/native_type_cast_tests.cpp)
        add_dependencies(native_type_cast_tests shadow_type_catalog)
        target_include_directories(native_type_cast_tests PRIVATE "${SHADOW_TYPE_CATALOG_DIR}")
        target_link_libraries(native_type_cast_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(native_type_cast_tests PRIVATE /W4 /permissive-)
            set_property(TARGET native_type_cast_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME native_type_cast_tests COMMAND native_type_cast_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(native_type_cast_tests PROPERTIES TIMEOUT 120 LABELS "reference;native-contract")
        add_executable(native_type_unwind_tests runtime/tests/native_type_unwind_tests.cpp)
        add_dependencies(native_type_unwind_tests shadow_type_catalog)
        target_include_directories(native_type_unwind_tests PRIVATE "${SHADOW_TYPE_CATALOG_DIR}")
        target_link_libraries(native_type_unwind_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(native_type_unwind_tests PRIVATE /W4 /permissive-)
            set_property(TARGET native_type_unwind_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME native_type_unwind_tests COMMAND native_type_unwind_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(native_type_unwind_tests PROPERTIES TIMEOUT 120 LABELS "reference;native-contract")
        # A single unchanged original caller, compiled separately from the game.
        set(SHADOW_TYPE_CALL_DIR "${CMAKE_CURRENT_BINARY_DIR}/Testing/type-call")
        add_custom_command(
            OUTPUT "${SHADOW_TYPE_CALL_DIR}/recomp_000.cpp" "${SHADOW_TYPE_CALL_DIR}/recomp_table.cpp"
                   "${SHADOW_TYPE_CALL_DIR}/type_call_fixture.h"
            COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tools/ppcrecomp/make_type_call_fixture.py"
                    --generator $<TARGET_FILE:ppcrecomp> --dol "${SHADOW_RECOMP_DOL}"
                    --catalog "${SHADOW_TYPE_CATALOG_DIR}/catalog.json" --out "${SHADOW_TYPE_CALL_DIR}"
            DEPENDS ppcrecomp shadow_type_catalog "${SHADOW_TYPE_CATALOG_DIR}/catalog.json"
                    tools/ppcrecomp/make_type_call_fixture.py VERBATIM)
        add_executable(native_type_call_tests runtime/tests/native_type_call_tests.cpp
            "${SHADOW_TYPE_CALL_DIR}/recomp_000.cpp" "${SHADOW_TYPE_CALL_DIR}/recomp_table.cpp"
            "${SHADOW_TYPE_CALL_DIR}/type_call_fixture.h")
        add_dependencies(native_type_call_tests shadow_type_catalog)
        target_include_directories(native_type_call_tests PRIVATE "${SHADOW_TYPE_CALL_DIR}" "${SHADOW_TYPE_CATALOG_DIR}")
        target_link_libraries(native_type_call_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(native_type_call_tests PRIVATE /W4 /permissive- /wd4102 /wd4702 ${SHADOW_ARCH_FLAGS})
            set_property(TARGET native_type_call_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME native_type_call_tests COMMAND native_type_call_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(native_type_call_tests PROPERTIES TIMEOUT 120 LABELS "reference;codegen;native-contract")
        set(SHADOW_TYPE_UNWIND_CALL_DIR "${CMAKE_CURRENT_BINARY_DIR}/Testing/type-unwind-call")
        add_custom_command(
            OUTPUT "${SHADOW_TYPE_UNWIND_CALL_DIR}/recomp_000.cpp" "${SHADOW_TYPE_UNWIND_CALL_DIR}/recomp_table.cpp"
                   "${SHADOW_TYPE_UNWIND_CALL_DIR}/type_call_fixture.h"
            COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tools/ppcrecomp/make_type_call_fixture.py"
                    --generator $<TARGET_FILE:ppcrecomp> --dol "${SHADOW_RECOMP_DOL}"
                    --catalog "${SHADOW_TYPE_CATALOG_DIR}/catalog.json" --out "${SHADOW_TYPE_UNWIND_CALL_DIR}"
                    --reference --functions "${SHADOW_RECOMP_FUNCTIONS}"
            DEPENDS ppcrecomp shadow_type_catalog "${SHADOW_TYPE_CATALOG_DIR}/catalog.json"
                    "${SHADOW_RECOMP_FUNCTIONS}" tools/ppcrecomp/make_type_call_fixture.py VERBATIM)
        add_executable(native_type_unwind_call_tests runtime/tests/native_type_unwind_call_tests.cpp
            "${SHADOW_TYPE_UNWIND_CALL_DIR}/recomp_000.cpp" "${SHADOW_TYPE_UNWIND_CALL_DIR}/recomp_table.cpp"
            "${SHADOW_TYPE_UNWIND_CALL_DIR}/type_call_fixture.h")
        add_dependencies(native_type_unwind_call_tests shadow_type_catalog)
        target_include_directories(native_type_unwind_call_tests PRIVATE "${SHADOW_TYPE_UNWIND_CALL_DIR}" "${SHADOW_TYPE_CATALOG_DIR}")
        target_link_libraries(native_type_unwind_call_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(native_type_unwind_call_tests PRIVATE /W4 /permissive- /wd4102 /wd4702 ${SHADOW_ARCH_FLAGS})
            set_property(TARGET native_type_unwind_call_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME native_type_unwind_call_tests COMMAND native_type_unwind_call_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(native_type_unwind_call_tests PROPERTIES TIMEOUT 120 LABELS "reference;codegen;native-contract")
        # Isolated generated semantic leaves; the original interpreter is the oracle.
        set(SHADOW_EMPTY_VARIADIC_DIR "${CMAKE_CURRENT_BINARY_DIR}/Testing/empty-variadic")
        add_custom_command(
            OUTPUT "${SHADOW_EMPTY_VARIADIC_DIR}/recomp_000.cpp" "${SHADOW_EMPTY_VARIADIC_DIR}/recomp_table.cpp"
            COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tools/ppcrecomp/make_empty_variadic_fixture.py"
                    --generator $<TARGET_FILE:ppcrecomp> --dol "${SHADOW_RECOMP_DOL}"
                    --functions "${CMAKE_CURRENT_SOURCE_DIR}/extract/functions.csv"
                    --leaves "${CMAKE_CURRENT_SOURCE_DIR}/extract/empty-variadic-leaves.txt"
                    --jump-tables "${CMAKE_CURRENT_SOURCE_DIR}/extract/jump-tables.txt"
                    --out "${SHADOW_EMPTY_VARIADIC_DIR}"
            DEPENDS ppcrecomp "${SHADOW_RECOMP_DOL}" extract/functions.csv extract/empty-variadic-leaves.txt
                    tools/ppcrecomp/make_empty_variadic_fixture.py tools/lift/type_call_liveness.py
                    tools/ppcrecomp/wait_points.py tools/gx_site_inventory.py extract/jump-tables.txt VERBATIM)
        add_executable(native_empty_variadic_tests runtime/tests/native_empty_variadic_tests.cpp
            "${SHADOW_EMPTY_VARIADIC_DIR}/recomp_000.cpp" "${SHADOW_EMPTY_VARIADIC_DIR}/recomp_table.cpp")
        target_include_directories(native_empty_variadic_tests PRIVATE "${SHADOW_EMPTY_VARIADIC_DIR}")
        target_link_libraries(native_empty_variadic_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(native_empty_variadic_tests PRIVATE /W4 /permissive- ${SHADOW_ARCH_FLAGS})
            set_property(TARGET native_empty_variadic_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME native_empty_variadic_tests COMMAND native_empty_variadic_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(native_empty_variadic_tests PROPERTIES TIMEOUT 120 LABELS "reference;codegen;native-contract")
        add_executable(native_scalar_math_tests runtime/tests/native_scalar_math_tests.cpp)
        target_link_libraries(native_scalar_math_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(native_scalar_math_tests PRIVATE /W4 /permissive-)
            set_property(TARGET native_scalar_math_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME native_scalar_math_tests COMMAND native_scalar_math_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(native_scalar_math_tests PROPERTIES TIMEOUT 120 LABELS "reference;native-contract")
        add_executable(native_scalar_call_tests runtime/tests/native_scalar_call_tests.cpp)
        target_include_directories(native_scalar_call_tests PRIVATE "${SHADOW_RECOMP_DIR}")
        target_link_libraries(native_scalar_call_tests PRIVATE shadow_lift shadow_recomp shadow_runtime)
        if(MSVC)
            target_compile_options(native_scalar_call_tests PRIVATE /W4 /permissive-)
            set_property(TARGET native_scalar_call_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME native_scalar_call_tests COMMAND native_scalar_call_tests)
        set_tests_properties(native_scalar_call_tests PROPERTIES TIMEOUT 60 LABELS "native-contract")
        add_executable(gameplay_scheduler_reference_tests runtime/tests/gameplay_scheduler_reference_tests.cpp)
        target_link_libraries(gameplay_scheduler_reference_tests PRIVATE shadow_native_gameplay shadow_runtime_reference)
        if(MSVC)
            target_compile_options(gameplay_scheduler_reference_tests PRIVATE /W4 /permissive-)
            set_property(TARGET gameplay_scheduler_reference_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME gameplay_scheduler_reference_tests COMMAND gameplay_scheduler_reference_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(gameplay_scheduler_reference_tests PROPERTIES TIMEOUT 120 LABELS "reference;native-gameplay")
        # The native file API's owners against the original Nintendo DVD SDK
        # words in the reference interpreter, on the API contract
        # (docs/native-asset-pipeline-20261005/README.md).
        add_executable(native_file_api_reference_tests runtime/tests/native_file_api_reference_tests.cpp
                       runtime/tests/support/fst_oracle.cpp)
        target_include_directories(native_file_api_reference_tests PRIVATE runtime/tests)
        target_link_libraries(native_file_api_reference_tests PRIVATE shadow_runtime_reference)
        if(MSVC)
            target_compile_options(native_file_api_reference_tests PRIVATE /W4 /permissive-)
            set_property(TARGET native_file_api_reference_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME native_file_api_reference_tests COMMAND native_file_api_reference_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(native_file_api_reference_tests PROPERTIES TIMEOUT 1800 LABELS "reference;native-file-api")
        # GameMemory's owners of memcpy (0x800054F4) and PRS expand (0x80043FE0) against the original words in
        # the reference interpreter, and every PRS member on the disc (findings-20261019-EV/EV_REPORT.md 7).
        add_executable(game_memory_owner_tests runtime/tests/game_memory_owner_tests.cpp)
        target_link_libraries(game_memory_owner_tests PRIVATE shadow_runtime_reference)
        if(MSVC)
            target_compile_options(game_memory_owner_tests PRIVATE /W4 /permissive-)
            set_property(TARGET game_memory_owner_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME game_memory_owner_tests COMMAND game_memory_owner_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(game_memory_owner_tests PROPERTIES TIMEOUT 1800 LABELS "reference;native-runtime")
        # Disc cut 2 (docs/DISC_SYSTEM_MAP.md): the native directory scan against the original 0x80044ED4 in the
        # reference interpreter, on the same image, namespace and heap stand-in.
        add_executable(native_directory_scan_tests runtime/tests/native_directory_scan_tests.cpp
                       runtime/tests/support/fst_oracle.cpp)
        target_include_directories(native_directory_scan_tests PRIVATE runtime/tests)
        target_link_libraries(native_directory_scan_tests PRIVATE shadow_runtime_reference)
        if(MSVC)
            target_compile_options(native_directory_scan_tests PRIVATE /W4 /permissive-)
            set_property(TARGET native_directory_scan_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME native_directory_scan_tests COMMAND native_directory_scan_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(native_directory_scan_tests PROPERTIES TIMEOUT 300 LABELS "reference;native-runtime")
        # The layout manager's link-group lists (docs/GPU_FILL.md "stage start"): the native owner of 0x800CAEAC
        # against the original words in the reference interpreter.
        add_executable(native_link_groups_tests runtime/tests/native_link_groups_tests.cpp)
        target_link_libraries(native_link_groups_tests PRIVATE shadow_runtime_reference)
        if(MSVC)
            target_compile_options(native_link_groups_tests PRIVATE /W4 /permissive-)
            set_property(TARGET native_link_groups_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME native_link_groups_tests COMMAND native_link_groups_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(native_link_groups_tests PROPERTIES TIMEOUT 600 LABELS "reference;native-runtime")
        # Disc cut 3: CRI's file device gcCi native, against its original words in the reference interpreter.
        add_executable(native_cri_file_device_tests runtime/tests/native_cri_file_device_tests.cpp)
        target_link_libraries(native_cri_file_device_tests PRIVATE shadow_runtime_reference)
        if(MSVC)
            target_compile_options(native_cri_file_device_tests PRIVATE /W4 /permissive-)
            set_property(TARGET native_cri_file_device_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME native_cri_file_device_tests COMMAND native_cri_file_device_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(native_cri_file_device_tests PROPERTIES TIMEOUT 300 LABELS "reference;native-runtime")
        # Disc cut 4: RenderWare's RtGCNFS native, against its original words in the reference interpreter.
        add_executable(native_rtgcnfs_tests runtime/tests/native_rtgcnfs_tests.cpp)
        target_link_libraries(native_rtgcnfs_tests PRIVATE shadow_runtime_reference)
        if(MSVC)
            target_compile_options(native_rtgcnfs_tests PRIVATE /W4 /permissive-)
            set_property(TARGET native_rtgcnfs_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME native_rtgcnfs_tests COMMAND native_rtgcnfs_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(native_rtgcnfs_tests PROPERTIES TIMEOUT 300 LABELS "reference;native-runtime")
        # TBGRead's chunked background read served inline (docs/STAGE_LOADING.md 9d), against the original loop,
        # RwFread and RtGCNFS words in the reference interpreter.
        add_executable(native_bg_read_tests runtime/tests/native_bg_read_tests.cpp)
        target_link_libraries(native_bg_read_tests PRIVATE shadow_runtime_reference)
        if(MSVC)
            target_compile_options(native_bg_read_tests PRIVATE /W4 /permissive-)
            set_property(TARGET native_bg_read_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME native_bg_read_tests COMMAND native_bg_read_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(native_bg_read_tests PROPERTIES TIMEOUT 300 LABELS "reference;native-runtime")
        # TExpandBG without a GameCube thread (docs/NATIVE_WORKERS.md W1): the native Thread constructor against the
        # original words in the reference interpreter.
        add_executable(native_expand_jobs_tests runtime/tests/native_expand_jobs_tests.cpp)
        target_link_libraries(native_expand_jobs_tests PRIVATE shadow_runtime_reference)
        if(MSVC)
            target_compile_options(native_expand_jobs_tests PRIVATE /W4 /permissive-)
            set_property(TARGET native_expand_jobs_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME native_expand_jobs_tests COMMAND native_expand_jobs_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(native_expand_jobs_tests PROPERTIES TIMEOUT 120 LABELS "reference;native-runtime")
        # The ONE archive layer in C++ (Phase 4, docs/ONE_LAYER.md): every function against the original words, and
        # every archive on the disc adopted and parsed both ways.
        add_executable(native_one_tests runtime/tests/native_one_tests.cpp)
        target_link_libraries(native_one_tests PRIVATE shadow_runtime_reference)
        if(MSVC)
            target_compile_options(native_one_tests PRIVATE /W4 /permissive-)
            set_property(TARGET native_one_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME native_one_tests COMMAND native_one_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(native_one_tests PROPERTIES TIMEOUT 1800 LABELS "reference;native-runtime")
        # The OS heap census (docs/MEMORY_ALLOCATORS.md) against the original OSAlloc words: a real heap churned to
        # full and back, the census checked after every call (first fit, coalescing, conservation, live blocks).
        add_executable(heap_census_tests runtime/tests/heap_census_tests.cpp)
        target_link_libraries(heap_census_tests PRIVATE shadow_runtime_reference)
        if(MSVC)
            target_compile_options(heap_census_tests PRIVATE /W4 /permissive-)
            set_property(TARGET heap_census_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME heap_census_tests COMMAND heap_census_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(heap_census_tests PROPERTIES TIMEOUT 600 LABELS "reference;native-runtime")
        # Sonicteam::System::Heap in C++20 against its original words: every byte after every call.
        add_executable(sonicteam_heap_tests runtime/tests/sonicteam_heap_tests.cpp)
        target_link_libraries(sonicteam_heap_tests PRIVATE shadow_runtime_reference)
        if(MSVC)
            target_compile_options(sonicteam_heap_tests PRIVATE /W4 /permissive-)
            set_property(TARGET sonicteam_heap_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME sonicteam_heap_tests COMMAND sonicteam_heap_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(sonicteam_heap_tests PROPERTIES TIMEOUT 600 LABELS "reference;native-runtime")
        # DCInvalidateRange / DCFlushRangeNoSync's native owner against their original words: every register.
        add_executable(cache_range_owner_tests runtime/tests/cache_range_owner_tests.cpp)
        target_link_libraries(cache_range_owner_tests PRIVATE shadow_runtime_reference)
        if(MSVC)
            target_compile_options(cache_range_owner_tests PRIVATE /W4 /permissive-)
            set_property(TARGET cache_range_owner_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME cache_range_owner_tests COMMAND cache_range_owner_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(cache_range_owner_tests PROPERTIES TIMEOUT 600 LABELS "reference;native-runtime")
        # The OS thread service's observable contract (docs/NATIVE_THREADS.md): scenarios on the original words in
        # the reference interpreter (stage 1); the native scheduler against them (stage 2).
        # Gate 2's dynamic half (include/gekko/guest_watch.h): exact row attribution of watched guest accesses.
        add_executable(guest_watch_tests runtime/tests/guest_watch_tests.cpp)
        target_link_libraries(guest_watch_tests PRIVATE shadow_runtime_reference)
        if(MSVC)
            # Symbols: the watch attributes leaf functions (no unwind record) only through a PDB.
            target_compile_options(guest_watch_tests PRIVATE /W4 /permissive- /Zi)
            target_link_options(guest_watch_tests PRIVATE /DEBUG)
            set_property(TARGET guest_watch_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME guest_watch_tests COMMAND guest_watch_tests)
        set_tests_properties(guest_watch_tests PROPERTIES TIMEOUT 60 LABELS "native;platform")
        add_executable(native_threads_tests runtime/tests/native_threads_tests.cpp)
        target_link_libraries(native_threads_tests PRIVATE shadow_runtime_reference)
        if(MSVC)
            target_compile_options(native_threads_tests PRIVATE /W4 /permissive-)
            set_property(TARGET native_threads_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME native_threads_tests COMMAND native_threads_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(native_threads_tests PROPERTIES TIMEOUT 300 LABELS "reference;native-runtime")
        # Deliberate original-game bug fixes (docs/GAME_ERRATA.md) on the original words: the bug reproduces without
        # the fix and every affected AI enters its intended state with it.
        add_executable(game_errata_tests runtime/tests/game_errata_tests.cpp)
        target_link_libraries(game_errata_tests PRIVATE shadow_runtime_reference)
        if(MSVC)
            target_compile_options(game_errata_tests PRIVATE /W4 /permissive-)
            set_property(TARGET game_errata_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME game_errata_tests COMMAND game_errata_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(game_errata_tests PROPERTIES TIMEOUT 120 LABELS "reference;errata")
        # CRI's server manager (docs/CRI_SERVERS.md section 1): NativeSvm against the original twenty functions.
        add_executable(native_svm_tests runtime/tests/native_svm_tests.cpp)
        target_link_libraries(native_svm_tests PRIVATE shadow_runtime_reference)
        if(MSVC)
            target_compile_options(native_svm_tests PRIVATE /W4 /permissive-)
            set_property(TARGET native_svm_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME native_svm_tests COMMAND native_svm_tests "${SHADOW_RECOMP_DOL}"
                 "${CMAKE_CURRENT_SOURCE_DIR}/extract/native-owners.txt")
        set_tests_properties(native_svm_tests PROPERTIES TIMEOUT 120 LABELS "reference;native-runtime")
        # CRI's stream joints (docs/CRI_SERVERS.md section 4b): NativeSj against the original SJMEM / SJRBF words.
        add_executable(native_sj_tests runtime/tests/native_sj_tests.cpp)
        target_link_libraries(native_sj_tests PRIVATE shadow_runtime_reference)
        if(MSVC)
            target_compile_options(native_sj_tests PRIVATE /W4 /permissive-)
            set_property(TARGET native_sj_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME native_sj_tests COMMAND native_sj_tests "${SHADOW_RECOMP_DOL}"
                 "${CMAKE_CURRENT_SOURCE_DIR}/extract/native-owners.txt")
        set_tests_properties(native_sj_tests PROPERTIES TIMEOUT 300 LABELS "reference;native-runtime")
        # CRI's ADX output to the voices (docs/CRI_SERVERS.md section 4c): NativeAdxOutput against the original.
        add_executable(native_adx_output_tests runtime/tests/native_adx_output_tests.cpp)
        target_link_libraries(native_adx_output_tests PRIVATE shadow_runtime_reference)
        if(MSVC)
            target_compile_options(native_adx_output_tests PRIVATE /W4 /permissive-)
            set_property(TARGET native_adx_output_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME native_adx_output_tests COMMAND native_adx_output_tests "${SHADOW_RECOMP_DOL}"
                 "${CMAKE_CURRENT_SOURCE_DIR}/extract/native-owners.txt")
        set_tests_properties(native_adx_output_tests PROPERTIES TIMEOUT 300 LABELS "reference;native-runtime")
        # R16: the guarded paired-single specialisations (ppc::psq_load_k / psq_store_k) against the generic
        # helpers they stand in for, byte-, FPR- and access-trace-exact (findings-20261019-EV/EV_REPORT.md 16).
        add_executable(psq_specialised_tests runtime/tests/psq_specialised_tests.cpp)
        target_link_libraries(psq_specialised_tests PRIVATE shadow_runtime_reference)
        if(MSVC)
            target_compile_options(psq_specialised_tests PRIVATE /W4 /permissive-)
            set_property(TARGET psq_specialised_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME psq_specialised_tests COMMAND psq_specialised_tests)
        set_tests_properties(psq_specialised_tests PROPERTIES TIMEOUT 1800 LABELS "reference;native-runtime")
        # FM: the flat native memory map's guest-visible behaviour - mapped, holes, uncommitted tails, straddles,
        # MSR[DR] off, access trace, journal - against the page-table semantics (findings-20261019-EV/EV_REPORT.md 17).
        add_executable(flat_memory_tests runtime/tests/flat_memory_tests.cpp)
        target_link_libraries(flat_memory_tests PRIVATE shadow_runtime_reference)
        if(MSVC)
            target_compile_options(flat_memory_tests PRIVATE /W4 /permissive-)
            set_property(TARGET flat_memory_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME flat_memory_tests COMMAND flat_memory_tests)
        set_tests_properties(flat_memory_tests PROPERTIES TIMEOUT 600 LABELS "reference;native-runtime")
        # FM: on THIS CPU, a split MOV / MOVBE store into a no-access page commits no byte and a split load writes
        # no destination (FM's no-partial-store case rests on it; checked per machine, not assumed).
        if(WIN32)
            add_executable(split_store_probe_tests runtime/tests/split_store_probe_tests.cpp)
            if(MSVC)
                target_compile_options(split_store_probe_tests PRIVATE /W4 /permissive- ${SHADOW_ARCH_FLAGS})
            endif()
            add_test(NAME split_store_probe_tests COMMAND split_store_probe_tests)
            set_tests_properties(split_store_probe_tests PROPERTIES TIMEOUT 120 LABELS "native-runtime")
        endif()
        # The native VIInit against the original VI library words in the
        # reference interpreter (docs/native-time-20261005/STAGE2_VI.md).
        add_executable(native_video_reference_tests runtime/tests/native_video_reference_tests.cpp)
        target_link_libraries(native_video_reference_tests PRIVATE shadow_runtime_reference)
        if(MSVC)
            target_compile_options(native_video_reference_tests PRIVATE /W4 /permissive-)
            set_property(TARGET native_video_reference_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME native_video_reference_tests COMMAND native_video_reference_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(native_video_reference_tests PROPERTIES TIMEOUT 300 LABELS "reference;native-video")
        add_executable(native_rw_world_reference_tests runtime/tests/native_rw_world_reference_tests.cpp)
        target_link_libraries(native_rw_world_reference_tests PRIVATE shadow_runtime_reference)
        if(MSVC)
            target_compile_options(native_rw_world_reference_tests PRIVATE /W4 /permissive-)
            set_property(TARGET native_rw_world_reference_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME native_rw_world_reference_tests COMMAND native_rw_world_reference_tests "${SHADOW_RECOMP_DOL}"
                 "${CMAKE_CURRENT_SOURCE_DIR}/test_data/platform/gx_rw_frontier.csv")
        set_tests_properties(native_rw_world_reference_tests PROPERTIES TIMEOUT 300 LABELS "reference;native-rw-world")
        add_executable(rw_pool_contract_tests runtime/tests/rw_pool_contract_tests.cpp)
        target_link_libraries(rw_pool_contract_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(rw_pool_contract_tests PRIVATE /W4 /permissive-)
            set_property(TARGET rw_pool_contract_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME rw_pool_contract_tests COMMAND rw_pool_contract_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(rw_pool_contract_tests PROPERTIES TIMEOUT 1200 LABELS "reference;native-rw-world")
        add_executable(rw_texture_write_contract_tests runtime/tests/rw_texture_write_contract_tests.cpp)
        target_link_libraries(rw_texture_write_contract_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(rw_texture_write_contract_tests PRIVATE /W4 /permissive-)
            set_property(TARGET rw_texture_write_contract_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME rw_texture_write_contract_tests COMMAND rw_texture_write_contract_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(rw_texture_write_contract_tests PROPERTIES TIMEOUT 900 LABELS "reference;native-rw-world")
        add_executable(rw_vector_contract_tests runtime/tests/rw_vector_contract_tests.cpp)
        target_link_libraries(rw_vector_contract_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(rw_vector_contract_tests PRIVATE /W4 /permissive-)
            set_property(TARGET rw_vector_contract_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME rw_vector_contract_tests COMMAND rw_vector_contract_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(rw_vector_contract_tests PROPERTIES TIMEOUT 1800 LABELS "reference;native-contract")
        add_executable(rw_matrix_contract_tests runtime/tests/rw_matrix_contract_tests.cpp)
        target_link_libraries(rw_matrix_contract_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(rw_matrix_contract_tests PRIVATE /W4 /permissive-)
            set_property(TARGET rw_matrix_contract_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME rw_matrix_contract_tests COMMAND rw_matrix_contract_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(rw_matrix_contract_tests PROPERTIES TIMEOUT 1800 LABELS "reference;native-contract")
        add_executable(rw_view_cache_contract_tests runtime/tests/rw_view_cache_contract_tests.cpp)
        target_link_libraries(rw_view_cache_contract_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(rw_view_cache_contract_tests PRIVATE /W4 /permissive-)
            set_property(TARGET rw_view_cache_contract_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME rw_view_cache_contract_tests COMMAND rw_view_cache_contract_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(rw_view_cache_contract_tests PROPERTIES TIMEOUT 1800 LABELS "reference;native-contract")
        add_executable(rw_world_contract_tests runtime/tests/rw_world_contract_tests.cpp)
        target_link_libraries(rw_world_contract_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(rw_world_contract_tests PRIVATE /W4 /permissive-)
            set_property(TARGET rw_world_contract_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME rw_world_contract_tests COMMAND rw_world_contract_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(rw_world_contract_tests PROPERTIES TIMEOUT 900 LABELS "reference;native-rw-world")
        add_executable(rw_world_stream_contract_tests runtime/tests/rw_world_stream_contract_tests.cpp)
        target_link_libraries(rw_world_stream_contract_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(rw_world_stream_contract_tests PRIVATE /W4 /permissive-)
            set_property(TARGET rw_world_stream_contract_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME rw_world_stream_contract_tests COMMAND rw_world_stream_contract_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(rw_world_stream_contract_tests PROPERTIES TIMEOUT 900 LABELS "reference;native-rw-world")
        add_executable(rw_world_decode_tests runtime/tests/rw_world_decode_tests.cpp)
        target_link_libraries(rw_world_decode_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(rw_world_decode_tests PRIVATE /W4 /permissive-)
            set_property(TARGET rw_world_decode_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME rw_world_decode_tests COMMAND rw_world_decode_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(rw_world_decode_tests PROPERTIES TIMEOUT 900 LABELS "reference;native-rw-world")
        add_executable(rw_registry_contract_tests runtime/tests/rw_registry_contract_tests.cpp)
        target_link_libraries(rw_registry_contract_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(rw_registry_contract_tests PRIVATE /W4 /permissive-)
            set_property(TARGET rw_registry_contract_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME rw_registry_contract_tests COMMAND rw_registry_contract_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(rw_registry_contract_tests PROPERTIES TIMEOUT 600 LABELS "reference;native-rw-world")
        add_executable(rw_device_state_contract_tests runtime/tests/rw_device_state_contract_tests.cpp)
        target_link_libraries(rw_device_state_contract_tests PRIVATE shadow_lift shadow_runtime_reference)
        if(MSVC)
            target_compile_options(rw_device_state_contract_tests PRIVATE /W4 /permissive-)
            set_property(TARGET rw_device_state_contract_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME rw_device_state_contract_tests COMMAND rw_device_state_contract_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(rw_device_state_contract_tests PROPERTIES TIMEOUT 300 LABELS "reference;native-rw-world")
        # The native GX draw sync / breakpoint / FIFO owners and deliveries
        # against the original GX library words in the reference interpreter
        # (docs/native-time-20261005/STAGE4_GX_SYNC.md).
        add_executable(native_gx_sync_reference_tests runtime/tests/native_gx_sync_reference_tests.cpp)
        target_link_libraries(native_gx_sync_reference_tests PRIVATE shadow_runtime_reference)
        if(MSVC)
            target_compile_options(native_gx_sync_reference_tests PRIVATE /W4 /permissive-)
            set_property(TARGET native_gx_sync_reference_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME native_gx_sync_reference_tests COMMAND native_gx_sync_reference_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(native_gx_sync_reference_tests PROPERTIES TIMEOUT 300 LABELS "reference;native-gx-sync")
        # Phase 4: the GX pipe site list (extract/gx-sites.txt) against the original GX
        # library words in the reference interpreter (GX_DATA_PLANE.md).
        add_executable(native_gx_sites_reference_tests runtime/tests/native_gx_sites_reference_tests.cpp)
        target_link_libraries(native_gx_sites_reference_tests PRIVATE shadow_runtime_reference)
        if(MSVC)
            target_compile_options(native_gx_sites_reference_tests PRIVATE /W4 /permissive-)
            set_property(TARGET native_gx_sites_reference_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME native_gx_sites_reference_tests COMMAND native_gx_sites_reference_tests "${SHADOW_RECOMP_DOL}"
                 "${CMAKE_CURRENT_SOURCE_DIR}/extract/gx-sites.txt")
        set_tests_properties(native_gx_sites_reference_tests PROPERTIES TIMEOUT 300 LABELS "reference;native-gx-sites")
    endif()

    add_executable(native_memory_tests runtime/tests/native_memory_tests.cpp)
    target_link_libraries(native_memory_tests PRIVATE shadow_native_memory Threads::Threads)
    if(MSVC)
        target_compile_options(native_memory_tests PRIVATE /W4 /permissive-)
        set_property(TARGET native_memory_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME native_memory_tests COMMAND native_memory_tests)
    set_tests_properties(native_memory_tests PROPERTIES TIMEOUT 60 LABELS "native-memory")

    add_executable(native_task_graph_tests runtime/tests/native_task_graph_tests.cpp)
    target_link_libraries(native_task_graph_tests PRIVATE shadow_native_execution)
    if(MSVC)
        target_compile_options(native_task_graph_tests PRIVATE /W4 /permissive-)
        set_property(TARGET native_task_graph_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME native_task_graph_tests COMMAND native_task_graph_tests)
    set_tests_properties(native_task_graph_tests PROPERTIES TIMEOUT 60 LABELS "native-execution")

    # Independent PC boot prerequisite, intentionally not linked into either
    # runtime provider or the game. No CPU, Bus or device dependencies.
    add_executable(native_pc_bootstrap_image_tests
        runtime/tests/native_pc_bootstrap_image_tests.cpp
        runtime/src/native_pc_bootstrap_image.cpp)
    target_include_directories(native_pc_bootstrap_image_tests PRIVATE runtime/include)
    target_compile_features(native_pc_bootstrap_image_tests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(native_pc_bootstrap_image_tests PRIVATE /W4 /permissive-)
        set_property(TARGET native_pc_bootstrap_image_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME native_pc_bootstrap_image_tests COMMAND native_pc_bootstrap_image_tests)
    if(NOT SHADOW_RECOMP_DOL STREQUAL "")
        add_test(NAME native_pc_bootstrap_original_crt_tests
                 COMMAND native_pc_bootstrap_image_tests "${SHADOW_RECOMP_DOL}")
    endif()

    add_executable(shadow_runtime_tests runtime/tests/native_bootstrap_tests.cpp)
    target_link_libraries(shadow_runtime_tests PRIVATE shadow_runtime)
    if(MSVC)
        target_compile_options(shadow_runtime_tests PRIVATE /W4 /permissive-)
        set_property(TARGET shadow_runtime_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME shadow_runtime_tests COMMAND shadow_runtime_tests)

    add_executable(gekko_core_tests runtime/tests/gekko_core_tests.cpp runtime/tests/support/fst_oracle.cpp)
    target_include_directories(gekko_core_tests PRIVATE runtime/tests)
    target_link_libraries(gekko_core_tests PRIVATE shadow_runtime_reference)
    if(MSVC)
        target_compile_options(gekko_core_tests PRIVATE /W4 /permissive-)
        set_property(TARGET gekko_core_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME gekko_core_tests COMMAND gekko_core_tests)

    # The native player's migration boundary (docs/SHADOW_PLAYER_NATIVE_RECONSTRUCTION.md, batch 00).
    add_executable(player_view_tests runtime/tests/player_view_tests.cpp)
    target_link_libraries(player_view_tests PRIVATE shadow_runtime_reference)
    if(MSVC)
        target_compile_options(player_view_tests PRIVATE /W4 /permissive-)
        set_property(TARGET player_view_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME player_view_tests COMMAND player_view_tests
             "${CMAKE_CURRENT_SOURCE_DIR}/test_data/player/player_shadow_fields.csv")
    set_tests_properties(player_view_tests PROPERTIES TIMEOUT 60 LABELS "player")

    if(WIN32 AND SHADOW_ENABLE_PLUME)
        # The RHI transfer queue against a real device: COPY-queue uploads read
        # back through a DIRECT list that waits on the batch semaphores.
        add_executable(render_rhi_tests runtime/tests/render_rhi_tests.cpp)
        target_link_libraries(render_rhi_tests PRIVATE shadow_runtime plume d3d12 dxgi)
        target_compile_definitions(render_rhi_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        if(MSVC)
            target_compile_options(render_rhi_tests PRIVATE /W4 /permissive- /wd4201)
            set_property(TARGET render_rhi_tests PROPERTY MSVC_RUNTIME_LIBRARY
                         "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME render_rhi_tests COMMAND render_rhi_tests)
        set_tests_properties(render_rhi_tests PROPERTIES
                             SKIP_RETURN_CODE 77 TIMEOUT 120 LABELS "gpu;integration")

        add_executable(native_gpu_constant_tests runtime/tests/native_gpu_constant_tests.cpp)
        target_link_libraries(native_gpu_constant_tests PRIVATE shadow_runtime plume d3d12 dxgi)
        target_compile_definitions(native_gpu_constant_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        if(MSVC)
            target_compile_options(native_gpu_constant_tests PRIVATE /W4 /permissive- /wd4201)
            set_property(TARGET native_gpu_constant_tests PROPERTY MSVC_RUNTIME_LIBRARY
                         "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME native_gpu_constant_tests COMMAND native_gpu_constant_tests)
        set_tests_properties(native_gpu_constant_tests PROPERTIES
                             SKIP_RETURN_CODE 77 TIMEOUT 120 LABELS "gpu;integration")

        add_executable(swapchain_lifecycle_tests runtime/tests/swapchain_lifecycle_tests.cpp)
        target_link_libraries(swapchain_lifecycle_tests PRIVATE shadow_runtime plume d3d12 dxgi)
        target_compile_definitions(swapchain_lifecycle_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        if(MSVC)
            target_compile_options(swapchain_lifecycle_tests PRIVATE /W4 /permissive- /wd4201)
            set_property(TARGET swapchain_lifecycle_tests PROPERTY MSVC_RUNTIME_LIBRARY
                         "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME swapchain_lifecycle_tests COMMAND swapchain_lifecycle_tests
                 "${CMAKE_BINARY_DIR}/Testing/swapchain")
        set_tests_properties(swapchain_lifecycle_tests PROPERTIES
                             SKIP_RETURN_CODE 77 TIMEOUT 60 LABELS "gpu;integration")
    endif()

    add_executable(host_timing_tests runtime/tests/host_timing_tests.cpp)
    target_link_libraries(host_timing_tests PRIVATE shadow_runtime)
    if(MSVC)
        target_compile_options(host_timing_tests PRIVATE /W4 /permissive-)
        set_property(TARGET host_timing_tests PROPERTY MSVC_RUNTIME_LIBRARY
                     "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME host_timing_tests COMMAND host_timing_tests)

    # Native runtime modules (docs/native-runtime-2026-09-16): texture decode,
    # RenderWare instance-geometry conversion, audio runtime (no device needed;
    # `audio_runtime_tests --device` plays on the default endpoint by hand).
    foreach(native_test gx_texture_decode_tests rw_geometry_convert_tests audio_runtime_tests
                       hw_runtime_tests native_assets_tests native_memory_runtime_tests native_audio_mixer_tests)
        add_executable(${native_test} runtime/tests/${native_test}.cpp)
        target_link_libraries(${native_test} PRIVATE shadow_runtime)
        if(MSVC)
            target_compile_options(${native_test} PRIVATE /W4 /permissive-)
            set_property(TARGET ${native_test} PROPERTY MSVC_RUNTIME_LIBRARY
                         "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        if(native_test STREQUAL "native_assets_tests" AND NOT SHADOW_RECOMP_DOL STREQUAL "")
            get_filename_component(native_test_sys "${SHADOW_RECOMP_DOL}" DIRECTORY)
            get_filename_component(native_test_game "${native_test_sys}" DIRECTORY)
            add_test(NAME ${native_test} COMMAND ${native_test} "${native_test_game}")
        elseif(native_test STREQUAL "gx_texture_decode_tests")
            # Real-raster oracle over the B45 Final Haunt MEM1 fixture (see below);
            # decoded PNGs for visual inspection go to the build tree.
            file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/gx-texture-decode-png")
            add_test(NAME ${native_test} COMMAND ${native_test}
                     "${SHADOW_PRIVATE_TEST_DATA}/renderer-re/final-haunt-b45.mem1"
                     "${CMAKE_CURRENT_BINARY_DIR}/gx-texture-decode-png")
        elseif(native_test STREQUAL "rw_geometry_convert_tests")
            # test_data/renderer-re/final-haunt-b45.mem1 (SHA-256 065e797a...c553a) is
            # run B45 of the 2026-09-16 renderer RE: the recompiled build of that day,
            # headless, --boot-stage stg0604 (Final Haunt), --budget 45000000000, then
            # --dump 0x80000000 0x1800000 converted from hex. Its expectations (entry
            # addresses, 8/20/5 entries, RE-02 section 11.2 DL totals) are tied to this
            # exact heap image, so the original bytes are kept, not a regeneration.
            add_test(NAME ${native_test} COMMAND ${native_test}
                     "${SHADOW_PRIVATE_TEST_DATA}/renderer-re/final-haunt-b45.mem1")
        else()
            add_test(NAME ${native_test} COMMAND ${native_test})
        endif()
        set_tests_properties(${native_test} PROPERTIES LABELS "native-runtime" TIMEOUT 300)
    endforeach()
    set_tests_properties(hw_runtime_tests PROPERTIES SKIP_RETURN_CODE 77)

    add_executable(native_file_api_tests runtime/tests/native_file_api_tests.cpp)
    target_link_libraries(native_file_api_tests PRIVATE shadow_runtime)
    if(MSVC)
        target_compile_options(native_file_api_tests PRIVATE /W4 /permissive-)
        set_property(TARGET native_file_api_tests PROPERTY MSVC_RUNTIME_LIBRARY
                     "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME native_file_api_tests COMMAND native_file_api_tests
             "${CMAKE_CURRENT_SOURCE_DIR}/extract/native-owners.txt"
             "${CMAKE_CURRENT_SOURCE_DIR}/extract/platform-sites.txt")
    set_tests_properties(native_file_api_tests PROPERTIES LABELS "native-runtime" TIMEOUT 300)

    if(MSVC)
        # L0r0-02: the host ISA gate's decision logic (synthetic processors) and its CPUID/XGETBV decoding
        # against IsProcessorFeaturePresent. Links the gate object directly, compiled for the baseline.
        add_executable(host_isa_gate_tests runtime/tests/host_isa_gate_tests.cpp)
        target_link_libraries(host_isa_gate_tests PRIVATE shadow_host_isa_gate)
        target_compile_features(host_isa_gate_tests PRIVATE cxx_std_20)
        target_compile_definitions(host_isa_gate_tests PRIVATE SHADOW_TEST_EXPECTED_LEVEL="${SHADOW_X64_ARCH}")
        target_compile_options(host_isa_gate_tests PRIVATE /W4 /permissive-)
        set_property(TARGET host_isa_gate_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        add_test(NAME host_isa_gate_tests COMMAND host_isa_gate_tests)
        set_tests_properties(host_isa_gate_tests PROPERTIES TIMEOUT 30 LABELS "native")
    endif()

    # Proves the recompiler and the interpreter implement the same instruction
    # set: every covered encoding is executed both ways over randomised state and
    # the whole architected result is compared.
    add_executable(ppc_recomp_tests runtime/tests/ppc_recomp_tests.cpp)
    target_link_libraries(ppc_recomp_tests PRIVATE shadow_runtime_reference)
    if(MSVC)
        target_compile_options(ppc_recomp_tests PRIVATE /W4 /permissive-)
        set_property(TARGET ppc_recomp_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME ppc_recomp_tests COMMAND ppc_recomp_tests)
    set_tests_properties(gekko_core_tests ppc_recomp_tests PROPERTIES LABELS "reference")

    add_executable(ax_pb_tests runtime/tests/ax_pb_tests.cpp)
    target_link_libraries(ax_pb_tests PRIVATE shadow_runtime)
    if(MSVC)
        set_property(TARGET ax_pb_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME ax_pb_tests COMMAND ax_pb_tests)

    add_executable(native_sound_store_tests runtime/tests/native_sound_store_tests.cpp)
    target_link_libraries(native_sound_store_tests PRIVATE shadow_runtime)
    if(MSVC)
        target_compile_options(native_sound_store_tests PRIVATE /W4 /permissive-)
        set_property(TARGET native_sound_store_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME native_sound_store_tests COMMAND native_sound_store_tests)

    add_executable(numeric_probe_tests runtime/tests/numeric_probe_tests.cpp)
    target_link_libraries(numeric_probe_tests PRIVATE shadow_runtime)
    if(MSVC)
        target_compile_options(numeric_probe_tests PRIVATE /W4 /permissive-)
        set_property(TARGET numeric_probe_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME numeric_probe_tests COMMAND numeric_probe_tests)
    set_tests_properties(numeric_probe_tests PROPERTIES LABELS "measurement" TIMEOUT 30)
    add_executable(native_fence_retirement_tests runtime/tests/native_fence_retirement_tests.cpp)
    target_include_directories(native_fence_retirement_tests PRIVATE thirdparty/plume)
    target_compile_features(native_fence_retirement_tests PRIVATE cxx_std_20)
    add_executable(native_segment_failure_tests runtime/tests/native_segment_failure_tests.cpp)
    target_include_directories(native_segment_failure_tests PRIVATE thirdparty/plume)
    target_link_libraries(native_segment_failure_tests PRIVATE shadow_runtime)
    target_compile_features(native_segment_failure_tests PRIVATE cxx_std_20)
    add_executable(native_presentation_retirement_tests runtime/tests/native_presentation_retirement_tests.cpp)
    target_include_directories(native_presentation_retirement_tests PRIVATE runtime/include thirdparty/plume)
    target_compile_features(native_presentation_retirement_tests PRIVATE cxx_std_20)
    add_executable(native_gpu_lifetime_tests runtime/tests/native_gpu_lifetime_tests.cpp)
    target_include_directories(native_gpu_lifetime_tests PRIVATE runtime/include)
    target_compile_features(native_gpu_lifetime_tests PRIVATE cxx_std_20)
    add_executable(copy_dependency_publication_tests runtime/tests/copy_dependency_publication_tests.cpp)
    target_include_directories(copy_dependency_publication_tests PRIVATE runtime/include)
    target_compile_features(copy_dependency_publication_tests PRIVATE cxx_std_20)
    add_executable(native_failure_publication_tests runtime/tests/native_failure_publication_tests.cpp)
    target_include_directories(native_failure_publication_tests PRIVATE runtime/include thirdparty/plume)
    target_link_libraries(native_failure_publication_tests PRIVATE shadow_runtime)
    target_compile_features(native_failure_publication_tests PRIVATE cxx_std_20)
    foreach(retirement_target IN ITEMS native_fence_retirement_tests native_segment_failure_tests native_presentation_retirement_tests native_gpu_lifetime_tests copy_dependency_publication_tests native_failure_publication_tests)
        if(MSVC)
            target_compile_options(${retirement_target} PRIVATE /W4 /permissive-)
            set_property(TARGET ${retirement_target} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME ${retirement_target} COMMAND ${retirement_target})
        set_tests_properties(${retirement_target} PROPERTIES LABELS "native-contract;retirement" TIMEOUT 30)
    endforeach()

    if(NOT SHADOW_RECOMP_DOL STREQUAL "")
        # Candidate native arithmetic is independently tested against original
        # words; not linked into shipping until its caller/packet contract closes.
        add_executable(native_billboard_tests runtime/tests/native_billboard_tests.cpp runtime/src/native_billboard.cpp)
        target_link_libraries(native_billboard_tests PRIVATE shadow_runtime_reference)
        target_compile_features(native_billboard_tests PRIVATE cxx_std_20)
        if(MSVC)
            target_compile_options(native_billboard_tests PRIVATE /W4 /permissive- /fp:strict)
            set_property(TARGET native_billboard_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME native_billboard_tests COMMAND native_billboard_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(native_billboard_tests PROPERTIES LABELS "reference;native-contract" TIMEOUT 60)
        # Complete original producer population; never a shipping Cpu adapter.
        add_executable(ptank_producer_contract_tests runtime/tests/ptank_producer_contract_tests.cpp)
        target_link_libraries(ptank_producer_contract_tests PRIVATE shadow_runtime_reference)
        target_compile_features(ptank_producer_contract_tests PRIVATE cxx_std_20)
        if(MSVC)
            target_compile_options(ptank_producer_contract_tests PRIVATE /W4 /permissive- /fp:strict)
            set_property(TARGET ptank_producer_contract_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME ptank_producer_contract_tests COMMAND ptank_producer_contract_tests "${SHADOW_RECOMP_DOL}")
        set_tests_properties(ptank_producer_contract_tests PROPERTIES LABELS "reference;native-contract" TIMEOUT 180)
        foreach(contract_target IN ITEMS format_lifetime_reference_tests ptank_source_view_reference_tests)
            add_executable(${contract_target} runtime/tests/${contract_target}.cpp)
            target_link_libraries(${contract_target} PRIVATE shadow_runtime_reference)
            target_compile_features(${contract_target} PRIVATE cxx_std_20)
            if(MSVC)
                target_compile_options(${contract_target} PRIVATE /W4 /permissive- /fp:strict)
                set_property(TARGET ${contract_target} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
            endif()
            add_test(NAME ${contract_target} COMMAND ${contract_target} "${SHADOW_RECOMP_DOL}")
            set_tests_properties(${contract_target} PROPERTIES LABELS "reference;native-contract" TIMEOUT 180)
        endforeach()
        add_executable(compiler_body_tests runtime/tests/compiler_body_tests.cpp)
        target_link_libraries(compiler_body_tests PRIVATE shadow_recomp shadow_runtime_reference)
        if(MSVC)
            set_property(TARGET compiler_body_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME compiler_body_tests COMMAND compiler_body_tests "${SHADOW_RECOMP_DOL}")
        # TIMEOUT: the wait-point contract has its own game-time watchdog (wait_point_contract); this bounds everything
        # else, so a hang costs minutes, not ctest's default 1500 s (RC-tests-3).
        set_tests_properties(compiler_body_tests PROPERTIES LABELS "reference;codegen" TIMEOUT 600)
        # The generated lowered call sites (extract/lift-sites.txt) against the
        # lifted subsystems: what each site writes back to its translated caller.
        add_executable(lift_sites_reference_tests runtime/tests/lift_sites_reference_tests.cpp)
        target_link_libraries(lift_sites_reference_tests PRIVATE shadow_recomp shadow_runtime_reference)
        if(MSVC)
            set_property(TARGET lift_sites_reference_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME lift_sites_reference_tests COMMAND lift_sites_reference_tests)
        set_tests_properties(lift_sites_reference_tests PROPERTIES LABELS "reference;lift" TIMEOUT 120)
    endif()

    add_executable(host_job_capacity_tests runtime/tests/host_job_capacity_tests.cpp)
    target_link_libraries(host_job_capacity_tests PRIVATE shadow_runtime)
    if(MSVC)
        set_property(TARGET host_job_capacity_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME host_job_capacity_tests COMMAND host_job_capacity_tests)
    add_test(NAME host_job_oversized_request COMMAND host_job_capacity_tests --oversized)
    set_tests_properties(host_job_capacity_tests PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)

    if(Python3_Interpreter_FOUND)
        set(SHADOW_FALLBACK_FIXTURE "${CMAKE_CURRENT_BINARY_DIR}/Testing/native-fallback-fixture")
        add_custom_command(
            OUTPUT "${SHADOW_FALLBACK_FIXTURE}/recomp_000.cpp"
                   "${SHADOW_FALLBACK_FIXTURE}/recomp_table.cpp"
                   "${SHADOW_FALLBACK_FIXTURE}/fallback_fixture.h"
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/ppcrecomp/make_fallback_fixture.py"
                    --generator $<TARGET_FILE:ppcrecomp> --out "${SHADOW_FALLBACK_FIXTURE}"
            DEPENDS ppcrecomp tools/ppcrecomp/make_fallback_fixture.py
            VERBATIM)
        # Three test executables compile the fixture. The Visual Studio generator attaches the rule to every
        # target that lists its outputs, so without one owning target they could run make_fallback_fixture.py
        # concurrently and delete each other's fallback.dol (seen 2026-09-28: "cannot read DOL"). The
        # consumers depend on this target, so the rule runs once and they find the outputs up to date.
        add_custom_target(shadow_fallback_fixture
            DEPENDS "${SHADOW_FALLBACK_FIXTURE}/recomp_000.cpp"
                    "${SHADOW_FALLBACK_FIXTURE}/recomp_table.cpp"
                    "${SHADOW_FALLBACK_FIXTURE}/fallback_fixture.h")
        add_executable(native_fallback_tests runtime/tests/native_fallback_tests.cpp
            "${SHADOW_FALLBACK_FIXTURE}/recomp_000.cpp"
            "${SHADOW_FALLBACK_FIXTURE}/recomp_table.cpp"
            "${SHADOW_FALLBACK_FIXTURE}/fallback_fixture.h")
        target_include_directories(native_fallback_tests PRIVATE "${SHADOW_FALLBACK_FIXTURE}")
        target_link_libraries(native_fallback_tests PRIVATE shadow_runtime_reference)
        if(MSVC)
            target_compile_options(native_fallback_tests PRIVATE /W4 /permissive- /wd4102 /wd4702
                                                               ${SHADOW_ARCH_FLAGS})
            set_property(TARGET native_fallback_tests PROPERTY MSVC_RUNTIME_LIBRARY
                         "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME native_fallback_tests COMMAND native_fallback_tests)
        set_tests_properties(native_fallback_tests PROPERTIES LABELS "reference;codegen")
        add_executable(native_execution_tests runtime/tests/native_execution_tests.cpp
            "${SHADOW_FALLBACK_FIXTURE}/recomp_000.cpp"
            "${SHADOW_FALLBACK_FIXTURE}/recomp_table.cpp"
            "${SHADOW_FALLBACK_FIXTURE}/fallback_fixture.h")
        target_include_directories(native_execution_tests PRIVATE "${SHADOW_FALLBACK_FIXTURE}")
        target_link_libraries(native_execution_tests PRIVATE shadow_runtime)
        if(MSVC)
            target_compile_options(native_execution_tests PRIVATE /W4 /permissive- /wd4102 /wd4702
                                                                 ${SHADOW_ARCH_FLAGS})
            set_property(TARGET native_execution_tests PROPERTY MSVC_RUNTIME_LIBRARY
                         "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME native_execution_tests COMMAND native_execution_tests)
        set_tests_properties(native_execution_tests PROPERTIES TIMEOUT 30 LABELS "native;codegen")
        add_executable(compiler_poll_tests runtime/tests/compiler_poll_tests.cpp
            "${SHADOW_FALLBACK_FIXTURE}/recomp_000.cpp"
            "${SHADOW_FALLBACK_FIXTURE}/recomp_table.cpp"
            "${SHADOW_FALLBACK_FIXTURE}/fallback_fixture.h")
        target_include_directories(compiler_poll_tests PRIVATE "${SHADOW_FALLBACK_FIXTURE}")
        add_dependencies(native_fallback_tests shadow_fallback_fixture)
        add_dependencies(native_execution_tests shadow_fallback_fixture)
        add_dependencies(compiler_poll_tests shadow_fallback_fixture)
        target_link_libraries(compiler_poll_tests PRIVATE shadow_runtime_reference)
        if(MSVC)
            target_compile_options(compiler_poll_tests PRIVATE /W4 /permissive- /wd4102 /wd4702
                                                              ${SHADOW_ARCH_FLAGS})
            set_property(TARGET compiler_poll_tests PROPERTY MSVC_RUNTIME_LIBRARY
                         "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME compiler_poll_tests COMMAND compiler_poll_tests)
        set_tests_properties(compiler_poll_tests PROPERTIES TIMEOUT 30 LABELS "reference;codegen")
        # ---- cs-interp (swarm 2026-11-12, RC-gen-14): generated-code vs reference-interpreter families ----
        # tools/ppcrecomp/make_interp_fixture.py writes ~3,000 instruction-family programs; the REAL ppcrecomp
        # translates them (production switches) and native_interp_diff_tests runs each natively and through the
        # interpreter from identical random state, at every interior entry for the programs that ask, comparing
        # all architected state. Its own fixture directory and owning target (see shadow_fallback_fixture above).
        set(SHADOW_INTERP_FIXTURE "${CMAKE_CURRENT_BINARY_DIR}/Testing/native-interp-fixture")
        add_custom_command(
            OUTPUT "${SHADOW_INTERP_FIXTURE}/recomp_000.cpp"
                   "${SHADOW_INTERP_FIXTURE}/recomp_001.cpp"
                   "${SHADOW_INTERP_FIXTURE}/recomp_table.cpp"
                   "${SHADOW_INTERP_FIXTURE}/interp_fixture.h"
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/ppcrecomp/make_interp_fixture.py"
                    --generator $<TARGET_FILE:ppcrecomp> --out "${SHADOW_INTERP_FIXTURE}"
            DEPENDS ppcrecomp tools/ppcrecomp/make_interp_fixture.py
            VERBATIM)
        add_custom_target(shadow_interp_fixture
            DEPENDS "${SHADOW_INTERP_FIXTURE}/recomp_000.cpp"
                    "${SHADOW_INTERP_FIXTURE}/recomp_001.cpp"
                    "${SHADOW_INTERP_FIXTURE}/recomp_table.cpp"
                    "${SHADOW_INTERP_FIXTURE}/interp_fixture.h")
        add_executable(native_interp_diff_tests runtime/tests/native_interp_diff_tests.cpp
            "${SHADOW_INTERP_FIXTURE}/recomp_000.cpp"
            "${SHADOW_INTERP_FIXTURE}/recomp_001.cpp"
            "${SHADOW_INTERP_FIXTURE}/recomp_table.cpp"
            "${SHADOW_INTERP_FIXTURE}/interp_fixture.h")
        target_include_directories(native_interp_diff_tests PRIVATE "${SHADOW_INTERP_FIXTURE}")
        add_dependencies(native_interp_diff_tests shadow_interp_fixture)
        target_link_libraries(native_interp_diff_tests PRIVATE shadow_runtime_reference)
        if(MSVC)
            target_compile_options(native_interp_diff_tests PRIVATE /W4 /permissive- /wd4102 /wd4702 /bigobj
                                                                   ${SHADOW_ARCH_FLAGS})
            set_property(TARGET native_interp_diff_tests PROPERTY MSVC_RUNTIME_LIBRARY
                         "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME native_interp_diff_tests COMMAND native_interp_diff_tests)
        set_tests_properties(native_interp_diff_tests PROPERTIES TIMEOUT 600 LABELS "reference;codegen;differential")
        # ---- end cs-interp ----
        # F1a differential harness: generated native code vs reference interpreter vs spec model.
        include("${CMAKE_CURRENT_SOURCE_DIR}/runtime/tests/difftest/difftest.cmake")
        add_test(NAME stage_benchmark_tools
                 COMMAND "${Python3_EXECUTABLE}" -B -m unittest test_stage_benchmark test_resource_usage_report test_frame_budget_report
                         test_memory_map_sampler)
        set_tests_properties(stage_benchmark_tools PROPERTIES
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tools"
            TIMEOUT 30 LABELS "tools;validation")
        add_test(NAME presentation_analysis
                 COMMAND "${Python3_EXECUTABLE}" -B -m unittest test_analyze_presentation test_present_trace_report
                         test_resolution_presets)
        add_test(NAME launcher_lifted_telemetry
                 COMMAND "${Python3_EXECUTABLE}" -B -m unittest test_launcher_telemetry)
        # OPTIMIZATION_PLAN phase A: stack sampling, GPU clock join, class attribution.
        add_test(NAME profiling_tools
                 COMMAND "${Python3_EXECUTABLE}" -B -m unittest test_native_sample test_gpu_clock_sampler
                         test_stack_report)
        set_tests_properties(presentation_analysis launcher_lifted_telemetry profiling_tools PROPERTIES
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tools"
            TIMEOUT 30 LABELS "tools;validation")
        add_test(NAME native_type_catalog_tools
                 COMMAND "${Python3_EXECUTABLE}" -B -m unittest test_type_catalog test_type_metadata_uses test_type_call_liveness)
        set_tests_properties(native_type_catalog_tools PROPERTIES
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tools/lift"
            TIMEOUT 30 LABELS "tools;native-contract")
        add_test(NAME executable_size_audit
                 COMMAND "${Python3_EXECUTABLE}" -B -m unittest test_size_audit)
        set_tests_properties(executable_size_audit PROPERTIES
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tools/ppcrecomp"
            TIMEOUT 30 LABELS "tools;codegen")
        # docs/REGION_FREE_BUILDER.md: disc identity, the address map and rebasing, on synthetic DOLs.
        add_test(NAME disc_map_unit
                 COMMAND "${Python3_EXECUTABLE}" -B -m unittest test_disc_map)
        set_tests_properties(disc_map_unit PROPERTIES
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tools/ppcrecomp"
            TIMEOUT 60 LABELS "tools;codegen")
        add_test(NAME ppcrecomp_output_structure
                 COMMAND "${Python3_EXECUTABLE}"
                         "${CMAKE_CURRENT_SOURCE_DIR}/tools/ppcrecomp/test_output_structure.py"
                         --generator $<TARGET_FILE:ppcrecomp>
                         --work "${CMAKE_CURRENT_BINARY_DIR}/Testing/ppcrecomp")
        # s3/gen: hot-body GPR read cache, differential evaluator + negative controls
        add_test(NAME ppcrecomp_gpr_cache
                 COMMAND "${Python3_EXECUTABLE}" -B
                         "${CMAKE_CURRENT_SOURCE_DIR}/tools/ppcrecomp/test_gpr_cache.py"
                         --generator $<TARGET_FILE:ppcrecomp>)
        set_tests_properties(ppcrecomp_gpr_cache PROPERTIES TIMEOUT 600 LABELS "tools;codegen")
        # cpu/opt2: hot-body value pass, differential evaluator + negative controls + the ppc_ops.h helper list
        add_test(NAME ppcrecomp_hot_value
                 COMMAND "${Python3_EXECUTABLE}" -B
                         "${CMAKE_CURRENT_SOURCE_DIR}/tools/ppcrecomp/test_hot_value.py"
                         --generator $<TARGET_FILE:ppcrecomp>
                         --ppc-ops "${CMAKE_CURRENT_SOURCE_DIR}/runtime/include/gekko/ppc_ops.h")
        set_tests_properties(ppcrecomp_hot_value PROPERTIES TIMEOUT 900 LABELS "tools;codegen")
        set(SHADOW_TEST_RUNTIME_CONFIGURATION_WORK_DIR
            "${CMAKE_CURRENT_BINARY_DIR}/Testing/runtime-configuration"
            CACHE PATH "Temporary work root for the CPU provider configuration test")
        add_test(NAME cpu_runtime_configuration
                 COMMAND "${Python3_EXECUTABLE}" -B
                         "${CMAKE_CURRENT_SOURCE_DIR}/tools/ppcrecomp/test_runtime_configuration.py"
                         --source "${CMAKE_CURRENT_SOURCE_DIR}"
                         --work "${SHADOW_TEST_RUNTIME_CONFIGURATION_WORK_DIR}"
                         --cmake "${CMAKE_COMMAND}")
        set_tests_properties(cpu_runtime_configuration PROPERTIES TIMEOUT 180 LABELS "native;build")
        if(MSVC)
            add_test(NAME native_runtime_boundary
                     COMMAND "${Python3_EXECUTABLE}" -B
                             "${CMAKE_CURRENT_SOURCE_DIR}/tools/ppcrecomp/check_native_boundary.py"
                             --dumpbin "${SHADOW_DUMPBIN}"
                             --native-library "$<TARGET_FILE:shadow_runtime>"
                             --reference-library "$<TARGET_FILE:shadow_runtime_reference>")
            set_tests_properties(native_runtime_boundary PROPERTIES TIMEOUT 120 LABELS "native;build")
            add_test(NAME lift_purity
                     COMMAND "${Python3_EXECUTABLE}" -B
                             "${CMAKE_CURRENT_SOURCE_DIR}/tools/lift/check_purity.py")
            set_tests_properties(lift_purity PROPERTIES TIMEOUT 60 LABELS "lift")
            add_test(NAME lift_scaffold_inventory
                     COMMAND "${Python3_EXECUTABLE}" -B
                             "${CMAKE_CURRENT_SOURCE_DIR}/tools/lift/check_scaffold.py")
            set_tests_properties(lift_scaffold_inventory PROPERTIES TIMEOUT 60 LABELS "lift")
            add_test(NAME render_determinism
                     COMMAND "${Python3_EXECUTABLE}" -B
                             "${CMAKE_CURRENT_SOURCE_DIR}/tools/lift/render_determinism.py"
                             --tool "$<TARGET_FILE:stage_preview>"
                             --files "${SHADOW_EMBED_SYS_DIR}/../files")
            set_tests_properties(render_determinism PROPERTIES TIMEOUT 900 LABELS "lift;render")
            add_test(NAME lift_l1_evidence
                     COMMAND "${Python3_EXECUTABLE}" -B
                             "${CMAKE_CURRENT_SOURCE_DIR}/tools/lift/verify_l1_input.py"
                             --dol "${SHADOW_RECOMP_DOL}"
                             --pruned "${SHADOW_RECOMP_DIR}/recomp_pruned.csv")
            set_tests_properties(lift_l1_evidence PROPERTIES TIMEOUT 120 LABELS "lift")
            if(TARGET ShadowTheHedgehog)
                add_test(NAME lift_native_math_evidence
                         COMMAND "${Python3_EXECUTABLE}" -B
                                 "${CMAKE_CURRENT_SOURCE_DIR}/tools/lift/verify_native_math.py"
                                 --dol "${SHADOW_RECOMP_DOL}" --generated "${SHADOW_RECOMP_DIR}"
                                 --map "$<TARGET_FILE_DIR:ShadowTheHedgehog>/ShadowTheHedgehog.map")
                set_tests_properties(lift_native_math_evidence PROPERTIES TIMEOUT 400 LABELS "lift;native-contract")
            endif()
            add_test(NAME native_liveness_roots
                     COMMAND "${Python3_EXECUTABLE}" -B
                             "${CMAKE_CURRENT_SOURCE_DIR}/tools/ppcrecomp/liveness_roots.py"
                             --runtime "${CMAKE_CURRENT_SOURCE_DIR}/runtime"
                             --census "${CMAKE_CURRENT_SOURCE_DIR}/test_data/address-census-20261005-v1/census-v2.json"
                             --check "${CMAKE_CURRENT_SOURCE_DIR}/extract/liveness-roots.txt")
            set_tests_properties(native_liveness_roots PROPERTIES TIMEOUT 120 LABELS "native;build")
            # The lifted tree's roots: the same scan without the sources that tree does not compile.
            set(SHADOW_LIFTED_WITHOUT_ARGS)
            foreach(_source ${SHADOW_LIFTED_UNCOMPILED_SOURCES})
                list(APPEND SHADOW_LIFTED_WITHOUT_ARGS --without "${_source}")
            endforeach()
            add_test(NAME native_liveness_roots_lifted
                     COMMAND "${Python3_EXECUTABLE}" -B
                             "${CMAKE_CURRENT_SOURCE_DIR}/tools/ppcrecomp/liveness_roots.py"
                             --runtime "${CMAKE_CURRENT_SOURCE_DIR}/runtime"
                             --census "${CMAKE_CURRENT_SOURCE_DIR}/test_data/address-census-20261005-v1/census-v2.json"
                             ${SHADOW_LIFTED_WITHOUT_ARGS}
                             --base "${CMAKE_CURRENT_SOURCE_DIR}/extract/liveness-roots.txt"
                             --check "${CMAKE_CURRENT_SOURCE_DIR}/extract/liveness-roots-lifted.txt")
            set_tests_properties(native_liveness_roots_lifted PROPERTIES TIMEOUT 120 LABELS "native;build")
            # The game image's roots (a lifted tree without tests): the lifted scan without runtime/tests/.
            add_test(NAME native_liveness_roots_game
                     COMMAND "${Python3_EXECUTABLE}" -B
                             "${CMAKE_CURRENT_SOURCE_DIR}/tools/ppcrecomp/liveness_roots.py"
                             --runtime "${CMAKE_CURRENT_SOURCE_DIR}/runtime"
                             --census "${CMAKE_CURRENT_SOURCE_DIR}/test_data/address-census-20261005-v1/census-v2.json"
                             ${SHADOW_LIFTED_WITHOUT_ARGS} --without-tests
                             --base "${CMAKE_CURRENT_SOURCE_DIR}/extract/liveness-roots.txt"
                             --check "${CMAKE_CURRENT_SOURCE_DIR}/extract/liveness-roots-game.txt")
            set_tests_properties(native_liveness_roots_game PROPERTIES TIMEOUT 120 LABELS "native;build")
            # native/win (docs/NATIVE_WINDOWS_MIGRATION.md, docs/agents/native-tools.md): the migration tools' own unit
            # tests, and the ratchet on what of the GameCube execution environment the GAME IMAGE still links
            # (translated functions, native functions taking Cpu&, guest-machine code bytes). The image is another
            # tree's product (lifted renderer, no tests), so the test takes that tree's link map by path and is
            # registered only when one is given. The ratchet file holds the numbers of the gate configuration
            # (SHADOW_SHIPPING=OFF, SHADOW_WATCH_SYMBOLS=ON, SHADOW_BUILD_REFERENCE=ON); another configuration needs its
            # own file. Lower it with verify_native_runtime.py --write-ratchet after a step; never raise it.
            add_test(NAME native_migration_tools
                     COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover -s . -p "test_*.py")
            set_tests_properties(native_migration_tools PROPERTIES
                WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tools/native" TIMEOUT 60 LABELS "tools;validation")
            # Always registered. Without a map (the variable unset, or the image not built) the test FAILS and says
            # so: an unmeasured image is not a passing one. The designated run is
            #   ctest -C Release -R "^native_runtime_ratchet$" --no-tests=error
            # so that a tree in which the test is not registered at all fails too.
            set(SHADOW_NATIVE_RATCHET_MAP "" CACHE FILEPATH
                "Link map of the game image (gate configuration) for the native_runtime_ratchet test; unset = the test fails")
            add_test(NAME native_runtime_ratchet
                     COMMAND "${Python3_EXECUTABLE}" -B
                             "${CMAKE_CURRENT_SOURCE_DIR}/tools/native/verify_native_runtime.py"
                             --map "${SHADOW_NATIVE_RATCHET_MAP}"
                             --ratchet "${CMAKE_CURRENT_SOURCE_DIR}/tools/native/native_ratchet.json")
            set_tests_properties(native_runtime_ratchet PROPERTIES TIMEOUT 120 LABELS "tools;validation;native")
            # GameCube SDK removal (docs/GC_REMOVAL_PLAN.md): the generator's liveness graph classified by the
            # platform manifest. Fails when more platform-SDK bodies are compiled than the ratchet (a new hook or
            # literal pinned SDK code: the work list names what enters it), or when the planner's prediction for a
            # real ownership step (OSInit owned) differs from the generator's run. Lower the ratchet as services
            # are removed. Needs the pal-v30 manifest and skeleton (tools/dolrecon), so registered only with them.
            # Per configuration (measured 2026-11-02): the lifted renderer retires six more RenderWare GameCube
            # driver bodies, so the shipping (release, SHADOW_RENDER_LIFTED) tree compiles 1,057 and the tests tree
            # (not lifted) 1,063. Measured 2026-11-07 after the room reverb's init / shutdown were owned (step 2,
            # docs/GC_REMOVAL_PLAN.md) on top of the CRI and other owners since: shipping 1,044, tests tree 1,050.
            # Measured after merging agent/axfx-effects (ReverbStd / Chorus / Delay, docs/agents/axfx-effects.md):
            # shipping 1,024, tests tree 1,030. After merging agent/card-saves (the CARD library is not compiled,
            # docs/agents/card-saves.md): shipping 968, tests tree 974. After os-other step B (console-settings
            # accessors, docs/agents/os-other.md): shipping 963, tests tree 969. After agent/vi (VI owners) and
            # card-saves step 2 (OS interrupt APIs): shipping 949, tests tree 955. After vi step 2 (VIConfigure) and
            # sdk-identify's per-build roots (the lifted tree drops rw_capture-only roots): shipping 912, tests 935.
            # After VISetBlack, the OS arena / clock / UART owners and the GX state API: shipping 855, tests 877.
            # After sdk-identify's names, card-saves gxC / gxD (GX vertex path, GXInit sync), vi rwR1 / rwR4 (RW
            # device surface, world-sector pipeline; lifted build only) and the CRI bridge default ON: shipping 520,
            # tests tree 770. After vi step 6 (VISetPostRetraceCallback + twins), os-other (heap results-only, OSPanic)
            # and sdk-identify rwR2 1a / 1b (RW pixel conversion, raster format): shipping 506, tests tree 758.
            # After merge round 6 (native __start / OSInit / decrementer, OSReport + printf / fprintf, AX registration,
            # scheduler count, RW registry, 41 verified names): shipping 421, tests tree 684.
            # After merge round 7 (GX TEV / indirect setters + display-copy scale pair, debug.trk, RW Im3D entry points +
            # flat native-owner dispatch, OSGetConsoleType / DMAErrorHandler, the OS font library): shipping 363,
            # tests tree 637. After merge round 8 (the guest_call seam, the RW raster life cycle slice 1, the RW device
            # raster wrappers, the os.threads readers + OSInitThreadQueue, SPEC2_MakeStatus retired): shipping 349,
            # tests tree 625. After merge round 9 (the GX display-copy leaves and GXInitFifoBase, two BP setters,
            # MIXQuit / MIXSetSoundMode): shipping 326, tests tree 609. After merge round 12 (the GXInit tail state, the
            # palette lock pair, two RW device-state rows, eleven AX voice entries, the default exception handler,
            # PPCHalt): shipping 294, tests tree 582. After merge round 13 (the raster lock / unlock / tile layout, RasterSetImage
            # with its format bodies and converters, ImageGetRaster, two OS leaves): shipping 239, tests tree 527. After merge
            # round 14 (the jump-table fix for ImageGetRaster, seven MIX entries, the MIX allocators): shipping 209, tests 497.
            # After merge round 15 (native OSSleepThread / OSWakeupThread / OSYieldThread, the AX voice sync + native PB
            # image, the Im2D line / triangle emitters): shipping 195, tests tree 488. After merge round 16 (scheduler cuts 1-2b,
            # the raster life cycle, the RW registry / pool / world reader, NativeTextureWrite, vector transforms): shipping 149,
            # tests tree 468. SDK removal 2026-11-12 (docs/agents/SDK_2026-11-12.md): sdk-axfx (the AXFX aux callbacks),
            # sdk-rwnum (the five GcnRasterClearRect switch arms) and sdk-rwdrv (the GX default texture region callback, the three
            # raster-render wrappers, GcnTextureBind): shipping 16 -> measured at merge.
            if(SHADOW_RENDER_LIFTED)
                set(SHADOW_GC_REMOVAL_RATCHET_DEFAULT 0)
            else()
                set(SHADOW_GC_REMOVAL_RATCHET_DEFAULT 443)
            endif()
            set(SHADOW_GC_REMOVAL_RATCHET ${SHADOW_GC_REMOVAL_RATCHET_DEFAULT})  # not a cache entry: an old tree would keep its first value
            set(SHADOW_DOLRECON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.build/dolrecon" CACHE PATH
                "tools/dolrecon outputs the planner classifies with (pal-v30 manifest, skeleton)")
            if(EXISTS "${SHADOW_DOLRECON_DIR}/pal-v30/platform_manifest.json" AND
               EXISTS "${SHADOW_DOLRECON_DIR}/skeleton/functions.csv")
                add_test(NAME platform_gc_removal_plan
                         COMMAND "${Python3_EXECUTABLE}" -B
                                 "${CMAKE_CURRENT_SOURCE_DIR}/tools/platform/removal_plan.py"
                                 --release-dir "${CMAKE_CURRENT_BINARY_DIR}"
                                 --out "${CMAKE_CURRENT_BINARY_DIR}/platform/removal_plan"
                                 --dolrecon-dir "${SHADOW_DOLRECON_DIR}"
                                 --control-own 0x80372B80
                                 --max-platform-bodies ${SHADOW_GC_REMOVAL_RATCHET})
                set_tests_properties(platform_gc_removal_plan PROPERTIES TIMEOUT 900 LABELS "platform;build"
                                     FIXTURES_SETUP platform_removal_plan)
                # The runtime's own use of SDK internals (tools/platform/runtime_coupling.py): every use classified,
                # and no more native -> SDK calls / dispatches than measured (11 on 2026-11-02, 6 after RtGCNFS async retired; lower as removed).
                set(SHADOW_RUNTIME_SDK_CALLS_RATCHET 2)  # not a cache entry, same reason
                add_test(NAME platform_runtime_coupling
                         COMMAND "${Python3_EXECUTABLE}" -B
                                 "${CMAKE_CURRENT_SOURCE_DIR}/tools/platform/runtime_coupling.py"
                                 --plan "${CMAKE_CURRENT_BINARY_DIR}/platform/removal_plan/removal_plan.json"
                                 --out "${CMAKE_CURRENT_BINARY_DIR}/platform/runtime_coupling"
                                 --require-classified --max-sdk-calls ${SHADOW_RUNTIME_SDK_CALLS_RATCHET})
                set_tests_properties(platform_runtime_coupling PROPERTIES TIMEOUT 300 LABELS "platform;build"
                                     FIXTURES_REQUIRED platform_removal_plan)
                # Gate 2's static half (tools/platform/retired_readers.py) on two real steps of known outcome:
                # owning OSSetCurrentContext retires OS globals the rest of the OS reads (0x800000D4: readers must be
                # found; OSInit itself is a native owner since 2026-11-07, so it is no longer the control), owning
                # ReverbHiInit / Shutdown retires only pointer-addressed state (no proven reader, its unproven
                # stores reported for the dynamic watch).
                foreach(control IN ITEMS "context;0x80372B80;readers" "reverb;0x803884E4,0x80388554;no-proven-readers")
                    list(GET control 0 control_name)
                    list(GET control 1 control_own)
                    list(GET control 2 control_expect)
                    add_test(NAME platform_retired_readers_${control_name}
                             COMMAND "${Python3_EXECUTABLE}" -B
                                     "${CMAKE_CURRENT_SOURCE_DIR}/tools/platform/retired_readers.py"
                                     --own ${control_own} --release-dir "${CMAKE_CURRENT_BINARY_DIR}"
                                     --dolrecon-dir "${SHADOW_DOLRECON_DIR}"
                                     --graph "${CMAKE_CURRENT_BINARY_DIR}/platform/removal_plan/live_graph.txt"
                                     --out "${CMAKE_CURRENT_BINARY_DIR}/platform/retired_readers/${control_name}"
                                     --expect ${control_expect})
                    set_tests_properties(platform_retired_readers_${control_name} PROPERTIES TIMEOUT 900
                                         LABELS "platform;build" FIXTURES_REQUIRED platform_removal_plan)
                endforeach()
            endif()
            # The native RtGCNFS owner serves synchronous files only and refuses an asynchronous one: this proves from
            # the DOL's bytes that no caller can open asynchronously (every reference to the async-capable openers
            # accounted for, each mode argument checked), with three mutated images that must be refused.
            add_test(NAME platform_rtfs_async_proof
                     COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tools/platform/rtfs_async_proof.py"
                             "${SHADOW_RECOMP_DOL}" --self-test)
            set_tests_properties(platform_rtfs_async_proof PROPERTIES TIMEOUT 120 LABELS "platform;native-contract")
            add_test(NAME native_jump_tables
                     COMMAND "${Python3_EXECUTABLE}" -B
                             "${CMAKE_CURRENT_SOURCE_DIR}/tools/ppcrecomp/jump_tables.py"
                             --dol "${SHADOW_RECOMP_DOL}"
                             --functions "${SHADOW_RECOMP_FUNCTIONS}"
                             --check "${CMAKE_CURRENT_SOURCE_DIR}/extract/jump-tables.txt")
            set_tests_properties(native_jump_tables PROPERTIES TIMEOUT 120 LABELS "native;build")
            # The checked-in address map of every other release whose extracted disc is on this machine must be
            # exactly what disc_map.py derives from the two DOLs (a release that is absent has no test).
            set(SHADOW_DISC_SEARCH "${CMAKE_CURRENT_SOURCE_DIR}/../EXTRACTED_DISKS" CACHE PATH
                "Folder of extracted GameCube discs (<any>/sys/main.dol) whose address maps CTest checks")
            file(GLOB _shadow_disc_dols "${SHADOW_DISC_SEARCH}/*/sys/main.dol")
            foreach(_dol IN LISTS _shadow_disc_dols)
                execute_process(
                    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tools/ppcrecomp/disc_map.py"
                            identify --catalog "${CMAKE_CURRENT_SOURCE_DIR}/extract/discs.json" --dol "${_dol}"
                            --field game_id
                    OUTPUT_VARIABLE _id ERROR_QUIET RESULT_VARIABLE _rc OUTPUT_STRIP_TRAILING_WHITESPACE)
                if(_rc EQUAL 0 AND NOT _id STREQUAL "GUPP8P")
                    add_test(NAME disc_address_map_${_id}
                             COMMAND "${Python3_EXECUTABLE}" -B
                                     "${CMAKE_CURRENT_SOURCE_DIR}/tools/ppcrecomp/disc_map.py" check
                                     --catalog "${CMAKE_CURRENT_SOURCE_DIR}/extract/discs.json"
                                     --canonical "${SHADOW_RECOMP_DOL}" --dol "${_dol}"
                                     --functions "${SHADOW_RECOMP_FUNCTIONS}"
                                     --check "${CMAKE_CURRENT_SOURCE_DIR}/extract/address-maps/${_id}.map")
                    set_tests_properties(disc_address_map_${_id} PROPERTIES TIMEOUT 120 LABELS "native;build")
                endif()
            endforeach()
            add_test(NAME native_wait_points
                     COMMAND "${Python3_EXECUTABLE}" -B
                             "${CMAKE_CURRENT_SOURCE_DIR}/tools/ppcrecomp/wait_points.py"
                             --dol "${SHADOW_RECOMP_DOL}"
                             --functions "${SHADOW_RECOMP_FUNCTIONS}"
                             --retired "${CMAKE_CURRENT_SOURCE_DIR}/extract/wait-points-retired.txt"
                             --retired-lifted "${CMAKE_CURRENT_SOURCE_DIR}/extract/wait-points-retired-lifted.txt"
                             --check "${CMAKE_CURRENT_SOURCE_DIR}/extract/wait-points.txt")
            set_tests_properties(native_wait_points PROPERTIES TIMEOUT 120 LABELS "native;build")
            if(NOT SHADOW_RENDER_LIFTED)
                # A wait point retired for every build must belong to a loop the reference build cannot run (this tree is the reference).
                add_test(NAME native_wait_points_retired_reach
                         COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tools/platform/retired_wait_reach.py"
                                 --pruned "${SHADOW_RECOMP_DIR}/recomp_pruned.csv"
                                 --retired "${CMAKE_CURRENT_SOURCE_DIR}/extract/wait-points-retired.txt")
                set_tests_properties(native_wait_points_retired_reach PROPERTIES TIMEOUT 900 LABELS "native;build"
                                     ENVIRONMENT "SHADOW_RECOMP_DOL=${SHADOW_RECOMP_DOL}")
            endif()
            add_test(NAME native_wait_points_classifier
                     COMMAND "${Python3_EXECUTABLE}" -B -m unittest test_wait_points
                     WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tools/ppcrecomp")
            set_tests_properties(native_wait_points_classifier PROPERTIES TIMEOUT 60 LABELS "native;build")
            # Phase 4 (docs/native-time-20261005/GX_DATA_PLANE.md): the GX pipe site list the
            # generator lowers (--gx-sites) must be exactly what the inventory produces now.
            add_test(NAME native_gx_sites
                     COMMAND "${Python3_EXECUTABLE}" -B
                             "${CMAKE_CURRENT_SOURCE_DIR}/tools/ppcrecomp/gx_sites.py"
                             --dol "${SHADOW_RECOMP_DOL}"
                             --functions "${SHADOW_RECOMP_FUNCTIONS}"
                             --jump-tables "${CMAKE_CURRENT_SOURCE_DIR}/extract/jump-tables.txt"
                             --check "${CMAKE_CURRENT_SOURCE_DIR}/extract/gx-sites.txt")
            set_tests_properties(native_gx_sites PROPERTIES TIMEOUT 300 LABELS "native;build")
            add_test(NAME native_gx_sites_classifier
                     COMMAND "${Python3_EXECUTABLE}" -B -m unittest test_gx_sites
                     WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tools/ppcrecomp")
            set_tests_properties(native_gx_sites_classifier PROPERTIES TIMEOUT 60 LABELS "native;build")
            # Step K (docs/agents/axfx-effects.md): the static-closure tool is checked against addresses whose answer is known
            # (RxPipelineExecute closes; the registry init callback 0x80464D30 must come back LIVE).
            add_test(NAME platform_closure_proof_tool
                     COMMAND "${Python3_EXECUTABLE}" -B -m unittest test_closure_proof
                     WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tools/platform")
            set_tests_properties(platform_closure_proof_tool PROPERTIES TIMEOUT 300 LABELS "native;build")
            # rwR1 (docs/agents/vi.md "Step 4"): the RwDevice-surface owners call the scaffold functions their lowered
            # sites call, with the sites' arguments (lift-sites-r2.txt), and the accessor returns what the original computes.
            add_test(NAME native_device_surface_owners
                     COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_device_surface_owners.py"
                             "${SHADOW_RECOMP_DOL}")
            set_tests_properties(native_device_surface_owners PROPERTIES TIMEOUT 60 LABELS "native;build")
            # L0r0-01: one host ISA / FP model per image that contains gekko guest-semantics code.
            # The check reads the CMake file API codemodel, so ask CMake to write one for this tree.
            set(SHADOW_FILE_API_QUERY "${CMAKE_BINARY_DIR}/.cmake/api/v1/query/client-gekkoforge/codemodel-v2")
            if(NOT EXISTS "${SHADOW_FILE_API_QUERY}")
                file(WRITE "${SHADOW_FILE_API_QUERY}" "")
            endif()
            set(SHADOW_ISA_REQUIRE)
            if(TARGET ShadowTheHedgehog)
                set(SHADOW_ISA_REQUIRE --require ShadowTheHedgehog)
            endif()
            add_test(NAME native_host_isa_consistency
                     COMMAND "${Python3_EXECUTABLE}" -B
                             "${CMAKE_CURRENT_SOURCE_DIR}/tools/ppcrecomp/check_host_isa_consistency.py"
                             --build "${CMAKE_BINARY_DIR}" --source "${CMAKE_CURRENT_SOURCE_DIR}"
                             --config $<CONFIG> --dumpbin "${SHADOW_DUMPBIN}" --linker "${CMAKE_LINKER}"
                             --cmake "${CMAKE_COMMAND}" --work "${CMAKE_CURRENT_BINARY_DIR}/Testing/host-isa-consistency"
                             ${SHADOW_ISA_REQUIRE})
            set_tests_properties(native_host_isa_consistency PROPERTIES TIMEOUT 300 LABELS "native;build")
            if(TARGET ShadowTheHedgehog)
                # L0r0-02: the images check CPUID/XGETBV for the SHADOW_X64_ARCH contract before the first
                # instruction compiled for it runs (initializer order, gate code, measured extension use,
                # refusal on the real image, presence in every ISA image).
                set(SHADOW_ISA_GATE_IMAGES
                    --image ShadowTheHedgehog "$<TARGET_FILE:ShadowTheHedgehog>"
                            "$<TARGET_FILE_DIR:ShadowTheHedgehog>/ShadowTheHedgehog.map")
                if(TARGET ShadowTheHedgehogReference)
                    list(APPEND SHADOW_ISA_GATE_IMAGES
                         --optional-image ShadowTheHedgehogReference "$<TARGET_FILE:ShadowTheHedgehogReference>"
                                          "$<TARGET_FILE_DIR:ShadowTheHedgehogReference>/ShadowTheHedgehogReference.map")
                endif()
                add_test(NAME native_host_isa_gate
                         COMMAND "${Python3_EXECUTABLE}" -B
                                 "${CMAKE_CURRENT_SOURCE_DIR}/tools/ppcrecomp/check_host_isa_gate.py"
                                 --build "${CMAKE_BINARY_DIR}" --source "${CMAKE_CURRENT_SOURCE_DIR}"
                                 --config $<CONFIG> --dumpbin "${SHADOW_DUMPBIN}" --cmake "${CMAKE_COMMAND}"
                                 --arch "${SHADOW_X64_ARCH}"
                                 --work "${CMAKE_CURRENT_BINARY_DIR}/Testing/host-isa-gate"
                                 ${SHADOW_ISA_GATE_IMAGES})
                set_tests_properties(native_host_isa_gate PROPERTIES TIMEOUT 900 LABELS "native;build")
            endif()
            if(TARGET ShadowTheHedgehog)
                add_test(NAME native_executable_boundary
                         COMMAND "${Python3_EXECUTABLE}" -B
                                 "${CMAKE_CURRENT_SOURCE_DIR}/tools/ppcrecomp/check_native_boundary.py"
                                 --dumpbin "${SHADOW_DUMPBIN}"
                                 --native-library "$<TARGET_FILE:shadow_runtime>"
                                 --reference-library "$<TARGET_FILE:shadow_runtime_reference>"
                                 --executable "$<TARGET_FILE:ShadowTheHedgehog>"
                                 --map "$<TARGET_FILE_DIR:ShadowTheHedgehog>/ShadowTheHedgehog.map")
                set_tests_properties(native_executable_boundary PROPERTIES TIMEOUT 120 LABELS "native;build")
                foreach(reference_option no-recomp verify-recomp trace)
                    add_test(NAME native_rejects_${reference_option}
                             COMMAND "${Python3_EXECUTABLE}" -B -c
                                     "import subprocess,sys; p=subprocess.run([sys.argv[1],sys.argv[2],'--no-boot'],capture_output=True,text=True,timeout=15); print(p.stdout+p.stderr); sys.exit(0 if p.returncode==2 and 'reference-only options' in p.stderr else 1)"
                                     "$<TARGET_FILE:ShadowTheHedgehog>" "--${reference_option}")
                    set_tests_properties(native_rejects_${reference_option} PROPERTIES TIMEOUT 20 LABELS "native;cli")
                endforeach()
            endif()
        endif()
    endif()
endif()

# The settings program (Settings.exe, target ShadowPadConfig; "Settings" in the Start menu): installed beside the game, it edits the
# display.cfg, options.cfg, pad.cfg and controls.cfg the game reads at start (docs/PC_SETTINGS_FILES.md).
if(WIN32 AND TARGET ShadowTheHedgehog AND SHADOW_PREBUILT_USED)
    # The prebuilt configurator, put where the build would have written it.
    add_custom_target(ShadowPadConfig ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:ShadowTheHedgehog>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SHADOW_PREBUILT_USED}/ShadowPadConfig.exe"
                "$<TARGET_FILE_DIR:ShadowTheHedgehog>/Settings.exe"
        DEPENDS "${SHADOW_PREBUILT_USED}/ShadowPadConfig.exe"
        COMMENT "Using the prebuilt settings program (Settings.exe)"
        VERBATIM)
elseif(WIN32 AND (TARGET ShadowTheHedgehog OR SHADOW_PREBUILT_EXPORT))
    # The game's own parsers and line-preserving writer are compiled in, so the program reads and writes the files as
    # the game does.
    add_executable(ShadowPadConfig WIN32 tools/padconfig/main.cpp tools/padconfig/pad_config.cpp tools/padconfig/controller_view.cpp tools/padconfig/padconfig.rc
                   tools/padconfig/settings_model.cpp runtime/src/gekko/config_file.cpp
                   runtime/lift/peripheral/gamepad_binding.cpp runtime/lift/peripheral/action_map.cpp
                   runtime/lift/peripheral/controls.cpp tools/ui/appearance.cpp tools/ui/theme.cpp)
    target_include_directories(ShadowPadConfig PRIVATE runtime/include runtime/lift tools/padconfig tools/ui)
    target_compile_features(ShadowPadConfig PRIVATE cxx_std_20)
    target_link_libraries(ShadowPadConfig PRIVATE comctl32 gdiplus dwmapi uxtheme)
    set_source_files_properties(tools/padconfig/padconfig.rc PROPERTIES OBJECT_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/padconfig/art/top.png;${CMAKE_CURRENT_SOURCE_DIR}/tools/padconfig/art/tilted.png;${CMAKE_CURRENT_SOURCE_DIR}/tools/padconfig/art/top_body.png;${CMAKE_CURRENT_SOURCE_DIR}/tools/padconfig/art/top_stick_main.png;${CMAKE_CURRENT_SOURCE_DIR}/tools/padconfig/art/top_stick_c.png;${CMAKE_CURRENT_SOURCE_DIR}/tools/padconfig/art/top_stick_cap_main.png;${CMAKE_CURRENT_SOURCE_DIR}/tools/padconfig/art/top_stick_cap_c.png")
    # Beside the game (bin/<config>, the game's RUNTIME_OUTPUT_DIRECTORY).
    set_target_properties(ShadowPadConfig PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin" OUTPUT_NAME Settings)
    if(MSVC)
        target_compile_options(ShadowPadConfig PRIVATE /W4 /permissive-)
        set_property(TARGET ShadowPadConfig PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    if(SHADOW_BUILD_TESTS)
        add_executable(pad_config_tests runtime/tests/pad_config_tests.cpp tools/padconfig/pad_config.cpp
                       runtime/src/gekko/config_file.cpp runtime/lift/peripheral/gamepad_binding.cpp)
        target_include_directories(pad_config_tests PRIVATE runtime/include runtime/lift runtime/tests tools/padconfig)
        target_compile_features(pad_config_tests PRIVATE cxx_std_20)
        if(MSVC)
            target_compile_options(pad_config_tests PRIVATE /W4 /permissive-)
            set_property(TARGET pad_config_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME pad_config_tests COMMAND pad_config_tests)
        set_tests_properties(pad_config_tests PROPERTIES TIMEOUT 60 LABELS "menu")
        # The settings program's files against the game's own loaders (display.cpp's parse_display_file,
        # pc_settings.cpp, action_map.cpp, gamepad_binding.cpp from shadow_lift): round trips keep other lines, only
        # changed keys are written, ranges are enforced, the defaults are the game's.
        add_executable(configurator_settings_tests runtime/tests/configurator_settings_tests.cpp
                       tools/padconfig/settings_model.cpp tools/padconfig/pad_config.cpp
                       runtime/src/gekko/config_file.cpp runtime/src/gekko/pc_settings.cpp
                       runtime/src/gekko/display.cpp runtime/src/gekko/camera.cpp runtime/src/gekko/host_jobs.cpp)
        target_include_directories(configurator_settings_tests PRIVATE runtime/include runtime/lift runtime/tests tools/padconfig)
        target_link_libraries(configurator_settings_tests PRIVATE shadow_lift)
        target_compile_features(configurator_settings_tests PRIVATE cxx_std_20)
        if(MSVC)
            target_compile_options(configurator_settings_tests PRIVATE /W4 /permissive-)
            set_property(TARGET configurator_settings_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME configurator_settings_tests COMMAND configurator_settings_tests)
        set_tests_properties(configurator_settings_tests PROPERTIES TIMEOUT 60 LABELS "menu")
        # appearance.cfg, the text size and colours the settings program and the mod manager share.
        add_executable(ui_appearance_tests runtime/tests/ui_appearance_tests.cpp tools/ui/appearance.cpp)
        target_include_directories(ui_appearance_tests PRIVATE runtime/tests tools/ui)
        target_compile_features(ui_appearance_tests PRIVATE cxx_std_20)
        if(MSVC)
            target_compile_options(ui_appearance_tests PRIVATE /W4 /permissive-)
            set_property(TARGET ui_appearance_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME ui_appearance_tests COMMAND ui_appearance_tests)
        set_tests_properties(ui_appearance_tests PROPERTIES TIMEOUT 60 LABELS "menu")
    endif()
endif()

# gekko/system.h changes System's members under SHADOW_RENDER_LIFTED, and the runtime is compiled with it in a lifted
# tree. A test that includes the header without it reads the wrong offsets, so every program linking the runtime gets it.
if(SHADOW_RENDER_LIFTED AND SHADOW_BUILD_TESTS)
    get_property(shadow_all_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    foreach(shadow_target IN LISTS shadow_all_targets)
        get_target_property(shadow_target_type ${shadow_target} TYPE)
        if(shadow_target_type STREQUAL "EXECUTABLE" AND NOT shadow_target MATCHES "^ShadowTheHedgehog")
            get_target_property(shadow_target_links ${shadow_target} LINK_LIBRARIES)
            if(shadow_target_links MATCHES "shadow_(runtime|recomp)")
                target_compile_definitions(${shadow_target} PRIVATE SHADOW_RENDER_LIFTED=1)
            endif()
        endif()
    endforeach()
endif()

# ---- the prebuilt parts, used or exported (see "prebuilt game-code-free parts" near the top) -------------------------
if(SHADOW_PREBUILT_USED)
    # Each prebuilt library keeps its target, and with it every usage requirement (include directories, definitions,
    # compile features, link order): only its compiled code is the release step's. The target compiles one empty file
    # and its archive step folds the prebuilt archive in (lib.exe merges a .lib given as input), so the link line is
    # the one a full build has, item for item.
    set(SHADOW_PREBUILT_STUB "${CMAKE_BINARY_DIR}/prebuilt_stub.cpp")
    file(WRITE "${SHADOW_PREBUILT_STUB}" "// Empty: this target's code is the prebuilt archive merged into it.\n")
    set(_shadow_prebuilt_archives "")
    foreach(_target IN LISTS SHADOW_PREBUILT_LIBRARIES SHADOW_PREBUILT_FOLDED_OBJECT_LIBRARIES)
        if(NOT TARGET ${_target})
            continue()
        endif()
        # The sources that are never prebuilt stay in their target and are compiled here.
        get_target_property(_sources ${_target} SOURCES)
        set(_local "")
        foreach(_source IN LISTS _sources)
            if("${_source}" IN_LIST SHADOW_PREBUILT_LOCAL_SOURCES)
                list(APPEND _local "${_source}")
            endif()
        endforeach()
        set_property(TARGET ${_target} PROPERTY SOURCES "${SHADOW_PREBUILT_STUB}" ${_local})
        if("${_target}" IN_LIST SHADOW_PREBUILT_LIBRARIES)
            set(_archive "${SHADOW_PREBUILT_USED}/${_target}.lib")
            # LNK4221: the empty object defines no symbol, which is the point.
            set_property(TARGET ${_target} APPEND PROPERTY STATIC_LIBRARY_OPTIONS "${_archive}" /IGNORE:4221)
            list(APPEND _shadow_prebuilt_archives "${_archive}")
        endif()
    endforeach()
    set_property(SOURCE "${SHADOW_PREBUILT_STUB}" PROPERTY OBJECT_DEPENDS "${_shadow_prebuilt_archives}")
elseif(SHADOW_PREBUILT_EXPORT)
    # The sources that are never prebuilt are built here as in a full build (a prebuilt program may link them:
    # ShadowModManager links shadow_mods, afs.cpp included, so it is the program a full build makes), and the release
    # step removes their objects from the exported archives (lib.exe /REMOVE). It reads their object names here.
    set(_shadow_local_objects "")
    foreach(_target IN LISTS SHADOW_PREBUILT_LIBRARIES SHADOW_PREBUILT_FOLDED_OBJECT_LIBRARIES)
        if(TARGET ${_target})
            get_target_property(_sources ${_target} SOURCES)
            foreach(_source IN LISTS _sources)
                if("${_source}" IN_LIST SHADOW_PREBUILT_LOCAL_SOURCES)
                    get_filename_component(_stem "${_source}" NAME_WE)
                    string(APPEND _shadow_local_objects "${_stem}.obj\n")
                endif()
            endforeach()
        endif()
    endforeach()
    file(WRITE "${CMAKE_BINARY_DIR}/prebuilt-export/local-objects.txt" "${_shadow_local_objects}")
    # The release step reads this list: target name and built file, one per line.
    set(_shadow_export "")
    foreach(_target IN LISTS SHADOW_PREBUILT_LIBRARIES SHADOW_PREBUILT_PROGRAMS)
        if(TARGET ${_target})
            string(APPEND _shadow_export "${_target}=$<TARGET_FILE:${_target}>\n")
        endif()
    endforeach()
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/prebuilt-export/targets-$<CONFIG>.txt" CONTENT "${_shadow_export}")
endif()
