#include "led.h"

#include "fsl_clock.h"
#include "fsl_device_registers.h"
#include "fsl_iocon.h"

#include "board_pins.h"

/** Timer counts in one PWM period, at a 1 MHz count: 488 Hz, well clear of visible flicker.
 *
 * The period is also the LED's tick, and each tick is an interrupt that lands wherever the core
 * happens to be, usually in a transfer, which it stretches. So the rate is a cost on the wire and
 * this is as slow as a clean fade allows. A power of two makes the breathing curve a shift. */
#define LED_PERIOD 2048U

/** MR0 is how long each period the pin is held low, so this is a match that never arrives: the
 *  pin stays low and the LED fully lit. Zero is dark. */
#define LED_FULL LED_PERIOD

/** Half a blink, as a shift of the tick: 32 ticks of 2 ms, so a little under 8 Hz.
 *
 * Taking the phase from the tick rather than restarting it per command is what makes a stream of
 * commands blink steadily: each one joins the blink already in progress.
 */
#define LED_BLINK_SHIFT 5U

/** How long after a command the blinking stops. Three half-blinks, so one lone command always
 *  covers a whole lit half however it lands in the phase. */
#define LED_QUIET_TICKS (3U << LED_BLINK_SHIFT)

/** One breath, as a shift of the tick: 2048 ticks, about four seconds. */
#define LED_BREATH_SHIFT 11U

volatile uint8_t led_activity_seen;
static volatile uint8_t connected;

/** The LED's own clock, in PWM periods. */
static uint32_t led_ticks;

/** Far enough back that a probe nobody has spoken to yet starts out breathing. */
static uint32_t last_activity = 0U - LED_QUIET_TICKS;

void led_connected(uint32_t on) {
    connected = (on != 0U);
}

/** Brightness through one breath: a triangle, squared so the fade looks even to the eye rather
 *  than lingering near full. */
static uint32_t breath(uint32_t now) {
    const uint32_t half = 1U << (LED_BREATH_SHIFT - 1U);
    uint32_t phase = now & ((half << 1) - 1U);
    if (phase >= half) {
        phase = (half << 1) - 1U - phase;
    }
    phase >>= LED_BREATH_SHIFT - 1U - 8U; // 0 to 255
    return (phase * phase) >> 5;          // 0 to 2032, inside LED_PERIOD
}

/** The startup file's vector table names it, weakly; this is the strong definition. */
void CTIMER3_IRQHandler(void);

/** Once per PWM period, just after the counter has wrapped. MR0 written here takes effect at once,
 *  and with the count this close to zero no period sees a half-applied level. */
void CTIMER3_IRQHandler(void) {
    CTIMER3->IR = CTIMER_IR_MR3INT_MASK;
    const uint32_t now = ++led_ticks;
    if (led_activity_seen != 0U) {
        led_activity_seen = 0U;
        last_activity = now;
    }

    uint32_t level;
    if ((uint32_t)(now - last_activity) < LED_QUIET_TICKS) {
        level = (((now >> LED_BLINK_SHIFT) & 1U) != 0U) ? LED_FULL : 0U;
    } else if (connected != 0U) {
        level = LED_FULL;
    } else {
        level = breath(now);
    }
    CTIMER3->MR[0] = level;
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
    CLOCK_AttachClk(kFRO_HF_to_CTIMER3);
    CLOCK_EnableClock(kCLOCK_Timer3);

    CTIMER3->TCR = CTIMER_TCR_CRST_MASK;
    CTIMER3->PR = (CLOCK_GetFroHfFreq() / 1000000U) - 1U;
    CTIMER3->MR[3] = LED_PERIOD - 1U;
    CTIMER3->MR[0] = 0U;
    CTIMER3->MCR = CTIMER_MCR_MR3R_MASK | CTIMER_MCR_MR3I_MASK;
    CTIMER3->PWMC = CTIMER_PWMC_PWMEN0_MASK;

    IOCON_PinMuxSet(IOCON, LED1_PORT, LED1_PIN, IOCON_FUNC3 | IOCON_MODE_INACT | IOCON_DIGITAL_EN);

    // Lowest priority: the LED can wait for anything, and USB must not wait for the LED.
    NVIC_SetPriority(CTIMER3_IRQn, (1UL << __NVIC_PRIO_BITS) - 1UL);
    NVIC_EnableIRQ(CTIMER3_IRQn);
    CTIMER3->TCR = CTIMER_TCR_CEN_MASK;
}
