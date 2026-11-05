#!/usr/bin/env python3
"""Compact, multi-panel figures for the course report (report/CS331-T008-Report.docx).

Reads report/file-transfer/data.json (written by make_figures.py) and writes
report/file-transfer/figures/report/*.png sized for a 6.2-inch text width.
Medians of accepted runs; whiskers show the IQR across the 5 repetitions.

Usage:  python report/file-transfer/make_figures.py && python report/file-transfer/make_report_figures.py
"""
import json
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch
import numpy as np

HERE = Path(__file__).resolve().parent
OUT = HERE / "figures" / "report"
OUT.mkdir(parents=True, exist_ok=True)
D = json.loads((HERE / "data.json").read_text())
CELLS = D["cells"]

B = ["select", "poll", "epoll", "uring"]
LAB = {"select": "select", "poll": "poll", "epoll": "epoll", "uring": "io_uring"}
COL = {"select": "#2a78d6", "poll": "#eb6834", "epoll": "#1baf7a", "uring": "#eda100"}
MRK = {"select": "o", "poll": "s", "epoll": "^", "uring": "D"}
INK, INK2, GRID = "#0b0b0b", "#52514e", "#e4e3df"
W = 6.2  # inches of text width

plt.rcParams.update({
    "font.family": "serif", "font.serif": ["Times New Roman", "DejaVu Serif"], "font.size": 8,
    "axes.titlesize": 8.5, "axes.titleweight": "bold", "axes.labelsize": 8, "xtick.labelsize": 7.5,
    "ytick.labelsize": 7.5, "legend.fontsize": 8, "axes.edgecolor": INK2, "xtick.color": INK2,
    "ytick.color": INK2, "axes.spines.top": False, "axes.spines.right": False, "axes.grid": True,
    "grid.color": GRID, "grid.linewidth": 0.6, "axes.axisbelow": True, "legend.frameon": False,
    "figure.dpi": 300,
})


def s(key, b, m, scale=1.0):
    c = CELLS[key][b][m]
    return (c["median"] * scale, c["q1"] * scale, c["q3"] * scale, c["n"]) if c else (np.nan, np.nan, np.nan, 0)


def bars(ax, keys, labels, metric, ylabel, scale=1.0, title=None, nmark=False):
    x = np.arange(len(keys))
    w = 0.2
    for i, b in enumerate(B):
        v = [s(k, b, metric, scale) for k in keys]
        med = np.array([t[0] for t in v])
        err = np.array([[t[0] - t[1] for t in v], [t[2] - t[0] for t in v]])
        pos = x + (i - 1.5) * w
        ax.bar(pos, med, width=w - 0.025, color=COL[b], label=LAB[b], yerr=err, capsize=1.5,
               error_kw={"ecolor": INK2, "lw": 0.7})
        if nmark:
            for p, t in zip(pos, v):
                if 0 < t[3] < 5:
                    ax.annotate(f"n={t[3]}", (p, t[2]), xytext=(0, 2), textcoords="offset points",
                                ha="center", fontsize=6, color=INK2)
    ax.set_xticks(x, labels)
    ax.set_ylabel(ylabel)
    ax.grid(axis="x", visible=False)
    if title:
        ax.set_title(title)


def lines(ax, keys, xs, metric, ylabel, title, logy=False):
    for i, b in enumerate(B):
        v = [s(k, b, metric) for k in keys]
        med = np.array([t[0] for t in v])
        err = np.array([[t[0] - t[1] for t in v], [t[2] - t[0] for t in v]])
        xo = np.array(xs) * (1 + (i - 1.5) * 0.05)
        ax.errorbar(xo, med, yerr=err, color=COL[b], marker=MRK[b], ms=4, lw=1.4, capsize=1.5,
                    label=LAB[b], mec="white", mew=0.6)
    ax.set_xscale("log")
    ax.set_xticks(xs, [str(v) for v in xs])
    ax.minorticks_off()
    if logy:
        ax.set_yscale("log")
    else:
        ax.set_ylim(bottom=0)
    ax.set_xlabel("Concurrent connections")
    ax.set_ylabel(ylabel)
    ax.set_title(title)


def legend_top(fig, ax, n=4):
    h, l = ax.get_legend_handles_labels()
    fig.legend(h, l, ncol=n, loc="upper center", bbox_to_anchor=(0.5, 1.0))


def save(fig, name, top=0.86):
    fig.tight_layout(rect=(0, 0, 1, top))
    fig.savefig(OUT / name, facecolor="white")
    plt.close(fig)


