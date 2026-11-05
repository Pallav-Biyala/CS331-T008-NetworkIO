#!/usr/bin/env python3
"""Build the file-transfer report figures from raw benchmark runs.

Reads every run directory under code/results/File Transfer Server/benchmarking-results,
keeps only runs whose run.json says accepted_run == true, and parses wrk's
client-measured.log. Each plotted point is the median of the accepted runs;
whiskers span the inter-quartile range (25th-75th percentile).

Usage:  python report/file-transfer/make_figures.py
Output: report/file-transfer/figures/*.png and data.json (medians used in the report)
"""
import json
import re
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
RES = ROOT / "code" / "results" / "File Transfer Server" / "benchmarking-results"
OUT = HERE / "figures"
OUT.mkdir(exist_ok=True)

BACKENDS = ["select", "poll", "epoll", "uring"]
LABEL = {"select": "select", "poll": "poll", "epoll": "epoll", "uring": "io_uring"}
# Categorical slots 1-4 of the reference palette, fixed order, plus distinct markers
COLOR = {"select": "#2a78d6", "poll": "#eb6834", "epoll": "#1baf7a", "uring": "#eda100"}
MARKER = {"select": "o", "poll": "s", "epoll": "^", "uring": "D"}
INK, INK2, GRID = "#0b0b0b", "#52514e", "#e4e3df"

plt.rcParams.update({
    "font.size": 10, "axes.edgecolor": INK2, "axes.labelcolor": INK,
    "xtick.color": INK2, "ytick.color": INK2, "axes.spines.top": False,
    "axes.spines.right": False, "axes.grid": True, "grid.color": GRID,
    "grid.linewidth": 0.8, "axes.axisbelow": True, "figure.dpi": 150,
    "legend.frameon": False, "axes.titleweight": "bold", "axes.titlesize": 11,
})

UNIT_S = {"us": 1e-6, "ms": 1e-3, "s": 1.0, "m": 60.0, "h": 3600.0}
RUN_RE = re.compile(r"filesrv-(\w+)-baseline-(.+)-connections(\d+)-filebench-(.+?)\.bin-.*-run(\d+)$")


def parse_run(d: Path):
    m = RUN_RE.match(d.name)
    if not m:
        return None
    backend, workload, conns, fsize, rep = m.groups()
    meta = json.loads((d / "run.json").read_text())
    log = (d / "client-measured.log").read_text(errors="replace")
    kv = {k: float(v) for k, v in re.findall(r"^(wrk_\w+) (\S+)$", log, re.M)}
    lat = re.search(r"Latency\s+([\d.]+)(us|ms|s|m|h)\b", log)
    avg_s = float(lat.group(1)) * UNIT_S[lat.group(2)] if lat else float("nan")
    dur = kv.get("wrk_duration_us", 0) / 1e6
    return {
        "backend": backend, "workload": workload, "conns": int(conns), "file": fsize,
        "run": int(rep), "accepted": bool(meta.get("accepted_run")),
        "reason": meta.get("client_output_note"),
        "rps": kv.get("wrk_requests", 0) / dur if dur else 0.0,
        "mbps": kv.get("wrk_bytes", 0) / dur / 1e6 if dur else 0.0,   # MB/s (10^6)
        "lat_avg_ms": avg_s * 1e3,
        "lat_p99_ms": kv.get("wrk_latency_p99_us", float("nan")) / 1e3,
    }


runs = [r for d in RES.rglob("filesrv-*-run*") if d.is_dir() and (r := parse_run(d))]


def stat(backend, workload, conns, fsize, metric):
    vals = [r[metric] for r in runs if r["accepted"] and r["backend"] == backend
            and r["workload"] == workload and r["conns"] == conns and r["file"] == fsize]
    if not vals:
        return None
    q1, med, q3 = np.percentile(vals, [25, 50, 75])
    return {"median": float(med), "q1": float(q1), "q3": float(q3), "n": len(vals)}


def save(fig, name):
    fig.tight_layout()
    fig.savefig(OUT / name, bbox_inches="tight", facecolor="white")
    plt.close(fig)


