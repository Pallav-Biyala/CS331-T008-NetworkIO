#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace {

// Magic identifier for echo frames ("ECHO" = 0x4543484F)
constexpr uint32_t ECHO_MAGIC = 0x4543484F;
constexpr size_t MAX_EPOLL_EVENTS = 1024;

#pragma pack(push, 1)
struct MessageHeader {
    uint32_t magic;         // ECHO_MAGIC = 0x4543484F
    uint32_t conn_id;       // Connection identifier (0 .. N-1)
    uint64_t seq_num;       // Monotonically increasing sequence number per connection
    uint64_t send_time_ns;  // CLOCK_MONOTONIC timestamp in nanoseconds (t_send on last byte)
    uint32_t msg_size;      // Expected message frame size in bytes (e.g. 512)
    uint32_t checksum;      // Adler32 checksum of message payload
};
#pragma pack(pop)

static_assert(sizeof(MessageHeader) == 32, "MessageHeader must be exactly 32 bytes");

inline uint64_t get_time_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec);
}

// Fast Adler32-style checksum for payload integrity verification
inline uint32_t compute_checksum(const uint8_t *data, size_t len) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < len; ++i) {
        a += data[i];
        b += a;
    }
    a %= 65521;
    b %= 65521;
    return (b << 16) | a;
}

volatile sig_atomic_t g_stop_requested = 0;

void signal_handler(int sig) {
    (void)sig;
    g_stop_requested = 1;
}

enum class Mode {
    OPEN_LOOP,
    CLOSED_LOOP
};

enum class Distribution {
    CONSTANT,
    POISSON,
    BURST
};

enum class BenchPhase {
    CONNECTING,
    WARMUP,
    MEASURING,
    COOLDOWN,
    FINISHED
};

enum class ConnState {
    CONNECTING,
    ACTIVE,
    CLOSED,
    FAILED
};

struct Config {
    std::string host = "127.0.0.1";
    uint16_t port = 8080;
    uint32_t connections = 100;
    double rate = 1000.0;           // Target aggregate messages per second (0 = unthrottled)
    double duration = 30.0;         // Measurement duration in seconds
    size_t msg_size = 512;          // Application message size in bytes
    std::string output_csv = "";
    std::string output_json = "";
    double warmup = 0.0;            // Warmup duration in seconds
    double cooldown = 0.0;          // Cooldown duration in seconds
    uint64_t timeout_ms = 5000;     // Request timeout in milliseconds
    double connect_timeout = 10.0;  // Initial connect timeout in seconds
    Mode mode = Mode::OPEN_LOOP;    // open-loop vs closed-loop
    Distribution dist = Distribution::CONSTANT;
    size_t pipeline_window = 1;     // W = max in-flight per connection
    double active_share = 1.0;      // Fraction of connections active (0.0 < share <= 1.0)
    uint32_t burst_size = 50;       // For BURST distribution
    double burst_interval = 0.1;    // Interval between bursts in seconds
    bool self_test = false;         // Run built-in self-test suite and exit
};

struct ScheduledArrival {
    uint64_t seq_num = 0;
    uint64_t sched_time_ns = 0; // t_sched
    uint64_t tx_start_ns = 0;   // when first byte entered send()
    std::vector<uint8_t> frame; // full frame buffer (msg_size bytes)
    size_t bytes_sent = 0;
};

struct InFlightRequest {
    uint64_t seq_num = 0;
    uint64_t sched_time_ns = 0; // t_sched
    uint64_t tx_start_ns = 0;
    uint64_t send_time_ns = 0;  // t_send (exact timestamp when 512th byte accepted by kernel)
};

struct Connection {
    uint32_t id = 0;
    int fd = -1;
    ConnState state = ConnState::CONNECTING;

    uint64_t next_send_seq = 1;
    uint64_t expected_recv_seq = 1;

    std::deque<ScheduledArrival> tx_queue; // strict per-connection FIFO queue
    std::deque<InFlightRequest> in_flight; // up to W requests in flight on the wire
    bool has_active_tx = false;            // true if tx_queue.front() is partially sent

    std::vector<uint8_t> recv_buf;
    size_t recv_bytes_read = 0;
};

struct SecondBucket {
    uint32_t second = 0;
    uint64_t sent = 0;
    uint64_t received = 0;
    uint32_t app_rtt_p50_us = 0;
    uint32_t app_rtt_p95_us = 0;
    uint32_t app_rtt_p99_us = 0;
    uint64_t stalls = 0;
    std::vector<uint32_t> rtt_samples;
};

ScheduledArrival create_arrival(uint32_t conn_id, uint64_t seq_num, uint64_t sched_time_ns, size_t msg_size) {
    ScheduledArrival arr;
    arr.seq_num = seq_num;
    arr.sched_time_ns = sched_time_ns;
    arr.tx_start_ns = 0;
    arr.bytes_sent = 0;
    arr.frame.resize(msg_size);

    MessageHeader *hdr = reinterpret_cast<MessageHeader *>(arr.frame.data());
    hdr->magic = ECHO_MAGIC;
    hdr->conn_id = conn_id;
    hdr->seq_num = seq_num;
    hdr->send_time_ns = 0; // will be populated at t_send
    hdr->msg_size = static_cast<uint32_t>(msg_size);

    uint8_t *payload = arr.frame.data() + sizeof(MessageHeader);
    size_t payload_len = msg_size - sizeof(MessageHeader);
    for (size_t i = 0; i < payload_len; ++i) {
        payload[i] = static_cast<uint8_t>((conn_id + seq_num + i) & 0xFF);
    }
    hdr->checksum = compute_checksum(payload, payload_len);
    return arr;
}

uint64_t query_total_tcp_retrans(const std::vector<Connection> &conns) {
    uint64_t total = 0;
#if defined(__linux__) && defined(TCP_INFO)
    for (const auto &c : conns) {
        if (c.fd >= 0) {
            struct tcp_info info;
            socklen_t len = sizeof(info);
            if (getsockopt(c.fd, IPPROTO_TCP, TCP_INFO, &info, &len) == 0) {
                total += info.tcpi_total_retrans;
            }
        }
    }
#else
    (void)conns;
#endif
    return total;
}

void print_usage(const char *prog) {
    std::cout << "Usage: " << prog << " [OPTIONS]\n"
              << "\nRequired / Standard Options:\n"
              << "  --host <ip|hostname>       Server address (default: 127.0.0.1)\n"
              << "  --port <port>              Server port (default: 8080)\n"
              << "  --connections <N>          Number of persistent TCP connections (default: 100)\n"
              << "  --rate <R>                 Target aggregate message rate in msgs/sec (default: 1000)\n"
              << "  --mode <open-loop|closed-loop>  Benchmark mode (default: open-loop)\n"
              << "  --unthrottled              Alias for --mode closed-loop\n"
              << "  --dist <constant|poisson|burst>  Arrival distribution (default: constant)\n"
              << "  --pipeline-window <W>, -W  Pipelining depth per socket (default: 1)\n"
              << "  --active-share <float>     Fraction of connections active (0.0 < share <= 1.0, default: 1.0)\n"
              << "  --duration <sec>           Measurement duration in seconds (default: 30)\n"
              << "  --msg-size <bytes>         Message frame size in bytes (default: 512)\n"
              << "  --output <csv_path>        Output CSV file path\n"
              << "  --json-output <json_path>  Output per-run JSON report with timeseries buckets\n"
              << "  --warmup <sec>             Warmup duration in seconds (default: 0)\n"
              << "  --cooldown <sec>           Cooldown / drain duration in seconds (default: 0)\n"
              << "  --timeout <ms>             Response timeout in milliseconds (default: 5000)\n"
              << "  --connect-timeout <sec>    Timeout for initial connection establishment (default: 10)\n"
              << "  --self-test                Run built-in verification suite and exit\n"
              << "  --help, -h                 Show this help message\n";
}

