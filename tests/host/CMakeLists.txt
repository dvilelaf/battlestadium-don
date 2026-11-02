# Test of the host program (T-110 part 2): the host library with a synthetic guest (no game, no generated code).
#
#   cmake -S tests/host -B build/host -DPS2RECOMP_DIR=<PS2Recomp tree with the patches applied>
#   cmake --build build/host -j6
#
# Normally driven by tools/pipeline/build_host.sh (inside the build container).
cmake_minimum_required(VERSION 3.21)
project(don_host_tests LANGUAGES C CXX)

set(PS2RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/PS2Recomp" CACHE PATH
    "PS2Recomp tree (with this repository's patches applied)")
get_filename_component(PS2RECOMP_DIR "${PS2RECOMP_DIR}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
if(NOT EXISTS "${PS2RECOMP_DIR}/ps2xRuntime/CMakeLists.txt")
  message(FATAL_ERROR "PS2RECOMP_DIR=${PS2RECOMP_DIR} has no ps2xRuntime/CMakeLists.txt")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
if(NOT MSVC)
  add_compile_options(-march=x86-64-v3 -ffp-contract=off)
endif()

# The runtime library, without raylib (the host owns the window and the audio).
set(PS2X_WITH_RAYLIB OFF CACHE BOOL "" FORCE)
foreach(opt PS2X_BUILD_RECOMP PS2X_BUILD_ANALYZER PS2X_BUILD_TEST PS2X_BUILD_STUDIO PS2X_ENABLE_FFMPEG
            PS2X_ENABLE_AGRESSIVE_LOGS PS2X_ENABLE_SCCACHE)
  set(${opt} OFF CACHE BOOL "" FORCE)
endforeach()
# The IOP emulator is a standalone library the runtime links.
add_subdirectory("${PS2RECOMP_DIR}/ps2xIOP" "${CMAKE_BINARY_DIR}/ps2xIOP" EXCLUDE_FROM_ALL)
add_subdirectory("${PS2RECOMP_DIR}/ps2xRuntime" "${CMAKE_BINARY_DIR}/ps2xRuntime" EXCLUDE_FROM_ALL)

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/host" "${CMAKE_BINARY_DIR}/src-host")

add_executable(host_synthetic_test host_synthetic_test.cpp)
target_link_libraries(host_synthetic_test PRIVATE don_host)
if(UNIX AND NOT APPLE)
  # SDL3 (copied next to the executable by build_host.sh) and the GS library folder.
  set_target_properties(host_synthetic_test PROPERTIES BUILD_RPATH "$ORIGIN;$ORIGIN/../gs" SKIP_BUILD_RPATH OFF)
endif()

# T-305: the arithmetic of DON_PACING_LOG's summary (header only).
add_executable(pacing_stats_test pacing_stats_test.cpp)
target_compile_features(pacing_stats_test PRIVATE cxx_std_20)

# T-757: the last-window fields of the perf line (header only).
add_executable(perf_window_test perf_window_test.cpp)
target_compile_features(perf_window_test PRIVATE cxx_std_20)
enable_testing()
add_test(NAME perf_window_test COMMAND perf_window_test)

# The audio output's prebuffer/underrun state (header only).
add_executable(audio_prebuffer_test audio_prebuffer_test.cpp)
target_compile_features(audio_prebuffer_test PRIVATE cxx_std_20)

# The audio output's rate control (header only).
add_executable(audio_rate_test audio_rate_test.cpp)
target_compile_features(audio_rate_test PRIVATE cxx_std_20)

# T-610: the game module loader (src/host/game_module_posix.cpp) against tiny fixture modules, no game needed. One
# fixture per failure kind (game_module_fixture.cpp, FX_KIND), built as shared libraries into fixtures/.
# T-616: also the folder form (manifest, SHA-256 from src/setup, crash marker); the SHA-256 comes from src/setup.
if(WIN32)
  # T-731: only what the loader needs (sha256_of_file, the manifest reader) and what those call. The whole of src/setup
  # does not compile for Windows yet (startup.cpp includes <sys/statvfs.h>; its Windows twins are other cards').
  set(_setup "${CMAKE_CURRENT_SOURCE_DIR}/../../src/setup")
  set(DON_TEST_SETUP_SRC "${_setup}/prepare_engine.cpp" "${_setup}/module_manifest.cpp" "${_setup}/iso_reader.cpp")
else()
  file(GLOB DON_TEST_SETUP_SRC CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../../src/setup/*.cpp"
                                                 "${CMAKE_CURRENT_SOURCE_DIR}/../../src/setup/platform/*_posix.cpp"
                                                 "${CMAKE_CURRENT_SOURCE_DIR}/../../src/setup/platform/*_linux.cpp")
endif()
find_package(ZLIB REQUIRED)
add_library(don_setup_for_tests STATIC ${DON_TEST_SETUP_SRC})
target_include_directories(don_setup_for_tests PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../../src/setup")
target_link_libraries(don_setup_for_tests PUBLIC ZLIB::ZLIB)
enable_testing()
if(WIN32)
  # T-731: the Windows loader against two tiny DLLs (a good module, one without the entry symbol), run under Wine.
  # The DLLs land next to the executable: the test finds them from its own path.
  add_executable(test_game_module_win32 test_game_module_win32.cpp
                 "${CMAKE_CURRENT_SOURCE_DIR}/../../src/host/game_module_win32.cpp"
                 "${CMAKE_CURRENT_SOURCE_DIR}/../../src/host/iop_native_bridge.cpp")
  target_include_directories(test_game_module_win32 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../src/host"
                             "${CMAKE_CURRENT_SOURCE_DIR}/../../src/game_module")
  set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/../../src/host/iop_native_bridge.cpp" PROPERTIES
                              INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/../../src/game_module;${PS2RECOMP_DIR}/ps2xIOP/src/emulator/core")
  target_link_libraries(test_game_module_win32 PRIVATE ps2_runtime don_setup_for_tests)
  foreach(fixture_kind ok:0 nosym:1)
    string(REPLACE ":" ";" fixture_parts "${fixture_kind}")
    list(GET fixture_parts 0 fixture_name)
    list(GET fixture_parts 1 fixture_value)
    add_library(fx_win32_${fixture_name} MODULE test_game_module_win32_fixture.cpp)
    target_include_directories(fx_win32_${fixture_name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../src/host"
                               "${CMAKE_CURRENT_SOURCE_DIR}/../../src/game_module")
    target_compile_definitions(fx_win32_${fixture_name} PRIVATE DON_GAME_MODULE_BUILD FX_KIND=${fixture_value})
    set_target_properties(fx_win32_${fixture_name} PROPERTIES PREFIX "" OUTPUT_NAME "fx_win32_${fixture_name}"
                          RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}" LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    add_dependencies(test_game_module_win32 fx_win32_${fixture_name})
  endforeach()
  set_target_properties(test_game_module_win32 PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
  add_test(NAME test_game_module_win32 COMMAND test_game_module_win32)
  # T-735: the crash trace's unhandled-exception filter and its chaining with the crash report, under Wine. The
  # trace, the report and the two files they need (the redirect's descriptors, the build id) are built as-is.
  set(_don_host_src "${CMAKE_CURRENT_SOURCE_DIR}/../../src/host")
  add_executable(test_crash_trace_win32 test_crash_trace_win32.cpp
                 "${_don_host_src}/crash_trace.cpp"
                 "${_don_host_src}/crash_trace_win32.cpp"
                 "${_don_host_src}/crash_report_win32.cpp"
                 "${_don_host_src}/log_redirect.cpp"
                 "${_don_host_src}/build_id.cpp")
  target_include_directories(test_crash_trace_win32 PRIVATE "${_don_host_src}")
  target_compile_definitions(test_crash_trace_win32 PRIVATE DON_BUILD_ID="test")
  target_compile_features(test_crash_trace_win32 PRIVATE cxx_std_20)
  set_target_properties(test_crash_trace_win32 PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
  add_test(NAME test_crash_trace_win32 COMMAND test_crash_trace_win32)
  return()
endif()
add_executable(game_module_test game_module_test.cpp
               "${CMAKE_CURRENT_SOURCE_DIR}/../../src/host/game_module_posix.cpp"
               "${CMAKE_CURRENT_SOURCE_DIR}/../../src/host/iop_native_bridge.cpp")
target_include_directories(game_module_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../src/host"
                           "${CMAKE_CURRENT_SOURCE_DIR}/../../src/game_module")
# The bridge includes the emulator's own iop_cpu.h and iop_memory.h (patch 0085 does the same for iop_native.cpp).
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/../../src/host/iop_native_bridge.cpp" PROPERTIES
                            INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/../../src/game_module;${PS2RECOMP_DIR}/ps2xIOP/src/emulator/core")
target_link_libraries(game_module_test PRIVATE don_host don_setup_for_tests ${CMAKE_DL_LIBS})
target_compile_definitions(game_module_test PRIVATE DON_FIXTURE_DIR="${CMAKE_CURRENT_BINARY_DIR}/fixtures")
enable_testing()
add_test(NAME game_module_test COMMAND game_module_test)
foreach(fixture_kind ok:0 abi:1 size:2 nobind:3 empty:4 nosym:5 iop:6 iopram:7 ioppc:8)
  string(REPLACE ":" ";" fixture_parts "${fixture_kind}")
  list(GET fixture_parts 0 fixture_name)
  list(GET fixture_parts 1 fixture_value)
  add_library(fx_${fixture_name} MODULE game_module_fixture.cpp)
  target_include_directories(fx_${fixture_name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../src/host"
                             "${CMAKE_CURRENT_SOURCE_DIR}/../../src/game_module")
  target_compile_definitions(fx_${fixture_name} PRIVATE DON_GAME_MODULE_BUILD FX_KIND=${fixture_value})
  set_target_properties(fx_${fixture_name} PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixtures")
  add_dependencies(game_module_test fx_${fixture_name})
endforeach()
