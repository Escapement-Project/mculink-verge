# Runtime code generation

Every SWD, JTAG and cJTAG transfer runs on code the probe generates at runtime. This page says how:
what the two generators share, the SWD generator, and the JTAG generator, which draws both JTAG and
cJTAG. TIMING.md has the timing rules they place instructions by: read it first. CJTAG.md has the
cJTAG wire, and the [measurement log](MEASUREMENTS.md) the evidence.

## What the generators share

- `src/gen.c` holds the schedule and the emitter (`src/gen.h`). A schedule is instructions placed
  on core cycles, sorted, at most one a cycle. The emitter writes a schedule out as code, with a
  filler or a counted loop in every gap. Each generator decides where its own instructions go. They
  run one at a time and share one event workspace.
- SWD and JTAG share SRAMX, from 0x0400_0000: SWD takes the lower 16 KB, and JTAG all 32 KB, since
  cJTAG's transfers on a long frame or chain do not fit beside the scans in the lower half, OScan6's
  at most clocks. A lab build keeps JTAG to the lower half too, for the code lab's arena. The two are never in use at once, so each generates its
  code again when it finds the other's there (`gen_owner`).
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
  AHB port. A sample placed after the edge that ends its bit's window reads the next bit if it is
  held past the window. So both generators keep this many cycles clear at such a window's end.
  `src/energy_monitor.c` sets the stall while a capture is asked for.
- Latency: a connection measures the target's read latency on the wire (`src/swd_sweep.c`,
  `src/jtag_sweep.c`, the search in `src/latency.h`). docs/wires.md says when. Vendor `0x89` and
  `0x8A` set, pin or measure it.
- `SWD_CODEGEN` and `JTAG_CODEGEN` build the two generators in. Both are on by default, and CMake
  refuses one without the other. With both off, the image runs ARM's paths alone: the reference the
  gate holds the generated paths to.
- A product image has no ARM path to fall back to. When generation fails, a transfer reports a
  protocol error with no transfer done, and `DAP_JTAG_Sequence`, `DAP_JTAG_IDCODE` and
  `DAP_WriteABORT` answer `DAP_ERROR`, before the wire moves (docs/host-interface.md).
- `tools/gen_host.py` builds the generators away from the firmware, under ASan and UBSan with gcc
  and clang and for QEMU's Cortex-M33, and requires every build to generate identical code.

## The SWD generator (src/swd_gen.c)

- `swd_gen_build(config)` writes two functions, read and write, into SRAMX
  0x0400_0000..0x0400_3FFF (the lab arena is the upper 16 KB). Each is
  `uint64_t fn(packet, words, count)`. It runs `count` transfers of one request back to back, and
  stops at the first acknowledge that is not OK. It returns the acknowledge in r0 (ERROR for a read
  parity mismatch), and in r1 the transfers not completed, the failing one included. `packet` is
  `swd_gen_packet(request)`: bit n is clocked on clock n, from start to park.
- C keeps ARM's command logic (`src/swd_dp.c`): posted AP reads, the RDBUFF read that ends a block,
  the write block's RDBUFF check, WAIT retries and timestamps. `transfer_words` retries the failing
  transfer with count 1, ARM's `retry--` semantics. A timestamp is captured after the transfer, and
  only on OK. DAP_Transfer reuses the generated `swd_transfer.h` macro, with `SWD_Transfer` as one
  generated transfer. When generation fails, `refused_words` stands in under the same contract: no
  wire, and DAP_TRANSFER_ERROR. A product image has no ARM transfer to fall back to.
- When: `swd_gen_ready()` compares (period, latency, turnaround, data_phase, idle, stall) with the
  last build. `dap_dispatch.c` captures DAP_SWJ_Clock's Hz (ARM keeps only a delay), and calls it
  after SWJ_Clock, SWD_Configure and TransferConfigure, so generation never lands inside a transfer.
  `energy_monitor.c` sets the stall when a capture is asked for or stopped, on CPU 1 between
  commands, so the next transfer regenerates before it runs. `swd_gen_builds` and
  `swd_gen_failures` count the outcomes; read them in the emulator.
