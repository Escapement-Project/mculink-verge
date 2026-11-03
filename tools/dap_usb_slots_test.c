#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "dap_core.h"
#include "dap_hid.h"
#include "dap_usb_slots.h"
#include "dap_shared.h"
#include "dap_core_security.h"
#include "reboot_usb.h"

#define SLOT_COUNT 3U

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
    assert(slot == 2U);
    assert(length == 64U);
    assert(*context->busy);
    if (!context->complete) {
        return false;
    }
    *context->busy = false;
    return true;
}

static void test_rx_completion_during_arm(void) {
    uint8_t tail = 0U;
    volatile uint8_t armed = 0U;
    uint16_t request_len[SLOT_COUNT] = {0};
    struct completion_context context = {nullptr, &armed, &tail, request_len, SLOT_COUNT, true};

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
    struct completion_context context = {&busy, nullptr, nullptr, nullptr, SLOT_COUNT, true};

    assert(dap_usb_start_tx(&busy, 2U, 64U, complete_tx, &context));
    assert(!busy);

    context.complete = false;
    assert(!dap_usb_start_tx(&busy, 2U, 64U, complete_tx, &context));
    assert(!busy);
}

/** CPU 1 fills reply slots at the tail, as core1_main.c does; returns whether it could. */
static bool core1_publish(volatile uint16_t* response_len, uint8_t* tail, uint16_t length) {
    if (!dap_core_has_work(dap_usb_slot_free(response_len, *tail), true, false)) {
        return false;
    }
    response_len[*tail] = length;
    *tail = dap_usb_next_slot(*tail, SLOT_COUNT);
    return true;
}

/** CPU 1 runs as far ahead as there are reply slots, and CPU 0 sends them in the order it ran. */
static void test_cross_core_response_handoff(void) {
    volatile uint16_t response_len[SLOT_COUNT] = {0};
    uint8_t head = 0U;
    uint8_t tail = 0U;

    for (uint16_t i = 0U; i < SLOT_COUNT; i++) {
        assert(core1_publish(response_len, &tail, (uint16_t)(10U + i)));
    }
    assert(!core1_publish(response_len, &tail, 99U));
    assert(tail == head);

    // CPU 0 sends the oldest first, and each release lets CPU 1 publish one more.
    assert(response_len[head] == 10U);
    head = dap_usb_slot_release(response_len, head, SLOT_COUNT);
    assert(core1_publish(response_len, &tail, 13U));
    assert(!core1_publish(response_len, &tail, 99U));
    for (uint16_t expect = 11U; expect <= 13U; expect++) {
        assert(response_len[head] == expect);
        head = dap_usb_slot_release(response_len, head, SLOT_COUNT);
    }
    assert(dap_usb_slot_free(response_len, head));
    assert(head == tail);
}

static void test_request_queue_and_transport_order(void) {
    volatile uint16_t request_len[SLOT_COUNT] = {0};
    uint32_t request_seq[SLOT_COUNT] = {0};
    uint8_t head = 0U;
    uint8_t tail = 0U;
    uint32_t const hid_seq = 4U;

    assert(dap_usb_slot_free(request_len, tail));
    request_seq[tail] = 5U;
    request_len[tail] = 64U;
    tail = dap_usb_next_slot(tail, SLOT_COUNT);
    assert(dap_usb_choose_hid(true, hid_seq, true, request_seq[head]));

    assert(dap_usb_slot_free(request_len, tail));
    request_seq[tail] = 6U;
    request_len[tail] = 64U;
    tail = dap_usb_next_slot(tail, SLOT_COUNT);
    assert(!dap_usb_choose_hid(true, 7U, true, request_seq[head]));

    head = dap_usb_slot_release(request_len, head, SLOT_COUNT);
    assert(head == 1U);
    assert(dap_usb_slot_free(request_len, 0U));
    assert(!dap_usb_slot_free(request_len, 1U));

    request_len[2] = 64U;
    tail = dap_usb_next_slot(tail, SLOT_COUNT);
    assert(tail == 0U);
    assert(dap_usb_slot_free(request_len, tail));
    assert(dap_usb_choose_hid(true, UINT32_MAX, true, 0U));
}

/** A new configuration's numbering, as tud_mount_cb starts it, across the wrap of the counter:
 *  CPU 1 runs only the new host's command, and CPU 0 sends only its reply. */
