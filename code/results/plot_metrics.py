import json
import matplotlib.pyplot as plt
import numpy as np

with open('/home/hemcharan/Desktop/CS331-T008-NetworkIO/code/results/parsed.json', 'r') as f:
    data = json.load(f)

servers = ["select", "poll", "epoll", "iouring", "iouring_sqpoll"]
conns = [10, 100, 1000, 5000]
str_conns = ["10", "100", "1000", "5000"]

def get_data(metric, use_null=False):
    res = {}
    for s in servers:
        res[s] = []
        for c in str_conns:
            val = data[s][c].get(metric)
            if val is None:
                res[s].append(np.nan if use_null else 0)
            else:
                res[s].append(val)
    return res

plt.figure(figsize=(10, 6))
tp_down = get_data("tp_down", use_null=True)
for s in servers:
    plt.plot(conns, tp_down[s], marker='o', label=s)
plt.title("Throughput (Downlink) vs Connections")
plt.xlabel("Connections (Log Scale)")
plt.ylabel("Throughput (Mbps)")
plt.xscale('log')
plt.grid(True)
plt.legend()
plt.savefig('/home/hemcharan/Desktop/CS331-T008-NetworkIO/code/results/throughput_plot.png')
plt.close()

plt.figure(figsize=(10, 6))
lat_99 = get_data("lat_99", use_null=True)
for s in servers:
    plt.plot(conns, lat_99[s], marker='o', label=s)
plt.title("99th Percentile Latency vs Connections")
plt.xlabel("Connections (Log Scale)")
plt.ylabel("Latency (ms) - Log Scale")
plt.xscale('log')
plt.yscale('log')
plt.grid(True)
plt.legend()
plt.savefig('/home/hemcharan/Desktop/CS331-T008-NetworkIO/code/results/latency_plot.png')
plt.close()

plt.figure(figsize=(10, 6))
cpu = get_data("cpu", use_null=True)
for s in servers:
    plt.plot(conns, cpu[s], marker='o', label=s)
plt.title("CPU Utilization vs Connections")
plt.xlabel("Connections (Log Scale)")
plt.ylabel("CPU Utilization (%)")
plt.xscale('log')
plt.grid(True)
plt.legend()
plt.savefig('/home/hemcharan/Desktop/CS331-T008-NetworkIO/code/results/cpu_plot.png')
plt.close()

plt.figure(figsize=(10, 6))
mem = get_data("mem", use_null=True)
for s in servers:
    plt.plot(conns, [m / 1024 for m in mem[s]], marker='o', label=s) # MB
plt.title("Memory Usage vs Connections")
plt.xlabel("Connections (Log Scale)")
plt.ylabel("Memory Delta (MB)")
plt.xscale('log')
plt.grid(True)
plt.legend()
plt.savefig('/home/hemcharan/Desktop/CS331-T008-NetworkIO/code/results/memory_plot.png')
plt.close()

