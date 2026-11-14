# Runtime code generation

Every SWD, JTAG and cJTAG transfer runs on code the probe generates at run time. This page is how:
what the two generators share, the SWD generator, and the JTAG generator, which draws both JTAG and
cJTAG. TIMING.md has the timing rules they place instructions by: read it first. CJTAG.md has the
cJTAG wire, and the [measurement log](MEASUREMENTS.md) the evidence.

## What the generators share

- `src/gen.c` holds the schedule, instructions placed on core cycles, sorted, at most one a cycle,
  and the emitter that writes a schedule out as code with a filler or a counted loop in every gap
  (`src/gen.h`). Each generator decides where its own instructions go; they run one at a time and
  share one event workspace.
- SWD and JTAG share SRAMX's lower 16 KB, 0x0400_0000..0x0400_3FFF; the code lab's arena is the
  upper 16 KB. They are never in use at once, so each generates again when it finds the other's
  code there (`gen_owner`).
- When: most settings arrive in a command of their own: `DAP_SWJ_Clock`, `DAP_SWD_Configure`,
  `DAP_TransferConfigure`, `DAP_JTAG_Configure`, cJTAG's scan format (`0x95`), a read latency set or
  measured through `0x89` or `0x8A`, and pacing turned on or off through `0x8B`. CPU 1 generates the
  new code as that command runs, for the wire in use (`0x89` and `0x8A` for their own wire). For
  JTAG it builds only the scans then, since a host often changes the chain or the framing again
  before it transfers, and the first transfer command builds the transfers. Other changes take
  effect at the next transfer command, which checks its code before the first transfer and generates
  it again if needed: an energy capture starting or stopping, and the device on the JTAG chain a
  transfer addresses. Each idle clock JTAG's learned pacing adds is generated at the end of the
  command that met the WAIT. Either way the wire waits while the code is generated.
- Stall (`gen_stall`, `src/gen.h`): the cycles another master can hold a load on the bit paths'
  AHB port. A sample placed after the edge that ends its bit's window reads the next bit if held
  past the window, so both generators keep this many cycles clear at such a window's end.
  `src/energy_monitor.c` sets it while a capture is asked for.
- Latency: a connection measures the target's read latency on the wire (`src/swd_sweep.c`,
  `src/jtag_sweep.c`, the search in `src/latency.h`): at a fast clock, when its first read fails,
  when the host changes the clock, and on a capture board when a capture changes which latencies
  the code serves. It measures only
  while the latency is automatic; vendor `0x89` and `0x8A` set, pin or measure it.
- `SWD_CODEGEN` and `JTAG_CODEGEN` build the two generators in. Both are on by default, and CMake
  refuses one without the other. With both off, the image runs ARM's paths alone: the reference the
  gate holds the generated paths to.
- A product image has no ARM path to fall back to. When generation fails, a transfer reports a
  protocol error with no transfer done, and `DAP_JTAG_Sequence`, `DAP_JTAG_IDCODE` and
  `DAP_WriteABORT` answer `DAP_ERROR`, before the wire moves (docs/host-interface.md).
- `tools/gen_host.py` builds the generators away from the firmware, under ASan and UBSan with gcc
  and clang and for QEMU's Cortex-M33, and requires every build to generate identical code.

## The SWD generator (src/swd_gen.c)

- `swd_gen_build(config)` writes two functions into SRAMX 0x0400_0000..0x0400_3FFF (lab arena is
  the upper 16 KB): read and write, each `uint64_t fn(packet, words, count)`. Runs `count`
  transfers of one request back to back, stops at the first non-OK ack; returns ack (ERROR for a
  read parity mismatch) in r0 and transfers not completed (failing one included) in r1. `packet`
  = `swd_gen_packet(request)`: bit n clocked on clock n (start..park).
- C keeps ARM's command logic (`src/swd_dp.c` codegen section): posted AP reads, the RDBUFF read
  ending a block, write-block RDBUFF check, WAIT retries (`transfer_words` retries the failing
  transfer with count 1, ARM's `retry--` semantics), timestamps (captured after the transfer;
  only OK). DAP_Transfer reuses the generated `swd_transfer.h` macro with `SWD_Transfer` = one
  generated transfer. When generation fails, `refused_words` stands in under the same contract:
  no wire, and DAP_TRANSFER_ERROR. A product image has no ARM transfer to fall back to.
