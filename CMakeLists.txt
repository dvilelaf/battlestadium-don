cmake_minimum_required(VERSION 3.21)

project(BattleStadiumDON LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(DON_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(DON_GENERATED_DIR "${DON_ROOT}/generated" CACHE PATH "Tree that holds the generated sources and their lists")
option(DON_LINK_GENERATED "Link the generated game code into don" ON)
set(DON_RUNTIME_DIR "${DON_ROOT}/third_party/PS2Recomp" CACHE PATH "PS2Recomp tree with this repository's patches applied")

# ---------------------------------------------------------------------------
# Inputs that must exist before anything is configured.
# The list of generated sources is written by tools/pipeline/generate.sh (a glob over 57,000
# files, and appending them one by one to a list, cost minutes at every configure).
# ---------------------------------------------------------------------------
set(DON_SOURCES_LIST "${DON_GENERATED_DIR}/sources.txt")
set(DON_BIG_SOURCES_LIST "${DON_GENERATED_DIR}/sources_big.txt")
set(DON_NORMAL_SOURCES "")
set(DON_BIG_SOURCES "")
if(DON_LINK_GENERATED)
foreach(f IN ITEMS "${DON_SOURCES_LIST}" "${DON_BIG_SOURCES_LIST}"
                   "${DON_GENERATED_DIR}/ps2_recompiled_functions.h")
    if(NOT EXISTS "${f}")
        message(FATAL_ERROR
            "${f} is missing: generated/ does not exist, is empty or predates the source lists. "
            "Run tools/pipeline/generate.sh (tools/pipeline/generate.sh --lists-only only rewrites the lists), "
            "or configure with -DDON_LINK_GENERATED=OFF to build the program without the game code.")
    endif()
endforeach()
endif()
if(NOT EXISTS "${DON_RUNTIME_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "third_party/PS2Recomp is missing. Run tools/setup/fetch_ps2recomp.sh.")
endif()
# Ordinary files go in unity batches of 32; the big ones (>= 200000 bytes, several GB of
# compiler memory each) are compiled alone, in a smaller job pool.
if(DON_LINK_GENERATED)
    file(STRINGS "${DON_SOURCES_LIST}" DON_NORMAL_SOURCES)
    file(STRINGS "${DON_BIG_SOURCES_LIST}" DON_BIG_SOURCES)
    list(TRANSFORM DON_NORMAL_SOURCES PREPEND "${DON_GENERATED_DIR}/")
    list(TRANSFORM DON_BIG_SOURCES PREPEND "${DON_GENERATED_DIR}/")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${DON_SOURCES_LIST}" "${DON_BIG_SOURCES_LIST}")
else()
    message(STATUS "DON_LINK_GENERATED=OFF: no generated sources; don links the runtime's empty game module")
    # The runtime's own empty don_game_module() and function table (all slots null): the game code arrives later as a
    # module loaded at run time.
    if(WIN32)
        set(DON_GAME_MODULE_LOADER "${CMAKE_CURRENT_SOURCE_DIR}/src/host/game_module_win32.cpp")
    else()
        set(DON_GAME_MODULE_LOADER "${CMAKE_CURRENT_SOURCE_DIR}/src/host/game_module_posix.cpp")
    endif()
    set(DON_NORMAL_SOURCES "${DON_RUNTIME_DIR}/ps2xRuntime/src/runner/register_functions.cpp"
                           "${DON_GAME_MODULE_LOADER}"
                           "${CMAKE_CURRENT_SOURCE_DIR}/src/host/iop_native_bridge.cpp"
                           "${CMAKE_CURRENT_SOURCE_DIR}/src/host/don_runtime_bridge.cpp"
                           "${CMAKE_CURRENT_SOURCE_DIR}/src/host/don_runtime_bridge_fns.cpp")
    # The bridge builds the module's DonRuntime (src/game_module/don_guest_abi.h, put in its own namespace there).
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/host/don_runtime_bridge.cpp" PROPERTIES
        INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/src/game_module"
        SKIP_UNITY_BUILD_INCLUSION ON)
    # T-648a: the module's IOP descriptor (src/game_module/don_iop_abi.h) is registered through the emulator's own types.
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/host/iop_native_bridge.cpp" PROPERTIES
        INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/src/game_module;${DON_RUNTIME_DIR}/ps2xIOP/src/emulator/core"
        SKIP_UNITY_BUILD_INCLUSION ON)
    # That file also defines the function table, which stays; only its empty don_game_module() gives way to the
    # loader's (src/host/game_module_posix.cpp). No runtime patch: the name is changed for this file alone.
    set_source_files_properties("${DON_RUNTIME_DIR}/ps2xRuntime/src/runner/register_functions.cpp" PROPERTIES
        COMPILE_DEFINITIONS "don_game_module=don_runtime_empty_game_module"
        SKIP_UNITY_BUILD_INCLUSION ON)
endif()

# ---------------------------------------------------------------------------
# Options for the whole tree (game, runtime and its dependencies).
# AGENTS.md: -march=x86-64-v3 -ffp-contract=off, and never fast-math.
# ---------------------------------------------------------------------------
if(MSVC)
    # clang-cl (the Windows route, D13): it takes -march=x86-64-v3 as it is but ignores the GNU spelling
    # of -ffp-contract=off (with a warning), and with FMA in x86-64-v3 the generated floating point would
    # fuse. /clang: passes the option through. No /fp:fast anywhere (the runtime's ReleaseMode.cmake adds
    # it, but is not reached with PS2X_WITH_RAYLIB=OFF; check `grep fp:fast build.ninja` stays empty).
    foreach(lang C CXX)
        set(CMAKE_${lang}_FLAGS "${CMAKE_${lang}_FLAGS} -march=x86-64-v3 /clang:-ffp-contract=off")
    endforeach()
    # The MSVC CRT marks getenv, fopen, strncpy... as unsafe; the runtime uses them on every platform.
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    foreach(lang C CXX)
        set(CMAKE_${lang}_FLAGS "${CMAKE_${lang}_FLAGS} -march=x86-64-v3 -ffp-contract=off")
    endforeach()
endif()

# ---------------------------------------------------------------------------
# Optional sanitizer (T-304 Part 1, 2026-10-13): DON_SANITIZE=address builds everything this
# project compiles and links (generated code, the runtime, src/game, src/host) with
# AddressSanitizer and keeps the frame pointer, so that a stack use after return or after scope
# is reported at its first use instead of crashing much later. Empty by default: the normal
# builds are unchanged. The flags above are untouched.
# ---------------------------------------------------------------------------
set(DON_SANITIZE "" CACHE STRING "sanitizer for the whole build (empty, address, undefined, ...)")
if(DON_SANITIZE)
    message(STATUS "DON_SANITIZE=${DON_SANITIZE}: -fsanitize=${DON_SANITIZE} for this project")
    foreach(lang C CXX)
        set(CMAKE_${lang}_FLAGS "${CMAKE_${lang}_FLAGS} -fsanitize=${DON_SANITIZE} -fno-omit-frame-pointer")
    endforeach()
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=${DON_SANITIZE}")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fsanitize=${DON_SANITIZE}")
endif()

