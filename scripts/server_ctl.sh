#!/usr/bin/env bash
# scripts/server_ctl.sh
# Remote/Local control interface for echo servers on the Server (Omen) machine.
# Commands: start, stop, snapshot, cleanup, status
#
# Assumptions:
# 1. Server binaries are in <repo>/build/echo-<mech>
# 2. Server binds to all interfaces (0.0.0.0) on the configured port
# 3. SIGUSR1 causes the server to append a JSON stats line to --stats-file

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

PID_FILE="/tmp/echo_server.pid"
LOG_FILE="/tmp/echo_server.log"
STATS_FILE="/tmp/server_stats.json"
CGROUP_PROCS="/sys/fs/cgroup/bench/cgroup.procs"

CMD="${1:-help}"

case "$CMD" in
    start)
        MECH="${2:-}"
        PORT="${3:-8080}"
        MSG_SIZE="${4:-512}"
        CPU_CORE="${5:-0}"
        shift 5 2>/dev/null || true
        EXTRA_FLAGS=("$@")

        if [[ -z "$MECH" ]]; then
            echo "[ERROR] Missing mechanism name (select|poll|epoll|uring)" >&2
            exit 1
        fi

        BIN="$REPO_DIR/build/echo-${MECH}"
        if [[ ! -x "$BIN" ]]; then
            echo "[ERROR] Server binary not found or not executable: $BIN" >&2
            exit 1
        fi

        # Clean any prior stale instance
        if [[ -f "$PID_FILE" ]]; then
            OLD_PID=$(cat "$PID_FILE" 2>/dev/null || true)
            if [[ -n "$OLD_PID" ]] && kill -0 "$OLD_PID" 2>/dev/null; then
                kill -TERM "$OLD_PID" 2>/dev/null || true
                sleep 0.2
                kill -9 "$OLD_PID" 2>/dev/null || true
            fi
            rm -f "$PID_FILE"
        fi
        rm -f "$STATS_FILE" "$LOG_FILE"

        # Launch server pinned to specified CPU core
        taskset -c "$CPU_CORE" "$BIN" \
            --port "$PORT" \
            --msg-size "$MSG_SIZE" \
            --buf-size 4096 \
            --nodelay \
            --stats-file "$STATS_FILE" \
            ${EXTRA_FLAGS[@]+"${EXTRA_FLAGS[@]}"} \
            > "$LOG_FILE" 2>&1 &
        
        SERVER_PID=$!
        echo "$SERVER_PID" > "$PID_FILE"

        # Attach to dedicated cgroup if available
        if [[ -w "$CGROUP_PROCS" ]]; then
            echo "$SERVER_PID" > "$CGROUP_PROCS" 2>/dev/null || true
        fi

        # Wait up to 3 seconds for port to open
        PORT_BOUND=0
        for _ in {1..30}; do
            if ss -ltn "( sport = :$PORT )" 2>/dev/null | grep -q ":$PORT"; then
                PORT_BOUND=1
                break
            fi
            # Also check if process died early
            if ! kill -0 "$SERVER_PID" 2>/dev/null; then
                echo "[ERROR] Server process $SERVER_PID terminated immediately. Logs:" >&2
                cat "$LOG_FILE" >&2
                exit 1
            fi
            sleep 0.1
        done

        if [[ $PORT_BOUND -eq 1 ]]; then
            echo "SERVER_STARTED PID=$SERVER_PID MECH=$MECH PORT=$PORT CPU=$CPU_CORE"
        else
            echo "[ERROR] Server PID $SERVER_PID failed to bind port $PORT within 3 seconds" >&2
            exit 1
        fi
        ;;

    stop)
        if [[ -f "$PID_FILE" ]]; then
            PID=$(cat "$PID_FILE" 2>/dev/null || true)
            if [[ -n "$PID" ]] && kill -0 "$PID" 2>/dev/null; then
                kill -TERM "$PID" 2>/dev/null || true
                for _ in {1..20}; do
                    if ! kill -0 "$PID" 2>/dev/null; then
                        break
                    fi
                    sleep 0.1
                done
                if kill -0 "$PID" 2>/dev/null; then
                    kill -9 "$PID" 2>/dev/null || true
                fi
            fi
            rm -f "$PID_FILE"
        fi
        echo "SERVER_STOPPED"
        ;;

    snapshot)
        if [[ ! -f "$PID_FILE" ]]; then
            echo "[ERROR] No running server PID file found" >&2
            exit 1
        fi
        PID=$(cat "$PID_FILE")
        if ! kill -0 "$PID" 2>/dev/null; then
            echo "[ERROR] Server process $PID is no longer running" >&2
            exit 1
        fi

        # Trigger JSON stats dump via SIGUSR1
        kill -USR1 "$PID" 2>/dev/null || true
        sleep 0.05

        # 1. CPU time (user and sys) in microseconds from /proc/<pid>/stat
        # /proc/<pid>/stat fields: 14: utime, 15: stime (in clock ticks)
        CLK_TCK=$(getconf CLK_TCK 2>/dev/null || echo 100)
        STAT_LINE=$(cat "/proc/$PID/stat" 2>/dev/null || true)
        UTIME_TICKS=0
        STIME_TICKS=0
        if [[ -n "$STAT_LINE" ]]; then
            # The executable name may contain spaces/parens, so take fields after last ')'
            REST="${STAT_LINE#*)}"
            read -r _ _ _ _ _ _ _ _ _ _ _ UTIME_TICKS STIME_TICKS _ <<< "$REST"
        fi
        USER_US=$(( (UTIME_TICKS * 1000000) / CLK_TCK ))
        SYS_US=$(( (STIME_TICKS * 1000000) / CLK_TCK ))

        # 2. Peak Memory (VmHWM in kB) from /proc/<pid>/status
        VMHWM_KB=0
        if [[ -f "/proc/$PID/status" ]]; then
            VMHWM_KB=$(awk '/VmHWM:/ {print $2; exit}' "/proc/$PID/status" 2>/dev/null || echo 0)
            VMHWM_KB="${VMHWM_KB:-0}"
        fi

        # 3. Server internal counters from latest JSON dump line in $STATS_FILE
        ACCEPTS=0
        READS=0
        WRITES=0
        ERRORS=0
        PEER_RESETS=0

        if [[ -f "$STATS_FILE" && -s "$STATS_FILE" ]]; then
            LAST_JSON=$(tail -n 1 "$STATS_FILE" 2>/dev/null || true)
            if [[ -n "$LAST_JSON" ]]; then
                ACCEPTS=$(echo "$LAST_JSON" | grep -o '"accepts":[0-9]*' | cut -d':' -f2 || echo 0)
                READS=$(echo "$LAST_JSON" | grep -o '"reads":[0-9]*' | cut -d':' -f2 || echo 0)
                WRITES=$(echo "$LAST_JSON" | grep -o '"writes":[0-9]*' | cut -d':' -f2 || echo 0)
                ERRORS=$(echo "$LAST_JSON" | grep -o '"errors":[0-9]*' | cut -d':' -f2 || echo 0)
                PEER_RESETS=$(echo "$LAST_JSON" | grep -o '"peer_resets":[0-9]*' | cut -d':' -f2 || echo 0)
            fi
        fi

        # Output single structured line for reliable shell parsing
        echo "SNAPSHOT user_us=${USER_US:-0} sys_us=${SYS_US:-0} vmhwm_kb=${VMHWM_KB:-0} accepts=${ACCEPTS:-0} reads=${READS:-0} writes=${WRITES:-0} errors=${ERRORS:-0} peer_resets=${PEER_RESETS:-0}"
        ;;

    cleanup)
        pkill -9 -f "build/echo-" 2>/dev/null || true
        rm -f "$PID_FILE" "$STATS_FILE" "$LOG_FILE"
        echo "SERVER_CLEANUP_DONE"
        ;;

    status)
        if [[ -f "$PID_FILE" ]] && kill -0 "$(cat "$PID_FILE")" 2>/dev/null; then
            echo "RUNNING PID=$(cat "$PID_FILE")"
        else
            echo "STOPPED"
        fi
        ;;

    *)
        echo "Usage: $0 {start <mech> <port> <msgsize> [cpu_core] [flags...]|stop|snapshot|cleanup|status}" >&2
        exit 1
        ;;
esac
