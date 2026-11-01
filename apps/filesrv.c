/*
 * filesrv.c - file-transfer server (plan section 8), baseline only.
 *
 * One app file that runs unchanged on every io/ module: it only issues the
 * I/O requests of the section 4 contract and reacts to their callbacks. It
 * contains no select/poll/epoll/io_uring code and makes no socket calls.
 *
 * ---------------------------------------------------------------------------
 * INTEGRATION STATUS - READ THIS FIRST
 * ---------------------------------------------------------------------------
 * core/core.h and io/io.h did not exist when this file was written. The code
 * uses exactly the function and callback NAMES from plan section 4; the C
 * types, return conventions and the few non-section-4 items below are
 * ASSUMPTIONS, collected here so they can be fixed in one place once the
 * real headers exist:
 *
 *   From io/io.h (section 4 names, assumed types):
 *     typedef struct conn conn_t;                       opaque connection
 *     int   io_init(const core_config_t *cfg);           0 ok, <0 error
 *     int   io_listen(int port);                         0 ok, <0 error
 *     int   io_run(void);                                returns at shutdown
 *     int   io_read(conn_t *c, size_t max);              0 = request queued
 *     void *io_get_buf(conn_t *c, size_t *cap);          NULL on failure
 *     int   io_write(conn_t *c, const void *buf, size_t len);
 *     int   io_read_file(conn_t *c, void *buf, size_t len, int fd, off_t off);
 *     void  io_close(conn_t *c);
 *     callbacks implemented here:
 *     void on_accept(conn_t *c);
 *     void on_read_done(conn_t *c, const char *buf, ssize_t n);  n==0: peer closed
 *     void on_write_done(conn_t *c);
 *     void on_file_read_done(conn_t *c, void *buf, ssize_t n);   n<0: -errno
 *     void on_close(conn_t *c, int reason);
 *
 *   From core/core.h (NOT specified by the plan beyond "flag parser" and
 *   "connection table"):
 *     core_config_t fields: port, max_conns, buf_size, chunk_size, docroot
 *       - docroot: there is NO documented flag for the document root in
 *         section 6. Needs a team decision (e.g. a --docroot flag in core).
 *     int    core_parse_flags(int argc, char **argv, core_config_t *cfg);
 *     size_t core_conn_index(const conn_t *c);  stable slot in [0, max_conns)
 *       - needed to find this app's per-connection state from a conn_t.
 *
 *   Not wired yet: g_stats (io.h section D) and refusing file-specific
 *   optional flags this file does not implement (--sendfile,
 *   --file-read=mmap, --uring-splice). The app counters below are printed at
 *   shutdown instead of being merged into the shared stats JSON.
 *
 * ---------------------------------------------------------------------------
 * PROTOCOL / POLICY DECISIONS
 * ---------------------------------------------------------------------------
 *   - Request syntax and its policies are in http_parser.h.
 *   - No HTTP pipelining (team brief; NOTE: this conflicts with plan 8.1 item
 *     4 and gate test T13, which must be resolved by the team). A client must
 *     wait for the response before sending the next request. If bytes arrive
 *     behind a complete request, the server answers 400 and closes, because
 *     it cannot tell an unannounced body from a pipelined request.
 *   - 400 and 405 responses always close the connection. 404 honours the
 *     request's keep-alive choice (the request was well formed).
 *   - Error responses carry a short text body with a correct Content-Length.
 *
 * ---------------------------------------------------------------------------
 * PER-CONNECTION STATE MACHINE (one I/O request in flight at a time)
 * ---------------------------------------------------------------------------
 *
 *   on_accept -> READING --io_read--> on_read_done
 *       incomplete request  -> keep bytes in carry, io_read again (READING)
 *       error / 404         -> WRITING (header + short body)
 *       200, size 0         -> WRITING (header only)
 *       200, size > 0       -> FILE_READ: io_get_buf, header at the start,
 *                              io_read_file into the bytes after the header
 *   on_file_read_done -> WRITING: io_write(header + chunk) in ONE write
 *   on_write_done
 *       body bytes left     -> FILE_READ: io_get_buf, io_read_file (no header)
 *       done, keep-alive    -> READING
 *       done, close         -> io_close
 *
 * Buffer lifetimes (plan section 4):
 *   - The buffer passed to on_read_done belongs to the module and is valid
 *     only during the callback. Request bytes that must outlive it are
 *     copied into this connection's carry slot; the parsed request (file
 *     name, keep-alive) is copied into an http_request_t on the stack before
 *     io_get_buf is called, because io_get_buf may hand back the same memory
 *     (e.g. the io_uring per-connection buffer).
 *   - The buffer from io_get_buf is filled by io_read_file and must stay
 *     untouched until on_write_done. The next chunk is only requested after
 *     on_write_done, so a buffer is never refilled while a write uses it,
 *     and unsent output per connection is bounded by one chunk (backpressure
 *     comes from the module not completing the write).
 */
