# Tests of the netplay pieces (D14, docs/knowledge/22-netplay.md, tasks T-501..T-508). They need neither the game
# nor SDL: only src/host/net/*.cpp and one header of the runtime (runtime/ps2_host_pad.h).
#
#   cmake -S tests/net -B build/net-tests
#   cmake --build build/net-tests -j2
#   DON_NET_TESTS_DIR=build/net-tests uv run --no-project --with pytest python -I -m pytest tests/net -q
#
# Every src/host/net/*.cpp goes into the library don_net (on Linux without *_win32.cpp, on Windows without
# *_posix.cpp), and every tests/net/test_net_*.cpp becomes one test program: a task adds its files and never
# edits this list.
cmake_minimum_required(VERSION 3.21)
project(don_net_tests LANGUAGES CXX)

set(DON_HOST_PAD_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/PS2Recomp/ps2xRuntime/include" CACHE PATH
    "Folder that contains runtime/ps2_host_pad.h")
get_filename_component(DON_HOST_PAD_INCLUDE_DIR "${DON_HOST_PAD_INCLUDE_DIR}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
if(NOT EXISTS "${DON_HOST_PAD_INCLUDE_DIR}/runtime/ps2_host_pad.h")
  message(FATAL_ERROR "DON_HOST_PAD_INCLUDE_DIR=${DON_HOST_PAD_INCLUDE_DIR} has no runtime/ps2_host_pad.h")
endif()

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
find_package(Threads REQUIRED)

file(GLOB NET_SRC CONFIGURE_DEPENDS "${HOST_SRC}/net/*.cpp")
if(WIN32)
  list(FILTER NET_SRC EXCLUDE REGEX "_posix\\.cpp$")
else()
  list(FILTER NET_SRC EXCLUDE REGEX "_win32\\.cpp$")
endif()
if(NET_SRC)
  add_library(don_net STATIC ${NET_SRC})
  target_include_directories(don_net PUBLIC "${HOST_SRC}" "${HOST_SRC}/net" "${DON_HOST_PAD_INCLUDE_DIR}")
  target_link_libraries(don_net PUBLIC Threads::Threads)
  if(WIN32)
    target_link_libraries(don_net PUBLIC ws2_32)
  endif()
else()
  add_library(don_net INTERFACE)
  target_include_directories(don_net INTERFACE "${HOST_SRC}" "${HOST_SRC}/net" "${DON_HOST_PAD_INCLUDE_DIR}")
endif()

file(GLOB NET_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_net_*.cpp")
foreach(test_src ${NET_TESTS})
  get_filename_component(test_name "${test_src}" NAME_WE)
  add_executable(${test_name} "${test_src}")
  target_link_libraries(${test_name} PRIVATE don_net)
  target_compile_definitions(${test_name} PRIVATE DON_REPO_ROOT="${DON_REPO_ROOT}")
endforeach()
