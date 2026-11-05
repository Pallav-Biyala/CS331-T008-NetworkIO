# Experiment Run Workflow (Offline)

> **Prerequisite**: both laptops have completed `linux_network_i_o_benchmark_setup_guide.md`
> **Phases 0–8**. Phase 0 is the only part that needs internet. Do it first, then disconnect.

---

## Machines

| | Laptop 1 | Laptop 2 |
|--|----------|----------|
| **Role** | Server | Client / load generator |
| **IP** | `192.168.50.1` | `192.168.50.2` |
| **Runs** | `build/echo-<mech>` (started by hand) | `./run_experiment.sh` |
| **Repo** | `~/CS331-T008-NetworkIO` | `~/CS331-T008-NetworkIO` |

## Offline rules

- **No internet anywhere during runs.** Nothing in the run path downloads anything: binaries are prebuilt, and Python plotting libraries come from apt (Phase 0).
- **Wi-Fi and Bluetooth stay blocked.** The only network is the direct Ethernet cable.
- **Clocks are not synced (no NTP).** That's fine. All latency is measured on Laptop 2 with `CLOCK_MONOTONIC`, and wall-clock time only labels directories.
- **Files move by USB.** `git push` waits until a machine is back online (see the end of this document).

---

## Time budget and backend order

| Backend | Cases per repetition | Approx. time |
|---|---|---|
| `select` | 41 (stops at 900 connections, FD_SETSIZE) | ~19 min |
| `poll`, `epoll`, `uring` | 66 each | ~31 min each |
| **One full repetition (all 4)** | 239 | **~1 h 50 min + ~5 min per server switch ≈ 2 h 10 min** |

Defaults: 512 B messages, warmup 5 s, measurement 20 s. Check any time with
`DRY_RUN=1 ./run_experiment.sh --full --mechanism epoll`.

**Priority:** finish **Rep 1 for all four backends first**. That alone produces all 8 graphs.
Add Reps 2 and 3 as time allows; each extra rep adds error bars. Rotate the order every rep:

| Rep | Order |
|-----|-------|
| 1 | select → poll → epoll → uring |
| 2 | poll → epoll → uring → select |
| 3 | epoll → uring → select → poll |
| 4 | uring → select → poll → epoll |

---

## Session start (both laptops, every boot)

Kernel tuning from `setup_*.sh` and IRQ affinity do **not** survive a reboot. After every boot, re-run setup guide
Phases 2, 3, 6 (Laptop 1) and 7 (Laptop 2), then:

**Laptop 1**
```bash
cd ~/CS331-T008-NetworkIO
sudo prlimit --pid $$ --nofile=65536:65536 --memlock=unlimited:unlimited
export NIC=enp3s0 SERVER_CPU=2 IRQ_CPU=4      # your values from setup guide Phase 4
ls build/echo-select build/echo-poll build/echo-epoll build/echo-uring
```

**Laptop 2**
```bash
cd ~/CS331-T008-NetworkIO
sudo prlimit --pid $$ --nofile=65536:65536
export CLIENT_CPU=2                            # your value from setup guide Phase 4
ulimit -Hn                                     # must be >= 21000 (10K connections)
./run_experiment.sh --self-test                # must end with: SELF-TEST PASSED
ping -c 3 192.168.50.1
```

---

## Per-backend loop (repeat for each backend in the order table)

### STEP 1 — Laptop 1: choose backend/rep and record system state
```bash
export MECH=epoll     # select | poll | epoll | uring
export REP=1
export RUN_DIR=~/CS331-T008-NetworkIO/server_runs/${MECH}_rep${REP}_$(date +%Y%m%d_%H%M%S)
mkdir -p "$RUN_DIR"
{ date --iso-8601=seconds; uname -a; lscpu; ip -brief address show dev "$NIC"; ethtool "$NIC" 2>/dev/null; } \
  > "$RUN_DIR/system_info.txt"
```

