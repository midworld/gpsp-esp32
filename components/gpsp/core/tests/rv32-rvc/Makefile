include ../host.mk

# Copyright (C) 2026 Midworld Kim
# SPDX-License-Identifier: GPL-3.0-or-later

HOST_CC ?= cc

include ../rv32-zb.mk

.PHONY: test clean
test: $(OUT)/test_rvc $(OUT)/compressed.bin $(OUT)/expanded.bin
	$(OUT)/test_rvc $(OUT)/compressed.bin $(OUT)/expanded.bin

$(OUT)/test_rvc: test_rvc.c ../rv32-rvc.h
	mkdir -p $(OUT)
	$(HOST_CC) $(TEST_FLAGS) -std=c11 -Wall -Wextra -Werror -O2 $< -o $@

$(OUT)/%.o: %.S
	mkdir -p $(OUT)
	$(RV_TOOL_PREFIX)as -march=rv32imc_zbb_zcb -mabi=ilp32 -o $@ $<

$(OUT)/%.bin: $(OUT)/%.o
	$(RV_TOOL_PREFIX)objcopy -O binary --only-section=.text $< $@

clean:
	rm -rf build build-sanitize

$(OUT)/test_rvc: Makefile ../host.mk $(TEST_HEADERS)
