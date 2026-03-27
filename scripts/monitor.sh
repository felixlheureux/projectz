#!/usr/bin/env bash
# Real-time monitoring dashboard for the running load balancer.
# Run in a separate terminal during load tests.
# Usage: sudo bash scripts/monitor.sh
set -euo pipefail

PID=$(pgrep -x loadbalancer 2>/dev/null || true)

if [ -z "$PID" ]; then
    echo "[ERROR] loadbalancer process not found. Start it first."
    exit 1
fi

echo "============================================="
echo " Load Balancer Monitor — PID: $PID"
echo " Press Ctrl+C to stop"
echo "============================================="
echo ""

while true; do
    clear
    echo "=== Load Balancer Monitor — $(date +%H:%M:%S) — PID: $PID ==="
    echo ""

    # 1. File descriptor count
    FD_COUNT=$(ls /proc/"$PID"/fd 2>/dev/null | wc -l)
    FD_LIMIT=$(cat /proc/"$PID"/limits 2>/dev/null | grep "Max open files" | awk '{print $4}')
    echo "[FDs]        $FD_COUNT / $FD_LIMIT"

    # 2. Listen socket count (SO_REUSEPORT workers)
    LISTEN=$(ss -tlnp 2>/dev/null | grep ":8080" | wc -l)
    echo "[Workers]    $LISTEN listen sockets on :8080"

    # 3. Established connections
    ESTABLISHED=$(ss -tnp 2>/dev/null | grep ":8080" | grep -c ESTAB || true)
    echo "[Conns]      $ESTABLISHED established"

    # 4. Accept queue depth per socket
    echo ""
    echo "[Accept Queues]"
    ss -tlnp 2>/dev/null | grep ":8080" | awk '{printf "  Recv-Q: %-6s Send-Q: %s\n", $2, $3}'

    # 5. TCP stats — SYN drops, overflows
    echo ""
    echo "[TCP Drops]"
    OVERFLOWS=$(grep -c "overflowed" /proc/net/netstat 2>/dev/null || netstat -s 2>/dev/null | grep -i "listen" || echo "  (unavailable)")
    echo "  $OVERFLOWS"

    # 6. CPU usage of the process
    echo ""
    echo "[CPU per thread]"
    ps -p "$PID" -L -o lwp,pcpu,psr --no-headers 2>/dev/null | head -10 || echo "  (unavailable)"

    # 7. Memory
    echo ""
    RSS=$(ps -p "$PID" -o rss= 2>/dev/null || echo "0")
    echo "[Memory]     RSS: $((RSS / 1024)) MB"

    sleep 1
done
