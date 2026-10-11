.DEFAULT_GOAL := test
include ../host.mk
include ../core.mk

.PHONY: test clean
test: $(BUILD)/test_game_switch
	mkdir -p $(BUILD)/roms
	$(BUILD)/test_game_switch

$(BUILD)/test_game_switch: test_game_switch.c $(OBJECTS)
	$(HOST_CC) $(TEST_FLAGS) $(FLAGS) $(DEPFLAGS) -MF $(BUILD)/test_game_switch.d \
		test_game_switch.c $(OBJECTS) -o $@

$(BUILD)/test_game_switch: Makefile ../core.mk ../host.mk
