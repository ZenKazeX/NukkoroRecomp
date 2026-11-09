# F1c: host-stack integrity and long-run corruption canary tests (test-only).
# Documentation: docs/recompiler-core-audit-2026-09-13/infrastructure/F1C_HOST_STACK_AND_CANARY.md
#
# Included from runtime/tests/difftest/difftest.cmake (inside `if(SHADOW_BUILD_TESTS)` /
# `if(Python3_Interpreter_FOUND)`, after difftest_f1b.cmake). Nothing here changes shadow_runtime,
# shadow_recomp, ppcrecomp or any shipped output.
#
#   1. tools/difftest/gen_f1c.py writes a synthetic DOL of guest loops closed through every control-transfer
#      shape (tiny, game-frame and game-body variants) plus the canary programs and real-mode handlers, and runs
#      the REAL ppcrecomp with the production arguments (gap sweep on) into ${SHADOW_F1C_DIR}.
#   2. difftest_f1c_generated compiles that output with shadow_recomp's code-generation flags.
#   3. f1c_stack_driver (shadow_runtime) measures the host stack of each loop over 1,000,000 iterations
#      (f1c_stack_driver_ref, on shadow_runtime_reference, supplies the interpreter's guest state for comparison);
#      f1c_canary_driver (shadow_runtime + a guard-page operator new) runs the long-run canary phases;
#      difftest_f1b_driver_prod_guarded is the unmodified F1b production driver with the guard allocator.
#      Native time (docs/native-time-20261005/DESIGN.md): the runtime counts no instructions and has no per-block
#      hardware tick, so the drivers sample the host stack with declining HLE probes at translated entry thunks
#      (run_f1c_stack.py's `tick1` sampler name selects that probe) and the canary's device acts at those probes.
#   4. ctest:
#        f1c_host_stack_integrity   must pass (R12 FIXED 2026-09-28: LK=0 transfers are published, not called)
#        f1c_host_stack_integrity_notail  must pass: the same generated objects at /Od, where MSVC performs no
#                                   tail-call optimisation at all, so boundedness cannot rest on a compiler choice
#        f1c_canary_longrun         must pass (phases A and B1)
#        f1c_guarded_f1b_vectors    must pass
#      f1c_counter_width_probe (canary phase B3) and canary phase B2 are RETIRED (native time, 2026-10-06): see below.
#      No test here is regression-pending any more (f1c_host_stack_game in difftest_f1c_game.cmake neither).

set(SHADOW_F1C_DIR "${CMAKE_CURRENT_BINARY_DIR}/Testing/difftest-f1c")
# Results live outside the generated directory: gen_f1c.py recreates ${SHADOW_F1C_DIR} on every regeneration.
set(SHADOW_F1C_RESULTS "${CMAKE_CURRENT_BINARY_DIR}/Testing/difftest-f1c-results")
set(SHADOW_F1C_SHARDS 8)
set(difftest_f1c_sources)
foreach(shard RANGE 0 7)
    string(REGEX REPLACE "^(.)$" "00\\1" shard_name "${shard}")
    list(APPEND difftest_f1c_sources "${SHADOW_F1C_DIR}/recomp_${shard_name}.cpp")
endforeach()
list(APPEND difftest_f1c_sources "${SHADOW_F1C_DIR}/recomp_table.cpp")

add_custom_command(
    OUTPUT ${difftest_f1c_sources}
           "${SHADOW_F1C_DIR}/recomp_decls.h"
           "${SHADOW_F1C_DIR}/f1c_fixture.h"
           "${SHADOW_F1C_DIR}/f1c_fixture.json"
    COMMAND "${Python3_EXECUTABLE}" -B "${SHADOW_DIFFTEST_TOOL}/gen_f1c.py"
            --generator $<TARGET_FILE:ppcrecomp>
            --out "${SHADOW_F1C_DIR}"
            --shards ${SHADOW_F1C_SHARDS}
    DEPENDS ppcrecomp
            "${SHADOW_DIFFTEST_TOOL}/gen_f1c.py"
            "${SHADOW_DIFFTEST_TOOL}/f1c_canary_programs.py"
            "${SHADOW_DIFFTEST_TOOL}/ppcenc.py"
            "${SHADOW_DIFFTEST_TOOL}/ppcenc_ext.py"
            "${SHADOW_DIFFTEST_TOOL}/emitter_classes.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/runtime/include/gekko/ppc_instruction_traits.h"
    COMMENT "difftest F1c: generating loop shapes and canary programs, running ppcrecomp with the production arguments"
    VERBATIM)

add_library(difftest_f1c_generated OBJECT ${difftest_f1c_sources})
# The generated table calls the lifted subsystems (gekko/lifted.h).
target_link_libraries(difftest_f1c_generated PRIVATE shadow_lift)
target_include_directories(difftest_f1c_generated PRIVATE "${SHADOW_F1C_DIR}"
                           "${CMAKE_CURRENT_SOURCE_DIR}/runtime/include")
