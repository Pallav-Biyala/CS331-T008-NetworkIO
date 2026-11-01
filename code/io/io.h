/*
 * io/io.h
 *
 * The plug-in contract between an app and an I/O module
 * (Design Plan section 4). GROUND TRUTH: do not change without review by
 * every owner.
 *
 * Exactly one module is linked into each build, chosen at compile time:
 *   make APP=echo IO=epoll
 * Modules: io_select.c, io_poll.c, io_epoll.c, io_uring.c.
 *
 * The contract is a set of I/O REQUESTS, not readiness events. The app says
 * what it wants; the module says when it is done and decides how: a readiness
 * module waits for readiness and then calls recv()/send(), io_uring queues
 * the request. The app never includes epoll.h, poll.h or liburing.h. If an
 * app needs a mechanism-specific line, the interface is missing something:
 * fix this header for everyone.
 *
 * Do not modify any mechanism to look like another (P2): each module is the
 * normal, textbook use of its mechanism; every extra feature is a flag.
 */
#ifndef IO_H
#define IO_H

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "../core/core.h"

/* ------------------------------------------------------------------ */
/* Why a connection was closed (argument of on_close)                  */
/* ------------------------------------------------------------------ */
typedef enum {
    IO_CLOSE_APP = 0,    /* the app called io_close()                          */
    IO_CLOSE_RESET,      /* ECONNRESET / EPIPE; counted in peer_resets         */
    IO_CLOSE_ERROR,      /* any other failure; counted in errors               */
    IO_CLOSE_LIMIT       /* refused: max_conns reached or (select) fd >= 1024  */
} io_close_reason_t;

/* ------------------------------------------------------------------ */
/* A. The app calls the module                                         */
/*    All return 0 on success or a negative errno:                     */
/*      -EBUSY   a read or write is already in flight on this conn     */
/*      -ENOTSUP the request needs a flag that is off (io_send_file)   */
/*      -EINVAL  bad argument (max > buf_size, len == 0, ...)          */
/*    A request that returns 0 is ALWAYS completed through a callback  */
/*    (section B), never by a nested call.                             */
/* ------------------------------------------------------------------ */

/* Name used in flag validation and result folders: "select", "poll", "epoll", "uring". */
const char *io_name(void);

/* Size of the per-connection module state, passed to core_init(). */
size_t io_priv_size(void);

/* Create the epoll fd, ring, shared scratch buffer, etc. Apply module flags from cfg
 * (they were already validated by core_validate_flags). */
int  io_init(const core_config_t *cfg);

/* Create the listening socket (via core_make_listener) and arm accepting. */
int  io_listen(const core_config_t *cfg);

/* The event loop. Returns when g_shutdown is set. Between iterations it must:
 *   - deliver queued completions (see "no recursion" below),
 *   - call core_stats_dump() if g_dump_stats is set,
 *   - increment g_stats.loop_iterations.
 * Accepting: by default readiness modules drain the accept queue until EAGAIN; io_uring keeps one ACCEPT outstanding and re-arms it.; cfg->accept_batch = N allows N,
*/
int  io_run(void);

void io_shutdown(void);

/* Ask for up to `max` bytes (max <= cfg->buf_size). Completes with on_read_done.
 * Precondition: no read or write in flight on c. Sets c->state = CONN_READING. */
int  io_read(conn_t *c, size_t max);

/* A buffer the app can fill before io_write (header + file bytes, assembled echo
 * frames). Readiness modules return the shared scratch buffer; uring returns the
 * connection's own buffer (or a free one of its write buffers with --uring-send-zc).
 * *cap receives the usable size. The buffer is valid until the next io_ call on c
 * or until the app returns to the loop, whichever is first. */
uint8_t *io_get_buf(conn_t *c, size_t *cap);

/* Send len bytes. `buf` stays UNCHANGED by the app until on_write_done.
 * Readiness modules try send() at once and, if it is incomplete, copy the unsent
 * remainder into an on-demand output slot (so they do not depend on buf after this
 * call returns). They never call epoll_ctl / change interest unless a send is
 * incomplete. Completes with on_write_done once every byte has been accepted by
 * the kernel. Sets c->state = CONN_WRITING. */
int  io_write(conn_t *c, const uint8_t *buf, size_t len);

/* Read len bytes of file `fd` starting at `off` into buf (a buffer from io_get_buf).
 * Baseline: pread(). With --uring-async-file-read on the uring build: IORING_OP_READ.
 * Completes with on_file_read_done. Does not change the connection state. */
