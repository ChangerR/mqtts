#!/usr/bin/env sh
set -eu
cd "$(dirname "$0")/.."
protoc -I proto --go_out=. --go_opt=module=github.com/ChangerR/mqtts/modules/authz \
  --go-grpc_out=. --go-grpc_opt=module=github.com/ChangerR/mqtts/modules/authz proto/authorization.proto
