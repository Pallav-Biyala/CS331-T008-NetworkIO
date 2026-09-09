#!/usr/bin/env bash
# ============================================================
# 3/3 — CONTEXT SWITCHES / CPU / MEMORY (perf stat + pidstat, isolated)
# Lower overhead than strace, but still isolated from the throughput
# run to keep that run's numbers clean.
# ============================================================

PORT=8080
LOAD_DURATION=15s
CONNECT_RATE=2000        # conns/sec tcpkali ramps at — needed for c5000/c10000
PERF_WINDOW=10
CONN_COUNTS=(10 100 500 1000 1500 2000 2500 3000 3500 4000 4500 5000)
ENGINES=(select poll epoll iouring iouring_sqpoll)
RESULTS=./results/perf
mkdir -p "$RESULTS"

declare -A BIN=(
  [select]=./code/server_select
  [poll]=./code/server_poll
  [epoll]=./code/server_epoll
  [iouring]=./code/server_iouring
  [iouring_sqpoll]=./code/server_iouring_sqpoll
)

shuffled=($(printf "%s\n" "${ENGINES[@]}" | shuf))

for engine in "${shuffled[@]}"; do
  for conns in "${CONN_COUNTS[@]}"; do

    tag="${engine}_c${conns}"
    echo "== $tag =="

    sudo fuser -k "$PORT"/tcp 2>/dev/null
    sleep 0.5

    taskset -c 0 "${BIN[$engine]}" "$PORT" > "$RESULTS/${tag}_server.log" 2>&1 &
    server_pid=$!
    sleep 1

    if ! kill -0 "$server_pid" 2>/dev/null; then
      echo "WARN: $tag server failed to start, see ${tag}_server.log" >&2
      continue
    fi

    ps -o rss= -p "$server_pid" > "$RESULTS/${tag}_rss_before.txt"

    taskset -c 1 tcpkali -c "$conns" \
      -m '{"ping"}' \
      -T "$LOAD_DURATION" \
      --connect-rate "$CONNECT_RATE" \
      "127.0.0.1:$PORT" \
      > "$RESULTS/${tag}_tcpkali_ignore.log" 2>&1 &
    load_pid=$!

    sleep 3

    if kill -0 "$server_pid" 2>/dev/null; then
      perf stat -e context-switches,cpu-migrations,cycles,instructions \
        -p "$server_pid" -o "$RESULTS/${tag}_perf.txt" -- sleep "$PERF_WINDOW"
      pidstat -p "$server_pid" 1 5 > "$RESULTS/${tag}_pidstat.log"
      ps -o rss= -p "$server_pid" > "$RESULTS/${tag}_rss_after.txt"
    else
      echo "WARN: $tag server died before perf could attach" >&2
    fi

    wait "$load_pid" 2>/dev/null
    kill "$server_pid" 2>/dev/null
    wait "$server_pid" 2>/dev/null
    sleep 4
  done
done

echo "Done. perf/pidstat/rss data in $RESULTS/"
