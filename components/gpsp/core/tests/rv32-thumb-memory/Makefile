include ../host.mk

CORE := ../..
RV32 := $(CORE)/riscv
HOST_CC ?= cc

.PHONY: test clean
test: $(BUILD)/test_thumb_memory
	$(BUILD)/test_thumb_memory

$(BUILD)/test_thumb_memory: test_thumb_memory.c $(RV32)/rv32_thumb_memory.c $(RV32)/rv32_thumb_memory.h
	mkdir -p $(BUILD)
	$(HOST_CC) $(TEST_FLAGS) -std=c11 -Wall -Wextra -Werror -O2 -DINLINE=inline \
		-I$(CORE) -I$(RV32) \
		-I$(CORE)/libretro/libretro-common/include \
		test_thumb_memory.c $(RV32)/rv32_thumb_memory.c -o $@

clean:
	rm -rf build build-sanitize

$(BUILD)/test_thumb_memory: Makefile ../host.mk $(TEST_HEADERS)