#include "../core/core.h"
#include "../io/io.h"
#include "http_parser.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* Plan 8.1 item 2: the document root holds a fixed set of at most 16 files. */
#define FS_MAX_FILES 16

typedef struct {
    char     name[HTTP_MAX_NAME_LEN + 1];
    size_t   name_len;
    int      fd;
    uint64_t size;
} fs_file_t;

typedef enum {
    FS_FREE = 0,  /* slot not in use                                     */
    FS_READING,   /* io_read outstanding, waiting for request bytes      */
    FS_FILE_READ, /* io_read_file outstanding into out_buf               */
    FS_WRITING,   /* io_write outstanding from out_buf                   */
    FS_CLOSING    /* io_close issued; ignore anything that still arrives */
} fs_state_t;

typedef struct {
    conn_t     *conn;
    fs_state_t  state;
    char       *carry;          /* HTTP_MAX_REQUEST_SIZE bytes, from the pool */
    size_t      carry_len;      /* bytes of an incomplete request kept so far */

    /* current response */
    const fs_file_t *file;      /* NULL for error responses                   */
    uint64_t    content_length; /* body bytes promised in Content-Length      */
    uint64_t    body_sent;      /* body bytes whose write has completed       */
    bool        close_after;    /* close once the response is fully written   */
    char       *out_buf;        /* buffer from io_get_buf, owned by module    */
    size_t      out_header_len; /* header bytes at the start of out_buf       */
    size_t      file_read_len;  /* bytes requested from io_read_file          */
    size_t      write_body_len; /* body bytes inside the outstanding write    */
} fs_conn_t;

typedef struct {
    uint64_t requests;
    uint64_t responses_200, responses_400, responses_404, responses_405;
    uint64_t rejected_trailing_bytes; /* bytes behind a request (no pipelining) */
    uint64_t body_bytes_sent;
    uint64_t aborted_transfers;       /* connection closed mid-response          */
    uint64_t errors;                  /* unexpected: must stay 0 in a valid run  */
} fs_stats_t;

/*
 * Server context. Callbacks carry no user pointer in the section 4 contract,
 * so the app's state has to live at file scope; it is kept in one struct.
 */
static struct {
    size_t      chunk_size;
    size_t      read_size;      /* --buf-size: amount requested per io_read */
    size_t      max_conns;
    fs_file_t   files[FS_MAX_FILES];
    size_t      file_count;
    fs_conn_t  *conns;          /* max_conns entries, allocated at startup */
    char       *carry_pool;     /* max_conns * HTTP_MAX_REQUEST_SIZE, mmap'd */
    size_t      carry_pool_size;
    fs_stats_t  stats;
} g_fs;

/* ---- connection lookup ---------------------------------------------------- */

