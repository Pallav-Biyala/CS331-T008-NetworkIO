# TCP Echo Load Generator (`echo_loadgen`)

A high-performance, nonblocking C++ TCP load generator designed for benchmarking Linux network I/O mechanisms (`select`, `poll`, `epoll`, and `io_uring`) under identical, reproducible workloads.

---

## 1. Overview & Architecture

### Protocol & Message Framing
- **Transport**: Persistent TCP stream sockets with `TCP_NODELAY` (Nagle disabled) and `O_NONBLOCK`.
- **Application Protocol**: Pure binary framing matching the server's `--msg-size` (default: 512 bytes).
- **Embedded Client Framing**: The existing server echoes arbitrary bytes without modifying payload contents. To enable zero-loss tracking, corruption detection, and microsecond-accurate RTT measurement, the load generator embeds a compact 32-byte binary header inside each 512-byte frame:
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
  - On frame completion, `echo_loadgen` validates the magic number, connection ID, sequence number, and payload checksum.
  - RTT is computed directly from `CLOCK_MONOTONIC`: $RTT = t_{recv} - t_{send}$.

### Scheduling & Fairness
- **Event-Driven Nonblocking epoll**: A single high-efficiency epoll event loop drives all persistent connections.
- **High-Precision Rate Pacing**: Uses Linux `timerfd` (CLOCK_MONOTONIC) and open-loop target scheduling to pace message emissions at the exact requested rate without clock drift or cumulative timing errors.
- **Fair Scheduling**: All active connections are maintained in a FIFO round-robin idle pool (`std::deque<uint32_t>`). When a message is scheduled, the front connection is dispatched. Once its echo response returns, it is returned to the back of the queue. This prevents any single connection from monopolizing the generator and ensures balanced traffic distribution.

