import sys, os, re
# Force UTF-8 output on Windows (avoids UnicodeEncodeError for non-ASCII chars)
if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')

import numpy as np
import pandas as pd
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

# ── Paths ─────────────────────────────────────────────────────────────────────
# Derive paths relative to this script so the script is portable.
SCRIPT_DIR  = os.path.dirname(os.path.abspath(__file__))
RESULTS_DIR = os.path.dirname(SCRIPT_DIR)           # .../results/
OUTPUT_DIR  = os.path.join(RESULTS_DIR, 'results_v2')
GRAPHS_DIR  = os.path.join(OUTPUT_DIR, 'graphs')
DATA_DIR    = os.path.join(OUTPUT_DIR, 'data')

os.makedirs(GRAPHS_DIR, exist_ok=True)
os.makedirs(DATA_DIR,   exist_ok=True)

ENGINES = ['select', 'poll', 'epoll', 'iouring', 'iouring_sqpoll']
CONNS   = [10, 100, 500, 1000, 1500, 2000, 2500, 3000, 3500, 4000, 4500, 5000]

# ── Benchmark provenance note ─────────────────────────────────────────────────
# Throughput/latency, strace syscall-counts, and perf context-switch/CPU/RSS
# data come from THREE SEPARATE isolated benchmark scripts with different
# durations and connection rates.  Numbers from different columns of the CSV
# must NOT be compared as if they came from the same run.
#
#  bench_throughput.sh  — tcpkali unlimited rate (pass 1): 15 s (<c5000), 30 s (>=c5000)
#                          tcpkali fixed rate 20 msg/s (pass 2): same durations
#  bench_strace.sh      — 20 s tcpkali load, strace -c window = 10 s (mid-run)
#                          WARNING: strace adds heavy ptrace overhead; throughput
#                          numbers from this run are NOT representative.
#  bench_perf.sh        — 25 s tcpkali load, perf stat window = 10 s (mid-run)
#                          pidstat 1 s x 5 samples after perf window
#                          RSS-after captured ~18 s into the load (before tcpkali
#                          finishes); not a final steady-state memory reading.

data = []

for engine in ENGINES:
    for conn in CONNS:
        # Use None for every metric by default.
        # None becomes NaN in the DataFrame, which matplotlib skips automatically
        # on all plot types.  This makes missing/failed data visually absent
        # rather than appearing as a spurious zero data point.
        throughput_up   = None   # uplink Mbps   (server -> client direction)
        throughput_down = None   # downlink Mbps  (client -> server direction)
        p99_lat         = None
        syscalls        = None
        ctx_switches    = None

        # ── Parse throughput (unlimited-rate pass) ────────────────────────────
        # Source: bench_throughput.sh pass 1 (no instrumentation).
        # Duration: 15 s for c<5000, 30 s for c>=5000.
        # "Uplink" in tcpkali's frame = server sending back echoed data to client.
        tp_file = os.path.join(RESULTS_DIR,
                               f"throughput/{engine}_c{conn}_tcpkali.log")
        if os.path.exists(tp_file):
            with open(tp_file, 'r') as f:
                content = f.read()
            # tcpkali line: "Aggregate bandwidth: X<down-arrow>, Y<up-arrow> Mbps"
            # The arrow characters can vary by terminal encoding, so match any
            # non-space/non-digit/non-dot character between the numbers.
            m = re.search(
                r'Aggregate bandwidth:\s*([\d.]+)\S*,\s*([\d.]+)\S*\s*Mbps',
                content)
            if m:
                throughput_down = float(m.group(1))
                throughput_up   = float(m.group(2))

        # ── Parse P99 latency (fixed-rate pass) ───────────────────────────────
        # Source: bench_throughput.sh pass 2 (--message-rate 20, separate server start).
        # Same durations as pass 1.  NOT the same server instance as pass 1.
        lat_file = os.path.join(RESULTS_DIR,
                                f"throughput/{engine}_c{conn}_latency_tcpkali.log")
        if os.path.exists(lat_file):
            with open(lat_file, 'r') as f:
                content = f.read()
            # Line: "Message latency at percentiles: p50/p95/p99/p99.9 ms"
            m = re.search(
                r'Message latency at percentiles:\s*[\d.]+/[\d.]+/([\d.]+)/',
                content)
            if m:
                p99_lat = float(m.group(1))

        # ── Parse syscalls from strace -c output ──────────────────────────────
        # Source: bench_strace.sh — separate run with heavy ptrace overhead.
        # strace -c total line has two possible formats:
        #   with errors:    "100.00  <secs>  <us/call>  <calls>  <errors>  total"
        #   without errors: "100.00  <secs>  <us/call>  <calls>            total"
        # The 4th numeric column is always the total call count.
        strace_file = os.path.join(RESULTS_DIR,
                                   f"strace/{engine}_c{conn}_strace.txt")
        if os.path.exists(strace_file):
            with open(strace_file, 'r') as f:
                content = f.read()
            # Regex: anchor to 100.00, skip seconds+usecs/call,
            #        capture calls (group 1), make errors column optional.
            m = re.search(
                r'100\.00\s+[\d.]+\s+\d+\s+(\d+)(?:\s+\d+)?\s+total',
                content)
            if m:
                syscalls = int(m.group(1))

        # ── Parse context switches from perf stat output ──────────────────────
        # Source: bench_perf.sh — separate run, perf stat over 10 s mid-run window.
        perf_file = os.path.join(RESULTS_DIR,
                                 f"perf/{engine}_c{conn}_perf.txt")
        if os.path.exists(perf_file):
            with open(perf_file, 'r') as f:
                content = f.read()
            # Remove digit-grouping commas before matching (e.g. "2,345" -> "2345")
            content_cleaned = content.replace(',', '')
            m = re.search(r'\s+(\d+)\s+context-switches', content_cleaned)
            if m:
                ctx_switches = int(m.group(1))

        data.append({
            'Engine':            engine,
            'Connections':       conn,
            'Throughput_Up_Mbps':   throughput_up,    # server->client (uplink)
            'Throughput_Down_Mbps': throughput_down,  # client->server (downlink)
            'P99_Latency_ms':    p99_lat,
            'Syscalls':          syscalls,
            'Context_Switches':  ctx_switches,
        })

