/** The target console: USB CDC on one side, Flexcomm 0 on the other.
 *
 * The MCU-Link wires the target's serial port to P0_24 and P0_25, which are Flexcomm 0. TinyUSB's
 * board support also claims Flexcomm 0 for its own debug logging, on different pins, so vcom_init
 * runs after board_init and takes the peripheral back. Losing that logging costs nothing here --
 * the probe has no console of its own -- and a Flexcomm can only serve one set of pins.
 */
#include "fsl_clock.h"
#include "fsl_device_registers.h"
#include "fsl_iocon.h"
#include "fsl_usart.h"

#include "tusb.h"
#include "vcom.h"

#define VCOM_USART USART0
#define VCOM_RXD_PIN 24U
#define VCOM_TXD_PIN 25U
#define VCOM_PORT 0U
#define VCOM_PIN_MODE (IOCON_FUNC1 | IOCON_MODE_INACT | IOCON_DIGITAL_EN | IOCON_SLEW_FAST)
#define VCOM_DEFAULT_BAUD 115200U

static uint32_t vcom_clock(void) { return CLOCK_GetFlexCommClkFreq(0U); }

static void vcom_configure(uint32_t baud, uint8_t stop_bits, uint8_t parity, uint8_t data_bits) {
    usart_config_t config;
    USART_GetDefaultConfig(&config);

    config.baudRate_Bps = baud;
    config.enableTx = true;
    config.enableRx = true;
    config.stopBitCount = (stop_bits == 2U) ? kUSART_TwoStopBit : kUSART_OneStopBit;
    config.parityMode = (parity == 1U) ? kUSART_ParityOdd : (parity == 2U) ? kUSART_ParityEven : kUSART_ParityDisabled;
    config.bitCountPerChar = (data_bits == 7U) ? kUSART_7BitsPerChar : kUSART_8BitsPerChar;

    USART_Deinit(VCOM_USART);
    USART_Init(VCOM_USART, &config, vcom_clock());
}

__attribute__((noinline)) void vcom_init(void) {
    CLOCK_AttachClk(kFRO_HF_DIV_to_FLEXCOMM0);
    IOCON_PinMuxSet(IOCON, VCOM_PORT, VCOM_RXD_PIN, VCOM_PIN_MODE);
    IOCON_PinMuxSet(IOCON, VCOM_PORT, VCOM_TXD_PIN, VCOM_PIN_MODE);
    vcom_configure(VCOM_DEFAULT_BAUD, 1U, 0U, 8U);
}

/** The host setting the port's parameters is what a terminal does on open, so follow it. */
void tud_cdc_line_coding_cb(uint8_t itf, cdc_line_coding_t const* coding) {
    (void)itf;
    vcom_configure(coding->bit_rate, coding->stop_bits, coding->parity, coding->data_bits);
}

void vcom_task(void) {
    // USB to target. Only take what the UART can accept now, so a target that is not reading cannot
    // stall the whole firmware -- the DAP interface has to keep answering either way.
    while (tud_cdc_available() && (USART_GetStatusFlags(VCOM_USART) & kUSART_TxFifoNotFullFlag)) {
        uint8_t byte;
        if (tud_cdc_read(&byte, 1) != 1) {
            break;
        }
        USART_WriteByte(VCOM_USART, byte);
    }

    // Target to USB.
    while ((USART_GetStatusFlags(VCOM_USART) & kUSART_RxFifoNotEmptyFlag) && tud_cdc_write_available() > 0U) {
        uint8_t byte = USART_ReadByte(VCOM_USART);
        tud_cdc_write(&byte, 1);
    }
    tud_cdc_write_flush();
}
