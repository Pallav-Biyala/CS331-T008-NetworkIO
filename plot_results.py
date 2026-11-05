#!/usr/bin/env python3
"""Generate the 8 benchmark graphs from run_experiment.sh output.

Usage:  python3 plot_results.py [RESULTS_DIR=results] [OUT_DIR=plots]

Reads every results.csv under RESULTS_DIR (recursive), parses tag = ${MECH}_rep${REP}_${SUITE}_${CASE},
takes the median across repetitions (error bars = min..max when >1 rep), and writes
plots/01_*.png .. 08_*.png plus plots/summary.csv and plots/validity_report.csv.

Only three metric families are plotted: throughput (echoed_rate), sent vs completed
(actual_send_rate / delivery_ratio_pct) and latency (app_rtt p50/p95/p99, e2e p99).
client_cpu_pct is NOT plotted; it is used only in validity_report.csv to flag runs
where the single-core load generator, not the server, was the bottleneck.
"""
import glob
import os
import re
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

RESULTS = sys.argv[1] if len(sys.argv) > 1 else "results"
OUT = sys.argv[2] if len(sys.argv) > 2 else "plots"

MECHS = ["select", "poll", "epoll", "uring"]
LABEL = {"select": "select", "poll": "poll", "epoll": "epoll", "uring": "io_uring"}
COLOR = {"select": "#2a78d6", "poll": "#eb6834", "epoll": "#1baf7a", "uring": "#eda100"}
MARKER = {"select": "o", "poll": "s", "epoll": "^", "uring": "D"}
DISTS = ["constant", "poisson", "burst2x", "burst", "burst10x"]
DIST_LABEL = {"constant": "constant", "poisson": "poisson", "burst2x": "burst 2×",
              "burst": "burst 4×", "burst10x": "burst 10×"}
TAG_RE = re.compile(r"^(?P<mech>[a-z]+)_rep(?P<rep>\d+)_(?P<suite>S\d(?:_[A-Z]+)*|PILOT)_(?P<case>.+)$")
METRICS = ["echoed_rate", "actual_send_rate", "offered_rate", "delivery_ratio_pct",
           "app_rtt_p50_us", "app_rtt_p95_us", "app_rtt_p99_us", "e2e_lat_p99_us"]

plt.rcParams.update({
    "figure.dpi": 130, "savefig.dpi": 160, "font.size": 9.5,
    "axes.spines.top": False, "axes.spines.right": False,
    "axes.edgecolor": "#8a8984", "axes.labelcolor": "#52514e",
    "xtick.color": "#52514e", "ytick.color": "#52514e",
    "axes.grid": True, "grid.color": "#e6e5e0", "grid.linewidth": 0.6,
    "axes.titlesize": 10, "axes.titleweight": "bold", "legend.frameon": False,
    "lines.linewidth": 2, "lines.markersize": 6,
})


def load():
    frames = []
    for path in sorted(glob.glob(os.path.join(RESULTS, "**", "results.csv"), recursive=True)):
        df = pd.read_csv(path)
        df["source"] = path
        frames.append(df)
    if not frames:
        sys.exit(f"No results.csv found under {RESULTS}/")
    df = pd.concat(frames, ignore_index=True)
    parsed = df["tag"].str.extract(TAG_RE)
    bad = parsed["mech"].isna()
    if bad.any():
        print(f"WARN: skipping {bad.sum()} rows with unparseable tag, e.g. {df.loc[bad, 'tag'].iloc[0]}")
    df = pd.concat([df, parsed], axis=1)[~bad].copy()
    df["rep"] = df["rep"].astype(int)
    df = df[df["suite"] != "PILOT"]
    # A re-run of the same (mech, rep, case) supersedes the older row.
    df = df.drop_duplicates(["mech", "rep", "suite", "case"], keep="last")
    return df


def aggregate(df):
    keys = ["mech", "suite", "case", "mode", "dist", "connections", "pipeline_window", "target_rate"]
    df = df.copy()
    df["share_r"] = df["active_share"].round(2)
    keys.append("share_r")
    g = df.groupby(keys, dropna=False)[METRICS]
    agg = g.median()
    lo, hi = g.min(), g.max()
    agg = agg.join(lo.add_suffix("_lo")).join(hi.add_suffix("_hi"))
    agg["reps"] = g.size()
    return agg.reset_index()


def sel(agg, suite, **eq):
    d = agg[agg["suite"].isin([suite] if isinstance(suite, str) else suite)]
    for k, v in eq.items():
        d = d[d[k] == v]
    return d


