import pandas as pd
import matplotlib.pyplot as plt
import os


# ============================================================
# SETUP
# ============================================================

BASE_DIR = os.path.dirname(os.path.abspath(__file__))

PLOTS_DIR = os.path.join(BASE_DIR, "plots")
os.makedirs(PLOTS_DIR, exist_ok=True)

# Read data
throughput = pd.read_csv(os.path.join(BASE_DIR, "throughput.csv"))
resource = pd.read_csv(os.path.join(BASE_DIR, "resource_usage.csv"))
latency = pd.read_csv(os.path.join(BASE_DIR, "latency.csv"))
syscalls = pd.read_csv(os.path.join(BASE_DIR, "syscalls.csv"))

# Server implementations
servers = [
    "server_select",
    "server_poll",
    "server_epoll",
    "server_iouring"
]

# server_select becomes unstable/unrepresentative beyond this N;
# truncate its line here and mark where it stopped.
SELECT_CUTOFF_N = 1000

# Connection values used in the benchmark
N_VALUES = [
    10, 100, 500, 1000, 1500, 2000,
    2500, 3000, 3500, 4000, 4500,
    5000, 6000, 7500, 9000, 10000
]

N_LABELS = [
    "10", "100", "500", "1K", "1.5K", "2K",
    "2.5K", "3K", "3.5K", "4K", "4.5K",
    "5K", "6K", "7.5K", "9K", "10K"
]

# Map each real N value to an equally spaced index position
N_POS = {n: i for i, n in enumerate(N_VALUES)}


def set_equal_spacing_xticks():
    plt.xticks(
        range(len(N_VALUES)),
        N_LABELS,
        rotation=45
    )
    x_pad = 0.03 * (len(N_VALUES) - 1)
    plt.xlim(-x_pad, len(N_VALUES) - 1 + x_pad)
    plt.margins(y=0.05)


def plot_server_line(df, server, value_col):
    """
    Plot one server's line. For server_select, truncate at
    SELECT_CUTOFF_N and draw a dotted horizontal guide line at
    the last point's y-value across the rest of the plot.
    """
    data = df[df["server"] == server].dropna(subset=["N", value_col])

    if server == "server_select":
        data = data[data["N"] <= SELECT_CUTOFF_N]

    if data.empty:
        return

    data = data.sort_values("N")
    x_pos = data["N"].map(N_POS)

    line, = plt.plot(
        x_pos,
        data[value_col],
        marker="o",
        label=server
    )

    if server == "server_select":
        last_x = x_pos.iloc[-1]
        plt.axvline(
            x=last_x,
            linestyle=":",
            color=line.get_color(),
            alpha=0.6,
            linewidth=1.5
        )


# ============================================================
# 1. DOWNLINK THROUGHPUT vs CONNECTIONS
# ============================================================

plt.figure(figsize=(8, 5))

for server in servers:
    plot_server_line(throughput, server, "bandwidth_down_mbps")

set_equal_spacing_xticks()

plt.xlabel("Number of Connections (N)")
plt.ylabel("Downlink Throughput (Mbps)")
plt.title("Downlink Throughput vs Number of Connections")

plt.legend()
plt.tight_layout()

plt.savefig(
    os.path.join(PLOTS_DIR, "downlink_throughput.png"),
    dpi=300,
    bbox_inches="tight"
)

plt.show()
plt.close()


# ============================================================
# 2. PACKET RATE vs CONNECTIONS
# ============================================================

plt.figure(figsize=(8, 5))

for server in servers:
    plot_server_line(throughput, server, "packet_rate")

set_equal_spacing_xticks()

plt.xlabel("Number of Connections (N)")
plt.ylabel("Packet Rate (packets/sec)")
plt.title("Packet Rate vs Number of Connections")

plt.legend()
plt.tight_layout()

plt.savefig(
    os.path.join(PLOTS_DIR, "packet_rate.png"),
    dpi=300,
    bbox_inches="tight"
)

plt.show()
plt.close()


# ============================================================
# 3. CPU UTILISATION vs CONNECTIONS
# ============================================================

plt.figure(figsize=(8, 5))