- Periods: the requested period is P = ceil(CPU/Hz). Clocks the probe drives (packet, write data)
  run at max(P, 3), with a low phase of 2 at P = 3 and P/2 otherwise. Clocks the target drives,
  turnaround clocks and idle clocks run at max(P, 4), with a low phase of P/2. No clock is ever
  shorter than asked: stretches only lengthen. P is at most 2^24. DAP_SWJ_Clock refuses slower
  clocks (8 Hz and below at 150 MHz), since no ARM path runs them in a product image.
- Stall (`gen_stall` in gen.h) is 2 while energy capture runs. DMA0's reads of ADC0 share AHB port
  10 with the secure GPIO, and hold a sample load up to 2 cycles (BUSES.md). A load before the next
  rising edge holds that edge back with it. One after it reads the next bit if it is held past its
  window. So a sample after the next rising edge keeps `stall` cycles from its window's end, and the
  served latencies count it. The read period starts at stall + 3 and grows, by up to 3 more and in
  either split of an odd period, until the latency keeps a cycle inside what the build serves. A = 7
  reads at P = 5 (30 MHz): 244 cycles a transfer at a 37.5 MHz request, against 210 without. The
  latency measurement builds without a stall, since it holds the conversions. On a board that can
  capture, a connection measures an unmeasured target at every masked period. A capture starting or
  stopping changes which latencies the build serves, and only a measured latency is served by both.
- Clock: a fall store, then a rise store, `str r5, [r9, #SET]`. Falls the probe drives are MPIN
  stores that carry SWDIO (MASK0 = ~(SWCLK|SWDIO), set at build; nothing else uses MPIN), so data
  changes with the fall. The trace order, W then v, matches ARM's, and the data's setup is the low
  phase and its hold the high phase. Falls the target drives are CLR stores: they must not touch
  SWDIO's latch, since the enable shows its level.
- Samples: a sample runs masked when the read period is under A + 6. It then goes at the centre of
  [A, P+A-1], and may follow the next rise. `msr primask` masks at the body's start, and the gap
  between transfers unmasks, so pending IRQs run between transfers. Otherwise a sample runs
  unmasked, as late as possible before the next rise: its early margin, P-1-A, is at least 5, and
  its late margin is A.
- Registers: r0 packet; r1 words (post-increment); r2 SWDIO mask (= 4, MPIN 1); r3 TXEN mask;
  r4 count; r5 1 (SWCLK mask, B register 1); r6 0; r7 delay loops; r8 GPIO (B registers); r9 MPIN0
  (SET +0x80, CLR +0x100, DIRSET +0x200, DIRCLR +0x280); r10 word; r11 acknowledge and temporary;
  r12 and lr samples and temporaries. Packet extraction alternates its temporaries between r11 and
  r12, so it never overwrites a register a store read the cycle before. Write data's temporaries
  are r12 and r11, with the parity folded into lr in free slots.
- Scheduler: (cycle, instruction) events, kept sorted in the shared workspace: 256 events in a
  SWD-only build, 1280 with JTAG. `clock()` moves a clock later until both its edge cycles are free.
  `op()` places an instruction before the next clock, and pushes `t`. `side()` places one that must
  not delay a clock: it avoids the grid of the clocks still to come. `sample()` picks the free cycle
  with the most margin. Emission fills a gap with `mov.w r6, r6`, or with
  `movw r7,#n; subs.w; bne.w` (3n cycles). Tails (WAIT, FAULT, protocol error, long runs of idle
  clocks) are sequential code, with clock loops on r12.
- Two placement rules keep the acknowledge's branch right. Its `cmp` sits in the cycle before the
  `bne`, so no delay loop between them can clobber the flags. And the branch comes after the last
  rising edge both outcomes share, never before the last turnaround clock.
- Checks: `tools/equiv.py` holds the generated transfers to ARM's over every SWD block and
  `DAP_Transfer` case, ignoring sample positions. `tools/gencheck.py` runs the engine against a
  timed target at every period from 3 to 40 cycles and at 75, 150 and 1500, and reports each
  sample's margin. The gate runs generated images on the engine against `tools/baseline`, and ARM's
  against `tools/baseline-armwire`, since their sample positions differ.

