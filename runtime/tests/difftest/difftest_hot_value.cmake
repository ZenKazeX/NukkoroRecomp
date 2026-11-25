# cpu/opt2: contract test of ppcrecomp --hot-value and --hot-layout (test-only).
#
# Included from runtime/tests/difftest/difftest.cmake. The F1a harness (vectors, production driver on the generated
# code, reference-interpreter driver, comparator) on ONE family, `hvalue`: single words through lfs / stfs (NaNs,
# denormals and infinities among them), compares whose CR field is written again, FPR halves written again, with
# labels, a call that reads CR, lfd and fmr between them; labels are entered directly too. The generated tree is
# built with --hot-value 63 --hot-layout, so the stores are forwarded words, the dead CR / FPR writes are gone, the
# rare checks are out of line and the bl of kind 5 enters its callee's hot body at entry 0 (the hook filter, then
# the body); native and interpreter must agree on every vector (registers, both FPR halves, CR, FPSCR, every memory
# access).
#
#   difftest_hot_value           must pass
#   difftest_hot_value_mutant2   WILL_FAIL: --hot-value-mutant 2 (calls are not observers of CR / the FPRs); the
#                                comparator must catch it (negative control)
# Mutant 1 (a single store forwards across a label) has no tree: in emitted code the first FP instruction after a
# label carries the MSR[FP] guard, an observer that ends the run by itself, so that mutant changes no generated
# text here. tools/ppcrecomp/test_hot_value.py runs it on bodies without the guard and catches it.

function(shadow_difftest_hot_value_tree suffix mutant)
    set(dir "${CMAKE_CURRENT_BINARY_DIR}/Testing/difftest-hot-value${suffix}")
    set(sources "${dir}/recomp_000.cpp" "${dir}/recomp_table.cpp")
    set(extra "")
    if(mutant)
        set(extra --hot-value-mutant ${mutant})
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
                --families hvalue
                --hot-value 63 --hot-layout ${extra}
        DEPENDS ppcrecomp
                "${SHADOW_DIFFTEST_TOOL}/gen_vectors.py"
                "${SHADOW_DIFFTEST_TOOL}/ppcenc.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/runtime/include/gekko/ppc_instruction_traits.h"
        COMMENT "difftest hot value${suffix}: generating vectors and running ppcrecomp --hot-value"
        VERBATIM)
    add_library(difftest_hot_value${suffix}_generated OBJECT ${sources})
    target_link_libraries(difftest_hot_value${suffix}_generated PRIVATE shadow_lift)
    target_include_directories(difftest_hot_value${suffix}_generated PRIVATE "${dir}"
                               "${CMAKE_CURRENT_SOURCE_DIR}/runtime/include")
    target_compile_features(difftest_hot_value${suffix}_generated PRIVATE cxx_std_17)
    if(MSVC)
        target_compile_options(difftest_hot_value${suffix}_generated PRIVATE /W4 /permissive- /wd4702 /wd4102 /wd4065
                                                                        /bigobj /MP8 ${SHADOW_ARCH_FLAGS})
        set_property(TARGET difftest_hot_value${suffix}_generated PROPERTY MSVC_RUNTIME_LIBRARY
                     "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    foreach(flavor prod ref)
        set(target difftest_hot_value${suffix}_driver_${flavor})
        add_executable(${target} runtime/tests/difftest/difftest_driver.cpp
                       $<TARGET_OBJECTS:difftest_hot_value${suffix}_generated>)
        add_dependencies(${target} difftest_hot_value${suffix}_generated)
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
    add_test(NAME difftest_hot_value${suffix}
             COMMAND "${Python3_EXECUTABLE}" -B "${SHADOW_DIFFTEST_TOOL}/run_difftest.py"
                     --prod $<TARGET_FILE:difftest_hot_value${suffix}_driver_prod>
                     --ref $<TARGET_FILE:difftest_hot_value${suffix}_driver_ref>
                     --generated "${dir}"
                     --spec "${SHADOW_DIFFTEST_SPEC}"
                     --seed "${SHADOW_DIFFTEST_SEED}" --scale "${SHADOW_DIFFTEST_SCALE}")
    set_tests_properties(difftest_hot_value${suffix} PROPERTIES TIMEOUT 900 LABELS "reference;native;codegen;differential")
    if(mutant)
        set_tests_properties(difftest_hot_value${suffix} PROPERTIES WILL_FAIL TRUE)
    endif()
endfunction()

shadow_difftest_hot_value_tree("" 0)
shadow_difftest_hot_value_tree("_mutant2" 2)
