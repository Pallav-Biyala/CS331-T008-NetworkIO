# Benchmark Setup Guide for `linux-network-io-bench`

> **Purpose** – This document provides a **step‑by‑step, reproducible** procedure to prepare two Linux laptops (one *server* – Omen, one *client* – Client) for running the TCP echo‑server benchmark suite.  It disables background interference, configures the network, builds the binaries, runs the load generator, and collects results.

---

## 1. Prerequisites

| Item | Minimum Requirement |
|------|---------------------|
| OS   | Ubuntu 22.04 LTS (or any recent Debian‑based distro) – kernel **≥ 6.0** (timerfd & epoll support) |
| CPU  | Modern x86‑64 with `performance` governor support |
| RAM  | ≥ 4 GB (per machine) |
| Network | Direct 1 Gbps Ethernet cable (crossover not needed on modern NICs) |
| Tools | `git`, `make`, `gcc`, `g++`, `pkg-config`, `liburing-dev` (for io_uring builds) |

> **Tip** – Clone the repository on *both* machines:
>
> ```bash
> git clone https://github.com/YourOrg/linux-network-io-bench.git
> cd linux-network-io-bench
> ```
>
> The default branch for this guide is `project2`.

---

## 2. Power‑Cycle & Basic System Clean‑Up

1. **Shut down both laptops** completely (do **not** suspend).
2. Power them back on and **log in as a regular user** (e.g., `koko`).
3. Close **all graphical tabs/applications** (web browsers, editors, email clients, etc.) – they can inject background CPU/network noise.
4. Open a terminal on each machine and **switch to the repository directory**:
   ```bash
   cd ~/linux-network-io-bench
   ```

---

## 3. Disable Unnecessary Background Services

Run the following commands **as root** (use `sudo`):

```bash
# 1. Disable automatic package updates
sudo systemctl stop apt-daily.timer apt-daily-upgrade.timer
sudo systemctl disable apt-daily.timer apt-daily-upgrade.timer

# 2. Stop the snap refresh daemon (if present)
sudo systemctl stop snapd.refresh.timer
sudo systemctl disable snapd.refresh.timer

# 3. Turn off NetworkManager auto‑updates (keep only the Ethernet interface up)
sudo systemctl mask NetworkManager-wait-online.service

# 4. Disable Bluetooth and Wi‑Fi (we use a wired link only)
sudo systemctl stop bluetooth.service
sudo systemctl disable bluetooth.service
nmcli radio wifi off

# 5. Stop any user‑space telemetry agents (e.g., apport, ubuntu-report)
sudo systemctl stop apport.service
sudo systemctl disable apport.service

# 6. Optional: stop graphical compositor if you are running a heavy desktop (KDE, GNOME)
#    This is optional but can shave a few % CPU.
# sudo systemctl isolate multi-user.target   # drops to console mode
```

> **Verification** – After the above, `systemctl list-units --type=service | grep -iE 'apt|snap|bluetooth|wifi|apport'` should show **inactive**/disabled.

---

## 4. System‑Level Performance Tuning

### 4.1 CPU Frequency Governor

```bash
# Force all CPUs to "performance" mode
for cpu in /sys/devices/system/cpu/cpu[0-9]*; do
  echo performance | sudo tee $cpu/cpufreq/scaling_governor >/dev/null
done
```

### 4.2 Turbo Boost (disable for deterministic latency)

```bash
# On Intel platforms
if [ -f /sys/devices/system/cpu/intel_pstate/no_turbo ]; then
  echo 1 | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo >/dev/null
else
  # Fallback for older kernels
  echo 0 | sudo tee /sys/devices/system/cpu/cpufreq/boost >/dev/null
fi
```

### 4.3 Network Stack Sysctls (recommended values are already in the repo scripts)

```bash
sudo sysctl -w net.core.somaxconn=16384 \
               net.ipv4.tcp_max_syn_backlog=16384 \
               net.core.rmem_max=16777216 \
               net.core.wmem_max=16777216 \
               net.ipv4.ip_local_port_range="1024 65535" \
               kernel.io_uring_disabled=0
```

### 4.4 Resource Limits (ulimit)

```bash
# Increase the number of open files
ulimit -n 65536
ulimit -l unlimited   # lockable memory (for io_uring)
```

