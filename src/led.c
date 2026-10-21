#include "led.h"

#include <stdbool.h>

#include "fsl_clock.h"
#include "fsl_device_registers.h"
#include "fsl_iocon.h"

#include "board_pins.h"
#include "tusb.h"

/** Timer counts in one PWM period, at a 1 MHz count: 488 Hz, well clear of visible flicker.
 *
 * The period is also the LEDs' tick, and each tick is an interrupt that lands wherever CPU 0
 * happens to be, usually in USB work, which it stretches. So the rate is a cost and this is as
 * slow as a clean fade allows. A power of two makes the ramps shifts. */
#define LED_PERIOD 2048U

/** A match register is how long each period the pin is held low, so this is a match that never
 *  arrives: the pin stays low and the LED fully lit. Zero is dark.
 *
 * Each timer takes its level from a shadow register as it wraps. The darkest levels are a count or
 * two, and a level written straight into the match register after the count had passed it would
 * miss its match and leave the LED fully lit for that period. */
#define LED_FULL LED_PERIOD

/** Lightness runs 0 to this, in even steps of L*. */
#define LED_LIGHT 255U

/** One FUNC ramp, as a shift of the tick: 512 ticks, 1.05 s. The JTAG triangle takes two. */
#define LED_RAMP_SHIFT 9U

/** One breath, as a shift of the tick: 2048 ticks, about four seconds. Twice the JTAG triangle,
 *  so the status LED's heartbeat is not mistaken for it. */
#define LED_BREATH_SHIFT 11U

_Static_assert((LED_RAMP_SHIFT >= 9U) && (LED_BREATH_SHIFT >= 9U), "a ramp has 256 steps of at least a tick");

/** How long a flicker inverts an LED, then how long it leaves it alone before activity can invert
 *  it again: 32 ticks, 65 ms each. Steady traffic flickers at 7.6 Hz. */
#define LED_FLICKER_TICKS 32U

/** The Pro's LEDs on port 1, schematic page 4. */
#define LED_PRO_PORT 1U
#define LED_USB_PIN 12U
#define LED_SWO_PIN 13U
#define LED_VCOM_PIN 15U
#define LED_FUNC_PIN 16U  // CTIMER1_MAT3, function 3
#define LED_SIO_PIN 17U
#define LED_NRG_PIN 18U

volatile uint8_t led_vcom_seen;

/** The LEDs' own clock, in PWM periods. */
static uint32_t led_ticks;

/** The PWM level for each lightness, filled by led_init. */
static uint16_t led_levels[LED_LIGHT + 1U];

typedef struct {
    uint8_t left;  // ticks of the flicker and the pause after it still to run
    bool again;    // activity during the pause, which starts the next flicker
} flicker_t;

static flicker_t status_flicker;
static flicker_t swo_flicker;
static flicker_t vcom_flicker;

/** CIE 1976 L*, 0 to 100 as lightness 0 to LED_LIGHT, to a PWM level. Luminance is
 *  ((L* + 16) / 116)^3, or L* / 903.3 at L* 8 and below. */
static uint32_t lightness_level(uint32_t lightness) {
    if (lightness * 100U <= 8U * LED_LIGHT) {
        uint32_t const scale = LED_LIGHT * 9033U;
        return ((lightness * 1000U * LED_FULL) + (scale / 2U)) / scale;
    }
    uint64_t const x = (lightness * 100U) + (16U * LED_LIGHT);
    uint64_t const scale = (uint64_t)116U * LED_LIGHT * 116U * LED_LIGHT * 116U * LED_LIGHT;
    return (uint32_t)(((x * x * x * LED_FULL) + (scale / 2U)) / scale);
}

/** Dark to light and back, starting dark: 2^shift ticks a round. */
static uint32_t triangle(uint32_t now, uint32_t shift) {
    uint32_t const half = 1U << (shift - 1U);
    uint32_t phase = now & ((half << 1) - 1U);
    if (phase >= half) {
        phase = (half << 1) - 1U - phase;
    }
    return phase >> (shift - 9U);
}

/** Take a flag another context sets. */
static bool take(volatile uint8_t* flag) {
    if (*flag == 0U) {
        return false;
    }
    *flag = 0U;
    return true;
}

/** `lightness`, inverted while a flicker runs. Activity starts one, unless one is already
 *  inverting: then it is the same burst. Activity in the pause after it starts the next once the
 *  pause is over, so a lone burst flickers once and steady traffic at an even rate. */
static uint32_t flicker(flicker_t* f, bool active, uint32_t lightness) {
    if (active && (f->left <= LED_FLICKER_TICKS)) {
        f->again = true;
    }
    if ((f->left == 0U) && f->again) {
        f->left = 2U * LED_FLICKER_TICKS;
        f->again = false;
    }
    bool const inverted = f->left > LED_FLICKER_TICKS;
    if (f->left != 0U) {
        f->left--;
    }
    if (!inverted) {
        return lightness;
    }
    return (lightness > (LED_LIGHT / 2U)) ? 0U : LED_LIGHT;
}

/** A switched LED: lit is a low pin. */
static void led_switch(uint32_t pin, uint32_t lightness) {
    GPIO->B[LED_PRO_PORT][pin] = (lightness > (LED_LIGHT / 2U)) ? 0U : 1U;
}

/** LED5 FUNC's lightness: which wire, by the shape of the ramp. */
static uint32_t func_lightness(uint32_t now) {
    if ((dap_shared.jumpers & BOARD_JUMPER_SWD_OFF) != 0U) {
        return LED_LIGHT;
    }
    uint32_t const ramp = (now & ((1U << LED_RAMP_SHIFT) - 1U)) >> (LED_RAMP_SHIFT - 8U);
    switch (dap_shared.led_wire) {
    case LED_WIRE_SWD:
        return ramp;
    case LED_WIRE_CJTAG:
        return LED_LIGHT - ramp;
    case LED_WIRE_JTAG:
        return LED_LIGHT - triangle(now, LED_RAMP_SHIFT + 1U);
    default:
        return 0U;
    }
}

