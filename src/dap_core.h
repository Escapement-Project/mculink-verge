#ifndef DAP_CORE_H
#define DAP_CORE_H

#include <stdbool.h>
#include <stdint.h>

#define DAP_CORE_MAILBOX_WAKE_MASK 1U

/** MBOXIRQ registers by the core each one interrupts (fsl_mailbox.h's kMAILBOX_CM33_Core1/Core0). */
#define DAP_CORE_MAILBOX_CPU1 0U
#define DAP_CORE_MAILBOX_CPU0 1U

/** A transport's next command can run once that transport's reply ring has a slot for the reply.
 *  An older command on the other transport, whose host may have stopped reading, does not hold it. */
static inline bool dap_core_ready(bool request_waiting, bool reply_free) {
    return request_waiting && reply_free;
}

static inline bool dap_core_has_work(bool bulk_ready, bool hid_ready) {
    return bulk_ready || hid_ready;
}

/** CPU 1's entry, from core1_start.S once .data and .bss are set up. */
[[noreturn]] void core1_main(void);

/** CPU 0: open the memory CPU 1 needs to CPU 1, before starting it. */
void dap_core_security_init(void);

#endif
