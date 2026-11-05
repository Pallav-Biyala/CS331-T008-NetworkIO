# Experiment Run Workflow

> **Prerequisite**: Both laptops have completed  
> `linux_network_i_o_benchmark_setup_guide.md` **Phases 1–8**  
> (system tuning + build + correctness gate all passed).

---

## Machines

| | Laptop 1 | Laptop 2 |
|--|----------|----------|
| **Role** | Server | Client / Load Generator |
| **IP** | `192.168.50.1` | `192.168.50.2` |
| **Manages** | Server binary + server-side counters | Client runner and client result files |

---

## One-Time Prep (Do Once Per Session)

### On Laptop 2 — Prepare the client runner
```bash
cd ~/Downloads/CS331-T008-NetworkIO
# Run the script from the repository so it finds echo_loadgen beside it.
ls echo_loadgen     # should exist; if not, run: make loadgen
chmod +x run_experiment.sh
```

### On Laptop 1 — Verify CPU variables are set in your terminal
```bash
echo $SERVER_CPU      # e.g. 2  — your chosen P-core
echo $IRQ_CPU         # e.g. 4  — different physical core for NIC IRQs
echo $NIC             # e.g. enp3s0
```
If empty, re-run Phase 4 exports from the setup guide.

### On Laptop 2 — Verify the load-generator CPU is set
```bash
echo "$CLIENT_CPU"       # dedicated client core selected in setup guide Phase 7
taskset -pc $$            # shell affinity for reference
```
The runner pins every load-generator invocation to `$CLIENT_CPU` and records it in the manifest.

---

## Per-Mechanism Run Loop (Manual Server, No SSH)

The client runner is client-only: it never starts, stops, or signals the server. First run one pilot per backend. Then do four measured repetitions, starting one backend at a time on Laptop 1 and running that backend's client suite on Laptop 2. Follow the rotated order table near the end of this document. For each repetition, use a fresh server process and a new results directory.

The pilot is a smoke check and is excluded from measured results. Run it once per backend before the measured repetitions; when repeating the steps below, skip Step 4 for later repetitions.

---

### STEP 1 — Laptop 1: Create results directory for this mechanism
```bash
export MECH=epoll     # use: select | poll | epoll | uring (binary suffix for io_uring)
export REP=1          # use 1, 2, 3, or 4
export RUN_DIR=~/Downloads/CS331-T008-NetworkIO/results/${MECH}_rep${REP}_$(date +%Y%m%d_%H%M%S)
mkdir -p $RUN_DIR
{
  date --iso-8601=seconds
  hostnamectl
  uname -a
  lscpu
  ip -brief address show dev "$NIC"
  ethtool "$NIC" 2>/dev/null || true
} > "$RUN_DIR/system_info.txt"
echo "Results will go to: $RUN_DIR"
```

### STEP 2 — Laptop 1: Start the server inside the bench cgroup
```bash
rm -f $RUN_DIR/server_stats.jsonl

sudo sh -c "echo \$\$ > /sys/fs/cgroup/bench/cgroup.procs && \
  prlimit --pid \$\$ --nofile=65536:65536 --memlock=unlimited:unlimited && \
  exec ~/Downloads/CS331-T008-NetworkIO/build/echo-${MECH} \
    --port 8080 \
    --msg-size 512 \
    --max-conns 10240 \
    --cpu ${SERVER_CPU} \
    --stats-file ${RUN_DIR}/server_stats.jsonl" &

SERVER_PID=$!
sleep 2    # wait for server to bind to port
echo "Server PID: $SERVER_PID"
```

### STEP 3 — Laptop 1: Quick sanity check on the live process
```bash
PID=$(pgrep echo-${MECH})
cat /proc/$PID/cgroup                            # must show: 0::/bench
taskset -cp $PID                                 # must show only: $SERVER_CPU
grep -E "Max open files|Max locked memory" /proc/$PID/limits
```
> If cgroup or affinity is wrong — kill and restart from STEP 2.

