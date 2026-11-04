#!/usr/bin/env bash
# run_experiment.sh
# Master Benchmark Orchestrator for CS331-T008 Scalable Network I/O
# Features:
# 1. Built-in self-test runner (--self-test)
# 2. Fast 3-minute pilot check (--pilot) across select, epoll, and io_uring at 5k & 25k msgs/s
# 3. Full 5-suite, 4-repetition Latin-Square benchmark suite (--full)
# 4. Optional SSH or local automated server process lifecycle management (SSH_SERVER)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$SCRIPT_DIR"

LOADGEN_BIN="$REPO_DIR/echo_loadgen"
RESULTS_DIR="$REPO_DIR/results"
JSON_DIR="$RESULTS_DIR/json"
MASTER_CSV="$RESULTS_DIR/results.csv"

# Configuration variables from environment or defaults
HOST="${HOST:-127.0.0.1}"
PORT="${PORT:-8080}"
SSH_SERVER="${SSH_SERVER:-}"
SERVER_REPO="${SERVER_REPO:-$REPO_DIR}"
DRY_RUN="${DRY_RUN:-0}"

mkdir -p "$RESULTS_DIR" "$JSON_DIR"

log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*"
}

# -----------------------------------------------------------------------------
# Remote / Local Server Control Helper
# -----------------------------------------------------------------------------
start_server() {
    local mech="$1"
    log "[SERVER] Starting server backend: echo-${mech} on port ${PORT}..."

    if [[ -n "$SSH_SERVER" ]]; then
        # Remote execution over SSH
        ssh -o BatchMode=yes -o ConnectTimeout=5 "$SSH_SERVER" \
            "${SERVER_REPO}/scripts/server_ctl.sh start ${mech} ${PORT} 512 0"
    else
        # Local execution if binary exists
        local srv_bin="$REPO_DIR/build/echo-${mech}"
        if [[ -x "$srv_bin" ]]; then
            pkill -f "echo-" 2>/dev/null || true
            sleep 0.2
            "$srv_bin" --port "$PORT" --msg-size 512 --buf-size 4096 --nodelay >/dev/null 2>&1 &
            sleep 0.5
        fi
    fi
}

stop_server() {
    local mech="$1"
    log "[SERVER] Stopping server backend: echo-${mech}..."

    if [[ -n "$SSH_SERVER" ]]; then
        ssh -o BatchMode=yes -o ConnectTimeout=5 "$SSH_SERVER" \
            "${SERVER_REPO}/scripts/server_ctl.sh stop" >/dev/null 2>&1 || true
    else
        pkill -f "echo-" 2>/dev/null || true
    fi
    sleep 0.5
}

# -----------------------------------------------------------------------------
# Run Loadgen Workload Helper
# -----------------------------------------------------------------------------
run_workload() {
    local mech="$1"
    local exp_id="$2"
    local rep="$3"
    local conns="$4"
    local rate="$5"
    local mode="$6"
    local pipe_w="$7"
    local active_s="$8"
    local dist="$9"
    local dur="${10}"
    local warmup="${11}"
    local cooldown="${12}"

    local json_file="${JSON_DIR}/${exp_id}_${mech}_rep${rep}_r${rate}_c${conns}.json"

    log "[BENCH] >>> Exp: $exp_id | Mech: $mech | Rep: $rep | Mode: $mode | Conns: $conns (Active: $active_s) | W: $pipe_w | Rate: $rate | Dist: $dist"

    if [[ "$DRY_RUN" == "1" ]]; then
        return 0
    fi

    # Build argument array
    local cmd=(
        "$LOADGEN_BIN"
        --host "$HOST"
        --port "$PORT"
        --connections "$conns"
        --pipeline-window "$pipe_w"
        --active-share "$active_s"
        --dist "$dist"
        --duration "$dur"
        --warmup "$warmup"
        --cooldown "$cooldown"
        --output "$MASTER_CSV"
        --json-output "$json_file"
    )

    if [[ "$mode" == "closed-loop" ]]; then
        cmd+=(--mode closed-loop)
    else
        cmd+=(--mode open-loop --rate "$rate")
    fi

    # Execute load generator
    "${cmd[@]}" || {
        log "[WARNING] Workload exited with status $?"
    }

    sleep 1
}

# -----------------------------------------------------------------------------
# Self-Test Mode
# -----------------------------------------------------------------------------
run_self_test() {
    log "================================================================="
    log "  Running echo_loadgen Built-in Self-Test Suite"
    log "================================================================="
    if [[ ! -x "$LOADGEN_BIN" ]]; then
        log "[FATAL] Load generator binary not found at $LOADGEN_BIN. Run 'make loadgen' first."
        exit 1
    fi
    "$LOADGEN_BIN" --self-test
}

# -----------------------------------------------------------------------------
# Pilot Check Mode (~3 Minutes)
# Tests select, epoll, and io_uring at 5,000 and 25,000 msgs/s
# -----------------------------------------------------------------------------
run_pilot() {
    log "================================================================="
    log "  Starting Pilot Check (select, epoll, uring at 5k & 25k msgs/s)"
    log "  Target Server: ${HOST}:${PORT}"
    log "================================================================="

    local pilot_mechs=("select" "epoll" "uring")
    local pilot_rates=(5000 25000)

    for mech in "${pilot_mechs[@]}"; do
        start_server "$mech"
        for rate in "${pilot_rates[@]}"; do
            run_workload "$mech" "PILOT" 1 100 "$rate" "open-loop" 1 1.0 "constant" 10 2 2
        done
        stop_server "$mech"
    done

    log "[PILOT] Pilot run completed successfully. Results saved in $RESULTS_DIR"
}

