# Build image for the PCSX2 pieces (D11) and, later, the rest of the project.
# Tag: don-build:1. Built by tools/container/build_image.sh, which prepares the build context
# (this directory plus the PCSX2 dependency patches taken from third_party/pcsx2).
# Contains no game files and nothing from the repository besides what is COPYed below.
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

# --- Base tools and development packages ("apt" rows of docs/knowledge/12-building-pcsx2.md) ---
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl git python3 pkg-config patch make ninja-build nasm \
        unzip xz-utils binutils \
        libstdc++-12-dev \
        zlib1g-dev libfreetype-dev libfontconfig-dev libcurl4-openssl-dev libpcap-dev \
        libdbus-1-dev libudev-dev \
        libx11-dev libxext-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev \
        libxfixes-dev libxss-dev libxkbcommon-dev \
        libwayland-dev libwayland-bin wayland-protocols extra-cmake-modules libegl-dev \
        libgl-dev libasound2-dev \
    && rm -rf /var/lib/apt/lists/*

# --- Compiler: clang-17 and lld-17 from apt.llvm.org (jammy), as PCSX2's own CI ---
# The repository key is pinned by hash (apt.llvm.org signs all releases with this one key).
RUN curl -fsSL -o /usr/share/keyrings/llvm-snapshot.asc https://apt.llvm.org/llvm-snapshot.gpg.key \
    && echo "8b2a587ffd672c4687e7581dad4b2f6c1bb2ad6b480cd9771ba2ff48e0b8c75d  /usr/share/keyrings/llvm-snapshot.asc" | sha256sum --check --strict - \
    && echo "deb [signed-by=/usr/share/keyrings/llvm-snapshot.asc] https://apt.llvm.org/jammy/ llvm-toolchain-jammy-17 main" \
        > /etc/apt/sources.list.d/llvm-17.list \
    && apt-get update && apt-get install -y --no-install-recommends clang-17 lld-17 \
    && rm -rf /var/lib/apt/lists/* \
    && ln -s /usr/bin/clang-17   /usr/local/bin/clang \
    && ln -s /usr/bin/clang++-17 /usr/local/bin/clang++ \
    && ln -s /usr/bin/clang-17   /usr/local/bin/cc \
    && ln -s /usr/bin/clang++-17 /usr/local/bin/c++ \
    && ln -s /usr/bin/ld.lld-17  /usr/local/bin/ld.lld
ENV CC=clang CXX=clang++

# --- CMake from Kitware's binary release (Ubuntu 22.04 ships 3.22; WHOLE_ARCHIVE needs >= 3.24) ---
# /usr/local/bin comes before /usr/bin, so this one wins over the cmake the apt packages pull in.
RUN curl -fsSL -o /tmp/cmake.tar.gz https://github.com/Kitware/CMake/releases/download/v3.31.9/cmake-3.31.9-linux-x86_64.tar.gz \
    && echo "312d78e0b7c5c9b4a97a87a46c3e525aa84895fe8135c238c4616bba73dc9518  /tmp/cmake.tar.gz" | sha256sum --check --strict - \
    && mkdir /opt/cmake && tar -xf /tmp/cmake.tar.gz -C /opt/cmake --strip-components=1 \
    && ln -s /opt/cmake/bin/cmake /opt/cmake/bin/ctest /opt/cmake/bin/cpack /usr/local/bin/ \
    && rm /tmp/cmake.tar.gz

# --- Dependencies built from source into /opt/deps (one layer each, so a late failure keeps the early ones) ---
ENV CMAKE_PREFIX_PATH=/opt/deps \
    PKG_CONFIG_PATH=/opt/deps/lib/pkgconfig:/opt/deps/share/pkgconfig \
    PATH=/opt/deps/bin:$PATH
# Parallel jobs for the dependency builds (build_image.sh ... --build-arg DON_JOBS=4); default: every core.
ARG DON_JOBS
RUN mkdir -p /opt/deps && echo /opt/deps/lib > /etc/ld.so.conf.d/opt-deps.conf

COPY build-deps.sh /ctx/build-deps.sh
COPY patches/ /ctx/patches/

RUN bash /ctx/build-deps.sh libbacktrace && ldconfig
RUN bash /ctx/build-deps.sh libpng && ldconfig
RUN bash /ctx/build-deps.sh libjpeg-turbo && ldconfig
RUN bash /ctx/build-deps.sh lz4 && ldconfig
RUN bash /ctx/build-deps.sh zstd && ldconfig
RUN bash /ctx/build-deps.sh libwebp && ldconfig
RUN bash /ctx/build-deps.sh plutovg && ldconfig
RUN bash /ctx/build-deps.sh plutosvg && ldconfig
RUN bash /ctx/build-deps.sh rapidyaml && ldconfig
RUN apt-get update && apt-get install -y --no-install-recommends \
        libxtst-dev libxrender-dev libdrm-dev libgbm-dev libxcb1-dev libpulse-dev \
    && rm -rf /var/lib/apt/lists/*
RUN bash /ctx/build-deps.sh sdl3 && ldconfig
RUN bash /ctx/build-deps.sh shaderc && ldconfig

# Vulkan loader and Mesa's software driver (lavapipe), so headless GS checks can run in the
# container; patchelf to fix the run path of libraries copied out of /opt/deps.
# Kept after the dependency layers so adding packages never rebuilds /opt/deps.
# ccache: compiler cache, used when run.sh gets CCACHE_DIR (T-706). Only the cache's use changes; the compilers stay.
RUN apt-get update && apt-get install -y --no-install-recommends \
        libvulkan1 mesa-vulkan-drivers vulkan-tools patchelf ccache \
    && rm -rf /var/lib/apt/lists/*

# Writable HOME for whatever user run.sh maps in (uid/gid are not in /etc/passwd).
ENV HOME=/tmp/home
RUN mkdir -p /tmp/home && chmod 1777 /tmp/home

WORKDIR /work
