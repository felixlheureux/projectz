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
sysctl -w vm.nr_hugepages=1024

# Raise the TIME_WAIT bucket and conntrack table limits above 2^18 (262,144).
# The Ubuntu defaults for both are exactly 262,144, which is why load tests
# plateau there regardless of ephemeral port budget or proxy-side tuning.
#
# Use 2^21 = 2,097,152 as recommended for 250k CPS with safety margin.
# The hash table bucket count MUST be scaled proportionally (25% of max)
# to maintain O(1) lookup performance and prevent CPU starvation from
# excessively long hash chains.
sysctl -w net.ipv4.tcp_max_tw_buckets=2097152
if modprobe nf_conntrack 2>/dev/null; then
    sysctl -w net.netfilter.nf_conntrack_max=2097152
    sysctl -w net.netfilter.nf_conntrack_buckets=524288
fi

# Bypass conntrack entirely for loopback traffic.  Conntrack tracks every TCP
# state transition; under a loopback load test the table fills in milliseconds.
# NOTRACK rules prevent nf_conntrack from ever seeing loopback packets, so the
# nf_conntrack_max ceiling becomes irrelevant for local benchmarks.
if command -v iptables &>/dev/null; then
    iptables -t raw -C PREROUTING -i lo -j NOTRACK 2>/dev/null || \
        iptables -t raw -A PREROUTING -i lo -j NOTRACK
    iptables -t raw -C OUTPUT -o lo -j NOTRACK 2>/dev/null || \
        iptables -t raw -A OUTPUT     -o lo -j NOTRACK
    echo "[tune_kernel] Loopback NOTRACK rules applied."
fi

echo "[tune_kernel] Persisting to /etc/sysctl.d/99-loadbalancer.conf..."

cat > /etc/sysctl.d/99-loadbalancer.conf <<EOF
# Layer 4 Load Balancer — 250k CPS kernel tuning
fs.file-max = 1000000
net.core.somaxconn = 65535
net.ipv4.tcp_max_syn_backlog = 65535
net.ipv4.ip_local_port_range = 1024 65535
net.core.netdev_max_backlog = 50000
net.ipv4.tcp_tw_reuse = 1
vm.nr_hugepages = 1024
net.ipv4.tcp_max_tw_buckets = 2097152
net.netfilter.nf_conntrack_max = 2097152
net.netfilter.nf_conntrack_buckets = 524288
EOF

# udev rule: guarantee nf_conntrack_max is applied the instant the module loads.
# If systemd-sysctl runs BEFORE nf_conntrack is loaded (common on Ubuntu 24.04
# where it's a dynamically loadable kernel module), the sysctl.d values silently
# fail.  This rule fires at the exact chronological moment nf_conntrack activates.
UDEV_RULE="/etc/udev/rules.d/91-conntrack.rules"
if [ ! -f "$UDEV_RULE" ]; then
    cat > "$UDEV_RULE" <<'EOF'
ACTION=="add", SUBSYSTEM=="module", KERNEL=="nf_conntrack", RUN+="/usr/lib/systemd/systemd-sysctl --prefix=/net/netfilter"
EOF
    echo "[tune_kernel] udev rule created: $UDEV_RULE"
fi

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