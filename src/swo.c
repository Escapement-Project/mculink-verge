/*
 * Copyright (c) 2013-2021 ARM Limited. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * From CMSIS-DAP, SWO.c. Changed by the Escapement Project: capture by DMA into a ring that never
 * pauses, and the differences listed below.
 */
/** SWO capture and the seven DAP_SWO commands, on CPU 1.
 *
 * Flexcomm 3 receives on P0_3, and DMA0 channel 8 copies every byte into dap_shared.swo_ring
 * through SWO_SEGMENTS reload descriptors linked in a circle. DMA0 is CPU 0's, shared with the
 * consoles: CPU 0 sets the controller up at boot, and this file touches channel 8 alone. Capture
 * costs no CPU time per byte and carries on through any command, however long. Each descriptor
 * raises INTA as it completes, and CPU 0's DMA interrupt counts them (src/main.c): the count is
 * what tells one lap of the ring from the next, since CPU 1 takes no interrupts and one command
 * can outlast a lap. The ring arithmetic is in src/swo_ring.h, where the host can test it.
 *
 * Replies follow ARM's SWO.c (CMSIS-DAP 2.1.2), with three differences. The ring never pauses: a
 * full ring overwrites its oldest bytes, so an overrun reports DAP_SWO_BUFFER_OVERRUN and the
 * reader skips forward to the oldest bytes still intact. P0_3 is Flexcomm 3's only while capture
 * runs, not from DAP_SWO_Mode on, so JTAG keeps TDO between captures. And DAP_SWO_ExtendedStatus
 * counts all eight bytes of index and timestamp in its reply length, where ARM's counts four.
 */
#include <stdbool.h>
#include <stdint.h>

#include "fsl_clock.h"
#include "fsl_device_registers.h"
#include "fsl_reset.h"

#include "DAP_config.h"
#include "DAP.h"
#include "board_pins.h"
#include "dap_shared.h"
#include "dma0.h"
#include "led.h"
#include "swo_ring.h"

static_assert(SWO_BUFFER_SIZE == SWO_USABLE_BYTES, "DAP_Info reports SWO_BUFFER_SIZE as the buffer");
static_assert(SWO_STREAM == 0, "no streaming endpoint yet");

#define SWO_FLEXCOMM 3U
#define SWO_USART USART3
#define SWO_DMA DMA0

/** A segment: byte reads from FIFORD into the next SWO_SEGMENT_BYTES of the ring, then on to the
 *  next descriptor with INTA raised. Nothing clears the trigger, so the circle runs until stopped. */
#define SWO_SEGMENT_XFERCFG                                                                         \
    (DMA_CHANNEL_XFERCFG_CFGVALID_MASK | DMA_CHANNEL_XFERCFG_RELOAD_MASK |                          \
     DMA_CHANNEL_XFERCFG_SETINTA_MASK | DMA_CHANNEL_XFERCFG_WIDTH(0U) | DMA_CHANNEL_XFERCFG_SRCINC(0U) | \
     DMA_CHANNEL_XFERCFG_DSTINC(1U) | DMA_CHANNEL_XFERCFG_XFERCOUNT(SWO_SEGMENT_BYTES - 1U))

#define SWO_LINE_ERRORS \
    (USART_STAT_FRAMERRINT_MASK | USART_STAT_PARITYERRINT_MASK | USART_STAT_RXNOISEINT_MASK | USART_STAT_DELTARXBRK_MASK)

/** Bounds on the two waits for hardware, in polls. Either finishes in well under a microsecond. */
#define SWO_DRAIN_POLLS 1000U

/** How long CPU 0's DMA interrupt can take between reading a completion and counting it. Its
 *  priority is the highest on CPU 0, so nothing preempts it in between. The poll bound is for the
 *  image checker, whose cycle counter does not move. */
#define SWO_COUNT_SETTLE_US 2U
#define SWO_COUNT_SETTLE_POLLS 10000U

enum {
    SWO_TRANSPORT_NONE,
    SWO_TRANSPORT_DATA_COMMAND,
};

static uint8_t transport = SWO_TRANSPORT_NONE;
static uint8_t mode = DAP_SWO_OFF;
static bool baudrate_set;
static bool capturing;
/** DAP_SWO_STREAM_ERROR and DAP_SWO_BUFFER_OVERRUN, held until a status reply reports them. */
static uint8_t errors;
/** dap_shared.swo_segments as capture started, so positions count from that capture. */
static uint32_t segments_at_start;
/** The position capture stopped at. */
static uint32_t written_at_stop;
static uint32_t read_position;
/** DAP_SWO_ExtendedStatus: the position last seen to move, and the test domain timer then. */
static uint32_t stamp_index;
static uint32_t stamp_tick;

