import csv
import os
import matplotlib.pyplot as plt

# Load data from the unified CSV in the root directory
csv_path = 'unified_benchmarks.csv'
data = []

try:
    with open(csv_path, 'r', encoding='utf-8') as f:
        reader = csv.DictReader(f)
        for row in reader:
            data.append({
                'Paradigm': row['Paradigm'],
                'Concurrency': int(row['Concurrency']),
                'Throughput': float(row['Throughput_Mbps']) if row['Throughput_Mbps'] else 0.0,
                'Latency': float(row['P99_Latency_ms']) if row['P99_Latency_ms'] else 0.0,
                'Syscalls': int(row['Total_Syscalls']) if row['Total_Syscalls'] else 0
            })
except FileNotFoundError:
    print(f"Error: {csv_path} not found. Run generate_unified_csv.py first.")
    exit()

# Group data by paradigm and sort by concurrency
paradigms = sorted(list(set(d['Paradigm'] for d in data)))
grouped = {
    p: sorted([d for d in data if d['Paradigm'] == p], key=lambda x: x['Concurrency'])
    for p in paradigms
}

# --- Automated Terminal Analysis ---
print("\n[ANALYSIS REPORT: KEY SYSTEM FINDINGS]")
print("-" * 50)

print("1. Maximum Syscall Overhead (at 5,000 Concurrency):")
for p, rows in grouped.items():
    high_load = next((r for r in rows if r['Concurrency'] == 5000), rows[-1])
    print(f"   - {p.ljust(15)}: {high_load['Syscalls']:>6} syscalls (at {high_load['Concurrency']} conns)")

print("\n2. P99 Latency Degradation (at 5,000 Concurrency):")
for p, rows in grouped.items():
    high_load = next((r for r in rows if r['Concurrency'] == 5000), rows[-1])
    print(f"   - {p.ljust(15)}: {high_load['Latency']:>6.1f} ms")

print("-" * 50 + "\n")

# --- Generate Graphs ---
output_dir = 'results/graphs'
os.makedirs(output_dir, exist_ok=True)

plt.style.use('seaborn-v0_8-whitegrid' if 'seaborn-v0_8-whitegrid' in plt.style.available else 'default')
colors = {'epoll': '#1f77b4', 'iouring': '#ff7f0e', 'iouring_sqpoll': '#2ca02c', 'poll': '#d62728', 'select': '#9467bd'}

# Chart 1: Syscalls vs Concurrency
plt.figure(figsize=(9, 5))
for p, rows in grouped.items():
    plt.plot([r['Concurrency'] for r in rows], [r['Syscalls'] for r in rows], marker='o', linewidth=2, label=p, color=colors.get(p))
plt.title('System Call Overhead vs. Concurrency', fontsize=12, fontweight='bold')
plt.xlabel('Connection Concurrency', fontsize=10)
plt.ylabel('Total System Calls', fontsize=10)
plt.legend(title='Paradigm')
plt.tight_layout()
plt.savefig(os.path.join(output_dir, 'new_syscalls_vs_concurrency.png'), dpi=300)
plt.close()

# Chart 2: Latency vs Concurrency
plt.figure(figsize=(9, 5))
for p, rows in grouped.items():
    plt.plot([r['Concurrency'] for r in rows], [r['Latency'] for r in rows], marker='o', linewidth=2, label=p, color=colors.get(p))
plt.title('P99 Latency vs. Concurrency', fontsize=12, fontweight='bold')
plt.xlabel('Connection Concurrency', fontsize=10)
plt.ylabel('P99 Latency (ms)', fontsize=10)
plt.legend(title='Paradigm')
plt.ylim(bottom=0)
plt.tight_layout()
plt.savefig(os.path.join(output_dir, 'new_latency_vs_concurrency.png'), dpi=300)
plt.close()

print(f"New plots successfully saved to '{output_dir}/'")
