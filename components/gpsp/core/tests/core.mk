CORE_DIR := ../..
# Full-core fixtures run the native interpreter regardless of build settings.
override HAVE_DYNAREC := 0
override platform := unix
override GPSP_AFFINE_PIE := 0
override GPSP_BENCHMARK := 0
$(foreach option,$(filter GPSP_BENCHMARK_%,$(.VARIABLES)),$(eval override $(option) := 0))
include $(CORE_DIR)/Makefile.common

HOST_CFLAGS ?= -O1 -g
DEFINES := -DFRONTEND_SUPPORTS_RGB565 -DROM_BUFFER_SIZE=8 -D__LIBRETRO__ \
	-DINLINE=inline -DHAVE_INTTYPES_H -DHAVE_STDINT_H -DHAVE_STRINGS_H
FLAGS = -DROM_DIR='"$(BUILD)/roms"' $(HOST_CFLAGS) $(DEFINES) $(INCFLAGS)
DEPFLAGS = -MMD -MP
OBJECTS := $(addprefix $(BUILD)/obj/,$(notdir $(SOURCES_C:.c=.o) \
	$(SOURCES_CC:.cc=.o) $(SOURCES_ASM:.S=.o)))

vpath %.c $(sort $(dir $(SOURCES_C)))
vpath %.cc $(sort $(dir $(SOURCES_CC)))
vpath %.S $(sort $(dir $(SOURCES_ASM)))

$(OBJECTS): $(CORE_DIR)/Makefile.common
$(BUILD)/obj/bios_data.o: $(CORE_DIR)/bios/open_gba_bios.bin

$(BUILD)/obj/%.o: %.c Makefile ../core.mk ../host.mk
	@mkdir -p $(BUILD)/obj
	$(HOST_CC) $(TEST_FLAGS) $(FLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD)/obj/%.o: %.cc Makefile ../core.mk ../host.mk
	@mkdir -p $(BUILD)/obj
	$(HOST_CXX) $(TEST_FLAGS) $(FLAGS) $(DEPFLAGS) -std=c++11 -fno-rtti -fno-exceptions -c $< -o $@

$(BUILD)/obj/%.o: %.S Makefile ../core.mk ../host.mk
	@mkdir -p $(BUILD)/obj
	cd $(CORE_DIR) && $(HOST_CC) -c $(abspath $<) -o $(abspath $@)

-include $(wildcard $(BUILD)/*.d $(BUILD)/obj/*.d)

.PHONY: clean
clean:
	rm -rf build build-sanitize
