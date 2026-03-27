#!/usr/bin/env bash
# Self-contained L4 proxy CPS (Connections Per Second) load test.
#
# This script:
#   1. Builds the proxy binary and the tcp_spammer
#   2. Starts a lightweight TCP backend (accept-and-discard, no proto parsing)
#   3. Starts the proxy (port 8080 → backend port 8081)
#   4. Runs the spammer for DURATION seconds
#   5. Kills everything on exit — including on failure or Ctrl-C
#
# Usage: bash scripts/load_test.sh [duration=10] [proxy_port=8080] [backend_port=8081]
#
# NOTE: The backend is a single accept-and-discard server — appropriate for pure CPS
# testing where connections are immediately closed. For throughput benchmarks you'd
# want a real ingester, ideally on a separate host.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
LB_DIR="$ROOT_DIR/services/loadbalancer"

DURATION="${1:-10}"
PROXY_PORT="${2:-8080}"
BACKEND_PORT="${3:-8081}"

BIN="$SCRIPT_DIR/tcp_spammer"
PROXY_BIN="$LB_DIR/build/loadbalancer"

PROXY_PID=""
BACKEND_PID=""

cleanup() {
    local exit_code=$?
    echo ""
    echo "[cleanup] Stopping proxy and backend..."
    [[ -n "$PROXY_PID" ]]   && kill "$PROXY_PID"   2>/dev/null || true
    [[ -n "$BACKEND_PID" ]] && kill "$BACKEND_PID" 2>/dev/null || true
    [[ -n "$PROXY_PID" ]]   && wait "$PROXY_PID"   2>/dev/null || true
    [[ -n "$BACKEND_PID" ]] && wait "$BACKEND_PID" 2>/dev/null || true
    echo "[cleanup] Done."
    exit "$exit_code"
}
trap cleanup EXIT INT TERM

echo "============================================="
echo " Layer 4 Load Balancer — RAW TCP CPS Test"
echo " Proxy:    127.0.0.1:$PROXY_PORT"
echo " Backend:  127.0.0.1:$BACKEND_PORT"
echo " Duration: ${DURATION}s"
echo "============================================="
echo ""

# ── Step 1: Build ─────────────────────────────────────────────────────────────
echo "[1/4] Building proxy..."
(cd "$LB_DIR" && make build -s)
echo "      OK: $PROXY_BIN"

echo "[2/4] Building tcp_spammer..."
g++ -O3 -std=c++17 -pthread "$SCRIPT_DIR/tcp_spammer.cpp" -o "$BIN"
echo "      OK: $BIN"

# ── Step 2: Start a lightweight TCP backend ────────────────────────────────────
# Accept-and-discard — suitable for CPS testing where connections close immediately.
echo "[3/4] Starting backend on port $BACKEND_PORT..."
python3 - "$BACKEND_PORT" <<'PYEOF' &
import asyncio, sys

async def handle(reader, writer):
    try:
        while True:
            data = await reader.read(4096)
            if not data:
                break
    except Exception:
        pass
    finally:
        try:
            writer.close()
        except Exception:
            pass

async def main():
    port = int(sys.argv[1])
    server = await asyncio.start_server(handle, '127.0.0.1', port)
    print(f"[backend] Listening on 127.0.0.1:{port}", flush=True)
    async with server:
        await server.serve_forever()

asyncio.run(main())
PYEOF
BACKEND_PID=$!

# Wait for backend to be ready
for i in $(seq 1 20); do
    if ss -tlnp 2>/dev/null | grep -q ":$BACKEND_PORT"; then
        echo "      OK: backend ready (pid $BACKEND_PID)"
        break
    fi
    sleep 0.1
    if [[ $i -eq 20 ]]; then
        echo "[ERROR] Backend did not start within 2 seconds."
        exit 1
    fi
done

# ── Step 3: Start the proxy ────────────────────────────────────────────────────
echo "[4/4] Starting proxy on port $PROXY_PORT..."
"$PROXY_BIN" &
PROXY_PID=$!

# Wait for all SO_REUSEPORT listen sockets to appear
for i in $(seq 1 30); do
    if ss -tlnp 2>/dev/null | grep -q ":$PROXY_PORT"; then
        break
    fi
    sleep 0.1
    if [[ $i -eq 30 ]]; then
        echo "[ERROR] Proxy did not start within 3 seconds."
        exit 1
    fi
done

# Give all worker threads a moment to enter their epoll loops
sleep 0.2

LISTEN_COUNT=$(ss -tlnp 2>/dev/null | grep -c ":$PROXY_PORT" || echo "?")
echo "      OK: proxy ready (pid $PROXY_PID) — $LISTEN_COUNT SO_REUSEPORT listen socket(s)"

# ── Step 4: Run the spammer ────────────────────────────────────────────────────
echo ""
echo "[Running] Hammering proxy for ${DURATION}s..."
echo ""
"$BIN" "$DURATION" "$PROXY_PORT"
