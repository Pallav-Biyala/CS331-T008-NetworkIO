# Linux Network I/O Benchmark: Complete System & Hardware Setup Guide

This guide provides the exact, step-by-step runbook to prepare **Laptop 1 (Server / Omen)** and **Laptop 2 (Client / Load Generator)** for a scientifically fair, reproducible comparison of `select()`, `poll()`, `epoll()`, and `io_uring()`.

Every step is designed to ensure that any observed differences in throughput, latency, or CPU cost are caused strictly by the I/O mechanism—not by wireless interference, SMT core sharing, thermal throttling, NIC interrupt stealing, or measurement artifacts.

---

## Phase 1: Power-Off & Physical Hardware Preparation

1. **Power off both laptops completely** (`shutdown -h now`). Allowing the laptops to cool before starting resets initial thermal conditions.
2. **Connect AC Power Bricks**:
   - Plug both laptops into their original high-wattage wall chargers. Never run benchmarks on battery power.
   - Ensure the laptops sit on a hard, flat surface with unobstructed intake/exhaust vents (elevate the rear feet slightly if possible to prevent thermal throttling).
3. **Unplug Unnecessary Peripherals**:
   - Remove all USB drives, external hard disks, USB hubs, second monitors, and audio dongles.
4. **Connect the Ethernet Cable**:
   - Connect a Cat6 (or Cat5e/Cat6a) Ethernet cable directly between the RJ45 Ethernet ports of Laptop 1 and Laptop 2 (avoid USB-to-Ethernet dongles if native PCIe RJ45 ports exist).
5. **Boot Both Laptops into Native Linux**:
   - Log into a lightweight session (or switch to a virtual console `Ctrl+Alt+F3` / minimal window manager).
   - Close all web browsers, IDEs (VS Code), Discord/Slack, Docker containers, and background updaters.

---

## Phase 2: Kill Wi-Fi, Bluetooth, and Background Daemons (Both Laptops)

Run these commands on **both Laptop 1 and Laptop 2** to eliminate radio interrupts, background network manager scans, and periodic system timers.

```bash
# 1. Soft-block and disable Wi-Fi and Bluetooth radios
sudo rfkill block wifi
sudo rfkill block bluetooth

# 2. If NetworkManager is installed, turn off Wi-Fi management
nmcli radio wifi off 2>/dev/null || true

# 3. Stop background services that cause periodic CPU/disk/network spikes
sudo systemctl stop bluetooth.service 2>/dev/null || true
sudo systemctl stop cups.service 2>/dev/null || true
sudo systemctl stop avahi-daemon.service avahi-daemon.socket 2>/dev/null || true
sudo systemctl stop ModemManager.service 2>/dev/null || true
sudo systemctl stop packagekit.service 2>/dev/null || true
sudo systemctl stop unattended-upgrades.service 2>/dev/null || true
sudo systemctl stop docker.service docker.socket 2>/dev/null || true
```

**Verify Radio Isolation**:
```bash
rfkill list
# Every Wireless LAN and Bluetooth entry must show: "Soft blocked: yes"
```

---

## Phase 3: Static Ethernet Link Setup (Before Auto-Detection)

Configure static IPs on the wired Ethernet interface **before** running any setup scripts so NIC auto-detection never selects a down or virtual interface.

### 1. Identify the Wired Ethernet Interface (Both Laptops)
```bash
ip -br link
```
Look for the physical Ethernet interface name (typically starts with `enp...`, `eno...`, or `eth0`).

### 2. On Laptop 1 (Server / Omen) — Configure `192.168.50.1/24`
```bash
export NIC="enp3s0"   # <-- REPLACE with Laptop 1's actual wired interface name

# Prevent NetworkManager from overriding the interface during the benchmark
sudo nmcli device set "$NIC" managed no 2>/dev/null || true

sudo ip link set dev "$NIC" up
sudo ip addr flush dev "$NIC"
sudo ip addr add 192.168.50.1/24 dev "$NIC"
```

### 3. On Laptop 2 (Client / Load Generator) — Configure `192.168.50.2/24`
```bash
export NIC="enp0s31f6" # <-- REPLACE with Laptop 2's actual wired interface name

sudo nmcli device set "$NIC" managed no 2>/dev/null || true

sudo ip link set dev "$NIC" up
sudo ip addr flush dev "$NIC"
sudo ip addr add 192.168.50.2/24 dev "$NIC"
```

