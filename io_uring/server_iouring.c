// =============================================================================
//  server_iouring.c  —  TCP Echo Server using io_uring
// =============================================================================
//
//  Architecture Overview
//  ─────────────────────
//  Unlike epoll (which tells you "this fd is readable NOW, go do a syscall"),
//  io_uring works on a completion model:
//
//    1. You SUBMIT an operation (accept / recv / send) into a ring buffer.
//    2. The kernel executes that operation asynchronously.
//    3. You HARVEST completion events from the same ring — zero extra syscalls
//       needed while the ring is busy.
//
//  Ring structure (two lock-free single-producer / single-consumer queues):
//
//    ┌──────────────────────────────────────────┐
//    │          Submission Queue (SQ)           │
//    │   user writes SQEs  →  kernel reads      │
//    └──────────────────────────────────────────┘
//    ┌──────────────────────────────────────────┐
//    │          Completion Queue (CQ)           │
//    │   kernel writes CQEs  →  user reads      │
//    └──────────────────────────────────────────┘
//
//  Each SQE (Submission Queue Entry) carries:
//    • opcode   – IORING_OP_ACCEPT / RECV / SEND / CLOSE
//    • fd       – which socket to act on
//    • buf/len  – where to read into / write from
//    • user_data – opaque 64-bit tag we get back in the CQE
//
//  user_data encoding
//  ──────────────────
//  We pack the operation type + client fd into a single 64-bit integer so the
//  CQE handler knows exactly what just completed without any hash-table lookup.
//
//    bits 63-32 : client fd (or 0 for ACCEPT completions)
//    bits 31-0  : op_type  (OP_ACCEPT / OP_RECV / OP_SEND)
//
// =============================================================================

#include "network_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <linux/io_uring.h>   // SQE / CQE structs and opcodes
#include <sys/syscall.h>      // __NR_io_uring_setup / enter / register
#include <sys/mman.h>         // mmap for ring buffers
#include <stdatomic.h>        // atomic loads/stores for ring tail/head
#include <stdint.h>

// =============================================================================
//  Tunables
// =============================================================================

#define QUEUE_DEPTH   256    // Number of SQEs in the ring (must be power-of-2)
#define MAX_CLIENTS   4096   // Maximum simultaneous connections
#define OUT_BUF_CAP   65536  // Per-client output buffer size (64 KB)

// =============================================================================
//  Operation type tags stored in user_data
// =============================================================================

#define OP_ACCEPT  1
#define OP_RECV    2
#define OP_SEND    3
#define OP_CLOSE   4

// Pack/unpack helpers
// user_data = (fd << 32) | op
static inline uint64_t make_user_data(int fd, uint32_t op) {
    return ((uint64_t)(uint32_t)fd << 32) | (uint64_t)op;
}
static inline int      ud_fd(uint64_t ud) { return (int)(uint32_t)(ud >> 32); }
static inline uint32_t ud_op(uint64_t ud) { return (uint32_t)(ud & 0xFFFFFFFF); }

// =============================================================================
//  Per-client state
// =============================================================================
//  Each accepted client gets one of these.  We keep a flat array indexed by fd
//  (up to MAX_CLIENTS) so look-up is O(1) without any malloc overhead per
//  request.

typedef struct {
    int      fd;                    // -1 means slot is free
    char     in_buf[BUFFER_SIZE];   // scratch space for recv
    char     out_buf[OUT_BUF_CAP];  // bytes waiting to be sent
    size_t   out_len;               // bytes currently in out_buf
    int      send_in_flight;        // 1 if a SEND SQE is outstanding
    int      recv_in_flight;        // 1 if a RECV SQE is outstanding
} client_state;

static client_state clients[MAX_CLIENTS];

static void init_clients(void) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        clients[i].fd             = -1;
        clients[i].out_len        = 0;
        clients[i].send_in_flight = 0;
        clients[i].recv_in_flight = 0;
    }
}

static client_state *get_client(int fd) {
    if (fd < 0 || fd >= MAX_CLIENTS) return NULL;
    return &clients[fd];
}

