/*
 * filesrv: a baseline file-transfer application.
 *
 * Serving model
 * -------------
 * A minimal, bounded HTTP/1.1 GET server. It answers GET requests for the
 * regular files that were enumerated at startup in the document root
 * "files/" (relative to the server's working directory), so the server must
 * be started from a directory that contains a "files" directory.
 *
 *   HTTP/1.1 200 OK for a known, safe filename
 *   HTTP/1.1 404 Not Found for a missing or rejected filename
 *   HTTP/1.1 400 Bad Request for malformed or oversized requests
 *   HTTP/1.1 405 Method Not Allowed for a method other than GET
 *
 * Connection handling is sequential keep-alive: one request is parsed and
 * fully answered before the next one on the same connection. Bytes of the
 * next request that arrive together with the current one are preserved in the
 * core carry slot (no concurrent pipelining). The connection is closed once
 * the peer closes, when the request asked for Connection: close, or when a
 * request could not be parsed (400), because the stream cannot be resynchronised.
 *
 * Buffer lifetime (the reason for this exact request ordering)
 * ------------------------------------------------------------
 * io_read()/io_get_buf() return shared scratch storage on select/poll/epoll and
 * a per-connection buffer on io_uring. io_read_file() stages its result in a
 * core slot and copies it into the caller's buffer only when the deferred
 * callback is delivered (readiness modules), while io_uring preads directly
 * into the caller's buffer. io_write() may retain the caller's buffer until
 * on_write_done.
 *
 * Therefore a response is never assembled as "header + file data" in one
 * buffer: request bytes are copied into the carry slot first, then the response
 * header is written into an io_get_buf() buffer and sent; only after its
 * on_write_done callback is a file chunk read with io_read_file() into an
 * io_get_buf() buffer and written back with io_write(). Every buffer is used
 * by at most one outstanding request at a time, which keeps the sequence valid
 * on all four backends.
 *
 * Non-goals: sendfile, mmap, io_uring async reads/splice, zero-copy send,
 * provided buffers, multishot operations, edge-triggered tuning and HTTP
 * pipelining are intentionally not implemented.
 */
#include "../core/core.h"
#include "../io/io.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* Document root, relative to the working directory the server is started in. */
#define FSRV_DOC_ROOT   "files"
#define FSRV_MAX_FILES  16
#define FSRV_MAX_HEADER 4096          /* maximum accepted request-header size   */
#define FSRV_NAME_MAX   256

typedef enum {
    FSRV_RECV = 0,    /* waiting for / parsing a request                        */
    FSRV_WRITE_HDR,   /* the response header is being written                   */
    FSRV_READ_FILE,   /* an io_read_file is in flight                           */
    FSRV_WRITE_BODY,  /* a file chunk is being written                          */
    FSRV_CLOSING      /* closing after the current response                     */
} fsrv_state_t;

struct filesrv_conn {
    fsrv_state_t state;
    int          file_index;   /* index into the file table, -1 if none       */
    off_t        file_off;     /* next file offset to read                    */
    size_t       file_left;    /* bytes of the response body still to send    */
    bool         close_after;  /* close the connection when the body is sent  */
};

struct fsrv_file {
    char  name[FSRV_NAME_MAX];
    int   fd;
    off_t size;
};

typedef struct {
    int  status;       /* 0 = servable, otherwise 400 / 405                    */
    bool keep_alive;   /* keep the connection after this response              */
    int  file_index;   /* >= 0 for a 200 response, -1 otherwise                */
} fsrv_request_t;

static struct fsrv_file g_files[FSRV_MAX_FILES];
static int g_file_count;

static struct filesrv_conn *fsrv_state(conn_t *c)
{
    return (struct filesrv_conn *)c->app_priv;
}

static void fsrv_fail(conn_t *c)
{
    CORE_STAT_INC(errors);
    io_close(c);
}

/* Forward declarations for the mutually recursive response/parse helpers. */
static void begin_response(conn_t *c, int status, const char *reason,
                           int file_index, bool close_after);
static void drain_carry(conn_t *c);
static void request_more(conn_t *c);
static void finish_response(conn_t *c);
static void start_file_read(conn_t *c);
static void handle_request(conn_t *c, const fsrv_request_t *req);

