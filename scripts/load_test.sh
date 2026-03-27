#!/usr/bin/env bash
# High-performance raw TCP layer 4 connection spammer
# Replaces `wrk` since the proxy doesn't parse HTTP responses yet.
set -euo pipefail

DURATION="${1:-10}"
PORT="${2:-8080}"
TARGET="127.0.0.1:$PORT"
BIN="scripts/tcp_spammer"

echo "============================================="
echo " Layer 4 Load Balancer — RAW TCP CPS Test"
echo " Target:   $TARGET"
echo " Duration: ${DURATION}s"
echo "============================================="
echo ""

if ! ss -tlnp | grep -q ":$PORT"; then
    echo "[ERROR] No process listening on port $PORT. Start the proxy first."
    exit 1
fi

echo "[1/2] Compiling C++ raw tcp connection spammer..."
g++ -O3 -pthread scripts/tcp_spammer.cpp -o "$BIN"

LISTEN_COUNT=$(ss -tlnp | grep ":$PORT" | wc -l)
echo "[INFO] Detected $LISTEN_COUNT listen sockets on port $PORT"
echo ""

echo "[2/2] Running load test for ${DURATION}s..."
./$BIN "$DURATION"

echo ""
echo "[DONE]"
