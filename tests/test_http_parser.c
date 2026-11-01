/*
 * test_http_parser.c - standalone unit tests for apps/http_parser.c.
 *
 * No networking, no I/O backend, no external framework. Deterministic.
 * Build and run:  make -f tests/parser.mk test
 * Exit status is 0 only if every check passes.
 */
#include "../apps/http_parser.h"

#include <stdio.h>
#include <string.h>

static int g_checks;
static int g_failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            g_failures++;                                                    \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                    \
    } while (0)

/* Parse a NUL-terminated literal. */
static http_parse_result_t parse_str(const char *s, http_request_t *req)
{
    return http_parse_request(s, strlen(s), req);
}

/* Expect an error with the given status for a literal request. */
static void expect_error(const char *s, int status)
{
    http_request_t req;
    http_parse_result_t r = parse_str(s, &req);
    CHECK(r == HTTP_PARSE_ERROR);
    CHECK(req.error_status == status);
    CHECK(!req.keep_alive);
}

/* ---- individual tests ---------------------------------------------------- */

static void test_valid_get(void)
{
    const char *s = "GET /file_64k.bin HTTP/1.1\r\nHost: x\r\nUser-Agent: t\r\n\r\n";
    http_request_t req;
    CHECK(parse_str(s, &req) == HTTP_PARSE_OK);
    CHECK(strcmp(req.name, "file_64k.bin") == 0);
    CHECK(req.name_len == strlen("file_64k.bin"));
    CHECK(req.request_len == strlen(s));
    CHECK(req.keep_alive);     /* HTTP/1.1 default */
    CHECK(!req.http_1_0);
    CHECK(req.error_status == 0);
}

static void test_no_headers(void)
{
    http_request_t req;
    CHECK(parse_str("GET /a HTTP/1.1\r\n\r\n", &req) == HTTP_PARSE_OK);
    CHECK(strcmp(req.name, "a") == 0);
}

static void test_root_is_empty_name(void)
{
    http_request_t req;
    CHECK(parse_str("GET / HTTP/1.1\r\n\r\n", &req) == HTTP_PARSE_OK);
    CHECK(req.name_len == 0); /* server answers 404 */
}

static void test_mixed_case_headers(void)
{
    http_request_t req;
    CHECK(parse_str("GET /a HTTP/1.1\r\nhOsT: x\r\ncOnNeCtIoN: ClOsE\r\n\r\n", &req)
          == HTTP_PARSE_OK);
    CHECK(!req.keep_alive);
    expect_error("GET /a HTTP/1.1\r\nTRANSFER-ENCODING: chunked\r\n\r\n", 400);
    expect_error("GET /a HTTP/1.1\r\ncontent-LENGTH: 5\r\n\r\n", 400);
}

static void test_keep_alive(void)
{
    http_request_t req;
    CHECK(parse_str("GET /a HTTP/1.1\r\nConnection: keep-alive\r\n\r\n", &req) == HTTP_PARSE_OK);
    CHECK(req.keep_alive);
    CHECK(parse_str("GET /a HTTP/1.0\r\nConnection: Keep-Alive\r\n\r\n", &req) == HTTP_PARSE_OK);
    CHECK(req.keep_alive);
    CHECK(req.http_1_0);
    /* HTTP/1.0 without the header defaults to close */
    CHECK(parse_str("GET /a HTTP/1.0\r\n\r\n", &req) == HTTP_PARSE_OK);
    CHECK(!req.keep_alive);
    /* token list with other tokens and whitespace */
    CHECK(parse_str("GET /a HTTP/1.0\r\nConnection:  Upgrade ,keep-alive \r\n\r\n", &req)
          == HTTP_PARSE_OK);
    CHECK(req.keep_alive);
}

