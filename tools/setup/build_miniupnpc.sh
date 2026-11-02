#!/usr/bin/env bash
# Builds miniupnpc (the UPnP IGD client, BSD-3-Clause), pinned and static, for Linux and Windows,
# into third_party/miniupnpc/{linux,windows}. T-515 (docs/knowledge/22-netplay.md section 7.1): the host
# opens its own UDP port on the router with it; only src/host/net/upnp.cpp includes it.
#
#   bash tools/setup/build_miniupnpc.sh            # Linux   -> third_party/miniupnpc/linux (build container)
#   bash tools/setup/build_miniupnpc.sh windows    # Windows -> third_party/miniupnpc/windows (clang-cl, win container)
#
# Both builds are static with the sample `upnpc` tool (for `upnpc -l`, listing the router's mappings)
# and no tests. The compilation runs inside the build container (same compiler as the game build), so it
# takes the project's lock and memory limit. Environment: DON_JOBS caps the parallelism (default 4).
set -euo pipefail

VERSION=2.2.8
SHA256=05b929679091b9921b6b6c1f25e39e4c8d1f4d46c8feb55a412aa697aee03a93   # first download, 2026-10-15
URL=https://miniupnp.tuxfamily.org/files/miniupnpc-$VERSION.tar.gz
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BASE=$ROOT/third_party/miniupnpc
MODE=${1:-linux}

case "$MODE" in
	linux)   PREFIX=$BASE/linux ;;
	windows) PREFIX=$BASE/windows ;;
	*) echo "usage: $0 [linux|windows]" >&2; exit 2 ;;
esac

# ---- inside the build container: configure, build, install --------------------------------------
if [[ -e /.dockerenv ]]; then
	JOBS=${DON_JOBS:-4}
	if [[ $MODE == windows ]]; then
		J=$JOBS; (( J > 6 )) && J=6
		cmake -S "$BASE/src-win" -B "$BASE/obj-win" -G Ninja -DCMAKE_BUILD_TYPE=Release \
			-DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/toolchain-windows-clangcl.cmake" \
			-DCMAKE_INSTALL_PREFIX="$PREFIX" \
			-DUPNPC_BUILD_STATIC=ON -DUPNPC_BUILD_SHARED=OFF -DUPNPC_BUILD_TESTS=OFF -DUPNPC_BUILD_SAMPLE=ON
		cmake --build "$BASE/obj-win" --parallel "$J"
		cmake --install "$BASE/obj-win"
	else
		cmake -S "$BASE/src" -B "$BASE/obj" -G Ninja -DCMAKE_BUILD_TYPE=Release \
			-DCMAKE_C_COMPILER=clang -DCMAKE_INSTALL_PREFIX="$PREFIX" \
			-DUPNPC_BUILD_STATIC=ON -DUPNPC_BUILD_SHARED=OFF -DUPNPC_BUILD_TESTS=OFF -DUPNPC_BUILD_SAMPLE=ON
		cmake --build "$BASE/obj" --parallel "$JOBS"
		cmake --install "$BASE/obj"
	fi
	echo "$VERSION" > "$PREFIX/VERSION"
	du -sh "$PREFIX"
	exit 0
fi

# ---- on the host: download, check and unpack, then start the right container --------------------
if [[ -f "$PREFIX/VERSION" && "$(cat "$PREFIX/VERSION")" == "$VERSION" ]] && \
   { [[ -f "$PREFIX/lib/libminiupnpc.a" ]] || [[ -f "$PREFIX/lib/libminiupnpc.lib" ]]; }; then
	echo "miniupnpc $VERSION already built in $PREFIX"
	exit 0
fi

mkdir -p "$BASE"
if [[ ! -f "$BASE/miniupnpc-$VERSION.tar.gz" ]]; then
	curl -fsSL --retry 5 -o "$BASE/miniupnpc-$VERSION.tar.gz" "$URL"
fi
echo "$SHA256  $BASE/miniupnpc-$VERSION.tar.gz" | sha256sum --check --strict -

export DON_JOBS=${DON_JOBS:-4}
if [[ $MODE == windows ]]; then
	rm -rf "$BASE/src-win" "$BASE/obj-win" "$PREFIX"
	mkdir -p "$BASE/src-win"
	tar -xf "$BASE/miniupnpc-$VERSION.tar.gz" -C "$BASE/src-win" --strip-components=1
	exec bash "${DON_RUN_WINDOWS:-$ROOT/tools/container/run_windows.sh}" bash "$ROOT/tools/setup/build_miniupnpc.sh" windows
fi

rm -rf "$BASE/src" "$BASE/obj" "$PREFIX"
mkdir -p "$BASE/src" "$BASE/obj"
tar -xf "$BASE/miniupnpc-$VERSION.tar.gz" -C "$BASE/src" --strip-components=1
bash "$ROOT/tools/container/run.sh" bash "$ROOT/tools/setup/build_miniupnpc.sh" linux
