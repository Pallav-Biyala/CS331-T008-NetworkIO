/*
 * Assumptions and unresolved design details:
 * - The plan has no default --port value, so the implementation uses 8080.
 * - The carry and output slot pool has two slots per connection, enough for
 *   the two core-owned slots present in conn_t. Pool exhaustion is reported.
 * - Stats are emitted as one JSON object per line, with app/io, full config,
 *   and cumulative counters; --stats-file is append-only for snapshots.
 * - core_carry_consume(c, n) consumes up to all available carry bytes when n
 *   exceeds carry_len, because its interface has no error return.
 * - Only the I/O module calls core_conn_free, and only after canceling or
 *   completing requests; core.c has no in-flight request field to inspect.
 *   Read/write partial-transfer and completion lifetimes remain the module's
 *   responsibility under io.h; core.c does not issue I/O requests.
 * - core_print_flags writes its one-line startup summary to stderr so stdout
 *   remains usable for JSON stats.
 * - The common listener binds an IPv4 wildcard address; the plan does not
 *   specify an IPv6 listener or dual-stack behavior.
 * - The fixed core.h has a nested comment that triggers -Wcomment; that one
 *   diagnostic is suppressed only while including the fixed headers.
 * - Section 12.3's [SETUP] machine tuning is an external benchmark step and
 *   has no per-machine values to encode in this shared core implementation.
 */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcomment"
#include "core.h"
#include "../io/io.h"
#pragma GCC diagnostic pop

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef SO_BUSY_POLL
#define SO_BUSY_POLL 46
#endif

const core_config_t *g_cfg;
core_stats_t g_stats;
volatile sig_atomic_t g_shutdown;
volatile sig_atomic_t g_dump_stats;
volatile sig_atomic_t g_reset_connection_peak;

static core_config_t active_cfg;
static conn_t *connections;
static uint32_t *free_conn_ids;
static uint32_t free_conn_count;
static uint32_t allocated_conn_count;
static uint32_t active_conn_count;
static uint32_t peak_conn_count;
static uint8_t *app_priv_storage;
static uint8_t *io_priv_storage;
static size_t app_priv_size;
static size_t module_priv_size;
static void *slot_mapping;
static size_t slot_mapping_size;
static uint32_t *free_slot_ids;
static uint8_t *slot_used;
static uint32_t slot_capacity;
static uint32_t free_slot_count;
static uint32_t baseline_buf_size;
static FILE *stats_file_stream;
static const char *active_app_name = "unknown";
static const char *active_io_name = "unknown";

static void
set_error(char *err, size_t errlen, const char *fmt, ...)
{
    va_list ap;

    if (err == NULL || errlen == 0) {
        return;
    }
    va_start(ap, fmt);
    (void)vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

static int
parse_u64(const char *text, uint64_t max_value, uint64_t *value)
{
    char *end = NULL;
    unsigned long long parsed;

    if (text == NULL || *text == '\0' || *text == '-') {
        return -1;
    }
    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0' || parsed > max_value) {
        return -1;
    }
    *value = (uint64_t)parsed;
    return 0;
}

static int
parse_int_value(const char *text, int min_value, int max_value, int *value)
{
    char *end = NULL;
    long parsed;

    if (text == NULL || *text == '\0') {
        return -1;
    }
    errno = 0;
    parsed = strtol(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0' ||
        parsed < min_value || parsed > max_value) {
        return -1;
    }
    *value = (int)parsed;
    return 0;
}

static int
parse_bool_value(const char *text, bool *value)
{
    if (text == NULL || strcmp(text, "1") == 0 || strcmp(text, "true") == 0 ||
        strcmp(text, "on") == 0) {
        *value = true;
        return 0;
    }
    if (strcmp(text, "0") == 0 || strcmp(text, "false") == 0 ||
        strcmp(text, "off") == 0) {
        *value = false;
        return 0;
    }
    return -1;
}

enum option_id {
    OPT_NODELAY = 1000,
    OPT_PORT,
    OPT_MAX_CONNS,
    OPT_BUF_SIZE,
    OPT_BACKLOG,
    OPT_RCVBUF,
    OPT_SNDBUF,
    OPT_ACCEPT_BATCH,
    OPT_BUSY_POLL,
    OPT_CPU,
    OPT_STATS_FILE,
    OPT_MSG_SIZE,
    OPT_CHUNK_SIZE,
    OPT_SENDFILE,
    OPT_FILE_READ,
    OPT_EPOLL_ET,
    OPT_EPOLL_MAXEVENTS,
    OPT_EPOLL_ONESHOT,
    OPT_URING_ENTRIES,
    OPT_URING_CQ_ENTRIES,
    OPT_URING_SUBMIT_BATCH,
    OPT_URING_FIXED_FILES,
    OPT_URING_PROVIDED_BUFFERS,
    OPT_URING_MULTISHOT_ACCEPT,
    OPT_URING_MULTISHOT_RECV,
    OPT_URING_FIXED_BUFFERS,
    OPT_URING_SINGLE_ISSUER,
    OPT_URING_DEFER_TASKRUN,
    OPT_URING_COOP_TASKRUN,
    OPT_URING_REGISTER_RING_FD,
    OPT_URING_POLL_FIRST,
    OPT_URING_SEND_ZC,
    OPT_URING_SQPOLL,
    OPT_URING_SQPOLL_CPU,
    OPT_URING_SQPOLL_IDLE,
    OPT_URING_ASYNC_FILE_READ,
    OPT_URING_SPLICE
};

