# Windows x86-64 from Linux: clang-cl + lld-link against the Microsoft CRT and Windows SDK that xwin
# fetched (decision D13, docs/knowledge/17-windows-toolchain.md). Used inside the image don-build-win:1
# through tools/container/run_windows.sh.
#
#   DON_XWIN_ROOT   folder written by `xwin splat` (contains crt/ and sdk/). Default:
#                   <repository>/third_party/xwin/sysroot. Environment variable or cache variable.
#
# Only the compiler and the system libraries are chosen here. The floating-point options of the game
# (AVX2, no contraction, no fast-math) are in the project files, next to the Linux ones.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

set(DON_XWIN_ROOT "$ENV{DON_XWIN_ROOT}" CACHE PATH "xwin splat output (contains crt/ and sdk/)")
if(NOT DON_XWIN_ROOT)
	set(DON_XWIN_ROOT "${CMAKE_CURRENT_LIST_DIR}/../third_party/xwin/sysroot")
endif()
get_filename_component(_xwin "${DON_XWIN_ROOT}" ABSOLUTE)

set(CMAKE_C_COMPILER clang-cl)
set(CMAKE_CXX_COMPILER clang-cl)
set(CMAKE_C_COMPILER_TARGET x86_64-pc-windows-msvc)
set(CMAKE_CXX_COMPILER_TARGET x86_64-pc-windows-msvc)
set(CMAKE_LINKER lld-link)
set(CMAKE_AR llvm-lib)
set(CMAKE_RC_COMPILER llvm-rc)
set(CMAKE_MT llvm-mt)
set(CMAKE_ASM_MASM_COMPILER llvm-ml)
set(CMAKE_ASM_MASM_FLAGS_INIT "--m64")

set(_inc "/imsvc ${_xwin}/crt/include /imsvc ${_xwin}/sdk/include/ucrt /imsvc ${_xwin}/sdk/include/um /imsvc ${_xwin}/sdk/include/shared /imsvc ${_xwin}/sdk/include/winrt")
# The image has clang 17 and xwin's MSVC STL (14.44) asks for clang >= 19 (yvals_core.h). It worked for
# everything built so far; remove the definition when the image gets clang >= 19.
set(_def "-D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH")
set(CMAKE_C_FLAGS_INIT "${_def} --target=x86_64-pc-windows-msvc -fuse-ld=lld-link ${_inc}")
set(CMAKE_CXX_FLAGS_INIT "${_def} --target=x86_64-pc-windows-msvc -fuse-ld=lld-link ${_inc}")
set(_lib "/libpath:${_xwin}/crt/lib/x86_64 /libpath:${_xwin}/sdk/lib/um/x86_64 /libpath:${_xwin}/sdk/lib/ucrt/x86_64")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_lib}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_lib}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_lib}")
# llvm-rc has no built-in include path.
set(CMAKE_RC_FLAGS_INIT "-I${_xwin}/crt/include -I${_xwin}/sdk/include/ucrt -I${_xwin}/sdk/include/um -I${_xwin}/sdk/include/shared")

set(CMAKE_TRY_COMPILE_CONFIGURATION Release)
set(CMAKE_FIND_ROOT_PATH "${_xwin}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
