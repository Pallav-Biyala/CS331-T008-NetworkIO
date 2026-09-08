import json

with open('/home/hemcharan/Desktop/CS331-T008-NetworkIO/code/results/parsed.json', 'r') as f:
    data = json.load(f)

servers = ["select", "poll", "epoll", "iouring", "iouring_sqpoll"]
connections = ["10", "100", "1000", "5000"]

print("\n## 3. Raw Metrics Data\n")
print("| Server | Connections | tp_down (Mbps) | tp_up (Mbps) | lat_50 (ms) | lat_95 (ms) | lat_99 (ms) | cpu (%) | mem (KB) | context_switches |")
print("|--------|-------------|----------------|--------------|-------------|-------------|-------------|---------|----------|------------------|")

for s in servers:
    for c in connections:
        d = data[s][c]
        row = f"| {s} | {c} | {d['tp_down']} | {d['tp_up']} | {d['lat_50']} | {d['lat_95']} | {d['lat_99']} | {d['cpu']} | {d['mem']} | {d['cs']} |"
        print(row)
