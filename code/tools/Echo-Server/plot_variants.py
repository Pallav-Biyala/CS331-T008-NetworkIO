import glob, os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

OUT = "code/benchmarking-graphs/echo/variants"
os.makedirs(OUT, exist_ok=True)
pd.set_option("display.width", 250); pd.set_option("display.max_columns", 30); pd.set_option("display.max_rows", 200)

# ---------------------------------------------------------------- load
v = pd.concat([pd.read_csv(f).assign(src=f) for f in sorted(glob.glob("code/results/Echo-Server/variants/*/results.csv"))], ignore_index=True)
v["label"] = v.tag.str.split("_rep").str[0]
v["case"] = v.tag.str.split("_rep1_").str[1]
v["run"] = v.groupby("tag").cumcount() + 1

b = pd.concat([pd.read_csv(f).assign(src=f) for f in sorted(glob.glob("code/results/Echo-Server/*_rep1_*/results.csv"))], ignore_index=True)
b["label"] = b.tag.str.split("_rep").str[0]
b["case"] = b.tag.str.split("_rep1_").str[1]
b["run"] = b.groupby("tag").cumcount() + 1

GROUPS = {  # mechanism -> [(label, legend name)]
    "epoll": [("epolloneshot", "--epoll-oneshot"), ("epollmaxev", "--epoll-maxevents=1"),
              ("epollet", "--epoll-et"), ("epollnagle", "--nodelay=0"), ("epollbuf", "--buf-size 512")],
    "uring": [("uringsmall", "--uring-entries 256"), ("uringnagle", "--nodelay=0")],
    "poll": [("pollnagle", "--nodelay=0")],
    "select": [("selectnagle", "--nodelay=0")],
}
SLOT = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4"]
MARK = ["o", "s", "^", "D", "v"]
NAME = {"uring": "io_uring", "epoll": "epoll", "poll": "poll", "select": "select"}


def series(df, label, cases, col):
    out = []
    for c in cases:
        d = df[(df.label == label) & (df.case == c)][col].to_numpy(dtype=float)
        out.append((np.median(d), d.min(), d.max()) if len(d) else (np.nan, np.nan, np.nan))
    return np.array(out)


def draw(ax, x, arr, color, marker, ls, label, hollow=False):
    m = ~np.isnan(arr[:, 0])
    if not m.any():
        return
    yerr = np.vstack([arr[m, 0] - arr[m, 1], arr[m, 2] - arr[m, 0]])
    ax.errorbar(np.array(x)[m], arr[m, 0], yerr=yerr if (yerr > 0).any() else None, color=color, ls=ls,
                marker=marker, mfc="white" if hollow else color, mec=color, mew=1.5, lw=2, ms=6,
                capsize=3, elinewidth=1, label=label)


def style(ax):
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
    ax.grid(color="#e6e5e0", lw=0.6)
    ax.set_axisbelow(True)


def kfmt(ax):
    ax.yaxis.set_major_formatter(matplotlib.ticker.FuncFormatter(lambda val, _: f"{val/1000:g}K"))


