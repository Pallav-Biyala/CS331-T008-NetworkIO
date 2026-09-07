# ==============================================================================
#  io_uring TCP Echo Server — Makefile
# ==============================================================================
#
#  Targets:
#    make            → build server_iouring binary
#    make clean      → remove build artefacts
#    make clean all  → full rebuild
#
#  Requirements: Linux kernel ≥ 5.6
#  No external libraries — raw io_uring syscalls only.
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
