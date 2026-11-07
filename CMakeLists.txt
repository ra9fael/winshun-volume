cmake_minimum_required(VERSION 3.16)
project(TaskbarVolume CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
    message(FATAL_ERROR "TaskbarVolume builds on Windows only")
endif()

add_executable(TaskbarVolume
    src/main.cpp
    src/mouse_logic.cpp
    src/audio_controller.cpp
    src/audio_worker.cpp
    src/taskbar_resolver.cpp
    src/mouse_hook.cpp
    src/overlay.cpp
)
target_include_directories(TaskbarVolume PRIVATE src)
target_compile_definitions(TaskbarVolume PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN CRT_SECURE_NO_WARNINGS)
target_link_libraries(TaskbarVolume PRIVATE ole32 oleaut32 uuid user32 gdi32 dwmapi)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # A published plugin must run on machines without a MinGW toolchain:
    # link libstdc++/libgcc/winpthread into volume.exe.
    target_link_options(TaskbarVolume PRIVATE -static)
endif()
# The exe must sit next to plugin.json: "runtime.program" is a relative path.
set_target_properties(TaskbarVolume PROPERTIES
    OUTPUT_NAME "volume"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}"
    SUFFIX ".exe")
# Multi-config generators (Visual Studio, Ninja Multi-Config) append the
# configuration name to RUNTIME_OUTPUT_DIRECTORY unless a per-config value is
# set; keep the exe next to plugin.json in every configuration.
foreach(cfg ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER "${cfg}" cfg_upper)
    set_target_properties(TaskbarVolume PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${cfg_upper} "${CMAKE_SOURCE_DIR}")
endforeach()

enable_testing()
add_executable(tvb_tests
    tests/test_main.cpp
    tests/test_mouse_logic.cpp
    src/mouse_logic.cpp
)
target_include_directories(tvb_tests PRIVATE src tests)
target_compile_definitions(tvb_tests PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN CRT_SECURE_NO_WARNINGS)
target_link_libraries(tvb_tests PRIVATE user32 gdi32)
add_test(NAME tvb_tests COMMAND tvb_tests)

# E2E helper: a controlled window + real audio session in one process.
add_executable(VolumeTestTarget tools/VolumeTestTarget/main.cpp)
target_compile_definitions(VolumeTestTarget PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN CRT_SECURE_NO_WARNINGS)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(VolumeTestTarget PRIVATE -municode) # wmain
    target_link_options(VolumeTestTarget PRIVATE -municode)
endif()
target_link_libraries(VolumeTestTarget PRIVATE ole32 uuid user32)
