import os
import re
import csv
from collections import defaultdict

# Define the paths to your log directories
BASE_DIR = "results"
DIRS = {
    "throughput": os.path.join(BASE_DIR, "throughput"),
    "perf": os.path.join(BASE_DIR, "perf"),
    "strace": os.path.join(BASE_DIR, "strace")
}

# Regex patterns to extract the exact metrics
REGEX_THROUGHPUT = re.compile(r"Aggregate bandwidth:.*?,\s*([\d\.]+)↑\s*Mbps")
REGEX_LATENCY = re.compile(r"Message latency at percentiles:\s*[\d\.]+/[\d\.]+/([\d\.]+)/")
REGEX_PERF = re.compile(r"^\s*([\d,]+)\s+context-switches", re.MULTILINE)
REGEX_STRACE = re.compile(r"^\s*100\.00\s+[\d\.]+\s+\d+\s+(\d+)\s+.*total", re.MULTILINE)

# Data structure: data[paradigm][concurrency] = { metrics... }
data = defaultdict(lambda: defaultdict(dict))

def parse_filename(filename):
    """Extracts paradigm and concurrency from filenames like 'epoll_c1000_perf.txt'"""
    match = re.match(r"^([a-zA-Z_]+)_c(\d+)_", filename)
    if match:
        return match.group(1), int(match.group(2))
    return None, None

def read_file(filepath):
    try:
        with open(filepath, 'r', encoding='utf-8') as f:
            return f.read()
    except FileNotFoundError:
        return ""

print("Crawling benchmark logs...")

# 1. Parse Throughput and Latency
if os.path.exists(DIRS["throughput"]):
    for filename in os.listdir(DIRS["throughput"]):
        paradigm, concurrency = parse_filename(filename)
        if not paradigm: continue
        
        filepath = os.path.join(DIRS["throughput"], filename)
        content = read_file(filepath)
        
        if "latency" in filename:
            match = REGEX_LATENCY.search(content)
            if match:
                data[paradigm][concurrency]["p99_latency_ms"] = match.group(1)
        elif "tcpkali" in filename:
            match = REGEX_THROUGHPUT.search(content)
            if match:
                data[paradigm][concurrency]["throughput_mbps"] = match.group(1)

# 2. Parse Perf (Context Switches)
if os.path.exists(DIRS["perf"]):
    for filename in os.listdir(DIRS["perf"]):
        paradigm, concurrency = parse_filename(filename)
        if not paradigm or "perf" not in filename: continue
        
        content = read_file(os.path.join(DIRS["perf"], filename))
        match = REGEX_PERF.search(content)
        if match:
            # Remove commas from numbers (e.g., '1,234' -> '1234')
            data[paradigm][concurrency]["context_switches"] = match.group(1).replace(",", "")

# 3. Parse Strace (Total Syscalls)
if os.path.exists(DIRS["strace"]):
    for filename in os.listdir(DIRS["strace"]):
        paradigm, concurrency = parse_filename(filename)
        if not paradigm or "strace" not in filename: continue
        
        content = read_file(os.path.join(DIRS["strace"], filename))
        match = REGEX_STRACE.search(content)
        if match:
            data[paradigm][concurrency]["total_syscalls"] = match.group(1)

# Generate the CSV
output_file = "unified_benchmarks.csv"
headers = ["Paradigm", "Concurrency", "Throughput_Mbps", "P99_Latency_ms", "Context_Switches", "Total_Syscalls"]

print(f"Writing parsed data to {output_file}...")

with open(output_file, 'w', newline='', encoding='utf-8') as csvfile:
    writer = csv.DictWriter(csvfile, fieldnames=headers)
    writer.writeheader()
    
    # Sort by paradigm, then by concurrency
    for paradigm in sorted(data.keys()):
        for concurrency in sorted(data[paradigm].keys()):
            row = data[paradigm][concurrency]
            writer.writerow({
                "Paradigm": paradigm,
                "Concurrency": concurrency,
                "Throughput_Mbps": row.get("throughput_mbps", ""),
                "P99_Latency_ms": row.get("p99_latency_ms", ""),
                "Context_Switches": row.get("context_switches", ""),
                "Total_Syscalls": row.get("total_syscalls", "")
            })

print("Done! Data extraction complete.")
