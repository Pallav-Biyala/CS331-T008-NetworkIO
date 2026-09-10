# Network I/O Server Performance Analysis

This document provides a comprehensive breakdown of the performance metrics across different Network I/O multiplexing strategies (`select`, `poll`, `epoll`, `io_uring`, and `io_uring` with `SQPOLL`).

> **Important — three separate benchmark scripts**
>
> All numbers in this report come from **three isolated benchmark scripts** with different durations, connection rates, and instrumentation levels.  Metrics from different columns must **not** be interpreted as if they came from the same run:
>
> | Script | Measures | tcpkali duration | Note |
> |--------|----------|-----------------|------|
> | `bench_throughput.sh` (pass 1) | Aggregate bandwidth (Mbps) | 15 s (c<5000), 30 s (c>=5000) | No instrumentation |
> | `bench_throughput.sh` (pass 2) | RTT latency percentiles | same | Fixed rate 20 msg/s/conn; **separate server start** from pass 1 |
> | `bench_strace.sh` | Syscall counts | 20 s | strace `-c` attached for 10 s window; **heavy ptrace overhead** — throughput numbers from this run are not representative |
> | `bench_perf.sh` | Context switches, CPU%, RSS | 25 s | `perf stat` for 10 s + `pidstat` 5 s; RSS-after captured mid-load (~18 s in) |
>

## 1. Metrics Tables

### Throughput (Mbps) — Downlink (client->server) / Uplink (server->client)
* **Source:** `bench_throughput.sh` pass 1 (unlimited message rate, no instrumentation).
  tcpkali reports `Aggregate bandwidth: X(down), Y(up) Mbps` at the end of the run.
  Duration: **15 s** for c < 5000 connections; **30 s** for c >= 5000 connections.
  *These numbers come from a clean run with no strace/perf overhead.*

![Throughput Plot](./throughput_plot.png)

| Connections | `select` | `poll` | `epoll` | `io_uring` | `io_uring` (sqpoll) |
|-------------|----------|--------|---------|------------|---------------------|
| **10** | 6852 ↓ / 6863 ↑ | 7221 ↓ / 7231 ↑ | 7341 ↓ / 7354 ↑ | 6776 ↓ / 6787 ↑ | 159 ↓ / 168 ↑ |
| **100** | 5569 ↓ / 5851 ↑ | 6390 ↓ / 6575 ↑ | 6372 ↓ / 6538 ↑ | 6343 ↓ / 6521 ↑ | 1582 ↓ / 1678 ↑ |
| **500** | 5541 ↓ / 5829 ↑ | 5664 ↓ / 5767 ↑ | 5920 ↓ / 6198 ↑ | 5571 ↓ / 5502 ↑ | 3956 ↓ / 3813 ↑ |
| **1000** | 5991 ↓ / 6042 ↑ | 5306 ↓ / 5327 ↑ | 5454 ↓ / 5451 ↑ | 4985 ↓ / 4805 ↑ | 3633 ↓ / 3407 ↑ |
| **1500** | 4866 ↓ / 4709 ↑ | 4775 ↓ / 4661 ↑ | 3686 ↓ / 3567 ↑ | 4135 ↓ / 3930 ↑ | 3403 ↓ / 3186 ↑ |
| **2000** | 4431 ↓ / 4234 ↑ | 4598 ↓ / 4417 ↑ | 3794 ↓ / 3643 ↑ | 3828 ↓ / 3639 ↑ | 3361 ↓ / 3139 ↑ |
| **2500** | 3981 ↓ / 3814 ↑ | 4153 ↓ / 3991 ↑ | 3648 ↓ / 3471 ↑ | 3457 ↓ / 3253 ↑ | 3205 ↓ / 2936 ↑ |
| **3000** | 4057 ↓ / 3882 ↑ | 4045 ↓ / 3880 ↑ | 3668 ↓ / 3459 ↑ | 3236 ↓ / 2996 ↑ | 3359 ↓ / 3104 ↑ |
| **3500** | 3849 ↓ / 3768 ↑ | 3937 ↓ / 3832 ↑ | 3345 ↓ / 3178 ↑ | 3282 ↓ / 3177 ↑ | 3515 ↓ / 3328 ↑ |
| **4000** | 3658 ↓ / 3476 ↑ | 3875 ↓ / 3718 ↑ | 3708 ↓ / 3931 ↑ | 3174 ↓ / 3055 ↑ | 3369 ↓ / 3162 ↑ |
| **4500** | 3659 ↓ / 3833 ↑ | 3824 ↓ / 3792 ↑ | 3584 ↓ / 3723 ↑ | 3043 ↓ / 2919 ↑ | 3425 ↓ / 3172 ↑ |
| **5000** | 3721 ↓ / 3644 ↑ | 3600 ↓ / 3601 ↑ | 3504 ↓ / 3688 ↑ | 2909 ↓ / 2813 ↑ | 3720 ↓ / 3595 ↑ |


