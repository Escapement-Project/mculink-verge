/** Put a bit path in SRAM, where it does not pay flash wait states.
 *
 * The bench measured this on 2026-10-16 and it is the largest effect on the wire. Flash on this
 * part runs at 11 wait states from 130 MHz up, so one fetch the prefetcher has not covered costs
 * 12 core cycles. A compact loop sits behind the prefetch buffer and never pays; our unrolled
 * data phases are straight-line code the prefetcher cannot stay ahead of, and two of every four
 * data bits paid exactly one and exactly two of those fetches. The result was 33 cycles a bit
 * against the stock firmware's uniform 22.
 *
 * `.ramfunc` is the board support's own section: the linker script puts it at the head of `.data`
 * and the startup code copies it out of flash with everything else, so nothing here needs a
 * linker script of its own.
 *
 * `noinline` is the load-bearing half. Without it a marked function can still be inlined into a
 * caller that lives in flash, which puts the code back where it came from and says nothing about
 * it in the map. The functions marked are the ones a host reaches once per command, so the call
 * that noinline keeps is paid once per command and not once per bit.
 *
 * SRAM is the budget this spends: `m_data` is 204 KB and holds the stack, the heap and everything
 * static. The linker's own ASSERT catches an overflow.
 *
 * **Anything else marked lands ahead of the bit paths and moves them.** The LED's 116-byte
 * interrupt handler, tried here to spare it flash fetches, shifted every bit path by that much and
 * cost 0.11% of block throughput: more than the whole LED costs from flash. Measure a block read
 * before and after adding anything to this section.
 */
#ifndef HOT_H
#define HOT_H

/** **Do not restructure the functions this marks without measuring a data bit afterwards.**
 *
 * Their data phase is allocation-critical. Peeling the last read out of the block loop with the
 * peeled transfer inlined a second time -- a change that leaves the data phase's instruction
 * stream identical, same opcodes, same order, same count, different registers -- cost exactly one
 * cycle on every one of the 32 data bits on hardware. That is 32 cycles a word against the 24.8
 * the change saved. Out of line the same peel wins, because the extra transfer then needs no
 * register here.
 *
 * Nothing upstream predicts it: the emulator counts instructions and sees no change, the gate
 * passes it, and so do all 1,212 differential cases. Forcing these functions to a 16-byte
 * boundary moved the figure by 0.00 cycles, which rules out alignment. The only instrument that
 * catches it is a capture folded by position in the transfer.
 */
#ifdef RAMFUNC_BIT_PATHS
#define HOT_PATH __attribute__((noinline, section(".ramfunc")))
#else
#define HOT_PATH
#endif

/** The code every command runs through on CPU 1, outside the bit paths: the loop, the dispatch,
 *  the block wrappers and what they call each command. In SRAM after every bit path, so it moves
 *  none of them: the linker takes every .ramfunc section before any .data one (src/sample_probe.c).
 *  tools/image.py checks none of it branches into flash. */
#ifdef RAMFUNC_BIT_PATHS
#define COMMAND_PATH __attribute__((noinline, section(".data_command_path")))
#else
#define COMMAND_PATH
#endif

/** Zeroed data touched only off the command path, placed after the rest of CPU 1's .bss
 *  (src/core1.ld.in) so that its size cannot push the statics every command reads into SRAM3,
 *  beside the buffers CPU 0 writes (BUSES.md). tools/image.py checks a product image for it. */
#ifdef RAMFUNC_BIT_PATHS
#define COLD_BSS __attribute__((section(".cold_bss")))
#else
#define COLD_BSS
#endif

#endif