### Concurrency Model & Little's Law Limitation
- The generator implements a **one-outstanding-request-per-connection** model (matching the server's backpressure design).
- **Throughput Upper Bound**: By Little's Law, the maximum achievable throughput with $C$ concurrent connections is:
  $$R_{max} = \frac{C}{\text{RTT}_{avg}}$$
  - *Example*: With $C = 100$ connections and an average RTT of $0.5\text{ ms}$, $R_{max} = 100 / 0.0005 = 200{,}000\text{ msgs/s}$ (ample headroom for 8,000 msgs/s).
  - *Example*: If server latency spikes to $20\text{ ms}$, $R_{max} = 100 / 0.020 = 5{,}000\text{ msgs/s}$.
- If all connections are waiting for replies, the load generator tracks **concurrency stalls** and explicitly reports when the target offered rate could not be achieved due to the concurrency bottleneck.

---

## 2. Building

Build using the root Makefile:

```bash
make echo_loadgen
```

Or compile directly with `g++`:

```bash
g++ -std=c++17 -O3 -Wall -Wextra -pthread code/tools/echo_loadgen.cpp -o echo_loadgen
```

---

## 3. Usage & Command-Line Arguments

```bash
./echo_loadgen \
    --host 192.168.50.1 \
    --port 8080 \
    --connections 100 \
    --rate 1000 \
    --duration 30 \
    --msg-size 512 \
    --output results.csv
```

### Arguments:
| Argument | Description | Default |
| :--- | :--- | :--- |
| `--host` | Server IP or hostname | `127.0.0.1` |
| `--port` | Server listening port | `8080` |
| `--connections` | Number of persistent TCP connections | `100` |
| `--rate` | Aggregate target offered message rate (msgs/sec) | `1000` |
| `--duration` | Measurement duration (seconds) | `30` |
| `--msg-size` | Application message size in bytes | `512` |
| `--output` | CSV output file path | *(stdout only if omitted)* |
| `--warmup` | Warmup duration in seconds before recording metrics | `0` |
| `--cooldown` | Cooldown / drain duration in seconds after test | `0` |
| `--timeout` | Request timeout in milliseconds | `5000` |
| `--connect-timeout` | Timeout for initial connection establishment (seconds) | `10` |

---

## 4. Output & Metrics

### Human-Readable Console Summary
```
======================================================================
               TCP ECHO LOAD GENERATOR SUMMARY
======================================================================
Target Configuration:
  Server:                  192.168.50.1:8080
  Concurrent Connections:  100
  Target Offered Rate:     1000.00 msgs/sec
  Message Size:            512 bytes
  Configured Duration:     30.00 s (Warmup: 10.00 s, Cooldown: 10.00 s)

Execution & Throughput:
  Actual Duration:         30.00 s
  Messages Sent:           30000 (1000.00 msgs/sec)
  Messages Echoed:         30000 (1000.00 msgs/sec)
  Target Offered Met:      YES

Latency Distribution (RTT):
  p50 (Median):            124 us (0.12 ms)
  p95:                     246 us (0.25 ms)
  p99:                     386 us (0.39 ms)
  Min / Avg / Max:         28 / 133.87 / 1097 us

Reliability & Error Counters:
  Connection Failures:     0
  Send / Recv Errors:      0 / 0
  Timeouts:                0
  Mismatched / Corrupted:  0
  Concurrency Stalls:      0
======================================================================
```

### CSV Schema (`--output results.csv`)
If the file does not exist, a header is written automatically. Subsequent runs append new rows:
- `target_rate`: Configured aggregate target rate (msgs/sec).
- `actual_send_rate`: Actual messages sent per second.
- `echoed_rate`: Successfully received echoed messages per second.
- `total_sent`: Total messages sent during measurement phase.
- `total_received`: Total messages echoed and verified during measurement phase.
- `rtt_p50_us`, `rtt_p95_us`, `rtt_p99_us`: RTT percentiles in microseconds.
- `conn_failures`: Failed TCP connections during connection phase.
- `send_errors`, `recv_errors`: Socket I/O errors encountered.
- `timeouts`: Number of request timeouts.
- `mismatched`: Number of corrupted, truncated, or out-of-order frames.
- `duration_s`: Requested duration.
- `actual_duration_s`: Measured elapsed time.
- `host`, `port`, `connections`, `msg_size`: Experiment parameters.
- `rtt_min_us`, `rtt_max_us`, `rtt_avg_us`: Additional latency statistics.
- `concurrency_stalls`: Count of ticks where all connections were busy.
- `target_achieved`: `1` if actual throughput reached $\ge 95\%$ of target offered rate, `0` otherwise.

---

## 5. Sample Commands for Experimental Workloads

Below are the sample commands for running the benchmark protocol (10s warmup, 30s measurement, 10s cooldown) across the target matrix. Assume Server IP is `192.168.50.1` and Port is `8080`.

### A. Connection Scaling Suite (Fixed Rate = 1000 msgs/s)
```bash
# 100 Connections
./echo_loadgen --host 192.168.50.1 --port 8080 --connections 100 --rate 1000 \
    --duration 30 --warmup 10 --cooldown 10 --msg-size 512 --output results_conn100.csv

# 500 Connections
./echo_loadgen --host 192.168.50.1 --port 8080 --connections 500 --rate 1000 \
    --duration 30 --warmup 10 --cooldown 10 --msg-size 512 --output results_conn500.csv

# 900 Connections
./echo_loadgen --host 192.168.50.1 --port 8080 --connections 900 --rate 1000 \
    --duration 30 --warmup 10 --cooldown 10 --msg-size 512 --output results_conn900.csv
```

### B. Offered-Load Scaling Suite (e.g. at 500 Connections)
```bash
# 500 msgs/sec
./echo_loadgen --host 192.168.50.1 --port 8080 --connections 500 --rate 500 \
    --duration 30 --warmup 10 --cooldown 10 --msg-size 512 --output results_rate500.csv

# 1000 msgs/sec
./echo_loadgen --host 192.168.50.1 --port 8080 --connections 500 --rate 1000 \
    --duration 30 --warmup 10 --cooldown 10 --msg-size 512 --output results_rate1000.csv

# 2000 msgs/sec
./echo_loadgen --host 192.168.50.1 --port 8080 --connections 500 --rate 2000 \
    --duration 30 --warmup 10 --cooldown 10 --msg-size 512 --output results_rate2000.csv

# 4000 msgs/sec
./echo_loadgen --host 192.168.50.1 --port 8080 --connections 500 --rate 4000 \
    --duration 30 --warmup 10 --cooldown 10 --msg-size 512 --output results_rate4000.csv

# 8000 msgs/sec
./echo_loadgen --host 192.168.50.1 --port 8080 --connections 500 --rate 8000 \
    --duration 30 --warmup 10 --cooldown 10 --msg-size 512 --output results_rate8000.csv
```

---

## 6. Assumptions & Limitations

1. **One-Outstanding-Request Model**: Each connection allows only one in-flight request at a time. Throughput is bounded by $C / \text{RTT}$. The tool tracks when this constraint prevents achieving the requested target rate.
2. **Coordinated Omission Avoidance**: The generator schedules message send deadlines open-loop based on elapsed time rather than in a closed loop. If the server slows down, the discrepancy between target rate and actual rate is exposed.
3. **Nagle & Delayed ACK**: `TCP_NODELAY` is enabled on both client and server to prevent Nagle buffering delays.
4. **Local Resource Limits**: At 900+ connections, client file descriptor limits (`RLIMIT_NOFILE`) are automatically raised to avoid socket exhaustion.
