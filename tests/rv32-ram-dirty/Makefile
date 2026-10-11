include ../host.mk

CODEGEN := ../../riscv
HOST_CC ?= cc

.PHONY: test clean
test: $(BUILD)/test_dirty
	$(BUILD)/test_dirty

$(BUILD)/test_dirty: test_dirty.c $(CODEGEN)/rv32_ram_units.c $(CODEGEN)/rv32_ram_units.h
	mkdir -p $(BUILD)
	$(HOST_CC) $(TEST_FLAGS) -std=c11 -Wall -Wextra -Werror -O2 -I$(CODEGEN) \
		test_dirty.c $(CODEGEN)/rv32_ram_units.c -o $@

clean:
	rm -rf build build-sanitize

$(BUILD)/test_dirty: Makefile ../host.mk $(TEST_HEADERS)
