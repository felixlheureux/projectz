#!/usr/bin/env bash
# Install the projectz load balancer as a systemd service on Ubuntu.
# Run as root on the bare metal LB node.
set -euo pipefail

BINARY="${1:-./loadbalancer}"
SERVICE_FILE="$(dirname "${BASH_SOURCE[0]}")/loadbalancer.service"
SYSCTL_FILE="$(dirname "${BASH_SOURCE[0]}")/loadbalancer-sysctl.conf"

[[ "$(id -u)" -eq 0 ]] || { echo "ERROR: must run as root"; exit 1; }
[[ -f "$BINARY" ]] || { echo "ERROR: binary not found at $BINARY"; exit 1; }

echo "Installing binary..."
install -m 755 "$BINARY" /usr/local/bin/loadbalancer

echo "Applying kernel tuning..."
cp "$SYSCTL_FILE" /etc/sysctl.d/99-projectz-lb.conf
sysctl -p /etc/sysctl.d/99-projectz-lb.conf

echo "Installing systemd unit..."
cp "$SERVICE_FILE" /etc/systemd/system/loadbalancer.service
systemctl daemon-reload
systemctl enable loadbalancer
systemctl restart loadbalancer

echo ""
echo "Done. Status:"
systemctl status loadbalancer --no-pager
echo ""
echo "Logs: journalctl -u loadbalancer -f"