for server in servers:
    plot_server_line(resource, server, "avg_cpu_pct")

set_equal_spacing_xticks()

plt.xlabel("Number of Connections (N)")
plt.ylabel("Average CPU Utilisation (%)")
plt.title("CPU Utilisation vs Number of Connections")

plt.legend()
plt.tight_layout()

plt.savefig(
    os.path.join(PLOTS_DIR, "cpu_utilisation.png"),
    dpi=300,
    bbox_inches="tight"
)

plt.show()
plt.close()


# ============================================================
# 4. VOLUNTARY CONTEXT SWITCHES vs CONNECTIONS
# ============================================================

plt.figure(figsize=(8, 5))

for server in servers:
    plot_server_line(resource, server, "avg_vol_cswch")

set_equal_spacing_xticks()

# Large dynamic range (0 to ~1300) - symlog makes small-server
# differences visible without hiding the big spikes.
plt.yscale("symlog", linthresh=1)

plt.xlabel("Number of Connections (N)")
plt.ylabel("Average Voluntary Context Switches")
plt.title("Voluntary Context Switches vs Number of Connections")

plt.legend()
plt.tight_layout()

plt.savefig(
    os.path.join(PLOTS_DIR, "voluntary_context_switches.png"),
    dpi=300,
    bbox_inches="tight"
)

plt.show()
plt.close()


# ============================================================
# 5. INVOLUNTARY CONTEXT SWITCHES vs CONNECTIONS
# ============================================================

plt.figure(figsize=(8, 5))

for server in servers:
    plot_server_line(resource, server, "avg_invol_cswch")

set_equal_spacing_xticks()

# Same reasoning - poll/iouring spikes swamp the rest on a linear axis.
plt.yscale("symlog", linthresh=1)

plt.xlabel("Number of Connections (N)")
plt.ylabel("Average Involuntary Context Switches")
plt.title("Involuntary Context Switches vs Number of Connections")

plt.legend()
plt.tight_layout()

plt.savefig(
    os.path.join(PLOTS_DIR, "involuntary_context_switches.png"),
    dpi=300,
    bbox_inches="tight"
)

plt.show()
plt.close()


# ============================================================
# 6. 99th PERCENTILE LATENCY vs CONNECTIONS
# ============================================================

plt.figure(figsize=(8, 5))

for server in servers:
    plot_server_line(latency, server, "latency_p99_ms")

set_equal_spacing_xticks()

# select/poll/epoll blow up to 2500ms+ while iouring stays low -
# symlog keeps the low end (iouring, early N) readable.
plt.yscale("symlog", linthresh=10)

plt.xlabel("Number of Connections (N)")
plt.ylabel("99th Percentile Latency (ms)")
plt.title("99th Percentile Latency vs Number of Connections")

plt.legend()
plt.tight_layout()

plt.savefig(
    os.path.join(PLOTS_DIR, "latency_p99.png"),
    dpi=300,
    bbox_inches="tight"
)

plt.show()
plt.close()


# ============================================================
# 7. TOTAL SYSTEM CALLS vs CONNECTIONS
# ============================================================

# Convert calls to numeric
syscalls["calls"] = pd.to_numeric(syscalls["calls"], errors="coerce")

# Sum all syscall types for each server and N
syscall_totals = (
    syscalls
    .dropna(subset=["N", "calls"])
    .groupby(["server", "N"])["calls"]
    .sum()
    .reset_index()
)

plt.figure(figsize=(8, 5))

for server in servers:
    plot_server_line(syscall_totals, server, "calls")

set_equal_spacing_xticks()

plt.yscale("log")

plt.xlabel("Number of Connections (N)")
plt.ylabel("Total System Calls")
plt.title("Total System Calls vs Number of Connections")

plt.legend()
plt.tight_layout()

plt.savefig(
    os.path.join(PLOTS_DIR, "total_syscalls.png"),
    dpi=300,
    bbox_inches="tight"
)

plt.show()
plt.close()


# ============================================================
# DONE
# ============================================================

print()
print("============================================")
print("All 7 plots generated successfully.")
print("============================================")
print()
print("Saved to:")
print(PLOTS_DIR)
print()