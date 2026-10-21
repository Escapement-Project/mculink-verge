/** A ring DMA0 fills through reload descriptors linked in a circle, free of hardware so
 *  tools/swo_ring_test.c can run it on the host. SWO capture (src/swo_ring.h) and the consoles'
 *  receive rings (src/vcom.c) are both one.
 *
 * Each descriptor fills the next DMA_RING_SEGMENT_BYTES and raises INTA, and CPU 0's DMA interrupt
 * counts the completions. Byte n of the stream lands at ring[n % ring_bytes]. Positions are byte
 * counts since the ring started, modulo 2^32, so every comparison is a difference.
 */
#ifndef DMA_RING_H
#define DMA_RING_H

#include <stdbool.h>
#include <stdint.h>

/** A descriptor's maximum: XFERCOUNT is ten bits, minus one encoded. */
#define DMA_RING_SEGMENT_BYTES 1024U

/** XFERCFG's XFERCOUNT field. */
#define DMA_XFERCOUNT_SHIFT 16U
#define DMA_XFERCOUNT_MASK 0x3FFU

/** A DMA0 descriptor, as the controller reads it: a channel's entry in the table at SRAMBASE, or
 *  one a reload links to. */
typedef struct {
    uint32_t xfercfg;
    uint32_t source_end;
    uint32_t destination_end;
    uint32_t link;
} dma_descriptor_t;

/** Bytes written since the ring started.
 *
 * `segments` is how many descriptors CPU 0's interrupt has counted, `pending` whether one has
 * completed that it has not counted yet, and `xfercfg` the channel's XFERCFG. The three have to
 * come from one moment, which the reader ensures by reading them twice. The interrupt clears the
 * completion before it counts it, so a read between the two finds neither and comes out one
 * segment short, never long. A descriptor that has just run out reads as a full one still to go,
 * which comes to the same position as the next descriptor not yet started.
 */
static inline uint32_t dma_ring_written(uint32_t segments, bool pending, uint32_t xfercfg) {
    uint32_t const left = ((xfercfg >> DMA_XFERCOUNT_SHIFT) & DMA_XFERCOUNT_MASK) + 1U;
    return ((segments + (pending ? 1U : 0U)) * DMA_RING_SEGMENT_BYTES) + (DMA_RING_SEGMENT_BYTES - left);
}

/** How much of a ring a reader can trust. dma_ring_written can read one segment short, so a reader
 *  that trusted the whole ring could take bytes the DMA had already overwritten. */
static inline uint32_t dma_ring_usable(uint32_t ring_bytes) {
    return ring_bytes - DMA_RING_SEGMENT_BYTES;
}

/** Bytes between the reader and the writer, or none when the writer reads behind the reader
 *  (dma_ring_written being short). */
static inline uint32_t dma_ring_unread(uint32_t written, uint32_t read) {
    int32_t const ahead = (int32_t)(written - read);
    return (ahead > 0) ? (uint32_t)ahead : 0U;
}

/** Has the writer come close enough round the ring that bytes from `read` on may be gone?
 *
 * A writer more than a segment behind the reader is one that has gone 2^31 bytes or more past it,
 * which a reader nobody polls reaches in minutes at the fastest rates, and is an overrun too.
 * Without that a ring the host has left long enough would never deliver again. */
static inline bool dma_ring_overrun(uint32_t written, uint32_t read, uint32_t ring_bytes) {
    uint32_t const ahead = written - read;
    return (ahead > dma_ring_usable(ring_bytes)) && (ahead < (0U - DMA_RING_SEGMENT_BYTES));
}

/** Where a reader that the writer has passed starts again: the newest bytes, less two segments of
 *  room, one for `written` having read a segment short and one for the writer to move before the
 *  reader's next check. With less, the copy that follows can count as another overrun, and so can
 *  every one after it. So a ring needs three segments at least. */
static inline uint32_t dma_ring_resync(uint32_t written, uint32_t ring_bytes) {
    return written - (dma_ring_usable(ring_bytes) - (2U * DMA_RING_SEGMENT_BYTES));
}

#endif
