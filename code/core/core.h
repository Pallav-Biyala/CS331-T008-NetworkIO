/*
 * core/core.h
 *
 * Shared by every app (echo, file transfer) and every I/O module
 * (select, poll, epoll, io_uring). This header is GROUND TRUTH for the
 * project: do not change it without review by every owner (Design Plan,
 * section 4, last rule), because a change here affects all 12 builds.
 *
 * Everything here is mechanism-neutral. Nothing in this file may include
 * epoll.h, poll.h or liburing.h.
 */
#ifndef CORE_H
#define CORE_H

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* ------------------------------------------------------------------ */
/* 1. Flags (Design Plan section 6)                                    */
/*    Defaults below ARE the hardcoded baseline. Every field is a      */
/*    command-line flag, off or default unless the flag is given.      */
/* ------------------------------------------------------------------ */

typedef struct {
    bool     et;               /* --epoll-et          edge-triggered                    */
    uint32_t maxevents;        /* --epoll-maxevents N default 256                       */
    bool     oneshot;          /* --epoll-oneshot                                       */
} core_epoll_flags_t;

typedef struct {
    uint32_t entries;          /* --uring-entries N      0 = auto: next power of two >= max_conns */
    uint32_t cq_entries;       /* --uring-cq-entries N   0 = liburing default (2x entries)        */
    uint32_t submit_batch;     /* --uring-submit-batch N 0 = submit once per loop iteration       */
    bool     fixed_files;      /* --uring-fixed-files                                             */
    bool     provided_buffers; /* --uring-provided-buffers                                        */
    bool     multishot_accept; /* --uring-multishot-accept                                        */
    bool     multishot_recv;   /* --uring-multishot-recv   requires provided_buffers             */
    bool     fixed_buffers;    /* --uring-fixed-buffers    one contiguous arena, one registration */
    bool     single_issuer;    /* --uring-single-issuer                                           */
    bool     defer_taskrun;    /* --uring-defer-taskrun    requires single_issuer                 */
    bool     coop_taskrun;     /* --uring-coop-taskrun                                            */
    bool     register_ring_fd; /* --uring-register-ring-fd                                        */
    bool     poll_first;       /* --uring-poll-first                                              */
    bool     send_zc;          /* --uring-send-zc          module keeps >= 2 write buffers/conn   */
    bool     sqpoll;           /* --uring-sqpoll                                                  */
    int      sqpoll_cpu;       /* --uring-sqpoll-cpu N     -1 = unset                             */
    uint32_t sqpoll_idle_ms;   /* --uring-sqpoll-idle MS                                          */
    bool     async_file_read;  /* --uring-async-file-read  io_read_file uses IORING_OP_READ       */
    bool     splice;           /* --uring-splice           io_send_file uses splice               */
} core_uring_flags_t;

typedef struct {
    /* 6.1 common flags */
    bool        nodelay;        /* --nodelay        /* --nodelay[=0|1]  TCP_NODELAY on accepted sockets. default true (= no delay).
                                                    --nodelay=0 leaves Nagle's algorithm on (= delay)      */
    uint16_t    port;           /* --port N                                                                   */
    uint32_t    max_conns;      /* --max-conns N    default 10240                                             */
    uint32_t    buf_size;       /* --buf-size N     read size: shared scratch (select/poll/epoll) or
                                                    per-connection buffer (io_uring). Default is chosen by
                                                    the app: 4096 echo, 65536 file                            */
    int         backlog;        /* --backlog N      default 4096                                              */
    int         rcvbuf;         /* --rcvbuf N       0 = kernel default                                        */
    int         sndbuf;         /* --sndbuf N       0 = kernel default                                        */
    uint32_t    accept_batch;   /* --accept-batch 0 = modules normal behavior (readiness: drain until EAGAIN; io_uring: 1 outstanding); N = explicit cap or depth*/
    uint32_t    busy_poll_usec; /* --busy-poll USEC 0 = off                                                   */
    int         cpu;            /* --cpu N          -1 = do not pin                                           */
    const char *stats_file;     /* --stats-file P   NULL = stdout                                             */
    uint32_t    msg_size;       /* --msg-size N     echo frame size, default 512                              */
    uint32_t    chunk_size;     /* --chunk-size N   file write chunk size, default 65536                      */

    /* 6.5 file and web flags */
    bool        sendfile;       /* --sendfile       io_send_file via sendfile(2) (select/poll/epoll)          */
    bool        file_read_mmap; /* --file-read=mmap                                                           */

    core_epoll_flags_t epoll;
    core_uring_flags_t uring;
} core_config_t;

