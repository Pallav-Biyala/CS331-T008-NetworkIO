#!/usr/bin/env bash
# scripts/run_server.sh
# Launch one of the echo servers with configurable options.
# Usage: ./run_server.sh -s <select|poll|epoll|uring> [-p <port>] [-m <msg-size>]
# Example: ./run_server.sh -s epoll -p 8080 -m 512

set -euo pipefail

# Default values
SERVER_TYPE=""
PORT=8080
MSG_SIZE=512

print_usage() {
  cat <<EOF
Usage: $0 -s <select|poll|epoll|uring> [-p <port>] [-m <msg-size>]
  -s  Server implementation to run (required)
  -p  Port to listen on (default: $PORT)
  -m  Message size in bytes (default: $MSG_SIZE)
EOF
}

# Parse arguments
while getopts "s:p:m:h" opt; do
  case $opt in
    s) SERVER_TYPE=$OPTARG ;;
    p) PORT=$OPTARG ;;
    m) MSG_SIZE=$OPTARG ;;
    h) print_usage; exit 0 ;;
    *) print_usage; exit 1 ;;
  esac
done

if [[ -z "$SERVER_TYPE" ]]; then
  echo "[ERROR] Server type (-s) is required"
  print_usage
  exit 1
fi

BIN="./build/echo-${SERVER_TYPE}"
if [[ ! -x "$BIN" ]]; then
  echo "[ERROR] Binary $BIN not found or not executable. Run 'make echo' first."
  exit 1
fi

# Ensure high file descriptor limits for high-connection sweeps
ulimit -n 65536 2>/dev/null || true
ulimit -l unlimited 2>/dev/null || true

echo "[INFO] Starting $SERVER_TYPE server on port $PORT with msg size $MSG_SIZE (max-conns: 16384, ulimit: $(ulimit -n))"
exec "$BIN" --port "$PORT" --msg-size "$MSG_SIZE" --max-conns 16384 --buf-size 4096 --nodelay

