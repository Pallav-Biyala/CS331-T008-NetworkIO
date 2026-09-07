# ==============================================================================
#  io_uring TCP Echo Server — Makefile
# ==============================================================================
#
#  Targets:
#    make            → build server_iouring binary
#    make clean      → remove build artefacts
#
#  Requirements: Linux kernel ≥ 5.1 (io_uring first appeared in 5.1;
#  IORING_OP_ACCEPT needs ≥ 5.5, IORING_OP_SEND/RECV ≥ 5.6).
#  No external libraries needed — we call the raw io_uring syscalls directly.
# ==============================================================================

CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -std=gnu11
TARGET  = server_iouring
SRCS    = server_iouring.c network_utils.c
OBJS    = $(SRCS:.c=.o)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
