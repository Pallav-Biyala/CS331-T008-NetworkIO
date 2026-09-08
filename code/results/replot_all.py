import os
import json
import matplotlib.pyplot as plt
import numpy as np

results_dir = "/home/hemcharan/Desktop/CS331-T008-NetworkIO/code/results"
servers = ["select", "poll", "epoll", "iouring", "iouring_sqpoll"]
conns = [10, 50, 100, 200, 400, 800, 1600, 3200, 4000, 4500, 5000]
str_conns = [str(c) for c in conns]

with open(os.path.join(results_dir, 'parsed.json'), 'r') as f:
    data = json.load(f)

def get_data(metric, use_null=False, clamp_min=None):
    res = {}
    for s in servers:
        res[s] = []
        for c in str_conns:
            val = data[s][c].get(metric)
            if val is None or np.isnan(val): 
                res[s].append(np.nan if use_null else 0)
            else: 
                if clamp_min is not None: val = max(val, clamp_min)
                res[s].append(val)
    return res

# 1. Throughput (X: Log, Y: Linear) -> Best for visualizing raw capacity cliffs
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

# 2. Latency (X: Log, Y: Log) -> Best because we care about <1ms differences AND 300ms spikes
plt.figure(figsize=(10, 6))
lat_99 = get_data("lat_99", use_null=True, clamp_min=0.01) 
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

# 3. CPU (X: Log, Y: Linear) -> Best because it operates in a fixed 0-100% bound
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

# 4. Memory (X: Log, Y: Log) -> Best because allocations jump from 100KB to 300,000KB
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

