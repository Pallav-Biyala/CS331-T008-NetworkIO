#!/usr/bin/env bash
# Client-only benchmark runner. Start/stop the selected server backend manually.
#
# One invocation = one backend x one repetition. Output:
#   code/results/Echo-Server/${MECH}_rep${REP}_${STAMP}/results.csv          (one row per case)
#   code/results/Echo-Server/${MECH}_rep${REP}_${STAMP}/json/rep${REP}_${SUITE}_${CASE}.json
# CSV `tag` column = ${MECH}_rep${REP}_${SUITE}_${CASE}  (parsed by plot_results.py)
set -uo pipefail   # no -e: one failed case must not abort a long run

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
LOADGEN_BIN="${LOADGEN_BIN:-$REPO_ROOT/build/echo_loadgen}"
HOST="${HOST:-192.168.50.1}"
PORT="${PORT:-8080}"
MSG_SIZE="${MSG_SIZE:-512}"
WARMUP_S="${WARMUP_S:-5}"
DURATION_S="${DURATION_S:-20}"
COOLDOWN_S="${COOLDOWN_S:-5}"
CONNECT_TIMEOUT_S="${CONNECT_TIMEOUT_S:-20}"
CLIENT_CPU="${CLIENT_CPU:-}"
REP="${REP:-1}"
OUT_DIR="${OUT_DIR:-}"
DRY_RUN="${DRY_RUN:-0}"

usage() {
    cat <<EOF
Usage: $0 --mechanism select|poll|epoll|uring [--pilot|--full] [--rep N]

Client-only runner: it never starts, stops, or signals the server. Start the
matching echo server manually on the server laptop before running this script.

Options:
  --mechanism NAME  Backend currently running on the server (required)
  --self-test       Run the load generator's built-in test (no server needed)
  --pilot           Short smoke check incl. a 10K-connection probe (default)
  --full            One complete five-suite repetition for this backend
  --rep N           Repetition number (also seeds Poisson/burst RNG) (default: $REP)

Environment:
  HOST PORT MSG_SIZE WARMUP_S DURATION_S COOLDOWN_S CONNECT_TIMEOUT_S CLIENT_CPU OUT_DIR
  DRY_RUN=1  print the case list + time estimate without contacting the server
EOF
}

MODE="pilot"; MECH=""; SELF_TEST=0
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

if [[ "$SELF_TEST" == 1 ]]; then exec "$LOADGEN_BIN" --self-test; fi

case "$MECH" in select|poll|epoll|uring) ;; *) echo "Pass --mechanism select|poll|epoll|uring" >&2; exit 2 ;; esac
[[ "$REP" =~ ^[1-9][0-9]*$ ]] || { echo "--rep must be a positive integer" >&2; exit 2; }

if [[ "$DRY_RUN" != 1 ]]; then
    [[ -n "$CLIENT_CPU" ]] || { echo "Set CLIENT_CPU to the dedicated client core from the setup guide" >&2; exit 2; }
    command -v taskset >/dev/null || { echo "taskset is required to pin the load generator" >&2; exit 1; }
    [[ -x "$LOADGEN_BIN" ]] || { echo "Load generator not found: $LOADGEN_BIN (run: make -f code/Makefile loadgen)" >&2; exit 1; }
    # 10K connections need ~21K fds on the client (loadgen raises soft limit up to the hard limit).
    if [[ "$MECH" != select ]] && (( $(ulimit -Hn) < 21000 )); then
        echo "Hard fd limit $(ulimit -Hn) < 21000; run: sudo prlimit --pid \$\$ --nofile=65536:65536" >&2; exit 1
    fi
    if ! (exec 3<>"/dev/tcp/$HOST/$PORT") 2>/dev/null; then
        echo "Cannot connect to $HOST:$PORT. Start echo-$MECH on the server laptop first." >&2; exit 1
    fi
fi

