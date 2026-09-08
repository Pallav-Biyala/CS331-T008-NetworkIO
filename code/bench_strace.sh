#!/usr/bin/env bash
# ============================================================
# 2/3 — SYSCALL COUNTS (strace -c, isolated trial per engine/conns)
# strace has heavy ptrace overhead — throughput/latency numbers
# from THIS run are not trustworthy, only the syscall counts are.
# ============================================================

PORT=8080
LOAD_DURATION=15s      # tcpkali just needs to keep traffic flowing
CONNECT_RATE=2000       # conns/sec tcpkali ramps at — needed for c5000/c10000
STRACE_WINDOW=10        # seconds of strace -c capture mid-run
CONN_COUNTS=(10 50 100 200 400 800 1600 3200 4000 4500 5000)
ENGINES=(select poll epoll iouring iouring_sqpoll)
RESULTS=./results/strace
mkdir -p "$RESULTS"

declare -A BIN=(
  [select]=./select_server
  [poll]=./poll_server
  [epoll]=./epoll_server
  [iouring]=./iouring_server
  [iouring_sqpoll]=./iouring_sqpoll_server
)

shuffled=($(printf "%s\n" "${ENGINES[@]}" | shuf))

for engine in "${shuffled[@]}"; do
  for conns in "${CONN_COUNTS[@]}"; do
    if [[ "$engine" == "select" && "$conns" -ge 1024 ]]; then
      echo "Skipping select @ $conns (exceeds FD_SETSIZE)"
      continue
    fi
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
