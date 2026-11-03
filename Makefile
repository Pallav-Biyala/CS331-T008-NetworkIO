CC       := gcc
CXX      := g++
PKG_CONFIG := pkg-config

CFLAGS   := -std=c17 -D_GNU_SOURCE -Wall -Wextra -Wno-comment
CXXFLAGS := -std=c++17 -O3 -Wall -Wextra -pthread
CPPFLAGS := -Icode/core -Icode/io
BUILD    := build

CORE     := code/core/core.c
ECHO     := code/apps/echo.c
FILESRV  := code/apps/filesrv.c
LOADGEN  := echo_loadgen
LOADGEN_SRC := code/tools/echo_loadgen.cpp

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

.PHONY: all echo filesrv loadgen clean

all: echo filesrv $(LOADGEN)

echo: $(ECHO_BINS)

filesrv: $(FILESRV_BINS)

loadgen: $(LOADGEN)

$(LOADGEN): $(LOADGEN_SRC)
	$(CXX) $(CXXFLAGS) $(LOADGEN_SRC) -o $@

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
	rm -rf $(BUILD) $(LOADGEN)

.PHONY: test-filesrv

test-filesrv: filesrv
	python3 code/tests/filesrv_gate.py \
		--select ./build/filesrv-select \
		--poll ./build/filesrv-poll \
		--epoll ./build/filesrv-epoll \
		--uring ./build/filesrv-uring
