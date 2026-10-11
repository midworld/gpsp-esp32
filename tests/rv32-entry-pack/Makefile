include ../host.mk

CODEGEN := ../../riscv
HOST_CC ?= cc

include ../rv32-zb.mk

.PHONY: test clean
test: $(OUT)/test_entry_pack
	$(OUT)/test_entry_pack

$(OUT)/test_entry_pack: test_entry_pack.c $(CODEGEN)/rv32_entry_pack.h
	mkdir -p $(OUT)
	$(HOST_CC) $(TEST_FLAGS) -std=c11 -Wall -Wextra -Werror -O2 $(ZB_CFLAGS) -I$(CODEGEN) \
		test_entry_pack.c -o $@

clean:
	rm -rf build build-sanitize

$(OUT)/test_entry_pack: Makefile ../host.mk $(TEST_HEADERS)