- When: `swd_gen_ready()` compares (period, latency, turnaround, data_phase, idle, stall) with the
  last build; `dap_dispatch.c` captures DAP_SWJ_Clock's Hz (ARM keeps only a delay) and calls it
  after SWJ_Clock / SWD_Configure / TransferConfigure, so generation never lands inside a transfer.
  `energy_monitor.c` sets the stall when a capture is asked for or stopped, on CPU 1 between
  commands, so the next transfer regenerates before it runs.
  `swd_gen_builds` / `swd_gen_failures` count outcomes (read them in the emulator).
- Periods: requested P = ceil(CPU/Hz). Probe-driven clocks (packet, write data) at max(P, 3), low
  = 2 at P = 3 else P/2; target-driven, turnaround and idle clocks at max(P, 4), low = P/2. No
  clock is ever shorter than asked; stretches only lengthen. P <= 2^24: DAP_SWJ_Clock refuses
  slower clocks (8 Hz and below at 150 MHz), since no ARM path runs them in a product image.
- Stall (`gen_stall` in gen.h, 2 while energy capture runs): DMA0's reads of ADC0 share AHB port
  10 with the secure GPIO and hold a sample load up to 2 cycles (BUSES.md). A load before the next
  rising edge holds that edge back with it; one after it reads the next bit if held past its
  window. So a sample after the next rising edge keeps `stall` cycles from its window's end, and
  the served latencies count it. The read period starts at stall + 3 and grows (up to 3 more,
  either split of an odd period) until the latency keeps a cycle inside what the build serves:
  A = 7 reads at P = 5 (30 MHz), 244 cycles a transfer at a 37.5 MHz request against 210 without.
  The latency measurement builds without a stall: it holds the conversions. On a board that can
  capture, a connection measures an unmeasured target at every masked period: a capture starting
  or stopping changes which latencies the build serves, and only a measured one is served by both.
