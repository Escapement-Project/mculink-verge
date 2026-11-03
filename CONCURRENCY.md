# Concurrency

Several things run at once in this firmware: CPU 1's command loop, CPU 0's loop, CPU 0's
interrupts, and the DMA and USB controllers writing memory on their own. This document lists the
state more than one of them touches, who writes it, and what keeps each access in order.

ARCHITECTURE.md says what each core runs. BUSES.md says which bus port each access uses, and why
the other core cannot move a wire edge; that is timing, not ordering, and this document leaves it
there.

## What runs

- **CPU 1** is one context. It enables no interrupts and polls its mailbox register for work
  (`core1_main.c`, `serve`). A fault stops it, and so does the NMI vendor request `0x05` raises. So
  nothing on CPU 1 ever runs between two of its own instructions.
- **CPU 0's loop** (`main.c`, `serve`) runs TinyUSB's task, and every TinyUSB class callback with
  it: a packet received, a reply sent, a configuration set, a control request. It also runs the
  consoles, Verge Aux, the DIO edges' bookkeeping, the energy capture's range re-arm and the
  reboot.
- **CPU 0's interrupts** come in three levels. The DIO pin interrupts sit alone at the top
  (`DIO_EDGES_PRIORITY`). DMA0 and USB share the next (`CPU0_PRIORITY`). The LED tick (CTimer 3)
  and SysTick come last. TinyUSB's interrupt queues events for the loop, and also keeps its own
  connection state and calls `tud_event_hook_cb` there (below).
- **DMA0** has channels 4 to 7 for the consoles, 8 for SWO and 21 for energy capture. **The USB
  controller** reads and writes the packet buffers in USB SRAM.

CPU 0 has two ways to keep an interrupt out. `cpu0_mask_all_but_edges` raises BASEPRI over every
interrupt but the DIO pins', which touch nothing else; the loop takes it where it changes the
energy capture's state, which the DMA interrupt also changes. A few short stretches turn every
interrupt off (PRIMASK): in the DMA interrupt between reading SWO's completion and counting it, in
the range re-arm between its timing check and its edge, and in the DIO edges' bookkeeping.

## How the cores signal each other

Each core has a mailbox register the other raises.

- CPU 0 raises CPU 1's when it publishes a command, a Verge Aux request or an abort, and when it
  frees a reply slot CPU 1 may be waiting for.
- CPU 1 raises CPU 0's when it frees a bulk request slot, publishes a reply or an Aux reply, asks
  for the energy capture's state, or gives the DIO pins back from SPI.

A core that waits clears its own register first, then looks for work, and waits only if it found
none, so a raise that lands after the look still ends the wait. CPU 1 looks at all of its work
(`dap_task`) and waits by polling the register's level, which cannot be missed
(`wait_for_mailbox`). CPU 0 clears its register at the top of its loop, so every task in the loop
is part of the look (`serve`). It then checks for work still queued, the interrupts' included
(`queued_work`), and waits from SRAM with every interrupt but the DIO pins' masked (`idle`,
`wait_for_work`), watching the mailbox, pending interrupts, the consoles and the DIO edges.

Every handover between the cores follows one rule. The writer writes the data, then a memory
barrier (`__DMB`), then the flag that says it is there. The reader reads the flag, then a barrier,
then the data. The flags are the lengths, states, counts and sequence numbers below. All of them are
`volatile`. Several have more than one writer, but each change of state has exactly one.

## Commands and replies

`dap_shared` holds four rings of `REQUEST_SLOTS` slots each: a request ring and a reply ring for
each transport. A slot is full while its length is non-zero. Whoever fills it writes the length last, and
whoever empties it writes zero once it no longer needs the bytes (`dap_usb_slots.h`).

| ring | fills | empties | indices |
| --- | --- | --- | --- |
| bulk requests | CPU 0, in `tud_vendor_rx_cb` | CPU 1, as it takes the command | tail CPU 0's, head CPU 1's |
| HID requests | CPU 0, in `tud_hid_set_report_cb` | CPU 1, as it takes the command | tail CPU 0's, head CPU 1's |
| bulk replies | CPU 1, after running the command | CPU 0, once USB has sent it or will not | tail CPU 1's, head CPU 0's |
| HID replies | CPU 1, after running the command | CPU 0, once USB has sent it or will not | tail CPU 1's, head CPU 0's |

Each index has one writer, and each length one writer per direction: the filler makes it non-zero
and the emptier makes it zero. So no slot needs a lock.

CPU 0 numbers every command from one count across both transports (`next_request_seq`). CPU 1
takes the oldest waiting command whose transport has a free reply slot (`dap_core_ready`,
`dap_usb_choose_hid`). Each transport's commands run in the order they arrived. CPU 0 sends each
transport's replies from the head of its own ring, so they leave in that order too.

