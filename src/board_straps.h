/** Which board this is and which of its functions a jumper turns off: read once by CPU 0 at boot,
 *  from the hardware-version straps, and published to CPU 1 in dap_shared (BOARDS.md).
 */
#ifndef BOARD_STRAPS_H
#define BOARD_STRAPS_H

#include <stdbool.h>
#include <stdint.h>

/** The board, as vendor 0x82's board byte reports it, so the values never change meaning.
 *
 * The plain MCU-Link and the Pro run this firmware. J8 is the plain board's alone: the Pro's 100-pin
 * package bonds those pins to other nets. An on-board MCU-Link refuses cJTAG, since nobody has
 * framed OScan1 through its buffers. */
#define BOARD_KIND_PLAIN 0U
#define BOARD_KIND_PRO 1U
/** An MCU-Link built into an evaluation board. */
#define BOARD_KIND_ON_BOARD 2U

/** A jumper fitted, each turning one function off as stock honours it. Stock
 *  removes a jumpered VCOM from the configuration rather than leave an interface that does
 *  nothing, and so does this firmware. */
#define BOARD_JUMPER_VCOM_OFF (1U << 0)   // HW_VER_6: plain J4, Pro J14
#define BOARD_JUMPER_SWD_OFF (1U << 1)    // HW_VER_7: plain J5, Pro J13
#define BOARD_JUMPER_USBSIO_OFF (1U << 2) // HW_VER_2: Pro J15; the DIO pins (dio.h)

/** Whether the debug interfaces exist, CMSIS-DAP v2 and v1. The SWD jumper removes them, as stock
 *  removes its CMSIS-DAP interface: another debugger owns the target's lines then. */
static inline bool board_has_debug(uint8_t board, uint8_t jumpers) {
    (void)board;
    return (jumpers & BOARD_JUMPER_SWD_OFF) == 0U;
}

/** Which consoles exist, for the descriptors and src/vcom.c to agree on. Each CDC instance is the
 *  next of these that exists, in this order. J14 takes VCOM alone off the Pro, as stock keeps its
 *  second VCOM. */
static inline bool board_has_vcom(uint8_t board, uint8_t jumpers) {
    (void)board;
    return (jumpers & BOARD_JUMPER_VCOM_OFF) == 0U;
}

/** VCOM2 is Flexcomm 1 on J26, whose pins the plain board's package does not bond. */
static inline bool board_has_vcom2(uint8_t board, uint8_t jumpers) {
    (void)jumpers;
    return board == BOARD_KIND_PRO;
}

/** Verge Aux carries the DIO pins, and the Pro's supply and energy commands, beside a debugger, so
 *  no jumper removes it: the SWD jumper leaves another debugger on the target, which is when it is
 *  wanted most. An on-board MCU-Link has none of them. */
static inline bool board_has_aux(uint8_t board, uint8_t jumpers) {
    (void)jumpers;
    return (board == BOARD_KIND_PRO) || (board == BOARD_KIND_PLAIN);
}

/** CPU 0: read the straps into dap_shared's board and jumpers. Call after dap_shared_init and
 *  board_init, which muxes HW_VER_2 to a USB host signal, and before anything that depends on
 *  the board: the descriptors, the VCOMs and CPU 1. Out of line, so tools/image.py can run it with
 *  each board's straps. */
void board_read_straps(void);

#endif
