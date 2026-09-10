#!/bin/bash
# pidstat_bench.sh — dedicated CPU / context-switch resource sampling
# for server_select/poll/epoll/iouring.
# Run from benchmarking_scripts/ (servers expected in ../code)
# Standalone: only produces resource_usage.csv. Throughput/latency numbers
# from tcpkali in this script are ignored — use throughput_bench.sh /
# latency_bench.sh for those.
# Note: no 'set -e' — a bad/unparseable run at one N should not kill the whole sweep

CODE_DIR="$(dirname "$0")/../code"
RESULTS_DIR="$(dirname "$0")/results"
mkdir -p "$RESULTS_DIR/raw"

SERVERS=("server_select" "server_poll" "server_epoll" "server_iouring")
N_VALUES=(10 100 1000 1500 2000 2500 3500 4000 4500 5000 6000 7500 9000 10000)
PORT=8080
DURATION=10
CONNECT_RATE=10000
MSG_RATE=100
OUTFILE="$RESULTS_DIR/resource_usage.csv"

ulimit -n 65536

# Clear the port in case a stray process from an earlier interrupted run
# (e.g. a manual Ctrl-C) is still bound to it.
fuser -k ${PORT}/tcp 2>/dev/null || true
sleep 1

echo "server,N,connect_rate,avg_cpu_pct,max_cpu_pct,avg_vol_cswch,avg_invol_cswch,sample_count,test_duration_s" > "$OUTFILE"

for server in "${SERVERS[@]}"; do
    BIN="$CODE_DIR/$server"
    if [ ! -x "$BIN" ]; then
        echo "Skipping $server (binary not found, run setup_bench_env.sh first)"
        continue
    fi

    for N in "${N_VALUES[@]}"; do
        echo "=== Resource usage: $server, N=$N ==="
        TAG="${server}_N${N}"

        "$BIN" $PORT 16384 > "$RESULTS_DIR/raw/${TAG}_rsrc_server.log" 2>&1 &
        SERVER_PID=$!
        sleep 1

        # stdbuf -oL: line-buffer pidstat's stdout so each 1s sample is
        # flushed to disk immediately, not held until process exit.
        PIDLOG="$RESULTS_DIR/raw/${TAG}_rsrc_pidstat.log"
        stdbuf -oL pidstat -u -w -p $SERVER_PID 1 > "$PIDLOG" 2>&1 &
        SAMPLER_PID=$!

        # tcpkali here is purely to generate load so the server isn't idle
        # while pidstat samples it — its own throughput/latency numbers
        # are ignored, that's what throughput_bench.sh / latency_bench.sh are for.
        tcpkali -c "$N" --connect-rate="$CONNECT_RATE" -m "$MSG" -T ${DURATION}s \
            localhost:$PORT > "$RESULTS_DIR/raw/${TAG}_rsrc_tcpkali.log" 2>&1 || true

        kill -INT "$SAMPLER_PID" 2>/dev/null || true
        sleep 1.5
        wait "$SAMPLER_PID" 2>/dev/null || true
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true

        DUR=$(grep "Test duration" "$RESULTS_DIR/raw/${TAG}_rsrc_tcpkali.log" | grep -oP '[\d.]+(?= s)')

        # Single awk pass: track which header block we're under (%CPU block
        # vs cswch/s block) and aggregate avg + max CPU, avg vol/involuntary
        # switches, and count how many real sample rows were seen — so a
        # zero-sample run reports 0 explicitly instead of silently NaN.
        read -r AVG_CPU MAX_CPU AVG_VOL AVG_INVOL NSAMP < <(awk '
            /%CPU/ && /Command/ {
                mode="cpu"
                for(i=1;i<=NF;i++) if($i=="%CPU") cpu_col=i
                next
            }
            /cswch\/s/ && /Command/ {
                mode="sw"
                for(i=1;i<=NF;i++) {
                    if($i=="cswch/s") vol_col=i
                    if($i=="nvcswch/s") invol_col=i
                }
                next
            }
            /Linux/ || NF==0 { next }
            mode=="cpu" && cpu_col {
                cpu_sum+=$cpu_col; cpu_n++
                if($cpu_col+0 > max_cpu+0) max_cpu=$cpu_col
            }
            mode=="sw" && vol_col {
                vol_sum+=$vol_col; invol_sum+=$invol_col; sw_n++
            }
            END {
                avg_cpu = (cpu_n>0) ? cpu_sum/cpu_n : "NaN"
                avg_vol = (sw_n>0) ? vol_sum/sw_n : "NaN"
                avg_invol = (sw_n>0) ? invol_sum/sw_n : "NaN"
                mc = (cpu_n>0) ? max_cpu : "NaN"
                printf "%s %s %s %s %d", avg_cpu, mc, avg_vol, avg_invol, cpu_n
            }
        ' "$PIDLOG")

        echo "$server,$N,$CONNECT_RATE,${AVG_CPU:-NaN},${MAX_CPU:-NaN},${AVG_VOL:-NaN},${AVG_INVOL:-NaN},${NSAMP:-0},${DUR:-NaN}" >> "$OUTFILE"

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