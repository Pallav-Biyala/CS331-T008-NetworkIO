/*
 * Assumptions and limits:
 * - The task lists plan sections 4, 5.1, 5.3, 10.1 and 10.2 for both files;
 *   poll's specific rules are in section 10.3, so this file follows 10.3 and
 *   the poll requirements in io.h as well.
 * - [SETUP] hardware, kernel and network values do not change this module's
 *   behavior; listener and accepted-socket options come from core helpers.
 * - Reads use one module-owned shared scratch buffer. The bytes are valid
 *   only during on_read_done unless passed to io_write; an incomplete write
 *   copies its unsent tail into a core slot before io_write returns.
 * - Readiness reads perform one recv per readiness notification. A short
 *   positive read is a normal completion, not an error.
 * - Immediate writes and pread results are delivered on a later io_run pass;
 *   close requests are deferred so callbacks never run inside io_* calls.
 * - A deferred pread is staged in a core slot and copied to its requested
 *   destination immediately before on_file_read_done, avoiding reuse of the
 *   shared scratch buffer before delivery.
 * - The module is intended for Linux/POSIX C17. This Windows workspace does
 *   not provide the requested Linux gcc build environment, so compilation
 *   against the specified command remains to be confirmed there.
 */
#include "io.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <unistd.h>

typedef enum {
    DEFER_NONE = 0,
    DEFER_WRITE,
    DEFER_FILE,
    DEFER_CLOSE
} deferred_kind_t;

typedef struct {
    size_t poll_index;
    size_t read_max;
    deferred_kind_t deferred;
    uint8_t *read_buf;
    uint8_t *file_slot;
    ssize_t deferred_result;
    io_close_reason_t close_reason;
    bool file_pending;
    bool write_is_file;
    int file_fd;
    off_t file_off;
    size_t file_left;
    bool write_more;
} poll_conn_t;

static int g_listener = -1;
static struct pollfd *g_fds;
static size_t g_nfds;
static size_t g_fd_cap;
static uint8_t *g_scratch;
static bool g_initialized;

static poll_conn_t *poll_priv(conn_t *c)
{
    return (poll_conn_t *)c->io_priv;
}

static bool deferred_pending(void)
{
    size_t i;
    for (i = 1; i < g_nfds; i++) {
        conn_t *c = core_conn_by_fd(g_fds[i].fd);
        if (c != NULL && poll_priv(c)->deferred != DEFER_NONE)
            return true;
    }
    return false;
}

static void defer_close(conn_t *c, io_close_reason_t reason)
{
    poll_conn_t *p = poll_priv(c);
    if (p->deferred == DEFER_CLOSE)
        return;
    p->deferred = DEFER_CLOSE;
    p->close_reason = reason;
    c->state = CONN_CLOSING;
}

static void finish_close(conn_t *c, io_close_reason_t reason)
{
    poll_conn_t *p = poll_priv(c);
    size_t i = p->poll_index;
    int fd = c->fd;

    if (fd >= 0)
        (void)close(fd);
    CORE_STAT_INC(closes);
    if (i > 0 && i < g_nfds && g_fds[i].fd == fd) {
        size_t last = --g_nfds;
        if (i != last) {
            conn_t *moved;
            g_fds[i] = g_fds[last];
            moved = core_conn_by_fd(g_fds[i].fd);
            if (moved != NULL)
                poll_priv(moved)->poll_index = i;
        }
        g_fds[last].fd = -1;
        g_fds[last].events = 0;
        g_fds[last].revents = 0;
    }
    p->deferred = DEFER_NONE;
    p->file_pending = false;
    p->write_is_file = false;
    if (p->file_slot != NULL) {
        core_slot_free(p->file_slot);
        p->file_slot = NULL;
    }
    if (c->out_slot != NULL) {
        core_slot_free(c->out_slot);
        c->out_slot = NULL;
    }
    c->fd = -1;
    on_close(c, reason);
    core_conn_free(c);
}

