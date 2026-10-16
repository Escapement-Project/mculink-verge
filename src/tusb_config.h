#ifndef TUSB_CONFIG_H
#define TUSB_CONFIG_H

#ifndef BOARD_TUD_RHPORT
#define BOARD_TUD_RHPORT      1
#endif

// The stock firmware reports a 512 byte packet size, which is high-speed bulk. Matching it keeps
// the host's packet accounting the same as it is today.
#ifndef BOARD_TUD_MAX_SPEED
#define BOARD_TUD_MAX_SPEED   OPT_MODE_HIGH_SPEED
#endif

#define CFG_TUSB_MCU          OPT_MCU_LPC55
#define CFG_TUSB_OS           OPT_OS_NONE
#define CFG_TUD_ENABLED       1
#define CFG_TUD_MAX_SPEED     BOARD_TUD_MAX_SPEED

// CFG_TUSB_MEM_SECTION and CFG_TUSB_MEM_ALIGN come from the BSP on the command line, because the
// LPC55 USB controller needs its buffers in a particular region and alignment.

#define CFG_TUD_ENDPOINT0_SIZE 64

#define CFG_TUD_CDC           0
#define CFG_TUD_MSC           0
#define CFG_TUD_HID           0
#define CFG_TUD_MIDI          0
#define CFG_TUD_VENDOR        1

// One DAP packet each way, so a full packet never has to be split across FIFO wraps.
#define CFG_TUD_VENDOR_RX_BUFSIZE 512
#define CFG_TUD_VENDOR_TX_BUFSIZE 512

#endif
