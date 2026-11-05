#!/usr/bin/env bash
# Client-only benchmark runner. Start/stop the selected server backend manually.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOADGEN_BIN="${LOADGEN_BIN:-$SCRIPT_DIR/echo_loadgen}"
HOST="${HOST:-192.168.50.1}"
PORT="${PORT:-8080}"
MSG_SIZE="${MSG_SIZE:-512}"
WARMUP_S="${WARMUP_S:-10}"
DURATION_S="${DURATION_S:-30}"
COOLDOWN_S="${COOLDOWN_S:-10}"
CLIENT_CPU="${CLIENT_CPU:-}"
REP="${REP:-1}"
OUT_DIR="${OUT_DIR:-}"
DRY_RUN="${DRY_RUN:-0}"

usage() {
    cat <<EOF
Usage: $0 --mechanism select|poll|epoll|uring [--pilot|--full] [options]

Client-only runner: it never starts, stops, or signals the server. Start the
matching echo server manually on the server laptop before running this script.

Options:
  --mechanism NAME  Required backend currently running on the server
  --self-test       Run the load generator's built-in test (no server needed)
  --pilot           Run a short connectivity/performance smoke check (default)
  --full            Run one complete five-suite repetition for this backend
  --rep N           Repetition number recorded in filenames and manifest (default: $REP)
  --help            Show this help

Environment:
  HOST, PORT, MSG_SIZE, WARMUP_S, DURATION_S, COOLDOWN_S, CLIENT_CPU, OUT_DIR, DRY_RUN
EOF
}

MODE="pilot"
MECH=""
SELF_TEST=0
while (($#)); do
    case "$1" in
        --mechanism|-m) (($# >= 2)) || { echo "Missing value for $1" >&2; exit 2; }; MECH="$2"; shift 2 ;;
        --pilot) MODE="pilot"; shift ;;
        --full) MODE="full"; shift ;;
        --self-test) SELF_TEST=1; shift ;;
        --rep) (($# >= 2)) || { echo "Missing value for --rep" >&2; exit 2; }; REP="$2"; shift 2 ;;
        --help|-h) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if [[ "$SELF_TEST" == 1 ]]; then
    "$LOADGEN_BIN" --self-test
    exit $?
fi

case "$MECH" in select|poll|epoll|uring) ;; *) echo "Pass --mechanism select|poll|epoll|uring" >&2; exit 2 ;; esac
[[ "$REP" =~ ^[1-9][0-9]*$ ]] || { echo "--rep must be a positive integer" >&2; exit 2; }
[[ -n "$CLIENT_CPU" ]] || { echo "Set CLIENT_CPU to the dedicated client core from the setup guide" >&2; exit 2; }
command -v taskset >/dev/null || { echo "taskset is required to pin the load generator" >&2; exit 1; }
[[ -x "$LOADGEN_BIN" ]] || { echo "Load generator not found/executable: $LOADGEN_BIN (run: make loadgen)" >&2; exit 1; }

STAMP="$(date '+%Y%m%d_%H%M%S')"
if [[ -z "$OUT_DIR" ]]; then
    OUT_DIR="$SCRIPT_DIR/results/${MECH}_rep${REP}_${STAMP}"