static fs_conn_t *fs_conn_of(conn_t *c)
{
    size_t idx = core_conn_index(c); /* ASSUMED core API, see header */
    if (idx >= g_fs.max_conns)
        return NULL;
    return &g_fs.conns[idx];
}

/* ---- file table ----------------------------------------------------------- */

static const fs_file_t *fs_lookup(const char *name, size_t name_len)
{
    for (size_t i = 0; i < g_fs.file_count; i++) {
        const fs_file_t *f = &g_fs.files[i];
        if (f->name_len == name_len && memcmp(f->name, name, name_len) == 0)
            return f;
    }
    return NULL;
}

static int fs_compare_files(const void *a, const void *b)
{
    return strcmp(((const fs_file_t *)a)->name, ((const fs_file_t *)b)->name);
}

static void fs_close_files(void)
{
    for (size_t i = 0; i < g_fs.file_count; i++) {
        if (g_fs.files[i].fd >= 0 && close(g_fs.files[i].fd) != 0)
            fprintf(stderr, "filesrv: close(%s): %s\n", g_fs.files[i].name, strerror(errno));
        g_fs.files[i].fd = -1;
    }
    g_fs.file_count = 0;
}

/*
 * Open every regular file in docroot once (plan 8.1 item 2). Names that the
 * request parser would reject are skipped with a warning, so every table
 * entry is reachable. More than FS_MAX_FILES servable files is an error
 * rather than a silent truncation.
 */
static int fs_load_files(const char *docroot)
{
    DIR *dir = opendir(docroot);
    if (dir == NULL) {
        fprintf(stderr, "filesrv: opendir(%s): %s\n", docroot, strerror(errno));
        return -1;
    }
    int dfd = dirfd(dir);
    int rc = 0;
    struct dirent *de;

    errno = 0;
    while ((de = readdir(dir)) != NULL) {
        const char *name = de->d_name;
        size_t len = strlen(name);
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;
        if (!http_name_is_valid(name, len)) {
            fprintf(stderr, "filesrv: skipping '%s': not a servable file name\n", name);
            continue;
        }
        int fd = openat(dfd, name, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            fprintf(stderr, "filesrv: open(%s): %s\n", name, strerror(errno));
            rc = -1;
            break;
        }
        struct stat st;
        if (fstat(fd, &st) != 0) {
            fprintf(stderr, "filesrv: fstat(%s): %s\n", name, strerror(errno));
            close(fd);
            rc = -1;
            break;
        }
        if (!S_ISREG(st.st_mode)) {
            fprintf(stderr, "filesrv: skipping '%s': not a regular file\n", name);
            close(fd);
            continue;
        }
        if (g_fs.file_count == FS_MAX_FILES) {
            fprintf(stderr, "filesrv: more than %d files in %s\n", FS_MAX_FILES, docroot);
            close(fd);
            rc = -1;
            break;
        }
        fs_file_t *f = &g_fs.files[g_fs.file_count++];
        memcpy(f->name, name, len + 1);
        f->name_len = len;
        f->fd = fd;
        f->size = (uint64_t)st.st_size;
        errno = 0;
    }
    if (rc == 0 && errno != 0) {
        fprintf(stderr, "filesrv: readdir(%s): %s\n", docroot, strerror(errno));
        rc = -1;
    }
    closedir(dir);

    if (rc != 0) {
        fs_close_files();
        return -1;
    }
    if (g_fs.file_count == 0)
        fprintf(stderr, "filesrv: warning: no servable files in %s\n", docroot);

    qsort(g_fs.files, g_fs.file_count, sizeof g_fs.files[0], fs_compare_files);
    for (size_t i = 0; i < g_fs.file_count; i++)
        fprintf(stderr, "filesrv: file /%s  %" PRIu64 " bytes\n",
                g_fs.files[i].name, g_fs.files[i].size);
    return 0;
}

/* ---- connection helpers --------------------------------------------------- */