target_compile_features(difftest_f1c_generated PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(difftest_f1c_generated PRIVATE /W4 /permissive- /wd4702 /wd4102 /wd4065
                                                          /bigobj /MP8 ${SHADOW_ARCH_FLAGS})
    set_property(TARGET difftest_f1c_generated PROPERTY MSVC_RUNTIME_LIBRARY
                 "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(f1c_test_dir "${CMAKE_CURRENT_SOURCE_DIR}/runtime/tests/difftest")
foreach(f1c_driver stack canary)
    set(f1c_target f1c_${f1c_driver}_driver)
    set(f1c_extra_sources "${f1c_test_dir}/f1c_host.cpp")
    if(f1c_driver STREQUAL "canary")
        list(APPEND f1c_extra_sources "${f1c_test_dir}/f1c_guard_alloc.cpp")
    endif()
    add_executable(${f1c_target} "${f1c_test_dir}/f1c_${f1c_driver}_driver.cpp" ${f1c_extra_sources}
                   $<TARGET_OBJECTS:difftest_f1c_generated>)
    add_dependencies(${f1c_target} difftest_f1c_generated)
    target_include_directories(${f1c_target} PRIVATE "${SHADOW_F1C_DIR}" "${f1c_test_dir}")
    target_link_libraries(${f1c_target} PRIVATE shadow_runtime)
    if(MSVC)
        # Same ISA flags as the generated objects (the inline ppc_ops.h helpers must be one variant).
        target_compile_options(${f1c_target} PRIVATE /W4 /permissive- ${SHADOW_ARCH_FLAGS})
        set_property(TARGET ${f1c_target} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        # The production stack reserve for the process; the measurements run on their own 1 GiB-reserve thread.
        # The map lets census_f1c.py / run_f1c_stack.py name the host CALL sites that keep frames.
        target_link_options(${f1c_target} PRIVATE /STACK:16777216
                            "/MAP:$<TARGET_FILE_DIR:${f1c_target}>/${f1c_target}.map")
    endif()
endforeach()

# Reference flavour of the stack driver: the guest-state cross-check (reference interpreter on the same program).
add_executable(f1c_stack_driver_ref "${f1c_test_dir}/f1c_stack_driver.cpp" "${f1c_test_dir}/f1c_host.cpp"
               $<TARGET_OBJECTS:difftest_f1c_generated>)
add_dependencies(f1c_stack_driver_ref difftest_f1c_generated)
target_include_directories(f1c_stack_driver_ref PRIVATE "${SHADOW_F1C_DIR}" "${f1c_test_dir}")
target_link_libraries(f1c_stack_driver_ref PRIVATE shadow_runtime_reference)
target_compile_definitions(f1c_stack_driver_ref PRIVATE DIFFTEST_REFERENCE=1)
if(MSVC)
    target_compile_options(f1c_stack_driver_ref PRIVATE /W4 /permissive- ${SHADOW_ARCH_FLAGS})
    set_property(TARGET f1c_stack_driver_ref PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_link_options(f1c_stack_driver_ref PRIVATE /STACK:16777216)
endif()

# R12 (2026-09-28): the same generated objects compiled with /Od /Ob0. MSVC performs no tail-call optimisation
# there, so any LK=0 transfer still lowered to a host call would keep one frame per guest loop lap and fail the
# unchanged acceptance script: host-stack boundedness has to come from the transfer protocol itself.
add_library(difftest_f1c_generated_notail OBJECT ${difftest_f1c_sources})
# The generated table calls the lifted subsystems (gekko/lifted.h).
target_link_libraries(difftest_f1c_generated_notail PRIVATE shadow_lift)
# Both object libraries list the generator's outputs; the Visual Studio generator attaches the generating rule
# to each, so they must not build in parallel (two concurrent gen_f1c.py runs delete each other's outputs).
add_dependencies(difftest_f1c_generated_notail difftest_f1c_generated)
target_include_directories(difftest_f1c_generated_notail PRIVATE "${SHADOW_F1C_DIR}"
                           "${CMAKE_CURRENT_SOURCE_DIR}/runtime/include")
target_compile_features(difftest_f1c_generated_notail PRIVATE cxx_std_17)
add_executable(f1c_stack_driver_notail "${f1c_test_dir}/f1c_stack_driver.cpp" "${f1c_test_dir}/f1c_host.cpp"
               $<TARGET_OBJECTS:difftest_f1c_generated_notail>)
add_dependencies(f1c_stack_driver_notail difftest_f1c_generated_notail)
target_include_directories(f1c_stack_driver_notail PRIVATE "${SHADOW_F1C_DIR}" "${f1c_test_dir}")
target_link_libraries(f1c_stack_driver_notail PRIVATE shadow_runtime)
if(MSVC)
    # /Od after the configuration's /O2 (cl warns D9025 and applies /Od).
    target_compile_options(difftest_f1c_generated_notail PRIVATE /W4 /permissive- /wd4702 /wd4102 /wd4065
                                                                 /bigobj /MP8 ${SHADOW_ARCH_FLAGS} /Od /Ob0)
    set_property(TARGET difftest_f1c_generated_notail PROPERTY MSVC_RUNTIME_LIBRARY
                 "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(f1c_stack_driver_notail PRIVATE /W4 /permissive- ${SHADOW_ARCH_FLAGS})
    set_property(TARGET f1c_stack_driver_notail PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_link_options(f1c_stack_driver_notail PRIVATE /STACK:16777216
                        "/MAP:$<TARGET_FILE_DIR:f1c_stack_driver_notail>/f1c_stack_driver_notail.map")
endif()

# The unmodified F1b production driver with every heap block guard-paged.
add_executable(difftest_f1b_driver_prod_guarded "${f1c_test_dir}/difftest_driver.cpp"
               "${f1c_test_dir}/f1c_guard_alloc.cpp" "${f1c_test_dir}/f1c_guard_report.cpp"
               $<TARGET_OBJECTS:difftest_f1b_generated>)
add_dependencies(difftest_f1b_driver_prod_guarded difftest_f1b_generated)
target_include_directories(difftest_f1b_driver_prod_guarded PRIVATE "${f1c_test_dir}")
target_link_libraries(difftest_f1b_driver_prod_guarded PRIVATE shadow_runtime)
if(MSVC)
    target_compile_options(difftest_f1b_driver_prod_guarded PRIVATE /W4 /permissive- ${SHADOW_ARCH_FLAGS})
    set_property(TARGET difftest_f1b_driver_prod_guarded PROPERTY MSVC_RUNTIME_LIBRARY
                 "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_link_options(difftest_f1b_driver_prod_guarded PRIVATE /STACK:16777216)
endif()

add_test(NAME f1c_host_stack_integrity
         COMMAND "${Python3_EXECUTABLE}" -B "${SHADOW_DIFFTEST_TOOL}/run_f1c_stack.py"
                 --driver $<TARGET_FILE:f1c_stack_driver>
                 --map "$<TARGET_FILE_DIR:f1c_stack_driver>/f1c_stack_driver.map"
                 --generated "${SHADOW_F1C_DIR}"
                 --ref-driver $<TARGET_FILE:f1c_stack_driver_ref>
                 --work "${SHADOW_F1C_RESULTS}/stack")
set_tests_properties(f1c_host_stack_integrity PROPERTIES TIMEOUT 3600 LABELS "native;host-stack")

add_test(NAME f1c_host_stack_integrity_notail
         COMMAND "${Python3_EXECUTABLE}" -B "${SHADOW_DIFFTEST_TOOL}/run_f1c_stack.py"
                 --driver $<TARGET_FILE:f1c_stack_driver_notail>
                 --map "$<TARGET_FILE_DIR:f1c_stack_driver_notail>/f1c_stack_driver_notail.map"
                 --generated "${SHADOW_F1C_DIR}"
                 --ref-driver $<TARGET_FILE:f1c_stack_driver_ref>
                 --work "${SHADOW_F1C_RESULTS}/stack-notail")
set_tests_properties(f1c_host_stack_integrity_notail PROPERTIES TIMEOUT 3600 LABELS "native;host-stack")

# Phase B2 (counter wrap past 2^32) is retired with the instruction clock: see f1c_canary_driver.cpp.
add_test(NAME f1c_canary_longrun
         COMMAND f1c_canary_driver --out "${SHADOW_F1C_RESULTS}/canary-longrun.json" --phases A,B1)
set_tests_properties(f1c_canary_longrun PROPERTIES TIMEOUT 900 LABELS "native;codegen;canary")

# f1c_counter_width_probe (canary phase B3) RETIRED 2026-10-06 (native time). It checked finding W1: that
# coalesce_ram_poll charged a whole number of idle-loop iterations into retire_block's u32 at hardware service
# intervals >= 2^32/3 (fixed 2026-09-22). coalesce_ram_poll, retire_block and Cpu::set_hardware_tick no longer
# exist - nothing is charged by instruction count and guest code runs in zero game time - so the probe has no
# observable contract left. The driver refuses `--phases B3`.

# Lanes N and NL: the production run(1) budget lane NB is retired with the instruction budget (native time; see
# difftest_driver.cpp), so the runner's default list (N,NL,NB) would be refused by the driver.
add_test(NAME f1c_guarded_f1b_vectors
         COMMAND "${Python3_EXECUTABLE}" -B "${SHADOW_DIFFTEST_TOOL}/run_f1c_guarded_vectors.py"
                 --plain $<TARGET_FILE:difftest_f1b_driver_prod>
                 --guarded $<TARGET_FILE:difftest_f1b_driver_prod_guarded>
                 --generated "${SHADOW_DIFFTEST_F1B_DIR}"
                 --work "${SHADOW_F1C_RESULTS}/guarded-f1b"
                 --lanes N,NL)
set_tests_properties(f1c_guarded_f1b_vectors PROPERTIES TIMEOUT 3600 LABELS "native;canary")

# Production objects: the game's own translated loops (fn_803D946C/fn_803D94AC and the jump_indirect
# ptmf adjuster cycle fn_803A1AAC/fn_803A1AD4), linked from shadow_recomp exactly as the executable is.
if(NOT SHADOW_RECOMP_DOL STREQUAL "")
    include("${CMAKE_CURRENT_LIST_DIR}/difftest_f1c_game.cmake")
endif()
