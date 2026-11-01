/*
 * http_parser.c - see http_parser.h for the supported subset and policies.
 *
 * Structure: first find the end of the header block (CRLF CRLF) inside the
 * 4 KB window, checking line endings on the way. Only when the block is
 * complete are the request line and headers validated, in this order:
 *   1. request-line syntax and version          -> 400
 *   2. header syntax                             -> 400
 *   3. method (only GET)                         -> 405
 *   4. body / transfer-encoding policy           -> 400
 *   5. request-target / file name                -> 400
 * so that a well-formed POST gets 405 even if it announces a body.
 *
 * All indexing is bounded by the lengths passed in; no function here relies
 * on NUL termination of the input.
 */
#include "http_parser.h"

#include <stdio.h>
#include <string.h>

/* ---- character classes (RFC 9110 5.6.2, 5.5) ---------------------------- */

static bool is_tchar(unsigned char c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
        return true;
    return c != '\0' && strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

/* Allowed in a header value: VCHAR, obs-text, SP, HTAB. */
static bool is_field_value_char(unsigned char c)
{
    return c == ' ' || c == '\t' || (c >= 0x21 && c != 0x7f);
}

static bool is_ows(unsigned char c) { return c == ' ' || c == '\t'; }

static unsigned char ascii_lower(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') ? (unsigned char)(c - 'A' + 'a') : c;
}

/* Case-insensitive compare of a length-delimited span with a lowercase literal. */
static bool span_equals_ci(const char *s, size_t len, const char *lower_literal)
{
    size_t lit_len = strlen(lower_literal);
    if (len != lit_len)
        return false;
    for (size_t i = 0; i < len; i++)
        if (ascii_lower((unsigned char)s[i]) != (unsigned char)lower_literal[i])
            return false;
    return true;
}

static bool span_equals(const char *s, size_t len, const char *literal)
{
    return len == strlen(literal) && memcmp(s, literal, len) == 0;
}

/* ---- public helpers ------------------------------------------------------ */

bool http_name_is_valid(const char *name, size_t len)
{
    if (len == 0 || len > HTTP_MAX_NAME_LEN)
        return false;
    if (name[0] == '.') /* rejects ".", "..", hidden files */
        return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok)
            return false;
        if (c == '.' && i + 1 < len && name[i + 1] == '.')
            return false; /* any ".." is rejected (plan 8.1) */
    }
    return true;
}

const char *http_reason_phrase(int status)
{
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    default:  return "";
    }
}

size_t http_format_response_header(char *dst, size_t cap, int status,
                                   uint64_t content_length, bool keep_alive)
{
    if (dst == NULL || cap == 0)
        return 0;
    int n = snprintf(dst, cap,
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Length: %llu\r\n"
                     "%s"
                     "Connection: %s\r\n"
                     "\r\n",
                     status, http_reason_phrase(status),
                     (unsigned long long)content_length,
                     status == 405 ? "Allow: GET\r\n" : "",
                     keep_alive ? "keep-alive" : "close");
    /* snprintf needs room for its NUL; n == cap means truncated. */
    if (n < 0 || (size_t)n >= cap)
        return 0;
    return (size_t)n;
}

/* ---- parsing ------------------------------------------------------------- */

static http_parse_result_t fail(http_request_t *req, int status)
{
    req->error_status = status;
    req->keep_alive = false;
    return HTTP_PARSE_ERROR;
}

/* Header facts collected while walking the header lines. */
typedef struct {
    bool saw_close;
    bool saw_keep_alive;
    bool has_body; /* nonzero Content-Length */
    bool has_transfer_encoding;
} header_facts_t;

/* Parse a comma-separated Connection value; tokens are case-insensitive. */
static bool parse_connection_value(const char *v, size_t len, header_facts_t *f)
{
    size_t i = 0;
    while (i < len) {
        while (i < len && (is_ows((unsigned char)v[i]) || v[i] == ','))
            i++;
        size_t start = i;
        while (i < len && v[i] != ',' && !is_ows((unsigned char)v[i]))
            i++;
        size_t tok_len = i - start;
        if (tok_len == 0)
            continue;
        for (size_t k = start; k < i; k++)
            if (!is_tchar((unsigned char)v[k]))
                return false;
        if (span_equals_ci(v + start, tok_len, "close"))
            f->saw_close = true;
        else if (span_equals_ci(v + start, tok_len, "keep-alive"))
            f->saw_keep_alive = true;
        /* other tokens (e.g. "Upgrade") are ignored */
        while (i < len && is_ows((unsigned char)v[i]))
            i++;
        if (i < len && v[i] != ',')
            return false; /* junk between tokens */
    }
    return true;
}

/* Content-Length must be 1*DIGIT. Returns false on syntax error. */
static bool parse_content_length(const char *v, size_t len, bool *nonzero)
{
    if (len == 0)
        return false;
    *nonzero = false;
    for (size_t i = 0; i < len; i++) {
        if (v[i] < '0' || v[i] > '9')
            return false;
        if (v[i] != '0')
            *nonzero = true;
    }
    return true;
}

