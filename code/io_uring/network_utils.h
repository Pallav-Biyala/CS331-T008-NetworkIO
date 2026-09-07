// These lines make sure this header's contents are processed only once.
// This is called an include guard.
#ifndef NETWORK_UTILS_H
#define NETWORK_UTILS_H

#define BUFFER_SIZE 4096

// Now we just declare the functions
int set_socket_non_blocking(int server_socket);
int set_tcp_nodelay(int fd);
int create_server_socket(int port, int backlog);

#endif // NETWORK_UTILS_H