df = pd.DataFrame(data)

# ── Export CSV ────────────────────────────────────────────────────────────────
# NaN in the CSV means: file was missing or parsing failed — not a genuine zero.
csv_path = os.path.join(DATA_DIR, 'all_metrics_v2.csv')
df.to_csv(csv_path, index=False)
print(f"Saved CSV  -> {csv_path}")

# ── Colour palette ────────────────────────────────────────────────────────────
COLORS = {
    'select':        '#e63946',
    'poll':          '#f4a261',
    'epoll':         '#2a9d8f',
    'iouring':       '#457b9d',
    'iouring_sqpoll':'#6a0572',
}
LABELS = {
    'select':        'select',
    'poll':          'poll',
    'epoll':         'epoll',
    'iouring':       'io_uring',
    'iouring_sqpoll':'io_uring (SQPOLL)',
}


def save_plot(title, ylabel, metric, log_y=False, symlog_y=False, fname=None):
    """
    Plot `metric` column from `df` for each engine.
    NaN values are automatically skipped by matplotlib, producing gaps in the
    line where data is missing rather than misleading zero segments.
    """
    fig, ax = plt.subplots(figsize=(11, 6))
    for engine in ENGINES:
        edf = df[df['Engine'] == engine]
        y = edf[metric].values.astype(float)   # NaN passes through cleanly
        ax.plot(edf['Connections'].values, y,
                marker='o', linewidth=2, markersize=5,
                color=COLORS[engine], label=LABELS[engine])
    ax.set_title(title, fontsize=14, fontweight='bold')
    ax.set_xlabel('Number of Connections', fontsize=12)
    ax.set_ylabel(ylabel, fontsize=12)
    if log_y:
        ax.set_yscale('log')
    elif symlog_y:
        ax.set_yscale('symlog', linthresh=10)
    ax.set_xscale('log')
    ax.legend(fontsize=10)
    ax.grid(True, which='both', linestyle='--', alpha=0.5)
    fig.tight_layout()
    path = os.path.join(GRAPHS_DIR, fname)
    fig.savefig(path, dpi=150)
    plt.close(fig)
    print(f"Saved plot -> {path}")


# Graph 1 — Throughput (uplink = server sending back echoed data to client).
# This is the direction that exercises the server's write path under load.
# The "downlink" direction (client->server) is also stored in the CSV.
save_plot(
    title='Throughput vs Connections\n(uplink: server -> client, unlimited rate, bench_throughput.sh pass 1)',
    ylabel='Uplink Throughput (Mbps)',
    metric='Throughput_Up_Mbps',
    log_y=False,
    fname='throughput_vs_conns.png',
)

# Graph 2 — P99 latency (fixed-rate pass, separate server instance).
save_plot(
    title='P99 Latency vs Connections\n(fixed rate 20 msg/s per conn, bench_throughput.sh pass 2)',
    ylabel='P99 Latency (ms) - log scale',
    metric='P99_Latency_ms',
    log_y=True,
    fname='p99_latency_vs_conns.png',
)

# Graph 3 — Total syscalls (strace -c, separate run with ptrace overhead).
save_plot(
    title='Total Syscalls vs Connections\n(10 s strace window, bench_strace.sh — NOT a performance run)',
    ylabel='Total Syscalls - log scale',
    metric='Syscalls',
    log_y=True,
    fname='syscalls_vs_conns.png',
)

# Graph 4 — Context switches (perf stat, separate run, 10 s window).
save_plot(
    title='Context Switches vs Connections\n(10 s perf stat window, bench_perf.sh)',
    ylabel='Context Switches - symlog scale',
    metric='Context_Switches',
    symlog_y=True,
    fname='context_switches_vs_conns.png',
)

print("\nAll done.  Results in:", OUTPUT_DIR)
