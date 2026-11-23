# Measurement log

What the bench measured, newest first. Each entry gives the day it was measured and, where the
record has it, the build it ran on. An entry is true of that build. Where a later entry changes a
finding, the earlier one keeps its text and points to it. The other pages give the current figures
and point here for the evidence. Entries on ARM's paths describe what the `+armwire` reference build
runs. The comparisons with NXP's firmware are on the [measurements
page](https://escapement-project.github.io/verge/measurements/), and the bench itself is in
BENCH.md.

## Where the Limits figures come from

The "Limits" table in docs/wires.md gives the fastest each transport runs. A change that moves one
of its figures updates this list in the same commit.

- **SWD:** the wire on 2026-11-04, and under energy capture, in "The re-measurement on df11e14",
  below. The current firmware's SWD transfers are the same as df11e14's.
- **JTAG and cJTAG:** the emulator's timing of the generated transfers, at the latency a CC1312R1
  measures on the bench: "JTAG and cJTAG on a CC1312R1, on 3296b39", below. JTAG at 30 MHz is also
  on the 2026-11-04 wire.
- **The console and SWO:** "The console past 19.2 Mbaud, on 8a6487d" and "SWO at 30 Mbaud, on
  d4c4c41", below.
- **USB:** what the firmware's `DAP_Info` reports, at high speed and at full speed
  (docs/host-interface.md, "Full speed").
- **NXP's firmware V3.174:** mculink-stock-notes: its SWD and JTAG clocks in `wire.md`, its
  consoles in `serial.md`, its `DAP_Info` in `identity.md`. Its reads at each clock, and JTAG's
  failure from 12.5 MHz, in mculink-bench's `results/throughput/2026-11-04.md`.

## J26's pin order, on 3296b39 (2026-11-14)

The Pro CJ8C2F60A3, with the MSO's digital leads on J26 pins 5 and 4. 4,000 bytes of 0x55 at
115200 baud sent on the second serial port toggled pin 5 alone: 40,000 edges, a median bit of
8.676 µs. The same bytes sent on the first serial port moved neither pin. So the probe transmits on
pin 5, as the Pro's schematic shows, and by the schematic receives on pin 4. UM11673 section 4.3
gives the reverse.

## JTAG and cJTAG on a CC1312R1, on 3296b39 (2026-11-14)

docs/wires.md's JTAG and cJTAG figures, taken again on the current firmware. The Pro CJ8C2F60A3 ran
`3296b39` built with gcc, the firmware of `957307c`, and a CC1312R1 LaunchPad was the target, with
nothing on its lines. The emulator timed the same build.

### Connecting

Ten connections a setting after a pin reset and five after a system reset, each through verge,
and every one read right.

- JTAG measured 7 after a pin reset at 12.5, 25 and 50 MHz. cJTAG measured 7 in OScan1, OScan2,
  OScan5 and OScan6 at 25 and 50 MHz. At 12.5 MHz the default latency already fits, so the probe
  has nothing to measure. All as on 2026-11-04.
- After a system reset through probe-rs, the first JTAG connection measured 10 and each later one 7.
  On 2026-11-04 every JTAG connection after the reset measured 10. The same verge against `df11e14`
  gave one 10 and then 7, twice, so what changed is on the host's side. We did not look for what it
  is.

### The frames, in the emulator

A cJTAG build samples TDO one cycle past the latency it measured (`src/jtag_sweep.c`), so a target
that measures 7 runs at 8. A 16-word block read asked for 50 MHz, at a latency in use of 8, gives
OScan1 and OScan2 frames of 19 cycles (7.9 MHz a JTAG clock) and OScan5 and OScan6 Shift frames of
16 (9.4 MHz). Each cycle of latency adds one cycle to a frame. That holds at every latency from 4 to
16 in OScan1 and OScan5, and at each one tried in OScan2 and OScan6 (7 to 9, 13 and 15). `df11e14`
gives the same. These are the frames the 2026-11-04 wire showed, and they correct that section's
emulator reading, below.

JTAG DR scans asked for 50 MHz (`tools/jtagcheck.py`), a clock in core cycles at each latency,
without and with the capture's 2-cycle stall:

| latency | 4-5 | 6-7 | 8 | 9-10 | 11 | 12 | 13 | 14 | 15 | 16 | 17 to 64 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| no stall | 5 | 5 | 5 | 6 | 7 | 8 | 9 | 11 | 12 | 12 | latency - 4 |
| stall 2 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | not timed |

A 16-word `DAP_TransferBlock` read matches the scans without the stall, from 6 to 12. With it, at 6
or 7 it keeps 5-cycle clocks with 8 periods of 6 in about 650, and from 8 it matches the scans.

### dap.py's block reads

`tools/dap.py`'s cJTAG sweep: 300 blocks of 127 words a clock, each checked against a 1 MHz read,
five rounds interleaved by format. Every block matched. KiB/s, the median over the rounds and the
spread across them:

| format | 12.5 MHz | 25 MHz | 50 MHz |
| --- | --- | --- | --- |
| OScan1 | 331 (0.3%) | 468 (1.7%) | 569 (3.3%) |
| OScan2 | 352 (2.0%) | 487 (1.8%) | 567 (6.9%) |
| OScan5 | 420 (2.4%) | 551 (5.8%) | 599 (3.0%) |
| OScan6 | 463 (0.9%) | 565 (8.1%) | 615 (7.6%) |

At the three clocks, OScan5 reads 27, 18 and 5% faster than OScan1, and OScan2 6, 4 and 0% faster.
OScan6 reads 10, 3 and 3% faster than OScan5. From 25 MHz, OScan6 against OScan5 sits inside the
spread. docs/wires.md said OScan6 read level with OScan5 at 12.5 MHz and slower from 25.

SWD under capture was not part of this run. docs/wires.md's stretched write clocks come from
2026-11-04's wire, below, by the store hold BUSES.md describes.

## The high range's lowest point, on 957307c (2026-11-14)

957307c keeps R1, the 0.5 mA load, as a point of the 350 mA setting's high range wherever it reads
24 codes over the amplifier's floor, down from 64. The Pro CJ8C2F60A3 ran the code of `957307c` in
the 350 mA setting, with an MSPM0L1306 LaunchPad on its target supply running dio-marker: one flash,
then three restarts. Each boot was read on auto and with the high range held, 6 s at 20,000 samples
a second each, and no sample was lost.

| boot | high range's points (code at mA) | floor | R1 over the floor | idle, held high / low range |
| --- | --- | --- | --- | --- |
| 1, after the flash | 94 at 0.5, 376 at 2.00, 9,381 at 49.0, 36,638 at 191 | 56 | 38 | 1.1116 / 1.0780 mA |
| 2 | 90, 374, 9,379, 36,640 | 57 | 33 | 1.1231 / 1.0782 mA |
| 3 | 92, 377, 9,373, 36,633 | 56 | 36 | 1.1101 / 1.0781 mA |
| 4 | 90, 374, 9,383, 36,639 | 57 | 33 | 1.1231 / 1.0781 mA |

- R1 counted on every boot. The held high range read the idle 3.0 to 4.2% over the low range,
  3.6% on average, against 3.0 to 5.7% and 4.6% over ten boots without R1 earlier the same day.
- The day's sixteen earlier boots, with the jumpers untouched and three of them through the
  bootloader, calibrated within 5 codes of each other at 2 mA.
- The idle's code was the same with and without R1, 207.9 to 209.0: only the table moved.
- Bursts read 6.718 to 6.733 mA, as before.
- The stream's under-range mark sits 64 codes over the floor. Through these boots' tables that is
  0.64 to 0.66 mA.

## Energy capture on d4c4c41, the backoff and the crystal timebase (2026-11-13)

The Pro CJ8C2F60A3 in the 350 mA setting, with an MSPM0L1306 LaunchPad on its target supply
running dio-marker: an idle of 1.08 mA, with bursts of 6.7 mA. Each run started from a power cycle
of the target unless it says otherwise. No sample was lost in any run.

### On d4c4c41

- **The low range came back** within 2.12 ms of a fall at every rate from 10,000 to 100,000
  samples a second: 289 of 290 falls, medians 1.08 to 1.60 ms. At 1,000 it never came back after
  the first 0.2 s: dio-marker's idle sits in the backoff's band, and a re-arm at that rate takes
  most of the 8 ms idle, so every low phase counted as short. cdf3f6c fixed it (below).
- **A switch down, with nothing else on the target:** the reading fell from 1.079 mA to 0.260 mA
  and came back with a time constant of 0.264 ms: 4.5 µF by the charge the dip missed, 5.3 µF by
  the time constant. 208e466 read the same. With SWD reads on the target, or after a probe-rs
  flash or reset, the target drew 1.13 to 1.22 mA, with time constants of 0.265 to 0.48 ms; a power
  cycle ended it. 2026-11-04's figure of 1.13 to 0.31 mA had the reads beside it.
- **Under the mark:** a low-power dio-marker build read 166.45 µA (median) on auto. With the high
  range held, the samples under the mark read 149 to 346 µA, 180.8 µA median.
- **The stand-in's share**, from the stream header the firmware writes: 23.3% at 100,000 samples
  a second, 14.9% at 1,000.

### The backoff, on cdf3f6c

The low range came back after 290 of 290 falls at 1,000 samples a second, in a median 4.00 ms
(2.00 to 6.00), and dio-marker's low phase spent 52% on the low range, as before the backoff
existed. 10,000 to 100,000 were unchanged.

The rate effect, the mean current against the same run at 20,000 samples a second:

| samples a second | 1,000 | 5,000 | 10,000 | 40,000 | 60,000 | 100,000 |
| --- | --- | --- | --- | --- | --- | --- |
| default build | +1.09% | -0.12% | -0.07% | +0.54% | +0.78% | +1.51% |

One boot of the first cut of this build, which came up from ISP mode after a USB hub reset,
calibrated the high range 19% high at every point, and the high range read 10 to 14% low on it. ISP
flashes alone calibrated normally, three times.

The same rate twice in a row read 0.02% apart. On d4c4c41 a repeat after the 100,000 run read 0.68%
apart, and a build whose dio-marker bursts last 200 µs read 6.81% low at 1,000 samples a second.

### The crystal timebase (cdf3f6c with the change committed as 0a3392c)

The timers that start each sample, stamp the edges and clock the ADC run from PLL0, on the board's
crystal.

- **Every averaging limit holds:** at 4,546, 9,036, 17,840, 34,786, 66,254 and 100,000 samples a
  second, 10 s each, the count came within 0.007% of the timer's rate by the host's clock, the same
  at the limits as at 100,000 with room to spare. So 27 + 32 N ADC cycles a sample holds.
- **Against the MSO:** a marker pin recorded by both, 748 edges paired over 13 s. Verge ran 52.7 ppm
  slow against the MSO, with 0.006 µs rms left after the line (0.059 µs at most). On FRO_HF the same
  comparison gave 1,146 ppm the other way, and 58.7 µs rms. The host's clock agrees: the Pro's
  crystal runs about 50 ppm slow.
- **The target's rail against the MSO's x10 probe**, four rounds of 20,000, 100,000 and 1,000
  samples a second: Verge read about 10 mV (0.3%) above the MSO. Verge's own readings at one rate
  agreed within 0.04 mV across the rounds, while the MSO's wandered 3 mV.
- **Rate effect** on this build: +1.28%, -0.05, +0.01, +0.19, +0.81 and +1.41% at the rates of the
  table above. In the 50 mA setting, where dio-marker stays on the high range and nothing switches,
  the same rates read within 0.09 points of each other: the 350 mA setting's rate effect is the
  range switching. At 20,000 the two settings read the same load 0.1% apart (2.1732 and 2.1710 mA).
  The 50 mA setting's high range read dio-marker's 1.08 mA idle 0.9% over the 350 mA setting's low
  range.
- **The stand-in's share** follows the conversion time, so the faster ADC clock moves it: at
  18.75 MHz it is 34% at 100,000 samples a second and 11% at 1,000, against d4c4c41's 23.3% and
  14.9%.

## SWO at 30 Mbaud, on d4c4c41 (2026-11-13)

The Pro's own CPU 0 sent an ITM counter as the trace source, loaded through the standalone
MCU-Link on the Pro's J3, which captured it. At 10, 15, 21.4, 25 and 30 Mbaud, two 3 s captures
each: every word in order, no break, no overrun reply, nothing left after the stop. At 30 Mbaud the
two captures held 9,000,699 and 9,000,702 bytes. NXP's firmware was not run on this source.

## The console past 19.2 Mbaud, on 8a6487d (2026-11-12)

4e449f8 runs a console from the 150 MHz main clock when the UART's own 96 MHz clock cannot make the
rate within 3%, and refuses a rate neither can make. The Pro CJ8C2F60A3, both ports looped back and
run at once (`tools/coverage.py console`):

- From 9600 baud to 19.2 Mbaud, the ports ran as they did on 2026-11-04.
- 10 Mbaud, refused on 2026-11-04, ran with 9.86 Mbaud of throughput. 21.4 Mbaud ran with 20.2,
  25 with 22.7 and 30 with 26.5.
- A rate neither clock makes keeps the rate before it: 2 then 31 Mbaud ran at 2.00, 10 then 40 at
  9.96.
- Linux's cdc-acm (kernel 7.3.3) does not pass the refusal on: a program setting 31 Mbaud gets
  success and reads 31,000,000 back.

## Energy capture readings behind the Pro's limits (2026-10-26 to 2026-11-05)

The Pro CJ8C2F60A3 in the 350 mA setting, on the build of each day.

- **The ADC's acquisition (2026-10-26),** with a 12 MHz ADC clock: 292 ns of acquisition read an
  unaveraged voltage 0.27% higher than one averaged 16 times, and the energy total carried that
  error. 625 ns, with the voltage and the current converted one after the other, read within 0.02%
  of 16 averages at 10,000 samples a second, at every averaging.
- **The high range's resistance (2026-10-26, 10-29 and 11-02):** it measured 0.274, 0.75 and 0.49 Ω
  at the boots on those days, against 0.26 Ω nominal, so its full scale was 366, 133 and 205 mA.
  J17's contact is in that path; it was not inspected. At 0.49 Ω, R1 read 155 codes over a floor
  of 57.
- **The capture's stall on the wire (2026-10-29):** with a capture at 100,000 samples a second, SWD
  reads at a 4-cycle clock read the next bit about once in 100 transfers, more often the more DMA0
  read, and in the emulator JTAG reads of a target that answers 10 to 13 cycles after a fall
  failed. The generated transfers have kept 2 cycles for the stall since.
- **The ADC's FIFO order (2026-10-30):** the voltage's result came before the current's in 1,600
  conversions of 1,600, with both FIFO selects alike.
- **A target the low range could not hold (2026-11-05, before the backoff):** a CC1312R1 LaunchPad
  also powered from its own USB took a steady 1.05 mA from the probe on the high range. After each
  switch down its reading dipped to 0.23 mA, then rose past 1.05 mA to the threshold in 1.4 ms, and
  the range switched about 590 times a second. The capture read 1.37 mA at 100,000 samples a second
  and 1.19 mA at 1,000. On 2026-11-10 the same board did not do it again, on the probe's supply
  alone or with its USB as well; its second supply is the likely cause.

## The re-measurement on df11e14 (2026-11-04)

Before publishing, we retook every figure the other documents give for the current firmware, and
compared JTAG and cJTAG as well as SWD. The Pro CJ8C2F60A3 ran `df11e14` built with gcc and with
clang. A CC1312R1 LaunchPad was the target for JTAG, cJTAG, SWO and the consoles, and an MSPM0L1306
LaunchPad for SWD. probe-rs 17beb8e80 was the host. mculink-bench's
`results/throughput/2026-11-04.md` has the throughput and the comparison with NXP's firmware. The
energy figures and SWO at 30 Mbaud were not part of this run.

Each run went twice. The first had nothing on the lines. The second had the logic analyser's
digital leads on them, to record the wire at 1 GS/s. The leads load the lines, so the latencies
here come from the first runs and the wire timing from the second.

### Connecting

The probe measures the target's read latency when a host connects (docs/wires.md). Ten
connections a setting, and every one read right.

- JTAG: the CC1312R1 measured 7 cycles after a pin reset, at 12.5, 25 and 50 MHz. After a system
  reset it measured 10. docs/wires.md says 11.
- cJTAG: 7 in OScan1, OScan2, OScan5 and OScan6 at 25 and 50 MHz, and 7 in OScan1 after a system
  reset. docs/wires.md says 8 for OScan1. At 12.5 MHz the default latency already fits, so the probe
  has nothing to measure.
- With the leads on, OScan1 and OScan2 measured 8 and the other formats 7.
- SWD: the L1306 measured 7 at 50, 37.5 and 30 MHz. The search took 8.4 ms at 50 MHz, 7.8 at 37.5
  and 4.6 at 30 when it started from 7, and 18.4, 17.0 and 8.2 ms from 14.
- The measurement took 3.5 to 5.9 ms of a JTAG connection and 2.9 to 4.4 ms of a cJTAG one.
  probe-rs's whole attach took a median of 68 to 75 ms in every case.

### The wire

A phase's clock below is each transfer's mean period across the phase, then the median over
transfers. The analyser samples at 1 ns, which cannot time a single 20 ns period to better than
about 15%, but a mean over a phase can.

- SWD: asked for 50 MHz, a transfer's request and write data run at 50 MHz, and its acknowledge and
  read data at 37.5 MHz. Every other clock asked for gets its step: 37.5 and 30 MHz exactly, 18.75
  MHz for 20, and 10, 5, 2 and 1 MHz exactly. gcc and clang drive the same wire. At 50 MHz a read
  has one 12-cycle period before its data, where the probe checks the acknowledge. A write has an
  11-cycle turnaround, and a 12-cycle period before its parity bit.
- JTAG: TCK runs at 30 MHz (5 cycles) when asked for 30 or 50 MHz, with the CC1312R1's latency of
  7. Asked for 20 MHz it runs at 18.75, and slower clocks asked for get their step.
- cJTAG asked for 50 MHz: the packets run at 50 MHz. With the leads' latency of 8, an OScan1 or
  OScan2 frame took 19 cycles, 7.9 MHz a JTAG clock. A two-packet OScan5 or OScan6 frame in Shift
  took 15 or 16 cycles.
  - Through probe-rs the leads moved the latency between two values: frames of 19 and 20 cycles
    appeared in OScan1, and 15 and 16 in OScan5. In the emulator a frame takes one more cycle for
    each cycle of latency, and every recorded frame matches one of the two.
  - At the clean latency of 7 the emulator gives 18 cycles in OScan1 and OScan2 (8.3 MHz) and 15
    in OScan5 and OScan6 (10.0 MHz). docs/wires.md's OScan5 frame, 16 cycles at a latency of 7, is
    a cycle long.
  - In the emulator, 51980d9's quarter-period hold (2026-11-11) leaves these 50 MHz frames as they
    were. It adds a cycle to OScan1 and OScan2 frames only when asked for 30 MHz.
  - (Later: "JTAG and cJTAG on a CC1312R1, on 3296b39", above, corrects the emulator reading.)

### Under energy capture

The Pro captured at 10,000 and 100,000 samples a second while the targets were read.

- Every read was right: 256 KiB five times in five over JTAG and cJTAG at 30 and 50 MHz, at both
  rates and without capture, and the L1306's 64 KiB of flash ten times in ten at 50, 37.5 and
  30 MHz at 100,000 samples a second.
- SWD read data runs at 30 MHz while capture runs, as docs/wires.md says. Writes stretch too, which
  docs/wires.md does not say. In about 12% of write transfers asked for 50 MHz, about three of the
  31 data periods take 4 cycles instead of 3. At slower clocks 3 to 4% of transfers take a few
  cycles longer, and 17 to 19% at 1 MHz. The capture's DMA delays a store, and a longer period is
  the safe direction.
- JTAG transfers keep 5-cycle clocks at a latency of 7, with a few periods of 6. The emulator
  agrees: a 16-word block read built for the capture's 2-cycle stall has 8 periods of 6 cycles in
  about 650, and a DR scan through DAP_JTAG_Sequence takes 6 cycles a clock (25 MHz). docs/wires.md
  says scans run at 25 MHz at a latency of 6 or 7 while capture runs. That holds for the sequence
  command, not for DAP_Transfer.
- Through probe-rs at 100,000 samples a second, a single-word read took 105 µs over JTAG against
  81 without capture, and 145 µs in OScan5 against 103. SWD reads at 50 MHz were 23% slower.

### SWO

The Pro captured the CC1312R1's ITM counter (`tools/swo_target/itm_counter.c`) at 1, 8, 12, 16 and
24 Mbaud, 3 s twice at each, with no break in the counter and no overrun. At 24 Mbaud it also ran
for 12 s with both consoles looping at 3, 12 and 19.2 Mbaud: 28.8 MB, no break. 30 Mbaud, the
UART's top rate, was not part of this run.

### The consoles

Both Pro ports at once, each looped back, returned every byte at every rate they accepted, up to
19.2 Mbaud. Throughput was 16.8 Mbaud at 19.2 (18.2 on 2026-10-21) and 11.3 at 12 (11.7). The probe
refuses 10 Mbaud: no divisor of the UART's 96 MHz clock comes within 3% of it, the nearest giving
9.6 and 10.67. The port then keeps its earlier rate without telling the host, and the loopback ran
at 0.12 Mbaud. (Later: "The console past 19.2 Mbaud, on 8a6487d", above.)

## cJTAG bring-up on a CC1312R1 (2026-10-16 to 2026-10-22)

The standalone MCU-Link and the Pro on the builds of those days, through ARM's paths and
`tools/dap.py cjtag --ti`, with a CC1312R1 LaunchPad as the target. docs/cjtag-targets.md states
what a host does with it.

- **Escape edges on ARM's SWD path (2026-10-16):** TMSC moved while TCKC was high 0.348 times a
  clock in the activation and 0.418 in a read (59 in a 141-clock read), against 0.006 and 0.000 on
  a J-Link. These edges were a strong candidate for what broke the reads, not a proven cause. The
  activation's command still landed at the same density, and the 43 edges predicted in the read
  did not match the 59 measured.
- **TDO (2026-10-17):** valid from 8 cycles (53 ns) after the fall that starts the TDO packet, at
  every period from 10 to 30 cycles, and through the rise to at least 7 cycles past the next fall.
  A high phase of one cycle was accepted. The shortest frame tried, 19 cycles a JTAG clock, read
  10 times in 10.
- **Power (2026-10-17):** ICEMelter woke the JTAG domain after 8 rising and 8 falling edges, always,
  and never after 7, with or without a reset among them. 40 µs after them was enough, against TI's
  200 µs; 10 to 20 µs was not reliable. From a half-woken domain, 8 wake clocks attached 0 times in
  3 and 20 attached 3 in 3. Test-Logic-Reset with KeepPoweredinTLR clear powered the domain off
  only if TCKC stopped there: three frames from Run-Test/Idle left it off, eight woke it again, and
  with the bit set it stayed on (2 of 2, 3 of 3, 2 of 2). AON_PMCTL's JTAGCFG register read 0x3,
  JTAG_PD_FORCE_ON clear.
- **The activation (2026-10-17):** TI's XDS110 sequence with the escape first read IDCODE, then
  routed the ICEPick and read CPUID, 3 times in 3 from a pin reset. From every starting state
  (fresh, a live OScan1 session with KeepPoweredinTLR set or clear, after a Test-Logic-Reset either
  way, a half-woken domain), with 20 and with 200 wake clocks, it read CPUID 36 times in 36.
  Without the escape, TI's sequence failed 3 times in 3 from a live OScan1 session. A
  Test-Logic-Reset inside a session dropped OScan1 every time; TI's sequence with its 200 µs wait
  recovered it 3 times in 3.
- **The ICEPick join (2026-10-17):** in one run of Run-Test/Idle clocks after the routing write, at
  5 MHz 8 clocks joined; at 10 MHz 16 failed and 20 joined; at 30 MHz 20 failed.
- **CNFG (2026-10-21):** CNFG0 read `0x03c00714` 3 times in 3, CNFG1 0 and CNFG2 0x120.
- **Other formats (2026-10-21 and 22):** STFMT 10 as an OScan1 command read CPUID, and 64-word ROM
  blocks matched the 1 MHz read at 1, 2, 5 and 10 MHz and failed from 15, ARM's OScan1 boundary
  too. In OScan5, 20 of 20 matched at every clock from 1 to 25 MHz on the standalone MCU-Link.
  After ECL, any count of raw clocks from 4 to 8 worked. ECL framed in OScan1 to a target already
  in OScan2 still read CPUID.
- **Detach and attach in OScan2 (2026-10-22):** after a detach with KeepPoweredinTLR clear, commands
  without a wake failed 3 times in 3 and a wake without an escape attached 3 in 3; with the bit
  set, commands without a wake attached 2 in 2. A target left live in OScan2 was attached by the
  escape-first sequence 12 times in 12.

## cJTAG, from ARM's paths to every generated format

Measured 2026-10-16 to 2026-10-22 on a CC1312R1 LaunchPad, first on ARM's paths and then on the
generated ones. CJTAG.md has the wire these ran on, and CODEGEN.md the generator.

### ARM's paths (2026-10-16 and 17)

- **Rates seen through the firmware:** paced frames ran clean at 1 MHz. The unpaced fast path,
  asked for 25 MHz, runs an irregular ~11 MHz (periods of 50 to 160 ns) and reads clean: 20 of 20
  64-word ROM blocks. Asked for 37.5 MHz, every transfer fails, because TDO is sampled too early
  (below).
- **Throughput on 2026-10-17:** ~230 KiB/s through `dap.py`, in 64-word blocks, USB included. It
  was flat from 15 MHz up, because the fast path is unpaced C.
- **Why the fast path fails when asked for 37.5 MHz:** it samples TDO a couple of cycles after the
  release, inside the 8-cycle latency. At 25 MHz the C between packets happened to delay the sample
  enough.

### Learned pacing (2026-10-17)

On the bench, 400 queued 127-word reads with the host's idle cycles at 0, timed with pacing off and
on:

| wire | clock | pacing off | pacing on | idle clocks learned |
| --- | --- | --- | --- | --- |
| JTAG | 50 MHz | 156.9 ms | 128.4 ms | 5 |
| JTAG | 25 MHz | 156.8 | 128.5 | 5 |
| JTAG | 10 MHz | 294.1 | 252.0 | 2 |
| cJTAG | 50 MHz | 350.7 | 301.5 | 2 |
| cJTAG | 25 MHz | 430.3 | 367.7 | 1 |
| cJTAG | 15 MHz | 530.6 | 470.5 | 1 |
| cJTAG | 10 MHz | 656.0 | 657.8 | 0: no WAITs there |

With a fixed idle of 6, JTAG at 50 MHz took 129.7 ms, so pacing finds it. The data was checked
against the 1 MHz read at every clock, over JTAG and cJTAG.

Through probe-rs, end to end, a 256 KiB read at 50 MHz took 0.33 s over JTAG and 0.56 s over
cJTAG, against 0.37 and 0.62 s without pacing. At 10 MHz it took 0.47 and 1.08 s. A 64 KiB flash
and verify took 0.67 and 0.76 s.

### OScan2 (2026-10-21 and 22)

- On the bench, the standalone MCU-Link read 127-word ROM blocks from the CC1312R1 through dap.py,
  300 a clock after 30 to let pacing learn. Two rounds agree within 1.5%:

  | MHz | OScan1 | OScan2 | OScan1 unpaced | OScan2 unpaced |
  | --- | --- | --- | --- | --- |
  | 12.5 | 330 KiB/s | 352 | 295 | 292 |
  | 25 | 464-468 | 481 | 420 | 408 |
  | 50 | 565-566 | 577-586 | 505-506 | 487-491 |

  Every block matched at 1 to 50 MHz in both formats. OScan2 is 2 to 7% faster with pacing, about
  what its packet count predicts. Pacing learns 5 idle clocks in OScan2 against 2 in OScan1: the
  CC1312's AP WAITs when a Capture-DR follows the last Update-DR too closely, and OScan2 sends
  those states in a third of the packets. Without pacing OScan2 is slower than OScan1, from the
  extra WAITs. Each learned clock regenerates the code, which costs a short run: 50 blocks right
  after a clock change put OScan2 at 323 KiB/s against OScan1's 423.
- **Attach cost (2026-10-22).** probe-rs found an OScan2 attach 13 to 38 ms slower than OScan1
  (115 to 122 against 83 ms at 12.5 MHz), while its 256 KiB reads ran 1.5 to 3.3% faster. Every
  framing switch regenerated scans and transfers: 16.2 to 17.5 ms at 12.5 MHz, 5.9 to 6.3 at 50.
  An OScan2 attach frames OScan1 first and switches. Its learned idle clocks restarted with every
  configuration change, so it relearned them each session, a regeneration a clock.

  Two changes followed. `jtag_gen_ready` builds the scans alone, and `jtag_gen_ready_transfers`
  builds the transfers when a transfer command needs them. With that, a switch costs 0.6 to
  0.8 ms. And learned clocks are kept per wire for the configuration they were learned for, and
  come back when the host does. A dap.py attach at 12.5 MHz pays one generation in either format,
  at its first transfer (17.1 ms in OScan2, 18.5 ms in OScan1). Before the changes it paid four in
  OScan2 and two in OScan1. The first OScan2 session after power-up still learns its clocks.

  Through probe-rs's CLI, 256 KiB, 5 interleaved repeats, after the changes (the attach before
  them in brackets):

  | clock | attach, OScan1 | attach, OScan2 | read, OScan1 | read, OScan2 | OScan2 faster |
  | --- | --- | --- | --- | --- | --- |
  | 12.5 MHz | 78.2 ms (79.9) | 73.7 ms (117.8) | 774 ms | 737 ms | +5.1% |
  | 25 MHz | 80.2 | 81.8 | 563 | 540 | +4.3% |
  | 50 MHz | 81.7 | 80.7 | 473 | 444 (577 KiB/s) | +6.7% |

  All 30 reads matched JTAG. A session's first transfer still pays one generation, in both
  formats.
