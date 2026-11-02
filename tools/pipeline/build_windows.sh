#!/usr/bin/env bash
# Cross-builds the Windows pieces of the port inside the image don-build-win:1 (decision D13: clang-cl,
# lld-link, the Microsoft CRT and SDK from xwin). Run it from the host; it starts the container itself
# through tools/container/run_windows.sh (memory limit and the shared lock).
#
#   bash tools/pipeline/build_windows.sh [--deps DIR] [--pcsx2 DIR] [step ...]
#
# steps (default: runtime spu2 vu gs):
#   runtime   configure the preset windows-release and build the PS2Recomp runtime and the host library
#             (ps2_iop ps2_runtime don_host and the object library don_host_main) into build/windows-release
#   spu2      the SPU2 bridge DLL, build/windows-spu2
#   vu        the VU bridge DLL, build/windows-vu
#   gs        the GS bridge DLL (PCSX2's GS), build/windows-gs
#   module-exe  don.exe with DON_LINK_GENERATED=OFF (game code in a module), build/windows-module
#   setup-test tests/setup cross-built (don_setup and every test), build/windows-setuptest
#   module-test tests/host cross-built: test_game_module_win32.exe and its fixture DLLs, build/windows-hosttest
#   game      EVERYTHING, the generated code's library and the link of don.exe. Not in the default list:
#             it takes hours and needs the generated/ sources (next piece of T-401).
#
#   --deps DIR    prefix of the Windows dependencies (default third_party/win-deps/clangcl, built by
#                 tools/setup/build_win_deps.sh); relative to the repository root
#   --pcsx2 DIR   PCSX2 tree with patches/pcsx2 applied (default third_party/pcsx2)
# Environment: DON_SLICE_SECONDS and DON_SKIP_CONFIGURE (step game, see below); DON_JOBS compile jobs (default 4, at most 6); DON_RUN_WINDOWS path of run_windows.sh when
#   this script is in a git worktree (the worktree's copy would take another lock and mount less).
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)

if [[ ! -e /.dockerenv ]]; then
	RUNNER=${DON_RUN_WINDOWS:-$ROOT/tools/container/run_windows.sh}
	exec bash "$RUNNER" sh -c "cd $(printf '%q' "$ROOT") && bash tools/pipeline/build_windows.sh $(printf '%q ' "$@")"
fi

cd "$ROOT"
DEPS=$ROOT/third_party/win-deps/clangcl
PCSX2=$ROOT/third_party/pcsx2
while [[ ${1:-} == --* ]]; do
	case $1 in
		--deps) DEPS=$(realpath -m "$2"); shift 2 ;;
		--pcsx2) PCSX2=$(realpath -m "$2"); shift 2 ;;
		*) echo "unknown option $1" >&2; exit 2 ;;
	esac
done
STEPS=("$@"); [[ ${#STEPS[@]} -eq 0 ]] && STEPS=(runtime spu2 vu gs)
J=${DON_JOBS:-4}; (( J > 6 )) && J=6
TC=$ROOT/cmake/toolchain-windows-clangcl.cmake

for step in "${STEPS[@]}"; do
	start=$(date +%s)
	case $step in
		runtime|game)
			# DON_SKIP_CONFIGURE=1: resume a build without configuring again (the configure takes minutes).
			[[ ${DON_SKIP_CONFIGURE:-0} == 1 && -f build/windows-release/build.ninja ]] \
				|| cmake --preset windows-release -DCMAKE_PREFIX_PATH="$DEPS"
			if [[ $step == game ]]; then
				# DON_SLICE_SECONDS=N: stop the build after N seconds (exit 124) so the container lock is released;
				# run it again and the build tool resumes where it stopped (see docs/BUILDING.md).
				${DON_SLICE_SECONDS:+timeout -k 20 "$DON_SLICE_SECONDS"} cmake --build build/windows-release --parallel "$J"
			else
				cmake --build build/windows-release --parallel "$J" --target ps2_iop ps2_runtime don_host don_host_main
			fi ;;
		module-exe)
			# The M6 model: don.exe without the generated code (DON_LINK_GENERATED=OFF); the game code arrives as
			# don_game.dll, loaded by src/host/game_module_win32.cpp. Own build folder, so `runtime`/`game` stay as they are.
			cmake -S . -B build/windows-module -G Ninja -DCMAKE_TOOLCHAIN_FILE="$TC" -DCMAKE_BUILD_TYPE=Release \
				-DFETCHCONTENT_FULLY_DISCONNECTED=ON -DCMAKE_PREFIX_PATH="$DEPS" -DDON_LINK_GENERATED=OFF
			cmake --build build/windows-module --parallel "$J" --target don ;;
		module-test)
			# T-731: tests/host cross-built for Windows; run the result with tools/container/run_wine.sh
			# (build/windows-hosttest/test_game_module_win32.exe).
			cmake -S tests/host -B build/windows-hosttest -G Ninja -DCMAKE_TOOLCHAIN_FILE="$TC" -DCMAKE_BUILD_TYPE=Release \
				-DCMAKE_PREFIX_PATH="$DEPS"
			cmake --build build/windows-hosttest --parallel "$J" --target test_game_module_win32 test_crash_trace_win32 ;;
		setup-test)
			# T-741: the setup library and every tests/setup program cross-built for Windows; run them under Wine
			# (tools/container/run_wine.sh, build/windows-setuptest/*.exe, z.dll beside them).
			cmake -S tests/setup -B build/windows-setuptest -G Ninja -DCMAKE_TOOLCHAIN_FILE="$TC" -DCMAKE_BUILD_TYPE=Release \
				-DCMAKE_PREFIX_PATH="$DEPS"
			cmake --build build/windows-setuptest --parallel "$J"
			cp -u "$DEPS"/bin/z.dll build/windows-setuptest/ ;;
		spu2|vu)
			cmake -S "src/${step}bridge" -B "build/windows-$step" -G Ninja -DCMAKE_BUILD_TYPE=Release \
				-DCMAKE_TOOLCHAIN_FILE="$TC" -DCMAKE_PREFIX_PATH="$DEPS" -DDON_PCSX2_SOURCE_DIR="$PCSX2" 2>&1 | tail -3
			cmake --build "build/windows-$step" --parallel "$J" ;;
		gs)
			if ! grep -q DON_GS_BRIDGE_SOURCE_DIR "$PCSX2/CMakeLists.txt"; then
				echo "error: patches/pcsx2 are not applied to $PCSX2 (bash tools/setup/fetch_pcsx2.sh)" >&2; exit 1
			fi
			cmake -S "$PCSX2" -B build/windows-gs -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="$TC" \
				-DCMAKE_PREFIX_PATH="$DEPS" -DENABLE_QT_UI=OFF -DENABLE_TESTS=OFF -DUSE_VULKAN=ON -DUSE_OPENGL=OFF \
				-DDISABLE_ADVANCE_SIMD=ON "-DDON_GS_BRIDGE_SOURCE_DIR=$ROOT/src/gsbridge" 2>&1 | tail -3
			cmake --build build/windows-gs --parallel "$J" --target don_gs ;;
		*) echo "unknown step $step" >&2; exit 2 ;;
	esac
	echo "[build_windows] $step done in $(( $(date +%s) - start )) s"
done
