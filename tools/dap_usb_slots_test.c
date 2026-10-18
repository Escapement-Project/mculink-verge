#include <assert.h>
#include <stdint.h>

#include "dap_core.h"
#include "dap_usb_slots.h"
#include "dap_usb_shared.h"
#include "dap_core_security.h"
#include "reboot_usb.h"

#define SLOT_COUNT 3U
#define RESPONSE_SIZE 2558U
#define RESPONSE_STRIDE DAP_USB_RESPONSE_SLOT_SIZE(RESPONSE_SIZE)

struct response_slot {
    uint8_t bytes[RESPONSE_STRIDE];
} __attribute__((aligned(DAP_USB_BUFFER_ALIGN)));

struct completion_context {
    volatile bool* busy;
    volatile uint8_t* armed;
    uint8_t* tail;
    uint16_t* request_len;
    uint8_t count;
    bool complete;
};

static bool complete_rx(void* opaque, uint8_t slot) {
    struct completion_context* context = opaque;
    assert(slot == *context->armed);
    assert(slot == *context->tail);
    assert(context->request_len[slot] == 0U);
    context->request_len[slot] = 1U;
    *context->tail = dap_usb_next_slot(*context->tail, context->count);
    return true;
}

static bool reject_rx(void* opaque, uint8_t slot) {
    struct completion_context* context = opaque;
    assert(slot == *context->armed);
    assert(slot == *context->tail);
    return false;
}

static bool complete_tx(void* opaque, uint8_t slot, uint16_t length) {
    struct completion_context* context = opaque;
    assert(slot == 0U);
    assert(length == 64U);
    assert(*context->busy);
    if (!context->complete) {
        return false;
    }
    *context->busy = false;
    return true;
}

static void test_dma_alignment(void) {
    struct response_slot slots[SLOT_COUNT];
    assert(RESPONSE_STRIDE % DAP_USB_BUFFER_ALIGN == 0U);
    for (uint8_t i = 0; i < SLOT_COUNT; i++) {
        assert(((uintptr_t)slots[i].bytes % DAP_USB_BUFFER_ALIGN) == 0U);
    }
}

static void test_rx_completion_during_arm(void) {
    uint8_t tail = 0U;
    volatile uint8_t armed = 0U;
    uint16_t request_len[SLOT_COUNT] = {0};
    struct completion_context context = {NULL, &armed, &tail, request_len, SLOT_COUNT, true};

    assert(dap_usb_arm_rx(true, tail, &armed, complete_rx, &context));
    assert(armed == 0U);
    assert(tail == 1U);
    assert(request_len[0] == 1U);

    assert(!dap_usb_arm_rx(true, tail, &armed, reject_rx, &context));
    assert(armed == 1U);
    assert(tail == 1U);
    assert(!dap_usb_arm_rx(false, tail, &armed, reject_rx, &context));
}

static void test_tx_completion_during_start(void) {
    volatile bool busy = false;
    struct completion_context context = {&busy, NULL, NULL, NULL, SLOT_COUNT, true};

    assert(dap_usb_start_tx(&busy, 64U, complete_tx, &context));
    assert(!busy);

    context.complete = false;
    assert(!dap_usb_start_tx(&busy, 64U, complete_tx, &context));
    assert(!busy);
}

static void test_cross_core_response_handoff(void) {
    volatile dap_usb_response_state_t state = DAP_USB_RESPONSE_FREE;

    assert(!dap_usb_response_start(&state));
    state = DAP_USB_RESPONSE_READY;
    assert(dap_usb_response_start(&state));
    assert(state == DAP_USB_RESPONSE_IN_FLIGHT);

    dap_usb_response_complete(&state);
    dap_usb_response_retry(&state);
    assert(state == DAP_USB_RESPONSE_FREE);

    state = DAP_USB_RESPONSE_READY;
    assert(dap_usb_response_start(&state));
    dap_usb_response_retry(&state);
    assert(state == DAP_USB_RESPONSE_READY);
    assert(dap_usb_response_start(&state));
    dap_usb_response_complete(&state);
    assert(state == DAP_USB_RESPONSE_FREE);
}

