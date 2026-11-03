#include "dio_edges.h"

#include "fsl_clock.h"
#include "fsl_reset.h"

#include "board_straps.h"
#include "dap_shared.h"
#include "dio_pins.h"
#include "energy_stream.h"

/** The average rate of interrupt runs CPU 0 keeps up with. A run takes every edge since the last
 *  on every pin, so edges arriving together cost one. */
#define DIO_EDGES_RUNS_HZ 100000U
/** Runs that may come back to back before the average applies. */
#define DIO_EDGES_BURST 32U
/** How long the interrupts rest once the rate or the record ran out: 1 ms. */
#define DIO_EDGES_REST_PER_S 1000U

#define DIO_EDGES_IRQS (0xFUL << PIN_INT0_IRQn)

static struct energy_stream* edges_stream;
static bool present;
/** In counts of the stamping clock: what a run costs, the most credit kept, and a rest. */
static uint32_t run_cost;
static uint32_t burst;
static uint32_t rest_counts;
static uint32_t credit;
static uint32_t credit_stamp;
static volatile bool resting;
static volatile uint32_t rest_from;
/** Shared with the interrupt, which the loop masks to change them. Edges were dropped since the
 *  last run recorded; the next run records the levels even with no edge latched; the pins whose
 *  detectors the loop cleared since. The next run's entry says all three. */
static volatile bool dropped;
static volatile bool sync;
static volatile uint8_t cleared;
/** The pins whose interrupts the loop has turned on. */
static uint8_t enabled;
/** dio_spi_turns as the loop last acted on it. The interrupt reads it too. */
static volatile uint32_t spi_turns_seen;
static uint32_t begins_seen;

__attribute__((noinline)) void dio_edges_init(struct energy_stream* stream, uint8_t board, uint8_t jumpers) {
    if ((board != BOARD_KIND_PRO) || ((jumpers & BOARD_JUMPER_USBSIO_OFF) != 0U)) {
        return;
    }
    edges_stream = stream;
    // CTIMER4 stamps from FRO_HF (src/energy_monitor.c).
    uint32_t const clock_hz = CLOCK_GetFroHfFreq();
    run_cost = clock_hz / DIO_EDGES_RUNS_HZ;
    burst = run_cost * DIO_EDGES_BURST;
    rest_counts = clock_hz / DIO_EDGES_REST_PER_S;
    CLOCK_EnableClock(kCLOCK_InputMux);
    RESET_ReleasePeripheralReset(kMUX0_RST_SHIFT_RSTn);
    CLOCK_EnableClock(kCLOCK_Pint);
    RESET_PeripheralReset(kPINT_RST_SHIFT_RSTn);
    uint8_t const select[] = DIO_PINTSEL;
    for (uint32_t pin = 0U; pin < 4U; pin++) {
        INPUTMUX->PINTSEL[pin] = select[pin];
    }
    // Edge-sensitive, every enable off: the detectors latch whatever happens, and the loop clears
    // them as it turns a pin on.
    PINT->ISEL = 0U;
    for (uint32_t irq = PIN_INT0_IRQn; irq <= PIN_INT3_IRQn; irq++) {
        NVIC_SetPriority((IRQn_Type)irq, DIO_EDGES_PRIORITY);
    }
    NVIC->ICPR[0] = DIO_EDGES_IRQS;
    NVIC->ISER[0] = DIO_EDGES_IRQS;
    present = true;
}

/** The startup file's vector table names all four, weakly; one run serves any of them. */
void PIN_INT0_IRQHandler(void);
void PIN_INT1_IRQHandler(void) __attribute__((alias("PIN_INT0_IRQHandler")));
void PIN_INT2_IRQHandler(void) __attribute__((alias("PIN_INT0_IRQHandler")));
void PIN_INT3_IRQHandler(void) __attribute__((alias("PIN_INT0_IRQHandler")));

