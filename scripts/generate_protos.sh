#!/usr/bin/env bash
# Generates Python classes from the core/proto definitions
set -euo pipefail

# Find project root (assumes script is in `scripts/`)
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." >/dev/null 2>&1 && pwd)"
PROTO_DIR="${PROJECT_ROOT}/core/proto"
OUT_DIR="${PROJECT_ROOT}/scripts/proto_build"

echo "============================================="
echo " Generating Protocol Buffers (Python)"
echo " Protocol Dir: $PROTO_DIR"
echo " Output Dir:   $OUT_DIR"
echo "============================================="

# Create output structure cleanly
rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR"

if ! command -v protoc &> /dev/null; then
    echo "[ERROR] protoc not found! Please run: sudo bash scripts/setup_deps.sh"
    exit 1
fi

# We compile the telemetry.proto directly into the outputs directory
protoc --proto_path="$PROTO_DIR" \
       --python_out="$OUT_DIR" \
       "$PROTO_DIR/telemetry.proto"

# Ensure Python treats it as a package module
touch "$OUT_DIR/__init__.py"

echo "[SUCCESS] Generated telemetry_pb2.py in $OUT_DIR"