## The JTAG generator (src/jtag_gen.c)

One generator draws both JTAG and cJTAG; `src/jtag_gen.c`'s opening comment has the design in brief.

- The generated scan function is 32 blocks of straight-line code, one JTAG clock each, entered at
  the block that leaves `count` of them to run. Every block has the same instructions in the same
  cycles, so the entry is a multiply away and no clock pays for a loop. OScan5's and OScan6's EOT
  frames are functions of one block. A transfer function runs `count` transfers of one request.
- JTAG: a clock is a store to the masked port register that lowers TCK and sets TDI together, then
  a rising-edge store. TMS is set once for the call. TDO changes `latency` cycles after every fall,
  so a bit is readable from then until one cycle less than the latency past the next fall. A short
  clock samples after that next fall, no more than 5 cycles past it, and runs masked. A build that
  measures the latency samples before the next fall, on as long a clock as that takes: a sample
  after it reads right only while TDO holds the bit, so a target with a shorter latency would read
  wrong there too, and the search would take it for a longer one.
- cJTAG: a clock is a frame of three TCKC packets: nTDI and TMS from the probe, then TDO from the
  target. TMSC moves only while TCKC is low (CJTAG.md). The probe lets go right after the fall that
  starts the TDO packet, buffer first, and takes the wire back right after the next fall, pin first.
  The sample goes just before that fall. Interrupts are masked only around the release, or for the
  whole call where a frame has no cycle to spare.
- No clock or packet is shorter than the period, and neither phase is shorter than its share.
- The JTAG generator honours the same stall on JTAG. A sample after the next fall keeps it clear of
  the end of TDO's hold (next fall + the latency less one, at most 6, less MARGIN), and the clock
  lengthens where nothing fits.
  `jtag_gen_info.stall_moves` says a build may change. A JTAG connection starts from the default
  latency, where every such build already leaves under CONNECT_MARGIN, so it measures there
  (tools/jtag_latency_test.c, test_stall_connect). cJTAG layouts ignore the stall: their samples
  come before the fall that ends their window, which the gate's `jtagcheck --stall 2` checks on
  every cJTAG format.
- Learned clocks are kept per wire, for the configuration they were learned for, and come back
  when the host does.

### OScan2

- OScan2 is wire `JTAG_GEN_OSCAN2` (2), beside JTAG and OScan1. `jtag_gen_wire()`
  (src/cjtag_dp.c) picks it from the framing. `JTAG_GEN_PINS(wire)` indexes the latency, since both
  cJTAG formats read the same TDO packet. `emit_tms` generates `jtag_gen_tms`: `count` one-packet
  frames with TMS held (fall, TMS store, rise), never masked, since nothing is released. It goes
  after the scan function, for OScan2 only, so OScan1's SRAMX layout is the same as without it.
  `frames_oscan2` (src/jtag_gen_dp.c) splits a held-TMS run where the tracked TAP enters or leaves
  Shift. Transfers lay Select-DR, Capture-DR, the Capture-to-Shift clock, Update-DR, Run-Test/Idle
  and the idle clocks as one-packet frames (`cx_tms_frame`, with the take-back after a released
  frame), and so do their failure path and the idle loop. The connect-time latency sweep and vendor
  0x8A's measurement run in OScan2 too.

### OScan5

- OScan5 is wire `JTAG_GEN_OSCAN5` (3). Outside Shift a frame is OScan1's three packets, so the
  scan function is OScan1's. In Shift, `jtag_gen_shift` frames nTDI and TDO alone (TMS 0).
  `jtag_gen_eot`, one block, is the frame that leaves with an EOT escape: two stores of TMSC's mask
  to the GPIO NOT register with TCKC high, each a period after the rise or the last edge, and the
  fall a period after that. The escape needs no data-dependent register. `layout_two_pin` and the
  transfers' `cx_shaped` take the frame's shape (three packets, Shift, EOT). `frames_oscan5`
  (src/jtag_gen_dp.c) splits scans by the tracked TAP, as OScan2's does. Transfers walk from
  Run-Test/Idle: bypass, request and data bits go in Shift as two-packet frames, the last with the
  escape. A failing acknowledge leaves by `slow_eot_frame`. `tools/jtagcheck.py` holds the escape's
  spacing: a second escape edge one cycle after the first fails it.