/** The Pro's six. */
static void pro_tick(uint32_t now) {
    led_switch(LED_USB_PIN, (tud_mounted() && !tud_suspended()) ? LED_LIGHT : 0U);
    led_switch(LED_SWO_PIN, flicker(&swo_flicker, take(&dap_shared.led_swo), 0U));
    led_switch(LED_VCOM_PIN, flicker(&vcom_flicker, take(&led_vcom_seen), 0U));
    CTIMER1->MSR[3] = led_levels[func_lightness(now)];
}

/** The startup file's vector table names it, weakly; this is the strong definition. */
void CTIMER3_IRQHandler(void);

/** Once per PWM period, as the counter wraps. */
void CTIMER3_IRQHandler(void) {
    CTIMER3->IR = CTIMER_IR_MR3INT_MASK;
    uint32_t const now = ++led_ticks;

    uint32_t const status =
        (dap_shared.led_connected != 0U) ? LED_LIGHT : triangle(now, LED_BREATH_SHIFT);
    CTIMER3->MSR[0] = led_levels[flicker(&status_flicker, take(&dap_shared.led_activity), status)];
    if (dap_shared.board == BOARD_KIND_PRO) {
        pro_tick(now);
    }
}

/** A PWM timer at LED_PERIOD from FRO_HF: `period` the match register that resets it, the others
 *  as PWM outputs whose match is how long the pin is held low. */
static void led_timer(CTIMER_Type* timer, uint32_t period) {
    timer->TCR = CTIMER_TCR_CRST_MASK;
    timer->PR = (CLOCK_GetFroHfFreq() / 1000000U) - 1U;
    timer->MR[period] = LED_PERIOD - 1U;
}

/** The Pro's GPIO LEDs dark and driven, FUNC on CTIMER1 match 3 with match 0 as its period. The
 *  manual recommends match 3 for the period, but match 3 is the only one PIO1_16 has. */
static void pro_init(void) {
    CLOCK_EnableClock(kCLOCK_Gpio1);
    uint32_t const switched = (1UL << LED_USB_PIN) | (1UL << LED_SWO_PIN) | (1UL << LED_VCOM_PIN) |
                              (1UL << LED_SIO_PIN) | (1UL << LED_NRG_PIN);
    GPIO->SET[LED_PRO_PORT] = switched;
    GPIO->DIRSET[LED_PRO_PORT] = switched;
    for (uint32_t pin = LED_USB_PIN; pin <= LED_NRG_PIN; pin++) {
        if ((switched & (1UL << pin)) != 0U) {
            IOCON_PinMuxSet(IOCON, LED_PRO_PORT, pin, IOCON_FUNC0 | IOCON_MODE_INACT | IOCON_DIGITAL_EN);
        }
    }

    CLOCK_AttachClk(kFRO_HF_to_CTIMER1);
    CLOCK_EnableClock(kCLOCK_Timer1);
    led_timer(CTIMER1, 0U);
    CTIMER1->MR[3] = 0U;
    CTIMER1->MSR[3] = 0U;
    CTIMER1->MCR = CTIMER_MCR_MR0R_MASK | CTIMER_MCR_MR3RL_MASK;
    CTIMER1->PWMC = CTIMER_PWMC_PWMEN3_MASK;
    IOCON_PinMuxSet(IOCON, LED_PRO_PORT, LED_FUNC_PIN, IOCON_FUNC3 | IOCON_MODE_INACT | IOCON_DIGITAL_EN);
    CTIMER1->TCR = CTIMER_TCR_CEN_MASK;
}

/** PIO0_5's function 3 is CTIMER3's match 0 output. Driving it from CTIMER3 MR0 gives the
 * fade an idle stock probe shows.
 *
 * P0_5 is also ISP1, which the ROM samples out of reset to choose between the bootloader and the
 * firmware, and the LED's own resistor is what pulls it up when nothing drives it. A reset returns
 * IOCON and the timer to their defaults, and with them the pin to an input, before the ROM looks.
 * So a lit LED cannot strand a unit in the bootloader, and the reboot path has nothing to unwind.
 */
__attribute__((noinline)) void led_init(void) {
    for (uint32_t i = 0U; i <= LED_LIGHT; i++) {
        led_levels[i] = (uint16_t)lightness_level(i);
    }
    if (dap_shared.board == BOARD_KIND_PRO) {
        pro_init();
    }

    CLOCK_AttachClk(kFRO_HF_to_CTIMER3);
    CLOCK_EnableClock(kCLOCK_Timer3);
    led_timer(CTIMER3, 3U);
    CTIMER3->MR[0] = 0U;
    CTIMER3->MSR[0] = 0U;
    CTIMER3->MCR = CTIMER_MCR_MR3R_MASK | CTIMER_MCR_MR3I_MASK | CTIMER_MCR_MR0RL_MASK;
    CTIMER3->PWMC = CTIMER_PWMC_PWMEN0_MASK;

    IOCON_PinMuxSet(IOCON, LED1_PORT, LED1_PIN, IOCON_FUNC3 | IOCON_MODE_INACT | IOCON_DIGITAL_EN);

    // Lowest priority: the LED can wait for anything, and USB must not wait for the LED.
    NVIC_SetPriority(CTIMER3_IRQn, (1UL << __NVIC_PRIO_BITS) - 1UL);
    NVIC_EnableIRQ(CTIMER3_IRQn);
    CTIMER3->TCR = CTIMER_TCR_CEN_MASK;
}
