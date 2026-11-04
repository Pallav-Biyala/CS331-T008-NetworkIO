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
| **Manages** | Server binary + snapshot | `run_experiment.sh` |

---

## One-Time Prep (Do Once Per Session)

### On Laptop 2 — Copy the shell script and load generator
```bash
cd ~/Downloads/CS331-T008-NetworkIO

# Copy the shell script (rename for clarity)
cp run_experiment.sh ~/run_experiment.sh   # already there after pull

# The load generator binary was built in Phase 8 of the setup guide
# Make sure it is present on Laptop 2:
ls echo_loadgen     # should exist; if not: scp from Laptop 1
```

### On Laptop 1 — Save the snapshot script
```bash
cat << 'EOF' > /tmp/server_snapshot.py
#!/usr/bin/env python3
import os, sys, time, signal, shutil
pid = int(sys.argv[1]); out_dir = sys.argv[2]
warmup_s = float(sys.argv[3]) if len(sys.argv) > 3 else 10.0
measure_s = float(sys.argv[4]) if len(sys.argv) > 4 else 30.0
os.makedirs(out_dir, exist_ok=True)
def snap(tag, target_elapsed, t0):
    now = time.monotonic(); actual = now - t0
    deviation_us = (actual - target_elapsed) * 1e6
    os.kill(pid, signal.SIGUSR1)
    shutil.copyfile("/sys/fs/cgroup/bench/cpu.stat", f"{out_dir}/cpu_stat_{tag}.txt")
    with open("/proc/stat") as fi, open(f"{out_dir}/proc_stat_{tag}.txt","w") as fo:
        [fo.write(l) for l in fi if l.startswith("cpu")]
    with open(f"{out_dir}/meta_{tag}.txt","w") as m:
        m.write(f"monotonic_ns={time.monotonic_ns()}\ntarget_elapsed_s={target_elapsed:.6f}\n"
                f"actual_elapsed_s={actual:.6f}\ndeviation_us={deviation_us:.2f}\n")
        try:
            with open(f"/proc/{pid}/status") as s:
                [m.write(l) for l in s if l.startswith(("VmHWM:","VmRSS:"))]
        except Exception: pass
t0 = time.monotonic()
rem = warmup_s - (time.monotonic() - t0)
if rem > 0: time.sleep(rem)
snap("t10_warmup_end", warmup_s, t0)
rem = warmup_s + measure_s - (time.monotonic() - t0)
if rem > 0: time.sleep(rem)
snap("t40_measure_end", warmup_s + measure_s, t0)
print(f"[Snapshot] done → {out_dir}")
EOF
chmod +x /tmp/server_snapshot.py
```

### On Laptop 1 — Verify CPU variables are set in your terminal
```bash
echo $SERVER_CPU      # e.g. 2  — your chosen P-core
echo $IRQ_CPU         # e.g. 4  — different physical core for NIC IRQs
echo $NIC             # e.g. enp3s0
```
If empty, re-run Phase 4 exports from the setup guide.

---

## Per-Mechanism Run Loop

> Repeat the entire block below **4 times** — once per mechanism:  
> `select` → `poll` → `epoll` → `io_uring`  
> (or in any order; vary the order between repetitions to cancel thermal bias)

---

### STEP 1 — Laptop 1: Create results directory for this mechanism
```bash
export MECH=epoll     # change to: select | poll | epoll | io_uring
export RUN_DIR=~/Downloads/CS331-T008-NetworkIO/results/${MECH}_$(date +%Y%m%d_%H%M%S)
mkdir -p $RUN_DIR
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

### STEP 4 — Laptop 1: Start snapshot script (second terminal, pinned to core 0)
```bash
# Open a NEW terminal on Laptop 1 for this:
taskset -c 0 python3 /tmp/server_snapshot.py \
    $(pgrep echo-${MECH}) \
    $RUN_DIR \
    10 30 &
