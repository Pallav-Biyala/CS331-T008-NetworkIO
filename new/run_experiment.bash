#!/usr/bin/env bash
# ==============================================================================
# Linux Network I/O Benchmark Orchestrator (run_experiment.sh)
# Executes the 5-suite benchmark matrix across select, poll, epoll, and io_uring
# using a 4-repetition Cyclic Latin-Square rotation to eliminate ordering bias.
# ==============================================================================
set -euo pipefail

HOST="${HOST:-192.168.50.1}"
PORT="${PORT:-8080}"
LOADGEN_BIN="${LOADGEN_BIN:-./echo_loadgen}"
OUT_DIR="${OUT_DIR:-results_$(date +%Y%m%d_%H%M%S)}"

WARMUP_S="${WARMUP_S:-5}"
DURATION_S="${DURATION_S:-20}"
COOLDOWN_S="${COOLDOWN_S:-2}"
MSG_SIZE="${MSG_SIZE:-512}"
REPS="${REPS:-4}"
PAUSE_BETWEEN_RUNS_S="${PAUSE_BETWEEN_RUNS_S:-1}"

# Optional remote server control hook:
# If SSH_SERVER is set (e.g., SSH_SERVER="user@192.168.50.1"), the script will
# automatically start/stop the corresponding backend binary on the server host.
# Otherwise, it prompts you or assumes a server wrapper is listening on $PORT.
SSH_SERVER="${SSH_SERVER:-}"
SERVER_DIR="${SERVER_DIR:-~/linux-network-io-bench}"

usage() {
    cat <<EOF
Usage: $0 [--pilot | --full | --self-test] [options]

Modes:
  --self-test    Run echo_loadgen --self-test locally (15s verification)
  --pilot        Run fast pilot check (select, epoll, io_uring at 5k & 25k msgs/s)
  --full         Run all 5 benchmark suites with 4-rep Cyclic Latin-Square rotation (default)

Environment Overrides:
  HOST=$HOST  PORT=$PORT  LOADGEN_BIN=$LOADGEN_BIN  OUT_DIR=$OUT_DIR
  WARMUP_S=$WARMUP_S  DURATION_S=$DURATION_S  COOLDOWN_S=$COOLDOWN_S  REPS=$REPS
  SSH_SERVER="$SSH_SERVER" (optional user@host to auto-restart server backends)
EOF
}

MODE="full"
if [[ $# -gt 0 ]]; then
    case "$1" in
        --self-test) MODE="selftest" ;;
        --pilot)     MODE="pilot" ;;
        --full)      MODE="full" ;;
        -h|--help)   usage; exit 0 ;;
        *)           echo "Unknown option: $1"; usage; exit 1 ;;
    esac
fi

if [[ ! -x "$LOADGEN_BIN" ]]; then
    echo "[BUILD] Compiling $LOADGEN_BIN..."
    g++ -std=c++17 -O3 -Wall -Wextra -pthread code/tools/echo_loadgen.cpp -o "$LOADGEN_BIN"
fi

if [[ "$MODE" == "selftest" ]]; then
    exec "$LOADGEN_BIN" --self-test
fi

mkdir -p "$OUT_DIR/json"
CSV_MAIN="$OUT_DIR/benchmark_all_runs.csv"

start_remote_server() {
    local backend="$1"
    if [[ -n "$SSH_SERVER" ]]; then
        echo "  [SERVER] Starting backend '$backend' on $SSH_SERVER..."
        ssh "$SSH_SERVER" "pkill -f 'echo_' || true; cd $SERVER_DIR && nohup ./echo_${backend} --port $PORT --msg-size $MSG_SIZE --max-conns 10240 >/tmp/echo_${backend}.log 2>&1 &"
        sleep 1
    fi
}

stop_remote_server() {
    local backend="$1"
    if [[ -n "$SSH_SERVER" ]]; then
        ssh "$SSH_SERVER" "pkill -SIGINT -f 'echo_${backend}' || true"
    fi
}

# Cyclic Latin-Square backend order for repetition r (1..4)
get_rotated_backends() {
    local rep="$1"
    local include_select="${2:-1}"
    if [[ "$include_select" == "1" ]]; then
        case $(( (rep - 1) % 4 )) in
            0) echo "select poll epoll io_uring" ;;
            1) echo "poll epoll io_uring select" ;;
            2) echo "epoll io_uring select poll" ;;
            3) echo "io_uring select poll epoll" ;;
        esac
    else
        case $(( (rep - 1) % 3 )) in
            0) echo "poll epoll io_uring" ;;
            1) echo "epoll io_uring poll" ;;
            2) echo "io_uring poll epoll" ;;
        esac
    fi
}

run_single_point() {
    local suite="$1"
    local backend="$2"
    local rep="$3"
    local seed=$(( 1000 + rep ))
    shift 3
    local extra_args=("$@")

    local tag="${suite}_${backend}_rep${rep}"
    local json_file="$OUT_DIR/json/${tag}_$(date +%H%M%S).json"

    start_remote_server "$backend"
    echo "[RUN] Suite=$suite | Backend=$backend | Rep=$rep | Args: ${extra_args[*]}"
    "$LOADGEN_BIN" \
        --host "$HOST" \
        --port "$PORT" \
        --msg-size "$MSG_SIZE" \
        --warmup "$WARMUP_S" \
        --duration "$DURATION_S" \
        --cooldown "$COOLDOWN_S" \
        --seed "$seed" \
        --tag "$tag" \
        --output "$CSV_MAIN" \
        --json-output "$json_file" \
        "${extra_args[@]}"
    stop_remote_server "$backend"
    sleep "$PAUSE_BETWEEN_RUNS_S"
}

