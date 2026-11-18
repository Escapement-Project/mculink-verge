# Contributing to Verge

The Escapement Project's [contribution guide](https://github.com/Escapement-Project/.github/blob/main/CONTRIBUTING.md)
applies here: issues, pull requests, commit messages, vendor firmware and the use of LLMs. This
file adds two rules particular to the firmware. Before a pull request, run the gate in the
README's [Check](README.md#check) section.

## Timing-critical code is assembly or generated

Anything that sets the phase or the length of a wire cycle is written in assembly, inline or in a
`.S` file, or generated at runtime by `src/swd_gen.c` and `src/jtag_gen.c` through `src/thumb.h`.
It is never C left to a compiler. A compiler is free to reorder, reallocate and reschedule, and
does so differently from one version or vendor to the next, so the timing of C belongs to one
compiler build, not to the source. `src/hot.h` records a change that left the instruction stream
identical and still cost a cycle on every data bit.

Some C from before the rule still sets wire timing. It is the list of what to replace:

- ARM's SWD paths: the unrolled C in `.ramfunc` and the transfer in `SW_DP.c`. Only the ARM
  reference build, with both generators off, carries them. It is the wire `tools/equiv.py` holds
  the generated paths to.
- ARM's JTAG and cJTAG paths in `JTAG_DP.c`, compiled four times, and their pacing. Again only the
  ARM reference build carries them.
- The sequence commands `DAP_SWJ_Sequence` and `DAP_SWD_Sequence`, which every build runs on ARM's
  C. `DAP_JTAG_Sequence` runs on the generated scans.
- `cJTAG_Escape` and `cJTAG_Clock` in `src/cjtag.c`, C paced by `DAP_Data.clock_delay`. They send
  vendor command `0x96`'s escapes, and a raw `DAP_SWJ_Sequence` while an OScan format is
  selected, in every build. Every constraint on them is a least time, and
  `tools/jtagcheck.py --wire raw` holds both compilers to them.

## Keep the Limits table current

The [Limits](docs/wires.md#limits) table records the fastest each transport runs, Verge's figures
beside those of NXP's firmware. A change that moves one of Verge's limits, faster or slower,
updates the table in the same commit. It also says where the new figure comes from, in
[Where the Limits figures come from](MEASUREMENTS.md#where-the-limits-figures-come-from). An entry
for NXP's firmware marked "not measured yet" is filled only by a bench run that measures it, never
by an estimate.
