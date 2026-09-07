// =============================================================================
//  server_iouring.c  —  TCP Echo Server using io_uring
//  (Intentionally handicapped to match poll/epoll overhead for fair benchmarking)
// =============================================================================
//
//  Four confounds resolved vs. the "naive" io_uring implementation:
//
//  Fix 1 — Memory Allocation
//    client_state is malloc()/free() per connection (not a static array),
//    matching the heap cost that poll and epoll both pay.  The heap pointer
//    is passed through SQE user_data so CQE dispatch is still O(1).
//
//  Fix 2 — Syscall Count
//    SOCK_NONBLOCK is NOT set in the ACCEPT SQE.  Instead, after each accept
//    completion, we call fcntl(F_GETFL) + fcntl(F_SETFL|O_NONBLOCK) manually,
//    paying the same 2 extra syscalls that poll and epoll pay via
//    set_socket_non_blocking().
//
//  Fix 3 — Connection Limits
//    MAX_CLIENTS raised to 10,000 to match poll's FD_SETSIZE ceiling and
//    epoll's unlimited model (bounded only by system limits).
//
//  Fix 4 — Ring Exhaustion Safety
//    drain_cq() checks whether the SQ ring is nearly full before submitting
//    each new SQE and flushes mid-loop when needed, preventing crashes when
//    > QUEUE_DEPTH events arrive simultaneously.
//
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
//  user_data encoding
//  ──────────────────
//  We pack op-type + heap pointer into a single 64-bit integer so each CQE
//  immediately identifies what just completed and which client_state owns it —
//  zero hash-table lookup.
//
//    bits 63-4  : heap pointer to client_state  (or 0 for ACCEPT)
//    bits 3-0   : op_type  (OP_ACCEPT / OP_RECV / OP_SEND / OP_CLOSE)
//
//  Because malloc() guarantees at least 8-byte alignment, the low 3 bits of
//  any valid pointer are always 0, so packing op (≤ 4) into bits 3-0 is safe.
//
// =============================================================================

#include "network_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>             // Fix 2: fcntl() for manual non-blocking
#include <sys/socket.h>
#include <netinet/in.h>
#include <linux/io_uring.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <stdatomic.h>
#include <stdint.h>

// =============================================================================
//  Tunables
// =============================================================================

#define QUEUE_DEPTH   256      // SQ/CQ ring depth (power-of-2)
#define MAX_CLIENTS   10000    // Fix 3: raised from 4096 to match poll ceiling
#define OUT_BUF_CAP   65536    // Per-client output buffer (64 KB)

// =============================================================================
//  Operation type tags  (packed into low 4 bits of user_data)
// =============================================================================

#define OP_ACCEPT  1
#define OP_RECV    2
#define OP_SEND    3
#define OP_CLOSE   4

// =============================================================================
//  Per-client state  (Fix 1: heap-allocated per connection)
// =============================================================================

typedef struct {
    int    fd;
    char   in_buf[BUFFER_SIZE];
    char   out_buf[OUT_BUF_CAP];
    size_t out_len;
    int    send_in_flight;
    int    recv_in_flight;
} client_state;

// Allocate a fresh client_state on the heap — same cost as poll/epoll
static client_state *alloc_client(int fd) {
    client_state *cs = malloc(sizeof(client_state));
    if (!cs) return NULL;
    cs->fd             = fd;
    cs->out_len        = 0;
    cs->send_in_flight = 0;
    cs->recv_in_flight = 0;
    return cs;
}

// Free the client's heap memory and close the fd
static void free_client(client_state *cs) {
    if (!cs) return;
    free(cs);
}

// =============================================================================
//  user_data helpers
//  Pack: tag = (ptr & ~0xFULL) | op   Unpack with ud_ptr() / ud_op()
// =============================================================================

static inline uint64_t make_ud(client_state *cs, uint32_t op) {
    // For ACCEPT we pass cs=NULL; for all others cs is a valid heap pointer.
    return ((uint64_t)(uintptr_t)cs & ~(uint64_t)0xF) | (uint64_t)(op & 0xF);
}

static inline client_state *ud_ptr(uint64_t ud) {
    return (client_state *)(uintptr_t)(ud & ~(uint64_t)0xF);
}

static inline uint32_t ud_op(uint64_t ud) {
    return (uint32_t)(ud & 0xF);
}