K = lambda w, c, f: f"{w}|c{c}|{f}"
E5A = "e5a-size-concurrency"

# ---- Fig: architecture ----
fig, ax = plt.subplots(figsize=(W, 2.6))
ax.set_xlim(0, 100); ax.set_ylim(0, 46); ax.axis("off")


def box(x, y, w, h, text, fc, tc=INK, fs=7, bold=False):
    ax.add_patch(FancyBboxPatch((x, y), w, h, boxstyle="round,pad=0.2,rounding_size=1.0", fc=fc, ec="none"))
    ax.text(x + w / 2, y + h / 2, text, ha="center", va="center", fontsize=fs, color=tc,
            fontweight="bold" if bold else "normal", linespacing=1.2)


def arrow(x1, y1, x2, y2):
    ax.annotate("", xy=(x2, y2), xytext=(x1, y1),
                arrowprops=dict(arrowstyle="-|>", color="#7b8794", lw=1.0, mutation_scale=9))


box(1, 38, 30, 6.5, "Echo server\n(apps/Echo-Server)", "#eef2f6")
box(36, 38, 30, 6.5, "HTTP file server\n(filesrv.c)", "#14283c", "white", bold=True)
box(1, 28.5, 65, 6, "core.c: connection table, carry / output slots, config & flags, statistics", "#eef2f6")
box(1, 14.5, 65, 10.5, "io.h request contract (one API for every backend)\n"
                       "app \u2192 module:  io_read, io_write, io_read_file, io_close\n"
                       "module \u2192 app (deferred, inside io_run):  on_accept, on_read_done,\n"
                       "on_write_done, on_file_read_done, on_close", "#dbe7f5", fs=6.6)
names = [("io_select.c\nselect(2)", "white"), ("io_poll.c\npoll(2)", "white"),
         ("io_epoll.c\nepoll(7)", "white"), ("io_uring.c\nliburing", INK)]
for i, (n, tc) in enumerate(names):
    box(1 + i * 16.6, 1.5, 15.2, 7, n, COL[B[i]], tc, fs=6.8, bold=True)
arrow(16, 38, 16, 34.8); arrow(51, 38, 51, 34.8); arrow(33.5, 28.5, 33.5, 25.3)
for i in range(4):
    arrow(8.6 + i * 16.6, 14.5, 8.6 + i * 16.6, 8.8)
ax.text(33.5, 11.2, "one backend linked per binary", ha="center", fontsize=6.2, color=INK2, style="italic",
        bbox=dict(fc="white", ec="none", pad=0.6))
box(72, 33, 27, 11.5, "run_experiment (Python)\npins CPUs, warm-up,\nmeasure, validate, archive", "#f6efe0", fs=6.6)
box(72, 17, 27, 11.5, "Load generators\nwrk + Lua report\ncurl slow readers (E8)", "#f6efe0", fs=6.6)
box(72, 1.5, 27, 11, "results/ (JSON, logs)\n\u2192 make_figures.py", "#f6efe0", fs=6.6)
arrow(85.5, 33, 85.5, 28.8); arrow(85.5, 17, 85.5, 12.8)
arrow(72, 24, 66.5, 40)
ax.text(68.2, 30, "HTTP,\nloopback", fontsize=6, color=INK2, ha="left", va="center")
fig.savefig(OUT / "fig1_architecture.png", facecolor="white", bbox_inches="tight", pad_inches=0.02)
plt.close(fig)

# ---- Fig: E5a ----
fig, axs = plt.subplots(1, 3, figsize=(W, 2.15))
ck = [K(E5A, c, "4KiB") for c in (1, 100, 1000)]
lines(axs[0], ck, [1, 100, 1000], "rps", "Responses/s", "(a) Throughput, 4 KiB")
lines(axs[1], ck, [1, 100, 1000], "lat_avg_ms", "Mean latency (ms, log)", "(b) Latency, 4 KiB", logy=True)
bars(axs[2], [K(E5A, 100, "4KiB"), K(E5A, 10, "64KiB"), K(E5A, 100, "1MiB")],
     ["4 KiB\n(c=100)", "64 KiB\n(c=10)", "1 MiB\n(c=100)"], "mbps", "MB/s", title="(c) Throughput vs file size")
legend_top(fig, axs[0])
save(fig, "fig2_e5a.png")