def line(ax, d, x, y, mech, ls="-", label=None, marker=True):
    d = d.sort_values(x)
    if d.empty:
        return
    yerr = None
    if (d["reps"] > 1).any():
        yerr = np.vstack([d[y] - d[y + "_lo"], d[y + "_hi"] - d[y]])
    ax.errorbar(d[x], d[y], yerr=yerr, color=COLOR[mech], ls=ls,
                marker=MARKER[mech] if marker else None,
                mfc=COLOR[mech] if ls == "-" else "white", mec=COLOR[mech], mew=1.5,
                capsize=2, elinewidth=1, label=label)


def mech_legend(fig, extra=None):
    handles = [plt.Line2D([], [], color=COLOR[m], marker=MARKER[m], label=LABEL[m]) for m in MECHS]
    if extra:
        handles += extra
    fig.legend(handles=handles, loc="lower center", ncol=len(handles), bbox_to_anchor=(0.5, -0.01))


def style_handles(pairs):
    return [plt.Line2D([], [], color="#52514e", ls=ls, marker=mk, mfc=mfc, mec="#52514e", label=lb)
            for ls, mk, mfc, lb in pairs]


def fd_line(ax):
    ax.axvline(1024, color="#8a8984", ls="--", lw=1)
    ax.text(1024, 0.98, " select FD_SETSIZE=1024", transform=ax.get_xaxis_transform(),
            va="top", ha="left", fontsize=8, color="#52514e")


def save(fig, name, title):
    fig.suptitle(title, fontweight="bold", fontsize=11.5)
    fig.tight_layout(rect=(0, 0.05, 1, 0.97))
    path = os.path.join(OUT, name)
    fig.savefig(path, bbox_inches="tight")
    plt.close(fig)
    print("wrote", path)


def conn_axis(ax):
    ax.set_xscale("log")
    ax.set_xticks([50, 100, 250, 500, 1000, 2500, 5000, 10000])
    ax.set_xticklabels(["50", "100", "250", "500", "1K", "2.5K", "5K", "10K"])
    ax.set_xlabel("Connections")


def kfmt(ax):
    ax.yaxis.set_major_formatter(matplotlib.ticker.FuncFormatter(
        lambda v, _: f"{v/1000:g}K" if v >= 1000 else f"{v:g}"))


def rate_axis(ax):
    ax.set_xscale("log")
    ticks = [1000, 2000, 4000, 8000, 16000, 25000, 50000]
    ax.set_xticks(ticks)
    ax.set_xticklabels([f"{t//1000}K" for t in ticks])
    ax.set_xlabel("Target offered rate (msgs/s)")


# ----------------------------------------------------------------------------
def g01(agg):
    fig, axes = plt.subplots(1, 2, figsize=(11, 4.2), sharey=True)
    for ax, r in zip(axes, [1000, 10000]):
        d = sel(agg, "S1_CONN", target_rate=r)
        for m in MECHS:
            dm = d[d["mech"] == m]
            line(ax, dm, "connections", "app_rtt_p50_us", m, ls="--")
            line(ax, dm, "connections", "app_rtt_p99_us", m, ls="-")
        conn_axis(ax); fd_line(ax)
        ax.set_yscale("log"); ax.set_ylim(bottom=10); ax.set_title(f"Offered load {r//1000}K msgs/s")
    axes[0].set_ylabel("App RTT (µs, log)")
    mech_legend(fig, style_handles([("-", "o", "#52514e", "p99"), ("--", "o", "white", "p50")]))
    save(fig, "01_conn_scaling_latency.png", "Connection scaling (50 → 10K): latency p50 / p99")


def g02(agg):
    fig, axes = plt.subplots(1, 2, figsize=(11, 4.2))
    for ax, r in zip(axes, [1000, 10000]):
        d = sel(agg, "S1_CONN", target_rate=r)
        for m in MECHS:
            dm = d[d["mech"] == m]
            line(ax, dm, "connections", "actual_send_rate", m, ls="--")
            line(ax, dm, "connections", "echoed_rate", m, ls="-")
        ax.axhline(r, color="#8a8984", lw=1, ls=":")
        conn_axis(ax); fd_line(ax); kfmt(ax)
        ax.set_ylim(0, r * 1.15); ax.set_title(f"Target {r//1000}K msgs/s (dotted)")
    axes[0].set_ylabel("Messages / s")
    mech_legend(fig, style_handles([("-", "o", "#52514e", "echoed (completed)"), ("--", "o", "white", "sent")]))
    save(fig, "02_conn_scaling_sent_vs_echoed.png", "Connection scaling (50 → 10K): sent vs echoed throughput")