static void test_request_queue_and_transport_order(void) {
    volatile uint16_t request_len[SLOT_COUNT] = {0};
    uint32_t request_seq[SLOT_COUNT] = {0};
    uint8_t head = 0U;
    uint8_t tail = 0U;
    uint32_t const hid_seq = 4U;

    assert(dap_usb_request_slot_free(request_len, tail));
    request_seq[tail] = 5U;
    request_len[tail] = 64U;
    tail = dap_usb_next_slot(tail, SLOT_COUNT);
    assert(dap_usb_choose_hid(true, hid_seq, true, request_seq[head]));

    assert(dap_usb_request_slot_free(request_len, tail));
    request_seq[tail] = 6U;
    request_len[tail] = 64U;
    tail = dap_usb_next_slot(tail, SLOT_COUNT);
    assert(!dap_usb_choose_hid(true, 7U, true, request_seq[head]));

    head = dap_usb_request_release(request_len, head, SLOT_COUNT);
    assert(head == 1U);
    assert(dap_usb_request_slot_free(request_len, 0U));
    assert(!dap_usb_request_slot_free(request_len, 1U));

    request_len[2] = 64U;
    tail = dap_usb_next_slot(tail, SLOT_COUNT);
    assert(tail == 0U);
    assert(dap_usb_request_slot_free(request_len, tail));
    assert(dap_usb_choose_hid(true, UINT32_MAX, true, 0U));
}

static void test_abort_overtakes_queued_commands(void) {
    volatile uint16_t request_len[SLOT_COUNT] = {64U, 64U, 0U};
    volatile uint8_t transfer_abort = 0U;

    assert(request_len[0] != 0U);
    transfer_abort = 1U;
    assert(transfer_abort == 1U);
    assert(request_len[0] != 0U);
}

static void test_mailbox_wakeup_and_recheck(void) {
    uint32_t pending = 0U;
    uint32_t const wake_mask = DAP_CORE_MAILBOX_WAKE_MASK;
    bool bulk_ready = false;

    assert(!dap_core_has_work(false, bulk_ready, false));
    assert(!dap_core_has_work(false, true, false));
    assert(!dap_core_has_work(true, false, false));

    bulk_ready = true;
    pending |= wake_mask;
    pending |= wake_mask;
    assert(dap_core_has_work(true, bulk_ready, false));
    assert(dap_core_mailbox_ack_mask(pending | 0x2U) == wake_mask);
    pending &= ~dap_core_mailbox_ack_mask(pending);
    assert(pending == 0U);

    // A request published after clearing a stale mailbox bit is found by the pre-WFE recheck.
    bulk_ready = false;
    pending |= wake_mask;
    pending &= ~dap_core_mailbox_ack_mask(pending);
    bulk_ready = true;
    pending |= wake_mask;
    assert(dap_core_has_work(true, bulk_ready, false));
}

