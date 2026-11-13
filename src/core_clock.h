/** What the core actually runs at, which ARM's clock arithmetic needs as a constant.
 *
 * TinyUSB's board file is called `BOARD_BootClockPLL150M` and configures PLL0 to 150 MHz, but it
 * then attaches **PLL1** at 144 MHz to MAIN_CLK and leaves PLL0 driving FLEXCOMM0. The 144 is
 * there so that MAIN_CLK/3 is exactly the 48 MHz a full-speed USB controller needs.
 *
 * This firmware never uses that controller: tusb_config.h puts the device on RHPORT 1, the
 * high-speed port, whose PHY PLL comes from the 16 MHz crystal and not from MAIN_CLK, and no host
 * controller is built. So the constraint that picks 144 does not apply here, and 130 to 150 MHz is
 * one flash wait-state bracket, so the faster clock costs no extra flash cycles.
 *
 * `board_core_clock_init` attaches PLL0 instead. It is already configured and locked by the time
 * board_init returns, so this is a clock selector and a voltage setting, not a PLL bring-up.
 */
#ifndef CORE_CLOCK_H
#define CORE_CLOCK_H

#ifdef CORE_150MHZ
#define CORE_CLOCK_HZ 150000000U
#else
#define CORE_CLOCK_HZ 144000000U
#endif

/** PLL0, from the 16 MHz crystal: the board support sets it to 150 MHz in either build. The SDK's
 *  CLOCK_GetPll0OutFreq reads a variable only CPU 0's image sets, so CPU 1 cannot ask for it. */
#define PLL0_CLOCK_HZ 150000000U

/** Move MAIN_CLK to PLL0, after board_init has brought both PLLs up. Does nothing at 144 MHz. */
void board_core_clock_init(void);

#endif
