# Echo-server benchmark: full project context

Hand this file to an LLM that has to help with the echo-server part of the CS331 report. Everything below reflects the repository state on 2026-11-06 (branch `loadgen-bench`).

---

## 1. The project

- **Course:** CS 331 Computer Networks, Team T008, Project 13: "Scalable Network I/O: From select to io_uring".
- **Team:**

  | Name | Roll no. | Role |
  |---|---|---|
  | Chitiveli Hemcharan Varma | 2411095 | Benchmarking |
  | Gella Naga Sai Krishna | 24110122 | io_uring |
  | Guransh Kaur Saran | 24110127 | select |
  | Kowel P Laloo | 24110168 | poll |
  | Pallav Biyala | 24110234 | epoll |

- **Scope:** two TCP servers, each built with four I/O backends (select, poll, epoll, io_uring):
  - an **echo server** (this file);
  - a **file-transfer server**, which other teammates benchmarked and wrote up separately (section 9).
- **Report:** IEEE two-column format, 10-page limit excluding references. The echo sections are still placeholders: IV.B architecture, V.B implementation, VI.A testbed, VII.A evaluation and IX.B reproducibility.

## 2. Code layout

| Path | What it is |
|---|---|
| `code/core/core.c`, `core.h` | Shared server core: options, sockets, stats |
| `code/io/io_select.c`, `io_poll.c`, `io_epoll.c`, `io_uring.c` | The four backends |
| `code/apps/echo.c` | Echo application (`code/apps/filesrv.c` is the file server) |
| `code/tools/echo_loadgen.cpp` | Our custom load generator (C++17, ~1,700 lines) |
| `code/tests/echo_gate.py` | Echo correctness test |
| `Makefile` | `make echo` builds `build/echo-{select,poll,epoll,uring}`; `make loadgen` builds the load generator |
| `run_experiment.sh` | Client-side experiment runner (Rep 1 matrix) |
| `plot_results.py` | Rep 1 graphs, `summary.csv` and `validity_report.csv` |
| `flag_experiment_runbook.md` | Step-by-step guide for the server-option experiment |
| `experiment_run_workflow.md`, `linux_network_i_o_benchmark_setup_guide.md` | Offline setup and run guides |
| `scripts/install_prerequisites.sh`, `setup_server.sh`, `setup_client.sh` | Machine provisioning and tuning |
| `docs/*.pdf` | Original team design plan (planned tcpkali, 7 repeats; superseded) |
| `results/` | Raw results (section 6) |
| `plots/`, `plots/variants/` | Generated graphs (not committed) |

### Echo server design
- **Threading:** single-threaded event loop, pinned to one CPU core (`--cpu`).
- **Message framing:** fixed 512-byte frames (`--msg-size`, default 512).
- **Defaults:**
  - TCP_NODELAY on;
  - port 8080;
  - `max-conns` 10,240;
  - backlog 4,096;
  - `epoll maxevents` 256;
  - buffer size set by the application.
- **Options that really change behaviour:**
  - All backends: `--nodelay=0` (the `=` is required, because the argument is optional), `--buf-size`, `--rcvbuf`, `--sndbuf`, `--backlog`, `--accept-batch`, `--busy-poll`.
  - epoll only: `--epoll-et`, `--epoll-oneshot`, `--epoll-maxevents=N`.
  - io_uring: only `--uring-entries` and `--uring-cq-entries` are implemented. The other ~15 `--uring-*` options (`fixed-files`, `provided-buffers`, `multishot`, `sqpoll`, `send-zc`, …) are parsed but rejected with ENOTSUP by `reject_optional_flags` in `io_uring.c`.
- **select limit:** select cannot watch descriptors at or above `FD_SETSIZE` (1024), so it can't serve much more than about 1,000 connections.
- **Stats:** sending `SIGUSR1` makes the server append counters (errors, ring overflows) to the `--stats-file` JSONL file.

## 3. Our benchmarking framework (a contribution we want to stress in the report)

### Load generator (`echo_loadgen`)
- **Message header:** each message carries a 32-byte header with a nanosecond `CLOCK_MONOTONIC` timestamp, a connection ID, a sequence number and an Adler-32 checksum. This means:
  - every reply is matched to its request;
  - corrupted or mismatched replies are counted;
  - latency is measured per message.
- **Open loop** (`--mode open-loop --rate R`):
  - Messages are sent on a fixed schedule whatever the server does, which avoids coordinated omission.
  - Arrival patterns: `constant`, `poisson`, `burst2x`, `burst` (4×) and `burst10x`.
- **Closed loop** (`--mode closed-loop --pipeline-window W`):
  - Each connection always keeps W requests in flight.
  - This measures the maximum throughput (the "ceiling").
