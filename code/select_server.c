#include "network_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/select.h> // gives access to select and fd_set
#include <sys/socket.h> // gives access to accept

#define MAX_CLIENTS 500

int client_fds[MAX_CLIENTS];

int main(int argc, char *argv[]){
	int port = 8080;
	int listen_fd = create_server_socket(port, 128);
	if (listen_fd < 0){
		fprintf(stderr, "Failed to create server socket\n");
		return 1;
	}

	printf("select_server listening on port %d (fd=%d)\n", port, listen_fd);

	for (int i = 0; i < MAX_CLIENTS; i++) {
 		client_fds[i] = -1;  // -1 means "no client in this slot"
	}

	while(1){
		fd_set readfds;
		FD_ZERO(&readfds); // clear the set
		FD_SET(listen_fd, &readfds); // always watch the listening socket
		int max_fd = listen_fd;

		// add all active clients to the set
		for(int i=0; i<MAX_CLIENTS; i++){
			if(client_fds[i] != -1) {
				FD_SET(client_fds[i], &readfds);
				if (client_fds[i] > max_fd) {
					max_fd = client_fds[i]; // select's first arg needs highest fd number
				}
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
			int new_fd = accept(listen_fd, NULL, NULL);
			if (new_fd < 0){
				perror("accept failed");
			} else {
				int added = 0;
				for (int i=0; i < MAX_CLIENTS; i++){
					if (client_fds[i] == -1){
						client_fds[i] = new_fd;
						added = 1;
						printf("New client connected: fd=%d (slot %d)\n", new_fd, i);
						break;
					}
				}
				if (!added){
					printf("Too many clients, rejecting fd=%d\n", new_fd);
					close(new_fd);
				}
			}
		}

		for(int i = 0; i<MAX_CLIENTS; i++){
			if (client_fds[i] != -1 && FD_ISSET(client_fds[i], &readfds)){
				char buf[BUFFER_SIZE];
				int n = read(client_fds[i], buf, BUFFER_SIZE);

				if (n<=0){
					// n = 0 => client closed the connection
					// n < 0 => error occurred
					printf("Client fd=%d disconnected\n", client_fds[i]);
					close(client_fds[i]);
					client_fds[i] = -1;
				} else {
					write(client_fds[i], buf, n);
				}
			}
		}
	}

	close(listen_fd);
	return 0;
}
