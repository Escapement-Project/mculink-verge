#include "board_pins.h"

#include "fsl_clock.h"
#include "fsl_device_registers.h"
#include "fsl_iocon.h"

#include "dap_shared.h"
#include "marker.h"

_Static_assert(DBGIF_TCK_SWCLK_PORT == DBGIF_PORT && DBGIF_TDI_PORT == DBGIF_PORT &&
                   DBGIF_TMS_SWDIO_PORT == DBGIF_PORT && DBGIF_TDO_SWO_PORT == DBGIF_PORT &&
                   DBGIF_TMS_SWDIO_TXEN_PORT == DBGIF_PORT && DBGIF_RESET_PORT == DBGIF_PORT &&
                   DBGIF_RESET_TXEN_PORT == DBGIF_PORT && DBGIF_ISP_CTRL_PORT == DBGIF_PORT,
               "the per-port writes below assume every signal is on DBGIF_PORT");

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

/** Digital, no pull, standard slew: what every pin here wants.
 *
 * The buffers on this board give the drive strength, so nothing here needs a pull resistor. The
 * two TXEN pins are the exception that matters: they must come up low, because a buffer enabled
 * before the firmware knows the port state would drive the target's wire.
 */
#define PIN_MODE (IOCON_MODE_INACT | IOCON_DIGITAL_EN)

/** Function 10 hands a port 0 pin to the secure GPIO block, which is DBGIF_GPIO. */
#define DEBUG_PIN_MODE (IOCON_FUNC10 | PIN_MODE)

/** P0_3's only receive function: Flexcomm 3's RXD. */
#define SWO_PIN_MODE (IOCON_FUNC1 | PIN_MODE)

uint8_t board_kind = BOARD_KIND_PLAIN;

__attribute__((noinline)) void board_debug_pins_init(void) {
    CLOCK_EnableClock(kCLOCK_Iocon);
    CLOCK_EnableClock(kCLOCK_Gpio_Sec);
    board_kind = dap_shared.board;

    // The secure GPIO's state first, while the pins still belong to the ordinary GPIO, so each pin
    // goes straight to its final level when IOCON hands it over. Buffers off, then the pins they
    // gate: the other way round drives the target for however long the two writes are apart.
    DBGIF_GPIO->CLR[DBGIF_PORT] = (1UL << DBGIF_TMS_SWDIO_TXEN_PIN) | (1UL << DBGIF_RESET_TXEN_PIN);
    DBGIF_GPIO->DIRSET[DBGIF_PORT] = (1UL << DBGIF_TMS_SWDIO_TXEN_PIN) | (1UL << DBGIF_RESET_TXEN_PIN);

    // Everything that talks to the target starts as an input, so connecting the probe to a running
    // board disturbs nothing until the host asks for a port.
    DBGIF_GPIO->DIRCLR[DBGIF_PORT] = (1UL << DBGIF_TCK_SWCLK_PIN) | (1UL << DBGIF_TDI_PIN) |
                                     (1UL << DBGIF_TMS_SWDIO_PIN) | (1UL << DBGIF_TDO_SWO_PIN) |
                                     (1UL << DBGIF_RESET_PIN);

    // Every debug signal is on port 0, which is what lets one port number serve the whole loop
    // and the single-port writes above.
    static const uint8_t pins[] = {
        DBGIF_TCK_SWCLK_PIN, DBGIF_TDI_PIN,            DBGIF_TMS_SWDIO_PIN,  DBGIF_TDO_SWO_PIN,
        DBGIF_RESET_PIN,     DBGIF_TMS_SWDIO_TXEN_PIN, DBGIF_RESET_TXEN_PIN,
    };
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        IOCON_PinMuxSet(IOCON, DBGIF_PORT, pins[i], DEBUG_PIN_MODE);
    }
    // PIO0_13 is one of the two I2C pins, and its IOCON word has an EGP bit that the others lack:
    // clear, the pin is an open-drain I2C cell that cannot drive high. RESET_TXEN would then never
    // turn the reset buffer round, and nRESET would never reach the target.
    IOCON_PinMuxSet(IOCON, DBGIF_PORT, DBGIF_RESET_TXEN_PIN, DEBUG_PIN_MODE | IOCON_GPIO_MODE);

    // PIO0_14 is the other I2C pin, so it needs the same bit to release the target's ISP pin. An
    // on-board MCU-Link's nets are unknown, so it is left alone there.
    if (board_kind != BOARD_KIND_ON_BOARD) {
        DBGIF_GPIO->SET[DBGIF_PORT] = 1UL << DBGIF_ISP_CTRL_PIN;
        DBGIF_GPIO->DIRSET[DBGIF_PORT] = 1UL << DBGIF_ISP_CTRL_PIN;
        IOCON_PinMuxSet(IOCON, DBGIF_PORT, DBGIF_ISP_CTRL_PIN, DEBUG_PIN_MODE | IOCON_GPIO_MODE);
    }

#ifdef MARKER_PINS
    // J8 is on port 1, which the secure GPIO does not reach, so it stays on the ordinary GPIO.
    if (board_kind == BOARD_KIND_PLAIN) {
        CLOCK_EnableClock(kCLOCK_Gpio1);
        for (uint32_t pin = 0U; pin < 4U; pin++) {
            IOCON_PinMuxSet(IOCON, J8_PORT, pin, IOCON_FUNC0 | PIN_MODE);
        }
        GPIO->CLR[J8_PORT] = J8_PIN_MASK;
        GPIO->DIRSET[J8_PORT] = J8_PIN_MASK;
    } else {
        marker_mask = 0U;
    }
#endif
}

void board_swo_pin(bool capture) {
    IOCON_PinMuxSet(IOCON, DBGIF_PORT, DBGIF_TDO_SWO_PIN, capture ? SWO_PIN_MODE : DEBUG_PIN_MODE);
}
