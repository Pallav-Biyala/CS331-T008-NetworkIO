import os
import re
import csv
from glob import glob
from collections import defaultdict

results_dir = "/home/hemcharan/Desktop/CS331-T008-NetworkIO/code/results"
servers = ["select", "poll", "epoll", "iouring", "iouring_sqpoll"]
connections = [10, 100, 1000, 5000]

data = defaultdict(lambda: defaultdict(dict))

def extract_throughput(file):
    try:
        with open(file, 'r') as f:
            for line in f:
                if "Aggregate bandwidth:" in line:
                    m = re.search(r"Aggregate bandwidth: ([\d.]+)↓, ([\d.]+)↑ Mbps", line)
                    if m:
                        return float(m.group(1)), float(m.group(2))
    except Exception:
        pass
    return None, None

def extract_latency(file):
    try:
        with open(file, 'r') as f:
            for line in f:
                if "Message latency at percentiles:" in line:
                    m = re.search(r"percentiles: ([\d.]+)/([\d.]+)/([\d.]+)/", line)
                    if m:
                        return float(m.group(1)), float(m.group(2)), float(m.group(3))
    except Exception:
        pass
    return None, None, None

def extract_cpu(file):
    try:
        with open(file, 'r') as f:
            for line in f:
                if line.startswith("Average:"):
                    parts = line.split()
                    if len(parts) >= 8:
                        return float(parts[7])
    except Exception:
        pass
    return None

def extract_mem(file_before, file_after):
    try:
        with open(file_before, 'r') as f:
            before = int(f.read().strip())
        with open(file_after, 'r') as f:
            after = int(f.read().strip())
        return after - before
    except Exception:
        return None

def extract_context_switches(file):
    try:
        with open(file, 'r') as f:
            for line in f:
                if "context-switches" in line:
                    return int(line.split()[0].replace(',', ''))
    except Exception:
        pass
    return None

for srv in servers:
    for c in connections:
        tp_down, tp_up = extract_throughput(os.path.join(results_dir, "throughput", f"{srv}_c{c}_tcpkali.log"))
        lat_50, lat_95, lat_99 = extract_latency(os.path.join(results_dir, "throughput", f"{srv}_c{c}_latency_tcpkali.log"))
        cpu = extract_cpu(os.path.join(results_dir, "perf", f"{srv}_c{c}_pidstat.log"))
        mem = extract_mem(os.path.join(results_dir, "perf", f"{srv}_c{c}_rss_before.txt"), 
                          os.path.join(results_dir, "perf", f"{srv}_c{c}_rss_after.txt"))
        cs = extract_context_switches(os.path.join(results_dir, "perf", f"{srv}_c{c}_perf.txt"))
        
        data[srv][c] = {
            "tp_down": tp_down,
            "tp_up": tp_up,
            "lat_50": lat_50,
            "lat_95": lat_95,
            "lat_99": lat_99,
            "cpu": cpu,
            "mem": mem,
            "cs": cs
        }

import json
print(json.dumps(data, indent=2))
