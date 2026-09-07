#include "network_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/select.h> // gives access to select and fd_set
#include <sys/socket.h> // gives access to accept
#include <errno.h>

#define MAX_CLIENTS 1000

int client_fds[MAX_CLIENTS];
int num_clients = 0; // CHANGED: replaces highest_slot_used — tracks exact count of active clients, no gaps

int main(int argc, char *argv[]){
        int port = (argc > 1) ? atoi(argv[1]) : 8080;
        int backlog = (argc > 2) ? atoi(argv[2]) : 1024;
        int listen_fd = create_server_socket(port, backlog);
        if (listen_fd < 0){
                fprintf(stderr, "Failed to create server socket\n");
                return 1;
        }

        printf("select_server listening on port %d (fd=%d)\n", port, listen_fd);

        // CHANGED: no longer need to pre-fill with -1 — the list starts empty (num_clients = 0)
        // and only ever holds real, active fds, so there are no gaps to mark

        while(1){
                fd_set readfds;
                FD_ZERO(&readfds); // clear the set
                FD_SET(listen_fd, &readfds); // always watch the listening socket
                int max_fd = listen_fd;

                // add all active clients to the set
                // CHANGED: loop now goes 0 to num_clients (exclusive) — every entry here is guaranteed
                // active, so the "!= -1" check is no longer needed
                for(int i=0; i<num_clients; i++){
                        FD_SET(client_fds[i], &readfds);
                        if (client_fds[i] > max_fd) {
                                max_fd = client_fds[i]; // select's first arg needs highest fd number
                        }
                }

                // block here until somethinfg ready
                int activity = select(max_fd + 1, &readfds, NULL, NULL, NULL);
                if (activity < 0){
                        perror("select error");
                        continue;
                }

                // accept block
                if (FD_ISSET(listen_fd, &readfds)){
                        while(1){
                                int new_fd = accept(listen_fd, NULL, NULL);
                                if (new_fd < 0){
                                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                                break; // no more pending connections, stop draining
                                        }
                                        perror("accept failed");
                                        break;
                                } else if (new_fd >= FD_SETSIZE) {
                                        printf("Rejecting fd=%d, exceeds FD_SETSIZE\n", new_fd);
                                        close(new_fd);
                                        continue; // keep draining even after rejection
                                } else if (num_clients >= MAX_CLIENTS) {
                                        // ADDED: list is full, no slots to search for anymore — reject directly
                                        printf("Too many clients, rejecting fd=%d\n", new_fd);
                                        close(new_fd);
                                } else {
                                        // CHANGED: no more searching for an empty slot — just append at the end
                                        client_fds[num_clients] = new_fd;
                                        num_clients++;
                                        set_socket_non_blocking(new_fd); // make client socket non blocking
					set_tcp_nodelay(new_fd); // disable Nagle's algo for low latency
                                        printf("New client connected: fd=%d (slot %d)\n", new_fd, num_clients - 1);
                                }
                        }
                }

                // CHANGED: loop now goes 0 to num_clients (exclusive), every entry guaranteed active
                for(int i = 0; i<num_clients; i++){
                        if (FD_ISSET(client_fds[i], &readfds)){
                                char buf[BUFFER_SIZE];
                                int n = read(client_fds[i], buf, BUFFER_SIZE);

                                if (n>0){
                                        // handle data
                                        write(client_fds[i], buf, n);
                                } else if (n<0){
                                        // n < 0 => error occurred
                                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                                // no data available right now, not an error - do nothing, continue
                                        } else {
                                                // genuine error
                                                perror("read error");
                                                close(client_fds[i]);
                                                // ADDED: swap-with-last removal — move the last client into this
                                                // now-empty slot, then shrink the list. Keeps the list gap-free
                                                // without needing to shift every element after it.
                                                client_fds[i] = client_fds[num_clients - 1];
                                                num_clients--;
                                                i--; // ADDED: re-check this same index next iteration, since it
                                                     // now holds a different (unprocessed) client after the swap
                                        }
                                } else {
                                        // n = 0 => client closed the connection
                                        printf("Client fd=%d disconnected\n", client_fds[i]);
                                        close(client_fds[i]);
                                        // ADDED: same swap-with-last removal as above
                                        client_fds[i] = client_fds[num_clients - 1];
                                        num_clients--;
                                        i--; // ADDED: re-check this index since it now holds a swapped-in client
                                }
                        }
                }
        }

        close(listen_fd);
        return 0;
}
