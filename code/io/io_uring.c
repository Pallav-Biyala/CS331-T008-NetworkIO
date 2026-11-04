/*
 * Assumptions and limits:
 * - Section 14 leaves the kernel version as a [SETUP] value to collect on
 *   the benchmark machines; no version is specified in the supplied plan.
 *   This baseline uses plain ACCEPT, RECV and SEND and makes no version-based
 *   assumptions about optional io_uring features.
 * - Only the plain baseline operations are implemented here. Every optional
 *   io_uring flag declared in core_config_t is rejected by io_init() with
 *   -ENOTSUP rather than being silently ignored. Ring entry count and CQ
 *   entry count are honored. Optional feature implementations require the
 *   target kernel version and flag scope to be resolved first.
 * - The baseline owns one core pool buffer per accepted connection. It is
 *   allocated on the first buffer request/read and held until the connection
 *   has no operation in flight and is closed. It backs RECV, file reads, and
 *   app writes obtained through io_get_buf().
 * - Plain SEND uses the caller's buffer unchanged until its completion. A
 *   positive short SEND is continued with another SEND before on_write_done.
 *   Reads complete on each positive RECV, including short reads; framing and
 *   carry handling remain the app's responsibility.
 * - Close waits for that connection's outstanding request to complete before
 *   closing its fd, returning its buffer, calling on_close, and freeing it.
 *   Generation-tagged user_data rejects stale completions.
 * - io_read_file uses synchronous pread() for the baseline and defers its
 *   callback until a later io_run() pass. --uring-async-file-read is rejected.
 *   io_send_file is available only with --uring-splice, which is also rejected
 *   here until its pipe lifecycle can be implemented and reviewed.
 * - The fixed core.h contains a nested comment that triggers GCC -Wcomment;
 *   suppress that warning only while including io.h.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcomment"
#include "io.h"
#pragma GCC diagnostic pop

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <liburing.h>

typedef enum {
    URING_OP_ACCEPT = 1,
    URING_OP_RECV,
    URING_OP_SEND,
    URING_OP_CANCEL
} uring_op_t;

typedef enum {
    DEFER_NONE = 0,
    DEFER_FILE,
    DEFER_CLOSE
} deferred_kind_t;

typedef struct {
    uint8_t *buffer;
    size_t read_max;
    size_t send_len;
    size_t send_off;
    const uint8_t *send_buf;
    deferred_kind_t deferred;
    io_close_reason_t close_reason;
    uint8_t *file_buf;
    ssize_t file_result;
    unsigned in_flight;
    bool write_more;
    uint64_t pending_tag;
    bool cancel_queued;
} uring_conn_t;

static struct io_uring g_ring;
static int g_listener = -1;
static conn_t **g_connections;
static bool g_accept_in_flight;
static unsigned g_last_overflow_count;
static bool g_initialized;

static uring_conn_t *uring_priv(conn_t *c)
{
    return (uring_conn_t *)c->io_priv;
}

static uint64_t make_tag(uint32_t id, uint32_t gen, uring_op_t op)
{
    return ((uint64_t)gen << 32) | ((uint64_t)(id & 0x00ffffffU) << 8) |
           (uint64_t)(unsigned)op;
}

static uring_op_t tag_op(uint64_t tag)
{
    return (uring_op_t)(tag & 0xffU);
}

static uint32_t tag_id(uint64_t tag)
{
    return (uint32_t)((tag >> 8) & 0x00ffffffU);
}

static uint32_t tag_gen(uint64_t tag)
{
    return (uint32_t)(tag >> 32);
}

static void account_ring_overflows(void)
{
    unsigned current = *g_ring.cq.koverflow;
    unsigned delta = current - g_last_overflow_count;
    if (delta != 0)
        CORE_STAT_ADD(ring_overflows, delta);
    g_last_overflow_count = current;
}

static int reject_optional_flags(const core_config_t *cfg)
{
    const core_uring_flags_t *f = &cfg->uring;
    if (f->submit_batch != 0 || f->fixed_files || f->provided_buffers ||
        f->multishot_accept || f->multishot_recv || f->fixed_buffers ||
        f->single_issuer || f->defer_taskrun || f->coop_taskrun ||
        f->register_ring_fd || f->poll_first || f->send_zc || f->sqpoll ||
        f->async_file_read || f->splice)
        return -ENOTSUP;
    return 0;
}

static int alloc_conn_buffer(conn_t *c)
{
    uring_conn_t *p = uring_priv(c);
    if (p->buffer != NULL)
        return 0;
    p->buffer = core_slot_alloc();
    return p->buffer != NULL ? 0 : -ENOMEM;
}

static struct io_uring_sqe *get_sqe(void)
{
    struct io_uring_sqe *sqe = io_uring_get_sqe(&g_ring);
    if (sqe != NULL)
        return sqe;
    if (io_uring_submit(&g_ring) < 0)
        return NULL;
    return io_uring_get_sqe(&g_ring);
}

static void close_with_errno(conn_t *c, int err)
{
    uring_conn_t *p = uring_priv(c);
    if (err == ECONNRESET || err == EPIPE) {
        CORE_STAT_INC(peer_resets);
        p->close_reason = IO_CLOSE_RESET;
    } else {
        CORE_STAT_INC(errors);
        p->close_reason = IO_CLOSE_ERROR;
    }
    p->deferred = DEFER_CLOSE;
    c->state = CONN_CLOSING;
}

static void defer_close(conn_t *c, io_close_reason_t reason)
{
    uring_conn_t *p = uring_priv(c);
    if (p->deferred == DEFER_CLOSE)
        return;
    p->deferred = DEFER_CLOSE;
    p->close_reason = reason;
    c->state = CONN_CLOSING;
}

static int queue_accept(void)
{
    struct io_uring_sqe *sqe = get_sqe();
    if (sqe == NULL)
        return -EIO;
    io_uring_prep_accept(sqe, g_listener, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    io_uring_sqe_set_data64(sqe, make_tag(0, 0, URING_OP_ACCEPT));
    g_accept_in_flight = true;
    return 0;
}

static int queue_recv(conn_t *c)
{
    uring_conn_t *p = uring_priv(c);
    struct io_uring_sqe *sqe;
    int rc = alloc_conn_buffer(c);
    if (rc != 0)
        return rc;
    sqe = get_sqe();
    if (sqe == NULL)
        return -EIO;
    io_uring_prep_recv(sqe, c->fd, p->buffer, (unsigned)p->read_max, 0);
    p->pending_tag = make_tag(c->id, c->gen, URING_OP_RECV);
    io_uring_sqe_set_data64(sqe, p->pending_tag);
    p->in_flight++;
    c->state = CONN_READING;
    return 0;
}

static int queue_send(conn_t *c)
{
    uring_conn_t *p = uring_priv(c);
    struct io_uring_sqe *sqe = get_sqe();
    if (sqe == NULL)
        return -EIO;
    io_uring_prep_send(sqe, c->fd, p->send_buf + p->send_off,
                       (unsigned)(p->send_len - p->send_off),
                       p->write_more ? MSG_MORE : 0);
    p->pending_tag = make_tag(c->id, c->gen, URING_OP_SEND);
    io_uring_sqe_set_data64(sqe, p->pending_tag);
    p->in_flight++;
    c->state = CONN_WRITING;
    return 0;
}

static void finish_close(conn_t *c, io_close_reason_t reason)
{
    uring_conn_t *p = uring_priv(c);
    int fd = c->fd;

    if (p->in_flight != 0)
        return;
    if (fd >= 0)
        (void)close(fd);
    if (c->id < g_cfg->max_conns && g_connections[c->id] == c)
        g_connections[c->id] = NULL;
    if (p->buffer != NULL) {
        core_slot_free(p->buffer);
        p->buffer = NULL;
    }
    c->fd = -1;
    p->deferred = DEFER_NONE;
    CORE_STAT_INC(closes);
    on_close(c, reason);
    core_conn_free(c);
}

static bool has_deferred(void)
{
    uint32_t i;
    for (i = 0; i < g_cfg->max_conns; i++) {
        conn_t *c = g_connections[i];
        if (c != NULL && uring_priv(c)->deferred != DEFER_NONE &&
            uring_priv(c)->in_flight == 0)
            return true;
    }
    return false;
}

static void deliver_deferred(void)
{
    uint32_t i;
    for (i = 0; i < g_cfg->max_conns; i++) {
        conn_t *c = g_connections[i];
        uring_conn_t *p;
        deferred_kind_t kind;
        if (c == NULL)
            continue;
        p = uring_priv(c);
        if (p->deferred == DEFER_NONE || p->in_flight != 0)
            continue;
        kind = p->deferred;
        if (kind == DEFER_CLOSE) {
            finish_close(c, p->close_reason);
        } else if (kind == DEFER_FILE) {
            uint8_t *buf = p->file_buf;
            ssize_t result = p->file_result;
            p->deferred = DEFER_NONE;
            p->file_buf = NULL;
            CORE_STAT_INC(file_reads);
            on_file_read_done(c, buf, result);
        }
    }
}

static void handle_accept(struct io_uring_cqe *cqe)
{
    int fd = cqe->res;
    g_accept_in_flight = false;
    if (g_shutdown) {
        if (fd >= 0) {
            (void)close(fd);
            CORE_STAT_INC(closes);
        }
        return;
    }
    if (fd < 0) {
        int err = -fd;
        if (err != EINTR && err != EAGAIN && err != EWOULDBLOCK) {
            if (err == ECONNRESET)
                CORE_STAT_INC(peer_resets);
            else
                CORE_STAT_INC(errors);
        } else if (err == EAGAIN || err == EWOULDBLOCK) {
            CORE_STAT_INC(eagain);
        }
        if (!g_shutdown && queue_accept() != 0)
            CORE_STAT_INC(errors);
        return;
    }
    if (core_setup_accepted(fd, g_cfg) < 0) {
        (void)close(fd);
        CORE_STAT_INC(errors);
        CORE_STAT_INC(closes);
    } else {
        conn_t *c = core_conn_alloc(fd);
        if (c == NULL) {
            (void)close(fd);
            CORE_STAT_INC(errors);
            CORE_STAT_INC(closes);
        } else {
            g_connections[c->id] = c;
            core_conn_activate(c);
            CORE_STAT_INC(accepts);
            on_accept(c);
        }
    }
    if (!g_shutdown && queue_accept() != 0)
        CORE_STAT_INC(errors);
}

static void handle_conn_cqe(struct io_uring_cqe *cqe)
{
    uint64_t tag = io_uring_cqe_get_data64(cqe);
    uring_op_t op = tag_op(tag);
    conn_t *c = core_conn_get(tag_id(tag), tag_gen(tag));
    uring_conn_t *p;
    int result = cqe->res;

    if (c == NULL)
        return;
    p = uring_priv(c);
    if (op == URING_OP_CANCEL)
        return;
    if (p->in_flight > 0)
        p->in_flight--;
    p->pending_tag = 0;
    p->cancel_queued = false;
    if (c->state == CONN_CLOSING || p->deferred == DEFER_CLOSE) {
        if (op == URING_OP_RECV)
            CORE_STAT_INC(reads);
        else if (op == URING_OP_SEND)
            CORE_STAT_INC(writes);
        return;
    }
    if (op == URING_OP_RECV)
        CORE_STAT_INC(reads);
    else if (op == URING_OP_SEND)
        CORE_STAT_INC(writes);
    if (result < 0) {
        int err = -result;
        if (err == EAGAIN || err == EWOULDBLOCK) {
            CORE_STAT_INC(eagain);
            if (op == URING_OP_RECV) {
                if (queue_recv(c) != 0)
                    close_with_errno(c, EIO);
            } else if (op == URING_OP_SEND) {
                if (queue_send(c) != 0)
                    close_with_errno(c, EIO);
            }
        } else if (err == EINTR) {
            if ((op == URING_OP_RECV && queue_recv(c) != 0) ||
                (op == URING_OP_SEND && queue_send(c) != 0))
                close_with_errno(c, EIO);
        } else {
            close_with_errno(c, err);
        }
        return;
    }
    if (op == URING_OP_RECV) {
        if (result == 0) {
            defer_close(c, IO_CLOSE_APP);
            return;
        }
        CORE_STAT_ADD(bytes_in, result);
        if ((size_t)result < p->read_max)
            CORE_STAT_INC(partial_reads);
        c->state = CONN_ACCEPTED;
        on_read_done(c, p->buffer, result);
    } else if (op == URING_OP_SEND) {
        size_t remaining = p->send_len - p->send_off;
        if (result == 0 && remaining != 0) {
            close_with_errno(c, EPIPE);
            return;
        }
        CORE_STAT_ADD(bytes_out, result);
        if ((size_t)result < remaining)
            CORE_STAT_INC(partial_writes);
        p->send_off += (size_t)result;
        if (p->send_off < p->send_len) {
            if (queue_send(c) != 0)
                close_with_errno(c, EIO);
            return;
        }
        p->send_buf = NULL;
        p->send_len = 0;
        p->send_off = 0;
        p->write_more = false;
        c->wr_buf = NULL;
        c->wr_len = 0;
        c->wr_off = 0;
        c->state = CONN_ACCEPTED;
        on_write_done(c);
    }
}

const char *io_name(void)
{
    return "uring";
}

size_t io_priv_size(void)
{
    return sizeof(uring_conn_t);
}

int io_init(const core_config_t *cfg)
{
    struct io_uring_params params;
    uint32_t entries;
    int rc;

    if (cfg == NULL || cfg->max_conns == 0 || cfg->buf_size == 0 ||
        cfg->max_conns > 0x00ffffffU)
        return -EINVAL;
    rc = reject_optional_flags(cfg);
    if (rc != 0)
        return rc;
    entries = cfg->uring.entries;
    if (entries == 0) {
        entries = 1;
        while (entries < cfg->max_conns && entries <= UINT32_MAX / 2U)
            entries <<= 1;
    }
    if (entries == 0 || (cfg->uring.cq_entries != 0 &&
                         cfg->uring.cq_entries < entries))
        return -EINVAL;
    memset(&params, 0, sizeof(params));
    if (cfg->uring.cq_entries != 0) {
        params.cq_entries = cfg->uring.cq_entries;
        params.flags |= IORING_SETUP_CQSIZE;
    }
    rc = io_uring_queue_init_params(entries, &g_ring, &params);
    if (rc < 0)
        return rc;
    g_connections = calloc(cfg->max_conns, sizeof(*g_connections));
    if (g_connections == NULL) {
        io_uring_queue_exit(&g_ring);
        return -ENOMEM;
    }
    g_listener = -1;
    g_accept_in_flight = false;
    g_last_overflow_count = *g_ring.cq.koverflow;
    g_initialized = true;
    return 0;
}

int io_listen(const core_config_t *cfg)
{
    int fd;
    int rc;
    if (!g_initialized || cfg == NULL)
        return -EINVAL;
    fd = core_make_listener(cfg);
    if (fd < 0)
        return -errno;
    g_listener = fd;
    rc = queue_accept();
    if (rc != 0) {
        (void)close(fd);
        g_listener = -1;
        return rc;
    }
    return 0;
}

int io_read(conn_t *c, size_t max)
{
    uring_conn_t *p;
    int rc;
    if (c == NULL || max == 0 || max > g_cfg->buf_size || max > UINT_MAX)
        return -EINVAL;
    p = uring_priv(c);
    if (c->state == CONN_READING || c->state == CONN_WRITING ||
        p->in_flight != 0 || p->deferred != DEFER_NONE)
        return -EBUSY;
    p->read_max = max;
    c->read_max = max;
    rc = queue_recv(c);
    if (rc != 0)
        close_with_errno(c, -rc);
    return 0;
}

uint8_t *io_get_buf(conn_t *c, size_t *cap)
{
    if (c == NULL || cap == NULL || !g_initialized)
        return NULL;
    if (alloc_conn_buffer(c) != 0)
        return NULL;
    *cap = g_cfg->buf_size;
    return uring_priv(c)->buffer;
}

static int start_write(conn_t *c, const uint8_t *buf, size_t len, bool more)
{
    uring_conn_t *p;
    if (c == NULL || buf == NULL || len == 0 || len > UINT_MAX)
        return -EINVAL;
    p = uring_priv(c);
    if (c->state == CONN_READING || c->state == CONN_WRITING ||
        p->in_flight != 0 || p->deferred != DEFER_NONE)
        return -EBUSY;
    p->send_buf = buf;
    p->send_len = len;
    p->send_off = 0;
    p->write_more = more;
    c->wr_buf = buf;
    c->wr_len = len;
    c->wr_off = 0;
    if (queue_send(c) != 0) {
        p->send_buf = NULL;
        c->wr_buf = NULL;
        c->wr_len = 0;
        close_with_errno(c, EIO);
    }
    return 0;
}

int io_write(conn_t *c, const uint8_t *buf, size_t len)
{
    return start_write(c, buf, len, false);
}

int io_write_more(conn_t *c, const uint8_t *buf, size_t len)
{
    return start_write(c, buf, len, true);
}

int io_read_file(conn_t *c, uint8_t *buf, size_t len, int fd, off_t off)
{
    uring_conn_t *p;
    ssize_t n;
    if (c == NULL || buf == NULL || len == 0 || len > g_cfg->buf_size ||
        fd < 0 || off < 0)
        return -EINVAL;
    p = uring_priv(c);
    if (p->in_flight != 0 || p->deferred != DEFER_NONE ||
        c->state == CONN_READING || c->state == CONN_WRITING)
        return -EBUSY;
    do {
        n = pread(fd, buf, len, off);
    } while (n < 0 && errno == EINTR);
    if (n < 0) {
        close_with_errno(c, errno);
        return 0;
    }
    p->file_buf = buf;
    p->file_result = n;
    p->deferred = DEFER_FILE;
    return 0;
}

int io_send_file(conn_t *c, int fd, off_t off, size_t len)
{
    if (c == NULL || fd < 0 || off < 0 || len == 0)
        return -EINVAL;
    if (!g_cfg->uring.splice)
        return -ENOTSUP;
    return -ENOTSUP;
}

void io_close(conn_t *c)
{
    if (c != NULL)
        defer_close(c, IO_CLOSE_APP);
}

int io_run(void)
{
    while (!g_shutdown) {
        struct io_uring_cqe *cqe;
        unsigned head;
        unsigned count = 0;
        int rc;

        deliver_deferred();
        if (g_dump_stats) {
            g_dump_stats = 0;
            core_stats_dump();
        }
        CORE_STAT_INC(loop_iterations);
        CORE_STAT_INC(wait_calls);
        rc = io_uring_submit_and_wait(&g_ring, 1);
        if (rc < 0 && rc != -EINTR) {
            CORE_STAT_INC(errors);
            continue;
        }
        io_uring_for_each_cqe(&g_ring, head, cqe) {
            if (tag_op(io_uring_cqe_get_data64(cqe)) == URING_OP_ACCEPT)
                handle_accept(cqe);
            else
                handle_conn_cqe(cqe);
            count++;
        }
        if (count != 0)
            io_uring_cq_advance(&g_ring, count);
        account_ring_overflows();
        if (has_deferred())
            continue;
    }
    /* Cancel outstanding work and retain buffers until original CQEs arrive. */
    if (g_accept_in_flight) {
        struct io_uring_sqe *sqe = get_sqe();
        if (sqe != NULL) {
            io_uring_prep_cancel64(sqe, make_tag(0, 0, URING_OP_ACCEPT), 0);
            io_uring_sqe_set_data64(sqe, make_tag(0, 0, URING_OP_CANCEL));
        }
    }
    for (uint32_t i = 0; i < g_cfg->max_conns; i++) {
        conn_t *c = g_connections[i];
        if (c != NULL) {
            uring_conn_t *p = uring_priv(c);
            defer_close(c, IO_CLOSE_APP);
            if (p->in_flight != 0 && !p->cancel_queued) {
                struct io_uring_sqe *sqe = get_sqe();
                if (sqe != NULL) {
                    io_uring_prep_cancel64(sqe, p->pending_tag, 0);
                    io_uring_sqe_set_data64(sqe,
                        make_tag(c->id, c->gen, URING_OP_CANCEL));
                    p->cancel_queued = true;
                }
            }
        }
    }
    /* There must be no in-flight request before connection memory is released. */
    for (;;) {
        bool pending = g_accept_in_flight;
        uint32_t i;
        struct io_uring_cqe *cqe;
        unsigned head;
        unsigned count = 0;
        for (i = 0; i < g_cfg->max_conns; i++) {
            conn_t *c = g_connections[i];
            if (c != NULL && uring_priv(c)->in_flight != 0)
                pending = true;
        }
        if (!pending)
            break;
        CORE_STAT_INC(wait_calls);
        if (io_uring_submit_and_wait(&g_ring, 1) < 0)
            continue;
        io_uring_for_each_cqe(&g_ring, head, cqe) {
            if (tag_op(io_uring_cqe_get_data64(cqe)) == URING_OP_ACCEPT)
                handle_accept(cqe);
            else if (tag_op(io_uring_cqe_get_data64(cqe)) == URING_OP_CANCEL) {
                count++;
                continue;
            } else
                handle_conn_cqe(cqe);
            count++;
        }
        if (count != 0)
            io_uring_cq_advance(&g_ring, count);
    }
    deliver_deferred();
    account_ring_overflows();
    for (uint32_t i = 0; i < g_cfg->max_conns; i++) {
        conn_t *c = g_connections[i];
        if (c != NULL)
            finish_close(c, IO_CLOSE_APP);
    }
    return 0;
}

void io_shutdown(void)
{
    if (g_listener >= 0) {
        (void)close(g_listener);
        g_listener = -1;
    }
    if (g_initialized)
        io_uring_queue_exit(&g_ring);
    free(g_connections);
    g_connections = NULL;
    g_accept_in_flight = false;
    g_last_overflow_count = 0;
    g_initialized = false;
}
