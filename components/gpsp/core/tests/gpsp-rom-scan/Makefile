include ../host.mk

CORE := ../..
HOST_CC ?= cc
HOST_CFLAGS ?= -O2
INCLUDES := -I$(CORE) -I$(CORE)/libretro/libretro-common/include

.PHONY: test clean
test: $(BUILD)/test_rom_scan
	$(BUILD)/test_rom_scan

$(BUILD)/test_rom_scan: test_rom_scan.c $(CORE)/gba_rom_scan.c $(CORE)/gba_rom_scan.h
	mkdir -p $(BUILD)
	$(HOST_CC) $(TEST_FLAGS) -std=c11 -Wall -Wextra -Werror $(HOST_CFLAGS) $(INCLUDES) \
		-Dmalloc=test_malloc -Dfree=test_free -c $(CORE)/gba_rom_scan.c \
		-o $(BUILD)/gba_rom_scan.o
	$(HOST_CC) $(TEST_FLAGS) -std=c11 -Wall -Wextra -Werror $(HOST_CFLAGS) $(INCLUDES) \
		test_rom_scan.c $(BUILD)/gba_rom_scan.o -o $@

clean:
	rm -rf build build-sanitize

$(BUILD)/test_rom_scan: Makefile ../host.mk $(TEST_HEADERS)
