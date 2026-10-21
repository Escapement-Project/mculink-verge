#include "board_straps.h"

#include <stdbool.h>
#include <stddef.h>

#include "fsl_clock.h"
#include "fsl_device_registers.h"
#include "fsl_iocon.h"

#include "dap_shared.h"

/** The hardware-version straps (DAPLink's lpc55xx IO_Config.h, PRO.md), each an input pulled low
 *  by the board or a jumper. HW_VER_4 low says a board id is fitted, and HW_VER_3 is that id, high
 *  on a Pro. The plain MCU-Link's 64-pin package bonds neither, nor HW_VER_2, so with their
 *  pull-ups all three read high there. */
typedef struct {
    uint8_t port;
    uint8_t pin;
} strap_t;

static const strap_t hw_ver_2 = {1U, 30U};
static const strap_t hw_ver_3 = {1U, 4U};
static const strap_t hw_ver_4 = {1U, 5U};
static const strap_t hw_ver_6 = {0U, 18U};
static const strap_t hw_ver_7 = {0U, 27U};

/** Loop iterations for the pull-ups to charge the strap pins: at two cycles or more each, at least
 *  40 us at 150 MHz. */
#define STRAP_SETTLE 3000U

/** Digital input with its pull-up, which stays on afterwards as DAPLink leaves it. */
static void strap_input(strap_t strap) {
    IOCON_PinMuxSet(IOCON, strap.port, strap.pin, IOCON_FUNC0 | IOCON_MODE_PULLUP | IOCON_DIGITAL_EN);
    GPIO->DIRCLR[strap.port] = 1UL << strap.pin;
}

static bool strap_low(strap_t strap) { return GPIO->B[strap.port][strap.pin] == 0U; }

__attribute__((noinline)) void board_read_straps(void) {
    CLOCK_EnableClock(kCLOCK_Iocon);
    CLOCK_EnableClock(kCLOCK_Gpio0);
    CLOCK_EnableClock(kCLOCK_Gpio1);
    static const strap_t straps[] = {hw_ver_2, hw_ver_3, hw_ver_4, hw_ver_6, hw_ver_7};
    for (size_t i = 0U; i < sizeof(straps) / sizeof(straps[0]); i++) {
        strap_input(straps[i]);
    }
    for (volatile uint32_t i = 0U; i < STRAP_SETTLE; i++) {
    }

    uint8_t board = BOARD_KIND_PLAIN;
    if (strap_low(hw_ver_4)) {
        board = strap_low(hw_ver_3) ? BOARD_KIND_ON_BOARD : BOARD_KIND_PRO;
    }
    uint8_t jumpers = 0U;
    if (strap_low(hw_ver_6)) {
        jumpers |= BOARD_JUMPER_VCOM_OFF;
    }
    if (strap_low(hw_ver_7)) {
        jumpers |= BOARD_JUMPER_SWD_OFF;
    }
    if (strap_low(hw_ver_2)) {
        jumpers |= BOARD_JUMPER_USBSIO_OFF;
    }
    dap_shared.board = board;
    dap_shared.jumpers = jumpers;
}

/** The boards this image runs on, numbered as BOARD_KIND_*, for verge: it reads this from the file
 *  before flashing and puts the image only on a board it names. An
 *  image without one is for the plain MCU-Link only. verge finds it by the magic; the link script
 *  puts it right after the vector table. Fields are only ever appended, with the format raised. */
typedef struct {
    uint8_t magic[8];
    uint8_t format;
    uint8_t length;
    uint16_t zero;
    uint32_t boards;
} image_boards_t;

_Static_assert(sizeof(image_boards_t) == 16U, "verge reads sixteen bytes at format 1");

__attribute__((section(".image_boards"), used)) const image_boards_t image_boards = {
    .magic = {'V', 'E', 'R', 'G', 'E', 'I', 'M', 'G'},
    .format = 1U,
    .length = sizeof(image_boards_t),
    .boards = (1UL << BOARD_KIND_PLAIN) | (1UL << BOARD_KIND_PRO),
};
