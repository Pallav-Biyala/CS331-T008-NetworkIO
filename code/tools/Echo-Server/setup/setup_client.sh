#!/usr/bin/env bash
# scripts/setup_client.sh
# System tuning script for the Client Laptop (Load Generator)
# Ensures zero-jitter, unconstrained socket issuance and microsecond-level timing.

set -euo pipefail

if [[ $EUID -ne 0 ]]; then
   echo "[ERROR] This script must be run as root (sudo ./scripts/setup_client.sh)" >&2
   exit 1
fi

NIC_IFACE="${1:-}"
if [[ -z "$NIC_IFACE" ]]; then
    NIC_IFACE=$(ip -br link | awk '$1 !~ /^(lo|wlan|wl|docker|virbr)/ && $2 ~ /UP|LOWER_UP/ {print $1; exit}')
    NIC_IFACE="${NIC_IFACE:-eth0}"
fi

echo "================================================================="
echo "   Applying Client Machine Tuning for High-Rate Load Generation"
echo "   Target Interface: $NIC_IFACE"
echo "================================================================="

# 1. CPU Governor to Performance
echo "[1/5] Setting CPU governor to performance..."
if command -v cpupower &>/dev/null; then
    cpupower frequency-set -g performance >/dev/null || true
elif [[ -d /sys/devices/system/cpu/cpu0/cpufreq ]]; then
    for gov in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
        echo "performance" > "$gov" 2>/dev/null || true
    done
fi

# 2. Kernel Socket & Port Range Tuning
echo "[2/5] Tuning outbound client socket ranges & recycling..."
sysctl -w net.ipv4.ip_local_port_range="1024 65535" >/dev/null
sysctl -w net.ipv4.tcp_tw_reuse=1 >/dev/null
sysctl -w net.core.somaxconn=16384 >/dev/null
sysctl -w net.core.rmem_max=16777216 >/dev/null
sysctl -w net.core.wmem_max=16777216 >/dev/null

# 3. File Descriptor Limits
echo "[3/5] Setting file descriptor limits to 65536..."
prlimit --pid=$$ --nofile=65536:65536 || true

if ! grep -q "bench_limits" /etc/security/limits.conf; then
    cat << 'EOF' >> /etc/security/limits.conf
# bench_limits
* soft nofile 65536
* hard nofile 65536
EOF
fi

# 4. Stop irqbalance on Client
echo "[4/5] Stopping irqbalance..."
if systemctl is-active --quiet irqbalance; then
    systemctl stop irqbalance || true
fi

# 5. Disable Energy Efficient Ethernet (EEE) to eliminate sleep wakeup jitter
echo "[5/5] Disabling EEE on client NIC ($NIC_IFACE)..."
if command -v ethtool &>/dev/null && ip link show "$NIC_IFACE" &>/dev/null; then
    ethtool --set-eee "$NIC_IFACE" eee off 2>/dev/null || true
    ethtool -C "$NIC_IFACE" adaptive-rx off adaptive-tx off 2>/dev/null || true
fi

echo "================================================================="
echo "[SUCCESS] Client machine tuning applied."
echo "Suggested Client Launch Command:"
echo "  taskset -c 1-2 ./echo_loadgen --host 192.168.50.1 --port 8080 --connections 500 --rate 2000 --duration 30 --output results.csv"
echo "================================================================="
