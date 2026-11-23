/** Vendor 0x88: run host-supplied code from RAM and time it.
 *
 * A bench instrument for the emulator's cost model, built only with the CODE_LAB option. The host
 * assembles a Thumb function, writes it into an arena, and calls it with interrupts masked. The
 * function reads DWT->CYCCNT itself around the code under test, so nothing of the call reaches
 * the measurement, and returns what it measured in r0 and r1.
 *
 * Two arenas, because where code runs from is one of the things being measured:
 *
 * - **SRAM**, a buffer in .bss at 0x2000xxxx. The core fetches it over S-AHB, the same port the
 *   GPIO stores go out on, which is where the transfer loops in .ramfunc run.
 * - **SRAMX**, the upper 16 KB of the 32 KB at 0x04000000. The core fetches it over C-AHB, so
 *   instruction fetch and GPIO traffic do not share a port. The lower half holds the generated
 *   code, which a lab build keeps to it (src/jtag_gen.h).
 *
 * Request, after the id:
 *
 *     [0]  op     LAB_INFO, LAB_WRITE, LAB_READ or LAB_RUN
 *
 *     LAB_INFO   reply: status, arena count, then base and size of each, u32 little-endian
 *     LAB_WRITE  [1] arena, [2..3] offset, [4..5] length, [6..] bytes; reply: status
 *     LAB_READ   [1] arena, [2..3] offset, [4..5] length; reply: status, bytes
 *     LAB_RUN    [1] arena, [2..3] entry offset, [4..19] r0..r3; reply: status, r0, r1
 *     LAB_EDGES  [1..4] half period in counts of PLL0, 0 to stop; reply: status
 *
 * LAB_EDGES toggles DIO2 (PIO1_2) from CTimer 0's match 3, so the DIO edges an energy stream
 * reports come at intervals known to the count, with no analyser. The pin's translator has to face
 * the header first (`verge dio --low 0,1,2`), and nothing on J19 may drive DIO2.
 *
 * Every offset and length is checked against the arena and the packet. A function that faults
 * resets the probe, which comes back on USB without hands; one that never returns hangs it with
 * interrupts off, and that is the host's to avoid.
 */
#include <stdbool.h>
#include <string.h>

#include "DAP_config.h"

#include "DAP.h"
#include "byte_io.h"
#include "code_lab.h"
#include "fsl_clock.h"
#include "fsl_iocon.h"
#include "fsl_reset.h"

#define LAB_INFO 0U
#define LAB_WRITE 1U
#define LAB_READ 2U
#define LAB_RUN 3U
#define LAB_EDGES 4U

#define LAB_SRAM_SIZE 4096U
#define LAB_SRAMX_BASE 0x04004000U
#define LAB_SRAMX_SIZE 0x4000U

#define LAB_WRITE_HEADER 7U // id, op, arena, offset, length
#define LAB_READ_HEADER 2U  // id, status

alignas(32) static uint8_t lab_sram[LAB_SRAM_SIZE];

struct arena {
    uint8_t* base;
    uint32_t size;
};

static bool arena_get(uint8_t index, struct arena* out) {
    if (index == 0U) {
        *out = (struct arena){lab_sram, LAB_SRAM_SIZE};
        return true;
    }
    if (index == 1U) {
        *out = (struct arena){(uint8_t*)LAB_SRAMX_BASE, LAB_SRAMX_SIZE};
        return true;
    }
    return false;
}

/** A lab build resets on a fault rather than spinning in the startup code's handler. A function
 * the host got wrong is the likely cause, and a reset is what gives the probe back. Naked, because
 * that function may have left the stack pointer anywhere: SYSRESETREQ through AIRCR directly. */
void HardFault_Handler(void);
__attribute__((naked)) void HardFault_Handler(void) {
    __asm volatile("ldr r0, =0xE000ED0C\n\t"
                   "ldr r1, =0x05FA0004\n\t"
                   "str r1, [r0]\n\t"
                   "dsb\n\t"
                   "1: b 1b\n\t"
                   ".ltorg\n\t");
}

