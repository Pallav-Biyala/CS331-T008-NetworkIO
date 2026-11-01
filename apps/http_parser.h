/*
 * http_parser.h - minimal HTTP/1.1 request parser for the file-transfer server.
 *
 * Implements the request subset of plan section 8.1 / 5.4:
 *   GET /<name> HTTP/1.1  (HTTP/1.0 also accepted)
 *   optional headers, terminating blank line, whole request <= 4 KB.
 *
 * The parser is pure: no sockets, no files, no global state. It parses one
 * contiguous byte range that the caller has accumulated (carry + new data),
 * so it never assumes that one socket read equals one request.
 *
 * Policy decisions (documented here because the plan does not fix them):
 *   - Lines must end in CRLF. A bare LF or a CR not followed by LF is a 400,
 *     detected as soon as it is seen (even before the request is complete).
 *   - Request bodies are not supported: "Content-Length: 0" is accepted,
 *     any other Content-Length and any Transfer-Encoding header give 400.
 *   - Methods other than GET (including HEAD) give 405 if the request is
 *     otherwise well formed. Methods are case-sensitive (RFC 9110).
 *   - The request target must be "/" followed by one file name made of
 *     [A-Za-z0-9._-], at most HTTP_MAX_NAME_LEN bytes, not starting with '.',
 *     not containing "..". Anything else ('/', '%', '?', '\\', ...) is a 400.
 *     "GET / HTTP/1.1" parses OK with an empty name (the server answers 404).
 *   - HTTP/1.1 defaults to keep-alive, HTTP/1.0 to close. A "close" token in
 *     any Connection header wins over "keep-alive". Header names and
 *     Connection tokens are compared case-insensitively.
 *   - Host is not required (load generators always send it; hand-written
 *     test clients may not).
 *   - Errors (400/405) always set keep_alive = false: after a rejected
 *     request the byte stream can no longer be trusted, so the server closes.
 */
#ifndef FILESRV_HTTP_PARSER_H
#define FILESRV_HTTP_PARSER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Maximum request size including the terminating CRLF CRLF (plan 5.4, 8.1). */
#define HTTP_MAX_REQUEST_SIZE 4096u

/* Maximum length of a file name in the request target (without the '/'). */
#define HTTP_MAX_NAME_LEN 255u

typedef enum {
    HTTP_PARSE_INCOMPLETE = 0, /* need more bytes; nothing is wrong yet     */
    HTTP_PARSE_OK,             /* a full, valid GET request was parsed       */
    HTTP_PARSE_ERROR           /* reject; req->error_status is 400 or 405    */
} http_parse_result_t;

typedef struct {
    char   name[HTTP_MAX_NAME_LEN + 1]; /* NUL-terminated name, no leading '/' */
    size_t name_len;
    bool   keep_alive;   /* keep the connection open after the response      */
    bool   http_1_0;     /* request line said HTTP/1.0                       */
    size_t request_len;  /* bytes consumed, including the final CRLF CRLF    */
    int    error_status; /* 0 on success, 400 or 405 on HTTP_PARSE_ERROR     */
} http_request_t;

/*
 * Parse the bytes data[0..len). The caller passes everything it has received
 * for the current request so far. On HTTP_PARSE_OK, req->request_len tells
 * how many bytes belonged to the request; bytes beyond that are not part of
 * it (the server treats them as a protocol violation, see filesrv.c).
 * Only the first HTTP_MAX_REQUEST_SIZE bytes are ever examined.
 */
http_parse_result_t http_parse_request(const char *data, size_t len,
                                       http_request_t *req);

/* True if name[0..len) is an acceptable file name (rules above). len 0 is
 * not a valid name. Also used by the server to validate its file table. */
bool http_name_is_valid(const char *name, size_t len);

/* Reason phrase for the status codes this server emits ("" if unknown). */
const char *http_reason_phrase(int status);

/*
 * Write a response header into dst (capacity cap):
 *   HTTP/1.1 <status> <reason>\r\n
 *   Content-Length: <content_length>\r\n
 *   [Allow: GET\r\n]                      (405 only)
 *   Connection: keep-alive|close\r\n
 *   \r\n
 * Returns the header length, or 0 if it does not fit (dst is then unspecified).
 */
size_t http_format_response_header(char *dst, size_t cap, int status,
                                   uint64_t content_length, bool keep_alive);

#endif /* FILESRV_HTTP_PARSER_H */
