# Optimisation opportunities

Found while building, to be spent at the bench once the probe works. Nothing here is urgent and
nothing here should be done before the firmware is correct.

**None of these are measured on our firmware.** The figures are from captures of the stock
firmware. Each entry says what it predicts, so a change can be checked against a number
rather than against "it feels faster".

## In the SWD bit path

### Three GPIO stores per bit, where two would do

A SWD bit is three GPIO stores: the data, then `CLR` to drop the clock, then `SET` to
raise it. The data and the falling clock edge could be a single masked `MPIN` write covering both
P0_0 and P0_2, since `MPIN` writes only the unmasked bits and both pins are on port 0.

Predicts: one store in three removed from the inner loop. At 14 MHz requested the measured period
is 140 ns, about 21 cycles at 150 MHz, so a store is worth roughly 2 to 4 cycles — call it 10 to
19% off the bit time if the loop is store-bound.

Our firmware has this shape through ARM's `SET`/`CLR` pin layer.

### The slow branch's delay cannot reach zero

`PIN_DELAY_SLOW` is `do { i--; } while (i)`, so the shortest delay it can express is one iteration,
about 3 cycles. Two of them per bit. `Set_Clock_Delay` switches to a delay-free loop only above
25 MHz.

That leaves a band no clock request can land in: everything below 25 MHz pays at least 6 cycles of
delay per bit, and the first request above it pays none. A delay of zero iterations would close it.

Predicts: at the top of the slow range, 6 cycles off a 21-cycle period, so about 28%.

## In the cJTAG path

### Our OScan1 frames pay PIN_DELAY twice

`JTAG_DP.c`'s cycle macros call `PIN_DELAY()` around what, on the OScan1 path, are no-op clock
calls -- the real clocking and pacing happen inside `cJTAG_Frame`. So every frame pays its own two
delays plus two dead ones from the macro layer.

Predicts: roughly halves two-pin frame time. The fix is to make `PIN_DELAY` a no-op in
`src/cjtag_jtag_dp.c` the way the clock calls already are, which is one more `#define` in the block
that shadows the others.

This one is cheap, safe and ours. It is first on the list.

### The buffer direction pin is toggled every frame

`cJTAG_Frame` releases TMSC for the TDO packet and re-enables it afterwards: two GPIO writes per
JTAG bit on top of the three packets. Both are needed for correctness -- the target drives TDO
whatever we do, so the buffer has to be off -- but a sequence that captures no TDO still pays them.

`JTAG_SEQUENCE_TDO` tells us when the host does not want TDO. The third packet must still be
clocked, because the target counts packets, but whether the wire has to be released during it is
worth testing on the analyser.

Predicts: two GPIO writes per bit removed from TDO-less sequences, which is most of a TAP walk.
**Unproven and the riskier of the two**: if the target drives TDO regardless, not releasing means
contention. Measure before keeping.

## Above the wire

### DAP packet size above 512

512 is the USB 2.0 ceiling for a high-speed **bulk endpoint**, and it is not tunable. It is not the
ceiling for a DAP packet: `DAP_Info` reports packet size as a 16-bit value, a bulk transfer spans
as many endpoint packets as it needs, and probe-rs sizes its buffer from the reported value
(`packet_buffer_len` is `max_packet_size + 1`) rather than from the descriptor.

Predicts: **at most 10%**, and probably half that. The capture says the wire is busy 90% of the
time at 14 MHz, with about 52 us of gap between 480-byte bursts; a 4096-byte read spends 416 us of
its 8.49 ms not clocking. Doubling the packet size recovers about half of that.

Worth more once the bit loop is faster: the same gap budget is a larger share of a shorter
transfer. Worth nothing if done first.

Two caveats. It deviates from stock, which reports 512, so a host that hardcodes rather than
reading `DAP_Info` would break -- check pyOCD and OpenOCD before relying on it. And it costs RAM:
two buffers at `DAP_PACKET_SIZE` here plus whatever `DAP.c` holds.

### Measure stock's top clocks at all

Every throughput figure we have is at 14 MHz or below, because 14 MHz was the highest clock the
bench target would attach at. Stock's figures above that are simply unknown, and they are the
real ceiling of the stock design.

First measurement to take at the bench, before optimising anything: a target that will attach above
25 MHz, and the same sweep.
