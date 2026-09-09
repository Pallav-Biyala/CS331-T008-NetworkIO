#!/usr/bin/env bash
# ============================================================
# 2/3 — SYSCALL COUNTS (strace -c, isolated trial per engine/conns)
# strace has heavy ptrace overhead — throughput/latency numbers
# from THIS run are not trustworthy, only the syscall counts are.
# ============================================================

PORT=8080
LOAD_DURATION=20s      # tcpkali just needs to keep traffic flowing
CONNECT_RATE=2000       # conns/sec tcpkali ramps at — needed for c5000/c10000
STRACE_WINDOW=10        # seconds of strace -c capture mid-run
CONN_COUNTS=(10 100 500 1000 1500 2000 2500 3000 3500 4000 4500 5000)
ENGINES=(select poll epoll iouring iouring_sqpoll)
RESULTS=./results/strace
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

    fuser -k "$PORT"/tcp 2>/dev/null
    sleep 0.5

    taskset -c 0 "${BIN[$engine]}" "$PORT" > "$RESULTS/${tag}_server.log" 2>&1 &
    server_pid=$!
    sleep 1

    if ! kill -0 "$server_pid" 2>/dev/null; then
      echo "WARN: $tag server failed to start, see ${tag}_server.log" >&2
      continue
    fi

    taskset -c 1 tcpkali -c "$conns" \
      -m '{"ping"}' \
      -T "$LOAD_DURATION" \
      --connect-rate "$CONNECT_RATE" \
      "127.0.0.1:$PORT" \
      > "$RESULTS/${tag}_tcpkali_ignore.log" 2>&1 &
    load_pid=$!

    sleep 3   # let load reach steady state before attaching strace

    if kill -0 "$server_pid" 2>/dev/null; then
      strace -f -c -o "$RESULTS/${tag}_strace.txt" -p "$server_pid" &
      strace_pid=$!
      sleep "$STRACE_WINDOW"
      kill -INT "$strace_pid" 2>/dev/null
      wait "$strace_pid" 2>/dev/null
    else
      echo "WARN: $tag server died before strace could attach" >&2
    fi

    wait "$load_pid" 2>/dev/null
    kill "$server_pid" 2>/dev/null
    wait "$server_pid" 2>/dev/null
    sleep 4
  done
done

echo "Done. Syscall counts in $RESULTS/*_strace.txt"
