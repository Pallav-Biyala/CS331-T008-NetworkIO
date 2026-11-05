#!/usr/bin/env bash
# scripts/install_prerequisites.sh
# Complete setup script for fresh/new Linux machines (Server & Client).
# Installs all core utilities, compilation toolchains, networking tools,
# and benchmark dependencies.

set -euo pipefail

if [[ $EUID -ne 0 ]]; then
   echo "[ERROR] Please run with sudo: sudo ./install_prerequisites.sh" >&2
   exit 1
fi

echo "================================================================="
echo "   Complete Machine Provisioning for Linux Network Benchmarking  "
echo "================================================================="

# 1. Connectivity Check
echo "[1/5] Checking Internet Connection..."
if ! ping -c 1 -W 2 8.8.8.8 &>/dev/null && ! ping -c 1 -W 2 1.1.1.1 &>/dev/null; then
    echo "-----------------------------------------------------------------"
    echo "[!] No active internet connection detected!"
    echo "    Since this machine is fresh, it must connect to internet once"
    echo "    to download and install all necessary packages."
    echo ""
    echo "    Quick fix - unblock Wi-Fi or plug in your phone's USB tethering:"
    echo "      sudo rfkill unblock wifi 2>/dev/null || true"
    echo "      nmcli radio wifi on 2>/dev/null || true"
    echo "-----------------------------------------------------------------"
    read -rp "Press [Enter] once your internet connection is live (or Ctrl+C to exit)..."
fi

# 2. Enable Universe and Multiverse repositories (Ubuntu essentials)
echo "[2/5] Ensuring standard package repositories (universe/multiverse) are enabled..."
if command -v add-apt-repository &>/dev/null; then
    add-apt-repository -y universe >/dev/null 2>&1 || true
    add-apt-repository -y multiverse >/dev/null 2>&1 || true
fi

echo "[3/5] Updating apt package indices..."
apt-get update -y

# 3. Comprehensive list of packages
PACKAGES=(
    # --- Compilers & Build Tools ---
    build-essential          # gcc, g++, make, libc-dev, dpkg-dev
    gcc
    g++
    make
    cmake
    pkg-config
    liburing-dev             # Linux io_uring headers & shared library

    # --- Basic Shell Utilities & Editors ---
    curl
    wget
    git
    nano
    vim
    htop
    tmux
    screen
    tar
    unzip
    software-properties-common

    # --- Networking Inspection & Control ---
    iproute2                 # ip command suite (ip link, ip addr)
    ethtool                  # NIC speed, duplex, offloads inspection
    net-tools                # ifconfig, netstat, arp
    netcat-openbsd           # nc utility for server smoke-testing
    iperf3                   # network bandwidth & MTU testing
    tcpdump                  # packet inspection for troubleshooting
    bridge-utils
    rfkill                   # radio switch (wifi/bluetooth)

    # --- System Tuning, Kernel & Diagnostics ---
    procps                   # sysctl, ps, pkill, top
    util-linux               # taskset, prlimit, lscpu
    pciutils                 # lspci (to inspect network card hardware)
    usbutils                 # lsusb (to inspect USB network adapters)
    cgroup-tools             # cgroups v2 control
    linux-tools-common       # perf and cpupower tools
    "linux-tools-$(uname -r)"

    # --- Scripting & Automation ---
    python3
    python3-pip
    python3-numpy            # plot_results.py (pip will NOT work offline)
    python3-pandas
    python3-matplotlib
)

echo "[4/5] Installing core dependencies (this may take 1-3 minutes)..."
for pkg in "${PACKAGES[@]}"; do
    echo "  -> Installing $pkg..."
    apt-get install -y "$pkg" 2>/dev/null || {
        echo "     [NOTE] Optional package $pkg skipped or not available in this repo."
    }
done

# Try installing cpupower if available
apt-get install -y cpupower linux-cpupower 2>/dev/null || true

# 4. Strict Verification
echo ""
echo "[5/5] Verifying installed tools..."
echo "-----------------------------------------------------------------"
MISSING=0

check() {
    local name="$1"
    local cmd="$2"
    if command -v "$cmd" &>/dev/null; then
        printf "  %-20s [OK]  (%s)\n" "$name" "$(command -v "$cmd")"
    else
        printf "  %-20s [FAIL] (Missing!)\n" "$name"
        MISSING=1
    fi
}

check "C Compiler"         "gcc"
check "C++ Compiler"       "g++"
check "Make"               "make"
check "Pkg-Config"         "pkg-config"
check "IP Route (ip)"      "ip"
check "Ethtool"            "ethtool"
check "Netcat (nc)"        "nc"
check "Taskset (CPU pin)"  "taskset"
check "PRLimit (Limits)"   "prlimit"
check "Python 3"           "python3"
check "Git"                "git"
check "Nano Editor"        "nano"

if python3 -c "import numpy, pandas, matplotlib" 2>/dev/null; then
    printf "  %-20s [OK]\n" "pandas+matplotlib"
else
    printf "  %-20s [FAIL] (needed by plot_results.py; install BEFORE going offline)\n" "pandas+matplotlib"
    MISSING=1
fi

# Verify liburing specifically
if pkg-config --exists liburing; then
    VER=$(pkg-config --modversion liburing 2>/dev/null || echo "detected")
    printf "  %-20s [OK]  (liburing version %s)\n" "liburing-dev" "$VER"
else
    printf "  %-20s [FAIL] (liburing development package missing!)\n" "liburing-dev"
    MISSING=1
fi

echo "================================================================="
if [[ $MISSING -eq 0 ]]; then
    echo " [SUCCESS] All essentials & dependencies are installed and verified!"
    echo " You are 100% ready to run the benchmark suite."
    echo ""
    echo " Once done, you can disconnect internet / turn Wi-Fi off:"
    echo "   nmcli radio wifi off && sudo rfkill block wifi"
else
    echo " [WARNING] Some packages failed to install. Check the output above."
fi
echo "================================================================="
