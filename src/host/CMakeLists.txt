# The host program of the port (T-110): SDL3 window, the PCSX2 GS library (loaded at run time) connected to the
# PS2Recomp runtime, options, screenshots.
#
# Usable with add_subdirectory(src/host) from a project that already has the target `ps2_runtime` (the runtime built
# with -DPS2X_WITH_RAYLIB=OFF): tests/host does it with a synthetic guest, the root CMakeLists.txt will do it with the
# game. Targets:
#   don_host       static library: don_host_run() (host.h), options, window, GS glue, pad table, screenshots.
#                  Nothing in it refers to generated code. Links ps2_runtime and SDL3::SDL3 (PUBLIC).
#   don_host_main  object library with main.cpp, the real executable's main (needs_elf = true).
# The GS library (libdon_gs) is NOT linked: it is loaded at run time, see gs_glue.h.
cmake_minimum_required(VERSION 3.21)

if(NOT TARGET ps2_runtime)
  message(FATAL_ERROR "src/host needs the target ps2_runtime (add the PS2Recomp runtime first)")
endif()
if(NOT TARGET SDL3::SDL3)
  find_package(SDL3 CONFIG REQUIRED)
endif()

add_library(don_host STATIC
  host.cpp
  options.cpp
  window.cpp
  window_native.cpp
  gs_glue.cpp
  gs_thread.cpp
  fp_env.cpp
  gs_trace.cpp
  audio.cpp
  spu2_glue.cpp
  vu_glue.cpp
  pad_table.cpp
  screenshots.cpp
  input.cpp
  input_sdl.cpp
  input_script.cpp
  profile.cpp
  profile_linux.cpp
  profile_win32.cpp # T-759: the Windows sampler (SuspendThread + GetThreadContext)
  # T-519/T-524: stdout/stderr always go to <user>/don.log and are also tee'd to the original
  # streams (one implementation, both platforms).
  log_redirect.cpp
  # T-520: system information for the [don] start block (OS, CPU, RAM, timer, power plan); the
  # per-platform twins guard themselves.
  sysinfo_posix.cpp
  sysinfo_win32.cpp
  # The crash report (unhandled exception / fatal signals) and, Windows only, the display-driver and
  # power events since the previous start; the per-platform twins guard themselves.
  crash_report_win32.cpp
  crash_report_posix.cpp
  crash_trace.cpp
  crash_trace_win32.cpp
  driver_events_win32.cpp
  build_id.cpp
  # T-517: the start screen (7.2): the state machine (no SDL), the SDL view and layer over netplay.h/upnp.h,
  # and net.ini.
  start_screen_model.cpp
  setup_screen_model.cpp
  setup_flow_model.cpp
  setup_screen.cpp
  setup_chime.cpp
  setup_screen_parts.cpp
  file_browser.cpp
  file_browser_view.cpp
  setup_browser_routing.cpp
  prepare_cli.cpp
  prepare_driver.cpp
  setup_run.cpp
  setup_script.cpp
  setup_demo.cpp
  setup_demo_fixtures.cpp
  start_screen.cpp
  net_config_file.cpp
  net/upnp.cpp
  net/protocol.cpp
  net/lockstep.cpp
  net/netplay.cpp
  net/netplay_client.cpp
  net/netplay_host.cpp
  net/netplay_session.cpp
  net/game_hash.cpp
  net/sim_transport.cpp
  net/udp_socket_posix.cpp
  net/udp_socket_win32.cpp
)
target_compile_features(don_host PUBLIC cxx_std_20)
target_include_directories(don_host
  PUBLIC "${CMAKE_CURRENT_LIST_DIR}"
  # don_gs.h, don_spu2.h and don_vu.h (the C APIs of the three libraries): the headers only, for the
  # types and the DON_* constants. The libraries themselves are loaded at run time (SDL_LoadObject).
  PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../gsbridge"
  PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../spu2bridge"
  PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../vubridge")
# The SDL3 half of audio.cpp (the output stream): the container and this program have SDL3, the pure
# tests build audio.cpp without it. ps2/iop/iop_host_spu2.h comes with ps2_runtime (ps2_iop is PUBLIC).
target_compile_definitions(don_host PRIVATE DON_AUDIO_SDL=1)
target_link_libraries(don_host PUBLIC ps2_runtime SDL3::SDL3)

# T-521 / T-523: the start screen's Dear ImGui view (docs/knowledge/22-netplay.md 7.3). The core is PCSX2's
# vendored copy at third_party/pcsx2/3rdparty/imgui (Dear ImGui 1.92.8, compiled unchanged); the two SDL3
# backends are the upstream copies in ui/imgui_backends/; the theme and widgets are the port's own (the LAS
# Recomp look, one bundled Nunito font).
set(_don_imgui_dir "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/pcsx2/3rdparty/imgui")
set(_don_imgui_core
    "${_don_imgui_dir}/src/imgui.cpp"
    "${_don_imgui_dir}/src/imgui_draw.cpp"
    "${_don_imgui_dir}/src/imgui_tables.cpp"
    "${_don_imgui_dir}/src/imgui_widgets.cpp")
target_sources(don_host PRIVATE
    ${_don_imgui_core}
    ui/imgui_backends/imgui_impl_sdl3.cpp
    ui/imgui_backends/imgui_impl_sdlrenderer3.cpp
    ui/theme.cpp
    ui/widgets.cpp)
# T-523: Nunito (SIL OFL) ships inside the binary. At configure time CMake reads the two .ttf files (kept,
# with their licence, under ui/fonts/) and writes one C array source into the build folder; no font is looked
# up on disk at run time. The same logic as ui/fonts/embed_fonts.cmake (used by the screenshot harness).
set(_don_font_gen_dir "${CMAKE_CURRENT_BINARY_DIR}/ui/fonts")
file(MAKE_DIRECTORY "${_don_font_gen_dir}")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/ui/fonts/Nunito-ExtraBold.ttf" _eb_hex HEX)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/ui/fonts/Nunito-Regular.ttf" _rg_hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _eb_hex "${_eb_hex}")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _rg_hex "${_rg_hex}")
file(WRITE "${_don_font_gen_dir}/fonts_embedded.cpp"
"// Generated by CMake from the TTFs in src/host/ui/fonts/. Do not edit.\n"
"#include \"fonts/fonts_embedded.h\"\n"
"namespace don_host::ui::fonts {\n"
"const unsigned char kExtraBold[] = { ${_eb_hex} };\n"
"const unsigned int kExtraBoldSize = sizeof(kExtraBold);\n"
"const unsigned char kRegular[] = { ${_rg_hex} };\n"
"const unsigned int kRegularSize = sizeof(kRegular);\n"
"} // namespace don_host::ui::fonts\n")
target_sources(don_host PRIVATE "${_don_font_gen_dir}/fonts_embedded.cpp")
target_include_directories(don_host PRIVATE
    "${_don_imgui_dir}/include"
    ui
    ui/imgui_backends)
