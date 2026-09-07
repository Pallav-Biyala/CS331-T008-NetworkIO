# io_uring TCP Echo Server

This folder contains the `io_uring` implementation (Engine #4) for the CS331 benchmarking project.

## Files

| File | Purpose |
|---|---|
| `network_utils.h` | Shared socket helpers header (copied from `../`) |
| `network_utils.c` | Shared socket helpers implementation (copied from `../`) |
| `server_iouring.c` | The io_uring echo server |
| `Makefile` | Build script |
| `notes` | Detailed command explanations and I/O architecture notes |

## Requirements

- **Linux kernel ≥ 5.6** (`IORING_OP_ACCEPT` needs 5.5, `IORING_OP_SEND`/`RECV` needs 5.6)
- `gcc` with C11 support (`-std=gnu11`)
- **No liburing** — the server uses raw `io_uring_setup`, `io_uring_enter` syscalls
  and `mmap` directly so every detail is visible in the source.

Check your kernel version:
```bash
uname -r
```

## Build

```bash
make clean all
```

`make clean all` removes old object files and the executable, then compiles and
links the server again. To build without cleaning, use `make`.

## Run

```bash
./server_iouring [port] [backlog]

# defaults: port=8080, backlog=SOMAXCONN
./server_iouring 8080
```

The server prints a listening message when the io_uring ring is initialized.
Press `Ctrl+C` to stop it.

## Quick Smoke Test

In a second terminal:
```bash
echo "hello io_uring" | nc 127.0.0.1 8080
```
You should see `hello io_uring` echoed back.

### Demonstrate Multiple Clients

Keep the server running in one terminal. In a second WSL/Linux terminal, run
five clients concurrently:

```bash
for i in 1 2 3 4 5; do
(
  python3 -c 'import socket,sys,time; n=sys.argv[1]; s=socket.create_connection(("127.0.0.1",8080)); s.sendall(("message from client "+n+"\n").encode()); print("client",n,"received:",s.recv(4096).decode().strip()); time.sleep(10); s.close(); print("client",n,"closed")' "$i"
) &
done
wait
```

Each client sends a different message, prints the echoed response, and keeps
its connection open for 10 seconds. The server terminal should show several
`New client connected` messages before the clients disconnect. Increase
`time.sleep(10)` to `time.sleep(30)` for a slower demonstration.

On Windows, enter the project through WSL because this implementation uses
Linux io_uring system calls:

```powershell
wsl --cd "/mnt/d/GNSK-2/SEM-5/Computer Networks/CS331-T008-NetworkIO/code/io_uring"
```

See [`notes`](notes) for an explanation of every command and a comparison of
`select()`, `poll()`, `epoll`, and `io_uring`.

---

## Fairness Fixes (Handicapping io_uring to match poll/epoll overhead)

To ensure the benchmark results reflect true algorithmic differences rather than
implementation shortcuts, four confounds were intentionally introduced:

| # | Confound | poll & epoll Baseline | Original io_uring | Fix Applied |
|---|---|---|---|---|
| 1 | **Memory Allocation** | `malloc()`/`free()` per client | Zero-cost static array | Added `malloc`/`free` per connection; pointer passed via `user_data` |
| 2 | **Syscall Count** | 3–4 syscalls per connection (uses `fcntl`) | 1–2 syscalls (used `SOCK_NONBLOCK` flag) | Removed `SOCK_NONBLOCK`; call `fcntl()` manually in accept handler |
| 3 | **Connection Limits** | Caps at 10,000 (poll) / unlimited (epoll) | Hard-capped at 4,096 | Raised `MAX_CLIENTS` to 10,000 |
| 4 | **Ring Exhaustion** | Safely drops data if buffer fills | Crashes if >256 events arrive at once | `drain_cq()` checks SQ fullness and flushes mid-loop |

---

## Architecture Notes

### Why io_uring is different from epoll

| Mechanism | Notification model | Syscalls per request |
|---|---|---|
| `select` / `poll` | Readiness (O(N) scan) | 1 per wake-up + 1 recv + 1 send |
| `epoll` | Readiness (O(1) interest list) | 1 `epoll_wait` + 1 `recv` + 1 `send` |
| `io_uring` | **Completion** (ring buffer) | ≈ 0 when ring is busy (batched) |

With `epoll`, even after the kernel tells you "fd X is readable", you still have
to issue a `recv()` syscall yourself — crossing the user-kernel boundary again.

With `io_uring` you submit a `RECV` SQE once. The kernel writes the data AND a
completion event. You never cross the boundary again just to do the I/O —
you only call `io_uring_enter()` to submit a batch of operations and optionally
wait for completions in a single syscall.

### Ring Buffer Layout

```
User Space                    Kernel Space
──────────────────────────────────────────
SQ tail ──writes──▶ SQEs ──reads──▶ SQ head
                                           (kernel processes ops)
CQ head ──reads──◀ CQEs ◀──writes── CQ tail
```

### user_data Encoding

Each SQE carries a 64-bit `user_data` tag that arrives back in the CQE.
The low 4 bits store the op type; the remaining bits store the heap pointer
to the `client_state` (safe because `malloc` guarantees ≥8-byte alignment):

```
bits 63–4 : heap pointer to client_state  (0 for ACCEPT / CLOSE)
bits  3–0 : op type  (OP_ACCEPT=1, OP_RECV=2, OP_SEND=3, OP_CLOSE=4)
```

### State Machine per Client

```
accept() completes  →  fcntl() non-block  →  malloc() client_state
      │
      ▼
[submit RECV] ──recv completes──▶ [submit SEND] ──send completes──▶ [submit RECV]
                                                                          │
                                                               (loop forever until EOF)
                                                                          │
                                                                   EOF / error
                                                                          │
                                                               free(client_state)
                                                               submit CLOSE
```
