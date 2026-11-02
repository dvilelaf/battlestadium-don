# SPDX-License-Identifier: GPL-3.0-or-later
# libdon_spu2: PCSX2's SPU2 core (v2.8.2, compiled unmodified from third_party/pcsx2) behind a C
# API (don_spu2.h), plus a self-test program that uses only that API. A standalone project, not
# part of PCSX2's CMake tree: PCSX2's SPU2 sources are compiled here against replacement headers
# (shim/) so that the library does not carry the rest of the emulator. Built by
# tools/pipeline/build_spu2.sh inside the build container:
#
#   cmake -S src/spu2bridge -B build/pcsx2-spu2 -G Ninja -DCMAKE_BUILD_TYPE=Release \
#         -DDON_PCSX2_SOURCE_DIR=third_party/pcsx2
cmake_minimum_required(VERSION 3.20)
project(don_spu2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE Release)
endif()

set(DON_PCSX2_SOURCE_DIR "" CACHE PATH "PCSX2 checkout (v2.8.2, fd9d310c), read only")
if(NOT EXISTS "${DON_PCSX2_SOURCE_DIR}/pcsx2/SPU2/spu2sys.cpp")
	message(FATAL_ERROR "DON_PCSX2_SOURCE_DIR does not contain pcsx2/SPU2: '${DON_PCSX2_SOURCE_DIR}'")
endif()
set(_spu2 "${DON_PCSX2_SOURCE_DIR}/pcsx2/SPU2")

# Not compiled: spu2.cpp (audio backend, VMManager and GS capture glue; replaced by don_spu2.cpp),
# Debug.cpp and Wavedump_wav.cpp (development builds only).
add_library(don_spu2 SHARED
	${_spu2}/ADSR.cpp
	${_spu2}/Dma.cpp
	${_spu2}/Mixer.cpp
	${_spu2}/ReadInput.cpp
	${_spu2}/RegTable.cpp
	${_spu2}/Reverb.cpp
	${_spu2}/ReverbResample.cpp
	${_spu2}/spu2sys.cpp
	${_spu2}/spu2freeze.cpp
	${CMAKE_CURRENT_LIST_DIR}/don_spu2.cpp
)

set_target_properties(don_spu2 PROPERTIES
	OUTPUT_NAME don_spu2
	CXX_VISIBILITY_PRESET hidden
	VISIBILITY_INLINES_HIDDEN YES
	POSITION_INDEPENDENT_CODE ON
	LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
	BUILD_WITH_INSTALL_RPATH ON
	INSTALL_RPATH "\$ORIGIN"
)

# Order matters: shim/ first so that it wins over PCSX2 headers of the same name; pcsx2/ only
# provides SPU2/*.h.
target_include_directories(don_spu2
	PUBLIC "${CMAKE_CURRENT_LIST_DIR}"
	PRIVATE "${CMAKE_CURRENT_LIST_DIR}/shim" "${DON_PCSX2_SOURCE_DIR}/pcsx2"
)
target_compile_definitions(don_spu2 PRIVATE DON_SPU2_BUILD=1)
if(MSVC)
	# clang-cl (the Windows route, D13): the forced include is /FI (it takes -include's argument for a
	# source file) and the GNU options it does not know are passed with /clang:.
	target_compile_options(don_spu2 PRIVATE
		"/FI${CMAKE_CURRENT_LIST_DIR}/shim/don_spu2_prelude.h"
		/clang:-fno-strict-aliasing
		/clang:-ffp-contract=off
	)
else()
	target_compile_options(don_spu2 PRIVATE
		"SHELL:-include \"${CMAKE_CURRENT_LIST_DIR}/shim/don_spu2_prelude.h\""
		# As PCSX2's own CMakeLists.
		-fno-strict-aliasing
		# PCSX2 uses -ffp-contract=fast; the only float math outside the integer mixer is the output
		# stage (DC filter), kept FMA-free so every host produces the same samples.
		-ffp-contract=off
	)
endif()

# D11: only a C API, no dependency on the host's C++ runtime.
if(NOT MSVC)
	target_link_options(don_spu2 PRIVATE -static-libstdc++ -static-libgcc)
endif()
if(UNIX AND NOT APPLE)
	target_link_options(don_spu2 PRIVATE
		"LINKER:--version-script=${CMAKE_CURRENT_LIST_DIR}/don_spu2.map")
	set_property(TARGET don_spu2 APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/don_spu2.map")
endif()

# spu2_selftest: uses only the C API of the library (no PCSX2 include path on purpose).
add_executable(spu2_selftest spu2_selftest.cpp)
target_include_directories(spu2_selftest PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(spu2_selftest PRIVATE don_spu2)
set_target_properties(spu2_selftest PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
	BUILD_WITH_INSTALL_RPATH ON
	INSTALL_RPATH "\$ORIGIN"
)
if(NOT MSVC)
	target_link_options(spu2_selftest PRIVATE -static-libstdc++ -static-libgcc)
endif()
