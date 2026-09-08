#include "network_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <poll.h>       // struct pollfd, poll(), POLLIN/POLLOUT/POLLERR/POLLHUP/POLLNVAL
#include <unistd.h>     // close()
#include <errno.h>      // errno, EINTR, EAGAIN, EWOULDBLOCK
#include <signal.h>

// #include <netinet/tcp.h>// TCP_NODELAY
// #include <netinet/in.h> // IPPROTO_TCP

// MAX_CLIENTS is a fixed cap, not a hard poll() limitation — poll has no
// FD_SETSIZE-style ceiling (that's a select-only limit). We cap here so that
// select/poll/epoll are all benchmarked against the identical connection
// ceiling per your team's fairness rule. See the note above main() for how
// to swap this for a realloc()-based dynamically growing array if you want
// to demonstrate poll's lack of a hard limit as a side discussion.
#define MAX_CLIENTS 10000
#define OUT_BUF_CAP 65536   // 64KB — matches server_epoll.c exactly for a fair comparison
#define POLL_TIMEOUT_MS -1  // block forever, mirrors epoll_wait(..., -1)

// --------------------------------------------------------------------------
//                         CLIENT STATE (identical shape to server_epoll.c)
// --------------------------------------------------------------------------
typedef struct {
    int fd;
    char out_buf[OUT_BUF_CAP];
    size_t out_len;  // unsent bytes currently buffered
    int writing;     // 1 if we're currently monitoring POLLOUT, 0 otherwise
} client_state;

client_state* create_client_state(int fd) {
    client_state* state = malloc(sizeof(client_state));
    if (!state) return NULL;
    state->fd = fd;
    state->out_len = 0;
    state->writing = 0;
    return state;
}

void destroy_client_state(client_state* state) {
    if (!state) return;
    close(state->fd);
    free(state);
}

// --------------------------------------------------------------------------
//                         HELPER FUNCTIONS
// --------------------------------------------------------------------------

// Applied identically to every engine per your team's fairness rule.
// int set_tcp_nodelay(int fd) {
//     int flag = 1;
//     if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) < 0) {
//         perror("setsockopt(TCP_NODELAY) failed");
//         return -1;
//     }
//     return 0;
// }

// Flushes as much of state->out_buf as the kernel will currently accept.
// Unlike epoll_ctl(MOD), poll has no kernel-side interest list to update —
// we just mutate fds[idx].events directly, which is why flush needs the
// fds array and this client's index, not just an fd handle.
int flush_outbound_buffer(struct pollfd *fds, int idx, client_state *state) {
    while (state->out_len > 0) {
        ssize_t sent = send(state->fd, state->out_buf, state->out_len, 0);

        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue; // interrupted by signal, retry send
            perror("send failed");
            return -1;
        }
        if (sent > 0) {
            memmove(state->out_buf, state->out_buf + sent, state->out_len - sent);
            state->out_len -= sent;
        }
    }

    // Same state-transition-only toggling as the epoll version: only touch
    // fds[idx].events when the interest actually changes, not every call.
    if (state->out_len > 0 && !state->writing) {
        fds[idx].events = POLLIN | POLLOUT;
        state->writing = 1;
    } else if (state->out_len == 0 && state->writing) {
        fds[idx].events = POLLIN;
        state->writing = 0;
    }
    return 0;
}

