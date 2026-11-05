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
#include <sys/time.h>
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
#include <thread>
#include <vector>

namespace {

// Magic identifier for echo frames ("ECHO" = 0x4543484F)
constexpr uint32_t ECHO_MAGIC = 0x4543484F;
constexpr size_t MAX_EPOLL_EVENTS = 1024;
constexpr uint32_t TIMER_FD_TAG = 0xFFFFFFFFU;

#pragma pack(push, 1)
struct MessageHeader {
    uint32_t magic;         // ECHO_MAGIC (0x4543484F)
    uint32_t conn_id;       // Connection identifier (0 .. N-1)
    uint64_t seq_num;       // Monotonically increasing sequence number per connection
    uint64_t send_time_ns;  // CLOCK_MONOTONIC timestamp when TX started
    uint32_t msg_size;      // Total message frame size in bytes (default: 512)
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

enum class WorkloadMode {
    OPEN_LOOP,
    CLOSED_LOOP
};

enum class ArrivalProcess {
    CONSTANT,
    POISSON,
    BURST
};

enum class ConnSelectPolicy {
    ROUND_ROBIN,
    RANDOM,
    FIFO_IDLE
};

struct Config {
    std::string host = "127.0.0.1";
    uint16_t port = 8080;
    uint32_t connections = 100;
    double active_share = 1.0;          // Fraction of connections active (0.0 < share <= 1.0)
    uint32_t active_connections = 0;    // If > 0, overrides active_share with exact K active connections

    WorkloadMode mode = WorkloadMode::OPEN_LOOP;
    ArrivalProcess arrival = ArrivalProcess::CONSTANT;
    std::string dist_label = "constant";
    ConnSelectPolicy conn_select = ConnSelectPolicy::ROUND_ROBIN;

    double rate = 1000.0;               // Target aggregate messages/sec in open-loop mode
    double burst_mult = 4.0;            // Multiplier if burst_on_rate is not explicitly set
    double burst_on_rate = -1.0;        // Explicit ON rate (msgs/s); < 0 means derive from rate & duty cycle
    double burst_off_rate = 0.0;        // Explicit OFF rate (msgs/s); default 0 (pure ON/OFF burst)
    double burst_on_ms = 50.0;          // Duration of ON spike in ms
    double burst_period_ms = 200.0;     // Total period (ON + OFF) in ms

    uint32_t pipeline_depth = 1;        // Max unacknowledged in-flight frames per connection (W)
    size_t max_pending = 65536;         // Max total queued arrivals before dropping
    size_t msg_size = 512;              // Total frame size in bytes (32B header + payload)

    double warmup = 5.0;                // Warmup duration in seconds
    double duration = 20.0;             // Measurement duration in seconds
    double cooldown = 2.0;              // Drain allowance in seconds after measurement ends
    uint64_t timeout_ms = 5000;         // Per-request response timeout in milliseconds
    double connect_timeout = 10.0;      // Initial connection barrier timeout in seconds

    uint64_t seed = 42;                 // Deterministic base seed
    std::string output_csv = "";        // Optional CSV output path
    std::string json_output = "";       // Optional JSON output path
    std::string tag = "";               // Optional experiment label/backend name
    bool self_test = false;             // Run built-in loopback verification suite
};

enum class ConnState {
    CONNECTING,
    ACTIVE,
    CLOSED,
    FAILED
};

enum class BenchPhase {
    WARMUP,
    MEASURING,
    COOLDOWN,
    FINISHED
};

// Represents a scheduled request waiting in a connection's transmit queue or currently being written
struct PendingReq {
    uint64_t seq_num = 0;
    uint64_t sched_time_ns = 0;
    uint64_t tx_start_ns = 0;
    bool in_measurement_cohort = false;
};

// Represents a request whose full 512B frame has been accepted by send() and is awaiting its echo
struct InFlightReq {
    uint64_t seq_num = 0;
    uint64_t sched_time_ns = 0;
    uint64_t tx_start_ns = 0;
    uint64_t send_time_ns = 0;  // Moment the LAST byte was accepted by local send()
    bool in_measurement_cohort = false;
};

struct Connection {
    uint32_t id = 0;
    int fd = -1;
    ConnState state = ConnState::CONNECTING;
    bool is_active = false;
    bool in_fifo_idle_queue = false;
    uint32_t epoll_events = 0;

    uint64_t next_assign_seq = 1;
    uint64_t expected_recv_seq = 1;

    // Strict FIFO queue of requests assigned to this connection awaiting transmission
    std::deque<PendingReq> tx_queue;

    // Single currently transmitting request (never overwritten mid-frame)
    bool has_active_tx = false;
    PendingReq active_tx;
    std::vector<uint8_t> send_buf;
    size_t send_bytes_written = 0;

    // Requests completely sent on the wire awaiting echo response (size <= pipeline_depth)
    std::deque<InFlightReq> in_flight;

    // Persistent receive buffer for handling arbitrary TCP segmentation & coalescing
    std::vector<uint8_t> recv_buf;
    size_t recv_bytes_buffered = 0;

    // Per-connection measurement counters
    uint64_t meas_assigned = 0;
    uint64_t meas_sent = 0;
    uint64_t meas_completed = 0;
    uint64_t meas_timeouts = 0;
    uint64_t rtt_sum_us = 0;
};

struct Percentiles {
    uint32_t min = 0;
    uint32_t p50 = 0;
    uint32_t p95 = 0;
    uint32_t p99 = 0;
    uint32_t max = 0;
    double avg = 0.0;
};

Percentiles compute_percentiles(std::vector<uint32_t> &samples) {
    Percentiles p;
    if (samples.empty()) return p;
    std::sort(samples.begin(), samples.end());
    size_t n = samples.size();
    p.min = samples.front();
    p.max = samples.back();
    p.p50 = samples[static_cast<size_t>((n - 1) * 0.50)];
    p.p95 = samples[static_cast<size_t>((n - 1) * 0.95)];
    p.p99 = samples[static_cast<size_t>((n - 1) * 0.99)];
    double sum = std::accumulate(samples.begin(), samples.end(), 0.0);
    p.avg = sum / static_cast<double>(n);
    return p;
}

uint64_t query_total_tcp_retrans(const std::vector<Connection> &conns) {
    uint64_t total_retrans = 0;
#ifdef TCP_INFO
    for (const auto &c : conns) {
        if (c.fd >= 0 && c.state == ConnState::ACTIVE) {
            struct tcp_info tcpi;
            socklen_t len = sizeof(tcpi);
            std::memset(&tcpi, 0, sizeof(tcpi));
            if (getsockopt(c.fd, IPPROTO_TCP, TCP_INFO, &tcpi, &len) == 0) {
                total_retrans += tcpi.tcpi_total_retrans;
            }
        }
    }
#else
    (void)conns;
#endif
    return total_retrans;
}

double rusage_cpu_seconds(const struct rusage &ru) {
    double user_s = static_cast<double>(ru.ru_utime.tv_sec) + static_cast<double>(ru.ru_utime.tv_usec) / 1e6;
    double sys_s = static_cast<double>(ru.ru_stime.tv_sec) + static_cast<double>(ru.ru_stime.tv_usec) / 1e6;
    return user_s + sys_s;
}

void print_usage(const char *prog) {
    std::cout
        << "Usage: " << prog << " [OPTIONS]\n\n"
        << "Connection & Topology Options:\n"
        << "  --host <ip|hostname>         Server address (default: 127.0.0.1)\n"
        << "  --port <port>                Server port (default: 8080)\n"
        << "  --connections <N>            Total persistent TCP connections (default: 100)\n"
        << "  --active-share <0..1>        Fraction of connections active (default: 1.0)\n"
        << "  --active-connections <K>     Exact number of active connections (overrides --active-share)\n"
        << "  --conn-select <policy>       Connection assignment: round-robin | random | fifo (default: round-robin)\n"
        << "  --pipeline-window <W>        Max in-flight requests per connection (default: 1; alias: --pipeline-depth)\n"
        << "  --max-pending <M>            Max total queued requests before drop (default: 65536)\n\n"
        << "Workload & Arrival Options:\n"
        << "  --mode <open-loop|closed-loop> Workload mode (default: open-loop)\n"
        << "  --unthrottled                Alias for --mode closed-loop\n"
        << "  --dist <type>                Arrival distribution: constant | poisson | burst | burst2x | burst4x | burst10x\n"
        << "                               (alias: --arrival)\n"
        << "  --rate <msgs/sec>            Target aggregate offered rate (default: 1000)\n"
        << "  --burst-on-ms <ms>           ON spike duration in ms for burst mode (default: 50)\n"
        << "  --burst-period-ms <ms>       Total cycle period in ms for burst mode (default: 200)\n"
        << "  --burst-on-rate <msgs/sec>   Explicit ON rate during burst (default: auto-computed from --rate)\n"
        << "  --burst-off-rate <msgs/sec>  Explicit OFF rate outside burst (default: 0)\n"
        << "  --burst-mult <M>             Burst multiplier if --burst-on-rate omitted (default: 4.0)\n"
        << "  --msg-size <bytes>           Frame size in bytes including 32B header (default: 512)\n"
        << "  --seed <uint64>              Deterministic RNG seed (default: 42)\n\n"
        << "Timing & Output Options:\n"
        << "  --warmup <sec>               Warmup duration in seconds (default: 5)\n"
        << "  --duration <sec>             Measurement duration in seconds (default: 20)\n"
        << "  --cooldown <sec>             Drain allowance in seconds after measurement (default: 2)\n"
        << "  --timeout <ms>               Request timeout in milliseconds (default: 5000)\n"
        << "  --connect-timeout <sec>      Initial connection barrier timeout in seconds (default: 10)\n"
        << "  --output <csv_path>          Append summary row to CSV file\n"
        << "  --json-output <json_path>    Write detailed JSON report (with 1s time series)\n"
        << "  --tag <label>                Optional backend/run label for CSV/JSON output\n"
        << "  --self-test                  Run built-in loopback verification suite and exit\n"
        << "  --help, -h                   Show this help message\n";
}

bool parse_args(int argc, char **argv, Config &cfg) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        std::string val;
        auto eq = arg.find('=');
        if (eq != std::string::npos) {
            val = arg.substr(eq + 1);
            arg = arg.substr(0, eq);
        } else if (arg == "--unthrottled" || arg == "--self-test" || arg == "--help" || arg == "-h") {
            // Flag without value
        } else if (i + 1 < argc) {
            val = argv[++i];
        }