# ---- Fig: E5b + E7 ----
fig, axs = plt.subplots(1, 4, figsize=(W, 2.15), gridspec_kw={"width_ratios": [1, 1, 1.45, 1.45]})
e5b = [K("e5b-close-per-response", 100, "4KiB"), K("e5b-keepalive-c300", 100, "4KiB")]
bars(axs[0], e5b, ["close", "keep-\nalive"], "rps", "Responses/s", title="(a) E5b throughput")
bars(axs[1], e5b, ["close", "keep-\nalive"], "lat_avg_ms", "Mean latency (ms)", title="(b) E5b latency")
e7 = [K(w, 100, "4MiB") for w in ("e7-baseline", "e7-chunk16", "e7-rcvbuf512", "e7-sndbuf64")]
e7l = ["base", "16K\nchunk", "RCV\n512K", "SND\n64K"]
bars(axs[2], e7, e7l, "mbps", "MB/s", title="(c) E7 throughput")
bars(axs[3], e7, e7l, "lat_avg_ms", "Mean latency (s)", scale=0.001, title="(d) E7 latency / 4 MiB")
legend_top(fig, axs[0])
save(fig, "fig3_e5b_e7.png")

# ---- Fig: E8 + E9 ----
fig, axs = plt.subplots(1, 4, figsize=(W, 2.15))
e8 = [K(f"e8-slow{n}", 100, "100MiB") for n in (0, 10, 100)]
bars(axs[0], e8, ["0", "10", "100"], "mbps", "Fast-client MB/s", title="(a) E8 throughput")
axs[0].set_xlabel("Slow readers")
bars(axs[1], e8, ["0", "10", "100"], "lat_avg_ms", "Download time (s)", scale=0.001, title="(b) E8 latency")
axs[1].set_xlabel("Slow readers")
e9 = [K("e9-nodelay1", 100, "4KiB"), K("e9-nodelay0", 100, "4KiB"), K("e9-nodelay1", 100, "64KiB"), K("e9-nodelay0", 100, "64KiB")]
e9l = ["4K\non", "4K\noff", "64K\non", "64K\noff"]
bars(axs[2], e9, e9l, "rps", "Responses/s", title="(c) E9 throughput")
bars(axs[3], e9, e9l, "lat_avg_ms", "Mean latency (ms)", title="(d) E9 latency")
for a in axs[2:]:
    a.set_xlabel("File · TCP_NODELAY")
legend_top(fig, axs[0])
save(fig, "fig4_e8_e9.png")

# ---- Fig: analysis ----
fig, axs = plt.subplots(1, 2, figsize=(W, 2.5), gridspec_kw={"width_ratios": [1.15, 1]})
cu = D["core_utilisation"][::-1]
y = np.arange(len(cu))
left = np.zeros(len(cu))
for m, lab, col in [("user", "User", "#2a78d6"), ("sys", "Kernel (syscalls)", "#eb6834"), ("softirq", "Kernel (softirq)", "#1baf7a")]:
    v = np.array([r[m] for r in cu])
    axs[0].barh(y, v, left=left, color=col, label=lab, height=0.66, edgecolor="white", linewidth=0.6)
    left += v
for yy, t in zip(y, left):
    axs[0].annotate(f"{t:.0f}%", (t, yy), xytext=(2, 0), textcoords="offset points", va="center", fontsize=6.5)
axs[0].axvline(100, color="#e34948", lw=1, ls="--")
axs[0].set_xlim(0, 104)
axs[0].set_yticks(y, [r["label"] for r in cu], fontsize=6.5)
axs[0].set_xlabel("Server core busy (%)")
axs[0].grid(axis="y", visible=False)
axs[0].set_title("(a) Server core utilisation")
axs[0].legend(loc="lower right", fontsize=6.5, handlelength=1, frameon=True, framealpha=0.95, edgecolor="none")
rel = D["cpu_per_response_vs_epoll"]
x = np.arange(len(rel))
for i, b in enumerate(["select", "poll", "uring"]):
    axs[1].bar(x + (i - 1) * 0.26, [r[b] for r in rel], width=0.23, color=COL[b], label=LAB[b])
axs[1].axhline(0, color=INK2, lw=0.8)
axs[1].set_xticks(x, ["4K\nc100", "4K\nc1000", "4K close\nc100", "1M\nc100", "4M\nc100", "100M\nc100"], fontsize=6.5)
axs[1].set_ylabel("CPU per response vs epoll (%)")
axs[1].grid(axis="x", visible=False)
axs[1].set_title("(b) Server CPU per response vs epoll")
axs[1].legend(loc="upper right", fontsize=6.5, handlelength=1, frameon=True, framealpha=0.95, edgecolor="none")
fig.tight_layout()
fig.savefig(OUT / "fig5_analysis.png", facecolor="white")
plt.close(fig)
print("report figures in", OUT)