static void test_connection_close(void)
{
    http_request_t req;
    CHECK(parse_str("GET /a HTTP/1.1\r\nConnection: close\r\n\r\n", &req) == HTTP_PARSE_OK);
    CHECK(!req.keep_alive);
    /* close wins over keep-alive, also across several headers */
    CHECK(parse_str("GET /a HTTP/1.1\r\nConnection: keep-alive, close\r\n\r\n", &req)
          == HTTP_PARSE_OK);
    CHECK(!req.keep_alive);
    CHECK(parse_str("GET /a HTTP/1.1\r\nConnection: keep-alive\r\nConnection: CLOSE\r\n\r\n", &req)
          == HTTP_PARSE_OK);
    CHECK(!req.keep_alive);
    /* "closed" is not "close" */
    CHECK(parse_str("GET /a HTTP/1.1\r\nConnection: closed\r\n\r\n", &req) == HTTP_PARSE_OK);
    CHECK(req.keep_alive);
}

static void test_unsupported_methods(void)
{
    expect_error("POST /a HTTP/1.1\r\n\r\n", 405);
    expect_error("HEAD /a HTTP/1.1\r\n\r\n", 405);
    expect_error("PUT /a HTTP/1.1\r\nContent-Length: 3\r\n\r\n", 405); /* 405 before body policy */
    expect_error("get /a HTTP/1.1\r\n\r\n", 405);                      /* methods are case-sensitive */
    expect_error("DELETE /../etc/passwd HTTP/1.1\r\n\r\n", 405);
}

static void test_malformed_request_line(void)
{
    expect_error("GET  /a HTTP/1.1\r\n\r\n", 400);    /* double space */
    expect_error("GET /a  HTTP/1.1\r\n\r\n", 400);
    expect_error("GET /a HTTP/1.1 x\r\n\r\n", 400);   /* extra component */
    expect_error("GET /a HTTP/2.0\r\n\r\n", 400);     /* unsupported version */
    expect_error("GET /a http/1.1\r\n\r\n", 400);     /* version is case-sensitive */
    expect_error("G(T /a HTTP/1.1\r\n\r\n", 400);     /* method not a token */
    expect_error("GET a HTTP/1.1\r\n\r\n", 400);      /* target without '/' */
    expect_error("GET http://h/a HTTP/1.1\r\n\r\n", 400); /* absolute-form not supported */
    expect_error("\r\nGET /a HTTP/1.1\r\n\r\n", 400); /* empty request line */
    expect_error("GET /a\tb HTTP/1.1\r\n\r\n", 400);  /* control char in target */
}

static void test_missing_components(void)
{
    expect_error("GET\r\n\r\n", 400);
    expect_error("GET /a\r\n\r\n", 400);
    expect_error("GET /a \r\n\r\n", 400);   /* empty version */
    expect_error(" /a HTTP/1.1\r\n\r\n", 400); /* empty method */
    expect_error("GET  HTTP/1.1\r\n\r\n", 400); /* empty target */
}

static void test_malformed_headers(void)
{
    expect_error("GET /a HTTP/1.1\r\nNoColon\r\n\r\n", 400);
    expect_error("GET /a HTTP/1.1\r\nHost : x\r\n\r\n", 400);   /* space before colon */
    expect_error("GET /a HTTP/1.1\r\n: x\r\n\r\n", 400);        /* empty name */
    expect_error("GET /a HTTP/1.1\r\nHost: x\r\n folded\r\n\r\n", 400); /* obs-fold */
    expect_error("GET /a HTTP/1.1\r\nConnection: close;x\r\n\r\n", 400);
    /* bare LF and lone CR are rejected, even before the request is complete */
    expect_error("GET /a HTTP/1.1\n\n", 400);
    expect_error("GET /a HTTP/1.1\r\nHost: x\n", 400);
    expect_error("GET /a HTTP/1.1\rX", 400);
    {   /* NUL byte inside a header value */
        static const char s[] = "GET /a HTTP/1.1\r\nX: a\0b\r\n\r\n";
        http_request_t req;
        CHECK(http_parse_request(s, sizeof s - 1, &req) == HTTP_PARSE_ERROR);
        CHECK(req.error_status == 400);
    }
}

