#!/usr/bin/env bash
# Kernel tuning for 250k+ concurrent connections on Ubuntu Linux.
# Run with: sudo bash scripts/tune_kernel.sh
# See specs.md §5 for rationale.
set -euo pipefail

echo "[tune_kernel] Applying sysctl parameters for 250k connections..."

sysctl -w fs.file-max=1000000
sysctl -w net.core.somaxconn=65535
sysctl -w net.ipv4.tcp_max_syn_backlog=65535
sysctl -w net.ipv4.ip_local_port_range="1024 65535"
sysctl -w net.core.netdev_max_backlog=50000
sysctl -w net.ipv4.tcp_tw_reuse=1

echo "[tune_kernel] Persisting to /etc/sysctl.d/99-loadbalancer.conf..."

cat > /etc/sysctl.d/99-loadbalancer.conf <<EOF
# Layer 4 Load Balancer — 250k CPS kernel tuning
fs.file-max = 1000000
net.core.somaxconn = 65535
net.ipv4.tcp_max_syn_backlog = 65535
net.ipv4.ip_local_port_range = 1024 65535
net.core.netdev_max_backlog = 50000
net.ipv4.tcp_tw_reuse = 1
EOF

echo "[tune_kernel] Setting process FD limits..."

if ! grep -q "loadbalancer" /etc/security/limits.d/99-loadbalancer.conf 2>/dev/null; then
    cat > /etc/security/limits.d/99-loadbalancer.conf <<EOF
# Layer 4 Load Balancer — process file descriptor limits
* soft nofile 1000000
* hard nofile 1000000
EOF
fi

echo "[tune_kernel] Done. Reboot or re-login for limits.conf changes to take effect."
echo "[tune_kernel] Verify with: sysctl fs.file-max && ulimit -n"