/* Fill cfg with the baseline defaults. The app passes its own default --buf-size. */
void core_config_default(core_config_t *cfg, uint32_t default_buf_size);

/* Parse the long options listed above. Unknown options and bad values fail.
 * Returns 0 on success, -1 on failure with a message in err. */
int core_parse_flags(int argc, char **argv, core_config_t *cfg, char *err, size_t errlen);

/* Refuse invalid combinations and flags that belong to another mechanism.
 * io_name is io_name() ("select", "poll", "epoll", "uring").
 * Examples that must fail: --uring-multishot-recv without --uring-provided-buffers,
 * --uring-defer-taskrun without --uring-single-issuer, --epoll-et on the uring build,
 * --sendfile on the uring build (use --uring-splice there).
 * Returns 0 on success, -1 on failure with a message in err. 
 * --nodelay takes an optional =0 or =1.*/
int core_validate_flags(const core_config_t *cfg, const char *io_name, char *err, size_t errlen);

/* Print every flag that differs from the baseline, one line, at startup. */
void core_print_flags(const core_config_t *cfg, const char *app_name, const char *io_name);

/* Read-only pointer to the active configuration, set by core_init(). */
extern const core_config_t *g_cfg;

/* ------------------------------------------------------------------ */
/* 2. Connections (Design Plan section 4: state machine)               */
/* ------------------------------------------------------------------ */

typedef enum {
    CONN_FREE = 0,
    CONN_ACCEPTED,
    CONN_READING,    /* an io_read is pending                         */
    CONN_WRITING,    /* an io_write / io_send_file is pending         */
    CONN_CLOSING
} conn_state_t;

/*
 * One record per connection, from a table allocated at startup.
 * RULE: at most ONE read or write request per connection is in flight.
 * The app moves READING -> WRITING -> READING and never asks for a read
 * while a write is pending. That bounds unsent output per connection and
 * pauses slow clients automatically (backpressure).
 */
typedef struct conn {
    int           fd;
    uint32_t      id;        /* index in the table, stable for the process lifetime  */
    uint32_t      gen;       /* incremented every time the slot is reused; modules
                                put (id, gen) into async request tags so a late
                                completion for a closed connection is recognized    */
    conn_state_t  state;

    /* Pending request bookkeeping. Written by the module, read-only for apps. */
    size_t        read_max;
    const uint8_t *wr_buf;
    size_t        wr_len;
    size_t        wr_off;

    /* Carry slot: bytes the app cannot use yet (partial frame, start of the next
     * pipelined request). Owned by the app, managed with core_carry_*.          */
    uint8_t      *carry;
    uint32_t      carry_len;

    /* Module-owned on-demand output slot (readiness modules copy an unsent
     * remainder here). Apps never touch it.                                      */
    uint8_t      *out_slot;

    void         *app_priv;  /* app per-connection state, size given to core_init  */
    void         *io_priv;   /* module per-connection state, size from io_priv_size() */
} conn_t;

/* Allocate the table and pools. Call once, before io_init().
 * app_priv_size and io_priv_size are the per-connection state sizes.
 * Returns 0 or -1. */
int core_init(const core_config_t *cfg, size_t app_priv_size, size_t io_priv_size);
void core_shutdown(void);

/* Returns NULL when max_conns connections already exist. Sets fd, state = ACCEPTED,
 * zeroes app_priv and io_priv, bumps gen. */
conn_t *core_conn_alloc(int fd);
/* Releases carry and out_slot, sets CONN_FREE, bumps gen. */
void    core_conn_free(conn_t *c);
conn_t *core_conn_by_fd(int fd);
/* NULL if the slot is free or gen does not match (stale completion). */
conn_t *core_conn_get(uint32_t id, uint32_t gen);
uint32_t core_conn_count(void);   /* connections currently allocated */

/* ------------------------------------------------------------------ */
/* 3. Slot pool and carry helpers                                      */
/*    One mmap'd pool created at startup. Pages become resident only   */
/*    when a slot is first used, so RSS reflects real use. No malloc   */
/*    or free is ever called inside the event loop.                    */
/*    Slot size = cfg->buf_size.                                       */
/* ------------------------------------------------------------------ */

void  *core_slot_alloc(void);          /* NULL if the pool is exhausted */
void   core_slot_free(void *slot);
size_t core_slot_size(void);

