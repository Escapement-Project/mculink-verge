# Stack checks

CPU0 reserves 4 KiB at the top of SRAM0. CPU1 reserves the first 16 KiB of SRAM1 (0x20010000..0x20013fff). The linker defines both
ranges, and the image checks verify their limits. CPU0's link refuses overlap with data.

CPU1's stack sits directly above CPU0's, so each core sets MSPLIM to its stack's bottom at reset
and an overflow faults instead of overwriting the memory below. CPU0's limit comes from NXP's
startup file, and its fault handler spins. CPU1 records the fault for CPU0 to report
(`tools/dap.py status`, with CFSR.STKOF set), on a fresh stack: after an overflow, MSP sits at
the limit and any push would lock the core up. The image checks read both reset handlers and
CPU1's fault handler for these writes.
Neither core reserves a heap. The gate traps entry to linked allocator functions and checks the
trap by deliberately calling each one. Serial formatting and all command/error paths exercised
by the image checks run with that trap active.

Inspect synchronous stack paths in a linked image:

```sh
uv run tools/stack_usage.py build/mculink_cjtag.elf build/mculink_cjtag_core1.elf --output stack-static.json
```

The analyzer follows direct calls and branches in Thumb code. It includes assembly and linked
library functions. A path through an indirect call, jump table, dynamic stack adjustment or
recursion has no reported bound. IT-controlled execution is also left unresolved. Generated
code needs separate execution coverage because it does not exist in the ELF.

Record stack use during image checks:

```sh
uv run tools/image.py build/mculink_cjtag.elf --core1 build/mculink_cjtag_core1.elf --packet-count 3 --stack-report stack-observed.json
```

This samples SP at each executed instruction, including generated code. Each entry starts with
a fresh test stack. The figures exclude its callers and exception entry. They describe the
executed cases; unexecuted paths can use more stack. Peripheral and endpoint models retain the
same limitations as the ordinary image checks.

CPU0's budget must include interrupted foreground code, nested handlers and their exception
frames, the DMA handler's energy integration among them. CPU1's normal command path has no
enabled peripheral interrupts; faults still need exception-frame space.

The gate checks the linked reservations and tests the analyzer against assembled fixtures.
Neither a passing gate nor observed stack use establishes a whole-program worst-case bound.