static const struct option long_options[] = {
    {"nodelay", optional_argument, NULL, OPT_NODELAY},
    {"port", required_argument, NULL, OPT_PORT},
    {"max-conns", required_argument, NULL, OPT_MAX_CONNS},
    {"buf-size", required_argument, NULL, OPT_BUF_SIZE},
    {"backlog", required_argument, NULL, OPT_BACKLOG},
    {"rcvbuf", required_argument, NULL, OPT_RCVBUF},
    {"sndbuf", required_argument, NULL, OPT_SNDBUF},
    {"accept-batch", required_argument, NULL, OPT_ACCEPT_BATCH},
    {"busy-poll", required_argument, NULL, OPT_BUSY_POLL},
    {"cpu", required_argument, NULL, OPT_CPU},
    {"stats-file", required_argument, NULL, OPT_STATS_FILE},
    {"msg-size", required_argument, NULL, OPT_MSG_SIZE},
    {"chunk-size", required_argument, NULL, OPT_CHUNK_SIZE},
    {"sendfile", no_argument, NULL, OPT_SENDFILE},
    {"file-read", required_argument, NULL, OPT_FILE_READ},
    {"epoll-et", no_argument, NULL, OPT_EPOLL_ET},
    {"epoll-maxevents", required_argument, NULL, OPT_EPOLL_MAXEVENTS},
    {"epoll-oneshot", no_argument, NULL, OPT_EPOLL_ONESHOT},
    {"uring-entries", required_argument, NULL, OPT_URING_ENTRIES},
    {"uring-cq-entries", required_argument, NULL, OPT_URING_CQ_ENTRIES},
    {"uring-submit-batch", required_argument, NULL, OPT_URING_SUBMIT_BATCH},
    {"uring-fixed-files", no_argument, NULL, OPT_URING_FIXED_FILES},
    {"uring-provided-buffers", no_argument, NULL, OPT_URING_PROVIDED_BUFFERS},
    {"uring-multishot-accept", no_argument, NULL, OPT_URING_MULTISHOT_ACCEPT},
    {"uring-multishot-recv", no_argument, NULL, OPT_URING_MULTISHOT_RECV},
    {"uring-fixed-buffers", no_argument, NULL, OPT_URING_FIXED_BUFFERS},
    {"uring-single-issuer", no_argument, NULL, OPT_URING_SINGLE_ISSUER},
    {"uring-defer-taskrun", no_argument, NULL, OPT_URING_DEFER_TASKRUN},
    {"uring-coop-taskrun", no_argument, NULL, OPT_URING_COOP_TASKRUN},
    {"uring-register-ring-fd", no_argument, NULL, OPT_URING_REGISTER_RING_FD},
    {"uring-poll-first", no_argument, NULL, OPT_URING_POLL_FIRST},
    {"uring-send-zc", no_argument, NULL, OPT_URING_SEND_ZC},
    {"uring-sqpoll", no_argument, NULL, OPT_URING_SQPOLL},
    {"uring-sqpoll-cpu", required_argument, NULL, OPT_URING_SQPOLL_CPU},
    {"uring-sqpoll-idle", required_argument, NULL, OPT_URING_SQPOLL_IDLE},
    {"uring-async-file-read", no_argument, NULL, OPT_URING_ASYNC_FILE_READ},
    {"uring-splice", no_argument, NULL, OPT_URING_SPLICE},
    {NULL, 0, NULL, 0}
};

void
core_config_default(core_config_t *cfg, uint32_t default_buf_size)
{
    if (cfg == NULL) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    baseline_buf_size = default_buf_size;
    cfg->nodelay = true;
    cfg->port = 8080;
    cfg->max_conns = 10240;
    cfg->buf_size = default_buf_size;
    cfg->backlog = 4096;
    cfg->cpu = -1;
    cfg->msg_size = 512;
    cfg->chunk_size = 65536;
    cfg->epoll.maxevents = 256;
    cfg->uring.sqpoll_cpu = -1;
}