int  io_read_file(conn_t *c, uint8_t *buf, size_t len, int fd, off_t off);

/* Kernel-side file send: only with --sendfile (select/poll/epoll, via sendfile(2))
 * or --uring-splice (uring). Otherwise returns -ENOTSUP. The app sends any in-memory
 * header with io_write FIRST, and the module sets MSG_MORE on that send when it knows
 * a file send follows (the app passes that knowledge through io_write_more below).
 * Completes with on_write_done. */
int  io_send_file(conn_t *c, int fd, off_t off, size_t len);

/* Same as io_write, but the module sets MSG_MORE so the bytes wait for the file data
 * that follows in io_send_file. Used only for the HTTP header when --sendfile or
 * --uring-splice is on. */
int  io_write_more(conn_t *c, const uint8_t *buf, size_t len);

/* Close the connection. The module cancels or waits for any in-flight request first,
 * then closes the fd, calls on_close(c, IO_CLOSE_APP) and core_conn_free(c). */
void io_close(conn_t *c);

/* ------------------------------------------------------------------ */
/* B. The module calls the app (implemented by the app files)               */
/*    Always called FROM INSIDE io_run(), never from inside an io_     */
/*    call. A request that finishes at once is queued on an internal   */
/*    list and delivered at the top of the next loop pass, with a      */
/*    zero wait timeout while the list is not empty. This is what      */
/*    prevents on_read_done -> io_write -> on_write_done -> io_read    */
/*    recursion, and it is the same in all four modules.               */
/* ------------------------------------------------------------------ */

/* A connection was accepted. c->state == CONN_ACCEPTED. Typical app: io_read(c, buf_size). */
void on_accept(conn_t *c);

/* Result of io_read.
 *   n >  0  bytes are in buf. `buf` is module-owned and VALID ONLY UNTIL THIS CALLBACK
 *           RETURNS, unless the app passes it to io_write (then it stays valid until
 *           on_write_done). Copy anything you need to keep (use the carry slot).
 *   n == 0  the peer closed (EOF). Typical app: drop the carry, io_close(c).
 * Failures are reported through on_close, never as n < 0. */
void on_read_done(conn_t *c, uint8_t *buf, ssize_t n);

/* Every byte of the last io_write / io_write_more / io_send_file was accepted by the kernel. */
void on_write_done(conn_t *c);

/* Result of io_read_file. n >= 0 bytes were read into buf (n == 0 at end of file). */
void on_file_read_done(conn_t *c, uint8_t *buf, ssize_t n);

/* The connection is gone. After this callback the module calls core_conn_free(c).
 * The app must release its own per-connection state here. */
void on_close(conn_t *c, io_close_reason_t reason);

/* ------------------------------------------------------------------ */
/* C. App lifecycle (implemented by the app files, called by core_main)     */
/* ------------------------------------------------------------------ */

/* After io_init, before io_listen. File server: open every file in the document root once,
 * cache descriptors and sizes (never open/close per request). */
int  app_init(const core_config_t *cfg);
void app_shutdown(void);

/* ------------------------------------------------------------------ */
/* D. Rules every module must follow (checked in review)               */
/*  1. Baseline only; every extra feature is a flag from section 6.    */
/*  2. select/poll/epoll read into ONE shared scratch buffer of        */
/*     cfg->buf_size. uring uses one buffer per connection (or the     */
/*     provided pool with --uring-provided-buffers). Never give the    */
/*     readiness modules per-connection read buffers.                  */
/*  3. Output slots, carry slots and the pool come from core_slot_*;   */
/*     no malloc/free inside the event loop.                           */
/*  4. Update g_stats on every read, write, EAGAIN, partial transfer,  */
/*     accept, close and error. ECONNRESET/EPIPE -> peer_resets,       */
/*     everything unexpected -> errors. Short reads are not errors.    */
/*  5. Never free a connection or buffer while a request on it is in   */
/*     flight (uring: use (id, gen) tags; on close cancel or wait).    */
/*  6. epoll baseline: add each socket once with EPOLLIN (level-       */
/*     triggered); try send() immediately; only on an incomplete send  */
/*     switch to EPOLLOUT (one epoll_ctl) and back (one epoll_ctl).    */
/*  7. select refuses fd >= FD_SETSIZE (IO_CLOSE_LIMIT); select and    */
/*     poll scan their whole set after every wait (that cost is what   */
/*     we measure; do not add an active list).                         */
/*  8. Sockets are accepted via core_setup_accepted so options match.  */
/* ------------------------------------------------------------------ */

#endif /* IO_H */
