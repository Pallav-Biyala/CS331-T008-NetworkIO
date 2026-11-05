# Experiment & Graph Spec (client-only runner, 50 → 10K connections)

> Source of truth: `run_experiment.sh` (matrix) and `plot_results.py` (graphs).
> Loadgen: `code/tools/echo_loadgen.cpp` (built by `make loadgen`). The copies in `new/` are superseded.

Only three metric families are plotted:
1. **Throughput** — `echoed_rate`
2. **Sent vs. completed** — `actual_send_rate` vs `echoed_rate` (`delivery_ratio_pct` in validity report)
3. **Latency** — `app_rtt_p50_us`, `app_rtt_p95_us`, `app_rtt_p99_us`, `e2e_lat_p99_us`

`client_cpu_pct` (load-generator CPU, *not* server CPU), `tcp_retrans_delta`, `sched_lag_*` and per-second
series are **not plotted**. `client_cpu_pct ≥ 90%` is used only in `plots/validity_report.csv` to flag points where
the single-core client, not the server, was the bottleneck.

## 1. Data layout

One `./run_experiment.sh --full --mechanism <m> --rep <n>` = one backend × one repetition →
`results/<m>_rep<n>_<stamp>/results.csv` (+ `json/rep<n>_<SUITE>_<CASE>.json`).
CSV `tag` = `<m>_rep<n>_<SUITE>_<CASE>`, `m ∈ {select, poll, epoll, uring}`.
Closed-loop rows have `target_rate = 0`. Defaults: 512 B msgs, warmup 5 s, measure 20 s, seed = 1000 + rep.

## 2. Matrix (66 cases for poll/epoll/uring, 41 for select; ≈ 1 h 50 min per rep for all 4 backends)

| Suite | Connections | Load | Other |
|---|---|---|---|
| `S1_CONN` | 50, 100, 250, 500, 900 (+ 2500, 5000, 10000 non-select) | 1K and 10K msgs/s | — |
| `S1_CONN_MAX` | same | closed-loop | — |
| `S2_RATE` | 500 | 1K, 4K, 8K, 12K, 16K, 25K, 35K, 50K | — |
| `S2_RATE_HIGHCONN` | 5000 (non-select) | 1K, 4K, 8K, 16K, 25K, 50K | — |
| `S3_ACTIVE` | 900 (+ 10000 non-select) | 5K msgs/s | active share 0.05, 0.1, 0.25, 0.5, 1.0 |
| `S4_PIPE` / `S4_PIPE_MAX` | 500 | 25K open-loop / closed-loop | W = 1, 2, 4, 8 |
| `S5_DIST` | 500 @ 15K, 5000 @ 10K (non-select) | avg rate | constant, poisson, burst2x, burst (4×), burst10x |

`select` stops at 900 connections (FD_SETSIZE = 1024).

## 3. Graphs (`python3 plot_results.py results plots`)

Median across reps; error bars = min..max when > 1 rep.

| # | File | Content |
|---|---|---|
| 01 | `01_conn_scaling_latency.png` | S1_CONN: p50 (dashed) & p99 (solid) vs connections, panels 1K / 10K; line at 1024 |
| 02 | `02_conn_scaling_sent_vs_echoed.png` | S1_CONN: sent (dashed) vs echoed (solid), panels 1K / 10K |
| 03 | `03_max_throughput_vs_conns.png` | S1_CONN_MAX: closed-loop echoed_rate vs connections |
| 04 | `04_load_sweep_sent_vs_completed.png` | S2: sent vs echoed vs target, panels 500 / 5000 conns; dotted = closed-loop ceiling from S1_CONN_MAX |
| 05 | `05_load_sweep_latency.png` | S2: p50 / p95 / p99 vs target rate, rows 500 / 5000 conns |
| 06 | `06_active_share_sweep.png` | S3: p99 (top) and echoed_rate (bottom) vs active share, columns 900 / 10000 |
| 07 | `07_pipeline_window.png` | S4: echoed_rate and p99 vs W, open-loop 25K (solid) vs closed-loop (dashed) |
| 08 | `08_arrival_distribution.png` | S5: grouped bars p50 / p99 / e2e p99 per distribution, rows 500@15K / 5000@10K |