/* Append to the carry slot, allocating it on first use.
 * Returns 0, or -1 if the slot would overflow or the pool is exhausted. */
int  core_carry_append(conn_t *c, const void *data, size_t len);
/* Drop the first n bytes of carry (move the rest down); frees the slot when empty. */
void core_carry_consume(conn_t *c, size_t n);
void core_carry_clear(conn_t *c);

/* ------------------------------------------------------------------ */
/* 4. Statistics (Design Plan sections 5.1, 11.2, 12.3)                */
/*    Plain integers, single thread, no atomics. The orchestrator      */
/*    takes a snapshot (SIGUSR1) before and after the measured run     */
/*    and subtracts. Counters are never reset.                         */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t loop_iterations;
    uint64_t wait_calls;        /* select / poll / epoll_wait / io_uring_enter           */
    uint64_t epoll_ctl_calls;   /* lets us check: zero per message on healthy epoll runs  */
    uint64_t reads;             /* recv() calls, or RECV completions                      */
    uint64_t writes;            /* send() calls, or SEND completions                      */
    uint64_t eagain;
    uint64_t partial_reads;
    uint64_t partial_writes;
    uint64_t accepts;
    uint64_t closes;
    uint64_t messages;          /* frames echoed / requests answered                      */
    uint64_t bytes_in;
    uint64_t bytes_out;
    uint64_t errors;            /* unexpected failures; must be 0 in an accepted run      */
    uint64_t peer_resets;       /* ECONNRESET / EPIPE; informational (section 5.1)        */
    uint64_t ring_overflows;    /* io_uring completion ring overflow events               */
    uint64_t file_reads;        /* io_read_file completions                               */
} core_stats_t;

extern core_stats_t g_stats;
#define CORE_STAT_INC(field)       (g_stats.field++)
#define CORE_STAT_ADD(field, n)    (g_stats.field += (uint64_t)(n))

/* Write the counters as JSON to cfg->stats_file (or stdout). Called on SIGUSR1,
 * at exit, and by tests. Must be async-signal-safe or deferred to the loop:
 * the handler sets g_dump_stats and io_run() calls core_stats_dump() between iterations. */
void core_stats_dump(void);

/* ------------------------------------------------------------------ */
/* 5. Process helpers                                                  */
/* ------------------------------------------------------------------ */

extern volatile sig_atomic_t g_shutdown;     /* set by SIGINT / SIGTERM; io_run() returns when set */
extern volatile sig_atomic_t g_dump_stats;   /* set by SIGUSR1; io_run() calls core_stats_dump()   */

void core_install_signals(void);             /* SIGPIPE ignored; SIGINT/SIGTERM/SIGUSR1 handlers  */
int  core_pin_cpu(int cpu);                  /* sched_setaffinity; no-op if cpu < 0               */

/* Common socket setup used by every module, so options cannot drift between builds:
 * SOCK_NONBLOCK | SOCK_CLOEXEC, SO_REUSEADDR, backlog, rcvbuf/sndbuf, busy-poll.
 * Returns the listening fd or -1. */
int  core_make_listener(const core_config_t *cfg);
/* Options for every accepted socket: TCP_NODELAY (enabled by default when cfg->nodelay is true), rcvbuf/sndbuf, busy-poll.
 * Returns 0 or -1. */
int  core_setup_accepted(int fd, const core_config_t *cfg);

/* Entry point shared by every app, so startup is identical in all builds:
 *   parse flags -> validate -> print flags -> core_init -> install signals -> pin cpu ->
 *   io_init -> app_init -> io_listen -> io_run -> app_shutdown -> io_shutdown ->
 *   core_stats_dump -> core_shutdown.
 * Each app's main() is: return core_main(argc, argv, "echo", 4096, sizeof(struct echo_conn)); */
int core_main(int argc, char **argv, const char *app_name,
              uint32_t default_buf_size, size_t app_priv_size);

/* Minimal logging. LOG_DEBUG compiles away unless CORE_DEBUG is defined. Never log in the hot path. */
void core_log(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#define LOG_ERR(...)   core_log(0, __VA_ARGS__)
#define LOG_INFO(...)  core_log(1, __VA_ARGS__)
#ifdef CORE_DEBUG
#define LOG_DEBUG(...) core_log(2, __VA_ARGS__)
#else
#define LOG_DEBUG(...) ((void)0)
#endif

#endif /* CORE_H */
