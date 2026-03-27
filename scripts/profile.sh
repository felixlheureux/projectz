#!/usr/bin/env bash
# CPU profiling for the load balancer using perf.
# Captures a call-graph flamegraph during a specified duration.
# Usage: sudo bash scripts/profile.sh [duration_seconds]
set -euo pipefail

DURATION="${1:-10}"
RESULTS_DIR="load_test_results"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)

mkdir -p "$RESULTS_DIR"

PID=$(pgrep -x loadbalancer 2>/dev/null || true)

if [ -z "$PID" ]; then
    echo "[ERROR] loadbalancer process not found. Start it first."
    exit 1
fi

if ! command -v perf &>/dev/null; then
    echo "[ERROR] perf not installed. Run: sudo apt install linux-tools-common linux-tools-\$(uname -r)"
    exit 1
fi

PERF_DATA="$RESULTS_DIR/perf_${TIMESTAMP}.data"
PERF_REPORT="$RESULTS_DIR/perf_${TIMESTAMP}.txt"

echo "============================================="
echo " CPU Profile — PID: $PID"
echo " Duration: ${DURATION}s"
echo " Output:   $PERF_DATA"
echo "============================================="
echo ""

echo "[1/3] Recording call graph for ${DURATION}s..."
perf record -g -p "$PID" -o "$PERF_DATA" -- sleep "$DURATION"

echo "[2/3] Generating report..."
perf report -i "$PERF_DATA" --stdio --no-children 2>&1 | head -80 | tee "$PERF_REPORT"

echo ""
echo "[3/3] Syscall breakdown (strace, 5s sample)..."
STRACE_OUT="$RESULTS_DIR/strace_${TIMESTAMP}.txt"
timeout 5 strace -c -p "$PID" 2>&1 | tee "$STRACE_OUT" || true

echo ""
echo "[DONE] Artifacts:"
echo "  perf data:    $PERF_DATA"
echo "  perf report:  $PERF_REPORT"
echo "  strace:       $STRACE_OUT"
echo ""
echo "To drill deeper: sudo perf report -i $PERF_DATA"
