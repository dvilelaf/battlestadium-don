#!/usr/bin/env bash
# Builds one PCSX2 dependency from source into /opt/deps. Runs inside the image build only.
#
# Usage: build-deps.sh <name>
#
# Versions and SHA-256 hashes are those pinned by PCSX2's own CI script
# (third_party/pcsx2/.github/workflows/scripts/linux/build-dependencies-qt.sh, PCSX2 v2.8.2),
# and the build options are the ones that script uses, with one deviation: SDL3 is built with
# video enabled, because the host program (D9) opens its window with it.
# Patches come from the PCSX2 tree (PATCH_DIR, filled by build_image.sh), not from this repository.
set -euo pipefail

INSTALLDIR=/opt/deps
PATCH_DIR=${PATCH_DIR:-/ctx/patches}
WORK=$(mktemp -d /tmp/deps-build.XXXXXX)
trap 'rm -rf "$WORK"' EXIT
cd "$WORK"

LIBBACKTRACE=ad106d5fdd5d960bd33fae1c48a351af567fd075
LIBJPEGTURBO=3.2.0
LIBPNG=1.6.58
LIBWEBP=1.6.0
SDL=SDL3-3.4.12
LZ4=1.10.0
ZSTD=1.5.7
PLUTOVG=1.3.2
PLUTOSVG=0.0.7
RAPIDYAML=0.12.1
SHADERC=2026.2
SHADERC_GLSLANG=275822a6261ee689aadb1da5f09a0ec2f058685c
SHADERC_SPIRVHEADERS=58006c901d1d5c37dece6b6610e9af87fa951375
SHADERC_SPIRVTOOLS=6337eb62cadd7d124ac6789bf39c0f71148f0a73

CMAKE_COMMON=(-G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=$INSTALLDIR" "-DCMAKE_INSTALL_PREFIX=$INSTALLDIR")

# fetch <sha256> <file> <url>
fetch() {
	curl -fsSL --retry 5 --retry-delay 5 -o "$2" "$3"
	echo "$1  $2" | sha256sum --check --strict -
}

record() { echo "$1" >> "$INSTALLDIR/VERSIONS.txt"; }

# keep_license <project> <file>[:<name>]... : copies the licence files the project ships (paths relative to the
# current source tree) into $INSTALLDIR/share/licenses/<project>/, where tools/pipeline/package_linux.py looks for
# the texts of the libraries it ships. A missing file stops the image build.
keep_license() {
	local project=$1 spec src name
	shift
	mkdir -p "$INSTALLDIR/share/licenses/$project"
	for spec in "$@"; do
		src=${spec%%:*}
		name=${spec#*:}
		[ "$name" = "$spec" ] && name=$(basename "$src")
		cp -L "$src" "$INSTALLDIR/share/licenses/$project/$name"
	done
}

build_cmake() { # <source dir> <build dir> [cmake args...]
	local src=$1 bld=$2
	shift 2
	cmake "${CMAKE_COMMON[@]}" -B "$bld" -S "$src" "$@"
	cmake --build "$bld" --parallel "${DON_JOBS:-$(nproc)}"
	cmake --install "$bld"
}

case "${1:-}" in
libbacktrace)
	fetch 96e5c2d7f2c482a60d5804da48a2eb9a0db0719b2c65dcc169fbfdcf37f3a45d lb.tar.gz \
		"https://github.com/ianlancetaylor/libbacktrace/archive/$LIBBACKTRACE/libbacktrace-$LIBBACKTRACE.tar.gz"
	tar xf lb.tar.gz
	cd "libbacktrace-$LIBBACKTRACE"
	./configure --prefix="$INSTALLDIR" --with-pic
	make -j"${DON_JOBS:-$(nproc)}"
	make install
	keep_license libbacktrace LICENSE
	record "libbacktrace $LIBBACKTRACE"
	;;
libpng)
	fetch 28eb403f51f0f7405249132cecfe82ea5c0ef97f1b32c5a65828814ae0d34775 png.tar.xz \
		"https://downloads.sourceforge.net/project/libpng/libpng16/$LIBPNG/libpng-$LIBPNG.tar.xz"
	fetch eee7dea22ed502868017971c86c63c4ed1e6085de0baebfdcc3d3322f00f3eb0 apng.patch.gz \
		"https://download.sourceforge.net/libpng-apng/libpng-$LIBPNG-apng.patch.gz"
	tar xf png.tar.xz
	gzip -dk apng.patch.gz
	cd "libpng-$LIBPNG"
	patch -p1 < ../apng.patch
	build_cmake . build -DBUILD_SHARED_LIBS=ON -DPNG_TESTS=OFF -DPNG_STATIC=OFF -DPNG_SHARED=ON -DPNG_TOOLS=OFF
	keep_license libpng LICENSE
	record "libpng $LIBPNG (+APNG patch)"
	;;
