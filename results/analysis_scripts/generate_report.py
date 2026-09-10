import sys, os, re
# Force UTF-8 output on Windows
if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')

import json
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np

# Derive all paths relative to this script for portability.
script_dir  = os.path.dirname(os.path.abspath(__file__))
results_dir = os.path.dirname(script_dir)           # …/results/
output_dir  = os.path.join(results_dir, 'results_v2')
graphs_dir  = os.path.join(output_dir, 'graphs')
os.makedirs(graphs_dir, exist_ok=True)
os.makedirs(output_dir, exist_ok=True)
root_dir    = os.path.dirname(results_dir)
servers = ["select", "poll", "epoll", "iouring", "iouring_sqpoll"]
conns = [10, 100, 500, 1000, 1500, 2000, 2500, 3000, 3500, 4000, 4500, 5000]
str_conns = [10, 100, 500, 1000, 1500, 2000, 2500, 3000, 3500, 4000, 4500, 5000]

# 1. PARSE RESULTS
data = {s: {c: {} for c in str_conns} for s in servers}

def extract_throughput(file):
    try:
        with open(file, 'r') as f:
            for line in f:
                if "Aggregate bandwidth:" in line:
                    m = re.search(r'Aggregate bandwidth:\s*([\d.]+)\S*,\s*([\d.]+)\S*\s*Mbps', line)
                    if m: return float(m.group(1)), float(m.group(2))
    except Exception: pass
    return 0, 0

def extract_latency(file):
    try:
        with open(file, 'r') as f:
            for line in f:
                if "Message latency at percentiles:" in line:
                    m = re.search(r'percentiles:\s*([\d.]+)/([\d.]+)/([\d.]+)/', line)
                    if m: return float(m.group(1)), float(m.group(2)), float(m.group(3))
    except Exception: pass
    return 0, 0, 0

def extract_cpu(file):
    try:
        with open(file, 'r') as f:
            for line in f:
                if line.startswith("Average:"):
                    parts = line.split()
                    if len(parts) >= 8: return float(parts[7])
    except Exception: pass
    return 0

def extract_mem(file_before, file_after):
    try:
        with open(file_before, 'r') as f: before = int(f.read().strip())
        with open(file_after, 'r') as f: after = int(f.read().strip())
        return after - before
    except Exception: return 0

def extract_context_switches(file):
    try:
        with open(file, 'r') as f:
            for line in f:
                if "context-switches" in line:
                    return int(line.split()[0].replace(',', ''))
    except Exception: pass
    return 0

for srv in servers:
    for c in str_conns:
        tp_down, tp_up = extract_throughput(os.path.join(results_dir, "throughput", f"{srv}_c{c}_tcpkali.log"))
        lat_50, lat_95, lat_99 = extract_latency(os.path.join(results_dir, "throughput", f"{srv}_c{c}_latency_tcpkali.log"))
        cpu = extract_cpu(os.path.join(results_dir, "perf", f"{srv}_c{c}_pidstat.log"))
        mem = extract_mem(os.path.join(results_dir, "perf", f"{srv}_c{c}_rss_before.txt"), 
                          os.path.join(results_dir, "perf", f"{srv}_c{c}_rss_after.txt"))
        cs = extract_context_switches(os.path.join(results_dir, "perf", f"{srv}_c{c}_perf.txt"))
        data[srv][c] = {"tp_down": tp_down, "tp_up": tp_up, "lat_50": lat_50, "lat_95": lat_95, "lat_99": lat_99, "cpu": cpu, "mem": mem, "cs": cs}

with open(os.path.join(output_dir, 'parsed.json'), 'w') as f:
    json.dump(data, f, indent=2)

# 2. GENERATE PLOTS
def get_data(metric, use_null=False):
    res = {}
    for s in servers:
        res[s] = []
        for c in str_conns:
            val = data[s][c].get(metric)
            # Only treat None as missing (file absent or parse failed).
            # A genuine zero (e.g. 0 context switches) is a real data point.
            if val is None:
                res[s].append(np.nan if use_null else 0)
            else:
                res[s].append(val)
    return res

plt.figure(figsize=(10, 6))
tp_down = get_data("tp_down", use_null=True)
for s in servers: plt.plot(conns, tp_down[s], marker='o', label=s)
plt.title("Throughput (Downlink) vs Connections")
plt.xlabel("Connections (Log Scale)")
plt.ylabel("Throughput (Mbps)")
plt.xscale('log')
plt.grid(True)
plt.legend()
plt.savefig(os.path.join(graphs_dir, 'throughput_plot.png'))
plt.close()