int
core_parse_flags(int argc, char **argv, core_config_t *cfg, char *err, size_t errlen)
{
    int option;

    if (cfg == NULL || argc < 0 || argv == NULL) {
        set_error(err, errlen, "invalid argument to core_parse_flags");
        return -1;
    }
    opterr = 0;
    optind = 1;
    while ((option = getopt_long(argc, argv, "", long_options, NULL)) != -1) {
        uint64_t number;
        int signed_number;
        bool boolean;

        switch (option) {
        case OPT_NODELAY:
            if (parse_bool_value(optarg, &boolean) != 0) {
                set_error(err, errlen, "--nodelay accepts only 0 or 1");
                return -1;
            }
            cfg->nodelay = boolean;
            break;
        case OPT_PORT:
            if (parse_u64(optarg, UINT16_MAX, &number) != 0 || number == 0) goto bad_value;
            cfg->port = (uint16_t)number;
            break;
        case OPT_MAX_CONNS:
            if (parse_u64(optarg, UINT32_MAX, &number) != 0 || number == 0) goto bad_value;
            cfg->max_conns = (uint32_t)number;
            break;
        case OPT_BUF_SIZE:
            if (parse_u64(optarg, UINT32_MAX, &number) != 0 || number == 0) goto bad_value;
            cfg->buf_size = (uint32_t)number;
            break;
        case OPT_BACKLOG:
            if (parse_int_value(optarg, 1, INT_MAX, &signed_number) != 0) goto bad_value;
            cfg->backlog = signed_number;
            break;
        case OPT_RCVBUF:
            if (parse_int_value(optarg, 0, INT_MAX, &signed_number) != 0) goto bad_value;
            cfg->rcvbuf = signed_number;
            break;
        case OPT_SNDBUF:
            if (parse_int_value(optarg, 0, INT_MAX, &signed_number) != 0) goto bad_value;
            cfg->sndbuf = signed_number;
            break;
        case OPT_ACCEPT_BATCH:
            if (parse_u64(optarg, UINT32_MAX, &number) != 0) goto bad_value;
            cfg->accept_batch = (uint32_t)number;
            break;
        case OPT_BUSY_POLL:
            if (parse_u64(optarg, INT_MAX, &number) != 0) goto bad_value;
            cfg->busy_poll_usec = (uint32_t)number;
            break;
        case OPT_CPU:
            if (parse_int_value(optarg, -1, INT_MAX, &signed_number) != 0) goto bad_value;
            cfg->cpu = signed_number;
            break;
        case OPT_STATS_FILE:
            if (optarg == NULL || *optarg == '\0') goto bad_value;
            cfg->stats_file = optarg;
            break;
        case OPT_MSG_SIZE:
            if (parse_u64(optarg, UINT32_MAX, &number) != 0 || number == 0) goto bad_value;
            cfg->msg_size = (uint32_t)number;
            break;
        case OPT_CHUNK_SIZE:
            if (parse_u64(optarg, UINT32_MAX, &number) != 0 || number == 0) goto bad_value;
            cfg->chunk_size = (uint32_t)number;
            break;
        case OPT_SENDFILE:
            cfg->sendfile = true;
            break;
        case OPT_FILE_READ:
            if (strcmp(optarg, "mmap") != 0) {
                set_error(err, errlen, "--file-read supports only mmap");
                return -1;
            }
            cfg->file_read_mmap = true;
            break;
        case OPT_EPOLL_ET: cfg->epoll.et = true; break;
        case OPT_EPOLL_MAXEVENTS:
            if (parse_u64(optarg, UINT32_MAX, &number) != 0 || number == 0) goto bad_value;
            cfg->epoll.maxevents = (uint32_t)number;
            break;
        case OPT_EPOLL_ONESHOT: cfg->epoll.oneshot = true; break;
        case OPT_URING_ENTRIES:
            if (parse_u64(optarg, UINT32_MAX, &number) != 0) goto bad_value;
            cfg->uring.entries = (uint32_t)number;
            break;
        case OPT_URING_CQ_ENTRIES:
            if (parse_u64(optarg, UINT32_MAX, &number) != 0) goto bad_value;
            cfg->uring.cq_entries = (uint32_t)number;
            break;
        case OPT_URING_SUBMIT_BATCH:
            if (parse_u64(optarg, UINT32_MAX, &number) != 0) goto bad_value;
            cfg->uring.submit_batch = (uint32_t)number;
            break;
        case OPT_URING_FIXED_FILES: cfg->uring.fixed_files = true; break;
        case OPT_URING_PROVIDED_BUFFERS: cfg->uring.provided_buffers = true; break;
        case OPT_URING_MULTISHOT_ACCEPT: cfg->uring.multishot_accept = true; break;
        case OPT_URING_MULTISHOT_RECV: cfg->uring.multishot_recv = true; break;
        case OPT_URING_FIXED_BUFFERS: cfg->uring.fixed_buffers = true; break;
        case OPT_URING_SINGLE_ISSUER: cfg->uring.single_issuer = true; break;
        case OPT_URING_DEFER_TASKRUN: cfg->uring.defer_taskrun = true; break;
        case OPT_URING_COOP_TASKRUN: cfg->uring.coop_taskrun = true; break;
        case OPT_URING_REGISTER_RING_FD: cfg->uring.register_ring_fd = true; break;
        case OPT_URING_POLL_FIRST: cfg->uring.poll_first = true; break;
        case OPT_URING_SEND_ZC: cfg->uring.send_zc = true; break;
        case OPT_URING_SQPOLL: cfg->uring.sqpoll = true; break;
        case OPT_URING_SQPOLL_CPU:
            if (parse_int_value(optarg, -1, INT_MAX, &signed_number) != 0) goto bad_value;
            cfg->uring.sqpoll_cpu = signed_number;
            break;
        case OPT_URING_SQPOLL_IDLE:
            if (parse_u64(optarg, UINT32_MAX, &number) != 0) goto bad_value;
            cfg->uring.sqpoll_idle_ms = (uint32_t)number;
            break;
        case OPT_URING_ASYNC_FILE_READ: cfg->uring.async_file_read = true; break;
        case OPT_URING_SPLICE: cfg->uring.splice = true; break;
        case '?':
        default:
            set_error(err, errlen, "unknown option or missing option value: %s",
                      optind > 0 && optind <= argc ? argv[optind - 1] : "(unknown)");
            return -1;
        }
        continue;

bad_value:
        set_error(err, errlen, "invalid value for option: %s",
                  optind > 0 && optind <= argc ? argv[optind - 1] : "(unknown)");
        return -1;
    }
    if (optind != argc) {
        set_error(err, errlen, "unexpected positional argument: %s", argv[optind]);
        return -1;
    }
    if (cfg->buf_size == 0 || cfg->max_conns == 0 || cfg->port == 0) {
        set_error(err, errlen, "port, max-conns, and buf-size must be nonzero");
        return -1;
    }
    return 0;
}