- **Other options:** `--connections`, `--active-share` (fraction of connections that send; the rest stay idle), `--warmup`, `--duration`, `--cooldown`, `--connect-timeout`, `--seed`, `--tag`, `--output` (CSV), `--json-output`, `--self-test`.
- **Connection barrier:** before measuring, it prints "Connection barrier complete: N / N established".

### CSV columns per run
- Configuration: `tag`, `mode`, `dist`, `connections`, `active_connections`, `pipeline_window`, `target_rate`.
- Rates: `actual_send_rate`, `echoed_rate`, `delivery_ratio_pct`.
- Latency: `app_rtt_p50/p95/p99_us`, `rtt_avg_us`, `e2e_lat_p50/p95/p99_us` (end-to-end, including time queued in the client).
- Health and validity: `sched_lag_*` (client lateness), `tput_1s_cv_pct`, `conn_sent_cv_pct` (fairness across connections), `client_cpu_pct`, `tcp_retrans_delta`, `conn_failures`, `send_errors`, `recv_errors`, `timeouts`, `mismatched`.
- Run details: `duration_s`, `max_fd`, `seed`.
- In closed loop, `target_rate` is reported as 0. We patched this, because it used to report a misleading rate.

### Runner (`run_experiment.sh`)
- Usage: `run_experiment.sh --mechanism M --full --rep N`. It runs on the client laptop only; the server is started manually.
- Run names follow the pattern `<mech>_rep<N>_<SUITE>_<CASE>`, with seed = 1000 + N, so every backend gets the same arrival sequence.
- Defaults: 5 s warm-up / 20 s measurement / 5 s cool-down; host 192.168.50.1:8080; 512-byte messages.
- Failed cases are written to `failed_cases.txt`. `DRY_RUN=1` prints the case list and a time estimate.
- It checks the file-descriptor limit (must be at least 21,000 for 10,000 connections).

### Plotter (`plot_results.py`)
- Produces graphs 01–08, `summary.csv` and `validity_report.csv`.
- Validity checks:
  - delivery below 99%;
  - errors greater than 0;
  - client CPU at or above 90%;
  - Little's law (closed loop): average RTT ≈ connections × W / throughput.
- Separate analysis script for the option variants (session scratchpad, not in the repo): it produces line plots `plots/variants/01_variant_throughput.png` and `02_variant_latency.png`, and prints validity checks: timing, drift, errors, Little's law, same-config repeats, and select at 10K.

## 4. Testbed
- **Machines:**
  - Laptop 1 runs the server; laptop 2 runs the client.
  - They're joined by a direct Ethernet cable, 192.168.50.0/24, server at .1.
  - Native Linux, no internet during the runs.
- **Pinning:** the server is pinned to one core, and the client is pinned via `taskset` to `CLIENT_CPU`.
- **Still to fill in:** CPU model, RAM, distribution, kernel (`uname -r`), network card and link speed (`ethtool`). We haven't collected these yet; the report needs them.
- **Note:** Wi-Fi was left on during some flag-experiment sessions.

## 5. Experiments run

### A. Rep 1 baseline (no server options), 2026-11-05
- **Timing:** 5 s warm-up / 20 s measurement / 5 s cool-down; 1 repetition.
- **Run counts:** select 41 cases (only up to 900 connections), poll, epoll and io_uring 66 each, giving 239 unique runs. poll was run twice (sessions 172543 and 201454), which gives 305 rows in total.
- **Suites:**
  - **S1_CONN:** connections {50, 100, 250, 500, 900, (2,500, 5,000, 10,000)} × offered rate {1K, 10K msgs/s}, open loop.
  - **S1_CONN_MAX:** closed loop, W = 1, at the same connection counts.
  - **S2_RATE:** 500 connections, rate {1K, 4K, 8K, 12K, 16K, 25K, 35K, 50K}.
  - **S2_RATE_HIGHCONN:** 5,000 connections, rate {1K, 4K, 8K, 16K, 25K, 50K}.
  - **S3_ACTIVE:** 900 and 10,000 connections at 5K msgs/s, active share {5, 10, 25, 50, 100}%.
  - **S4_PIPE:** 500 connections, W {1, 2, 4, 8}, at 25K msgs/s; **S4_PIPE_MAX:** the same in closed loop.
  - **S5_DIST:** 500 connections at 15K and 5,000 connections at 10K, for each arrival pattern.
  - A small PILOT set was run first (`results/pilot_*`).

### B. Server-option ("flag") experiment
- **Timing:** 10 s warm-up / 30 s measurement / 10 s cool-down; seed 1001; 1 repetition.
- **Cases per session (9):**
  - 500 connections at {1K, 25K, 50K} msgs/s;
  - 500 connections at 15K with 4× bursts;
  - closed loop at 500 connections, W = 1 and W = 8;
  - 10,000 connections at 10K open loop, and 10,000 connections closed loop;
  - a drift check (the 1K case repeated at the end of the session).
