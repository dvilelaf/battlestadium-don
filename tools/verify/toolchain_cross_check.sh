#!/bin/sh
# T-619: compile and link a freestanding stub with the pinned clang on the host, in debian:12 and in fedora:40,
# and print the sha256 of every .o and .so. Identical lines across the labels mean the toolchain is reproducible.
# Usage: toolchain_cross_check.sh [toolchain_root (default build/toolchain/linux)] [out_dir (default work/t619/out)]
# Network: only `docker pull` of the two images. Nothing is installed on the host or in the containers.
set -eu

TC=$(cd "${1:-build/toolchain/linux}" && pwd)
mkdir -p "${2:-work/t619/out}"
OUT=$(cd "${2:-work/t619/out}" && pwd)
CLANG="$TC/bin/clang++"
RES="$TC/lib/clang/17"   # resource dir of LLVM 17.0.6 (A1: -isystem <clang resource dir>/include)

mkdir -p "$OUT/src" "$OUT/hostlibs"
# The 161-byte stub of work/t603/mod.cpp (exports don_mod, no game code).
cat > "$OUT/src/mod.cpp" <<'SRC'
typedef void (*cb_t)(void*);
extern "C" __attribute__((visibility("default"))) int don_mod(cb_t cb, void* h, int* flag){ cb(h); if(*flag) return -1; return 7; }
SRC

# ld.lld needs libxml2.so.2, which debian:12 does not ship. Without network, take the host's copy and its ICU
# dependencies into a folder that is only put on LD_LIBRARY_PATH inside the debian:12 container. It is used by the linker
# program only; it does not enter the output bytes.
XML=$(ldd "$TC/bin/ld.lld" | sed -n 's/^[[:space:]]*libxml2.so.2 => \([^ ]*\) .*/\1/p')
for l in $(ldd "$XML" | sed -n 's/^[[:space:]]*\(libicu[a-z0-9.]*\) => \([^ ]*\) .*/\2/p') "$XML"; do
	cp -L "$l" "$OUT/hostlibs/"
done

# The same commands run in every environment. Paths inside the containers are identical to the host's.
BUILD='
set -eu
cd "$1/src"
D="$1/$2"; mkdir -p "$D"
# FLAGS: A1 of docs/tasks/M6-public-release.md:76-131 plus the ones noted below.
# -std=gnu++20: work/t603/run.sh.   -O1 -fPIC -funwind-tables: A1 / work/t603/run.sh.   No -g: A1.
# -nostdinc -isystem: A1 (the compiler headers are removed by -nostdinc, so give back the resource dir).
# -ffreestanding -fno-rtti -fno-threadsafe-statics -fvisibility=hidden -fno-stack-protector -ffp-contract=off
# -march=x86-64-v3: A1 and work/t603/run.sh.   -fno-crash-diagnostics: A1.
# -ffile-prefix-map=$PWD=.: A1; keeps the build folder out of the object.
"$3" -std=gnu++20 -O1 -fPIC -funwind-tables -nostdinc -isystem "$4/include" -ffreestanding -fno-rtti \
	-fno-threadsafe-statics -fvisibility=hidden -fno-stack-protector -ffp-contract=off -march=x86-64-v3 \
	-fno-crash-diagnostics -ffile-prefix-map="$PWD"=. -c mod.cpp -o "$D/mod.o"
# Link: -shared -nostdlib (the module is freestanding, no imports: A1 and T-604) with lld (this script).
"$3" -shared -fPIC -nostdlib -fuse-ld=lld "$D/mod.o" -o "$D/mod-nostdlib.so"
'

echo "== host"
LD_LIBRARY_PATH="" sh -c "$BUILD" sh "$OUT" host "$CLANG" "$RES"

for pair in debian12:debian:12 fedora40:fedora:40; do
	label=${pair%%:*}; image=${pair#*:}
	echo "== $image"
	docker pull -q "$image" >/dev/null
	# The host's libxml2 is given only to debian:12 (the one place ld.lld misses it); fedora:40 gets nothing.
	envarg=""
	[ "$label" = debian12 ] && envarg="-e LD_LIBRARY_PATH=$OUT/hostlibs"
	# Toolchain read-only at the same path; output folder writable; run as the invoking user.
	# shellcheck disable=SC2086
	docker run --rm --user "$(id -u):$(id -g)" -v "$TC:$TC:ro" -v "$OUT:$OUT" $envarg "$image" \
		sh -c "$BUILD" sh "$OUT" "$label" "$CLANG" "$RES"
done

echo "== sha256"
cd "$OUT"
for f in host debian12 fedora40; do
	for o in mod.o mod-nostdlib.so; do
		sha256sum "$f/$o" | sed "s|\$| ($f)|"
	done
done
