#!/usr/bin/env bash
# scripts/run_experiments.sh
# Master automated experiment runner for CS331-T008 Network I/O Benchmarking.
# Runs on the Client machine (192.168.50.2).
#
# Assumptions:
# 1. Server is reachable via passwordless SSH at $SERVER_USER@$SERVER_HOST
# 2. Server repo path matches $SERVER_REPO (default ~/linux-network-io-bench)
# 3. Dedicated cgroup or /proc/<pid>/stat is available on the server
# 4. Local client has echo_loadgen built in repo root

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# Default CLI arguments
DRY_RUN=0
ONLY_EXP=""
ONLY_MECH=""
MAX_HOURS=3.0
ROUNDS=5
CONF_FILE="$SCRIPT_DIR/experiments.conf"
SERVER_HOST="${SERVER_HOST:-192.168.50.1}"
PORT=8080
MSG_SIZE=512
WARMUP=5
COOLDOWN=5
USE_SSH=0
SERVER_USER="${SERVER_USER:-koko}"
SERVER_REPO="${SERVER_REPO:-~/linux-network-io-bench}"

print_usage() {
    cat <<EOF
Usage: $0 [options]
  --mech <type>           Run experiments for a specific mechanism only (select|poll|epoll|uring)
  --only <exp>            Filter to run only a specific experiment (e.g. E1, E1s, E2, E3)
  --rounds <int>          Number of repeats per configuration (default: $ROUNDS)
  --server-host <ip>      Remote server IP (default: $SERVER_HOST)
  --port <port>           Server port (default: $PORT)
  --max-hours <float>     Hard timeout guard in hours (default: $MAX_HOURS)
  --dry-run               Show execution plan without sending traffic
  --conf <file>           Path to experiments configuration file (default: $CONF_FILE)
  --use-ssh               Enable remote server start/stop via SSH (default: disabled)
  --help, -h              Show this help message
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --mech) ONLY_MECH="$2"; shift 2 ;;
        --only) ONLY_EXP="$2"; shift 2 ;;
        --rounds) ROUNDS="$2"; shift 2 ;;
        --server-host) SERVER_HOST="$2"; shift 2 ;;
        --port) PORT="$2"; shift 2 ;;
        --max-hours) MAX_HOURS="$2"; shift 2 ;;
        --dry-run) DRY_RUN=1; shift ;;
        --conf) CONF_FILE="$2"; shift 2 ;;
        --use-ssh) USE_SSH=1; shift ;;
        --server-user) SERVER_USER="$2"; shift 2 ;;
        -h|--help) print_usage; exit 0 ;;
        *) echo "[ERROR] Unknown option: $1" >&2; print_usage; exit 1 ;;
    esac
done

RESULTS_DIR="$REPO_DIR/results"
RAW_DIR="$RESULTS_DIR/raw"
LOG_FILE="$RESULTS_DIR/run.log"
MASTER_CSV="$RESULTS_DIR/master.csv"
ENV_FILE="$RESULTS_DIR/env.txt"

mkdir -p "$RESULTS_DIR" "$RAW_DIR"

log() {
    local msg="[$(date '+%Y-%m-%d %H:%M:%S')] $*"
    echo "$msg"
    echo "$msg" >> "$LOG_FILE"
}

# Cleanup on exit or abort
cleanup_on_exit() {
    local exit_code=$?
    if [[ $exit_code -ne 0 ]]; then
        log "[WARNING] Script interrupted or finished with exit code $exit_code."
    fi
    if [[ $USE_SSH -eq 1 ]]; then
        log "[INFO] Cleaning up remote server processes via SSH..."
        ssh -o BatchMode=yes -o ConnectTimeout=5 "${SERVER_USER}@${SERVER_HOST}" \
            "${SERVER_REPO}/scripts/server_ctl.sh cleanup" >/dev/null 2>&1 || true
    fi
    pkill -f "echo_loadgen" >/dev/null 2>&1 || true
    exit $exit_code
}
trap cleanup_on_exit INT TERM EXIT

