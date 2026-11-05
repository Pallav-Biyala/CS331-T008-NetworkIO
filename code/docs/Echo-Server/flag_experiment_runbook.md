# Flag Experiment Runbook (Final Set)

The last set of runs before the report. It measures how server flags change throughput and latency compared with the Rep 1 baseline, and re-measures poll if its earlier data drifted.

- **Running time:** about 1 h 30 min for all sessions, leaving time for re-runs within a 3-hour slot.
- **Per case:** 10 s warm-up, 30 s measurement, up to 10 s drain (the original plan's values).
- **Baseline:** Rep 1 (already done) is the baseline. No baseline sessions are repeated. Rep 1 used a 5 s warm-up and 20 s measurement, so the report should state that the flag runs used 10 s and 30 s.
- **Tools:** `run_experiment.sh` is **not** used. Laptop 2 calls `./echo_loadgen` through the helper functions in section 1.
- **Offline:** nothing here needs internet.

---

## 0. Before you start

- Both laptops are set up (setup guide Phases 0–8), connected by the Ethernet cable, Wi-Fi off, on AC power.
- **If either laptop rebooted**, re-run setup guide Phases 2, 3, 6 (Laptop 1) and 7 (Laptop 2) first.
- Close everything else on both laptops: browsers, file managers, editors.
- Use **one terminal per laptop for the whole runbook.** Variables and helper functions exist only in that terminal. If you open a new one, repeat section 1 in it.

---

## 1. Terminal setup (once per terminal)

### Laptop 1 (server)
```bash
cd ~/CS331-T008-NetworkIO
export SERVER_CPU=<n>                 # your server core from setup guide Phase 4
sudo prlimit --pid $$ --nofile=65536:65536 --memlock=unlimited:unlimited
ulimit -n                             # must print 65536
ls build/echo-select build/echo-poll build/echo-epoll build/echo-uring   # all four must exist
sudo pkill -x echo-select; sudo pkill -x echo-poll; sudo pkill -x echo-epoll; sudo pkill -x echo-uring   # clear leftovers
health() {   # CPU frequency (kHz) and temperatures (millidegrees C)
  cat /sys/devices/system/cpu/cpu${SERVER_CPU}/cpufreq/scaling_cur_freq
  paste <(cat /sys/class/thermal/thermal_zone*/type) <(cat /sys/class/thermal/thermal_zone*/temp)
}
health                                # note the frequency; you will compare against it
```

### Laptop 2 (client)
```bash
cd ~/CS331-T008-NetworkIO
export CLIENT_CPU=<n>                 # your client core from setup guide Phase 4
sudo prlimit --pid $$ --nofile=65536:65536
ulimit -n                             # must print 65536
ls -l echo_loadgen                    # must exist; if not: make loadgen
ping -c 2 192.168.50.1                # must get replies
```

Then paste this whole block into the **Laptop 2** terminal:

```bash
WARMUP=10; DUR=30; COOL=10

# run TAG CONNS RATE DIST W MODE  -> one load-generator case
run() {
  local tag=$1 dir=results/variants/${1%%_rep*} extra=()
  mkdir -p "$dir/json"
  [[ $6 == open-loop ]] && extra=(--rate "$3")
  taskset -c "$CLIENT_CPU" ./echo_loadgen --host 192.168.50.1 --port 8080 \
    --connections "$2" --active-share 1.0 --pipeline-window "$5" --dist "$4" \
    --mode "$6" "${extra[@]}" --duration $DUR --warmup $WARMUP --cooldown $COOL \
    --msg-size 512 --seed 1001 --connect-timeout 20 --tag "$tag" \
    --output "$dir/results.csv" --json-output "$dir/json/$tag.json"
  sleep 2
}

# cases LABEL [nohigh]  -> the 9 standard cases (7 with "nohigh", for select)
cases() {
  local L=$1
  run ${L}_rep1_S2_RATE_c500_r1000          500   1000  constant 1 open-loop
  run ${L}_rep1_S2_RATE_c500_r25000         500   25000 constant 1 open-loop
  run ${L}_rep1_S2_RATE_c500_r50000         500   50000 constant 1 open-loop
  run ${L}_rep1_S5_DIST_c500_r15000_burst   500   15000 burst    1 open-loop
  run ${L}_rep1_S1_CONN_MAX_c500_closed     500   0     constant 1 closed-loop
  run ${L}_rep1_S4_PIPE_MAX_c500_w8_closed  500   0     constant 8 closed-loop
  if [[ $2 != nohigh ]]; then
    run ${L}_rep1_S1_CONN_c10000_r10000     10000 10000 constant 1 open-loop
    run ${L}_rep1_S1_CONN_MAX_c10000_closed 10000 0     constant 1 closed-loop
  fi
  run ${L}_rep1_S2_RATE_c500_r1000end       500   1000  constant 1 open-loop   # drift check
}

# pollscale  -> clean poll connection scaling, with a drift check at start and end
pollscale() {
  local L=pollscale
  run ${L}_rep1_S1_CONN_c500_r1000        500   1000  constant 1 open-loop
  run ${L}_rep1_S1_CONN_c900_r10000       900   10000 constant 1 open-loop
  run ${L}_rep1_S1_CONN_c2500_r10000      2500  10000 constant 1 open-loop
  run ${L}_rep1_S1_CONN_c5000_r10000      5000  10000 constant 1 open-loop
  run ${L}_rep1_S1_CONN_c10000_r10000     10000 10000 constant 1 open-loop
  run ${L}_rep1_S1_CONN_MAX_c2500_closed  2500  0     constant 1 closed-loop
  run ${L}_rep1_S1_CONN_MAX_c10000_closed 10000 0     constant 1 closed-loop
  run ${L}_rep1_S1_CONN_c500_r1000end     500   1000  constant 1 open-loop   # drift check
}
```
Check: `type run cases pollscale` should print all three functions.

### What the 9 standard cases measure

| Case | Setup | Why it's in the set |
|---|---|---|
| `S2_RATE_c500_r1000` | 500 conns, 1K msgs/s | Light load; where Nagle and wake-up costs show most |
| `S2_RATE_c500_r25000` | 500 conns, 25K msgs/s | Moderate load |
| `S2_RATE_c500_r50000` | 500 conns, 50K msgs/s | Heavy load, close to where queues build |
| `S5_DIST_c500_r15000_burst` | 500 conns, 15K avg, 4× bursts | Tail latency under bursts |
| `S1_CONN_MAX_c500_closed` | 500 conns, closed loop, 1 in flight | The ~131K ceiling from Rep 1 |
| `S4_PIPE_MAX_c500_w8_closed` | 500 conns, closed loop, 8 in flight | Pipelined ceiling (~175K in Rep 1) |
| `S1_CONN_c10000_r10000` | 10,000 conns, 10K msgs/s | Latency at scale (not for select) |
| `S1_CONN_MAX_c10000_closed` | 10,000 conns, closed loop | Ceiling at scale (not for select) |
| `S2_RATE_c500_r1000end` | Repeat of the first case | Drift check: should match the first case |

All cases use seed 1001, so arrival patterns are identical across sessions.

---

## 2. Check poll Rep 2 (Laptop 2, 10 seconds)

This decides whether you run session 9 (`pollscale`).

```bash
D=$(ls -d results/poll_rep2_* | tail -1); echo "$D"
grep -E 'S1_CONN_c500_r1000,|S2_RATE_c500_r1000,|S1_CONN_MAX_c500_closed|S4_PIPE_MAX_c500_w1_closed' "$D/results.csv" | cut -d, -f1,12,23
```
Columns: tag, throughput (msgs/s), p50 latency (µs).
- **Clean:** the two `c500_r1000` rows are both about 100 µs and both closed-loop rows are about 131K. Use poll Rep 2 as poll's baseline and **skip session 9**.
- **Drifted:** the `S2_RATE` row is much slower, or `S4_PIPE_MAX_c500_w1_closed` is far below 131K. **Run session 9** to get clean poll scaling numbers.

---

## 3. The sessions

Run them in this order. Every session is a flag variant; its baseline is the matching Rep 1 data.

| # | BACKEND | FLAGS | LABEL | Laptop 2 command | What it tests |
|---|---|---|---|---|---|
| 1 | epoll | `--epoll-oneshot` | epolloneshot | `cases epolloneshot` | One extra syscall per message (re-arm) |
| 2 | epoll | `--epoll-maxevents=1` | epollmaxev | `cases epollmaxev` | One ready connection per wake-up |
| 3 | epoll | `--epoll-et` | epollet | `cases epollet` | Edge-triggered epoll |
| 4 | epoll | `--nodelay=0` | epollnagle | `cases epollnagle` | Nagle's algorithm on |
| 5 | epoll | `--buf-size 512` | epollbuf | `cases epollbuf` | One frame per read |
| 6 | uring | `--uring-entries 256` | uringsmall | `cases uringsmall` | Small submission ring |
| 7 | uring | `--nodelay=0` | uringnagle | `cases uringnagle` | Nagle's algorithm on |
| 8 | poll | `--nodelay=0` | pollnagle | `cases pollnagle` | Nagle's algorithm on |
| 9 | poll | *(none)* | pollscale | `pollscale` | Clean poll scaling (**only if section 2 showed drift**) |
| 10 | select | `--nodelay=0` | selectnagle | `cases selectnagle nohigh` | Nagle's algorithm on |

Time per session: about 7 min of cases plus about 2 min for restart and checks, so about 9 minutes. The select session is about 7 minutes. Total about 1 h 30 min.

**If you must cut,** drop sessions in this order: 5, 8, 3.

---

## 4. One session, step by step

Example: session 1. For other sessions, change only the **first line on Laptop 1** and the **command on Laptop 2**.

### Step A — Laptop 1: start the server
```bash
BACKEND=epoll; FLAGS="--epoll-oneshot"; LABEL=epolloneshot
pkill -x echo-$BACKEND; sleep 1
RUN_DIR=$HOME/CS331-T008-NetworkIO/server_runs/${LABEL}_$(date +%H%M%S); mkdir -p "$RUN_DIR"
./build/echo-$BACKEND --port 8080 --msg-size 512 --max-conns 10240 --cpu $SERVER_CPU \
  --stats-file $RUN_DIR/server_stats.jsonl $FLAGS > $RUN_DIR/server.log 2>&1 &
sleep 2; PID=$(pgrep -n -x echo-$BACKEND); echo "PID=$PID"; cat $RUN_DIR/server.log
taskset -cp $PID                      # must show only $SERVER_CPU
```
**Continue only if** `PID=` shows a number, there's no `Exit` line, and `taskset` shows your server core. Otherwise see section 6.

### Step B — Laptop 2: run the cases (about 7 minutes; don't touch either laptop)
```bash
cases epolloneshot
```
Each case prints a summary. Check:
- `Connection barrier complete: N / N established`
- `Conn / Send / Recv / Bad: 0 / 0 / 0 / 0`. A handful of timeouts at 10,000 connections are acceptable.
- The last case (`...r1000end`) should show latency close to the first case. A large increase means the server drifted; tell Claude.

### Step C — Laptop 1: record server counters, stop the server, check health
```bash
kill -USR1 $PID; sleep 1
tail -1 $RUN_DIR/server_stats.jsonl | python3 -c "import json,sys; s=json.load(sys.stdin).get('stats',{}); print('server errors', s.get('errors'), 'ring_overflows', s.get('ring_overflows'))"
kill $PID; sleep 2; pgrep -x echo-$BACKEND || echo stopped      # must print: stopped
health                                # frequency within 5% of the first reading; no temperature near its limit
```
If the frequency dropped more than 5% or a temperature is near its limit, wait 2 minutes and run `health` again before the next session.

### The first line of Step A for every session

```bash
BACKEND=epoll;  FLAGS="--epoll-oneshot";       LABEL=epolloneshot   # 1
BACKEND=epoll;  FLAGS="--epoll-maxevents=1";   LABEL=epollmaxev     # 2
BACKEND=epoll;  FLAGS="--epoll-et";            LABEL=epollet        # 3
BACKEND=epoll;  FLAGS="--nodelay=0";           LABEL=epollnagle     # 4
BACKEND=epoll;  FLAGS="--buf-size 512";        LABEL=epollbuf       # 5
BACKEND=uring;  FLAGS="--uring-entries 256";   LABEL=uringsmall     # 6
BACKEND=uring;  FLAGS="--nodelay=0";           LABEL=uringnagle     # 7
BACKEND=poll;   FLAGS="--nodelay=0";           LABEL=pollnagle      # 8
BACKEND=poll;   FLAGS="";                      LABEL=pollscale      # 9 (only if poll Rep 2 drifted)
BACKEND=select; FLAGS="--nodelay=0";           LABEL=selectnagle    # 10
```

**After session 1, send the Laptop 2 output to Claude** before continuing, so the tags and numbers can be checked once.

---

## 5. Flag rules (checked against the server code)

- **Write `--nodelay=0` with `=`.** With a space, the server treats `0` as an unexpected argument and won't start. (`--epoll-maxevents`, `--buf-size` and `--uring-entries` accept either form.)
- **`--epoll-*` flags work only with epoll.** On other backends the server refuses to start.
- **Only `--uring-entries` and `--uring-cq-entries` work for io_uring.** `--uring-fixed-files`, `--uring-provided-buffers`, `--uring-multishot-*`, `--uring-single-issuer`, `--uring-defer-taskrun`, `--uring-sqpoll` and the other `--uring-*` options are not implemented; the server exits with "operation not supported".
- **Not used:** `--busy-poll` (needs root or a global sysctl that would affect other runs), `--backlog` and `--accept-batch` (connection setup only, outside the measurement), `--rcvbuf`/`--sndbuf` (small effect at 512 B), `--sendfile`/`--file-read`/`--chunk-size` (file server only).

---

## 6. Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `PID=` empty, `[1]+ Exit 127` | Wrong binary path | Run from `~/CS331-T008-NetworkIO`; the command uses `./build/...`. Type it fresh rather than editing an old command with the up arrow. |
| `PID=` empty, `server.log` shows an error | A flag was rejected | Check spelling and section 5. |
| `Address already in use` in `server.log` | An old server is still running | `pkill -x echo-$BACKEND` (or `sudo pkill -x echo-$BACKEND`), then repeat Step A. |
| Client prints `Connection refused` | Server isn't running | Repeat Step A and check `PID=`. |
| Fewer than 10000 / 10000 connections | fd limit too low | `ulimit -n` must be 65536 on both laptops; re-run the `prlimit` line in section 1. |
| `run: command not found` | New terminal | Paste the helper block from section 1 again. |
| Server still listed after `kill` | Not stopped yet | `kill -9 $PID` |
| A session went wrong part-way | — | Restart the server (Step A) and run the same `cases` command again. The newest result for each case replaces the older one. |

---

## 7. After the runs

### Laptop 2: quick check
```bash
for f in results/variants/*/results.csv; do echo "$f: $(($(wc -l < "$f") - 1)) rows"; done
```
Expected rows: 9 per session (7 for select sessions, 8 for `pollscale`). More rows mean a case was re-run, which is fine.

### Copy to USB
```bash
# Laptop 2
USB=/media/$USER/<USB_LABEL>
mkdir -p "$USB/CS331"
cp -a ~/CS331-T008-NetworkIO/results "$USB/CS331/"
sync
# Laptop 1
USB=/media/$USER/<USB_LABEL>
mkdir -p "$USB/CS331"
cp -a ~/CS331-T008-NetworkIO/server_runs "$USB/CS331/"
sync
```

### Bring them into the repo on an online machine
Copy `results/variants/` (and `results/poll_rep2_*` if it isn't in git yet) into the repo's `results/` folder, and `server_runs/` into the repo root. Then:
```bash
git add results server_runs
git commit -m "Flag experiment results"
git push origin loadgen-bench
```
Claude then adds the flag-comparison and clean-poll graphs and updates the report.