- **Sessions:**

  | Label | Backend | Option |
  |---|---|---|
  | `epolloneshot` | epoll | `--epoll-oneshot` (ran twice; 18 rows; JSON overwritten) |
  | `epollmaxev` | epoll | `--epoll-maxevents=1` |
  | `epollet` | epoll | `--epoll-et` |
  | `epollnagle` | epoll | `--nodelay=0` |
  | `epollbuf` | epoll | `--buf-size 512` |
  | `uringsmall` | io_uring | `--uring-entries 256` |
  | `uringnagle` | io_uring | `--nodelay=0` |
  | `pollnagle` | poll | `--nodelay=0` |
  | `selectnagle` | select | `--nodelay=0` |

  The planned `pollscale` session was not run.
- **Baseline:** no fresh baseline was run; the variants are compared against Rep 1 (different timing; see the issues).

## 6. Result locations
- Rep 1: `results/<mech>_rep1_<timestamp>/results.csv` plus per-run JSON and `manifest.txt`.
- Variants: `results/variants/<label>/results.csv` plus `json/`.
- Graphs:
  - `plots/01_conn_scaling_latency`, `02_conn_scaling_sent_vs_echoed`, `03_max_throughput_vs_conns`, `04_load_sweep_sent_vs_completed`, `05_load_sweep_latency`, `06_active_share_sweep`, `07_pipeline_window`, `08_arrival_distribution`;
  - `plots/variants/01_variant_throughput`, `02_variant_latency`.
- Warning: `results/README.md` is out of date. It describes a planned 5-repeat protocol that was never run.

## 7. Findings we consider valid

### Rep 1 mechanism comparison
- **epoll has the lowest latency:**
  - median 53–92 µs;
  - p99 at most ~150 µs;
  - flat from 50 to 10,000 connections.
- **io_uring:**
  - same throughput as epoll;
  - median latency 2–4× epoll's at 5,000 connections.
- **select:**
  - competitive up to 900 connections;
  - cannot go beyond about 1,000 (FD_SETSIZE).
- **Shared ceiling:** select, epoll and io_uring all stop at about 131K msgs/s in closed loop with W = 1. With W = 8 the ceiling rises to about 175K.
- **Client CPU:** at most ~43% everywhere, so the client was not the bottleneck.
- **Bursts dominate tail latency:** about 13 ms p99 with 4× bursts, versus about 100 µs with steady arrivals.

### Server-option effects (robust: much larger than the noise)
- **`--epoll-maxevents=1`** (one event per wake-up):
  - 500 connections: ceiling 131K → 58K (−56%);
  - 10,000 connections: 125K → 20K (−84%), with a median RTT of 503 ms.
- **`--buf-size 512`:** W = 8 ceiling 177K → 148K (−16%).
- **`--nodelay=0` (Nagle on), W = 8 ceiling:** epoll −3%, select −4%, io_uring −9%.
- **No measurable effect:** `--epoll-oneshot`, `--epoll-et`, `--uring-entries 256`.

### Noise levels (used as validity thresholds)
- **Throughput, same configuration repeated:** at most 0.3% difference; across sessions, at most 0.7%.
- **Steady-state latency:**
  - repeats within a session: ±4%;
  - start/end drift ratio within a session: 0.84–1.21;
  - across sessions: up to 35%.
- **Consequence:** latency differences under ~35% between sessions are not treated as findings.
- **Burst p99:** 2,561 µs vs 12,551 µs for identical runs, so it is unusable for comparisons.
- **Little's law:** ratio 1.00 for every closed-loop run except select at 10,000 connections.

## 8. Unresolved issues (report them honestly; causes are possible, not proven)

1. **poll degradation.**
   - What we saw: both Rep 1 poll runs show the same configurations getting much slower later in the session:
     - median ≈ 1.6–1.7 ms at 1,000 connections;
     - W = 8 ceiling ≈ 100K versus ~175K for the others.
   - The fresh `pollnagle` server passed its drift check even after the 10,000-connection cases.
   - Possible cause: state building up over a long server lifetime.
   - Next step: restart the server per suite; log server state.
2. **Session-level latency shifts.**
   - What we saw:
     - io_uring variants are 20–35% faster than the baseline for two unrelated options;
     - epoll p99 at 25K is about +20% in every variant, whatever the option.
   - Possible causes: the timing change (20 s vs 30 s), Wi-Fi, temperature, background load.
   - Next step: run a same-day baseline for every comparison.
3. **select at 10,000 connections.**
   - What we saw:
     - Open loop delivered only 10.19%.
     - Closed loop reported 100% delivery at 131.7K msgs/s, but Little's law implies only about 1,020 connections were really served.
     - The load generator reported 10,000 active connections and 0 failures.
   - Possible cause: the server drops connections above FD 1024 during warm-up, and the load generator doesn't count them.
   - Next step: count dropped connections.
