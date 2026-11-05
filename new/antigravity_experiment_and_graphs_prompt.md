# Prompt for Antigravity: Plotting & Experiment Update for Client-Only Runner (Up to 10K Connections)

Copy and paste the prompt below directly to Antigravity:

---

We have updated our client-only `run_experiment.sh` and `echo_loadgen.cpp` to focus on **high variance in connection scaling (from 50 up to 10,000 connections)** and **high variance in message send load (rates from 500 to 50,000 msgs/s, closed-loop saturation, active shares from 0.05 to 1.0, pipeline windows 1–8, and 5 arrival distributions: `constant`, `poisson`, `burst2x`, `burst` [4×], and `burst10x`)**.

Please **drop the old 16-graph plan**—specifically remove all `client_cpu_pct`, `echoed_rate / client_cpu_pct` efficiency, `tcp_retrans_delta`, `sched_lag`, and per-second timeseries charts (`client_cpu_pct` is client-side load generator CPU, not server CPU).

Keep **only three core metric families** across all plots:
1. **Throughput (`echoed_rate` in msgs/sec)**
2. **Messages Sent vs. Completed (`actual_send_rate` / `total_sent` vs `total_received` / `delivery_ratio_pct`)**
3. **Latency (`app_rtt_p50_us`, `app_rtt_p95_us`, `app_rtt_p99_us`, and `e2e_lat_p99_us`)**

---

## 1. How Data Is Organized by `run_experiment.sh`

Each backend repetition writes to `results/${MECH}_rep${REP}_${STAMP}/results.csv` (and per-case JSONs in `json/rep${REP}_${SUITE}_${CASE}.json`), where `tag` is formatted as `${MECH}_rep${REP}_${SUITE}_${CASE}` and `MECH` is one of `select`, `poll`, `epoll`, `uring`.

The 5 suites in `run_experiment.sh` are:
* **`S1_CONN` & `S1_CONN_MAX` (Connection Scaling `50` to `10,000`):**
  * Connections: `50, 100, 250, 500, 750, 900` (all 4 backends) and `1500, 2500, 5000, 7500, 10000` (`poll`, `epoll`, `uring`; `select` stops at `900` due to `FD_SETSIZE = 1024`).
  * Tested at `--rate 1000` (`c${c}_r1000`), `--rate 10000` (`c${c}_r10000`), and `--mode closed-loop` (`c${c}_closed`).
* **`S2_RATE`, `S2_RATE_HIGHCONN`, & `S2_RATE_MAX` (Offered Load Sweep):**
  * At `500` connections: rates `500, 1000, 2000, 4000, 8000, 12000, 16000, 25000, 35000, 50000` + `closed-loop`.
  * At `5000` connections (`poll`, `epoll`, `uring`): rates `1000, 4000, 8000, 16000, 25000, 50000` + `closed-loop`.
* **`S3_ACTIVE` (Active-Share / Idle-Socket Tax Sweep):**
  * Active shares (`--active-share`): `0.05, 0.1, 0.25, 0.5, 1.0` at `--rate 5000` across `900` connections (all 4 backends), `5000` connections, and `10000` connections (`poll`, `epoll`, `uring`).
* **`S4_PIPE` & `S4_PIPE_MAX` (Pipeline Window Sweep):**
  * Pipeline windows (`--pipeline-window`): `1, 2, 4, 8` tested at `100` conns (`10K` rate), `500` conns (`25K` rate), and `500` conns (`closed-loop`).
* **`S5_DIST` (Arrival Distribution & Burst-Intensity Sweep):**
  * Distributions (`--dist`): `constant`, `poisson`, `burst2x` (2× spike), `burst` (4× spike), and `burst10x` (10× micro-burst) tested at `500` conns (`5K` and `15K` avg msgs/s) and `5000` conns (`10K` avg msgs/s).

---

## 2. Generate Only These 8 Graphs

| # | Graph File / Title | Suite Filter | X-Axis | Y-Axis / Panels (Only Sent, Throughput, Latency) |
| :--- | :--- | :--- | :--- | :--- |
| **01** | **Connection Scaling (`50` to `10K`) — Latency (`p50` & `p99`)** | `S1_CONN` | Connections (`50` to `10,000`) | 2 panels (`1K` rate and `10K` rate): `app_rtt_p50_us` & `app_rtt_p99_us` (draw vertical dashed line at `x = 1024` for `select FD_SETSIZE`) |
| **02** | **Connection Scaling (`50` to `10K`) — Sent vs. Echoed Throughput** | `S1_CONN` | Connections (`50` to `10,000`) | `actual_send_rate` (dashed) vs `echoed_rate` (solid) at `1K` and `10K` target rates |
| **03** | **Max Throughput Ceiling (Closed-Loop) vs. Connections (`50` to `10K`)** | `S1_CONN_MAX` | Connections (`50` to `10,000`) | `echoed_rate` (and `actual_send_rate`) in `--mode closed-loop` |
| **04** | **Offered Load Sweep (`500` to `50K`) — Sent vs. Completed Throughput** | `S2_RATE` & `S2_RATE_HIGHCONN` | Target Rate (`500` to `50,000` msgs/s) | 2 panels (`500` conns and `5000` conns): `actual_send_rate` (dashed) vs `echoed_rate` (solid) |
| **05** | **Offered Load Sweep (`500` to `50K`) — Latency (`p50 / p95 / p99`)** | `S2_RATE` & `S2_RATE_HIGHCONN` | Target Rate (`500` to `50,000` msgs/s) | 2 panels (`500` conns and `5000` conns): `app_rtt_p50_us`, `app_rtt_p95_us`, `app_rtt_p99_us` (log scale) |
| **06** | **Active-Share Sweep (`5%` to `100%` Active at `900`, `5K`, `10K` Conns)** | `S3_ACTIVE` | Active Share (`0.05, 0.1, 0.25, 0.5, 1.0`) | 3 panels (`900`, `5000`, `10000` conns): `app_rtt_p99_us` and `echoed_rate` |
| **07** | **Pipeline Window Scaling (`W = 1, 2, 4, 8`)** | `S4_PIPE` & `S4_PIPE_MAX` | Pipeline Window (`1, 2, 4, 8`) | Left panel: `echoed_rate` (at `25K` open-loop & `closed-loop`)<br>Right panel: `app_rtt_p99_us` |
| **08** | **Arrival Distribution & Burst Intensity (`constant`, `poisson`, `burst2x`, `burst`, `burst10x`)** | `S5_DIST` | Distribution (`constant`, `poisson`, `burst2x`, `burst`, `burst10x`) | Grouped bars for `app_rtt_p50_us`, `app_rtt_p99_us`, and `e2e_lat_p99_us` (at `500` conns `@15K` and `5000` conns `@10K`) |