/* Parse one header line [line, line+len) (no CRLF). Returns false on 400. */
static bool parse_header_line(const char *line, size_t len, header_facts_t *f)
{
    /* Obsolete line folding (continuation line) is rejected (RFC 9112 5.2). */
    if (len == 0 || is_ows((unsigned char)line[0]))
        return false;

    size_t colon = 0;
    while (colon < len && is_tchar((unsigned char)line[colon]))
        colon++;
    /* Name must be non-empty and followed directly by ':' (no whitespace). */
    if (colon == 0 || colon >= len || line[colon] != ':')
        return false;

    const char *name = line;
    size_t name_len = colon;
    size_t vs = colon + 1, ve = len;
    while (vs < ve && is_ows((unsigned char)line[vs]))
        vs++;
    while (ve > vs && is_ows((unsigned char)line[ve - 1]))
        ve--;
    for (size_t i = vs; i < ve; i++)
        if (!is_field_value_char((unsigned char)line[i]))
            return false;

    const char *value = line + vs;
    size_t value_len = ve - vs;

    if (span_equals_ci(name, name_len, "connection")) {
        if (!parse_connection_value(value, value_len, f))
            return false;
    } else if (span_equals_ci(name, name_len, "content-length")) {
        bool nonzero;
        if (!parse_content_length(value, value_len, &nonzero))
            return false;
        if (nonzero)
            f->has_body = true;
    } else if (span_equals_ci(name, name_len, "transfer-encoding")) {
        f->has_transfer_encoding = true;
    }
    return true;
}

/*
 * Find the end of the header block within the 4 KB window and check CRLF
 * discipline on the way. Returns OK with *end = index just past CRLF CRLF,
 * INCOMPLETE if more bytes may still complete it, or ERROR.
 */
static http_parse_result_t find_header_end(const char *data, size_t len, size_t *end)
{
    size_t window = len < HTTP_MAX_REQUEST_SIZE ? len : HTTP_MAX_REQUEST_SIZE;

    for (size_t i = 0; i < window; i++) {
        if (data[i] == '\n')
            return HTTP_PARSE_ERROR; /* bare LF: every LF is consumed after a CR below */
        if (data[i] != '\r')
            continue;
        if (i + 1 >= window)
            break; /* CR is the last byte we may look at */
        if (data[i + 1] != '\n')
            return HTTP_PARSE_ERROR; /* CR not followed by LF */
        /* CRLF at i. Blank line if it starts a line: at 0 the request line
         * itself is empty, which is invalid; after a CRLF it ends the block. */
        if (i == 0)
            return HTTP_PARSE_ERROR;
        if (i >= 2 && data[i - 2] == '\r' && data[i - 1] == '\n') {
            *end = i + 2;
            return HTTP_PARSE_OK;
        }
        i++; /* skip the LF we just validated */
    }
    /* No terminator in the window. If the window is full, the request is
     * oversized; otherwise wait for more bytes. */
    return len >= HTTP_MAX_REQUEST_SIZE ? HTTP_PARSE_ERROR : HTTP_PARSE_INCOMPLETE;
}

http_parse_result_t http_parse_request(const char *data, size_t len, http_request_t *req)
{
    memset(req, 0, sizeof *req);
    if (data == NULL)
        return fail(req, 400);

    size_t end;
    http_parse_result_t r = find_header_end(data, len, &end);
    if (r == HTTP_PARSE_INCOMPLETE)
        return r;
    if (r == HTTP_PARSE_ERROR)
        return fail(req, 400);

    /* ---- request line: method SP target SP version CRLF ---- */
    const char *line = data;
    size_t line_len = 0;
    while (line[line_len] != '\r') /* a CR exists before end (validated above) */
        line_len++;

    const char *sp1 = memchr(line, ' ', line_len);
    if (sp1 == NULL)
        return fail(req, 400);
    size_t method_len = (size_t)(sp1 - line);
    const char *target = sp1 + 1;
    size_t rest = line_len - method_len - 1;
    const char *sp2 = memchr(target, ' ', rest);
    if (sp2 == NULL)
        return fail(req, 400);
    size_t target_len = (size_t)(sp2 - target);
    const char *version = sp2 + 1;
    size_t version_len = rest - target_len - 1;

    if (method_len == 0 || target_len == 0 || version_len == 0)
        return fail(req, 400);
    if (memchr(version, ' ', version_len) != NULL)
        return fail(req, 400); /* extra component */
    for (size_t i = 0; i < method_len; i++)
        if (!is_tchar((unsigned char)line[i]))
            return fail(req, 400);
    for (size_t i = 0; i < target_len; i++) {
        unsigned char c = (unsigned char)target[i];
        if (c <= 0x20 || c == 0x7f)
            return fail(req, 400);
    }

    if (span_equals(version, version_len, "HTTP/1.1"))
        req->http_1_0 = false;
    else if (span_equals(version, version_len, "HTTP/1.0"))
        req->http_1_0 = true;
    else
        return fail(req, 400);

    /* ---- header lines ---- */
    header_facts_t facts = {0};
    size_t pos = line_len + 2;
    while (pos < end - 2) { /* end - 2 is the final blank line's CRLF */
        size_t hl = 0;
        while (data[pos + hl] != '\r')
            hl++;
        if (!parse_header_line(data + pos, hl, &facts))
            return fail(req, 400);
        pos += hl + 2;
    }

    /* ---- method ---- */
    if (!span_equals(line, method_len, "GET"))
        return fail(req, 405);

    /* ---- body policy: GET with a body is not supported ---- */
    if (facts.has_transfer_encoding || facts.has_body)
        return fail(req, 400);

    /* ---- target: "/" or "/<valid name>" ---- */
    if (target[0] != '/')
        return fail(req, 400);
    const char *name = target + 1;
    size_t name_len = target_len - 1;
    if (name_len > 0 && !http_name_is_valid(name, name_len))
        return fail(req, 400);
    memcpy(req->name, name, name_len);
    req->name[name_len] = '\0';
    req->name_len = name_len;

    req->keep_alive = req->http_1_0 ? (facts.saw_keep_alive && !facts.saw_close)
                                    : !facts.saw_close;
    req->request_len = end;
    req->error_status = 0;
    return HTTP_PARSE_OK;
}
