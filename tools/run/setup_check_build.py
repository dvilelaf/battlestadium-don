"""The compile half of the fast check (T-704, T-713): `don` in the container, the PS2Recomp tool and the staged install.

Imported by tools/run/setup_check.py, which puts this folder on sys.path (scripts run with `python -I`).
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# T-713: the check runs `don` under AddressSanitizer + UBSan (CMakeLists.txt DON_SANITIZE), because a use after free like
# the one in FileBrowserModel::open_dir (T-701) reads freed memory without crashing in a plain binary.
# DON_CHECK_SANITIZE overrides the list ("" = a plain binary).
DEFAULT_SANITIZE = "address,undefined"
DEFAULT_BUILD_DIR = "build/check-asan"
# halt_on_error + abort_on_error=0: the first report ends the process with exit code 1 and its text reaches the log.
# detect_leaks=0: a leak at exit is not a bug of the path under test (the window is closed by the path's own QUIT).
SANITIZER_ENV = {
    "ASAN_OPTIONS": "halt_on_error=1:abort_on_error=0:detect_leaks=0:exitcode=1",
    "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1:exitcode=1",
}


# The image don-build:1 has clang 17.0.6 (apt.llvm.org) but not its sanitizer runtimes (docs/journal/015: ld.lld cannot open
# libclang_rt.asan-x86_64.a), and nothing may be installed in it. The same release publishes them as libclang-rt-17-dev:
# the check unpacks it into <build dir>-rt/ and points clang there with -resource-dir (headers linked to the image's).
RT_URL = ("https://apt.llvm.org/jammy/pool/main/l/llvm-toolchain-17/libclang-rt-17-dev_17.0.6~%2B%2B20231209124227%2B"
          "6009708b4367-1~exp1~20231209124336.77_amd64.deb")
RT_SHA256 = "77d2a3cb3636102766da6db59817fd36fc305be164c012cd5fc04a2d47161ada"
IMAGE_RESOURCE_DIR = "/usr/lib/llvm-17/lib/clang/17"


def sanitizer_resource_dir(out):
    """A clang resource dir with the sanitizer runtimes, fetched once (RT_URL, checked against RT_SHA256)."""
    import hashlib
    import tempfile
    import urllib.request
    rt = out.parent / (out.name + "-rt")
    if (rt / "lib/linux/libclang_rt.asan-x86_64.a").is_file():
        return rt
    out.parent.mkdir(parents=True, exist_ok=True)
    deb = urllib.request.urlopen(RT_URL, timeout=120).read()
    if hashlib.sha256(deb).hexdigest() != RT_SHA256:
        raise RuntimeError("libclang-rt-17-dev does not match RT_SHA256")
    pos = 8   # ar archive: "!<arch>\n", then members of a 60-byte header (name 16, ..., size at 48..58) and the data
    while pos < len(deb):
        name, size = deb[pos:pos + 16].decode().strip(), int(deb[pos + 48:pos + 58])
        if name.startswith("data.tar"):   # data.tar.zst: unpacked by the host's tar (the standard library has no zstd)
            with tempfile.TemporaryDirectory(dir=out.parent) as tmp:
                data = Path(tmp) / name
                data.write_bytes(deb[pos + 60:pos + 60 + size])
                subprocess.run(["tar", "-xf", str(data), "-C", tmp, "--wildcards", "*/lib/linux/*x86_64.[as]*"], check=True)
                (rt / "lib").mkdir(parents=True, exist_ok=True)
                shutil.copytree(next(Path(tmp).rglob("linux")), rt / "lib/linux", dirs_exist_ok=True)
        pos += 60 + size + size % 2
    (rt / "include").symlink_to(f"{IMAGE_RESOURCE_DIR}/include")
    return rt


def sanitize():
    return os.environ.get("DON_CHECK_SANITIZE", DEFAULT_SANITIZE)


def sanitizer_env():
    return dict(SANITIZER_ENV) if sanitize() else {}


def runtime_dir():
    """The PS2Recomp tree the release is made from: the pinned commit plus the committed patch series, in its own
    folder (tools/pipeline/package_linux.py prepare_runtime). The shared third_party tree is not used: it can carry
    unfinished patches, and the recompiler's output must match the reference hashes."""
    if os.environ.get("DON_RUNTIME_DIR"):
        return Path(os.environ["DON_RUNTIME_DIR"]).resolve()
    sys.path.insert(0, str(ROOT / "tools" / "pipeline"))
    import package_linux
    return package_linux.prepare_runtime(package_linux.main_checkout(), ROOT)[0]


def build_dir():
    return (ROOT / os.environ.get("DON_CHECK_BUILD_DIR", DEFAULT_BUILD_DIR)).resolve()