# ==============================================================================
# Pre-Flight Sanity Checks
# ==============================================================================
log "================================================================="
log "   CS331-T008 Automated Benchmark Runner Startup"
log "================================================================="

if [[ ! -f "$CONF_FILE" ]]; then
    log "[FATAL] Config file not found: $CONF_FILE"
    exit 1
fi

LOADGEN_BIN="$REPO_DIR/echo_loadgen"
if [[ ! -x "$LOADGEN_BIN" ]]; then
    log "[FATAL] Loadgen binary not found at $LOADGEN_BIN. Did you run 'make loadgen'?"
    exit 1
fi

# Check Client File Descriptors limit
CURRENT_NOFILE=$(ulimit -n)
if [[ "$CURRENT_NOFILE" -lt 16384 ]]; then
    log "[WARNING] ulimit -n is $CURRENT_NOFILE (< 16384). Attempting to increase..."
    ulimit -n 65536 2>/dev/null || true
    CURRENT_NOFILE=$(ulimit -n)
    if [[ "$CURRENT_NOFILE" -lt 10240 ]]; then
        log "[FATAL] Insufficient open file limit (ulimit -n = $CURRENT_NOFILE). Minimum 10240 required for high-connection sweeps."
        exit 1
    fi
fi
log "[OK] Client ulimit -n = $CURRENT_NOFILE"

# Check CPU scaling governor on client
if [[ -d /sys/devices/system/cpu/cpu0/cpufreq ]]; then
    GOV=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo "unknown")
    if [[ "$GOV" != "performance" ]]; then
        log "[WARNING] Client CPU governor is '$GOV' (recommended: 'performance')."
    else
        log "[OK] Client CPU governor is performance"
    fi
fi

if [[ $USE_SSH -eq 1 ]]; then
    # Check SSH connectivity to server
    log "[INFO] Verifying SSH connectivity to ${SERVER_USER}@${SERVER_HOST}..."
    if ! ssh -o BatchMode=yes -o ConnectTimeout=5 "${SERVER_USER}@${SERVER_HOST}" "echo SSH_OK" &>/dev/null; then
        log "[FATAL] Cannot connect via passwordless SSH to ${SERVER_USER}@${SERVER_HOST}."
        log "        Please run ./scripts/setup_ssh.sh first to configure keys."
        exit 1
    fi
    log "[OK] Passwordless SSH verified."

    # Check server binaries and server_ctl.sh
    log "[INFO] Verifying server binaries on remote host..."
    CHECK_REMOTE_BINS="
        set -e
        for b in echo-select echo-poll echo-epoll echo-uring; do
            if [ ! -x ${SERVER_REPO}/build/\$b ]; then
                echo \"MISSING_BIN:\$b\"; exit 1
            fi
        done
        if [ ! -x ${SERVER_REPO}/scripts/server_ctl.sh ]; then
            echo \"MISSING_CTL\"; exit 1
        fi
        echo \"REMOTE_BINS_OK\"
    "
    REMOTE_CHECK_RES=$(ssh -o BatchMode=yes -o ConnectTimeout=5 "${SERVER_USER}@${SERVER_HOST}" "$CHECK_REMOTE_BINS" 2>&1 || true)
    if [[ "$REMOTE_CHECK_RES" != *"REMOTE_BINS_OK"* ]]; then
        log "[FATAL] Remote server check failed: $REMOTE_CHECK_RES"
        log "        Ensure 'make echo' was executed on the Server laptop in $SERVER_REPO"
        exit 1
    fi
    log "[OK] Remote server binaries and server_ctl.sh verified."
else
    log "[INFO] Direct Mode (No SSH): Target server expected at ${SERVER_HOST}:${PORT}"
    if nc -z -w 2 "$SERVER_HOST" "$PORT" 2>/dev/null; then
        log "[OK] Target server at ${SERVER_HOST}:${PORT} is UP and reachable!"
    else
        log "[NOTICE] Server at ${SERVER_HOST}:${PORT} is currently not reachable."
        log "         Ensure the server is running on the Server laptop (e.g. ./scripts/run_server.sh -s epoll)."
    fi
