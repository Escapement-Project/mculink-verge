/** The target consoles: a USB CDC interface each on one side, a Flexcomm UART each on the other.
 *
 * VCOM is the target's serial port on P0_24 and P0_25, Flexcomm 0, on both boards. The Pro adds
 * VCOM2 on J26, Flexcomm 1 on PIO1_10 and PIO1_11. board_straps.h says which exist: a jumper takes VCOM's
 * interface out of the configuration, so CDC instances number only the consoles that exist.
 *
 * TinyUSB's board support also claims Flexcomm 0 for its own debug logging, on different pins, so
 * vcom_init runs after board_init and takes the peripheral back. Losing that logging costs nothing
 * here -- the probe has no console of its own -- and a Flexcomm can only serve one set of pins.
 */
#include <stdint.h>

#include "fsl_clock.h"
#include "fsl_device_registers.h"
#include "fsl_iocon.h"
#include "fsl_usart.h"

#include "board/peripherals.h"
#include "board_straps.h"
#include "dap_shared.h"
#include "tusb.h"
#include "vcom.h"

#define VCOM_PIN_MODE (IOCON_MODE_INACT | IOCON_DIGITAL_EN | IOCON_SLEW_FAST)

typedef struct {
    USART_Type* usart;
    clock_attach_id_t clock;
    clock_div_name_t fractional_divider;
    uint32_t flexcomm;
    uint32_t port;
    uint32_t rxd;
    uint32_t txd;
    uint32_t function;
} vcom_uart_t;

static const vcom_uart_t vcom_uart = {
    .usart = USART0,
    .clock = kFRO_HF_DIV_to_FLEXCOMM0,
    .fractional_divider = kCLOCK_DivFlexFrg0,
    .flexcomm = 0U,
    .port = 0U,
    .rxd = 24U,
    .txd = 25U,
    .function = IOCON_FUNC1,
};
static const vcom_uart_t vcom2_uart = {
    .usart = USART1,
    .clock = kFRO_HF_DIV_to_FLEXCOMM1,
    .fractional_divider = kCLOCK_DivFlexFrg1,
    .flexcomm = 1U,
    .port = 1U,
    .rxd = 10U,
    .txd = 11U,
    .function = IOCON_FUNC2,
};

/** The board support's logging pins. Left on Flexcomm 0 they are a second RXD and TXD, and the
 *  receiver follows P0_29, which reads low on the plain board: a break that never ends, and not
 *  one byte from the target. */
#define BSP_LOG_PORT 0U
#define BSP_LOG_RXD_PIN 29U
#define BSP_LOG_TXD_PIN 30U
/** Nothing is wired to them on the plain board, and the pull-up keeps them quiet. */
#define UNUSED_PIN_MODE (IOCON_FUNC0 | IOCON_MODE_PULLUP | IOCON_DIGITAL_EN)
/** On the Pro they are the energy measurement's flip-flop reset and output, so no pull, as the
 *  part comes out of reset. */
#define UNDRIVEN_PIN_MODE (IOCON_FUNC0 | IOCON_MODE_INACT | IOCON_DIGITAL_EN)

#define VCOM_DEFAULT_BAUD 115200U

/** The consoles that exist, by CDC instance: in SRAM, since CPU 0's wait reads them. */
static struct {
    USART_Type* usart;
    uint32_t clock_hz;
} vcom[CFG_TUD_CDC];
static uint32_t vcom_count;

static void vcom_configure(uint32_t instance, uint32_t baud, uint8_t stop_bits, uint8_t parity, uint8_t data_bits) {
    usart_config_t config;
    USART_GetDefaultConfig(&config);

    config.baudRate_Bps = baud;
    config.enableTx = true;
    config.enableRx = true;
    config.stopBitCount = (stop_bits == 2U) ? kUSART_TwoStopBit : kUSART_OneStopBit;
    config.parityMode = (parity == 1U) ? kUSART_ParityOdd : (parity == 2U) ? kUSART_ParityEven : kUSART_ParityDisabled;
    config.bitCountPerChar = (data_bits == 7U) ? kUSART_7BitsPerChar : kUSART_8BitsPerChar;

    USART_Deinit(vcom[instance].usart);
    USART_Init(vcom[instance].usart, &config, vcom[instance].clock_hz);
}

