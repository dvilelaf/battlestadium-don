# Tests of the host program's sound output (T-303): the pure part of src/host/audio.{h,cpp} (ring,
# float to 16-bit conversion, WAV writer) always; with -DDON_AUDIO_SDL=ON (in the build container, which
# has SDL3) also the SDL3 part of audio.cpp and the glue src/host/spu2_glue.{h,cpp} against the real
# library build/spu2/libdon_spu2.so.
#
#   cmake -S tests/audio -B build/audio-tests -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DDON_AUDIO_SDL=ON
#   cmake --build build/audio-tests -j2
#   DON_AUDIO_TESTS_DIR=build/audio-tests uv run --no-project --with pytest python -I -m pytest tests/audio -q
#
# The pure target needs nothing but a C++20 compiler (it is built with the system compiler too); the SDL
# targets are turned on by the option so that the pure part also builds where SDL3 is not installed.
#
# test_spu2_glue needs the IOP host SPU2 patch applied to the PS2Recomp tree
# (third_party/PS2Recomp/ps2xIOP/include/ps2x/iop/iop_host_spu2.h, patch 0006); DON_PS2RECOMP_DIR selects
# another tree.
cmake_minimum_required(VERSION 3.21)
project(don_audio_tests LANGUAGES CXX)

option(DON_AUDIO_SDL "Also build the SDL3 targets (the audio device and the SPU2 library glue)" OFF)

get_filename_component(DON_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(HOST_SRC "${DON_REPO_ROOT}/src/host")
set(SPU2BRIDGE_INCLUDE_DIR "${DON_REPO_ROOT}/src/spu2bridge")
set(DON_PS2RECOMP_DIR "${DON_REPO_ROOT}/third_party/PS2Recomp" CACHE PATH
    "PS2Recomp tree with patches/ps2recomp applied (for ps2x/iop/iop_host_spu2.h)")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
if(NOT MSVC)
  add_compile_options(-ffp-contract=off -Wall -Wextra)
endif()

# The pure part of audio.cpp; with the option it is compiled with the SDL3 part as well.
add_library(don_audio STATIC "${HOST_SRC}/audio.cpp")
target_include_directories(don_audio PUBLIC "${HOST_SRC}" "${SPU2BRIDGE_INCLUDE_DIR}")

add_executable(test_audio_pure test_audio_pure.cpp)
target_link_libraries(test_audio_pure PRIVATE don_audio)

if(DON_AUDIO_SDL)
  find_package(SDL3 REQUIRED CONFIG)
  target_compile_definitions(don_audio PUBLIC DON_AUDIO_SDL=1)
  target_link_libraries(don_audio PUBLIC SDL3::SDL3)

  if(NOT EXISTS "${DON_PS2RECOMP_DIR}/ps2xIOP/include/ps2x/iop/iop_host_spu2.h")
    message(FATAL_ERROR "DON_PS2RECOMP_DIR=${DON_PS2RECOMP_DIR} has no ps2x/iop/iop_host_spu2.h; "
                        "apply patches/ps2recomp (bash tools/setup/fetch_ps2recomp.sh)")
  endif()
  add_library(don_spu2_glue STATIC "${HOST_SRC}/spu2_glue.cpp")
  target_include_directories(don_spu2_glue PUBLIC "${HOST_SRC}" "${SPU2BRIDGE_INCLUDE_DIR}"
                                                  "${DON_PS2RECOMP_DIR}/ps2xIOP/include")
  target_link_libraries(don_spu2_glue PUBLIC SDL3::SDL3)

  add_executable(test_spu2_glue test_spu2_glue.cpp)
  target_link_libraries(test_spu2_glue PRIVATE don_spu2_glue don_audio)
  target_compile_definitions(test_spu2_glue PRIVATE DON_REPO_ROOT="${DON_REPO_ROOT}")

  add_executable(test_audio_sdl test_audio_sdl.cpp)
  target_link_libraries(test_audio_sdl PRIVATE don_audio SDL3::SDL3)
endif()