# -----------------------------------------------------------------------------
# Full 5-Suite Benchmark with 4-Rep Cyclic Latin-Square Rotation (~126 Minutes)
# -----------------------------------------------------------------------------
run_full() {
    log "================================================================="
    log "  Starting Full 5-Suite, 4-Repetition Benchmark"
    log "  Cyclic Latin-Square Rotations: S->P->E->U, P->E->U->S, E->U->S->P, U->S->P->E"
    log "  Target Server: ${HOST}:${PORT}"
    log "================================================================="

    # 4 cyclic Latin-Square rotations
    local latin_rep1=("select" "poll" "epoll" "uring")
    local latin_rep2=("poll" "epoll" "uring" "select")
    local latin_rep3=("epoll" "uring" "select" "poll")
    local latin_rep4=("uring" "select" "poll" "epoll")

    for rep in 1 2 3 4; do
        local -n current_order="latin_rep${rep}"
        log "-----------------------------------------------------------------"
        log "  Beginning Repetition $rep / 4 (Order: ${current_order[*]})"
        log "-----------------------------------------------------------------"

        for mech in "${current_order[@]}"; do
            start_server "$mech"

            # Suite 1: Connection Scaling (Fixed rate: 1000 msgs/s, Duration: 30s, Warmup: 5s, Cooldown: 5s)
            log ">> [Suite 1: Connection Scaling] mech=$mech"
            for c in 100 500 900; do
                run_workload "$mech" "S1_CONN" "$rep" "$c" 1000 "open-loop" 1 1.0 "constant" 30 5 5
            done

            # Suite 2: Offered-Load Rate Scaling (Fixed conn: 500, Rates: 500 to 25,000 msgs/s)
            log ">> [Suite 2: Rate Scaling] mech=$mech"
            for r in 500 1000 2000 4000 8000 16000 25000; do
                # For select, skip rates that exceed its capacity at 500 conns if needed
                if [[ "$mech" == "select" && "$r" -gt 16000 ]]; then
                    continue
                fi
                run_workload "$mech" "S2_RATE" "$rep" 500 "$r" "open-loop" 1 1.0 "constant" 30 5 5
            done

            # Closed-loop ceiling for Suite 2
            run_workload "$mech" "S2_RATE_MAX" "$rep" 500 0 "closed-loop" 1 1.0 "constant" 30 5 5

            # Suite 3: Active-Share / Sparse Activity (conn: 1000, rate: 2000, active-share: 0.1, 0.5, 1.0)
            log ">> [Suite 3: Active-Share] mech=$mech"
            for share in 0.1 0.5 1.0; do
                run_workload "$mech" "S3_ACTIVE" "$rep" 1000 2000 "open-loop" 1 "$share" "constant" 30 5 5
            done

            # Suite 4: Pipelining Sweep (conn: 100, rate: 5000, W = 1, 2, 4, 8)
            log ">> [Suite 4: Pipelining W] mech=$mech"
            for w in 1 2 4 8; do
                run_workload "$mech" "S4_PIPE" "$rep" 100 5000 "open-loop" "$w" 1.0 "constant" 30 5 5
            done

            # Suite 5: Traffic Arrival Patterns (conn: 500, rate: 2000, Constant vs Poisson vs Burst)
            log ">> [Suite 5: Arrival Patterns] mech=$mech"
            for d in "constant" "poisson" "burst"; do
                run_workload "$mech" "S5_DIST" "$rep" 500 2000 "open-loop" 1 1.0 "$d" 30 5 5
            done

            stop_server "$mech"
        done
    done

    log "================================================================="
    log "  All 4 Repetitions Completed Successfully!"
    log "  Master CSV results saved to: $MASTER_CSV"
    log "  Timeseries JSON reports saved in: $JSON_DIR"
    log "================================================================="
}

# -----------------------------------------------------------------------------
# Entry Point
# -----------------------------------------------------------------------------
case "${1:-help}" in
    --self-test)
        run_self_test
        ;;
    --pilot)
        run_pilot
        ;;
    --full)
        run_full
        ;;
    --help|-h|help)
        cat <<EOF
Usage: $0 [MODE]

Modes:
  --self-test     Run 10-second built-in verification test suite
  --pilot         Run 3-minute pilot check (select, epoll, uring at 5k & 25k msgs/s)
  --full          Run complete 5-suite, 4-rep Cyclic Latin-Square benchmark suite (~126 min)

Environment Variables:
  HOST            Server IP (default: 127.0.0.1)
  PORT            Server port (default: 8080)
  SSH_SERVER      Remote server login (e.g. user@192.168.50.1) to auto-manage server
  SERVER_REPO     Path to repo on remote server (default: same as local repo)
  DRY_RUN         Set to 1 to print commands without executing load
EOF
        ;;
    *)
        echo "[ERROR] Unknown option: $1"
        exit 1
        ;;
esac
