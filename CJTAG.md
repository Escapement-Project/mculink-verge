# cJTAG

cJTAG is IEEE 1149.7: JTAG carried on two pins, TCKC and TMSC. This page has what the standard
says as far as the firmware uses it, and the wire the firmware drives. CODEGEN.md has how the probe
generates the frames, and [cJTAG targets](docs/cjtag-targets.md) what a host does to bring a TI
target up. The [measurement log](MEASUREMENTS.md) has the rates each format reached.

## Why cJTAG needs the probe's firmware

ARM's reference SWD bit path moves SWDIO while SWCLK is high:

```c
#define SW_CLOCK_CYCLE() \
  PIN_SWCLK_CLR(); PIN_DELAY(); PIN_SWCLK_SET(); PIN_DELAY()
#define SW_WRITE_BIT(bit) \
  PIN_SWDIO_OUT(bit); PIN_SWCLK_CLR(); ...
```

`SW_CLOCK_CYCLE` leaves the clock high, and `SW_WRITE_BIT` changes data before pulling it low. That
is harmless for SWD, where the target samples on the rising edge. On a 1149.7 wire a TMSC
transition while TCKC is high is an escape edge (10.4.2 e), so this path makes one on every bit
change. One edge in a TCKC-high interval is still data, since an escape takes two or more (Table
10-9), but the same order leaves a target that samples on the falling edge almost no setup, and
SWCLK rests high between commands. A host cannot avoid it: the only command that releases the wire
for the target's part of a frame, `DAP_SWD_Sequence`, runs on that bit path. So cJTAG needs a probe
that moves TMSC only while TCKC is low, and that is a change inside the probe.

## What IEEE 1149.7-2022 says, as far as this code uses it

Clause 25 covers the OScan formats, 10.4 the escapes, and 9.10 the configuration registers.

- **STFMT's operand is the format number**, with OScan formats from 8. STFMT 9 is OScan1, 10
  OScan2, 13 OScan5 and 14 OScan6.
- **Which bits a format sends** (Table 25-1): OScan1 sends nTDI, TMS and TDO for every JTAG
  clock; OScan2 drops nTDI and TDO outside Shift; OScan5 drops TMS in Shift; OScan6 is OScan2
  outside Shift and OScan5 in it, plus one PAD bit in the first Shift packet after one outside it
  (rule 25.4.2 g). A missing nTDI means TDI 1.
- **OScan5 and 6 leave Shift with an EOT escape on an nTDI bit** (Figure 25-11): the probe holds
  TCKC high through that bit and toggles TMSC twice in it, and the bit's TMS becomes 1.
- **Escapes** (10.4.1) are counted in TMSC edges while TCKC is high: 2-3 for EOT, 8 or more for a
  reset. Each edge comes at least one minimum TCKC period after the rise or the previous edge.
- **The TAP advances** (25.6) on the TDO bit's rising edge when the packet has one, and otherwise
  a period after the packet ends, so OScan2's packets outside Shift are pipelined.
- **CNFG0 says which optional formats a part has**: its OSCANS field flags the pairs OScan3:2,
  OScan5:4 and OScan7:6.

## The wire

- **A frame** is three packets for each JTAG clock in OScan1: nTDI, TMS and TDO. A packet is: set
  TMSC with TCKC low, raise TCKC, lower TCKC. TMSC moves only while TCKC is low; a move with TCKC
  high is an escape edge (the gate's `moves_while_high` rule).
- **The TDO packet.** The probe releases TMSC (TXEN, then the pin) right after the fall that
  starts the packet. TDO is valid from a fixed latency after that fall, the cJTAG counterpart of
  SWD's A. The probe measures it when a host connects, and samples a cycle later (docs/wires.md,
  "cJTAG"). On a CC1312R1 TDO stays valid through the rise and at least 7 cycles past the next fall.
  The probe takes the wire back (pin direction, then TXEN, two stores) right after that next fall,
  then stores the next frame's first packet.
- **Probe-driven packets** are as short as their stores allow. A CC1312R1 takes a high phase of one
  cycle (6.7 ns). The low phase holds what must happen in it: one data store for the TMS packet,
  the take-back and the data for the nTDI packet after a TDO packet. The generator stretches only
  the TDO packet, as the SWD generator stretches the clocks the target drives: uniform packets would
  each take the TDO packet's 8 cycles or more.
- **The sampling edge.** A T4 TAP.7, such as the CC1312R1's, samples TMSC on the falling edge until
  the host's STC1 sets SREDGE, as TI's activation does, and on the rising edge after it (1149.7
  23.4.1.4.4; the TRM's Table 6-4 calls it SEDGE). The generator stores TMSC a quarter
  period after a fall the target samples, so the store has hold time on either edge. After a TDO
  packet's fall the target samples nothing, and nothing waits there.
- **No Test-Logic-Reset inside a session.** On a CC1312R1 any entry drops OScan1, so the generated
  tails reach Run-Test/Idle without passing through it ([cJTAG targets](docs/cjtag-targets.md)).

## ARM's paths

The `+armwire` reference build frames cJTAG through ARM's C, which the gate holds the generated
frames to with `tools/equiv.py`. It compiles ARM's `JTAG_DP.c` four times, and only the wire
framing differs:

- as it ships, with its five entry points renamed `jtag4_*`, for conventional JTAG;
- through `src/cjtag_jtag_dp.c`, renamed `cjtag_*`, with the pin layer shadowed so that each JTAG
  clock becomes one OScan1 frame;
- through `src/cjtag_jtag_dp_fast.c`, renamed `cjtagf_*`, which drives that frame without its
  pacing loop for requests above the switch point;
- through `src/cjtag_jtag_dp_tracked.c`, renamed `cjtagt_*`, which follows the TAP from the TMS it
  sends and frames each clock in OScan2, OScan5 or OScan6 by the TAP's state (`cJTAG_FrameTap`).

`src/cjtag_dp.c` owns the real names and picks a path per call. No path carries a per-bit branch,
and none duplicates the others' TAP walking, `ir_before` and `ir_length` accounting or transfer
retry logic.

The copies rest on one contract: `JTAG_DP.c` expresses every clock through four macros, which
expand, in order, to `PIN_TDI_OUT` (sometimes), `PIN_TCK_CLR`, `PIN_DELAY`, `PIN_TDO_IN`
(sometimes), `PIN_TCK_SET`, `PIN_DELAY`. Shadowing those names stages TMS and TDI, opens a frame on
`PIN_TCK_CLR`, and sends it at whichever of `PIN_TDO_IN` or `PIN_TCK_SET` comes first. The
submodule is pinned to a release, so the order cannot change unless someone moves the tag.

A frame is entered and left with TCKC low and the wire driven, which is not where ARM's JTAG code
rests. Taking the wire back after the TDO packet moves TMSC from whatever the target drove to what
the probe's output latch holds, and doing that with TCKC high would put an escape edge in every
frame. OpenOCD's OScan1 driver sequences the three packets the same way, with TCK resting low
between them.

Framing sends no TCKC period of its own. Before leaving OScan2 or OScan6, whose frames outside Shift
carry no TDO, the host clocks the trailing frame that finishes the target's last move, so it lands
on a rise the host chose. OScan5's frames all carry TDO, so leaving it needs no trailing frame.
