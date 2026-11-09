# The Vulkan renderer (docs/VULKAN_RENDERER.md), built on its own while it grows (stage V1):
#
#   cmake -S runtime/lift/render_vk -B .build/vk -G "Visual Studio 18 2026" -A x64
#   cmake --build .build/vk --config Release -j 2
#
# Android (the bring-up activity, android/README.md):
#
#   cmake -S runtime/lift/render_vk -B .build/vk-android-x86_64 -G Ninja
#         -DCMAKE_MAKE_PROGRAM=C:/AndroidSDK/cmake/3.22.1/bin/ninja.exe
#         -DCMAKE_TOOLCHAIN_FILE=C:/AndroidSDK/ndk/27.2.12479018/build/cmake/android.toolchain.cmake
#         -DANDROID_ABI=x86_64 -DANDROID_PLATFORM=android-33 -DCMAKE_BUILD_TYPE=Release
#   cmake --build .build/vk-android-x86_64 -j 2
#
# It reads the D3D12 renderer's packet contract (runtime/lift/render/frame.h, view.h) and its HLSL
# (runtime/lift/render/shaders), compiling the HLSL to SPIR-V with DXC, and uses the VulkanMemoryAllocator that plume
# vendors. Nothing here is part of the game's build yet (integration hooks: docs/VULKAN_RENDERER.md 8).

cmake_minimum_required(VERSION 3.25)
project(shadow_render_vk LANGUAGES C CXX)

