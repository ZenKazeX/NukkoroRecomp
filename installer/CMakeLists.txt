# The setup window: a small Win32 host that shows installer/web in WebView2 and runs installer/Backend.ps1.
# Install.cmd builds it into .build/setup; it is independent of the game build.
cmake_minimum_required(VERSION 3.25)
project(NukkoroSetup VERSION 1.0.0 LANGUAGES CXX RC)
if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "The setup window is a Windows desktop program; build it with Visual Studio C++ x64.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# The WebView2 SDK is downloaded from NuGet at configure time (pinned version and hash), not stored in git.
set(WEBVIEW2_VERSION "1.0.2903.40")
set(WEBVIEW2_SHA256 "ef128016dd1e51c59178c827ed5b8aa3322c57afa8675d930f8109505542ad74")
set(WEBVIEW2_DIR "${CMAKE_BINARY_DIR}/webview2-${WEBVIEW2_VERSION}")
if(NOT EXISTS "${WEBVIEW2_DIR}/build/native/include/WebView2.h")
    set(package "${CMAKE_BINARY_DIR}/webview2-${WEBVIEW2_VERSION}.nupkg")
    file(DOWNLOAD "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/${WEBVIEW2_VERSION}" "${package}"
         EXPECTED_HASH SHA256=${WEBVIEW2_SHA256} STATUS status TLS_VERIFY ON)
    list(GET status 0 code)
    if(NOT code EQUAL 0)
        message(FATAL_ERROR "Could not download the WebView2 SDK ${WEBVIEW2_VERSION} from nuget.org: ${status}")
    endif()
    file(ARCHIVE_EXTRACT INPUT "${package}" DESTINATION "${WEBVIEW2_DIR}")
endif()

# Art pack (installer/art, not in git): its icon is used when present.
set(SETUP_ICON_LINE "")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/art/app.ico")
    set(SETUP_ICON_LINE "IDI_APP ICON \"${CMAKE_CURRENT_SOURCE_DIR}/art/app.ico\"")
endif()
configure_file(resources/app.rc.in "${CMAKE_BINARY_DIR}/app.rc" @ONLY)

add_executable(NukkoroSetup WIN32 src/main.cpp "${CMAKE_BINARY_DIR}/app.rc")
target_include_directories(NukkoroSetup PRIVATE "${WEBVIEW2_DIR}/build/native/include" resources)
target_compile_definitions(NukkoroSetup PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(NukkoroSetup PRIVATE /W4 /permissive- /utf-8 /EHsc)
target_link_libraries(NukkoroSetup PRIVATE "${WEBVIEW2_DIR}/build/native/x64/WebView2LoaderStatic.lib"
    user32 ole32 oleaut32 shell32 shlwapi version dwmapi advapi32)