### OScan6

- OScan6 is wire `JTAG_GEN_OSCAN6` (4). It takes OScan2's TMS packets (`jtag_gen_tms`) outside
  Shift, and OScan5's `jtag_gen_shift` and `jtag_gen_eot` in it. The first Shift frame after one
  outside it carries the PAD. `frames_oscan6` (src/jtag_gen_dp.c) frames it through the scan
  function, as OScan1's three packets with the PAD in the TMS packet's place. Where that frame also
  leaves Shift, it goes through `jtag_gen_eot_pad` instead: one block, the escape and then the PAD.
  The PAD goes out at the frame's nTDI level, as ARM's path sends it. Transfers send their first
  Shift frame the same way. `x->d1` is the last store that reads the nTDI register, so the next
  frame's nTDI is not computed into it before the PAD goes out. `frames()` and `hold()` are
  `COMMAND_PATH`: past gcc's inlining they would land in flash behind a veneer, which the image
  check refuses.

### Checks on every format

- `tools/gen_host.py` builds every JTAG and cJTAG configuration, transfers included, four ways, and
  requires the four to agree.
- `tools/jtagcheck.py` runs each format against `TimedCjtagTarget` at every clock it checks.
- `tools/equiv.py` holds each format's blocks, transfers and sequences to ARM's path for that
  format. The gate runs all of them for both compilers.

### Learned pacing

- After each DAP_Transfer or DAP_TransferBlock that met a WAIT, the generated transfers get one
  more Run-Test/Idle clock after every transfer (`jtag_gen_paced`, up to 32), regenerated between
  commands. A change of clock, latency, chain position or host idle starts again from none. A
  target that never answers WAIT sees ARM's wire. Vendor 0x8B `mode` (0 off, 1 on, anything else a
  query) answers `status, on, paced`. equiv.py turns pacing off.
- There is no WAIT retry inside the generated code: with pacing there is next to nothing left for
  it to speed up.

## CMSIS-DAP semantics the generated paths must keep

### SWD (ARM's SW_DP.c and DAP.c)

- Request byte: bit0 APnDP, bit1 RnW, bit2 A2, bit3 A3, bit4 MATCH_VALUE, bit5 MATCH_MASK,
  bit7 TIMESTAMP. Packet: start 1, APnDP, RnW, A2, A3, parity(bits0-3), stop 0, park 1.
- After park: release SWDIO (`PIN_SWDIO_OUT_DISABLE`: TXEN P0_28 low through CLR, then DIRCLR
  SWDIO). Then the turnaround, `DAP_Data.swd_conf.turnaround` clocks (1..4, from
  DAP_SWD_Configure), then 3 acknowledge bits (ack0 first; OK=1, WAIT=2, FAULT=4).
- OK and a read: 32 data bits LSB first and the parity, a turnaround, then `PIN_SWDIO_OUT_ENABLE`
  (DIRSET, then TXEN SET). A parity error answers `DAP_TRANSFER_ERROR` (0x08).
- OK and a write: a turnaround, the enable, then 32 data bits and the parity, driven by the probe.
- Then: a timestamp if requested (DWT CYCCNT into `DAP_Data.timestamp`); the idle cycles
  (`DAP_Data.transfer.idle_cycles`, SWDIO driven 0); finally SWDIO driven 1.
- WAIT or FAULT: if `swd_conf.data_phase`, clock a dummy 33-bit data phase (for a read before the
  turnaround; for a write after it, with SWDIO 0); then the turnaround, the enable, and SWDIO 1.
- Any other acknowledge (a protocol error): clock the turnaround and 33 cycles, then the enable and
  SWDIO 1.
- Retries: `DAP_Data.transfer.retry_count` on WAIT, abortable by `DAP_TransferAbort`.
  Match retries: `DAP_Data.transfer.match_retry`, `match_mask`.
