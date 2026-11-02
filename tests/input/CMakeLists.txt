# Tests of the input layer (T-111) and the scripted input (T-112). They need neither the game, nor a gamepad,
# nor SDL: only src/host/input.cpp, src/host/input_script.cpp and one header of the runtime.
#
#   cmake -S tests/input -B build/input-tests -DDON_HOST_PAD_INCLUDE_DIR=<dir that holds runtime/ps2_host_pad.h>
#   cmake --build build/input-tests -j2
#   DON_INPUT_TESTS_DIR=build/input-tests uv run --no-project --with pytest python -I -m pytest tests/input -q
#
# DON_HOST_PAD_INCLUDE_DIR defaults to the PS2Recomp include folder of third_party (it holds the header once
# the patches are applied there). With -DDON_INPUT_SDL=ON (in the build container, which has SDL3) the SDL
# side (src/host/input_sdl.cpp) is compiled too, and a small program that feeds it synthetic events is built.
cmake_minimum_required(VERSION 3.21)
project(don_input_tests LANGUAGES CXX)

set(DON_HOST_PAD_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/PS2Recomp/ps2xRuntime/include" CACHE PATH
    "Folder that contains runtime/ps2_host_pad.h")
get_filename_component(DON_HOST_PAD_INCLUDE_DIR "${DON_HOST_PAD_INCLUDE_DIR}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
if(NOT EXISTS "${DON_HOST_PAD_INCLUDE_DIR}/runtime/ps2_host_pad.h")
  message(FATAL_ERROR "DON_HOST_PAD_INCLUDE_DIR=${DON_HOST_PAD_INCLUDE_DIR} has no runtime/ps2_host_pad.h")
endif()
option(DON_INPUT_SDL "Also build the SDL3 side (needs SDL3)" OFF)

get_filename_component(DON_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(HOST_SRC "${DON_REPO_ROOT}/src/host")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
if(NOT MSVC)
  add_compile_options(-ffp-contract=off -Wall -Wextra)
endif()

add_library(don_input STATIC "${HOST_SRC}/input.cpp" "${HOST_SRC}/input_script.cpp")
target_include_directories(don_input PUBLIC "${HOST_SRC}" "${DON_HOST_PAD_INCLUDE_DIR}")

add_executable(test_input_mapping test_input_mapping.cpp)
target_link_libraries(test_input_mapping PRIVATE don_input)

add_executable(test_input_script test_input_script.cpp)
target_link_libraries(test_input_script PRIVATE don_input)
target_compile_definitions(test_input_script PRIVATE DON_REPO_ROOT="${DON_REPO_ROOT}")

if(DON_INPUT_SDL)
  find_package(SDL3 REQUIRED CONFIG)
  add_library(don_input_sdl STATIC "${HOST_SRC}/input_sdl.cpp")
  target_link_libraries(don_input_sdl PUBLIC don_input SDL3::SDL3)
  add_executable(test_input_sdl test_input_sdl.cpp)
  target_link_libraries(test_input_sdl PRIVATE don_input_sdl)
endif()

# T-306: the pad table (two-port and multitap descriptions).
add_executable(test_input_pad_table test_input_pad_table.cpp "${HOST_SRC}/pad_table.cpp")
target_link_libraries(test_input_pad_table PRIVATE don_input)

# T-308: vibration (the command, the renewal schedule, the pad table's store for the runtime's actuator callback).
add_executable(test_input_rumble test_input_rumble.cpp "${HOST_SRC}/pad_table.cpp")
target_link_libraries(test_input_rumble PRIVATE don_input)