static size_t format_header(uint8_t *dst, size_t cap, int status,
                            const char *reason, unsigned long long body_len,
                            bool close_after)
{
    const char *connection = close_after ? "close" : "keep-alive";
    int written = snprintf((char *)dst, cap,
                           "HTTP/1.1 %d %s\r\n"
                           "Content-Length: %llu\r\n"
                           "Connection: %s\r\n"
                           "\r\n",
                           status, reason, body_len, connection);

    if (written < 0 || (size_t)written >= cap)
        return (size_t)-1;
    return (size_t)written;
}

/* A request path is a single filename directly under the document root. Slashes,
 * backslashes, percent escapes, control characters and the dot entries are
 * rejected so no request can name a file the startup table does not hold. */
static bool safe_filename(const uint8_t *name, size_t len)
{
    size_t i;

    if (len == 0 || len >= FSRV_NAME_MAX)
        return false;
    if ((len == 1 && name[0] == '.') ||
        (len == 2 && name[0] == '.' && name[1] == '.'))
        return false;
    for (i = 0; i < len; i++) {
        unsigned char ch = name[i];
        if (ch < 0x21 || ch == 0x7f || ch == '/' || ch == '\\' || ch == '%')
            return false;
    }
    return true;
}

static int lookup_file(const uint8_t *name, size_t len)
{
    int i;

    for (i = 0; i < g_file_count; i++) {
        if (strlen(g_files[i].name) == len &&
            memcmp(g_files[i].name, name, len) == 0)
            return i;
    }
    return -1;
}

static size_t find_header_end(const uint8_t *buf, size_t len)
{
    size_t i;

    if (buf == NULL)
        return 0;
    for (i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' &&
            buf[i + 2] == '\r' && buf[i + 3] == '\n')
            return i + 4;
    }
    return 0;
}

static bool is_http_token_char(unsigned char ch)
{
    if ((ch >= '0' && ch <= '9') ||
        (ch >= 'A' && ch <= 'Z') ||
        (ch >= 'a' && ch <= 'z'))
        return true;

    switch (ch) {
    case '!':
    case '#':
    case '$':
    case '%':
    case '&':
    case '\'':
    case '*':
    case '+':
    case '-':
    case '.':
    case '^':
    case '_':
    case '`':
    case '|':
    case '~':
        return true;
    default:
        return false;
    }
}

