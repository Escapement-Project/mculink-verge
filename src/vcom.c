/** The target consoles: a USB CDC interface each on one side, a Flexcomm UART each on the other.
 *
 * VCOM is the target's serial port on P0_24 and P0_25, Flexcomm 0, on both boards. The Pro adds
 * VCOM2 on J26, Flexcomm 1 on PIO1_10 and PIO1_11. board_straps.h says which exist: a jumper takes VCOM's
 * interface out of the configuration, so CDC instances number only the consoles that exist.
 *
 * TinyUSB's board support also claims Flexcomm 0 for its own debug logging, on different pins, so
 * vcom_init runs after board_init and takes the peripheral back. Losing that logging costs nothing
 * here -- the probe has no console of its own -- and a Flexcomm can only serve one set of pins.
 *
 * DMA0 moves every byte, so CPU 0's loop moves runs of them and has no deadline to meet. Each
 * console receives into a ring like SWO's (src/dma_ring.h), and sends what the host wrote in one
 * transfer at a time from a buffer of its own.
 */
#include <stdint.h>

#include "fsl_clock.h"
#include "fsl_device_registers.h"
#include "fsl_iocon.h"
#include "fsl_usart.h"

#include "board/peripherals.h"
#include "board_straps.h"
#include "dap_shared.h"
#include "dma0.h"
#include "dma_ring.h"
#include "led.h"
#include "tusb.h"
#include "vcom.h"
#include "vcom_line.h"

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

/** A receive ring of four segments. At 19.2 Mbaud, the fastest the UART makes, a reader can fall
 *  1.6 ms behind before it loses the oldest bytes. */
#define VCOM_RX_SEGMENTS 4U
#define VCOM_RX_RING_BYTES (VCOM_RX_SEGMENTS * DMA_RING_SEGMENT_BYTES)
static_assert(VCOM_RX_SEGMENTS >= 3U, "dma_ring_resync needs three segments");

/** A send is as much as TinyUSB holds from the host, in one descriptor. */
#define VCOM_TX_BYTES CFG_TUD_CDC_RX_BUFSIZE
static_assert(VCOM_TX_BYTES <= DMA_RING_SEGMENT_BYTES, "a send is one descriptor");

static_assert((VCOM_RX_SEGMENTS % 2U) == 0U, "each descriptor raises the same flag every lap");

/** Receive segment `segment` of the ring's count, with its flag (src/dma_ring.h). */
#define VCOM_RX_XFERCFG(segment)                                                                    \
    (DMA_CHANNEL_XFERCFG_CFGVALID_MASK | DMA_CHANNEL_XFERCFG_RELOAD_MASK |                          \
     (dma_ring_flag_b(segment) ? DMA_CHANNEL_XFERCFG_SETINTB_MASK : DMA_CHANNEL_XFERCFG_SETINTA_MASK) | \
     DMA_CHANNEL_XFERCFG_WIDTH(0U) | DMA_CHANNEL_XFERCFG_SRCINC(0U) | DMA_CHANNEL_XFERCFG_DSTINC(1U) |  \
     DMA_CHANNEL_XFERCFG_XFERCOUNT(DMA_RING_SEGMENT_BYTES - 1U))
#define VCOM_TX_XFERCFG(count)                                                                         \
    (DMA_CHANNEL_XFERCFG_CFGVALID_MASK | DMA_CHANNEL_XFERCFG_SETINTA_MASK | DMA_CHANNEL_XFERCFG_WIDTH(0U) | \
     DMA_CHANNEL_XFERCFG_SRCINC(1U) | DMA_CHANNEL_XFERCFG_DSTINC(0U) | DMA_CHANNEL_XFERCFG_XFERCOUNT((count) - 1U))

/** The consoles that exist, by CDC instance: in SRAM, since CPU 0's wait reads them. */
static struct {
    USART_Type* usart;
    uint32_t clock_hz;
} vcom[CFG_TUD_CDC];
static uint32_t vcom_count;

static struct {
    uint32_t channel;
    uint32_t read;                // the next position to hand to TinyUSB
    volatile uint32_t segments;   // completed, counted by vcom_dma_done
    volatile bool failed;
} vcom_rx[CFG_TUD_CDC];

static struct {
    uint32_t channel;
    volatile bool busy;           // a send is running; vcom_dma_done clears it
    volatile bool failed;
    bool recode;
    uint32_t cfg;
    uint32_t osr;
    uint32_t brg;
} vcom_tx[CFG_TUD_CDC];

