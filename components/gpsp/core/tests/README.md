# Tests

- Run from the core root on a POSIX host with GNU Make, a C/C++ compiler
  and Python 3.
- `make -C tests check`: native CPU, emitter, timing, state, memory, video,
  sound and build-configuration checks. No SDK, device or game ROM is needed.
- `make -C tests check-sanitize`: the same checks with AddressSanitizer and
  UndefinedBehaviorSanitizer. Requires compiler support.
- `make -C tests check-rv32 RV_TOOL_PREFIX=riscv32-unknown-elf-`: includes
  comparisons against GNU assembler output. The assembler must support Zcb.
- `make -C tests check-matrix RV_TOOL_PREFIX=riscv32-unknown-elf-`: runs the
  RV32 suites in 9 ISA configurations.
- `make -C tests/rv32-thumb-batch test-side-stub RV_TOOL_PREFIX=...`: also
  compares the helper stub with output from a cross compiler.

## Options

- `HOST_CC`, `HOST_CXX`, `PYTHON`: native tools.
- `RV_TOOL_PREFIX`: cross-tool prefix, including the trailing `-`.
- `ZB="ZBA ZBB ZBS"`, `RVC=1`, `ZCB=1`: individual emitter
  configurations. Defaults are zero or empty.
- Outputs use local `build/` or `build-sanitize/` directories. ISA variants
  have separate subdirectories. Clean a suite when changing compiler flags.
- Synthetic ROM fixtures are created locally during the tests.
- Full-core host fixtures always use the interpreter, independently of exported
  JIT, XespV and benchmark build settings.

## Scope

- Native emitter tests check bytes and guest-state models. They do not run
  generated code on a RISC-V CPU.
- Executable-memory permissions, instruction-cache publication, cycle CSRs
  and XespV execution need target validation in the frontend project.
- The original ARM64/MIPS assembler checks remain available as `make -C tests`.
  Set `ARMV8PFX` and `MIPS32PFX` for those toolchains.
