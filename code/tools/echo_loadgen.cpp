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
#include <sstream>
#include <string>
#include <vector>

namespace {

// Magic identifier for echo frames ("ECHO" = 0x4543484F)
constexpr uint32_t ECHO_MAGIC = 0x4543484F;
constexpr size_t MAX_EPOLL_EVENTS = 1024;

#pragma pack(push, 1)
struct MessageHeader {
    uint32_t magic;         // ECHO_MAGIC
    uint32_t conn_id;       // Connection identifier (0 .. N-1)
    uint64_t seq_num;       // Monotonically increasing sequence number per connection
    uint64_t send_time_ns;  // CLOCK_MONOTONIC timestamp in nanoseconds
    uint32_t msg_size;      // Expected message frame size in bytes (e.g. 512)
    uint32_t checksum;      // Checksum of message payload
};
#pragma pack(pop)

static_assert(sizeof(MessageHeader) == 32, "MessageHeader must be exactly 32 bytes");

inline uint64_t get_time_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
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

struct Config {
    std::string host = "127.0.0.1";
    uint16_t port = 8080;
    uint32_t connections = 100;
    double rate = 1000.0;           // Target aggregate messages per second (0 = unthrottled)
    double duration = 30.0;         // Measurement duration in seconds
    size_t msg_size = 512;          // Application message size in bytes
    std::string output_csv = "";
    double warmup = 0.0;            // Warmup duration in seconds
    double cooldown = 0.0;          // Cooldown duration in seconds
    uint64_t timeout_ms = 5000;     // Request timeout in milliseconds
    double connect_timeout = 10.0;  // Initial connect timeout in seconds
    bool unthrottled = false;       // If true, send as fast as possible (closed-loop)
    double active_share = 1.0;      // Fraction of connections that actively send traffic (0.0 < share <= 1.0)
};

enum class ConnState {
    CONNECTING,
    IDLE,
    SENDING,
    WAITING_REPLY,
    CLOSED,
    FAILED
};

enum class BenchPhase {
    CONNECTING,
    WARMUP,
    MEASURING,
    COOLDOWN,
    FINISHED
};

struct Connection {
    uint32_t id = 0;
    int fd = -1;
    ConnState state = ConnState::CONNECTING;

    uint64_t next_send_seq = 1;
    uint64_t expected_recv_seq = 1;

    std::vector<uint8_t> send_buf;
    size_t send_bytes_written = 0;
    uint64_t current_send_time_ns = 0;

    std::vector<uint8_t> recv_buf;
    size_t recv_bytes_read = 0;

