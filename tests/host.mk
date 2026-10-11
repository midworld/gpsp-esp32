HOST_CC ?= cc
HOST_CXX ?= c++
PYTHON ?= python3
RV_TOOL_PREFIX ?= riscv32-unknown-elf-
SANITIZE ?= 0
ifeq ($(SANITIZE),1)
TEST_FLAGS := -g -fsanitize=address,undefined -fno-sanitize-recover=all
TEST_SUFFIX := -sanitize
else ifneq ($(SANITIZE),0)
$(error SANITIZE must be 0 or 1)
endif
BUILD := build$(TEST_SUFFIX)
TEST_HEADERS := $(wildcard ../../*.h ../../include/*.h ../../riscv/*.h *.h) \
    ../rv32-rvc.h ../rv32-zb.mk
