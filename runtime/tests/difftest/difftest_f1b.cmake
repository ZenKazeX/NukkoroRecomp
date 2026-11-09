# F1b differential harness: exhaustive practical coverage (FP, paired singles, supervisor instructions,
# block-boundary sequences, interrupt recognition). Native time (2026-10-06, docs/native-time-20261005/DESIGN.md):
# the vectors that raised a line or placed a decrementer edge at a retired-instruction count are retired (no
# instruction count exists; a line asserted before entry is taken at the first recognition point), lanes NB/N1
# are retired with the instruction budget, and native lanes carry no retired count (gen_f1b.py, compare_f1b.py).
# Documentation: docs/recompiler-core-audit-2026-09-13/infrastructure/DIFFTEST_F1B.md
#
# Included from runtime/tests/difftest/difftest.cmake (itself inside `if(SHADOW_BUILD_TESTS)` /
# `if(Python3_Interpreter_FOUND)`). Test-only: nothing here changes shadow_runtime, shadow_recomp,
# ppcrecomp or any shipped output.
#
#   1. tools/difftest/gen_f1b.py writes a synthetic DOL (text0 at 0x80010000, real-mode exception handlers
#      in text1 at 0x00000500) and runs the REAL ppcrecomp with the production arguments (gap sweep on)
#      into ${SHADOW_DIFFTEST_F1B_DIR}.
#   2. difftest_f1b_generated compiles that output with shadow_recomp's code-generation flags.
#   3. difftest_f1b_driver_prod (shadow_runtime) and difftest_f1b_driver_ref (shadow_runtime_reference)
#      link the same objects and the shared driver source; guest code is entered only through Cpu::run.
#   4. ctest difftest_f1b runs tools/difftest/run_difftest_f1b.py: both drivers (lanes N NL / R RL I IF IR)
#      and the comparator, which computes the spec lane in-process.

set(SHADOW_DIFFTEST_F1B_SEED "20260915" CACHE STRING "difftest F1b vector generator seed")
set(SHADOW_DIFFTEST_F1B_SCALE "1.0" CACHE STRING "difftest F1b family count multiplier")
set(SHADOW_DIFFTEST_F1B_JOBS "8" CACHE STRING "difftest F1b comparator worker processes")
set(SHADOW_DIFFTEST_F1B_DIR "${CMAKE_CURRENT_BINARY_DIR}/Testing/difftest-f1b")
set(SHADOW_DIFFTEST_F1B_SHARDS 8)
set(difftest_f1b_sources)
foreach(shard RANGE 0 7)
    string(REGEX REPLACE "^(.)$" "00\\1" shard_name "${shard}")
    list(APPEND difftest_f1b_sources "${SHADOW_DIFFTEST_F1B_DIR}/recomp_${shard_name}.cpp")
endforeach()
list(APPEND difftest_f1b_sources "${SHADOW_DIFFTEST_F1B_DIR}/recomp_table.cpp")
file(GLOB difftest_f1b_spec_files CONFIGURE_DEPENDS "${SHADOW_DIFFTEST_SPEC}/*.py")

add_custom_command(
    OUTPUT ${difftest_f1b_sources}
           "${SHADOW_DIFFTEST_F1B_DIR}/recomp_decls.h"
           "${SHADOW_DIFFTEST_F1B_DIR}/vectors.txt"
           "${SHADOW_DIFFTEST_F1B_DIR}/vectors.jsonl"
    COMMAND "${Python3_EXECUTABLE}" -B "${SHADOW_DIFFTEST_TOOL}/gen_f1b.py"
            --generator $<TARGET_FILE:ppcrecomp>
            --out "${SHADOW_DIFFTEST_F1B_DIR}"
            --seed "${SHADOW_DIFFTEST_F1B_SEED}"
            --scale "${SHADOW_DIFFTEST_F1B_SCALE}"
            --shards ${SHADOW_DIFFTEST_F1B_SHARDS}
            --spec "${SHADOW_DIFFTEST_SPEC}"
    DEPENDS ppcrecomp
            "${SHADOW_DIFFTEST_TOOL}/gen_f1b.py"
            "${SHADOW_DIFFTEST_TOOL}/gen_vectors.py"
            "${SHADOW_DIFFTEST_TOOL}/ppcenc.py"
            "${SHADOW_DIFFTEST_TOOL}/ppcenc_ext.py"
            "${SHADOW_DIFFTEST_TOOL}/emitter_classes.py"
            "${SHADOW_DIFFTEST_TOOL}/spec_runner.py"
            "${SHADOW_DIFFTEST_TOOL}/spec_runner_f1b.py"
            ${difftest_f1b_spec_files}
    COMMENT "difftest F1b: generating vectors and running ppcrecomp with the production arguments"
    VERBATIM)

add_library(difftest_f1b_generated OBJECT ${difftest_f1b_sources})
# The generated table calls the lifted subsystems (gekko/lifted.h).
target_link_libraries(difftest_f1b_generated PRIVATE shadow_lift)
target_include_directories(difftest_f1b_generated PRIVATE "${SHADOW_DIFFTEST_F1B_DIR}"
                           "${CMAKE_CURRENT_SOURCE_DIR}/runtime/include")
target_compile_features(difftest_f1b_generated PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(difftest_f1b_generated PRIVATE /W4 /permissive- /wd4702 /wd4102 /wd4065
                                                          /bigobj /MP8 ${SHADOW_ARCH_FLAGS})
    set_property(TARGET difftest_f1b_generated PROPERTY MSVC_RUNTIME_LIBRARY
                 "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

foreach(difftest_flavor prod ref)
    set(difftest_target difftest_f1b_driver_${difftest_flavor})
    add_executable(${difftest_target} runtime/tests/difftest/difftest_driver.cpp
                   $<TARGET_OBJECTS:difftest_f1b_generated>)
    add_dependencies(${difftest_target} difftest_f1b_generated)
    if(difftest_flavor STREQUAL "ref")
        target_link_libraries(${difftest_target} PRIVATE shadow_runtime_reference)
        target_compile_definitions(${difftest_target} PRIVATE DIFFTEST_REFERENCE=1)
    else()
        target_link_libraries(${difftest_target} PRIVATE shadow_runtime)
    endif()
    if(MSVC)
        target_compile_options(${difftest_target} PRIVATE /W4 /permissive- ${SHADOW_ARCH_FLAGS})
        set_property(TARGET ${difftest_target} PROPERTY MSVC_RUNTIME_LIBRARY
                     "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_link_options(${difftest_target} PRIVATE /STACK:16777216)
    endif()
endforeach()

add_test(NAME difftest_f1b
         COMMAND "${Python3_EXECUTABLE}" -B "${SHADOW_DIFFTEST_TOOL}/run_difftest_f1b.py"
                 --prod $<TARGET_FILE:difftest_f1b_driver_prod>
                 --ref $<TARGET_FILE:difftest_f1b_driver_ref>
                 --generated "${SHADOW_DIFFTEST_F1B_DIR}"
                 --spec "${SHADOW_DIFFTEST_SPEC}"
                 --known "${SHADOW_DIFFTEST_TOOL}/known_candidates_f1b.json"
                 --seed "${SHADOW_DIFFTEST_F1B_SEED}" --scale "${SHADOW_DIFFTEST_F1B_SCALE}"
                 --jobs "${SHADOW_DIFFTEST_F1B_JOBS}")
set_tests_properties(difftest_f1b PROPERTIES TIMEOUT 1800 LABELS "reference;native;codegen;differential")