# PCSX2's imconfig.h enables FreeType (and links imgui_freetype.cpp + FreeType); the start screen uses the
# self-contained stb_truetype rasterizer instead, so no new dependency enters the build (don_imconfig.h).
target_compile_definitions(don_host PRIVATE "IMGUI_USER_CONFIG=\"don_imconfig.h\"")
# Third-party code (the vendored core and the copied backends): compiled as-is, warnings quieted.
if(MSVC)
    set(_don_imgui_nowarn "/w")
else()
    set(_don_imgui_nowarn "-w")
endif()
set_source_files_properties(${_don_imgui_core}
    ui/imgui_backends/imgui_impl_sdl3.cpp
    ui/imgui_backends/imgui_impl_sdlrenderer3.cpp
    PROPERTIES COMPILE_OPTIONS "${_don_imgui_nowarn}")
if(WIN32)
  # T-509: the netplay UDP sockets (net/udp_socket_win32.cpp) use Winsock.
  target_link_libraries(don_host PRIVATE ws2_32)
  # T-520: timeBeginPeriod(1)/timeEndPeriod(1) in sysinfo_win32.cpp (1 ms timer for the audio pacing).
  target_link_libraries(don_host PRIVATE winmm)
  # T-759: EnumProcessModules / GetModuleInformation in profile_win32.cpp.
  target_link_libraries(don_host PRIVATE psapi)
endif()
# AGENTS.md: the game's floating point rules apply to the host as well.
if(MSVC)
  # clang-cl: the GNU spelling of -ffp-contract is ignored; /clang: passes it through.
  target_compile_options(don_host PRIVATE -march=x86-64-v3 /clang:-ffp-contract=off)
else()
  target_compile_options(don_host PRIVATE -march=x86-64-v3 -ffp-contract=off -Wall -Wextra)
endif()

add_library(don_host_main OBJECT main.cpp cpu_check.cpp)
target_link_libraries(don_host_main PUBLIC don_host)
if(MSVC)
  target_compile_options(don_host_main PRIVATE -march=x86-64-v3 /clang:-ffp-contract=off)
else()
  target_compile_options(don_host_main PRIVATE -march=x86-64-v3 -ffp-contract=off)
endif()
# T-518: the processor check runs before anything else and must not itself need AVX2: baseline x86-64 for this
# file only (a source file's options come after the target's on the command line, so this -march wins).
set_source_files_properties(cpu_check.cpp PROPERTIES COMPILE_OPTIONS "-march=x86-64")