/** RESET_PeripheralReset waits on a readback that the image checker's register model never
 *  gives, and the reset takes effect when the write lands. */
static void peripheral_reset(reset_ip_name_t peripheral) {
    uint32_t const index = (uint32_t)peripheral >> 16;
    uint32_t const mask = 1UL << ((uint32_t)peripheral & 0xFFFFU);
    SYSCON->PRESETCTRLSET[index] = mask;
    __DSB();
    SYSCON->PRESETCTRLCLR[index] = mask;
    __DSB();
}

static void uart_on(void) {
    CLOCK_AttachClk(kFRO_HF_DIV_to_FLEXCOMM3);
    CLOCK_SetClkDiv(kCLOCK_DivFlexFrg3, 0U, false);  // the fractional divider at one
    CLOCK_EnableClock(kCLOCK_FlexComm3);
    peripheral_reset(kFC3_RST_SHIFT_RSTn);
    FLEXCOMM3->PSELID = FLEXCOMM_PSELID_PERSEL(1U);  // USART
    SWO_USART->CFG = USART_CFG_DATALEN(1U);         // 8N1, still disabled
}

static void uart_off(void) {
    SWO_USART->CFG = 0U;
    CLOCK_DisableClock(kCLOCK_FlexComm3);
    CLOCK_AttachClk(kNONE_to_FLEXCOMM3);
}

/** Line errors and FIFO overflows since the last look. Flexcomm 3 has to be clocked. */
static void collect_errors(void) {
    uint32_t const line = SWO_USART->STAT & SWO_LINE_ERRORS;
    if (line != 0U) {
        SWO_USART->STAT = line;
        errors |= DAP_SWO_STREAM_ERROR;
    }
    if ((SWO_USART->FIFOSTAT & USART_FIFOSTAT_RXERR_MASK) != 0U) {
        SWO_USART->FIFOSTAT = USART_FIFOSTAT_RXERR_MASK;
        errors |= DAP_SWO_BUFFER_OVERRUN;
    }
    if (dap_shared.swo_dma_error != 0U) {
        dap_shared.swo_dma_error = 0U;
        errors |= DAP_SWO_STREAM_ERROR;
    }
}

/** The DMA's position since capture started. Positions are modulo 2^32, so taking the segments
 *  before the start off the total is the same as counting from the start. */
static uint32_t dma_written(void) {
    return dma0_ring_written(&dap_shared.swo_segments, SWO_DMA_CHANNEL) - (segments_at_start * SWO_SEGMENT_BYTES);
}

static uint32_t written(void) {
    return capturing ? dma_written() : written_at_stop;
}

static void capture_start(void) {
    volatile dma_descriptor_t* const ring = dap_shared.swo_dma;
    uint32_t const source = (uint32_t)&SWO_USART->FIFORD;
    for (uint32_t k = 0U; k < SWO_SEGMENTS; k++) {
        ring[k].xfercfg = SWO_SEGMENT_XFERCFG;
        ring[k].source_end = source;
        ring[k].destination_end = (uint32_t)&dap_shared.swo_ring[((k + 1U) * SWO_SEGMENT_BYTES) - 1U];
        ring[k].link = (uint32_t)&ring[(k + 1U) % SWO_SEGMENTS];
    }
    // The channel's table entry runs segment 0 the first time round; the circle has it after that.
    volatile dma_descriptor_t* const entry = &dap_shared.dma_table[SWO_DMA_CHANNEL];
    entry->xfercfg = 0U;
    entry->source_end = source;
    entry->destination_end = ring[0].destination_end;
    entry->link = ring[0].link;

    SWO_USART->FIFOCFG = USART_FIFOCFG_EMPTYRX_MASK | USART_FIFOCFG_EMPTYTX_MASK;
    SWO_USART->STAT = SWO_LINE_ERRORS;
    SWO_USART->FIFOSTAT = USART_FIFOSTAT_RXERR_MASK;
    SWO_DMA->COMMON[0].ERRINT = SWO_DMA_MASK;
    dap_shared.swo_dma_error = 0U;

    // A completion from the last capture that CPU 0 has read but not yet counted would land after
    // the snapshot, and every position from here would read a segment long.
    SWO_DMA->COMMON[0].INTA = SWO_DMA_MASK;
    uint32_t const cleared = TIMESTAMP_GET();
    for (uint32_t i = 0U; (i < SWO_COUNT_SETTLE_POLLS) &&
                          ((TIMESTAMP_GET() - cleared) < (SWO_COUNT_SETTLE_US * (TIMESTAMP_CLOCK / 1000000U)));
         i++) {
    }
    segments_at_start = dap_shared.swo_segments;
    read_position = 0U;
    written_at_stop = 0U;
    stamp_index = 0U;
    stamp_tick = TIMESTAMP_GET();
    errors = 0U;
    __DMB();

    // The DMA first, so the first byte the UART takes has somewhere to go.
    SWO_DMA->CHANNEL[SWO_DMA_CHANNEL].CFG = DMA_CHANNEL_CFG_PERIPHREQEN_MASK;
    SWO_DMA->COMMON[0].INTENSET = SWO_DMA_MASK;
    SWO_DMA->COMMON[0].ENABLESET = SWO_DMA_MASK;
    SWO_DMA->CHANNEL[SWO_DMA_CHANNEL].XFERCFG = SWO_SEGMENT_XFERCFG | DMA_CHANNEL_XFERCFG_SWTRIG_MASK;
    board_swo_pin(true);
    SWO_USART->FIFOCFG = USART_FIFOCFG_ENABLERX_MASK | USART_FIFOCFG_DMARX_MASK;
    SWO_USART->CFG |= USART_CFG_ENABLE_MASK;
    capturing = true;
}

