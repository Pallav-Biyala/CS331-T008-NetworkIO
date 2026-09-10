#!/bin/bash
# setup_bench_env.sh — one-time system prep for select/poll/epoll/io_uring benchmarking
set -e

echo "=== Checking required tools ==="
for tool in tcpkali strace pidstat vmstat gcc; do
    if ! command -v $tool &>/dev/null; then
        echo "Missing: $tool"
        MISSING=1
    fi
done

if [ "$MISSING" == "1" ]; then
    echo "Installing missing packages..."
    sudo apt update
    sudo apt install -y tcpkali strace sysstat build-essential liburing-dev
fi

echo "=== Raising fd limit for this shell ==="
ulimit -n 65536

echo "=== Applying sysctl tuning ==="
sudo sysctl -w net.core.somaxconn=16384
sudo sysctl -w net.ipv4.tcp_tw_reuse=1
sudo sysctl -w net.ipv4.ip_local_port_range="10000 65535"
sudo sysctl -w net.core.netdev_max_backlog=16384
sudo sysctl -w net.ipv4.tcp_max_syn_backlog=16384

echo "=== Current limits/settings ==="
ulimit -n
sysctl net.core.somaxconn net.ipv4.tcp_tw_reuse net.ipv4.ip_local_port_range net.core.netdev_max_backlog net.ipv4.tcp_max_syn_backlog

echo "=== Enabling sysstat data collection (for pidstat) ==="
sudo sed -i 's/ENABLED="false"/ENABLED="true"/' /etc/default/sysstat 2>/dev/null || true
sudo systemctl restart sysstat 2>/dev/null || true

echo "=== Compiling servers ==="
# Assumes this script lives in benchmarking_scripts/, servers live in ../code
CODE_DIR="$(dirname "$0")/../code"

if [ -d "$CODE_DIR" ]; then
    cd "$CODE_DIR"
    gcc -O2 -o server_select server_select.c network_utils.c
    gcc -O2 -o server_poll server_poll.c network_utils.c
    gcc -O2 -o server_epoll server_epoll.c network_utils.c
    gcc -O2 -o server_iouring server_iouring.c network_utils.c -luring
    echo "Binaries built in $CODE_DIR"
    cd - >/dev/null
else
    echo "WARNING: $CODE_DIR not found, skipping compilation. Run from benchmarking_scripts/ with code/ as a sibling folder."
fi

echo "=== Done. Environment ready for benchmarking. ==="
echo "NOTE: ulimit -n is per-shell — re-source or re-run 'ulimit -n 65536' in any new terminal you use for testing."