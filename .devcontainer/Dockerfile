# syntax=docker/dockerfile:1.5

FROM mcr.microsoft.com/devcontainers/base:ubuntu-22.04

ARG LLHTTP_VERSION=v6.0.7

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    ninja-build \
    pkg-config \
    git \
    gdb \
    clang \
    clang-format \
    clang-tidy \
    lldb \
    python3 \
    libssl-dev \
    libyaml-cpp-dev \
    libsqlite3-dev \
    libhiredis-dev \
    ca-certificates \
  && rm -rf /var/lib/apt/lists/*

RUN set -eux; \
  rm -rf /tmp/llhttp; \
  git clone --depth 1 --branch "${LLHTTP_VERSION}" https://github.com/nodejs/llhttp.git /tmp/llhttp; \
  printf '%s\n' \
    'prefix=@CMAKE_INSTALL_PREFIX@' \
    'exec_prefix=${prefix}' \
    'libdir=${exec_prefix}/lib' \
    'includedir=${prefix}/include' \
    '' \
    'Name: llhttp' \
    'Description: llhttp' \
    'Version: @PROJECT_VERSION@' \
    'Libs: -L${libdir} -lllhttp' \
    'Cflags: -I${includedir}' \
    > /tmp/llhttp/libllhttp.pc.in; \
  cmake -S /tmp/llhttp -B /tmp/llhttp/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON -DCMAKE_INSTALL_PREFIX=/usr/local; \
  cmake --build /tmp/llhttp/build -- -j"$(nproc)"; \
  cmake --install /tmp/llhttp/build; \
  cp /usr/local/lib/pkgconfig/libllhttp.pc /usr/lib/x86_64-linux-gnu/pkgconfig/llhttp.pc; \
  ldconfig; \
  rm -rf /tmp/llhttp