__attribute__((section(".ramfunc"))) void PIN_INT0_IRQHandler(void) {
    uint32_t const stamp = CTIMER4->TC;
    uint32_t const rise = PINT->RISE & DIO_PINS;
    uint32_t const fall = PINT->FALL & DIO_PINS;
    // Clear only what was read: an edge since stays latched for the next run.
    PINT->RISE = rise;
    PINT->FALL = fall;
    // This run took every pin's edges, so the other pins' requests are its too. One that is still
    // asserted comes straight back.
    NVIC->ICPR[0] = DIO_EDGES_IRQS;
    uint8_t const levels = dio_pins_levels(GPIO->PIN[0], GPIO->PIN[1]);
    bool const synced = sync;
    if (((rise | fall) == 0U) && !synced) {
        return;
    }
    uint32_t const elapsed = stamp - credit_stamp;
    credit_stamp = stamp;
    credit = (elapsed >= (burst - credit)) ? burst : (credit + elapsed);
    struct energy_stream* const stream = edges_stream;
    uint32_t const at = stream->edge_entries_written;
    if ((credit < run_cost) || ((at - stream->edge_entries_read) >= ENERGY_STREAM_EDGE_ENTRIES)) {
        // A sync asked for, and the pins cleared, wait for the run that records.
        NVIC->ICER[0] = DIO_EDGES_IRQS;
        rest_from = stamp;
        resting = true;
        dropped = true;
        return;
    }
    credit -= run_cost;
    // CPU 1 counts before SPI moves the pins (src/dio.c). A count read after the levels that is
    // still the even one the loop turned the pins on under says the levels were read first; any
    // other, and they may be SPI's. Such a run still pays its credit: with the pins turned on as
    // SPI took them, every SPI clock would run it.
    __DMB();
    uint32_t const turns = dap_shared.dio_spi_turns;
    if ((turns != spi_turns_seen) || ((turns & 1U) != 0U)) {
        dropped = true;
        return;
    }
    energy_stream_edge_entry_t* const entry = &stream->edge_entries[at & (ENERGY_STREAM_EDGE_ENTRIES - 1U)];
    entry->stamp = stamp;
    entry->rise = (uint8_t)rise;
    entry->fall = (uint8_t)fall;
    entry->levels = levels;
    entry->flags = (uint8_t)((dropped ? ENERGY_STREAM_ENTRY_LOST : 0U) | (synced ? ENERGY_STREAM_ENTRY_SYNC : 0U) |
                             ((uint32_t)cleared << ENERGY_STREAM_ENTRY_CLEARED_SHIFT));
    dropped = false;
    sync = false;
    cleared = 0U;
    __DMB();
    stream->edge_entries_written = at + 1U;
}

/** Whether CPU 0 owns the stream's state (src/aux.c, stream_owned_by_cpu1). */
__attribute__((section(".ramfunc"))) static bool owned(struct energy_stream const* stream) {
    return ((dap_shared.energy_control_ack & DAP_ENERGY_QUIET_FLAG) == 0U) && !stream->timeline_pending;
}

/** Clears the detectors of `pins` and asks the next run to record the levels, saying so. Masked,
 *  so that no run takes the request without the pins. */
__attribute__((section(".ramfunc"))) static void resync(uint8_t pins) {
    uint32_t const primask = __get_PRIMASK();
    __disable_irq();
    PINT->RISE = pins;
    PINT->FALL = pins;
    cleared |= pins;
    sync = true;
    __set_PRIMASK(primask);
    NVIC->ISPR[0] = 1UL << PIN_INT0_IRQn;
}

__attribute__((section(".ramfunc"))) static void set_dropped(bool value) {
    uint32_t const primask = __get_PRIMASK();
    __disable_irq();
    dropped = value;
    __set_PRIMASK(primask);
}

/** A new stream starts with no edges lost, unless the interrupt is resting: edges until the rest
 *  ends are lost to the new stream too. `resting` is read under the mask, so a rest that starts
 *  meanwhile keeps its mark. */
__attribute__((section(".ramfunc"))) static void begin_dropped(void) {
    uint32_t const primask = __get_PRIMASK();
    __disable_irq();
    dropped = resting;
    __set_PRIMASK(primask);
}

