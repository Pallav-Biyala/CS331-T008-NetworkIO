#!/bin/bash
# syscall_bench.sh — syscall count/time profiling for server_select/poll/epoll/iouring
# Run from benchmarking_scripts/ (servers expected in ../code)
# Standalone: only produces syscalls.csv. Uses strace -c.
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
MARKER="MARK"
MSG="${MARKER}$(python3 -c 'print("x"*508)')"

OUTFILE="$RESULTS_DIR/syscalls.csv"

ulimit -n 65536

# Clear port in case a previous run left a server alive.
fuser -k ${PORT}/tcp 2>/dev/null || true
sleep 1

echo "server,N,syscall,calls,seconds_total,seconds_per_call,errors" > "$OUTFILE"


for server in "${SERVERS[@]}"; do

    BIN="$CODE_DIR/$server"

    if [ ! -x "$BIN" ]; then
        echo "Skipping $server (binary not found, run setup_bench_env.sh first)"
        continue
    fi

    for N in "${N_VALUES[@]}"; do

        echo "=== Syscalls: $server, N=$N ==="

        TAG="${server}_N${N}"
        STRACE_LOG="$RESULTS_DIR/raw/${TAG}_strace.log"

        # Start server under strace.
        strace -c \
            -o "$STRACE_LOG" \
            "$BIN" $PORT 16384 \
            > "$RESULTS_DIR/raw/${TAG}_sys_server.log" 2>&1 &

        STRACE_PID=$!

        sleep 1

        # Drive the server with the same workload used by latency benchmark.
        tcpkali \
            -c "$N" \
            --connect-rate="$CONNECT_RATE" \
            -m "$MSG" \
            -r "$MSG_RATE" \
            --latency-marker "$MARKER" \
            -T ${DURATION}s \
            localhost:$PORT \
            > "$RESULTS_DIR/raw/${TAG}_sys_tcpkali.log" 2>&1 || true

        # Find the actual server process launched by strace.
        SERVER_PID=$(pgrep -f "^$BIN $PORT 16384" | head -1)

        # Ask server to terminate so strace can write its summary.
        if [ -n "$SERVER_PID" ]; then
            kill "$SERVER_PID" 2>/dev/null || true
        fi

        # Wait for strace to finish and flush its output.
        wait "$STRACE_PID" 2>/dev/null || true


        # ============================================================
        # Parse strace -c output
        # ============================================================

        PARSED_ROWS=0

        if [ -s "$STRACE_LOG" ]; then

            while IFS= read -r line; do

                # Ignore headers, separators and total line.
                [[ "$line" =~ ^[[:space:]]*% ]] && continue
                [[ "$line" =~ ^[[:space:]]*-+ ]] && continue
                [[ "$line" =~ total[[:space:]]*$ ]] && continue

                # Split whitespace-separated fields.
                read -ra FIELDS <<< "$line"

                # Need at least:
                # %time seconds usecs/call calls syscall
                if [ "${#FIELDS[@]}" -lt 5 ]; then
                    continue
                fi

                SECONDS_TOTAL="${FIELDS[1]}"
                CALLS="${FIELDS[3]}"

                # If there is an errors column:
                #
                # %time seconds usecs/call calls errors syscall
                #
                if [ "${#FIELDS[@]}" -ge 6 ]; then
                    ERRORS="${FIELDS[4]}"
                    SYSCALL="${FIELDS[5]}"
                else
                    ERRORS="0"
                    SYSCALL="${FIELDS[4]}"
                fi

                # Make sure calls is numeric before calculating.
                if [[ "$CALLS" =~ ^[0-9]+$ ]] && [ "$CALLS" -gt 0 ]; then
                    SECONDS_PER_CALL=$(awk \
                        -v seconds="$SECONDS_TOTAL" \
                        -v calls="$CALLS" \
                        'BEGIN { printf "%.9f", seconds / calls }')
                else
                    SECONDS_PER_CALL="NaN"
                fi

                echo "$server,$N,$SYSCALL,$CALLS,$SECONDS_TOTAL,$SECONDS_PER_CALL,$ERRORS" \
                    >> "$OUTFILE"

                PARSED_ROWS=$((PARSED_ROWS + 1))

            done < "$STRACE_LOG"

        fi


        # If strace produced no usable rows, record a NaN row.
        if [ "$PARSED_ROWS" -eq 0 ]; then
            echo "$server,$N,NaN,NaN,NaN,NaN,NaN" >> "$OUTFILE"
        fi


        # ============================================================
        # Cleanup before next run
        # ============================================================

        if [ -n "$SERVER_PID" ]; then
            kill -9 "$SERVER_PID" 2>/dev/null || true
        fi

        kill -9 "$STRACE_PID" 2>/dev/null || true

        pkill -9 -f "$BIN" 2>/dev/null || true
        fuser -k ${PORT}/tcp 2>/dev/null || true

        sleep 2

    done
done


echo "Done. Results: $OUTFILE"
echo "Raw logs: $RESULTS_DIR/raw/"