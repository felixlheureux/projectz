#!/usr/bin/env bash
# Installs all required dependencies for compiling and running
# the Python Telemetry Mock End-to-End tests.
# Usage: sudo bash scripts/setup_deps.sh
set -euo pipefail

echo "============================================="
echo " Installing Telemetry E2E Dependencies"
echo "============================================="

# Ensure apt is up to date
apt-get update -y

# Install the Protocol Compiler (protoc) and the Python runtime bindings
apt-get install -y protobuf-compiler python3-protobuf

# Verify installation
if ! command -v protoc &> /dev/null; then
    echo "[ERROR] protoc failed to install."
    exit 1
fi

echo "[SUCCESS] Dependencies installed."
protoc --version