### STEP 4 — Laptop 2: Run the pilot first (quick 15s sanity check)
```bash
# On Laptop 2:
cd ~/Downloads/CS331-T008-NetworkIO
export MECH=epoll     # set this on the client too; shells do not share variables between laptops
HOST=192.168.50.1 \
CLIENT_CPU="$CLIENT_CPU" \
MSG_SIZE=512 \
WARMUP_S=5 \
DURATION_S=10 \
COOLDOWN_S=2 \
OUT_DIR=results/pilot_${MECH}_$(date +%Y%m%d_%H%M%S) \
./run_experiment.sh --pilot --mechanism "$MECH"
```
> Check output: no connection failures, delivery_ratio_pct ≈ 100%.  
> If pilot fails — fix connectivity before running the full suite.
After the pilot, stop the pilot server. Start a fresh process with an empty `server_stats.jsonl` by repeating Step 2 (including `rm -f`), then recheck PID, cgroup, and affinity. This keeps pilot traffic out of measured repetition counters. For repetitions 2–4, skip the pilot and use a fresh server and results directory directly.

### STEP 5 — Laptop 1: Record the server baseline for the measured repetition
The client suite has many workload cells, so the old fixed t=10s/t=40s helper does not align with them. These snapshots are repetition-level server counters, not per-cell server measurements.
```bash
PID=$(pgrep -n -x echo-${MECH})
kill -USR1 "$PID"                 # appends a cumulative JSON snapshot
sleep 1
# Run the snapshot helper: captures cgroup cpu.stat, /proc/stat,
# timing deviation and server memory at the measurement-start point.
/tmp/server_snapshot.py "$PID" "$RUN_DIR/snapshots" "${WARMUP_S:-10}" "${DURATION_S:-30}"
cat /sys/fs/cgroup/bench/cpu.stat > "$RUN_DIR/cpu_stat_before.txt"
cat /sys/devices/system/cpu/cpu${SERVER_CPU}/cpufreq/scaling_cur_freq > "$RUN_DIR/cpu_freq_before.txt"
paste <(cat /sys/class/thermal/thermal_zone*/type) \
      <(cat /sys/class/thermal/thermal_zone*/temp) > "$RUN_DIR/thermal_before.txt"
```

### STEP 6 — Laptop 2: Run this mechanism's full suite for one repetition
```bash
# On Laptop 2. Set the same MECH and REP as Laptop 1. Run once per backend
# in each repetition, following the balanced order table below.
export REP=1                         # use 1, 2, 3, or 4
export CLIENT_OUT=results/${MECH}_rep${REP}_$(date +%Y%m%d_%H%M%S)
HOST=192.168.50.1 \
CLIENT_CPU="$CLIENT_CPU" \
MSG_SIZE=512 \
WARMUP_S=10 \
DURATION_S=30 \
COOLDOWN_S=10 \
OUT_DIR="$CLIENT_OUT" \
./run_experiment.sh --full --mechanism "$MECH" --rep "$REP"
```
The runner writes one `results.csv`, a `manifest` text file, a command log, and one JSON time series per workload cell under the chosen output directory. It exits if it cannot connect to the manually started server. Record the printed `CLIENT_OUT` path on Laptop 2 for the copy step. Keep each mechanism/repetition output directory intact.

At the defaults, one backend/repetition runs about 20–21 workload cells and takes roughly 17–18 minutes, plus time to switch and verify servers. Four backends across four repetitions take about 4.5 hours, plus pilots and manual handling. Only compare rate points present for both backends; the runner omits select's 25,000 msgs/s cell.

### STEP 7 — Laptop 1: Record the repetition-end counters and check validity
After the full client suite finishes, on Laptop 1:
```bash
PID=$(pgrep -n -x echo-${MECH})
kill -USR1 "$PID"
sleep 1
# Run the snapshot helper again: captures cgroup cpu.stat, /proc/stat,
# timing deviation and server memory at the measurement-end point.
/tmp/server_snapshot.py "$PID" "$RUN_DIR/snapshots" "${WARMUP_S:-10}" "${DURATION_S:-30}"
cat /sys/fs/cgroup/bench/cpu.stat > "$RUN_DIR/cpu_stat_after.txt"
cat /sys/devices/system/cpu/cpu${SERVER_CPU}/cpufreq/scaling_cur_freq > "$RUN_DIR/cpu_freq_after.txt"
paste <(cat /sys/class/thermal/thermal_zone*/type) \
      <(cat /sys/class/thermal/thermal_zone*/temp) > "$RUN_DIR/thermal_after.txt"
# CPU frequency must not have dropped >5%
cat "$RUN_DIR/cpu_freq_after.txt"

# Temperature must be stable (not throttling)
cat "$RUN_DIR/thermal_after.txt"

# Server errors must be 0
python3 -c "
import json
recs = [json.loads(l) for l in open('${RUN_DIR}/server_stats.jsonl') if l.strip()]
if recs:
    s = recs[-1].get('stats', {})
    print('errors:', s.get('errors', 0))
    print('ring_overflows:', s.get('ring_overflows', 0))
"
```
> If temperature is too high — pause, let it cool, then continue.