elif [[ "$OUT_DIR" != /* ]]; then
    OUT_DIR="$SCRIPT_DIR/$OUT_DIR"
fi
mkdir -p "$OUT_DIR/json"
exec > >(tee -a "$OUT_DIR/run.log") 2>&1
CSV="$OUT_DIR/results.csv"
MANIFEST="$OUT_DIR/manifest.txt"

cat > "$MANIFEST" <<EOF
mechanism=$MECH
repetition=$REP
mode=$MODE
host=$HOST
port=$PORT
message_size_bytes=$MSG_SIZE
warmup_seconds=$WARMUP_S
measurement_seconds=$DURATION_S
cooldown_seconds=$COOLDOWN_S
started_at=$(date --iso-8601=seconds)
client_hostname=$(hostname)
client_kernel=$(uname -sr)
client_nic=${NIC:-unknown}
client_cpu=$CLIENT_CPU
git_revision=$(git -C "$SCRIPT_DIR" rev-parse HEAD 2>/dev/null || echo unknown)
client_preflight_tcp_probe=yes
runner=$SCRIPT_DIR/run_experiment.sh
EOF

log() { echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*"; }

if [[ "$DRY_RUN" != 1 ]]; then
    # Fail before recording empty/invalid data if the manually managed server is absent.
    if ! (exec 3<>"/dev/tcp/$HOST/$PORT") 2>/dev/null; then
        echo "Cannot connect to $HOST:$PORT. Start echo-$MECH on the server laptop first." >&2
        exit 1
    fi
    exec 3>&-
    exec 3<&-
fi

run_workload() {
    local suite="$1" case_name="$2" conns="$3" rate="$4" mode="$5"
    local window="$6" share="$7" dist="$8"
    local stem="rep${REP}_${suite}_${case_name}"
    local json="$OUT_DIR/json/${stem}.json"
    local -a cmd=(taskset -c "$CLIENT_CPU" "$LOADGEN_BIN" --host "$HOST" --port "$PORT"
        --connections "$conns" --pipeline-window "$window" --active-share "$share"
        --dist "$dist" --duration "$DURATION_S" --warmup "$WARMUP_S"
        --cooldown "$COOLDOWN_S" --msg-size "$MSG_SIZE"
        --output "$CSV" --json-output "$json" --mode "$mode")
    if [[ "$mode" == open-loop ]]; then cmd+=(--rate "$rate"); fi
    log "[$MECH rep $REP] $suite/$case_name: mode=$mode conns=$conns rate=$rate window=$window active=$share dist=$dist"
    printf '%q ' "${cmd[@]}" >> "$OUT_DIR/commands.log"
    printf '\n' >> "$OUT_DIR/commands.log"
    if [[ "$DRY_RUN" == 1 ]]; then return 0; fi
    "${cmd[@]}"
    sleep 1
}

log "Backend=$MECH repetition=$REP mode=$MODE target=$HOST:$PORT output=$OUT_DIR"
if [[ "$MODE" == pilot ]]; then
    run_workload PILOT rate_5000 100 5000 open-loop 1 1.0 constant
    run_workload PILOT rate_25000 100 25000 open-loop 1 1.0 constant
else
    # Suite 1: connection scaling, keeping FD numbers below select's ceiling.
    for c in 100 500 900; do run_workload S1_CONN "c${c}" "$c" 1000 open-loop 1 1.0 constant; done

    # Suite 2: offered-load scaling, plus a closed-loop ceiling.
    for r in 500 1000 2000 4000 8000 16000 25000; do
        if [[ "$MECH" == select && "$r" -gt 16000 ]]; then continue; fi
        run_workload S2_RATE "r${r}" 500 "$r" open-loop 1 1.0 constant
    done
    run_workload S2_RATE_MAX closed_loop 500 0 closed-loop 1 1.0 constant

    # Suite 3: active-share sweep. 900 connections stays within select's FD ceiling.
    for share in 0.1 0.5 1.0; do run_workload S3_ACTIVE "share${share}" 900 2000 open-loop 1 "$share" constant; done

    # Suite 4: pipelining sweep.
    for w in 1 2 4 8; do run_workload S4_PIPE "w${w}" 100 5000 open-loop "$w" 1.0 constant; done

    # Suite 5: arrival distribution sweep.
    for d in constant poisson burst; do run_workload S5_DIST "$d" 500 2000 open-loop 1 1.0 "$d"; done
fi

printf 'finished_at=%s\n' "$(date --iso-8601=seconds)" >> "$MANIFEST"
log "Completed. CSV: $CSV | JSON: $OUT_DIR/json | Manifest: $MANIFEST"
