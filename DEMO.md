# CS331-T008 — Network I/O Benchmark: Demo Commands

Run these in order from the `code/` directory. Steps 1–5 reset every new
terminal session/reboot — redo them if you're demoing in a fresh shell.

## 1. Navigate to project
```bash
cd ~/Desktop/CS331-T008-NetworkIO/code
```

## 2. Kill any leftover server processes
```bash
pkill -f select_server; pkill -f poll_server; pkill -f epoll_server; pkill -f iouring_server
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
gcc -O2 select_server.c network_utils.c -o select_server
gcc -O2 server_poll.c network_utils.c -o poll_server
gcc -O2 server_epoll.c network_utils.c -o epoll_server
gcc -O2 server_iouring.c network_utils.c -o iouring_server -luring
gcc -O2 server_iouring_sqpoll.c network_utils.c -o iouring_sqpoll_server
```

## 6. Make benchmark scripts executable
```bash
chmod +x bench_throughput.sh bench_strace.sh bench_perf.sh
```

## 7. Sanity check — one manual trial before the full run
```bash
./select_server 8080 &
tcpkali -c 10 -m '{"ping"}' -T 5s 127.0.0.1:8080
kill %1
```
Confirm the job ends with `Terminated`, not `Broken pipe`.

## 8. Run the full benchmark matrix
```bash
./bench_throughput.sh
./bench_strace.sh
./bench_perf.sh
```

- `bench_throughput.sh` → `./results/throughput/` — **two passes per cell**:
  - `<tag>_tcpkali.log` — unlimited rate → aggregate bandwidth (Mbps) + packet rate
  - `<tag>_latency_tcpkali.log` — fixed rate (20 msg/s/conn) → p50/p95/p99/p99.9 latency
- `bench_strace.sh` → `./results/strace/` — syscall counts (`strace -c`) per engine/connection-count
- `bench_perf.sh` → `./results/perf/` — context switches, CPU cycles, RSS, and CPU% (`perf stat` + `pidstat`)

## 9. Spot-check results
```bash
# Bandwidth (unlimited rate)
cat results/throughput/epoll_c1000_tcpkali.log

# Latency percentiles (fixed rate)
cat results/throughput/epoll_c1000_latency_tcpkali.log

# Syscall counts
cat results/strace/epoll_c1000_strace.txt

# perf / CPU statscat results/perf/epoll_c1000_perf.txt
```
