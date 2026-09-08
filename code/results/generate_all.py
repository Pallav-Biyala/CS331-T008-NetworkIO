import os
import re
import json
import matplotlib.pyplot as plt
import numpy as np

results_dir = "/home/hemcharan/Desktop/CS331-T008-NetworkIO/code/results"
servers = ["select", "poll", "epoll", "iouring", "iouring_sqpoll"]
conns = [10, 50, 100, 200, 400, 800, 1600, 3200, 4000, 4500, 5000]
str_conns = [str(c) for c in conns]

# 1. PARSE RESULTS
data = {s: {c: {} for c in str_conns} for s in servers}

def extract_throughput(file):
    try:
        with open(file, 'r') as f:
            for line in f:
                if "Aggregate bandwidth:" in line:
                    m = re.search(r"Aggregate bandwidth: ([\d.]+)↓, ([\d.]+)↑ Mbps", line)
                    if m: return float(m.group(1)), float(m.group(2))
    except Exception: pass
    return None, None

def extract_latency(file):
    try:
        with open(file, 'r') as f:
            for line in f:
                if "Message latency at percentiles:" in line:
                    m = re.search(r"percentiles: ([\d.]+)/([\d.]+)/([\d.]+)/", line)
                    if m: return float(m.group(1)), float(m.group(2)), float(m.group(3))
    except Exception: pass
    return None, None, None

def extract_cpu(file):
    try:
        with open(file, 'r') as f:
            for line in f:
                if line.startswith("Average:"):
                    parts = line.split()
                    if len(parts) >= 8: return float(parts[7])
    except Exception: pass
    return None

def extract_mem(file_before, file_after):
    try:
        with open(file_before, 'r') as f: before = int(f.read().strip())
        with open(file_after, 'r') as f: after = int(f.read().strip())
        return after - before
    except Exception: return None

def extract_context_switches(file):
    try:
        with open(file, 'r') as f:
            for line in f:
                if "context-switches" in line:
                    return int(line.split()[0].replace(',', ''))
    except Exception: pass
    return None

for srv in servers:
    for c in str_conns:
        tp_down, tp_up = extract_throughput(os.path.join(results_dir, "throughput", f"{srv}_c{c}_tcpkali.log"))
        lat_50, lat_95, lat_99 = extract_latency(os.path.join(results_dir, "throughput", f"{srv}_c{c}_latency_tcpkali.log"))
        cpu = extract_cpu(os.path.join(results_dir, "perf", f"{srv}_c{c}_pidstat.log"))
        mem = extract_mem(os.path.join(results_dir, "perf", f"{srv}_c{c}_rss_before.txt"), 
                          os.path.join(results_dir, "perf", f"{srv}_c{c}_rss_after.txt"))
        cs = extract_context_switches(os.path.join(results_dir, "perf", f"{srv}_c{c}_perf.txt"))
        data[srv][c] = {"tp_down": tp_down, "tp_up": tp_up, "lat_50": lat_50, "lat_95": lat_95, "lat_99": lat_99, "cpu": cpu, "mem": mem, "cs": cs}

with open(os.path.join(results_dir, 'parsed.json'), 'w') as f:
    json.dump(data, f, indent=2)

# 2. GENERATE PLOTS
def get_data(metric, use_null=False):
    res = {}
    for s in servers:
        res[s] = []
        for c in str_conns:
            val = data[s][c].get(metric)
            if val is None: res[s].append(np.nan if use_null else 0)
            else: res[s].append(val)
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
plt.savefig(os.path.join(results_dir, 'throughput_plot.png'))
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
plt.savefig(os.path.join(results_dir, 'latency_plot.png'))
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
plt.savefig(os.path.join(results_dir, 'cpu_plot.png'))
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
plt.savefig(os.path.join(results_dir, 'memory_plot.png'))
plt.close()

# 3. GENERATE MARKDOWN
def fmt(val, template="{}", null_val="*N/A*"): return template.format(val) if val is not None else null_val