        try {
            if (arg == "--host") {
                if (val.empty()) return false;
                cfg.host = val;
            } else if (arg == "--port") {
                if (val.empty()) return false;
                int p = std::stoi(val);
                if (p <= 0 || p > 65535) return false;
                cfg.port = static_cast<uint16_t>(p);
            } else if (arg == "--connections") {
                if (val.empty()) return false;
                long c = std::stol(val);
                if (c <= 0) return false;
                cfg.connections = static_cast<uint32_t>(c);
            } else if (arg == "--active-share") {
                if (val.empty()) return false;
                cfg.active_share = std::stod(val);
            } else if (arg == "--active-connections") {
                if (val.empty()) return false;
                long k = std::stol(val);
                if (k < 0) return false;
                cfg.active_connections = static_cast<uint32_t>(k);
            } else if (arg == "--mode") {
                if (val == "open-loop" || val == "open") {
                    cfg.mode = WorkloadMode::OPEN_LOOP;
                } else if (val == "closed-loop" || val == "closed" || val == "unthrottled") {
                    cfg.mode = WorkloadMode::CLOSED_LOOP;
                } else {
                    std::cerr << "Error: Invalid --mode '" << val << "'\n";
                    return false;
                }
            } else if (arg == "--unthrottled") {
                cfg.mode = WorkloadMode::CLOSED_LOOP;
            } else if (arg == "--arrival" || arg == "--dist") {
                cfg.dist_label = val;
                if (val == "constant") {
                    cfg.arrival = ArrivalProcess::CONSTANT;
                } else if (val == "poisson") {
                    cfg.arrival = ArrivalProcess::POISSON;
                } else if (val == "burst" || val == "burst4x") {
                    cfg.arrival = ArrivalProcess::BURST;
                    cfg.burst_on_ms = 50.0;
                    cfg.burst_period_ms = 200.0;
                    cfg.burst_mult = 4.0;
                } else if (val == "burst2x") {
                    cfg.arrival = ArrivalProcess::BURST;
                    cfg.burst_on_ms = 100.0;
                    cfg.burst_period_ms = 200.0;
                    cfg.burst_mult = 2.0;
                } else if (val == "burst10x") {
                    cfg.arrival = ArrivalProcess::BURST;
                    cfg.burst_on_ms = 20.0;
                    cfg.burst_period_ms = 200.0;
                    cfg.burst_mult = 10.0;
                } else {
                    std::cerr << "Error: Invalid --dist/--arrival '" << val << "'\n";
                    return false;
                }
            } else if (arg == "--conn-select") {
                if (val == "round-robin" || val == "rr") {
                    cfg.conn_select = ConnSelectPolicy::ROUND_ROBIN;
                } else if (val == "random" || val == "rand") {
                    cfg.conn_select = ConnSelectPolicy::RANDOM;
                } else if (val == "fifo" || val == "fifo-idle") {
                    cfg.conn_select = ConnSelectPolicy::FIFO_IDLE;
                } else {
                    std::cerr << "Error: Invalid --conn-select '" << val << "'\n";
                    return false;
                }
            } else if (arg == "--rate") {
                if (val.empty()) return false;
                cfg.rate = std::stod(val);
                if (cfg.rate == 0.0) {
                    cfg.mode = WorkloadMode::CLOSED_LOOP;
                }
            } else if (arg == "--burst-mult") {
                if (val.empty()) return false;
                cfg.burst_mult = std::stod(val);
            } else if (arg == "--burst-on-rate") {
                if (val.empty()) return false;
                cfg.burst_on_rate = std::stod(val);
            } else if (arg == "--burst-off-rate") {
                if (val.empty()) return false;
                cfg.burst_off_rate = std::stod(val);
            } else if (arg == "--burst-on-ms") {
                if (val.empty()) return false;
                cfg.burst_on_ms = std::stod(val);
            } else if (arg == "--burst-period-ms") {
                if (val.empty()) return false;
                cfg.burst_period_ms = std::stod(val);
            } else if (arg == "--pipeline-depth" || arg == "--pipeline" || arg == "--pipeline-window") {
                if (val.empty()) return false;
                long w = std::stol(val);
                if (w <= 0 || w > 1024) {
                    std::cerr << "Error: --pipeline-window must be in [1, 1024]\n";
                    return false;
                }
                cfg.pipeline_depth = static_cast<uint32_t>(w);
            } else if (arg == "--max-pending") {
                if (val.empty()) return false;
                long mp = std::stol(val);
                if (mp <= 0) return false;
                cfg.max_pending = static_cast<size_t>(mp);
            } else if (arg == "--duration") {
                if (val.empty()) return false;
                cfg.duration = std::stod(val);
            } else if (arg == "--warmup") {
                if (val.empty()) return false;
                cfg.warmup = std::stod(val);
            } else if (arg == "--cooldown") {
                if (val.empty()) return false;
                cfg.cooldown = std::stod(val);
            } else if (arg == "--timeout") {
                if (val.empty()) return false;
                long t = std::stol(val);
                if (t <= 0) return false;
                cfg.timeout_ms = static_cast<uint64_t>(t);
            } else if (arg == "--connect-timeout") {
                if (val.empty()) return false;
                cfg.connect_timeout = std::stod(val);
            } else if (arg == "--msg-size") {
                if (val.empty()) return false;
                long ms = std::stol(val);
                if (ms < static_cast<long>(sizeof(MessageHeader))) {
                    std::cerr << "Error: --msg-size must be at least " << sizeof(MessageHeader) << " bytes\n";
                    return false;
                }
                cfg.msg_size = static_cast<size_t>(ms);
            } else if (arg == "--seed") {
                if (val.empty()) return false;
                cfg.seed = std::stoull(val);
            } else if (arg == "--output") {
                if (val.empty()) return false;
                cfg.output_csv = val;
            } else if (arg == "--json-output") {
                if (val.empty()) return false;
                cfg.json_output = val;
            } else if (arg == "--tag") {
                if (val.empty()) return false;
                cfg.tag = val;
            } else if (arg == "--self-test") {
                cfg.self_test = true;
            } else if (arg == "--help" || arg == "-h") {
                print_usage(argv[0]);
                std::exit(0);
            } else {
                std::cerr << "Error: Unknown argument: " << arg << "\n";
                return false;
            }
        } catch (const std::exception &ex) {
            std::cerr << "Error parsing argument " << arg << ": " << ex.what() << "\n";
            return false;
        }
    }

    if (cfg.self_test) return true;

