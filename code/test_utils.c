#include "network_utils.h"
#include <stdio.h>
#include <unistd.h>

int main(void) {
    int port = 8080;
    
    printf("Testing server socket creation on port %d...\n", port);
    
    int listen_fd = create_server_socket(port);
    if (listen_fd < 0) {
        fprintf(stderr, "TEST FAILED: Could not create server socket.\n");
        return 1;
    }

    printf("TEST PASSED: Server socket created successfully with FD = %d\n", listen_fd);

    // Clean up
    close(listen_fd);
    printf("Closed socket FD %d. Test complete!\n", listen_fd);

    return 0;
}