libjpeg-turbo)
	fetch 6f30092cef9fb839779646608f4ee14ae3cbac989c47fa05e841b0841f09878e jpeg.tar.gz \
		"https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/$LIBJPEGTURBO/libjpeg-turbo-$LIBJPEGTURBO.tar.gz"
	tar xf jpeg.tar.gz
	cd "libjpeg-turbo-$LIBJPEGTURBO"
	build_cmake . build -DENABLE_STATIC=OFF -DENABLE_SHARED=ON -DCMAKE_INSTALL_DEFAULT_LIBDIR=lib -DCMAKE_POSITION_INDEPENDENT_CODE=1
	keep_license libjpeg-turbo LICENSE.md README.ijg
	record "libjpeg-turbo $LIBJPEGTURBO"
	;;
lz4)
	fetch 537512904744b35e232912055ccf8ec66d768639ff3abe5788d90d792ec5f48b lz4.tar.gz \
		"https://github.com/lz4/lz4/releases/download/v$LZ4/lz4-$LZ4.tar.gz"
	tar xf lz4.tar.gz
	cd "lz4-$LZ4"
	build_cmake build/cmake build-dir -DBUILD_SHARED_LIBS=ON -DLZ4_BUILD_CLI=OFF -DLZ4_BUILD_LEGACY_LZ4C=OFF
	keep_license lz4 lib/LICENSE:LICENSE
	record "lz4 $LZ4"
	;;
zstd)
	fetch eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3 zstd.tar.gz \
		"https://github.com/facebook/zstd/releases/download/v$ZSTD/zstd-$ZSTD.tar.gz"
	tar xf zstd.tar.gz
	cd "zstd-$ZSTD"
	build_cmake build/cmake build -DBUILD_SHARED_LIBS=ON -DZSTD_BUILD_SHARED=ON -DZSTD_BUILD_STATIC=OFF -DZSTD_BUILD_PROGRAMS=OFF
	keep_license zstd LICENSE COPYING
	record "zstd $ZSTD"
	;;
libwebp)
	fetch e4ab7009bf0629fd11982d4c2aa83964cf244cffba7347ecd39019a9e38c4564 webp.tar.gz \
		"https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-$LIBWEBP.tar.gz"
	tar xf webp.tar.gz
	cd "libwebp-$LIBWEBP"
	build_cmake . build -DWEBP_BUILD_ANIM_UTILS=OFF -DWEBP_BUILD_CWEBP=OFF -DWEBP_BUILD_DWEBP=OFF \
		-DWEBP_BUILD_GIF2WEBP=OFF -DWEBP_BUILD_IMG2WEBP=OFF -DWEBP_BUILD_VWEBP=OFF -DWEBP_BUILD_WEBPINFO=OFF \
		-DWEBP_BUILD_WEBPMUX=OFF -DWEBP_BUILD_EXTRAS=OFF -DBUILD_SHARED_LIBS=ON
	keep_license libwebp COPYING PATENTS
	record "libwebp $LIBWEBP"
	;;
sdl3)
	fetch f07b958a9ac5020fb7a44cadb957f658b2149c3c8abb4f63145fac9303249db7 sdl.tar.gz \
		"https://libsdl.org/release/$SDL.tar.gz"
	tar xf sdl.tar.gz
	cd "$SDL"
	# Unlike PCSX2's CI, video stays enabled (the host program needs windows).
	build_cmake . build -DBUILD_SHARED_LIBS=ON -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF \
		-DSDL_DIALOG=OFF -DSDL_TRAY=OFF
	keep_license SDL3 LICENSE.txt
	record "SDL3 ${SDL#SDL3-}"
	;;
plutovg)
	fetch 7bd4e79ce18b1d47517e7e91fbb7cf19d4f01942804a519bc7c0bf32b6325dd5 plutovg.tar.gz \
		"https://github.com/sammycage/plutovg/archive/v$PLUTOVG/plutovg-$PLUTOVG.tar.gz"
	tar xf plutovg.tar.gz
	cd "plutovg-$PLUTOVG"
	build_cmake . build -DBUILD_SHARED_LIBS=ON -DPLUTOVG_BUILD_EXAMPLES=OFF
	keep_license plutovg LICENSE
	record "plutovg $PLUTOVG"
	;;