static void parse_request(const uint8_t *data, size_t len,
                          fsrv_request_t *req)
{
    size_t line_end = 0;
    size_t p;
    size_t i;
    size_t target_start;
    size_t target_len;
    size_t method_len;
    bool found = false;
    bool is_get;
    bool saw_connection_close = false;
    const uint8_t *target;

    if (req == NULL)
        return;

    req->status = 0;
    req->keep_alive = true;
    req->file_index = -1;

    if (data == NULL) {
        req->status = 400;
        return;
    }

    /*
     * The caller supplies exactly one complete header block, including
     * the final CRLFCRLF. Reject incomplete or oversized input.
     */
    if (len < 4 || len > FSRV_MAX_HEADER ||
        data[len - 4] != '\r' || data[len - 3] != '\n' ||
        data[len - 2] != '\r' || data[len - 1] != '\n') {
        req->status = 400;
        return;
    }

    /* Locate the request-line CRLF. */
    for (i = 0; i + 1 < len; i++) {
        if (data[i] == '\r' && data[i + 1] == '\n') {
            line_end = i;
            found = true;
            break;
        }

        /* Bare LF and bare CR are invalid in a request line. */
        if (data[i] == '\n' ||
            (data[i] == '\r' &&
             (i + 1 >= len || data[i + 1] != '\n'))) {
            req->status = 400;
            return;
        }
    }

    if (!found || line_end == 0) {
        req->status = 400;
        return;
    }

    /*
     * Parse exactly:
     *     METHOD SP request-target SP HTTP/1.1
     *
     * Extra spaces and tabs in the request line are rejected.
     */
    p = 0;
    while (p < line_end && data[p] != ' ')
        p++;

    if (p == 0 || p >= line_end) {
        req->status = 400;
        return;
    }

    method_len = p;

    for (i = 0; i < method_len; i++) {
        if (!is_http_token_char(data[i])) {
            req->status = 400;
            return;
        }
    }

    is_get = method_len == 3 &&
             memcmp(data, "GET", 3) == 0;

    p++; /* Skip the single space after METHOD. */
    target_start = p;

    while (p < line_end && data[p] != ' ')
        p++;

    if (p == target_start || p >= line_end) {
        req->status = 400;
        return;
    }

    target = data + target_start;
    target_len = p - target_start;

    p++; /* Skip the single space before HTTP-version. */

    if (p >= line_end) {
        req->status = 400;
        return;
    }

    /* The version must be exactly HTTP/1.1, with no trailing spaces. */
    if (line_end - p != 8 ||
        memcmp(data + p, "HTTP/1.1", 8) != 0) {
        req->status = 400;
        return;
    }

    /*
     * Validate all header lines before deciding whether the method
     * is supported. This ensures malformed POST requests get 400,
     * and valid unsupported methods can still honor Connection: close.
     */
    i = line_end + 2;

    while (i < len - 2) {
        size_t e = i;
        size_t colon;
        size_t v;
        size_t ve;

        /* Find the end of this header line. */
        while (e + 1 < len &&
               !(data[e] == '\r' && data[e + 1] == '\n')) {
            if (data[e] == '\r' || data[e] == '\n') {
                req->status = 400;
                return;
            }
            e++;
        }

        if (e + 1 >= len) {
            req->status = 400;
            return;
        }

        /* Empty line terminates the header section. */
        if (e == i)
            break;

        /* Header names must be nonempty tokens immediately followed by ':'. */
        colon = i;
        while (colon < e && data[colon] != ':')
            colon++;

        if (colon == i || colon >= e) {
            req->status = 400;
            return;
        }

        {
            size_t j;

            for (j = i; j < colon; j++) {
                if (!is_http_token_char(data[j])) {
                    req->status = 400;
                    return;
                }
            }
        }

        /* Reject forbidden control characters in the field value. */
        for (v = colon + 1; v < e; v++) {
            unsigned char ch = data[v];

            if ((ch < 0x20 && ch != '\t') || ch == 0x7f) {
                req->status = 400;
                return;
            }
        }

        /* Trim optional whitespace around the field value. */
        v = colon + 1;
        ve = e;

        while (v < ve && (data[v] == ' ' || data[v] == '\t'))
            v++;

        while (ve > v &&
               (data[ve - 1] == ' ' || data[ve - 1] == '\t'))
            ve--;

        if (colon - i == 10 &&
            strncasecmp((const char *)data + i, "Connection", 10) == 0) {
            /*
             * Recognize comma-separated Connection tokens.
             * If any token is "close", close wins even if another
             * Connection header says "keep-alive".
             */
            size_t token_start = v;

            while (token_start < ve) {
                size_t token_end = token_start;

                while (token_end < ve && data[token_end] != ',')
                    token_end++;

                while (token_start < token_end &&
                       (data[token_start] == ' ' ||
                        data[token_start] == '\t'))
                    token_start++;

                while (token_end > token_start &&
                       (data[token_end - 1] == ' ' ||
                        data[token_end - 1] == '\t'))
                    token_end--;

                if (token_end - token_start == 5 &&
                    strncasecmp((const char *)data + token_start,
                                "close", 5) == 0) {
                    saw_connection_close = true;
                }

                if (token_end == ve)
                    break;

                token_start = token_end + 1;
            }
        }

        i = e + 2;
    }

    req->keep_alive = !saw_connection_close;

    /* A valid but unsupported method gets 405 only after header parsing. */
    if (!is_get) {
        req->status = 405;
        return;
    }

    /* This server only supports a single filename under the document root. */
    if (target_len < 1 || target[0] != '/') {
        req->status = 400;
        return;
    }

    if (!safe_filename(target + 1, target_len - 1)) {
        req->file_index = -1; /* Rejected filename -> 404. */
        return;
    }

    req->file_index = lookup_file(target + 1, target_len - 1);
}

static void begin_response(conn_t *c, int status, const char *reason,
                           int file_index, bool close_after)
{
    struct filesrv_conn *st = fsrv_state(c);
    unsigned long long body_len = 0;
    uint8_t *buf;
    size_t cap = 0;
    size_t header_len;

    if (file_index >= 0)
        body_len = (unsigned long long)g_files[file_index].size;

    buf = io_get_buf(c, &cap);
    if (buf == NULL) {
        fsrv_fail(c);
        return;
    }
    header_len = format_header(buf, cap, status, reason, body_len, close_after);
    if (header_len == (size_t)-1) {
        fsrv_fail(c);
        return;
    }

    st->state = FSRV_WRITE_HDR;
    st->file_index = file_index;
    st->file_off = 0;
    st->file_left = file_index >= 0 ? (size_t)g_files[file_index].size : 0;
    st->close_after = close_after;

    if (io_write(c, buf, header_len) < 0)
        fsrv_fail(c);
}

