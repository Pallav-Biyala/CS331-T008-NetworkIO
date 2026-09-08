# CS331-T008 — Network I/O Benchmark: Demo Commands

## Repository Structure
- `code/`: Contains the raw C implementations for the network engines and shared utility headers.
- `results/`: Contains the benchmark shell scripts, analysis scripts (`analysis_scripts/`), raw logs (`perf/`, `strace/`, `throughput/`), and generated graphs (`graphs/`).
- `ppt/`: Contains the project presentation.
- `report/`: Contains the final technical report.
- `metrics_analysis.md`: The final generated markdown report summarizing the benchmark data.

Run these in order from the project root directory. Steps 1–5 reset every new
terminal session/reboot — redo them if you're demoing in a fresh shell.

## 1. Navigate to project
```bash
cd ~/Desktop/CS331-T008-NetworkIO
```

## 2. Kill any leftover server processes
```bash
pkill -f server_select; pkill -f server_poll; pkill -f server_epoll; pkill -f server_iouring
sudo fuser -k 8080/tcp 2>/dev/null
```

## 3. Raise file descriptor limit (**mandatory** — do this every new terminal session)
```bash
ulimit -n 65536
```
Without this the OS caps each process at 1024 open sockets. tcpkali needs one socket
per connection, so c5000/c10000 trials will fail with
`Could not create N connections in allotted time` if this is not set.

## 4. System tuning
```bash
sudo sysctl -w fs.file-max=200000
sudo sysctl -w net.ipv4.tcp_tw_reuse=1
sudo sysctl -w kernel.yama.ptrace_scope=0
sudo sysctl -w kernel.perf_event_paranoid=-1
```

## 5. Rebuild all five servers
```bash
gcc -O2 code/server_select.c code/network_utils.c -o code/server_select
gcc -O2 code/server_poll.c code/network_utils.c -o code/server_poll
gcc -O2 code/server_epoll.c code/network_utils.c -o code/server_epoll
gcc -O2 code/server_iouring.c code/network_utils.c -o code/server_iouring
gcc -O2 code/server_iouring_sqpoll.c code/network_utils.c -o code/server_iouring_sqpoll
```

## 6. Make benchmark scripts executable
```bash
chmod +x results/bench_throughput.sh results/bench_strace.sh results/bench_perf.sh
```

## 7. Sanity check — one manual trial before the full run
```bash
./code/server_select 8080 &
tcpkali -c 10 -m '{"ping"}' -T 5s 127.0.0.1:8080
kill %1
```
Confirm the job ends with `Terminated`, not `Broken pipe`.

## 8. Run the full benchmark matrix
```bash
./results/bench_throughput.sh
./results/bench_strace.sh
./results/bench_perf.sh
```

- `results/bench_throughput.sh` → `./results/throughput/` — **two passes per cell**:
  - `<tag>_tcpkali.log` — unlimited rate → aggregate bandwidth (Mbps) + packet rate
  - `<tag>_latency_tcpkali.log` — fixed rate (20 msg/s/conn) → p50/p95/p99/p99.9 latency
- `results/bench_strace.sh` → `./results/strace/` — syscall counts (`strace -c`) per engine/connection-count
- `results/bench_perf.sh` → `./results/perf/` — context switches, CPU cycles, RSS, and CPU% (`perf stat` + `pidstat`)

## 9. Spot-check results
```bash
# Bandwidth (unlimited rate)
cat results/throughput/epoll_c1000_tcpkali.log

# Latency percentiles (fixed rate)
cat results/throughput/epoll_c1000_latency_tcpkali.log

# Syscall counts
cat results/strace/epoll_c1000_strace.txt

# perf / CPU stats
cat results/perf/epoll_c1000_perf.txt
```

## 10. Generate Final Report and Results
Run the Python script to parse the data, generate the plots, and output the final `metrics_analysis.md` report to the root folder:
```bash
python3 results/analysis_scripts/generate_report.py
```
This script will automatically save all plotted `.png` images and the parsed `parsed.json` data into the `results/graphs/` directory.
