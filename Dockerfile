# syntax=docker/dockerfile:1.5

FROM ubuntu:22.04 AS builder

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
    ca-certificates \
  && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  && cmake --build build --target mqtts

FROM ubuntu:22.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    libssl3 \
    libyaml-cpp0.7 \
    libsqlite3-0 \
    libhiredis0.14 \
    netcat-openbsd \
    ca-certificates \
  && rm -rf /var/lib/apt/lists/*

RUN useradd -r -s /bin/false -d /app mqtts \
  && mkdir -p /app/bin /app/lib /app/config /app/logs \
  && chown -R mqtts:mqtts /app

COPY --from=builder /src/build/mqtts /app/bin/mqtts
COPY --from=builder /src/build/3rd/gperftools/libtcmalloc_minimal.so /app/lib/libtcmalloc_minimal.so
COPY mqtts.yaml /app/config/mqtts.yaml

RUN chmod +x /app/bin/mqtts \
  && chown -R mqtts:mqtts /app

ENV LD_LIBRARY_PATH=/app/lib

WORKDIR /app
EXPOSE 1883 8883 18080

USER mqtts

HEALTHCHECK --interval=30s --timeout=10s --start-period=5s --retries=3 \
  CMD nc -z localhost 1883 || exit 1

CMD ["/app/bin/mqtts", "-c", "/app/config/mqtts.yaml"]