// =============================================================================
//  Minimal raw io_uring wrapper  (no liburing dependency)
// =============================================================================

static int io_uring_setup(unsigned entries, struct io_uring_params *p) {
    return (int)syscall(__NR_io_uring_setup, entries, p);
}

static int io_uring_enter(int ring_fd, unsigned to_submit,
                          unsigned min_complete, unsigned flags) {
    return (int)syscall(__NR_io_uring_enter, ring_fd, to_submit,
                        min_complete, flags, NULL, 0);
}

typedef struct {
    int ring_fd;

    // Submission Queue
    unsigned               *sq_head;
    unsigned               *sq_tail;
    unsigned               *sq_ring_mask;
    unsigned               *sq_array;
    struct io_uring_sqe    *sqes;

    // Completion Queue
    unsigned               *cq_head;
    unsigned               *cq_tail;
    unsigned               *cq_ring_mask;
    struct io_uring_cqe    *cqes;

    unsigned sq_entries;
    unsigned cq_entries;
} io_uring_t;

static int ring_init(io_uring_t *r, unsigned depth) {
    struct io_uring_params params;
    memset(&params, 0, sizeof(params));

    r->ring_fd = io_uring_setup(depth, &params);
    if (r->ring_fd < 0) { perror("io_uring_setup"); return -1; }

    r->sq_entries = params.sq_entries;
    r->cq_entries = params.cq_entries;

    // mmap 1: SQ ring (head/tail/mask/array)
    size_t sq_ring_sz = params.sq_off.array + params.sq_entries * sizeof(unsigned);
    void *sq_ring = mmap(NULL, sq_ring_sz, PROT_READ|PROT_WRITE,
                         MAP_SHARED|MAP_POPULATE, r->ring_fd, IORING_OFF_SQ_RING);
    if (sq_ring == MAP_FAILED) { perror("mmap sq_ring"); return -1; }

    r->sq_head      = (unsigned *)((char *)sq_ring + params.sq_off.head);
    r->sq_tail      = (unsigned *)((char *)sq_ring + params.sq_off.tail);
    r->sq_ring_mask = (unsigned *)((char *)sq_ring + params.sq_off.ring_mask);
    r->sq_array     = (unsigned *)((char *)sq_ring + params.sq_off.array);

    // mmap 2: SQE array
    size_t sqes_sz = params.sq_entries * sizeof(struct io_uring_sqe);
    r->sqes = mmap(NULL, sqes_sz, PROT_READ|PROT_WRITE,
                   MAP_SHARED|MAP_POPULATE, r->ring_fd, IORING_OFF_SQES);
    if (r->sqes == MAP_FAILED) { perror("mmap sqes"); return -1; }

    // mmap 3: CQ ring (head/tail/mask/cqes)
    size_t cq_ring_sz = params.cq_off.cqes + params.cq_entries * sizeof(struct io_uring_cqe);
    void *cq_ring = mmap(NULL, cq_ring_sz, PROT_READ|PROT_WRITE,
                         MAP_SHARED|MAP_POPULATE, r->ring_fd, IORING_OFF_CQ_RING);
    if (cq_ring == MAP_FAILED) { perror("mmap cq_ring"); return -1; }

    r->cq_head      = (unsigned *)((char *)cq_ring + params.cq_off.head);
    r->cq_tail      = (unsigned *)((char *)cq_ring + params.cq_off.tail);
    r->cq_ring_mask = (unsigned *)((char *)cq_ring + params.cq_off.ring_mask);
    r->cqes         = (struct io_uring_cqe *)((char *)cq_ring + params.cq_off.cqes);

    return 0;
}

// ─── SQ helpers ──────────────────────────────────────────────────────────────

// How many SQE slots are currently free in the ring?
static inline unsigned sq_space_left(io_uring_t *r) {
    unsigned head = atomic_load_explicit((_Atomic unsigned *)r->sq_head,
                                          memory_order_acquire);
    return r->sq_entries - (*r->sq_tail - head);
}

// Get a free SQE slot; returns NULL if ring is full.
static struct io_uring_sqe *ring_get_sqe(io_uring_t *r) {
    unsigned head = atomic_load_explicit((_Atomic unsigned *)r->sq_head,
                                          memory_order_acquire);
    if (*r->sq_tail - head >= r->sq_entries) return NULL;
    unsigned idx = *r->sq_tail & (*r->sq_ring_mask);
    r->sq_array[idx] = idx;
    return &r->sqes[idx];
}

