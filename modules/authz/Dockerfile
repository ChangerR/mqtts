# Build from the module directory: docker build -t mqtts-authz modules/authz
FROM golang:1.25-bookworm AS builder
WORKDIR /src
COPY go.mod go.sum ./
RUN go mod download
COPY . .
RUN CGO_ENABLED=0 go build -trimpath -o /mqtts-authz ./cmd/mqtts-authz

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd -g 10001 mqtts && useradd -r -u 10001 -g mqtts mqtts \
    && mkdir /data && chown mqtts:mqtts /data
COPY --from=builder /mqtts-authz /usr/local/bin/mqtts-authz
USER mqtts
ENV AUTHZ_LISTEN=0.0.0.0:50051 AUTHZ_STORE=/data/authorization.db
VOLUME /data
EXPOSE 50051
ENTRYPOINT ["/usr/local/bin/mqtts-authz"]
