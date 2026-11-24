/** Put a function a transfer runs through in SRAM, where it does not pay flash wait states.
 *
 * Flash on this part runs at 11 wait states from 130 MHz up, so one fetch the prefetcher has not
 * covered costs 12 core cycles. A compact loop sits behind the prefetch buffer and never pays;
 * straight-line code the prefetcher cannot stay ahead of pays on every miss, and between two wire
 * edges that is time on the wire.
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
 * **Anything else marked lands ahead of the transfer loops and moves them.** The LED's interrupt
 * handler, tried here to spare it flash fetches, shifted everything after it and cost more block
 * throughput than the whole LED costs from flash (MEASUREMENTS.md, "The LED"). Measure a block read
 * before and after adding anything to this section.
 */
#ifndef HOT_H
#define HOT_H

#ifdef RAMFUNC_BIT_PATHS
#define HOT_PATH __attribute__((noinline, section(".ramfunc")))
#else
#define HOT_PATH
#endif

/** The code every command runs through on CPU 1, outside the transfer loops: the loop, the
 *  dispatch, the block wrappers and what they call each command. In SRAM after every transfer
 *  loop, so it moves none of them: the linker takes every .ramfunc section before any .data one.
 *  tools/image.py checks none of it branches into flash. */
#ifdef RAMFUNC_BIT_PATHS
#define COMMAND_PATH __attribute__((noinline, section(".data_command_path")))
#else
#define COMMAND_PATH
#endif

/** The SWD generator, the JTAG generator's scans and the schedule both share (src/gen.c,
 *  src/swd_gen.c, src/jtag_gen.c): branchy C that runs when a configuration changes and in every
 *  connect-time latency search, where flash wait states doubled the time of each SWD build. In SRAM
 *  after every transfer loop, so it moves none of them. tools/image.py checks everything they call
 *  stays in SRAM, the compiler's memset and the JTAG transfer emitter aside. */
#ifdef RAMFUNC_BIT_PATHS
#define GEN_PATH __attribute__((section(".data_gen_path")))
#else
#define GEN_PATH
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