static void free_client(int fd) {
    if (fd < 0 || fd >= MAX_CLIENTS) return;
    clients[fd].fd             = -1;
    clients[fd].out_len        = 0;
    clients[fd].send_in_flight = 0;
    clients[fd].recv_in_flight = 0;
}

// =============================================================================
//  Minimal io_uring wrapper
//  (avoids the liburing dependency so the code is self-contained and
//   clearly shows every syscall and memory-mapped region)
// =============================================================================

// Raw syscall wrappers ----------------------------------------------------

static int io_uring_setup(unsigned entries, struct io_uring_params *p) {
    return (int)syscall(__NR_io_uring_setup, entries, p);
}

static int io_uring_enter(int ring_fd, unsigned to_submit,
                          unsigned min_complete, unsigned flags) {
    return (int)syscall(__NR_io_uring_enter, ring_fd, to_submit,
                        min_complete, flags, NULL, 0);
}

// Ring descriptor ---------------------------------------------------------

typedef struct {
    // ring file descriptor returned by io_uring_setup
    int ring_fd;

    // --- Submission Queue (SQ) -------------------------------------------
    unsigned  *sq_head;      // kernel's head pointer (we only read)
    unsigned  *sq_tail;      // our   tail pointer    (we advance)
    unsigned  *sq_ring_mask; // mask = ring_entries - 1
    unsigned  *sq_array;     // indirection array: sq_array[tail & mask] = sqe_index
    struct io_uring_sqe *sqes; // the actual SQE array (separate mmap)

    // --- Completion Queue (CQ) -------------------------------------------
    unsigned  *cq_head;      // our   head pointer    (we advance)
    unsigned  *cq_tail;      // kernel's tail pointer (we only read)
    unsigned  *cq_ring_mask;
    struct io_uring_cqe *cqes; // the actual CQE array

    unsigned sq_entries;     // size of SQ ring
    unsigned cq_entries;     // size of CQ ring
} io_uring_t;

// Initialise the ring and set up all mmap regions --------------------------

static int ring_init(io_uring_t *r, unsigned depth) {
    struct io_uring_params params;
    memset(&params, 0, sizeof(params));

    r->ring_fd = io_uring_setup(depth, &params);
    if (r->ring_fd < 0) {
        perror("io_uring_setup");
        return -1;
    }

    r->sq_entries = params.sq_entries;
    r->cq_entries = params.cq_entries;

    // ----------------------------------------------------------------
    //  mmap 1 — SQ ring (head, tail, mask, array live here)
    // ----------------------------------------------------------------
    size_t sq_ring_size = params.sq_off.array +
                          params.sq_entries * sizeof(unsigned);

    void *sq_ring = mmap(NULL, sq_ring_size,
                         PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_POPULATE,
                         r->ring_fd, IORING_OFF_SQ_RING);
    if (sq_ring == MAP_FAILED) {
        perror("mmap sq_ring");
        return -1;
    }

    r->sq_head      = (unsigned *)((char *)sq_ring + params.sq_off.head);
    r->sq_tail      = (unsigned *)((char *)sq_ring + params.sq_off.tail);
    r->sq_ring_mask = (unsigned *)((char *)sq_ring + params.sq_off.ring_mask);
    r->sq_array     = (unsigned *)((char *)sq_ring + params.sq_off.array);

    // ----------------------------------------------------------------
    //  mmap 2 — SQE array (actual submission queue entries)
    // ----------------------------------------------------------------
    size_t sqes_size = params.sq_entries * sizeof(struct io_uring_sqe);

    r->sqes = (struct io_uring_sqe *)mmap(NULL, sqes_size,
                                          PROT_READ | PROT_WRITE,
                                          MAP_SHARED | MAP_POPULATE,
                                          r->ring_fd, IORING_OFF_SQES);
    if (r->sqes == MAP_FAILED) {
        perror("mmap sqes");
        return -1;
    }

    // ----------------------------------------------------------------
    //  mmap 3 — CQ ring (head, tail, mask, CQE array all live here)
    // ----------------------------------------------------------------
    size_t cq_ring_size = params.cq_off.cqes +
                          params.cq_entries * sizeof(struct io_uring_cqe);

    void *cq_ring = mmap(NULL, cq_ring_size,
                         PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_POPULATE,
                         r->ring_fd, IORING_OFF_CQ_RING);
    if (cq_ring == MAP_FAILED) {
        perror("mmap cq_ring");
        return -1;
    }

    r->cq_head      = (unsigned *)((char *)cq_ring + params.cq_off.head);
    r->cq_tail      = (unsigned *)((char *)cq_ring + params.cq_off.tail);
    r->cq_ring_mask = (unsigned *)((char *)cq_ring + params.cq_off.ring_mask);
    r->cqes         = (struct io_uring_cqe *)((char *)cq_ring + params.cq_off.cqes);

    return 0;
}