static void close_with_errno(conn_t *c, int err)
{
    io_close_reason_t reason;
    if (err == ECONNRESET || err == EPIPE) {
        CORE_STAT_INC(peer_resets);
        reason = IO_CLOSE_RESET;
    } else {
        CORE_STAT_INC(errors);
        reason = IO_CLOSE_ERROR;
    }
    defer_close(c, reason);
}

static int add_connection(conn_t *c)
{
    poll_conn_t *p = poll_priv(c);
    if (g_nfds >= g_fd_cap)
        return -1;
    p->poll_index = g_nfds;
    g_fds[g_nfds].fd = c->fd;
    g_fds[g_nfds].events = 0;
    g_fds[g_nfds].revents = 0;
    g_nfds++;
    return 0;
}

static void accept_ready(void)
{
    uint32_t accepted = 0;
    uint32_t limit = g_cfg->accept_batch;

    for (;;) {
        int fd;
        conn_t *c;
        int saved_errno;

        if (limit != 0 && accepted >= limit)
            break;
        fd = accept4(g_listener, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            saved_errno = errno;
            if (saved_errno == EINTR)
                continue;
            if (saved_errno == EAGAIN || saved_errno == EWOULDBLOCK) {
                CORE_STAT_INC(eagain);
                break;
            }
            CORE_STAT_INC(errors);
            break;
        }
        accepted++;
        if (core_setup_accepted(fd, g_cfg) < 0) {
            (void)close(fd);
            CORE_STAT_INC(errors);
            CORE_STAT_INC(closes);
            continue;
        }
        c = core_conn_alloc(fd);
        if (c == NULL) {
            (void)close(fd);
            CORE_STAT_INC(errors);
            CORE_STAT_INC(closes);
            continue;
        }
        if (add_connection(c) < 0) {
            /* Capacity is max_conns + listener; this is defensive only. */
            (void)close(fd);
            CORE_STAT_INC(errors);
            CORE_STAT_INC(closes);
            c->fd = -1;
            core_conn_free(c);
            continue;
        }
        core_conn_activate(c);
        CORE_STAT_INC(accepts);
        on_accept(c);
    }
}

static void deliver_deferred(void)
{
    size_t i = 1;
    while (i < g_nfds) {
        conn_t *c = core_conn_by_fd(g_fds[i].fd);
        poll_conn_t *p;
        deferred_kind_t kind;
        ssize_t result;
        if (c == NULL) {
            i++;
            continue;
        }
        p = poll_priv(c);
        kind = p->deferred;
        result = p->deferred_result;
        if (kind == DEFER_NONE) {
            i++;
            continue;
        }
        p->deferred = DEFER_NONE;
        if (kind == DEFER_CLOSE) {
            finish_close(c, p->close_reason);
            continue;
        }
        if (kind == DEFER_WRITE) {
            c->state = CONN_ACCEPTED;
            on_write_done(c);
        } else if (kind == DEFER_FILE) {
            p->file_pending = false;
            if (result > 0)
                memcpy(p->read_buf, p->file_slot, (size_t)result);
            core_slot_free(p->file_slot);
            p->file_slot = NULL;
            on_file_read_done(c, p->read_buf, result);
        }
        if (i < g_nfds && core_conn_by_fd(g_fds[i].fd) == c)
            i++;
    }
}

static void handle_readable(conn_t *c)
{
    poll_conn_t *p = poll_priv(c);
    ssize_t n;
    do {
        n = recv(c->fd, g_scratch, p->read_max, 0);
    } while (n < 0 && errno == EINTR);
    CORE_STAT_INC(reads);
    if (n < 0) {
        int err = errno;
        if (err == EAGAIN || err == EWOULDBLOCK) {
            CORE_STAT_INC(eagain);
            return;
        }
        close_with_errno(c, err);
        return;
    }
    if (n > 0) {
        CORE_STAT_ADD(bytes_in, n);
        if ((size_t)n < p->read_max)
            CORE_STAT_INC(partial_reads);
    }
    c->state = CONN_ACCEPTED;
    on_read_done(c, g_scratch, n);
}