### STEP 2 — Laptop 1: start the server inside the bench cgroup
```bash
sudo -v      # cache the sudo password now; a backgrounded sudo cannot prompt and would hang
sudo sh -c "echo \$\$ > /sys/fs/cgroup/bench/cgroup.procs; \
  prlimit --pid \$\$ --nofile=65536:65536 --memlock=unlimited:unlimited && \
  exec $HOME/CS331-T008-NetworkIO/build/echo-${MECH} \
    --port 8080 --msg-size 512 --max-conns 10240 --cpu ${SERVER_CPU} \
    --stats-file ${RUN_DIR}/server_stats.jsonl" > "$RUN_DIR/server.log" 2>&1 &
sleep 2
```
Use `$HOME`, not `~`, in this command: `~` inside the quotes would expand to `/root` under `sudo`, giving `Exit 127`.
If you see `[1]+ Exit ...`, the server did not start; run `cat "$RUN_DIR/server.log"` to see why.

### STEP 3 — Laptop 1: sanity-check the live process
```bash
PID=$(pgrep -n -x echo-${MECH}); echo "PID=$PID"
cat /proc/$PID/cgroup                     # must show: 0::/bench
taskset -cp $PID                          # must show only: $SERVER_CPU
grep -E "Max open files|Max locked memory" /proc/$PID/limits   # 65536 / unlimited
```
If any check is wrong: `sudo pkill -x echo-${MECH}`, then repeat STEP 2.

### STEP 4 — Laptop 2: pilot (Rep 1 only; skip in later reps)
```bash
export MECH=epoll REP=1                   # same values as Laptop 1 (shells don't share variables)
OUT_DIR=results/pilot_${MECH} ./run_experiment.sh --pilot --mechanism "$MECH"
```
The pilot runs about 30 s. Pass criteria:
- each case prints `Connection barrier complete: N / N established`
- for poll/epoll/uring, **`10000 / 10000`** on the last case. Fewer means the fd limit, `somaxconn` or port range is wrong; fix it before continuing.
- `Delivery Ratio` ≈ 100% on the 5K and 25K cases; `Conn / Send / Recv / Bad: 0 / 0 / 0 / 0`.

Pilot rows are tagged `PILOT` and ignored by the plotter.

### STEP 5 — Laptop 1: baseline counters
```bash
sudo kill -USR1 "$PID"; sleep 1           # appends a cumulative JSON snapshot to server_stats.jsonl
cat /sys/fs/cgroup/bench/cpu.stat > "$RUN_DIR/cpu_stat_before.txt"
cat /sys/devices/system/cpu/cpu${SERVER_CPU}/cpufreq/scaling_cur_freq > "$RUN_DIR/cpu_freq_before.txt"
paste <(cat /sys/class/thermal/thermal_zone*/type) <(cat /sys/class/thermal/thermal_zone*/temp) \
  > "$RUN_DIR/thermal_before.txt"
```

### STEP 6 — Laptop 2: full suite for this backend and repetition
```bash
./run_experiment.sh --full --mechanism "$MECH" --rep "$REP"
```
- Output goes to `results/${MECH}_rep${REP}_<timestamp>/` (`results.csv`, `json/`, `manifest.txt`, `commands.log`, `run.log`).
- **Don't touch either laptop** while it runs (~31 min, or ~19 min for select): no browsers, no file managers, no typing.
- A case that exits non-zero is logged to `failed_cases.txt` and the suite **continues**; it does not abort.

### STEP 7 — Laptop 1: end counters and validity
```bash
sudo kill -USR1 "$PID"; sleep 1
cat /sys/fs/cgroup/bench/cpu.stat > "$RUN_DIR/cpu_stat_after.txt"
cat /sys/devices/system/cpu/cpu${SERVER_CPU}/cpufreq/scaling_cur_freq > "$RUN_DIR/cpu_freq_after.txt"
paste <(cat /sys/class/thermal/thermal_zone*/type) <(cat /sys/class/thermal/thermal_zone*/temp) \
  > "$RUN_DIR/thermal_after.txt"
paste "$RUN_DIR/cpu_freq_before.txt" "$RUN_DIR/cpu_freq_after.txt"   # must not drop > 5%
tail -1 "$RUN_DIR/server_stats.jsonl" | python3 -c \
  "import json,sys; s=json.load(sys.stdin).get('stats',{}); print('errors', s.get('errors'), 'ring_overflows', s.get('ring_overflows'))"
```
If the CPU throttled (frequency dropped more than 5%, or temperatures near the limit), let the laptop cool for 5 minutes before the next backend.