Each transport has a reply ring of its own because a reply offered on an IN endpoint waits there
until a host reads it. A host that has gone never will, and Linux polls an HID endpoint only while
its device node is open. In a ring both transports shared, such a reply would hold the head. The
other transport's replies would queue behind it, and once they filled the ring, CPU 1 could take no
command from either. With a ring each, a gone host's replies hold back only its own interface
(`check_interfaces_apart` in `tools/usb_checks.py`). The next host on that interface has to read
them first, as it would with one transport.

The USB controller writes a request straight into its slot. CPU 0 arms EP1 OUT only into a free
slot. With none free the endpoint stays unarmed, the controller answers NAK, and the host waits.
CPU 0 starts one reply at a time on each transport. Which slot is armed and whether a reply is in
flight are CPU 0's loop's alone, since TinyUSB's callbacks run in the loop too.

### Staged copies

CPU 0 copies a staged command into `dap_stage` before it writes the command's length, so CPU 1
sees it whole. CPU 1 releases the request slot as soon as it takes the command,
so the next command can arrive while this one runs. The staged buffers therefore cannot be the
slots: CPU 0 takes them in turn, one more of them than there are slots, so a buffer comes round
again only after `REQUEST_SLOTS` more commands, and CPU 0 cannot receive that many while the
command in it runs (`dap_stage_t`). A staged reply goes the other way: CPU 1 writes it before it
publishes the reply's length, and CPU 0 copies it into the USB buffer before it starts sending.

Vendor command `0x8C` switches staging: CPU 1 writes `stage_commands`, and CPU 0 reads it as each
command arrives. Each command carries its own flag (`request_stage`), so a change takes effect
between commands, never inside one.

### Aborts

`DAP_TransferAbort` has to reach the transfer running now, so it never joins a ring. CPU 1
publishes the address of ARM's `DAP_TransferAbort` flag in `dap_shared` before it takes its first
command. When an abort arrives, CPU 0 writes 1 there directly, after checking that the address lies
in CPU 1's RAM (a CPU 1 that faulted can leave anything in it), then a barrier, then CPU 1's
mailbox (`transfer_abort`). It is the only write CPU 0 makes into CPU 1's memory. A transfer
clears the flag as it starts, as ARM's code does, so an abort that lands before a transfer starts
is gone by the time it runs.

### A new configuration

Each time the host sets a configuration, which follows every bus reset, CPU 0 starts a new
numbering in `tud_mount_cb`. It sets `first_live_seq` to the next command's number, forgets each
transport's reply in flight, and raises the abort flag. CPU 1 drops any command numbered before
`first_live_seq` unrun, checking that as the last thing before it runs one, so the abort has the
least room to miss. CPU 0 drops those commands' replies unsent (`send_bulk_head`, `send_hid_head`). The same callback
sets `bulk_packet_size` for the new speed, before any command of that configuration can arrive.

A bus reset's speed comes from TinyUSB's interrupt: `tud_event_hook_cb` records it in
`reset_speed`, and the loop takes it with an atomic exchange (`usb_speed.c`).

## Verge Aux

Verge Aux has one request slot, in `dap_stage`, with a state byte. Each move has one writer, which
fills the slot first and moves the state last (`aux_slot.h`):

- CPU 0 moves it from free to request, when a request arrives on EP5 OUT.
- CPU 1 moves it from request to reply, before its next CMSIS-DAP command (`aux_serve`).
- CPU 0 moves it from reply to free, once it has copied the reply out.

CPU 0 arms EP5 OUT only while the slot is free, so the controller never writes a request over one
CPU 1 holds. CPU 0 stamps each request with its count of USB configurations (`generation`), and a
reply from an earlier one is dropped.

## Energy capture

### Who owns the state

The capture's state lives in CPU 0's SRAM (`struct energy_integration`), and CPU 1 reaches it
through a pointer CPU 0 puts in `dap_shared` before it starts CPU 1. A handshake gives CPU 1 the
whole of it when it needs it (`energy_integration.h`):

1. CPU 1 writes a new request into `energy_control_request`: a count that goes up by two each time,
   with the quiet flag set when it wants the state and clear when it hands it back. Then it raises
   CPU 0's mailbox (`control_request` in `energy_monitor.c`).
2. CPU 0 copies the request into `energy_control_ack`, from its loop only, with BASEPRI raised
   (`energy_control_task`).