# ------------------------------------------------------------ figure 1: throughput
fig, axes = plt.subplots(4, 2, figsize=(12, 14))
for r, (mech, vars_) in enumerate(GROUPS.items()):
    a1, a2 = axes[r]
    xw, cw = [1, 8], ["S1_CONN_MAX_c500_closed", "S4_PIPE_MAX_c500_w8_closed"]
    xc, cc = [500, 10000], ["S1_CONN_MAX_c500_closed", "S1_CONN_MAX_c10000_closed"]
    draw(a1, xw, series(b, mech, cw, "echoed_rate"), "#141a21", "o", "--", "Rep 1 baseline", hollow=True)
    if mech != "select":
        draw(a2, xc, series(b, mech, cc, "echoed_rate"), "#141a21", "o", "--", "Rep 1 baseline", hollow=True)
    for i, (lab, nm) in enumerate(vars_):
        draw(a1, xw, series(v, lab, cw, "echoed_rate"), SLOT[i], MARK[i], "-", nm)
        if mech != "select":
            draw(a2, xc, series(v, lab, cc, "echoed_rate"), SLOT[i], MARK[i], "-", nm)
    a1.set_xscale("log", base=2); a1.set_xticks(xw); a1.set_xticklabels(["1", "8"])
    a1.set_xlabel("Requests in flight per connection (500 conns, closed loop)")
    a2.set_xscale("log"); a2.set_xticks(xc); a2.set_xticklabels(["500", "10,000"])
    a2.set_xlabel("Connections (closed loop, 1 in flight)")
    for a in (a1, a2):
        style(a); kfmt(a); a.set_ylim(0, 200000)
    a1.set_ylabel(f"{NAME[mech]}\nEchoed msgs/s")
    a1.legend(fontsize=8, loc="lower right", frameon=False)
    if mech == "select":
        a2.cla()
        a2.set_xticks([]); a2.set_yticks([])
        for s in a2.spines.values():
            s.set_color("#d6dce4")
        a2.text(0.5, 0.5, "select cannot serve 10,000 connections\n(FD_SETSIZE = 1024)", ha="center",
                va="center", transform=a2.transAxes, color="#586373")
axes[0, 0].set_title("Ceiling vs requests in flight", fontweight="bold")
axes[0, 1].set_title("Ceiling vs connections", fontweight="bold")
fig.suptitle("Flag variants: throughput (dashed = Rep 1 baseline; whiskers = min–max over repeated runs)",
             fontweight="bold", fontsize=11.5)
fig.tight_layout(rect=(0, 0, 1, 0.97))
fig.savefig(f"{OUT}/01_variant_throughput.png", dpi=150, bbox_inches="tight")
plt.close(fig)

# ------------------------------------------------------------ figure 2: latency
fig, axes = plt.subplots(4, 2, figsize=(12, 14))
xr, cr = [1000, 25000, 50000], ["S2_RATE_c500_r1000", "S2_RATE_c500_r25000", "S2_RATE_c500_r50000"]
for r, (mech, vars_) in enumerate(GROUPS.items()):
    for col, (metric, nm_) in enumerate([("app_rtt_p50_us", "p50"), ("app_rtt_p99_us", "p99")]):
        a = axes[r, col]
        draw(a, xr, series(b, mech, cr, metric), "#141a21", "o", "--", "Rep 1 baseline", hollow=True)
        for i, (lab, nm) in enumerate(vars_):
            draw(a, xr, series(v, lab, cr, metric), SLOT[i], MARK[i], "-", nm)
        a.set_xscale("log"); a.set_xticks(xr); a.set_xticklabels(["1K", "25K", "50K"])
        a.set_ylim(bottom=0, top=a.get_ylim()[1] * 1.15)   # linear from zero: differences at true size
        a.set_xlabel("Offered load (msgs/s, 500 conns)")
        style(a)
        if col == 0:
            a.set_ylabel(f"{NAME[mech]}\nRTT (µs)")
            a.legend(fontsize=8, loc="lower right", frameon=False, ncol=2)
axes[0, 0].set_title("Median (p50) RTT vs offered load", fontweight="bold")
axes[0, 1].set_title("Tail (p99) RTT vs offered load", fontweight="bold")
fig.suptitle("Flag variants: latency (dashed = Rep 1 baseline; whiskers = min–max over repeated runs)",
             fontweight="bold", fontsize=11.5)
fig.tight_layout(rect=(0, 0, 1, 0.97))
fig.savefig(f"{OUT}/02_variant_latency.png", dpi=150, bbox_inches="tight")
plt.close(fig)

# ------------------------------------------------------------ validity checks
print("== 1. Measurement timing (warm-up is not in the CSV; duration_s is) ==")
print("Rep 1 duration_s:", sorted(b.duration_s.unique()), "| variants duration_s:", sorted(v.duration_s.unique()))