SNAP_PID=$!
echo "Snapshot PID: $SNAP_PID"
```
> This fires SIGUSR1 at exactly t=10s and t=40s, capturing CPU stats.  
> **Do not kill it — let it finish on its own.**

### STEP 5 — Laptop 2: Run the pilot first (quick 15s sanity check)
```bash
# On Laptop 2:
cd ~/Downloads/CS331-T008-NetworkIO
HOST=192.168.50.1 \
MSG_SIZE=512 \
WARMUP_S=5 \
DURATION_S=10 \
COOLDOWN_S=2 \
./run_experiment.sh --pilot
```
> Check output: no connection failures, delivery_ratio_pct ≈ 100%.  
> If pilot fails — fix connectivity before running the full suite.

### STEP 6 — Laptop 2: Run the full benchmark suite
```bash
# On Laptop 2 — this runs all 5 suites with 4 rotated repetitions:
HOST=192.168.50.1 \
MSG_SIZE=512 \
WARMUP_S=10 \
DURATION_S=30 \
COOLDOWN_S=10 \
REPS=4 \
OUT_DIR=results_${MECH}_$(date +%Y%m%d_%H%M%S) \
./run_experiment.sh --full
```
> Total wall time ≈ 4 reps × ~25 cells × 50s = **~85 minutes per mechanism**.  
> The script prints progress for each run. Let it finish uninterrupted.

### STEP 7 — Laptop 1: Validity checks after each run (during cooldown gaps)
Between suites, spot-check on Laptop 1:
```bash
# CPU frequency must not have dropped >5%
cat /sys/devices/system/cpu/cpu${SERVER_CPU}/cpufreq/scaling_cur_freq

# Temperature must be stable (not throttling)
paste <(cat /sys/class/thermal/thermal_zone*/type) \
      <(cat /sys/class/thermal/thermal_zone*/temp)

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
wait $SNAP_PID    # wait for snapshot script to finish cleanly
echo "Done. Results in: $RUN_DIR"
```

### STEP 9 — Laptop 1: Collect client CSV from Laptop 2
```bash
# Copy results from Laptop 2 into the same run directory
scp kowel2@192.168.50.2:~/Downloads/CS331-T008-NetworkIO/results_${MECH}_*/*.csv \
    $RUN_DIR/
```

### STEP 10 — Verify the run is valid before accepting it
```bash
# Check client CSV — these columns must all be 0:
# conn_failures, send_errors, recv_errors, timeouts, mismatched
head -2 $RUN_DIR/*.csv    # view header + first data row

# Check snapshot files were written
ls $RUN_DIR/
# Must see:
#   server_stats.jsonl
#   cpu_stat_t10_warmup_end.txt
#   cpu_stat_t40_measure_end.txt
#   proc_stat_t10_warmup_end.txt
#   proc_stat_t40_measure_end.txt
#   meta_t10_warmup_end.txt
#   meta_t40_measure_end.txt
```
> If snapshot files are missing — the snapshot script didn't run. Discard and repeat.  
> If client errors > 0 — discard and repeat after a cooldown.

---

## After All 4 Mechanisms Are Done

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
| `server_stats.jsonl` | `results/<mech>_<ts>/` | Server (`--stats-file`) |
| `cpu_stat_t10_*.txt` | `results/<mech>_<ts>/` | `server_snapshot.py` |
| `cpu_stat_t40_*.txt` | `results/<mech>_<ts>/` | `server_snapshot.py` |
| `meta_t*.txt` (timing deviation, RSS) | `results/<mech>_<ts>/` | `server_snapshot.py` |
| `benchmark_all_runs.csv` | `results_<mech>_<ts>/` on Laptop 2 | `run_experiment.sh` |
| `json/<suite>_<backend>_rep*.json` | `results_<mech>_<ts>/json/` on Laptop 2 | `run_experiment.sh` |

---

## Mechanism Order Across Repetitions (Eliminate Thermal Bias)

Do not always run in the same order. Suggested ordering across the 4 full runs:

| Full Run | Order |
|----------|-------|
| Run 1 | select → poll → epoll → io_uring |
| Run 2 | epoll → io_uring → poll → select |
| Run 3 | io_uring → select → epoll → poll |
| Run 4 | poll → epoll → select → io_uring |

> The shell script already rotates mechanism order within each repetition (Cyclic Latin-Square). This table controls the **per-mechanism full-suite** ordering across your manual restarts.
