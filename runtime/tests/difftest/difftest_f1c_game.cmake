# F1c host-stack integrity on the production translation (test-only). Included from difftest_f1c.cmake when
# SHADOW_RECOMP_DOL is set. Links shadow_recomp (the shipped 64-shard objects) and shadow_runtime; runs the
# game's own cycles fn_803D946C/fn_803D94AC (cross-record back edge after a record fall-through) and
# fn_803A1AAC/fn_803A1AD4 (recomp::jump_indirect default path) for 1,000,000 iterations with MSR[EE] = 0 and 1.
# Failed on the 2026-09-14 core (verify-r2 host-stack-cross-record, audit R12); must pass since R12 (2026-09-28).

add_executable(f1c_game_stack_driver "${f1c_test_dir}/f1c_game_stack_driver.cpp" "${f1c_test_dir}/f1c_host.cpp")
target_include_directories(f1c_game_stack_driver PRIVATE "${f1c_test_dir}")
target_link_libraries(f1c_game_stack_driver PRIVATE shadow_recomp shadow_runtime)
if(MSVC)
    target_compile_options(f1c_game_stack_driver PRIVATE /W4 /permissive- ${SHADOW_ARCH_FLAGS})
    set_property(TARGET f1c_game_stack_driver PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_link_options(f1c_game_stack_driver PRIVATE /STACK:16777216
                        "/MAP:$<TARGET_FILE_DIR:f1c_game_stack_driver>/f1c_game_stack_driver.map")
endif()

add_test(NAME f1c_host_stack_game
         COMMAND "${Python3_EXECUTABLE}" -B "${SHADOW_DIFFTEST_TOOL}/run_f1c_stack.py"
                 --driver $<TARGET_FILE:f1c_game_stack_driver>
                 --map "$<TARGET_FILE_DIR:f1c_game_stack_driver>/f1c_game_stack_driver.map"
                 --samplers hook
                 --work "${SHADOW_F1C_RESULTS}/stack-game")
set_tests_properties(f1c_host_stack_game PROPERTIES TIMEOUT 1800 LABELS "native;host-stack")
