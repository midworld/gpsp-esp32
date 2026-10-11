<!-- Copyright (C) 2026 Midworld Kim -->

# RV32 JIT Lab

- Input: four Thumb instructions at ROM PC `0x08000100`:
  `TST r0,r0; BEQ +0; MOVS r1,#2; MOVS r2,#3`.
- Boundary: stop after the fourth instruction. This is a fixed fragment,
  not a complete ROM scanner run.
- Current emitter: call the production instruction emitter, patch the
  internal branch, and add the ROM fallthrough link slot.
- IR emitter: decode the four operations, analyze flag liveness, then emit
  one shared body. Side entries share that body when appended at its tail.
- The IR emitter accepts only this forward TST/BEQ/MOVS/MOVS shape. It is
  experimental and is not linked into the firmware.
- The flow emitter accepts forward TST/BEQ/MOVS regions up to 32 Thumb ops.
  It makes branch targets and fallthroughs basic-block entries. A detached
  side block stops at the next published entry.
- Every flow block charges its cycles at its exit, including a 32-op scan
  limit. A block that can exceed 64 ROM cycles uses a low-budget one-step
  path. This experiment is not linked into the firmware.
- Host wait-table symbols use low virtual addresses. The emitter only
  encodes their addresses; the host never reads them.

## Run

- `make run`: print code bytes, entry offsets, memory capacities, and host
  translation time with byte accounting disabled. IR runs cover the head,
  the head plus branch join, and all four entry addresses.
- `make validate`: compare generated RV32 code with a Thumb reference for
  entry points, flags, waits, and execution budgets. The flow checks include
  a branchless 32-op limit and a detached BEQ entry.
- `make counts`: print generated code sizes for the current, IR, and flow
  emitters on the four-op and 32-op inputs.
- `make bytes`: print the same result with emitter byte accounting enabled.
- `build/current --dump` and `build/ir --dump`: print generated RV32 words.
- `build/ir --entries head|head-join|all [--append]`: select entry points.
  `--append` emits the head first and adds selected side entries before any
  other block occupies the cache tail. Late side-entry discovery is pending.
- `build/flow [--cap32] [--all-entries] [--reverse-entries]`: count the
  branch-block code and optional detached middle entries. The all-entry
  modes count side code in discovery order; they do not install cache aliases.
- `build/flow --iterations N`: also time IR decode and code emission on the
  host. The default flow run does not measure time.
- `build/current_batch [--all-entries]`: count the production 32-op batch
  and optional side adapters with their cold steps.
- `build/current --iterations N --trials M`: time `N` translations after
  256 warmups and report the median of `M` trials. Defaults: 100,000 and 5.
- `build/ir --iterations N`: use the same 100,000 iteration, five trial
  default. It reports decode plus emit and emit-only CPU time separately.
- `make stack`: print compiler-reported stack frames for the active paths.

## Budget

- `code_bytes` includes link slots and any cold stub. Add the separate
  8-byte ROM cache header for the block.
- The side-code totals exclude cache alias metadata and link maintenance.
- Flow prints temporary IR and layout sizes. Its two lab output buffers are
  capacities, not peak stack use or persistent cache metadata.
- IR `entry_region` bytes are layout slices between entry offsets, not the
  cost of one guest instruction.
- `workspace_capacity_bytes` includes the output buffer and lab metadata.
  `emitter_stage_capacity_bytes` is the largest local staging array.
  These capacities overlap compiler stack frames; do not add them to the
  reported stack path.
- IR `metadata_bytes` counts its 392-byte decode array and 20-byte layout.
  The decode array is temporary; a future cache integration can reconstruct
  it instead of retaining it. The shared production lookup table is excluded.
- Host CPU nanoseconds measure translation, not generated-code execution.
  `current` covers the emitter and link patch; IR total includes its decode.
  Neither figure predicts ESP32-P4 cycles.