plt.figure(figsize=(10, 6))
lat_99 = get_data("lat_99", use_null=True)
for s in servers: plt.plot(conns, lat_99[s], marker='o', label=s)
plt.title("99th Percentile Latency vs Connections")
plt.xlabel("Connections (Log Scale)")
plt.ylabel("Latency (ms) - Log Scale")
plt.xscale('log')
plt.yscale('log')
plt.grid(True)
plt.legend()
plt.savefig(os.path.join(graphs_dir, 'latency_plot.png'))
plt.close()

plt.figure(figsize=(10, 6))
cpu_data = get_data("cpu", use_null=True)
for s in servers: plt.plot(conns, cpu_data[s], marker='o', label=s)
plt.title("CPU Utilization vs Connections")
plt.xlabel("Connections (Log Scale)")
plt.ylabel("CPU Utilization (%)")
plt.xscale('log')
plt.grid(True)
plt.legend()
plt.savefig(os.path.join(graphs_dir, 'cpu_plot.png'))
plt.close()

plt.figure(figsize=(10, 6))
mem_data = get_data("mem", use_null=True)
for s in servers:
    y_vals = [max(m/1024.0, 0.01) if not np.isnan(m) else np.nan for m in mem_data[s]]
    plt.plot(conns, y_vals, marker='o', label=s)
plt.title("Memory Usage vs Connections")
plt.xlabel("Connections (Log Scale)")
plt.ylabel("Memory Delta (MB) - Log Scale")
plt.xscale('log')
plt.yscale('log')
plt.grid(True)
plt.legend()
plt.savefig(os.path.join(graphs_dir, 'memory_plot.png'))
plt.close()

# 3. GENERATE MARKDOWN
def fmt(val, template="{}", null_val="*N/A*"): return template.format(val) if val is not None else null_val

md = []
md.append("# Network I/O Server Performance Analysis\n")
md.append(
    "This document provides a comprehensive breakdown of the performance metrics across "
    "different Network I/O multiplexing strategies (`select`, `poll`, `epoll`, `io_uring`, "
    "and `io_uring` with `SQPOLL`).\n"
)

# ── Important caveat about benchmark structure ────────────────────────────────
md.append("> **Important — three separate benchmark scripts**\n>\n"
    "> All numbers in this report come from **three isolated benchmark scripts** "
    "with different durations, connection rates, and instrumentation levels.  "
    "Metrics from different columns must **not** be interpreted as if they came "
    "from the same run:\n>\n"
    "> | Script | Measures | tcpkali duration | Note |\n"
    "> |--------|----------|-----------------|------|\n"
    "> | `bench_throughput.sh` (pass 1) | Aggregate bandwidth (Mbps) | 15 s (c<5000), 30 s (c>=5000) | No instrumentation |\n"
    "> | `bench_throughput.sh` (pass 2) | RTT latency percentiles | same | Fixed rate 20 msg/s/conn; **separate server start** from pass 1 |\n"
    "> | `bench_strace.sh` | Syscall counts | 20 s | strace `-c` attached for 10 s window; **heavy ptrace overhead** — throughput numbers from this run are not representative |\n"
    "> | `bench_perf.sh` | Context switches, CPU%, RSS | 25 s | `perf stat` for 10 s + `pidstat` 5 s; RSS-after captured mid-load (~18 s in) |\n>\n"
)
md.append("## 1. Metrics Tables\n")

md.append("### Throughput (Mbps) — Downlink (client->server) / Uplink (server->client)")
md.append(
    "* **Source:** `bench_throughput.sh` pass 1 (unlimited message rate, no instrumentation).\n"
    "  tcpkali reports `Aggregate bandwidth: X(down), Y(up) Mbps` at the end of the run.\n"
    "  Duration: **15 s** for c < 5000 connections; **30 s** for c >= 5000 connections.\n"
    "  *These numbers come from a clean run with no strace/perf overhead.*\n"
)
md.append("![Throughput Plot](./throughput_plot.png)\n")
md.append("| Connections | `select` | `poll` | `epoll` | `io_uring` | `io_uring` (sqpoll) |")
md.append("|-------------|----------|--------|---------|------------|---------------------|")
for c in str_conns:
    row = f"| **{c}** | " + " | ".join([
        f"{fmt(data[s][c].get('tp_down'), '{:.0f}')} ↓ / {fmt(data[s][c].get('tp_up'), '{:.0f}')} ↑" if data[s][c].get('tp_down') is not None else "*N/A*"
        for s in servers
    ]) + " |"
    md.append(row)
md.append("\n")