if [[ "$MODE" == "pilot" ]]; then
    echo "======================================================================"
    echo " Running Pilot Verification (select, epoll, io_uring @ 5k & 25k msgs/s)"
    echo " Output directory: $OUT_DIR"
    echo "======================================================================"
    for rate in 5000 25000; do
        for backend in select epoll io_uring; do
            run_single_point "pilot_r${rate}" "$backend" 1 \
                --connections 500 --active-connections 500 \
                --mode open-loop --arrival constant --rate "$rate" --pipeline-depth 1
        done
    done
    echo "[PILOT COMPLETE] Inspect $CSV_MAIN for client_cpu_pct, sched_lag_p99_us, and delivery_ratio_pct."
    exit 0
fi

echo "======================================================================"
echo " Starting Full 5-Suite Benchmark ($REPS Repetitions, Rotated Order)"
echo " Output CSV: $CSV_MAIN"
echo "======================================================================"

for (( rep=1; rep<=REPS; rep++ )); do
    BACKENDS_ALL=$(get_rotated_backends "$rep" 1)
    BACKENDS_HIGHFD=$(get_rotated_backends "$rep" 0)

    echo ""
    echo ">>> REPETITION $rep / $REPS (Order: $BACKENDS_ALL) <<<"

    # ------------------------------------------------------------------
    # Suite 1: Idle-Socket Overhead (Fixed Active K=50 vs Fully Active K=N)
    # ------------------------------------------------------------------
    for conns in 100 500 900; do
        for backend in $BACKENDS_ALL; do
            # 1A: Fixed 50 active connections (100 msgs/s per active socket across all N)
            run_single_point "S1_idle_fixed50_c${conns}" "$backend" "$rep" \
                --connections "$conns" --active-connections 50 \
                --mode open-loop --arrival constant --rate 5000 --pipeline-depth 1

            # 1B: 100% active connections (K = N)
            run_single_point "S1_full_active_c${conns}" "$backend" "$rep" \
                --connections "$conns" --active-connections "$conns" \
                --mode open-loop --arrival constant --rate 5000 --pipeline-depth 1
        done
    done

    # ------------------------------------------------------------------
    # Suite 2: Offered Load & Saturation Sweep + Closed-Loop Ceiling
    # ------------------------------------------------------------------
    for rate in 2000 10000 25000 50000; do
        for backend in $BACKENDS_ALL; do
            run_single_point "S2_rate_${rate}" "$backend" "$rep" \
                --connections 500 --active-connections 500 \
                --mode open-loop --arrival constant --rate "$rate" --pipeline-depth 1
        done
    done
    for backend in $BACKENDS_ALL; do
        run_single_point "S2_closed_loop_w1" "$backend" "$rep" \
            --connections 500 --active-connections 500 \
            --mode closed-loop --pipeline-depth 1
    done

    # ------------------------------------------------------------------
    # Suite 3: Arrival Process Realism (Constant vs Poisson vs ON/OFF Burst)
    # ------------------------------------------------------------------
    for backend in $BACKENDS_ALL; do
        run_single_point "S3_constant_10k" "$backend" "$rep" \
            --connections 500 --active-connections 500 \
            --mode open-loop --arrival constant --rate 10000 --pipeline-depth 1

        run_single_point "S3_poisson_10k" "$backend" "$rep" \
            --connections 500 --active-connections 500 \
            --mode open-loop --arrival poisson --rate 10000 --pipeline-depth 1

        # 40,000 msgs/s for 50ms ON, 0 msgs/s for 150ms OFF => 10,000 msgs/s average over 200ms period
        run_single_point "S3_burst_10k" "$backend" "$rep" \
            --connections 500 --active-connections 500 \
            --mode open-loop --arrival burst --rate 10000 \
            --burst-on-ms 50 --burst-period-ms 200 --burst-on-rate 40000 --burst-off-rate 0 \
            --pipeline-depth 1
    done

    # ------------------------------------------------------------------
    # Suite 4: Pipelining / Batch Efficiency (W=4 vs W=1 from Suite 2)
    # ------------------------------------------------------------------
    for backend in $BACKENDS_ALL; do
        run_single_point "S4_pipe_w4_25k" "$backend" "$rep" \
            --connections 500 --active-connections 500 \
            --mode open-loop --arrival constant --rate 25000 --pipeline-depth 4

        run_single_point "S4_pipe_w4_closed" "$backend" "$rep" \
            --connections 500 --active-connections 500 \
            --mode closed-loop --pipeline-depth 4
    done

    # ------------------------------------------------------------------
    # Suite 5: High-FD Scaling Beyond FD_SETSIZE (poll, epoll, io_uring)
    # ------------------------------------------------------------------
    for backend in $BACKENDS_HIGHFD; do
        run_single_point "S5_highfd_c3000_k50" "$backend" "$rep" \
            --connections 3000 --active-connections 50 \
            --mode open-loop --arrival constant --rate 5000 --pipeline-depth 1

        run_single_point "S5_highfd_c3000_k3000" "$backend" "$rep" \
            --connections 3000 --active-connections 3000 \
            --mode open-loop --arrival constant --rate 5000 --pipeline-depth 1
    done
done

echo "======================================================================"
echo " All 5 Benchmark Suites Completed Successfully!"
echo " Summary CSV: $CSV_MAIN"
echo " JSON Reports: $OUT_DIR/json/"
echo "======================================================================"