static void test_incomplete_and_fragmented(void)
{
    const char *s = "GET /file_1m.bin HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
    size_t len = strlen(s);
    http_request_t req;
    /* Every proper prefix is incomplete, never an error (byte-at-a-time, T3). */
    for (size_t i = 0; i < len; i++)
        CHECK(http_parse_request(s, i, &req) == HTTP_PARSE_INCOMPLETE);
    CHECK(http_parse_request(s, len, &req) == HTTP_PARSE_OK);
    CHECK(strcmp(req.name, "file_1m.bin") == 0);
    CHECK(!req.keep_alive);
    CHECK(req.request_len == len);
    CHECK(http_parse_request("", 0, &req) == HTTP_PARSE_INCOMPLETE);
}

static void test_trailing_bytes_reported(void)
{
    /* Two requests in one buffer: the parser reports where the first ends;
     * the server (not the parser) decides to reject the trailing bytes. */
    const char *first = "GET /a HTTP/1.1\r\n\r\n";
    char buf[128];
    snprintf(buf, sizeof buf, "%sGET /b HTTP/1.1\r\n\r\n", first);
    http_request_t req;
    CHECK(parse_str(buf, &req) == HTTP_PARSE_OK);
    CHECK(req.request_len == strlen(first));
    CHECK(strcmp(req.name, "a") == 0);
}

static void test_size_limit(void)
{
    static char buf[HTTP_MAX_REQUEST_SIZE + 64];
    http_request_t req;
    const char *head = "GET /a HTTP/1.1\r\nX-Pad: ";

    /* Exactly 4096 bytes including the final CRLF CRLF: accepted. */
    size_t head_len = strlen(head);
    size_t pad = HTTP_MAX_REQUEST_SIZE - head_len - 4;
    memcpy(buf, head, head_len);
    memset(buf + head_len, 'p', pad);
    memcpy(buf + head_len + pad, "\r\n\r\n", 4);
    CHECK(http_parse_request(buf, HTTP_MAX_REQUEST_SIZE, &req) == HTTP_PARSE_OK);
    CHECK(req.request_len == HTTP_MAX_REQUEST_SIZE);

    /* One byte more: rejected with 400. */
    memset(buf + head_len, 'p', pad + 1);
    memcpy(buf + head_len + pad + 1, "\r\n\r\n", 4);
    CHECK(http_parse_request(buf, HTTP_MAX_REQUEST_SIZE + 1, &req) == HTTP_PARSE_ERROR);
    CHECK(req.error_status == 400);

    /* 4096 bytes with no terminator yet: already oversized, not incomplete. */
    memset(buf, 'A', HTTP_MAX_REQUEST_SIZE);
    memcpy(buf, head, head_len);
    CHECK(http_parse_request(buf, HTTP_MAX_REQUEST_SIZE, &req) == HTTP_PARSE_ERROR);
    CHECK(req.error_status == 400);
    /* 4095 bytes without terminator: still incomplete. */
    CHECK(http_parse_request(buf, HTTP_MAX_REQUEST_SIZE - 1, &req) == HTTP_PARSE_INCOMPLETE);
}

static void test_path_traversal(void)
{
    expect_error("GET /../etc/passwd HTTP/1.1\r\n\r\n", 400);
    expect_error("GET /.. HTTP/1.1\r\n\r\n", 400);
    expect_error("GET /a..b HTTP/1.1\r\n\r\n", 400);
    expect_error("GET /%2e%2e/etc/passwd HTTP/1.1\r\n\r\n", 400);
    expect_error("GET /..%2fetc HTTP/1.1\r\n\r\n", 400);
    expect_error("GET //etc/passwd HTTP/1.1\r\n\r\n", 400);
    expect_error("GET /..\\x HTTP/1.1\r\n\r\n", 400);
}