// Get a fresh SQE slot from the ring ----------------------------------------
// Returns NULL when the ring is full (caller should submit first).

static struct io_uring_sqe *ring_get_sqe(io_uring_t *r) {
    unsigned head = atomic_load_explicit((_Atomic unsigned *)r->sq_head,
                                         memory_order_acquire);
    unsigned tail = *r->sq_tail;  // only we write tail

    if (tail - head >= r->sq_entries)
        return NULL; // ring full

    unsigned index = tail & (*r->sq_ring_mask);
    r->sq_array[index] = index; // SQ array maps slot → SQE index (1-to-1 here)
    return &r->sqes[index];
}

// Advance SQ tail after filling an SQE -------------------------------------

static void ring_submit_advance(io_uring_t *r) {
    atomic_store_explicit((_Atomic unsigned *)r->sq_tail,
                          *r->sq_tail + 1,
                          memory_order_release);
}

// Submit all pending SQEs to the kernel, optionally wait for completions ----
//  wait_nr > 0  → block until at least wait_nr CQEs are ready
//  wait_nr == 0 → non-blocking submit only

static int ring_submit(io_uring_t *r, unsigned wait_nr) {
    unsigned tail = atomic_load_explicit((_Atomic unsigned *)r->sq_tail,
                                          memory_order_relaxed);
    unsigned head = atomic_load_explicit((_Atomic unsigned *)r->sq_head,
                                          memory_order_relaxed);
    unsigned to_submit = tail - head;

    if (to_submit == 0 && wait_nr == 0)
        return 0;

    unsigned flags = (wait_nr > 0) ? IORING_ENTER_GETEVENTS : 0;
    int ret = io_uring_enter(r->ring_fd, to_submit, wait_nr, flags);
    if (ret < 0) {
        perror("io_uring_enter");
        return -1;
    }
    return ret;
}

// =============================================================================
//  SQE helpers — one function per operation type
// =============================================================================

// Submit an ACCEPT SQE — kernel will accept the next client on server_fd
static int submit_accept(io_uring_t *r, int server_fd,
                         struct sockaddr_in *addr, socklen_t *addrlen) {
    struct io_uring_sqe *sqe = ring_get_sqe(r);
    if (!sqe) return -1;

    memset(sqe, 0, sizeof(*sqe));
    sqe->opcode      = IORING_OP_ACCEPT;
    sqe->fd          = server_fd;
    sqe->addr        = (uint64_t)(uintptr_t)addr;
    sqe->addr2       = (uint64_t)(uintptr_t)addrlen;
    sqe->accept_flags = SOCK_NONBLOCK; // accepted socket is non-blocking
    sqe->user_data   = make_user_data(server_fd, OP_ACCEPT);

    ring_submit_advance(r);
    return 0;
}

// Submit a RECV SQE — kernel will read up to len bytes into buf from client_fd
static int submit_recv(io_uring_t *r, int client_fd, void *buf, size_t len) {
    struct io_uring_sqe *sqe = ring_get_sqe(r);
    if (!sqe) return -1;

    memset(sqe, 0, sizeof(*sqe));
    sqe->opcode    = IORING_OP_RECV;
    sqe->fd        = client_fd;
    sqe->addr      = (uint64_t)(uintptr_t)buf;
    sqe->len       = (unsigned)len;
    sqe->user_data = make_user_data(client_fd, OP_RECV);

    ring_submit_advance(r);
    return 0;
}