    if (cfg.connections == 0) {
        std::cerr << "Error: --connections must be positive\n";
        return false;
    }
    if (cfg.active_connections > cfg.connections) {
        std::cerr << "Error: --active-connections (" << cfg.active_connections
                  << ") cannot exceed --connections (" << cfg.connections << ")\n";
        return false;
    }
    if (cfg.active_connections == 0 && (cfg.active_share <= 0.0 || cfg.active_share > 1.0)) {
        std::cerr << "Error: --active-share must be in range (0.0, 1.0]\n";
        return false;
    }
    if (cfg.mode == WorkloadMode::OPEN_LOOP && cfg.rate <= 0.0) {
        std::cerr << "Error: --rate must be positive in open-loop mode\n";
        return false;
    }
    if (cfg.duration <= 0.0 || cfg.warmup < 0.0 || cfg.cooldown < 0.0) {
        std::cerr << "Error: invalid phase duration(s)\n";
        return false;
    }
    if (cfg.arrival == ArrivalProcess::BURST) {
        if (cfg.burst_on_ms <= 0.0 || cfg.burst_period_ms <= cfg.burst_on_ms) {
            std::cerr << "Error: --burst-on-ms must be > 0 and < --burst-period-ms\n";
            return false;
        }
        if (cfg.burst_off_rate < 0.0) {
            std::cerr << "Error: --burst-off-rate must be >= 0\n";
            return false;
        }
        if (cfg.burst_on_rate < 0.0) {
            double duty = cfg.burst_on_ms / cfg.burst_period_ms;
            double off_frac = 1.0 - duty;
            double required_on = (cfg.rate - cfg.burst_off_rate * off_frac) / duty;
            if (required_on <= 0.0) {
                std::cerr << "Error: invalid burst parameters produce non-positive ON rate\n";
                return false;
            }
            cfg.burst_on_rate = required_on;
        }
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

void build_wire_frame(Connection &conn, const PendingReq &req, size_t msg_size) {
    conn.send_buf.resize(msg_size);
    MessageHeader *hdr = reinterpret_cast<MessageHeader *>(conn.send_buf.data());
    hdr->magic = ECHO_MAGIC;
    hdr->conn_id = conn.id;
    hdr->seq_num = req.seq_num;
    hdr->send_time_ns = req.tx_start_ns;
    hdr->msg_size = static_cast<uint32_t>(msg_size);

    uint8_t *payload = conn.send_buf.data() + sizeof(MessageHeader);
    size_t payload_len = msg_size - sizeof(MessageHeader);
    for (size_t i = 0; i < payload_len; ++i) {
        payload[i] = static_cast<uint8_t>((conn.id + req.seq_num + i) & 0xFF);
    }
    hdr->checksum = compute_checksum(payload, payload_len);
    conn.send_bytes_written = 0;
}

const char *mode_to_str(WorkloadMode m) {
    return (m == WorkloadMode::CLOSED_LOOP) ? "closed-loop" : "open-loop";
}

const char *policy_to_str(ConnSelectPolicy p) {
    switch (p) {
        case ConnSelectPolicy::ROUND_ROBIN: return "round-robin";
        case ConnSelectPolicy::RANDOM:      return "random";
        case ConnSelectPolicy::FIFO_IDLE:   return "fifo";
    }
    return "round-robin";
}

// ============================================================================
// Core Benchmark Engine
// ============================================================================
int run_benchmark(const Config &cfg, uint64_t *out_completed = nullptr) {
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        rlim_t needed = static_cast<rlim_t>(cfg.connections * 2 + 1024);
        if (rl.rlim_cur < needed) {
            rl.rlim_cur = std::min<rlim_t>(needed, rl.rlim_max);
            setrlimit(RLIMIT_NOFILE, &rl);
        }
    }

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

    int timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timer_fd < 0) {
        perror("timerfd_create");
        close(epoll_fd);
        return 1;
    }