# T-505: the build id (build_id.cpp). Computed at configure time; only that file carries the definition, so
# a new commit rebuilds it alone. "<12-hex commit>[-dirty]-<ffmpeg|noffmpeg>", "unknown" outside a checkout.
get_filename_component(_don_repo_root "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
# The packaged program is rebuilt without re-configuring (tools/pipeline/package_linux.py), so CMake must
# re-run configure when the checkout's HEAD moves, or build_id.cpp keeps the previous commit and two builds
# compare equal or refuse to join. The files that encode HEAD are added as configure dependencies
# (CMAKE_CONFIGURE_DEPENDS). `git rev-parse --git-path` resolves the real path in a worktree (where .git is
# a file): HEAD holds the hash when detached, and a symbolic HEAD additionally needs the ref file it points
# to (which may be packed) plus packed-refs.
execute_process(COMMAND git -C "${_don_repo_root}" rev-parse --git-path HEAD
                OUTPUT_VARIABLE _don_head_file OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
execute_process(COMMAND git -C "${_don_repo_root}" symbolic-ref --quiet HEAD
                OUTPUT_VARIABLE _don_head_ref OUTPUT_STRIP_TRAILING_WHITESPACE
                RESULT_VARIABLE _don_head_ref_result)
set(_don_head_ref_file "")
set(_don_packed_refs "")
if(_don_head_ref_result EQUAL 0 AND NOT _don_head_ref STREQUAL "")
  execute_process(COMMAND git -C "${_don_repo_root}" rev-parse --git-path "${_don_head_ref}"
                  OUTPUT_VARIABLE _don_head_ref_file OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  execute_process(COMMAND git -C "${_don_repo_root}" rev-parse --git-path packed-refs
                  OUTPUT_VARIABLE _don_packed_refs OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()
# In the main checkout git prints these relative to the repository root (".git/HEAD"); a loose ref or
# packed-refs may not exist: only existing files are added.
set(_don_build_id_deps)
foreach(_don_dep IN ITEMS "${_don_head_file}" "${_don_head_ref_file}" "${_don_packed_refs}")
  if(NOT _don_dep STREQUAL "")
    get_filename_component(_don_dep "${_don_dep}" ABSOLUTE BASE_DIR "${_don_repo_root}")
    if(EXISTS "${_don_dep}")
      list(APPEND _don_build_id_deps "${_don_dep}")
    endif()
  endif()
endforeach()
if(_don_build_id_deps)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_don_build_id_deps})
endif()
# DON_BUILD_COMMIT (environment): tools/pipeline/package_linux.py builds a clean, detached checkout inside a
# container that may not see the repository's object store (a clone borrowing objects from another checkout),
# so git cannot answer there; the script passes the commit it checked out and no git query is made.
set(_don_dirty_suffix "")
if(NOT "$ENV{DON_BUILD_COMMIT}" STREQUAL "")
  string(SUBSTRING "$ENV{DON_BUILD_COMMIT}" 0 12 _don_git_commit)
else()
  execute_process(COMMAND git -C "${_don_repo_root}" rev-parse --short=12 HEAD
                  OUTPUT_VARIABLE _don_git_commit OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  # `git diff --quiet` exits 0 for no differences and 1 for differences; any other status (128: not a
  # repository, missing objects, unsafe directory) means git could not tell, which must not read as "dirty".
  execute_process(COMMAND git -C "${_don_repo_root}" diff --quiet HEAD
                  RESULT_VARIABLE _don_git_dirty ERROR_QUIET)
  if(_don_git_dirty STREQUAL "1")
    set(_don_dirty_suffix "-dirty")
  elseif(NOT _don_git_dirty STREQUAL "0")
    message(WARNING "git could not compare the work tree with HEAD (${_don_git_dirty}): build id commit is unknown")
    set(_don_git_commit "")
  endif()
endif()
if(_don_git_commit STREQUAL "")
  set(_don_git_commit "unknown")
endif()
if(DON_ENABLE_FFMPEG)
  set(_don_ffmpeg_suffix "ffmpeg")
else()
  set(_don_ffmpeg_suffix "noffmpeg")
endif()
set(DON_BUILD_ID "${_don_git_commit}${_don_dirty_suffix}-${_don_ffmpeg_suffix}")
set_source_files_properties(build_id.cpp PROPERTIES COMPILE_DEFINITIONS "DON_BUILD_ID=\"${DON_BUILD_ID}\"")

# T-515: UPnP (the host opens its own UDP port on the router; only net/upnp.cpp includes miniupnpc).
# Guarded: without the prefix (tools/setup/build_miniupnpc.sh) the port builds and upnp_open reports
# "not built with UPnP" (the #else branch of upnp.cpp).
if(WIN32)
  set(DON_UPNP_LIB "libminiupnpc.lib")
else()
  set(DON_UPNP_LIB "libminiupnpc.a")
endif()
if(WIN32)
  set(_don_upnp_default "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/miniupnpc/windows")
else()
  set(_don_upnp_default "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/miniupnpc/linux")
endif()
set(DON_UPNP_PREFIX "${_don_upnp_default}" CACHE PATH "miniupnpc install prefix (tools/setup/build_miniupnpc.sh)")
if(EXISTS "${DON_UPNP_PREFIX}/lib/${DON_UPNP_LIB}")
  target_include_directories(don_host PRIVATE "${DON_UPNP_PREFIX}/include")
  target_compile_definitions(don_host PRIVATE DON_WITH_UPNP=1)
  target_link_libraries(don_host PRIVATE "${DON_UPNP_PREFIX}/lib/${DON_UPNP_LIB}")
  if(WIN32)
    # miniupnpc is built as a static .lib on Windows; without this its declspec(dllimport) leaves the symbols
    # undefined at link time.
    target_compile_definitions(don_host PRIVATE MINIUPNP_STATICLIB)
    target_link_libraries(don_host PRIVATE ws2_32 iphlpapi)
  endif()
endif()