Persist the limits for the current user (add to `~/.bashrc` or `/etc/security/limits.conf`):

```bash
# ~/.bashrc addition
ulimit -n 65536
ulimit -l unlimited
```

### 4.5 Disable C‑State Power‑Saving (optional, for ultra‑stable latency)

```bash
# Requires reboot to take effect
echo "processor" | sudo tee /sys/devices/system/cpu/cpuidle/disable >/dev/null
```

---

## 5. Network Configuration – Direct Ethernet Link

### 5.1 Identify the Ethernet Interface

Both laptops may name the Ethernet device differently (e.g., `eth0`, `enp3s0`, `ens33`). Run:

```bash
ip link show | grep -iE 'state UP|ether'
```

The interface that shows `UP` and has an `ether` (MAC) address is the one you will configure. Replace `eth0` in the commands below with the actual name if it differs.

### 5.2 Assign Static IP Addresses

We will use a simple /24 network. The server (Omen) will be `192.168.50.1` and the client will be `192.168.50.2`.

#### Ubuntu 22.04 (netplan) – Preferred Method

Create a netplan YAML file on **each** machine (adjust the IP accordingly):

```bash
sudo mkdir -p /etc/netplan
```

**On Omen (server):**

```yaml
# /etc/netplan/01-static-eth0.yaml
network:
  version: 2
  ethernets:
    eth0:   # replace with your interface name if different
      dhcp4: no
      addresses: [192.168.50.1/24]
      gateway4: 192.168.50.1
      nameservers:
        addresses: [8.8.8.8, 8.8.4.4]
```

**On Client:**

```yaml
# /etc/netplan/01-static-eth0.yaml
network:
  version: 2
  ethernets:
    eth0:
      dhcp4: no
      addresses: [192.168.50.2/24]
      gateway4: 192.168.50.1
      nameservers:
        addresses: [8.8.8.8, 8.8.4.4]
```

Apply the configuration:

```bash
sudo netplan apply
```

#### Fallback – Manual `ip` Commands (works on any Linux)

If you prefer not to touch netplan, you can assign the address directly (temporary until reboot):

```bash
# On Omen (server)
sudo ip addr add 192.168.50.1/24 dev eth0
sudo ip link set eth0 up

# On Client
sudo ip addr add 192.168.50.2/24 dev eth0
sudo ip link set eth0 up
```

### 5.3 Disable Wi‑Fi and Bluetooth (ensure only Ethernet traffic)

```bash
# Turn off Wi‑Fi via NetworkManager
nmcli radio wifi off
# Turn off Bluetooth
sudo systemctl stop bluetooth.service
sudo systemctl disable bluetooth.service
```

### 5.4 Verify the Link and Speed

Check that the NIC negotiated 1 Gbps:

```bash
sudo ethtool eth0 | grep -i "speed"
```

If the speed shows `1000Mb/s`, you are ready. If it falls back to a lower speed, verify the cable and NIC capabilities.

### 5.5 Test Connectivity

From **client**:

```bash
ping -c 5 192.168.50.1   # should receive replies from server
```

From **server**:

```bash
ping -c 5 192.168.50.2   # should receive replies from client
```

If both pings succeed with low latency (< 1 ms), the Ethernet link is correctly configured and ready for benchmarking.

---

## 6. Build the Benchmark Binaries

All builds are **isolated** from the server code; they only compile the echo servers and the load‑generator.

```bash
# From the repo root (both machines have the same source)
make echo          # builds echo-select, echo-poll, echo-epoll, echo-uring
make echo_loadgen  # builds the load‑generator binary (echo_loadgen)
```

> The `Makefile` uses `CXXFLAGS=-O3 -pthread`. Adjust if you need debug symbols (`-g`).

---

## 7. Verify Server Binaries

Start each server in a **separate terminal** on the *Omen* machine (the server). Example for epoll:

```bash
./build/echo-epoll --port 8080 --msg-size 512 &
PID=$!
# Wait a moment, then test with netcat
printf '\x45\x43\x48\x4F' | nc -N 192.168.50.2 8080
# Stop the server
kill -TERM $PID; wait $PID
```

Repeat for `echo-select`, `echo-poll`, `echo-uring`.

---

