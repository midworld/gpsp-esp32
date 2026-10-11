include ../host.mk

CORE := ../..
HOST_CC ?= cc
CFLAGS := -std=c11 -Wall -Wextra -Werror -Wno-sign-compare -O2 -DINLINE=inline \
    -DGPSP_RV32_JIT -ffunction-sections -fdata-sections \
    -I$(CORE) -I$(CORE)/libretro/libretro-common/include
ifeq ($(shell uname -s),Darwin)
DEAD_STRIP := -Wl,-dead_strip
else
DEAD_STRIP := -Wl,--gc-sections
endif

.PHONY: test clean
test: $(BUILD)/test_cycle_budget
	$(BUILD)/test_cycle_budget

$(BUILD)/test_cycle_budget: test_cycle_budget.c $(CORE)/main.c \
    $(CORE)/savestate.c $(CORE)/riscv/rv32_cycle_budget.h
	mkdir -p $(BUILD)
	$(HOST_CC) $(TEST_FLAGS) $(CFLAGS) test_cycle_budget.c $(CORE)/main.c \
		$(CORE)/savestate.c $(DEAD_STRIP) -o $@

clean:
	rm -rf build build-sanitize

$(BUILD)/test_cycle_budget: Makefile ../host.mk $(TEST_HEADERS)