print("\n== 2. Drift check per session: p50 of r1000end / r1000 (flag outside 0.77-1.30) ==")
rows = []
for (lab, run), d in v.groupby(["label", "run"]):
    s = d.set_index("case")
    if "S2_RATE_c500_r1000" in s.index and "S2_RATE_c500_r1000end" in s.index:
        a, e = s.loc["S2_RATE_c500_r1000", "app_rtt_p50_us"], s.loc["S2_RATE_c500_r1000end", "app_rtt_p50_us"]
        rows.append((lab, run, a, e, round(e / a, 2), "FLAG" if not 0.77 <= e / a <= 1.30 else ""))
print(pd.DataFrame(rows, columns=["label", "run", "p50_start", "p50_end", "ratio", ""]).to_string(index=False))

print("\n== 3. Errors, delivery < 99% (open loop), client CPU >= 90% ==")
allr = pd.concat([v.assign(set="variant"), b.assign(set="rep1")], ignore_index=True)
allr["errs"] = allr[["conn_failures", "send_errors", "recv_errors", "timeouts", "mismatched"]].sum(axis=1)
bad = allr[(allr.errs > 0) | ((allr["mode"] == "open-loop") & (allr.delivery_ratio_pct < 99)) | (allr.client_cpu_pct >= 90)]
bad = bad[bad.label.isin([l for g in GROUPS.values() for l, _ in g]) |
          bad.case.isin(["S1_CONN_MAX_c500_closed", "S4_PIPE_MAX_c500_w8_closed", "S1_CONN_MAX_c10000_closed",
                         "S2_RATE_c500_r1000", "S2_RATE_c500_r25000", "S2_RATE_c500_r50000", "S1_CONN_c10000_r10000"])]
print(bad[["set", "tag", "run", "errs", "timeouts", "delivery_ratio_pct", "client_cpu_pct"]].to_string(index=False))

print("\n== 4. Little's law (closed loop): avg RTT vs conns*W/throughput; ratio should be <= 1 and not tiny ==")
cl = allr[(allr["mode"] == "closed-loop") & allr.case.isin(
    ["S1_CONN_MAX_c500_closed", "S4_PIPE_MAX_c500_w8_closed", "S1_CONN_MAX_c10000_closed"])].copy()
cl["expected_us"] = cl.active_connections * cl.pipeline_window / cl.echoed_rate * 1e6
cl["ratio"] = (cl.rtt_avg_us / cl.expected_us).round(2)
cl["flag"] = np.where((cl.ratio < 0.5) | (cl.ratio > 1.15), "FLAG", "")
keep = cl.label.isin(["epoll", "uring", "poll", "select"] + [l for g in GROUPS.values() for l, _ in g])
print(cl[keep][["set", "tag", "run", "echoed_rate", "rtt_avg_us", "expected_us", "ratio", "flag"]]
      .round(0).to_string(index=False))

print("\n== 5. Same-config repeats: epolloneshot run 1 vs run 2 ==")
e = v[v.label == "epolloneshot"].pivot_table(index="case", columns="run",
                                             values=["echoed_rate", "app_rtt_p50_us", "app_rtt_p99_us"])
for m in ["echoed_rate", "app_rtt_p50_us", "app_rtt_p99_us"]:
    e[(m, "diff_%")] = ((e[(m, 2)] - e[(m, 1)]) / e[(m, 1)] * 100).round(1)
print(e.round(0).to_string())

print("\n== 6. Baseline repeats: Rep 1 tags that occur more than once ==")
dup = b[b.groupby("tag").tag.transform("size") > 1]
print(dup.groupby("label").src.unique().to_string())
pc = dup[dup.case.isin(["S2_RATE_c500_r1000", "S2_RATE_c500_r25000", "S1_CONN_MAX_c500_closed",
                        "S4_PIPE_MAX_c500_w8_closed"])]
print(pc[["tag", "run", "src", "echoed_rate", "app_rtt_p50_us", "app_rtt_p99_us"]].to_string(index=False))

print("\n== 7. select at 10,000 connections ==")
s10 = v[(v.label == "selectnagle") & v.case.str.contains("c10000")]
print(s10[["tag", "mode", "connections", "active_connections", "max_fd", "scheduled", "total_sent",
           "total_received", "incomplete_at_end", "delivery_ratio_pct", "echoed_rate", "rtt_avg_us"]].to_string(index=False))
