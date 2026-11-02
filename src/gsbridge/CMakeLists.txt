# Built from inside PCSX2's CMake project (patch 0001-bridge-cmake-hook): the targets PCSX2 and
# PCSX2_FLAGS only exist there. Configure with -DDON_GS_BRIDGE_SOURCE_DIR=<this directory>.
# PCSX2's own compiler flags apply here (D11); the game's floating point flags do not.
add_library(don_gs SHARED
	don_gs.cpp
	don_gs_host.cpp
)

set_target_properties(don_gs PROPERTIES
	OUTPUT_NAME don_gs
	CXX_VISIBILITY_PRESET hidden
	VISIBILITY_INLINES_HIDDEN YES
	LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
	BUILD_WITH_INSTALL_RPATH ON
	INSTALL_RPATH "\$ORIGIN"
)

target_include_directories(don_gs
	PUBLIC "${CMAKE_CURRENT_LIST_DIR}"
	PRIVATE
		"${CMAKE_BINARY_DIR}/common/include"
		"${CMAKE_SOURCE_DIR}"
		"${CMAKE_SOURCE_DIR}/pcsx2"
)

target_compile_definitions(don_gs PRIVATE DON_GS_BUILD=1)
target_link_libraries(don_gs PRIVATE PCSX2_FLAGS PCSX2)

# D11: the library exposes only a C API and does not depend on the host's C++ runtime.
if(NOT MSVC)
	target_link_options(don_gs PRIVATE -static-libstdc++ -static-libgcc)
endif()

if(UNIX AND NOT APPLE)
	# Export only the C API (hides the PCSX2 objects and static libraries).
	target_link_options(don_gs PRIVATE
		"LINKER:--version-script=${CMAKE_CURRENT_LIST_DIR}/don_gs.map")
	set_property(TARGET don_gs APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/don_gs.map")
endif()

# The GS reads shaders and fonts from <library directory>/resources.
add_custom_command(TARGET don_gs POST_BUILD
	COMMAND "${CMAKE_COMMAND}" -E copy_directory
		"${CMAKE_SOURCE_DIR}/bin/resources"
		"$<TARGET_FILE_DIR:don_gs>/resources"
	VERBATIM
)

# gsreplay: command line tool that uses only the C API (don_gs.h) of the library. It has no PCSX2
# include path on purpose.
add_executable(gsreplay gsreplay.cpp)
# Zstd::Zstd (found by PCSX2's own CMake) reads the .gs.zst dumps.
target_link_libraries(gsreplay PRIVATE don_gs Zstd::Zstd)
set_target_properties(gsreplay PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
	BUILD_WITH_INSTALL_RPATH ON
	INSTALL_RPATH "\$ORIGIN"
)
if(NOT MSVC)
	target_link_options(gsreplay PRIVATE -static-libstdc++ -static-libgcc)
endif()
