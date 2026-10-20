cmake_minimum_required(VERSION 3.21)

project(TelemetryMon
    VERSION 1.0.0
    DESCRIPTION "Per-frame metrics + GPU/CPU telemetry + frame images, consuming the PresentMon shared service"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# MaaWin32Screencap, consumed as a git submodule of this repository.
set(MWS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/MaaWin32Screencap" CACHE PATH "Path to the MaaWin32Screencap submodule")
set(MWS_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory("${MWS_ROOT}" MaaWin32Screencap EXCLUDE_FROM_ALL)
# /GL objects break the def-file generation that WINDOWS_EXPORT_ALL_SYMBOLS
# relies on (same workaround as PresentMonConsole).
set_target_properties(MaaWin32Screencap PROPERTIES INTERPROCEDURAL_OPTIMIZATION_RELEASE OFF)

add_executable(TelemetryMon
    src/Main.cpp
    src/PmClient.cpp
    src/FrameCapture.cpp)
target_include_directories(TelemetryMon PRIVATE src)
target_compile_definitions(TelemetryMon PRIVATE UNICODE _UNICODE)
target_compile_options(TelemetryMon PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(TelemetryMon PRIVATE user32 MaaWin32Screencap::MaaWin32Screencap)

# Ship the service client DLL next to the exe (from the installed PresentMon).
add_custom_command(TARGET TelemetryMon POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/PresentMonAPI2.dll" $<TARGET_FILE_DIR:TelemetryMon>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:MaaWin32Screencap> $<TARGET_FILE_DIR:TelemetryMon>)