### 4. Verify Physical Link Speed, Duplex, and Connectivity
Run on **both** laptops:
```bash
ethtool "$NIC" | grep -E "Speed:|Duplex:|Link detected:"
```
Expected output:
- `Speed: 1000Mb/s` (or `2500Mb/s`, matching both ends)
- `Duplex: Full`
- `Link detected: yes`

Test connectivity from **Laptop 2 (Client)**:
```bash
ping -c 4 192.168.50.1
```

---

## Phase 4: Discover True Physical CPU & SMT Topology (Both Laptops)

Do **not** blindly assume logical `CPU 1` and `CPU 2` are separate physical cores. On many Intel/AMD laptops, `CPU 0` and `CPU 1` (or `CPU 2` and `CPU 3`) are SMT (Hyper-Threading) siblings sharing the same physical execution core and L1/L2 caches, or split between Performance (P) and Efficiency (E) cores.

Run this command on **both Laptop 1 and Laptop 2**:
```bash
lscpu -e=CPU,CORE,SOCKET,NODE,L1d:L1i:L2:L3,MAXMHZ,MINMHZ
```

### Example Output Interpretation
```text
CPU CORE SOCKET NODE L1d:L1i:L2:L3 MAXMHZ   MINMHZ
  0    0      0    0 0:0:0:0       4700.000 400.000
  1    0      0    0 0:0:0:0       4700.000 400.000
  2    1      0    0 1:1:1:0       4700.000 400.000
  3    1      0    0 1:1:1:0       4700.000 400.000
  4    2      0    0 2:2:2:0       4700.000 400.000
  5    2      0    0 2:2:2:0       4700.000 400.000
```

### Rules for Selecting Cores (Write Down Your Numbers)
1. **Leave `CORE 0` (`CPU 0` and its sibling `CPU 1`) for OS housekeeping**.
2. **Pick ONE dedicated Physical Performance Core (highest `MAXMHZ`) for the benchmark process**:
   - In the example above, `CORE 1` consists of logical `CPU 2` and `CPU 3`.
   - Set `BENCH_CPU=2` (we will pin the single-threaded server or load generator to `CPU 2`) and **leave its SMT sibling `CPU 3` completely idle** (no tasks, no NIC IRQs).
3. **Pick a DIFFERENT Physical Core for NIC Interrupts**:
   - In the example above, `CORE 2` consists of logical `CPU 4` and `CPU 5`.
   - Set `IRQ_CPU=4` (NIC interrupts can live on `CPU 4` or across other non-server cores, never on `CPU 2` or `CPU 3`).

Export these variables in your terminal on **Laptop 1 (Server)**:
```bash
export SERVER_CPU=2          # <-- Replace with your chosen logical CPU on Physical Core 1
export SERVER_SMT_SIBLING=3  # <-- Replace with its SMT sibling (if SMT is present)
export IRQ_CPU=4             # <-- Replace with a logical CPU on a different Physical Core
```

Export the equivalent variables in your terminal on **Laptop 2 (Client)**:
```bash
export CLIENT_CPU=2          # <-- Replace with your chosen logical CPU for echo_loadgen
export CLIENT_SMT_SIBLING=3
export CLIENT_IRQ_CPU=4      # <-- Replace with a logical CPU on a different Physical Core
```

---

## Phase 5: Inspect NIC Queues, Offloads, and IRQ Mappings (Both Laptops)

Before running the tuning scripts, inspect and record the baseline NIC state so nothing is altered blindly.

```bash
# 1. Inspect MSI-X queues / channels
ethtool -l "$NIC" 2>/dev/null || true

# 2. Inspect hardware offloads (Preserve GRO, GSO, TSO at defaults!)
ethtool -k "$NIC" | grep -E "generic-receive-offload|generic-segmentation-offload|tcp-segmentation-offload|rx-checksumming|tx-checksumming"

# 3. Inspect current IRQ mappings for the NIC
grep "$NIC" /proc/interrupts

# 4. Inspect RPS/RFS settings (keep defaults unless misconfigured)
head -n 20 /sys/class/net/"$NIC"/queues/rx-*/rps_cpus 2>/dev/null || true
```

> **Important Fairness Rule**: Do **not** disable GRO, GSO, TSO, or checksum offloads. Keep standard Linux TCP autotuning (`net.ipv4.tcp_moderate_rcvbuf=1`) enabled. Only disable **Energy Efficient Ethernet (EEE)** and **Adaptive IRQ Coalescing**.

---

## Phase 6: Configure Laptop 1 (Server / Omen)

