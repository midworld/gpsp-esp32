include ../host.mk

CODEGEN := ../../riscv
CORE := ../..
HOST_CC ?= cc

include ../rv32-zb.mk

.PHONY: test clean
test: $(OUT)/test_arm_system
	$(OUT)/test_arm_system

$(OUT)/test_arm_system: test_arm_system.c ../rv32-rvc.h $(CODEGEN)/rv32_arm_system.c $(CODEGEN)/rv32_arm_system.h $(CODEGEN)/rv32_arm_alu.c $(CODEGEN)/rv32_arm_branch.c $(CODEGEN)/rv32_codegen.c $(CODEGEN)/rv32_register_map.h
	mkdir -p $(OUT)
	$(HOST_CC) $(TEST_FLAGS) -std=c11 -Wall -Wextra -Werror -O2 $(ZB_CFLAGS) -DGPSP_RV32_TEST_HOOKS=1 -DINLINE=inline \
		-D__LIBRETRO__ -DFRONTEND_SUPPORTS_RGB565 -I$(CORE)/libretro/libretro-common/include \
		-I$(CODEGEN) test_arm_system.c $(CODEGEN)/rv32_arm_alu.c $(CODEGEN)/rv32_arm_branch.c \
		$(CODEGEN)/rv32_codegen.c -o $@

clean:
	rm -rf build build-sanitize

$(OUT)/test_arm_system: Makefile ../host.mk $(TEST_HEADERS)