4. **poll Rep 1 rerun at 10,000 connections:** 1,358 errors despite 100% delivery.
5. **epolloneshot was run twice under the same names.** The JSON files were overwritten, and we don't know which run had Wi-Fi off.
6. **Different timing:** Rep 1 used 5/20/5 s; the variants used 10/30/10 s.
7. **Wi-Fi was on** during some flag sessions; the effect isn't quantified.
8. **epollnagle drift ratio of 1.21** is borderline.
9. **10,000-connection closed-loop runs had 3–15 timeouts each.** Negligible.
10. **The shared 131K ceiling is unexplained.**
    - Client CPU is ruled out.
    - Possible causes: per-syscall or per-wake-up cost on the single server core (the maxevents result supports this), or the NIC/interrupt path.
    - Next step: record server CPU per run.
11. **Only one repetition,** so there are no error bars except the noise estimates above.
12. **io_uring advanced features untested,** because they aren't implemented (see section 2).

## 9. Teammates' file-server work (context for the merged report)
- **Setup:**
  - one laptop (Intel i5-10300H, 4 cores / 8 threads), WSL2, kernel 6.18.54;
  - loopback only;
  - `wrk` with 4 threads on CPUs 2–5, server on CPU 1;
  - 10/30/10 s timing; 5 repetitions, reported as median and IQR.
- **Experiments:**
  - E5a: concurrency × file size;
  - E5b: keep-alive vs close;
  - E7: chunk size and socket buffers;
  - E8: slow readers;
  - E9: TCP_NODELAY.
- **Key results:**
  - Throughput is similar across backends; poll is −9% vs epoll at 1,000 connections.
  - io_uring has 4.5× epoll's mean latency at 1,000 connections (p99 13.5 s vs 0.24 s).
  - io_uring uses 7–13% less server CPU per response, partly because of an implementation difference: one fewer copy.
  - Server core utilisation stayed at or below 48%; client CPU was not recorded.
  - Keep-alive gives +5–8%.
  - A 16 KiB chunk gives −41%; socket buffers have no effect.
  - 100 slow readers cost fast clients 7–11%.
  - Nagle on costs −62% for 4 KiB files and very little for 64 KiB.

### How the two setups relate (planned report storyline)
- **They complement each other:**
  - The file server has more repetitions; it uses loopback and the closed-loop `wrk`, which suffers from coordinated omission.
  - The echo server uses two physical machines and open-loop load, with built-in validity checks, but only one repetition.
- **Both agree:**
  - io_uring has higher latency than epoll at high connection counts. This is the strongest cross-validated result.
  - Throughput is similar across backends.
  - poll is weaker at high concurrency. For echo this is confounded by issue 1.
- **Neither found the bottleneck:**
  - The file-server team measured server CPU (≤48%); we measured client CPU (≤43%).
  - Shared next step: measure both.
- **Nagle effect differs in size (62% vs 3–9%):**
  - Possible cause: multi-write file replies trigger the Nagle plus delayed-ACK stall, whereas a single 512-byte echo write isn't delayed when nothing is unacknowledged.
  - This is a hypothesis.
- **Chunk/buffer size:** smaller is worse in both (−41% vs −16%).

## 10. Planned echo report content
- **IV.B:** architecture, plus a small load-generator diagram.
- **V.B:** per-backend implementation and the io_uring option limitation.
- **VI.A:** testbed diagram, parameter table, and a comparison table of the two setups.
- **VII.A:**
  - validity method (one paragraph);
  - mechanism comparison: latency vs connections, ceiling vs W, and a per-mechanism table of about 6 numbers;
  - load and bursts, briefly;
  - option effects as a single "% change vs baseline" bar chart;
  - a known-issues table (what we saw / possible cause / next step).
- **Shared sections:**
  - rewrite the "Overall observations" and the summary to cover both servers;
  - V.A environment: split into file (WSL2) and echo (two native machines);
  - IX.B reproducibility commands;
  - Appendix A: the template table has 3 rows but there are 5 members;
  - Appendix B: add Claude Code next to ChatGPT.
- **Budget:** about 3.5–4 pages for echo.
- **Style rules:** concise, plain wording, simple metrics; causes are always labelled "possible"; no claims built on latency differences under the noise level or on burst p99.

## 11. Repo housekeeping
- Uncommitted:
  - `plots/`;
  - an edit to `experiment_run_workflow.md` (`$HOME`, `sudo -v`);
  - execute bit on `run_experiment.sh` and `plot_results.py`;
  - `git stash@{0}`.
- `core.autocrlf=true` on the Windows clone; `.gitattributes` forces LF.
- Commit and push only when asked.