static void fs_reset_response(fs_conn_t *fc)
{
    fc->file = NULL;
    fc->content_length = 0;
    fc->body_sent = 0;
    fc->close_after = false;
    fc->out_buf = NULL;
    fc->out_header_len = 0;
    fc->file_read_len = 0;
    fc->write_body_len = 0;
}

/* Forget the connection. Safe to call more than once. */
static void fs_release(fs_conn_t *fc)
{
    if (fc->state == FS_FILE_READ || fc->state == FS_WRITING)
        g_fs.stats.aborted_transfers++;
    fc->state = FS_FREE;
    fc->conn = NULL;
    fc->carry_len = 0;
    fs_reset_response(fc);
}

/*
 * App-initiated close. The state is released first, so any callback that a
 * module still delivers for this connection (it should not) finds the slot
 * FREE or CLOSING and is ignored. Whether io_close later triggers on_close is
 * not specified; on_close is idempotent either way.
 */
static void fs_close(fs_conn_t *fc)
{
    conn_t *c = fc->conn;
    fs_release(fc);
    fc->state = FS_CLOSING;
    io_close(c);
}

/* Unexpected condition: count it as an error (plan 5.1) and close. */
static void fs_fail(fs_conn_t *fc, const char *what)
{
    g_fs.stats.errors++;
    fprintf(stderr, "filesrv: error: %s\n", what);
    fs_close(fc);
}

static void fs_start_reading(fs_conn_t *fc)
{
    fs_reset_response(fc);
    fc->state = FS_READING;
    if (io_read(fc->conn, g_fs.read_size) != 0)
        fs_fail(fc, "io_read failed");
}

/*
 * Ask for the next body chunk: read up to min(chunk, room after the header,
 * bytes still owed) at the connection's own file offset. The file offset is
 * body_sent: chunks are requested strictly after the previous write
 * completed, so everything before body_sent has been handed to the kernel.
 */
static void fs_request_chunk(fs_conn_t *fc, char *out, size_t cap, size_t header_len)
{
    uint64_t remaining = fc->content_length - fc->body_sent;
    size_t room = cap - header_len;
    size_t want = g_fs.chunk_size;
    if (want > room)
        want = room;
    if ((uint64_t)want > remaining)
        want = (size_t)remaining;
    if (want == 0) {
        fs_fail(fc, "io_get_buf buffer too small for header plus one byte");
        return;
    }

    fc->out_buf = out;
    fc->out_header_len = header_len;
    fc->file_read_len = want;
    fc->state = FS_FILE_READ;
    if (io_read_file(fc->conn, out + header_len, want, fc->file->fd,
                     (off_t)fc->body_sent) != 0)
        fs_fail(fc, "io_read_file failed");
}

/* Header-only response with a short text body (errors) or empty body. */
static void fs_send_simple(fs_conn_t *fc, int status, bool keep_alive)
{
    switch (status) {
    case 400: g_fs.stats.responses_400++; break;
    case 404: g_fs.stats.responses_404++; break;
    case 405: g_fs.stats.responses_405++; break;
    default: break;
    }

    char body[64];
    int body_len = snprintf(body, sizeof body, "%d %s\n", status, http_reason_phrase(status));
    if (body_len < 0 || (size_t)body_len >= sizeof body) {
        fs_fail(fc, "error body formatting");
        return;
    }

    size_t cap = 0;
    char *out = io_get_buf(fc->conn, &cap);
    if (out == NULL) {
        fs_fail(fc, "io_get_buf failed");
        return;
    }
    size_t hl = http_format_response_header(out, cap, status, (uint64_t)body_len, keep_alive);
    if (hl == 0 || cap - hl < (size_t)body_len) {
        fs_fail(fc, "io_get_buf buffer too small for error response");
        return;
    }
    memcpy(out + hl, body, (size_t)body_len);

    fs_reset_response(fc);
    fc->close_after = !keep_alive;
    fc->out_buf = out;
    fc->state = FS_WRITING;
    if (io_write(fc->conn, out, hl + (size_t)body_len) != 0)
        fs_fail(fc, "io_write failed");
}