def g03(agg):
    fig, ax = plt.subplots(figsize=(7.5, 4.4))
    d = sel(agg, "S1_CONN_MAX")
    for m in MECHS:
        line(ax, d[d["mech"] == m], "connections", "echoed_rate", m)
    conn_axis(ax); fd_line(ax); kfmt(ax)
    ax.set_ylim(bottom=0); ax.set_ylabel("Echoed msgs / s (closed-loop, W=1)")
    mech_legend(fig)
    save(fig, "03_max_throughput_vs_conns.png", "Max throughput ceiling (closed-loop) vs connections")


def g04(agg):
    fig, axes = plt.subplots(1, 2, figsize=(11, 4.4), sharey=True)
    for ax, (suite, c) in zip(axes, [("S2_RATE", 500), ("S2_RATE_HIGHCONN", 5000)]):
        d = sel(agg, suite)
        ceil = sel(agg, "S1_CONN_MAX", connections=c)
        for m in MECHS:
            dm = d[d["mech"] == m]
            line(ax, dm, "target_rate", "actual_send_rate", m, ls="--")
            line(ax, dm, "target_rate", "echoed_rate", m, ls="-")
            cm = ceil[ceil["mech"] == m]
            if not cm.empty:
                ax.axhline(cm["echoed_rate"].iloc[0], color=COLOR[m], lw=1, ls=":", alpha=0.9)
        xs = np.array([1000, 50000])
        ax.plot(xs, xs, color="#8a8984", lw=1, ls="-", alpha=0.6)
        rate_axis(ax); ax.set_yscale("log"); ax.set_ylim(bottom=500); kfmt(ax)
        ax.set_title(f"{c} connections")
    axes[0].set_ylabel("Messages / s (log)")
    mech_legend(fig, style_handles([("-", "o", "#52514e", "echoed"), ("--", "o", "white", "sent"),
                                    (":", None, None, "closed-loop ceiling")]))
    save(fig, "04_load_sweep_sent_vs_completed.png", "Offered-load sweep: sent vs completed (grey = ideal y=x)")


def g05(agg):
    fig, axes = plt.subplots(2, 3, figsize=(13, 7), sharex=True, sharey=True)
    for row, (suite, c) in enumerate([("S2_RATE", 500), ("S2_RATE_HIGHCONN", 5000)]):
        d = sel(agg, suite)
        for col, (y, nm) in enumerate([("app_rtt_p50_us", "p50"), ("app_rtt_p95_us", "p95"), ("app_rtt_p99_us", "p99")]):
            ax = axes[row, col]
            for m in MECHS:
                line(ax, d[d["mech"] == m], "target_rate", y, m)
            rate_axis(ax); ax.set_yscale("log"); ax.set_ylim(bottom=10)
            ax.set_title(f"{c} conns — {nm}")
            if col == 0:
                ax.set_ylabel("App RTT (µs, log)")
    mech_legend(fig)
    save(fig, "05_load_sweep_latency.png", "Offered-load sweep: latency p50 / p95 / p99")


def g06(agg):
    conns = [c for c in (900, 10000) if not sel(agg, "S3_ACTIVE", connections=c).empty] or [900]
    fig, axes = plt.subplots(2, len(conns), figsize=(5.5 * len(conns), 7), squeeze=False, sharex=True)
    for col, c in enumerate(conns):
        d = sel(agg, "S3_ACTIVE", connections=c)
        for m in MECHS:
            dm = d[d["mech"] == m]
            line(axes[0, col], dm, "share_r", "app_rtt_p99_us", m)
            line(axes[1, col], dm, "share_r", "echoed_rate", m)
        axes[0, col].set_title(f"{c} connections @ 5K msgs/s")
        axes[0, col].set_yscale("log")
        axes[1, col].axhline(5000, color="#8a8984", lw=1, ls=":")
        axes[1, col].set_ylim(0, 5750); kfmt(axes[1, col])
        for ax in axes[:, col]:
            ax.set_xscale("log"); ax.set_xticks([0.05, 0.1, 0.25, 0.5, 1.0])
            ax.set_xticklabels(["5%", "10%", "25%", "50%", "100%"])
        axes[1, col].set_xlabel("Active share of connections")
    axes[0, 0].set_ylabel("App RTT p99 (µs, log)")
    axes[1, 0].set_ylabel("Echoed msgs / s")
    mech_legend(fig)
    save(fig, "06_active_share_sweep.png", "Active-share sweep (idle-socket tax)")