static void test_new_configuration_drops_the_last_hosts_work(void) {
    uint32_t const queued[] = {UINT32_MAX - 1U, UINT32_MAX};  // the last host's, not yet run
    uint32_t const replied = queued[0] - 1U;                  // and a reply it never read

    uint32_t const first_live = queued[1] + 1U;  // the host resets the bus and sets a configuration
    uint32_t const fresh = first_live;

    uint32_t ran = 0U;
    uint32_t replies[] = {replied, 0U};
    uint32_t published = 1U;
    uint32_t const taken[] = {queued[0], queued[1], fresh};
    for (uint32_t i = 0U; i < 3U; i++) {
        if (!dap_usb_stale(taken[i], first_live)) {
            ran++;
            replies[published++] = taken[i];
        }
    }
    assert(ran == 1U && published == 2U && replies[1] == fresh);

    uint32_t sent = 0U;
    for (uint32_t i = 0U; i < published; i++) {
        if (!dap_usb_stale(replies[i], first_live)) {
            assert(replies[i] == fresh);
            sent++;
        }
    }
    assert(sent == 1U);
    assert(!dap_usb_stale(first_live + 1000U, first_live));
}

/** CPU 0 publishes a command, then raises CPU 1's mailbox register. CPU 1 clears the register,
 *  then checks for work, and waits on the level only if the check found none. At every
 *  interleaving of those four steps, and whether or not the register was left raised before,
 *  CPU 1 either finds the work or finds the register raised. */
static void test_mailbox_level_handoff(void) {
    assert(!dap_core_has_work(false, true, false));
    assert(!dap_core_has_work(true, false, false));
    assert(dap_core_has_work(true, true, false));
    assert(dap_core_has_work(true, false, true));

    for (uint32_t stale = 0U; stale < 2U; stale++) {
        for (uint32_t publish_at = 0U; publish_at < 4U; publish_at++) {
            for (uint32_t raise_at = publish_at + 1U; raise_at < 4U; raise_at++) {
                bool published = false;
                bool raised = stale != 0U;
                bool found = false;
                uint32_t cpu1_step = 0U;
                for (uint32_t t = 0U; t < 4U; t++) {
                    if (t == publish_at) {
                        published = true;
                    } else if (t == raise_at) {
                        raised = true;
                    } else if (cpu1_step++ == 0U) {
                        raised = false;
                    } else {
                        found = dap_core_has_work(true, published, false);
                    }
                }
                assert(found || raised);
            }
        }
    }
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
    assert((registers[USB_SECURITY_MASTER_SEC_LEVEL] & USB_SECURITY_DMA0_MASTER_LEVEL_MASK) ==
           USB_SECURITY_DMA0_MASTER_LEVEL_NS_PRIV);
    assert((registers[USB_SECURITY_MASTER_SEC_ANTI_POL] & USB_SECURITY_DMA0_MASTER_LEVEL_MASK) ==
           USB_SECURITY_DMA0_MASTER_ANTI_POL_NS_PRIV);
    assert(registers[USB_SECURITY_AHB_PORT8_SLAVE0_RULE] == UINT32_MAX);
    assert(registers[USB_SECURITY_AHB_PORT8_SLAVE1_RULE] == UINT32_MAX);
    assert(registers[USB_SECURITY_AHB_PORT9_SLAVE0_RULE] == UINT32_MAX);
    assert((registers[USB_SECURITY_MASTER_SEC_LEVEL] & USB_SECURITY_MASTER_DEFINED_MASK &
            ~USB_SECURITY_MASTER_LEVEL_MASK) ==
           (master_level_before & ~USB_SECURITY_MASTER_LEVEL_MASK));
    assert((registers[USB_SECURITY_MASTER_SEC_ANTI_POL] & USB_SECURITY_MASTER_DEFINED_MASK &
            ~USB_SECURITY_MASTER_LEVEL_MASK) ==
           (master_anti_pol_before & ~USB_SECURITY_MASTER_LEVEL_MASK));
    assert((registers[USB_SECURITY_MASTER_SEC_LEVEL] & ~USB_SECURITY_MASTER_DEFINED_MASK) == 0U);
    assert((registers[USB_SECURITY_MASTER_SEC_ANTI_POL] & ~USB_SECURITY_MASTER_DEFINED_MASK) == 0U);
}

