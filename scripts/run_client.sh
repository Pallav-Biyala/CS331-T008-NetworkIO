#!/usr/bin/env bash
# scripts/run_client.sh
# Wrapper to invoke the load generator (client) with flexible flags.
# Usage: ./run_client.sh -s <select|poll|epoll|uring> [options]
# Options control the load‑generator parameters.
# Example:
#   ./run_client.sh -s epoll \
#       -c 500 -r 4000 -d 30 -w 10 -C 10 -m 512 -o results_epoll.csv

set -euo pipefail

# Default values for the load generator
SERVER_TYPE=""
HOST="192.168.50.1"
PORT=8080
CONNS=500
RATE=4000
DURATION=30
WARMUP=10
COOLDOWN=10
MSG_SIZE=512
ACTIVE_SHARE=1.0
UNTHROTTLED=0
OUTPUT="results.csv"

print_usage() {
  cat <<EOF
Usage: $0 -s <select|poll|epoll|uring> [options]
  -s  Server implementation that will be run on the remote machine (required)
  -h  Host IP of the server (default: $HOST)
  -p  Server port (default: $PORT)
  -c  Number of concurrent TCP connections (default: $CONNS)
  -r  Target request rate (ops/sec) (default: $RATE)
  -u  Unthrottled mode (maximum throughput, sets rate=0)
  -a  Active connection share (0.0 < share <= 1.0, default: $ACTIVE_SHARE)
  -d  Duration of the steady‑state phase in seconds (default: $DURATION)
  -w  Warm‑up time in seconds (default: $WARMUP)
  -C  Cool‑down time in seconds (default: $COOLDOWN)
  -m  Message size in bytes (default: $MSG_SIZE)
  -o  Output CSV file (default: $OUTPUT)
  -h  Show this help message
EOF
}

# Parse arguments
while getopts "s:h:p:c:r:ua:d:w:C:m:o:" opt; do
  case $opt in
    s) SERVER_TYPE=$OPTARG ;;
    h) HOST=$OPTARG ;;
    p) PORT=$OPTARG ;;
    c) CONNS=$OPTARG ;;
    r) RATE=$OPTARG ;;
    u) UNTHROTTLED=1; RATE=0 ;;
    a) ACTIVE_SHARE=$OPTARG ;;
    d) DURATION=$OPTARG ;;
    w) WARMUP=$OPTARG ;;
    C) COOLDOWN=$OPTARG ;;
    m) MSG_SIZE=$OPTARG ;;
    o) OUTPUT=$OPTARG ;;
    *) print_usage; exit 1 ;;
  esac
done

if [[ -z "$SERVER_TYPE" ]]; then
  echo "[ERROR] Server type (-s) is required"
  print_usage
  exit 1
fi

# Build a descriptive output filename if not explicitly given
if [[ "$OUTPUT" == "results.csv" ]]; then
  OUTPUT="results_${SERVER_TYPE}_c${CONNS}_r${RATE}_m${MSG_SIZE}.csv"
fi

EXTRA_FLAGS=""
if [[ $UNTHROTTLED -eq 1 || "$RATE" == "0" ]]; then
  EXTRA_FLAGS="--unthrottled"
fi

# Pin the client to a couple of cores for reproducibility
TASKSET_CMD="taskset -c 1-2"

echo "[INFO] Running load generator against $SERVER_TYPE server at $HOST:$PORT"
$TASKSET_CMD ./echo_loadgen \
  --host "$HOST" \
  --port "$PORT" \
  --connections "$CONNS" \
  --rate "$RATE" \
  $EXTRA_FLAGS \
  --active-share "$ACTIVE_SHARE" \
  --duration "$DURATION" \
  --warmup "$WARMUP" \
  --cooldown "$COOLDOWN" \
  --msg-size "$MSG_SIZE" \
  --output "$OUTPUT"