- DAP_Transfer posts AP reads: the first AP read's data is discarded, and the value arrives with
  the next AP read or an RDBUFF read. In DAP_TransferBlock, a read block posts the AP read once,
  loops, and reads the last word from RDBUFF. A write block checks the last write with an RDBUFF
  read.
- Equivalence: `tools/equiv.py` drives two builds over 536 SWD block cases (every acknowledge,
  turnaround, data phase and idle cycle count) and requires identical wire events and replies. A
  generated path must pass it against ARM's build.
- Idle clocks after a transfer must drive SWDIO low. High on a rising edge is a start bit: the
  target then reads a malformed packet and needs a line reset.

### JTAG and cJTAG (ARM's JTAG_DP.c, JTAG_Transfer)

- C keeps these, as for SWD: posted reads (the DR returns the previous read's data, and RDBUFF ends
  a block), JTAG's acknowledge encoding (OK/FAULT 0b010, WAIT 0b001, mapped to DAP's), WAIT retry,
  match retry, `idle_cycles` in Run-Test/Idle, timestamps and `DAP_TransferAbort`. The generated
  function runs `count` transfers of one request and stops at the first acknowledge that is not OK.

## Pins and addresses (LPC55S69, at the non-secure addresses the image uses)

- The debug pins are on the secure GPIO block, at 0x400A_8000 (`DBGIF_GPIO`, src/board_pins.h). It
  has port 0's register layout: B (byte) registers at base + pin; W (word) at base+0x1000+4*pin;
  DIR +0x2000; MASK +0x2080; PIN +0x2100; MPIN +0x2180; SET +0x2200; CLR +0x2280; NOT +0x2300;
  DIRSET +0x2380; DIRCLR +0x2400. The ordinary GPIO at 0x4008_C000, which the straps, J8 and the
  Pro's DIO pins use, has the same layout per port. For port n, the B registers are at
  base + 32n + pin, W at base + 0x1000 + 128n + 4*pin, and SET, CLR and NOT 4n further on.
- BOARDS.md has the pins. P0_28, SWDIO's TXEN, is high while the probe drives. J8's marker pins are
  port 1, pins 0-3.
- SRAMX, 0x0400_0000-0x0400_7FFF, is executable and always clocked. The generated code takes all of
  it, or the lower 16 KB in a lab build, whose code lab arena is the upper 16 KB. Nothing else uses
  it.
  Data accesses to SRAMX go over C-AHB and would contend with fetch, so keep data in SRAM.
- DWT->CYCCNT is at 0xE000_1004, enabled by `board_timestamp_init`.
- Code executes from 0x2000_0000 and 0x0400_0000. The SAU is off, so every access is secure, at
  whichever alias.
- After writing code, run `__DSB(); __ISB();` before calling it, as code_lab.c does.

## Tools

- `tools/m33.py` is the timing engine (`Engine`, `Params`). Its defaults are the fitted values.
- `tools/cycles.py` is the emulator. `--engine` times it with m33, always on for a generated image.
  `--latency 7` adds `TimedSwdTarget`, which reports each read's `Read.early`, `late` and `margin`.
  Its `serve="wire"` returns what the timing puts on the pin, and `"intended"` keeps the run going.
  A memory hook must cover plain memory only: one that covers MMIO changes unicorn's behaviour on
  the GPIO block.
- `tools/lab.py` drives vendor 0x88 (`src/code_lab.c`, built with `-DCODE_LAB=ON` into
  `build-lab/`): LAB_INFO, WRITE, READ, RUN and EDGES. Arena 0 is a 4 KB SRAM buffer, arena 1 is
  SRAMX. `HardwareLab` and `EmulatedLab` share an interface (write, run, arenas), so the same blob
  runs in both. The `timed(body)` template measures a body with CYCCNT; use `drained=True` cases to
  avoid the PPB boundary. `swd_read_asm` (the sample within its clock) and `swd_read_pipelined`
  (the sample at a delay from the launching edge, stored to a buffer) are working generator
  prototypes.
- `tools/dap.py` has `swd_reset` (a line reset, then DPIDR), `limit`, `block` and `sample`
  (vendor 0x87).