md = []
md.append("# Network I/O Server Performance Analysis\n")
md.append("This document provides a comprehensive breakdown of the performance metrics across different Network I/O multiplexing strategies (`select`, `poll`, `epoll`, `io_uring`, and `io_uring` with `SQPOLL`).\n")
md.append("## 1. Metrics Tables\n")

# Throughput Table
md.append("### Throughput (Mbps) - Downlink ↓ / Uplink ↑")
md.append("* **Calculation/Source:** Extracted directly from the `tcpkali` tool logs located in `results/throughput/*_tcpkali.log`. The benchmark tool outputs a line `Aggregate bandwidth: X↓, Y↑ Mbps` at the end of its 30-second run. We extract the `X` (downlink) and `Y` (uplink) values which represent the raw application-layer payload throughput achieved by the server.\n")
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
md.append("### Latency (ms) - 50th / 95th / 99th Percentile")
md.append("* **Calculation/Source:** Also extracted from the `tcpkali` logs in `results/throughput/*_latency_tcpkali.log`. The tool outputs a summary line `Message latency at percentiles: 17.5/67.8/297.7/297.7 ms (50/95/99/99.9%)`. We extract the first three values which represent the 50th (median), 95th, and 99th percentile round-trip times (RTT). This measures the time from the client sending a message until it receives the server's response.\n")
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
md.append("* **Calculation/Source:** Monitored using the `pidstat` tool running in the background during the benchmark and saved to `results/perf/*_pidstat.log`. We parse the final `Average:` line printed by `pidstat` at the end of the test. We extract the `%CPU` column (the 8th column), which is calculated as `%usr + %system`. This percentage represents the total time a **single CPU core** was saturated by the server process (e.g., 94.0% means the process used 94% of one core's capacity).\n")
md.append("![CPU Plot](./cpu_plot.png)\n")
md.append("| Connections | `select` | `poll` | `epoll` | `io_uring` | `io_uring` (sqpoll) |")
md.append("|-------------|----------|--------|---------|------------|---------------------|")
for c in str_conns:
    row = f"| **{c}** | " + " | ".join([fmt(data[s][c].get('cpu'), "{:.1f}%") for s in servers]) + " |"
    md.append(row)
md.append("\n")

# Memory Table
md.append("### Memory Usage (RSS Delta in KB)")
md.append("* **Calculation/Source:** Before and after the 30-second benchmark, a script read the Resident Set Size (RSS) directly from the kernel via `/proc/<pid>/statm`. These values are saved in `results/perf/*_rss_before.txt` and `*_rss_after.txt`. The values are converted to Kilobytes, and the final metric is the formula `RSS_After_Test - RSS_Before_Test`. This represents the net memory growth (heap allocations, kernel buffers mapped to user space, or leaks) during the load test.\n")
md.append("![Memory Plot](./memory_plot.png)\n")
md.append("| Connections | `select` | `poll` | `epoll` | `io_uring` | `io_uring` (sqpoll) |")
md.append("|-------------|----------|--------|---------|------------|---------------------|")
for c in str_conns:
    row = f"| **{c}** | " + " | ".join([fmt(data[s][c].get('mem'), "{}") for s in servers]) + " |"
    md.append(row)
md.append("\n")

# Context Switches Table
md.append("### Context Switches (per 10s benchmark)")
md.append("* **Calculation/Source:** Profiled using the `perf stat -e context-switches` command attached to the server process for a fixed 10-second window while under maximum load. The raw count is extracted from the `results/perf/*_perf.txt` logs. This counts how many times the kernel had to swap the server process on and off the CPU, indicating scheduling overhead and event-loop blocking.\n")
md.append("| Connections | `select` | `poll` | `epoll` | `io_uring` | `io_uring` (sqpoll) |")
md.append("|-------------|----------|--------|---------|------------|---------------------|")
for c in str_conns:
    row = f"| **{c}** | " + " | ".join([fmt(data[s][c].get('cs'), "{}") for s in servers]) + " |"
    md.append(row)
md.append("\n---\n")

