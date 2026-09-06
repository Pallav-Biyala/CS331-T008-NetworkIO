#include "network_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>

// ---------------------------------------------------------------------------
// DECISION: MAX_CLIENTS strategy
// ---------------------------------------------------------------------------
// select() has a hard structural ceiling around FD_SETSIZE (1024 on glibc)
// that cannot be raised without recompiling glibc — it's not a tunable
// constant. So instead of picking one fixed client count, the array here is
// sized for the C10K case (10000), and the ACTUAL load per benchmark run is
// controlled externally (by how many connections tcpkali/wrk opens), not by
// this constant. This lets the same binary support two benchmark phases:
//   1. Fair 3-way comparison: run select/poll/epoll all at <=1000 connections
//      (select's real limit), same conditions across all three.
//   2. C10K demonstration: run poll/epoll/io_uring alone up to 10000 to show
//      where select structurally falls out of contention.
// This constant is just headroom for the array; it never forces a load.
#define MAX_CLIENTS 10000

int main(int argc, char *argv[]) {
    // DECISION: CLI port argument, with a default so `./server_poll` alone
    // still works. Mirrored into server_select.c the same way, so both
    // support `./server_select 8080` / `./server_poll 9000` for running
    // multiple engines side by side during benchmarking, per the original
    // team goal.
    int port = 8080;
    if (argc >= 2) {
        port = atoi(argv[1]);
        if (port <= 0 || port > 65535) {
            fprintf(stderr, "Invalid port: %s\n", argv[1]);
            return 1;
        }
    }

    int listen_fd = create_server_socket(port, 128); // backlog=128, matches server_select.c
    if (listen_fd < 0) {
        fprintf(stderr, "Failed to create server socket\n");
        return 1;
    }
    printf("poll_server listening on port %d (fd=%d)\n", port, listen_fd);
    // Note: create_server_socket() makes listen_fd non-blocking internally.
    // Same is true for server_select.c — just less visible there since it's
    // hidden inside the shared utility rather than called out locally.

    struct pollfd fds[MAX_CLIENTS + 1]; // index 0 = listener, 1..MAX_CLIENTS = clients
    for (int i = 0; i < MAX_CLIENTS + 1; i++) {
        fds[i].fd = -1;
        fds[i].events = POLLIN;
    }
    fds[0].fd = listen_fd;
    int nfds = 1; // one past the highest occupied slot; kept tight via compaction below

    while (1) {
        int activity = poll(fds, nfds, -1); // block indefinitely, matches select's NULL timeout
        if (activity < 0) {
            perror("poll error");
            continue;
        }

        // -------------------------------------------------------------
        // Listening socket
        // -------------------------------------------------------------
        // DECISION: drain-loop accept(), mirrored into server_select.c too
        // (see patch note below / select's file). Without this, a burst of
        // simultaneous connections needs one full poll()/select() round
        // trip per connection just to empty the accept queue — an
        // unrelated bottleneck that has nothing to do with the mechanism
        // being tested. Draining fully on both sides removes that variable.
        if (fds[0].revents & POLLIN) {
            while (1) {
                int new_fd = accept(listen_fd, NULL, NULL);
                if (new_fd < 0) {
                    // EAGAIN/EWOULDBLOCK means queue empty; any other errno
                    // is a genuine error but doesn't change the loop exit
                    // here since listen_fd is non-blocking either way.
                    break;
                }

                int added = 0;
                for (int i = 1; i < MAX_CLIENTS + 1; i++) {
                    if (fds[i].fd == -1) {
                        fds[i].fd = new_fd;
                        fds[i].events = POLLIN;
                        if (i >= nfds) nfds = i + 1;
                        added = 1;
                        printf("New client connected: fd=%d (slot %d)\n", new_fd, i);
                        break;
                    }
                }
                if (!added) {
                    printf("Too many clients, rejecting fd=%d\n", new_fd);
                    close(new_fd);
                }
            }
        }

        // -------------------------------------------------------------
        // Client sockets
        // -------------------------------------------------------------
        for (int i = 1; i < nfds; i++) {
            if (fds[i].fd == -1) continue;
            if (fds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
                char buf[BUFFER_SIZE];
                int n = read(fds[i].fd, buf, BUFFER_SIZE);
                if (n <= 0) {
                    printf("Client fd=%d disconnected\n", fds[i].fd);
                    close(fds[i].fd);

                    // FIX (was a real bug, not a design choice): compact the
                    // array by swapping the last active slot into this one,
                    // instead of just marking fds[i].fd = -1 and leaving
                    // nfds unchanged. Without this, nfds only ever grows,
                    // so both the kernel and this loop keep scanning dead
                    // -1 slots forever — artificially inflating poll's
                    // measured O(N) cost with N counting long-gone clients.
                    fds[i] = fds[nfds - 1];
                    fds[nfds - 1].fd = -1;
                    nfds--;
                    i--; // re-check this index next iteration; it now holds
                         // the slot we just swapped in
                } else {
                    // No partial-write loop: matches server_select.c exactly.
                    // Under heavy load (e.g. tcpkali saturating the socket
                    // send buffer), write() can return fewer bytes than n or
                    // fail with EAGAIN, silently dropping data. This is a
                    // shared, already-flagged tradeoff (data integrity vs.
                    // simplicity) present identically in both files — no
                    // action needed unless the team decides both should add
                    // proper buffering, in which case it must be added to
                    // both at once.
                    write(fds[i].fd, buf, n);
                }
            }
        }
    }

    close(listen_fd);
    return 0;
}
