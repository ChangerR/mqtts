# syntax=docker/dockerfile:1.5

FROM mcr.microsoft.com/devcontainers/base:ubuntu-22.04

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
