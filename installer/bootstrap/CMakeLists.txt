# The downloadable installer exe: the release package and the built setup window, in one file.
#   cmake -S installer/bootstrap -B .build/bootstrap -DPACKAGE_ZIP=<NukkoroRecomp-X.zip> -DSETUP_EXE=<NukkoroSetup.exe> -DNUKKORO_VERSION=X
cmake_minimum_required(VERSION 3.25)
project(NukkoroBootstrap LANGUAGES CXX RC)

foreach(required PACKAGE_ZIP SETUP_EXE NUKKORO_VERSION)
    if(NOT ${required})
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

configure_file(bootstrap.rc.in "${CMAKE_BINARY_DIR}/bootstrap.rc" @ONLY)
add_executable(NukkoroBootstrap WIN32 main.cpp "${CMAKE_BINARY_DIR}/bootstrap.rc")
set_target_properties(NukkoroBootstrap PROPERTIES OUTPUT_NAME "NukkoroRecomp-${NUKKORO_VERSION}-Setup")
target_compile_definitions(NukkoroBootstrap PRIVATE UNICODE _UNICODE "NUKKORO_VERSION=L\"${NUKKORO_VERSION}\"")
target_compile_options(NukkoroBootstrap PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(NukkoroBootstrap PRIVATE user32 gdi32 shell32)
target_link_options(NukkoroBootstrap PRIVATE /MANIFEST:NO)
set_source_files_properties("${CMAKE_BINARY_DIR}/bootstrap.rc" PROPERTIES OBJECT_DEPENDS "${PACKAGE_ZIP};${SETUP_EXE}")
