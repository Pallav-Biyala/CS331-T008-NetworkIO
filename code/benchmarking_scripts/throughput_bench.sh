#!/bin/bash
# throughput_bench.sh — throughput test for server_select/poll/epoll/iouring
# Run from benchmarking_scripts/ (servers expected in ../code)
# Standalone: only produces throughput.csv. No pidstat/CPU sampling here —
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
MSG=$(python3 -c 'print("x"*512)')
OUTFILE="$RESULTS_DIR/throughput.csv"

ulimit -n 65536

# Clear the port in case a stray process from an earlier interrupted run
# (e.g. a manual Ctrl-C) is still bound to it.
fuser -k ${PORT}/tcp 2>/dev/null || true
sleep 1

echo "server,N,connect_rate,bandwidth_down_mbps,bandwidth_up_mbps,packet_rate,test_duration_s" > "$OUTFILE"

for server in "${SERVERS[@]}"; do
    BIN="$CODE_DIR/$server"
    if [ ! -x "$BIN" ]; then
        echo "Skipping $server (binary not found, run setup_bench_env.sh first)"
        continue
    fi

    for N in "${N_VALUES[@]}"; do
        echo "=== Throughput: $server, N=$N ==="
        TAG="${server}_N${N}"

        "$BIN" $PORT 16384 > "$RESULTS_DIR/raw/${TAG}_server.log" 2>&1 &
        SERVER_PID=$!
        sleep 1

        tcpkali -c "$N" --connect-rate="$CONNECT_RATE" -m "$MSG" -T ${DURATION}s \
            localhost:$PORT > "$RESULTS_DIR/raw/${TAG}_tcpkali.log" 2>&1 || true

        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true

        TKLOG="$RESULTS_DIR/raw/${TAG}_tcpkali.log"
        DOWN=$(grep "Aggregate bandwidth" "$TKLOG" | grep -oP '[\d.]+(?=↓)')
        UP=$(grep "Aggregate bandwidth" "$TKLOG" | grep -oP '[\d.]+(?=↑)')
        PKT=$(grep "Packet rate estimate" "$TKLOG" | grep -oP '[\d.]+' | head -1)
        DUR=$(grep "Test duration" "$TKLOG" | grep -oP '[\d.]+(?= s)')

        echo "$server,$N,$CONNECT_RATE,${DOWN:-NaN},${UP:-NaN},${PKT:-NaN},${DUR:-NaN}" >> "$OUTFILE"

        # Cleanup: make sure nothing is still holding the port before the
        # next run — catches orphaned server processes (e.g. from a manual
        # Ctrl-C) that would otherwise cause "Address already in use" on
        # the next bind.
        kill -9 "$SERVER_PID" 2>/dev/null || true
        pkill -9 -f "$BIN" 2>/dev/null || true
        fuser -k ${PORT}/tcp 2>/dev/null || true
        sleep 2
    done
done

echo "Done. Results: $OUTFILE (raw logs in $RESULTS_DIR/raw/)"