- Clock = fall store + rise store `str r5, [r9, #SET]`. Probe-driven falls are MPIN stores
  carrying SWDIO (MASK0 = ~(SWCLK|SWDIO), set at build; nothing else uses MPIN), so data changes
  with the fall: trace order W-then-v matches ARM's, and setup = low, hold = high. Target-driven
  falls are CLR stores (they must not touch SWDIO's latch: the enable shows its level).
- Samples: masked iff read period < A + 6 (centre of [A, P+A-1], may follow the next rise;
  `msr primask` at body start, unmask in the gap so pending IRQs run between transfers); else
  unmasked, as late as possible before the next rise (early margin P-1-A >= 5, late margin A).
- Registers: r0 packet, r1 words (post-inc), r2 SWDIO mask (=4, MPIN 1), r3 TXEN mask, r4 count,
  r5 1 (SWCLK mask, B-register 1), r6 0, r7 delay loops, r8 GPIO (B regs), r9 MPIN0 (SET +0x80,
  CLR +0x100, DIRSET +0x200, DIRCLR +0x280), r10 word, r11 ack/temp, r12 + lr samples/temps.
  Packet extraction temps alternate r11/r12 (never overwrite a register a store read the cycle
  before); write data temps r12/r11, parity folded into lr in free slots.
- Scheduler: sorted (cycle, insn) events (the shared workspace: 256 in a SWD-only build, 1280 with
  JTAG); `clock()` bumps a clock until
  both edge cycles are free; `op()` = before the next clock (pushes `t`); `side()` = must not
  delay a clock (avoids the grid of the clocks still to come); `sample()` picks the max-margin
  free cycle. Emission fills gaps with `mov.w r6, r6` or `movw r7,#n; subs.w; bne.w` (3n cycles).
  Tails (WAIT/FAULT/protocol error, long idle runs) are sequential code with clock loops on r12.
- Two placement rules keep the acknowledge's branch right: its `cmp` sits in the cycle before the
  `bne`, so no delay loop between them can clobber the flags, and the branch comes after the last
  rising edge both outcomes share, never before the last turnaround clock.
- Checks: `tools/equiv.py` holds the generated transfers to ARM's over every SWD block and
  `DAP_Transfer` case, ignoring sample positions. `tools/gencheck.py` runs the engine against a
  timed target at every period from 3 to 40 cycles and at 75, 150 and 1500, and reports each
  sample's margin. The gate runs generated images on the engine against `tools/baseline`, and ARM's
  against `tools/baseline-armwire`: their sample positions differ.

## The JTAG generator (src/jtag_gen.c)

One generator draws both JTAG and cJTAG; `src/jtag_gen.c`'s opening comment has the design in brief.

- The generated scan function is 32 blocks of straight-line code, one JTAG clock each, entered at
  the block that leaves `count` of them to run. Every block has the same instructions in the same
  cycles, so the entry is a multiply away and no clock pays for a loop. OScan5's and OScan6's EOT
  frames are functions of one block. A transfer function runs `count` transfers of one request.
- JTAG: a clock is a store to the masked port register that lowers TCK and sets TDI together, then
  a rising-edge store; TMS is set once for the call. TDO is readable from `latency` cycles after the
  fall that launched it until a few cycles past the next fall, so a short clock samples it after
  that next fall and runs masked.
- cJTAG: a clock is a frame of three TCKC packets, nTDI and TMS from the probe, then TDO from the
  target, and TMSC moves only while TCKC is low (CJTAG.md). The probe lets go right after the fall
  that starts the TDO packet, buffer first, and takes the wire back right after the next fall, pin
  first. The sample goes just before that fall. Interrupts are masked only around the release, or
  for the whole call where a frame has no cycle to spare.
- No clock or packet is shorter than the period, and neither phase shorter than its share.
- The JTAG generator honours the same stall on JTAG: a sample after the next fall keeps it
  clear of TDO's hold (next fall + 6, less MARGIN), and the clock lengthens where nothing fits.
  `jtag_gen_info.stall_moves` says a build may change. A JTAG connection starts from the default
  latency, where every such build already leaves under CONNECT_MARGIN, so it measures there
  (tools/jtag_latency_test.c, test_stall_connect). cJTAG layouts ignore the stall: their samples
  come before the fall that ends their window, which the gate's `jtagcheck --stall 2` checks on
  every cJTAG format.
- Learned clocks are kept per wire, for the configuration they were learned for, and come back
  when the host does.

### OScan2

- Wire `JTAG_GEN_OSCAN2` (2) beside JTAG and OScan1; `jtag_gen_wire()` (src/cjtag_dp.c) picks it
  from the framing, `JTAG_GEN_PINS(wire)` indexes the latency (both cJTAG formats read the same TDO
  packet). `emit_tms` generates `jtag_gen_tms`, `count` one-packet frames with TMS held (fall, TMS
  store, rise; never masked, since nothing is released), after the scan function, for OScan2 only,
  so OScan1's SRAMX layout is unchanged. `frames_oscan2` (src/jtag_gen_dp.c) splits a held-TMS run
  where the tracked TAP enters or leaves Shift. Transfers lay Select-DR, Capture-DR, the
  Capture-to-Shift clock, Update-DR, Run-Test/Idle and the idle clocks as one-packet frames
  (`cx_tms_frame`, with the take-back after a released frame); their failure path and the idle loop
  too. The connect-time latency sweep and vendor 0x8A's measurement run in OScan2 too.

### OScan5

- Wire `JTAG_GEN_OSCAN5` (3). Outside Shift a frame is OScan1's three packets, so the scan function
  is OScan1's. In Shift, `jtag_gen_shift` frames nTDI and TDO alone (TMS 0), and `jtag_gen_eot`, one
  block, the frame that leaves with an EOT escape: two stores of TMSC's mask to the GPIO NOT
  register with TCKC high, each a period after the rise or the last edge, the fall a period after
  that. No data-dependent register is needed for the escape. `layout_two_pin` and the transfers'
  `cx_shaped` take the frame's shape (three packets, Shift, EOT); `frames_oscan5`
  (src/jtag_gen_dp.c) splits scans by the tracked TAP as OScan2's does. Transfers walk from
  Run-Test/Idle: bypass, request and data bits in Shift as two-packet frames, the last with the
  escape; a failing acknowledge leaves by `slow_eot_frame`. `tools/jtagcheck.py` holds the escape's
  spacing: a second escape edge one cycle after the first fails it.

### OScan6

- Wire `JTAG_GEN_OSCAN6` (4) takes OScan2's TMS packets (`jtag_gen_tms`) outside Shift and OScan5's
  `jtag_gen_shift` and `jtag_gen_eot` in it. The first Shift frame after one outside it has the PAD:
  `frames_oscan6` (src/jtag_gen_dp.c) frames it through the scan function, OScan1's three packets
  with the PAD in the TMS packet's place, or through `jtag_gen_eot_pad` (one block, the escape then
  the PAD) where it also leaves. The PAD goes out at the frame's nTDI level, as ARM's path sends it.
  Transfers send their first Shift frame the same way. `x->d1` is the last store that reads the
  nTDI register, so the next frame's nTDI is not computed into it before the PAD goes out.
  `frames()` and `hold()` are `COMMAND_PATH`: past gcc's inlining they would land in flash behind a
  veneer, which the image check refuses.