static void capture_stop(void) {
    SWO_USART->CFG &= ~USART_CFG_ENABLE_MASK;
    board_swo_pin(false);
    // What the UART has already taken is the host's: let the DMA empty the FIFO before stopping it.
    for (uint32_t i = 0U; (i < SWO_DRAIN_POLLS) && ((SWO_USART->FIFOSTAT & USART_FIFOSTAT_RXNOTEMPTY_MASK) != 0U);
         i++) {
    }
    collect_errors();
    written_at_stop = dma_written();
    capturing = false;

    SWO_DMA->COMMON[0].ENABLECLR = SWO_DMA_MASK;
    for (uint32_t i = 0U; (i < SWO_DRAIN_POLLS) && ((SWO_DMA->COMMON[0].BUSY & SWO_DMA_MASK) != 0U); i++) {
    }
    SWO_DMA->COMMON[0].ABORT = SWO_DMA_MASK;
    SWO_DMA->COMMON[0].INTENCLR = SWO_DMA_MASK;
    SWO_USART->FIFOCFG = USART_FIFOCFG_EMPTYRX_MASK;
}

/** Bring the errors and the timestamp up to date, skip the reader past an overrun, and return
 *  the writer's position. */
static uint32_t poll(void) {
    if (capturing) {
        collect_errors();
    }
    uint32_t const at = written();
    if (at != stamp_index) {
        stamp_index = at;
        stamp_tick = TIMESTAMP_GET();
    }
    if (swo_overrun(at, read_position)) {
        errors |= DAP_SWO_BUFFER_OVERRUN;
        read_position = swo_resync(at);
    }
    return at;
}

/** The status byte, which reports each error once. */
static uint8_t take_status(void) {
    uint8_t const status = (uint8_t)((capturing ? DAP_SWO_CAPTURE_ACTIVE : 0U) | errors);
    errors = 0U;
    return status;
}

static void put_u32(uint8_t* out, uint32_t value) {
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
}

uint32_t SWO_Transport(const uint8_t* request, uint8_t* response) {
    bool const ok = !capturing && (request[0] <= SWO_TRANSPORT_DATA_COMMAND);
    if (ok) {
        transport = request[0];
    }
    response[0] = ok ? DAP_OK : DAP_ERROR;
    return (1U << 16) | 1U;
}

uint32_t SWO_Mode(const uint8_t* request, uint8_t* response) {
    if (capturing) {
        capture_stop();
    }
    if (mode == DAP_SWO_UART) {
        uart_off();
    }
    bool ok = true;
    switch (request[0]) {
        case DAP_SWO_OFF:
            break;
        case DAP_SWO_UART:
            uart_on();
            break;
        default:
            ok = false;
            break;
    }
    mode = ok ? request[0] : DAP_SWO_OFF;
    baudrate_set = false;
    response[0] = ok ? DAP_OK : DAP_ERROR;
    return (1U << 16) | 1U;
}

