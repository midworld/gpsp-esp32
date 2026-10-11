include ../host.mk

CORE := ../..
HOST_CXX ?= c++
HOST_FLAGS := -std=c++11 -Wall -Wextra -Werror -Wno-unused-but-set-variable \
    -O2 -DINLINE=inline \
    -D__LIBRETRO__ -DFRONTEND_SUPPORTS_RGB565 -fno-exceptions -fno-rtti \
    -ffunction-sections -fdata-sections -I$(CORE) \
    -I$(CORE)/include -I$(CORE)/libretro/libretro-common/include
ifeq ($(shell uname -s),Darwin)
DEAD_STRIP := -Wl,-dead_strip
else
DEAD_STRIP := -Wl,--gc-sections
endif
DEPS := Makefile test_bitmap.cc $(CORE)/video.cc $(wildcard $(CORE)/*.h) \
    $(wildcard $(CORE)/include/*.h)

.PHONY: test test-sanitize clean
test: $(BUILD)/test_bitmap
	$(BUILD)/test_bitmap

test-sanitize: $(BUILD)/test_bitmap_sanitize
	$(BUILD)/test_bitmap_sanitize

$(BUILD)/test_bitmap: $(DEPS)
	mkdir -p $(BUILD)
	$(HOST_CXX) $(TEST_FLAGS) $(HOST_FLAGS) test_bitmap.cc $(DEAD_STRIP) -o $@

$(BUILD)/test_bitmap_sanitize: $(DEPS)
	mkdir -p $(BUILD)
	$(HOST_CXX) $(TEST_FLAGS) $(HOST_FLAGS) -g -fsanitize=address,undefined \
		-fno-sanitize-recover=all test_bitmap.cc $(DEAD_STRIP) -o $@

clean:
	rm -rf build build-sanitize

$(BUILD)/test_bitmap $(BUILD)/test_bitmap_sanitize: Makefile ../host.mk $(TEST_HEADERS)