3. CPU 1 waits for the copy, a bounded number of polls (`capture_quiet`). If none comes, it records
   the capture as failed. CPU 1 asks for the state only when CPU 0 has it, not for a pause it makes
   while holding it already: inside a start, calibrating at boot, or reading the supply after a
   stop. So a request for the state that timed out is withdrawn by a new one that leaves the state
   with CPU 0, and a loop that comes back late keeps it and ends the stream (`stream_fault` in
   `aux.c`). At boot, on a Pro that can measure energy, CPU 1 waits without a bound, so it cannot
   finish starting until CPU 0's loop runs.

The acknowledgement comes from the loop, so the DMA interrupt is not running when CPU 0 gives the
state away. While the quiet flag stands, the interrupt leaves energy completions alone
(`DMA0_IRQHandler`), though SWO's and the consoles' go on as usual, and the loop's range re-arm and
stream commands stand aside too.

While CPU 1 holds the state it drains what is left in the ring at a pause or stop, writes a
stream's end, and for a start clears the last failure and writes the tables, the DMA descriptors,
the position and an unidentified stream's header. Then the start hands the state back, but it is
not finished: it goes on to write a new session's identity, reset the total and the decoder, write
an identified stream's header, mark the capture running, and start the timeline. CPU 0 stays
off all of that for three reasons. The DMA channel is still off, and enabled last, so no completion
can reach the interrupt; the interrupt also integrates nothing while `running` is clear. The loop
treats the stream as CPU 1's while `timeline_pending` is set. And the identity has its own
sequence counter (below). From then on CPU 0's DMA interrupt advances the state, one segment at a
time.

### Values read while the other core writes them

Some values are read by the core that does not own them, under a sequence counter: the writer
makes the counter odd, writes, then makes it even again, and a reader that saw an odd count, or a
count that changed while it read, reads again.

- `energy_accumulator_sequence` guards the energy total and the decoder's position. The owner
  writes them; CPU 1 reads them for `0x90` modes 1 and 3.
- `energy_dma_sequence` guards a segment's completion while CPU 0 clears it, integrates it and
  counts it in `energy_segments`. CPU 1's `ring_written` reads the count, the completion flags and
  the channel's progress, and repeats until all of them agree.
- `capture_id_sequence` guards the capture's identity, which CPU 1 writes at each new session's
  start, after it hands the state back. CPU 0 reads it when Verge Aux asks to end a stream only if
  the identity matches (`0x90` mode 13), and waits while it is odd.

### Records either core adds to

Four records take entries from one side while the other owns the rest of the state. Each has its
own written and read counts (`energy_stream.h`).

- **Holds.** When CPU 1 measures a target's read latency, it stops the conversion timer, and on
  release records the gap (`holds_written`). CPU 0 picks the holds up as it integrates the next
  batch of pairs. The hold's end time is written just after the count, which is safe only because
  no pair after the hold exists until the timer starts again.
- **A start's first trigger.** CPU 1 sets `timeline_pending` until it has published when the first
  pair was triggered. CPU 0 waits for it before it times a batch, a bounded number of polls, and
  until then treats the stream as CPU 1's.
- **Markers.** A marker (`0x94`) runs on CPU 1, like any command, and CPU 1 records it whichever
  core owns the stream (`marks_written`). Ending a stream must not lose one half recorded, so the
  two sides check each other. CPU 1 sets `mark_busy`, then looks at `marking`. The core ending the
  stream clears `marking`, then waits, a bounded number of polls, while `mark_busy` is set
  (`energy_stream_unmark`). A marker either sees the stream already ended, or is in the record
  before the end flushes it.
- **DIO edges.** CPU 0's pin interrupt records edges (`edge_entries_written`) whichever core owns
  the stream, and the owner sends them (`dio_edges_task`).

The stream's messages go into 16 slots in SRAM4. The owner writes them and moves `written`; CPU
0's loop sends them and moves `sent`. A start with an identity moves `sent` too, while CPU 0 holds
its messages back until the start's reply has gone (`stream_held`).

### Failure

`energy_dma_error` records that a capture failed: a DMA error, an ADC overflow, a segment not
taken in time, or a handover that timed out. CPU 0's DMA interrupt and CPU 1 both set it; only a
new session's start clears it, on CPU 1. CPU 0's loop reads it to answer stream commands and to end
a stream whose capture failed (`stream_fault` in `aux.c`). So the start clears it before it hands
the state back. Cleared any later, the loop could find the old failure beside the new stream, and
end a capture that runs.

### The range re-arm

The DMA interrupt sets `energy_range_retry_pending` when a segment shows the current has fallen
below the low range's limit. CPU 0's loop then clocks the range latch (P0_17), with every interrupt
off from its last timing check to the edge, reading CTimer 2 to find a gap between pairs. CPU 1
clears `energy_range_retry_enabled` and the pending flag before it pauses a capture, and a start
sets enabled again. A re-arm already past its check of enabled can still clock the latch after
that; the start's own edge, which it sends whatever P0_17 was left at, covers it. Once CPU 1 has
the quiet acknowledgement, which comes from the same loop, no re-arm is under way.