int
core_validate_flags(const core_config_t *cfg, const char *io_name,
                    char *err, size_t errlen)
{
    bool is_select;
    bool is_poll;
    bool is_epoll;
    bool is_uring;

    if (cfg == NULL || io_name == NULL) {
        set_error(err, errlen, "invalid config or I/O mechanism name");
        return -1;
    }
    is_select = strcmp(io_name, "select") == 0;
    is_poll = strcmp(io_name, "poll") == 0;
    is_epoll = strcmp(io_name, "epoll") == 0;
    is_uring = strcmp(io_name, "uring") == 0;
    if (!is_select && !is_poll && !is_epoll && !is_uring) {
        set_error(err, errlen, "unknown I/O mechanism: %s", io_name);
        return -1;
    }
    if ((cfg->epoll.et || cfg->epoll.oneshot || cfg->epoll.maxevents != 256) && !is_epoll) {
        set_error(err, errlen, "epoll flags require the epoll mechanism");
        return -1;
    }
    if ((cfg->uring.entries != 0 || cfg->uring.cq_entries != 0 ||
         cfg->uring.submit_batch != 0 || cfg->uring.fixed_files ||
         cfg->uring.provided_buffers || cfg->uring.multishot_accept ||
         cfg->uring.multishot_recv || cfg->uring.fixed_buffers ||
         cfg->uring.single_issuer || cfg->uring.defer_taskrun ||
         cfg->uring.coop_taskrun || cfg->uring.register_ring_fd ||
         cfg->uring.poll_first || cfg->uring.send_zc || cfg->uring.sqpoll ||
         cfg->uring.sqpoll_cpu != -1 || cfg->uring.sqpoll_idle_ms != 0 ||
         cfg->uring.async_file_read || cfg->uring.splice) && !is_uring) {
        set_error(err, errlen, "io_uring flags require the uring mechanism");
        return -1;
    }
    if (cfg->uring.multishot_recv && !cfg->uring.provided_buffers) {
        set_error(err, errlen, "--uring-multishot-recv requires --uring-provided-buffers");
        return -1;
    }
    if (cfg->uring.defer_taskrun && !cfg->uring.single_issuer) {
        set_error(err, errlen, "--uring-defer-taskrun requires --uring-single-issuer");
        return -1;
    }
    if (cfg->sendfile && is_uring) {
        set_error(err, errlen, "--sendfile is unavailable with uring; use --uring-splice");
        return -1;
    }
    if (cfg->uring.splice && !is_uring) {
        set_error(err, errlen, "--uring-splice requires the uring mechanism");
        return -1;
    }
    return 0;
}

static void
print_changed_flag(const char *name, const char *value, bool *first)
{
    (void)fprintf(stderr, "%s%s=%s", *first ? "" : " ", name, value);
    *first = false;
}

void
core_print_flags(const core_config_t *cfg, const char *app_name, const char *io_name)
{
    bool first = true;
    char value[32];

    if (cfg == NULL) return;
    (void)fprintf(stderr, "flags app=%s io=%s", app_name ? app_name : "unknown",
                  io_name ? io_name : "unknown");
#define PRINT_BOOL_FLAG(member, flag) do { \
    if ((member)) print_changed_flag((flag), "1", &first); \
} while (0)
    if (!cfg->nodelay) print_changed_flag("--nodelay", "0", &first);
    if (cfg->port != 8080) { (void)snprintf(value, sizeof(value), "%u", (unsigned)cfg->port); print_changed_flag("--port", value, &first); }
    if (cfg->max_conns != 10240) { (void)snprintf(value, sizeof(value), "%" PRIu32, cfg->max_conns); print_changed_flag("--max-conns", value, &first); }
    if (cfg->buf_size != baseline_buf_size) { (void)snprintf(value, sizeof(value), "%" PRIu32, cfg->buf_size); print_changed_flag("--buf-size", value, &first); }
    if (cfg->backlog != 4096) { (void)snprintf(value, sizeof(value), "%d", cfg->backlog); print_changed_flag("--backlog", value, &first); }
    if (cfg->rcvbuf != 0) { (void)snprintf(value, sizeof(value), "%d", cfg->rcvbuf); print_changed_flag("--rcvbuf", value, &first); }
    if (cfg->sndbuf != 0) { (void)snprintf(value, sizeof(value), "%d", cfg->sndbuf); print_changed_flag("--sndbuf", value, &first); }
    if (cfg->accept_batch != 0) { (void)snprintf(value, sizeof(value), "%" PRIu32, cfg->accept_batch); print_changed_flag("--accept-batch", value, &first); }
    if (cfg->busy_poll_usec != 0) { (void)snprintf(value, sizeof(value), "%" PRIu32, cfg->busy_poll_usec); print_changed_flag("--busy-poll", value, &first); }
    if (cfg->cpu != -1) { (void)snprintf(value, sizeof(value), "%d", cfg->cpu); print_changed_flag("--cpu", value, &first); }
    if (cfg->msg_size != 512) { (void)snprintf(value, sizeof(value), "%" PRIu32, cfg->msg_size); print_changed_flag("--msg-size", value, &first); }
    if (cfg->chunk_size != 65536) { (void)snprintf(value, sizeof(value), "%" PRIu32, cfg->chunk_size); print_changed_flag("--chunk-size", value, &first); }
    if (cfg->stats_file != NULL) print_changed_flag("--stats-file", cfg->stats_file, &first);
    PRINT_BOOL_FLAG(cfg->sendfile, "--sendfile");
    PRINT_BOOL_FLAG(cfg->file_read_mmap, "--file-read=mmap");
    PRINT_BOOL_FLAG(cfg->epoll.et, "--epoll-et");
    if (cfg->epoll.maxevents != 256) { (void)snprintf(value, sizeof(value), "%" PRIu32, cfg->epoll.maxevents); print_changed_flag("--epoll-maxevents", value, &first); }
    PRINT_BOOL_FLAG(cfg->epoll.oneshot, "--epoll-oneshot");
    if (cfg->uring.entries != 0) { (void)snprintf(value, sizeof(value), "%" PRIu32, cfg->uring.entries); print_changed_flag("--uring-entries", value, &first); }
    if (cfg->uring.cq_entries != 0) { (void)snprintf(value, sizeof(value), "%" PRIu32, cfg->uring.cq_entries); print_changed_flag("--uring-cq-entries", value, &first); }
    if (cfg->uring.submit_batch != 0) { (void)snprintf(value, sizeof(value), "%" PRIu32, cfg->uring.submit_batch); print_changed_flag("--uring-submit-batch", value, &first); }
    PRINT_BOOL_FLAG(cfg->uring.fixed_files, "--uring-fixed-files");
    PRINT_BOOL_FLAG(cfg->uring.provided_buffers, "--uring-provided-buffers");
    PRINT_BOOL_FLAG(cfg->uring.multishot_accept, "--uring-multishot-accept");
    PRINT_BOOL_FLAG(cfg->uring.multishot_recv, "--uring-multishot-recv");
    PRINT_BOOL_FLAG(cfg->uring.fixed_buffers, "--uring-fixed-buffers");
    PRINT_BOOL_FLAG(cfg->uring.single_issuer, "--uring-single-issuer");
    PRINT_BOOL_FLAG(cfg->uring.defer_taskrun, "--uring-defer-taskrun");
    PRINT_BOOL_FLAG(cfg->uring.coop_taskrun, "--uring-coop-taskrun");
    PRINT_BOOL_FLAG(cfg->uring.register_ring_fd, "--uring-register-ring-fd");
    PRINT_BOOL_FLAG(cfg->uring.poll_first, "--uring-poll-first");
    PRINT_BOOL_FLAG(cfg->uring.send_zc, "--uring-send-zc");
    PRINT_BOOL_FLAG(cfg->uring.sqpoll, "--uring-sqpoll");
    if (cfg->uring.sqpoll_cpu != -1) { (void)snprintf(value, sizeof(value), "%d", cfg->uring.sqpoll_cpu); print_changed_flag("--uring-sqpoll-cpu", value, &first); }
    if (cfg->uring.sqpoll_idle_ms != 0) { (void)snprintf(value, sizeof(value), "%" PRIu32, cfg->uring.sqpoll_idle_ms); print_changed_flag("--uring-sqpoll-idle", value, &first); }
    PRINT_BOOL_FLAG(cfg->uring.async_file_read, "--uring-async-file-read");
    PRINT_BOOL_FLAG(cfg->uring.splice, "--uring-splice");
