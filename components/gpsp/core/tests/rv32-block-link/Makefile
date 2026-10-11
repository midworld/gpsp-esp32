include ../host.mk

CODEGEN := ../../riscv
HOST_CC ?= cc

include ../rv32-zb.mk

.PHONY: test clean
test: $(OUT)/test_block_link
	$(OUT)/test_block_link

$(OUT)/test_block_link: test_block_link.c $(CODEGEN)/rv32_block_link.c $(CODEGEN)/rv32_block_link.h $(CODEGEN)/rv32_codegen.c $(CODEGEN)/rv32_codegen.h $(CODEGEN)/rv32_register_map.h
	mkdir -p $(OUT)
	$(HOST_CC) $(TEST_FLAGS) -std=c11 -Wall -Wextra -Werror -O2 $(ZB_CFLAGS) -I$(CODEGEN) \
		-DGPSP_RV32_TEST_HOOKS=1 test_block_link.c $(CODEGEN)/rv32_block_link.c $(CODEGEN)/rv32_codegen.c -o $@

clean:
	rm -rf build build-sanitize

$(OUT)/test_block_link: Makefile ../host.mk $(TEST_HEADERS)
