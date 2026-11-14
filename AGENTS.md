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

- ARM's SWD paths: the unrolled C in `.ramfunc` and SW_DP.c's transfer. Only the ARM reference
  build (both generators off) carries them, as the wire tools/equiv.py holds the generated paths to.
- The JTAG and cJTAG paths (`JTAG_DP.c`, compiled four times) and their pacing:
  likewise the ARM reference build's alone.
- The sequence commands `DAP_SWJ_Sequence` and `DAP_SWD_Sequence`, which every build runs on ARM's
  C. `DAP_JTAG_Sequence` runs on the generated scans.
- `cJTAG_Escape` and `cJTAG_Clock` (`src/cjtag.c`), C paced by `DAP_Data.clock_delay`: vendor
  0x96's escapes and raw `DAP_SWJ_Sequence` with an OScan format selected, in every build (user,
  2026-11-11). Every constraint on them is a least time; `tools/jtagcheck.py --wire raw` holds both
  compilers to them.

**Gate on both compilers.** `uv run tools/gate.py` before committing firmware or tool changes. It
runs the host checks, then builds gcc and clang, each in the default configuration and as the ARM
reference, with `SWD_CODEGEN` and `JTAG_CODEGEN` off. The two options go together; CMake refuses
one without the other. It holds every image to its baselines in the emulator,
each generated image to its compiler's ARM paths on every `tools/equiv.py` matrix, and the
reference images' cJTAG frames to `tools/jtagcheck.py`'s wire rules. Two more gcc
images, one with `DAP_TIMING_DIAGNOSTICS` and one with `CODE_LAB`, get `tools/image.py` alone.
`ninja -C <build> check` checks one image only.

**Keep the "Limits" table in docs/wires.md current.** It is the record of the fastest each transport
runs, ours beside stock's. A change that moves one of ours, faster or slower, updates the table in
the same commit, and says where the new figure comes from in MEASUREMENTS.md's "Where the Limits
figures come from". Stock's entries marked "not measured yet" are filled when a bench run measures
them; none is filled from a guess.

**Keep BUSES.md current.** It maps every master's accesses to the AHB matrix ports and says which
ports each timed window uses. A new peripheral access, DMA channel or buffer in another SRAM bank
gets a row there, and one that lands on a timed window's port needs a handshake or another port:
the other core on a shared port has moved a wire edge twice.

**C that can run on the host runs there under sanitizers.** `tools/gen_host.py` builds the transfer
generator under ASan and UBSan with gcc and clang, and for QEMU's Cortex-M33 board with both, and
requires all four to generate identical code. New logic that does not touch hardware gets the same
treatment.

**Name the wire protocols: SWD, JTAG and cJTAG.** In prose, comments and output alike. Not
"four-wire JTAG", "two-pin cJTAG" or "the two-pin path": JTAG is always four-wire and cJTAG always
two-pin here, and "two-pin" alone could be SWD. Keep a pin count only where IEEE 1149.7's own pin
modes are the subject, such as STC2 switching a part to four-pin.

**Nothing read out of NXP's firmware image goes in this repository.** Describe the stock firmware
only by what USB or the wire shows, and say how it was observed. mculink-stock-notes holds those
observations.

## Known traps

- **unicorn 2.1.4 mis-executes with memory hooks over SRAM.** It skipped one load in a clang image
  and the generator never returned, so `tools/cycles.py` installs its memory hooks only between
  `begin()` and `end()`. A read hook on a load inside an IT block also breaks the IT state: the
  next branch's target skips its first instruction (a `blx` into generated code lost its `push`).
  So loads are counted from the code hook and only writes are hooked. A write hook over an IT
  block's conditional store breaks it too: the instruction at the next 1 KiB boundary ran as if
  still conditional, and a translation made that way was reused on later calls. So the image
  checks watch the ordinary GPIO through an MMIO page (`watch_gpio_ports`), not a write hook. A
  fault or hang that QEMU and hardware do not show is the emulator; check with `tools/gen_host.py`
  or on hardware before blaming a compiler.
- **Nothing obliges a compiler to word-align a byte array.** Buffers that the generated code or a
  word copy touches carry `alignas` with at least four-byte alignment. clang left the reply buffer
  on a halfword.
