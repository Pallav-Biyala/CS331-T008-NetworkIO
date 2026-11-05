# Benchmarking Methodology & Protocol Deviations

This document logs all methodological specifications and deliberate deviations from the initial team design PDF (`docs/Copy of Fair Benchmarking Design Plan select, poll, epoll, io_uring.pdf`) to meet the strict 3-hour machine sweep constraint while preserving scientific rigor.

---

## 1. Experimental Protocol Deviations

1. **Custom High-Precision Load Generator**:
   - *Design PDF:* Proposed `tcpkali` with separate idle client instances.
   - *Implementation:* Dropped `tcpkali` in favor of custom C++17 `echo_loadgen`. Embeds deterministic 32-byte binary header with nanosecond `CLOCK_MONOTONIC` timestamps and Adler32 checksums, ensuring zero undetected packet corruptions, precise RTT percentiles, and exact connection ID tracking.
   - *Note:* Absolute latency is directly comparable only within this tool across the four mechanisms.

2. **Phase Durations (5s / 20s / 5s)**:
   - *Design PDF:* 10s warmup, 30s measurement, 10s cooldown (~50s per run).
   - *Implementation:* Adjusted to 5s warmup, 20s steady-state measurement, 5s cooldown (~30s-35s per run) to fit the complete 49-configuration matrix with 5 repeats inside the 3.0-hour hard limit.

3. **Repetition Count**:
   - *Design PDF:* Recommended 7 repeats (minimum 5).
   - *Implementation:* 5 repeats per configuration (meeting the PDF's stated minimum acceptable statistical sample).

4. **Experiment Scope Reduction**:
   - *Dropped Experiments:* E4 (Message size sensitivity), E7 (Socket buffer sensitivity), and individual Tier-2/Tier-3 extension flags dropped to protect the 3-hour time budget.
   - *Retained Core Experiments:* E1 (Connection scaling up to 10k), E1s (Saturation throughput), E2 (Active vs idle ratio), and E3 (Offered load vs latency curve).

5. **E3 Configuration**:
   - Standardized to 500 connections across a 4-point offered load progression up to saturation.

6. **E2 Configuration**:
   - Evaluated at 1% and 10% active share (at 1,000 connections) and 1% active share (at 10,000 connections). 100% active share reuses baseline data from E1.

7. **Scheduling & Fairness**:
   - Implemented round-based randomized block scheduling (seeded shuffle across mechanisms and workload blocks) to prevent systematic drift from thermal or kernel cache bias.

8. **Warmup Telemetry**:
   - The load generator's built-in 5s warmup phase cleanly absorbs connection establishment handshakes and initial TCP slow-start before the 20s measurement window begins, verified by taking server `/proc` and cgroup telemetry snapshots before and after the measurement phase.
