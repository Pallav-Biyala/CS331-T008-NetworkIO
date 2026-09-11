# AI Documentation:
---
### 1) Tools Used:
- ChatGPT
- Claude

Used to understand the benchmarking tools (tcpkali, pidstat, strace), build and debug the shell scripts that drive them and parse output into CSV, and interpret the resulting throughput/latency/CPU data.

---

### 2) Thought Process:
AI supported understanding, scripting, and interpretation — not experimental design or judgement calls. Learn what each tool measures → use AI to write/debug the driving and parsing scripts → catch silent parsing errors → sanity-check unexpected results → characterize the test environment.

---

### 3) Step by Step Details:

**Stage 1 — Background:** Four TCP echo servers (`select`, `poll`, `epoll`, `io_uring`) sharing the same protocol logic, differing only in I/O multiplexing model.

**Stage 2 — Understanding the Tools:** Learned tcpkali's flags and output format, pidstat's alternating `-u`/`-w` tables and need for `stdbuf -oL`, and strace's `-c` summary table.
> *Prompt (ChatGPT):* "Explain tcpkali's flags to me — connect-rate, -c, -m, -T — and what changes if I omit -r versus set -r 100. Also what do --latency-marker and --latency-percentiles actually do, and what does the summary output look like line by line?"
> *Prompt (ChatGPT):* "Walk me through pidstat -u -w -p <PID> 1 output — why does it print -u and -w as separate tables with a repeated timestamp header each second instead of one merged row? Also why would output go missing if I kill the process — is that a buffering issue?"
> *Prompt (Claude):* "For strace -c on a server process — does it matter if I launch the server as strace's child versus attach to an already-running PID? Will I lose setup-time syscalls either way? And why shouldn't I run this at the same time as the throughput/latency load?"

**Stage 3 — Building the Scripts:** Wrote `throughput_bench.sh`, `latency_bench.sh`, `pidstat_bench.sh`, `strace_bench.sh`, with explicit `NaN` rows for untestable server/N combinations.
> *Prompt (Claude):* "I need throughput_bench.sh, latency_bench.sh, pidstat_bench.sh, and strace_bench.sh to sweep N = 10..10000 across four servers, background pidstat during each run, and write everything into throughput.csv, latency.csv, syscalls.csv with a consistent server,N,... schema. server_select should get explicit NaN rows at N=5000/10000 instead of being skipped."

**Stage 4 — Debugging:** Found and fixed a bandwidth regex bug, a `set -e` bug that killed the sweep after the first failure, a pidstat column-layout bug, and a percentile regex bug pulling the wrong number.
> *Prompt (ChatGPT):* "My bandwidth_down_mbps and bandwidth_up_mbps columns are coming out identical in the CSV — here's the tcpkali output and the regex I'm using, what's wrong?"
> *Prompt (ChatGPT):* "My sweep script only ever produces server_select rows in the CSV even though it's supposed to loop over four servers — here's the script, why would it stop after the first server?"
> *Prompt (Claude):* "pidstat's column positions seem to shift depending on the row — should I parse by fixed column index or by header label? Here's a sample of the raw output."
> *Prompt (ChatGPT):* "My p50 latency values look suspiciously close to my N values in the CSV — here's a batch of the raw tcpkali output and the regex I used to pull percentiles, can you check if it's matching the wrong number?"

**Stage 5 — Running the Sweeps:** Ran across N = 10 to 10,000 for all four servers, distinguished expected long-running silence from real hangs, and characterized the test environment (Ubuntu, ARM64, 4 vCPUs, Apple Silicon–virtualized, `PREEMPT_DYNAMIC`).
> *Prompt (Claude):* "Our throughput numbers don't show epoll or io_uring clearly beating select/poll at low N — is that expected, or does it suggest something's wrong with the test setup?"
> *Prompt (ChatGPT):* "Here's my uname -a and /proc/cpuinfo output — summarize the architecture, core/NUMA layout, kernel config, and any relevant CPU vulnerability mitigations, and flag anything that would affect how io_uring benchmark results should be interpreted."

---

### 4) Role of AI in the Final Project

Used for: conceptual understanding of tcpkali/pidstat/strace, shell-script design, CSV parsing and regex debugging, distinguishing real failures from expected behavior, result interpretation, and environment characterization. The team remained responsible for the implementations, running sweeps, validating raw logs against parsed CSVs, and final reporting decisions.

---