static void test_shared_state_initialization(void) {
    static dap_shared_t state;
    uint8_t* bytes = (uint8_t*)&state;
    for (size_t i = 0U; i < sizeof(state); i++) {
        bytes[i] = 0xA5U;
    }

    static dap_stage_t stage;
    stage.aux.state = 0xA5U;
    static struct energy_integration energy;
    dap_shared_init(&state, &stage, &energy);
    assert(stage.aux.state == AUX_SLOT_FREE);

    size_t const pointer = offsetof(dap_shared_t, energy_integration);
    size_t const packet = offsetof(dap_shared_t, bulk_packet_size);
    for (size_t i = 0U; i < offsetof(dap_shared_t, request); i++) {
        if ((i != offsetof(dap_shared_t, stage_commands)) &&
            ((i < pointer) || (i >= pointer + sizeof(state.energy_integration))) &&
            ((i < packet) || (i >= packet + sizeof(state.bulk_packet_size)))) {
            assert(bytes[i] == 0U);
        }
    }
    assert(state.energy_integration == &energy);
    assert(state.bulk_packet_size == DAP_SHARED_PACKET_SIZE);
    for (size_t i = 0U; i < REQUEST_SLOTS; i++) {
        assert(dap_usb_slot_free(state.response_len, (uint8_t)i));
        assert(dap_usb_slot_free(state.hid_request_len, (uint8_t)i));
    }
    assert(state.response_head == 0U && state.response_tail == 0U);
    assert(state.transfer_abort == nullptr);
    assert(state.stage_commands == 1U);
}

/** The USB DMA buffers in the struct both images place at one address. */
static void test_shared_buffer_alignment(void) {
    assert(offsetof(dap_shared_t, request) % DAP_USB_BUFFER_ALIGN == 0U);
    assert(sizeof(((dap_shared_t*)0)->request[0]) % DAP_USB_BUFFER_ALIGN == 0U);
    assert(offsetof(dap_shared_t, response) % DAP_USB_BUFFER_ALIGN == 0U);
    assert(sizeof(((dap_shared_t*)0)->response[0]) % DAP_USB_BUFFER_ALIGN == 0U);
    assert(offsetof(dap_shared_t, hid_request) % DAP_USB_BUFFER_ALIGN == 0U);
}

static void test_cpu0_usb_reboot_request(void) {
    assert(reboot_usb_control_request_valid(REBOOT_USB_REQUEST_TYPE, REBOOT_USB_VENDOR_REQUEST, 0U, 0U, 0U));
    assert(reboot_usb_control_request_valid(REBOOT_USB_REQUEST_TYPE, REBOOT_USB_VENDOR_REQUEST, 1U, 0U, 0U));
    assert(!reboot_usb_control_request_valid(REBOOT_USB_REQUEST_TYPE, REBOOT_USB_VENDOR_REQUEST, 2U, 0U, 0U));
    assert(!reboot_usb_control_request_valid(0xC0U, REBOOT_USB_VENDOR_REQUEST, 1U, 0U, 0U));
    assert(!reboot_usb_control_request_valid(REBOOT_USB_REQUEST_TYPE, REBOOT_USB_VENDOR_REQUEST, 1U, 1U, 0U));
    assert(!reboot_usb_control_request_valid(REBOOT_USB_REQUEST_TYPE, REBOOT_USB_VENDOR_REQUEST, 1U, 0U, 1U));
    assert(!reboot_usb_control_request_valid(REBOOT_USB_REQUEST_TYPE, 2U, 1U, 0U, 0U));
}

static void test_hid_padding(void) {
    uint8_t response[DAP_SHARED_HID_REPORT_SIZE + 1U];
    for (uint32_t produced = 0U; produced <= DAP_SHARED_HID_REPORT_SIZE; produced++) {
        memset(response, 0xA5, sizeof(response));
        assert(dap_hid_reply(response, produced) == DAP_SHARED_HID_REPORT_SIZE);
        for (uint32_t i = 0U; i < produced; i++) { assert(response[i] == 0xA5U); }
        for (uint32_t i = produced; i < DAP_SHARED_HID_REPORT_SIZE; i++) { assert(response[i] == 0U); }
        assert(response[DAP_SHARED_HID_REPORT_SIZE] == 0xA5U);
    }
}

int main(void) {
    test_hid_padding();
    test_rx_completion_during_arm();
    test_tx_completion_during_start();
    test_cross_core_response_handoff();
    test_request_queue_and_transport_order();
    test_new_configuration_drops_the_last_hosts_work();
    test_mailbox_level_handoff();
    test_dap_core_security_attribution();
    test_shared_state_initialization();
    test_shared_buffer_alignment();
    test_cpu0_usb_reboot_request();
    return 0;
}