static void ring_submit_advance(io_uring_t *r) {
    atomic_store_explicit((_Atomic unsigned *)r->sq_tail,
                          *r->sq_tail + 1, memory_order_release);
}

// Submit pending SQEs; wait_nr > 0 blocks until that many CQEs are ready.
static int ring_submit(io_uring_t *r, unsigned wait_nr) {
    unsigned tail = atomic_load_explicit((_Atomic unsigned *)r->sq_tail,
                                          memory_order_relaxed);
    unsigned head = atomic_load_explicit((_Atomic unsigned *)r->sq_head,
                                          memory_order_relaxed);
    unsigned to_submit = tail - head;
    if (to_submit == 0 && wait_nr == 0) return 0;
    unsigned flags = wait_nr > 0 ? IORING_ENTER_GETEVENTS : 0;
    int ret = io_uring_enter(r->ring_fd, to_submit, wait_nr, flags);
    if (ret < 0 && errno != EINTR) { perror("io_uring_enter"); return -1; }
    return ret;
}

// =============================================================================
//  SQE submit helpers — one per op type
// =============================================================================

// Fix 4 helper: flush the ring mid-loop if it's nearly full
static void maybe_flush(io_uring_t *r) {
    if (sq_space_left(r) < 4) {
        ring_submit(r, 0);
    }
}

static int submit_accept(io_uring_t *r, int server_fd,
                         struct sockaddr_in *addr, socklen_t *addrlen) {
    maybe_flush(r);  // Fix 4: guard against ring exhaustion
    struct io_uring_sqe *sqe = ring_get_sqe(r);
    if (!sqe) return -1;
    memset(sqe, 0, sizeof(*sqe));
    sqe->opcode       = IORING_OP_ACCEPT;
    sqe->fd           = server_fd;
    sqe->addr         = (uint64_t)(uintptr_t)addr;
    sqe->addr2        = (uint64_t)(uintptr_t)addrlen;
    // Fix 2: DO NOT set SOCK_NONBLOCK here; we call fcntl() manually below
    sqe->accept_flags = 0;
    sqe->user_data    = make_ud(NULL, OP_ACCEPT);
    ring_submit_advance(r);
    return 0;
}

static int submit_recv(io_uring_t *r, client_state *cs) {
    maybe_flush(r);  // Fix 4
    struct io_uring_sqe *sqe = ring_get_sqe(r);
    if (!sqe) return -1;
    memset(sqe, 0, sizeof(*sqe));
    sqe->opcode    = IORING_OP_RECV;
    sqe->fd        = cs->fd;
    sqe->addr      = (uint64_t)(uintptr_t)cs->in_buf;
    sqe->len       = BUFFER_SIZE;
    sqe->user_data = make_ud(cs, OP_RECV);
    ring_submit_advance(r);
    return 0;
}

static int submit_send(io_uring_t *r, client_state *cs) {
    maybe_flush(r);  // Fix 4
    struct io_uring_sqe *sqe = ring_get_sqe(r);
    if (!sqe) return -1;
    memset(sqe, 0, sizeof(*sqe));
    sqe->opcode    = IORING_OP_SEND;
    sqe->fd        = cs->fd;
    sqe->addr      = (uint64_t)(uintptr_t)cs->out_buf;
    sqe->len       = (unsigned)cs->out_len;
    sqe->msg_flags = MSG_NOSIGNAL;
    sqe->user_data = make_ud(cs, OP_SEND);
    ring_submit_advance(r);
    return 0;
}

static int submit_close(io_uring_t *r, int fd) {
    maybe_flush(r);  // Fix 4
    struct io_uring_sqe *sqe = ring_get_sqe(r);
    if (!sqe) { close(fd); return 0; }   // fallback: close synchronously
    memset(sqe, 0, sizeof(*sqe));
    sqe->opcode    = IORING_OP_CLOSE;
    sqe->fd        = fd;
    sqe->user_data = make_ud(NULL, OP_CLOSE);
    ring_submit_advance(r);
    return 0;
}

// =============================================================================
//  Completion handlers
// =============================================================================

// Forward declarations
static void on_accept(io_uring_t *r, int server_fd,
                      struct sockaddr_in *addr, socklen_t *addrlen, int result);