__attribute__((section(".ramfunc"), noinline)) void dio_edges_task(void) {
    if (!present) {
        return;
    }
    struct energy_stream* const stream = edges_stream;
    uint32_t const turns = dap_shared.dio_spi_turns;
    // A start counts after it writes the mask, so one counted has its mask.
    uint32_t const begins = stream->edge_begins;
    __DMB();
    // A new stream's edges wait until CPU 0 owns its state: until then nothing sends them, and a
    // fast pin would fill the record while CPU 1 finishes the start.
    bool const ready = owned(stream);
    bool const waiting = (begins != begins_seen) && !ready;
    bool const began = (begins != begins_seen) && ready;
    if (began) {
        begins_seen = begins;
    }
    uint8_t const reported = (stream->active && !waiting) ? stream->edge_mask : 0U;
    if (reported == 0U) {
        set_dropped(false);
    } else if (began) {
        begin_dropped();
    }
    if (turns != spi_turns_seen) {
        // CPU 1 turned the interrupts off for SPI (src/dio.c).
        spi_turns_seen = turns;
        enabled = 0U;
        if (reported != 0U) {
            set_dropped(true);
        }
    }
    uint8_t const wanted = ((turns & 1U) != 0U) ? 0U : reported;
    uint32_t const off = (uint32_t)enabled & ~(uint32_t)wanted;
    uint32_t const on = (uint32_t)wanted & ~(uint32_t)enabled;
    PINT->CIENR = off;
    PINT->CIENF = off;
    // A new stream's edges start at a run that records the levels with every detector cleared
    // after its header; a pin turned on starts the same way.
    uint8_t const fresh = began ? wanted : (uint8_t)on;
    if (fresh != 0U) {
        resync(fresh);
        PINT->SIENR = on;
        PINT->SIENF = on;
        // The enables land before the count is read again. Without a barrier nothing orders a store
        // to one peripheral before a load from another, USB SRAM included.
        __DMB();
        if (dap_shared.dio_spi_turns != turns) {
            // SPI took the pins meanwhile: CPU 1 counts before it turns them off, so they stay off
            // whichever of us wrote last. The run resync asked for records nothing if it read the
            // levels after the count (PIN_INT0_IRQHandler), and the sync goes with the pins. The
            // next pass sees the count.
            uint32_t const primask = __get_PRIMASK();
            __disable_irq();
            PINT->CIENR = on;
            PINT->CIENF = on;
            sync = false;
            __set_PRIMASK(primask);
            return;
        }
    }
    enabled = wanted;
    if (resting && ((CTIMER4->TC - rest_from) >= rest_counts)) {
        resting = false;
        NVIC->ICPR[0] = DIO_EDGES_IRQS;
        NVIC->ISER[0] = DIO_EDGES_IRQS;
        if ((enabled != 0U) && (dap_shared.dio_spi_turns == turns)) {
            resync(enabled);
        }
    }
    // Edges go with the next pairs message, but no later than 10 ms after the first of them, nor
    // once they half fill the record. Sending them sends the pairs message being built first.
    uint32_t const read = stream->edge_entries_read;
    uint32_t const written = stream->edge_entries_written;
    if ((reported == 0U) || (written == read) || !owned(stream)) {
        return;
    }
    uint32_t const oldest = stream->edge_entries[read & (ENERGY_STREAM_EDGE_ENTRIES - 1U)].stamp;
    if (((written - read) < (ENERGY_STREAM_EDGE_ENTRIES / 2U)) && ((CTIMER4->TC - oldest) < (stream->clock_hz / 100U))) {
        return;
    }
    uint32_t const basepri = cpu0_mask_all_but_edges();
    if (stream->active) {
        energy_stream_close(stream);
    }
    cpu0_unmask(basepri);
}

/** Only edges CPU 0 can send are work: CPU 1 sends them while it owns the state. */
__attribute__((section(".ramfunc"), noinline)) bool dio_edges_waiting(void) {
    return present && owned(edges_stream) &&
           ((edges_stream->edge_entries_written - edges_stream->edge_entries_read) >= (ENERGY_STREAM_EDGE_ENTRIES / 2U));
}

__attribute__((section(".ramfunc"), noinline)) uint8_t dio_edges_pins(uint8_t requested, uint8_t* levels,
                                                                       uint32_t* stamp) {
    // Again if SPI counted meanwhile: CPU 1 counts before it moves the pins and after it gives
    // them back (src/dio.c), so levels read between two reads of one even count are the pins' own.
    for (;;) {
        uint32_t const turns = dap_shared.dio_spi_turns;
        __DMB();
        uint8_t const pins = (present && ((turns & 1U) == 0U)) ? (uint8_t)(requested & DIO_PINS) : 0U;
        *levels = (uint8_t)(dio_pins_levels(GPIO->PIN[0], GPIO->PIN[1]) & pins);
        *stamp = CTIMER4->TC;
        __DMB();
        if (dap_shared.dio_spi_turns == turns) {
            return pins;
        }
    }
}