static void handle_request(conn_t *c, const fsrv_request_t *req)
{
    if (req->status == 400) {
        begin_response(c, 400, "Bad Request", -1, true);
        return;
    }
    if (req->status == 405) {
        begin_response(c, 405, "Method Not Allowed", -1, !req->keep_alive);
        return;
    }
    if (req->file_index < 0) {
        begin_response(c, 404, "Not Found", -1, !req->keep_alive);
        return;
    }
    begin_response(c, 200, "OK", req->file_index, !req->keep_alive);
}

static void request_more(conn_t *c)
{
    size_t cap = g_cfg->buf_size;
    size_t carry_len = c->carry_len;

    if (carry_len >= cap) {
        /* No room left for another byte and still no terminator: oversized. */
        begin_response(c, 400, "Bad Request", -1, true);
        return;
    }
    fsrv_state(c)->state = FSRV_RECV;
    /* Never ask for more than the carry slot can hold without overflowing. */
    if (io_read(c, cap - carry_len) < 0)
        fsrv_fail(c);
}

static void drain_carry(conn_t *c)
{
    size_t carry_len = c->carry_len;
    size_t scan = carry_len < FSRV_MAX_HEADER ? carry_len : FSRV_MAX_HEADER;
    size_t header_len = find_header_end(c->carry, scan);

    if (header_len != 0) {
        fsrv_request_t req;

        if (header_len > FSRV_MAX_HEADER) {
            begin_response(c, 400, "Bad Request", -1, true);
            return;
        }
        parse_request(c->carry, header_len, &req);
        core_carry_consume(c, header_len); /* keep any pipelined remainder */
        handle_request(c, &req);
        return;
    }
    if (carry_len >= FSRV_MAX_HEADER) {
        begin_response(c, 400, "Bad Request", -1, true);
        return;
    }
    request_more(c);
}

static void start_file_read(conn_t *c)
{
    struct filesrv_conn *st = fsrv_state(c);
    size_t limit = g_cfg->buf_size;
    size_t chunk = (size_t)st->file_left;
    uint8_t *buf;
    size_t cap = 0;

    if (g_cfg->chunk_size != 0 && g_cfg->chunk_size < limit)
        limit = g_cfg->chunk_size;
    if (chunk > limit)
        chunk = limit;
    if (chunk == 0) {
        finish_response(c);
        return;
    }

    buf = io_get_buf(c, &cap);
    if (buf == NULL || chunk > cap) {
        fsrv_fail(c);
        return;
    }
    st->state = FSRV_READ_FILE;
    if (io_read_file(c, buf, chunk, g_files[st->file_index].fd, st->file_off) < 0)
        fsrv_fail(c);
}

static void finish_response(conn_t *c)
{
    struct filesrv_conn *st = fsrv_state(c);

    CORE_STAT_INC(messages);
    st->file_index = -1;
    st->file_off = 0;
    st->file_left = 0;
    if (st->close_after) {
        st->state = FSRV_CLOSING;
        io_close(c);
        return;
    }
    st->state = FSRV_RECV;
    drain_carry(c); /* serve a request already buffered behind this one */
}

/* ------------------------------------------------------------------ */
/* Startup: open the document root once and cache files and sizes.     */
/* ------------------------------------------------------------------ */

static void close_cached_files(void)
{
    int i;

    for (i = 0; i < g_file_count; i++) {
        if (g_files[i].fd >= 0)
            (void)close(g_files[i].fd);
        g_files[i].fd = -1;
        g_files[i].name[0] = '\0';
        g_files[i].size = 0;
    }
    g_file_count = 0;
}