    uint64_t in_flight_start_ns = 0;
};

void print_usage(const char *prog) {
    std::cout << "Usage: " << prog << " [OPTIONS]\n"
              << "\nRequired / Standard Options:\n"
              << "  --host <ip|hostname>     Server address (default: 127.0.0.1)\n"
              << "  --port <port>            Server port (default: 8080)\n"
              << "  --connections <N>        Number of persistent TCP connections (default: 100)\n"
              << "  --rate <R>               Target aggregate message rate in msgs/sec (0 = unthrottled, default: 1000)\n"
              << "  --unthrottled            Send as fast as possible (maximum throughput mode)\n"
              << "  --active-share <float>   Fraction of connections active (0.0 < share <= 1.0, default: 1.0)\n"
              << "  --duration <sec>         Measurement duration in seconds (default: 30)\n"
              << "  --msg-size <bytes>       Message frame size in bytes (default: 512)\n"
              << "  --output <csv_path>      Output CSV file path\n"
              << "\nOptional Protocol & Timing Options:\n"
              << "  --warmup <sec>           Warmup duration in seconds before measurement (default: 0)\n"
              << "  --cooldown <sec>         Cooldown / drain duration in seconds (default: 0)\n"
              << "  --timeout <ms>           Response timeout in milliseconds (default: 5000)\n"
              << "  --connect-timeout <sec>  Timeout for establishing initial connections (default: 10)\n"
              << "  --help, -h               Show this help message\n";
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
            // boolean flag without value
            cfg.unthrottled = true;
            cfg.rate = 0.0;
            continue;
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
                cfg.unthrottled = true;
            }
        } else if (arg == "--unthrottled") {
            cfg.unthrottled = true;
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
    if (!cfg.unthrottled && cfg.rate <= 0.0) {
        std::cerr << "Error: --rate must be positive (or specify --unthrottled)\n";
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

void prepare_frame(Connection &conn, const Config &cfg, uint64_t now_ns) {
    conn.send_buf.resize(cfg.msg_size);
    MessageHeader *hdr = reinterpret_cast<MessageHeader *>(conn.send_buf.data());
    hdr->magic = ECHO_MAGIC;
    hdr->conn_id = conn.id;
    hdr->seq_num = conn.next_send_seq;
    hdr->send_time_ns = now_ns;
    hdr->msg_size = static_cast<uint32_t>(cfg.msg_size);

    uint8_t *payload = conn.send_buf.data() + sizeof(MessageHeader);
    size_t payload_len = cfg.msg_size - sizeof(MessageHeader);
    for (size_t i = 0; i < payload_len; ++i) {
        payload[i] = static_cast<uint8_t>((conn.id + conn.next_send_seq + i) & 0xFF);
    }
    hdr->checksum = compute_checksum(payload, payload_len);

    conn.send_bytes_written = 0;
    conn.current_send_time_ns = now_ns;
    conn.in_flight_start_ns = now_ns;
}

} // namespace

int main(int argc, char **argv) {
    Config cfg;
    if (!parse_args(argc, argv, cfg)) {
        print_usage(argv[0]);
        return 1;
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

    // Create timerfd for high-precision event pacing
    int timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timer_fd < 0) {
        perror("timerfd_create");
        close(epoll_fd);
        return 1;
    }

    // Configure timerfd pacing tick:
    // If rate is high, tick interval is 1/rate (or bounded by 100 microseconds).
    uint64_t timer_interval_ns = 1000000000ULL / static_cast<uint64_t>(std::max(1.0, cfg.rate));
    if (timer_interval_ns < 100000ULL) {
        timer_interval_ns = 100000ULL; // Minimum 100 us tick for epoll wakeup pacing
    }
    struct itimerspec its;
    its.it_interval.tv_sec = timer_interval_ns / 1000000000ULL;
    its.it_interval.tv_nsec = timer_interval_ns % 1000000000ULL;
    its.it_value = its.it_interval;
    if (timerfd_settime(timer_fd, 0, &its, nullptr) < 0) {
        perror("timerfd_settime");
        close(timer_fd);
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
    std::deque<uint32_t> idle_conns;

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
              << cfg.host << ":" << cfg.port << "...\n";

    uint64_t conn_start_ns = get_time_ns();
    uint64_t conn_timeout_ns = static_cast<uint64_t>(cfg.connect_timeout * 1e9);
    uint32_t connected_count = 0;
    uint64_t conn_failures = 0;

    for (uint32_t i = 0; i < cfg.connections; ++i) {
        conns[i].id = i;
        conns[i].send_buf.resize(cfg.msg_size);
        conns[i].recv_buf.resize(cfg.msg_size);
        conns[i].recv_bytes_read = 0;
        conns[i].send_bytes_written = 0;

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
            conns[i].state = ConnState::IDLE;
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
                if (read(timer_fd, &ticks, sizeof(ticks)) < 0) {
                    // Ignore EINTR/EAGAIN
                }
                continue;
            }
            if (id >= cfg.connections) continue;
            Connection &conn = conns[id];
            if (conn.state != ConnState::CONNECTING) continue;

            int so_error = 0;
            socklen_t len = sizeof(so_error);
            if (getsockopt(conn.fd, SOL_SOCKET, SO_ERROR, &so_error, &len) == 0 && so_error == 0) {
                conn.state = ConnState::IDLE;
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

    std::vector<uint32_t> rtt_samples_us;
    size_t sample_reserve = cfg.unthrottled ? 2000000 : (static_cast<size_t>(cfg.rate * cfg.duration * 1.5) + 10000);
    rtt_samples_us.reserve(sample_reserve);

    uint64_t measurement_actual_start_ns = 0;
    uint64_t measurement_actual_end_ns = 0;

    auto send_on_conn = [&](uint32_t cid, uint64_t now_ns) {
        Connection &conn = conns[cid];
        prepare_frame(conn, cfg, now_ns);
        struct epoll_event ev_mod;
        ev_mod.data.u32 = cid;
        ssize_t n = send(conn.fd, conn.send_buf.data(), cfg.msg_size, MSG_NOSIGNAL);
        if (n == static_cast<ssize_t>(cfg.msg_size)) {
            conn.send_bytes_written = cfg.msg_size;
            conn.state = ConnState::WAITING_REPLY;
            if (phase == BenchPhase::MEASURING) total_sent++;
            ev_mod.events = EPOLLIN | EPOLLERR | EPOLLHUP;
            epoll_ctl(epoll_fd, EPOLL_CTL_MOD, conn.fd, &ev_mod);
        } else if (n > 0) {
            conn.send_bytes_written = static_cast<size_t>(n);
            conn.state = ConnState::SENDING;
            ev_mod.events = EPOLLOUT | EPOLLIN | EPOLLERR | EPOLLHUP;
            epoll_ctl(epoll_fd, EPOLL_CTL_MOD, conn.fd, &ev_mod);
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            conn.send_bytes_written = 0;
            conn.state = ConnState::SENDING;
            ev_mod.events = EPOLLOUT | EPOLLIN | EPOLLERR | EPOLLHUP;
            epoll_ctl(epoll_fd, EPOLL_CTL_MOD, conn.fd, &ev_mod);
        } else {
            if (phase == BenchPhase::MEASURING) send_errors++;
            conn.state = ConnState::CLOSED;
            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, conn.fd, nullptr);
            close(conn.fd);
            conn.fd = -1;
        }
    };

    auto start_unthrottled_burst = [&]() {
        uint64_t now_ns = get_time_ns();
        std::vector<uint32_t> ready_ids(idle_conns.begin(), idle_conns.end());
        idle_conns.clear();
        for (uint32_t cid : ready_ids) {
            send_on_conn(cid, now_ns);
        }
    };

    if (phase == BenchPhase::WARMUP) {
        std::cout << "Starting " << cfg.warmup << "s warmup phase "
                  << (cfg.unthrottled ? "(unthrottled / max rate)...\n" : ("at target rate " + std::to_string(static_cast<int>(cfg.rate)) + " msgs/sec...\n"));
        if (cfg.unthrottled) {
            start_unthrottled_burst();
        }
    } else {
        std::cout << "Starting " << cfg.duration << "s measurement phase "
                  << (cfg.unthrottled ? "(unthrottled / max rate)...\n" : ("at target rate " + std::to_string(static_cast<int>(cfg.rate)) + " msgs/sec...\n"));
        measurement_actual_start_ns = phase_start_ns;
        if (cfg.unthrottled) {
            start_unthrottled_burst();
        }
    }

    uint64_t timeout_ns = cfg.timeout_ms * 1000000ULL;

    while (phase != BenchPhase::FINISHED && !g_stop_requested) {
        int nfds = epoll_wait(epoll_fd, events, MAX_EPOLL_EVENTS, 20);
        uint64_t now_ns = get_time_ns();

        // 1. Process epoll events
        for (int e = 0; e < nfds; ++e) {
            uint32_t id = events[e].data.u32;
            if (id == 0xFFFFFFFF) {
                uint64_t ticks = 0;
                if (read(timer_fd, &ticks, sizeof(ticks)) < 0) {
                    // Ignore EINTR/EAGAIN
                }
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

            // Handle Writable (partial send completion)
            if ((ev_mask & EPOLLOUT) && conn.state == ConnState::SENDING) {
                size_t remaining = cfg.msg_size - conn.send_bytes_written;
                ssize_t n = send(conn.fd, conn.send_buf.data() + conn.send_bytes_written, remaining, MSG_NOSIGNAL);
                if (n > 0) {
                    conn.send_bytes_written += n;
                    if (conn.send_bytes_written == cfg.msg_size) {
                        conn.state = ConnState::WAITING_REPLY;
                        if (phase == BenchPhase::MEASURING) total_sent++;
                        ev.events = EPOLLIN | EPOLLERR | EPOLLHUP;
                        ev.data.u32 = id;
                        epoll_ctl(epoll_fd, EPOLL_CTL_MOD, conn.fd, &ev);
                    }
                } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    // Still blocked
                } else {
                    if (phase == BenchPhase::MEASURING) send_errors++;
                    conn.state = ConnState::CLOSED;
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, conn.fd, nullptr);
                    close(conn.fd);
                    conn.fd = -1;
                }
            }

            // Handle Readable (incoming echoed frame)
            if ((ev_mask & EPOLLIN) && (conn.state == ConnState::WAITING_REPLY || conn.state == ConnState::IDLE)) {
                while (true) {
                    size_t remaining = cfg.msg_size - conn.recv_bytes_read;
                    ssize_t n = recv(conn.fd, conn.recv_buf.data() + conn.recv_bytes_read, remaining, 0);
                    if (n > 0) {
                        conn.recv_bytes_read += n;
                        if (conn.recv_bytes_read == cfg.msg_size) {
                            // Frame complete! Verify framing and integrity.
                            const MessageHeader *hdr = reinterpret_cast<const MessageHeader *>(conn.recv_buf.data());
                            bool valid = (hdr->magic == ECHO_MAGIC) &&
                                         (hdr->conn_id == conn.id) &&
                                         (hdr->seq_num == conn.expected_recv_seq) &&
                                         (hdr->msg_size == cfg.msg_size);

                            if (valid) {
                                const uint8_t *payload = conn.recv_buf.data() + sizeof(MessageHeader);
                                size_t payload_len = cfg.msg_size - sizeof(MessageHeader);
                                if (hdr->checksum != compute_checksum(payload, payload_len)) {
                                    valid = false;
                                }
                            }

                            if (valid) {
                                uint64_t recv_time_ns = get_time_ns();
                                uint64_t rtt_ns = (recv_time_ns >= hdr->send_time_ns) ? (recv_time_ns - hdr->send_time_ns) : 0;
                                if (phase == BenchPhase::MEASURING) {
                                    total_received++;
                                    rtt_samples_us.push_back(static_cast<uint32_t>(rtt_ns / 1000));
                                }
                                conn.expected_recv_seq++;
                                conn.next_send_seq++;
                                conn.recv_bytes_read = 0;
                                if (cfg.unthrottled && (phase == BenchPhase::WARMUP || phase == BenchPhase::MEASURING)) {
                                    send_on_conn(id, get_time_ns());
                                } else {
                                    conn.state = ConnState::IDLE;
                                    idle_conns.push_back(id);
                                }
                            } else {
                                if (phase == BenchPhase::MEASURING) mismatched_msgs++;
                                conn.recv_bytes_read = 0;
                                conn.expected_recv_seq++;
                                conn.next_send_seq++;
                                if (cfg.unthrottled && (phase == BenchPhase::WARMUP || phase == BenchPhase::MEASURING)) {
                                    send_on_conn(id, get_time_ns());
                                } else {
                                    conn.state = ConnState::IDLE;
                                    idle_conns.push_back(id);
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

        // 2. Timeout Checking for in-flight requests
        for (uint32_t i = 0; i < cfg.connections; ++i) {
            Connection &conn = conns[i];
            if ((conn.state == ConnState::WAITING_REPLY || conn.state == ConnState::SENDING) &&
                conn.in_flight_start_ns > 0 &&
                (now_ns - conn.in_flight_start_ns > timeout_ns)) {
                if (phase == BenchPhase::MEASURING) timeouts++;
                // Reset connection state after timeout to unfreeze
                conn.state = ConnState::IDLE;
                conn.recv_bytes_read = 0;
                conn.send_bytes_written = 0;
                conn.in_flight_start_ns = 0;
                conn.next_send_seq++;
                conn.expected_recv_seq = conn.next_send_seq;
                if (is_active[i]) {
                    if (cfg.unthrottled && (phase == BenchPhase::WARMUP || phase == BenchPhase::MEASURING)) {
                        send_on_conn(i, get_time_ns());
                    } else {
                        idle_conns.push_back(i);
                    }
                }
            }
        }

        // 3. Phase Transitions
        if (phase == BenchPhase::WARMUP) {
            if (now_ns - phase_start_ns >= warmup_duration_ns) {
                std::cout << "Warmup complete. Starting " << cfg.duration
                          << "s measurement phase " << (cfg.unthrottled ? "(unthrottled / max rate)...\n" : ("at target rate " + std::to_string(static_cast<int>(cfg.rate)) + " msgs/sec...\n"));
                phase = BenchPhase::MEASURING;
                phase_start_ns = now_ns;
                measurement_actual_start_ns = now_ns;
                messages_scheduled = 0;
                if (cfg.unthrottled) {
                    start_unthrottled_burst();
                }
            }
        } else if (phase == BenchPhase::MEASURING) {
            if (now_ns - phase_start_ns >= measure_duration_ns) {
                measurement_actual_end_ns = now_ns;
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

        // 4. Open-Loop Rate Generation / Scheduling (used when not unthrottled)
        if (!cfg.unthrottled && (phase == BenchPhase::WARMUP || phase == BenchPhase::MEASURING)) {
            uint64_t elapsed_in_phase_ns = now_ns - phase_start_ns;
            uint64_t target_messages = static_cast<uint64_t>((elapsed_in_phase_ns * cfg.rate) / 1e9);

            while (messages_scheduled < target_messages) {
                if (idle_conns.empty()) {
                    // All connections are currently waiting for an echoed reply.
                    // This is a concurrency bottleneck under 1-outstanding-request model!
                    if (phase == BenchPhase::MEASURING) concurrency_stalls++;
                    messages_scheduled++; // Advance schedule to track target offered load accurately
                    continue;
                }

                uint32_t cid = idle_conns.front();
                idle_conns.pop_front();
                messages_scheduled++;

                send_on_conn(cid, now_ns);
            }
        }
    }

    if (measurement_actual_end_ns == 0) {
        measurement_actual_end_ns = get_time_ns();
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

    uint32_t rtt_min = 0, rtt_max = 0, rtt_p50 = 0, rtt_p95 = 0, rtt_p99 = 0;
    double rtt_avg = 0.0;

    if (!rtt_samples_us.empty()) {
        std::sort(rtt_samples_us.begin(), rtt_samples_us.end());
        size_t n = rtt_samples_us.size();
        rtt_min = rtt_samples_us.front();
        rtt_max = rtt_samples_us.back();
        rtt_p50 = rtt_samples_us[static_cast<size_t>(n * 0.50)];
        rtt_p95 = rtt_samples_us[static_cast<size_t>(n * 0.95)];
        rtt_p99 = rtt_samples_us[static_cast<size_t>(n * 0.99)];
        double sum = std::accumulate(rtt_samples_us.begin(), rtt_samples_us.end(), 0.0);
        rtt_avg = sum / n;
    }

    bool target_achieved = cfg.unthrottled ? true : (total_sent >= static_cast<uint64_t>(cfg.rate * actual_duration_s * 0.95));

    // Print formatted summary table to stdout
    std::cout << "\n======================================================================\n"
              << "               TCP ECHO LOAD GENERATOR SUMMARY\n"
              << "======================================================================\n"
              << std::fixed << std::setprecision(2)
              << "Target Configuration:\n"
              << "  Server:                  " << cfg.host << ":" << cfg.port << "\n"
              << "  Concurrent Connections:  " << cfg.connections << " (" << active_count << " active, "
              << (cfg.connections - active_count) << " idle)\n"
              << "  Active Share:            " << (cfg.active_share * 100.0) << " %\n"
              << "  Target Offered Rate:     " << (cfg.unthrottled ? "UNTHROTTLED (max speed)" : (std::to_string(cfg.rate) + " msgs/sec")) << "\n"
              << "  Message Size:            " << cfg.msg_size << " bytes\n"
              << "  Configured Duration:     " << cfg.duration << " s"
              << " (Warmup: " << cfg.warmup << " s, Cooldown: " << cfg.cooldown << " s)\n\n"
              << "Execution & Throughput:\n"
              << "  Actual Duration:         " << actual_duration_s << " s\n"
              << "  Messages Sent:           " << total_sent << " (" << actual_send_rate << " msgs/sec)\n"
              << "  Messages Echoed:         " << total_received << " (" << echoed_rate << " msgs/sec)\n"
              << "  Target Offered Met:      " << (target_achieved ? "YES" : "NO (Bottlenecked by concurrency/RTT)") << "\n\n"
              << "Latency Distribution (RTT):\n"
              << "  p50 (Median):            " << rtt_p50 << " us (" << (rtt_p50 / 1000.0) << " ms)\n"
              << "  p95:                     " << rtt_p95 << " us (" << (rtt_p95 / 1000.0) << " ms)\n"
              << "  p99:                     " << rtt_p99 << " us (" << (rtt_p99 / 1000.0) << " ms)\n"
              << "  Min / Avg / Max:         " << rtt_min << " / " << rtt_avg << " / " << rtt_max << " us\n\n"
              << "Reliability & Error Counters:\n"
              << "  Connection Failures:     " << conn_failures << "\n"
              << "  Send / Recv Errors:      " << send_errors << " / " << recv_errors << "\n"
              << "  Timeouts:                " << timeouts << "\n"
              << "  Mismatched / Corrupted:  " << mismatched_msgs << "\n"
              << "  Concurrency Stalls:      " << concurrency_stalls << "\n"
              << "======================================================================\n";

    if (!target_achieved && !cfg.unthrottled) {
        std::cout << "\n[NOTICE] Throughput limitation reached:\n"
                  << "  The generator was unable to sustain " << cfg.rate << " msgs/sec with "
                  << cfg.connections << " connections (Achieved: " << actual_send_rate << " msgs/sec).\n"
                  << "  Under the 1-outstanding-request model, Little's Law limits max rate to:\n"
                  << "    R_max = Connections / RTT_avg = " << cfg.connections << " / ("
                  << rtt_avg << " us) = " << static_cast<uint64_t>(cfg.connections / (rtt_avg / 1e6 + 1e-9)) << " msgs/sec.\n";
    }

    // Write to CSV if --output is specified
    if (!cfg.output_csv.empty()) {
        ensure_parent_dir(cfg.output_csv);
        bool file_exists = (access(cfg.output_csv.c_str(), F_OK) == 0);
        std::ofstream csv(cfg.output_csv, std::ios::app);
        if (csv.is_open()) {
            if (!file_exists || std::ifstream(cfg.output_csv).peek() == std::ifstream::traits_type::eof()) {
                csv << "target_rate,actual_send_rate,echoed_rate,total_sent,total_received,"
                    << "rtt_p50_us,rtt_p95_us,rtt_p99_us,conn_failures,send_errors,recv_errors,"
                    << "timeouts,mismatched,duration_s,actual_duration_s,host,port,connections,"
                    << "msg_size,rtt_min_us,rtt_max_us,rtt_avg_us,concurrency_stalls,target_achieved\n";
            }
            csv << std::fixed << std::setprecision(2)
                << cfg.rate << ","
                << actual_send_rate << ","
                << echoed_rate << ","
                << total_sent << ","
                << total_received << ","
                << rtt_p50 << ","
                << rtt_p95 << ","
                << rtt_p99 << ","
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
                << rtt_min << ","
                << rtt_max << ","
                << rtt_avg << ","
                << concurrency_stalls << ","
                << (target_achieved ? 1 : 0) << "\n";
            std::cout << "Results successfully written to: " << cfg.output_csv << "\n";
        } else {
            std::cerr << "Warning: Could not open output CSV file: " << cfg.output_csv << "\n";
        }
    }

    return (conn_failures == 0 && send_errors == 0 && recv_errors == 0 && mismatched_msgs == 0) ? 0 : 2;
}
