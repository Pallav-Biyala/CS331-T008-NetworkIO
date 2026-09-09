import os
import re
import pandas as pd
import matplotlib.pyplot as plt

RESULTS_DIR = '/home/hemcharan/Documents/CS331-T008-NetworkIO/results'
ENGINES = ['select', 'poll', 'epoll', 'iouring', 'iouring_sqpoll']
CONNS = [10, 100, 500, 1000, 1500, 2000, 2500, 3000, 3500, 4000, 4500, 5000]

data = []

for engine in ENGINES:
    for conn in CONNS:
        throughput = 0.0
        p99_lat = 0.0
        syscalls = 0
        ctx_switches = 0
        
        # Parse throughput
        tp_file = os.path.join(RESULTS_DIR, f"throughput/{engine}_c{conn}_tcpkali.log")
        if os.path.exists(tp_file):
            with open(tp_file, 'r') as f:
                content = f.read()
                m = re.search(r'Aggregate bandwidth: [\d.]+↓, ([\d.]+)↑ Mbps', content)
                if m:
                    throughput = float(m.group(1))
                    
        # Parse latency
        lat_file = os.path.join(RESULTS_DIR, f"throughput/{engine}_c{conn}_latency_tcpkali.log")
        if os.path.exists(lat_file):
            with open(lat_file, 'r') as f:
                content = f.read()
                m = re.search(r'Message latency at percentiles: [\d.]+/[\d.]+/([\d.]+)/[\d.]+ ms', content)
                if m:
                    p99_lat = float(m.group(1))
                    
        # Parse syscalls
        strace_file = os.path.join(RESULTS_DIR, f"strace/{engine}_c{conn}_strace.txt")
        if os.path.exists(strace_file):
            with open(strace_file, 'r') as f:
                content = f.read()
                m = re.search(r'\s+[\d.]+\s+[\d.]+\s+\d+\s+(\d+)\s+total', content)
                if m:
                    syscalls = int(m.group(1))
                    
        # Parse context switches
        perf_file = os.path.join(RESULTS_DIR, f"perf/{engine}_c{conn}_perf.txt")
        if os.path.exists(perf_file):
            with open(perf_file, 'r') as f:
                content = f.read()
                # Remove commas from the number, like '2,345'
                content_cleaned = content.replace(',', '')
                m = re.search(r'\s+(\d+)\s+context-switches', content_cleaned)
                if m:
                    ctx_switches = int(m.group(1))
                    
        data.append({
            'Engine': engine,
            'Connections': conn,
            'Throughput_Mbps': throughput,
            'P99_Latency_ms': p99_lat,
            'Syscalls': syscalls,
            'Context_Switches': ctx_switches
        })

df = pd.DataFrame(data)

# Export CSV
csv_path = os.path.join(RESULTS_DIR, 'analysis_scripts/data/all_metrics.csv')
df.to_csv(csv_path, index=False)
print(f"Saved CSV data to {csv_path}")

# Plotting
# 1. Throughput
plt.figure(figsize=(10, 6))
for engine in ENGINES:
    engine_df = df[df['Engine'] == engine]
    plt.plot(engine_df['Connections'], engine_df['Throughput_Mbps'], marker='o', label=engine)
plt.title('Throughput vs Connections')
plt.ylabel('Throughput (Mbps)')
plt.xlabel('Connections')
plt.legend()
plt.grid(True)
plt.savefig(os.path.join(RESULTS_DIR, 'graphs/throughput_vs_conns.png'))
plt.close()

# 2. Latency
plt.figure(figsize=(10, 6))
for engine in ENGINES:
    engine_df = df[df['Engine'] == engine]
    plt.plot(engine_df['Connections'], engine_df['P99_Latency_ms'], marker='o', label=engine)
plt.title('P99 Latency vs Connections (Log Scale)')
plt.ylabel('P99 Latency (ms)')
plt.xlabel('Connections')
plt.yscale('log')
plt.legend()
plt.grid(True)
plt.savefig(os.path.join(RESULTS_DIR, 'graphs/p99_latency_vs_conns.png'))
plt.close()

# 3. Syscalls
plt.figure(figsize=(10, 6))
for engine in ENGINES:
    engine_df = df[df['Engine'] == engine]
    plt.plot(engine_df['Connections'], engine_df['Syscalls'], marker='o', label=engine)
plt.title('Total Syscalls vs Connections (Log Scale, 10s Window)')
plt.ylabel('Syscalls')
plt.xlabel('Connections')
plt.yscale('log')
plt.legend()
plt.grid(True)
plt.savefig(os.path.join(RESULTS_DIR, 'graphs/syscalls_vs_conns.png'))
plt.close()

# 4. Context Switches
plt.figure(figsize=(10, 6))
for engine in ENGINES:
    engine_df = df[df['Engine'] == engine]
    plt.plot(engine_df['Connections'], engine_df['Context_Switches'], marker='o', label=engine)
plt.title('Context Switches vs Connections (Log Scale, 10s Window)')
plt.ylabel('Context Switches')
plt.xlabel('Connections')
plt.yscale('symlog') # symlog handles 0 context switches for iouring_sqpoll
plt.legend()
plt.grid(True)
plt.savefig(os.path.join(RESULTS_DIR, 'graphs/context_switches_vs_conns.png'))
plt.close()

print("Generated all plots successfully in results/graphs/")
