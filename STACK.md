# Stack checks

CPU0 reserves 4 KiB at the top of SRAM0. CPU1 uses SRAM4's 16 KiB. The linker defines both
ranges, and the image checks verify their limits. CPU0's link also refuses overlap with data
and the heap.

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