// Submit a SEND SQE — kernel will send len bytes from buf on client_fd
static int submit_send(io_uring_t *r, int client_fd, const void *buf, size_t len) {
    struct io_uring_sqe *sqe = ring_get_sqe(r);
    if (!sqe) return -1;

    memset(sqe, 0, sizeof(*sqe));
    sqe->opcode    = IORING_OP_SEND;
    sqe->fd        = client_fd;
    sqe->addr      = (uint64_t)(uintptr_t)buf;
    sqe->len       = (unsigned)len;
    sqe->msg_flags = MSG_NOSIGNAL; // don't raise SIGPIPE on broken pipe
    sqe->user_data = make_user_data(client_fd, OP_SEND);

    ring_submit_advance(r);
    return 0;
}

// Submit a CLOSE SQE — kernel will close client_fd asynchronously
static int submit_close(io_uring_t *r, int client_fd) {
    struct io_uring_sqe *sqe = ring_get_sqe(r);
    if (!sqe) return -1;

    memset(sqe, 0, sizeof(*sqe));
    sqe->opcode    = IORING_OP_CLOSE;
    sqe->fd        = client_fd;
    sqe->user_data = make_user_data(client_fd, OP_CLOSE);

    ring_submit_advance(r);
    return 0;
}

// =============================================================================
//  Completion handlers — called when a CQE arrives for each op type
// =============================================================================

// Forward declarations
static void handle_accept_completion(io_uring_t *r, int server_fd,
                                     struct sockaddr_in *client_addr,
                                     socklen_t *client_addrlen,
                                     int result);
static void handle_recv_completion  (io_uring_t *r, int client_fd, int result);
static void handle_send_completion  (io_uring_t *r, int client_fd, int result);
static void handle_close_completion (int client_fd, int result);

// ── ACCEPT completion ────────────────────────────────────────────────────────
// result == new client fd  (or < 0 on error)
// After each accepted client we immediately re-arm another ACCEPT so the
// server can always handle the next incoming connection.

static void handle_accept_completion(io_uring_t *r, int server_fd,
                                     struct sockaddr_in *client_addr,
                                     socklen_t *client_addrlen,
                                     int result) {
    // Re-arm accept immediately — we want to always have one queued
    if (submit_accept(r, server_fd, client_addr, client_addrlen) < 0) {
        fprintf(stderr, "submit_accept re-arm failed\n");
    }

    if (result < 0) {
        if (-result != EAGAIN && -result != EWOULDBLOCK) {
            fprintf(stderr, "accept completion error: %s\n", strerror(-result));
        }
        return;
    }

    int client_fd = result;
    printf("New client connected: fd=%d\n", client_fd);

    // Disable Nagle's algorithm for low-latency echo
    set_tcp_nodelay(client_fd);

    // Initialise per-client slot
    if (client_fd >= MAX_CLIENTS) {
        fprintf(stderr, "fd %d exceeds MAX_CLIENTS (%d), dropping\n",
                client_fd, MAX_CLIENTS);
        submit_close(r, client_fd);
        return;
    }

    client_state *cs  = get_client(client_fd);
    cs->fd            = client_fd;
    cs->out_len       = 0;
    cs->send_in_flight = 0;
    cs->recv_in_flight = 0;

    // Kick off the first RECV for this client
    if (submit_recv(r, client_fd, cs->in_buf, BUFFER_SIZE) < 0) {
        fprintf(stderr, "submit_recv failed for fd=%d\n", client_fd);
        free_client(client_fd);
        submit_close(r, client_fd);
        return;
    }
    cs->recv_in_flight = 1;
}

// ── RECV completion ──────────────────────────────────────────────────────────
// result > 0  → bytes received; echo them back
// result == 0 → clean EOF (client closed connection)
// result < 0  → error

