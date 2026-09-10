#!/bin/bash
# latency_bench.sh — latency test for server_select/poll/epoll/iouring
# Run from benchmarking_scripts/ (servers expected in ../code)
# Standalone: only produces latency.csv. No pidstat/CPU sampling here —
# see pidstat_bench.sh for resource_usage.csv.
# Note: no 'set -e' — a bad/unparseable run at one N should not kill the whole sweep

CODE_DIR="$(dirname "$0")/../code"
RESULTS_DIR="$(dirname "$0")/results"
mkdir -p "$RESULTS_DIR/raw"

SERVERS=("server_select" "server_poll" "server_epoll" "server_iouring")
N_VALUES=(10 100 1000 1500 2000 2500 3500 4000 4500 5000 6000 7500 9000 10000)
PORT=8080
DURATION=10
CONNECT_RATE=10000
MSG_RATE=100    # fixed msg/s per connection, keep constant across all runs
MARKER="MARK"
MSG="${MARKER}$(python3 -c 'print("x"*508)')"   # 512 bytes total, marker at the start so tcpkali can detect message boundaries
OUTFILE="$RESULTS_DIR/latency.csv"

ulimit -n 65536

# Clear the port in case a stray process from an earlier interrupted run
# (e.g. a manual Ctrl-C) is still bound to it.
fuser -k ${PORT}/tcp 2>/dev/null || true
sleep 1

echo "server,N,connect_rate,msg_rate,latency_p50_ms,latency_p95_ms,latency_p99_ms,test_duration_s" > "$OUTFILE"

for server in "${SERVERS[@]}"; do
    BIN="$CODE_DIR/$server"

    if [ ! -x "$BIN" ]; then
        echo "Skipping $server (binary not found, run setup_bench_env.sh first)"
        continue
    fi

    for N in "${N_VALUES[@]}"; do
        echo "=== Latency: $server, N=$N ==="
        TAG="${server}_N${N}"

        "$BIN" $PORT 16384 > "$RESULTS_DIR/raw/${TAG}_lat_server.log" 2>&1 &
        SERVER_PID=$!
        sleep 1

        tcpkali -c "$N" --connect-rate="$CONNECT_RATE" \
            -m "$MSG" -r "$MSG_RATE" \
            --latency-marker "$MARKER" \
            --latency-percentiles=50,95,99 \
            -T ${DURATION}s localhost:$PORT \
            > "$RESULTS_DIR/raw/${TAG}_lat_tcpkali.log" 2>&1 || true

        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true

        TKLOG="$RESULTS_DIR/raw/${TAG}_lat_tcpkali.log"

        parse_latencies() {
            local log="$1"
            local line

            line=$(grep -i "latency at percentiles" "$log" | head -1)

            if [[ -n "$line" ]]; then
                echo "$line" | grep -oP \
                    'percentiles:\s*\K[\d.]+/[\d.]+/[\d.]+(?=\s*ms)'
            fi
        }

        LATVALS=$(parse_latencies "$TKLOG")

        if [[ -n "$LATVALS" ]]; then
            IFS='/' read -r P50 P95 P99 <<< "$LATVALS"
        else
            P50=""
            P95=""
            P99=""
        fi

        DUR=$(grep "Test duration" "$TKLOG" | grep -oP '[\d.]+(?= s)')

        echo "$server,$N,$CONNECT_RATE,$MSG_RATE,${P50:-NaN},${P95:-NaN},${P99:-NaN},${DUR:-NaN}" >> "$OUTFILE"

        # Cleanup before next run
        kill -9 "$SERVER_PID" 2>/dev/null || true
        pkill -9 -f "$BIN" 2>/dev/null || true
        fuser -k ${PORT}/tcp 2>/dev/null || true
        sleep 2
    done
done
echo "Done. Results: $OUTFILE (raw logs in $RESULTS_DIR/raw/)"