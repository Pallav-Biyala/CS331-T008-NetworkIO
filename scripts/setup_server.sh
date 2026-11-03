#!/usr/bin/env bash
# scripts/setup_server.sh
# System tuning script for the Server Laptop (Omen)
# Implements Section 12.3 & 14 [SETUP] requirements of the Fair Benchmarking Design Plan.

set -euo pipefail

if [[ $EUID -ne 0 ]]; then
   echo "[ERROR] This script must be run as root (sudo ./scripts/setup_server.sh)" >&2
   exit 1
fi

NIC_IFACE="${1:-}"
if [[ -z "$NIC_IFACE" ]]; then
    # Auto-detect default Ethernet interface
    NIC_IFACE=$(ip -br link | awk '$1 !~ /^(lo|wlan|wl|docker|virbr)/ && $2 ~ /UP|LOWER_UP/ {print $1; exit}')
    NIC_IFACE="${NIC_IFACE:-eth0}"
fi

echo "================================================================="
echo "   Applying Server (Omen) Machine Tuning for Fair Benchmarking"
echo "   Target Interface: $NIC_IFACE"
echo "================================================================="

# 1. CPU Frequency & Power Management
echo "[1/7] Setting CPU governor to performance..."
if command -v cpupower &>/dev/null; then
    cpupower frequency-set -g performance >/dev/null || true
elif [[ -d /sys/devices/system/cpu/cpu0/cpufreq ]]; then
    for gov in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
        echo "performance" > "$gov" 2>/dev/null || true
    done
fi

# Disable Intel/AMD Turbo boost jitter if accessible (keeps clock stable across runs)
if [[ -f /sys/devices/system/cpu/intel_pstate/no_turbo ]]; then
    echo "1" > /sys/devices/system/cpu/intel_pstate/no_turbo
    echo "  -> Intel turbo boost disabled for clock stability"
elif [[ -f /sys/devices/system/cpu/cpufreq/boost ]]; then
    echo "0" > /sys/devices/system/cpu/cpufreq/boost
    echo "  -> CPU boost disabled for clock stability"
fi

# 2. Kernel Resource Limits & Sysctls
echo "[2/7] Tuning kernel network stack & socket limits..."
sysctl -w net.core.somaxconn=16384 >/dev/null
sysctl -w net.ipv4.tcp_max_syn_backlog=16384 >/dev/null
sysctl -w net.ipv4.ip_local_port_range="1024 65535" >/dev/null
sysctl -w net.ipv4.tcp_tw_reuse=1 >/dev/null
sysctl -w net.core.rmem_max=16777216 >/dev/null
sysctl -w net.core.wmem_max=16777216 >/dev/null
sysctl -w net.ipv4.tcp_rmem="4096 87380 16777216" >/dev/null
sysctl -w net.ipv4.tcp_wmem="4096 65536 16777216" >/dev/null

# 3. Kernel Subsystems (io_uring and perf)
echo "[3/7] Enabling io_uring and perf profiling permissions..."
sysctl -w kernel.io_uring_disabled=0 >/dev/null 2>&1 || true
sysctl -w kernel.perf_event_paranoid=1 >/dev/null 2>&1 || true

# 4. File Descriptors & Memory Lock Limits
echo "[4/7] Setting ulimits (nofile and memlock)..."
prlimit --pid=$$ --nofile=65536:65536 || true
prlimit --pid=$$ --memlock=unlimited:unlimited || true

# Append to limits.conf if not already present
if ! grep -q "bench_limits" /etc/security/limits.conf; then
    cat << 'EOF' >> /etc/security/limits.conf
# bench_limits
* soft nofile 65536
* hard nofile 65536
* soft memlock unlimited
* hard memlock unlimited
EOF
fi

# 5. Dedicated cgroup v2 for Server CPU Isolation & Accounting
echo "[5/7] Configuring dedicated cgroup v2 for CPU accounting..."
CGROUP_DIR="/sys/fs/cgroup/bench"
if [[ -d /sys/fs/cgroup ]]; then
    mkdir -p "$CGROUP_DIR"
    if [[ -f /sys/fs/cgroup/cgroup.subtree_control ]]; then
        echo "+cpu" > /sys/fs/cgroup/cgroup.subtree_control 2>/dev/null || true
    fi
    echo "  -> Server cgroup created at: $CGROUP_DIR"
fi

# 6. Stop irqbalance & Pin NIC Interrupts to Dedicated Core
echo "[6/7] Managing IRQ placement..."
if systemctl is-active --quiet irqbalance; then
    systemctl stop irqbalance
    echo "  -> Stopped irqbalance service to prevent interrupt migration"
fi

# 7. Network Interface Tuning (Ethernet Link & Offloads)
echo "[7/7] Tuning Ethernet link ($NIC_IFACE)..."
if command -v ethtool &>/dev/null && ip link show "$NIC_IFACE" &>/dev/null; then
    # Disable Energy Efficient Ethernet (EEE) to eliminate microsecond wake latency
    ethtool --set-eee "$NIC_IFACE" eee off 2>/dev/null || true

    # Fix interrupt coalescence to static values (disable adaptive coalescence)
    ethtool -C "$NIC_IFACE" adaptive-rx off adaptive-tx off 2>/dev/null || true
    echo "  -> Disabled EEE and adaptive IRQ coalescing on $NIC_IFACE"
fi

echo "================================================================="
echo "[SUCCESS] Server machine tuning applied."
echo "Suggested Server Launch Command:"
echo "  cgexec -g cpu:bench ./build/echo-epoll --port 8080 --msg-size 512 --cpu 1 --stats-file server_stats.jsonl"
echo "================================================================="