static void handle_recv_completion(io_uring_t *r, int client_fd, int result) {
    client_state *cs = get_client(client_fd);
    if (!cs || cs->fd != client_fd) return; // stale completion

    cs->recv_in_flight = 0;

    if (result <= 0) {
        // EOF or error — close the connection
        if (result < 0 && -result != ECONNRESET && -result != EPIPE) {
            fprintf(stderr, "recv error on fd=%d: %s\n",
                    client_fd, strerror(-result));
        } else {
            printf("Client disconnected: fd=%d\n", client_fd);
        }
        free_client(client_fd);
        submit_close(r, client_fd);
        return;
    }

    // We received 'result' bytes — copy into output buffer for echoing
    size_t bytes = (size_t)result;

    if (cs->out_len + bytes > OUT_BUF_CAP) {
        // Output buffer would overflow — drop this client
        fprintf(stderr, "Output buffer overflow on fd=%d, closing\n", client_fd);
        free_client(client_fd);
        submit_close(r, client_fd);
        return;
    }

    memcpy(cs->out_buf + cs->out_len, cs->in_buf, bytes);
    cs->out_len += bytes;

    // If no SEND is already in flight, submit one now
    if (!cs->send_in_flight) {
        if (submit_send(r, client_fd, cs->out_buf, cs->out_len) < 0) {
            fprintf(stderr, "submit_send failed for fd=%d\n", client_fd);
            free_client(client_fd);
            submit_close(r, client_fd);
            return;
        }
        cs->send_in_flight = 1;
        // Don't re-arm RECV until the SEND completes — keeps flow simple
        // (for a throughput benchmark you can pipeline these)
    } else {
        // A SEND is already in flight; the send-completion handler will
        // re-arm the next RECV after draining the buffer.
    }
}

// ── SEND completion ──────────────────────────────────────────────────────────
// result > 0 → bytes sent; shift buffer, re-arm RECV if drained
// result < 0 → error

static void handle_send_completion(io_uring_t *r, int client_fd, int result) {
    client_state *cs = get_client(client_fd);
    if (!cs || cs->fd != client_fd) return; // stale completion

    cs->send_in_flight = 0;

    if (result < 0) {
        if (-result != EPIPE && -result != ECONNRESET) {
            fprintf(stderr, "send error on fd=%d: %s\n",
                    client_fd, strerror(-result));
        }
        free_client(client_fd);
        submit_close(r, client_fd);
        return;
    }

    size_t sent = (size_t)result;
    if (sent > cs->out_len) sent = cs->out_len; // safety clamp

    // Shift unacknowledged bytes to the front of out_buf
    cs->out_len -= sent;
    if (cs->out_len > 0) {
        memmove(cs->out_buf, cs->out_buf + sent, cs->out_len);
        // Still data to send — re-arm SEND
        if (submit_send(r, client_fd, cs->out_buf, cs->out_len) < 0) {
            fprintf(stderr, "submit_send (drain) failed for fd=%d\n", client_fd);
            free_client(client_fd);
            submit_close(r, client_fd);
            return;
        }
        cs->send_in_flight = 1;
    } else {
        // Output buffer fully drained — re-arm RECV to get more data
        if (!cs->recv_in_flight) {
            if (submit_recv(r, client_fd, cs->in_buf, BUFFER_SIZE) < 0) {
                fprintf(stderr, "submit_recv (re-arm) failed for fd=%d\n", client_fd);
                free_client(client_fd);
                submit_close(r, client_fd);
                return;
            }
            cs->recv_in_flight = 1;
        }
    }
}

// ── CLOSE completion ─────────────────────────────────────────────────────────
// We just log any unexpected errors; the fd slot was already freed.

static void handle_close_completion(int client_fd, int result) {
    if (result < 0) {
        fprintf(stderr, "close completion error on fd=%d: %s\n",
                client_fd, strerror(-result));
    }
}

// =============================================================================
//  Completion Queue drain — process all available CQEs
// =============================================================================
// Returns the number of CQEs processed.

