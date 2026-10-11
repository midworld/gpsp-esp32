MAIN := ../../main
HOST_CC ?= cc

.PHONY: test clean
test: build/test_save_file
	build/test_save_file

build/test_save_file: test_save_file.c $(MAIN)/gpsp_save_file.c $(MAIN)/include/gpsp_save_file.h
	mkdir -p build
	$(HOST_CC) -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -O2 -I$(MAIN)/include \
		-Drename=test_rename -c $(MAIN)/gpsp_save_file.c -o build/gpsp_save_file.o
	$(HOST_CC) -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -O2 -I$(MAIN)/include \
		test_save_file.c build/gpsp_save_file.o -o $@

clean:
	rm -rf build
