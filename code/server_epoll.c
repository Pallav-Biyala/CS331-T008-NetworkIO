#include "network_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/epoll.h> // this calls epoll_create1()
#include <unistd.h> // this calls close()
#include <errno.h> // Required for errno and EINTR

#define MAX_EVENTS 100
#define OUT_BUF_CAP 65536 // 64KB output buffer
// --------------------------------------------------------------------------
//                         CLIENT STATE 
// -------------------------------------------------------------------------
// So now, the only problem left is that if we recieved say 2KB of data, and now we want to send all 2KB of data. but say sending 1.2KB, kernel Buffer filled and then it would hit EAGAIN and this return -1. This means we sent only 1.2KB of data and rest 0.8KB we lost. 

// So to solve this we now have a client state where we store the data and track how much we sent. By this way, even if it hits EAGAIN, we wont lose data as now we have stored it.

typedef struct {
    int fd;
    char out_buf[OUT_BUF_CAP];
    size_t out_len; // How many unsent bytes are currently stored in out_buf
    int writing; // State transition flag: 1 if interested in EPOLLOUT, 0 otherwise
} client_state;

// Allocate memory for a new client's state
client_state* create_client_state(int fd) {
    client_state* state = malloc(sizeof(client_state));
    if (!state) return NULL;
    state->fd = fd;
    state->out_len = 0;
    state->writing = 0;
    return state;
}

// So we sent all data of state --> destroyt it
void destroy_client_state(client_state* state) {
    if (!state) return;
    close(state->fd); // Closing fd automatically removes it from epoll!
    free(state);
}



// --------------------------------------------------------------------------
//                         HELPER FUNCTION
// -------------------------------------------------------------------------
// Creating function to add Socket to the epoll
int register_socket_epoll(int epoll_fd, int server_socket) {

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN; // This means we are telling kernel to wake us up when someone tries to connect to server socket
    ev.data.ptr = NULL; // this tell we are this and hence NULL means server socket.

    // so we defined eveything. now we connect using ctl
    // syntax is: whcih epoll, what operation, which socket, what instructions
    // we used EPOLL_CTL_ADD because we are adding server socket to the epoll
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_socket, &ev) == -1) {
        return -1;
    }
    return 0;
}

// So this function will help us to push the remaining bytes to the socket
// 0 on success and -1 on failure
int flush_outbound_buffer(int epoll_fd, client_state *state){
    while (state->out_len > 0){
        ssize_t sent = send(state->fd, state->out_buf, state->out_len, 0);

        if (sent < 0){
            if (errno == EAGAIN || errno == EWOULDBLOCK){
                break;
            }
            if (errno == EINTR){
                // system call interupted by singal, retry send
                continue; 
            }
            perror("send failed");
            return -1;
        }
        if (sent > 0){
            // Shift remaining unsent bytes to the beginning of out_buf
            memmove(state->out_buf, state->out_buf + sent, state->out_len - sent);
            state->out_len -= sent;
        }
    }

    // Update epoll event interest based on buffer state
    // so now Only trigger epoll_ctl(MOD) when the interest state actually transitions
    if (state->out_len>0 && !state->writing){
        struct epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.data.ptr = state;
        // Still have unsent data: listen for BOTH read and write readiness
        ev.events = EPOLLIN | EPOLLOUT;

        if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, state->fd, &ev) == -1) {
            perror("epoll_ctl: MOD failed in flush");
            return -1;
        }

        state->writing = 1;
    }
    else if (state->out_len == 0 && state->writing){
        struct epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.data.ptr = state;
        // Buffer completely drained: listen for read events ONLY
        ev.events = EPOLLIN;

        if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, state->fd, &ev) == -1) {
            perror("epoll_ctl: MOD failed in flush");
            return -1;
        }

        state->writing = 0;
    }
    return 0;
}



