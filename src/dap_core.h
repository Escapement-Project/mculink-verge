#ifndef DAP_CORE_H
#define DAP_CORE_H

#include <stdbool.h>
#include <stdint.h>

#define DAP_CORE_MAILBOX_WAKE_MASK 1U

static inline bool dap_core_has_work(bool response_free, bool bulk_ready, bool hid_ready) {
    return response_free && (bulk_ready || hid_ready);
}

static inline uint32_t dap_core_mailbox_ack_mask(uint32_t pending) {
    return pending & DAP_CORE_MAILBOX_WAKE_MASK;
}

void dap_core1_main(void) __attribute__((noreturn));
void dap_core1_mailbox_wait_init(void);
void dap_core_security_init(void);

#endif
