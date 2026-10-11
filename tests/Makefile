
ARMV8PFX=/opt/buildroot-armv8el-uclibc/bin/aarch64-buildroot-linux-uclibc
MIPS32PFX=/opt/buildroot-mipsel32-o32-uclibc/bin/mipsel-buildroot-linux-uclibc

all:
	gcc -o arm64gen arm64gen.c -ggdb -I../arm/
	./arm64gen > bytecode.bin
	$(ARMV8PFX)-as -o bytecoderef.o arm64gen.S
	$(ARMV8PFX)-objcopy -O binary bytecoderef.o bytecoderef.bin
	@ cmp bytecoderef.bin bytecode.bin || echo "Bytecode mismatch"
	@ cmp bytecoderef.bin bytecode.bin && echo "Test passed!"
	gcc -o mipsgen mipsgen.c -ggdb -I../mips/
	./mipsgen > bytecode.bin
	$(MIPS32PFX)-as -EL -o bytecoderef.o mipsgen.S
	$(MIPS32PFX)-objcopy -j .text -O binary bytecoderef.o bytecoderef.bin
	@ cmp bytecoderef.bin bytecode.bin || echo "Bytecode mismatch"
	@ cmp bytecoderef.bin bytecode.bin && echo "Test passed!"

include host.mk

CORE := ..
HOST_FLAGS := -std=c++11 -O1 -ffunction-sections -fdata-sections \
    -D__LIBRETRO__ -DFRONTEND_SUPPORTS_RGB565 -DINLINE=inline \
    -I$(CORE) -I$(CORE)/include -I$(CORE)/libretro \
    -I$(CORE)/libretro/libretro-common/include
ifeq ($(shell uname -s),Darwin)
DEAD_STRIP := -Wl,-dead_strip
else
DEAD_STRIP := -Wl,--gc-sections
endif
CPU_TESTS := arm_alu_interpreter arm_block_transfer arm_one_step shift_interpreter
CPU_BINS := $(addprefix $(BUILD)/,$(CPU_TESTS)) $(BUILD)/arm_mul_interpreter
HOST_SUITES := rv32-thumb-alu rv32-thumb-batch \
    rv32-arm-system rv32-block-link rv32-arm-branch rv32-entry-pack \
    rv32-flow-align rv32-cycle-budget rv32-ram-dirty rv32-fallback-stub \
    rv32-thumb-memory gpsp-rom-scan video-bitmap game-switch core-regressions
RV32_SUITES := rv32-codegen rv32-thumb-alu rv32-thumb-batch \
    rv32-arm-system rv32-block-link rv32-arm-branch rv32-rvc rv32-entry-pack \
    rv32-flow-align

.PHONY: check check-cpu check-config check-rv32 check-matrix check-sanitize
check: check-cpu check-config
	@set -e; for suite in $(HOST_SUITES); do $(MAKE) -C $$suite test; done
	$(MAKE) -C rv32-codegen test-host

check-cpu: $(CPU_BINS)
	@set -e; for test in $(CPU_BINS); do $$test; done

check-config:
	MAKE="$(MAKE)" $(PYTHON) test_make_config.py

$(BUILD)/%: %.cc cpu_jit_stubs.cc $(CORE)/cpu.cc $(wildcard $(CORE)/*.h) host.mk Makefile
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(TEST_FLAGS) $(HOST_FLAGS) -DGPSP_RV32_JIT=1 $< cpu_jit_stubs.cc $(DEAD_STRIP) -o $@

$(BUILD)/arm_mul_interpreter: rv32-arm-mul/host_interpreter.cc cpu_jit_stubs.cc rv32-arm-mul/cases.h $(CORE)/cpu.cc $(wildcard $(CORE)/*.h) host.mk Makefile
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(TEST_FLAGS) $(HOST_FLAGS) -DGPSP_RV32_JIT=1 $< cpu_jit_stubs.cc $(DEAD_STRIP) -o $@

check-rv32:
	@set -e; for suite in $(RV32_SUITES); do $(MAKE) -C $$suite test; done

check-matrix:
	+MAKE="$(MAKE)" $(SHELL) rv32-matrix.sh

check-sanitize:
	$(MAKE) check SANITIZE=1