/** DMA0 writes the rings and reads the send buffers behind the compiler's back. Each access is
 *  fenced by a __DMB, whose memory clobber makes the compiler read and write them for real. */
static uint8_t vcom_rx_ring[CFG_TUD_CDC][VCOM_RX_RING_BYTES];
static uint8_t vcom_tx_buffer[CFG_TUD_CDC][VCOM_TX_BYTES];
alignas(16) static volatile dma_descriptor_t vcom_rx_descriptors[CFG_TUD_CDC][VCOM_RX_SEGMENTS];

static bool vcom_configure(uint32_t instance) {
    USART_Type* const uart = vcom[instance].usart;
    if ((uart->STAT & USART_STAT_TXIDLE_MASK) == 0U) {
        return false;
    }
    uint32_t const mask = 1UL << vcom_rx[instance].channel;
    DMA0->COMMON[0].ENABLECLR = mask;
    if ((DMA0->COMMON[0].BUSY & mask) != 0U) {
        return false;
    }
    // Keep the FIFOs and RX ring intact across the format change.
    uart->CFG &= ~USART_CFG_ENABLE_MASK;
    uart->OSR = vcom_tx[instance].osr;
    uart->BRG = vcom_tx[instance].brg;
    uart->CFG = vcom_tx[instance].cfg | USART_CFG_ENABLE_MASK;
    DMA0->COMMON[0].ENABLESET = mask;
    // An error interrupt can disable the channel between the idle check and this enable.
    if (vcom_rx[instance].failed) {
        DMA0->COMMON[0].ENABLECLR = mask;
    }
    return true;
}

/** Start the receive ring, which runs from here on, through every reconfiguration. */
static void rx_start(uint32_t instance) {
    uint32_t const channel = vcom_rx[instance].channel;
    uint32_t const mask = 1UL << channel;
    volatile dma_descriptor_t* const ring = vcom_rx_descriptors[instance];
    uint32_t const source = (uint32_t)&vcom[instance].usart->FIFORD;
    for (uint32_t k = 0U; k < VCOM_RX_SEGMENTS; k++) {
        ring[k].xfercfg = VCOM_RX_XFERCFG(k);
        ring[k].source_end = source;
        ring[k].destination_end = (uint32_t)&vcom_rx_ring[instance][((k + 1U) * DMA_RING_SEGMENT_BYTES) - 1U];
        ring[k].link = (uint32_t)&ring[(k + 1U) % VCOM_RX_SEGMENTS];
    }
    // The channel's table entry runs segment 0 the first time round; the circle has it after that.
    volatile dma_descriptor_t* const entry = &dap_shared.dma_table[channel];
    entry->xfercfg = 0U;
    entry->source_end = source;
    entry->destination_end = ring[0].destination_end;
    entry->link = ring[0].link;
    DMA0->COMMON[0].INTA = mask;
    DMA0->COMMON[0].INTB = mask;
    vcom_rx[instance].segments = 0U;
    vcom_rx[instance].read = 0U;
    vcom_rx[instance].failed = false;
    __DMB();
    DMA0->CHANNEL[channel].CFG = DMA_CHANNEL_CFG_PERIPHREQEN_MASK;
    DMA0->COMMON[0].INTENSET = mask;
    DMA0->COMMON[0].ENABLESET = mask;
    DMA0->CHANNEL[channel].XFERCFG = VCOM_RX_XFERCFG(0U) | DMA_CHANNEL_XFERCFG_SWTRIG_MASK;
}

static void tx_start(uint32_t instance) {
    uint32_t const channel = vcom_tx[instance].channel;
    uint32_t const mask = 1UL << channel;
    volatile dma_descriptor_t* const entry = &dap_shared.dma_table[channel];
    entry->xfercfg = 0U;
    entry->destination_end = (uint32_t)&vcom[instance].usart->FIFOWR;
    entry->link = 0U;
    DMA0->CHANNEL[channel].CFG = DMA_CHANNEL_CFG_PERIPHREQEN_MASK;
    DMA0->COMMON[0].INTENSET = mask;
    DMA0->COMMON[0].ENABLESET = mask;
}