static int drain_cq(io_uring_t *r, int server_fd,
                    struct sockaddr_in *client_addr,
                    socklen_t *client_addrlen) {
    unsigned head = atomic_load_explicit((_Atomic unsigned *)r->cq_head,
                                          memory_order_acquire);
    unsigned tail = atomic_load_explicit((_Atomic unsigned *)r->cq_tail,
                                          memory_order_acquire);
    int processed = 0;

    while (head != tail) {
        struct io_uring_cqe *cqe = &r->cqes[head & (*r->cq_ring_mask)];

        uint64_t ud  = cqe->user_data;
        int      res = cqe->res;
        uint32_t op  = ud_op(ud);
        int      fd  = ud_fd(ud);

        switch (op) {
            case OP_ACCEPT:
                handle_accept_completion(r, server_fd,
                                         client_addr, client_addrlen, res);
                break;
            case OP_RECV:
                handle_recv_completion(r, fd, res);
                break;
            case OP_SEND:
                handle_send_completion(r, fd, res);
                break;
            case OP_CLOSE:
                handle_close_completion(fd, res);
                break;
            default:
                fprintf(stderr, "Unknown op=%u in CQE\n", op);
                break;
        }

        head++;
        processed++;
    }

    // Advance CQ head so the kernel can reuse those slots
    if (processed > 0) {
        atomic_store_explicit((_Atomic unsigned *)r->cq_head,
                              head, memory_order_release);
    }

    return processed;
}

// =============================================================================
//  main
// =============================================================================

int main(int argc, char *argv[]) {

    // ── Parse arguments ──────────────────────────────────────────────────────
    int port    = (argc > 1) ? atoi(argv[1]) : 8080;
    int backlog = (argc > 2) ? atoi(argv[2]) : SOMAXCONN;

    // ── Create and bind the listening socket ─────────────────────────────────
    // We reuse the same create_server_socket() helper as the epoll server.
    // It sets SO_REUSEADDR, binds, listens, and makes the fd non-blocking.
    int server_fd = create_server_socket(port, backlog);
    if (server_fd < 0) {
        return 1;
    }

    // ── Initialise the io_uring ring ─────────────────────────────────────────
    io_uring_t ring;
    memset(&ring, 0, sizeof(ring));
    if (ring_init(&ring, QUEUE_DEPTH) < 0) {
        close(server_fd);
        return 1;
    }

    // ── Initialise per-client state table ────────────────────────────────────
    init_clients();

    // ── Prime the first ACCEPT SQE ───────────────────────────────────────────
    // We keep one ACCEPT permanently in the ring; each time it fires we
    // re-arm it immediately (see handle_accept_completion).
    struct sockaddr_in client_addr;
    socklen_t client_addrlen = sizeof(client_addr);
    memset(&client_addr, 0, sizeof(client_addr));

    if (submit_accept(&ring, server_fd, &client_addr, &client_addrlen) < 0) {
        fprintf(stderr, "Initial submit_accept failed\n");
        close(server_fd);
        close(ring.ring_fd);
        return 1;
    }

    // Submit the accept SQE and start waiting
    if (ring_submit(&ring, 0) < 0) {
        close(server_fd);
        close(ring.ring_fd);
        return 1;
    }

    printf("io_uring echo server listening on port %d "
           "(queue_depth=%d, server_fd=%d, ring_fd=%d)\n",
           port, QUEUE_DEPTH, server_fd, ring.ring_fd);

    // =========================================================================
    //  Main event loop
    //  ─────────────────
    //  1. Submit any pending SQEs and block until at least 1 CQE arrives.
    //  2. Drain all available CQEs (each one may enqueue more SQEs).
    //  3. Repeat.
    //
    //  Crucially, the kernel can complete multiple operations in the time we
    //  spend processing the batch — that is the batching advantage of io_uring
    //  over epoll.  With SQPOLL enabled (not used here to keep the code
    //  dependency-free) you can eliminate the io_uring_enter() syscall
    //  entirely for the submit path.
    // =========================================================================
    while (1) {
        // Block until at least 1 completion is ready AND submit any pending SQEs
        if (ring_submit(&ring, 1) < 0) {
            // EINTR is fine — just retry
            if (errno == EINTR) continue;
            perror("ring_submit");
            break;
        }

        // Drain all ready completions
        drain_cq(&ring, server_fd, &client_addr, &client_addrlen);

        // Submit any SQEs that drain_cq() just enqueued (non-blocking)
        ring_submit(&ring, 0);
    }

    close(server_fd);
    close(ring.ring_fd);
    return 0;
}