uint32_t SWO_Baudrate(const uint8_t* request, uint8_t* response) {
    uint32_t baudrate = (uint32_t)request[0] | ((uint32_t)request[1] << 8) | ((uint32_t)request[2] << 16) |
                        ((uint32_t)request[3] << 24);
    if (baudrate > SWO_UART_MAX_BAUDRATE) {
        baudrate = SWO_UART_MAX_BAUDRATE;
    }
    uint32_t actual = 0U;
    if (mode == DAP_SWO_UART) {
        // FRO_HF_DIV read from its registers. The main clock is a constant: the SDK keeps PLL0's
        // rate in a variable only CPU 0's image sets, so on this core it reads 0 Hz.
        uint32_t const fro = CLOCK_GetFroHfFreq() / ((SYSCON->FROHFDIV & SYSCON_FROHFDIV_DIV_MASK) + 1U);
        swo_uart_clock_t clock = {0};
        actual = swo_uart_clock(fro, CORE_CLOCK_HZ, baudrate, &clock);
        if (actual != 0U) {
            uint32_t const cfg = SWO_USART->CFG;
            SWO_USART->CFG = cfg & ~USART_CFG_ENABLE_MASK;
            // Only the main clock's own selectors are glitch-free, so the divider changes with no
            // clock in, and the new source arrives at the new ratio rather than the old one.
            CLOCK_AttachClk(kNONE_to_FLEXCOMM3);
            CLOCK_SetClkDiv(kCLOCK_DivFlexFrg3, clock.mult, false);
            CLOCK_AttachClk(clock.from_main ? kMAIN_CLK_to_FLEXCOMM3 : kFRO_HF_DIV_to_FLEXCOMM3);
            SWO_USART->OSR = USART_OSR_OSRVAL(clock.osrval);
            SWO_USART->BRG = USART_BRG_BRGVAL(clock.brgval);
            SWO_USART->CFG = cfg;
        } else if (capturing) {
            capture_stop();
        }
    }
    baudrate_set = (actual != 0U);
    put_u32(response, actual);
    return (4U << 16) | 4U;
}

uint32_t SWO_Control(const uint8_t* request, uint8_t* response) {
    bool const start = (request[0] & DAP_SWO_CAPTURE_ACTIVE) != 0U;
    bool ok = true;
    if (start && !capturing) {
        ok = (mode == DAP_SWO_UART) && baudrate_set;
        if (ok) {
            capture_start();
        }
    } else if (!start && capturing) {
        capture_stop();
    }
    response[0] = ok ? DAP_OK : DAP_ERROR;
    return (1U << 16) | 1U;
}

uint32_t SWO_Status(uint8_t* response) {
    uint32_t const at = poll();
    response[0] = take_status();
    put_u32(&response[1], swo_unread(at, read_position));
    return 5U;
}

uint32_t SWO_ExtendedStatus(const uint8_t* request, uint8_t* response) {
    uint32_t const at = poll();
    uint32_t length = 0U;
    if ((request[0] & 0x01U) != 0U) {
        response[length] = take_status();
        length += 1U;
    }
    if ((request[0] & 0x02U) != 0U) {
        put_u32(&response[length], swo_unread(at, read_position));
        length += 4U;
    }
    if ((request[0] & 0x04U) != 0U) {
        put_u32(&response[length], stamp_index);
        put_u32(&response[length + 4U], stamp_tick);
        length += 8U;
    }
    return (1U << 16) | length;
}

uint32_t SWO_Data(const uint8_t* request, uint8_t* response) {
    uint32_t const at = poll();
    uint8_t status = take_status();
    uint32_t count = 0U;
    if (transport == SWO_TRANSPORT_DATA_COMMAND) {
        uint32_t limit = (uint32_t)request[0] | ((uint32_t)request[1] << 8);
        if (limit > (DAP_PACKET_SIZE - 4U)) {
            limit = DAP_PACKET_SIZE - 4U;
        }
        count = swo_unread(at, read_position);
        if (count > limit) {
            count = limit;
        }
        swo_copy(&response[3], dap_shared.swo_ring, read_position, count);
        uint32_t const after = written();
        if (swo_overrun(after, read_position)) {
            // The DMA came round to these bytes while they were being copied.
            status |= DAP_SWO_BUFFER_OVERRUN;
            count = 0U;
            read_position = swo_resync(after);
        } else {
            read_position += count;
            if (count != 0U) {
                led_swo();
            }
        }
    }
    response[0] = status;
    response[1] = (uint8_t)count;
    response[2] = (uint8_t)(count >> 8);
    return (2U << 16) | (3U + count);
}