__attribute__((section(".ramfunc"))) static bool recover(uint32_t instance) {
    uint32_t const rx_mask = 1UL << vcom_rx[instance].channel;
    uint32_t const tx_mask = 1UL << vcom_tx[instance].channel;
    uint32_t const failed = (vcom_rx[instance].failed ? rx_mask : 0U) |
                            (vcom_tx[instance].failed ? tx_mask : 0U);
    if (failed == 0U) {
        return true;
    }
    if ((DMA0->COMMON[0].BUSY & failed) != 0U) {
        return false;
    }
    DMA0->COMMON[0].ABORT = failed;
    DMA0->COMMON[0].INTA = failed;
    DMA0->COMMON[0].ERRINT = failed;
    if ((failed & rx_mask) != 0U) {
        rx_start(instance);
    }
    if ((failed & tx_mask) != 0U) {
        // A prefix may have reached the UART; release the buffer without replaying it.
        vcom_tx[instance].busy = false;
        vcom_tx[instance].failed = false;
        tx_start(instance);
    }
    return !vcom_rx[instance].failed && !vcom_tx[instance].failed;
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
    usart_config_t config;
    USART_GetDefaultConfig(&config);
    config.baudRate_Bps = VCOM_DEFAULT_BAUD;
    config.enableTx = true;
    config.enableRx = true;
    if (USART_Init(uart->usart, &config, CLOCK_GetFlexCommClkFreq(uart->flexcomm)) != kStatus_Success) {
        return;
    }
    uart->usart->FIFOCFG |= USART_FIFOCFG_DMARX_MASK | USART_FIFOCFG_DMATX_MASK;
    uint32_t const instance = vcom_count++;
    vcom[instance].usart = uart->usart;
    vcom[instance].clock_hz = CLOCK_GetFlexCommClkFreq(uart->flexcomm);
    vcom_rx[instance].channel = VCOM_DMA_RX_CHANNEL(uart->flexcomm);
    vcom_tx[instance].channel = VCOM_DMA_TX_CHANNEL(uart->flexcomm);
    tx_start(instance);
    rx_start(instance);
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

/** Unsupported settings leave the last accepted coding intact. TinyUSB still reports the host's
 *  requested coding through GET_LINE_CODING. */
void tud_cdc_line_coding_cb(uint8_t itf, cdc_line_coding_t const* coding) {
    if (itf < vcom_count) {
        uint32_t osr, brg;
        if (!vcom_line_divisors(vcom[itf].clock_hz, coding->bit_rate, coding->stop_bits,
                                coding->parity, coding->data_bits, &osr, &brg)) {
            return;
        }
        uint32_t const parity = coding->parity == 1U ? kUSART_ParityOdd :
                                coding->parity == 2U ? kUSART_ParityEven : kUSART_ParityDisabled;
        vcom_tx[itf].cfg = USART_CFG_PARITYSEL(parity) | USART_CFG_STOPLEN(coding->stop_bits == 2U) |
                          USART_CFG_DATALEN(coding->data_bits == 8U);
        vcom_tx[itf].osr = osr;
        vcom_tx[itf].brg = brg;
        vcom_tx[itf].recode = true;
    }
}

/** Where console `instance`'s receive ring has been written to, unless `*lost`. */
__attribute__((always_inline)) static inline uint32_t rx_written(uint32_t instance, bool* lost) {
    return dma0_ring_written(&vcom_rx[instance].segments, vcom_rx[instance].channel, lost);
}

/** USB to target: one send at a time, of everything TinyUSB holds, and a new line coding between
 *  two sends. The host's next write waits for the line, the loop does not. */
__attribute__((section(".ramfunc"))) static void send(uint32_t instance) {
    if (vcom_tx[instance].busy || vcom_tx[instance].failed) {
        return;
    }
    if (vcom_tx[instance].recode) {
        if (!vcom_configure(instance)) {
            return;
        }
        vcom_tx[instance].recode = false;
    }
    if (tud_cdc_n_available(instance) == 0U) {
        return;
    }
    uint32_t const count = tud_cdc_n_read(instance, vcom_tx_buffer[instance], VCOM_TX_BYTES);
    if (count == 0U) {
        return;
    }
    uint32_t const channel = vcom_tx[instance].channel;
    dap_shared.dma_table[channel].source_end = (uint32_t)&vcom_tx_buffer[instance][count - 1U];
    vcom_tx[instance].busy = true;
    __DMB();
    DMA0->CHANNEL[channel].XFERCFG = VCOM_TX_XFERCFG(count) | DMA_CHANNEL_XFERCFG_SWTRIG_MASK;
    led_vcom();
}

/** Target to USB: as much of what has arrived as TinyUSB has room for. A ring the host has let
 *  fill drops its oldest bytes.
 *
 * Nothing checks the ring again after the copy, as SWO_Data does. This core counts the
 * completions itself, so the position is exact and the copy starts a whole segment clear of the
 * writer: a segment takes 0.53 ms at 19.2 Mbaud, and the copy is at most TinyUSB's 512 bytes.
 * Exact unless the count is lost: the next segment's flag says so before the interrupt runs, and
 * `failed` after it. The interrupt sets `failed` well after it clears the flags, but it runs whole
 * between two instructions of this loop. */
__attribute__((section(".ramfunc"))) static void receive(uint32_t instance) {
    bool lost = false;
    uint32_t const at = rx_written(instance, &lost);
    if (lost) {
        vcom_dma_error(1UL << vcom_rx[instance].channel);
    }
    if (lost || vcom_rx[instance].failed) {
        return;
    }
    uint32_t read = vcom_rx[instance].read;
    if (dma_ring_overrun(at, read, VCOM_RX_RING_BYTES)) {
        read = dma_ring_resync(at, VCOM_RX_RING_BYTES);
    }
    uint32_t count = dma_ring_unread(at, read);
    uint32_t const room = tud_cdc_n_write_available(instance);
    if (count > room) {
        count = room;
    }
    if (count != 0U) {
        led_vcom();
    }
    while (count != 0U) {
        uint32_t const offset = read % VCOM_RX_RING_BYTES;
        uint32_t run = VCOM_RX_RING_BYTES - offset;
        if (run > count) {
            run = count;
        }
        uint32_t const taken = tud_cdc_n_write(instance, &vcom_rx_ring[instance][offset], run);
        read += taken;
        count -= taken;
        if (taken != run) {
            break;
        }
    }
    vcom_rx[instance].read = read;
}

__attribute__((section(".ramfunc"), noinline)) void vcom_task(void) {
    for (uint32_t i = 0U; i < vcom_count; i++) {
        if (!recover(i)) {
            continue;
        }
        send(i);
        receive(i);
        // With a transfer in flight this sends nothing, and TinyUSB sends the rest when it
        // completes: this task does not run again until there are bytes to move.
        tud_cdc_n_write_flush(i);
    }
}

/** The conditions send and receive act on, which have to stay the same as theirs. The FIFO
 *  counts are TinyUSB's, in SRAM, which tools/image.py checks. */
__attribute__((section(".ramfunc"), noinline)) bool vcom_ready(void) {
    for (uint32_t i = 0U; i < vcom_count; i++) {
        if (vcom_rx[i].failed || vcom_tx[i].failed) {
            return true;
        }
        // The position is exact here, so a writer anywhere else is bytes or an overrun to resync.
        // A lost one is the ring to restart.
        bool lost = false;
        uint32_t const at = rx_written(i, &lost);
        if (lost || (!vcom_tx[i].busy && (vcom_tx[i].recode || (tud_cdc_n_available(i) > 0U))) ||
            ((at != vcom_rx[i].read) && (tud_cdc_n_write_available(i) > 0U))) {
            return true;
        }
    }
    return false;
}

__attribute__((section(".ramfunc"))) void vcom_dma_done(uint32_t done, uint32_t done_b) {
    uint32_t lost = 0U;
    for (uint32_t i = 0U; i < vcom_count; i++) {
        uint32_t const rx_mask = 1UL << vcom_rx[i].channel;
        if (!vcom_rx[i].failed) {
            switch (dma_ring_take(vcom_rx[i].segments, (done & rx_mask) != 0U, (done_b & rx_mask) != 0U)) {
                case DMA_RING_ONE:
                    vcom_rx[i].segments++;
                    break;
                case DMA_RING_LOST:
                    lost |= rx_mask;
                    break;
                default:
                    break;
            }
        }
        if ((done & (1UL << vcom_tx[i].channel)) != 0U && !vcom_tx[i].failed) {
            vcom_tx[i].busy = false;
        }
    }
    // A ring whose count is lost starts again, as a failed one does.
    if (lost != 0U) {
        vcom_dma_error(lost);
    }
}

__attribute__((section(".ramfunc"))) void vcom_dma_error(uint32_t failed) {
    uint32_t stop = 0U;
    for (uint32_t i = 0U; i < vcom_count; i++) {
        uint32_t const rx_mask = 1UL << vcom_rx[i].channel;
        uint32_t const tx_mask = 1UL << vcom_tx[i].channel;
        if ((failed & rx_mask) != 0U) {
            vcom_rx[i].failed = true;
            stop |= rx_mask;
        }
        if ((failed & tx_mask) != 0U) {
            vcom_tx[i].failed = true;
            stop |= tx_mask;
        }
    }
    if (stop != 0U) {
        DMA0->COMMON[0].ENABLECLR = stop;
        DMA0->COMMON[0].INTENCLR = stop;
    }
}
