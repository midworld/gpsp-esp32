include ../host.mk

RV32 := ../../riscv
HOST_CC ?= cc
RV32_CC ?= $(RV_TOOL_PREFIX)gcc
RV32_OBJCOPY ?= $(RV_TOOL_PREFIX)objcopy

include ../rv32-zb.mk
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
DEAD_STRIP := -Wl,-dead_strip
else
DEAD_STRIP := -Wl,--gc-sections
RETRY_LDFLAGS := -no-pie
endif

.PHONY: test test-side-stub clean
test: $(OUT)/test_thumb_batch $(OUT)/test_thumb_batch_bytes $(OUT)/test_thumb_retry
	$(OUT)/test_thumb_batch
	$(OUT)/test_thumb_batch_bytes
	$(OUT)/test_thumb_retry

test-side-stub: $(OUT)/test_thumb_batch $(OUT)/side_stub.bin
	$(OUT)/test_thumb_batch $(OUT)/side_stub.bin

$(OUT)/side_stub.o: $(RV32)/rv32_stub.S $(RV32)/rv32_register_map.h
	mkdir -p $(OUT)
	$(RV32_CC) -march=rv32imac_zicsr_zifencei -mabi=ilp32 -mno-relax \
		-c $< -o $@

$(OUT)/side_stub.elf: $(OUT)/side_stub.o
	$(RV32_CC) -nostdlib -march=rv32imac_zicsr_zifencei -mabi=ilp32 \
		-mno-relax -Wl,-Ttext=0x30001000 \
		-Wl,-e,gpsp_rv32_thumb_side_preflight_stub -Wl,--gc-sections \
		$< -o $@

$(OUT)/side_stub.bin: $(OUT)/side_stub.elf
	$(RV32_OBJCOPY) -O binary -j .text $< $@

$(OUT)/test_thumb_batch: test_thumb_batch.c ../rv32-rvc.h $(RV32)/rv32_block_emit.c $(RV32)/rv32_arm_branch.c $(RV32)/rv32_thumb_alu.c $(RV32)/rv32_codegen.c $(RV32)/rv32_fallback.c $(RV32)/rv32_block_link.c $(RV32)/rv32_register_map.h
	mkdir -p $(OUT)
	$(HOST_CC) $(TEST_FLAGS) -std=c11 -Wall -Wextra -Werror -O2 $(ZB_CFLAGS) -ffunction-sections \
		-fdata-sections -DGPSP_RV32_TEST_HOOKS=1 -I$(RV32) test_thumb_batch.c \
		$(RV32)/rv32_arm_branch.c $(RV32)/rv32_thumb_alu.c \
		$(RV32)/rv32_codegen.c \
		$(RV32)/rv32_fallback.c $(RV32)/rv32_block_link.c \
		$(DEAD_STRIP) -o $@

$(OUT)/test_thumb_batch_bytes: test_thumb_batch.c ../rv32-rvc.h $(RV32)/rv32_block_emit.c $(RV32)/rv32_arm_branch.c $(RV32)/rv32_thumb_alu.c $(RV32)/rv32_codegen.c $(RV32)/rv32_fallback.c $(RV32)/rv32_block_link.c $(RV32)/rv32_register_map.h
	mkdir -p $(OUT)
	$(HOST_CC) $(TEST_FLAGS) -std=c11 -Wall -Wextra -Werror -O2 $(ZB_CFLAGS) -ffunction-sections \
		-fdata-sections -DGPSP_BENCHMARK_JIT_CODE_BYTES=1 \
		-DGPSP_RV32_TEST_HOOKS=1 -I$(RV32) test_thumb_batch.c \
		$(RV32)/rv32_arm_branch.c $(RV32)/rv32_thumb_alu.c \
		$(RV32)/rv32_codegen.c \
		$(RV32)/rv32_fallback.c $(RV32)/rv32_block_link.c \
		$(DEAD_STRIP) -o $@

$(OUT)/test_thumb_retry: test_thumb_retry.c retry_symbols.S $(RV32)/rv32_block_emit.c $(RV32)/rv32_codegen.c $(wildcard $(RV32)/*.h)
	mkdir -p $(OUT)
	$(HOST_CC) $(TEST_FLAGS) -std=c11 -Wall -Wextra -Werror -O2 $(ZB_CFLAGS) \
		-ffunction-sections -fdata-sections \
		-DGPSP_RV32_TEST_HOOKS=1 -I$(RV32) test_thumb_retry.c \
		retry_symbols.S $(RV32)/rv32_codegen.c \
		$(DEAD_STRIP) $(RETRY_LDFLAGS) -o $@

clean:
	rm -rf build build-sanitize

$(OUT)/test_thumb_batch $(OUT)/test_thumb_batch_bytes $(OUT)/test_thumb_retry: Makefile ../host.mk $(TEST_HEADERS)