# Latency Table
md.append("### Latency (ms) — 50th / 95th / 99th Percentile")
md.append(
    "* **Source:** `bench_throughput.sh` pass 2 (`--message-rate 20` msg/s per connection).\n"
    "  This is a **separate server instance** from pass 1 — throughput and latency "
    "were measured independently because flooding the pipe (pass 1) prevents "
    "per-message RTT timestamping.  Duration: **15 s** / **30 s** (same schedule as pass 1).\n"
    "  tcpkali outputs `Message latency at percentiles: p50/p95/p99/p99.9 ms`; "
    "we extract p50, p95, and p99.\n"
)
md.append("![Latency Plot](./latency_plot.png)\n")
md.append("| Connections | `select` | `poll` | `epoll` | `io_uring` | `io_uring` (sqpoll) |")
md.append("|-------------|----------|--------|---------|------------|---------------------|")
for c in str_conns:
    row = f"| **{c}** | " + " | ".join([
        f"{fmt(data[s][c].get('lat_50'), '{:.1f}')} / {fmt(data[s][c].get('lat_95'), '{:.1f}')} / {fmt(data[s][c].get('lat_99'), '{:.1f}')}" if data[s][c].get('lat_50') is not None else "*N/A*"
        for s in servers
    ]) + " |"
    md.append(row)
md.append("\n")

# CPU Table
md.append("### CPU Utilization (%)")
md.append(
    "* **Source:** `bench_perf.sh` — `pidstat -p <pid> 1 5` run after the `perf stat` "
    "window (i.e., approximately 18 s into the 25 s load).  "
    "We parse the final `Average:` line and extract the `%CPU` column "
    "(`%usr + %system`), representing the fraction of one CPU core used by the server process.\n"
    "  *This is from a separate run from the throughput benchmark.*\n"
)
md.append("![CPU Plot](./cpu_plot.png)\n")
md.append("| Connections | `select` | `poll` | `epoll` | `io_uring` | `io_uring` (sqpoll) |")
md.append("|-------------|----------|--------|---------|------------|---------------------|")
for c in str_conns:
    row = f"| **{c}** | " + " | ".join([fmt(data[s][c].get('cpu'), "{:.1f}%") for s in servers]) + " |"
    md.append(row)
md.append("\n")

# Memory Table
md.append("### Memory Usage — RSS Delta (KB)")
md.append(
    "* **Source:** `bench_perf.sh` reads RSS via `ps -o rss= -p <pid>` (units: KB) "
    "before the load starts and again after the `perf stat` + `pidstat` windows "
    "(approximately 18 s into the 25 s tcpkali run, **before tcpkali has finished**).\n"
    "  The delta `RSS_after - RSS_before` reflects net memory growth during the "
    "measured window.  Because tcpkali is still running when RSS-after is sampled, "
    "this is **not** a final steady-state reading.\n"
    "  Note: `ps -o rss=` returns values already in KB — no unit conversion is applied.\n"
    "  *This is from a separate run from the throughput benchmark.*\n"
)
md.append("![Memory Plot](./memory_plot.png)\n")
md.append("| Connections | `select` | `poll` | `epoll` | `io_uring` | `io_uring` (sqpoll) |")
md.append("|-------------|----------|--------|---------|------------|---------------------|")
for c in str_conns:
    row = f"| **{c}** | " + " | ".join([fmt(data[s][c].get('mem'), "{}") for s in servers]) + " |"
    md.append(row)
md.append("\n")

# Context Switches Table
md.append("### Context Switches (10 s perf stat window)")
md.append(
    "* **Source:** `bench_perf.sh` — `perf stat -e context-switches -p <pid>` "
    "attached mid-run for a fixed 10 s window (starting 3 s after load begins, "
    "after connection ramp-up).  Measures how many times the kernel descheduled "
    "the server process during that window.\n"
    "  *This is from a separate run from the throughput benchmark.*\n"
)
md.append("| Connections | `select` | `poll` | `epoll` | `io_uring` | `io_uring` (sqpoll) |")
md.append("|-------------|----------|--------|---------|------------|---------------------|")
for c in str_conns:
    row = f"| **{c}** | " + " | ".join([fmt(data[s][c].get('cs'), "{}") for s in servers]) + " |"
    md.append(row)
md.append("\n---\n")

# Raw Metrics Dump
md.append("## 2. Raw Metrics Data\n")
md.append("| Server | Connections | tp_down (Mbps) | tp_up (Mbps) | lat_50 (ms) | lat_95 (ms) | lat_99 (ms) | cpu (%) | mem (KB) | context_switches |")
md.append("|--------|-------------|----------------|--------------|-------------|-------------|-------------|---------|----------|------------------|")
for s in servers:
    for c in str_conns:
        d = data[s][c]
        row = f"| {s} | {c} | {d.get('tp_down')} | {d.get('tp_up')} | {d.get('lat_50')} | {d.get('lat_95')} | {d.get('lat_99')} | {d.get('cpu')} | {d.get('mem')} | {d.get('cs')} |"
        md.append(row)

with open(os.path.join(output_dir, 'metrics_analysis.md'), 'w') as f:
    f.write('\n'.join(md))
print(f"Saved markdown report → {output_dir}/metrics_analysis.md")

