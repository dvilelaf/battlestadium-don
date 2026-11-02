# Tests of the first-run disc handling (task B-01). No game file is read: the images are built in the test.
#
#   cmake -S tests/setup -B build/setup-tests
#   cmake --build build/setup-tests -j4
#   ctest --test-dir build/setup-tests --output-on-failure
#
# Every src/setup/*.cpp goes into the library don_setup, and every tests/setup/test_setup_*.cpp becomes one test
# program: a task adds its files and never edits this list.
cmake_minimum_required(VERSION 3.21)
project(don_setup_tests LANGUAGES CXX)

get_filename_component(DON_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(SETUP_DIR "${DON_REPO_ROOT}/src/setup")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
if(NOT MSVC)
  add_compile_options(-ffp-contract=off -Wall -Wextra)
endif()
enable_testing()

find_package(ZLIB REQUIRED)

# The platform twins: *_win32.cpp on Windows (cross-built with cmake/toolchain-windows-clangcl.cmake, run under Wine),
# *_posix.cpp and *_linux.cpp elsewhere.
if(WIN32)
  file(GLOB SETUP_PLATFORM_SRC CONFIGURE_DEPENDS "${SETUP_DIR}/platform/*_win32.cpp")
else()
  file(GLOB SETUP_PLATFORM_SRC CONFIGURE_DEPENDS "${SETUP_DIR}/platform/*_posix.cpp" "${SETUP_DIR}/platform/*_linux.cpp")
endif()
file(GLOB SETUP_SRC CONFIGURE_DEPENDS "${SETUP_DIR}/*.cpp")
list(APPEND SETUP_SRC ${SETUP_PLATFORM_SRC})
add_library(don_setup STATIC ${SETUP_SRC})
target_include_directories(don_setup PUBLIC "${SETUP_DIR}")
target_link_libraries(don_setup PUBLIC ZLIB::ZLIB)

file(GLOB SETUP_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_setup_*.cpp")
# Not built for Windows, each for a reason that is the test's subject and has no Windows meaning (the Windows
# twins of these subjects are tested by test_platform_win32, test_startup_discover_win32 and test_process_win32):
#   instance, user_paths  fork() a second process that takes the lock (twin: test_platform_win32)
#   process, startup      fork/exec/waitpid on POSIX children (twin: test_process_win32, test_startup_discover_win32)
#   governor              fake /proc and /sys trees and kill(pid, 0) (twin: test_platform_win32)
#   file_browser,         the POSIX places ("/"), symlink loops, mode-000 folders and invalid UTF-8 file names; the
#   browser_crash         Windows places (drive letters) are "Later" in docs/tasks/M7-windows.md; browser_crash also
#                         names a folder with Japanese characters, which fs::path::string() cannot convert on Windows
set(SETUP_LINUX_ONLY test_setup_instance test_setup_user_paths test_setup_process test_setup_startup
                     test_setup_governor test_setup_file_browser test_setup_browser_crash)
foreach(test_src ${SETUP_TESTS})
  get_filename_component(test_name "${test_src}" NAME_WE)
  if(WIN32 AND test_name IN_LIST SETUP_LINUX_ONLY)
    message(STATUS "${test_name}: skipped on Windows (POSIX-only subject, see SETUP_LINUX_ONLY)")
    continue()
  endif()
  add_executable(${test_name} "${test_src}")
  target_link_libraries(${test_name} PRIVATE don_setup)
  if(WIN32)
    target_include_directories(${test_name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/win32_compat")
    target_compile_options(${test_name} PRIVATE /FIunistd.h)  # setenv() in tests that do not include it
    target_compile_definitions(${test_name} PRIVATE _CRT_SECURE_NO_WARNINGS)
  endif()
  add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# T-749: CommandLineToArgvW (the child side of its argv check) lives in shell32.
if(WIN32 AND TARGET test_setup_unicode_paths)
  target_link_libraries(test_setup_unicode_paths PRIVATE shell32)
endif()

# T-730: named for its subject (module_platform.h), so the test_setup_*.cpp glob above does not take it.
add_executable(test_module_platform "${CMAKE_CURRENT_SOURCE_DIR}/test_module_platform.cpp")
target_link_libraries(test_module_platform PRIVATE don_setup)
add_test(NAME test_module_platform COMMAND test_module_platform)

# The Windows twins' own tests (T-731..T-737). test_platform_win32 compiles to a no-op main that passes elsewhere; the
# other two need <windows.h> and exist only in the Windows cross-build.
set(WIN32_TWIN_TESTS test_platform_win32)
if(WIN32)
  list(APPEND WIN32_TWIN_TESTS test_startup_discover_win32 test_process_win32)
endif()
foreach(test_name ${WIN32_TWIN_TESTS})
  add_executable(${test_name} "${CMAKE_CURRENT_SOURCE_DIR}/${test_name}.cpp")
  target_link_libraries(${test_name} PRIVATE don_setup)
  if(WIN32)
    target_compile_definitions(${test_name} PRIVATE _CRT_SECURE_NO_WARNINGS)
  endif()
  add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
