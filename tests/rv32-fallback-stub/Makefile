include ../host.mk

CORE := ../../riscv
HOST_CC ?= cc

.PHONY: test clean
test: $(BUILD)/test_fallback_stub
	$(BUILD)/test_fallback_stub

$(BUILD)/test_fallback_stub: test_fallback_stub.c $(CORE)/rv32_fallback.c $(CORE)/rv32_codegen.c $(CORE)/rv32_fallback.h $(CORE)/rv32_interpreter_stub.h \
		$(CORE)/rv32_register_map.h
	mkdir -p $(BUILD)
	$(HOST_CC) $(TEST_FLAGS) -std=c11 -Wall -Wextra -Werror -O2 -I$(CORE) \
		test_fallback_stub.c $(CORE)/rv32_fallback.c $(CORE)/rv32_codegen.c -o $@

clean:
	rm -rf build build-sanitize

$(BUILD)/test_fallback_stub: Makefile ../host.mk $(TEST_HEADERS)
