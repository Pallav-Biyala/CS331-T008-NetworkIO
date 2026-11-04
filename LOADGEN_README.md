# TCP Echo Load Generator (`echo_loadgen`)

A high-performance, nonblocking C++ TCP load generator designed for benchmarking Linux network I/O mechanisms (`select`, `poll`, `epoll`, and `io_uring`) under identical, reproducible workloads.

---

## 1. Overview & Architecture

### Protocol & Message Framing
- **Transport**: Persistent TCP stream sockets with `TCP_NODELAY` (Nagle disabled) and `O_NONBLOCK`.
- **Application Protocol**: Pure binary framing matching the server's `--msg-size` (default: 512 bytes).
- **Embedded Client Framing**: The existing server echoes arbitrary bytes without modifying payload contents. To enable zero-loss tracking, corruption detection, and nanosecond-accurate RTT measurement, the load generator embeds a compact 32-byte binary header inside each 512-byte frame:
  ```
  +-------------------------------------------------------------+
  | MessageHeader (32 bytes)                                    |
  |  - magic (4 bytes): 0x4543484F ("ECHO")                     |
  |  - conn_id (4 bytes): connection identifier (0 .. N-1)      |
  |  - seq_num (8 bytes): monotonic sequence number per conn    |
  |  - send_time_ns (8 bytes): CLOCK_MONOTONIC send timestamp   |
  |  - msg_size (4 bytes): expected total frame size (512)      |
  |  - checksum (4 bytes): Adler32 checksum of payload          |
  +-------------------------------------------------------------+
  | Payload (480 bytes): deterministic data pattern             |
  +-------------------------------------------------------------+
  ```
- **Integrity & Partial I/O Handling**:
  - The client accumulates received bytes in per-connection buffers until a full 512-byte frame is received, correctly handling TCP segmentation and coalescing.
  - On frame completion, `echo_loadgen` validates the magic number, connection ID, sequence number, and payload checksum against the front of `conn.in_flight`.

### Strict Per-Connection FIFO Queuing & Pipelining ($W$)
- Pipelining window $W$ is configured via `--pipeline-window W` (or `-W`, default 1).
- Each connection maintains:
  - `conn.tx_queue`: strict FIFO queue of scheduled arrivals awaiting dispatch.
  - `conn.in_flight`: tracking queue of up to $W$ in-flight requests on the wire.
- When an in-flight slot opens up (upon response completion), `pump_conn_tx_queue()` drains the front of `conn.tx_queue` in exact FIFO sequence order.

### Non-Overlapping Latency Definitions
- Each request records `sched_time_ns` ($t_{\text{sched}}$), `tx_start_ns`, and `send_time_ns` ($t_{\text{send}}$) at the exact moment its 512th byte is accepted by the kernel.
- Upon completion at $t_{\text{recv}}$:
  - $\text{sched\_lag} = t_{\text{send}} - t_{\text{sched}}$ (client queue / scheduling delay)
  - $\text{app\_rtt} = t_{\text{recv}} - t_{\text{send}}$ (network RTT + server processing time)
  - $\text{e2e\_lat} = t_{\text{recv}} - t_{\text{sched}}$ (end-to-end latency: $\text{e2e\_lat} = \text{sched\_lag} + \text{app\_rtt}$)

### Modes & High-Precision Rate Pacing
- **Open-Loop Mode** (`--mode open-loop`):
  - Constant-rate pacing configures `timerfd` once per phase with `TFD_TIMER_ABSTIME` as a periodic timer (interval floored at 50 µs) with drift-free 128-bit integer deadlines $t_k = t_0 + \lfloor k \cdot 10^9 / \lambda \rfloor$ (zero `timerfd_settime` calls in the hot loop).
  - Poisson and Burst distributions re-arm a one-shot absolute timer only when advancing to a future deadline.
- **Closed-Loop Mode** (`--mode closed-loop` or `--unthrottled`):
  - Concurrency ceiling runs: immediately refills each active socket's pipeline window $W$ to measure saturation throughput without mixing into open-loop curves.

### Measurement-Window Deltas
- Snapshots Linux `TCP_INFO` (`tcpi_total_retrans`) across active sockets and `getrusage(RUSAGE_SELF)` at the start of `MEASURING` (after warmup) and again after drain completes.
- Computes `tcp_retrans_delta` and `client_cpu_pct` over the measurement duration.

---

## 2. Building

Build using the root Makefile:

```bash
make echo_loadgen
```

Or compile directly with `g++`:

```bash
g++ -std=c++17 -O3 -Wall -Wextra -Werror -pthread code/tools/echo_loadgen.cpp -o echo_loadgen
```

---

## 3. Usage & Command-Line Arguments

```bash
./echo_loadgen \
    --host 192.168.50.1 \
    --port 8080 \
    --connections 100 \
    --rate 1000 \
    --mode open-loop \
    --pipeline-window 1 \
    --duration 30 \
    --msg-size 512 \
    --output results.csv \
    --json-output results/run.json
```

### Arguments:
| Argument | Description | Default |
| :--- | :--- | :--- |
| `--host` | Server IP or hostname | `127.0.0.1` |
| `--port` | Server listening port | `8080` |
| `--connections` | Number of persistent TCP connections | `100` |
| `--rate` | Aggregate target offered message rate (msgs/sec) | `1000` |
| `--mode` | Benchmark mode (`open-loop` or `closed-loop`) | `open-loop` |
| `--unthrottled` | Alias for `--mode closed-loop` (maximum throughput) | `false` |
| `--dist` | Arrival distribution (`constant`, `poisson`, `burst`) | `constant` |
| `--pipeline-window`, `-W` | Pipelining depth per connection ($W$) | `1` |
| `--active-share` | Fraction of connections active ($0.0 < \text{share} \le 1.0$) | `1.0` |
| `--duration` | Measurement duration (seconds) | `30` |
| `--msg-size` | Application message size in bytes | `512` |
| `--output` | CSV output file path | *(stdout only if omitted)* |
| `--json-output` | Per-run JSON report with 1s timeseries throughput buckets | *(none)* |
| `--warmup` | Warmup duration in seconds before recording metrics | `0` |
| `--cooldown` | Cooldown / drain duration in seconds after test | `0` |
| `--timeout` | Request timeout in milliseconds | `5000` |
| `--connect-timeout` | Timeout for initial connection establishment (seconds) | `10` |
| `--self-test` | Run built-in self-test verification suite and exit | `false` |

---

## 4. Execution Workflow Scripts (`run_experiment.sh`)

The repository includes `run_experiment.sh` to automate the testing and benchmarking workflows:

1. **Self-Test Verification**:
   ```bash
   ./run_experiment.sh --self-test
   ```

2. **3-Minute Pilot Check**:
   ```bash
   HOST=192.168.50.1 PORT=8080 ./run_experiment.sh --pilot
   ```

3. **Full 5-Suite Benchmark (4-Rep Cyclic Latin-Square Rotation)**:
   ```bash
   HOST=192.168.50.1 PORT=8080 ./run_experiment.sh --full
   ```

If `SSH_SERVER="user@192.168.50.1"` is set, the script automatically manages server processes remotely across runs.