static void on_recv  (io_uring_t *r, client_state *cs, int result);
static void on_send  (io_uring_t *r, client_state *cs, int result);

// ── ACCEPT ───────────────────────────────────────────────────────────────────
static void on_accept(io_uring_t *r, int server_fd,
                      struct sockaddr_in *addr, socklen_t *addrlen, int result) {
    // Re-arm the persistent ACCEPT SQE immediately
    if (submit_accept(r, server_fd, addr, addrlen) < 0)
        fprintf(stderr, "submit_accept re-arm failed\n");

    if (result < 0) {
        if (-result != EAGAIN && -result != EWOULDBLOCK)
            fprintf(stderr, "accept error: %s\n", strerror(-result));
        return;
    }

    int client_fd = result;

    // Fix 2: manually call fcntl() instead of using SOCK_NONBLOCK in the SQE.
    // This matches the 2-syscall overhead that epoll and poll both pay via
    // set_socket_non_blocking().
    if (set_socket_non_blocking(client_fd) < 0) {
        fprintf(stderr, "set_socket_non_blocking failed for fd=%d\n", client_fd);
        submit_close(r, client_fd);
        return;
    }

    set_tcp_nodelay(client_fd);

    // Fix 1: allocate client state on the heap just like poll/epoll do
    client_state *cs = alloc_client(client_fd);
    if (!cs) {
        fprintf(stderr, "malloc failed for fd=%d\n", client_fd);
        submit_close(r, client_fd);
        return;
    }

    printf("New client connected: fd=%d\n", client_fd);

    if (submit_recv(r, cs) < 0) {
        fprintf(stderr, "submit_recv failed for fd=%d\n", client_fd);
        submit_close(r, client_fd);
        free_client(cs);
        return;
    }
    cs->recv_in_flight = 1;
}

// ── RECV ─────────────────────────────────────────────────────────────────────
static void on_recv(io_uring_t *r, client_state *cs, int result) {
    cs->recv_in_flight = 0;

    if (result <= 0) {
        if (result < 0 && -result != ECONNRESET && -result != EPIPE)
            fprintf(stderr, "recv error fd=%d: %s\n", cs->fd, strerror(-result));
        else
            printf("Client disconnected: fd=%d\n", cs->fd);
        int fd = cs->fd;
        free_client(cs);          // Fix 1: free heap state
        submit_close(r, fd);
        return;
    }

    size_t bytes = (size_t)result;
    if (cs->out_len + bytes > OUT_BUF_CAP) {
        fprintf(stderr, "Output buffer overflow fd=%d, closing\n", cs->fd);
        int fd = cs->fd;
        free_client(cs);
        submit_close(r, fd);
        return;
    }

    memcpy(cs->out_buf + cs->out_len, cs->in_buf, bytes);
    cs->out_len += bytes;

    if (!cs->send_in_flight) {
        if (submit_send(r, cs) < 0) {
            int fd = cs->fd;
            free_client(cs);
            submit_close(r, fd);
            return;
        }
        cs->send_in_flight = 1;
    }
    // If a send is already in flight, on_send() will re-arm the recv.
}

// ── SEND ─────────────────────────────────────────────────────────────────────
static void on_send(io_uring_t *r, client_state *cs, int result) {
    cs->send_in_flight = 0;

    if (result < 0) {
        if (-result != EPIPE && -result != ECONNRESET)
            fprintf(stderr, "send error fd=%d: %s\n", cs->fd, strerror(-result));
        int fd = cs->fd;
        free_client(cs);
        submit_close(r, fd);
        return;
    }

    size_t sent = (size_t)result;
    if (sent > cs->out_len) sent = cs->out_len;

    cs->out_len -= sent;
    if (cs->out_len > 0) {
        memmove(cs->out_buf, cs->out_buf + sent, cs->out_len);
        // Still data to drain — re-arm SEND
        if (submit_send(r, cs) < 0) {
            int fd = cs->fd;
            free_client(cs);
            submit_close(r, fd);
        } else {
            cs->send_in_flight = 1;
        }
    } else {
        // Buffer fully drained — re-arm RECV
        if (!cs->recv_in_flight) {
            if (submit_recv(r, cs) < 0) {
                int fd = cs->fd;
                free_client(cs);
                submit_close(r, fd);
                return;
            }
            cs->recv_in_flight = 1;
        }
    }
}