# Theoretical
md.append("## 2. Theoretical Expectations vs. Practical Results\n")
md.append("### 1. `select`\n* **Theoretical Expectation:** O(N) complexity where N is the highest file descriptor. Suffers from high overhead due to copying fd sets between user and kernel space on every invocation. Hard limited to 1024 connections due to the `FD_SETSIZE` macro in Linux.\n* **Practical Result:** Exactly matches theory regarding scalability. It **fails entirely at >= 1024 connections**. However, at low scale (10-100 connections), it achieves the highest raw throughput. As connections approach its limit, throughput degrades significantly.\n")
md.append("### 2. `poll`\n* **Theoretical Expectation:** Similar O(N) complexity to `select` (must traverse the entire array of fds), but removes the hard 1024 limit by using a dynamically sized array. Expected to scale poorly in CPU/latency as connections increase.\n* **Practical Result:** Matches theory. It successfully handles 5000 connections, but performance collapses under the O(N) traversal weight. Throughput drops severely at high scales, CPU maxes out at nearly 100%, and latency jumps massively.\n")
md.append("### 3. `epoll`\n* **Theoretical Expectation:** O(1) or O(active events) complexity. Highly scalable as it only returns fds that are actually ready, avoiding the O(N) traversal. Uses memory mapping to avoid copying fd sets per syscall. Expected to be the standard high-performance option.\n* **Practical Result:** Performs exceptionally well across most scales with very consistent throughput and low latency. However, at extreme connection density (4000-5000), it hits a bottleneck where tail latency spikes catastrophically. This suggests that `epoll_wait` wakeups or lock contention in the event loop is causing tail latency outliers despite theoretical O(1) scaling.\n")
md.append("### 4. `io_uring` (Standard)\n* **Theoretical Expectation:** Asynchronous I/O framework that uses shared memory ring buffers between user space and kernel to submit I/O requests and reap completions. Completely avoids syscall overhead (zero copy of fd lists, batched submissions). Expected to outperform `epoll` at high concurrency.\n* **Practical Result:** At lower connection counts, throughput is slightly lower than `epoll`. However, its latency characteristics are vastly superior under load (4000-5000 conns). It achieves this while using less CPU than epoll. This perfectly validates the theoretical advantage of batched asynchronous completion avoiding event-loop jitter.\n")
md.append("### 5. `io_uring` with SQPOLL (Submission Queue Polling)\n* **Theoretical Expectation:** Uses a dedicated kernel thread to poll the submission queue. This entirely eliminates the `io_uring_enter` syscall. User-space pushes to the ring, and the kernel immediately picks it up. Expected to have the absolute highest throughput and lowest latency at the cost of tying up one CPU core at 100%.\n* **Practical Result:** Extremely polarized. At low load, it performs terribly due to thrashing. But at high load (3200+ conns), the theoretical benefits materialize. It achieves the highest throughput and lowest median latency, though tail latency can jitter due to scheduling.\n")
md.append("### Conclusion\n* For low-to-medium connection counts (<1000), `epoll` remains the most balanced and efficient tool.\n* `io_uring` (standard) provides much more stable and predictable tail latency under extreme load (5000+ conns) compared to `epoll`.\n* `io_uring_sqpoll` should **never** be used for light workloads due to massive spinning overhead, but it offers the highest absolute peak performance and median latency under massive saturation.\n")

# Raw Metrics Dump
md.append("## 3. Raw Metrics Data\n")
md.append("| Server | Connections | tp_down (Mbps) | tp_up (Mbps) | lat_50 (ms) | lat_95 (ms) | lat_99 (ms) | cpu (%) | mem (KB) | context_switches |")
md.append("|--------|-------------|----------------|--------------|-------------|-------------|-------------|---------|----------|------------------|")
for s in servers:
    for c in str_conns:
        d = data[s][c]
        row = f"| {s} | {c} | {d.get('tp_down')} | {d.get('tp_up')} | {d.get('lat_50')} | {d.get('lat_95')} | {d.get('lat_99')} | {d.get('cpu')} | {d.get('mem')} | {d.get('cs')} |"
        md.append(row)

with open(os.path.join(results_dir, 'metrics_analysis.md'), 'w') as f:
    f.write('\n'.join(md))