typedef uint64_t (*lab_fn)(uint32_t, uint32_t, uint32_t, uint32_t);

/** DIO2 toggling every `half` counts of PLL0, the clock the stream stamps with; 0 gives the pin
 *  back to GPIO. */
static void lab_edges(uint32_t half) {
    CTIMER0->TCR = 0U;
    if (half == 0U) {
        IOCON_PinMuxSet(IOCON, 1U, 2U, IOCON_FUNC0 | IOCON_MODE_INACT | IOCON_DIGITAL_EN);
        return;
    }
    CLOCK_AttachClk(kPLL0_to_CTIMER0);
    CLOCK_EnableClock(kCLOCK_Timer0);
    RESET_PeripheralReset(kCTIMER0_RST_SHIFT_RSTn);
    CTIMER0->PR = 0U;
    CTIMER0->MR[3] = half - 1U;
    CTIMER0->MCR = CTIMER_MCR_MR3R_MASK;
    CTIMER0->EMR = CTIMER_EMR_EMC3(3U);
    IOCON_PinMuxSet(IOCON, 1U, 2U, IOCON_FUNC3 | IOCON_MODE_INACT | IOCON_DIGITAL_EN);
    CTIMER0->TCR = CTIMER_TCR_CEN_MASK;
}

uint32_t code_lab_command(const uint8_t* request, uint8_t* response) {
    const uint32_t op = request[1];
    struct arena a;
    response[1] = DAP_ERROR;

    if (op == LAB_INFO) {
        response[1] = DAP_OK;
        response[2] = 2U;
        uint8_t* out = response + 3;
        for (uint8_t i = 0U; i < 2U; i++) {
            (void)arena_get(i, &a);
            put_u32_le(out, (uint32_t)a.base);
            put_u32_le(out + 4, a.size);
            out += 8;
        }
        return (uint32_t)(out - response);
    }

    if (op == LAB_WRITE) {
        const uint32_t offset = read_u16_le(request + 3);
        const uint32_t length = read_u16_le(request + 5);
        if (length > DAP_PACKET_SIZE - LAB_WRITE_HEADER || !arena_get(request[2], &a) || offset > a.size ||
            length > a.size - offset) {
            return 2U;
        }
        memcpy(a.base + offset, request + LAB_WRITE_HEADER, length);
        response[1] = DAP_OK;
        return 2U;
    }

    if (op == LAB_READ) {
        const uint32_t offset = read_u16_le(request + 3);
        const uint32_t length = read_u16_le(request + 5);
        if (length > DAP_PACKET_SIZE - LAB_READ_HEADER || !arena_get(request[2], &a) || offset > a.size ||
            length > a.size - offset) {
            return 2U;
        }
        memcpy(response + LAB_READ_HEADER, a.base + offset, length);
        response[1] = DAP_OK;
        return LAB_READ_HEADER + length;
    }

    if (op == LAB_RUN) {
        const uint32_t entry = read_u16_le(request + 3);
        if (!arena_get(request[2], &a) || entry >= a.size || (entry & 1U) != 0U) {
            return 2U;
        }
        const lab_fn fn = (lab_fn)((uint32_t)(a.base + entry) | 1U);
        const uint32_t primask = __get_PRIMASK();
        __disable_irq();
        // The arena was written as data; the barriers make the core fetch what is there now.
        __DSB();
        __ISB();
        const uint64_t result = fn(read_u32_le(request + 5), read_u32_le(request + 9), read_u32_le(request + 13),
                                   read_u32_le(request + 17));
        __set_PRIMASK(primask);
        response[1] = DAP_OK;
        put_u32_le(response + 2, (uint32_t)result);
        put_u32_le(response + 6, (uint32_t)(result >> 32));
        return 10U;
    }

    if (op == LAB_EDGES) {
        lab_edges(read_u32_le(request + 2));
        response[1] = DAP_OK;
        return 2U;
    }

    return 2U;
}
