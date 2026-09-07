#include "network_utils.h" // this imports our header file
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>  // for close()
#include <sys/socket.h> // for socket(), bind() listen()
#include <netinet/in.h> // for struct sockaddr_in and htons()
#include <fcntl.h> // for fcntl() non blocking
#include <netinet/tcp.h> // Required for TCP_NODELAY and IPPROTO_TCP

// So to create our server non blocking, we perform the following operation
int set_socket_non_blocking(int server_socket){
    // so firstly lets get th current flags of the socket using fcntl() and F_GETFL
    int flags = fcntl(server_socket, F_GETFL, 0);

    // Now we set its flag to non blocking using fcntl(), F_SETFL and O_NONBLOCK
    if (flags == -1) {
        perror("fcntl(F_GETFL) failed");
        return -1;
    }
    if (fcntl(server_socket, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl(F_SETFL) failed");
        return -1;
    }
    return 0;
}


// Disable Nagle's algorithm for minimum latency
int set_tcp_nodelay(int fd) {
    int flag = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int)) < 0) {
        perror("setsockopt(TCP_NODELAY) failed");
        return -1;
    }
    return 0;
}

// So firstly let's try to create our own socket
int create_server_socket(int port, int backlog) {
    // So to setup the server, we need to create a socket.
    // Now, since we are creating a TCP Server, we will use SOCK_Stream and 0 means by default it can go to the protocol depending on the type we gave. AF_INET means we would use IPv4 Addressing
    int server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        perror("Socket Creation Failed");
        return -1;
    }

    // Now we will many times, run the server, benchmark and change something, run again. This may sometimes result in Bind failed: Address in use. So to avoid this, we use SO_REUSEADDR option. This allows us to reuse the address and port combination without waiting for the OS to clean it up.
    int opt = 1;
    if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt(SO_REUSEADDR) failed");
        close(server_socket);
        return -1;
    }


    // So now to use bind, in C for IPv4 we need to use the struct sockaddr_in which has Address family, Port and IP Address.
    struct sockaddr_in server_address;

    // so lets clean all the memory of the struct sockaddr_in so that we don't have any garbage values in it. 
    memset(&server_address, 0, sizeof(server_address)); // so take memory at server address, set all to 0 and upto the size of the server_address

    server_address.sin_family = AF_INET; // this defines the address family as IPv4
    server_address.sin_port = htons(port); // this defines the port number, we use htons to convert the port number to network byte order
    server_address.sin_addr.s_addr = INADDR_ANY; // this defines the IP Address and INADDR_ANY just means that server binds to all local network interfaces (IP Addresses) available on machine.

    // Now we need to bind the socket so that it get its IP Address + Port as label. so the syntax is bind(socket, struct sockaddr *address, socklen_t address_len). 
    // if bind returns -1, it failed and if 0 it successed. so we need to check that too
    if (bind(server_socket, (struct sockaddr*)&server_address, sizeof(server_address)) == -1) {
        perror("Bind Failed");
        close(server_socket);
        return -1;
    }

    // Now once this done, our server is ready to accept connections, so we can now listen to the socket and wait for connections. so its synatx is: listen(socket, backlog). if it returns -1 failed and if 0 succeeded.
    int actual_backlog = backlog > 0 ? backlog : SOMAXCONN;
    if (listen(server_socket, actual_backlog) == -1) {
        perror("Listen Failed");
        close(server_socket);
        return -1;
    }

    // now we make this listening socket non blocking
    if (set_socket_non_blocking(server_socket) == -1) {
        close(server_socket);
        return -1;
    }
    return server_socket;
}