### Checks on every format

- `tools/gen_host.py` builds every JTAG and cJTAG configuration, transfers included, four ways, and
  requires the four to agree.
- `tools/jtagcheck.py` runs each format against `TimedCjtagTarget` at every clock it checks.
- `tools/equiv.py` holds each format's blocks, transfers and sequences to ARM's path for that
  format. The gate runs all of them for both compilers.

### Learned pacing

- After each DAP_Transfer or DAP_TransferBlock that met a WAIT, the generated transfers get one
  more Run-Test/Idle clock after every transfer (`jtag_gen_paced`, up to 32), regenerated between
  commands; a change of clock, latency, chain position or host idle starts again from none.
  A target that never answers WAIT sees ARM's wire. Vendor 0x8B `mode` (0 off, 1 on, else query)
  answers `status, on, paced`; equiv.py turns it off.
- There is no WAIT retry inside the generated code: with pacing there is next to nothing left for
  it to speed up.

## CMSIS-DAP semantics the generated paths must keep

### SWD (from src/swd_dp.c, ARM's SW_DP.c/DAP.c)

- Request byte: bit0 APnDP, bit1 RnW, bit2 A2, bit3 A3, bit4 MATCH_VALUE, bit5 MATCH_MASK,
  bit7 TIMESTAMP. Packet: start 1, APnDP, RnW, A2, A3, parity(bits0-3), stop 0, park 1.
  `SWD_WithParity` stores the packet parity in request bit 6.
- After park: release SWDIO (`PIN_SWDIO_OUT_DISABLE`: TXEN P0_28 low via CLR, then DIRCLR
  SWDIO), turnaround = `DAP_Data.swd_conf.turnaround` clocks (1..4, from DAP_SWD_Configure;
  `SWD_Turnaround()` asserts the range), then 3 ack bits (ack0 first; OK=1, WAIT=2, FAULT=4).
- OK + read: 32 data bits LSB first + parity, turnaround, `PIN_SWDIO_OUT_ENABLE` (DIRSET then
  TXEN SET). Parity error -> `DAP_TRANSFER_ERROR` (0x08).
- OK + write: turnaround, enable, 32 data bits + parity (driven by probe).
- Then: timestamp if requested (DWT CYCCNT -> `DAP_Data.timestamp`); idle cycles
  (`DAP_Data.transfer.idle_cycles`, SWDIO driven 0); finally SWDIO driven 1.
- WAIT/FAULT: if `swd_conf.data_phase`, clock a dummy 33-bit data phase (read: before the
  turnaround; write: after it with SWDIO 0); turnaround; enable; SWDIO 1.
- Any other ack (protocol error): clock turnaround + 33 cycles, enable, SWDIO 1.
- Retries: `DAP_Data.transfer.retry_count` on WAIT, abortable by `DAP_TransferAbort`.
  Match retries: `DAP_Data.transfer.match_retry`, `match_mask`.