plutosvg)
	fetch 78561b571ac224030cdc450ca2986b4de915c2ba7616004a6d71a379bffd15f3 plutosvg.tar.gz \
		"https://github.com/sammycage/plutosvg/archive/v$PLUTOSVG/plutosvg-$PLUTOSVG.tar.gz"
	tar xf plutosvg.tar.gz
	cd "plutosvg-$PLUTOSVG"
	build_cmake . build -DBUILD_SHARED_LIBS=ON -DPLUTOSVG_ENABLE_FREETYPE=OFF -DPLUTOSVG_BUILD_EXAMPLES=OFF
	keep_license plutosvg LICENSE
	record "plutosvg $PLUTOSVG"
	;;
rapidyaml)
	fetch e9efcdd17f86287748793cf21d106e461fcad8d103a3e5a23632afe93828660d ryml.tgz \
		"https://github.com/biojppm/rapidyaml/releases/download/v$RAPIDYAML/rapidyaml-$RAPIDYAML-src.tgz"
	tar xf ryml.tgz
	cd "rapidyaml-$RAPIDYAML-src"
	build_cmake . build -DBUILD_SHARED_LIBS=ON
	keep_license rapidyaml LICENSE.txt
	record "rapidyaml $RAPIDYAML"
	;;
shaderc)
	fetch f924178e75e3293082481b25ed64d5e48a795b479dac3bd3c83d23070855df42 shaderc.tar.gz \
		"https://github.com/google/shaderc/archive/v$SHADERC/shaderc-$SHADERC.tar.gz"
	fetch 971848a1cc639ce8dc244e778b17efe0f690e32ac398a75e31d1c67ad06d3e0a glslang.tar.gz \
		"https://github.com/KhronosGroup/glslang/archive/$SHADERC_GLSLANG/shaderc-glslang-$SHADERC_GLSLANG.tar.gz"
	fetch a6cb1b300bb8171795e116457e858e555334749f9cacaed8068ae0ef8681110c spirv-headers.tar.gz \
		"https://github.com/KhronosGroup/SPIRV-Headers/archive/$SHADERC_SPIRVHEADERS/shaderc-spirv-headers-$SHADERC_SPIRVHEADERS.tar.gz"
	fetch e156be0bd81c8812f1bff8e520422bfa9df61b3045587b9eb483185f1074a7b2 spirv-tools.tar.gz \
		"https://github.com/KhronosGroup/SPIRV-Tools/archive/$SHADERC_SPIRVTOOLS/shaderc-spirv-tools-$SHADERC_SPIRVTOOLS.tar.gz"
	tar xf shaderc.tar.gz
	cd "shaderc-$SHADERC/third_party"
	tar xf ../../glslang.tar.gz && mv "glslang-$SHADERC_GLSLANG" glslang
	tar xf ../../spirv-headers.tar.gz && mv "SPIRV-Headers-$SHADERC_SPIRVHEADERS" spirv-headers
	tar xf ../../spirv-tools.tar.gz && mv "SPIRV-Tools-$SHADERC_SPIRVTOOLS" spirv-tools
	cd ..
	patch -p1 < "$PATCH_DIR/shaderc-changes.patch"
	build_cmake . build -DSHADERC_SKIP_TESTS=ON -DSHADERC_SKIP_EXAMPLES=ON -DSHADERC_SKIP_COPYRIGHT_CHECK=ON
	keep_license shaderc LICENSE:LICENSE third_party/glslang/LICENSE.txt:LICENSE.glslang.txt \
		third_party/spirv-tools/LICENSE:LICENSE.spirv-tools third_party/spirv-headers/LICENSE:LICENSE.spirv-headers
	record "shaderc $SHADERC (glslang ${SHADERC_GLSLANG:0:7}, SPIRV-Headers ${SHADERC_SPIRVHEADERS:0:7}, SPIRV-Tools ${SHADERC_SPIRVTOOLS:0:7}, PCSX2 patch)"
	;;
*)
	echo "usage: $0 libbacktrace|libpng|libjpeg-turbo|lz4|zstd|libwebp|sdl3|plutovg|plutosvg|rapidyaml|shaderc" >&2
	exit 2
	;;
esac