fi

# Generate results/env.txt
log "[INFO] Recording environment metadata to $ENV_FILE..."
{
    echo "=== Client Environment ($(date)) ==="
    uname -a
    git -C "$REPO_DIR" log -1 --format="commit: %H%ndate: %ci%nsubject: %s" || true
    git -C "$REPO_DIR" status --short || true
    echo "--- CPU Info ---"
    lscpu 2>/dev/null || true
    echo "--- Scaling Governor ---"
    cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor 2>/dev/null | head -n 4 || true
    echo "--- Network Sysctls ---"
    sysctl net.core.somaxconn net.ipv4.tcp_max_syn_backlog net.core.rmem_max net.core.wmem_max net.ipv4.ip_local_port_range 2>/dev/null || true
    echo "--- Ethtool Client NIC ---"
    ethtool enp3s0 2>/dev/null | grep -E "Speed|Duplex|Link detected" || true
    echo ""
    echo "=== Remote Server Environment ==="
    ssh "${SERVER_USER}@${SERVER_HOST}" "uname -a; cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || true" 2>/dev/null || true
} > "$ENV_FILE"

# ==============================================================================
# Parse and Filter Experiments Configuration
# ==============================================================================
CONFIG_LIST=()
while IFS= read -r line || [[ -n "$line" ]]; do
    # Strip whitespace & comments
    line=$(echo "$line" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//' -e 's/#.*//')
    [[ -z "$line" ]] && continue

    IFS=',' read -r exp mech conns rate active_share duration <<< "$line"
    if [[ -n "$ONLY_EXP" && "$exp" != "$ONLY_EXP" ]]; then
        continue
    fi
    if [[ -n "$ONLY_MECH" && "$mech" != "$ONLY_MECH" ]]; then
        continue
    fi
    CONFIG_LIST+=("$line")
done < "$CONF_FILE"

TOTAL_CONFIGS=${#CONFIG_LIST[@]}
if [[ $TOTAL_CONFIGS -eq 0 ]]; then
    log "[FATAL] No configurations matched filter (only='$ONLY_EXP')."
    exit 1
fi
log "[INFO] Loaded $TOTAL_CONFIGS unique configurations from $CONF_FILE"

# Estimate total sweep time: each test ~ duration + warmup (5s) + cooldown (5s) + overhead (5s)
AVG_RUN_SEC=35
TOTAL_RUNS=$(( TOTAL_CONFIGS * ROUNDS ))
ESTIMATED_TOTAL_HOURS=$(awk -v runs="$TOTAL_RUNS" -v sec="$AVG_RUN_SEC" 'BEGIN { printf "%.2f", (runs * sec) / 3600.0 }')
log "[INFO] Schedule: $TOTAL_CONFIGS configs x $ROUNDS rounds = $TOTAL_RUNS total runs."
log "[INFO] Estimated total execution time: ~${ESTIMATED_TOTAL_HOURS} hours (Cap: ${MAX_HOURS}h)."

if [[ $DRY_RUN -eq 1 ]]; then
    log "================================================================="
    log "   DRY RUN MODE ENABLED - Listing Configurations"
    log "================================================================="
    printf "%-5s %-8s %-8s %-10s %-8s %-8s\n" "EXP" "MECH" "CONNS" "RATE" "SHARE" "DURATION"
    for cfg in "${CONFIG_LIST[@]}"; do
        IFS=',' read -r exp mech conns rate active_share duration <<< "$cfg"
        printf "%-5s %-8s %-8s %-10s %-8s %-8s\n" "$exp" "$mech" "$conns" "$rate" "$active_share" "${duration}s"
    done
    log "[DRY RUN] Completed. Exiting without running tests."
    exit 0
fi

# ==============================================================================
# Initialize Master CSV Header
# ==============================================================================
LOADGEN_CSV_HEADER="target_rate,actual_send_rate,echoed_rate,total_sent,total_received,rtt_p50_us,rtt_p95_us,rtt_p99_us,conn_failures,send_errors,recv_errors,timeouts,mismatched,duration_s,actual_duration_s,host,port,connections,msg_size,rtt_min_us,rtt_max_us,rtt_avg_us,concurrency_stalls,target_achieved"
MASTER_HEADER="exp,mech,repeat,round,timestamp,git_commit,connections,active_share,target_rate,msg_size,valid,invalid_reason,${LOADGEN_CSV_HEADER},server_cpu_user_us,server_cpu_sys_us,server_cpu_us_per_msg,server_vmhwm_kb,server_accepts,server_reads,server_writes,server_errors,server_peer_resets,cpu_freq_before_khz,cpu_freq_after_khz"

if [[ ! -f "$MASTER_CSV" || ! -s "$MASTER_CSV" ]]; then
    echo "$MASTER_HEADER" > "$MASTER_CSV"
fi

for e in E1 E1s E2 E3; do
    EXP_CSV="$RESULTS_DIR/${e}.csv"
    if [[ ! -f "$EXP_CSV" || ! -s "$EXP_CSV" ]]; then
        echo "$MASTER_HEADER" > "$EXP_CSV"
    fi
done

GIT_COMMIT=$(git -C "$REPO_DIR" rev-parse --short HEAD 2>/dev/null || echo "unknown")
SWEEP_START_SEC=$(date +%s)
MAX_SECONDS=$(awk -v h="$MAX_HOURS" 'BEGIN { printf "%d", h * 3600 }')
RANDOM_SEED=${RANDOM_SEED:-$(date +%s)}
log "[INFO] Randomization Seed: $RANDOM_SEED"

# Helper to check if a specific run already exists and was valid
is_run_completed() {
    local exp="$1" mech="$2" conns="$3" rate="$4" share="$5" rep="$6"
    if [[ ! -f "$MASTER_CSV" ]]; then return 1; fi
    # Check if a row with exp,mech,repeat,*,*,*,conns,share,rate exists with valid=1
    awk -F',' -v e="$exp" -v m="$mech" -v c="$conns" -v r="$rate" -v s="$share" -v rep="$rep" '
        $1 == e && $2 == m && $3 == rep && $7 == c && $8 == s && $9 == r && $11 == "1" { found=1; exit }
        END { exit !found }
    ' "$MASTER_CSV"
}

# ==============================================================================
# Main Round-Based Execution Loop
# ==============================================================================
TOTAL_RUNS_COMPLETED=0
TOTAL_INVALID_RUNS=0

for round in $(seq 1 "$ROUNDS"); do
    ROUND_START_SEC=$(date +%s)
    log "================================================================="
    log "   Starting Round $round of $ROUNDS"
    log "================================================================="

    # Group configs into workload blocks
    # Workload key: exp_conns_rate_share_duration
    declare -A BLOCKS=()
    for cfg in "${CONFIG_LIST[@]}"; do
        IFS=',' read -r exp mech conns rate active_share duration <<< "$cfg"
        key="${exp}__${conns}__${rate}__${active_share}__${duration}"
        BLOCKS["$key"]+="${cfg}|"
    done

    # Randomize block order
    BLOCK_KEYS=($(printf "%s\n" "${!BLOCKS[@]}" | shuf --random-source=<(yes "$RANDOM_SEED")))

    for block_key in "${BLOCK_KEYS[@]}"; do
        RAW_BLOCK_CFGS="${BLOCKS[$block_key]}"
        IFS='|' read -ra BLOCK_CFGS <<< "$RAW_BLOCK_CFGS"
        
        # Randomize mechanism order within block
        SHUFFLED_CFGS=($(printf "%s\n" "${BLOCK_CFGS[@]}" | grep -v '^$' | shuf --random-source=<(yes "$RANDOM_SEED")))

        for cfg in "${SHUFFLED_CFGS[@]}"; do
            [[ -z "$cfg" ]] && continue
            IFS=',' read -r exp mech conns rate active_share duration <<< "$cfg"

            # Check if this run is already completed (Resume feature)
            if is_run_completed "$exp" "$mech" "$conns" "$rate" "$active_share" "$round"; then
                log "[SKIP] Run $exp $mech c=$conns r=$rate share=$active_share round=$round already exists and valid."
                continue
            fi

            # Run with retry logic (up to 2 retries = 3 attempts max)
            ATTEMPT=0
            MAX_ATTEMPTS=3
            RUN_VALID=0
            RUN_INVALID_REASON=""

            while [[ $ATTEMPT -lt $MAX_ATTEMPTS ]]; do
                ATTEMPT=$((ATTEMPT + 1))
                log "[RUN] (R$round/A$ATTEMPT) $exp | $mech | conns=$conns | rate=$rate | share=$active_share | dur=${duration}s"

                CPU_FREQ_BEFORE=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq 2>/dev/null || echo 0)
                TIMESTAMP=$(date -u +"%Y-%m-%dT%H:%M:%SZ")
                RAW_OUTPUT_CSV="$RAW_DIR/${exp}-${mech}-c${conns}-a${active_share}-r${rate}-round${round}-att${ATTEMPT}.csv"
                rm -f "$RAW_OUTPUT_CSV"

                # 1. Start Server & Snapshot (if SSH enabled)
                if [[ $USE_SSH -eq 1 ]]; then
                    START_OUT=$(ssh -o BatchMode=yes -o ConnectTimeout=10 "${SERVER_USER}@${SERVER_HOST}" \
                        "${SERVER_REPO}/scripts/server_ctl.sh start $mech $PORT $MSG_SIZE 0" 2>&1 || true)
                    
                    if [[ "$START_OUT" != *"SERVER_STARTED"* ]]; then
                        RUN_VALID=0
                        RUN_INVALID_REASON="server_start_failed: $START_OUT"
                        log "[ERROR] $RUN_INVALID_REASON"
                        sleep 1
                        continue
                    fi
                fi

                # 2. Wait until Server Port is accepting TCP connections
                PORT_READY=0
                for _ in {1..30}; do
                    if nc -z -w 1 "$SERVER_HOST" "$PORT" 2>/dev/null; then
                        PORT_READY=1
                        break
                    fi
                    sleep 0.1
                done

                if [[ $PORT_READY -eq 0 ]]; then
                    RUN_VALID=0
                    RUN_INVALID_REASON="server_port_${SERVER_HOST}_${PORT}_unreachable"
                    log "[ERROR] $RUN_INVALID_REASON"
                    if [[ $USE_SSH -eq 1 ]]; then
                        ssh "${SERVER_USER}@${SERVER_HOST}" "${SERVER_REPO}/scripts/server_ctl.sh stop" >/dev/null 2>&1 || true
                    else
                        log "[WAIT] Please ensure the '$mech' server is running on the Server laptop ($SERVER_HOST:$PORT)."
                    fi
                    continue
                fi

                # 3. Snapshot Server Counters Before Loadgen
                SNAP_BEFORE=""
                if [[ $USE_SSH -eq 1 ]]; then
                    SNAP_BEFORE=$(ssh -o BatchMode=yes -o ConnectTimeout=5 "${SERVER_USER}@${SERVER_HOST}" \
                        "${SERVER_REPO}/scripts/server_ctl.sh snapshot" 2>&1 || true)
                fi

                # 4. Execute Load Generator on Client
                EXTRA_LOADGEN_ARGS=""
                if [[ "$rate" == "0" ]]; then
                    EXTRA_LOADGEN_ARGS="--unthrottled"
                fi

                set +e
                $LOADGEN_BIN \
                    --host "$SERVER_HOST" \
                    --port "$PORT" \
                    --connections "$conns" \
                    --rate "$rate" \
                    $EXTRA_LOADGEN_ARGS \
                    --active-share "$active_share" \
                    --warmup "$WARMUP" \
                    --duration "$duration" \
                    --cooldown "$COOLDOWN" \
                    --msg-size "$MSG_SIZE" \
                    --output "$RAW_OUTPUT_CSV" \
                    > /dev/null 2>&1
                LOADGEN_RC=$?
                set -e

                # 5. Snapshot Server Counters After Loadgen & Stop Server
                SNAP_AFTER=""
                if [[ $USE_SSH -eq 1 ]]; then
                    SNAP_AFTER=$(ssh -o BatchMode=yes -o ConnectTimeout=5 "${SERVER_USER}@${SERVER_HOST}" \
                        "${SERVER_REPO}/scripts/server_ctl.sh snapshot" 2>&1 || true)
                    ssh -o BatchMode=yes -o ConnectTimeout=5 "${SERVER_USER}@${SERVER_HOST}" \
                        "${SERVER_REPO}/scripts/server_ctl.sh stop" >/dev/null 2>&1 || true
                fi

                CPU_FREQ_AFTER=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq 2>/dev/null || echo 0)

                # Check Loadgen Exit & Output file
                if [[ $LOADGEN_RC -ne 0 || ! -s "$RAW_OUTPUT_CSV" ]]; then
                    RUN_VALID=0
                    RUN_INVALID_REASON="loadgen_failed_rc_${LOADGEN_RC}"
                    log "[WARNING] $RUN_INVALID_REASON"
                    sleep 1
                    continue
                fi

                # Parse loadgen CSV row
                LOADGEN_ROW=$(tail -n 1 "$RAW_OUTPUT_CSV")
                IFS=',' read -r lg_target_rate lg_actual_send lg_echoed lg_total_sent lg_total_recv \
                    lg_p50 lg_p95 lg_p99 lg_conn_fail lg_send_err lg_recv_err lg_timeouts lg_mismatched \
                    lg_dur lg_act_dur lg_host lg_port lg_conns lg_msg lg_min lg_max lg_avg lg_stalls lg_achieved <<< "$LOADGEN_ROW"

                # Parse Snapshot metrics helper
                parse_snap() {
                    local snap_str="$1" key="$2"
                    echo "$snap_str" | grep -o "${key}=[0-9]*" | cut -d'=' -f2 || echo 0
                }

                USER_BEFORE=$(parse_snap "$SNAP_BEFORE" "user_us")
                SYS_BEFORE=$(parse_snap "$SNAP_BEFORE" "sys_us")
                ACCEPTS_BEFORE=$(parse_snap "$SNAP_BEFORE" "accepts")
                READS_BEFORE=$(parse_snap "$SNAP_BEFORE" "reads")
                WRITES_BEFORE=$(parse_snap "$SNAP_BEFORE" "writes")
                ERRORS_BEFORE=$(parse_snap "$SNAP_BEFORE" "errors")
                PEER_RESETS_BEFORE=$(parse_snap "$SNAP_BEFORE" "peer_resets")

                USER_AFTER=$(parse_snap "$SNAP_AFTER" "user_us")
                SYS_AFTER=$(parse_snap "$SNAP_AFTER" "sys_us")
                VMHWM_KB=$(parse_snap "$SNAP_AFTER" "vmhwm_kb")
                ACCEPTS_AFTER=$(parse_snap "$SNAP_AFTER" "accepts")
                READS_AFTER=$(parse_snap "$SNAP_AFTER" "reads")
                WRITES_AFTER=$(parse_snap "$SNAP_AFTER" "writes")
                ERRORS_AFTER=$(parse_snap "$SNAP_AFTER" "errors")
                PEER_RESETS_AFTER=$(parse_snap "$SNAP_AFTER" "peer_resets")

                DIFF_USER_US=$(( USER_AFTER >= USER_BEFORE ? USER_AFTER - USER_BEFORE : 0 ))
                DIFF_SYS_US=$(( SYS_AFTER >= SYS_BEFORE ? SYS_AFTER - SYS_BEFORE : 0 ))
                TOTAL_CPU_US=$(( DIFF_USER_US + DIFF_SYS_US ))
                CPU_PER_MSG=0
                if [[ -n "$lg_total_recv" && "$lg_total_recv" -gt 0 ]]; then
                    CPU_PER_MSG=$(awk -v cpu="$TOTAL_CPU_US" -v msgs="$lg_total_recv" 'BEGIN { printf "%.2f", cpu / msgs }')
                fi

                DIFF_ACCEPTS=$(( ACCEPTS_AFTER >= ACCEPTS_BEFORE ? ACCEPTS_AFTER - ACCEPTS_BEFORE : 0 ))
                DIFF_READS=$(( READS_AFTER >= READS_BEFORE ? READS_AFTER - READS_BEFORE : 0 ))
                DIFF_WRITES=$(( WRITES_AFTER >= WRITES_BEFORE ? WRITES_AFTER - WRITES_BEFORE : 0 ))
                DIFF_ERRORS=$(( ERRORS_AFTER >= ERRORS_BEFORE ? ERRORS_AFTER - ERRORS_BEFORE : 0 ))
                DIFF_RESETS=$(( PEER_RESETS_AFTER >= PEER_RESETS_BEFORE ? PEER_RESETS_AFTER - PEER_RESETS_BEFORE : 0 ))

                # Validity Evaluation
                RUN_VALID=1
                RUN_INVALID_REASON=""

                if [[ "$lg_conn_fail" -gt 0 ]]; then RUN_VALID=0; RUN_INVALID_REASON="conn_failures_${lg_conn_fail}"; fi
                if [[ "$lg_send_err" -gt 0 ]]; then RUN_VALID=0; RUN_INVALID_REASON="send_errors_${lg_send_err}"; fi
                if [[ "$lg_recv_err" -gt 0 ]]; then RUN_VALID=0; RUN_INVALID_REASON="recv_errors_${lg_recv_err}"; fi
                if [[ "$lg_timeouts" -gt 0 ]]; then RUN_VALID=0; RUN_INVALID_REASON="timeouts_${lg_timeouts}"; fi
                if [[ "$lg_mismatched" -gt 0 ]]; then RUN_VALID=0; RUN_INVALID_REASON="mismatched_${lg_mismatched}"; fi

                # Check CPU frequency drop > 5%
                if [[ "$CPU_FREQ_BEFORE" -gt 0 && "$CPU_FREQ_AFTER" -gt 0 ]]; then
                    FREQ_DROP_PCT=$(awk -v b="$CPU_FREQ_BEFORE" -v a="$CPU_FREQ_AFTER" 'BEGIN { printf "%.2f", ((b - a) / b) * 100 }')
                    if (( $(echo "$FREQ_DROP_PCT > 5.0" | bc -l) )); then
                        RUN_VALID=0
                        RUN_INVALID_REASON="cpu_freq_drop_${FREQ_DROP_PCT}%"
                    fi
                fi

                # In E3, target_achieved=0 is expected saturation, NOT invalid
                # (loadgen checks already handled above)

                if [[ $RUN_VALID -eq 1 ]]; then
                    break
                else
                    log "[RETRY] Run invalid ($RUN_INVALID_REASON). Retrying attempt $ATTEMPT/$MAX_ATTEMPTS..."
                    sleep 2
                fi
            done

            if [[ $RUN_VALID -eq 0 ]]; then
                TOTAL_INVALID_RUNS=$((TOTAL_INVALID_RUNS + 1))
                log "[FAILED] Run permanently marked invalid after $MAX_ATTEMPTS attempts: $RUN_INVALID_REASON"
            else
                log "[OK] Completed $exp $mech c=$conns rate=$rate: ${lg_echoed} msgs/s, p50=${lg_p50}us"
            fi

            TOTAL_RUNS_COMPLETED=$((TOTAL_RUNS_COMPLETED + 1))

            # Assemble Master CSV Row
            ROW="${exp},${mech},${round},${round},${TIMESTAMP},${GIT_COMMIT},${conns},${active_share},${rate},${MSG_SIZE},${RUN_VALID},${RUN_INVALID_REASON},${LOADGEN_ROW},${DIFF_USER_US},${DIFF_SYS_US},${CPU_PER_MSG},${VMHWM_KB},${DIFF_ACCEPTS},${DIFF_READS},${DIFF_WRITES},${DIFF_ERRORS},${DIFF_RESETS},${CPU_FREQ_BEFORE},${CPU_FREQ_AFTER}"

            echo "$ROW" >> "$MASTER_CSV"
            echo "$ROW" >> "$RESULTS_DIR/${exp}.csv"

            sleep 2
        done
    done

    # ==========================================================================
    # Time Guard & Progress Check after each round
    # ==========================================================================
    NOW_SEC=$(date +%s)
    ELAPSED_TOTAL=$(( NOW_SEC - SWEEP_START_SEC ))
    ELAPSED_HOURS=$(awk -v sec="$ELAPSED_TOTAL" 'BEGIN { printf "%.2f", sec / 3600.0 }')
    AVG_ROUND_SEC=$(( ELAPSED_TOTAL / round ))
    REMAINING_ROUNDS=$(( ROUNDS - round ))
    PROJECTED_REMAINING_SEC=$(( AVG_ROUND_SEC * REMAINING_ROUNDS ))
    PROJECTED_TOTAL_HOURS=$(awk -v sec="$(( ELAPSED_TOTAL + PROJECTED_REMAINING_SEC ))" 'BEGIN { printf "%.2f", sec / 3600.0 }')

    log "-----------------------------------------------------------------"
    log "[ROUND $round SUMMARY] Elapsed: ${ELAPSED_HOURS}h | Projected Total: ${PROJECTED_TOTAL_HOURS}h (Cap: ${MAX_HOURS}h)"
    log "Runs completed so far: $TOTAL_RUNS_COMPLETED | Invalid runs: $TOTAL_INVALID_RUNS"
    log "-----------------------------------------------------------------"

    if (( $(echo "$PROJECTED_TOTAL_HOURS > $MAX_HOURS" | bc -l) )); then
        log "[ALERT] Projected execution time (${PROJECTED_TOTAL_HOURS}h) exceeds --max-hours (${MAX_HOURS}h)!"
        if [[ $REMAINING_ROUNDS -gt 0 ]]; then
            NEXT_PROJECTED_END=$(( ELAPSED_TOTAL + AVG_ROUND_SEC ))
            if [[ $NEXT_PROJECTED_END -gt $MAX_SECONDS ]]; then
                log "[TIME GUARD] Stopping now: starting Round $((round + 1)) would exceed the hard cap of ${MAX_HOURS}h."
                log "[TIME GUARD] Completed $round full rounds across all configurations."
                break
            fi
        fi
    fi
done

SWEEP_FINISH_SEC=$(date +%s)
TOTAL_SWEEP_HOURS=$(awk -v sec="$(( SWEEP_FINISH_SEC - SWEEP_START_SEC ))" 'BEGIN { printf "%.2f", sec / 3600.0 }')

log "================================================================="
log "[SUCCESS] Benchmark sweep completed in ${TOTAL_SWEEP_HOURS} hours."
log "Total completed runs: $TOTAL_RUNS_COMPLETED | Total invalid runs: $TOTAL_INVALID_RUNS"
log "Results written to: $MASTER_CSV"
log "Experiment CSVs: $RESULTS_DIR/E1.csv, $RESULTS_DIR/E1s.csv, $RESULTS_DIR/E2.csv, $RESULTS_DIR/E3.csv"
log "================================================================="

