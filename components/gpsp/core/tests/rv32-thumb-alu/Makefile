include ../host.mk

CODEGEN := ../../riscv
HOST_CC ?= cc

include ../rv32-zb.mk

.PHONY: test clean
test: $(OUT)/test_thumb_alu
	$(OUT)/test_thumb_alu

$(OUT)/test_thumb_alu: test_thumb_alu.c ../rv32-rvc.h $(CODEGEN)/rv32_thumb_alu.c $(CODEGEN)/rv32_thumb_alu.h $(CODEGEN)/rv32_register_map.h $(CODEGEN)/rv32_codegen.c $(CODEGEN)/rv32_codegen.h
	mkdir -p $(OUT)
	$(HOST_CC) $(TEST_FLAGS) -std=c11 -Wall -Wextra -Werror -O2 $(ZB_CFLAGS) -DGPSP_RV32_TEST_HOOKS=1 -I$(CODEGEN) \
		test_thumb_alu.c $(CODEGEN)/rv32_thumb_alu.c $(CODEGEN)/rv32_codegen.c -o $@

clean:
	rm -rf build build-sanitize

$(OUT)/test_thumb_alu: Makefile ../host.mk $(TEST_HEADERS)