Run the following commands on **Laptop 1 (Server)** from the repository root (`~/linux-network-io-bench`):

### 1. Run the Server Setup Script Explicitly Passing `$NIC`
```bash
sudo ./scripts/setup_server.sh "$NIC"
```

### 2. Verify & Lock TCP Autotuning, cgroup v2, and NIC Settings
```bash
# Ensure TCP receive buffer autotuning remains enabled
sudo sysctl -w net.ipv4.tcp_moderate_rcvbuf=1

# Ensure cgroup v2 'bench' exists and has NO CPU bandwidth quota (no throttling)
sudo mkdir -p /sys/fs/cgroup/bench
echo "+cpu" | sudo tee /sys/fs/cgroup/cgroup.subtree_control >/dev/null 2>&1 || true
echo "max 100000" | sudo tee /sys/fs/cgroup/bench/cpu.max >/dev/null 2>&1 || true
```

### 3. Keep NIC Interrupts Away from the Server's Physical Core
Inspect `/proc/interrupts` for `$NIC`. If the NIC has multiple MSI-X queues, keep them away from `$SERVER_CPU` and `$SERVER_SMT_SIBLING` (for example, assigning them to `$IRQ_CPU` or spreading across non-server cores):

```bash
sudo systemctl stop irqbalance 2>/dev/null || true

for irq in $(grep "$NIC" /proc/interrupts | awk -F: '{print $1}' | tr -d ' '); do
    echo "$IRQ_CPU" | sudo tee /proc/irq/$irq/smp_affinity_list >/dev/null 2>&1 || true
done

# Verify actual effective affinity applied by the kernel
for irq in $(grep "$NIC" /proc/interrupts | awk -F: '{print $1}' | tr -d ' '); do
    echo "Server NIC IRQ $irq -> Core(s): $(cat /proc/irq/$irq/effective_affinity_list)"
done
```
Confirm that **none** of the printed cores match `$SERVER_CPU` or `$SERVER_SMT_SIBLING`.

### 4. Apply File Descriptor & Locked-Memory Limits to Your Active Shell
```bash
sudo prlimit --pid $$ --nofile=65536:65536 --memlock=unlimited:unlimited
```

### 5. Final Server Verification Checklist
Run every line below and confirm the expected output before starting any test:
```bash
ulimit -n                                                         # Expected: 65536
ulimit -l                                                         # Expected: unlimited
cat /proc/sys/net/core/somaxconn                                  # Expected: 16384
cat /proc/sys/kernel/io_uring_disabled                            # Expected: 0
cat /sys/devices/system/cpu/cpu${SERVER_CPU}/cpufreq/scaling_governor # Expected: performance
cat /sys/fs/cgroup/bench/cpu.max                                  # Expected: max 100000
ethtool --show-eee "$NIC" 2>/dev/null | grep -i "EEE status"      # Expected: disabled / not supported
ethtool -c "$NIC" 2>/dev/null | grep -i "Adaptive"                # Expected: Adaptive RX: off  TX: off
```

---

## Phase 7: Configure Laptop 2 (Client / Load Generator)

Run the following commands on **Laptop 2 (Client)** from the repository root (`~/linux-network-io-bench`):

### 1. Run the Client Setup Script Explicitly Passing `$NIC`
```bash
sudo ./scripts/setup_client.sh "$NIC"
```

### 2. Isolate Client NIC Interrupts Away from `$CLIENT_CPU`
```bash
sudo systemctl stop irqbalance 2>/dev/null || true

for irq in $(grep "$NIC" /proc/interrupts | awk -F: '{print $1}' | tr -d ' '); do
    echo "$CLIENT_IRQ_CPU" | sudo tee /proc/irq/$irq/smp_affinity_list >/dev/null 2>&1 || true
done

# Verify effective IRQ affinity
for irq in $(grep "$NIC" /proc/interrupts | awk -F: '{print $1}' | tr -d ' '); do
    echo "Client NIC IRQ $irq -> Core(s): $(cat /proc/irq/$irq/effective_affinity_list)"
done
```
Confirm that **none** of the printed cores match `$CLIENT_CPU` or `$CLIENT_SMT_SIBLING`.

### 3. Apply File Descriptor & Locked-Memory Limits to Your Active Shell
```bash
sudo prlimit --pid $$ --nofile=65536:65536 --memlock=unlimited:unlimited
```

