# AI Documentation:
---
### 1) Tools Used:
The AI tools used by me in this project are:
- ChatGPT
- Claude

The primary use was to understand the benchmarking tools themselves (tcpkali, pidstat, strace), build and debug the shell scripts that drive them and parse their output into CSV, and interpret the resulting throughput/latency/CPU data — including cases where the results didn't match the naive theoretical expectation.

---


### 2) Thought Process:
AI was used as a supporting tool for understanding, scripting, and interpretation — not as a replacement for the team's own experimental design or judgement calls.

The general workflow was:

- Understand what's being compared (four servers differing only in I/O multiplexing model) as background context.
- Learn how tcpkali, pidstat, and strace each generate load or report measurements, and what their raw output actually looks like.
- Use AI to help write and debug the shell scripts that invoke those tools, parse their output, and save it to CSV.
- Run the sweeps, inspect the raw output, and catch cases where parsing was silently wrong.
- Discuss unexpected or counter-intuitive results with AI rather than accepting them at face value.
- Characterize the test environment itself, since it directly affects how results should be interpreted.

AI was particularly useful for tracing subtle bugs in output parsing, and for pushing back on assumptions (e.g. "epoll/io_uring must be faster") that the raw numbers didn't actually support.

---

### 3) Step by Step Details:

### Stage 1 — Background

Four TCP echo servers (`server_select`, `server_poll`, `server_epoll`, `server_iouring`) sharing the same protocol logic, differing only in I/O multiplexing model, formed the object of comparison. This was established quickly as context; the main learning effort with AI went into the benchmarking tools and the scripting/parsing pipeline used to measure them (Stage 2 onward)

### Stage 2 — Understanding the Benchmarking Tools

AI was used to build a working understanding of the three measurement tools driving the whole pipeline, since getting correct data depended on knowing exactly what each tool reports and how:
- **tcpkali** — how it generates load (`--connect-rate`, `-c`, `-m`, `-T`), the difference between letting message rate run uncapped (`-r` omitted, for throughput) versus fixing it (`-r 100`, for latency), why `--latency-marker` and `--latency-percentiles` are required for it to report latency at all, and the exact structure of its summary output (aggregate bandwidth line, packet-rate line, `percentiles: X/Y/Z ms (50/95/99%)` line) that the parsing scripts depend on
- **pidstat** — how `-u -w -p <PID> 1` samples CPU% and voluntary/involuntary context switches once per second, why it prints `-u`/`-w` as separate alternating tables with a repeated timestamp header each interval (rather than one merged row), and why its output needs `stdbuf -oL` to flush live instead of sitting buffered in memory until a clean exit
- **strace** — using `strace -c` to get a per-syscall summary table (calls, time, errors) for a server process, why launching the server as strace's direct child (rather than attaching to an already-running PID) is needed to also capture setup-time syscalls, and why it wasn't run concurrently with the throughput/latency load (its overhead would distort those numbers)

### Stage 3 — Implementing the Scripts: Driving Load, Parsing, and Saving to CSV

AI helped write and iterate on the benchmarking scripts (`throughput_bench.sh`, `latency_bench.sh`, `pidstat_bench.sh`, and `strace_bench.sh`), covering the full pipeline from invocation to stored data:
- invoking tcpkali with the right flags per test type (throughput: uncapped rate; latency: fixed rate + latency marker + percentiles), and backgrounding pidstat alongside each run
- extracting the needed fields out of raw tool output — bandwidth and packet-rate numbers from tcpkali, CPU%/context-switch numbers from pidstat's header-labeled tables, percentile latencies from tcpkali's percentile line, syscall counts from strace -c's summary table
- structuring that extracted data into CSV rows (`throughput.csv`, `latency.csv`, `syscalls.csv`) with a consistent schema (`server,N,...`) across all four servers and all N values
- deciding to write explicit `NaN` rows (rather than 0, blank, or omitting the row entirely) for combinations like `server_select` at N=5000/10000, so the CSV kept a complete server × N grid for later plotting rather than silently missing rows

### Stage 4 — Debugging the Scripts

Several real bugs were found and fixed with AI's help by cross-checking parsed CSV output against the raw logs, rather than trusting the numbers at face value:
- a bandwidth regex bug where downlink and uplink both matched the same trailing number, making bandwidth_down_mbps and bandwidth_up_mbps identical in early CSVs
- a `set -e` bug that silently killed the whole sweep after server_select's first failed run, so early CSVs only ever contained server_select rows
- a pidstat parsing bug caused by an incorrect assumed column layout (fixed via header-label detection instead of fixed positions), plus a separate stdio-buffering issue where unflushed pidstat output was lost on kill (fixed with `stdbuf -oL`)
- a latency-percentile regex bug where a "contains the percentile number anywhere on the line" approach silently matched unrelated numbers — N values, byte counts, or a wrong ms number — instead of the actual percentile value, producing confident-looking but wrong data (e.g. p50 equal to the N value). This was traced by noticing the extracted numbers scaled suspiciously with N, then fixed by parsing tcpkali's single `percentiles: X/Y/Z ms (50/95/99%)` line positionally instead of per-percentile
This reinforced a general practice of treating any AI-suggested regex or parsing logic as provisional until verified against the actual raw log format, since a plausible-looking fix can produce fabricated-but-clean values instead of an honest NaN.

### Stage 5 — Running the Sweeps

Each script runs across N = {10, 100, 1000, 1500,.....,10000} concurrent connections for all four servers (server_select skipping N=5000/10000 due to its hard fd cap, with NaN rows kept for grid completeness). AI helped distinguish expected long-running silence in the scripts (redirected tcpkali output, ~13s per run × 18 runs) from an actual hang, by checking the raw per-run logs for real data rather than assuming a failure from the lack of terminal output.

**Test environment**, characterized with AI's help since it directly affects interpretation:
- Ubuntu Linux, kernel 7.0.0-31-generic
- ARM64 / aarch64 architecture, 4 vCPUs (1 thread/core, 1 NUMA node), running in an Apple Silicon–based virtualized environment
- `PREEMPT_DYNAMIC` kernel configuration
- Most listed CPU vulnerabilities not affected; notable exceptions are Speculative Store Bypass (vulnerable), Spectre v1 (mitigated via `__user` pointer sanitization), and Spectre v2 (CSV2 mitigation present, BHB mitigation not present)

Because io_uring's behavior and performance are kernel-version dependent, results are described as measurements from this specific 4-vCPU ARM64 virtualized environment rather than generalized as representative of Linux or x86 server performance.

---

### 4) Role of AI in the Final Project

AI was used primarily for:
- Conceptual understanding of tcpkali, pidstat, and strace as measurement tools
- Shell-script design for driving load and sampling resource usage
- Parsing tool output into CSV and debugging parsing/regex bugs
- Distinguishing real failures from expected script behavior
- Benchmark-result interpretation, including flagging unsupported assumptions
- Test-environment characterization
The team remained responsible for the server implementations, running the actual sweeps, validating raw logs against parsed CSVs, and making final interpretation and reporting decisions.

AI-generated suggestions — especially parsing regexes and fixes — were verified against raw log output by the team before being trusted or incorporated.

---
