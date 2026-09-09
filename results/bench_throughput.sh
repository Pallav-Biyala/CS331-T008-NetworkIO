#!/usr/bin/env bash
# ============================================================
# 1/3 — THROUGHPUT & LATENCY (clean run, no instrumentation)
#
# Two passes per (engine × connection-count) cell:
#
#  Pass 1 — unlimited message rate
#    → measures: aggregate bandwidth (Mbps), packet rate
#    → output:   results/throughput/<tag>_tcpkali.log
#
#  Pass 2 — fixed message rate (--message-rate 20)
#    → measures: p50 / p95 / p99 / p99.9 round-trip latency
#    → output:   results/throughput/<tag>_latency_tcpkali.log
#
# Note: you cannot get both max-throughput AND latency from one
# tcpkali run — unlimited rate floods the pipe (good for Mbps),
# but tcpkali can only compute per-message RTT when the send rate
# is fixed so it can timestamp each message individually.
# ============================================================

PORT=8080
CONNECT_RATE=5000        # conns/sec — raised so ramp fits inside the window at c5000/c10000
MESSAGE_RATE=20          # msgs/sec per connection for the latency pass
CONN_COUNTS=(10 100 500 1000 1500 2000 2500 3000 3500 4000 4500 5000)
ENGINES=(select poll epoll iouring iouring_sqpoll)
RESULTS=./results/throughput
mkdir -p "$RESULTS"

declare -A BIN=(
  [select]=./code/server_select
  [poll]=./code/server_poll
  [epoll]=./code/server_epoll
  [iouring]=./code/server_iouring
  [iouring_sqpoll]=./code/server_iouring_sqpoll
)

# Pre-flight: confirm tcpkali version and flag support
echo "tcpkali version: $(tcpkali --version 2>&1 | head -1)"
if ! tcpkali --help 2>&1 | grep -q -- '--message-rate'; then
  echo "WARN: this tcpkali build has no --message-rate — latency pass percentiles will be empty." >&2
fi

# Duration scales with connection count so ramp-up doesn't eat the whole window.
# At CONNECT_RATE=5000: c5000 ramps in ~1s, c10000 in ~2s — both fit in 30s easily.
duration_for() {
  local conns=$1
  if   (( conns >= 5000 )); then echo "30s"
  else                           echo "15s"
  fi
}

# start_server: kills any existing process on PORT, starts the server pinned to CPU 0,
# and echoes only the background PID so the caller can capture it cleanly.
# IMPORTANT: fuser stdout is suppressed (>/dev/null 2>&1) — without this, fuser -k
# prints the killed PIDs to stdout, which would be captured into server_pid alongside
# the real PID and break every subsequent kill -0 check.
start_server() {
  local engine=$1 tag=$2
  sudo fuser -k "$PORT"/tcp >/dev/null 2>&1
  sleep 0.5
  taskset -c 0 "${BIN[$engine]}" "$PORT" > "$RESULTS/${tag}_server.log" 2>&1 &
  echo $!
}

stop_server() {
  local pid=$1
  kill "$pid" 2>/dev/null
  wait "$pid" 2>/dev/null
  sleep 4   # let TIME_WAIT sockets drain before the next trial
}

# Randomise engine order to avoid OS-warmth / cache-ordering bias
shuffled=($(printf "%s\n" "${ENGINES[@]}" | shuf))

for engine in "${shuffled[@]}"; do
  for conns in "${CONN_COUNTS[@]}"; do
    # select() FD_SETSIZE is now dynamically expanded to 10000 in server_select.c.

    tag="${engine}_c${conns}"
    dur=$(duration_for "$conns")

    # ------------------------------------------------------------------
    # PASS 1 — unlimited rate → bandwidth / packet-rate measurement
    # ------------------------------------------------------------------
    echo "== $tag | pass1: bandwidth (unlimited rate, duration=$dur) =="
    server_pid=$(start_server "$engine" "$tag")
    sleep 1
    if ! kill -0 "$server_pid" 2>/dev/null; then
      echo "WARN: $tag server failed to start — see ${tag}_server.log" >&2
      continue
    fi

    taskset -c 1 tcpkali \
      -c "$conns" \
      -m '{"ping"}' \
      -T "$dur" \
      --connect-rate "$CONNECT_RATE" \
      "127.0.0.1:$PORT" \
      > "$RESULTS/${tag}_tcpkali.log" 2>&1 \
      || echo "WARN: tcpkali (pass1) failed for $tag — see ${tag}_tcpkali.log" >&2

    stop_server "$server_pid"

    # ------------------------------------------------------------------
    # PASS 2 — fixed rate → latency percentile measurement
    # ------------------------------------------------------------------
    echo "== $tag | pass2: latency (message-rate=$MESSAGE_RATE, duration=$dur) =="
    server_pid=$(start_server "$engine" "${tag}_lat")
    sleep 1
    if ! kill -0 "$server_pid" 2>/dev/null; then
      echo "WARN: ${tag}_lat server failed to start — see ${tag}_lat_server.log" >&2
      continue
    fi

    taskset -c 1 tcpkali \
      -c "$conns" \
      -m '{"ping"}' \
      -T "$dur" \
      --connect-rate "$CONNECT_RATE" \
      --message-rate "$MESSAGE_RATE" \
      --latency-marker '{"ping"}' \
      --latency-percentiles=50,95,99,99.9 \
      "127.0.0.1:$PORT" \
      > "$RESULTS/${tag}_latency_tcpkali.log" 2>&1 \
      || echo "WARN: tcpkali (pass2) failed for $tag — see ${tag}_latency_tcpkali.log" >&2

    stop_server "$server_pid"
  done
done

echo ""
echo "Done. Results in $RESULTS/"
echo "  Bandwidth : <tag>_tcpkali.log"
echo "  Latency   : <tag>_latency_tcpkali.log"