def line_plot(ax, xs, cells, metric, xlabel, ylabel):
    """cells: list of (workload, conns, fsize) aligned with xs."""
    for i, b in enumerate(BACKENDS):
        s = [stat(b, *c, metric) for c in cells]
        med = np.array([v["median"] for v in s])
        err = np.array([[v["median"] - v["q1"] for v in s], [v["q3"] - v["median"] for v in s]])
        # tiny horizontal offset so overlapping backends stay visible
        xo = np.array(xs) * (1 + (i - 1.5) * 0.04)
        ax.errorbar(xo, med, yerr=err, color=COLOR[b], marker=MARKER[b], ms=7, lw=2,
                    capsize=3, label=LABEL[b], mec="white", mew=1.2)
    ax.set_xlabel(xlabel)
    ax.set_ylabel(ylabel)
    ax.legend(ncol=4, loc="upper center", bbox_to_anchor=(0.5, -0.18))


def bar_plot(ax, groups, cells, metric, ylabel, fmt="{:.0f}", annotate_n=True):
    """groups: x tick labels; cells: list of (workload, conns, fsize) per group."""
    x = np.arange(len(groups))
    w = 0.2
    for i, b in enumerate(BACKENDS):
        s = [stat(b, *c, metric) for c in cells]
        med = np.array([v["median"] if v else np.nan for v in s])
        err = np.array([[v["median"] - v["q1"] if v else 0 for v in s],
                        [v["q3"] - v["median"] if v else 0 for v in s]])
        pos = x + (i - 1.5) * w
        ax.bar(pos, med, width=w - 0.03, color=COLOR[b], label=LABEL[b],
               yerr=err, capsize=2, error_kw={"ecolor": INK2, "lw": 1})
        if annotate_n:
            for p, v in zip(pos, s):
                if v and v["n"] < 5:
                    ax.annotate(f"n={v['n']}", (p, v["q3"]), xytext=(0, 3),
                                textcoords="offset points", ha="center", fontsize=7, color=INK2)
    ax.set_xticks(x, groups)
    ax.set_ylabel(ylabel)
    ax.grid(axis="x", visible=False)
    ax.legend(ncol=4, loc="upper center", bbox_to_anchor=(0.5, -0.14))


# ---------------- E5a ----------------
E5A = "e5a-size-concurrency"
conc = [1, 100, 1000]
cells = [(E5A, c, "4KiB") for c in conc]

fig, ax = plt.subplots(figsize=(7, 4.2))
line_plot(ax, conc, cells, "rps", "Concurrent connections (log scale)", "Throughput (responses/s)")
ax.set_xscale("log"); ax.set_xticks(conc, [str(c) for c in conc]); ax.set_ylim(bottom=0)
ax.set_title("E5a-1  Throughput vs concurrency (4 KiB file)")
save(fig, "e5a_1_throughput_vs_concurrency.png")

fig, ax = plt.subplots(figsize=(7, 4.2))
line_plot(ax, conc, cells, "lat_avg_ms", "Concurrent connections (log scale)", "Average latency (ms, log scale)")
ax.set_xscale("log"); ax.set_yscale("log"); ax.set_xticks(conc, [str(c) for c in conc])
ax.set_title("E5a-2  Average latency vs concurrency (4 KiB file)")
save(fig, "e5a_2_latency_vs_concurrency.png")

size_cells = [(E5A, 100, "4KiB"), (E5A, 10, "64KiB"), (E5A, 100, "1MiB")]
fig, ax = plt.subplots(figsize=(7, 4.2))
bar_plot(ax, ["4 KiB\n(100 conns)", "64 KiB\n(10 conns)", "1 MiB\n(100 conns)"], size_cells, "mbps",
         "Throughput (MB/s)")
ax.set_title("E5a-3  Throughput vs file size")
save(fig, "e5a_3_throughput_vs_filesize.png")

# ---------------- E5b ----------------
e5b = [("e5b-close-per-response", 100, "4KiB"), ("e5b-keepalive-c300", 100, "4KiB")]
for metric, ylab, tag, title in [("rps", "Throughput (responses/s)", "4_throughput", "Throughput"),
                                  ("lat_avg_ms", "Average latency (ms)", "5_latency", "Average latency")]:
    fig, ax = plt.subplots(figsize=(7, 4.2))
    bar_plot(ax, ["Connection: close\n(new TCP conn per file)", "Keep-alive\n(connection reused)"], e5b, metric, ylab)
    ax.set_title(f"E5b-{tag[0]}  {title}: close vs keep-alive (4 KiB, 100 conns)")
    save(fig, f"e5b_{tag}_close_vs_keepalive.png")

