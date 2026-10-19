#ifndef DAP_CORE_H
#define DAP_CORE_H

#include <stdbool.h>
#include <stdint.h>

#define DAP_CORE_MAILBOX_WAKE_MASK 1U

/** MBOXIRQ registers by the core each one interrupts (fsl_mailbox.h's kMAILBOX_CM33_Core1/Core0). */
#define DAP_CORE_MAILBOX_CPU1 0U
#define DAP_CORE_MAILBOX_CPU0 1U

static inline bool dap_core_has_work(bool response_free, bool bulk_ready, bool hid_ready) {
    return response_free && (bulk_ready || hid_ready);
}

/** CPU 1's entry, from core1_start.S once .data and .bss are set up. */
void core1_main(void) __attribute__((noreturn));

/** CPU 0: open the memory CPU 1 needs to CPU 1, before starting it. */
void dap_core_security_init(void);

#endif
