# Project Title: Scalable Network I/O: From select to io_uring
---
* **Course:** CS 331 - Computer Networks (2026-27 Sem 1)
* **Team ID:** T008
* **Project ID:** 13
---
## Team Members
| Name | Roll Number | Role |
| :--- | :--- | :--- |
| Chitiveli Hemcharan Varma | 2411095 | Benchmarking |
| Gella Naga Sai Krishna | 24110122 | io_uring |
| Guransh Kaur Saran | 24110127 | select |
| Kowel P Laloo | 24110168 | poll |
| Pallav Biyala | 24110234 | epoll |

---

## Project Overview

This project investigates Linux network I/O mechanisms by implementing and evaluating server applications using four backends:

* **select**
* **poll**
* **epoll**
* **io_uring**

The repository includes a shared networking core, an echo server, a file server, and tools for correctness testing and performance experiments.

## Prerequisites

* Linux or WSL with a suitable GCC toolchain
* GNU Make
* Python 3
* `pkg-config`
* liburing development files for the io_uring backend

On Ubuntu or Debian, the relevant dependencies can be installed with:

```bash
sudo apt update
sudo apt install build-essential pkg-config liburing-dev python3
```

## Building

From the repository root, build all eight server variants:

```bash
make
```

Build only the echo-server variants:

```bash
make echo
```

Build only the file-server variants:

```bash
make filesrv
```

Generated executables are placed in `build/`.

## File Server

The file server provides a minimal HTTP/1.1 GET interface for the test files in the repository's `files/` directory.

Run the server from the repository root so it can find that directory.

Start one backend at a time, for example:

```bash
./build/filesrv-select --port 8081
```

The other variants are:

```bash
./build/filesrv-poll --port 8081
./build/filesrv-epoll --port 8081
./build/filesrv-uring --port 8081
```

Stop the running server with `Ctrl+C` before starting another backend on the same port.

For a basic manual test, request a fixture from another terminal:

```bash
curl -i http://127.0.0.1:8081/hello.txt
```

The server should return an HTTP response with the file contents and the appropriate content length.

## Automated File-Server Tests

Run the complete file-server correctness suite:

```bash
make test-filesrv
```

The target builds the file-server variants if necessary and runs `code/tests/filesrv_gate.py` against all four backends.

The suite checks:

1. Valid and empty file responses, including `Content-Length`
2. Missing files and HTTP 404 responses
3. Unsupported HTTP methods
4. Path traversal attempts
5. Malformed requests
6. Incomplete requests
7. Persistent connections
8. A concurrent workload of 1,000 requests per backend
9. Server survival and successful service after the other checks

### Verified Correctness Results

In the recorded test run, all four backends passed all nine checks:

| Backend   | Checks passed |
| --------- | ------------: |
| select    |           9/9 |
| poll      |           9/9 |
| epoll     |           9/9 |
| io_uring  |           9/9 |
| **Total** |     **36/36** |

The concurrency workload passed for all four backends, accounting for 4,000 successful requests across the four test runs.

These results describe the recorded correctness tests. They are not performance measurements and do not establish that one backend is faster than another.

## Benchmarking

Performance experiments will compare the I/O mechanisms under controlled, reproducible workloads. Comparisons should keep the workload and relevant server behavior consistent while changing the I/O mechanism being evaluated.

Benchmark results will be documented after the experiments have been run and analyzed.
