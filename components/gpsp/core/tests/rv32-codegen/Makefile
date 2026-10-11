include ../host.mk

RV_AS := $(RV_TOOL_PREFIX)as
RV_LD := $(RV_TOOL_PREFIX)ld
RV_OBJCOPY := $(RV_TOOL_PREFIX)objcopy
HOST_CC ?= cc

CODEGEN := ../../riscv

include ../rv32-zb.mk

.PHONY: test clean
test: $(OUT)/test_codegen $(OUT)/reference.bin $(OUT)/compressed.bin \
		$(OUT)/short_branch.bin $(OUT)/zcb.bin $(OUT)/test_arm_alu
	$(PYTHON) test_register_map.py "$(HOST_CC)" $(CODEGEN)/rv32_register_map.h
	$(OUT)/test_codegen $(OUT)/reference.bin $(OUT)/compressed.bin \
		$(OUT)/short_branch.bin $(OUT)/zcb.bin
	$(OUT)/test_arm_alu

$(OUT):
	mkdir -p $(OUT)

$(OUT)/test_codegen: test_codegen.c ../rv32-rvc.h $(CODEGEN)/rv32_codegen.c $(CODEGEN)/rv32_codegen.h | $(OUT)
	$(HOST_CC) $(TEST_FLAGS) -std=c11 -Wall -Wextra -Werror -O2 $(ZB_CFLAGS) -I$(CODEGEN) \
		-DGPSP_RV32_TEST_HOOKS=1 test_codegen.c $(CODEGEN)/rv32_codegen.c -o $@

$(OUT)/test_arm_alu: test_arm_alu.c ../rv32-rvc.h $(CODEGEN)/rv32_arm_alu.c $(CODEGEN)/rv32_arm_alu.h $(CODEGEN)/rv32_register_map.h $(CODEGEN)/rv32_codegen.c $(CODEGEN)/rv32_codegen.h | $(OUT)
	$(HOST_CC) $(TEST_FLAGS) -std=c11 -Wall -Wextra -Werror -O2 $(ZB_CFLAGS) -DGPSP_RV32_TEST_HOOKS=1 -I$(CODEGEN) \
		test_arm_alu.c $(CODEGEN)/rv32_arm_alu.c $(CODEGEN)/rv32_codegen.c -o $@

$(OUT)/reference.o: reference.S | $(OUT)
	$(RV_AS) -march=rv32im_zba_zbb_zbs -mabi=ilp32 -o $@ $<

$(OUT)/compressed.o: compressed.S | $(OUT)
	$(RV_AS) -march=rv32imc -mabi=ilp32 -o $@ $<

$(OUT)/short_branch.o: short_branch.S | $(OUT)
	$(RV_AS) -march=rv32imc -mabi=ilp32 -o $@ $<

$(OUT)/zcb.o: zcb.S | $(OUT)
	$(RV_AS) -march=rv32imc_zcb -mabi=ilp32 -o $@ $<

$(OUT)/%.elf: $(OUT)/%.o
	$(RV_LD) -Ttext=0 -e _start -o $@ $<

$(OUT)/%.bin: $(OUT)/%.elf
	$(RV_OBJCOPY) -O binary --only-section=.text $< $@

clean:
	rm -rf build build-sanitize

$(OUT)/test_codegen $(OUT)/test_arm_alu: Makefile ../host.mk $(TEST_HEADERS)

.PHONY: test-host
test-host: $(OUT)/test_codegen $(OUT)/test_arm_alu
	$(PYTHON) test_register_map.py "$(HOST_CC)" $(CODEGEN)/rv32_register_map.h
	$(OUT)/test_codegen
	$(OUT)/test_arm_alu