### STEP 8 — Laptop 1: Stop the server after Laptop 2 finishes
```bash
kill $SERVER_PID
echo "Done. Results in: $RUN_DIR"
```

### STEP 9 — Transfer client results by USB (no SSH)
```bash
# On Laptop 2, mount a USB drive in the file manager and set its mount path:
USB_MOUNT="/media/$USER/USB_LABEL"   # replace USB_LABEL with the mounted drive name
mkdir -p "$USB_MOUNT/CS331-T008-NetworkIO"
cp -a "$CLIENT_OUT" "$USB_MOUNT/CS331-T008-NetworkIO/"
sync

# Move the USB drive to Laptop 1, mount it, and copy the exact run directory:
USB_MOUNT="/media/$USER/USB_LABEL"   # set Laptop 1's mount path
CLIENT_DIR_NAME="${MECH}_rep${REP}_YYYYMMDD_HHMMSS"  # exact name printed on Laptop 2
cp -a "$USB_MOUNT/CS331-T008-NetworkIO/$CLIENT_DIR_NAME" \
    "$RUN_DIR/client_${MECH}_rep${REP}"
```

### STEP 10 — Verify the run is valid before accepting it
```bash
# Check client CSV — these columns must all be 0:
# conn_failures, send_errors, recv_errors, timeouts, mismatched
head -2 "$RUN_DIR"/client_${MECH}_rep${REP}/results.csv

# Check repetition-level server snapshots and copied client artifacts
ls $RUN_DIR/
# Must see:
#   server_stats.jsonl
#   cpu_stat_before.txt
#   cpu_stat_after.txt
#   cpu_freq_before.txt
#   cpu_freq_after.txt
#   thermal_before.txt
#   thermal_after.txt
#   system_info.txt
#   client_${MECH}_rep${REP}/results.csv
#   client_${MECH}_rep${REP}/manifest.txt
```
> If either server stats snapshot is missing — repeat the manual SIGUSR1 capture and wait for the JSONL file to update.
> If client errors > 0 — discard and repeat after a cooldown.

---

## After All 4 Repetitions Are Done

### Collect all results into one place
```bash
cd ~/Downloads/CS331-T008-NetworkIO
ls results/    # should have one directory per mechanism run
```

### Commit to the branch
```bash
git add results/
git commit -m "Add benchmark results: all 4 mechanisms, 4 reps each"
git push origin loadgen-bench
```

---

## Quick Reference — What Goes Where

| File | Location | Created by |
|------|----------|-----------|
| `server_stats.jsonl` | `results/<mech>_rep<n>_<ts>/` | Server (`--stats-file`) |
| `system_info.txt`, `cpu_stat_*`, `cpu_freq_*`, `thermal_*` | `results/<mech>_rep<n>_<ts>/` | Laptop 1 operator |
| `snapshots/cpu_stat_*.txt`, `snapshots/proc_stat_*.txt`, `snapshots/meta_*.txt` | `results/<mech>_rep<n>_<ts>/snapshots/` | `server_snapshot.py` (Steps 5 & 7) |
| `results.csv` | Per-run output directory on Laptop 2 | `run_experiment.sh` |
| `manifest.txt` (key/value text) | Per-run output directory on Laptop 2 | `run_experiment.sh` |
| `commands.log` | Per-run output directory on Laptop 2 | `run_experiment.sh` |
| `run.log` | Per-run output directory on Laptop 2 | `run_experiment.sh` |
| `json/rep*_*.json` | Per-run output directory on Laptop 2 | `run_experiment.sh` |

---

## Mechanism Order Across Repetitions (Eliminate Thermal Bias)

Do not always run in the same order. Suggested ordering across the 4 full runs:

| Full Run | Order |
|----------|-------|
| Run 1 | select → poll → epoll → io_uring |
| Run 2 | epoll → io_uring → poll → select |
| Run 3 | io_uring → select → epoll → poll |
| Run 4 | poll → epoll → select → io_uring |

> Run one client suite per table cell. For `io_uring`, pass `--mechanism uring` and start `build/echo-uring`. Start the indicated backend on Laptop 1, run `--full --mechanism <name> --rep <n>` on Laptop 2, then stop the server before moving to the next cell. The runner deliberately does not automate or signal the server.