def g07(agg):
    fig, axes = plt.subplots(1, 2, figsize=(11, 4.4))
    for suite, ls in [("S4_PIPE", "-"), ("S4_PIPE_MAX", "--")]:
        d = sel(agg, suite)
        for m in MECHS:
            dm = d[d["mech"] == m]
            line(axes[0], dm, "pipeline_window", "echoed_rate", m, ls=ls)
            line(axes[1], dm, "pipeline_window", "app_rtt_p99_us", m, ls=ls)
    for ax in axes:
        ax.set_xscale("log", base=2); ax.set_xticks([1, 2, 4, 8]); ax.set_xticklabels(["1", "2", "4", "8"])
        ax.set_xlabel("Pipeline window W (500 conns)")
    axes[0].set_ylabel("Echoed msgs / s"); axes[0].set_ylim(bottom=0); kfmt(axes[0])
    axes[0].set_title("Throughput")
    axes[1].set_ylabel("App RTT p99 (µs, log)"); axes[1].set_yscale("log"); axes[1].set_title("Tail latency")
    mech_legend(fig, style_handles([("-", "o", "#52514e", "open-loop 25K"), ("--", "o", "white", "closed-loop")]))
    save(fig, "07_pipeline_window.png", "Pipeline window scaling (W = 1, 2, 4, 8)")


def g08(agg):
    scen = [(500, 15000), (5000, 10000)]
    mets = [("app_rtt_p50_us", "App RTT p50"), ("app_rtt_p99_us", "App RTT p99"), ("e2e_lat_p99_us", "End-to-end p99")]
    fig, axes = plt.subplots(len(scen), 3, figsize=(14, 7.5), squeeze=False)
    width = 0.2
    x = np.arange(len(DISTS))
    for row, (c, r) in enumerate(scen):
        d = sel(agg, "S5_DIST", connections=c, target_rate=r)
        for col, (y, nm) in enumerate(mets):
            ax = axes[row, col]
            present = [m for m in MECHS if not d[d["mech"] == m].empty]
            for i, m in enumerate(present):
                dm = d[d["mech"] == m].set_index("dist").reindex(DISTS)
                off = (i - (len(present) - 1) / 2) * width
                yerr = None
                if (dm["reps"] > 1).any():
                    yerr = np.vstack([dm[y] - dm[y + "_lo"], dm[y + "_hi"] - dm[y]])
                ax.bar(x + off, dm[y], width * 0.9, color=COLOR[m], yerr=yerr,
                       error_kw=dict(elinewidth=1, capsize=2, ecolor="#52514e"), zorder=2)
            ax.set_xticks(x); ax.set_xticklabels([DIST_LABEL[k] for k in DISTS], fontsize=8.5)
            ax.set_yscale("log"); ax.grid(axis="x", visible=False)
            ax.set_title(f"{c} conns @ {r//1000}K avg — {nm}")
            if col == 0:
                ax.set_ylabel("µs (log)")
    handles = [matplotlib.patches.Patch(color=COLOR[m], label=LABEL[m]) for m in MECHS]
    fig.legend(handles=handles, loc="lower center", ncol=4, bbox_to_anchor=(0.5, -0.01))
    save(fig, "08_arrival_distribution.png", "Arrival distribution & burst intensity")


def validity(df):
    flags = []
    for _, r in df.iterrows():
        why = []
        if r.get("client_cpu_pct", 0) >= 90:
            why.append(f"client CPU {r['client_cpu_pct']:.0f}% (loadgen-bound)")
        if r["mode"] == "open-loop" and r["delivery_ratio_pct"] < 95:
            why.append(f"delivery {r['delivery_ratio_pct']:.1f}%")
        errs = int(r.get("conn_failures", 0) + r.get("send_errors", 0) + r.get("recv_errors", 0)
                   + r.get("timeouts", 0) + r.get("mismatched", 0))
        if errs:
            why.append(f"{errs} conn/send/recv/timeout/mismatch errors")
        if r["active_connections"] < round(r["connections"] * r["active_share"]) * 0.99:
            why.append("fewer active conns than requested")
        if why:
            flags.append({"tag": r["tag"], "issues": "; ".join(why)})
    out = pd.DataFrame(flags, columns=["tag", "issues"])
    out.to_csv(os.path.join(OUT, "validity_report.csv"), index=False)
    print(f"validity_report.csv: {len(out)} flagged runs (saturation/delivery<95% is EXPECTED at high load;"
          " client CPU >=90% means that point measures the client, not the server)")


def main():
    os.makedirs(OUT, exist_ok=True)
    df = load()
    print(f"Loaded {len(df)} runs: " + ", ".join(f"{m}={int((df['mech'] == m).sum())}" for m in MECHS)
          + f"; reps={sorted(df['rep'].unique())}")
    agg = aggregate(df)
    agg.to_csv(os.path.join(OUT, "summary.csv"), index=False)
    for fn in (g01, g02, g03, g04, g05, g06, g07, g08):
        try:
            fn(agg)
        except Exception as exc:  # one missing suite must not block the others
            print(f"SKIP {fn.__name__}: {exc!r}")
    validity(df)


if __name__ == "__main__":
    main()