get_filename_component(SHADOW_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(SHADOW_LIFT_DIR "${SHADOW_SOURCE_ROOT}/runtime/lift")
set(SHADOW_VMA_DIR "${SHADOW_SOURCE_ROOT}/thirdparty/plume/contrib/VulkanMemoryAllocator/include")

if(ANDROID)
    # The NDK's own Vulkan headers and loader (libvulkan.so of the system image), never the desktop SDK's.
    add_library(shadow_vulkan INTERFACE)
    target_link_libraries(shadow_vulkan INTERFACE vulkan)
else()
    find_package(Vulkan REQUIRED)
    add_library(shadow_vulkan INTERFACE)
    target_link_libraries(shadow_vulkan INTERFACE Vulkan::Vulkan)
endif()
if(NOT EXISTS "${SHADOW_VMA_DIR}/vk_mem_alloc.h")
    message(FATAL_ERROR "VulkanMemoryAllocator not found at ${SHADOW_VMA_DIR}")
endif()

# DXC with SPIR-V output: the Vulkan SDK's (the Windows SDK's dxc.exe has no SPIR-V back end).
find_program(SHADOW_VK_DXC dxc
    HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin"
    NO_DEFAULT_PATH)
if(NOT SHADOW_VK_DXC)
    find_program(SHADOW_VK_DXC dxc REQUIRED)
endif()

# The shaders exactly as the D3D12 build compiles them (-O3 -WX -HV 2021 -Gis), plus the Vulkan mapping:
#   -fvk-use-dx-layout              constant buffers keep D3D's packing (the C++ blocks are shared byte for byte)
#   -fvk-bind-resource-heap 0 1     ResourceDescriptorHeap = set 1 binding 0 (sampled images) for graphics,
#                                   set 1 binding 2 (storage buffers) for compute (bindless.h)
#   -fvk-bind-sampler-heap 1 1      SamplerDescriptorHeap = set 1 binding 1
# cbuffers b0 / b1 / b2 land on set 0 bindings 0 / 1 / 2 (DXC's default register mapping).
set(SHADOW_VK_SHADER_DIR "${CMAKE_BINARY_DIR}/spirv")
set(SHADOW_VK_SHADER_HEADERS)
foreach(entry IN ITEMS "utility|VSFullscreen|vs_6_6"
                       "utility|VSTriangle|vs_6_6"
                       "utility|PSSolid|ps_6_6"
                       "utility|PSTexture|ps_6_6"
                       "utility|PSResolve|ps_6_6"
                       "utility|PSScale|ps_6_6"
                       "mesh|VSMesh|vs_6_6"
                       "mesh|PSBase|ps_6_6"
                       "mesh|PSEnvironment|ps_6_6"
                       "mesh|PSDual|ps_6_6"
                       "particles|VSParticle|vs_6_6"
                       "particles|PSParticle|ps_6_6"
                       "immediate|VSImmediate|vs_6_6"
                       "immediate|PSImmediate|ps_6_6"
                       "glare|PSGlareCapture|ps_6_6"
                       "glare|PSGlareBlur|ps_6_6"
                       "glare|PSGlareBox|ps_6_6"
                       "glare|PSGlareMerge|ps_6_6"
                       "glare|PSGlareComposite|ps_6_6"
                       "glare|PSResolveGlare|ps_6_6"
                       "skinning|CSSkin|cs_6_6"
                       "vk:present|VSFullscreenRotated|vs_6_6")
    string(REPLACE "|" ";" parts "${entry}")
    list(GET parts 0 source_name)
    list(GET parts 1 entry_point)
    list(GET parts 2 profile)
    set(heap_binding 0)
    if(profile STREQUAL "cs_6_6")
        set(heap_binding 2)
    endif()
    # "vk:<name>": render_vk's own shaders (render_vk/shaders); the others are the D3D12 renderer's.
    if(source_name MATCHES "^vk:(.*)$")
        set(source "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${CMAKE_MATCH_1}.hlsl")
    else()
        set(source "${SHADOW_LIFT_DIR}/render/shaders/${source_name}.hlsl")
    endif()
    set(header "${SHADOW_VK_SHADER_DIR}/spv_${entry_point}.h")
    add_custom_command(
        OUTPUT "${header}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${SHADOW_VK_SHADER_DIR}"
        COMMAND "${SHADOW_VK_DXC}" -nologo -spirv -fspv-target-env=vulkan1.3 -fvk-use-dx-layout
                -fvk-bind-resource-heap ${heap_binding} 1 -fvk-bind-sampler-heap 1 1
                -T ${profile} -E ${entry_point} -O3 -WX -HV 2021 -Gis
                -Vn kSpv${entry_point} -Fh "${header}" "${source}"
        DEPENDS "${source}"
        COMMENT "Compiling ${entry_point} to SPIR-V (DXC ${profile})"
        VERBATIM)
    list(APPEND SHADOW_VK_SHADER_HEADERS "${header}")
endforeach()
add_custom_target(shadow_render_vk_shaders DEPENDS ${SHADOW_VK_SHADER_HEADERS})

# The platform-neutral core: device, memory, bindless set, frame loop, swapchain (Win32 and Android surfaces),
# pipelines. Everything but the packet.
add_library(shadow_render_vk_core STATIC
    bindless.cpp
    device.cpp
    frame_graph.cpp
    frame_loop.cpp
    gpu_timer.cpp
    pipelines.cpp
    report.cpp
    resources.cpp
    swapchain.cpp
    vma.cpp)
add_dependencies(shadow_render_vk_core shadow_render_vk_shaders)
target_compile_features(shadow_render_vk_core PUBLIC cxx_std_20)
# "render_vk/x.h" (and "render/frame.h", the packet contract) from runtime/lift; the generated SPIR-V headers; VMA.
target_include_directories(shadow_render_vk_core
    PUBLIC "${SHADOW_LIFT_DIR}"
    PRIVATE "${SHADOW_VK_SHADER_DIR}" "${SHADOW_VMA_DIR}")
target_link_libraries(shadow_render_vk_core PUBLIC shadow_vulkan)

function(shadow_vk_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
        set_property(TARGET ${target} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    else()
        # The VkX x{VK_STRUCTURE_TYPE_...} idiom leaves the other members value-initialised on purpose.
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
    endif()
endfunction()
shadow_vk_warnings(shadow_render_vk_core)
if(NOT MSVC)
    set_source_files_properties(vma.cpp PROPERTIES COMPILE_OPTIONS "-w")  # VMA's own warnings
endif()

# The packet renderer (render_vk::Renderer) and the packet contract's own code (render/frame.cpp: FrameBuilder).
add_library(shadow_render_vk STATIC
    renderer.cpp
    renderer_draws.cpp
    renderer_effects.cpp
    renderer_resources.cpp
    "${SHADOW_LIFT_DIR}/render/effect_layers.cpp"
    "${SHADOW_LIFT_DIR}/render/frame.cpp")
target_link_libraries(shadow_render_vk PUBLIC shadow_render_vk_core)
shadow_vk_warnings(shadow_render_vk)

# The headless checks (split-screen packets rendered, read back, compared, rendered twice), shared by the desktop test
# and the Android activity.
add_library(shadow_render_vk_checks STATIC tests/smoke_checks.cpp tests/draw_checks.cpp tests/effect_checks.cpp
    tests/graph_checks.cpp)
target_link_libraries(shadow_render_vk_checks PUBLIC shadow_render_vk)
shadow_vk_warnings(shadow_render_vk_checks)

if(ANDROID)
    # The activity: render_vk::Renderer driven with packets on an ANativeWindow (android/README.md).
    set(SHADOW_GLUE "${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c")
    # The packet stream reader and texture import: the replay mode (a recorded game stream in the app's files folder).
    add_library(shadow_vk SHARED android/main.cpp "${SHADOW_GLUE}"
        "${SHADOW_LIFT_DIR}/render/packet_stream.cpp"
        "${SHADOW_LIFT_DIR}/assets/chunk_stream.cpp"
        "${SHADOW_LIFT_DIR}/assets/texel_decode.cpp"
        "${SHADOW_LIFT_DIR}/assets/texture_dictionary.cpp"
        "${SHADOW_LIFT_DIR}/assets/texture_import.cpp")
    target_include_directories(shadow_vk PRIVATE "${ANDROID_NDK}/sources/android/native_app_glue")
    target_link_libraries(shadow_vk PRIVATE shadow_render_vk_checks android log)
    # The glue's entry point must survive the link: NativeActivity looks it up by name.
    target_link_options(shadow_vk PRIVATE "-uANativeActivity_onCreate")
    shadow_vk_warnings(shadow_vk)
    set_source_files_properties("${SHADOW_GLUE}" PROPERTIES COMPILE_OPTIONS "-w")  # the NDK's own glue
    return()
endif()

add_executable(shadow_render_vk_smoke tests/vk_smoke.cpp)
target_link_libraries(shadow_render_vk_smoke PRIVATE shadow_render_vk_checks)
target_include_directories(shadow_render_vk_smoke PRIVATE "${SHADOW_SOURCE_ROOT}/thirdparty/stb")
shadow_vk_warnings(shadow_render_vk_smoke)

# The replay of a recorded packet stream (render/packet_stream.h) - real game frames - compared with the D3D12
# renderer's captures of the same frames.
add_executable(shadow_render_vk_replay
    tests/vk_replay.cpp
    "${SHADOW_LIFT_DIR}/render/packet_stream.cpp"
    "${SHADOW_LIFT_DIR}/assets/chunk_stream.cpp"
    "${SHADOW_LIFT_DIR}/assets/texel_decode.cpp"
    "${SHADOW_LIFT_DIR}/assets/texture_dictionary.cpp"
    "${SHADOW_LIFT_DIR}/assets/texture_import.cpp")
target_link_libraries(shadow_render_vk_replay PRIVATE shadow_render_vk)
target_include_directories(shadow_render_vk_replay PRIVATE "${SHADOW_SOURCE_ROOT}/thirdparty/stb")
shadow_vk_warnings(shadow_render_vk_replay)

if(WIN32)
    # The windowed check: a Win32 window presented through the swapchain for a number of frames.
    add_executable(shadow_render_vk_window tests/vk_window.cpp)
    target_link_libraries(shadow_render_vk_window PRIVATE shadow_render_vk)
    shadow_vk_warnings(shadow_render_vk_window)
endif()