static void fs_start_response(fs_conn_t *fc, const http_request_t *req)
{
    g_fs.stats.requests++;
    const fs_file_t *f = fs_lookup(req->name, req->name_len);
    if (f == NULL) {
        fs_send_simple(fc, 404, req->keep_alive);
        return;
    }
    g_fs.stats.responses_200++;

    size_t cap = 0;
    char *out = io_get_buf(fc->conn, &cap);
    if (out == NULL) {
        fs_fail(fc, "io_get_buf failed");
        return;
    }
    size_t hl = http_format_response_header(out, cap, 200, f->size, req->keep_alive);
    if (hl == 0) {
        fs_fail(fc, "io_get_buf buffer too small for response header");
        return;
    }

    fs_reset_response(fc);
    fc->file = f;
    fc->content_length = f->size;
    fc->close_after = !req->keep_alive;

    if (f->size == 0) { /* zero-length file: header only, never call io_read_file */
        fc->out_buf = out;
        fc->state = FS_WRITING;
        if (io_write(fc->conn, out, hl) != 0)
            fs_fail(fc, "io_write failed");
        return;
    }
    /* Header and first chunk travel in one io_write (plan 8.1 item 3, 8.5). */
    fs_request_chunk(fc, out, cap, hl);
}

/* ---- callbacks (called only from inside io_run) ---------------------------- */

void on_accept(conn_t *c)
{
    fs_conn_t *fc = fs_conn_of(c);
    if (fc == NULL) {
        g_fs.stats.errors++;
        fprintf(stderr, "filesrv: error: connection index out of range\n");
        io_close(c);
        return;
    }
    if (fc->state != FS_FREE && fc->state != FS_CLOSING) {
        /* The slot was handed out again without on_close: a module bug. */
        g_fs.stats.errors++;
        fprintf(stderr, "filesrv: error: accept on a slot still in use\n");
    }
    fs_release(fc);
    fc->conn = c;
    fc->carry = g_fs.carry_pool + (size_t)(fc - g_fs.conns) * HTTP_MAX_REQUEST_SIZE;
    fs_start_reading(fc);
}

void on_read_done(conn_t *c, const char *buf, ssize_t n)
{
    fs_conn_t *fc = fs_conn_of(c);
    if (fc == NULL || fc->conn != c || fc->state == FS_FREE || fc->state == FS_CLOSING)
        return; /* stale completion for a connection we already closed */
    if (fc->state != FS_READING) {
        fs_fail(fc, "read completion while not reading");
        return;
    }
    if (n <= 0) {
        /* Peer closed (n == 0). A half-received request is discarded. The
         * contract reports failures via on_close, so n < 0 is unexpected. */
        if (n < 0)
            g_fs.stats.errors++;
        fs_close(fc);
        return;
    }

    /*
     * Treat carry + new bytes as one stream. Fast path: nothing carried, so
     * parse the module's buffer in place. Otherwise append to the carry
     * slot; at most HTTP_MAX_REQUEST_SIZE bytes are kept, which is all the
     * parser will ever look at.
     */
    const char *data;
    size_t data_len;
    size_t total_len; /* every byte received for this request so far */
    if (fc->carry_len == 0) {
        data = buf;
        data_len = (size_t)n;
        total_len = (size_t)n;
    } else {
        size_t room = HTTP_MAX_REQUEST_SIZE - fc->carry_len;
        size_t take = (size_t)n < room ? (size_t)n : room;
        memcpy(fc->carry + fc->carry_len, buf, take);
        total_len = fc->carry_len + (size_t)n;
        fc->carry_len += take;
        data = fc->carry;
        data_len = fc->carry_len;
    }

    http_request_t req;
    http_parse_result_t r = http_parse_request(data, data_len, &req);

    if (r == HTTP_PARSE_INCOMPLETE) {
        /* The parser returns INCOMPLETE only below 4 KB, so everything fit. */
        if (data == buf) {
            memcpy(fc->carry, buf, (size_t)n);
            fc->carry_len = (size_t)n;
        }
        if (io_read(c, g_fs.read_size) != 0)
            fs_fail(fc, "io_read failed");
        return;
    }

    /* From here on neither buf nor the carry is used again: req holds copies. */
    fc->carry_len = 0;

    if (r == HTTP_PARSE_ERROR) {
        g_fs.stats.requests++;
        fs_send_simple(fc, req.error_status, false);
        return;
    }
    if (req.request_len != total_len) {
        /* Bytes behind the request: a pipelined request or an undeclared
         * body. Not supported; reject and close (see header). */
        g_fs.stats.requests++;
        g_fs.stats.rejected_trailing_bytes++;
        fs_send_simple(fc, 400, false);
        return;
    }
    fs_start_response(fc, &req);
}

