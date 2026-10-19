/** CPU 0's clock bring-up. CPU 1 starts after this has run and changes no clock source. */
#include "board_pins.h"

#include "bsp/board_api.h"

#include "core_clock.h"
#include "fsl_clock.h"
#include "fsl_power.h"
#include "fsl_device_registers.h"

/** Take MAIN_CLK off PLL1 and put it on PLL0, which is already locked at 150 MHz.
 *
 * Order matters going up in frequency: the core voltage and the flash wait states have to be
 * right for the new rate before the rate changes. 130 to 150 MHz is one wait-state bracket, so
 * the flash call changes nothing here and is made because the next person to move this number
 * will not be moving it within a bracket.
 *
 * board_init has already run SysTick_Config against the old SystemCoreClock, so the millisecond
 * tick TinyUSB counts on needs setting again.
 */
void board_core_clock_init(void) {
#ifdef CORE_150MHZ
    POWER_SetVoltageForFreq(CORE_CLOCK_HZ);
    CLOCK_SetFLASHAccessCyclesForFreq(CORE_CLOCK_HZ);
    CLOCK_AttachClk(kPLL0_to_MAIN_CLK);
    SystemCoreClock = CORE_CLOCK_HZ;
    SysTick_Config(SystemCoreClock / 1000U);
#endif
}

/** Start the high-speed FRO and its divider, which both the console and SWO clock off.
 *
 * Two separate things are off at this point and neither reports itself.
 *
 * The 96 MHz output of FRO192M is disabled out of reset (UM11126: ENA_96MHZCLK resets to 0) and
 * the board support never enables it -- BOARD_BootClockPLL150M calls CLOCK_SetupFROClocking for
 * the 12 MHz output only, because the core runs off PLL1. CLOCK_GetFroHfFreq then reports 0 Hz,
 * USART_Init rejects the baud rate it cannot reach, and the peripheral stays silent.
 *
 * FROHFDIV's HALT bit resets to 1 (UM11126 table 104), so even with the source running the divider
 * passes nothing. CLOCK_SetClkDiv writes the divider register whole, which clears HALT.
 *
 * Anything else here that attaches to FRO_HF_DIV depends on this call.
 */
__attribute__((noinline)) void board_clocks_init(void) {
    CLOCK_SetupFROClocking(96000000U);
    CLOCK_SetClkDiv(kCLOCK_DivFrohfClk, 1U, true);
}

/** TinyUSB's hook for anything a board needs doing once the stack is up. This one needs nothing.
 *
 * Its weak default lives in the hw/bsp/board.c that CMakeLists.txt drops, so with that file gone
 * the board layer owns it. See the note there for why it is dropped.
 */
void board_init_after_tusb(void) {}