// =============================================================================
//  Completion Queue drain  (Fix 4: flushes mid-loop when SQ is nearly full)
// =============================================================================

static void drain_cq(io_uring_t *r, int server_fd,
                     struct sockaddr_in *addr, socklen_t *addrlen) {
    unsigned head = atomic_load_explicit((_Atomic unsigned *)r->cq_head,
                                          memory_order_acquire);
    unsigned tail = atomic_load_explicit((_Atomic unsigned *)r->cq_tail,
                                          memory_order_acquire);

    while (head != tail) {
        struct io_uring_cqe *cqe = &r->cqes[head & (*r->cq_ring_mask)];

        uint64_t     ud  = cqe->user_data;
        int          res = cqe->res;
        uint32_t     op  = ud_op(ud);
        client_state *cs = ud_ptr(ud);

        // Advance head BEFORE dispatching so the kernel can reuse the slot.
        // Fix 4: also check ring pressure after every handler call.
        head++;
        atomic_store_explicit((_Atomic unsigned *)r->cq_head,
                              head, memory_order_release);

        switch (op) {
            case OP_ACCEPT:
                on_accept(r, server_fd, addr, addrlen, res);
                break;
            case OP_RECV:
                on_recv(r, cs, res);
                break;
            case OP_SEND:
                on_send(r, cs, res);
                break;
            case OP_CLOSE:
                // Nothing to do — fd was closed by the kernel.
                // cs is NULL for CLOSE SQEs we submitted after free_client().
                break;
            default:
                fprintf(stderr, "Unknown op=%u in CQE\n", op);
                break;
        }

        // Fix 4: flush SQ mid-loop if it's getting full so we don't stall
        if (sq_space_left(r) < 4) {
            ring_submit(r, 0);
            // Refresh CQ tail — new completions may have arrived
            tail = atomic_load_explicit((_Atomic unsigned *)r->cq_tail,
                                         memory_order_acquire);
        }
    }
}

// =============================================================================
//  main
// =============================================================================

int main(int argc, char *argv[]) {

    int port    = (argc > 1) ? atoi(argv[1]) : 8080;
    int backlog = (argc > 2) ? atoi(argv[2]) : SOMAXCONN;

    // Create and bind the listening socket (reuses shared network_utils helper)
    int server_fd = create_server_socket(port, backlog);
    if (server_fd < 0) return 1;

    // Initialise the io_uring ring
    io_uring_t ring;
    memset(&ring, 0, sizeof(ring));
    if (ring_init(&ring, QUEUE_DEPTH) < 0) {
        close(server_fd);
        return 1;
    }

    // Prime the first ACCEPT SQE — one always lives in the ring permanently;
    // each completion re-arms it immediately.
    struct sockaddr_in client_addr;
    socklen_t client_addrlen = sizeof(client_addr);
    memset(&client_addr, 0, sizeof(client_addr));

    if (submit_accept(&ring, server_fd, &client_addr, &client_addrlen) < 0) {
        fprintf(stderr, "Initial submit_accept failed\n");
        close(server_fd);
        close(ring.ring_fd);
        return 1;
    }

    if (ring_submit(&ring, 0) < 0) {
        close(server_fd);
        close(ring.ring_fd);
        return 1;
    }

    printf("io_uring echo server listening on port %d "
           "(queue_depth=%d, max_clients=%d, server_fd=%d, ring_fd=%d)\n",
           port, QUEUE_DEPTH, MAX_CLIENTS, server_fd, ring.ring_fd);

    // =========================================================================
    //  Main event loop
    //  1. Block until ≥1 CQE is ready (and simultaneously submits any SQEs)
    //  2. Drain all ready CQEs — each one may enqueue more SQEs
    //  3. Flush any freshly enqueued SQEs (non-blocking)
    // =========================================================================
    while (1) {
        if (ring_submit(&ring, 1) < 0) {
            if (errno == EINTR) continue;
            perror("ring_submit");
            break;
        }

        drain_cq(&ring, server_fd, &client_addr, &client_addrlen);

        // Non-blocking flush for SQEs enqueued during drain_cq
        ring_submit(&ring, 0);
    }

    close(server_fd);
    close(ring.ring_fd);
    return 0;
}
