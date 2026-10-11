HOST_CC ?= cc
CFLAGS ?= -O2

.PHONY: test clean
test: build/test_command_queue
	build/test_command_queue

build/test_command_queue: test_command_queue.c ../../protocol/gpsp_command_queue.h
	mkdir -p build
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror $(CFLAGS) -pthread \
		-I../../protocol test_command_queue.c -o $@

clean:
	rm -rf build
