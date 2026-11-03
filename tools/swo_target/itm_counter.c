/** Trace source for checking the probe's SWO capture, for a CC13x2/CC26x2 LaunchPad.
 *
 * Streams an incrementing 32-bit counter through ITM stimulus port 0, so every capture is checked
 * word by word: a packet is 0x03 then the counter, little-endian. Each write waits for the port's
 * FIFO, so the target never drops a word and any gap in the counter is the probe's.
 *
 * SWV leaves on DIO16, JTAG's TDO, which is an ordinary DIO while the part stays in cJTAG
 * (swcu185g 6.3), so it reaches the probe's TDO/SWO input on the existing harness. Debug over
 * JTAG takes the pin back.
 *
 * The host steers it through `swo_ctl` at the start of SRAM. A probe-rs detach writes DEMCR to 0,
 * which switches the ITM off, so the loop reconfigures whenever it finds TRCENA clear as well as
 * when the host changes the prescaler.
 */
#include <stdint.h>

#include "ti/devices/cc13x2_cc26x2/driverlib/ioc.h"
#include "ti/devices/cc13x2_cc26x2/driverlib/osc.h"
#include "ti/devices/cc13x2_cc26x2/driverlib/setup.h"
#include "ti/devices/cc13x2_cc26x2/inc/hw_cpu_itm.h"
#include "ti/devices/cc13x2_cc26x2/inc/hw_cpu_scs.h"
#include "ti/devices/cc13x2_cc26x2/inc/hw_cpu_tpiu.h"
#include "ti/devices/cc13x2_cc26x2/inc/hw_ioc.h"
#include "ti/devices/cc13x2_cc26x2/inc/hw_memmap.h"
#include "ti/devices/cc13x2_cc26x2/inc/hw_types.h"

#define SWO_CTL_MAGIC 0x304D5449U  // "ITM0"
#define ITM_FIFOREADY 1U
#define TPIU_PROTOCOL_NRZ 2U
#define TPIU_FFCR_TRIGIN 0x100U  // and the formatter off: ITM alone needs no frames
#define ITM_UNLOCK 0xC5ACCE55U

struct swo_ctl {
    uint32_t magic;
    uint32_t acpr;     ///< Host-written TPIU prescaler: baud = 48 MHz / (acpr + 1).
    uint32_t gap;      ///< Host-written busy-wait iterations between words; 0 sends back to back.
    uint32_t next;     ///< The counter value the next packet carries.
    uint32_t configs;  ///< How many times the trace path has been set up.
};

__attribute__((section(".swo_ctl"))) volatile struct swo_ctl swo_ctl;

extern uint32_t _stack_top, _data, _edata, _ldata, _bss, _ebss;
void reset_handler(void);
static void fault_handler(void) {
    for (;;) {
    }
}

__attribute__((section(".vectors"), used)) static void (*const vectors[16])(void) = {
    (void (*)(void))&_stack_top, reset_handler, fault_handler, fault_handler, fault_handler,
    fault_handler, fault_handler, 0, 0, 0, 0, fault_handler, fault_handler, 0, fault_handler,
    fault_handler,
};

static void trace_setup(uint32_t acpr) {
    HWREG(CPU_SCS_BASE + CPU_SCS_O_DEMCR) |= CPU_SCS_DEMCR_TRCENA;
    HWREG(CPU_TPIU_BASE + CPU_TPIU_O_CSPSR) = 1U;
    HWREG(CPU_TPIU_BASE + CPU_TPIU_O_SPPR) = TPIU_PROTOCOL_NRZ;
    HWREG(CPU_TPIU_BASE + CPU_TPIU_O_ACPR) = acpr;
    HWREG(CPU_TPIU_BASE + CPU_TPIU_O_FFCR) = TPIU_FFCR_TRIGIN;
    HWREG(CPU_ITM_BASE + CPU_ITM_O_LAR) = ITM_UNLOCK;
    HWREG(CPU_ITM_BASE + CPU_ITM_O_TCR) = 0U;
    HWREG(CPU_ITM_BASE + CPU_ITM_O_TPR) = 0U;
    HWREG(CPU_ITM_BASE + CPU_ITM_O_TER) = 1U;
    HWREG(CPU_ITM_BASE + CPU_ITM_O_TCR) = (1U << CPU_ITM_TCR_ATBID_S) | CPU_ITM_TCR_ITMENA;
    HWREG(IOC_BASE + IOC_O_IOCFG16) =
        IOC_PORT_MCU_SWV | IOC_CURRENT_8MA | IOC_STRENGTH_MAX | IOC_NO_IOPULL | IOC_SLEW_DISABLE;
    swo_ctl.configs++;
}

int main(void) {
    // The crystal, so the baud rate the probe measures carries no error from the target's clock.
    OSCHF_TurnOnXosc();
    while (!OSCHF_AttemptToSwitchToXosc()) {
    }

    swo_ctl.acpr = 47U;  // 1 MHz
    swo_ctl.gap = 0U;
    swo_ctl.next = 0U;
    swo_ctl.configs = 0U;
    swo_ctl.magic = SWO_CTL_MAGIC;

    uint32_t applied = ~0U;
    uint32_t n = 0U;
    for (;;) {
        uint32_t const acpr = swo_ctl.acpr;
        if ((acpr != applied) || ((HWREG(CPU_SCS_BASE + CPU_SCS_O_DEMCR) & CPU_SCS_DEMCR_TRCENA) == 0U)) {
            trace_setup(acpr);
            applied = acpr;
        }
        // Back to the top while full, so a detach that switches the ITM off cannot leave this waiting.
        if ((HWREG(CPU_ITM_BASE + CPU_ITM_O_STIM0) & ITM_FIFOREADY) == 0U) {
            continue;
        }
        HWREG(CPU_ITM_BASE + CPU_ITM_O_STIM0) = n;
        n++;
        swo_ctl.next = n;
        for (uint32_t i = swo_ctl.gap; i != 0U; i--) {
            __asm volatile("");
        }
    }
}

void reset_handler(void) {
    // driverlib is built for the hard-float ABI, so CP10 and CP11 go on before any of it runs.
    HWREG(CPU_SCS_BASE + CPU_SCS_O_CPACR) |= 0xFU << 20;
    // Not optional: the boot ROM leaves the device untrimmed, and TI's own startup does this first.
    SetupTrimDevice();
    uint32_t const* from = &_ldata;
    for (uint32_t* to = &_data; to < &_edata; to++) {
        *to = *from++;
    }
    for (uint32_t* to = &_bss; to < &_ebss; to++) {
        *to = 0U;
    }
    main();
}
