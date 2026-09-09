#include "network_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <errno.h>
#include <signal.h>

#define MAX_CLIENTS 10000
#define OUT_BUF_CAP 65536

// Custom fd_set definition to safely bypass glibc's hardcoded FD_SETSIZE=1024 limit and FORTIFY_SOURCE
#define MY_FD_SETSIZE MAX_CLIENTS
typedef struct {
    long int fds_bits[MY_FD_SETSIZE / (8 * sizeof(long int)) + 1];
} my_fd_set;

#define MY_FD_SET(fd, set) ((set)->fds_bits[(fd) / (8 * sizeof(long))] |= (1UL << ((fd) % (8 * sizeof(long)))))
#define MY_FD_ISSET(fd, set) (((set)->fds_bits[(fd) / (8 * sizeof(long))] & (1UL << ((fd) % (8 * sizeof(long))))) != 0)
#define MY_FD_ZERO(set) memset((set), 0, sizeof(*(set)))

typedef struct {
    int fd;
    char out_buf[OUT_BUF_CAP];
    size_t out_len;
} client_state;

client_state* clients[MAX_CLIENTS];
int num_clients = 0;

client_state* create_client_state(int fd) {
    client_state* state = malloc(sizeof(client_state));
    if (!state) return NULL;
    state->fd = fd;
    state->out_len = 0;
    return state;
}

int flush_outbound_buffer(client_state *state) {
    while (state->out_len > 0) {
        ssize_t sent = send(state->fd, state->out_buf, state->out_len, 0);

        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            perror("send failed");
            return -1;
        }
        if (sent > 0) {
            memmove(state->out_buf, state->out_buf + sent, state->out_len - sent);
            state->out_len -= sent;
        }
    }
    return 0;
}

int main(int argc, char *argv[]) {
    signal(SIGPIPE, SIG_IGN);

    int port = (argc > 1) ? atoi(argv[1]) : 8080;
    int backlog = (argc > 2) ? atoi(argv[2]) : SOMAXCONN;
    int listen_fd = create_server_socket(port, backlog);
    if (listen_fd < 0){
        fprintf(stderr, "Failed to create server socket\n");
        return 1;
    }

    printf("server_select listening on port %d (fd=%d, max_clients=%d)\n", port, listen_fd, MAX_CLIENTS);

    while(1){
        my_fd_set readfds;
        my_fd_set writefds;
        MY_FD_ZERO(&readfds);
        MY_FD_ZERO(&writefds);
        MY_FD_SET(listen_fd, &readfds);
        int max_fd = listen_fd;

        for(int i=0; i<num_clients; i++){
            MY_FD_SET(clients[i]->fd, &readfds);
            if (clients[i]->out_len > 0) {
                MY_FD_SET(clients[i]->fd, &writefds);
            }
            if (clients[i]->fd > max_fd) {
                max_fd = clients[i]->fd;
            }
        }

        int activity = select(max_fd + 1, (fd_set*)&readfds, (fd_set*)&writefds, NULL, NULL);
        if (activity < 0){
            if (errno == EINTR) continue;
            perror("select error");
            break;
        }

        if (MY_FD_ISSET(listen_fd, &readfds)){
            while(1){
                int new_fd = accept(listen_fd, NULL, NULL);
                if (new_fd < 0){
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    }
                    perror("accept failed");
                    break;
                } else if (new_fd >= MY_FD_SETSIZE) {
                    printf("Rejecting fd=%d, exceeds MY_FD_SETSIZE\n", new_fd);
                    close(new_fd);
                    continue;
                } else if (num_clients >= MAX_CLIENTS) {
                    printf("Too many clients, rejecting fd=%d\n", new_fd);
                    close(new_fd);
                } else {
                    client_state* state = create_client_state(new_fd);
                    if (!state) {
                        close(new_fd);
                        continue;
                    }
                    clients[num_clients] = state;
                    num_clients++;
                    set_socket_non_blocking(new_fd);
                    set_tcp_nodelay(new_fd);
                    printf("New client connected: fd=%d (slot %d)\n", new_fd, num_clients - 1);
                }
            }
        }

        for(int i = 0; i<num_clients; i++){
            int should_close = 0;
            client_state *state = clients[i];

            if (MY_FD_ISSET(state->fd, &writefds)) {
                if (flush_outbound_buffer(state) < 0) {
                    should_close = 1;
                }
            }

            if (!should_close && MY_FD_ISSET(state->fd, &readfds)){
                char buf[BUFFER_SIZE];
                int n = recv(state->fd, buf, BUFFER_SIZE, 0);

                if (n>0){
                    if (state->out_len + n <= OUT_BUF_CAP) {
                        memcpy(state->out_buf + state->out_len, buf, n);
                        state->out_len += n;
                    } else {
                        fprintf(stderr, "Outbound buffer overflow on client_fd %d\n", state->fd);
                        should_close = 1;
                    }
                    if (!should_close && flush_outbound_buffer(state) < 0) {
                        should_close = 1;
                    }
                } else if (n<0){
                    if (!(errno == EAGAIN || errno == EWOULDBLOCK)) {
                        perror("read error");
                        should_close = 1;
                    }
                } else {
                    printf("Client fd=%d disconnected\n", state->fd);
                    should_close = 1;
                }
            }

            if (should_close) {
                close(state->fd);
                free(state);
                clients[i] = clients[num_clients - 1];
                num_clients--;
                i--; 
            }
        }
    }

    for (int i = 0; i < num_clients; i++) {
        close(clients[i]->fd);
        free(clients[i]);
    }
    close(listen_fd);
    return 0;
}
