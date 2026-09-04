# Network Engine Codebase

This directory contains the C implementations for all four Linux network engines, shared utility headers, and build scripts.

## Directory Layout
* `network_utils.h` — Shared socket creation and non-blocking helpers (headers file).
* `network_utils.c` — Shared socket creation and non-blocking helpers (implementation file).
* `test_utils.c` — Test File to check network_utils
* `server_select.c` — Engine #1 (`select`)
* `server_poll.c`   — Engine #2 (`poll`)
* `server_epoll.c`  — Engine #3 (`epoll`)
* `server_iouring.c` — Engine #4 (`io_uring`)

## To check the network_utils:

1) Create Object File:
`gcc -Wall -Wextra -c network_utils.c -o network_utils.o`

2) Link the Test file with object file:
`gcc -Wall -Wextra test_utils.c network_utils.o -o test_runner`

3) Run the File:
`./test_runner`