## 8. Run a Full Benchmark Run (Client Side)

### 8.1 Basic Example

```bash
./echo_loadgen \
    --host 192.168.50.1 \
    --port 8080 \
    --connections 500 \
    --rate 4000 \
    --duration 30 \
    --warmup 10 \
    --cooldown 10 \
    --msg-size 512 \
    --output results_conn500_rate4000.csv
```

### 8.2 Full Matrix (Optional Wrapper Script)

Create a small bash script `run_matrix.sh` on the client to iterate over all combos:

```bash
#!/usr/bin/env bash
set -euo pipefail

SERVER=192.168.50.1
PORT=8080
MSG=512
DURATION=30
WARMUP=10
COOLDOWN=10

connections=(100 500 900)
rates=(500 1000 2000 4000 8000)

for C in "${connections[@]}"; do
  for R in "${rates[@]}"; do
    OUT="results_conn${C}_rate${R}.csv"
    echo "--- Running: C=${C}, R=${R} msgs/s -> ${OUT}"
    ./echo_loadgen \
      --host $SERVER --port $PORT \
      --connections $C --rate $R \
      --duration $DURATION --warmup $WARMUP --cooldown $COOLDOWN \
      --msg-size $MSG --output $OUT
  done
done
```

Make it executable (`chmod +x run_matrix.sh`) and run it. The script will generate a CSV per combination.

---

## 9. Post‑Run: Collect & Analyse Results

All CSV files share the same header (see `LOADGEN_README.md` lines 127‑144). To aggregate results:

```bash
python3 - <<'PY'
import pandas as pd, glob, sys
files = glob.glob('results_*.csv')
all_df = pd.concat([pd.read_csv(f) for f in files], ignore_index=True)
print(all_df[['connections','target_rate','actual_send_rate','echoed_rate','rtt_avg','target_achieved']])
# Optionally export a summary
all_df.to_csv('combined_summary.csv', index=False)
PY
```

You can now plot throughput vs. connections, latency percentiles, etc. (see the *Collect & Visualise* section in **LOADGEN_README.md** for a quick matplotlib example).

---

## 10. Cleanup & Reset

When the experiment is finished:

```bash
# Stop any lingering server processes
pkill -f "echo-" || true
# Remove build artifacts (optional)
make clean
# Restore CPU governor to default (usually "powersave")
for cpu in /sys/devices/system/cpu/cpu[0-9]*; do
  echo powersave | sudo tee $cpu/cpufreq/scaling_governor >/dev/null
done
# Re‑enable system services if you need them again
sudo systemctl enable apt-daily.timer apt-daily-upgrade.timer
sudo systemctl start apt-daily.timer apt-daily-upgrade.timer
# Re‑enable Wi‑Fi / Bluetooth if desired
nmcli radio wifi on
sudo systemctl start bluetooth.service
```

---

## 11. Troubleshooting FAQ

| Symptom | Likely Cause | Fix |
|---------|--------------|-----|
| **Very low send rate** (`actual_send_rate << target_rate`) | Too few connections for the RTT observed (Little’s Law) | Increase `--connections` or lower the target rate. |
| **High `concurrency_stalls`** | All connections waiting for replies (server bottleneck) | Verify server CPU scaling governor, check server load, or increase message size to reduce per‑msg overhead. |
| **Timeouts / mismatched messages** | Network drops or NIC offloading interference | Disable generic‑receive‑offload (GRO) and large‑receive‑offload (LRO): `sudo ethtool -K eth0 gro off lro off`. |
| **`socket: Too many open files`** | `ulimit -n` still low | Raise the limit (e.g., `sudo sysctl -w fs.file-max=200000` and update `/etc/security/limits.conf`). |
| **`timerfd_create` fails** | Kernel version < 5.1 | Upgrade kernel (the repo requires ≥ 6.0 for best performance). |

---

## 12. References & Further Reading

- **Load‑generator design** – see `LOADGEN_README.md` (protocol framing, rate‑pacing, Little’s Law). 
- **Linux networking performance** – *Linux Performance* (Brendan Gregg) for sysctl tuning.
- **io_uring** – Documentation in `/usr/share/doc/liburing-doc/`.

---

*Prepared by Antigravity – your AI coding assistant.*