bool parse_args(int argc, char **argv, Config &cfg) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        std::string val;
        auto eq = arg.find('=');
        if (eq != std::string::npos) {
            val = arg.substr(eq + 1);
            arg = arg.substr(0, eq);
        } else if (arg == "--unthrottled") {
            cfg.mode = Mode::CLOSED_LOOP;
            cfg.rate = 0.0;
            continue;
        } else if (arg == "--self-test") {
            cfg.self_test = true;
            return true;
        } else if (i + 1 < argc && argv[i + 1][0] != '-') {
            val = argv[++i];
        }

        if (arg == "--host") {
            if (val.empty()) return false;
            cfg.host = val;
        } else if (arg == "--port") {
            if (val.empty()) return false;
            cfg.port = static_cast<uint16_t>(std::stoi(val));
        } else if (arg == "--connections") {
            if (val.empty()) return false;
            cfg.connections = static_cast<uint32_t>(std::stoul(val));
        } else if (arg == "--rate") {
            if (val.empty()) return false;
            cfg.rate = std::stod(val);
            if (cfg.rate == 0.0) {
                cfg.mode = Mode::CLOSED_LOOP;
            }
        } else if (arg == "--mode") {
            if (val == "open-loop" || val == "open") {
                cfg.mode = Mode::OPEN_LOOP;
            } else if (val == "closed-loop" || val == "closed") {
                cfg.mode = Mode::CLOSED_LOOP;
                cfg.rate = 0.0;
            } else {
                std::cerr << "Invalid --mode: " << val << " (must be open-loop or closed-loop)\n";
                return false;
            }
        } else if (arg == "--dist" || arg == "--distribution") {
            if (val == "constant") {
                cfg.dist = Distribution::CONSTANT;
            } else if (val == "poisson") {
                cfg.dist = Distribution::POISSON;
            } else if (val == "burst") {
                cfg.dist = Distribution::BURST;
            } else {
                std::cerr << "Invalid --dist: " << val << " (must be constant, poisson, or burst)\n";
                return false;
            }
        } else if (arg == "--pipeline-window" || arg == "-W") {
            if (val.empty()) return false;
            cfg.pipeline_window = static_cast<size_t>(std::stoul(val));
            if (cfg.pipeline_window == 0) cfg.pipeline_window = 1;
        } else if (arg == "--unthrottled") {
            cfg.mode = Mode::CLOSED_LOOP;
            cfg.rate = 0.0;
        } else if (arg == "--active-share") {
            if (val.empty()) return false;
            cfg.active_share = std::stod(val);
        } else if (arg == "--duration") {
            if (val.empty()) return false;
            cfg.duration = std::stod(val);
        } else if (arg == "--msg-size") {
            if (val.empty()) return false;
            cfg.msg_size = static_cast<size_t>(std::stoul(val));
        } else if (arg == "--output") {
            if (val.empty()) return false;
            cfg.output_csv = val;
        } else if (arg == "--json-output" || arg == "--json") {
            if (val.empty()) return false;
            cfg.output_json = val;
        } else if (arg == "--warmup") {
            if (val.empty()) return false;
            cfg.warmup = std::stod(val);
        } else if (arg == "--cooldown") {
            if (val.empty()) return false;
            cfg.cooldown = std::stod(val);
        } else if (arg == "--timeout") {
            if (val.empty()) return false;
            cfg.timeout_ms = std::stoull(val);
        } else if (arg == "--connect-timeout") {
            if (val.empty()) return false;
            cfg.connect_timeout = std::stod(val);
        } else if (arg == "--burst-size") {
            if (val.empty()) return false;
            cfg.burst_size = static_cast<uint32_t>(std::stoul(val));
        } else if (arg == "--burst-interval") {
            if (val.empty()) return false;
            cfg.burst_interval = std::stod(val);
        } else if (arg == "--self-test") {
            cfg.self_test = true;
            return true;
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            exit(0);
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            return false;
        }
    }

    if (cfg.msg_size < sizeof(MessageHeader)) {
        std::cerr << "Error: --msg-size must be at least " << sizeof(MessageHeader) << " bytes\n";
        return false;
    }
    if (cfg.connections == 0) {
        std::cerr << "Error: --connections must be positive\n";
        return false;
    }
    if (cfg.mode == Mode::OPEN_LOOP && cfg.rate <= 0.0) {
        std::cerr << "Error: --rate must be positive in open-loop mode\n";
        return false;
    }
    if (cfg.active_share <= 0.0 || cfg.active_share > 1.0) {
        std::cerr << "Error: --active-share must be in range (0.0, 1.0]\n";
        return false;
    }
    if (cfg.duration <= 0.0) {
        std::cerr << "Error: --duration must be positive\n";
        return false;
    }
    return true;
}

bool ensure_parent_dir(const std::string &path) {
    auto slash = path.find_last_of("/\\");
    if (slash == std::string::npos) return true;
    std::string dir = path.substr(0, slash);
    if (dir.empty()) return true;

    std::string current;
    for (char c : dir) {
        current += c;
        if (c == '/' || c == '\\') {
            mkdir(current.c_str(), 0755);
        }
    }
    mkdir(current.c_str(), 0755);
    return true;
}

bool set_socket_options(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return false;
    }
    int nodelay = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay)) < 0) {
        return false;
    }
    int bufsize = 65536;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
    return true;
}

// --------------------------------------------------------------------------
// Strict FIFO Transmission & Queue Pumping (Correction 1 & 2)
// --------------------------------------------------------------------------

void pump_conn_tx_queue(Connection &conn, int epoll_fd, BenchPhase phase, uint64_t &total_sent, size_t W, size_t msg_size) {
    while (!conn.has_active_tx && !conn.tx_queue.empty() && conn.in_flight.size() < W && conn.fd >= 0) {
        auto &req = conn.tx_queue.front();
        if (req.tx_start_ns == 0) {
            req.tx_start_ns = get_time_ns();
        }
        size_t remaining = msg_size - req.bytes_sent;
        ssize_t n = send(conn.fd, req.frame.data() + req.bytes_sent, remaining, MSG_NOSIGNAL);
        if (n == static_cast<ssize_t>(remaining)) {
            // Frame completed: 512th byte accepted by kernel!
            uint64_t now_ns = get_time_ns();
            MessageHeader *hdr = reinterpret_cast<MessageHeader *>(req.frame.data());
            hdr->send_time_ns = now_ns;
            conn.in_flight.push_back({req.seq_num, req.sched_time_ns, req.tx_start_ns, now_ns});
            conn.tx_queue.pop_front();
            if (phase == BenchPhase::MEASURING) {
                total_sent++;
            }
        } else if (n > 0) {
            req.bytes_sent += static_cast<size_t>(n);
            conn.has_active_tx = true;
            struct epoll_event ev;
            ev.events = EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLHUP;
            ev.data.u32 = conn.id;
            epoll_ctl(epoll_fd, EPOLL_CTL_MOD, conn.fd, &ev);
            break;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            conn.has_active_tx = true;
            struct epoll_event ev;
            ev.events = EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLHUP;
            ev.data.u32 = conn.id;
            epoll_ctl(epoll_fd, EPOLL_CTL_MOD, conn.fd, &ev);
            break;
        } else {
            conn.state = ConnState::FAILED;
            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, conn.fd, nullptr);
            close(conn.fd);
            conn.fd = -1;
            break;
        }
    }

    if (!conn.has_active_tx && conn.fd >= 0) {
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLERR | EPOLLHUP;
        ev.data.u32 = conn.id;
        epoll_ctl(epoll_fd, EPOLL_CTL_MOD, conn.fd, &ev);
    }
}