def build(log):
    """Incremental build of `don` without linked game code in the container (the flags of the linux-dev preset)."""
    jobs = os.environ.get("DON_JOBS", "6")
    out = build_dir()
    rel = os.path.relpath(out, ROOT)
    runtime = runtime_dir()
    flags = ("-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld "
             "-DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld -DFETCHCONTENT_FULLY_DISCONNECTED=ON -DDON_ENABLE_FFMPEG=ON "
             "-DCMAKE_BUILD_TYPE=Dev -DDON_GENERATED_OPT=1 -DDON_PCH=ON -DDON_LINK_GENERATED=OFF")
    if sanitize():
        rt = sanitizer_resource_dir(out)   # absolute: the repository is mounted at the same path in the container
        flags += f" -DDON_SANITIZE={sanitize()} -DCMAKE_C_FLAGS=-resource-dir={rt} -DCMAKE_CXX_FLAGS=-resource-dir={rt}"
    script = ('cmake -S . -B "$1" -G Ninja $2 "-DCMAKE_C_FLAGS_DEV=-O2 -DNDEBUG" "-DCMAKE_CXX_FLAGS_DEV=-O2 -DNDEBUG" '
              + f'"-DDON_RUNTIME_DIR={runtime}" '
              + '&& cmake --build "$1" -j "$3" --target don && mkdir -p "$1/libs" && cp -a /opt/deps/lib/*.so.* "$1/libs/"')
    env = dict(os.environ, DON_JOBS=jobs)
    r = subprocess.run(["bash", str(ROOT / "tools/container/run.sh"), "bash", "-c", script, "_", rel, flags, jobs],
                       cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
    return r.returncode == 0 and (out / "don").is_file()


def container(script, *args, log):
    env = dict(os.environ, DON_JOBS=os.environ.get("DON_JOBS", "6"))
    r = subprocess.run(["bash", str(ROOT / "tools/container/run.sh"), "bash", "-c", script, "_", *args], cwd=ROOT,
                       env=env, stdout=log, stderr=subprocess.STDOUT)
    return r.returncode == 0


def ensure_install(log):
    """The staged install `don` needs for the preparing screens (src/host/prepare_cli.h: bin/ps2_recomp, config/, sdk/,
    toolchain/) around the fresh build: <build dir>/install/. ps2_recomp is built (incrementally) from the PS2Recomp
    tree, the pinned clang is fetched once, and the bridge libraries and resources that the game needs (--full only) are
    taken from DON_CHECK_LIBS (default: build/player/don-linux of the main checkout, left by
    tools/pipeline/package_linux.py). Returns the path of the staged `don`, or None."""
    out = build_dir()
    runtime = runtime_dir()
    recomp_dir = out.parent / (out.name + "-recomp")
    rel = os.path.relpath(recomp_dir, ROOT)
    cache = recomp_dir / "CMakeCache.txt"
    if cache.is_file() and f"CMAKE_HOME_DIRECTORY:INTERNAL={runtime}\n" not in cache.read_text():
        shutil.rmtree(recomp_dir)   # made for another PS2Recomp tree
    if not (recomp_dir / "build.ninja").is_file() and not container(
            'cmake -S "$2" -B "$1" -G Ninja -DCMAKE_BUILD_TYPE=Release -DPS2X_BUILD_RUNTIME=OFF -DPS2X_BUILD_STUDIO=OFF '
            '-DPS2X_BUILD_TEST=OFF', rel, str(runtime), log=log):
        return None
    if not container('cmake --build "$1" -j "$2" --target ps2_recomp', rel, os.environ.get("DON_JOBS", "6"), log=log):
        return None
    recomp = next(recomp_dir.rglob("ps2_recomp"), None)
    toolchain = ROOT / "build" / "toolchain" / "linux"
    if not (toolchain / "bin").is_dir():
        subprocess.run([sys.executable, "-I", str(ROOT / "tools/pipeline/fetch_toolchain.py")], cwd=ROOT, stdout=log,
                       stderr=subprocess.STDOUT)
    if recomp is None or not (toolchain / "bin").is_dir():
        return None
    libs_src = Path(os.environ.get("DON_CHECK_LIBS", (ROOT / "third_party").resolve().parent / "build/player/don-linux"))
    stage_libs = out / "stage-libs"
    shutil.rmtree(stage_libs, ignore_errors=True)
    stage_libs.mkdir(parents=True)
    for f in (out / "libs").glob("*.so*"):
        shutil.copy2(f, stage_libs / f.name)
    if libs_src.is_dir():
        for f in libs_src.iterdir():
            if f.name != "don" and f.name != "BUILT_FROM.txt":
                if f.is_dir():
                    shutil.copytree(f, stage_libs / f.name, dirs_exist_ok=True)
                else:
                    shutil.copy2(f, stage_libs / f.name)
    install = out / "install"
    stamp = install / ".don-stamp"
    want = f"{(out / 'don').stat().st_mtime_ns} {recomp.stat().st_mtime_ns}"
    if stamp.is_file() and stamp.read_text() == want:
        return install / "don"
    r = subprocess.run([sys.executable, "-I", str(ROOT / "tools/module/stage_install.py"), "--out", str(install), "--don",
                        str(out / "don"), "--libs", str(stage_libs), "--recomp", str(recomp), "--toolchain",
                        str(toolchain)], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
    if r.returncode != 0 or not (install / "don").is_file():
        return None
    stamp.write_text(want)
    return install / "don"
