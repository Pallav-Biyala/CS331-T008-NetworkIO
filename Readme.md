# Scalable Network I/O: From `select` to `io_uring`

**Course:** CS 331 – Computer Networks (2026–27 Sem 1)  
**Team:** T008  
**Project ID:** 13

## Team

| Name | Roll Number | Role |
| :--- | ---: | :--- |
| Chitiveli Hemcharan Varma | 2411095 | Echo Server |
| Gella Naga Sai Krishna | 24110122 | File Transfer Server |
| Guransh Kaur Saran | 24110127 | Echo Server |
| Kowel P Laloo | 24110168 | Echo Server |
| Pallav Biyala | 24110234 | File Transfer Server |

## Project Overview

This project implements and evaluates two network server applications:

- **TCP Echo Server:** Receives client data and sends it back to the originating client.
- **HTTP File Transfer Server:** Serves files over HTTP/1.1.

Both applications support four I/O multiplexing mechanisms:

- `select`
- `poll`
- `epoll`
- `io_uring`

The project compares their implementation characteristics and performance under different workloads, connection counts, and server configurations.

## Repository Structure

```text
CS331-T008-NetworkIO/
├── code/
│   ├── apps/
│   │   ├── Echo-Server/
│   │   └── File-Transfer-Server/
│   ├── core/                         Shared connection and statistics code
│   ├── io/                           Four I/O backend implementations
│   ├── tests/                        Correctness and benchmark checks
│   ├── tools/                        Experiment runners and configurations
│   ├── results/                      Archived experiment results
│   └── benchmarking-graphs/          Benchmark analysis and figures
├── report/                           Final project report
├── presentation-slides/              Project presentation
└── AI-logs/                          AI usage disclosure
```

Build artifacts and new local experiment outputs are generated locally and are not part of the submitted source structure.

Run all build, test, and benchmark commands from the repository root. The root `Makefile` includes the project targets defined in `code/Makefile`.

## Requirements

For building and running correctness checks:

- Linux or WSL
- GCC and GNU Make
- Python 3
- `pkg-config`
- liburing development files

For benchmark campaigns, the following additional tools are required:

- `wrk`
- `curl`

On Ubuntu or Debian, install the dependencies using:

```bash
sudo apt update
sudo apt install build-essential pkg-config liburing-dev python3 curl wrk
```

## Building

Build all eight server variants:

```bash
make
```

Alternatively, build only one application family:

```bash
make echo
make filesrv
```

The compiled executables are generated in the local `build/` directory.

### Available Executables

**Echo Server**

```text
build/echo-select
build/echo-poll
build/echo-epoll
build/echo-uring
```

**File Transfer Server**

```text
build/filesrv-select
build/filesrv-poll
build/filesrv-epoll
build/filesrv-uring
```

## Running the Servers

### TCP Echo Server

For example, start the `select` implementation:

```bash
./build/echo-select --port 8080
```

The other implementations can be started by replacing `select` with `poll`, `epoll`, or `uring`.

### HTTP File Transfer Server

The HTTP file server serves files from:

```text
code/apps/File-Transfer-Server/files/
```

Start the `select` implementation:

```bash
./build/filesrv-select --port 8081
```

From another terminal, request a file:

```bash
curl -i http://127.0.0.1:8081/hello.txt
```

The other implementations are available as `filesrv-poll`, `filesrv-epoll`, and `filesrv-uring`.

## Testing

Run the automated test suite:

```bash
make test
```

The focused test targets are:

```bash
make test-filesrv
make test-filesrv-bench
make test-runner-acceptance
```

Their purposes are:

| Target | Purpose |
| :--- | :--- |
| `test-filesrv` | Checks HTTP correctness, malformed requests, persistent connections, concurrency, and server survival across all four backends. |
| `test-filesrv-bench` | Verifies benchmark fixture contents and HTTP response lengths. |
| `test-runner-acceptance` | Checks the experiment runner's connection-acceptance validation. |

The recorded File Transfer Server correctness run passed **9/9 checks per backend (36/36 total)**, including 4,000 successful requests in the concurrency checks.

These are correctness results and should not be interpreted as performance comparisons.

## Benchmarking and Experiments

Generate the deterministic benchmark fixtures:

```bash
make bench-fixtures
```

Run an experiment configuration from the repository root:

```bash
python3 code/tools/File-Transfer-Server/run_experiment \
  code/tools/File-Transfer-Server/configs/filesrv-e5a-size-concurrency.json
```

Experiment output defaults to the local, ignored `results/` directory. Archived campaign results are maintained under:

```text
code/results/File Transfer Server/
```

### Completed Experiment Campaigns

The File Transfer Server evaluation includes the following campaigns:

| Campaign | Focus |
| :--- | :--- |
| E5a | File-size and concurrency effects |
| E5b | Connection reuse and keep-alive behaviour |
| E7 | Application and socket buffer sensitivity |
| E8 | Impact of slow-reading clients |
| E9 | Effect of `TCP_NODELAY` |

The E8 campaign contains **57 accepted runs and 3 rejected runs**. Rejected runs are retained for transparency but excluded from accepted measurements. They must remain explicitly identified as rejected when interpreting or reproducing the results.

Benchmark analysis scripts and generated figures are maintained under:

```text
code/benchmarking-graphs/file-transfer/
```

The final report discusses the experimental methodology, measured results, limitations, and performance observations.

## Reproducibility

For reproducible results:

1. Build the required server variants.
2. Generate the deterministic benchmark fixtures.
3. Run the desired experiment configuration from the repository root.
4. Retain the raw experiment output.
5. Use the provided analysis scripts to process the accepted runs and generate figures.
6. Consult the project report for the experimental methodology and interpretation.

Use the same system configuration and workload parameters when comparing results across backends.

## Documentation

Additional project materials are available in the repository:

- **Final Report:** `report/`
- **Presentation:** `presentation-slides/`
- **AI Usage Disclosure:** `AI-logs/`
- **Benchmark Analysis:** `code/benchmarking-graphs/file-transfer/`

---

**CS331-T008 | Project 13 | 2026–27 Semester 1**