#undef PRINT_BOOL_FLAG
    (void)fputc('\n', stderr);
}

static bool
multiply_size(size_t a, size_t b, size_t *result)
{
    if (a != 0 && b > SIZE_MAX / a) return false;
    *result = a * b;
    return true;
}

int
core_init(const core_config_t *cfg, size_t app_size, size_t io_size)
{
    size_t bytes;
    uint32_t i;

    if (cfg == NULL || cfg->max_conns == 0 || cfg->buf_size == 0 ||
        connections != NULL) {
        errno = EINVAL;
        return -1;
    }
    if (cfg->max_conns > UINT32_MAX / 2U) {
        errno = EOVERFLOW;
        return -1;
    }
    active_cfg = *cfg;
    g_cfg = &active_cfg;
    app_priv_size = app_size;
    module_priv_size = io_size;
    connections = calloc(cfg->max_conns, sizeof(*connections));
    free_conn_ids = malloc((size_t)cfg->max_conns * sizeof(*free_conn_ids));
    if (!multiply_size(cfg->max_conns, app_size, &bytes)) goto fail;
    if (bytes != 0) app_priv_storage = calloc(1, bytes);
    if (!multiply_size(cfg->max_conns, io_size, &bytes)) goto fail;
    if (bytes != 0) io_priv_storage = calloc(1, bytes);
    slot_capacity = cfg->max_conns * 2U;
    if (!multiply_size(slot_capacity, cfg->buf_size, &slot_mapping_size)) goto fail;
    free_slot_ids = malloc((size_t)slot_capacity * sizeof(*free_slot_ids));
    slot_used = calloc(slot_capacity, sizeof(*slot_used));
    if (connections == NULL || free_conn_ids == NULL ||
        (app_size != 0 && app_priv_storage == NULL) ||
        (io_size != 0 && io_priv_storage == NULL) ||
        free_slot_ids == NULL || slot_used == NULL) goto fail;
    slot_mapping = mmap(NULL, slot_mapping_size, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (slot_mapping == MAP_FAILED) {
        slot_mapping = NULL;
        goto fail;
    }
    for (i = 0; i < cfg->max_conns; ++i) {
        conn_t *c = &connections[i];
        c->fd = -1;
        c->id = i;
        c->state = CONN_FREE;
        c->app_priv = app_size ? app_priv_storage + (size_t)i * app_size : NULL;
        c->io_priv = io_size ? io_priv_storage + (size_t)i * io_size : NULL;
        free_conn_ids[i] = cfg->max_conns - i - 1U;
    }
    free_conn_count = cfg->max_conns;
    free_slot_count = slot_capacity;
    for (i = 0; i < slot_capacity; ++i) free_slot_ids[i] = slot_capacity - i - 1U;
    if (active_cfg.stats_file != NULL) {
        stats_file_stream = fopen(active_cfg.stats_file, "a");
        if (stats_file_stream == NULL) goto fail;
    }
    allocated_conn_count = 0;
    active_conn_count = 0;
    peak_conn_count = 0;
    memset(&g_stats, 0, sizeof(g_stats));
    g_shutdown = 0;
    g_dump_stats = 0;
    return 0;

fail:
    core_shutdown();
    if (errno == 0) errno = ENOMEM;
    return -1;
}

void
core_shutdown(void)
{
    if (stats_file_stream != NULL) (void)fclose(stats_file_stream);
    stats_file_stream = NULL;
    if (slot_mapping != NULL) (void)munmap(slot_mapping, slot_mapping_size);
    free(slot_used);
    free(free_slot_ids);
    free(io_priv_storage);
    free(app_priv_storage);
    free(free_conn_ids);
    free(connections);
    slot_mapping = NULL;
    slot_mapping_size = 0;
    slot_used = NULL;
    free_slot_ids = NULL;
    io_priv_storage = NULL;
    app_priv_storage = NULL;
    free_conn_ids = NULL;
    connections = NULL;
    free_conn_count = 0;
    free_slot_count = 0;
    slot_capacity = 0;
    allocated_conn_count = 0;
    app_priv_size = 0;
    module_priv_size = 0;
    g_cfg = NULL;
}

conn_t *
core_conn_alloc(int fd)
{
    uint32_t id;
    conn_t *c;

    if (connections == NULL || fd < 0 || free_conn_count == 0) return NULL;
    id = free_conn_ids[--free_conn_count];
    c = &connections[id];
    ++c->gen;
    if (c->gen == 0) ++c->gen;
    c->fd = fd;
    c->state = CONN_ACCEPTED;
    c->active = false;
    c->read_max = 0;
    c->wr_buf = NULL;
    c->wr_len = 0;
    c->wr_off = 0;
    c->carry = NULL;
    c->carry_len = 0;
    c->out_slot = NULL;
    if (c->app_priv != NULL) memset(c->app_priv, 0, app_priv_size);
    if (c->io_priv != NULL) memset(c->io_priv, 0, module_priv_size);
    ++allocated_conn_count;
    return c;
}

void
core_conn_activate(conn_t *c)
{
    if (c == NULL || c->state == CONN_FREE || c->active) return;
    c->active = true;
    ++active_conn_count;
    if (active_conn_count > peak_conn_count)
        peak_conn_count = active_conn_count;
}

void
core_conn_free(conn_t *c)
{
    uintptr_t base;
    uintptr_t ptr;
    size_t offset;
    uint32_t id;

    if (c == NULL || connections == NULL) return;
    base = (uintptr_t)(void *)connections;
    ptr = (uintptr_t)(void *)c;
    if (ptr < base || ptr - base >= (size_t)active_cfg.max_conns * sizeof(*connections) ||
        (ptr - base) % sizeof(*connections) != 0 || c->state == CONN_FREE) return;
    id = c->id;
    if (c->active && active_conn_count != 0) --active_conn_count;
    c->active = false;
    if (c->carry != NULL) core_slot_free(c->carry);
    if (c->out_slot != NULL) core_slot_free(c->out_slot);
    c->carry = NULL;
    c->carry_len = 0;
    c->out_slot = NULL;
    c->fd = -1;
    c->read_max = 0;
    c->wr_buf = NULL;
    c->wr_len = 0;
    c->wr_off = 0;
    c->state = CONN_FREE;
    ++c->gen;
    if (c->gen == 0) ++c->gen;
    if (c->app_priv != NULL) memset(c->app_priv, 0, app_priv_size);
    if (c->io_priv != NULL) memset(c->io_priv, 0, module_priv_size);
    offset = (size_t)id;
    if (offset < active_cfg.max_conns && free_conn_count < active_cfg.max_conns) {
        free_conn_ids[free_conn_count++] = id;
        if (allocated_conn_count != 0) --allocated_conn_count;
    }
}

conn_t *
core_conn_by_fd(int fd)
{
    uint32_t i;
    if (connections == NULL || fd < 0) return NULL;
    for (i = 0; i < active_cfg.max_conns; ++i) {
        if (connections[i].state != CONN_FREE && connections[i].fd == fd)
            return &connections[i];
    }
    return NULL;
}

conn_t *
core_conn_get(uint32_t id, uint32_t gen)
{
    conn_t *c;
    if (connections == NULL || id >= active_cfg.max_conns) return NULL;
    c = &connections[id];
    return c->state != CONN_FREE && c->gen == gen ? c : NULL;
}

uint32_t
core_conn_count(void)
{
    return allocated_conn_count;
}

void *
core_slot_alloc(void)
{
    uint32_t id;
    if (slot_mapping == NULL || free_slot_count == 0) return NULL;
    id = free_slot_ids[--free_slot_count];
    slot_used[id] = 1;
    return (uint8_t *)slot_mapping + (size_t)id * active_cfg.buf_size;
}

void
core_slot_free(void *slot)
{
    uintptr_t base;
    uintptr_t ptr;
    size_t offset;
    size_t id;

    if (slot == NULL || slot_mapping == NULL) return;
    base = (uintptr_t)slot_mapping;
    ptr = (uintptr_t)slot;
    if (ptr < base) return;
    offset = (size_t)(ptr - base);
    if (offset >= slot_mapping_size || offset % active_cfg.buf_size != 0) return;
    id = offset / active_cfg.buf_size;
    if (id >= slot_capacity || slot_used[id] == 0 || free_slot_count >= slot_capacity) return;
    slot_used[id] = 0;
    free_slot_ids[free_slot_count++] = (uint32_t)id;
}

size_t
core_slot_size(void)
{
    return g_cfg != NULL ? g_cfg->buf_size : 0;
}

int
core_carry_append(conn_t *c, const void *data, size_t len)
{
    size_t cap;
    uint8_t *new_slot;

    if (c == NULL || (len != 0 && data == NULL) || c->state == CONN_FREE) return -1;
    if (len == 0) return 0;
    cap = core_slot_size();
    if (c->carry_len > cap || len > cap - c->carry_len) return -1;
    if (c->carry == NULL) {
        new_slot = core_slot_alloc();
        if (new_slot == NULL) return -1;
        c->carry = new_slot;
    }
    memmove(c->carry + c->carry_len, data, len);
    c->carry_len += (uint32_t)len;
    return 0;
}

void
core_carry_consume(conn_t *c, size_t n)
{
    if (c == NULL || c->carry == NULL) return;
    if (n >= c->carry_len) {
        core_carry_clear(c);
        return;
    }
    memmove(c->carry, c->carry + n, c->carry_len - n);
    c->carry_len -= (uint32_t)n;
}

void
core_carry_clear(conn_t *c)
{
    if (c == NULL) return;
    if (c->carry != NULL) core_slot_free(c->carry);
    c->carry = NULL;
    c->carry_len = 0;
}

static void
json_string(FILE *stream, const char *text)
{
    const unsigned char *p = (const unsigned char *)(text ? text : "");
    (void)fputc('"', stream);
    while (*p != '\0') {
        switch (*p) {
        case '"': (void)fputs("\\\"", stream); break;
        case '\\': (void)fputs("\\\\", stream); break;
        case '\b': (void)fputs("\\b", stream); break;
        case '\f': (void)fputs("\\f", stream); break;
        case '\n': (void)fputs("\\n", stream); break;
        case '\r': (void)fputs("\\r", stream); break;
        case '\t': (void)fputs("\\t", stream); break;
        default:
            if (*p < 0x20) (void)fprintf(stream, "\\u%04x", (unsigned)*p);
            else (void)fputc(*p, stream);
        }
        ++p;
    }
    (void)fputc('"', stream);
}

#define JSON_BOOL(stream, name, value, comma) \
    (void)fprintf((stream), "\"%s\":%s%s", (name), (value) ? "true" : "false", (comma) ? "," : "")
#define JSON_U32(stream, name, value, comma) \
    (void)fprintf((stream), "\"%s\":%" PRIu32 "%s", (name), (uint32_t)(value), (comma) ? "," : "")
#define JSON_U64(stream, name, value, comma) \
    (void)fprintf((stream), "\"%s\":%" PRIu64 "%s", (name), (uint64_t)(value), (comma) ? "," : "")

void
core_stats_dump(void)
{
    FILE *stream = stats_file_stream != NULL ? stats_file_stream : stdout;
    (void)fputs("{\"app\":", stream);
    json_string(stream, active_app_name);
    (void)fputs(",\"io\":", stream);
    json_string(stream, active_io_name);
    (void)fputs(",\"config\":{", stream);
    if (g_cfg != NULL) {
        JSON_BOOL(stream, "nodelay", g_cfg->nodelay, true);
        (void)fputs("\"stats_file\":", stream);
        json_string(stream, g_cfg->stats_file);
        (void)fputc(',', stream);
        JSON_U32(stream, "port", g_cfg->port, true);
        JSON_U32(stream, "max_conns", g_cfg->max_conns, true);
        JSON_U32(stream, "buf_size", g_cfg->buf_size, true);
        (void)fprintf(stream, "\"backlog\":%d,\"rcvbuf\":%d,\"sndbuf\":%d,", g_cfg->backlog, g_cfg->rcvbuf, g_cfg->sndbuf);
        JSON_U32(stream, "accept_batch", g_cfg->accept_batch, true);
        JSON_U32(stream, "busy_poll_usec", g_cfg->busy_poll_usec, true);
        (void)fprintf(stream, "\"cpu\":%d,", g_cfg->cpu);
        JSON_U32(stream, "msg_size", g_cfg->msg_size, true);
        JSON_U32(stream, "chunk_size", g_cfg->chunk_size, true);
        JSON_BOOL(stream, "sendfile", g_cfg->sendfile, true);
        JSON_BOOL(stream, "file_read_mmap", g_cfg->file_read_mmap, true);
        JSON_BOOL(stream, "epoll_et", g_cfg->epoll.et, true);
        JSON_U32(stream, "epoll_maxevents", g_cfg->epoll.maxevents, true);
        JSON_BOOL(stream, "epoll_oneshot", g_cfg->epoll.oneshot, true);
        JSON_U32(stream, "uring_entries", g_cfg->uring.entries, true);
        JSON_U32(stream, "uring_cq_entries", g_cfg->uring.cq_entries, true);
        JSON_U32(stream, "uring_submit_batch", g_cfg->uring.submit_batch, true);
        JSON_BOOL(stream, "uring_fixed_files", g_cfg->uring.fixed_files, true);
        JSON_BOOL(stream, "uring_provided_buffers", g_cfg->uring.provided_buffers, true);
        JSON_BOOL(stream, "uring_multishot_accept", g_cfg->uring.multishot_accept, true);
        JSON_BOOL(stream, "uring_multishot_recv", g_cfg->uring.multishot_recv, true);
        JSON_BOOL(stream, "uring_fixed_buffers", g_cfg->uring.fixed_buffers, true);
        JSON_BOOL(stream, "uring_single_issuer", g_cfg->uring.single_issuer, true);
        JSON_BOOL(stream, "uring_defer_taskrun", g_cfg->uring.defer_taskrun, true);
        JSON_BOOL(stream, "uring_coop_taskrun", g_cfg->uring.coop_taskrun, true);
        JSON_BOOL(stream, "uring_register_ring_fd", g_cfg->uring.register_ring_fd, true);
        JSON_BOOL(stream, "uring_poll_first", g_cfg->uring.poll_first, true);
        JSON_BOOL(stream, "uring_send_zc", g_cfg->uring.send_zc, true);
        JSON_BOOL(stream, "uring_sqpoll", g_cfg->uring.sqpoll, true);
        (void)fprintf(stream, "\"uring_sqpoll_cpu\":%d,", g_cfg->uring.sqpoll_cpu);
        JSON_U32(stream, "uring_sqpoll_idle_ms", g_cfg->uring.sqpoll_idle_ms, true);
        JSON_BOOL(stream, "uring_async_file_read", g_cfg->uring.async_file_read, true);
        JSON_BOOL(stream, "uring_splice", g_cfg->uring.splice, false);
    }
    (void)fputs("},\"stats\":{", stream);
    JSON_U64(stream, "loop_iterations", g_stats.loop_iterations, true);
    JSON_U64(stream, "wait_calls", g_stats.wait_calls, true);
    JSON_U64(stream, "epoll_ctl_calls", g_stats.epoll_ctl_calls, true);
    JSON_U64(stream, "reads", g_stats.reads, true);
    JSON_U64(stream, "writes", g_stats.writes, true);
    JSON_U64(stream, "eagain", g_stats.eagain, true);
    JSON_U64(stream, "partial_reads", g_stats.partial_reads, true);
    JSON_U64(stream, "partial_writes", g_stats.partial_writes, true);
    JSON_U64(stream, "accepts", g_stats.accepts, true);
    JSON_U64(stream, "closes", g_stats.closes, true);
    JSON_U32(stream, "active_connections", active_conn_count, true);
    if (g_reset_connection_peak) {
        peak_conn_count = 0;
        g_reset_connection_peak = 0;
    }
    JSON_U32(stream, "peak_connections", peak_conn_count, true);
    JSON_U64(stream, "messages", g_stats.messages, true);
    JSON_U64(stream, "bytes_in", g_stats.bytes_in, true);
    JSON_U64(stream, "bytes_out", g_stats.bytes_out, true);
    JSON_U64(stream, "errors", g_stats.errors, true);
    JSON_U64(stream, "peer_resets", g_stats.peer_resets, true);
    JSON_U64(stream, "ring_overflows", g_stats.ring_overflows, true);
    JSON_U64(stream, "file_reads", g_stats.file_reads, false);
    (void)fputs("}}\n", stream);
    (void)fflush(stream);
}

static void
signal_handler(int signo)
{
    int saved_errno = errno;
    if (signo == SIGINT || signo == SIGTERM) g_shutdown = 1;
    else if (signo == SIGUSR1) g_dump_stats = 1;
    else if (signo == SIGUSR2) g_reset_connection_peak = 1;
    errno = saved_errno;
}

void
core_install_signals(void)
{
    struct sigaction action;
    struct sigaction ignore;

    memset(&action, 0, sizeof(action));
    action.sa_handler = signal_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    (void)sigaction(SIGINT, &action, NULL);
    (void)sigaction(SIGTERM, &action, NULL);
    (void)sigaction(SIGUSR1, &action, NULL);
    (void)sigaction(SIGUSR2, &action, NULL);
    memset(&ignore, 0, sizeof(ignore));
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    (void)sigaction(SIGPIPE, &ignore, NULL);
}

int
core_pin_cpu(int cpu)
{
    cpu_set_t set;

    if (cpu < 0) return 0;
    if (cpu >= CPU_SETSIZE) {
        errno = EINVAL;
        return -1;
    }
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return sched_setaffinity(0, sizeof(set), &set);
}

static int
set_socket_options(int fd, const core_config_t *cfg, bool accepted)
{
    int value;

    if (accepted) {
        value = cfg->nodelay ? 1 : 0;
        if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value)) != 0) return -1;
    }
    if (cfg->rcvbuf > 0 && setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &cfg->rcvbuf, sizeof(cfg->rcvbuf)) != 0) return -1;
    if (cfg->sndbuf > 0 && setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &cfg->sndbuf, sizeof(cfg->sndbuf)) != 0) return -1;
    if (cfg->busy_poll_usec > 0) {
        value = (int)cfg->busy_poll_usec;
        if (setsockopt(fd, SOL_SOCKET, SO_BUSY_POLL, &value, sizeof(value)) != 0) return -1;
    }
    return 0;
}

