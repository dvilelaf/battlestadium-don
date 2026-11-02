# SPDX-License-Identifier: GPL-3.0-or-later
# libdon_vu: PCSX2's microVU (the VU recompiler) and VU interpreter (v2.8.2, compiled unmodified
# from third_party/pcsx2) behind a C API (don_vu.h), plus a self-test program that uses only that
# API. A standalone project, not part of PCSX2's CMake tree: PCSX2's VU sources are compiled here
# against the real PCSX2 headers, with the rest of the emulator replaced by small shims (shim/).
# Built by tools/pipeline/build_vu.sh inside the build container:
#
#   cmake -S src/vubridge -B build/pcsx2-vu -G Ninja -DCMAKE_BUILD_TYPE=Release \
#         -DDON_PCSX2_SOURCE_DIR=third_party/pcsx2
#
# Structure after KillzoneRecomp's ext/kzvu (https://github.com/drakolordx7/KillzoneRecomp, commit
# 5f0700079f9ac4e14799d66d01747dbcdcec55f6, GPL-3.0-or-later), ported from MSVC to Linux/clang.
cmake_minimum_required(VERSION 3.20)
project(don_vu LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON) # PCSX2 builds with -std=gnu++20
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE Release)
endif()

if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
	message(FATAL_ERROR "microVU only exists for x86-64")
endif()

set(DON_PCSX2_SOURCE_DIR "" CACHE PATH "PCSX2 checkout (v2.8.2, fd9d310c), read only")
if(NOT EXISTS "${DON_PCSX2_SOURCE_DIR}/pcsx2/x86/microVU.cpp")
	message(FATAL_ERROR "DON_PCSX2_SOURCE_DIR does not contain pcsx2/x86/microVU.cpp: '${DON_PCSX2_SOURCE_DIR}'")
endif()
set(P2 "${DON_PCSX2_SOURCE_DIR}")
set(P2SRC "${P2}/pcsx2")
set(P3 "${P2}/3rdparty")
set(HERE "${CMAKE_CURRENT_LIST_DIR}")

# Compile settings of PCSX2's own targets (cmake/BuildParameters.cmake, as recorded in the GS bridge's
# build.ninja): gnu++20, SSE4.1, no exceptions, no strict aliasing.
add_library(don_vu_flags INTERFACE)
target_compile_definitions(don_vu_flags INTERFACE
	_M_X86=1
	__SSE4_1__=1
	XBYAK_NO_EXCEPTION
	FMT_USE_EXCEPTIONS=0
	FMT_USE_RTTI=0
)
target_compile_options(don_vu_flags INTERFACE
	-msse -msse2 -msse4.1 -mfxsr
	-fno-exceptions
	-w
)
if(WIN32)
	# clang-cl (the Windows route, D13): GNU options it does not know are passed with /clang:. Windows
	# has no -fPIC, no symbol visibility switch (the C API is exported by don_vu.h) and no -pthread.
	# _WIN32 selects PCSX2's Windows code paths; __POSIX__ is not defined there.
	target_compile_options(don_vu_flags INTERFACE
		/clang:-fno-strict-aliasing
		# PCSX2 uses -ffp-contract=fast; no -mfma is enabled so nothing can fuse, and the project rule is
		# -ffp-contract=off for everything that computes floating point (the VU interpreter does).
		/clang:-ffp-contract=off
	)
else()
	target_compile_definitions(don_vu_flags INTERFACE __POSIX__)
	target_compile_options(don_vu_flags INTERFACE
		-fno-strict-aliasing
		-fPIC
		-fvisibility=hidden
		-pthread
		# PCSX2 uses -ffp-contract=fast; no -mfma is enabled so nothing can fuse, and the project rule is
		# -ffp-contract=off for everything that computes floating point (the VU interpreter does).
		-ffp-contract=off
	)
endif()
target_include_directories(don_vu_flags SYSTEM INTERFACE
	"${HERE}/shim"
	"${P2SRC}"
	"${P2}"
	"${P3}/include"
	"${P3}/fmt/include"
	"${P3}/fast_float/include"
	"${P3}/xbyak"
)

