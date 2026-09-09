// =============================================================================
//  server_iouring.c  —  TCP Echo Server using io_uring
//  (Idiomatic version — handicaps removed, MAX_CLIENTS actually enforced)
// =============================================================================
//
//  Changes vs. the "fair benchmarking" build:
//
//  Removed Fix 2 (manual fcntl syscalls)
//    accept_flags now sets SOCK_NONBLOCK directly in the ACCEPT SQE, so the
//    kernel hands back an already-nonblocking fd — no fcntl(F_GETFL)/
//    fcntl(F_SETFL) round trip per connection. This is the whole point of
//    io_uring: skip the syscall, not re-add it.
//
//  Fixed Fix 3 (MAX_CLIENTS was declared but never enforced)
//    Added an active_clients counter, incremented in on_accept() and
//    decremented wherever a client is freed, so the server actually rejects
//    connections past MAX_CLIENTS instead of accepting unbounded clients
//    while claiming a 10,000 cap.
//
//  Fix 1 (heap alloc per connection) and registered buffers / multishot
//  accept-recv are NOT changed here — those are larger structural rewrites.
//  If you want the fully idiomatic version (registered buffers, multishot
//  ACCEPT/RECV, no per-connection malloc), say so and I'll do that pass too.
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
#include <linux/io_uring.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <stdatomic.h>
#include <stdint.h>
#include <signal.h>

// =============================================================================
//  Tunables
// =============================================================================

#define QUEUE_DEPTH   16384    // SQ/CQ ring depth (power-of-2)
#define MAX_CLIENTS   10000    // now actually enforced — see active_clients below
#define OUT_BUF_CAP   65536    // Per-client output buffer (64 KB)

// =============================================================================
//  Operation type tags  (packed into low 4 bits of user_data)
// =============================================================================

#define OP_ACCEPT  1
#define OP_RECV    2
#define OP_SEND    3
#define OP_CLOSE   4

// =============================================================================
//  Per-client state
// =============================================================================

typedef struct {
    int    fd;
    char   in_buf[BUFFER_SIZE];
    char   out_buf[OUT_BUF_CAP];
    size_t out_len;
    int    send_in_flight;
    int    recv_in_flight;
} client_state;

// Live connection count — enforces MAX_CLIENTS (previously declared, never checked)
static int active_clients = 0;

static client_state *alloc_client(int fd) {
    client_state *cs = malloc(sizeof(client_state));
    if (!cs) return NULL;
    cs->fd             = fd;
    cs->out_len        = 0;
    cs->send_in_flight = 0;
    cs->recv_in_flight = 0;
    active_clients++;
    return cs;
}

static void free_client(client_state *cs) {
    if (!cs) return;
    active_clients--;
    free(cs);
}

// =============================================================================
//  user_data helpers
// =============================================================================

