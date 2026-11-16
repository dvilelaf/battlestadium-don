# libchdr, to read CHD disc images in the first-run setup (issue #6, src/setup/chd_reader.cpp): PCSX2's vendored copy
# (third_party/pcsx2/3rdparty/libchdr, BSD-3-Clause) with its LZMA (3rdparty/lzma, public domain), built here as
# static libraries, plus zlib and zstd, which the port already has (zstd is a dependency of the GS library: the Linux
# image has it and the Windows prefix of tools/setup/build_win_deps.sh builds it).
#
#   include(<repository>/cmake/libchdr.cmake)  ->  target `libchdr` (CMAKE_CURRENT_LIST_DIR finds the repository)
if(TARGET libchdr)
  return()
endif()
get_filename_component(_don_chd_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(_don_chd_3rd "${_don_chd_root}/third_party/pcsx2/3rdparty")
if(NOT EXISTS "${_don_chd_3rd}/libchdr/CMakeLists.txt")
  message(FATAL_ERROR "third_party/pcsx2 is missing (libchdr). Run tools/setup/fetch_pcsx2.sh.")
endif()
enable_language(C)
find_package(ZLIB REQUIRED)
# zstd: its own CMake package where there is one (the Windows prefix), else PCSX2's FindZstd (pkg-config style).
find_package(zstd CONFIG QUIET)
if(TARGET zstd::libzstd_shared)
  add_library(Zstd::Zstd ALIAS zstd::libzstd_shared)
elseif(TARGET zstd::libzstd_static)
  add_library(Zstd::Zstd ALIAS zstd::libzstd_static)
else()
  list(APPEND CMAKE_MODULE_PATH "${_don_chd_root}/third_party/pcsx2/cmake")
  find_package(Zstd 1.5.5 REQUIRED)
endif()
add_subdirectory("${_don_chd_3rd}/lzma" "${CMAKE_BINARY_DIR}/third_party/lzma" EXCLUDE_FROM_ALL)
add_subdirectory("${_don_chd_3rd}/libchdr" "${CMAKE_BINARY_DIR}/third_party/libchdr" EXCLUDE_FROM_ALL)
# Third-party C: its warnings are not ours to fix.
if(MSVC)
  target_compile_options(libchdr PRIVATE /w)
  target_compile_options(pcsx2-lzma PRIVATE /w)
else()
  target_compile_options(libchdr PRIVATE -w)
  target_compile_options(pcsx2-lzma PRIVATE -w)
endif()