static void handle_writable(conn_t *c)
{
    poll_conn_t *p = poll_priv(c);
    ssize_t n;

    if (p->write_is_file) {
        off_t off = p->file_off;
        do {
            n = sendfile(c->fd, p->file_fd, &off, p->file_left);
        } while (n < 0 && errno == EINTR);
        CORE_STAT_INC(writes);
        if (n < 0) {
            int err = errno;
            if (err == EAGAIN || err == EWOULDBLOCK) {
                CORE_STAT_INC(eagain);
                return;
            }
            close_with_errno(c, err);
            return;
        }
        if (n > 0) {
            CORE_STAT_ADD(bytes_out, n);
            if ((size_t)n < p->file_left)
                CORE_STAT_INC(partial_writes);
        }
        p->file_off = off;
        p->file_left -= (size_t)n;
        if (n == 0 && p->file_left != 0) {
            close_with_errno(c, EIO);
            return;
        }
        if (p->file_left != 0)
            return;
    } else {
        size_t remaining = c->wr_len - c->wr_off;
        int flags = p->write_more ? MSG_MORE : 0;
        do {
            n = send(c->fd, c->wr_buf + c->wr_off, remaining, flags);
        } while (n < 0 && errno == EINTR);
        CORE_STAT_INC(writes);
        if (n < 0) {
            int err = errno;
            if (err == EAGAIN || err == EWOULDBLOCK) {
                CORE_STAT_INC(eagain);
                return;
            }
            close_with_errno(c, err);
            return;
        }
        if (n > 0) {
            CORE_STAT_ADD(bytes_out, n);
            if ((size_t)n < remaining)
                CORE_STAT_INC(partial_writes);
        }
        c->wr_off += (size_t)n;
        if (n == 0 && remaining != 0) {
            close_with_errno(c, EPIPE);
            return;
        }
        if (c->wr_off < c->wr_len)
            return;
    }

    if (c->out_slot != NULL) {
        core_slot_free(c->out_slot);
        c->out_slot = NULL;
    }
    p->write_is_file = false;
    p->write_more = false;
    c->wr_buf = NULL;
    c->wr_len = 0;
    c->wr_off = 0;
    p->deferred = DEFER_WRITE;
}

const char *io_name(void)
{
    return "poll";
}

size_t io_priv_size(void)
{
    return sizeof(poll_conn_t);
}

int io_init(const core_config_t *cfg)
{
    if (cfg == NULL || cfg->max_conns == 0 || cfg->buf_size == 0)
        return -EINVAL;
    g_fd_cap = (size_t)cfg->max_conns + 1;
    g_fds = calloc(g_fd_cap, sizeof(*g_fds));
    g_scratch = malloc(cfg->buf_size);
    if (g_fds == NULL || g_scratch == NULL) {
        free(g_fds);
        free(g_scratch);
        g_fds = NULL;
        g_scratch = NULL;
        return -ENOMEM;
    }
    g_nfds = 1;
    g_listener = -1;
    g_fds[0].fd = -1;
    g_initialized = true;
    return 0;
}

int io_listen(const core_config_t *cfg)
{
    int fd;
    if (!g_initialized || cfg == NULL)
        return -EINVAL;
    fd = core_make_listener(cfg);
    if (fd < 0)
        return -errno;
    g_listener = fd;
    g_fds[0].fd = fd;
    g_fds[0].events = POLLIN;
    return 0;
}

int io_read(conn_t *c, size_t max)
{
    poll_conn_t *p;
    if (c == NULL || max == 0 || max > g_cfg->buf_size)
        return -EINVAL;
    p = poll_priv(c);
    if (c->state == CONN_READING || c->state == CONN_WRITING || p->file_pending || p->deferred != DEFER_NONE)
        return -EBUSY;
    p->read_max = max;
    c->read_max = max;
    c->state = CONN_READING;
    return 0;
}

