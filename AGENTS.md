# AGENTS.md

Agent rules for this firmware. Read BENCH.md first for the bench, CODEGEN.md before touching the
generated transfers.

## Rules

**Timing-critical code is assembly or generated, never C left to an optimiser.** Anything that sets
the phase or the length of a wire cycle goes in assembly (inline or a `.S` file) or is emitted at
runtime by `src/swd_gen.c` through `src/thumb.h`. A C compiler is free to reorder, reallocate
and re-schedule, and does so differently across versions and vendors, so C timing is a property of
one compiler build, not of the source. `src/hot.h` records a change that left the instruction
stream identical and still cost a cycle on every data bit.

Code that predates the rule and breaks it, which is the work list:

- ARM's SWD paths: the unrolled C in `.ramfunc`, which serves every SWD transfer in a build with
  `SWD_CODEGEN` off, and SW_DP.c's transfer, the generated paths' fallback when nothing could be
  generated.
- The four-wire JTAG and two-pin paths (`JTAG_DP.c`, compiled three times) and their pacing: the
  generated JTAG paths' fallback, and all a build with `JTAG_CODEGEN` off has.
- The sequence commands (`DAP_SWJ_Sequence`, `DAP_SWD_Sequence`, `DAP_JTAG_Sequence`).

**Gate on both compilers.** `uv run tools/gate.py` before committing firmware or tool changes. It
runs the host checks, then builds gcc and clang, each in the default configuration and with
`SWD_CODEGEN` and `JTAG_CODEGEN` off, and holds every image to its baselines in the emulator.
`ninja -C <build> check` checks one image only.

**Keep README.md's "Limits" table current.** It is the record of the fastest each transport
runs. A change that moves one of those limits, faster or slower, updates the table in the same
commit, with how the new figure was measured.

**Keep BUSES.md current.** It maps every master's accesses to the AHB matrix ports and says which
ports each timed window uses. A new peripheral access, DMA channel or buffer in another SRAM bank
gets a row there, and one that lands on a timed window's port needs a handshake or another port:
the other core on a shared port has moved a wire edge twice.

**C that can run on the host runs there under sanitizers.** `tools/gen_host.py` builds the transfer
generator under ASan and UBSan with gcc and clang, and for QEMU's Cortex-M33 board with both, and
requires all four to generate identical code. New logic that does not touch hardware gets the same
treatment.

## Known traps

- **unicorn 2.1.4 mis-executes with memory hooks over SRAM.** It skipped one load in a clang image
  and the generator never returned, so `tools/cycles.py` installs its memory hooks only between
  `begin()` and `end()`. A read hook on a load inside an IT block also breaks the IT state: the
  next branch's target skips its first instruction (a `blx` into generated code lost its `push`).
  So loads are counted from the code hook and only writes are hooked. A fault or hang that QEMU
  and hardware do not show is the emulator; check with `tools/gen_host.py` or on hardware before
  blaming a compiler.
- **Nothing obliges a compiler to word-align a byte array.** Buffers that the generated code or a
  word copy touches carry `__attribute__((aligned(4)))`. clang left the reply buffer on a halfword.
