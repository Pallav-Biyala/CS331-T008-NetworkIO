import csv
from collections import defaultdict

records = []
try:
    with open('unified_benchmarks.csv', 'r', encoding='utf-8') as f:
        for row in csv.DictReader(f):
            if row['Throughput_Mbps'] and row['Concurrency']:
                records.append({
                    'Paradigm': row['Paradigm'],
                    'Concurrency': int(row['Concurrency']),
                    'Throughput': float(row['Throughput_Mbps']),
                    'Latency': float(row['P99_Latency_ms']) if row['P99_Latency_ms'] else 0.0,
                    'Syscalls': int(row['Total_Syscalls']) if row['Total_Syscalls'] else 0
                })
except Exception as e:
    print(f"Error reading CSV: {e}")
    exit()

paradigms = sorted(list(set(r['Paradigm'] for r in records)))

print("\n=== 1. PEAK THROUGHPUT (When did each paradigm max out?) ===")
for p in paradigms:
    p_data = [r for r in records if r['Paradigm'] == p]
    if p_data:
        peak = max(p_data, key=lambda x: x['Throughput'])
        print(f"{p.ljust(15)}: {peak['Throughput']:>8.2f} Mbps  (at {peak['Concurrency']} connections)")

print("\n=== 2. SYSCALLS AT HIGH CONCURRENCY (Overhead at 5000 conns) ===")
for p in paradigms:
    high_conn = [r for r in records if r['Paradigm'] == p and r['Concurrency'] == 5000]
    if high_conn:
        print(f"{p.ljust(15)}: {high_conn[0]['Syscalls']:>8} syscalls")
    else:
        # Fallback to the highest available if 5000 failed/wasn't tested
        max_c = max([r['Concurrency'] for r in records if r['Paradigm'] == p])
        fallback = [r for r in records if r['Paradigm'] == p and r['Concurrency'] == max_c][0]
        print(f"{p.ljust(15)}: {fallback['Syscalls']:>8} syscalls (at {max_c} conns)")

print("\n=== 3. LATENCY DEGRADATION (P99 Latency at highest load) ===")
for p in paradigms:
    high_conn = [r for r in records if r['Paradigm'] == p and r['Concurrency'] == 5000]
    if high_conn:
        print(f"{p.ljust(15)}: {high_conn[0]['Latency']:>6.1f} ms")
    else:
        max_c = max([r['Concurrency'] for r in records if r['Paradigm'] == p])
        fallback = [r for r in records if r['Paradigm'] == p and r['Concurrency'] == max_c][0]
        print(f"{p.ljust(15)}: {fallback['Latency']:>6.1f} ms (at {max_c} conns)")
print("")