### A latency measurement

A read-latency measurement on CPU 1 holds the capture: it stops the conversion timer, then waits, a
bounded number of polls, for the conversion under way and the DMA reads after it to finish
(`energy_monitor_hold`). It measures either way. No new conversion starts, so no DMA reaches ADC0's
port while the measurement runs. The hold is recorded for the stream as above, and releasing it
starts the timer again.

## SWO

DMA writes the trace into a ring in `dap_shared` without stopping. CPU 0's DMA interrupt counts each
completed segment into `swo_segments`. The ring's segments raise INTA and INTB in turn, so two
completions before the interrupt runs show as both flags, and the count is known to be lost rather
than silently short (`dma_ring.h`). The interrupt then sets `swo_lost` before it clears the flags,
so a CPU 1 read that finds the flags clear also finds the loss. Every interrupt is off between
reading SWO's completion and counting it.

CPU 1 turns the count and the channel's progress into a position, and reads again until the two
agree (`dma0_ring_written`). When it restarts the ring, it waits `SWO_COUNT_SETTLE_US` after
clearing the flags, so a count the interrupt was making lands before CPU 1 takes the new start. A
DMA error on SWO's channel goes from CPU 0's interrupt to CPU 1 through `swo_dma_error`. CPU 1
clears both flags once it has acted on them.

## The consoles

The consoles never leave CPU 0. DMA fills each console's receive ring the same way as SWO's: the
DMA interrupt counts the segments, and the loop reads a ring's position with the same rereading and
restarts a ring whose count was lost. To send, the loop marks the console busy, then a barrier,
then starts the channel; the interrupt clears busy when the transfer completes. Failures are
recorded by the interrupt and by the loop (`vcom_dma_done`, `vcom_dma_error`). None of this needs
BASEPRI: each flag has one writer per change, and a channel's descriptors change only while it is
off.

## The DMA controller

Both cores program DMA0, each its own channels. The descriptor table has one entry per channel,
and the controller's shared registers take one bit per channel, written as a set or a clear, so
neither core's write disturbs the other's channels. Each owner writes its descriptors with the
channel off, then a barrier, then enables it.

## The DIO pins

CPU 1 drives the DIO pins (`0x8E`) and lends them to SPI (`0x91`). CPU 0's pin interrupts time
their edges for the energy stream. SPI must not show up as edges, so the two cores coordinate
through `dio_spi_turns`, which is odd while SPI has the pins.

- When SPI takes the pins, CPU 1 counts, then turns the pin interrupts off (`dio_spi_begin`).
- When SPI gives them back, CPU 1 counts again, then raises CPU 0's mailbox (`dio_spi_end`).
- CPU 0's loop turns the interrupts back on only when the count is even. After a barrier it reads
  the count again, and turns them off if it changed (`dio_edges_task`).
- On CPU 0, whatever reads the pins' levels reads the count again after them. CPU 1 counts before
  SPI moves the pins, so an unchanged even count means the levels are the pins' own. Otherwise the
  pin interrupt records nothing and marks the edges lost, though the run still counts against its
  rate (`PIN_INT0_IRQHandler`), and a stream's header reads them again (`dio_edges_pins`).

The pin interrupt can also rest: past a rate of edges it turns itself off for a while and marks the
edges lost (`resting`). The loop turns it back on once the rest is over.

## The LEDs

CPU 1 and CPU 0's loop report to the LEDs with one byte store per input (`led.h`), and the LED tick
reads them, clears the ones that mark an event and reads TinyUSB's state. Losing a store to a race
costs at most one flicker, so nothing orders them.

## Set once, before CPU 1 starts

CPU 0 clears the handover state and reads the straps before it starts CPU 1 (`dap_shared_init`,
`board_read_straps`): the board, its jumpers, whether it can measure energy, and the pointer to the
energy state. CPU 1 only reads those. The same call sets staging on and the high-speed packet size,
which change later as described above.

## Counts read without ordering

SysTick counts milliseconds into `system_ticks` while CPU 0's loop runs. The loop's wait masks it,
so while CPU 0 is idle the count moves once per wake, at least every 2.048 ms LED tick; the reboot's
wait and the full-speed retry that read it wait at least as long as they ask. CPU 1 counts the
commands it runs and the stale ones it drops, and notes the one running, in `dap_shared.core1`. On
a fault it writes the fault registers first and `exception` last. CPU 0 copies the record whole for
vendor request `0x04`, so its counts can be from two moments, but a record with `exception` set is
complete. Each of these has one writer.
