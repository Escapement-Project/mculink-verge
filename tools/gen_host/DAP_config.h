/* Stand-in for src/DAP_config.h, so the transfer generator builds away from the firmware.
 *
 * The pins and the GPIO block's register offsets are the LPC55S69's, so the generated code is the
 * firmware's own apart from the addresses it loads. GPIO sits where the harness maps memory.
 */
#ifndef GEN_HOST_DAP_CONFIG_H
#define GEN_HOST_DAP_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#define DBGIF_TCK_SWCLK_PORT 0U
#define DBGIF_TCK_SWCLK_PIN 0U
#define DBGIF_TMS_SWDIO_PORT 0U
#define DBGIF_TMS_SWDIO_PIN 2U
#define DBGIF_TMS_SWDIO_TXEN_PORT 0U
#define DBGIF_TMS_SWDIO_TXEN_PIN 28U
#define DBGIF_TDI_PORT 0U
#define DBGIF_TDI_PIN 1U
#define DBGIF_TDO_SWO_PORT 0U
#define DBGIF_TDO_SWO_PIN 3U
#define CPU_CLOCK 150000000U
#define DAP_DEFAULT_SWJ_CLOCK 1000000U

typedef struct {
    volatile uint8_t B[4][32];
    uint8_t reserved0[0x1000 - 4 * 32];
    volatile uint32_t W[4][32];
    uint8_t reserved1[0x1000 - 4 * 32 * 4];
    volatile uint32_t DIR[4];
    uint8_t reserved2[0x70];
    volatile uint32_t MASK[4];
    uint8_t reserved3[0x70];
    volatile uint32_t PIN[4];
    uint8_t reserved4[0x70];
    volatile uint32_t MPIN[4];
    uint8_t reserved5[0x70];
    volatile uint32_t SET[4];
    uint8_t reserved6[0x70];
    volatile uint32_t CLR[4];
    uint8_t reserved7[0x70];
    volatile uint32_t NOT[4];
    uint8_t reserved8[0x70];
    volatile uint32_t DIRSET[4];
    uint8_t reserved9[0x70];
    volatile uint32_t DIRCLR[4];
} GPIO_Type;

_Static_assert(offsetof(GPIO_Type, W) == 0x1000, "LPC55S69 GPIO layout");
_Static_assert(offsetof(GPIO_Type, MASK) == 0x2080, "LPC55S69 GPIO layout");
_Static_assert(offsetof(GPIO_Type, MPIN) == 0x2180, "LPC55S69 GPIO layout");
_Static_assert(offsetof(GPIO_Type, SET) == 0x2200, "LPC55S69 GPIO layout");
_Static_assert(offsetof(GPIO_Type, CLR) == 0x2280, "LPC55S69 GPIO layout");
_Static_assert(offsetof(GPIO_Type, DIRSET) == 0x2380, "LPC55S69 GPIO layout");
_Static_assert(offsetof(GPIO_Type, DIRCLR) == 0x2400, "LPC55S69 GPIO layout");

#define GPIO_ADDR 0x38180000U
#define GPIO ((GPIO_Type*)GPIO_ADDR)
#define DBGIF_GPIO GPIO
#define __DSB() ((void)0)
#define __ISB() ((void)0)

#endif
