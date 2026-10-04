# syntax=docker/dockerfile:1.5

FROM ubuntu:22.04 AS builder

ARG LLHTTP_VERSION=v6.0.7

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    ninja-build \
    pkg-config \
    git \
    libssl-dev \
    libyaml-cpp-dev \
    libsqlite3-dev \
    libhiredis-dev \
    protobuf-compiler \
    libprotobuf-dev \
    libgrpc++-dev \
    protobuf-compiler-grpc \
    libcurl4-openssl-dev \
    nlohmann-json3-dev \
    python3 \
    ca-certificates \
  && rm -rf /var/lib/apt/lists/*

RUN --mount=type=secret,id=proxy_ca set -eux; \
  if [ -f /run/secrets/proxy_ca ]; then export GIT_SSL_CAINFO=/run/secrets/proxy_ca; fi; \
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

WORKDIR /src
COPY . .

RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  && cmake --build build --target mqtts --parallel 3

FROM ubuntu:22.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    libssl3 \
    libyaml-cpp0.7 \
    libsqlite3-0 \
    libhiredis0.14 \
    libprotobuf23 \
    libgrpc++1 \
    libcurl4 \
    netcat-openbsd \
    ca-certificates \
  && rm -rf /var/lib/apt/lists/*

RUN useradd -r -s /bin/false -d /app mqtts \
  && mkdir -p /app/bin /app/lib /app/config /app/logs /data \
  && chown -R mqtts:mqtts /app /data

COPY --from=builder /src/build/mqtts /app/bin/mqtts
COPY --from=builder /usr/local/lib/libllhttp.so* /app/lib/
COPY mqtts.yaml /app/config/mqtts.yaml

RUN chmod +x /app/bin/mqtts \
  && chown -R mqtts:mqtts /app /data

ENV LD_LIBRARY_PATH=/app/lib

WORKDIR /app
EXPOSE 1883 8883

USER mqtts

HEALTHCHECK --interval=30s --timeout=10s --start-period=5s --retries=3 \
  CMD nc -z localhost 1883 || exit 1

CMD ["/app/bin/mqtts", "-c", "/app/config/mqtts.yaml"]