void on_file_read_done(conn_t *c, void *buf, ssize_t n)
{
    fs_conn_t *fc = fs_conn_of(c);
    if (fc == NULL || fc->conn != c || fc->state == FS_FREE || fc->state == FS_CLOSING)
        return;
    if (fc->state != FS_FILE_READ || buf != fc->out_buf + fc->out_header_len) {
        fs_fail(fc, "unexpected file-read completion");
        return;
    }
    if (n <= 0) {
        /* n < 0: pread failed. n == 0: the file is shorter than its size at
         * startup. Either way Content-Length can no longer be honoured, so
         * the only correct action is to close the connection. */
        fs_fail(fc, n < 0 ? "file read failed" : "file shrank during transfer");
        return;
    }
    if ((size_t)n > fc->file_read_len) {
        fs_fail(fc, "file read returned more than requested");
        return;
    }

    /* A short read is fine: send what we got, the next chunk continues at
     * body_sent + n. The write never exceeds Content-Length because
     * file_read_len <= content_length - body_sent. */
    fc->write_body_len = (size_t)n;
    fc->state = FS_WRITING;
    if (io_write(c, fc->out_buf, fc->out_header_len + (size_t)n) != 0)
        fs_fail(fc, "io_write failed");
}

void on_write_done(conn_t *c)
{
    fs_conn_t *fc = fs_conn_of(c);
    if (fc == NULL || fc->conn != c || fc->state == FS_FREE || fc->state == FS_CLOSING)
        return;
    if (fc->state != FS_WRITING) {
        fs_fail(fc, "write completion while not writing");
        return;
    }

    /* Every byte of the last io_write was accepted by the kernel; the
     * buffer is free again (the module may hand it out once more). */
    fc->body_sent += fc->write_body_len;
    g_fs.stats.body_bytes_sent += fc->write_body_len;
    fc->write_body_len = 0;
    fc->out_buf = NULL;
    fc->out_header_len = 0;

    if (fc->file != NULL && fc->body_sent < fc->content_length) {
        size_t cap = 0;
        char *out = io_get_buf(c, &cap);
        if (out == NULL) {
            fs_fail(fc, "io_get_buf failed");
            return;
        }
        fs_request_chunk(fc, out, cap, 0);
        return;
    }

    /* Response complete: exactly content_length body bytes were sent. */
    if (fc->close_after) {
        fs_close(fc);
        return;
    }
    fs_start_reading(fc);
}

void on_close(conn_t *c, int reason)
{
    (void)reason; /* reason codes are module-defined; peer resets are counted there */
    fs_conn_t *fc = fs_conn_of(c);
    if (fc == NULL)
        return;
    if (fc->state == FS_CLOSING) { /* follows our own io_close */
        fc->state = FS_FREE;
        return;
    }
    if (fc->conn == c)
        fs_release(fc);
}

