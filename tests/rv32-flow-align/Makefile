include ../host.mk

RV32 := ../../riscv
HOST_CC ?= cc
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
DEAD_STRIP := -Wl,-dead_strip
else
DEAD_STRIP := -Wl,--gc-sections
endif

include ../rv32-zb.mk

CFLAGS := -std=c11 -Wall -Wextra -Werror -O2 $(ZB_CFLAGS) \
	-DGPSP_RV32_TEST_HOOKS=1 -ffunction-sections \
	-fdata-sections -I$(RV32)

.PHONY: test clean
test: $(OUT)/test_thumb $(OUT)/test_arm
	$(OUT)/test_thumb
	$(OUT)/test_arm

$(OUT)/test_thumb $(OUT)/test_arm: Makefile ../rv32-zb.mk $(wildcard $(RV32)/*.h)

$(OUT)/test_thumb: test_thumb.c $(RV32)/rv32_thumb_flow_full.c \
		$(RV32)/rv32_codegen.c $(RV32)/rv32_thumb_alu.c \
		$(RV32)/rv32_flow_timing.c $(RV32)/rv32_fallback.c
	mkdir -p $(OUT)
	$(HOST_CC) $(TEST_FLAGS) $(CFLAGS) test_thumb.c $(RV32)/rv32_codegen.c \
		$(RV32)/rv32_thumb_alu.c $(RV32)/rv32_flow_timing.c \
		$(RV32)/rv32_fallback.c $(DEAD_STRIP) -o $@

$(OUT)/test_arm: test_arm.c $(RV32)/rv32_arm_flow_full.c \
		$(RV32)/rv32_codegen.c $(RV32)/rv32_arm_branch.c \
		$(RV32)/rv32_arm_alu.c $(RV32)/rv32_flow_timing.c \
		$(RV32)/rv32_block_link.c
	mkdir -p $(OUT)
	$(HOST_CC) $(TEST_FLAGS) $(CFLAGS) test_arm.c $(RV32)/rv32_codegen.c \
		$(RV32)/rv32_arm_branch.c $(RV32)/rv32_arm_alu.c \
		$(RV32)/rv32_flow_timing.c $(RV32)/rv32_block_link.c \
		$(DEAD_STRIP) -o $@

clean:
	rm -rf build build-sanitize

$(OUT)/test_thumb $(OUT)/test_arm: Makefile ../host.mk $(TEST_HEADERS)