### Latency (ms) — 50th / 95th / 99th Percentile
* **Source:** `bench_throughput.sh` pass 2 (`--message-rate 20` msg/s per connection).
  This is a **separate server instance** from pass 1 — throughput and latency were measured independently because flooding the pipe (pass 1) prevents per-message RTT timestamping.  Duration: **15 s** / **30 s** (same schedule as pass 1).
  tcpkali outputs `Message latency at percentiles: p50/p95/p99/p99.9 ms`; we extract p50, p95, and p99.

![Latency Plot](./latency_plot.png)

| Connections | `select` | `poll` | `epoll` | `io_uring` | `io_uring` (sqpoll) |
|-------------|----------|--------|---------|------------|---------------------|
| **10** | 0.0 / 0.3 / 1.0 | 0.0 / 0.1 / 0.6 | 0.0 / 0.1 / 0.6 | 0.0 / 0.1 / 1.0 | 0.7 / 0.9 / 1.0 |
| **100** | 0.0 / 0.1 / 0.7 | 0.0 / 0.1 / 0.7 | 0.0 / 0.1 / 0.3 | 0.0 / 0.1 / 0.3 | 0.8 / 0.9 / 1.0 |
| **500** | 0.1 / 0.3 / 0.5 | 0.1 / 0.2 / 0.9 | 0.0 / 0.2 / 0.4 | 0.0 / 0.2 / 0.5 | 0.8 / 1.0 / 1.1 |
| **1000** | 0.3 / 0.5 / 0.9 | 0.3 / 0.5 / 0.9 | 0.1 / 0.3 / 0.6 | 0.1 / 0.4 / 0.9 | 0.8 / 1.0 / 1.4 |
| **1500** | 0.5 / 0.7 / 1.2 | 0.5 / 0.7 / 1.2 | 0.1 / 0.4 / 0.8 | 0.3 / 0.7 / 1.5 | 0.8 / 1.4 / 1.6 |
| **2000** | 0.8 / 1.5 / 2.0 | 0.7 / 0.9 / 1.4 | 0.2 / 0.5 / 0.8 | 0.5 / 1.0 / 1.5 | 0.9 / 1.6 / 1.7 |
| **2500** | 1.0 / 1.5 / 2.2 | 0.9 / 1.6 / 2.5 | 0.4 / 1.0 / 3.7 | 0.5 / 1.1 / 1.5 | 1.0 / 1.8 / 2.1 |
| **3000** | 1.2 / 1.7 / 2.3 | 1.1 / 1.8 / 2.5 | 0.5 / 1.1 / 3.2 | 0.6 / 1.5 / 2.7 | 1.0 / 1.8 / 4.1 |
| **3500** | 1.4 / 2.1 / 3.8 | 1.3 / 2.0 / 2.5 | 0.5 / 1.3 / 1.9 | 0.6 / 1.5 / 4.3 | 1.1 / 2.1 / 2.8 |
| **4000** | 1.6 / 2.7 / 15.8 | 1.3 / 2.2 / 9.1 | 0.6 / 1.2 / 2.2 | 0.9 / 4.6 / 14.7 | 1.1 / 2.5 / 5.2 |
| **4500** | 1.7 / 2.8 / 16.3 | 1.6 / 2.5 / 7.1 | 0.8 / 1.5 / 1.8 | 1.1 / 19.4 / 36.0 | 1.1 / 2.3 / 10.1 |
| **5000** | 1.7 / 2.9 / 11.5 | 1.7 / 2.8 / 12.0 | 0.8 / 1.5 / 1.9 | 15.8 / 39.7 / 50.3 | 1.1 / 2.2 / 3.4 |


### CPU Utilization (%)
* **Source:** `bench_perf.sh` — `pidstat -p <pid> 1 5` run after the `perf stat` window (i.e., approximately 18 s into the 25 s load).  We parse the final `Average:` line and extract the `%CPU` column (`%usr + %system`), representing the fraction of one CPU core used by the server process.
  *This is from a separate run from the throughput benchmark.*

![CPU Plot](./cpu_plot.png)