- DAP_Transfer posts AP reads (first AP read's data discarded; the value arrives with the next
  AP read or an RDBUFF read). DAP_TransferBlock: read block posts the AP read once, loops,
  reads the last word from RDBUFF (peeled into `SWD_TransferTail*`); write block checks the last
  write with an RDBUFF read.
- Equivalence: `tools/equiv.py` drives two builds over 536 SWD block cases (every ack, turnaround, data
  phase, idle cycles) and requires identical wire events and replies. A generated path must
  pass it against ARM's build.
- Idle clocks after a transfer must drive SWDIO low: high on a rising edge is a start bit, the
  target reads a malformed packet and needs a line reset.

### JTAG and cJTAG (ARM's JTAG_DP.c, JTAG_Transfer)

- Posted reads (the DR
  returns the previous read's data; RDBUFF ends a block), JTAG ack encoding (OK/FAULT 0b010,
  WAIT 0b001, mapped to DAP's), WAIT retry, match retry, `idle_cycles` in Run-Test/Idle,
  timestamps, `DAP_TransferAbort`. C keeps these, as for SWD; the generated function runs
  `count` transfers of one request and stops at the first non-OK ack.

## Pins and addresses (LPC55S69, at the non-secure addresses the image uses)

- The debug pins are on the secure GPIO block, at 0x400A_8000 (`DBGIF_GPIO`, src/board_pins.h),
  which has port 0's register layout: B (byte) regs at base + pin; W (word) at base+0x1000+4*pin;
  DIR +0x2000; MASK +0x2080; PIN +0x2100; MPIN +0x2180; SET +0x2200; CLR +0x2280; NOT +0x2300;
  DIRSET +0x2380; DIRCLR +0x2400. The ordinary GPIO at 0x4008_C000, which the straps, J8 and the
  Pro's DIO pins use, has the same layout per port: for port n, B regs at base + 32n + pin, W at
  base + 0x1000 + 128n + 4*pin, and SET, CLR and NOT 4n further on.
- Pins: BOARDS.md. P0_28 SWDIO TXEN is high while the probe drives. J8 marker pins: port 1,
  pins 0-3.
- SRAMX 0x0400_0000-0x0400_7FFF: executable, always clocked. The generated code takes the lower
  16 KB and the code lab's arena, in a lab build, the upper 16 KB; nothing else uses it. Data
  accesses to SRAMX go over C-AHB and would contend with fetch: keep data in SRAM.
- DWT->CYCCNT 0xE000_1004 (enabled by `board_timestamp_init`).
- Code executes from 0x2000_0000 and 0x0400_0000. The SAU is off, so every access is secure, at
  whichever alias.
- Barriers after writing code: `__DSB(); __ISB();` before calling it (code_lab.c does this).

## Tools

- `tools/m33.py`: timing engine (`Engine`, `Params`); defaults are the fitted values.
- `tools/cycles.py`: emulator. `--engine` uses m33, always on for a generated image;
  `--latency 7` adds `TimedSwdTarget` (per-read `Read.early/late/margin`; `serve="wire"` returns
  what timing puts on the pin, `"intended"` keeps the run going). Memory hook must cover plain
  memory only (an MMIO-covering hook changes unicorn's behaviour on the GPIO block).
- `tools/lab.py` + vendor 0x88 (`src/code_lab.c`, `-DCODE_LAB=ON`, build dir `build-lab/`):
  LAB_INFO/WRITE/READ/RUN/EDGES; arenas 0 = 4 KB SRAM buffer, 1 = SRAMX. `HardwareLab` and
  `EmulatedLab` share an interface (write/run/arenas), so the same blob runs in both.
  `timed(body)` template measures a body with CYCCNT (use `drained=True` cases to avoid the PPB
  boundary); `swd_read_asm` (sample within its clock) and `swd_read_pipelined` (sample at a delay
  from the launching edge, stored to a buffer) are working generator prototypes.
- `tools/dap.py`: `swd_reset` (line reset + DPIDR), `limit`, `block`, `sample` (vendor 0x87).