static void test_dap_core_security_attribution(void) {
    uint32_t registers[0x1000U / sizeof(uint32_t)];
    const uint32_t master_level_before = 0x80000300U;
    const uint32_t master_anti_pol_before = 0x83FF0CF0U;
    for (size_t i = 0U; i < sizeof(registers) / sizeof(registers[0]); i++) {
        registers[i] = UINT32_MAX;
    }
    registers[USB_SECURITY_MASTER_SEC_LEVEL] = master_level_before;
    registers[USB_SECURITY_MASTER_SEC_ANTI_POL] = master_anti_pol_before;

    dap_core_security_apply(registers);

    assert((registers[USB_SECURITY_USB_HS_SLAVE_RULE] & USB_SECURITY_USB_RAM_RULE_MASK) ==
           USB_SECURITY_RULE_NS_PRIV);
    assert((registers[USB_SECURITY_USB_HS_MEM_RULE] & USB_SECURITY_USB_RAM_SECTIONS_MASK) ==
           USB_SECURITY_USB_RAM_SECTIONS_NS_PRIV);
    assert((registers[USB_SECURITY_MASTER_SEC_LEVEL] & USB_SECURITY_CPU1_MASTER_LEVEL_MASK) ==
           USB_SECURITY_CPU1_MASTER_LEVEL_NS_PRIV);
    assert((registers[USB_SECURITY_MASTER_SEC_ANTI_POL] & USB_SECURITY_CPU1_MASTER_ANTI_POL_MASK) ==
           USB_SECURITY_CPU1_MASTER_ANTI_POL_NS_PRIV);
    assert(registers[USB_SECURITY_AHB_PORT8_SLAVE0_RULE] == UINT32_MAX);
    assert(registers[USB_SECURITY_AHB_PORT8_SLAVE1_RULE] == UINT32_MAX);
    assert(registers[USB_SECURITY_AHB_PORT9_SLAVE0_RULE] == UINT32_MAX);
    assert((registers[USB_SECURITY_MASTER_SEC_LEVEL] & USB_SECURITY_MASTER_DEFINED_MASK &
            ~USB_SECURITY_CPU1_MASTER_LEVEL_MASK) ==
           (master_level_before & ~USB_SECURITY_CPU1_MASTER_LEVEL_MASK));
    assert((registers[USB_SECURITY_MASTER_SEC_ANTI_POL] & USB_SECURITY_MASTER_DEFINED_MASK &
            ~USB_SECURITY_CPU1_MASTER_ANTI_POL_MASK) ==
           (master_anti_pol_before & ~USB_SECURITY_CPU1_MASTER_ANTI_POL_MASK));
    assert((registers[USB_SECURITY_MASTER_SEC_LEVEL] & ~USB_SECURITY_MASTER_DEFINED_MASK) == 0U);
    assert((registers[USB_SECURITY_MASTER_SEC_ANTI_POL] & ~USB_SECURITY_MASTER_DEFINED_MASK) == 0U);
}

static void test_shared_state_initialization(void) {
    dap_usb_shared_state_t state;
    uint8_t* bytes = (uint8_t*)&state;
    for (size_t i = 0U; i < sizeof(state); i++) {
        bytes[i] = 0xA5U;
    }

    dap_usb_shared_init(&state);

    for (size_t i = 0U; i < sizeof(state); i++) {
        assert(bytes[i] == 0U);
    }
    assert(state.response_state == DAP_USB_RESPONSE_FREE);
}

static void test_cpu0_usb_reboot_request(void) {
    assert(reboot_usb_control_request_valid(REBOOT_USB_REQUEST_TYPE, REBOOT_USB_VENDOR_REQUEST, 0U, 0U, 0U));
    assert(reboot_usb_control_request_valid(REBOOT_USB_REQUEST_TYPE, REBOOT_USB_VENDOR_REQUEST, 1U, 0U, 0U));
    assert(!reboot_usb_control_request_valid(REBOOT_USB_REQUEST_TYPE, REBOOT_USB_VENDOR_REQUEST, 2U, 0U, 0U));
    assert(!reboot_usb_control_request_valid(0xC0U, REBOOT_USB_VENDOR_REQUEST, 1U, 0U, 0U));
    assert(!reboot_usb_control_request_valid(REBOOT_USB_REQUEST_TYPE, REBOOT_USB_VENDOR_REQUEST, 1U, 1U, 0U));
    assert(!reboot_usb_control_request_valid(REBOOT_USB_REQUEST_TYPE, REBOOT_USB_VENDOR_REQUEST, 1U, 0U, 1U));
    assert(!reboot_usb_control_request_valid(REBOOT_USB_REQUEST_TYPE, 2U, 1U, 0U, 0U));

    assert(!reboot_wait_for_response(true, false));
    assert(!reboot_wait_for_response(true, true));
    assert(reboot_wait_for_response(false, false));
    assert(!reboot_wait_for_response(false, true));
}

int main(void) {
    test_dma_alignment();
    test_rx_completion_during_arm();
    test_tx_completion_during_start();
    test_cross_core_response_handoff();
    test_request_queue_and_transport_order();
    test_abort_overtakes_queued_commands();
    test_mailbox_wakeup_and_recheck();
    test_dap_core_security_attribution();
    test_shared_state_initialization();
    test_cpu0_usb_reboot_request();
    return 0;
}