# ---------------------------------------------------------------------------
# PS2Recomp runtime (only the runtime: no recompiler, analyzer, test or studio, and no raylib:
# the host program in src/host owns the window, the loop and the input). Nothing is fetched
# from the network: SDL3 comes from CMAKE_PREFIX_PATH (the build container's /opt/deps).
# ---------------------------------------------------------------------------
set(PS2X_WITH_RAYLIB OFF CACHE BOOL "" FORCE)
foreach(opt PS2X_BUILD_RECOMP PS2X_BUILD_ANALYZER PS2X_BUILD_TEST PS2X_BUILD_STUDIO)
    set(${opt} OFF CACHE BOOL "" FORCE)
endforeach()
# FFmpeg-backed decoding of the game's videos (T-305): off by default, so the normal builds are
# unchanged. With it on, the runtime's sceMpeg stub decodes through a static FFmpeg that
# tools/setup/build_ffmpeg.sh builds into third_party/ffmpeg/linux (found by pkg-config; on
# Windows the runtime's own CMake downloads a prebuilt archive, see docs/knowledge/19-videos.md).
option(DON_ENABLE_FFMPEG "Decode the game's videos through FFmpeg (needs tools/setup/build_ffmpeg.sh)" OFF)
set(DON_FFMPEG_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg/linux" CACHE PATH "FFmpeg install prefix (Linux)")
set(PS2X_ENABLE_FFMPEG ${DON_ENABLE_FFMPEG} CACHE BOOL "" FORCE)
if(DON_ENABLE_FFMPEG AND NOT WIN32)
    set(ENV{PKG_CONFIG_PATH} "${DON_FFMPEG_PREFIX}/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
endif()
# The per-function entry tracker is compiled into every generated function when this is on;
# diagnostics must cost nothing when off (AGENTS.md), so it is off here.
option(DON_AGGRESSIVE_LOGS "Compile the runtime's per-function trace into the generated code" OFF)
set(PS2X_ENABLE_AGRESSIVE_LOGS ${DON_AGGRESSIVE_LOGS} CACHE BOOL "" FORCE)
set(PS2X_ENABLE_SCCACHE OFF CACHE BOOL "" FORCE)

add_subdirectory("${DON_RUNTIME_DIR}" "${CMAKE_BINARY_DIR}/ps2recomp" EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# The game executable
# ---------------------------------------------------------------------------
set(DON_RUNTIME_SRC "${DON_RUNTIME_DIR}/ps2xRuntime")

add_executable(don
    ${DON_NORMAL_SOURCES}
    ${DON_BIG_SOURCES}
    src/game/don_overrides.cpp
    src/game/don_overrides_calls.cpp
)

# With FFmpeg the game's own video player works (journal 034), so the port no longer presses Start for it
# by default (src/host/video_skip.h).
if(DON_ENABLE_FFMPEG)
    target_compile_definitions(don PRIVATE DON_WITH_FFMPEG=1)
endif()

set_target_properties(don PROPERTIES
    UNITY_BUILD ON
    UNITY_BUILD_BATCH_SIZE 32
)
if(DON_BIG_SOURCES)
    set_source_files_properties(${DON_BIG_SOURCES} PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
endif()
# generated/register_functions.cpp is a 15 MB table of function registrations, not hot code: a
# single file that never runs a game instruction. With GCC it took 36 minutes at -O3 (T-014
# follow-up), so it is compiled at -O1 in every preset: the dev preset already lowers every
# generated file, the release preset does not.
if(DON_LINK_GENERATED AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties("${DON_GENERATED_DIR}/register_functions.cpp"
        PROPERTIES COMPILE_OPTIONS "-O1")
endif()
# config/native_overrides.txt is the list of addresses the port replaces at run time: the recompiler reads it, and
# don_overrides.cpp checks at startup that every replaceFunction target is in it.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/config/native_overrides.txt")
file(STRINGS "${CMAKE_SOURCE_DIR}/config/native_overrides.txt" DON_NATIVE_OVERRIDE_LINES REGEX "^0x")
set(DON_NATIVE_OVERRIDES "")
foreach(line IN LISTS DON_NATIVE_OVERRIDE_LINES)
    string(REGEX MATCH "^0x[0-9A-Fa-f]+" address "${line}")
    string(APPEND DON_NATIVE_OVERRIDES "${address}u,")
endforeach()
set_source_files_properties(src/game/don_overrides.cpp PROPERTIES COMPILE_DEFINITIONS "DON_NATIVE_OVERRIDES=${DON_NATIVE_OVERRIDES}")
if(CMAKE_BUILD_TYPE STREQUAL "Dev")
    set_property(SOURCE src/game/don_overrides.cpp APPEND PROPERTY COMPILE_DEFINITIONS DON_DEV_BUILD=1)
endif()
# The override is small and unrelated to generated code
set_source_files_properties(src/game/don_overrides.cpp PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)

# Memory control: at most 16 jobs in total (Ninja default), 3 of them compiling big files,
# one link at a time.
set_property(GLOBAL APPEND PROPERTY JOB_POOLS don_big_compile=3 don_link=1)
if(DON_BIG_SOURCES)
    set_source_files_properties(${DON_BIG_SOURCES} PROPERTIES JOB_POOL_COMPILE don_big_compile)
endif()
set_target_properties(don PROPERTIES JOB_POOL_LINK don_link)

if(DON_LINK_GENERATED)
    target_include_directories(don PRIVATE "${DON_GENERATED_DIR}")
else()
    target_compile_definitions(don PRIVATE DON_NO_GENERATED=1)
    target_link_libraries(don PRIVATE ${CMAKE_DL_LIBS})
endif()

# The translated IOP sound module (patch 0085): tools/iop_recomp/make_native.py makes generated/iop_native/adxi_native.cpp
# from the user's own disc (tools/pipeline/generate.sh runs it). Without that file, or with the option OFF, the build is
# exactly as before and the IOP interprets the module.
option(DON_IOP_NATIVE "Compile the translated IOP sound module when generated/iop_native/ has it" ON)
set(DON_IOP_NATIVE_CPP "${DON_GENERATED_DIR}/iop_native/adxi_native.cpp")
if(DON_IOP_NATIVE AND DON_LINK_GENERATED AND EXISTS "${DON_IOP_NATIVE_CPP}")
    if(EXISTS "${DON_RUNTIME_DIR}/ps2xIOP/include/ps2x/iop/iop_native_module.h")
        message(STATUS "IOP native module: ${DON_IOP_NATIVE_CPP}")
        target_sources(don PRIVATE src/host/iop_native.cpp)
        # The generated code includes the emulator's own headers (iop_cpu.h, iop_memory.h); one of the big files.
        set_source_files_properties(src/host/iop_native.cpp PROPERTIES
            INCLUDE_DIRECTORIES "${DON_RUNTIME_DIR}/ps2xIOP/src/emulator/core"
            OBJECT_DEPENDS "${DON_IOP_NATIVE_CPP}"
            SKIP_UNITY_BUILD_INCLUSION ON
            JOB_POOL_COMPILE don_big_compile)
    else()
        message(STATUS "IOP native module: generated/iop_native exists but PS2Recomp lacks patch 0085, not compiled")
    endif()
endif()
# The host program: SDL3 window, the GS library (loaded at run time), options, screenshots.
# don_host_main is its main(); the runtime's own main (and raylib) are not part of this program.
add_subdirectory(src/host "${CMAKE_BINARY_DIR}/src-host")
if(DON_ENABLE_FFMPEG)
    # The host reads the same definition as the game's overrides (src/host/video_skip.h).
    target_compile_definitions(don_host PRIVATE DON_WITH_FFMPEG=1)
endif()
# First-run setup code (src/setup): one static library linked into don. The platform globs may match nothing yet.
find_package(ZLIB REQUIRED)
if(WIN32)
    file(GLOB DON_SETUP_SRC CONFIGURE_DEPENDS "${DON_ROOT}/src/setup/*.cpp"
                                              "${DON_ROOT}/src/setup/platform/*_win32.cpp")
else()
    file(GLOB DON_SETUP_SRC CONFIGURE_DEPENDS "${DON_ROOT}/src/setup/*.cpp"
                                              "${DON_ROOT}/src/setup/platform/*_posix.cpp"
                                              "${DON_ROOT}/src/setup/platform/*_linux.cpp")
endif()
add_library(don_setup STATIC ${DON_SETUP_SRC})
target_include_directories(don_setup PUBLIC "${DON_ROOT}/src/setup")
target_link_libraries(don_setup PUBLIC ZLIB::ZLIB)
target_compile_options(don_setup PRIVATE -ffp-contract=off)
target_link_libraries(don PRIVATE don_host don_host_main ps2_runtime don_setup)
if(NOT DON_LINK_GENERATED)
    # main.cpp loads the --game-module before the runtime exists (src/host/game_module_posix.cpp).
    target_compile_definitions(don_host_main PRIVATE DON_NO_GENERATED=1)
    target_include_directories(don_host_main PRIVATE src/host)
endif()
if(WIN32)
    # The linker's stack reserve for the program: the main thread and every thread started without an
    # explicit stack size, which on Windows takes it from the PE header (1 MB by default; glibc gives a
    # std::thread 8 MB). That covers the game thread (PS2Runtime::startGameThread, a std::thread: the
    # generated functions call each other as C++ calls) and the host's other threads (audio, input).
    # Threads of the three libraries loaded at run time are created by them, and use the same default.
    # Reserve only: the pages are committed as they are touched. The size is 16 MB, twice the Linux
    # default, until the deepest recursion is measured (docs/knowledge/17-windows-toolchain.md).
    target_link_options(don PRIVATE "LINKER:/STACK:16777216")
endif()
if(MSVC)
    # T-519: the player double-clicks the .exe, so it is a window application and Windows opens no console
    # for it. main() stays the entry point: /SUBSYSTEM:WINDOWS alone would look for WinMain (the CRT's
    # WinMainCRTStartup), so mainCRTStartup, which calls main, is named. What the program prints when there
    # is no console (and there is a console again when it is started from one) is src/host/log_redirect_*.
    target_link_options(don PRIVATE "/SUBSYSTEM:WINDOWS" "/ENTRY:mainCRTStartup")
endif()
if(UNIX AND NOT APPLE)
    # SDL3 (copied next to the executable by tools/pipeline/build.sh) and the GS library folder.
    set_target_properties(don PROPERTIES BUILD_RPATH "$ORIGIN;$ORIGIN/../gs" SKIP_BUILD_RPATH OFF)
endif()

# Optimisation level of the executable's own objects (generated code, overrides, main), for
# the development preset. Empty = whatever CMAKE_BUILD_TYPE gives (-O3 in Release).
# Without Release/RelWithDebInfo the runtime does not enable link-time optimisation.
set(DON_GENERATED_OPT "" CACHE STRING "Value N of -ON for the executable's own sources (dev preset)")
if(DON_GENERATED_OPT)
    target_compile_options(don PRIVATE "-O${DON_GENERATED_OPT}")
endif()

# Precompiled header with what every generated file includes (the declarations header is
# 4.9 MB). Off by default: see the measurements in the T-014 follow-up report.
option(DON_PCH "Precompile the headers every generated file includes" OFF)
if(DON_PCH AND DON_LINK_GENERATED)
    target_precompile_headers(don PRIVATE
        <stdexcept>
        "${DON_RUNTIME_SRC}/include/ps2_runtime_macros.h"
        "${DON_RUNTIME_SRC}/include/ps2_runtime.h"
        "${DON_GENERATED_DIR}/ps2_recompiled_functions.h"
        "${DON_GENERATED_DIR}/ps2_recompiled_stubs.h"
        "${DON_RUNTIME_SRC}/include/ps2_syscalls.h"
        "${DON_RUNTIME_SRC}/include/ps2_stubs.h")
endif()