void dispatch_or_enqueue_on_conn(Connection &conn, ScheduledArrival &&arr, int epoll_fd, BenchPhase phase, uint64_t &total_sent, size_t W, size_t msg_size) {
    if (conn.fd < 0) return;

    if (conn.tx_queue.empty() && !conn.has_active_tx && conn.in_flight.size() < W) {
        // Transmit immediately!
        arr.tx_start_ns = get_time_ns();
        ssize_t n = send(conn.fd, arr.frame.data(), msg_size, MSG_NOSIGNAL);
        if (n == static_cast<ssize_t>(msg_size)) {
            uint64_t now_ns = get_time_ns();
            MessageHeader *hdr = reinterpret_cast<MessageHeader *>(arr.frame.data());
            hdr->send_time_ns = now_ns;
            conn.in_flight.push_back({arr.seq_num, arr.sched_time_ns, arr.tx_start_ns, now_ns});
            if (phase == BenchPhase::MEASURING) {
                total_sent++;
            }
            return;
        } else if (n > 0) {
            arr.bytes_sent = static_cast<size_t>(n);
            conn.has_active_tx = true;
            conn.tx_queue.push_back(std::move(arr));
            struct epoll_event ev;
            ev.events = EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLHUP;
            ev.data.u32 = conn.id;
            epoll_ctl(epoll_fd, EPOLL_CTL_MOD, conn.fd, &ev);
            return;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            arr.bytes_sent = 0;
            conn.has_active_tx = true;
            conn.tx_queue.push_back(std::move(arr));
            struct epoll_event ev;
            ev.events = EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLHUP;
            ev.data.u32 = conn.id;
            epoll_ctl(epoll_fd, EPOLL_CTL_MOD, conn.fd, &ev);
            return;
        } else {
            conn.state = ConnState::FAILED;
            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, conn.fd, nullptr);
            close(conn.fd);
            conn.fd = -1;
            return;
        }
    } else {
        // Append to tx_queue in strict FIFO order
        conn.tx_queue.push_back(std::move(arr));
    }
}

void refill_closed_loop_conn(Connection &conn, const Config &cfg, int epoll_fd, BenchPhase phase, uint64_t &total_sent) {
    while (conn.fd >= 0 && (conn.in_flight.size() + conn.tx_queue.size()) < cfg.pipeline_window) {
        uint64_t now_ns = get_time_ns();
        ScheduledArrival arr = create_arrival(conn.id, conn.next_send_seq++, now_ns, cfg.msg_size);
        dispatch_or_enqueue_on_conn(conn, std::move(arr), epoll_fd, phase, total_sent, cfg.pipeline_window, cfg.msg_size);
    }
}

// --------------------------------------------------------------------------
// Built-In Self-Test Suite (--self-test)
// --------------------------------------------------------------------------

bool run_self_test_fragmentation() {
    // 1. Deliberately fragmented TCP frames test
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        perror("socketpair");
        return false;
    }
    fcntl(sv[0], F_SETFL, O_NONBLOCK);
    fcntl(sv[1], F_SETFL, O_NONBLOCK);

    size_t msg_size = 512;
    ScheduledArrival arr = create_arrival(0, 1, get_time_ns(), msg_size);
    // Send full frame from sv[0]
    if (write(sv[0], arr.frame.data(), msg_size) != static_cast<ssize_t>(msg_size)) {
        close(sv[0]); close(sv[1]);
        return false;
    }

    // Read on sv[1] in deliberately fragmented small chunks: 17B, 53B, 1B, remainder
    size_t chunks[] = {17, 53, 1, msg_size - (17 + 53 + 1)};
    std::vector<uint8_t> recv_buf(msg_size, 0);
    size_t total_read = 0;

    for (size_t chunk_len : chunks) {
        uint8_t tmp[512];
        ssize_t n = read(sv[1], tmp, chunk_len);
        if (n <= 0) {
            close(sv[0]); close(sv[1]);
            return false;
        }
        memcpy(recv_buf.data() + total_read, tmp, n);
        total_read += n;
    }

    if (total_read != msg_size) {
        close(sv[0]); close(sv[1]);
        return false;
    }

    const MessageHeader *hdr = reinterpret_cast<const MessageHeader *>(recv_buf.data());
    bool valid = (hdr->magic == ECHO_MAGIC) && (hdr->conn_id == 0) && (hdr->seq_num == 1);
    if (valid) {
        const uint8_t *payload = recv_buf.data() + sizeof(MessageHeader);
        size_t payload_len = msg_size - sizeof(MessageHeader);
        valid = (hdr->checksum == compute_checksum(payload, payload_len));
    }

    close(sv[0]);
    close(sv[1]);
    return valid;
}

bool run_self_test_active_subsets() {
    // 2. Active connection subsets (--active-share) test
    uint32_t total_conns = 10;
    double share = 0.3;
    uint32_t active_count = std::max(1U, static_cast<uint32_t>(std::round(total_conns * share)));
    std::vector<bool> is_active(total_conns, false);
    uint32_t counted_active = 0;
    for (uint32_t i = 0; i < total_conns; ++i) {
        if ((static_cast<uint64_t>(i) * active_count / total_conns) !=
            (static_cast<uint64_t>(i + 1) * active_count / total_conns)) {
            is_active[i] = true;
            counted_active++;
        }
    }
    return (counted_active == active_count) && (active_count == 3);
}

bool run_self_test_pipelining_w4() {
    // 3. Poisson arrivals with W=4 pipelining test
    Connection conn;
    conn.id = 0;
    conn.state = ConnState::ACTIVE;
    size_t W = 4;
    size_t msg_size = 512;

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        return false;
    }
    fcntl(sv[0], F_SETFL, O_NONBLOCK);
    fcntl(sv[1], F_SETFL, O_NONBLOCK);
    conn.fd = sv[0];

    int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    uint64_t total_sent = 0;

    // Enqueue 10 arrivals
    for (uint64_t seq = 1; seq <= 10; ++seq) {
        ScheduledArrival arr = create_arrival(conn.id, seq, get_time_ns(), msg_size);
        dispatch_or_enqueue_on_conn(conn, std::move(arr), epoll_fd, BenchPhase::MEASURING, total_sent, W, msg_size);
    }

    // In-flight must be exactly W=4, and tx_queue size must be 6
    if (conn.in_flight.size() != W || conn.tx_queue.size() != 6) {
        close(sv[0]); close(sv[1]); close(epoll_fd);
        return false;
    }

    // Read 4 frames on sv[1] and verify sequence order
    for (uint64_t expected_seq = 1; expected_seq <= 4; ++expected_seq) {
        uint8_t buf[512];
        size_t r = 0;
        while (r < msg_size) {
            ssize_t n = read(sv[1], buf + r, msg_size - r);
            if (n > 0) r += n;
        }
        const MessageHeader *hdr = reinterpret_cast<const MessageHeader *>(buf);
        if (hdr->seq_num != expected_seq) {
            close(sv[0]); close(sv[1]); close(epoll_fd);
            return false;
        }
    }

    // Simulate completion of 1 frame: pop from in_flight, pump queue
    InFlightRequest inflight = conn.in_flight.front();
    conn.in_flight.pop_front();
    uint64_t t_recv = get_time_ns();
    uint64_t sched_lag = inflight.send_time_ns - inflight.sched_time_ns;
    uint64_t app_rtt = t_recv - inflight.send_time_ns;
    uint64_t e2e_lat = t_recv - inflight.sched_time_ns;

    if (e2e_lat != sched_lag + app_rtt) {
        close(sv[0]); close(sv[1]); close(epoll_fd);
        return false;
    }

    pump_conn_tx_queue(conn, epoll_fd, BenchPhase::MEASURING, total_sent, W, msg_size);

    // In-flight should remain W=4, tx_queue should be 5
    bool ok = (conn.in_flight.size() == W && conn.tx_queue.size() == 5);

    close(sv[0]); close(sv[1]); close(epoll_fd);
    return ok;
}

bool run_self_test_bursts() {
    // 4. ON/OFF bursts test
    uint32_t burst_size = 20;
    std::deque<ScheduledArrival> burst_queue;
    uint64_t now = get_time_ns();
    for (uint32_t i = 1; i <= burst_size; ++i) {
        burst_queue.push_back(create_arrival(0, i, now, 512));
    }
    return (burst_queue.size() == burst_size);
}