static inline uint64_t make_ud(client_state *cs, uint32_t op) {
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

    unsigned               *sq_head;
    unsigned               *sq_tail;
    unsigned               *sq_ring_mask;
    unsigned               *sq_array;
    struct io_uring_sqe    *sqes;

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

    size_t sq_ring_sz = params.sq_off.array + params.sq_entries * sizeof(unsigned);
    void *sq_ring = mmap(NULL, sq_ring_sz, PROT_READ|PROT_WRITE,
                         MAP_SHARED|MAP_POPULATE, r->ring_fd, IORING_OFF_SQ_RING);
    if (sq_ring == MAP_FAILED) { perror("mmap sq_ring"); return -1; }

    r->sq_head      = (unsigned *)((char *)sq_ring + params.sq_off.head);
    r->sq_tail      = (unsigned *)((char *)sq_ring + params.sq_off.tail);
    r->sq_ring_mask = (unsigned *)((char *)sq_ring + params.sq_off.ring_mask);
    r->sq_array     = (unsigned *)((char *)sq_ring + params.sq_off.array);

    size_t sqes_sz = params.sq_entries * sizeof(struct io_uring_sqe);
    r->sqes = mmap(NULL, sqes_sz, PROT_READ|PROT_WRITE,
                   MAP_SHARED|MAP_POPULATE, r->ring_fd, IORING_OFF_SQES);
    if (r->sqes == MAP_FAILED) { perror("mmap sqes"); return -1; }

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

static inline unsigned sq_space_left(io_uring_t *r) {
    unsigned head = atomic_load_explicit((_Atomic unsigned *)r->sq_head,
                                          memory_order_acquire);
    return r->sq_entries - (*r->sq_tail - head);
}

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
//  SQE submit helpers
// =============================================================================

static void maybe_flush(io_uring_t *r) {
    if (sq_space_left(r) < 4) {
        ring_submit(r, 0);
    }
}

static int submit_accept(io_uring_t *r, int server_fd,
                         struct sockaddr_in *addr, socklen_t *addrlen) {
    maybe_flush(r);
    struct io_uring_sqe *sqe = ring_get_sqe(r);
    if (!sqe) return -1;
    memset(sqe, 0, sizeof(*sqe));
    sqe->opcode       = IORING_OP_ACCEPT;
    sqe->fd           = server_fd;
    sqe->addr         = (uint64_t)(uintptr_t)addr;
    sqe->addr2        = (uint64_t)(uintptr_t)addrlen;
    // Idiomatic: ask the kernel for an already-nonblocking fd directly —
    // no fcntl() round trip needed after accept completes.
    sqe->accept_flags = SOCK_NONBLOCK;
    sqe->user_data    = make_ud(NULL, OP_ACCEPT);
    ring_submit_advance(r);
    return 0;
}

static int submit_recv(io_uring_t *r, client_state *cs) {
    maybe_flush(r);
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
    maybe_flush(r);
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
    maybe_flush(r);
    struct io_uring_sqe *sqe = ring_get_sqe(r);
    if (!sqe) { close(fd); return 0; }
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

    // MAX_CLIENTS is now actually enforced — previously this check did not
    // exist anywhere, so the server accepted unlimited connections while
    // claiming a 10,000 cap in the startup banner.
    if (active_clients >= MAX_CLIENTS) {
        fprintf(stderr, "MAX_CLIENTS (%d) reached, rejecting fd=%d\n",
                MAX_CLIENTS, client_fd);
        submit_close(r, client_fd);
        return;
    }

    // fd is already non-blocking (SOCK_NONBLOCK set in the ACCEPT SQE above) —
    // no fcntl() calls needed.
    set_tcp_nodelay(client_fd);

    client_state *cs = alloc_client(client_fd);
    if (!cs) {
        fprintf(stderr, "malloc failed for fd=%d\n", client_fd);
        submit_close(r, client_fd);
        return;
    }

    printf("New client connected: fd=%d (active=%d)\n", client_fd, active_clients);

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
        free_client(cs);
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
        if (submit_send(r, cs) < 0) {
            int fd = cs->fd;
            free_client(cs);
            submit_close(r, fd);
        } else {
            cs->send_in_flight = 1;
        }
    } else {
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
//  Completion Queue drain
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
                break;
            default:
                fprintf(stderr, "Unknown op=%u in CQE\n", op);
                break;
        }

        if (sq_space_left(r) < 4) {
            ring_submit(r, 0);
            tail = atomic_load_explicit((_Atomic unsigned *)r->cq_tail,
                                         memory_order_acquire);
        }
    }
}

// =============================================================================
//  main
// =============================================================================

int main(int argc, char *argv[]) {
    signal(SIGPIPE, SIG_IGN);
    int port    = (argc > 1) ? atoi(argv[1]) : 8080;
    int backlog = (argc > 2) ? atoi(argv[2]) : SOMAXCONN;

    int server_fd = create_server_socket(port, backlog);
    if (server_fd < 0) return 1;

    io_uring_t ring;
    memset(&ring, 0, sizeof(ring));
    if (ring_init(&ring, QUEUE_DEPTH) < 0) {
        close(server_fd);
        return 1;
    }

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

    while (1) {
        if (ring_submit(&ring, 1) < 0) {
            if (errno == EINTR) continue;
            perror("ring_submit");
            break;
        }

        drain_cq(&ring, server_fd, &client_addr, &client_addrlen);

        ring_submit(&ring, 0);
    }

    close(server_fd);
    close(ring.ring_fd);
    return 0;
}