int
core_make_listener(const core_config_t *cfg)
{
    int fd;
    int reuse = 1;
    struct sockaddr_in address;

    if (cfg == NULL) {
        errno = EINVAL;
        return -1;
    }
    fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0 ||
        set_socket_options(fd, cfg, false) != 0) {
        int saved_errno = errno;
        (void)close(fd);
        errno = saved_errno;
        return -1;
    }
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(cfg->port);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(fd, cfg->backlog) != 0) {
        int saved_errno = errno;
        (void)close(fd);
        errno = saved_errno;
        return -1;
    }
    return fd;
}

int
core_setup_accepted(int fd, const core_config_t *cfg)
{
    if (fd < 0 || cfg == NULL) {
        errno = EINVAL;
        return -1;
    }
    return set_socket_options(fd, cfg, true);
}

void
core_log(int level, const char *fmt, ...)
{
    static const char *const labels[] = {"ERROR", "INFO", "DEBUG"};
    va_list ap;
    const char *label = level >= 0 && (size_t)level < sizeof(labels) / sizeof(labels[0])
        ? labels[level] : "LOG";

    (void)fprintf(stderr, "[%s] ", label);
    va_start(ap, fmt);
    (void)vfprintf(stderr, fmt, ap);
    va_end(ap);
    (void)fputc('\n', stderr);
}

