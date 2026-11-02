# Tests of the profiler of src/host/profile.{h,cpp} and src/host/profile_linux.cpp (T-203). They need
# neither the game, nor the runtime, nor SDL: only a C++20 compiler and the system's threads.
#
#   cmake -S tests/profile -B build/profile-tests
#   cmake --build build/profile-tests -j2
#   uv run --no-project --with pytest python -I -m pytest tests/profile -q
#
# The build folder is build/profile-tests, or $DON_PROFILE_TESTS_DIR for the pytest.
#
# profile_probe measures itself with the profiler: a worker thread spends a known proportion of its
# processor time in two functions and writes a profile; test_profile_tests.py runs it, checks the
# profile and runs tools/verify/profile_summary.py on it. On a platform where the sampler is not
# available (profile.cpp's fallback) the program still builds and runs: the pytest then skips the
# checks that need a profile, saying why.
cmake_minimum_required(VERSION 3.21)
project(don_profile_tests LANGUAGES CXX)

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

# profile.cpp is the portable part, profile_linux.cpp the sampler (empty where it does not apply).
add_executable(profile_probe
  profile_probe.cpp
  "${HOST_SRC}/profile.cpp"
  "${HOST_SRC}/profile_linux.cpp"
)
target_include_directories(profile_probe PRIVATE "${HOST_SRC}")
target_link_libraries(profile_probe PRIVATE Threads::Threads)