static void vcom_start(const vcom_uart_t* uart) {
    CLOCK_AttachClk(uart->clock);
    // The board support leaves Flexcomm 0's fractional divider set for its own logging clock,
    // which puts it at 61.44 MHz rather than 96. Every common rate is still within the 3% the
    // driver accepts, but at a megabaud and above it is 2.40% out, and a link needs room for the
    // target's error too. A multiplier of zero is a ratio of one.
    CLOCK_SetClkDiv(uart->fractional_divider, 0U, false);
    IOCON_PinMuxSet(IOCON, uart->port, uart->rxd, uart->function | VCOM_PIN_MODE);
    IOCON_PinMuxSet(IOCON, uart->port, uart->txd, uart->function | VCOM_PIN_MODE);
    uint32_t const instance = vcom_count++;
    vcom[instance].usart = uart->usart;
    vcom[instance].clock_hz = CLOCK_GetFlexCommClkFreq(uart->flexcomm);
    vcom_configure(instance, VCOM_DEFAULT_BAUD, 1U, 0U, 8U);
}

__attribute__((noinline)) void vcom_init(void) {
    uint8_t const board = dap_shared.board;
    uint8_t const jumpers = dap_shared.jumpers;
    uint32_t const released = (board == BOARD_KIND_PLAIN) ? UNUSED_PIN_MODE : UNDRIVEN_PIN_MODE;
    IOCON_PinMuxSet(IOCON, BSP_LOG_PORT, BSP_LOG_RXD_PIN, released);
    IOCON_PinMuxSet(IOCON, BSP_LOG_PORT, BSP_LOG_TXD_PIN, released);
    if (board_has_vcom(board, jumpers)) {
        vcom_start(&vcom_uart);
    }
    if (board_has_vcom2(board, jumpers)) {
        vcom_start(&vcom2_uart);
    }
}

/** board_init calls this in place of the board support's, which CMakeLists.txt leaves out: that one
 *  starts Flexcomm 0 as a log console on P0_29 and P0_30, which on the Pro are energy measurement
 *  signals. */
void BOARD_InitBootPeripherals(void) {}

/** The host setting the port's parameters is what a terminal does on open, so follow it. */
void tud_cdc_line_coding_cb(uint8_t itf, cdc_line_coding_t const* coding) {
    if (itf < vcom_count) {
        vcom_configure(itf, coding->bit_rate, coding->stop_bits, coding->parity, coding->data_bits);
    }
}

__attribute__((section(".ramfunc"))) void vcom_task(void) {
    for (uint32_t i = 0U; i < vcom_count; i++) {
        USART_Type* const usart = vcom[i].usart;
        // USB to target. Only take what the UART can accept now, so a target that is not reading
        // cannot stall the whole firmware -- the DAP interface has to keep answering either way.
        while (tud_cdc_n_available(i) && (USART_GetStatusFlags(usart) & kUSART_TxFifoNotFullFlag)) {
            uint8_t byte;
            if (tud_cdc_n_read(i, &byte, 1) != 1) {
                break;
            }
            USART_WriteByte(usart, byte);
        }

        // Target to USB.
        while ((USART_GetStatusFlags(usart) & kUSART_RxFifoNotEmptyFlag) && tud_cdc_n_write_available(i) > 0U) {
            uint8_t byte = USART_ReadByte(usart);
            tud_cdc_n_write(i, &byte, 1);
        }
        // With a transfer in flight this sends nothing, and TinyUSB sends the rest when it
        // completes: this task does not run again until there are bytes to move.
        tud_cdc_n_write_flush(i);
    }
}

/** vcom_task's two loop conditions, which have to stay the same as its own. The FIFO counts are
 *  TinyUSB's, in SRAM, which tools/image.py checks. */
__attribute__((section(".ramfunc"), noinline)) bool vcom_ready(void) {
    for (uint32_t i = 0U; i < vcom_count; i++) {
        uint32_t const status = USART_GetStatusFlags(vcom[i].usart);
        if ((((status & kUSART_RxFifoNotEmptyFlag) != 0U) && (tud_cdc_n_write_available(i) > 0U)) ||
            (((status & kUSART_TxFifoNotFullFlag) != 0U) && (tud_cdc_n_available(i) > 0U))) {
            return true;
        }
    }
    return false;
}
