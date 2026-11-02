# Tests of the host program's VU1 glue (T-205 part 2): src/host/vu_glue.{h,cpp} connected to the REAL
# library build/vu/libdon_vu.so, in the build container (SDL3, clang 17, cmake, ninja).
#
#   cmake -S tests/vuglue -B build/vuglue-tests -G Ninja -DCMAKE_CXX_COMPILER=clang++
#   cmake --build build/vuglue-tests -j2
#   DON_VUGLUE_TESTS_DIR=build/vuglue-tests uv run --no-project --with pytest python -I -m pytest tests/vuglue -q
#
# test_vu_glue plays the runtime's part against the real library (no game, no runtime): it calls the
# callbacks of runtime/ps2_host_vu1.h exactly as the runtime's VIF1 does. test_vu_select checks the
# pure parser of DON_VU1.
#
# test_vu_glue needs the host VU1 hook applied to the PS2Recomp tree
# (third_party/PS2Recomp/ps2xRuntime/include/runtime/ps2_host_vu1.h, patch 0020); DON_PS2RECOMP_DIR
# selects another tree.
cmake_minimum_required(VERSION 3.21)
project(don_vuglue_tests LANGUAGES CXX)

get_filename_component(DON_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(HOST_SRC "${DON_REPO_ROOT}/src/host")
set(VUBRIDGE_INCLUDE_DIR "${DON_REPO_ROOT}/src/vubridge")
set(DON_PS2RECOMP_DIR "${DON_REPO_ROOT}/third_party/PS2Recomp" CACHE PATH
    "PS2Recomp tree with patches/ps2recomp applied (for runtime/ps2_host_vu1.h)")

find_package(SDL3 REQUIRED CONFIG)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
if(NOT MSVC)
  # AGENTS.md: the game's floating point rules apply to the host program, and this test exercises it.
  add_compile_options(-ffp-contract=off -Wall -Wextra)
endif()

if(NOT EXISTS "${DON_PS2RECOMP_DIR}/ps2xRuntime/include/runtime/ps2_host_vu1.h")
  message(FATAL_ERROR "DON_PS2RECOMP_DIR=${DON_PS2RECOMP_DIR} has no runtime/ps2_host_vu1.h; "
                      "apply patches/ps2recomp (bash tools/setup/fetch_ps2recomp.sh)")
endif()

# The glue itself, as the host program builds it (src/host/CMakeLists.txt adds it to don_host; this
# project compiles the one file so that it needs nothing else of the host).
add_library(don_vu_glue STATIC "${HOST_SRC}/vu_glue.cpp")
target_include_directories(don_vu_glue PUBLIC "${HOST_SRC}" "${VUBRIDGE_INCLUDE_DIR}"
                                              "${DON_PS2RECOMP_DIR}/ps2xRuntime/include")
target_link_libraries(don_vu_glue PUBLIC SDL3::SDL3)

add_executable(test_vu_glue test_vu_glue.cpp)
target_link_libraries(test_vu_glue PRIVATE don_vu_glue)
target_compile_definitions(test_vu_glue PRIVATE DON_REPO_ROOT="${DON_REPO_ROOT}")

add_executable(test_vu_select test_vu_select.cpp)
target_link_libraries(test_vu_select PRIVATE don_vu_glue)

# T-304: the object owns its table (no reference to the caller's DonVuApi). No library: the table is
# fake, so the executable runs and passes even where build/vu is not there.
add_executable(test_vu_glue_lifetime test_vu_glue_lifetime.cpp)
target_link_libraries(test_vu_glue_lifetime PRIVATE don_vu_glue)
