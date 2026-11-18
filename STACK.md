# Stack checks

CPU 0 reserves 4 KiB at the top of SRAM0 for its stack. CPU 1 reserves the first 16 KiB of SRAM1
(0x20010000..0x20013fff). The linker defines both ranges, and the image checks hold each to its
limits. CPU 0's link refuses a stack that overlaps its data.

CPU 1's stack sits directly above CPU 0's. So each core sets MSPLIM to its stack's bottom at reset,
and an overflow faults instead of overwriting the memory below. CPU 0's limit comes from NXP's
startup file, and its fault handler spins. CPU 1 records the fault for CPU 0 to report
(`tools/dap.py core1`, with CFSR.STKOF set). It does that on a fresh stack: after an overflow, MSP
sits at the limit, and any push would lock the core up. The image checks read both reset handlers,
and CPU 1's fault handler, for these writes.

Neither core reserves a heap. The gate traps entry to every allocator function linked in, and checks
the trap by calling each one on purpose. Serial formatting, and every command and error path the
image checks exercise, run with the trap set.

To inspect the synchronous stack paths in a linked image:

```sh
uv run tools/stack_usage.py build/mculink_cjtag.elf \
    build/mculink_cjtag_core1.elf --output stack-static.json
```

The analyser follows direct calls and branches in Thumb code, assembly and linked library functions
included. It reports no bound for a path through an indirect call, a jump table, a dynamic stack
adjustment or recursion. It leaves IT-controlled execution unresolved too. Generated code is not in
the ELF, so it needs execution coverage of its own.

To record stack use during the image checks:

```sh
uv run tools/image.py build/mculink_cjtag.elf \
    --core1 build/mculink_cjtag_core1.elf \
    --stack-report stack-observed.json
```

This samples SP at every instruction executed, generated code included. Each entry starts on a
fresh test stack. The figures leave out its callers and exception entry. They describe the cases
that ran: a path that did not run can use more stack. The peripheral and endpoint models have the
same limits here as in the ordinary image checks.

CPU 0's budget has to cover interrupted foreground code, nested handlers and their exception frames,
the DMA handler's energy integration among them. CPU 1's normal command path enables no peripheral
interrupt, but a fault still needs room for an exception frame.

The gate checks the linked reservations, and tests the analyser against assembled fixtures. Neither
a passing gate nor the stack use observed gives a worst-case bound for the whole program.