### 4. Final Client Verification Checklist
```bash
ulimit -n                                                         # Expected: 65536
cat /proc/sys/net/ipv4/ip_local_port_range                        # Expected: 1024 65535
cat /proc/sys/net/ipv4/tcp_tw_reuse                               # Expected: 1
cat /sys/devices/system/cpu/cpu${CLIENT_CPU}/cpufreq/scaling_governor # Expected: performance
```

---

## Phase 8: Build Binaries & Pass the Correctness Gate

1. **Build all server variants and `echo_loadgen`** on their respective machines:
   ```bash
   make clean && make all
   ```
2. **Verify Async-Signal-Safe `SIGUSR1` Handling**:
   - Confirm in `code/core/core.c` that the `SIGUSR1` handler only sets a `volatile sig_atomic_t` flag and does **not** invoke `fopen()`, `fprintf()`, or `fflush()` inside signal context. The JSON stats dump must occur inside the main event loop iteration.
3. **Run the Echo Correctness Gate on Laptop 1**:
   ```bash
   python3 code/tests/echo_gate.py
   ```
   All tests (`T1` through `T14` across `select`, `poll`, `epoll`, and `io_uring`) must pass before collecting benchmark numbers.

---

## Phase 9: Executing a Benchmark Run & Verify Live Process State

### 1. Understanding the `select()` Descriptor Ceiling (`FD_SETSIZE = 1024`)
`select()` checks file descriptor **numbers** (`fd < 1024`), not connection count. Because `0` (`stdin`), `1` (`stdout`), `2` (`stderr`), the listening socket, and any open stats file consume ~4–6 descriptors:
- Use `--connections 1000` (or `950` if needed) as the maximum common point across all 4 backends (`select`, `poll`, `epoll`, `io_uring`).
- Run `5000` and `10000` connection tests on `poll`, `epoll`, and `io_uring`, documenting `select` as unsupported above `FD_SETSIZE`.

### 2. Start the Server Process Inside the `bench` cgroup (Laptop 1)
Launch a fresh server process for every run, pinned to the single physical core `$SERVER_CPU`:

```bash
rm -f server_stats.jsonl
sudo sh -c "echo \$\$ > /sys/fs/cgroup/bench/cgroup.procs && \
  prlimit --pid \$\$ --nofile=65536:65536 --memlock=unlimited:unlimited && \
  exec ./build/echo-epoll --port 8080 --msg-size 512 --max-conns 10240 --cpu ${SERVER_CPU} --stats-file server_stats.jsonl"
```

### 3. Verify Live Server Process Limits & Affinity (`/proc/<PID>/limits`)
Open a second terminal on Laptop 1 (or background check) and verify the **actual running process**:
```bash
PID=$(pidof echo-epoll)

# 1. Verify process is inside the bench cgroup
cat /proc/$PID/cgroup                        # Must show: 0::/bench

# 2. Verify CPU affinity is locked to the single physical core
taskset -cp $PID                             # Must show: current affinity list: <SERVER_CPU>

# 3. Verify open files (65536) and locked memory (unlimited) on the live PID
grep -E "Max open files|Max locked memory" /proc/$PID/limits
```

### 4. Local Server-Side Monotonic Snapshot Script (No SSH Over Test Wire)
To avoid injecting SSH packets onto the test interface during measurement, use this lightweight local helper on **Laptop 1** (pinned to housekeeping core `0`). It waits for connections to establish, uses `CLOCK_MONOTONIC` (`python3` monotonic clock) to capture exact snapshots at $t = 10.000\text{ s}$ (end of warmup) and $t = 40.000\text{ s}$ (end of 30s measurement), and records actual timing deviations, CPU frequency, and temperature:

