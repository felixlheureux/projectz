#!/usr/bin/env bash
# Regenerates all protobuf bindings from core/proto.
# Run this whenever a .proto file changes — before compiling any service.
#
# Requirements:
#   protoc          — brew install protobuf
#   protoc-gen-go   — go install google.golang.org/protobuf/cmd/protoc-gen-go@latest
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

# Ensure protoc-gen-go is on PATH (go install puts it in $GOPATH/bin).
export PATH="$(go env GOPATH)/bin:$PATH"

command -v protoc        &>/dev/null || { echo "ERROR: protoc not found — brew install protobuf"; exit 1; }
command -v protoc-gen-go &>/dev/null || { echo "ERROR: protoc-gen-go not found — go install google.golang.org/protobuf/cmd/protoc-gen-go@latest"; exit 1; }

echo "Generating Go bindings..."
protoc \
  --proto_path=core/proto \
  --go_out=. \
  --go_opt=module=projectz \
  core/proto/telemetry/v1/telemetry.proto

echo "Done. Generated files:"
find core/telemetry -name "*.pb.go"
