/*
 * Assumptions and limits:
 * - [SETUP] Hardware, kernel, and network details affect benchmark results but
 *   do not change this module's baseline behavior; shared socket settings are
 *   applied by core_make_listener() and core_setup_accepted().
 * - The plan's epoll flags are treated as the only optional features. They
 *   remain off unless their corresponding core_config_t fields are set.
 * - Reads use the required single shared scratch buffer. Its data is valid
 *   through on_read_done(), and remains valid for a write until completion;
 *   an incomplete send copies its remainder into a core output slot.
 * - io_read_file uses synchronous pread(), stages results in a core slot, and
 *   copies into the caller's buffer immediately before the deferred callback.
 *   This keeps callbacks out of io_* calls and protects results when callers
 *   use the shared scratch buffer.
 * - A short positive recv is a normal completed read. Writes continue from
 *   the saved offset until complete; edge-triggered writes drain to EAGAIN.
 *   EPOLLONESHOT registrations are re-armed after each delivered event.
 * - The fixed core header contains a nested comment that triggers GCC's
 *   -Wcomment; that diagnostic is suppressed only while including io.h.
 * - The Linux compiler was invoked through WSL for a syntax-only check.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcomment"
#include "io.h"
#pragma GCC diagnostic pop

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <unistd.h>

#define EPOLL_LISTENER_TAG UINT64_MAX

typedef enum {
    DEFER_NONE = 0,
    DEFER_WRITE,
    DEFER_FILE,
    DEFER_CLOSE
} deferred_kind_t;

typedef struct {
    size_t read_max;
    deferred_kind_t deferred;
    uint8_t *read_buf;
    uint8_t *file_slot;
    ssize_t deferred_result;
    io_close_reason_t close_reason;
    bool file_pending;
    bool write_is_file;
    bool write_more;
    bool epoll_armed;
    bool et_ready;
    uint32_t registered_events;
    int file_fd;
    off_t file_off;
    size_t file_left;
} epoll_conn_t;

static int g_epoll_fd = -1;
static int g_listener = -1;
static struct epoll_event *g_events;
static size_t g_event_cap;
static uint8_t *g_scratch;
static conn_t **g_connections;
static uint32_t g_deferred_count;
static bool g_initialized;

static epoll_conn_t *epoll_priv(conn_t *c)
{
    return (epoll_conn_t *)c->io_priv;
}

static uint64_t conn_tag(const conn_t *c)
{
    return ((uint64_t)c->gen << 32) | c->id;
}

static uint32_t desired_events(const conn_t *c)
{
    uint32_t events = EPOLLRDHUP;
    if (c->state == CONN_READING)
        events |= EPOLLIN;
    else if (c->state == CONN_WRITING)
        events |= EPOLLOUT;
    else if (c->state == CONN_ACCEPTED)
        events |= EPOLLIN;
    if (g_cfg->epoll.et)
        events |= EPOLLET;
    if (g_cfg->epoll.oneshot)
        events |= EPOLLONESHOT;
    return events;
}

static int epoll_control(int op, int fd, struct epoll_event *event)
{
    int rc;
    CORE_STAT_INC(epoll_ctl_calls);
    rc = epoll_ctl(g_epoll_fd, op, fd, event);
    return rc;
}

static int modify_interest(conn_t *c)
{
    epoll_conn_t *p = epoll_priv(c);
    struct epoll_event event;

    memset(&event, 0, sizeof(event));
    event.events = desired_events(c);
    event.data.u64 = conn_tag(c);
    if (epoll_control(EPOLL_CTL_MOD, c->fd, &event) < 0)
        return -1;
    p->epoll_armed = true;
    p->registered_events = event.events;
    return 0;
}

static int ensure_interest(conn_t *c)
{
    epoll_conn_t *p = epoll_priv(c);
    uint32_t events = desired_events(c);
    if (p->registered_events != events || (g_cfg->epoll.oneshot && !p->epoll_armed))
        return modify_interest(c);
    return 0;
}

static void set_deferred(conn_t *c, deferred_kind_t kind)
{
    epoll_conn_t *p = epoll_priv(c);
    if (p->deferred == DEFER_NONE && kind != DEFER_NONE)
        g_deferred_count++;
    else if (p->deferred != DEFER_NONE && kind == DEFER_NONE)
        g_deferred_count--;
    p->deferred = kind;
}

static void defer_close(conn_t *c, io_close_reason_t reason)
{
    epoll_conn_t *p = epoll_priv(c);
    if (p->deferred == DEFER_CLOSE)
        return;
    set_deferred(c, DEFER_CLOSE);
    p->close_reason = reason;
    c->state = CONN_CLOSING;
}

static void close_with_errno(conn_t *c, int err)
{
    if (err == ECONNRESET || err == EPIPE) {
        CORE_STAT_INC(peer_resets);
        defer_close(c, IO_CLOSE_RESET);
    } else {
        CORE_STAT_INC(errors);
        defer_close(c, IO_CLOSE_ERROR);
    }
}

static void finish_close(conn_t *c, io_close_reason_t reason)
{
    epoll_conn_t *p = epoll_priv(c);
    int fd = c->fd;

    if (fd >= 0) {
        if (epoll_control(EPOLL_CTL_DEL, fd, NULL) < 0 && errno != ENOENT && errno != EBADF)
            CORE_STAT_INC(errors);
        (void)close(fd);
    }
    CORE_STAT_INC(closes);
    set_deferred(c, DEFER_NONE);
    p->file_pending = false;
    p->write_is_file = false;
    p->epoll_armed = false;
    p->et_ready = false;
    if (c->id < g_cfg->max_conns && g_connections[c->id] == c) {
        g_connections[c->id] = NULL;
    }
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

static void defer_completed_write(conn_t *c)
{
    epoll_conn_t *p = epoll_priv(c);
    if (c->out_slot != NULL) {
        core_slot_free(c->out_slot);
        c->out_slot = NULL;
    }
    p->write_is_file = false;
    p->write_more = false;
    c->wr_buf = NULL;
    c->wr_len = 0;
    c->wr_off = 0;
    set_deferred(c, DEFER_WRITE);
}

static bool has_deferred(void)
{
    return g_deferred_count != 0;
}

static void deliver_deferred(void)
{
    uint32_t id;

    for (id = 0; id < g_cfg->max_conns; id++) {
        conn_t *c = g_connections[id];
        epoll_conn_t *p;
        deferred_kind_t kind;
        ssize_t result;

        if (c == NULL)
            continue;
        p = epoll_priv(c);
        kind = p->deferred;
        result = p->deferred_result;
        if (kind == DEFER_NONE)
            continue;
        set_deferred(c, DEFER_NONE);
        if (kind == DEFER_CLOSE) {
            finish_close(c, p->close_reason);
        } else if (kind == DEFER_WRITE) {
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
    }
}

static void accept_ready(void)
{
    uint32_t accepted = 0;
    uint32_t limit = g_cfg->accept_batch;

    for (;;) {
        int fd;
        conn_t *c;
        epoll_conn_t *p;
        struct epoll_event event;
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
            CORE_STAT_INC(closes);
            continue;
        }
        p = epoll_priv(c);
        memset(&event, 0, sizeof(event));
        event.events = desired_events(c);
        event.data.u64 = conn_tag(c);
        if (epoll_control(EPOLL_CTL_ADD, fd, &event) < 0) {
            (void)close(fd);
            CORE_STAT_INC(errors);
            CORE_STAT_INC(closes);
            c->fd = -1;
            core_conn_free(c);
            continue;
        }
        p->epoll_armed = true;
        p->registered_events = event.events;
        g_connections[c->id] = c;
        CORE_STAT_INC(accepts);
        on_accept(c);
    }
}

static void handle_readable(conn_t *c)
{
    epoll_conn_t *p = epoll_priv(c);
    ssize_t n;

    do {
        n = recv(c->fd, g_scratch, p->read_max, 0);
    } while (n < 0 && errno == EINTR);
    CORE_STAT_INC(reads);
    if (n < 0) {
        int err = errno;
        if (err == EAGAIN || err == EWOULDBLOCK) {
            CORE_STAT_INC(eagain);
            p->et_ready = false;
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
    p->et_ready = g_cfg->epoll.et && (size_t)n == p->read_max;
    c->state = CONN_ACCEPTED;
    on_read_done(c, g_scratch, n);
}

static void handle_writable(conn_t *c)
{
    epoll_conn_t *p = epoll_priv(c);
    for (;;) {
        ssize_t n;
        size_t remaining;
        int flags;

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
            if (p->file_left != 0) {
                if (g_cfg->epoll.et)
                    continue;
                return;
            }
        } else {
            remaining = c->wr_len - c->wr_off;
            flags = p->write_more ? MSG_MORE : 0;
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
            if (c->wr_off < c->wr_len) {
                if (g_cfg->epoll.et)
                    continue;
                return;
            }
        }
        defer_completed_write(c);
        return;
    }
}

static void process_edge_ready(void)
{
    uint32_t id;

    if (!g_cfg->epoll.et)
        return;
    for (id = 0; id < g_cfg->max_conns; id++) {
        conn_t *c = g_connections[id];
        epoll_conn_t *p;
        if (c == NULL)
            continue;
        p = epoll_priv(c);
        while (p->et_ready && c->state == CONN_READING && p->deferred == DEFER_NONE) {
            handle_readable(c);
        }
    }
}

const char *io_name(void)
{
    return "epoll";
}

size_t io_priv_size(void)
{
    return sizeof(epoll_conn_t);
}

int io_init(const core_config_t *cfg)
{
    if (cfg == NULL || cfg->max_conns == 0 || cfg->buf_size == 0 || cfg->epoll.maxevents == 0 ||
        cfg->epoll.maxevents > (uint32_t)INT_MAX)
        return -EINVAL;
    g_event_cap = cfg->epoll.maxevents;
    g_events = calloc(g_event_cap, sizeof(*g_events));
    g_connections = calloc(cfg->max_conns, sizeof(*g_connections));
    g_scratch = malloc(cfg->buf_size);
    if (g_events == NULL || g_connections == NULL || g_scratch == NULL) {
        free(g_events);
        free(g_connections);
        free(g_scratch);
        g_events = NULL;
        g_connections = NULL;
        g_scratch = NULL;
        return -ENOMEM;
    }
    g_epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (g_epoll_fd < 0) {
        int err = errno;
        free(g_events);
        free(g_connections);
        free(g_scratch);
        g_events = NULL;
        g_connections = NULL;
        g_scratch = NULL;
        return -err;
    }
    g_listener = -1;
    g_deferred_count = 0;
    g_initialized = true;
    return 0;
}

int io_listen(const core_config_t *cfg)
{
    int fd;
    struct epoll_event event;

    if (!g_initialized || cfg == NULL)
        return -EINVAL;
    fd = core_make_listener(cfg);
    if (fd < 0)
        return -errno;
    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN;
    if (cfg->epoll.et)
        event.events |= EPOLLET;
    if (cfg->epoll.oneshot)
        event.events |= EPOLLONESHOT;
    event.data.u64 = EPOLL_LISTENER_TAG;
    if (epoll_control(EPOLL_CTL_ADD, fd, &event) < 0) {
        int err = errno;
        (void)close(fd);
        return -err;
    }
    g_listener = fd;
    return 0;
}

int io_read(conn_t *c, size_t max)
{
    epoll_conn_t *p;
    if (c == NULL || max == 0 || max > g_cfg->buf_size)
        return -EINVAL;
    p = epoll_priv(c);
    if (c->state == CONN_READING || c->state == CONN_WRITING || p->file_pending || p->deferred != DEFER_NONE)
        return -EBUSY;
    p->read_max = max;
    c->read_max = max;
    c->state = CONN_READING;
    if (ensure_interest(c) < 0) {
        int err = errno;
        close_with_errno(c, err);
    }
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
    epoll_conn_t *p;
    size_t sent = 0;

    if (c == NULL || buf == NULL || len == 0)
        return -EINVAL;
    p = epoll_priv(c);
    if (c->state == CONN_READING || c->state == CONN_WRITING || p->file_pending || p->deferred != DEFER_NONE)
        return -EBUSY;
    c->state = CONN_WRITING;
    c->wr_buf = buf;
    c->wr_len = len;
    c->wr_off = 0;
    p->write_more = more;

    do {
        ssize_t n;
        size_t remaining = len - sent;
        do {
            n = send(c->fd, buf + sent, remaining, more ? MSG_MORE : 0);
        } while (n < 0 && errno == EINTR);
        CORE_STAT_INC(writes);
        if (n < 0) {
            int err = errno;
            if (err != EAGAIN && err != EWOULDBLOCK) {
                close_with_errno(c, err);
                return 0;
            }
            CORE_STAT_INC(eagain);
            break;
        }
        if (n > 0) {
            CORE_STAT_ADD(bytes_out, n);
            if ((size_t)n < remaining)
                CORE_STAT_INC(partial_writes);
        }
        sent += (size_t)n;
        if (n == 0 && remaining != 0) {
            close_with_errno(c, EPIPE);
            return 0;
        }
        if (sent == len)
            break;
    } while (g_cfg->epoll.et);

    if (sent == len) {
        c->wr_buf = NULL;
        c->wr_len = 0;
        c->wr_off = 0;
        p->write_more = false;
        set_deferred(c, DEFER_WRITE);
        return 0;
    }

    c->out_slot = core_slot_alloc();
    if (c->out_slot == NULL || len - sent > core_slot_size()) {
        if (c->out_slot != NULL) {
            core_slot_free(c->out_slot);
            c->out_slot = NULL;
        }
        close_with_errno(c, ENOBUFS);
        return 0;
    }
    memcpy(c->out_slot, buf + sent, len - sent);
    c->wr_buf = c->out_slot;
    c->wr_len = len - sent;
    c->wr_off = 0;
    if (ensure_interest(c) < 0) {
        int err = errno;
        close_with_errno(c, err);
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
    epoll_conn_t *p;
    ssize_t n;

    if (c == NULL || buf == NULL || len == 0 || fd < 0 || off < 0)
        return -EINVAL;
    p = epoll_priv(c);
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
        int err = errno;
        core_slot_free(p->file_slot);
        p->file_slot = NULL;
        close_with_errno(c, err);
        return 0;
    }
    CORE_STAT_INC(file_reads);
    p->read_buf = buf;
    p->deferred_result = n;
    p->file_pending = true;
    set_deferred(c, DEFER_FILE);
    return 0;
}

int io_send_file(conn_t *c, int fd, off_t off, size_t len)
{
    epoll_conn_t *p;
    ssize_t n;
    off_t next_off = off;
    size_t sent = 0;

    if (c == NULL || fd < 0 || off < 0 || len == 0)
        return -EINVAL;
    if (!g_cfg->sendfile)
        return -ENOTSUP;
    p = epoll_priv(c);
    if (c->state == CONN_READING || c->state == CONN_WRITING || p->file_pending || p->deferred != DEFER_NONE)
        return -EBUSY;
    c->state = CONN_WRITING;
    p->write_is_file = true;
    p->file_fd = fd;
    p->file_off = off;
    p->file_left = len;
    do {
        do {
            n = sendfile(c->fd, fd, &next_off, len - sent);
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
            break;
        }
        if (n > 0) {
            CORE_STAT_ADD(bytes_out, n);
            if ((size_t)n < len - sent)
                CORE_STAT_INC(partial_writes);
        }
        sent += (size_t)n;
        if (n == 0 && sent < len) {
            close_with_errno(c, EIO);
            return 0;
        }
        if (sent == len)
            break;
    } while (g_cfg->epoll.et);
    p->file_off = next_off;
    p->file_left = len - sent;
    if (p->file_left == 0) {
        defer_completed_write(c);
    } else if (ensure_interest(c) < 0) {
        int err = errno;
        close_with_errno(c, err);
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
        int ready;
        int timeout;
        int i;

        deliver_deferred();
        if (g_dump_stats) {
            g_dump_stats = 0;
            core_stats_dump();
        }
        CORE_STAT_INC(loop_iterations);
        process_edge_ready();
        timeout = has_deferred() ? 0 : -1;
        CORE_STAT_INC(wait_calls);
        ready = epoll_wait(g_epoll_fd, g_events, (int)g_event_cap, timeout);
        if (ready < 0 && errno == EINTR && !g_shutdown)
            continue;
        if (ready < 0) {
            CORE_STAT_INC(errors);
            continue;
        }
        for (i = 0; i < ready; i++) {
            uint64_t tag = g_events[i].data.u64;
            uint32_t events = g_events[i].events;
            conn_t *c;
            epoll_conn_t *p;
            if (tag == EPOLL_LISTENER_TAG) {
                if ((events & (EPOLLIN | EPOLLERR | EPOLLHUP)) != 0)
                    accept_ready();
                if (g_cfg->epoll.oneshot) {
                    struct epoll_event listener_event;
                    memset(&listener_event, 0, sizeof(listener_event));
                    listener_event.events = EPOLLIN;
                    if (g_cfg->epoll.et)
                        listener_event.events |= EPOLLET;
                    listener_event.events |= EPOLLONESHOT;
                    listener_event.data.u64 = EPOLL_LISTENER_TAG;
                    if (epoll_control(EPOLL_CTL_MOD, g_listener, &listener_event) < 0)
                        CORE_STAT_INC(errors);
                }
                continue;
            }
            c = core_conn_get((uint32_t)tag, (uint32_t)(tag >> 32));
            if (c == NULL)
                continue;
            p = epoll_priv(c);
            if (g_cfg->epoll.oneshot)
                p->epoll_armed = false;
            if (p->deferred != DEFER_NONE || c->state == CONN_CLOSING)
                continue;
            if ((events & EPOLLERR) != 0) {
                int err = 0;
                socklen_t errlen = sizeof(err);
                if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &errlen) < 0 || err == 0)
                    err = EIO;
                close_with_errno(c, err);
                continue;
            }
            if (c->state == CONN_READING && (events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP)) != 0)
                handle_readable(c);
            else if (c->state == CONN_WRITING && (events & (EPOLLOUT | EPOLLHUP | EPOLLRDHUP)) != 0)
                handle_writable(c);
            if (g_cfg->epoll.oneshot && p->deferred == DEFER_NONE && c->state != CONN_CLOSING &&
                (c->state == CONN_READING || c->state == CONN_WRITING) && !p->epoll_armed) {
                if (ensure_interest(c) < 0) {
                    int err = errno;
                    close_with_errno(c, err);
                }
            }
        }
    }
    for (uint32_t id = 0; id < g_cfg->max_conns; id++) {
        conn_t *c = g_connections[id];
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
    if (g_epoll_fd >= 0) {
        (void)close(g_epoll_fd);
        g_epoll_fd = -1;
    }
    free(g_events);
    free(g_connections);
    free(g_scratch);
    g_events = NULL;
    g_connections = NULL;
    g_scratch = NULL;
    g_event_cap = 0;
    g_deferred_count = 0;
    g_initialized = false;
}