```bash
cat << 'EOF' > /tmp/server_snapshot.py
#!/usr/bin/env python3
import os, sys, time, signal, shutil

if len(sys.argv) < 3:
    print("Usage: server_snapshot.py <server_pid> <out_dir> [warmup_s=10] [measure_s=30]")
    sys.exit(1)

pid = int(sys.argv[1])
out_dir = sys.argv[2]
warmup_s = float(sys.argv[3]) if len(sys.argv) > 3 else 10.0
measure_s = float(sys.argv[4]) if len(sys.argv) > 4 else 30.0
os.makedirs(out_dir, exist_ok=True)

def snap(tag, target_elapsed, t0):
    now = time.monotonic()
    actual_elapsed = now - t0
    deviation_us = (actual_elapsed - target_elapsed) * 1e6
    os.kill(pid, signal.SIGUSR1)
    shutil.copyfile("/sys/fs/cgroup/bench/cpu.stat", f"{out_dir}/cpu_stat_{tag}.txt")
    with open("/proc/stat", "r") as f_in, open(f"{out_dir}/proc_stat_{tag}.txt", "w") as f_out:
        for line in f_in:
            if line.startswith("cpu"):
                f_out.write(line)
    with open(f"{out_dir}/meta_{tag}.txt", "w") as f_meta:
        f_meta.write(f"monotonic_ns={time.monotonic_ns()}\n")
        f_meta.write(f"target_elapsed_s={target_elapsed:.6f}\n")
        f_meta.write(f"actual_elapsed_s={actual_elapsed:.6f}\n")
        f_meta.write(f"deviation_us={deviation_us:.2f}\n")
        try:
            with open("/proc/self/status") as _: pass
            with open(f"/proc/{pid}/status") as f_st:
                for l in f_st:
                    if l.startswith(("VmHWM:", "VmRSS:")):
                        f_meta.write(l)
        except Exception:
            pass

t0 = time.monotonic()
# Sleep until t = warmup_s (10.0s)
rem1 = warmup_s - (time.monotonic() - t0)
if rem1 > 0:
    time.sleep(rem1)
snap("t10_warmup_end", warmup_s, t0)

# Sleep until t = warmup_s + measure_s (40.0s)
target2 = warmup_s + measure_s
rem2 = target2 - (time.monotonic() - t0)
if rem2 > 0:
    time.sleep(rem2)
snap("t40_measure_end", target2, t0)
print(f"[Snapshot] Completed t10 and t40 snapshots in {out_dir}")
EOF
chmod +x /tmp/server_snapshot.py

# This script is REQUIRED. Call it twice per repetition (see experiment_run_workflow.md Steps 5 and 7):
#   Before the client run  (measurement-start snapshot):
#     /tmp/server_snapshot.py "$SERVER_PID" "$RUN_DIR/snapshots" "$WARMUP_S" "$DURATION_S"
#   After the client run   (measurement-end snapshot):
#     /tmp/server_snapshot.py "$SERVER_PID" "$RUN_DIR/snapshots" "$WARMUP_S" "$DURATION_S"
# Output files land in $RUN_DIR/snapshots/ (cpu_stat_*.txt, proc_stat_*.txt, meta_*.txt).
```

### 5. Start the Single-Threaded Client Pinned to ONE Core (Laptop 2)
On **Laptop 2 (Client)**, pin `echo_loadgen` to the single dedicated physical core `$CLIENT_CPU` (never a multi-core range like `1-2`):

```bash
taskset -c ${CLIENT_CPU} ./build/echo_loadgen \
    --host 192.168.50.1 --port 8080 \
    --connections 1000 --rate 20000 \
    --active-share 1.0 --msg-size 512 \
    --warmup 10 --duration 30 --cooldown 10 \
    --output results_epoll_1000c.csv
```
*(Verify on Laptop 2 during the first run that `grep -E "Max open files" /proc/$(pidof echo_loadgen)/limits` shows `65536`.)*

---

## Phase 10: Thermal, Frequency & Validity Checks Between Runs

After every 30-second measured run (during the 10-second cooldown), verify that the run is valid before accepting the data:

1. **Check CPU Frequency & Thermal Stability (Laptop 1 & Laptop 2)**:
   ```bash
   cat /sys/devices/system/cpu/cpu${SERVER_CPU}/cpufreq/scaling_cur_freq
   paste <(cat /sys/class/thermal/thermal_zone*/type) <(cat /sys/class/thermal/thermal_zone*/temp)
   ```
   If CPU frequency dropped by more than 5% between `t10` and `t40` due to thermal throttling, let the laptop cool down and repeat the run.
2. **Check Server & Client Error Counters**:
   - Server `errors` in `server_stats.jsonl` must be `0`.
   - For `io_uring`, CQ overflow must be `0`.
   - Client `conn_failures`, `send_errors`, `recv_errors`, `timeouts`, and `mismatched` in the CSV must all be `0`.
3. **Check Offered Rate vs. Achieved Rate**:
   - Compare `target_rate`, `actual_send_rate`, `echoed_rate`, and `concurrency_stalls` in the client output. Never claim a target rate was achieved if `concurrency_stalls` prevented the generator from reaching it.
4. **Separate `perf` Syscall Profiling from Primary Latency Runs**:
   - Never run `perf trace`, `strace`, or `perf record` during primary latency/throughput runs. Execute short diagnostic runs separately when collecting syscall breakdowns.