/* ---- startup and shutdown -------------------------------------------------- */

static int fs_init(const core_config_t *cfg)
{
    memset(&g_fs, 0, sizeof g_fs);
    if (cfg->chunk_size == 0 || cfg->buf_size == 0 || cfg->max_conns == 0) {
        fprintf(stderr, "filesrv: chunk size, buf size and max conns must be > 0\n");
        return -1;
    }
    if (cfg->docroot == NULL) {
        fprintf(stderr, "filesrv: no document root configured\n");
        return -1;
    }
    g_fs.chunk_size = cfg->chunk_size;
    g_fs.read_size = cfg->buf_size;
    g_fs.max_conns = cfg->max_conns;
    for (size_t i = 0; i < FS_MAX_FILES; i++)
        g_fs.files[i].fd = -1;

    if (fs_load_files(cfg->docroot) != 0)
        return -1;

    /* Per-connection records and carry slots are allocated once here, never
     * inside the event loop. The carry pool is mapped lazily: a page is only
     * touched (and counted in memory use) when a request actually spans more
     * than one read on that slot. */
    g_fs.conns = calloc(g_fs.max_conns, sizeof *g_fs.conns);
    if (g_fs.conns == NULL) {
        fprintf(stderr, "filesrv: out of memory for %zu connections\n", g_fs.max_conns);
        fs_close_files();
        return -1;
    }
    g_fs.carry_pool_size = g_fs.max_conns * (size_t)HTTP_MAX_REQUEST_SIZE;
    void *pool = mmap(NULL, g_fs.carry_pool_size, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (pool == MAP_FAILED) {
        fprintf(stderr, "filesrv: mmap carry pool: %s\n", strerror(errno));
        free(g_fs.conns);
        g_fs.conns = NULL;
        fs_close_files();
        return -1;
    }
    g_fs.carry_pool = pool;

    fprintf(stderr, "filesrv: baseline (pread + io_write), chunk-size %zu, buf-size %zu, "
                    "max-conns %zu, %zu files\n",
            g_fs.chunk_size, g_fs.read_size, g_fs.max_conns, g_fs.file_count);
    return 0;
}

static void fs_print_stats(void)
{
    const fs_stats_t *s = &g_fs.stats;
    fprintf(stderr,
            "filesrv stats: requests %" PRIu64 " 200 %" PRIu64 " 400 %" PRIu64
            " 404 %" PRIu64 " 405 %" PRIu64 " trailing-bytes %" PRIu64
            " body-bytes %" PRIu64 " aborted %" PRIu64 " errors %" PRIu64 "\n",
            s->requests, s->responses_200, s->responses_400, s->responses_404,
            s->responses_405, s->rejected_trailing_bytes, s->body_bytes_sent,
            s->aborted_transfers, s->errors);
}

static void fs_shutdown(void)
{
    fs_print_stats();
    if (g_fs.carry_pool != NULL && munmap(g_fs.carry_pool, g_fs.carry_pool_size) != 0)
        fprintf(stderr, "filesrv: munmap: %s\n", strerror(errno));
    g_fs.carry_pool = NULL;
    free(g_fs.conns);
    g_fs.conns = NULL;
    fs_close_files();
}

int main(int argc, char **argv)
{
    core_config_t cfg;
    if (core_parse_flags(argc, argv, &cfg) != 0) /* ASSUMED core API */
        return 2;
    if (fs_init(&cfg) != 0)
        return 1;

    int rc = 0;
    if (io_init(&cfg) != 0) {
        fprintf(stderr, "filesrv: io_init failed\n");
        rc = 1;
    } else if (io_listen(cfg.port) != 0) {
        fprintf(stderr, "filesrv: io_listen(%d) failed\n", cfg.port);
        rc = 1;
    } else if (io_run() != 0) {
        rc = 1;
    }
    fs_shutdown();
    return rc;
}
