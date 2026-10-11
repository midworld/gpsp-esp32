RV32 := ../../components/gpsp/core/riscv
HOST_CC ?= cc
HOST_CFLAGS := -std=c11 -Wall -Wextra -Werror -O2 -fstack-usage \
    -ffunction-sections -fdata-sections -I$(RV32)
OBJS := build/current.o build/lab_report.o build/rv32_arm_branch.o \
    build/rv32_thumb_alu.o build/rv32_codegen.o build/rv32_fallback.o \
    build/rv32_block_link.o
IR_OBJS := build/ir.o build/lab_report.o build/rv32_thumb_ir.o \
    build/rv32_thumb_ir_emit.o build/rv32_codegen.o build/rv32_fallback.o \
    build/rv32_block_link.o
VALIDATE_OBJS := build/validate.o build/rv32_thumb_ir.o \
    build/rv32_thumb_ir_emit.o build/rv32_codegen.o build/rv32_fallback.o \
    build/rv32_block_link.o
FLOW_OBJS := build/flow.o build/rv32_thumb_ir.o \
    build/rv32_thumb_flow_emit.o build/rv32_codegen.o \
    build/rv32_fallback.o build/rv32_block_link.o
FLOW_VALIDATE_OBJS := build/validate_flow.o build/rv32_thumb_ir.o \
    build/rv32_thumb_flow_emit.o build/rv32_codegen.o \
    build/rv32_fallback.o build/rv32_block_link.o
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
DEAD_STRIP := -Wl,-dead_strip
else
DEAD_STRIP := -Wl,--gc-sections
endif

.PHONY: all run counts bytes stack validate clean
all: build/current build/ir build/validate build/flow \
    build/validate_flow build/current_batch

run: build/current build/ir
	build/current
	build/ir --entries all
	build/ir --entries all --append
	build/ir --entries head-join --append
	build/ir --entries head

validate: build/validate build/validate_flow
	build/validate
	build/validate_flow

counts: build/current build/ir build/flow build/current_batch
	@printf 'fixture=branch4 entries=all translator=current\n'
	@build/current --iterations 1 --trials 1 > build/current-counts.out && \
		sed -n '/^translator=/p;/^code_bytes=/p' build/current-counts.out
	@printf 'fixture=branch4 entries=head-join translator=thumb_ir\n'
	@build/ir --entries head-join --iterations 1 > build/ir-join-counts.out && \
		sed -n '/^translator=/p;/^code_bytes=/p' build/ir-join-counts.out
	@printf 'fixture=branch4 entries=all translator=thumb_ir\n'
	@build/ir --entries all --iterations 1 > build/ir-all-counts.out && \
		sed -n '/^translator=/p;/^code_bytes=/p' build/ir-all-counts.out
	build/flow
	build/flow --all-entries
	build/current_batch
	build/current_batch --all-entries
	build/flow --cap32
	build/flow --cap32 --all-entries
	build/flow --cap32 --all-entries --reverse-entries

bytes: build/current_bytes
	build/current_bytes

stack: build/current build/ir
	@python3 stack_budget.py build/current.su build/rv32_thumb_alu.su
	@python3 stack_ir.py build/ir.su build/rv32_thumb_ir_emit.su \
		build/rv32_fallback.su build/rv32_block_link.su

build/current: $(OBJS)
	$(HOST_CC) $(OBJS) $(DEAD_STRIP) -o $@

build/current_bytes: build/current_bytes.o $(filter-out build/current.o,$(OBJS))
	$(HOST_CC) $^ $(DEAD_STRIP) -o $@

build/ir: $(IR_OBJS)
	$(HOST_CC) $^ $(DEAD_STRIP) -o $@

build/validate: $(VALIDATE_OBJS)
	$(HOST_CC) $^ $(DEAD_STRIP) -o $@

build/flow: $(FLOW_OBJS)
	$(HOST_CC) $^ $(DEAD_STRIP) -o $@

build/validate_flow: $(FLOW_VALIDATE_OBJS)
	$(HOST_CC) $^ $(DEAD_STRIP) -o $@

build/current_batch: build/current_batch.o $(filter-out build/current.o,$(OBJS))
	$(HOST_CC) $^ $(DEAD_STRIP) -o $@

build/current.o: current.c lab_report.h $(RV32)/rv32_block_emit.c
	mkdir -p build
	$(HOST_CC) $(HOST_CFLAGS) -c $< -o $@

build/current_bytes.o: current.c lab_report.h $(RV32)/rv32_block_emit.c
	mkdir -p build
	$(HOST_CC) $(HOST_CFLAGS) -DGPSP_BENCHMARK_JIT_CODE_BYTES=1 \
		-c $< -o $@

build/ir.o: ir.c lab_report.h rv32_thumb_ir_emit.h rv32_thumb_ir.h
	mkdir -p build
	$(HOST_CC) $(HOST_CFLAGS) -c $< -o $@

build/validate.o: validate.c rv32_thumb_ir_emit.h rv32_thumb_ir.h
	mkdir -p build
	$(HOST_CC) $(HOST_CFLAGS) -c $< -o $@

build/flow.o: flow.c rv32_thumb_flow_emit.h rv32_thumb_ir.h
	mkdir -p build
	$(HOST_CC) $(HOST_CFLAGS) -c $< -o $@

build/validate_flow.o: validate_flow.c rv32_thumb_flow_emit.h rv32_thumb_ir.h
	mkdir -p build
	$(HOST_CC) $(HOST_CFLAGS) -c $< -o $@

build/current_batch.o: current_batch.c $(RV32)/rv32_block_emit.c
	mkdir -p build
	$(HOST_CC) $(HOST_CFLAGS) -c $< -o $@

build/rv32_thumb_ir.o build/rv32_thumb_ir_emit.o \
    build/rv32_thumb_flow_emit.o: build/%.o: %.c
	mkdir -p build
	$(HOST_CC) $(HOST_CFLAGS) -c $< -o $@

build/rv32_thumb_ir.o: rv32_thumb_ir.h
build/rv32_thumb_ir_emit.o: rv32_thumb_ir_emit.h rv32_thumb_ir.h
build/rv32_thumb_flow_emit.o: rv32_thumb_flow_emit.h rv32_thumb_ir.h

build/lab_report.o: lab_report.c lab_report.h
	mkdir -p build
	$(HOST_CC) $(HOST_CFLAGS) -c $< -o $@

build/rv32_%.o: $(RV32)/rv32_%.c
	mkdir -p build
	$(HOST_CC) $(HOST_CFLAGS) -c $< -o $@

clean:
	rm -rf build
