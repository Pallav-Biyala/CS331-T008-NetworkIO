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
mem = get_data("mem", use_null=True)
for s in servers:
    # Convert to MB. For log scale, replace 0 with a very small value (e.g. 0.01) so it plots.
    y_vals = []
    for m in mem[s]:
        if np.isnan(m):
            y_vals.append(np.nan)
        else:
            mb = m / 1024.0
            y_vals.append(max(mb, 0.01))
    
    plt.plot(conns, y_vals, marker='o', label=s)

plt.title("Memory Usage vs Connections")
plt.xlabel("Connections (Log Scale)")
plt.ylabel("Memory Delta (MB) - Log Scale")
plt.xscale('log')
plt.yscale('log')
plt.grid(True)
plt.legend()
plt.savefig('/home/hemcharan/Desktop/CS331-T008-NetworkIO/code/results/memory_plot.png')
plt.close()
