# Scalable Network I/O: From select to io_uring

**Course:** CS 331 – Computer Networks (2026–27 Sem 1)
**Team:** T008
**Project ID:** 13

## Team

| Name | Roll Number | Role |
| :--- | ---: | :--- |
| Chitiveli Hemcharan Varma | 2411095 | Benchmarking |
| Gella Naga Sai Krishna | 24110122 | io_uring |
| Guransh Kaur Saran | 24110127 | select |
| Kowel P Laloo | 24110168 | poll |
| Pallav Biyala | 24110234 | epoll |

## Project layout

```text
code/
  apps/Echo-Server/                 Echo server
  apps/File-Transfer-Server/        HTTP file server and its fixtures
  core/                             Shared connection and statistics code
  io/                               select, poll, epoll, and io_uring backends
  tests/File-Transfer-Server/       File server correctness and benchmark checks
  tools/File-Transfer-Server/       Experiment runner, configs, and helpers
  results/File Transfer Server/     Archived campaign, pilot, and smoke results
build/                              Local compiled executables (generated)
results/                            New local experiment output (generated)
```

Run build, test, and benchmark commands from the repository root. The root
`Makefile` includes the project targets in `code/Makefile`.

## Requirements

For building and correctness checks:

- Linux or WSL with GCC and GNU Make
- Python 3
- `pkg-config` and liburing development files

For benchmark campaigns, also install `wrk` and `curl`.

On Ubuntu or Debian:

```bash
sudo apt update
sudo apt install build-essential pkg-config liburing-dev python3 curl wrk
```

## Build

From the repository root, build all eight variants:

```bash
make
```

Or build one application family:

```bash
make echo
make filesrv
```

Executables are generated in `build/`.

## File server

The HTTP/1.1 file server serves files from
`code/apps/File-Transfer-Server/files/`. Start it from the repository root,
for example:

```bash
./build/filesrv-select --port 8081
```

Other variants are `filesrv-poll`, `filesrv-epoll`, and `filesrv-uring`.
Check a response from another terminal:

```bash
curl -i http://127.0.0.1:8081/hello.txt
```

## Tests

Run the complete automated suite:

```bash
make test
```

The focused targets are:

```bash
make test-filesrv
make test-filesrv-bench
make test-runner-acceptance
```

`test-filesrv` checks HTTP correctness, malformed requests, persistent
connections, concurrency, and server survival across all four backends.
`test-filesrv-bench` verifies fixture bytes and response lengths. The runner
test covers connection-acceptance validation.

The recorded correctness run passed 9/9 checks per backend (36/36 total),
including 4,000 successful requests in the concurrency checks. These are
correctness results, not performance comparisons.

## Experiments

Generate all deterministic file fixtures, then run a configuration from the
repository root:

```bash
make bench-fixtures
python3 code/tools/File-Transfer-Server/run_experiment \
  code/tools/File-Transfer-Server/configs/filesrv-e5a-size-concurrency.json
```

Experiment output defaults to the local, ignored `results/` directory. The
campaign archives currently in the repository are under
`code/results/File Transfer Server/`. Keep accepted and rejected raw runs
together when reporting results; document rejected runs rather than counting
them as accepted measurements.

The E5a, E5b, E7, E8, and E9 campaigns have been run. E8 currently has 57
accepted runs and 3 rejected runs; those three should remain identified as
rejected in any analysis. Graphs and the final interpretation are still to be
prepared.
