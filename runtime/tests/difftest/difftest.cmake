# F1a differential harness: spec model vs reference interpreter vs generated native code.
# Documentation: docs/recompiler-core-audit-2026-09-13/infrastructure/DIFFTEST_F1A.md
#
# Included from CMakeLists.txt inside `if(SHADOW_BUILD_TESTS)` / `if(Python3_Interpreter_FOUND)`.
# Test-only: nothing here changes shadow_runtime, shadow_recomp, ppcrecomp or any shipped output.
#
#   1. tools/difftest/gen_vectors.py writes a synthetic DOL + vectors and runs the REAL ppcrecomp
#      with the production arguments (--dol --functions --out --shards --report --vectors; gap
#      sweep on) into ${SHADOW_DIFFTEST_DIR}.
#   2. difftest_f1a_generated compiles that output with exactly shadow_recomp's code-generation
#      flags (/W4 /permissive- /wd4702 /wd4102 /wd4065 /bigobj ${SHADOW_ARCH_FLAGS}).
#   3. difftest_driver_prod (shadow_runtime) and difftest_driver_ref (shadow_runtime_reference)
#      link the same objects and enter guest code only through Cpu::run.
#   4. ctest difftest_f1a runs tools/difftest/run_difftest.py: both drivers, the spec runner and
#      the comparator.

set(SHADOW_DIFFTEST_SEED "20260914" CACHE STRING "difftest F1a vector generator seed")
set(SHADOW_DIFFTEST_SCALE "1.0" CACHE STRING "difftest F1a family count multiplier")
set(SHADOW_DIFFTEST_SPEC "${CMAKE_CURRENT_SOURCE_DIR}/test_data/core-audit/spec" CACHE PATH
    "Directory of the blind Gekko spec model (gekko_state.py, int.py, branch.py, loadstore.py)")
set(SHADOW_DIFFTEST_DIR "${CMAKE_CURRENT_BINARY_DIR}/Testing/difftest-f1a")
set(SHADOW_DIFFTEST_TOOL "${CMAKE_CURRENT_SOURCE_DIR}/tools/difftest")
# gen_vectors.py --shards; the OUTPUT list below must name exactly these shards.
set(SHADOW_DIFFTEST_SHARDS 4)
set(difftest_f1a_sources
    "${SHADOW_DIFFTEST_DIR}/recomp_000.cpp"
    "${SHADOW_DIFFTEST_DIR}/recomp_001.cpp"
    "${SHADOW_DIFFTEST_DIR}/recomp_002.cpp"
    "${SHADOW_DIFFTEST_DIR}/recomp_003.cpp"
    "${SHADOW_DIFFTEST_DIR}/recomp_table.cpp")

add_custom_command(
    OUTPUT ${difftest_f1a_sources}
           "${SHADOW_DIFFTEST_DIR}/recomp_decls.h"
           "${SHADOW_DIFFTEST_DIR}/vectors.txt"
           "${SHADOW_DIFFTEST_DIR}/vectors.jsonl"
    COMMAND "${Python3_EXECUTABLE}" -B "${SHADOW_DIFFTEST_TOOL}/gen_vectors.py"
            --generator $<TARGET_FILE:ppcrecomp>
            --out "${SHADOW_DIFFTEST_DIR}"
            --seed "${SHADOW_DIFFTEST_SEED}"
            --scale "${SHADOW_DIFFTEST_SCALE}"
            --shards ${SHADOW_DIFFTEST_SHARDS}
            --spec "${SHADOW_DIFFTEST_SPEC}"
    DEPENDS ppcrecomp
            "${SHADOW_DIFFTEST_TOOL}/gen_vectors.py"
            "${SHADOW_DIFFTEST_TOOL}/ppcenc.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/runtime/include/gekko/ppc_instruction_traits.h"
    COMMENT "difftest F1a: generating vectors and running ppcrecomp with the production arguments"
    VERBATIM)

add_library(difftest_f1a_generated OBJECT ${difftest_f1a_sources})
# The generated table calls the lifted subsystems (gekko/lifted.h).
target_link_libraries(difftest_f1a_generated PRIVATE shadow_lift)
target_include_directories(difftest_f1a_generated PRIVATE "${SHADOW_DIFFTEST_DIR}"
                           "${CMAKE_CURRENT_SOURCE_DIR}/runtime/include")
target_compile_features(difftest_f1a_generated PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(difftest_f1a_generated PRIVATE /W4 /permissive- /wd4702 /wd4102 /wd4065
                                                          /bigobj /MP8 ${SHADOW_ARCH_FLAGS})
    set_property(TARGET difftest_f1a_generated PROPERTY MSVC_RUNTIME_LIBRARY
                 "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

foreach(difftest_flavor prod ref)
    set(difftest_target difftest_driver_${difftest_flavor})
    add_executable(${difftest_target} runtime/tests/difftest/difftest_driver.cpp
                   $<TARGET_OBJECTS:difftest_f1a_generated>)
    add_dependencies(${difftest_target} difftest_f1a_generated)
    if(difftest_flavor STREQUAL "ref")
        target_link_libraries(${difftest_target} PRIVATE shadow_runtime_reference)
        target_compile_definitions(${difftest_target} PRIVATE DIFFTEST_REFERENCE=1)
    else()
        target_link_libraries(${difftest_target} PRIVATE shadow_runtime)
    endif()
    if(MSVC)
        # Same ISA flags as the generated objects, so the inline ppc_ops.h helpers the driver
        # sees cannot be a different (ODR-merged) variant from the ones the shards use.
        target_compile_options(${difftest_target} PRIVATE /W4 /permissive- ${SHADOW_ARCH_FLAGS})
        set_property(TARGET ${difftest_target} PROPERTY MSVC_RUNTIME_LIBRARY
                     "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_link_options(${difftest_target} PRIVATE /STACK:16777216)
    endif()
endforeach()

add_test(NAME difftest_f1a
         COMMAND "${Python3_EXECUTABLE}" -B "${SHADOW_DIFFTEST_TOOL}/run_difftest.py"
                 --prod $<TARGET_FILE:difftest_driver_prod>
                 --ref $<TARGET_FILE:difftest_driver_ref>
                 --generated "${SHADOW_DIFFTEST_DIR}"
                 --spec "${SHADOW_DIFFTEST_SPEC}"
                 --known "${SHADOW_DIFFTEST_TOOL}/known_candidates_f1a.json"
                 --seed "${SHADOW_DIFFTEST_SEED}" --scale "${SHADOW_DIFFTEST_SCALE}")
set_tests_properties(difftest_f1a PROPERTIES TIMEOUT 900 LABELS "reference;native;codegen;differential")

# F1b (exhaustive practical coverage): separate generated set, drivers and ctest entry.
include("${CMAKE_CURRENT_LIST_DIR}/difftest_f1b.cmake")

# F1c (host-stack integrity and long-run corruption canaries): generated set, drivers and ctest entries.
include("${CMAKE_CURRENT_LIST_DIR}/difftest_f1c.cmake")
