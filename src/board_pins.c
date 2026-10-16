#include "board_pins.h"

#include "fsl_clock.h"
#include "fsl_device_registers.h"
#include "fsl_iocon.h"

_Static_assert(DBGIF_TCK_SWCLK_PORT == DBGIF_PORT && DBGIF_TDI_PORT == DBGIF_PORT &&
                   DBGIF_TMS_SWDIO_PORT == DBGIF_PORT && DBGIF_TDO_SWO_PORT == DBGIF_PORT &&
                   DBGIF_TMS_SWDIO_TXEN_PORT == DBGIF_PORT && DBGIF_RESET_PORT == DBGIF_PORT &&
                   DBGIF_RESET_TXEN_PORT == DBGIF_PORT && LED1_PORT == DBGIF_PORT,
               "the per-port writes below assume every signal is on DBGIF_PORT");

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

/** Start the cycle counter behind TIMESTAMP_GET.
 *
 * DWT is powered down until TRCENA is set and the counter does not run until CYCCNTENA is, so
 * without this DWT->CYCCNT reads 0 forever. Two commands depend on it. DAP_SWJ_Pins with a
 * non-zero wait spins until the counter passes a deadline, so a pin that never reaches the asked
 * level hangs the probe rather than timing out; and a transfer with DAP_TRANSFER_TIMESTAMP set
 * reports 0 to a host that asked for a time.
 */
__attribute__((noinline)) void board_timestamp_init(void) {
    DCB->DEMCR |= DCB_DEMCR_TRCENA_Msk;
    DIB->DLAR = 0xC5ACCE55U; // unlock the SCS, on parts that implement the software lock
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

/** Digital GPIO, no pull, standard slew: what every debug pin wants.
 *
 * The buffers on this board give the drive strength, so nothing here needs a pull resistor. The
 * two TXEN pins are the exception that matters: they must come up low, because a buffer enabled
 * before the firmware knows the port state would drive the target's wire.
 */
#define DEBUG_PIN_MODE (IOCON_FUNC0 | IOCON_MODE_INACT | IOCON_DIGITAL_EN)

void board_debug_pins_init(void) {
    CLOCK_EnableClock(kCLOCK_Iocon);
    CLOCK_EnableClock(kCLOCK_Gpio0);

    // Every debug signal is on port 0, which is what lets one port number serve the whole loop
    // and the single-port writes below.
    static const uint8_t pins[] = {
        DBGIF_TCK_SWCLK_PIN, DBGIF_TDI_PIN, DBGIF_TMS_SWDIO_PIN,      DBGIF_TDO_SWO_PIN,
        DBGIF_RESET_PIN,     LED1_PIN,      DBGIF_TMS_SWDIO_TXEN_PIN, DBGIF_RESET_TXEN_PIN,
    };
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        IOCON_PinMuxSet(IOCON, DBGIF_PORT, pins[i], DEBUG_PIN_MODE);
    }

    // Buffers off first, then the pins they gate. Doing it the other way round drives the target
    // for however long the two writes are apart.
    GPIO->CLR[DBGIF_PORT] = (1UL << DBGIF_TMS_SWDIO_TXEN_PIN) | (1UL << DBGIF_RESET_TXEN_PIN);
    GPIO->DIRSET[DBGIF_PORT] = (1UL << DBGIF_TMS_SWDIO_TXEN_PIN) | (1UL << DBGIF_RESET_TXEN_PIN);

    // Everything that talks to the target starts as an input, so connecting the probe to a running
    // board disturbs nothing until the host asks for a port.
    GPIO->DIRCLR[DBGIF_PORT] = (1UL << DBGIF_TCK_SWCLK_PIN) | (1UL << DBGIF_TDI_PIN) | (1UL << DBGIF_TMS_SWDIO_PIN) |
                               (1UL << DBGIF_TDO_SWO_PIN) | (1UL << DBGIF_RESET_PIN);

    GPIO->SET[LED1_PORT] = (1UL << LED1_PIN); // active low, so set is off
    GPIO->DIRSET[LED1_PORT] = (1UL << LED1_PIN);
}
