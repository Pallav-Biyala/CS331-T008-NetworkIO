# tests/parser.mk - standalone build for the HTTP parser unit tests.
#
# Deliberately separate from the project's main Makefile (planned interface:
# `make APP=... IO=...`, `make test` for the correctness gate), so it cannot
# conflict with it. Needs no I/O backend.
#
#   make -f tests/parser.mk test       # build and run
#   make -f tests/parser.mk asan       # same under ASan + UBSan (gate T12 style)
#   make -f tests/parser.mk clean

CC      := gcc
CFLAGS  := -std=c17 -D_GNU_SOURCE -O2 -g -fno-omit-frame-pointer -Wall -Wextra
OUT     := build/tests

SRCS := apps/http_parser.c tests/test_http_parser.c
HDRS := apps/http_parser.h

.PHONY: test asan clean

$(OUT)/test_http_parser: $(SRCS) $(HDRS)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -o $@ $(SRCS)

$(OUT)/test_http_parser_asan: $(SRCS) $(HDRS)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -o $@ $(SRCS)

test: $(OUT)/test_http_parser
	./$(OUT)/test_http_parser

asan: $(OUT)/test_http_parser_asan
	./$(OUT)/test_http_parser_asan

clean:
	rm -f $(OUT)/test_http_parser $(OUT)/test_http_parser_asan