| Connections | `select` | `poll` | `epoll` | `io_uring` | `io_uring` (sqpoll) |
|-------------|----------|--------|---------|------------|---------------------|
| **10** | 100.0% | 100.0% | 99.8% | 100.0% | 100.0% |
| **100** | 99.8% | 100.0% | 99.8% | 100.0% | 100.0% |
| **500** | 100.0% | 99.8% | 100.2% | 100.2% | 100.2% |
| **1000** | 100.0% | 100.0% | 99.8% | 99.8% | 100.0% |
| **1500** | 100.0% | 100.4% | 100.0% | 100.0% | 100.2% |
| **2000** | 100.2% | 100.0% | 99.8% | 100.0% | 100.0% |
| **2500** | 100.0% | 99.8% | 100.2% | 100.0% | 99.8% |
| **3000** | 99.8% | 100.2% | 100.0% | 100.0% | 100.0% |
| **3500** | 99.8% | 99.8% | 100.2% | 100.0% | 99.2% |
| **4000** | 100.2% | 100.4% | 100.2% | 99.2% | 99.4% |
| **4500** | 100.0% | 100.4% | 100.2% | 99.8% | 99.2% |
| **5000** | 99.8% | 98.6% | 99.8% | 99.8% | 99.0% |


### Memory Usage — RSS Delta (KB)
* **Source:** `bench_perf.sh` reads RSS via `ps -o rss= -p <pid>` (units: KB) before the load starts and again after the `perf stat` + `pidstat` windows (approximately 18 s into the 25 s tcpkali run, **before tcpkali has finished**).
  The delta `RSS_after - RSS_before` reflects net memory growth during the measured window.  Because tcpkali is still running when RSS-after is sampled, this is **not** a final steady-state reading.
  Note: `ps -o rss=` returns values already in KB — no unit conversion is applied.
  *This is from a separate run from the throughput benchmark.*

![Memory Plot](./memory_plot.png)

| Connections | `select` | `poll` | `epoll` | `io_uring` | `io_uring` (sqpoll) |
|-------------|----------|--------|---------|------------|---------------------|
| **10** | 80 | 80 | 80 | 120 | 120 |
| **100** | 6316 | 8212 | 6204 | 6144 | 8272 |
| **500** | 34832 | 32904 | 32988 | 33124 | 34992 |
| **1000** | 64448 | 65900 | 64316 | 67868 | 67772 |
| **1500** | 96584 | 97220 | 96952 | 102540 | 102552 |
| **2000** | 128816 | 128096 | 128336 | 135404 | 135404 |
| **2500** | 160964 | 161008 | 161860 | 170064 | 170280 |
| **3000** | 193908 | 193688 | 192232 | 203072 | 204880 |
| **3500** | 224956 | 225848 | 224180 | 237568 | 237764 |
| **4000** | 257360 | 257524 | 257596 | 272500 | 272416 |
| **4500** | 282292 | 285348 | 288264 | 305508 | 301100 |
| **5000** | 298808 | 298960 | 320608 | 340156 | 340168 |


### Context Switches (10 s perf stat window)
* **Source:** `bench_perf.sh` — `perf stat -e context-switches -p <pid>` attached mid-run for a fixed 10 s window (starting 3 s after load begins, after connection ramp-up).  Measures how many times the kernel descheduled the server process during that window.
  *This is from a separate run from the throughput benchmark.*

| Connections | `select` | `poll` | `epoll` | `io_uring` | `io_uring` (sqpoll) |
|-------------|----------|--------|---------|------------|---------------------|
| **10** | 72 | 75 | 89 | 84 | 20001 |
| **100** | 100 | 76 | 83 | 85 | 20012 |
| **500** | 87 | 80 | 95 | 83 | 10165 |
| **1000** | 70 | 83 | 80 | 79 | 5201 |
| **1500** | 85 | 70 | 85 | 72 | 3503 |
| **2000** | 80 | 79 | 66 | 80 | 2691 |
| **2500** | 87 | 71 | 72 | 85 | 3112 |
| **3000** | 92 | 74 | 65 | 93 | 4165 |
| **3500** | 89 | 78 | 64 | 79 | 6780 |
| **4000** | 96 | 91 | 95 | 87 | 6472 |
| **4500** | 84 | 78 | 64 | 104 | 6857 |
| **5000** | 117 | 93 | 86 | 83 | 7999 |

---

## 2. Raw Metrics Data

