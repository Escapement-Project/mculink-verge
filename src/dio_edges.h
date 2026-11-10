/** DIO edges for Verge Aux's energy stream (docs/host-interface.md, "DIO edges: 0xa3"), on CPU 0.
 *
 * Each DIO pin has a pin interrupt of its own, at CPU 0's only priority above the rest. The
 * interrupt stamps itself with the stream's clock, takes which pins rose and fell since it last ran
 * and the levels after, and records that for the stream's owner to send (src/energy_stream.h).
 * Every edge costs CPU 0 an interrupt, so past an average rate the interrupts rest for a while and
 * the stream marks the edges lost.
 *
 * The pin interrupts see the pins whatever their function: an input, an output the host drives,
 * or SPI's, which a stream does not report (src/dio.c turns the interrupts off around SPI).
 */
#ifndef DIO_EDGES_H
#define DIO_EDGES_H

#include <stdbool.h>
#include <stdint.h>

#include "fsl_device_registers.h"

struct energy_stream;

/** The pin interrupts' priority, and everything else's on CPU 0. Nothing else may share the pin
 *  interrupts' level: an edge waits for whatever runs at or above it. */
#define DIO_EDGES_PRIORITY 0U
#define CPU0_PRIORITY 1U

/** Masks every interrupt on CPU 0 but the pin interrupts, which touch nothing else, and returns
 *  what to restore. */
static inline __attribute__((always_inline)) uint32_t cpu0_mask_all_but_edges(void) {
    uint32_t const basepri = __get_BASEPRI();
    __set_BASEPRI(CPU0_PRIORITY << (8U - __NVIC_PRIO_BITS));
    return basepri;
}

static inline __attribute__((always_inline)) void cpu0_unmask(uint32_t basepri) {
    __set_BASEPRI(basepri);
}

/** On a Pro with the DIO pins on, before CPU 1 starts: the pin interrupts, off until a stream asks
 *  for its pins. `stream` is the one they record for. */
void dio_edges_init(struct energy_stream* stream, uint8_t board, uint8_t jumpers);

/** CPU 0's loop: turns the pin interrupts on and off to match the stream and SPI, ends a rest,
 *  and sends edges that have waited 10 ms or half fill the record while CPU 0 owns the stream. */
void dio_edges_task(void);

/** Whether the record is half full and CPU 0 owns the stream, so the loop should not wait. */
bool dio_edges_waiting(void);

/** The pins of `requested` a stream can report and, in `levels`, their levels now, then the
 *  stamping timer: mode 6's header, on CPU 0. */
uint8_t dio_edges_pins(uint8_t requested, uint8_t* levels, uint32_t* stamp);

#endif