// --------------------------------------------------------------------------
//                          MAIN FUNCTION
// -------------------------------------------------------------------------
int main(int argc, char* argv[]) {
        
    // ------------------------------------------------------------------------
    //                         BUILDING THE SOCKET & EPOLL
    // -------------------------------------------------------------------------

    // so this argc and argv will allow us to take inputs from the terminal Linux.
    int port = (argc > 1) ? atoi(argv[1]) : 8080;
    int backlog = (argc > 2) ? atoi(argv[2]) : SOMAXCONN;

    // Let's add the socket
    int server_socket = create_server_socket(port, backlog);
    if (server_socket == -1) {
        perror("server socket creation failed");
        return 1;
    }
    // So firstly lets create a epoll instance
    int epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        perror("epoll_create1 failed");
        close(server_socket);
        return 1;
    }

    
    // ------------------------------------------------------------------------
    //                         CONNECTING SOCKET & EPOLL
    // -------------------------------------------------------------------------
    
    // So we successfully created server socket and kernel's epoll. now we want to connect both of them, i.e., take the socket and attach it to kernel's epoll so that socket can tell kernel what all pids its interested in and then as they come, it alerts server
    if (register_socket_epoll(epoll_fd, server_socket) < 0) {
        perror("epoll_ctl: server_socket failed");
        close(server_socket);
        close(epoll_fd);
        return 1;
    }

    // So by this way we successfully linked Kernel epoll and socket. So whenever request comes, epoll would tell socket, someone came. Now we need to tell kernel to wait forever and then tell us if someone comes. So for that we have epoll_wait()

    // so this will store all the fds kernerl can return at a time
    struct epoll_event events[MAX_EVENTS];
    printf("epoll server listening on port %d with backlog %d (server_fd=%d, epoll_fd=%d)\n", port, backlog, server_socket, epoll_fd);


    // ------------------------------------------------------------------------
    //                         RUNNING THE SERVER
    // -------------------------------------------------------------------------
    while (1) {
        // for debugging
        // printf("Sleeping for 3 seconds... send data from BOTH terminals NOW!\n");
        // sleep(3); // Pauses server so events accumulate in kernel buffer


        // so now server runs forever. And now we tell epoll to wait until someone tries to connect
        // syntax --> which epoll, where to store results, max events, how much to wait (-1 means forever)
        int n = epoll_wait(epoll_fd,events,MAX_EVENTS,-1);
        if (n == -1) {
            if (errno == EINTR) continue; // Retry if interrupted by OS signal
            perror("epoll_wait failed");
            break;
        }

        // printf("Events triggered count n = %d\n", n);
        // epoll_wait returns number of fds who are ready to either connect or send request.
        for (int i = 0; i < n; ++i) {
            // int current_fd = events[i].data.fd;
            // printf("Processing current_fd = %d\n", current_fd);

            
            // -------------------------------------------------------
            //                      CLIENT CONNECT
            // -------------------------------------------------------

            // So if its the first time a new client approaches, server needs to establish connection with it and hence for that epoll adds the fd of server itself in events so that it knows it has got an connection request and has to connect
            if (events[i].data.ptr == NULL) {
                while (1) {
                    int client_fd = accept(server_socket, NULL, NULL);
                    if (client_fd < 0) {
                        // Queue is completely drained—normal non-blocking behavior
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        perror("accept failed");
                        break;
                    }

                    // Setting accepted client socket to non-blocking
                    if (set_socket_non_blocking(client_fd) < 0) {
                        close(client_fd);
                        continue;
                    }

                    set_tcp_nodelay(client_fd);  // Disable Nagle's algorithm

                    // creating client state
                    client_state* state = create_client_state(client_fd);
                    if (!state) {
                        close(client_fd);
                        continue;
                    }

                    // now we register client_fd with epoll so that if it ready, we know
                    struct epoll_event client_ev;
                    memset(&client_ev, 0, sizeof(client_ev));
                    client_ev.events = EPOLLIN;
                    client_ev.data.ptr = state;

                    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &client_ev) == -1) {
                        perror("epoll_ctl: client_fd failed");
                        destroy_client_state(state);
                        continue;
                    }
                    else {
                        printf("New client connected with client_fd = %d\n", client_fd);
                    }
                }
            }
            // --------------------------------------------------------------
            //                          CLIENT EVENT
            // ---------------------------------------------------------------
            else {
                
                // now this means client_fd is ready. So now we can recieve request
                client_state* state = (client_state*)events[i].data.ptr;
                // We now access socket fd via state->fd
                // printf("Event triggered for client_fd = %d\n", state->fd);
                uint32_t revents = events[i].events;
                
                // This handles rest errors that kernel might send
                if (revents & (EPOLLERR | EPOLLHUP)) {
                    destroy_client_state(state);
                    continue;
                }
                // So firstly we need to check that do we have something that couldnt be sent due to kernel buffer being filled.
                // Handle Writable Event (EPOLLOUT)
                if (revents & EPOLLOUT){
                    if (flush_outbound_buffer(epoll_fd, state) < 0){
                        destroy_client_state(state);
                        continue;
                    }
                }

                // Now we sent all the data (all 0.8KB left sent) and hence now we can recieve new data and send it
                // Handle Readable Event (EPOLLIN)
                if (revents & EPOLLIN){
                    char buffer[BUFFER_SIZE];
                    ssize_t data = recv(state->fd, buffer, sizeof(buffer), 0);

                    if (data < 0) {
                        // Kernel buffer is completely empty
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            continue; // no data left to read
                        }
                        perror("recv failed");
                        destroy_client_state(state);
                        continue;
                    }
                    else if (data == 0) {
                        // so client is done with request and hence now we can remove him 
                        printf("Client disconnected (client_fd = %d)\n", state->fd);
                        destroy_client_state(state);
                        continue;
                    }
                    else {
                        // so now we are recieving data. so we recieve and then we send
                        // Buffer incoming data into out_buf
                        if (state->out_len + data <= OUT_BUF_CAP) {
                            memcpy(state->out_buf + state->out_len, buffer, data);
                            state->out_len += data;
                        } 
                        else {
                            fprintf(stderr, "Outbound buffer overflow on client_fd %d\n", state->fd);
                            destroy_client_state(state);
                            continue;
                        }

                        // Attempt to send/flush buffered data immediately
                        if (flush_outbound_buffer(epoll_fd, state) < 0) {
                            destroy_client_state(state);
                            continue;
                        }
                    }
                }
            }
        }
    }

    close(server_socket);
    close(epoll_fd);
    return 0;
}