- **Before OScan5 and OScan6 were generated,** the clock sweep of 2026-10-21 put OScan1 at 7.8 µs
  a word at 50 MHz and 9.2 µs at 25 MHz, in 127-word blocks. So about 1.3 µs of a word at 50 MHz
  scales with the clock.

### OScan5 (2026-10-22)

- **Decoded on the wire, on ARM's path,** with escape edges counted inside each TCKC high phase:
  every rise of the recording was accounted for, with 30 EOT escapes for 30 Shift exits, no TMSC
  edge with TCKC high anywhere else, and the TAP back in Run-Test/Idle.
- **On the bench,** the standalone MCU-Link attached to the CC1312R1 with dap.py's robust attach,
  then sent STFMT 13. The recording decodes as intended: 926 frames, every rise used, 30 escapes
  for 30 Shift exits and no stray edge. CPUID read, and 64-word blocks matched 20 of 20 at 1 to
  50 MHz. Rates in 127-word ROM blocks, 300 a clock after 300 to let pacing learn, with two rounds
  within 1%:

  | MHz | OScan1 | OScan2 | OScan5 |
  | --- | --- | --- | --- |
  | 12.5 | 327-329 KiB/s | 349-350 | 415-419 |
  | 25 | 471 | 477-482 | 542-546 |
  | 50 | 551-556 | 567-569 | 588-592 |

  OScan5 is 27% faster than OScan1 at 12.5 MHz, where the packets set the rate: two a Shift clock
  against three. It is 6% faster at 50 MHz, where the TDO latency sets most of a Shift frame (16
  core cycles against OScan1's 18 at latency 8) and USB starts to bound the rest.

### OScan6, and the four formats in one run (2026-10-22)

- **On the bench, on ARM's path,** the standalone MCU-Link with the robust attach read CPUID and
  the 96-bit DR pattern, and 64-word blocks matched 20 of 20 at 1 to 50 MHz. Decoded: 927 frames
  using all 2125 rises, 30 EOT escapes for 30 Shift exits, 30 PAD packets for 30 Shift entries and
  no stray edge.
- **Rates,** all four formats in one run on one image, in 127-word blocks, 300 after 300, with two
  rounds within 1%:

  | MHz | OScan1 | OScan2 | OScan5 | OScan6 |
  | --- | --- | --- | --- | --- |
  | 12.5 | 306 KiB/s | 304-305 | 392-394 | 397-398 |
  | 25 | 454-456 | 427-433 | 510-514 | 438 |
  | 50 | 519-523 | 502-511 | 560 | 547-548 |

  OScan5 is the fastest on this target from 25 MHz up, and level with OScan6 at 12.5. OScan6 and
  OScan2 send the clocks outside Shift as one packet. The CC1312's AP answers WAIT when a
  Capture-DR comes too soon after an Update-DR, so pacing adds idle clocks there. Every figure here
  is 5 to 12% below the afternoon's (the OScan5 table). The afternoon's image, measured again in
  this run, matched these within 2%, so the bench changed, not the firmware. OScan2 fell most,
  which points at the target answering WAIT more (not measured).

## SWO capture

Measured 2026-10-19 to 2026-10-21, on the dual-core image's DMA capture.

**On 2026-10-19, on the dual-core image's DMA capture (`src/swo.c`).** The target was a
LAUNCHXL-CC1312R1 over cJTAG, running `tools/swo_target/itm_counter.c`. It sends a counter on ITM
port 0 out of DIO16 (TDO) as fast as the port takes it, so every byte is checkable and the target
drops nothing. The analyser on the line read 1.0000 and 7.9999 Mbaud from back-to-back frames: the
line was saturated and the counter unbroken.

| through | rate | captured | counter |
| --- | --- | --- | --- |
| `dap.py swo`, 2 s | 1 MHz | 200,008 bytes, 100.0 KB/s | 40,000 words, no break |
| `dap.py swo`, 3 s, twice | 8 MHz | 2,400,064 and 2,400,049 bytes, 800.0 KB/s | 480,011 and 480,009 words, no break |
| `probe-rs itm swo`, debug build, 0.5 s | 1 MHz | 10,067 words after the decoder locked | no break |
| `probe-rs itm swo`, release build, 3 s | 8 MHz | 117,871 words decoded | one three-word blip, no reply lost |

`dap.py` polls with DAP_SWO_Data back to back: about 14,000 polls a second, each under the
508-byte maximum, so the host keeps ahead of the line. probe-rs decodes and prints every packet,
which a debug build does at about 70 KB/s. Behind that the probe overruns, says so, and the stream
resumes after the gap. probe-rs ignores the overrun bit and fails a read on the stream-error bit,
which a capture that starts mid-frame sets once.

**Above 8 MHz, 2026-10-21, through the Pro,** on sweep images with a raised cap. The target and
`dap.py swo` were the same. At 8 MHz the Pro first matched the standalone board's figures above
(2,400,047 and 2,400,062 bytes, no break).

| Flexcomm 3 clock | rate | alone, 3 s twice | under console load, 15 s |
| --- | --- | --- | --- |
| 96 MHz, FRO_HF_DIV | 9.6 MHz | 960.0 KB/s, no break | |
| 96 MHz | 12 MHz | 1200.0 KB/s, no break | |
| 96 MHz | 16 MHz, oversampling 6 | 1600.0 KB/s, no break | 23,999,995 bytes, no break |
| 120 MHz, main clock / 1.25 | 8 MHz | 800.0 KB/s, no break | 11,999,917 bytes, no break |
| 120 MHz | 12 MHz | 1200.0 KB/s, no break | 17,999,934 bytes, no break |
| 120 MHz | 24 MHz, oversampling 5 | 2400.0 KB/s, no break; 30 s, 71,999,402 bytes, no break | 825 breaks, 4.8% lost |

Console load: both of the Pro's consoles looped back at once, one second's worth at 1, 2 and 3
Mbaud. The 24 MHz breaks all fall in the 2 and 3 Mbaud runs. The 120 MHz clock is over the
fractional divider's 100 MHz limit (UM11126 4.5.49), and every rate above 10 MHz is over the data
sheet's asynchronous USART maximum.

**At 24 MHz in the shipped image, 2026-10-21,** with the cap at 24 MHz. `swo_uart_clock` in
`src/swo_ring.h` takes the 96 MHz clock when it makes the rate within 0.5%, and otherwise the main
clock through Flexcomm 3's fractional divider. Of these rates, only 24 comes from the divider. The
sweep and the rate switching below were repeated on the final image with the same results.

- Through the Pro, 24, 16, 12, 9.6, 8 and 1 MHz, 3 s twice each: every rate answered exactly and
  every capture unbroken.
- 24 MHz while both consoles looped back at 3, 12 and 19.2 Mbaud, 12 s twice: 28,799,849 and
  28,799,931 bytes, no break.
- One session moving 24, 8, 24, 12 MHz without a mode change: all unbroken. 30 MHz asked answers
  24.
- probe-rs (`itm swo`, release build, 3 s at 24 MHz from a 48 MHz trace clock) decoded 76,278
  packets. Its print loop was the limit, and the one stream error was the one a capture starting
  mid-frame raises.

Through the standalone MCU-Link, with the LaunchPad moved to its debug connector:

- 24, 16, 12 and 8 MHz, 3 s twice each, unbroken.
- 24, 8, 24 MHz in one session, unbroken. 30 MHz asked answers 24.
- 24 MHz for 10 s while its console looped back at 3, 12 and 19.2 Mbaud: 23,999,973 bytes, no
  break.

Its input buffer is the one part of the SWO path the two boards do not share.

**At 30 MHz, 2026-10-21, the Pro as the source.** The CC1312 stops at 24 MHz, so the Pro's own
CPU 0 sent the trace instead. The standalone MCU-Link halted it through the Pro's J3 and ran an ITM
counter from SRAM: the same stream as `itm_counter.c`. The trace clock was the 150 MHz main clock
undivided, with SWO on PIO0_10 to J3 pin 6, so the rate is 150 MHz / (ACPR + 1). A reset of the
Pro brings its firmware back. The standalone MCU-Link captured with `dap.py swo`, 3 s twice each:

- On the shipped 24 MHz image: 1, 12.5, 15, 18.75 and 21.4 MHz unbroken. 25 MHz asked answers 24,
  and the 4% between the ends breaks the stream on every reply.
- On a sweep image with the cap at 30 MHz: 21.4, 25 and 30 MHz unbroken, 30 MHz at 9,000,675 and
  9,000,009 bytes (3000 KB/s). 30 MHz for 20 s: 59,997,323 bytes, 11,999,464 words, no break.
  30 MHz for 20 s with the console looping back for 12 of them, a second each at 3, 6 and 12 Mbaud
  and then 19.2: 59,997,409 bytes, no break, and every console run identical. 37.5 MHz asked
  answers 30, with a stream error on every reply.
- On the shipped image: 30 MHz for 3 s, 9,000,468 bytes, no break. 40 MHz asked answers 30.

The analyser cannot resolve 30 MHz, about 3 samples a bit, so the evidence is the answered rate,
an unbroken counter and 3.0 MB/s together. 30 MHz is the fastest the UART's clock makes, so this
is the probe's ceiling. The Pro's input buffer has not seen anything above 24 MHz.

The emulator still cannot run capture: CPU 0's clock setup never runs there, so the 96 MHz clock
reads 0 Hz, SWO_Baudrate makes its rate from the main clock instead, and no capture is started. The
ring and rate arithmetic is tested on the host instead (`tools/swo_ring_test.py`, in the gate).

## The console

Measured 2026-10-19 to 2026-10-21.

The console passed its test on 2026-10-19, after a fix: until then it had never received a byte.

The test opens the CDC port and talks to a target's UART at a few baud rates, including 921600 and
above. It shows that `vcom_init` took Flexcomm 0 back from the board support, and that the
fractional divider reset fixed the 2.4% error at a megabaud.

Loopback, 300,000 bytes a run, a jumper across TX and RX (`tools/coverage.py console`, then one
console at a time):

| probe | consoles | clean at | lost |
| --- | --- | --- | --- |
| standalone MCU-Link, 2026-10-19 | one (J7) | 9600, 115200, 921600, 1M, 2M baud | 18 to 62 bytes at 3 Mbaud |
| MCU-Link Pro, 2026-10-20 | each alone | 115200, 1M, 2M baud; VCOM also 3M | VCOM2 one byte at 3 Mbaud |
| MCU-Link Pro, 2026-10-20 | both at once | 1 Mbaud, twice | 2 to 4% at 2 Mbaud, 3 to 6% at 3 |

The losses are RX FIFO overruns: CPU 0's loop moves console bytes one at a time, and two consoles
share that loop. NXP's firmware V3.172 was clean with both of the Pro's consoles at 3 Mbaud.

**On DMA, 2026-10-21:** each probe's consoles looped back at once, one second's worth each. By then
`tools/coverage.py console` ran every console of the probe together. It set any rate through
Linux's termios2, where pyserial's path stops at 4 Mbaud, and it timed each run to its last byte,
so a refused rate could not pass as a slower one.

| probe | consoles | clean at | through |
| --- | --- | --- | --- |
| MCU-Link Pro | both at once | 115200, 1M, 2M, 3M, 4M, 6M, 8M, 12M, 16M, 19.2M baud | 11.7 Mbaud of 12, 18.2 of 19.2 |
| standalone MCU-Link | one (J7) | 115200, 3M, 12M, 19.2M baud | 12.0 Mbaud of 12, 18.9 of 19.2 |

19.2 Mbaud is the UART's fastest from its 96 MHz clock (oversampling 5). The same image started and
stopped SWO capture 20 times at 8 MHz while both consoles streamed at 3 and 12 Mbaud: every console
byte back, every capture unbroken. With a 24 MHz SWO sweep image ("SWO capture", above) and both
consoles run to 19.2 Mbaud during the capture, 20 s of SWO lost nothing; an earlier 15 s run lost
three gaps, 7,305 bytes, all after the consoles had finished.

## DAP_Transfer ran from flash in the generated build

Measured 2026-10-16, on the generated transfers.

A host sends anything shorter than a block as one DAP_Transfer: an access under 1 KB, the address
write that opens each 1 KB window, every register poke during an attach. In the generated build
that command ran ARM's loop from flash, and each transfer re-checked the generator's
configuration with a memcmp from flash. `dap.py block --command transfer` times the same DP reads
both ways (50 MHz, 16, 64 and 120 words a command, synchronous, per word from the slope):

| | DAP_TransferBlock | DAP_Transfer |
| --- | --- | --- |
| before | 1.64 µs/word | 7.24 µs/word |
| loop in SRAM, configuration checked once per command | 1.68 | 2.60 |

The prediction was 2 to 3 µs. Through probe-rs (`swd-throughput`, same probe-rs build),
a 256-byte read at 50 MHz went from 562 µs to 251, and 64 bytes from 188 to 104; reads of 1 KB and
up did not move. The fitted fixed cost per probe-rs call fell from 205 µs to 71, so most of what
looked like a probe-rs cost was this. The default build never had it: its DAP_Transfer was in
`.ramfunc` all along, which is why the generated build was slower than it below 1 KB.

The gate missed it because it measures blocks. It also hid the next problem: once
`swd_gen_ready` had one caller, clang inlined it, and the gate, which recognised a generating
image by that function's symbol, judged the clang image against the default build's baselines.

## The USB path at 50 MHz, and what a perfect host gets

Measured 2026-10-16, on the generated transfers.

At 50 MHz the wire stopped being most of a block read. probe-rs read SRAM at 1,397 KiB/s, half
of what the wire allows, so the question became how much of the rest is the probe.

**`tools/dap.py block --queue N` is the instrument.** It queues commands through libusb's
asynchronous API, so the probe never waits on the host. A queue of 2 already saturates, deeper
queues change nothing, and the host's CPU is 17% busy at 126 words: what it measures is the
probe and the link. 50 MHz, DP reads, queue 8, three runs each, identical to 0.1%:

| build | 1 word | 32 words | 126 words |
| --- | --- | --- | --- |
| generated transfers, as before | 22,965 commands/s | 1,349 KiB/s | 2,067 KiB/s |
| + memcpy a block at a time | 23,093 | 1,429 | 2,242 |
| + TinyUSB in SRAM | 30,399 | 1,618 | 2,351 |
| clang, same, buffers aligned | 33,476 | 1,686 | 2,383 |

The configuration that shipped then (ARM's C paths, 10 MHz) gains from TinyUSB in SRAM too: 25,235
to 35,103 one-word commands a second, 717 to 729 KiB/s at 126 words.

**Where a command's time went, from the marker** (126-word blocks, before → after the two changes):

| | before | after |
| --- | --- | --- |
| clocks running | 186.1 µs | 186.5 µs |
| before the first clock | 6.1 | 6.0 |
| after the last clock | 1.8 | 1.8 |
| gap to the next command | 45.5 | 14.6 |
| period | 239.6 | 208.9 |

The generator schedules 170.5 µs of clocking for 126 reads; the other 16 µs are interrupts, which
run between transfers because the transfers mask them. Folding the gaps between transfers shows
about 2.4 per command at 4.4 µs each from SRAM, 6 µs from flash. A one-word command spends 14 µs
before its first clock, mostly those same interrupts.

**newlib-nano's memcpy was 8 cycles a byte.** It is built for size and copies a byte per
iteration, and every reply passes through it into the endpoint buffer: 26 µs for a 507-byte
reply. Ours (`src/mem.S`) moves 32 bytes per ldm/stm pair. It saved 18.5 µs of the predicted 24.

**clang left the reply buffer on a halfword**, so its replies still went through the byte path
and its 126-word figure was 7% under gcc's until the buffers were aligned.

**What is left, per 126-word command:** 14.6 µs between commands (dispatch, TinyUSB's task, the
main loop), 6 µs before the first clock (command dispatch, still in flash), and about 10 µs of
interrupts inside the block. At 50 MHz that is 15% of the period. The copy into the endpoint
buffer can go entirely: the high-speed controller reaches only USB RAM, so the slots and the reply
would have to live there and the bulk endpoints be driven without TinyUSB's vendor class.

**probe-rs, against the same probe, is a separate gap.** Within a run of blocks it keeps the probe
as busy as the perfect host does (39 µs between blocks against 45 for dap.py on the old build).
But every 1 KB, where the access port's address wraps, it sends two small commands and waits for
each reply before continuing: about 170 µs of round trips per 1 KB against 470 µs of blocks.
That, not the probe, is most of the difference between 1,397 and the 2,000 the probe gave a
script.

## The transfers are generated at runtime, and the wire runs at every period from three cycles

Measured 2026-10-16, on the generated transfers. They became the default build on 2026-10-20.

**What it is.** A build with generated SWD transfers writes its SWD transfer code into SRAMX
whenever the host changes the clock, the turnaround, the data phase or the idle cycles. Every edge,
data store and sample sits at a cycle the timing model chose. ARM's command logic stays in C around
it: posted reads, the RDBUFF reads and WAIT retries. `src/swd_gen.c` has the details and CODEGEN.md
the reasoning.

**The host now gets the rate it asks for, to one cycle.** The period is the shortest whole number
of cycles that is no faster than the request. Clocks the probe drives go down to 3 cycles, 50 MHz.
Clocks the target drives go down to 4, 37.5 MHz, because a read needs a sample and its use as well
as the two edges.

**It holds to ARM's wire.** `tools/equiv.py --ignore-samples` passes all 536 block cases at 50,
37.5, 25, 12.5, 10, 1 and 0.1 MHz, and all 360 single-register cases at 50, 37.5, 10 and 1 MHz.
Only where the samples sit differs: a sample can trail the edge that launched its bit by two clocks.

**Every period ran clean on the MSPM0L1306.** A sweep that reads DPIDR in blocks and writes
CTRL/STAT, checking 18 bits of each write, found no error at any period from 3 to 40 cycles,
or at 3, 2, 1 and 0.1 MHz. probe-rs wrote random words to SRAM and read them back correctly at
every clock from 0.1 to 50 MHz.

**Throughput through probe-rs, 64 KB of flash, same probe and same part:**

| asked | generated | previous firmware |
| --- | --- | --- |
| 50 MHz | 1407 KiB/s | fails |
| 37.5 MHz | 1312 | fails |
| 25 MHz | 1172 | fails |
| 15 MHz | 830 | fails |
| 10 MHz | 629 | 577 |
| 5 MHz | 360 | 324 |
| 1 MHz | 80 | 79 |

The previous firmware fails above 10 MHz on this part because it samples too close to the edge,
which "The cost model times the bus" found. At 37.5 MHz the wire is idle about half the time. A
running block reads a word in 210 cycles, which is 2.7 MB/s, and probe-rs gets 1.3. USB and the host
take the rest.

**The masked port register reaches the pin when CLR does.** The generator lowers the clock and
sets the data bit in one store to that register, which no earlier measurement covered. On the
analyser at the target, over 2.4 million clocks at P = 20, the low phases of clocks that fall that
way and of clocks that fall on CLR differ by 0.17 ns.

**Reads stop at 37.5 MHz while the core makes the clock.** A read clock is four instructions and
the core pairs none of them: the corpus timed a 16-bit load or store against a 16-bit add, in both
orders, at a cycle each.

**A timer can make the clock instead, and then reads run at 50 MHz.** SWCLK's pin is also
CTimer 0's match output, and in PWM mode that timer counts the core's own clock. It holds its
phase to the cycle at every period tried, from 2 to 10 cycles. With the timer making the
target's clocks, the core only loads and keeps each bit. A lab prototype read DPIDR correctly
2,000 times out of 2,000 with those clocks at 50 MHz, and the analyser shows them at exactly that
rate. Stopping the timer is the hard part. A store to any of its registers holds the core up four
cycles, which at 50 MHz always swallows one bit's sampling window, so the prototype stops the
timer with a DMA write instead, triggered by a second timer. None of this is in the generator.

## The cost model times the bus, and reproduces every SWD result so far

Measured 2026-10-16, on the code lab build and ARM's paths.

**A code lab, vendor 0x88.** A build with `-DCODE_LAB=ON` runs a function the host writes into
RAM, with interrupts masked. The function reads the cycle counter around the code under test, so
the call itself never enters the measurement. `tools/lab.py` assembles a case, runs it on the
probe and in the emulator at the same address, and compares the two. A lab build resets itself if
a function faults.

**Fetch from SRAM shares a bus with every GPIO access; fetch from SRAMX does not.** The bit paths
run from SRAM at 0x2000_0000, which the core fetches over S-AHB, the port its loads and stores
also use. SRAMX at 0x0400_0000 is fetched over C-AHB. The same code in each, every repeat
identical:

| case | SRAM | SRAMX |
| --- | --- | --- |
| 16 GPIO stores, 32-bit encodings | 32 cycles | 16 + 2 |
| 16 GPIO loads, 32-bit encodings | 32 | 16 + 2 |
| 16 stack loads, 16-bit encodings | 24 | 16 + 2 |
| 16 `mov.w` | 16 or 17, by alignment | 16 |

From SRAM every cycle carries one bus transaction, an instruction word or a data access. From
SRAMX the two run side by side. The +2 is the cycle counter's own read waiting for the last
access to finish. NOPs pair up and cost half a cycle each, so they are no use as timing filler.

**The pin round trip is seven cycles against the U575, and exact.** A DPIDR read generated as
straight-line code in SRAMX, one instruction a cycle, with the sample swept across the clock
period:

- A bit is readable when the sample load issues 7 to P + 6 cycles after the store that raised
  the clock, where P is the period. The read comes back one bit ahead at 7 cycles past its own
  edge and one bit behind at 6 cycles past the previous one, so both ends give the same number.
- No position gave a mixture of results.
- Without a target, a load sees the probe's own SWDIO store 3 cycles later. The target, its
  cable and the two buffers make up the other 4.

The window is a whole period wide at every rate. At the delay-free step's five-cycle period a
sample nine cycles after its edge would have two cycles either side. That sample comes after the
next rising edge, which a generator can place and compiled C cannot.

**The emulator now models fetch and the bus.** `tools/m33.py` decides when each instruction
issues, from its fetch, the instruction before it and the bus slots data has already taken. It is
fitted against the lab: 494 of 516 cases exact. `cycles.py` runs on it on request. A modelled SWD
target launches each bit on a rising edge and makes it readable 7 cycles later. Every read records
how far it sits from both ends of its window.

**With nothing fitted to them, it reproduces every SWD result the bench has:**

- the generated read sweep, all 70 positions, including which way each failure goes;
- vendor 0x87, at every position of both shapes, and its after-shape period to within a cycle
  over 32 clocks. The old model was two cycles a clock out;
- the block-size table at 16.67 MHz on the default build: the last read fails at every size, with
  acknowledge 3, and the loop's reads pass;
- the same for both variants tried on the bench, the tail shifted by a halfword and the tail
  inlined;
- the after-edge build reading clean at 16.67 MHz, and the delay-free step failing in both.

It also measures the tail's clock as the averaged capture did: three cycles high and six low
after ack0's rising edge, against four and five in the loop. Varying the parameters the fit does
not pin down leaves the result unchanged.

**Why the block's last read fails.** In the loop the rising-edge store's data access takes the
bus just as the next instruction word is due. The fetch waits a cycle, and that idle cycle
separates the rising and falling edges. The tail runs the same instructions at a different word
alignment and does not wait. Its falling edge comes a cycle sooner, the sample lands 6 cycles
after the edge, and it reads the bit before. The loop's own reads sit exactly at the window's
edge, so the loop passes by no margin at all.

**Two predictions, tested on the bench:**

- On the default build at 16.67 MHz, every write through `DAP_Transfer` misreads its acknowledge
  as 3, and reads pass. Confirmed, 50 of 50 in each of five orderings of reads and writes.
- `DAP_Transfer` reads at that rate sample data bits 1 and 2 a cycle early, which DPIDR's bits
  would hide. Wrong: the access port's TAR, written with values that would expose it, read back
  clean. So the model is a cycle out in that stretch of compiled code from SRAM.

**Refitted on code timed in isolation.** Timing each case between filler, so the closing
cycle-counter read waits on nothing in the body, removed most of what the first fit had got
wrong. From SRAMX the model now matches all 692 such cases at four alignments: every instruction a
cycle, loads and stores back to back, a computed value ready two cycles after its load, every
taken branch two cycles plus one for a straddling target. One more rule came out of it: an access
through the core's private bus, where the cycle counter is, waits two cycles behind an access over
S-AHB, and the reverse. From SRAM, straight-line code is still a cycle out at one alignment and
loops drift by up to a cycle an iteration.

**The wire agrees.** SWCLK trains from SRAMX, built from every GPIO store form (SET and CLR, NOT,
byte, word, and SET with a byte), span exactly their issue cycles. Every form reaches the pin at
the same delay, to a tenth of a cycle. The analyser cannot resolve pulses of one or two cycles.

**Generated reads run at 37.5 MHz, and the model predicts every one.** A read generated with each
sample placed a set number of cycles after the edge that launched its bit, and stored to a buffer
by the next free cycle, needs four instructions a clock. At every period from 4 to 8 cycles,
37.5 to 18.75 MHz, and every sample position, 45 configurations, the bench and the model agree on
the verdict and on the cycle count. The window's edges are included.

**Generated writes run at 50 MHz.** With the SWDIO store anywhere from one cycle before its rising
edge to one cycle after the previous one, every write landed, at every period from 3 to 10
cycles. Setup and hold at the target are each under a cycle.

**The MSPM0L1306 runs at 37.5 MHz too: its ceiling was ours.** On the second probe, now running
the lab build, the L1306 shows the same seven-cycle window start as the U575, at every period from
8 to 30 cycles. The scheduler, placing samples with it, ran clean against the L1306 at every period
from 4 to 40 cycles. "The last read of a block at 16.67 MHz", below, assumed the target set this
part's ceiling. It does not: the part fails above 10 MHz on ARM's paths because of where they
sample.

**A scheduler that places everything from the model runs at every period.** Given a period and a
list of transfers, a prototype places every edge, SWDIO store and sample by the model alone, with
each sample at the middle of its window. Two writes and five reads, acknowledges, data and parity
all checked, ran clean at every period from 4 to 40 cycles, 37.5 to 3.75 MHz, ten times each. The
cycle count the probe measured equalled the model's at all 37 periods. A rate is then any whole
number of cycles: the furthest a request falls short is 20% at the top of the range, against 56%
today, and nothing runs faster than asked.

TIMING.md has the rules these give a generator. CODEGEN.md is the context for implementing it.

In detail, the read-window prediction held at every sample delay that did not collide, in
`tools/lab.py swd_read_pipelined` at P = 4..8 on the U575. A soak of 200 reads each at P=4 d=9
(margin 1), P=4 d=7 (margin 0, the window's edge), P=5 d=9 and P=8 d=10 ran all clean, 7,200
target bits each. For writes, `tools/lab.py swd_write_asm` wrote the AP's TAR with the SWDIO
store `lead` cycles before the rising edge, for lead 1..P-1 at P = 3, 4, 5, 6, 8 and 10, 10 writes
each, and read them back at 1 MHz: all landed. So setup and hold at the target are each under one
cycle, and any placement off the edge cycles works. 50 MHz writes (P = 3) work on this harness,
request phase included.

## A register change cost a cycle on every data bit (2026-10-16)

Measured on ARM's SWD paths with the data phase rewritten (`src/swd_dp.c` then), running from
SRAM. Peeling the last read out of the block loop, with the peeled transfer inlined a second time,
left the data phase's instruction stream identical: the same opcodes, in the same order and
number, in different registers. It cost exactly one cycle on every one of the 32 data bits on
hardware, 32 cycles a word against the 24.8 the change saved. Out of line the same peel won,
because the extra transfer then needed no register in the block function.

Nothing upstream predicted it. The emulator counts instructions and saw no change, the gate passed
it, and so did all 1,212 differential cases. Forcing the functions to a 16-byte boundary moved the
figure by 0.00 cycles, which rules out alignment. Only a capture folded by position in the transfer
caught it.

## Bit paths in flash paid the prefetcher (2026-10-16)

ARM's SWD data phases, unrolled and run from flash. Flash runs at 11 wait states from 130 MHz up,
so a fetch the prefetcher has not covered costs 12 core cycles. A compact loop sits behind the
prefetch buffer and never pays; the unrolled data phases are straight-line code it cannot stay
ahead of, and two of every four data bits paid exactly one and exactly two of those fetches. The
result was 33 cycles a bit against the stock firmware's uniform 22. Run from SRAM (`.ramfunc`),
the same code paid none.

## The last read of a block at 16.67 MHz

Measured 2026-10-16, on ARM's paths.

The MSPM0L1306 on this bench connects at 1, 5 and 10 MHz and fails to connect at 20, 30 and 50 MHz
with the compiled bit paths. (Later: "The cost model times the bus", above.)
A throughput sweep could not go above 10 MHz on it, so the faster settings needed either a faster
target or a recording with no target attached at all, which `tools/dap.py sweep` already does.

**The STM32U575 is that faster target.** Its datasheet (DS13737 Rev 10, Tables 152 and 153) gives
SWCLK up to 66.5 MHz and TCK up to 38 MHz at 2.7 to 3.6 V. Both figures come from
characterisation at 30 pF, so `dap.py limit` through the real harness is the number to trust.
It also has 786 KB of SRAM against the L1306's 4 KiB, which lifts the transfer-size cap on
`swd-throughput`. SWD needs none of the TDI rework. The nRF54L family is no help here: the
nRF54L15 datasheet (v0.7) caps SWDCLK at 8 MHz, the same band this MSPM0 fails in. The nRF54LM20's
own figure was not checked.

**Measured 2026-10-16, ours against the U575 over CN5: clean to 10.00 MHz, failing at 16.67.**
`dap.py limit` checks every word at every rate the firmware has. Delay 2 (10.00 MHz on the wire)
and below read 3,200 DPIDRs each with no bad word. Delay 1 (16.67 MHz) fails, and so does the
delay-free path at 30 MHz. There is nothing between 10 and 16.67 on the ladder. The U575 does
better than the MSPM0, which failed at 9.375 MHz under NXP's firmware, but nowhere near its
datasheet.

**The wire is clean; the probe misreads it.** Recorded at our J6 (SWDIO and SWCLK, 100 MS/s) and
decoded by sampling at each rising edge, as the target does. Every read at both settings carries OK
and `0x0BE12477`, including the one the probe reported as acknowledge 3. At the connector the
target's bit arrives 15 ns after the rising edge that launched it (10 or 20 ns at this resolution,
mean 15.3 at 10 MHz and 14.9 at the faster setting), so the cable and the U575 together spend 15 ns.

The faster setting is not 16.67 MHz on the wire for a block: data bits run at 60 and 70 ns
(mean 67.5), acknowledge bits at 60 and 80, and the period at 10 MHz is 100 ns throughout.

Acknowledge 3 is an OK (1,0,0) seen one bit behind: the turnaround's high level, then 1 and 0.

**Only one function misreads, and it does every time.** At the faster setting `DAP_Transfer` read
19,200 DPIDRs without a fault, and every block-size test gave the same answer, 200 runs each:

| block | words returned, acknowledge |
| --- | --- |
| 1 word | 0, 3 |
| 2 words | 1, 3 |
| 4 words | 3, 3 |
| 64 words | 63, 3 |

The block command peels its last read into `SWD_TransferTailSlow`, an out-of-line copy of the
transfer, and that copy misreads its acknowledge on every call. The loop that does the other 63
never does. At 10 MHz all four sizes come back whole. The tail's acknowledge reads are the same
instructions as the loop's, in the same order and the same sizes, using the same GPIO aliases,
at a different address.

**It is not instruction alignment, and it is not the out-of-line function.** Two rebuilds, each
run through the same block-size test, 200 runs a size:

- A `nop` at the top of the tail shifted it by a halfword, flipping the 4-byte alignment of every
  instruction in it, and the compiler re-laid the neighbours too. Still 200 of 200 failing.
- Forcing the tail inline into the block function, so the last read is code in the loop's own
  function again, the peel undone. Still 200 of 200 failing, and the loop's reads still clean.

So the failure goes with the block's last read, whatever code carries it.

**On the wire the last read's clock is a cycle off.** Averaged over a recording of 14,977 one-word
blocks and 4,025 sixty-four-word blocks, which washes out the analyser's 10 ns steps because its
phase differs from read to read:

| | high after ack0's rise | low before ack1's rise | target's ack1 edge after the rise |
| --- | --- | --- | --- |
| last read, 1-word blocks | 21.8 ns | 38.2 ns | 17.6 ns |
| last read, 64-word blocks | 21.7 | 38.4 | 17.8 |
| the loop's reads | 27.9 | 32.2 | 17.1 |

Standard errors are 0.01 to 0.15 ns. The period is the same 60 ns and the target answers the
same, but in the last read the falling edge comes 6 ns, one core cycle, sooner after the rising
edge. Its instructions are the loop's, so one of the two clock stores reaches the pin a cycle
off from where it does in the loop. That one cycle is the difference between every read passing
and every read failing, which puts the loop's own margin at this setting under a cycle. A
comparison made earlier from a single transfer pointed the same way and was wrongly retracted as
quantisation: the averages bear it out.

**An earlier reading here, that the probe's own pins and buffers take ~45 ns, was wrong.** It
was a model fitted to one failure, and the two results above break it.

**Measured with vendor 0x87 (`dap.py sample`): the next edge's bit reaches the sample about five
cycles after the store that raised the clock.** At a 333 ns period, 1,000 reads at each position:
production's place before the edge and every position up to 2 filler cycles after it read
correctly, and from 3 on every read comes back one bit ahead, acknowledge 4. No position in
between gave a mixture. The filler times as exactly one cycle a step from a fill of 4 up (0 to 4
costs 5), and the period does not move with the sample. A computed branch of about two cycles
sits between the store and the filler, so the edge is near five cycles, ~35 ns, after the store
issues. In the loop, the sample before the next edge comes about eight cycles after this one at
the faster setting, so the loop has two or three cycles in hand. The tail runs the same
instructions and does not.

**Measured with both shapes of vendor 0x87: production samples at the early end of a
window about a period wide, and leaves most of it unused.** 1,000 reads a position, fill 24, a
260-280 ns period:

- **Lead shape, production's own pattern** (rising edge, filler, falling-edge store, production's
  delay loop, sample): every high phase from 0 to 24 cycles reads correctly. Even the shortest,
  about five or six cycles from the rising-edge store to the sample, already sees the bit that
  edge launched.
- **After shape** (sample in the high phase): correct up to 3 filler cycles past the edge, one bit
  ahead from 4. With operands reloaded through a struct the edge sat between 2 and 3, so it
  moves by a cycle with the code around it.
- A zero-length filler run is one cycle cheaper than the trend in both shapes, so position 0 sits
  a cycle earlier than its number.

So a bit is readable from about five or six cycles after the store that launches it until about
as long after the next one. Production samples a cycle before the next rising edge, at the
window's early end: one or two cycles in at the 16.67 MHz setting, which the block tail's
one-cycle shift used up, with five or six cycles of the window behind the sample unused.
**Prediction, not yet tested:** sampling a few cycles after the rising-edge store would sit near
the middle at that setting, and keep about two cycles either side even at the delay-free step's
five-cycle period. How much room there is past the edge depends on the target's round trip, which
was 15 ns here and which a closer or faster target shortens.

**Sampling after the edge fixes the 16.67 MHz setting (option `SWD_SAMPLE_AFTER_EDGE`, off by
default).** The delayed path samples one instruction after its rising-edge store, with
each transfer masked. The delay-free path is unchanged, because with no delay its low phase
would shrink to a cycle. A build with the option names itself `+after-edge`.

| | default | after-edge |
| --- | --- | --- |
| block-size test at 16.67 MHz, 200 runs a size | the last read fails every time | all whole |
| `DAP_Transfer` at 16.67 MHz | 19,200 clean | 19,200 clean |
| `dap.py limit` | clean to 10.00 | clean to 16.67; 128,000 reads at each of 16.67 and 10 |
| block read, 126 words, depth 2, 16.67 MHz | fails | 973 KiB/s |
| block read, same, 10 MHz, two rounds of three, alternated | 701.6-701.8 | 697.9-698.0 |

The 0.53% at 10 MHz is about four cycles a transfer, which is what saving, setting and
restoring PRIMASK costs. In `cycles.py --check` every SWD trace keeps its event count, and the
only change is that each sample now follows a rising edge rather than a falling one. Turning
the option on by default means re-blessing those five traces.

**What holds it back from the default.** A transfer stays masked for its whole length: 3 µs at 16.67
MHz, 46 µs at 1 MHz, longer below. SWO receives by interrupt into a 16-entry FIFO
(`RTE_USART3_DMA_EN 0`), which fills in about 20 µs at its 8 MHz maximum. So SWO streamed fast
alongside slow SWD would drop bytes, where today it does not. (Later: "SWO capture", above.) VCOM is
polled from the main loop and is not affected. The right shape is after the edge with a mask at the
fast settings and before the edge, unmasked, at the slow ones. That is a second copy of the delayed
path, and as unrolled C it does not fit in SRAM. Generating the bit paths made it free (CODEGEN.md).

JTAG reads TDO, which changes on the falling edge, before the rising one: half a period
rather than a whole one.

A failed connect still clocks the wire for five seconds while probe-rs retries, and those groups
are not worth reading: each retry changes the clock it asks for, so the period in them is not the
one first asked for.

## The LED: a timer's job, and what it costs the wire

Measured 2026-10-16, on ARM's paths at 10 MHz.

**Ours fades the LED with CTIMER3 as a PWM, and adds activity.** The LED breathes when nothing is
using the probe, blinks while commands arrive, and is fully lit when a host has said it is connected
and is idle. The period interrupt runs the policy and a command pays one byte store.
`tools/image.py` runs the interrupt against the image and checks all three, including that one
command is visible whatever tick it lands on. Two mutations of the policy were confirmed to fail
it.

Measured with `dap.py block --words 126 --blocks 400 --pipeline 2` at 10 MHz, three runs a build,
no spread within any build:

| build | KiB/s | against no LED |
| --- | --- | --- |
| no LED | 703.1 | |
| polled from the main loop, the first version | 701.6 | -0.21% |
| CTIMER3 PWM, 1 kHz tick | 702.5 | -0.09% |
| CTIMER3 PWM, 488 Hz tick, the one kept | 702.8 | -0.04% |
| 488 Hz with the handler in `.ramfunc` | 701.9 | -0.17% |

- **The polled version cost 1.5 µs a command**, several times what its instructions explain. It
  ran on the path between one command and the next, which is exactly where the next one waits.
  Flash fetch misses are the likely reason and were not tested, because the timer design removes
  the call from that path whatever the cause.
- **The PWM's cost is its interrupt rate.** Each tick lands wherever the core is, usually in a
  transfer, and stretches it. Halving the rate recovered 0.23 KiB/s against a predicted 0.3.
  `LED_PERIOD` in `src/led.c` is the knob.
- **Moving the handler into SRAM made it worse.** It went in at the head of `.ramfunc`, shifted
  every bit path by 116 bytes and cost 0.11%. `src/hot.h` now says so.

**Confirmed by eye** on the bench probe with `tools/dap.py led`, which walks it through breathing,
blinking and lit and says which is due.

## The command pipeline over USB

Measured 2026-10-16, on ARM's paths at 10 MHz.

### The USB side: the probe held one packet and told the host eight

`tools/dap.py block --pipeline N` keeps N commands in flight instead of waiting for each reply.
Measured on a 64-word block read at 10 MHz asked:

| commands in flight | end to end |
| --- | --- |
| 1 | 581 KiB/s |
| 2 | 646 |
| 3 | 646 |
| 4 | times out |

**A second command in flight is worth 11%**, and it is worth it for a reason nothing on the wire
can fix: with one in flight the wire sits idle while the host notices the reply and sends the next
request. The third adds nothing, and the fourth does not work.

`DAP_Info` said eight packets, copied from ARM's reference. It became 2, which is what the stack
can actually hold: one packet in the endpoint buffer and one in the class buffer.

**TinyUSB's vendor class is a byte stream, not a packet interface**, and that is the whole reason.
`tud_vendor_read` drains whatever the receive buffer holds, so a buffer sized for two commands
hands both to one read and the second is answered by nobody. Raising both buffers to eight packets
was tried and made it worse: the host times out from three in flight upward and throughput falls
as 1/N. A buffer of exactly one packet is what keeps CMSIS-DAP's one-command-per-packet framing
true, so `CFG_TUD_VENDOR_RX_BUFSIZE` and `TX_BUFSIZE` must stay at 512.

### Taken: TinyUSB's non-FIFO vendor mode, which keeps packet boundaries

A buffer size of zero selects it. A received packet arrives at `tud_vendor_rx_cb` with its own
length, and `tud_vendor_write` becomes one `usbd_edpt_xfer`. The command loop holds two request
slots and arms the receive endpoint only while one is free, so a command it cannot place is
refused by NAK rather than dropped.

| | FIFO mode | non-FIFO, two slots |
| --- | --- | --- |
| 1 command in flight | 581 KiB/s | 608 |
| 2 in flight | 646 | 669 |
| 4 in flight | times out | 568, no data lost |

### Through probe-rs, which is the host that matters

Everything above is `tools/dap.py`, which reads a DP register and touches no access port. The
numbers a user sees come through probe-rs, and mculink-bench's `swd-throughput` measures those.
Asked for 10 MHz, the fitted ceiling over 64 to 4096 byte transfers:

| | NXP's firmware | ours |
| --- | --- | --- |
| read32 | 469 KiB/s | 593 |
| write32 | 503 | 612 |

1.26x on reads and 1.22x on writes, against the same target through the same harness.

**probe-rs already pipelines, and it sizes the pipeline from what the probe reports minus one.**
The spare packet is where `DAP_TransferAbort` has to land: an abort exists to stop a transfer that
is running now, so it has to overtake the queue rather than join it. A probe advertising two
therefore gets a pipeline of one and no overlap at all, which is why `DAP_PACKET_COUNT` is three.
Measured, three runs each: reads 568 to 593 KiB/s and writes 584 to 612, every run of the deeper
pipeline ahead of every run of the shallower one.

**Our firmware was not honouring that contract.** The abort was queued like any other command, so
the flag was set only once the transfer it was meant to abort had finished. ARM's own MCU-Link
example sets it in the endpoint callback and never queues it; `tud_vendor_rx_cb` now does the
same.

### What limits the pipeline, and why two in flight is the number

The depth a host can use is the number of request slots in `src/main.c`, set by `-DREQUEST_SLOTS`
and reported to the host as `DAP_PACKET_COUNT`. Built with 2, 4 and 8 slots and swept:

| host depth | 2 slots | 4 slots | 8 slots |
| --- | --- | --- | --- |
| 1 | 608 KiB/s | 612 | 632 |
| 2 | 669 | 669 | 669 |
| 4 | 568 | 669 | 669 |
| 8 | host times out | host times out | 667 |
| 12 | | | host times out |

**Throughput saturates at two in flight and stays there.** Four and eight slots measure the same
669 KiB/s as two. The wire is 92% of a block read, so the link only has to stay a little ahead of
it, and two commands is enough for that.

**Nothing above the slot count is the probe misbehaving.** With no free slot it NAKs, which is the
correct thing to do and loses nothing. What fails is a host that blocks on a write while refusing
to read replies, which deadlocks against its own backpressure. `tools/dap.py` does exactly that,
and so would anything using synchronous bulk transfers. The probe recovers the moment the host
drains: a depth-8 run against two slots times out, and the next depth-2 run measures 669.5.

So the knob stayed at two then, which was honest and saturating. (Later: "Through probe-rs, which
is the host that matters", above.) It is worth re-measuring against a target that answers far
above 10 MHz, where the wire would stop being 92% of a block read and the link could become the
limit again.