uint8_t *io_get_buf(conn_t *c, size_t *cap)
{
    if (c == NULL || cap == NULL || !g_initialized)
        return NULL;
    *cap = g_cfg->buf_size;
    return g_scratch;
}

static int start_write(conn_t *c, const uint8_t *buf, size_t len, bool more)
{
    poll_conn_t *p;
    ssize_t n;
    if (c == NULL || buf == NULL || len == 0)
        return -EINVAL;
    p = poll_priv(c);
    if (c->state == CONN_READING || c->state == CONN_WRITING || p->file_pending || p->deferred != DEFER_NONE)
        return -EBUSY;
    c->state = CONN_WRITING;
    c->wr_buf = buf;
    c->wr_len = len;
    c->wr_off = 0;
    p->write_more = more;
    do {
        n = send(c->fd, buf, len, more ? MSG_MORE : 0);
    } while (n < 0 && errno == EINTR);
    CORE_STAT_INC(writes);
    if (n < 0) {
        int err = errno;
        if (err != EAGAIN && err != EWOULDBLOCK) {
            close_with_errno(c, err);
            return 0;
        }
        CORE_STAT_INC(eagain);
        n = 0;
    }
    if (n > 0)
        CORE_STAT_ADD(bytes_out, n);
    if (n > 0 && (size_t)n < len)
        CORE_STAT_INC(partial_writes);
    if ((size_t)n == len) {
        c->wr_buf = NULL;
        c->wr_len = 0;
        c->wr_off = 0;
        p->write_more = false;
        p->deferred = DEFER_WRITE;
        return 0;
    }
    c->out_slot = core_slot_alloc();
    if (c->out_slot == NULL || len - (size_t)n > core_slot_size()) {
        if (c->out_slot != NULL) {
            core_slot_free(c->out_slot);
            c->out_slot = NULL;
        }
        close_with_errno(c, ENOBUFS);
        return 0;
    }
    memcpy(c->out_slot, buf + n, len - (size_t)n);
    c->wr_buf = c->out_slot;
    c->wr_len = len - (size_t)n;
    c->wr_off = 0;
    g_fds[p->poll_index].events = POLLOUT;
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
    poll_conn_t *p;
    ssize_t n;
    if (c == NULL || buf == NULL || len == 0 || fd < 0 || off < 0)
        return -EINVAL;
    p = poll_priv(c);
    if (c->state == CONN_READING || c->state == CONN_WRITING || p->file_pending || p->deferred != DEFER_NONE)
        return -EBUSY;
    if (len > core_slot_size())
        return -EINVAL;
    p->file_slot = core_slot_alloc();
    if (p->file_slot == NULL)
        return -ENOMEM;
    do {
        n = pread(fd, p->file_slot, len, off);
    } while (n < 0 && errno == EINTR);
    if (n < 0) {
        core_slot_free(p->file_slot);
        p->file_slot = NULL;
        close_with_errno(c, EIO);
        return 0;
    }
    CORE_STAT_INC(file_reads);
    p->read_buf = buf;
    p->deferred_result = n;
    p->file_pending = true;
    p->deferred = DEFER_FILE;
    return 0;
}

int io_send_file(conn_t *c, int fd, off_t off, size_t len)
{
    poll_conn_t *p;
    ssize_t n;
    off_t next_off = off;
    bool blocked = false;
    if (c == NULL || fd < 0 || off < 0 || len == 0)
        return -EINVAL;
    if (!g_cfg->sendfile)
        return -ENOTSUP;
    p = poll_priv(c);
    if (c->state == CONN_READING || c->state == CONN_WRITING || p->file_pending || p->deferred != DEFER_NONE)
        return -EBUSY;
    c->state = CONN_WRITING;
    p->write_is_file = true;
    p->file_fd = fd;
    p->file_off = off;
    p->file_left = len;
    do {
        n = sendfile(c->fd, fd, &next_off, len);
    } while (n < 0 && errno == EINTR);
    CORE_STAT_INC(writes);
    if (n < 0) {
        int err = errno;
        if (err != EAGAIN && err != EWOULDBLOCK) {
            close_with_errno(c, err);
            return 0;
        }
        CORE_STAT_INC(eagain);
        blocked = true;
        n = 0;
    }
    if (n > 0) {
        CORE_STAT_ADD(bytes_out, n);
        if ((size_t)n < len)
            CORE_STAT_INC(partial_writes);
    }
    p->file_off = next_off;
    p->file_left = len - (size_t)n;
    if (n == 0 && len != 0 && !blocked) {
        close_with_errno(c, EIO);
        return 0;
    }
    if (p->file_left == 0) {
        p->write_is_file = false;
        p->deferred = DEFER_WRITE;
    } else {
        g_fds[p->poll_index].events = POLLOUT;
    }
    return 0;
}