int app_init(const core_config_t *cfg)
{
    DIR *dir;
    struct dirent *entry;
    int dir_fd;
    int count = 0;

    (void)cfg;
    g_file_count = 0;
    dir = opendir(FSRV_DOC_ROOT);
    if (dir == NULL) {
        LOG_ERR("cannot open document root '%s': %s", FSRV_DOC_ROOT, strerror(errno));
        return -1;
    }
    dir_fd = dirfd(dir);
    if (dir_fd < 0) {
        LOG_ERR("cannot obtain directory handle for '%s': %s", FSRV_DOC_ROOT, strerror(errno));
        (void)closedir(dir);
        return -1;
    }

    while ((entry = readdir(dir)) != NULL) {
        struct stat info;
        size_t name_len;
        int fd;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        /* AT_SYMLINK_NOFOLLOW: a symlink is not a regular file, so it is skipped
         * and the table can only ever reference real files inside the root. */
        if (fstatat(dir_fd, entry->d_name, &info, AT_SYMLINK_NOFOLLOW) != 0) {
            LOG_ERR("cannot stat '%s': %s", entry->d_name, strerror(errno));
            (void)closedir(dir);
            close_cached_files();
            return -1;
        }
        if (!S_ISREG(info.st_mode))
            continue;

        /* More regular files than the table can hold: fail rather than truncate. */
        if (count >= FSRV_MAX_FILES) {
            LOG_ERR("document root '%s' holds more than %d regular files",
                    FSRV_DOC_ROOT, FSRV_MAX_FILES);
            (void)closedir(dir);
            close_cached_files();
            return -1;
        }
        name_len = strlen(entry->d_name);
        if (name_len == 0 || name_len >= FSRV_NAME_MAX) {
            LOG_ERR("filename in '%s' is too long to cache", FSRV_DOC_ROOT);
            (void)closedir(dir);
            close_cached_files();
            return -1;
        }
        fd = openat(dir_fd, entry->d_name,
            O_RDONLY | O_CLOEXEC | O_NOFOLLOW);

        if (fd < 0) {
            LOG_ERR("cannot open '%s': %s",
                    entry->d_name, strerror(errno));
            (void)closedir(dir);
            close_cached_files();
            return -1;
        }

        /* Validate the file we actually opened. */
        if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) ||
            info.st_size < 0) {
            LOG_ERR("'%s' is not a valid regular file", entry->d_name);
            (void)close(fd);
            (void)closedir(dir);
            close_cached_files();
            return -1;
        }

        memcpy(g_files[count].name, entry->d_name, name_len + 1);
        g_files[count].fd = fd;
        g_files[count].size = info.st_size;

        count++;
        g_file_count = count;
    }

    (void)closedir(dir);
    g_file_count = count;
    LOG_INFO("filesrv serving %d file(s) from '%s'", g_file_count, FSRV_DOC_ROOT);
    return 0;
}

void app_shutdown(void)
{
    close_cached_files();
}

/* ------------------------------------------------------------------ */
/* Connection callbacks                                                */
/* ------------------------------------------------------------------ */

void on_accept(conn_t *c)
{
    struct filesrv_conn *st = fsrv_state(c);

    st->state = FSRV_RECV;
    st->file_index = -1;
    st->file_off = 0;
    st->file_left = 0;
    st->close_after = false;

    drain_carry(c); /* carry is empty; this issues the first io_read */
}

void on_read_done(conn_t *c, uint8_t *buf, ssize_t n)
{
    if (fsrv_state(c)->state != FSRV_RECV) {
        fsrv_fail(c);
        return;
    }
    if (n == 0) {
        core_carry_clear(c);
        io_close(c);
        return;
    }
    if (n < 0 || (size_t)n > g_cfg->buf_size) {
        fsrv_fail(c);
        return;
    }
    if (core_carry_append(c, buf, (size_t)n) < 0) {
        begin_response(c, 400, "Bad Request", -1, true);
        return;
    }
    drain_carry(c);
}

void on_write_done(conn_t *c)
{
    struct filesrv_conn *st = fsrv_state(c);

    if (st->state != FSRV_WRITE_HDR && st->state != FSRV_WRITE_BODY) {
        fsrv_fail(c);
        return;
    }
    if (st->file_left > 0)
        start_file_read(c);
    else
        finish_response(c);
}

void on_file_read_done(conn_t *c, uint8_t *buf, ssize_t n)
{
    struct filesrv_conn *st = fsrv_state(c);

    if (st->state != FSRV_READ_FILE || n <= 0 || (size_t)n > st->file_left) {
        /* Short read or shrinking file: the promised Content-Length cannot be met. */
        fsrv_fail(c);
        return;
    }
    st->file_off += n;
    st->file_left -= (size_t)n;
    st->state = FSRV_WRITE_BODY;
    if (io_write(c, buf, (size_t)n) < 0)
        fsrv_fail(c);
}

void on_close(conn_t *c, io_close_reason_t reason)
{
    struct filesrv_conn *st = fsrv_state(c);

    (void)reason;
    core_carry_clear(c);
    st->state = FSRV_CLOSING;
    st->file_index = -1;
    st->file_off = 0;
    st->file_left = 0;
}

int main(int argc, char **argv)
{
    return core_main(argc, argv, "filesrv", 65536, sizeof(struct filesrv_conn));
}
