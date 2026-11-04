CC       := gcc
PKG_CONFIG := pkg-config

CFLAGS   := -std=c17 -D_GNU_SOURCE -Wall -Wextra -Wno-comment
CPPFLAGS := -Icode/core -Icode/io
BUILD    := build

CORE     := code/core/core.c
ECHO     := code/apps/Echo-Server/echo.c
FILESRV  := code/apps/File-Transfer-Server/filesrv.c

SELECT   := code/io/io_select.c
POLL     := code/io/io_poll.c
EPOLL    := code/io/io_epoll.c
URING    := code/io/io_uring.c

ECHO_BINS := $(BUILD)/echo-select \
             $(BUILD)/echo-poll \
             $(BUILD)/echo-epoll \
             $(BUILD)/echo-uring

FILESRV_BINS := $(BUILD)/filesrv-select \
                $(BUILD)/filesrv-poll \
                $(BUILD)/filesrv-epoll \
                $(BUILD)/filesrv-uring

.PHONY: all echo filesrv clean

all: echo filesrv

echo: $(ECHO_BINS)

filesrv: $(FILESRV_BINS)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/echo-select: $(CORE) $(SELECT) $(ECHO) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(CORE) $(SELECT) $(ECHO) -o $@

$(BUILD)/echo-poll: $(CORE) $(POLL) $(ECHO) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(CORE) $(POLL) $(ECHO) -o $@

$(BUILD)/echo-epoll: $(CORE) $(EPOLL) $(ECHO) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(CORE) $(EPOLL) $(ECHO) -o $@

$(BUILD)/echo-uring: $(CORE) $(URING) $(ECHO) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(CORE) $(URING) $(ECHO) \
		$$($(PKG_CONFIG) --cflags --libs liburing) -o $@

$(BUILD)/filesrv-select: $(CORE) $(SELECT) $(FILESRV) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(CORE) $(SELECT) $(FILESRV) -o $@

$(BUILD)/filesrv-poll: $(CORE) $(POLL) $(FILESRV) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(CORE) $(POLL) $(FILESRV) -o $@

$(BUILD)/filesrv-epoll: $(CORE) $(EPOLL) $(FILESRV) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(CORE) $(EPOLL) $(FILESRV) -o $@

$(BUILD)/filesrv-uring: $(CORE) $(URING) $(FILESRV) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(CORE) $(URING) $(FILESRV) \
		$$($(PKG_CONFIG) --cflags --libs liburing) -o $@

clean:
	rm -rf $(BUILD)

.PHONY: test test-filesrv test-runner-acceptance

test: test-filesrv test-filesrv-bench test-runner-acceptance

test-runner-acceptance:
	python3 code/tests/File-Transfer-Server/run_experiment_acceptance_test.py

test-filesrv: filesrv
	python3 code/tests/File-Transfer-Server/filesrv_gate.py \
		--select ./build/filesrv-select \
		--poll ./build/filesrv-poll \
		--epoll ./build/filesrv-epoll \
		--uring ./build/filesrv-uring

# Benchmark fixture: generated (deterministic, not tracked by Git).
FILE_FIXTURES := code/apps/File-Transfer-Server/files
BENCH_FIXTURE := $(FILE_FIXTURES)/bench-4MiB.bin
BENCH_FIXTURES := $(FILE_FIXTURES)/bench-4KiB.bin \
                  $(FILE_FIXTURES)/bench-64KiB.bin \
                  $(FILE_FIXTURES)/bench-1MiB.bin \
                  $(FILE_FIXTURES)/bench-100MiB.bin

.PHONY: bench-fixture bench-fixtures test-filesrv-bench

bench-fixture: $(BENCH_FIXTURE)

bench-fixtures: $(BENCH_FIXTURES)

$(BENCH_FIXTURE): code/tools/File-Transfer-Server/make_fixture.py
	python3 code/tools/File-Transfer-Server/make_fixture.py $@ 4194304

$(FILE_FIXTURES)/bench-4KiB.bin: code/tools/File-Transfer-Server/make_fixture.py
	python3 code/tools/File-Transfer-Server/make_fixture.py $@ 4096

$(FILE_FIXTURES)/bench-64KiB.bin: code/tools/File-Transfer-Server/make_fixture.py
	python3 code/tools/File-Transfer-Server/make_fixture.py $@ 65536

$(FILE_FIXTURES)/bench-1MiB.bin: code/tools/File-Transfer-Server/make_fixture.py
	python3 code/tools/File-Transfer-Server/make_fixture.py $@ 1048576

$(FILE_FIXTURES)/bench-100MiB.bin: code/tools/File-Transfer-Server/make_fixture.py
	python3 code/tools/File-Transfer-Server/make_fixture.py $@ 104857600

test-filesrv-bench: filesrv bench-fixture
	python3 code/tests/File-Transfer-Server/filesrv_bench_check.py \
		--select ./build/filesrv-select \
		--poll ./build/filesrv-poll \
		--epoll ./build/filesrv-epoll \
		--uring ./build/filesrv-uring
