include ../host.mk

CODEGEN := ../../riscv
HOST_CC ?= cc

include ../rv32-zb.mk

.PHONY: test clean
test: $(OUT)/test_arm_branch
	$(OUT)/test_arm_branch

$(OUT)/test_arm_branch: test_arm_branch.c ../rv32-rvc.h $(CODEGEN)/rv32_arm_branch.c $(CODEGEN)/rv32_arm_branch.h $(CODEGEN)/rv32_register_map.h $(CODEGEN)/rv32_codegen.c $(CODEGEN)/rv32_codegen.h
	mkdir -p $(OUT)
	$(HOST_CC) $(TEST_FLAGS) -std=c11 -Wall -Wextra -Werror -O2 $(ZB_CFLAGS) -DGPSP_RV32_TEST_HOOKS=1 -I$(CODEGEN) \
		test_arm_branch.c $(CODEGEN)/rv32_arm_branch.c $(CODEGEN)/rv32_codegen.c -o $@

clean:
	rm -rf build build-sanitize

$(OUT)/test_arm_branch: Makefile ../host.mk $(TEST_HEADERS)
