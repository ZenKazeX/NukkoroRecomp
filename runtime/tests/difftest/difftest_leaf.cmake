# cs2/cpu: contract test of ppcrecomp --inline-leaves (test-only).
#
# Included from runtime/tests/difftest/difftest.cmake. The F1a harness (vectors, production driver on the generated
# code, reference-interpreter driver, comparator) on ONE family, `leafi`: small straight-line leaves (memory,
# conditional blr, a forward branch, floating point, LR/CTR reads, CR fields) entered directly and through one or two
# bl. The generated tree is built with --inline-leaves 24 --inline-no-hooks, so every bl of those leaves is a
# spliced body; native and interpreter must agree on every vector.
#
#   difftest_leaf          must pass
#   difftest_leaf_mutant   WILL_FAIL: the same tree generated with --inline-leaves-mutant 1 (the splice drops the
#                          callee's first instruction); the comparator must catch it (negative control)

function(shadow_difftest_leaf_tree suffix mutant)
    set(dir "${CMAKE_CURRENT_BINARY_DIR}/Testing/difftest-leaf${suffix}")
    set(sources "${dir}/recomp_000.cpp" "${dir}/recomp_table.cpp")
    set(extra "")
    if(mutant)
        set(extra --inline-leaves-mutant ${mutant})
    endif()
    add_custom_command(
        OUTPUT ${sources} "${dir}/recomp_decls.h" "${dir}/vectors.txt" "${dir}/vectors.jsonl"
        COMMAND "${Python3_EXECUTABLE}" -B "${SHADOW_DIFFTEST_TOOL}/gen_vectors.py"
                --generator $<TARGET_FILE:ppcrecomp>
                --out "${dir}"
                --seed "${SHADOW_DIFFTEST_SEED}"
                --scale "${SHADOW_DIFFTEST_SCALE}"
                --shards 1
                --spec "${SHADOW_DIFFTEST_SPEC}"
                --families leafi
                --inline-leaves 24 ${extra}
        DEPENDS ppcrecomp
                "${SHADOW_DIFFTEST_TOOL}/gen_vectors.py"
                "${SHADOW_DIFFTEST_TOOL}/ppcenc.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/runtime/include/gekko/ppc_instruction_traits.h"
        COMMENT "difftest leaf${suffix}: generating leaf vectors and running ppcrecomp --inline-leaves"
        VERBATIM)
    add_library(difftest_leaf${suffix}_generated OBJECT ${sources})
    target_link_libraries(difftest_leaf${suffix}_generated PRIVATE shadow_lift)
    target_include_directories(difftest_leaf${suffix}_generated PRIVATE "${dir}"
                               "${CMAKE_CURRENT_SOURCE_DIR}/runtime/include")
    target_compile_features(difftest_leaf${suffix}_generated PRIVATE cxx_std_17)
    if(MSVC)
        target_compile_options(difftest_leaf${suffix}_generated PRIVATE /W4 /permissive- /wd4702 /wd4102 /wd4065
                                                                        /bigobj /MP8 ${SHADOW_ARCH_FLAGS})
        set_property(TARGET difftest_leaf${suffix}_generated PROPERTY MSVC_RUNTIME_LIBRARY
                     "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    foreach(flavor prod ref)
        set(target difftest_leaf${suffix}_driver_${flavor})
        add_executable(${target} runtime/tests/difftest/difftest_driver.cpp
                       $<TARGET_OBJECTS:difftest_leaf${suffix}_generated>)
        add_dependencies(${target} difftest_leaf${suffix}_generated)
        if(flavor STREQUAL "ref")
            target_link_libraries(${target} PRIVATE shadow_runtime_reference)
            target_compile_definitions(${target} PRIVATE DIFFTEST_REFERENCE=1)
        else()
            target_link_libraries(${target} PRIVATE shadow_runtime)
        endif()
        if(MSVC)
            target_compile_options(${target} PRIVATE /W4 /permissive- ${SHADOW_ARCH_FLAGS})
            set_property(TARGET ${target} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
            target_link_options(${target} PRIVATE /STACK:16777216)
        endif()
    endforeach()
    add_test(NAME difftest_leaf${suffix}
             COMMAND "${Python3_EXECUTABLE}" -B "${SHADOW_DIFFTEST_TOOL}/run_difftest.py"
                     --prod $<TARGET_FILE:difftest_leaf${suffix}_driver_prod>
                     --ref $<TARGET_FILE:difftest_leaf${suffix}_driver_ref>
                     --generated "${dir}"
                     --spec "${SHADOW_DIFFTEST_SPEC}"
                     --seed "${SHADOW_DIFFTEST_SEED}" --scale "${SHADOW_DIFFTEST_SCALE}")
    set_tests_properties(difftest_leaf${suffix} PROPERTIES TIMEOUT 900 LABELS "reference;native;codegen;differential")
    if(mutant)
        set_tests_properties(difftest_leaf${suffix} PROPERTIES WILL_FAIL TRUE)
    endif()
endfunction()

shadow_difftest_leaf_tree("" 0)
shadow_difftest_leaf_tree("_mutant" 1)