# PCSX2's pcsx2/common slice and the x86 emitter.
add_library(don_vu_common OBJECT
	${P2}/common/AlignedMalloc.cpp
	${P2}/common/Assertions.cpp
	${P2}/common/Console.cpp
	${P2}/common/Error.cpp
	${P2}/common/FileSystem.cpp
	${P2}/common/Perf.cpp
	${P2}/common/StringUtil.cpp
	${P2}/common/SmallString.cpp
	${P2}/common/Timer.cpp
	${P2}/common/emitter/bmi.cpp
	${P2}/common/emitter/fpu.cpp
	${P2}/common/emitter/groups.cpp
	${P2}/common/emitter/jmp.cpp
	${P2}/common/emitter/legacy.cpp
	${P2}/common/emitter/legacy_sse.cpp
	${P2}/common/emitter/movs.cpp
	${P2}/common/emitter/simd.cpp
	${P2}/common/emitter/x86emitter.cpp
	${P3}/fmt/src/format.cc
)
target_link_libraries(don_vu_common PRIVATE don_vu_flags)

# PCSX2's VU code, unmodified. don_vu_prefix.h is force-included: it pulls in every PCSX2 header the
# sources use and then redirects gifUnit / EmuConfig / vu1Thread to the shims.
set(DON_VU_PCSX2_SRCS
	${P2SRC}/x86/microVU.cpp
	${P2SRC}/VUops.cpp
	${P2SRC}/VUflags.cpp
	${P2SRC}/VU1microInterp.cpp
	${P2SRC}/VU0microInterp.cpp
)
set(DON_VU_OWN_SRCS
	${HERE}/don_vu.cpp
	${HERE}/shim/don_vu_shims.cpp
	${HERE}/shim/don_vu_eerec_stubs.cpp
)
# The memory of the code cache: the one place that calls the operating system (AGENTS.md, Portability).
if(WIN32)
	list(APPEND DON_VU_OWN_SRCS ${HERE}/shim/don_vu_codemem_win32.cpp)
else()
	list(APPEND DON_VU_OWN_SRCS ${HERE}/shim/don_vu_codemem_posix.cpp)
endif()
add_library(don_vu_vu OBJECT ${DON_VU_PCSX2_SRCS} ${DON_VU_OWN_SRCS})
target_link_libraries(don_vu_vu PRIVATE don_vu_flags)
target_include_directories(don_vu_vu PRIVATE "${HERE}")
if(WIN32)
	# clang-cl takes the path after -include for a source file: the forced include is /FI.
	set(DON_VU_FORCE_INCLUDE "/FI\"${HERE}/shim/don_vu_prefix.h\"")
else()
	set(DON_VU_FORCE_INCLUDE "-include \"${HERE}/shim/don_vu_prefix.h\"")
endif()
set_source_files_properties(${DON_VU_PCSX2_SRCS} ${DON_VU_OWN_SRCS} PROPERTIES
	COMPILE_FLAGS "${DON_VU_FORCE_INCLUDE}")

add_library(don_vu SHARED $<TARGET_OBJECTS:don_vu_common> $<TARGET_OBJECTS:don_vu_vu>)
set_target_properties(don_vu PROPERTIES
	OUTPUT_NAME don_vu
	LINKER_LANGUAGE CXX
	LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
	BUILD_WITH_INSTALL_RPATH ON
	INSTALL_RPATH "\$ORIGIN"
)
target_include_directories(don_vu PUBLIC "${HERE}")
target_link_libraries(don_vu PRIVATE don_vu_flags)
# D11: only a C API, no dependency on the host's C++ runtime.
if(WIN32)
	# The exports are the DON_VU_API (dllexport) functions of don_vu.h. pathcch: PCSX2's common/FileSystem.cpp.
	# The C++ runtime is the MSVC one (the VC++ redistributable, or /MT: see docs/knowledge/17-windows-toolchain.md).
	target_link_libraries(don_vu PRIVATE pathcch)
else()
	target_link_options(don_vu PRIVATE -static-libstdc++ -static-libgcc
		"LINKER:--version-script=${HERE}/don_vu.map")
	set_property(TARGET don_vu APPEND PROPERTY LINK_DEPENDS "${HERE}/don_vu.map")
endif()

# vu_selftest: uses only the C API of the library (no PCSX2 include path on purpose).
add_executable(vu_selftest ${HERE}/vu_selftest.cpp)
target_include_directories(vu_selftest PRIVATE "${HERE}")
target_link_libraries(vu_selftest PRIVATE don_vu)
set_target_properties(vu_selftest PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
	BUILD_WITH_INSTALL_RPATH ON
	INSTALL_RPATH "\$ORIGIN"
)
if(NOT WIN32)
	target_link_options(vu_selftest PRIVATE -static-libstdc++ -static-libgcc)
endif()