# ---------------- E7 ----------------
e7_names = ["e7-baseline", "e7-chunk16", "e7-rcvbuf512", "e7-sndbuf64"]
e7_labels = ["Baseline\n64 KiB chunk", "16 KiB chunk", "SO_RCVBUF\n512 KiB", "SO_SNDBUF\n64 KiB"]
e7 = [(n, 100, "4MiB") for n in e7_names]
for metric, ylab, tag, title in [("mbps", "Throughput (MB/s)", "6_throughput", "Throughput"),
                                  ("lat_avg_ms", "Average latency (ms)", "7_latency", "Average latency")]:
    fig, ax = plt.subplots(figsize=(7.5, 4.2))
    bar_plot(ax, e7_labels, e7, metric, ylab)
    ax.set_title(f"E7-{tag[0]}  {title} vs buffer setting (4 MiB file, 100 conns)")
    save(fig, f"e7_{tag}_vs_buffer.png")

# ---------------- E8 ----------------
e8_labels = ["0 slow readers", "10 slow readers", "100 slow readers"]
e8 = [(f"e8-slow{n}", 100, "100MiB") for n in (0, 10, 100)]
fig, ax = plt.subplots(figsize=(7, 4.2))
bar_plot(ax, e8_labels, e8, "mbps", "Fast-client throughput (MB/s)")
ax.set_title("E8-8  Throughput vs slow readers (100 MiB file, 100 fast conns)")
save(fig, "e8_8_throughput_vs_slow_readers.png")

fig, ax = plt.subplots(figsize=(7, 4.2))
s_cells = e8
bar_plot(ax, e8_labels, s_cells, "lat_avg_ms", "Average download time (s)")
ax.yaxis.set_major_formatter(matplotlib.ticker.FuncFormatter(lambda v, _: f"{v/1000:.0f}"))
ax.set_title("E8-9  Latency vs slow readers (time to fetch 100 MiB)")
save(fig, "e8_9_latency_vs_slow_readers.png")

# ---------------- E9 ----------------
for metric, ylab, tag, title in [("rps", "Throughput (responses/s)", "10_throughput", "Throughput"),
                                  ("lat_avg_ms", "Average latency (ms)", "11_latency", "Average latency")]:
    fig, axes = plt.subplots(1, 2, figsize=(9.5, 4.2))
    for ax, fsize in zip(axes, ["4KiB", "64KiB"]):
        bar_plot(ax, ["TCP_NODELAY on\n(Nagle off)", "TCP_NODELAY off\n(Nagle on)"],
                 [("e9-nodelay1", 100, fsize), ("e9-nodelay0", 100, fsize)], metric, ylab)
        ax.set_title(f"{fsize.replace('KiB', ' KiB')} file")
        ax.get_legend().remove()
    handles, labels = axes[0].get_legend_handles_labels()
    fig.suptitle(f"E9-{tag.split('_')[0]}  {title}: TCP_NODELAY on vs off (100 conns)", fontweight="bold")
    fig.tight_layout(rect=(0, 0.08, 1, 1))
    fig.legend(handles, labels, ncol=4, loc="lower center", frameon=False)
    fig.savefig(OUT / f"e9_{tag}_nodelay.png", bbox_inches="tight", facecolor="white")
    plt.close(fig)

# ---------------- data dump for the report ----------------
data = {}
for r in runs:
    key = f"{r['workload']}|c{r['conns']}|{r['file']}"
    data.setdefault(key, {})
for key in data:
    w, c, f = key.split("|")
    data[key] = {b: {m: stat(b, w, int(c[1:]), f, m) for m in ("rps", "mbps", "lat_avg_ms", "lat_p99_ms")}
                 for b in BACKENDS}
rejected = [{k: r[k] for k in ("backend", "workload", "run", "reason")} for r in runs if not r["accepted"]]
(HERE / "data.json").write_text(json.dumps({"cells": data, "rejected_runs": rejected}, indent=1))
print(f"{len(runs)} runs parsed, {len(rejected)} rejected; figures in {OUT}")