void io_close(conn_t *c)
{
    if (c != NULL)
        defer_close(c, IO_CLOSE_APP);
}

int io_run(void)
{
    while (!g_shutdown) {
        size_t i;
        int result;
        int timeout;

        deliver_deferred();
        if (g_dump_stats) {
            g_dump_stats = 0;
            core_stats_dump();
        }
        CORE_STAT_INC(loop_iterations);
        for (i = 1; i < g_nfds; i++) {
            conn_t *c = core_conn_by_fd(g_fds[i].fd);
            poll_conn_t *p;
            if (c == NULL)
                continue;
            p = poll_priv(c);
            if (c->state == CONN_CLOSING || p->deferred != DEFER_NONE)
                g_fds[i].events = 0;
            else if (c->state == CONN_READING)
                g_fds[i].events = POLLIN;
            else if (c->state == CONN_WRITING)
                g_fds[i].events = POLLOUT;
            else
                g_fds[i].events = 0;
            g_fds[i].revents = 0;
        }
        timeout = deferred_pending() ? 0 : -1;
        CORE_STAT_INC(wait_calls);
        result = poll(g_fds, (nfds_t)g_nfds, timeout);
        if (result < 0 && errno == EINTR && !g_shutdown)
            continue;
        if (result < 0) {
            CORE_STAT_INC(errors);
            continue;
        }
        /* Scan the entire compact array on every poll return. */
        for (i = 0; i < g_nfds; i++) {
            short events = g_fds[i].revents;
            if (events == 0)
                continue;
            if (i == 0) {
                if (events & (POLLIN | POLLERR | POLLHUP))
                    accept_ready();
                if (events & POLLNVAL)
                    CORE_STAT_INC(errors);
                continue;
            }
            {
                conn_t *c = core_conn_by_fd(g_fds[i].fd);
                if (c == NULL || c->state == CONN_CLOSING)
                    continue;
                if (events & POLLNVAL) {
                    close_with_errno(c, EBADF);
                    continue;
                }
                if ((events & POLLERR) != 0) {
                    int err = 0;
                    socklen_t errlen = sizeof(err);
                    if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &errlen) < 0 || err == 0)
                        err = EIO;
                    close_with_errno(c, err);
                    continue;
                }
                if (c->state == CONN_READING && (events & (POLLIN | POLLHUP)) != 0)
                    handle_readable(c);
                else if (c->state == CONN_WRITING && (events & (POLLOUT | POLLHUP)) != 0)
                    handle_writable(c);
            }
        }
        for (i = 1; i < g_nfds;) {
            conn_t *c = core_conn_by_fd(g_fds[i].fd);
            if (c != NULL && poll_priv(c)->deferred == DEFER_CLOSE) {
                finish_close(c, poll_priv(c)->close_reason);
                continue;
            }
            i++;
        }
    }
    while (g_nfds > 1) {
        conn_t *c = core_conn_by_fd(g_fds[g_nfds - 1].fd);
        if (c == NULL) {
            g_nfds--;
            continue;
        }
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
    free(g_fds);
    free(g_scratch);
    g_fds = NULL;
    g_scratch = NULL;
    g_nfds = 0;
    g_fd_cap = 0;
    g_initialized = false;
}