### STEP 8 — Laptop 1: stop the server
```bash
sudo pkill -INT -x echo-${MECH}; sleep 2
pgrep -x echo-${MECH} || echo "stopped"    # must print: stopped
```
`sudo` is required because the server was started via `sudo sh`; a plain `kill` cannot signal it.

### STEP 9 — Laptop 2: quick client check
```bash
D=$(ls -d results/${MECH}_rep${REP}_* | tail -1); echo "$D"
cat "$D/failed_cases.txt" 2>/dev/null || echo "no failed cases"
awk -F, 'NR==1{for(i=1;i<=NF;i++)h[$i]=i; next}
  {e=$h["conn_failures"]+$h["send_errors"]+$h["recv_errors"]+$h["timeouts"]+$h["mismatched"];
   if(e>0 || $h["delivery_ratio_pct"]<95) printf "%-45s deliv=%6.1f%% errs=%d cpu=%s%%\n",$h["tag"],$h["delivery_ratio_pct"],e,$h["client_cpu_pct"]}' "$D/results.csv"
```
How to read it:
- **Low delivery at high load** (`r35000`, `r50000`, `S2_RATE_HIGHCONN`, `burst10x`) **is expected**; that's the saturation point the graphs show.
- **Errors or low delivery at light load** (≤ 10K msgs/s, ≤ 900 connections) mean something is wrong (link, limits, server crash). Check `server.log`, fix the cause, then re-run the whole backend for this rep. The plotter keeps the newest copy of each case.
- `client_cpu_pct ≥ 90` means that point measured the load generator, not the server. It will be flagged in `plots/validity_report.csv`; mention it in the report rather than re-running.

Then go to the next backend in the order table (back to STEP 1).

---

## After the runs: collect, plot, archive (all offline)

### 1. Move client results to the plotting machine by USB
```bash
# Laptop 2
USB=/media/$USER/<USB_LABEL>
mkdir -p "$USB/CS331"
cp -a ~/CS331-T008-NetworkIO/results "$USB/CS331/"
# Laptop 1 (server-side counters; optional for graphs)
cp -a ~/CS331-T008-NetworkIO/server_runs "$USB/CS331/"
sync
```

### 2. Generate the 8 graphs
On Laptop 2 (or any machine with `python3-pandas` and `python3-matplotlib`, Linux or Windows):
```bash
cd ~/CS331-T008-NetworkIO
python3 plot_results.py results plots
ls plots/          # 01_..08_*.png, summary.csv, validity_report.csv
```
The plotter searches recursively, so pointing it at the USB copy also works: `python3 plot_results.py $USB/CS331/results plots`.

### 3. Archive with git (offline), push later
```bash
git add results plots server_runs
git commit -m "Benchmark results: <N> reps x 4 backends"
git bundle create "$USB/CS331/results.bundle" loadgen-bench    # backup on USB
# later, on any machine with internet:
git push origin loadgen-bench
```

---

## Quick reference: what goes where

| File | Location | Created by |
|------|----------|-----------|
| `results.csv`, `json/*.json`, `manifest.txt`, `commands.log`, `run.log`, `failed_cases.txt` | Laptop 2 `results/<mech>_rep<n>_<ts>/` | `run_experiment.sh` |
| pilot output | Laptop 2 `results/pilot_<mech>/` | `run_experiment.sh --pilot` |
| `server_stats.jsonl`, `server.log`, `system_info.txt`, `cpu_stat_*`, `cpu_freq_*`, `thermal_*` | Laptop 1 `server_runs/<mech>_rep<n>_<ts>/` | server + operator (Steps 1–7) |
| `01_*.png` … `08_*.png`, `summary.csv`, `validity_report.csv` | `plots/` | `plot_results.py` |