STAMP="$(date '+%Y%m%d_%H%M%S')"
if [[ -z "$OUT_DIR" ]]; then
    OUT_DIR="$REPO_ROOT/code/results/Echo-Server/${MECH}_rep${REP}_${STAMP}"
elif [[ "$OUT_DIR" != /* ]]; then
    OUT_DIR="$REPO_ROOT/$OUT_DIR"
fi
if [[ "$DRY_RUN" != 1 ]]; then
    mkdir -p "$OUT_DIR/json"
    exec > >(tee -a "$OUT_DIR/run.log") 2>&1
fi
CSV="$OUT_DIR/results.csv"
MANIFEST="$OUT_DIR/manifest.txt"
SEED=$(( 1000 + REP ))   # same arrival sequence for every backend within a repetition

log() { echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*"; }

N_CASES=0; N_FAILED=0; EST_S=0
# run_case SUITE CASE CONNS RATE MODE WINDOW SHARE DIST
run_case() {
    local suite="$1" case_name="$2" conns="$3" rate="$4" mode="$5" window="$6" share="$7" dist="$8"
    local tag="${MECH}_rep${REP}_${suite}_${case_name}"
    local json="$OUT_DIR/json/rep${REP}_${suite}_${case_name}.json"
    local -a cmd=(taskset -c "$CLIENT_CPU" "$LOADGEN_BIN" --host "$HOST" --port "$PORT"
        --connections "$conns" --pipeline-window "$window" --active-share "$share"
        --dist "$dist" --mode "$mode" --duration "$DURATION_S" --warmup "$WARMUP_S"
        --cooldown "$COOLDOWN_S" --connect-timeout "$CONNECT_TIMEOUT_S" --msg-size "$MSG_SIZE"
        --seed "$SEED" --tag "$tag" --output "$CSV" --json-output "$json")
    [[ "$mode" == open-loop ]] && cmd+=(--rate "$rate")
    N_CASES=$((N_CASES + 1))
    EST_S=$((EST_S + WARMUP_S + DURATION_S + 3 + conns / 2500))
    if [[ "$DRY_RUN" == 1 ]]; then
        printf '%3d  %-18s %-22s conns=%-6s mode=%-11s rate=%-6s W=%s share=%-4s dist=%s\n' \
            "$N_CASES" "$suite" "$case_name" "$conns" "$mode" "$rate" "$window" "$share" "$dist"
        return 0
    fi
    log "[$MECH rep $REP] #$N_CASES $suite/$case_name conns=$conns mode=$mode rate=$rate W=$window share=$share dist=$dist"
    printf '%q ' "${cmd[@]}" >> "$OUT_DIR/commands.log"; printf '\n' >> "$OUT_DIR/commands.log"
    "${cmd[@]}"
    local rc=$?
    if (( rc != 0 )); then
        N_FAILED=$((N_FAILED + 1))
        log "WARN: $suite/$case_name exited $rc (errors/failed conns; row still recorded if run completed)"
        echo "$tag rc=$rc" >> "$OUT_DIR/failed_cases.txt"
    fi
    sleep 1
}

# select's FD_SETSIZE=1024 caps it at 900 connections; all >1000-conn cases skip select.
HI() { [[ "$MECH" != select ]]; }

if [[ "$DRY_RUN" != 1 ]]; then
    cat > "$MANIFEST" <<EOF
mechanism=$MECH
repetition=$REP
mode=$MODE
seed=$SEED
host=$HOST
port=$PORT
message_size_bytes=$MSG_SIZE
warmup_seconds=$WARMUP_S
measurement_seconds=$DURATION_S
cooldown_seconds=$COOLDOWN_S
started_at=$(date --iso-8601=seconds)
client_hostname=$(hostname)
client_kernel=$(uname -sr)
client_cpu=$CLIENT_CPU
git_revision=$(git -C "$SCRIPT_DIR" rev-parse HEAD 2>/dev/null || echo unknown)
EOF
    log "Backend=$MECH rep=$REP mode=$MODE target=$HOST:$PORT output=$OUT_DIR"
fi

if [[ "$MODE" == pilot ]]; then
    DURATION_S=5; WARMUP_S=2
    run_case PILOT c100_r5000    100  5000  open-loop 1 1.0 constant
    run_case PILOT c500_r25000   500  25000 open-loop 1 1.0 constant
    run_case PILOT c500_closed   500  0     closed-loop 1 1.0 constant
    HI && run_case PILOT c10000_r1000 10000 1000 open-loop 1 1.0 constant
else
    CONNS_LO="50 100 250 500 900"          # all backends (select-safe)
    CONNS_HI="2500 5000 10000"             # poll/epoll/uring only

    # S1: connection scaling at fixed offered load (graphs 01, 02)
    for c in $CONNS_LO; do for r in 1000 10000; do
        run_case S1_CONN "c${c}_r${r}" "$c" "$r" open-loop 1 1.0 constant; done; done
    if HI; then for c in $CONNS_HI; do for r in 1000 10000; do
        run_case S1_CONN "c${c}_r${r}" "$c" "$r" open-loop 1 1.0 constant; done; done; fi

    # S1_CONN_MAX: closed-loop throughput ceiling vs connections (graph 03; also the
    # saturation reference line in graphs 04/05 and W=1 closed-loop point in graph 07)
    for c in $CONNS_LO; do run_case S1_CONN_MAX "c${c}_closed" "$c" 0 closed-loop 1 1.0 constant; done
    if HI; then for c in $CONNS_HI; do run_case S1_CONN_MAX "c${c}_closed" "$c" 0 closed-loop 1 1.0 constant; done; fi

    # S2: offered-load sweep (graphs 04, 05)
    for r in 1000 4000 8000 12000 16000 25000 35000 50000; do
        run_case S2_RATE "c500_r${r}" 500 "$r" open-loop 1 1.0 constant; done
    if HI; then for r in 1000 4000 8000 16000 25000 50000; do
        run_case S2_RATE_HIGHCONN "c5000_r${r}" 5000 "$r" open-loop 1 1.0 constant; done; fi

    # S3: active-share / idle-socket tax at 5K msgs/s (graph 06)
    for s in 0.05 0.1 0.25 0.5 1.0; do run_case S3_ACTIVE "c900_share${s}" 900 5000 open-loop 1 "$s" constant; done
    if HI; then for s in 0.05 0.1 0.25 0.5 1.0; do
        run_case S3_ACTIVE "c10000_share${s}" 10000 5000 open-loop 1 "$s" constant; done; fi

    # S4: pipeline window at 500 conns (graph 07)
    for w in 1 2 4 8; do run_case S4_PIPE     "c500_w${w}_r25000" 500 25000 open-loop   "$w" 1.0 constant; done
    for w in 1 2 4 8; do run_case S4_PIPE_MAX "c500_w${w}_closed" 500 0     closed-loop "$w" 1.0 constant; done

    # S5: arrival distribution / burst intensity (graph 08)
    for d in constant poisson burst2x burst burst10x; do
        run_case S5_DIST "c500_r15000_${d}" 500 15000 open-loop 1 1.0 "$d"; done
    if HI; then for d in constant poisson burst2x burst burst10x; do
        run_case S5_DIST "c5000_r10000_${d}" 5000 10000 open-loop 1 1.0 "$d"; done; fi
fi

if [[ "$DRY_RUN" == 1 ]]; then
    echo "--- $MECH: $N_CASES cases, estimated ~$((EST_S / 60)) min (WARMUP_S=$WARMUP_S DURATION_S=$DURATION_S)"
    exit 0
fi
printf 'finished_at=%s\ncases=%s\nfailed_cases=%s\n' "$(date --iso-8601=seconds)" "$N_CASES" "$N_FAILED" >> "$MANIFEST"
log "Completed $N_CASES cases ($N_FAILED non-zero exits). CSV: $CSV"