// --------------------------------------------------------------------------
//                          MAIN
// --------------------------------------------------------------------------
//
// DYNAMIC-GROWTH VARIANT (for the "poll has no hard connection limit, unlike
// select" discussion in your report, without actually using it in the
// benchmarked build): replace the two malloc() calls below with an initial
// small allocation, and in the accept loop, when nfds == capacity, do
// capacity *= 2; fds = realloc(fds, capacity * sizeof(struct pollfd));
// clients = realloc(clients, capacity * sizeof(client_state*));
// Keep this OUT of the version you benchmark against epoll, since your team
// agreed on a fixed identical cap for fairness — mention it only as a
// "poll could do this, select structurally cannot" aside in the writeup.
//
int main(int argc, char* argv[]) {

    signal(SIGPIPE, SIG_IGN);
    
    int port = (argc > 1) ? atoi(argv[1]) : 8080;
    int backlog = (argc > 2) ? atoi(argv[2]) : SOMAXCONN;

    int server_socket = create_server_socket(port, backlog);
    if (server_socket == -1) {
        perror("server socket creation failed");
        return 1;
    }

    // Parallel arrays: fds[i] <-> clients[i] describe the same connection.
    // Index 0 is reserved for the listening socket itself (clients[0] unused).
    struct pollfd *fds = malloc(sizeof(struct pollfd) * (MAX_CLIENTS + 1));
    client_state **clients = malloc(sizeof(client_state*) * (MAX_CLIENTS + 1));
    if (!fds || !clients) {
        fprintf(stderr, "malloc failed for pollfd/client arrays\n");
        free(fds);
        free(clients);
        close(server_socket);
        return 1;
    }

    fds[0].fd = server_socket;
    fds[0].events = POLLIN;
    fds[0].revents = 0;
    clients[0] = NULL;

    int nfds = 1; // count of live entries in fds[]/clients[], including index 0

    printf("poll server listening on port %d with backlog %d (server_fd=%d, max_clients=%d)\n",
           port, backlog, server_socket, MAX_CLIENTS);

    while (1) {
        int ready = poll(fds, nfds, POLL_TIMEOUT_MS);

        // *** USER-SPACE <-> KERNEL-SPACE COPY POINT ***
        // Every single call to poll() copies the *entire* fds[] array
        // (nfds structs, active or idle) from user space into the kernel,
        // and the kernel copies the whole thing back out with revents
        // filled in — regardless of how many fds actually became ready.
        // With 10,000 idle connections and 1 active one, this is a
        // 10,000-struct round-trip copy for a single byte of traffic.
        // This is the memory-bandwidth tax epoll eliminates by keeping
        // its interest list resident in kernel memory across calls
        // (epoll_ctl only sends deltas, not the whole list, every time).

        if (ready == -1) {
            if (errno == EINTR) continue; // interrupted by signal, retry
            perror("poll failed");
            break;
        }
        if (ready == 0) continue; // unreachable with timeout = -1, kept for safety

        // ---- Listening socket: drain the accept queue ----
        if (fds[0].revents & POLLIN) {
            while (1) {
                int client_fd = accept(server_socket, NULL, NULL);
                if (client_fd < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break; // queue drained
                    perror("accept failed");
                    break;
                }

                if (set_socket_non_blocking(client_fd) < 0) { close(client_fd); continue; }
                if (set_tcp_nodelay(client_fd) < 0) { close(client_fd); continue; }

                if (nfds > MAX_CLIENTS) {
                    fprintf(stderr, "MAX_CLIENTS (%d) reached, rejecting fd=%d\n", MAX_CLIENTS, client_fd);
                    close(client_fd);
                    continue;
                }

                client_state *state = create_client_state(client_fd);
                if (!state) { close(client_fd); continue; }

                fds[nfds].fd = client_fd;
                fds[nfds].events = POLLIN;
                fds[nfds].revents = 0;
                clients[nfds] = state;
                nfds++;

                printf("New client connected with client_fd = %d (active=%d)\n", client_fd, nfds - 1);
            }
        }

        // ---- O(N) ITERATION BOTTLENECK ----
        // epoll_wait only returns the subset of fds that are actually ready.
        // poll() gives no such shortcut: we must walk every single slot in
        // fds[1..nfds) on *every* loop iteration to discover which ones have
        // revents != 0, even when only one of thousands changed state. This
        // linear scan (plus the copy above) is exactly the O(N) cost your
        // benchmarking report should show degrading as connection count grows,
        // versus epoll's O(1)-per-ready-fd behaviour.
        for (int i = 1; i < nfds; i++) {
            if (fds[i].revents == 0) continue; // we still visited this slot — O(N) either way

            client_state *state = clients[i];
            int should_close = 0;

            if (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                should_close = 1;
            } else {
                if (fds[i].revents & POLLOUT) {
                    if (flush_outbound_buffer(fds, i, state) < 0) should_close = 1;
                }
                if (!should_close && (fds[i].revents & POLLIN)) {
                    char buffer[BUFFER_SIZE];
                    ssize_t data = recv(state->fd, buffer, sizeof(buffer), 0);

                    if (data < 0) {
                        if (!(errno == EAGAIN || errno == EWOULDBLOCK)) {
                            perror("recv failed");
                            should_close = 1;
                        }
                        // else: kernel buffer empty, nothing to do this round
                    } else if (data == 0) {
                        printf("Client disconnected (client_fd = %d)\n", state->fd);
                        should_close = 1;
                    } else {
                        if (state->out_len + (size_t)data <= OUT_BUF_CAP) {
                            memcpy(state->out_buf + state->out_len, buffer, data);
                            state->out_len += data;
                        } else {
                            fprintf(stderr, "Outbound buffer overflow on client_fd %d\n", state->fd);
                            should_close = 1;
                        }
                        if (!should_close && flush_outbound_buffer(fds, i, state) < 0) {
                            should_close = 1;
                        }
                    }
                }
            }

            if (should_close) {
                // ---- O(1) COMPACTION ----
                // Never shift the tail of fds[]/clients[] left here (that's
                // O(N) per disconnect, and with churny load generators like
                // tcpkali opening/closing thousands of short-lived connections,
                // it would dominate wall-clock time and pollute your
                // measurements). Instead: swap the last active slot into
                // this one, shrink nfds, and step i back so the loop's i++
                // re-examines this index next — it now holds different data.
                destroy_client_state(state);
                int last = nfds - 1;
                if (i != last) {
                    fds[i] = fds[last];
                    clients[i] = clients[last];
                }
                nfds--;
                i--; // re-check this index on next iteration
            }
        }
    }

    for (int i = 1; i < nfds; i++) destroy_client_state(clients[i]);
    free(fds);
    free(clients);
    close(server_socket);
    return 0;
}