    struct epoll_event ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.u32 = TIMER_FD_TAG;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, timer_fd, &ev) < 0) {
        perror("epoll_ctl timer_fd");
        close(timer_fd);
        close(epoll_fd);
        return 1;
    }

    // Determine exact fixed active connection set K out of N
    uint32_t target_active_count = (cfg.active_connections > 0)
        ? cfg.active_connections
        : std::max(1U, static_cast<uint32_t>(std::round(cfg.connections * cfg.active_share)));

    std::vector<Connection> conns(cfg.connections);
    for (uint32_t i = 0; i < cfg.connections; ++i) {
        conns[i].id = i;
        if ((static_cast<uint64_t>(i) * target_active_count / cfg.connections) !=
            (static_cast<uint64_t>(i + 1) * target_active_count / cfg.connections)) {
            conns[i].is_active = true;
        }
    }

    std::cout << "Establishing " << cfg.connections << " persistent TCP connections ("
              << target_active_count << " active, " << (cfg.connections - target_active_count)
              << " idle) to " << cfg.host << ":" << cfg.port << "...\n";

    uint64_t conn_start_ns = get_time_ns();
    uint64_t conn_timeout_ns = static_cast<uint64_t>(cfg.connect_timeout * 1e9);
    uint32_t connected_count = 0;
    uint64_t conn_failures = 0;
    int max_fd_seen = -1;

    for (uint32_t i = 0; i < cfg.connections; ++i) {
        conns[i].send_buf.resize(cfg.msg_size);
        conns[i].recv_buf.resize( std::max<size_t>(cfg.msg_size * std::max(4U, cfg.pipeline_depth * 2), 8192) );

        int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0 || !set_socket_options(fd)) {
            if (fd >= 0) close(fd);
            conn_failures++;
            conns[i].state = ConnState::FAILED;
            continue;
        }
        max_fd_seen = std::max(max_fd_seen, fd);
        conns[i].fd = fd;

        int rc = connect(fd, reinterpret_cast<struct sockaddr *>(&server_addr), sizeof(server_addr));
        if (rc == 0) {
            conns[i].state = ConnState::ACTIVE;
            connected_count++;
            conns[i].epoll_events = EPOLLIN | EPOLLERR | EPOLLHUP;
            ev.events = conns[i].epoll_events;
            ev.data.u32 = i;
            epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev);
        } else if (rc < 0 && errno == EINPROGRESS) {
            conns[i].state = ConnState::CONNECTING;
            conns[i].epoll_events = EPOLLOUT | EPOLLERR | EPOLLHUP;
            ev.events = conns[i].epoll_events;
            ev.data.u32 = i;
            epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev);
        } else {
            close(fd);
            conns[i].fd = -1;
            conn_failures++;
            conns[i].state = ConnState::FAILED;
        }
    }

    struct epoll_event events[MAX_EPOLL_EVENTS];
    while (connected_count + conn_failures < cfg.connections && !g_stop_requested) {
        uint64_t now_ns = get_time_ns();
        if (now_ns - conn_start_ns > conn_timeout_ns) {
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

        int nfds = epoll_wait(epoll_fd, events, MAX_EPOLL_EVENTS, 25);
        for (int e = 0; e < nfds; ++e) {
            uint32_t id = events[e].data.u32;
            if (id == TIMER_FD_TAG || id >= cfg.connections) continue;
            Connection &conn = conns[id];
            if (conn.state != ConnState::CONNECTING) continue;

            int so_error = 0;
            socklen_t len = sizeof(so_error);
            if (getsockopt(conn.fd, SOL_SOCKET, SO_ERROR, &so_error, &len) == 0 && so_error == 0) {
                conn.state = ConnState::ACTIVE;
                connected_count++;
                conn.epoll_events = EPOLLIN | EPOLLERR | EPOLLHUP;
                ev.events = conn.epoll_events;
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

    // Build fixed vector of active established connection IDs
    std::vector<uint32_t> active_conn_ids;
    std::deque<uint32_t> fifo_eligible_conns;
    active_conn_ids.reserve(target_active_count);
    for (uint32_t i = 0; i < cfg.connections; ++i) {
        if (conns[i].state == ConnState::ACTIVE && conns[i].is_active) {
            active_conn_ids.push_back(i);
            fifo_eligible_conns.push_back(i);
            conns[i].in_fifo_idle_queue = true;
        }
    }

    std::cout << "Connection barrier complete: " << connected_count << " / " << cfg.connections
              << " established (" << active_conn_ids.size() << " active, max_fd=" << max_fd_seen << ").\n";

    if (active_conn_ids.empty()) {
        std::cerr << "Fatal: Zero active connections established. Aborting.\n";
        close(timer_fd);
        close(epoll_fd);
        return 1;
    }

    // Separate deterministic RNG streams for arrival scheduling vs connection selection
    std::mt19937_64 rng_arrival(cfg.seed ^ 0x9E3779B97F4A7C15ULL);
    std::mt19937_64 rng_conn(cfg.seed ^ 0xBF58476D1CE4E5B9ULL);
    std::uniform_real_distribution<double> uni_arrival(1e-12, 1.0);
    std::uniform_int_distribution<size_t> uni_conn(0, active_conn_ids.size() - 1);

    // Phase timing state
    BenchPhase phase = (cfg.warmup > 0.0) ? BenchPhase::WARMUP : BenchPhase::MEASURING;
    uint64_t phase_start_ns = get_time_ns();
    uint64_t warmup_duration_ns = static_cast<uint64_t>(cfg.warmup * 1e9);
    uint64_t measure_duration_ns = static_cast<uint64_t>(cfg.duration * 1e9);
    uint64_t cooldown_duration_ns = static_cast<uint64_t>(cfg.cooldown * 1e9);
    uint64_t timeout_ns = cfg.timeout_ms * 1000000ULL;

    // Measurement counters (strictly for measurement-cohort requests)
    uint64_t meas_scheduled = 0;
    uint64_t meas_dropped_queue_full = 0;
    uint64_t meas_sent = 0;
    uint64_t meas_completed = 0;
    uint64_t meas_send_errors = 0;
    uint64_t meas_recv_errors = 0;
    uint64_t meas_timeouts = 0;
    uint64_t meas_mismatched = 0;
    uint64_t meas_pipeline_stalls = 0;
    uint64_t timer_expirations_total = 0;
    size_t current_total_queued = 0;
    size_t max_total_queued_seen = 0;
    size_t current_meas_outstanding = 0; // Queued + active_tx + in_flight belonging to MEASURING cohort

    size_t sample_reserve = (cfg.mode == WorkloadMode::CLOSED_LOOP)
        ? 2000000
        : (static_cast<size_t>(cfg.rate * cfg.duration * 1.25) + 10000);
    std::vector<uint32_t> sched_lag_samples_us;
    std::vector<uint32_t> app_rtt_samples_us;
    std::vector<uint32_t> e2e_lat_samples_us;
    sched_lag_samples_us.reserve(sample_reserve);
    app_rtt_samples_us.reserve(sample_reserve);
    e2e_lat_samples_us.reserve(sample_reserve);

    uint32_t num_sec_buckets = std::max(1U, static_cast<uint32_t>(std::floor(cfg.duration)));
    std::vector<uint64_t> sec_bucket_completed(num_sec_buckets, 0);

    // Resource & TCP retransmission snapshots at measurement boundaries
    uint64_t measurement_actual_start_ns = 0;
    uint64_t measurement_actual_end_ns = 0;
    struct rusage ru_meas_start{}, ru_meas_end{};
    uint64_t tcp_retrans_start = 0;
    uint64_t tcp_retrans_end = 0;

    auto update_epoll_events = [&](Connection &conn, uint32_t new_events) {
        if (conn.fd >= 0 && conn.epoll_events != new_events) {
            struct epoll_event ev_mod;
            std::memset(&ev_mod, 0, sizeof(ev_mod));
            ev_mod.events = new_events;
            ev_mod.data.u32 = conn.id;
            epoll_ctl(epoll_fd, EPOLL_CTL_MOD, conn.fd, &ev_mod);
            conn.epoll_events = new_events;
        }
    };

    auto close_connection_on_error = [&](Connection &conn) {
        if (conn.has_active_tx && conn.active_tx.in_measurement_cohort) {
            if (current_meas_outstanding > 0) current_meas_outstanding--;
        }
        conn.has_active_tx = false;
        while (!conn.tx_queue.empty()) {
            if (conn.tx_queue.front().in_measurement_cohort && current_meas_outstanding > 0) {
                current_meas_outstanding--;
            }
            if (current_total_queued > 0) current_total_queued--;
            conn.tx_queue.pop_front();
        }
        while (!conn.in_flight.empty()) {
            if (conn.in_flight.front().in_measurement_cohort && current_meas_outstanding > 0) {
                current_meas_outstanding--;
            }
            conn.in_flight.pop_front();
        }
        conn.state = ConnState::CLOSED;
        if (conn.fd >= 0) {
            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, conn.fd, nullptr);
            close(conn.fd);
            conn.fd = -1;
        }
    };

    // Forward declaration of transmit pump
    auto try_write_active_tx = [&](Connection &conn, uint64_t now_ns) -> bool {
        while (conn.has_active_tx && conn.state == ConnState::ACTIVE) {
            size_t remaining = cfg.msg_size - conn.send_bytes_written;
            ssize_t n = 0;
            do {
                n = send(conn.fd, conn.send_buf.data() + conn.send_bytes_written, remaining, MSG_NOSIGNAL);
            } while (n < 0 && errno == EINTR);

            if (n > 0) {
                conn.send_bytes_written += static_cast<size_t>(n);
                if (conn.send_bytes_written == cfg.msg_size) {
                    // Frame completely accepted by kernel! Record t_send = send_done_ns
                    uint64_t send_done_ns = get_time_ns();
                    InFlightReq if_req;
                    if_req.seq_num = conn.active_tx.seq_num;
                    if_req.sched_time_ns = conn.active_tx.sched_time_ns;
                    if_req.tx_start_ns = conn.active_tx.tx_start_ns;
                    if_req.send_time_ns = send_done_ns;
                    if_req.in_measurement_cohort = conn.active_tx.in_measurement_cohort;

                    conn.in_flight.push_back(if_req);
                    if (if_req.in_measurement_cohort) {
                        meas_sent++;
                        conn.meas_sent++;
                    }
                    conn.has_active_tx = false;
                    conn.send_bytes_written = 0;
                    return true; // Completed frame write
                }
            } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                update_epoll_events(conn, EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLHUP);
                return false;
            } else {
                if (phase == BenchPhase::MEASURING) meas_send_errors++;
                close_connection_on_error(conn);
                return false;
            }
            (void)now_ns;
        }
        return true;
    };

    // Strict FIFO pump: starts transmitting from conn.tx_queue only when
    // !conn.has_active_tx && conn.in_flight.size() < cfg.pipeline_depth
    auto pump_conn_tx_queue = [&](Connection &conn, uint64_t now_ns) {
        while (conn.state == ConnState::ACTIVE &&
               !conn.has_active_tx &&
               conn.in_flight.size() < cfg.pipeline_depth &&
               !conn.tx_queue.empty()) {
            PendingReq req = conn.tx_queue.front();
            conn.tx_queue.pop_front();
            if (current_total_queued > 0) current_total_queued--;

            req.tx_start_ns = now_ns;
            conn.has_active_tx = true;
            conn.active_tx = req;
            build_wire_frame(conn, req, cfg.msg_size);

            if (!try_write_active_tx(conn, now_ns)) {
                break; // Blocked on partial write (EPOLLOUT armed)
            }
        }
        if (conn.state == ConnState::ACTIVE && !conn.has_active_tx) {
            update_epoll_events(conn, EPOLLIN | EPOLLERR | EPOLLHUP);
            if (cfg.conn_select == ConnSelectPolicy::FIFO_IDLE &&
                conn.tx_queue.empty() &&
                conn.in_flight.size() < cfg.pipeline_depth &&
                !conn.in_fifo_idle_queue) {
                fifo_eligible_conns.push_back(conn.id);
                conn.in_fifo_idle_queue = true;
            }
        }
    };

    // Assigns a scheduled request to a specific connection respecting strict FIFO order
    auto dispatch_or_enqueue_on_conn = [&](Connection &conn, uint64_t sched_time_ns, uint64_t now_ns, bool in_meas) {
        if (conn.state != ConnState::ACTIVE) return;

        PendingReq req;
        req.seq_num = conn.next_assign_seq++;
        req.sched_time_ns = sched_time_ns;
        req.tx_start_ns = 0;
        req.in_measurement_cohort = in_meas;

        if (in_meas) {
            conn.meas_assigned++;
            current_meas_outstanding++;
        }

        // Correction 1: Only transmit immediately if tx_queue is empty, no active partial TX, and in_flight < W
        if (conn.tx_queue.empty() && !conn.has_active_tx && conn.in_flight.size() < cfg.pipeline_depth) {
            req.tx_start_ns = now_ns;
            conn.has_active_tx = true;
            conn.active_tx = req;
            build_wire_frame(conn, req, cfg.msg_size);
            try_write_active_tx(conn, now_ns);
            if (!conn.has_active_tx) {
                update_epoll_events(conn, EPOLLIN | EPOLLERR | EPOLLHUP);
            }
        } else {
            if (in_meas) meas_pipeline_stalls++;
            if (current_total_queued >= cfg.max_pending) {
                // Bounded queue full: drop arrival and undo sequence/outstanding increment
                conn.next_assign_seq--;
                if (in_meas) {
                    meas_dropped_queue_full++;
                    conn.meas_assigned--;
                    if (current_meas_outstanding > 0) current_meas_outstanding--;
                }
                return;
            }
            conn.tx_queue.push_back(req);
            current_total_queued++;
            max_total_queued_seen = std::max(max_total_queued_seen, current_total_queued);
        }
    };

    auto refill_closed_loop_conn = [&](Connection &conn, uint64_t now_ns) {
        if (cfg.mode != WorkloadMode::CLOSED_LOOP) return;
        if (phase != BenchPhase::WARMUP && phase != BenchPhase::MEASURING) return;
        bool in_meas = (phase == BenchPhase::MEASURING);
        while (conn.state == ConnState::ACTIVE &&
               !conn.has_active_tx &&
               conn.tx_queue.empty() &&
               conn.in_flight.size() < cfg.pipeline_depth) {
            if (in_meas) meas_scheduled++;
            dispatch_or_enqueue_on_conn(conn, now_ns, now_ns, in_meas);
        }
    };

    // Open-loop arrival deadline generator (Constant, Poisson, or Burst)
    uint64_t phase_k = 0;
    uint64_t next_sched_ns = phase_start_ns;
    size_t rr_cursor = 0;
    uint64_t burst_period_ns = static_cast<uint64_t>(cfg.burst_period_ms * 1e6);
    uint64_t burst_on_ns = static_cast<uint64_t>(cfg.burst_on_ms * 1e6);

    auto advance_next_arrival_deadline = [&]() {
        phase_k++;
        if (cfg.arrival == ArrivalProcess::CONSTANT) {
            // Drift-free absolute deadline: t_k = t_0 + floor(k * 1e9 / rate)
            uint64_t rate_int = std::max<uint64_t>(1ULL, static_cast<uint64_t>(std::llround(cfg.rate * 1000.0)));
            unsigned __int128 num = static_cast<unsigned __int128>(phase_k) * 1000000000000ULL;
            uint64_t offset_ns = static_cast<uint64_t>(num / rate_int);
            next_sched_ns = phase_start_ns + offset_ns;
        } else if (cfg.arrival == ArrivalProcess::POISSON) {
            double u = uni_arrival(rng_arrival);
            double dt_s = -std::log(u) / cfg.rate;
            uint64_t dt_ns = std::max<uint64_t>(1ULL, static_cast<uint64_t>(dt_s * 1e9));
            next_sched_ns += dt_ns;
        } else if (cfg.arrival == ArrivalProcess::BURST) {
            while (true) {
                uint64_t rel_ns = next_sched_ns - phase_start_ns;
                uint64_t cycle_idx = rel_ns / burst_period_ns;
                uint64_t cycle_pos_ns = rel_ns % burst_period_ns;
                if (cycle_pos_ns < burst_on_ns) {
                    uint64_t step_ns = std::max<uint64_t>(1ULL, static_cast<uint64_t>(1e9 / cfg.burst_on_rate));
                    if (cycle_pos_ns + step_ns < burst_on_ns) {
                        next_sched_ns += step_ns;
                        break;
                    } else {
                        // Transition to OFF window
                        next_sched_ns = phase_start_ns + cycle_idx * burst_period_ns + burst_on_ns;
                    }
                } else {
                    if (cfg.burst_off_rate <= 0.0) {
                        // Pure ON/OFF burst: jump directly to start of next ON period
                        next_sched_ns = phase_start_ns + (cycle_idx + 1) * burst_period_ns;
                        break;
                    } else {
                        uint64_t step_ns = std::max<uint64_t>(1ULL, static_cast<uint64_t>(1e9 / cfg.burst_off_rate));
                        uint64_t next_cycle_start = (cycle_idx + 1) * burst_period_ns;
                        if (cycle_pos_ns + step_ns < next_cycle_start) {
                            next_sched_ns += step_ns;
                            break;
                        } else {
                            next_sched_ns = phase_start_ns + next_cycle_start;
                            break;
                        }
                    }
                }
            }
        }
    };

    auto configure_phase_timer = [&]() {
        struct itimerspec its;
        std::memset(&its, 0, sizeof(its));
        if (cfg.mode == WorkloadMode::CLOSED_LOOP || phase == BenchPhase::COOLDOWN) {
            // 1 ms periodic wakeup for phase/timeout checks
            its.it_interval.tv_nsec = 1000000L;
            its.it_value.tv_nsec = 1000000L;
            timerfd_settime(timer_fd, 0, &its, nullptr);
        } else if (cfg.arrival == ArrivalProcess::CONSTANT) {
            // Periodic timer with absolute initial deadline (Correction 4)
            uint64_t period_ns = std::max<uint64_t>(50000ULL, static_cast<uint64_t>(1e9 / cfg.rate));
            its.it_interval.tv_sec = static_cast<time_t>(period_ns / 1000000000ULL);
            its.it_interval.tv_nsec = static_cast<long>(period_ns % 1000000000ULL);
            its.it_value.tv_sec = static_cast<time_t>(next_sched_ns / 1000000000ULL);
            its.it_value.tv_nsec = static_cast<long>(next_sched_ns % 1000000000ULL);
            timerfd_settime(timer_fd, TFD_TIMER_ABSTIME, &its, nullptr);
        } else {
            // One-shot absolute timer for Poisson / Burst
            its.it_value.tv_sec = static_cast<time_t>(next_sched_ns / 1000000000ULL);
            its.it_value.tv_nsec = static_cast<long>(next_sched_ns % 1000000000ULL);
            timerfd_settime(timer_fd, TFD_TIMER_ABSTIME, &its, nullptr);
        }
    };

    auto select_active_connection = [&]() -> uint32_t {
        if (cfg.conn_select == ConnSelectPolicy::ROUND_ROBIN) {
            uint32_t cid = active_conn_ids[rr_cursor % active_conn_ids.size()];
            rr_cursor++;
            return cid;
        } else if (cfg.conn_select == ConnSelectPolicy::RANDOM) {
            return active_conn_ids[uni_conn(rng_conn)];
        } else {
            // FIFO_IDLE with fallback to round-robin if all connections are busy
            while (!fifo_eligible_conns.empty()) {
                uint32_t cid = fifo_eligible_conns.front();
                fifo_eligible_conns.pop_front();
                conns[cid].in_fifo_idle_queue = false;
                if (conns[cid].state == ConnState::ACTIVE) {
                    return cid;
                }
            }
            uint32_t cid = active_conn_ids[rr_cursor % active_conn_ids.size()];
            rr_cursor++;
            return cid;
        }
    };

    // Start initial phase
    if (phase == BenchPhase::MEASURING) {
        measurement_actual_start_ns = phase_start_ns;
        getrusage(RUSAGE_SELF, &ru_meas_start);
        tcp_retrans_start = query_total_tcp_retrans(conns);
    }
    advance_next_arrival_deadline();
    configure_phase_timer();

    if (cfg.mode == WorkloadMode::CLOSED_LOOP) {
        uint64_t now_ns = get_time_ns();
        for (uint32_t cid : active_conn_ids) {
            refill_closed_loop_conn(conns[cid], now_ns);
        }
    }

    uint64_t last_timeout_scan_ns = phase_start_ns;

    // ========================================================================
    // Main Event Loop
    // ========================================================================
    while (phase != BenchPhase::FINISHED && !g_stop_requested) {
        int nfds = epoll_wait(epoll_fd, events, MAX_EPOLL_EVENTS, 5);
        uint64_t now_ns = get_time_ns();

        // 1. Process ready socket and timer events
        for (int e = 0; e < nfds; ++e) {
            uint32_t id = events[e].data.u32;
            if (id == TIMER_FD_TAG) {
                uint64_t expirations = 0;
                if (read(timer_fd, &expirations, sizeof(expirations)) > 0) {
                    if (phase == BenchPhase::MEASURING) {
                        timer_expirations_total += expirations;
                    }
                }
                continue;
            }
            if (id >= cfg.connections) continue;
            Connection &conn = conns[id];
            if (conn.state != ConnState::ACTIVE) continue;

            uint32_t ev_mask = events[e].events;
            if (ev_mask & (EPOLLERR | EPOLLHUP)) {
                if (phase == BenchPhase::MEASURING) meas_recv_errors++;
                close_connection_on_error(conn);
                continue;
            }

            // Resume partial write if EPOLLOUT ready
            if ((ev_mask & EPOLLOUT) && conn.has_active_tx) {
                if (try_write_active_tx(conn, now_ns)) {
                    pump_conn_tx_queue(conn, now_ns);
                    refill_closed_loop_conn(conn, now_ns);
                }
            }

            // Drain incoming echoed frames until EAGAIN (handles multi-frame coalescing & fragmentation)
            if ((ev_mask & EPOLLIN) && conn.state == ConnState::ACTIVE) {
                while (conn.state == ConnState::ACTIVE) {
                    if (conn.recv_bytes_buffered == conn.recv_buf.size()) {
                        conn.recv_buf.resize(conn.recv_buf.size() * 2);
                    }
                    size_t avail = conn.recv_buf.size() - conn.recv_bytes_buffered;
                    ssize_t n = 0;
                    do {
                        n = recv(conn.fd, conn.recv_buf.data() + conn.recv_bytes_buffered, avail, 0);
                    } while (n < 0 && errno == EINTR);

                    if (n > 0) {
                        conn.recv_bytes_buffered += static_cast<size_t>(n);
                        size_t parse_offset = 0;

                        // Parse all complete frames in recv_buf
                        while (conn.recv_bytes_buffered - parse_offset >= cfg.msg_size) {
                            const uint8_t *frame_ptr = conn.recv_buf.data() + parse_offset;
                            const MessageHeader *hdr = reinterpret_cast<const MessageHeader *>(frame_ptr);
                            const uint8_t *payload = frame_ptr + sizeof(MessageHeader);
                            size_t payload_len = cfg.msg_size - sizeof(MessageHeader);

                            bool valid = !conn.in_flight.empty() &&
                                         (hdr->magic == ECHO_MAGIC) &&
                                         (hdr->conn_id == conn.id) &&
                                         (hdr->seq_num == conn.expected_recv_seq) &&
                                         (hdr->seq_num == conn.in_flight.front().seq_num) &&
                                         (hdr->msg_size == cfg.msg_size) &&
                                         (hdr->checksum == compute_checksum(payload, payload_len));

                            uint64_t recv_time_ns = get_time_ns();
                            if (valid) {
                                InFlightReq req = conn.in_flight.front();
                                conn.in_flight.pop_front();
                                conn.expected_recv_seq++;

                                if (req.in_measurement_cohort) {
                                    meas_completed++;
                                    conn.meas_completed++;
                                    if (current_meas_outstanding > 0) current_meas_outstanding--;

                                    uint64_t sched_lag_ns = (req.send_time_ns >= req.sched_time_ns)
                                        ? (req.send_time_ns - req.sched_time_ns) : 0;
                                    uint64_t app_rtt_ns = (recv_time_ns >= req.send_time_ns)
                                        ? (recv_time_ns - req.send_time_ns) : 0;
                                    uint64_t e2e_lat_ns = (recv_time_ns >= req.sched_time_ns)
                                        ? (recv_time_ns - req.sched_time_ns) : 0;

                                    uint32_t app_rtt_us = static_cast<uint32_t>(app_rtt_ns / 1000ULL);
                                    sched_lag_samples_us.push_back(static_cast<uint32_t>(sched_lag_ns / 1000ULL));
                                    app_rtt_samples_us.push_back(app_rtt_us);
                                    e2e_lat_samples_us.push_back(static_cast<uint32_t>(e2e_lat_ns / 1000ULL));
                                    conn.rtt_sum_us += app_rtt_us;

                                    if (recv_time_ns >= measurement_actual_start_ns) {
                                        uint32_t sec_idx = static_cast<uint32_t>(
                                            (recv_time_ns - measurement_actual_start_ns) / 1000000000ULL);
                                        if (sec_idx < num_sec_buckets) {
                                            sec_bucket_completed[sec_idx]++;
                                        }
                                    }
                                }
                            } else {
                                if (phase == BenchPhase::MEASURING || phase == BenchPhase::COOLDOWN) {
                                    meas_mismatched++;
                                }
                                close_connection_on_error(conn);
                                break;
                            }
                            parse_offset += cfg.msg_size;
                        }

                        if (conn.state == ConnState::ACTIVE && parse_offset > 0) {
                            size_t leftover = conn.recv_bytes_buffered - parse_offset;
                            if (leftover > 0) {
                                std::memmove(conn.recv_buf.data(), conn.recv_buf.data() + parse_offset, leftover);
                            }
                            conn.recv_bytes_buffered = leftover;

                            // Dispatch queued requests in strict FIFO order now that in_flight slots freed up
                            pump_conn_tx_queue(conn, get_time_ns());
                            refill_closed_loop_conn(conn, get_time_ns());
                        }
                    } else if (n == 0) {
                        if (phase == BenchPhase::MEASURING) meas_recv_errors++;
                        close_connection_on_error(conn);
                        break;
                    } else {
                        if (errno != EAGAIN && errno != EWOULDBLOCK) {
                            if (phase == BenchPhase::MEASURING) meas_recv_errors++;
                            close_connection_on_error(conn);
                        }
                        break;
                    }
                }
            }
        }

        now_ns = get_time_ns();

        // 2. Phase Transitions
        if (phase == BenchPhase::WARMUP && (now_ns - phase_start_ns >= warmup_duration_ns)) {
            phase = BenchPhase::MEASURING;
            phase_start_ns = now_ns;
            measurement_actual_start_ns = now_ns;
            phase_k = 0;
            next_sched_ns = phase_start_ns;
            getrusage(RUSAGE_SELF, &ru_meas_start);
            tcp_retrans_start = query_total_tcp_retrans(conns);
            advance_next_arrival_deadline();
            configure_phase_timer();
            if (cfg.mode == WorkloadMode::CLOSED_LOOP) {
                for (uint32_t cid : active_conn_ids) {
                    refill_closed_loop_conn(conns[cid], now_ns);
                }
            }
        } else if (phase == BenchPhase::MEASURING && (now_ns - phase_start_ns >= measure_duration_ns)) {
            measurement_actual_end_ns = now_ns;
            if (cfg.cooldown > 0.0 && current_meas_outstanding > 0) {
                phase = BenchPhase::COOLDOWN;
                phase_start_ns = now_ns;
                configure_phase_timer();
            } else {
                phase = BenchPhase::FINISHED;
                break;
            }
        } else if (phase == BenchPhase::COOLDOWN) {
            if (current_meas_outstanding == 0 || (now_ns - phase_start_ns >= cooldown_duration_ns)) {
                phase = BenchPhase::FINISHED;
                break;
            }
        }

        // 3. Open-Loop Arrival Scheduling
        if (cfg.mode == WorkloadMode::OPEN_LOOP &&
            (phase == BenchPhase::WARMUP || phase == BenchPhase::MEASURING)) {
            uint64_t phase_limit_ns = (phase == BenchPhase::WARMUP) ? warmup_duration_ns : measure_duration_ns;
            bool in_meas = (phase == BenchPhase::MEASURING);
            bool deadline_advanced = false;

            while (next_sched_ns <= now_ns && (next_sched_ns - phase_start_ns) < phase_limit_ns) {
                if (in_meas) meas_scheduled++;
                uint32_t cid = select_active_connection();
                dispatch_or_enqueue_on_conn(conns[cid], next_sched_ns, now_ns, in_meas);
                advance_next_arrival_deadline();
                deadline_advanced = true;
            }

            if (deadline_advanced && cfg.arrival != ArrivalProcess::CONSTANT) {
                // Re-arm one-shot timerfd only once for the next future deadline
                struct itimerspec its;
                std::memset(&its, 0, sizeof(its));
                its.it_value.tv_sec = static_cast<time_t>(next_sched_ns / 1000000000ULL);
                its.it_value.tv_nsec = static_cast<long>(next_sched_ns % 1000000000ULL);
                timerfd_settime(timer_fd, TFD_TIMER_ABSTIME, &its, nullptr);
            }
        }

        // 4. Periodic Timeout Scan (every 10 ms)
        now_ns = get_time_ns();
        if (now_ns - last_timeout_scan_ns >= 10000000ULL) {
            last_timeout_scan_ns = now_ns;
            for (uint32_t cid : active_conn_ids) {
                Connection &conn = conns[cid];
                if (conn.state != ConnState::ACTIVE) continue;

                bool timed_out = false;
                if (!conn.in_flight.empty() &&
                    now_ns > conn.in_flight.front().send_time_ns &&
                    (now_ns - conn.in_flight.front().send_time_ns > timeout_ns)) {
                    timed_out = true;
                } else if (conn.has_active_tx &&
                           now_ns > conn.active_tx.tx_start_ns &&
                           (now_ns - conn.active_tx.tx_start_ns > timeout_ns)) {
                    timed_out = true;
                }
                if (timed_out) {
                    if (phase == BenchPhase::MEASURING || phase == BenchPhase::COOLDOWN) {
                        meas_timeouts++;
                        conn.meas_timeouts++;
                    }
                    // Safe timeout recovery: close the desynchronized TCP stream rather than reusing it
                    close_connection_on_error(conn);
                }
            }
        }
    }

    uint64_t drain_end_ns = get_time_ns();
    if (measurement_actual_end_ns == 0) {
        measurement_actual_end_ns = drain_end_ns;
    }
    getrusage(RUSAGE_SELF, &ru_meas_end);
    tcp_retrans_end = query_total_tcp_retrans(conns);

    for (auto &conn : conns) {
        if (conn.fd >= 0) {
            close(conn.fd);
            conn.fd = -1;
        }
    }
    close(timer_fd);
    close(epoll_fd);

    // ========================================================================
    // Compute Summary Statistics
    // ========================================================================
    double actual_duration_s = static_cast<double>(measurement_actual_end_ns - measurement_actual_start_ns) / 1e9;
    if (actual_duration_s <= 0.0) actual_duration_s = cfg.duration;
    double total_meas_Plus_drain_s = static_cast<double>(drain_end_ns - measurement_actual_start_ns) / 1e9;
    if (total_meas_Plus_drain_s <= 0.0) total_meas_Plus_drain_s = actual_duration_s;

    double offered_rate = static_cast<double>(meas_scheduled) / actual_duration_s;
    double actual_send_rate = static_cast<double>(meas_sent) / actual_duration_s;
    double echoed_rate = static_cast<double>(meas_completed) / actual_duration_s;
    double delivery_ratio_pct = (meas_scheduled > 0)
        ? (100.0 * static_cast<double>(meas_completed) / static_cast<double>(meas_scheduled))
        : 0.0;

    Percentiles p_sched = compute_percentiles(sched_lag_samples_us);
    Percentiles p_app   = compute_percentiles(app_rtt_samples_us);
    Percentiles p_e2e   = compute_percentiles(e2e_lat_samples_us);

    // Per-second throughput stability (CV %)
    double sec_mean = 0.0, sec_cv_pct = 0.0;
    if (!sec_bucket_completed.empty()) {
        for (uint64_t v : sec_bucket_completed) sec_mean += static_cast<double>(v);
        sec_mean /= static_cast<double>(sec_bucket_completed.size());
        if (sec_mean > 0.0 && sec_bucket_completed.size() > 1) {
            double var = 0.0;
            for (uint64_t v : sec_bucket_completed) {
                double d = static_cast<double>(v) - sec_mean;
                var += d * d;
            }
            var /= static_cast<double>(sec_bucket_completed.size());
            sec_cv_pct = 100.0 * std::sqrt(var) / sec_mean;
        }
    }

    // Per-active-connection fairness distribution
    uint64_t conn_sent_min = UINT64_MAX, conn_sent_max = 0;
    double conn_sent_mean = 0.0, conn_sent_cv_pct = 0.0;
    for (uint32_t cid : active_conn_ids) {
        uint64_t s = conns[cid].meas_sent;
        conn_sent_min = std::min(conn_sent_min, s);
        conn_sent_max = std::max(conn_sent_max, s);
        conn_sent_mean += static_cast<double>(s);
    }
    if (!active_conn_ids.empty()) {
        conn_sent_mean /= static_cast<double>(active_conn_ids.size());
        if (conn_sent_mean > 0.0 && active_conn_ids.size() > 1) {
            double var = 0.0;
            for (uint32_t cid : active_conn_ids) {
                double d = static_cast<double>(conns[cid].meas_sent) - conn_sent_mean;
                var += d * d;
            }
            var /= static_cast<double>(active_conn_ids.size());
            conn_sent_cv_pct = 100.0 * std::sqrt(var) / conn_sent_mean;
        }
    } else {
        conn_sent_min = 0;
    }

    double cpu_used_s = rusage_cpu_seconds(ru_meas_end) - rusage_cpu_seconds(ru_meas_start);
    double client_cpu_pct = std::max(0.0, 100.0 * cpu_used_s / total_meas_Plus_drain_s);
    uint64_t tcp_retrans_delta = (tcp_retrans_end >= tcp_retrans_start)
        ? (tcp_retrans_end - tcp_retrans_start) : 0;

    bool target_achieved = (cfg.mode == WorkloadMode::CLOSED_LOOP)
        ? (meas_completed > 0)
        : (delivery_ratio_pct >= 95.0 && meas_completed >= static_cast<uint64_t>(cfg.rate * actual_duration_s * 0.95));

    double effective_active_share = static_cast<double>(active_conn_ids.size()) / static_cast<double>(cfg.connections);
    // Closed-loop has no target rate; report 0 so it is never grouped with the default --rate (1000).
    double reported_target_rate = (cfg.mode == WorkloadMode::CLOSED_LOOP) ? 0.0 : cfg.rate;

    // Console Summary Output
    std::cout << "\n======================================================================\n"
              << "               TCP ECHO LOAD GENERATOR SUMMARY\n"
              << "======================================================================\n"
              << std::fixed << std::setprecision(2)
              << "Target Configuration:\n"
              << "  Server:                  " << cfg.host << ":" << cfg.port
              << (cfg.tag.empty() ? "" : (" [" + cfg.tag + "]")) << "\n"
              << "  Mode / Dist / Policy:    " << mode_to_str(cfg.mode) << " / "
              << cfg.dist_label << " / " << policy_to_str(cfg.conn_select) << "\n"
              << "  Connections / Pipeline:  " << cfg.connections << " total (" << active_conn_ids.size()
              << " active, max_fd=" << max_fd_seen << "), W=" << cfg.pipeline_depth << "\n"
              << "  Target Offered Rate:     "
              << (cfg.mode == WorkloadMode::CLOSED_LOOP ? "CLOSED-LOOP (max concurrency)" : (std::to_string(static_cast<int>(cfg.rate)) + " msgs/sec")) << "\n"
              << "  Frame Size / Seed:       " << cfg.msg_size << " B (32B hdr + " << (cfg.msg_size - 32) << "B payload), seed=" << cfg.seed << "\n\n"
              << "Accounting & Throughput:\n"
              << "  Measurement Duration:    " << actual_duration_s << " s (Warmup: " << cfg.warmup << " s, Cooldown cap: " << cfg.cooldown << " s)\n"
              << "  Scheduled Arrivals:      " << meas_scheduled << " (" << offered_rate << " msgs/sec)\n"
              << "  Messages Sent:           " << meas_sent << " (" << actual_send_rate << " msgs/sec)\n"
              << "  Valid Echoes Completed:  " << meas_completed << " (" << echoed_rate << " msgs/sec)\n"
              << "  Delivery Ratio:          " << delivery_ratio_pct << " % (Incomplete at end: " << current_meas_outstanding << ")\n"
              << "  1s Throughput CV:        " << sec_cv_pct << " %\n\n"
              << "Latency Breakdown (microseconds) — [p50 / p95 / p99 / avg / max]:\n"
              << "  Scheduler Lag (send-sched): " << p_sched.p50 << " / " << p_sched.p95 << " / " << p_sched.p99
              << " / " << p_sched.avg << " / " << p_sched.max << " us\n"
              << "  Application RTT (recv-send):" << p_app.p50 << " / " << p_app.p95 << " / " << p_app.p99
              << " / " << p_app.avg << " / " << p_app.max << " us\n"
              << "  End-to-End (recv-sched):    " << p_e2e.p50 << " / " << p_e2e.p95 << " / " << p_e2e.p99
              << " / " << p_e2e.avg << " / " << p_e2e.max << " us\n\n"
              << "Diagnostics, Fairness & Client Health:\n"
              << "  Per-Conn Sent [min/max/CV]: " << conn_sent_min << " / " << conn_sent_max << " / " << conn_sent_cv_pct << " %\n"
              << "  Client CPU / TCP Retrans:   " << client_cpu_pct << " % / " << tcp_retrans_delta << " retrans\n"
              << "  Max Pending Queue / Drops:  " << max_total_queued_seen << " / " << meas_dropped_queue_full << "\n"
              << "  Pipeline Stalls / Timeouts: " << meas_pipeline_stalls << " / " << meas_timeouts << "\n"
              << "  Conn / Send / Recv / Bad:   " << conn_failures << " / " << meas_send_errors
              << " / " << meas_recv_errors << " / " << meas_mismatched << "\n"
              << "======================================================================\n";

    // Write CSV output (includes both rtt_* and app_rtt_* / e2e_lat_* column aliases for full plotter compatibility)
    if (!cfg.output_csv.empty()) {
        ensure_parent_dir(cfg.output_csv);
        bool file_exists = (access(cfg.output_csv.c_str(), F_OK) == 0);
        std::ofstream csv(cfg.output_csv, std::ios::app);
        if (csv.is_open()) {
            if (!file_exists || std::ifstream(cfg.output_csv).peek() == std::ifstream::traits_type::eof()) {
                csv << "tag,mode,dist,conn_select,connections,active_connections,active_share,pipeline_window,"
                    << "target_rate,offered_rate,actual_send_rate,echoed_rate,delivery_ratio_pct,"
                    << "scheduled,total_sent,total_received,incomplete_at_end,dropped_queue_full,"
                    << "sched_lag_p50_us,sched_lag_p95_us,sched_lag_p99_us,sched_lag_max_us,"
                    << "app_rtt_p50_us,app_rtt_p95_us,app_rtt_p99_us,rtt_p50_us,rtt_p95_us,rtt_p99_us,rtt_min_us,rtt_avg_us,rtt_max_us,"
                    << "e2e_lat_p50_us,e2e_lat_p95_us,e2e_lat_p99_us,e2e_lat_max_us,"
                    << "tput_1s_cv_pct,conn_sent_min,conn_sent_max,conn_sent_cv_pct,"
                    << "client_cpu_pct,tcp_retrans_delta,max_pending_queue,concurrency_stalls,"
                    << "conn_failures,send_errors,recv_errors,timeouts,mismatched,"
                    << "duration_s,actual_duration_s,host,port,msg_size,seed,max_fd,target_achieved\n";
            }
            csv << std::fixed << std::setprecision(2)
                << (cfg.tag.empty() ? "default" : cfg.tag) << ","
                << mode_to_str(cfg.mode) << ","
                << cfg.dist_label << ","
                << policy_to_str(cfg.conn_select) << ","
                << cfg.connections << ","
                << active_conn_ids.size() << ","
                << effective_active_share << ","
                << cfg.pipeline_depth << ","
                << reported_target_rate << ","
                << offered_rate << ","
                << actual_send_rate << ","
                << echoed_rate << ","
                << delivery_ratio_pct << ","
                << meas_scheduled << ","
                << meas_sent << ","
                << meas_completed << ","
                << current_meas_outstanding << ","
                << meas_dropped_queue_full << ","
                << p_sched.p50 << "," << p_sched.p95 << "," << p_sched.p99 << "," << p_sched.max << ","
                << p_app.p50 << "," << p_app.p95 << "," << p_app.p99 << ","
                << p_app.p50 << "," << p_app.p95 << "," << p_app.p99 << ","
                << p_app.min << "," << p_app.avg << "," << p_app.max << ","
                << p_e2e.p50 << "," << p_e2e.p95 << "," << p_e2e.p99 << "," << p_e2e.max << ","
                << sec_cv_pct << "," << conn_sent_min << "," << conn_sent_max << "," << conn_sent_cv_pct << ","
                << client_cpu_pct << "," << tcp_retrans_delta << "," << max_total_queued_seen << "," << meas_pipeline_stalls << ","
                << conn_failures << "," << meas_send_errors << "," << meas_recv_errors << ","
                << meas_timeouts << "," << meas_mismatched << ","
                << cfg.duration << "," << actual_duration_s << ","
                << cfg.host << "," << cfg.port << "," << cfg.msg_size << ","
                << cfg.seed << "," << max_fd_seen << "," << (target_achieved ? 1 : 0) << "\n";
        }
    }

    // Write JSON report if requested
    if (!cfg.json_output.empty()) {
        ensure_parent_dir(cfg.json_output);
        std::ofstream jf(cfg.json_output);
        if (jf.is_open()) {
            jf << std::fixed << std::setprecision(2)
               << "{\n"
               << "  \"tag\": \"" << (cfg.tag.empty() ? "default" : cfg.tag) << "\",\n"
               << "  \"mode\": \"" << mode_to_str(cfg.mode) << "\",\n"
               << "  \"dist\": \"" << cfg.dist_label << "\",\n"
               << "  \"conn_select\": \"" << policy_to_str(cfg.conn_select) << "\",\n"
               << "  \"connections\": " << cfg.connections << ",\n"
               << "  \"active_connections\": " << active_conn_ids.size() << ",\n"
               << "  \"active_share\": " << effective_active_share << ",\n"
               << "  \"pipeline_window\": " << cfg.pipeline_depth << ",\n"
               << "  \"msg_size\": " << cfg.msg_size << ",\n"
               << "  \"seed\": " << cfg.seed << ",\n"
               << "  \"max_fd\": " << max_fd_seen << ",\n"
               << "  \"target_rate\": " << reported_target_rate << ",\n"
               << "  \"offered_rate\": " << offered_rate << ",\n"
               << "  \"actual_send_rate\": " << actual_send_rate << ",\n"
               << "  \"echoed_rate\": " << echoed_rate << ",\n"
               << "  \"delivery_ratio_pct\": " << delivery_ratio_pct << ",\n"
               << "  \"scheduled\": " << meas_scheduled << ",\n"
               << "  \"total_sent\": " << meas_sent << ",\n"
               << "  \"total_received\": " << meas_completed << ",\n"
               << "  \"incomplete_at_end\": " << current_meas_outstanding << ",\n"
               << "  \"dropped_queue_full\": " << meas_dropped_queue_full << ",\n"
               << "  \"sched_lag_us\": {\"p50\": " << p_sched.p50 << ", \"p95\": " << p_sched.p95
               << ", \"p99\": " << p_sched.p99 << ", \"avg\": " << p_sched.avg << ", \"max\": " << p_sched.max << "},\n"
               << "  \"app_rtt_us\": {\"min\": " << p_app.min << ", \"p50\": " << p_app.p50
               << ", \"p95\": " << p_app.p95 << ", \"p99\": " << p_app.p99 << ", \"avg\": " << p_app.avg << ", \"max\": " << p_app.max << "},\n"
               << "  \"e2e_lat_us\": {\"p50\": " << p_e2e.p50 << ", \"p95\": " << p_e2e.p95
               << ", \"p99\": " << p_e2e.p99 << ", \"avg\": " << p_e2e.avg << ", \"max\": " << p_e2e.max << "},\n"
               << "  \"client_cpu_pct\": " << client_cpu_pct << ",\n"
               << "  \"tcp_retrans_delta\": " << tcp_retrans_delta << ",\n"
               << "  \"conn_sent_cv_pct\": " << conn_sent_cv_pct << ",\n"
               << "  \"tput_1s_cv_pct\": " << sec_cv_pct << ",\n"
               << "  \"sec_buckets\": [";
            for (size_t i = 0; i < sec_bucket_completed.size(); ++i) {
                jf << (i ? ", " : "") << sec_bucket_completed[i];
            }
            jf << "],\n"
               << "  \"errors\": {\"conn_failures\": " << conn_failures
               << ", \"send_errors\": " << meas_send_errors
               << ", \"recv_errors\": " << meas_recv_errors
               << ", \"timeouts\": " << meas_timeouts
               << ", \"mismatched\": " << meas_mismatched << "}\n"
               << "}\n";
        }
    }

    if (out_completed) {
        *out_completed = meas_completed;
    }

    return (conn_failures == 0 && meas_send_errors == 0 && meas_recv_errors == 0 && meas_mismatched == 0) ? 0 : 2;
}

// ============================================================================
// Built-In Loopback Verification Suite (--self-test)
// ============================================================================
int run_self_test() {
    std::cout << "[SELF-TEST] Starting built-in loopback verification suite...\n";

    int listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);
    if (bind(listen_fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0 ||
        listen(listen_fd, 128) < 0) {
        perror("bind/listen");
        close(listen_fd);
        return 1;
    }

    socklen_t alen = sizeof(addr);
    getsockname(listen_fd, reinterpret_cast<struct sockaddr *>(&addr), &alen);
    uint16_t port = ntohs(addr.sin_port);

    volatile bool stop_server = false;
    std::thread srv([&]() {
        int ep = epoll_create1(EPOLL_CLOEXEC);
        struct epoll_event ev{}, evs[64];
        ev.events = EPOLLIN;
        ev.data.fd = listen_fd;
        epoll_ctl(ep, EPOLL_CTL_ADD, listen_fd, &ev);

        std::vector<int> client_fds;
        uint8_t buf[16384];
        uint64_t toggle = 0;

        while (!stop_server) {
            int n = epoll_wait(ep, evs, 64, 10);
            for (int i = 0; i < n; ++i) {
                int fd = evs[i].data.fd;
                if (fd == listen_fd) {
                    while (true) {
                        int cfd = accept4(listen_fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
                        if (cfd < 0) break;
                        set_socket_options(cfd);
                        ev.events = EPOLLIN;
                        ev.data.fd = cfd;
                        epoll_ctl(ep, EPOLL_CTL_ADD, cfd, &ev);
                        client_fds.push_back(cfd);
                    }
                } else {
                    ssize_t r = recv(fd, buf, sizeof(buf), 0);
                    if (r > 0) {
                        toggle++;
                        if ((toggle % 3 == 0) && r > 17) {
                            ssize_t half = r / 2;
                            ssize_t w1 = send(fd, buf, half, MSG_NOSIGNAL);
                            (void)w1;
                            ssize_t w2 = send(fd, buf + half, r - half, MSG_NOSIGNAL);
                            (void)w2;
                        } else {
                            ssize_t w = send(fd, buf, r, MSG_NOSIGNAL);
                            (void)w;
                        }
                    } else if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                        epoll_ctl(ep, EPOLL_CTL_DEL, fd, nullptr);
                        close(fd);
                    }
                }
            }
        }
        for (int cfd : client_fds) close(cfd);
        close(ep);
    });

    int rc = 0;
    {
        Config c;
        c.host = "127.0.0.1";
        c.port = port;
        c.connections = 20;
        c.active_connections = 10;
        c.rate = 2000;
        c.warmup = 0.5;
        c.duration = 2.0;
        c.cooldown = 0.5;
        c.pipeline_depth = 1;
        c.arrival = ArrivalProcess::CONSTANT;
        c.dist_label = "constant";
        c.tag = "selftest_constant_w1";
        std::cout << "\n[SELF-TEST 1/3] Constant rate + fragmented framing + active subset (K=10/20)...\n";
        uint64_t comp = 0;
        rc |= run_benchmark(c, &comp);
        if (comp < 3800) rc |= 4;
    }

    {
        Config c;
        c.host = "127.0.0.1";
        c.port = port;
        c.connections = 20;
        c.rate = 4000;
        c.warmup = 0.5;
        c.duration = 2.0;
        c.cooldown = 0.5;
        c.pipeline_depth = 4;
        c.arrival = ArrivalProcess::POISSON;
        c.dist_label = "poisson";
        c.conn_select = ConnSelectPolicy::RANDOM;
        c.tag = "selftest_poisson_w4";
        std::cout << "\n[SELF-TEST 2/3] Poisson arrivals + W=4 pipelining + random selection...\n";
        uint64_t comp = 0;
        rc |= run_benchmark(c, &comp);
        if (comp < 7000) rc |= 4;
    }

    {
        Config c;
        c.host = "127.0.0.1";
        c.port = port;
        c.connections = 20;
        c.rate = 3000;
        c.burst_on_ms = 40.0;
        c.burst_period_ms = 200.0;
        c.burst_on_rate = 15000.0;
        c.burst_off_rate = 0.0;
        c.warmup = 0.5;
        c.duration = 2.0;
        c.cooldown = 0.5;
        c.pipeline_depth = 2;
        c.arrival = ArrivalProcess::BURST;
        c.dist_label = "burst";
        c.tag = "selftest_burst_w2";
        std::cout << "\n[SELF-TEST 3/3] ON/OFF Burst arrivals + W=2 pipelining...\n";
        uint64_t comp = 0;
        rc |= run_benchmark(c, &comp);
        if (comp < 5500) rc |= 4;
    }

    stop_server = true;
    srv.join();
    close(listen_fd);

    if (rc == 0) {
        std::cout << "\n[SELF-TEST PASSED] All framing, scheduling, and pipeline invariants verified.\n";
    } else {
        std::cerr << "\n[SELF-TEST FAILED] Return code: " << rc << "\n";
    }
    return rc;
}

} // namespace

int main(int argc, char **argv) {
    Config cfg;
    if (!parse_args(argc, argv, cfg)) {
        print_usage(argv[0]);
        return 1;
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    if (cfg.self_test) {
        return run_self_test();
    }
    return run_benchmark(cfg);
}