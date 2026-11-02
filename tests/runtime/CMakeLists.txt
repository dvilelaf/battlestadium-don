# Tests of the PS2Recomp runtime and IOP emulator that need neither the game nor the PCSX2 libraries.
#
#   cmake -S tests/runtime -B build/rt-tests -DPS2RECOMP_DIR=<path of a PS2Recomp tree with the patches applied>
#   cmake --build build/rt-tests -j6
#   DON_RT_TESTS_DIR=build/rt-tests uv run --no-project --with pytest python -I -m pytest tests/runtime -q
#
# PS2RECOMP_DIR defaults to third_party/PS2Recomp. One executable per topic; adding one is a line in the
# "Tests" section below (plus the .cpp). Tests that need only the IOP emulator link ps2_iop and nothing else.
cmake_minimum_required(VERSION 3.21)
project(don_runtime_tests LANGUAGES CXX)

set(PS2RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/PS2Recomp" CACHE PATH
    "PS2Recomp tree (with this repository's patches applied) to test")
get_filename_component(PS2RECOMP_DIR "${PS2RECOMP_DIR}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
if(NOT EXISTS "${PS2RECOMP_DIR}/ps2xIOP/CMakeLists.txt")
  message(FATAL_ERROR "PS2RECOMP_DIR=${PS2RECOMP_DIR} has no ps2xIOP/CMakeLists.txt")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
if(NOT MSVC)
  # The project's floating point rules (AGENTS.md), applied to the code under test as well.
  add_compile_options(-march=x86-64-v3 -ffp-contract=off)
endif()

# The IOP emulator is a standalone library: no window, no network, no other dependency.
add_subdirectory("${PS2RECOMP_DIR}/ps2xIOP" "${CMAKE_BINARY_DIR}/ps2xIOP" EXCLUDE_FROM_ALL)

# The runtime library (ps2_runtime), for tests that exercise the GIF arbiter, the GS hook, the memory
# or the VU1 without a window. Same options as the root CMakeLists.txt. Its configure step fetches
# raylib (and two more libraries) from the network. Tests linking ps2_runtime are skipped when
# DON_RT_BUILD_RUNTIME is OFF.
option(DON_RT_BUILD_RUNTIME "Configure the PS2Recomp runtime library for the tests that link it" ON)
# DON_RT_FFMPEG=ON builds the runtime with FFmpeg (found by pkg-config, as DON_ENABLE_FFMPEG does in the root
# CMakeLists.txt: DON_FFMPEG_PREFIX) and adds the test that needs a real decoder (mpeg_ipu_dma_test).
option(DON_RT_FFMPEG "Build the runtime with FFmpeg and the tests that decode a stream" OFF)
set(DON_FFMPEG_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/ffmpeg/linux" CACHE PATH "FFmpeg install prefix (Linux)")
if(DON_RT_BUILD_RUNTIME)
  foreach(opt PS2X_BUILD_RECOMP PS2X_BUILD_ANALYZER PS2X_BUILD_TEST PS2X_BUILD_STUDIO PS2X_ENABLE_FFMPEG
              PS2X_ENABLE_AGRESSIVE_LOGS PS2X_ENABLE_SCCACHE)
    set(${opt} OFF CACHE BOOL "" FORCE)
  endforeach()
  if(DON_RT_FFMPEG AND NOT WIN32)
    get_filename_component(DON_FFMPEG_PREFIX "${DON_FFMPEG_PREFIX}" ABSOLUTE)
    set(ENV{PKG_CONFIG_PATH} "${DON_FFMPEG_PREFIX}/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
    set(PS2X_ENABLE_FFMPEG ON CACHE BOOL "" FORCE)
  endif()
  add_subdirectory("${PS2RECOMP_DIR}/ps2xRuntime" "${CMAKE_BINARY_DIR}/ps2xRuntime" EXCLUDE_FROM_ALL)
endif()

function(don_rt_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE ${ARGN})
endfunction()

# ---- Tests -----------------------------------------------------------------------------------------
don_rt_test(iop_host_spu2_test ps2_iop)
don_rt_test(module_buffer_load_test ps2_iop)
# T-304: the IOP heap holds the blocks of the game's modules plus the effects driver's (patch 0057).
don_rt_test(iop_heap_capacity_test ps2_iop)
target_include_directories(iop_heap_capacity_test PRIVATE "${PS2RECOMP_DIR}/ps2xIOP/src/emulator")
# T-304, patch 0059: the IOP respects the guest's interrupt-suspend state (per thread).
don_rt_test(iop_intr_suspend_test ps2_iop)
# T-304, patch 0062: a thread with interrupts suspended is not preempted at the end of a slice.
don_rt_test(iop_no_preempt_test ps2_iop)
# T-304, patch 0058: freed heap space is offered again; a refused request is reported.
don_rt_test(iop_heap_reuse_test ps2_iop)
# T-305, patch 0071: the end of an SPU2 DMA comes 24 IOP cycles per halfword after its start.
don_rt_test(iop_spu2_dma_time_test ps2_iop)
target_include_directories(iop_spu2_dma_time_test PRIVATE "${PS2RECOMP_DIR}/ps2xIOP/src/emulator")
target_include_directories(iop_heap_reuse_test PRIVATE "${PS2RECOMP_DIR}/ps2xIOP/src/emulator")
# T-305, patch 0072: an RPC handler runs after the IOP threads of higher priority than its server thread.
don_rt_test(iop_rpc_server_priority_test ps2_iop)
# T-214, patch 0067: the disc files are read once in the background (header-only helper).
don_rt_test(file_prefetch_test ps2_iop)
target_include_directories(file_prefetch_test PRIVATE "${PS2RECOMP_DIR}/ps2xRuntime/include")
if(TARGET ps2_runtime)
  don_rt_test(host_gs_hook_test ps2_runtime)
  don_rt_test(host_vu1_hook_test ps2_runtime)
  # T-320: the third path held by MSKPATH3 leaves one GIF packet per lifting of the mask.
  don_rt_test(path3_release_test ps2_runtime)
  # T-202 part 1: the lanes of the runtime's MMI macros (the writer of the game's VU1 constant block).
  don_rt_test(mmi_lanes_test ps2_runtime)
  # host_loop_test, host_pad_source_test, init_thread_priority_test, change_priority_return_test: a real
  # PS2Runtime with host functions as the guest (no window). With raylib in the build (the default; absent with
  # -DPS2X_WITH_RAYLIB=OFF, patch 0009) the raylib functions the runtime references are replaced by recorders
  # in the test (host_test_support.h): Linux only, it needs the linker option below.
  set(host_tests host_loop_test host_pad_source_test init_thread_priority_test change_priority_return_test guest_heap_limit_test host_gs_display_test iop_batch_test iop_batch_proxy_test scheduler_fast_path_test ee_timer_test mpeg_stub_code_test pacing_neutral_test irq_di_section_test unwind_not_return_test mc_file_numbers_test mc_read_only_test pad_actuator_test gs_dbuff_frame_mode_test thread_wake_return_test thread_status_pacing_wait_test session_clock_test)
  if(DON_RT_FFMPEG)
    # T-305, patch 0064: needs a real MPEG decoder.
    list(APPEND host_tests mpeg_ipu_dma_test)
  else()
    # T-305, patch 0070: with no FFmpeg the stub decodes black pictures.
    list(APPEND host_tests mpeg_black_decoder_test)
  endif()
  foreach(host_test ${host_tests})
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${host_test}.cpp")
      don_rt_test(${host_test} ps2_runtime)
      if(TARGET raylib AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_compile_definitions(${host_test} PRIVATE DON_RT_HAS_RAYLIB=1)
        target_link_options(${host_test} PRIVATE -Wl,--allow-multiple-definition)
      endif()
    endif()
  endforeach()
endif()