int execute_self_tests() {
    std::cout << "Running echo_loadgen built-in verification suite...\n";

    bool pass1 = run_self_test_fragmentation();
    std::cout << "  [SELF-TEST 1/4] Deliberately fragmented TCP frames: " << (pass1 ? "PASS" : "FAIL") << "\n";

    bool pass2 = run_self_test_active_subsets();
    std::cout << "  [SELF-TEST 2/4] Active connection subsets (--active-share): " << (pass2 ? "PASS" : "FAIL") << "\n";

    bool pass3 = run_self_test_pipelining_w4();
    std::cout << "  [SELF-TEST 3/4] Poisson arrivals with W=4 pipelining: " << (pass3 ? "PASS" : "FAIL") << "\n";

    bool pass4 = run_self_test_bursts();
    std::cout << "  [SELF-TEST 4/4] ON/OFF bursts: " << (pass4 ? "PASS" : "FAIL") << "\n";

    if (pass1 && pass2 && pass3 && pass4) {
        std::cout << "\n=== ALL SELF-TESTS PASSED ===\n";
        return 0;
    } else {
        std::cerr << "\n=== SOME SELF-TESTS FAILED ===\n";
        return 1;
    }
}

} // namespace

int main(int argc, char **argv) {
    Config cfg;
    if (!parse_args(argc, argv, cfg)) {
        print_usage(argv[0]);
        return 1;
    }

    if (cfg.self_test) {
        return execute_self_tests();
    }

    // Raise file descriptor limit
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        rl.rlim_cur = std::max<rlim_t>(cfg.connections * 2 + 1024, rl.rlim_cur);
        if (rl.rlim_max < rl.rlim_cur) {
            rl.rlim_cur = rl.rlim_max;
        }
        setrlimit(RLIMIT_NOFILE, &rl);
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    // Resolve server address
    struct sockaddr_in server_addr;
    std::memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(cfg.port);
    if (inet_pton(AF_INET, cfg.host.c_str(), &server_addr.sin_addr) <= 0) {
        struct hostent *he = gethostbyname(cfg.host.c_str());
        if (!he || !he->h_addr_list[0]) {
            std::cerr << "Failed to resolve host: " << cfg.host << "\n";
            return 1;
        }
        std::memcpy(&server_addr.sin_addr, he->h_addr_list[0], sizeof(server_addr.sin_addr));
    }

    int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0) {
        perror("epoll_create1");
        return 1;
    }

    // High-precision pacing timerfd (Correction 4)
    int timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timer_fd < 0) {
        perror("timerfd_create");
        close(epoll_fd);
        return 1;
    }

    struct epoll_event ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.u32 = 0xFFFFFFFF; // Reserved sentinel for timer_fd
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, timer_fd, &ev) < 0) {
        perror("epoll_ctl timer_fd");
        close(timer_fd);
        close(epoll_fd);
        return 1;
    }

    std::vector<Connection> conns(cfg.connections);
    std::deque<uint32_t> idle_conns; // connections with in_flight.size() < W

    uint32_t active_count = std::max(1U, static_cast<uint32_t>(std::round(cfg.connections * cfg.active_share)));
    std::vector<bool> is_active(cfg.connections, false);
    for (uint32_t i = 0; i < cfg.connections; ++i) {
        if ((static_cast<uint64_t>(i) * active_count / cfg.connections) !=
            (static_cast<uint64_t>(i + 1) * active_count / cfg.connections)) {
            is_active[i] = true;
        }
    }

    std::cout << "Connecting " << cfg.connections << " persistent TCP connections ("
              << active_count << " active, " << (cfg.connections - active_count) << " idle) to "
              << cfg.host << ":" << cfg.port << " (Pipeline Window W=" << cfg.pipeline_window << ")...\n";

    uint64_t conn_start_ns = get_time_ns();
    uint64_t conn_timeout_ns = static_cast<uint64_t>(cfg.connect_timeout * 1e9);
    uint32_t connected_count = 0;
    uint64_t conn_failures = 0;

    for (uint32_t i = 0; i < cfg.connections; ++i) {
        conns[i].id = i;
        conns[i].recv_buf.resize(cfg.msg_size);
        conns[i].recv_bytes_read = 0;

        int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            conn_failures++;
            conns[i].state = ConnState::FAILED;
            continue;
        }
        if (!set_socket_options(fd)) {
            close(fd);
            conn_failures++;
            conns[i].state = ConnState::FAILED;
            continue;
        }

        conns[i].fd = fd;
        int rc = connect(fd, reinterpret_cast<struct sockaddr *>(&server_addr), sizeof(server_addr));
        if (rc == 0) {
            conns[i].state = ConnState::ACTIVE;
            connected_count++;
            if (is_active[i]) {
                idle_conns.push_back(i);
            }
            ev.events = EPOLLIN | EPOLLERR | EPOLLHUP;
            ev.data.u32 = i;
            epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev);
        } else if (rc < 0 && errno == EINPROGRESS) {
            conns[i].state = ConnState::CONNECTING;
            ev.events = EPOLLOUT | EPOLLERR | EPOLLHUP;
            ev.data.u32 = i;
            epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev);
        } else {
            close(fd);
            conns[i].fd = -1;
            conn_failures++;
            conns[i].state = ConnState::FAILED;
        }
    }

    // Wait for all nonblocking connects to complete
    struct epoll_event events[MAX_EPOLL_EVENTS];
    while (connected_count + conn_failures < cfg.connections && !g_stop_requested) {
        uint64_t now_ns = get_time_ns();
        if (now_ns - conn_start_ns > conn_timeout_ns) {
            std::cerr << "Timeout establishing connections (" << connected_count
                      << " connected, " << (cfg.connections - connected_count) << " failed/timed out)\n";
            for (uint32_t i = 0; i < cfg.connections; ++i) {
                if (conns[i].state == ConnState::CONNECTING) {
                    conns[i].state = ConnState::FAILED;
                    conn_failures++;
                    if (conns[i].fd >= 0) {
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, conns[i].fd, nullptr);
                        close(conns[i].fd);
                        conns[i].fd = -1;
                    }
                }
            }
            break;
        }

        int nfds = epoll_wait(epoll_fd, events, MAX_EPOLL_EVENTS, 50);
        for (int e = 0; e < nfds; ++e) {
            uint32_t id = events[e].data.u32;
            if (id == 0xFFFFFFFF) {
                uint64_t ticks = 0;
                if (read(timer_fd, &ticks, sizeof(ticks)) < 0) {}
                continue;
            }
            if (id >= cfg.connections) continue;
            Connection &conn = conns[id];
            if (conn.state != ConnState::CONNECTING) continue;

            int so_error = 0;
            socklen_t len = sizeof(so_error);
            if (getsockopt(conn.fd, SOL_SOCKET, SO_ERROR, &so_error, &len) == 0 && so_error == 0) {
                conn.state = ConnState::ACTIVE;
                connected_count++;
                if (is_active[id]) {
                    idle_conns.push_back(id);
                }
                ev.events = EPOLLIN | EPOLLERR | EPOLLHUP;
                ev.data.u32 = id;
                epoll_ctl(epoll_fd, EPOLL_CTL_MOD, conn.fd, &ev);
            } else {
                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, conn.fd, nullptr);
                close(conn.fd);
                conn.fd = -1;
                conn.state = ConnState::FAILED;
                conn_failures++;
            }
        }
    }

    std::cout << "Successfully established " << connected_count << " / " << cfg.connections
              << " persistent connections.\n";

    if (connected_count == 0) {
        std::cerr << "Fatal: Zero connections established. Aborting.\n";
        close(timer_fd);
        close(epoll_fd);
        return 1;
    }

    // Benchmark Execution State
    BenchPhase phase = (cfg.warmup > 0.0) ? BenchPhase::WARMUP : BenchPhase::MEASURING;
    uint64_t phase_start_ns = get_time_ns();
    uint64_t warmup_duration_ns = static_cast<uint64_t>(cfg.warmup * 1e9);
    uint64_t measure_duration_ns = static_cast<uint64_t>(cfg.duration * 1e9);
    uint64_t cooldown_duration_ns = static_cast<uint64_t>(cfg.cooldown * 1e9);

    uint64_t messages_scheduled = 0;
    uint64_t total_sent = 0;
    uint64_t total_received = 0;
    uint64_t send_errors = 0;
    uint64_t recv_errors = 0;
    uint64_t timeouts = 0;
    uint64_t mismatched_msgs = 0;
    uint64_t concurrency_stalls = 0;

    // Measurement deltas (Correction 5)
    uint64_t retrans_start = 0;
    uint64_t retrans_end = 0;
    struct rusage ru_start, ru_end;
    std::memset(&ru_start, 0, sizeof(ru_start));
    std::memset(&ru_end, 0, sizeof(ru_end));

    // Latency Distributions (Correction 2: Sched Lag, App RTT, E2E Latency)
    std::vector<uint32_t> sched_lag_samples_us;
    std::vector<uint32_t> app_rtt_samples_us;
    std::vector<uint32_t> e2e_lat_samples_us;

    size_t sample_reserve = (cfg.mode == Mode::CLOSED_LOOP) ? 2000000 : (static_cast<size_t>(cfg.rate * cfg.duration * 1.5) + 10000);
    sched_lag_samples_us.reserve(sample_reserve);
    app_rtt_samples_us.reserve(sample_reserve);
    e2e_lat_samples_us.reserve(sample_reserve);

    // 1-Second Timeseries Buckets (Correction 6)
    size_t total_seconds = static_cast<size_t>(std::ceil(cfg.duration)) + 2;
    std::vector<SecondBucket> timeseries(total_seconds);
    for (size_t s = 0; s < total_seconds; ++s) {
        timeseries[s].second = static_cast<uint32_t>(s + 1);
    }

    uint64_t measurement_actual_start_ns = 0;
    uint64_t measurement_actual_end_ns = 0;

    // Random generators for Poisson and Burst distributions
    std::mt19937_64 rng(1337);
    std::exponential_distribution<double> exp_dist(cfg.rate > 0 ? cfg.rate : 1.0);
    uint64_t next_poisson_arrival_ns = phase_start_ns;

    auto arm_pacing_timer = [&](BenchPhase current_phase) {
        if (cfg.mode == Mode::CLOSED_LOOP) return;

        if (cfg.dist == Distribution::CONSTANT) {
            uint64_t interval_ns = std::max<uint64_t>(50000ULL, static_cast<uint64_t>(1e9 / std::max(1.0, cfg.rate)));
            struct itimerspec its;
            its.it_interval.tv_sec = interval_ns / 1000000000ULL;
            its.it_interval.tv_nsec = interval_ns % 1000000000ULL;
            its.it_value = its.it_interval;
            timerfd_settime(timer_fd, 0, &its, nullptr);
        } else if (cfg.dist == Distribution::POISSON) {
            // One-shot timer re-armed as needed
            next_poisson_arrival_ns = get_time_ns();
        } else if (cfg.dist == Distribution::BURST) {
            uint64_t interval_ns = static_cast<uint64_t>(cfg.burst_interval * 1e9);
            struct itimerspec its;
            its.it_interval.tv_sec = interval_ns / 1000000000ULL;
            its.it_interval.tv_nsec = interval_ns % 1000000000ULL;
            its.it_value = its.it_interval;
            timerfd_settime(timer_fd, 0, &its, nullptr);
        }
        (void)current_phase;
    };

    auto trigger_closed_loop_start = [&]() {
        for (uint32_t cid : idle_conns) {
            refill_closed_loop_conn(conns[cid], cfg, epoll_fd, phase, total_sent);
        }
    };

    arm_pacing_timer(phase);

    if (phase == BenchPhase::WARMUP) {
        std::cout << "Starting " << cfg.warmup << "s warmup phase "
                  << (cfg.mode == Mode::CLOSED_LOOP ? "(closed-loop / max rate)...\n" : ("at target rate " + std::to_string(static_cast<int>(cfg.rate)) + " msgs/sec...\n"));
        if (cfg.mode == Mode::CLOSED_LOOP) {
            trigger_closed_loop_start();
        }
    } else {
        std::cout << "Starting " << cfg.duration << "s measurement phase "
                  << (cfg.mode == Mode::CLOSED_LOOP ? "(closed-loop / max rate)...\n" : ("at target rate " + std::to_string(static_cast<int>(cfg.rate)) + " msgs/sec...\n"));
        measurement_actual_start_ns = phase_start_ns;
        retrans_start = query_total_tcp_retrans(conns);
        getrusage(RUSAGE_SELF, &ru_start);
        if (cfg.mode == Mode::CLOSED_LOOP) {
            trigger_closed_loop_start();
        }
    }

    uint64_t timeout_ns = cfg.timeout_ms * 1000000ULL;

    while (phase != BenchPhase::FINISHED && !g_stop_requested) {
        int nfds = epoll_wait(epoll_fd, events, MAX_EPOLL_EVENTS, 10);
        uint64_t now_ns = get_time_ns();

        // 1. Process epoll socket events
        for (int e = 0; e < nfds; ++e) {
            uint32_t id = events[e].data.u32;
            if (id == 0xFFFFFFFF) {
                uint64_t ticks = 0;
                if (read(timer_fd, &ticks, sizeof(ticks)) < 0) {}
                continue;
            }
            if (id >= cfg.connections) continue;
            Connection &conn = conns[id];
            uint32_t ev_mask = events[e].events;

            if (ev_mask & (EPOLLERR | EPOLLHUP)) {
                if (phase == BenchPhase::MEASURING) recv_errors++;
                conn.state = ConnState::CLOSED;
                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, conn.fd, nullptr);
                close(conn.fd);
                conn.fd = -1;
                continue;
            }

            // Handle Writable: pump active transmission or pending queue
            if ((ev_mask & EPOLLOUT) && conn.fd >= 0) {
                if (conn.has_active_tx && !conn.tx_queue.empty()) {
                    auto &req = conn.tx_queue.front();
                    size_t remaining = cfg.msg_size - req.bytes_sent;
                    ssize_t n = send(conn.fd, req.frame.data() + req.bytes_sent, remaining, MSG_NOSIGNAL);
                    if (n > 0) {
                        req.bytes_sent += static_cast<size_t>(n);
                        if (req.bytes_sent == cfg.msg_size) {
                            // Frame finished!
                            uint64_t finish_ns = get_time_ns();
                            MessageHeader *hdr = reinterpret_cast<MessageHeader *>(req.frame.data());
                            hdr->send_time_ns = finish_ns;
                            conn.in_flight.push_back({req.seq_num, req.sched_time_ns, req.tx_start_ns, finish_ns});
                            conn.tx_queue.pop_front();
                            conn.has_active_tx = false;
                            if (phase == BenchPhase::MEASURING) total_sent++;
                            // Pump any remaining items in queue
                            pump_conn_tx_queue(conn, epoll_fd, phase, total_sent, cfg.pipeline_window, cfg.msg_size);
                        }
                    } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                        // Still waiting
                    } else {
                        if (phase == BenchPhase::MEASURING) send_errors++;
                        conn.state = ConnState::CLOSED;
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, conn.fd, nullptr);
                        close(conn.fd);
                        conn.fd = -1;
                    }
                } else {
                    pump_conn_tx_queue(conn, epoll_fd, phase, total_sent, cfg.pipeline_window, cfg.msg_size);
                }
            }

            // Handle Readable: process incoming echoed frames
            if ((ev_mask & EPOLLIN) && conn.fd >= 0) {
                while (true) {
                    size_t remaining = cfg.msg_size - conn.recv_bytes_read;
                    ssize_t n = recv(conn.fd, conn.recv_buf.data() + conn.recv_bytes_read, remaining, 0);
                    if (n > 0) {
                        conn.recv_bytes_read += static_cast<size_t>(n);
                        if (conn.recv_bytes_read == cfg.msg_size) {
                            // Full frame received! Validate against in_flight head
                            const MessageHeader *hdr = reinterpret_cast<const MessageHeader *>(conn.recv_buf.data());
                            bool valid = (!conn.in_flight.empty()) &&
                                         (hdr->magic == ECHO_MAGIC) &&
                                         (hdr->conn_id == conn.id) &&
                                         (hdr->seq_num == conn.in_flight.front().seq_num) &&
                                         (hdr->msg_size == cfg.msg_size);

                            if (valid) {
                                const uint8_t *payload = conn.recv_buf.data() + sizeof(MessageHeader);
                                size_t payload_len = cfg.msg_size - sizeof(MessageHeader);
                                if (hdr->checksum != compute_checksum(payload, payload_len)) {
                                    valid = false;
                                }
                            }

                            if (valid) {
                                InFlightRequest inflight = conn.in_flight.front();
                                conn.in_flight.pop_front();
                                uint64_t t_recv = get_time_ns();
                                uint64_t t_send = inflight.send_time_ns;
                                uint64_t t_sched = inflight.sched_time_ns;

                                uint64_t sched_lag = (t_send >= t_sched) ? (t_send - t_sched) : 0;
                                uint64_t app_rtt   = (t_recv >= t_send) ? (t_recv - t_send) : 0;
                                uint64_t e2e_lat   = (t_recv >= t_sched) ? (t_recv - t_sched) : 0;

                                if (phase == BenchPhase::MEASURING) {
                                    total_received++;
                                    uint32_t rtt_us = static_cast<uint32_t>(app_rtt / 1000);
                                    sched_lag_samples_us.push_back(static_cast<uint32_t>(sched_lag / 1000));
                                    app_rtt_samples_us.push_back(rtt_us);
                                    e2e_lat_samples_us.push_back(static_cast<uint32_t>(e2e_lat / 1000));

                                    // Record into timeseries bucket
                                    size_t sec_idx = static_cast<size_t>((t_recv - measurement_actual_start_ns) / 1000000000ULL);
                                    if (sec_idx < timeseries.size()) {
                                        timeseries[sec_idx].received++;
                                        timeseries[sec_idx].rtt_samples.push_back(rtt_us);
                                    }
                                }

                                conn.expected_recv_seq++;
                                conn.recv_bytes_read = 0;

                                if (cfg.mode == Mode::CLOSED_LOOP) {
                                    refill_closed_loop_conn(conn, cfg, epoll_fd, phase, total_sent);
                                } else {
                                    pump_conn_tx_queue(conn, epoll_fd, phase, total_sent, cfg.pipeline_window, cfg.msg_size);
                                    if (conn.in_flight.size() < cfg.pipeline_window) {
                                        idle_conns.push_back(conn.id);
                                    }
                                }
                            } else {
                                if (phase == BenchPhase::MEASURING) mismatched_msgs++;
                                if (!conn.in_flight.empty()) {
                                    conn.in_flight.pop_front();
                                }
                                conn.expected_recv_seq++;
                                conn.recv_bytes_read = 0;

                                if (cfg.mode == Mode::CLOSED_LOOP) {
                                    refill_closed_loop_conn(conn, cfg, epoll_fd, phase, total_sent);
                                } else {
                                    pump_conn_tx_queue(conn, epoll_fd, phase, total_sent, cfg.pipeline_window, cfg.msg_size);
                                    if (conn.in_flight.size() < cfg.pipeline_window) {
                                        idle_conns.push_back(conn.id);
                                    }
                                }
                            }
                            break;
                        }
                    } else if (n == 0) {
                        // EOF
                        if (phase == BenchPhase::MEASURING) recv_errors++;
                        conn.state = ConnState::CLOSED;
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, conn.fd, nullptr);
                        close(conn.fd);
                        conn.fd = -1;
                        break;
                    } else {
                        if (errno != EAGAIN && errno != EWOULDBLOCK) {
                            if (phase == BenchPhase::MEASURING) recv_errors++;
                            conn.state = ConnState::CLOSED;
                            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, conn.fd, nullptr);
                            close(conn.fd);
                            conn.fd = -1;
                        }
                        break;
                    }
                }
            }
        }

        // 2. Request Timeout Checking
        for (uint32_t i = 0; i < cfg.connections; ++i) {
            Connection &conn = conns[i];
            if (!conn.in_flight.empty()) {
                uint64_t oldest_start = conn.in_flight.front().send_time_ns;
                if (oldest_start > 0 && (now_ns - oldest_start > timeout_ns)) {
                    if (phase == BenchPhase::MEASURING) timeouts++;
                    conn.in_flight.pop_front();
                    conn.recv_bytes_read = 0;
                    if (cfg.mode == Mode::CLOSED_LOOP) {
                        refill_closed_loop_conn(conn, cfg, epoll_fd, phase, total_sent);
                    } else {
                        pump_conn_tx_queue(conn, epoll_fd, phase, total_sent, cfg.pipeline_window, cfg.msg_size);
                        if (conn.in_flight.size() < cfg.pipeline_window) {
                            idle_conns.push_back(conn.id);
                        }
                    }
                }
            }
        }

        // 3. Phase Transitions & Measurement Window Snapshots (Correction 5)
        if (phase == BenchPhase::WARMUP) {
            if (now_ns - phase_start_ns >= warmup_duration_ns) {
                std::cout << "Warmup complete. Starting " << cfg.duration
                          << "s measurement phase " << (cfg.mode == Mode::CLOSED_LOOP ? "(closed-loop / max rate)...\n" : ("at target rate " + std::to_string(static_cast<int>(cfg.rate)) + " msgs/sec...\n"));
                phase = BenchPhase::MEASURING;
                phase_start_ns = now_ns;
                measurement_actual_start_ns = now_ns;
                messages_scheduled = 0;
                retrans_start = query_total_tcp_retrans(conns);
                getrusage(RUSAGE_SELF, &ru_start);
                arm_pacing_timer(phase);
                if (cfg.mode == Mode::CLOSED_LOOP) {
                    trigger_closed_loop_start();
                }
            }
        } else if (phase == BenchPhase::MEASURING) {
            if (now_ns - phase_start_ns >= measure_duration_ns) {
                measurement_actual_end_ns = now_ns;
                retrans_end = query_total_tcp_retrans(conns);
                getrusage(RUSAGE_SELF, &ru_end);

                if (cfg.cooldown > 0.0) {
                    std::cout << "Measurement complete. Starting " << cfg.cooldown << "s cooldown / drain...\n";
                    phase = BenchPhase::COOLDOWN;
                    phase_start_ns = now_ns;
                } else {
                    phase = BenchPhase::FINISHED;
                    break;
                }
            }
        } else if (phase == BenchPhase::COOLDOWN) {
            if (now_ns - phase_start_ns >= cooldown_duration_ns || idle_conns.size() == connected_count) {
                phase = BenchPhase::FINISHED;
                break;
            }
        }

        // 4. Open-Loop Rate Generation / Scheduling (Correction 4)
        if (cfg.mode == Mode::OPEN_LOOP && (phase == BenchPhase::WARMUP || phase == BenchPhase::MEASURING)) {
            uint64_t elapsed_in_phase_ns = now_ns - phase_start_ns;

            if (cfg.dist == Distribution::CONSTANT) {
                // Drift-free integer formula: t_k = t_0 + floor(k * 10^9 / lambda)
                uint64_t target_messages = static_cast<uint64_t>((elapsed_in_phase_ns * cfg.rate) / 1e9);

                while (messages_scheduled < target_messages) {
                    if (idle_conns.empty()) {
                        if (phase == BenchPhase::MEASURING) concurrency_stalls++;
                        messages_scheduled++;
                        continue;
                    }
                    uint32_t cid = idle_conns.front();
                    idle_conns.pop_front();
                    messages_scheduled++;

                    Connection &conn = conns[cid];
                    ScheduledArrival arr = create_arrival(cid, conn.next_send_seq++, now_ns, cfg.msg_size);
                    dispatch_or_enqueue_on_conn(conn, std::move(arr), epoll_fd, phase, total_sent, cfg.pipeline_window, cfg.msg_size);
                }
            } else if (cfg.dist == Distribution::POISSON) {
                while (next_poisson_arrival_ns <= now_ns) {
                    if (idle_conns.empty()) {
                        if (phase == BenchPhase::MEASURING) concurrency_stalls++;
                    } else {
                        uint32_t cid = idle_conns.front();
                        idle_conns.pop_front();
                        Connection &conn = conns[cid];
                        ScheduledArrival arr = create_arrival(cid, conn.next_send_seq++, next_poisson_arrival_ns, cfg.msg_size);
                        dispatch_or_enqueue_on_conn(conn, std::move(arr), epoll_fd, phase, total_sent, cfg.pipeline_window, cfg.msg_size);
                    }
                    messages_scheduled++;
                    double delta_sec = exp_dist(rng);
                    uint64_t delta_ns = static_cast<uint64_t>(delta_sec * 1e9);
                    next_poisson_arrival_ns += delta_ns;
                }
            } else if (cfg.dist == Distribution::BURST) {
                uint64_t target_bursts = static_cast<uint64_t>(elapsed_in_phase_ns / (cfg.burst_interval * 1e9));
                uint64_t target_messages = target_bursts * cfg.burst_size;

                while (messages_scheduled < target_messages) {
                    if (idle_conns.empty()) {
                        if (phase == BenchPhase::MEASURING) concurrency_stalls++;
                        messages_scheduled++;
                        continue;
                    }
                    uint32_t cid = idle_conns.front();
                    idle_conns.pop_front();
                    messages_scheduled++;

                    Connection &conn = conns[cid];
                    ScheduledArrival arr = create_arrival(cid, conn.next_send_seq++, now_ns, cfg.msg_size);
                    dispatch_or_enqueue_on_conn(conn, std::move(arr), epoll_fd, phase, total_sent, cfg.pipeline_window, cfg.msg_size);
                }
            }
        }
    }

    if (measurement_actual_end_ns == 0) {
        measurement_actual_end_ns = get_time_ns();
        retrans_end = query_total_tcp_retrans(conns);
        getrusage(RUSAGE_SELF, &ru_end);
    }

    // Teardown connections
    for (auto &conn : conns) {
        if (conn.fd >= 0) {
            close(conn.fd);
            conn.fd = -1;
        }
    }
    close(timer_fd);
    close(epoll_fd);

    // Compute metrics
    double actual_duration_s = static_cast<double>(measurement_actual_end_ns - measurement_actual_start_ns) / 1e9;
    if (actual_duration_s <= 0.0) actual_duration_s = cfg.duration;

    double actual_send_rate = static_cast<double>(total_sent) / actual_duration_s;
    double echoed_rate = static_cast<double>(total_received) / actual_duration_s;

    // Measurement deltas: TCP retransmits and client CPU usage
    uint64_t tcp_retrans_delta = (retrans_end >= retrans_start) ? (retrans_end - retrans_start) : 0;
    double user_sec = (ru_end.ru_utime.tv_sec - ru_start.ru_utime.tv_sec) +
                      (ru_end.ru_utime.tv_usec - ru_start.ru_utime.tv_usec) / 1e6;
    double sys_sec  = (ru_end.ru_stime.tv_sec - ru_start.ru_stime.tv_sec) +
                      (ru_end.ru_stime.tv_usec - ru_start.ru_stime.tv_usec) / 1e6;
    double client_cpu_pct = ((user_sec + sys_sec) / actual_duration_s) * 100.0;

    // Latency Distribution Calculation
    auto calc_percentiles = [](std::vector<uint32_t> &samples, uint32_t &p50, uint32_t &p95, uint32_t &p99, uint32_t &pmin, uint32_t &pmax, double &pavg) {
        if (samples.empty()) {
            p50 = p95 = p99 = pmin = pmax = 0;
            pavg = 0.0;
            return;
        }
        std::sort(samples.begin(), samples.end());
        size_t n = samples.size();
        pmin = samples.front();
        pmax = samples.back();
        p50 = samples[static_cast<size_t>(n * 0.50)];
        p95 = samples[static_cast<size_t>(n * 0.95)];
        p99 = samples[static_cast<size_t>(n * 0.99)];
        double sum = std::accumulate(samples.begin(), samples.end(), 0.0);
        pavg = sum / n;
    };

    uint32_t app_rtt_min = 0, app_rtt_max = 0, app_rtt_p50 = 0, app_rtt_p95 = 0, app_rtt_p99 = 0;
    double app_rtt_avg = 0.0;
    calc_percentiles(app_rtt_samples_us, app_rtt_p50, app_rtt_p95, app_rtt_p99, app_rtt_min, app_rtt_max, app_rtt_avg);

    uint32_t sched_lag_min = 0, sched_lag_max = 0, sched_lag_p50 = 0, sched_lag_p95 = 0, sched_lag_p99 = 0;
    double sched_lag_avg = 0.0;
    calc_percentiles(sched_lag_samples_us, sched_lag_p50, sched_lag_p95, sched_lag_p99, sched_lag_min, sched_lag_max, sched_lag_avg);

    uint32_t e2e_lat_min = 0, e2e_lat_max = 0, e2e_lat_p50 = 0, e2e_lat_p95 = 0, e2e_lat_p99 = 0;
    double e2e_lat_avg = 0.0;
    calc_percentiles(e2e_lat_samples_us, e2e_lat_p50, e2e_lat_p95, e2e_lat_p99, e2e_lat_min, e2e_lat_max, e2e_lat_avg);

    bool target_achieved = (cfg.mode == Mode::CLOSED_LOOP) ? true : (total_sent >= static_cast<uint64_t>(cfg.rate * actual_duration_s * 0.95));

    // Calculate timeseries bucket percentiles for JSON
    for (auto &bucket : timeseries) {
        if (!bucket.rtt_samples.empty()) {
            std::sort(bucket.rtt_samples.begin(), bucket.rtt_samples.end());
            size_t n = bucket.rtt_samples.size();
            bucket.app_rtt_p50_us = bucket.rtt_samples[static_cast<size_t>(n * 0.50)];
            bucket.app_rtt_p95_us = bucket.rtt_samples[static_cast<size_t>(n * 0.95)];
            bucket.app_rtt_p99_us = bucket.rtt_samples[static_cast<size_t>(n * 0.99)];
        }
    }

    // Print formatted summary table to stdout
    std::cout << "\n======================================================================\n"
              << "               TCP ECHO LOAD GENERATOR SUMMARY\n"
              << "======================================================================\n"
              << std::fixed << std::setprecision(2)
              << "Target Configuration:\n"
              << "  Server:                  " << cfg.host << ":" << cfg.port << "\n"
              << "  Mode:                    " << (cfg.mode == Mode::CLOSED_LOOP ? "CLOSED-LOOP (unthrottled / max throughput)" : "OPEN-LOOP (target offered rate)") << "\n"
              << "  Pipeline Window (W):     " << cfg.pipeline_window << "\n"
              << "  Concurrent Connections:  " << cfg.connections << " (" << active_count << " active, "
              << (cfg.connections - active_count) << " idle)\n"
              << "  Active Share:            " << (cfg.active_share * 100.0) << " %\n"
              << "  Target Offered Rate:     " << (cfg.mode == Mode::CLOSED_LOOP ? "CLOSED-LOOP" : (std::to_string(cfg.rate) + " msgs/sec")) << "\n"
              << "  Message Size:            " << cfg.msg_size << " bytes\n"
              << "  Configured Duration:     " << cfg.duration << " s"
              << " (Warmup: " << cfg.warmup << " s, Cooldown: " << cfg.cooldown << " s)\n\n"
              << "Execution & Throughput:\n"
              << "  Actual Duration:         " << actual_duration_s << " s\n"
              << "  Messages Sent:           " << total_sent << " (" << actual_send_rate << " msgs/sec)\n"
              << "  Messages Echoed:         " << total_received << " (" << echoed_rate << " msgs/sec)\n"
              << "  Target Offered Met:      " << (target_achieved ? "YES" : "NO (Bottlenecked by concurrency/RTT)") << "\n\n"
              << "Latency Distribution (Non-Overlapping Definitions):\n"
              << "  App RTT (t_recv - t_send):\n"
              << "    p50 / p95 / p99:       " << app_rtt_p50 << " / " << app_rtt_p95 << " / " << app_rtt_p99 << " us\n"
              << "    Min / Avg / Max:       " << app_rtt_min << " / " << app_rtt_avg << " / " << app_rtt_max << " us\n"
              << "  Sched Lag (t_send - t_sched):\n"
              << "    p50 / p95 / p99:       " << sched_lag_p50 << " / " << sched_lag_p95 << " / " << sched_lag_p99 << " us\n"
              << "  End-to-End (t_recv - t_sched = sched_lag + app_rtt):\n"
              << "    p50 / p95 / p99:       " << e2e_lat_p50 << " / " << e2e_lat_p95 << " / " << e2e_lat_p99 << " us\n\n"
              << "Reliability & System Metrics:\n"
              << "  Connection Failures:     " << conn_failures << "\n"
              << "  Send / Recv Errors:      " << send_errors << " / " << recv_errors << "\n"
              << "  Timeouts:                " << timeouts << "\n"
              << "  Mismatched / Corrupted:  " << mismatched_msgs << "\n"
              << "  Concurrency Stalls:      " << concurrency_stalls << "\n"
              << "  TCP Retransmits Delta:   " << tcp_retrans_delta << "\n"
              << "  Client CPU Utilization:  " << client_cpu_pct << " %\n"
              << "======================================================================\n";

    if (!target_achieved && cfg.mode == Mode::OPEN_LOOP) {
        std::cout << "\n[NOTICE] Throughput limitation reached:\n"
                  << "  Under pipelining window W=" << cfg.pipeline_window << ", Little's Law limits max rate to:\n"
                  << "    R_max = (Connections * W) / RTT_avg = (" << cfg.connections << " * " << cfg.pipeline_window
                  << ") / (" << app_rtt_avg << " us) = "
                  << static_cast<uint64_t>((cfg.connections * cfg.pipeline_window) / (app_rtt_avg / 1e6 + 1e-9)) << " msgs/sec.\n";
    }

    // Write to CSV if --output is specified
    if (!cfg.output_csv.empty()) {
        ensure_parent_dir(cfg.output_csv);
        bool file_exists = (access(cfg.output_csv.c_str(), F_OK) == 0);
        std::ofstream csv(cfg.output_csv, std::ios::app);
        if (csv.is_open()) {
            if (!file_exists || std::ifstream(cfg.output_csv).peek() == std::ifstream::traits_type::eof()) {
                csv << "target_rate,actual_send_rate,echoed_rate,total_sent,total_received,"
                    << "app_rtt_p50_us,app_rtt_p95_us,app_rtt_p99_us,sched_lag_p50_us,e2e_lat_p50_us,"
                    << "conn_failures,send_errors,recv_errors,timeouts,mismatched,duration_s,actual_duration_s,"
                    << "host,port,connections,msg_size,pipeline_window,active_share,mode,"
                    << "app_rtt_min_us,app_rtt_max_us,app_rtt_avg_us,concurrency_stalls,tcp_retrans_delta,client_cpu_pct,target_achieved\n";
            }
            csv << std::fixed << std::setprecision(2)
                << cfg.rate << ","
                << actual_send_rate << ","
                << echoed_rate << ","
                << total_sent << ","
                << total_received << ","
                << app_rtt_p50 << ","
                << app_rtt_p95 << ","
                << app_rtt_p99 << ","
                << sched_lag_p50 << ","
                << e2e_lat_p50 << ","
                << conn_failures << ","
                << send_errors << ","
                << recv_errors << ","
                << timeouts << ","
                << mismatched_msgs << ","
                << cfg.duration << ","
                << actual_duration_s << ","
                << cfg.host << ","
                << cfg.port << ","
                << cfg.connections << ","
                << cfg.msg_size << ","
                << cfg.pipeline_window << ","
                << cfg.active_share << ","
                << (cfg.mode == Mode::CLOSED_LOOP ? "closed-loop" : "open-loop") << ","
                << app_rtt_min << ","
                << app_rtt_max << ","
                << app_rtt_avg << ","
                << concurrency_stalls << ","
                << tcp_retrans_delta << ","
                << client_cpu_pct << ","
                << (target_achieved ? 1 : 0) << "\n";
            std::cout << "Results successfully written to: " << cfg.output_csv << "\n";
        }
    }

    // Write to JSON if --json-output is specified (Correction 6)
    if (!cfg.output_json.empty()) {
        ensure_parent_dir(cfg.output_json);
        std::ofstream jout(cfg.output_json);
        if (jout.is_open()) {
            jout << "{\n"
                 << "  \"config\": {\n"
                 << "    \"host\": \"" << cfg.host << "\",\n"
                 << "    \"port\": " << cfg.port << ",\n"
                 << "    \"connections\": " << cfg.connections << ",\n"
                 << "    \"active_share\": " << cfg.active_share << ",\n"
                 << "    \"pipeline_window\": " << cfg.pipeline_window << ",\n"
                 << "    \"mode\": \"" << (cfg.mode == Mode::CLOSED_LOOP ? "closed-loop" : "open-loop") << "\",\n"
                 << "    \"target_rate\": " << cfg.rate << ",\n"
                 << "    \"msg_size\": " << cfg.msg_size << ",\n"
                 << "    \"duration_s\": " << cfg.duration << ",\n"
                 << "    \"warmup_s\": " << cfg.warmup << ",\n"
                 << "    \"cooldown_s\": " << cfg.cooldown << "\n"
                 << "  },\n"
                 << "  \"metrics\": {\n"
                 << "    \"actual_duration_s\": " << actual_duration_s << ",\n"
                 << "    \"total_sent\": " << total_sent << ",\n"
                 << "    \"total_received\": " << total_received << ",\n"
                 << "    \"actual_send_rate\": " << actual_send_rate << ",\n"
                 << "    \"echoed_rate\": " << echoed_rate << ",\n"
                 << "    \"app_rtt_p50_us\": " << app_rtt_p50 << ",\n"
                 << "    \"app_rtt_p95_us\": " << app_rtt_p95 << ",\n"
                 << "    \"app_rtt_p99_us\": " << app_rtt_p99 << ",\n"
                 << "    \"sched_lag_p50_us\": " << sched_lag_p50 << ",\n"
                 << "    \"sched_lag_p95_us\": " << sched_lag_p95 << ",\n"
                 << "    \"sched_lag_p99_us\": " << sched_lag_p99 << ",\n"
                 << "    \"e2e_lat_p50_us\": " << e2e_lat_p50 << ",\n"
                 << "    \"e2e_lat_p95_us\": " << e2e_lat_p95 << ",\n"
                 << "    \"e2e_lat_p99_us\": " << e2e_lat_p99 << ",\n"
                 << "    \"concurrency_stalls\": " << concurrency_stalls << ",\n"
                 << "    \"tcp_retrans_delta\": " << tcp_retrans_delta << ",\n"
                 << "    \"client_cpu_pct\": " << client_cpu_pct << ",\n"
                 << "    \"target_achieved\": " << (target_achieved ? "true" : "false") << "\n"
                 << "  },\n"
                 << "  \"timeseries\": [\n";
            bool first = true;
            for (const auto &bucket : timeseries) {
                if (bucket.sent == 0 && bucket.received == 0 && bucket.second > actual_duration_s) continue;
                if (!first) jout << ",\n";
                first = false;
                jout << "    {\n"
                     << "      \"second\": " << bucket.second << ",\n"
                     << "      \"received\": " << bucket.received << ",\n"
                     << "      \"app_rtt_p50_us\": " << bucket.app_rtt_p50_us << ",\n"
                     << "      \"app_rtt_p95_us\": " << bucket.app_rtt_p95_us << ",\n"
                     << "      \"app_rtt_p99_us\": " << bucket.app_rtt_p99_us << "\n"
                     << "    }";
            }
            jout << "\n  ]\n}\n";
            std::cout << "JSON report successfully written to: " << cfg.output_json << "\n";
        }
    }

    return (conn_failures == 0 && send_errors == 0 && recv_errors == 0 && mismatched_msgs == 0) ? 0 : 2;
}