static void test_invalid_name_components(void)
{
    expect_error("GET /dir/file HTTP/1.1\r\n\r\n", 400);  /* subdirectory */
    expect_error("GET /./a HTTP/1.1\r\n\r\n", 400);
    expect_error("GET /. HTTP/1.1\r\n\r\n", 400);
    expect_error("GET /.hidden HTTP/1.1\r\n\r\n", 400);
    expect_error("GET /a?x=1 HTTP/1.1\r\n\r\n", 400);     /* query not supported */
    expect_error("GET /a#f HTTP/1.1\r\n\r\n", 400);
    expect_error("GET /a\\b HTTP/1.1\r\n\r\n", 400);
    expect_error("GET /a:b HTTP/1.1\r\n\r\n", 400);
    {   /* name longer than HTTP_MAX_NAME_LEN */
        char buf[600];
        int n = snprintf(buf, sizeof buf, "GET /");
        memset(buf + n, 'a', HTTP_MAX_NAME_LEN + 1);
        n += (int)HTTP_MAX_NAME_LEN + 1;
        snprintf(buf + n, sizeof buf - (size_t)n, " HTTP/1.1\r\n\r\n");
        expect_error(buf, 400);
    }

    CHECK(http_name_is_valid("file-4k_v2.bin", 14));
    CHECK(!http_name_is_valid("", 0));
    CHECK(!http_name_is_valid("..", 2));
    CHECK(!http_name_is_valid("a/b", 3));
}

static void test_body_and_transfer_encoding(void)
{
    http_request_t req;
    CHECK(parse_str("GET /a HTTP/1.1\r\nContent-Length: 0\r\n\r\n", &req) == HTTP_PARSE_OK);
    CHECK(parse_str("GET /a HTTP/1.1\r\nContent-Length: 000\r\n\r\n", &req) == HTTP_PARSE_OK);
    expect_error("GET /a HTTP/1.1\r\nContent-Length: 10\r\n\r\n", 400);
    expect_error("GET /a HTTP/1.1\r\nContent-Length: -1\r\n\r\n", 400);
    expect_error("GET /a HTTP/1.1\r\nContent-Length: \r\n\r\n", 400);
    expect_error("GET /a HTTP/1.1\r\nContent-Length: 1x\r\n\r\n", 400);
    expect_error("GET /a HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n", 400);
    expect_error("GET /a HTTP/1.1\r\nTransfer-Encoding: identity\r\n\r\n", 400);
}

static void test_response_header(void)
{
    char buf[256];
    size_t n = http_format_response_header(buf, sizeof buf, 200, 1048576u, true);
    const char *want = "HTTP/1.1 200 OK\r\nContent-Length: 1048576\r\n"
                       "Connection: keep-alive\r\n\r\n";
    CHECK(n == strlen(want));
    CHECK(memcmp(buf, want, n) == 0);

    n = http_format_response_header(buf, sizeof buf, 405, 0, false);
    want = "HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\n"
           "Allow: GET\r\nConnection: close\r\n\r\n";
    CHECK(n == strlen(want));
    CHECK(memcmp(buf, want, n) == 0);

    /* Does not fit: returns 0, never overflows. */
    CHECK(http_format_response_header(buf, 10, 200, 1, true) == 0);
    n = http_format_response_header(buf, sizeof buf, 404, 0, true);
    CHECK(http_format_response_header(buf, n, 404, 0, true) == 0);   /* no room for NUL */
    CHECK(http_format_response_header(buf, n + 1, 404, 0, true) == n);
}

int main(void)
{
    struct { const char *name; void (*fn)(void); } tests[] = {
        {"valid GET", test_valid_get},
        {"no headers", test_no_headers},
        {"root path -> empty name", test_root_is_empty_name},
        {"mixed-case header names/values", test_mixed_case_headers},
        {"keep-alive", test_keep_alive},
        {"Connection: close", test_connection_close},
        {"unsupported methods -> 405", test_unsupported_methods},
        {"malformed request line", test_malformed_request_line},
        {"missing request-line components", test_missing_components},
        {"malformed headers / line endings", test_malformed_headers},
        {"incomplete and fragmented requests", test_incomplete_and_fragmented},
        {"trailing bytes after request", test_trailing_bytes_reported},
        {"4 KB size limit", test_size_limit},
        {"path traversal", test_path_traversal},
        {"invalid name components", test_invalid_name_components},
        {"request body / transfer-encoding", test_body_and_transfer_encoding},
        {"response header formatting", test_response_header},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        int before = g_failures;
        tests[i].fn();
        printf("%-40s %s\n", tests[i].name, g_failures == before ? "ok" : "FAILED");
    }
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