int
core_main(int argc, char **argv, const char *app_name,
          uint32_t default_buf_size, size_t app_size)
{
    core_config_t cfg;
    char err[256];
    const char *mechanism = io_name();
    bool core_ready = false;
    bool io_ready = false;
    bool app_ready = false;
    int result = EXIT_FAILURE;

    core_config_default(&cfg, default_buf_size);
    if (core_parse_flags(argc, argv, &cfg, err, sizeof(err)) != 0) {
        LOG_ERR("%s", err);
        return EXIT_FAILURE;
    }
    if (core_validate_flags(&cfg, mechanism, err, sizeof(err)) != 0) {
        LOG_ERR("%s", err);
        return EXIT_FAILURE;
    }
    active_app_name = app_name ? app_name : "unknown";
    active_io_name = mechanism ? mechanism : "unknown";
    core_print_flags(&cfg, active_app_name, active_io_name);
    if (core_init(&cfg, app_size, io_priv_size()) != 0) {
        LOG_ERR("core initialization failed: %s", strerror(errno));
        goto cleanup;
    }
    core_ready = true;
    core_install_signals();
    if (core_pin_cpu(cfg.cpu) != 0) {
        LOG_ERR("cannot pin CPU %d: %s", cfg.cpu, strerror(errno));
        goto cleanup;
    }
    if (io_init(g_cfg) != 0) {
        LOG_ERR("I/O initialization failed: %s", strerror(errno));
        goto cleanup;
    }
    io_ready = true;
    if (app_init(g_cfg) != 0) {
        LOG_ERR("application initialization failed");
        goto cleanup;
    }
    app_ready = true;
    if (io_listen(g_cfg) != 0) {
        LOG_ERR("listen initialization failed: %s", strerror(errno));
        goto cleanup;
    }
    if (io_run() != 0) {
        LOG_ERR("I/O event loop failed: %s", strerror(errno));
        goto cleanup;
    }
    result = EXIT_SUCCESS;

cleanup:
    if (app_ready) app_shutdown();
    if (io_ready) io_shutdown();
    if (core_ready) {
        core_stats_dump();
        core_shutdown();
    }
    return result;
}

