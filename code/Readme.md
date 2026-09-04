# Network Engine Codebase

This directory contains the C implementations for all four Linux network engines, shared utility headers, and build scripts.

## Directory Layout
* `network_utils.hpp` — Shared socket creation and non-blocking helpers.
* `server_select.cpp` — Engine #1 (`select`)
* `server_poll.cpp`   — Engine #2 (`poll`)
* `server_epoll.cpp`  — Engine #3 (`epoll`)
* `server_iouring.cpp` — Engine #4 (`io_uring`)