| Server | Connections | tp_down (Mbps) | tp_up (Mbps) | lat_50 (ms) | lat_95 (ms) | lat_99 (ms) | cpu (%) | mem (KB) | context_switches |
|--------|-------------|----------------|--------------|-------------|-------------|-------------|---------|----------|------------------|
| select | 10 | 6852.258 | 6863.258 | 0.0 | 0.3 | 1.0 | 100.0 | 80 | 72 |
| select | 100 | 5568.569 | 5851.111 | 0.0 | 0.1 | 0.7 | 99.8 | 6316 | 100 |
| select | 500 | 5541.327 | 5828.641 | 0.1 | 0.3 | 0.5 | 100.0 | 34832 | 87 |
| select | 1000 | 5990.787 | 6041.643 | 0.3 | 0.5 | 0.9 | 100.0 | 64448 | 70 |
| select | 1500 | 4866.404 | 4708.878 | 0.5 | 0.7 | 1.2 | 100.0 | 96584 | 85 |
| select | 2000 | 4431.373 | 4234.345 | 0.8 | 1.5 | 2.0 | 100.2 | 128816 | 80 |
| select | 2500 | 3981.355 | 3813.502 | 1.0 | 1.5 | 2.2 | 100.0 | 160964 | 87 |
| select | 3000 | 4057.048 | 3882.027 | 1.2 | 1.7 | 2.3 | 99.8 | 193908 | 92 |
| select | 3500 | 3849.336 | 3767.504 | 1.4 | 2.1 | 3.8 | 99.8 | 224956 | 89 |
| select | 4000 | 3657.643 | 3476.115 | 1.6 | 2.7 | 15.8 | 100.2 | 257360 | 96 |
| select | 4500 | 3659.24 | 3832.89 | 1.7 | 2.8 | 16.3 | 100.0 | 282292 | 84 |
| select | 5000 | 3720.618 | 3644.148 | 1.7 | 2.9 | 11.5 | 99.8 | 298808 | 117 |
| poll | 10 | 7220.883 | 7231.302 | 0.0 | 0.1 | 0.6 | 100.0 | 80 | 75 |
| poll | 100 | 6390.107 | 6574.964 | 0.0 | 0.1 | 0.7 | 100.0 | 8212 | 76 |
| poll | 500 | 5664.073 | 5767.284 | 0.1 | 0.2 | 0.9 | 99.8 | 32904 | 80 |
| poll | 1000 | 5306.197 | 5326.88 | 0.3 | 0.5 | 0.9 | 100.0 | 65900 | 83 |
| poll | 1500 | 4774.527 | 4660.747 | 0.5 | 0.7 | 1.2 | 100.4 | 97220 | 70 |
| poll | 2000 | 4598.123 | 4416.621 | 0.7 | 0.9 | 1.4 | 100.0 | 128096 | 79 |
| poll | 2500 | 4152.696 | 3990.893 | 0.9 | 1.6 | 2.5 | 99.8 | 161008 | 71 |
| poll | 3000 | 4044.683 | 3880.415 | 1.1 | 1.8 | 2.5 | 100.2 | 193688 | 74 |
| poll | 3500 | 3936.844 | 3832.0 | 1.3 | 2.0 | 2.5 | 99.8 | 225848 | 78 |
| poll | 4000 | 3874.504 | 3717.6 | 1.3 | 2.2 | 9.1 | 100.4 | 257524 | 91 |
| poll | 4500 | 3823.834 | 3792.425 | 1.6 | 2.5 | 7.1 | 100.4 | 285348 | 78 |
| poll | 5000 | 3600.123 | 3601.111 | 1.7 | 2.8 | 12.0 | 98.6 | 298960 | 93 |
| epoll | 10 | 7340.904 | 7354.381 | 0.0 | 0.1 | 0.6 | 99.8 | 80 | 89 |
| epoll | 100 | 6372.282 | 6538.263 | 0.0 | 0.1 | 0.3 | 99.8 | 6204 | 83 |
| epoll | 500 | 5920.348 | 6198.023 | 0.0 | 0.2 | 0.4 | 100.2 | 32988 | 95 |
| epoll | 1000 | 5453.798 | 5451.471 | 0.1 | 0.3 | 0.6 | 99.8 | 64316 | 80 |
| epoll | 1500 | 3685.638 | 3567.197 | 0.1 | 0.4 | 0.8 | 100.0 | 96952 | 85 |
| epoll | 2000 | 3793.852 | 3642.548 | 0.2 | 0.5 | 0.8 | 99.8 | 128336 | 66 |
| epoll | 2500 | 3648.252 | 3470.883 | 0.4 | 1.0 | 3.7 | 100.2 | 161860 | 72 |
| epoll | 3000 | 3667.676 | 3459.38 | 0.5 | 1.1 | 3.2 | 100.0 | 192232 | 65 |
| epoll | 3500 | 3344.564 | 3178.374 | 0.5 | 1.3 | 1.9 | 100.2 | 224180 | 64 |
| epoll | 4000 | 3707.927 | 3930.815 | 0.6 | 1.2 | 2.2 | 100.2 | 257596 | 95 |
| epoll | 4500 | 3584.479 | 3723.225 | 0.8 | 1.5 | 1.8 | 100.2 | 288264 | 64 |
| epoll | 5000 | 3504.5 | 3687.646 | 0.8 | 1.5 | 1.9 | 99.8 | 320608 | 86 |
| iouring | 10 | 6775.611 | 6787.294 | 0.0 | 0.1 | 1.0 | 100.0 | 120 | 84 |
| iouring | 100 | 6343.074 | 6521.023 | 0.0 | 0.1 | 0.3 | 100.0 | 6144 | 85 |
| iouring | 500 | 5571.157 | 5501.69 | 0.0 | 0.2 | 0.5 | 100.2 | 33124 | 83 |
| iouring | 1000 | 4985.095 | 4805.246 | 0.1 | 0.4 | 0.9 | 99.8 | 67868 | 79 |
| iouring | 1500 | 4135.455 | 3930.212 | 0.3 | 0.7 | 1.5 | 100.0 | 102540 | 72 |
| iouring | 2000 | 3828.319 | 3638.769 | 0.5 | 1.0 | 1.5 | 100.0 | 135404 | 80 |
| iouring | 2500 | 3456.871 | 3252.594 | 0.5 | 1.1 | 1.5 | 100.0 | 170064 | 85 |
| iouring | 3000 | 3236.38 | 2995.76 | 0.6 | 1.5 | 2.7 | 100.0 | 203072 | 93 |
| iouring | 3500 | 3282.361 | 3177.093 | 0.6 | 1.5 | 4.3 | 100.0 | 237568 | 79 |
| iouring | 4000 | 3174.26 | 3055.049 | 0.9 | 4.6 | 14.7 | 99.2 | 272500 | 87 |
| iouring | 4500 | 3043.35 | 2919.109 | 1.1 | 19.4 | 36.0 | 99.8 | 305508 | 104 |
| iouring | 5000 | 2908.802 | 2812.868 | 15.8 | 39.7 | 50.3 | 99.8 | 340156 | 83 |
| iouring_sqpoll | 10 | 158.726 | 168.46 | 0.7 | 0.9 | 1.0 | 100.0 | 120 | 20001 |
| iouring_sqpoll | 100 | 1582.119 | 1678.463 | 0.8 | 0.9 | 1.0 | 100.0 | 8272 | 20012 |
| iouring_sqpoll | 500 | 3955.955 | 3812.795 | 0.8 | 1.0 | 1.1 | 100.2 | 34992 | 10165 |
| iouring_sqpoll | 1000 | 3632.53 | 3407.149 | 0.8 | 1.0 | 1.4 | 100.0 | 67772 | 5201 |
| iouring_sqpoll | 1500 | 3403.106 | 3185.855 | 0.8 | 1.4 | 1.6 | 100.2 | 102552 | 3503 |
| iouring_sqpoll | 2000 | 3360.774 | 3138.51 | 0.9 | 1.6 | 1.7 | 100.0 | 135404 | 2691 |
| iouring_sqpoll | 2500 | 3204.677 | 2936.125 | 1.0 | 1.8 | 2.1 | 99.8 | 170280 | 3112 |
| iouring_sqpoll | 3000 | 3359.044 | 3103.708 | 1.0 | 1.8 | 4.1 | 100.0 | 204880 | 4165 |
| iouring_sqpoll | 3500 | 3515.063 | 3327.972 | 1.1 | 2.1 | 2.8 | 99.2 | 237764 | 6780 |
| iouring_sqpoll | 4000 | 3369.371 | 3161.988 | 1.1 | 2.5 | 5.2 | 99.4 | 272416 | 6472 |
| iouring_sqpoll | 4500 | 3425.026 | 3172.159 | 1.1 | 2.3 | 10.1 | 99.2 | 301100 | 6857 |
| iouring_sqpoll | 5000 | 3720.197 | 3594.645 | 1.1 | 2.2 | 3.4 | 99.0 | 340168 | 7999 |