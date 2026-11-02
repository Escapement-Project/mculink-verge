#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "energy_stream.h"

#define CLOCK_HZ 96000000U
/** 1,000 pairs a second: a trigger every 96,000 counts, the first 48,000 after the timer starts. */
#define TICKS 48000U
#define PERIOD (2U * TICKS)

static uint16_t u16(uint8_t const* at) {
    return (uint16_t)(at[0] | (at[1] << 8U));
}

static uint32_t u32(uint8_t const* at) {
    return (uint32_t)at[0] | ((uint32_t)at[1] << 8U) | ((uint32_t)at[2] << 16U) | ((uint32_t)at[3] << 24U);
}

static uint64_t u64(uint8_t const* at) {
    return (uint64_t)u32(at) | ((uint64_t)u32(at + 4) << 32U);
}

static uint64_t ns(uint64_t counts) {
    return (uint64_t)(((unsigned __int128)counts * 1000000000U) / CLOCK_HZ);
}

/** The next published message, taken as CPU 0's loop takes it; NULL when there is none. */
static uint8_t const* take(struct energy_stream* stream, uint16_t* length) {
    if (stream->sent == stream->written) {
        return NULL;
    }
    uint32_t const at = stream->sent & (ENERGY_STREAM_SLOTS - 1U);
    *length = stream->length[at];
    stream->sent++;
    return energy_stream_slots[at];
}

static energy_calibration_t const low = {.code = {100U, 30000U}, .current_na = {5000U, 900000U}, .count = 2U};
static energy_calibration_t const high = {
    .code = {200U, 20000U, 40000U, 60000U}, .current_na = {1000000U, 50000000U, 100000000U, 150000000U}, .count = 4U};

static void reset_stream(struct energy_stream* stream) {
    memset(stream, 0, sizeof(*stream));
}

static void start(struct energy_stream* stream, uint32_t started_raw, bool streaming) {
    reset_stream(stream);
    energy_stream_timeline_prepare(stream, 1U);
    if (streaming) {
        energy_stream_header_t const header = {
            .rate_hz = CLOCK_HZ / PERIOD, .setting = 1U, .full_scale_mv = 3000U, .first_sequence = 1U,
            .previous_low = true, .low = &low, .high = &high, .dio_mask = 0U, .dio_levels = 5U, .edge_resolution_ns = 0U};
        assert(energy_stream_begin(stream, &header));
    }
    energy_stream_timeline_run(stream, true, CLOCK_HZ, TICKS, started_raw);
}

static void test_header(void) {
    static struct energy_stream stream;
    start(&stream, 0U, true);
    uint16_t length = 0U;
    uint8_t const* m = take(&stream, &length);
    assert(m != NULL && length == ENERGY_STREAM_HEADER_BYTES);
    assert(m[0] == 0xA1U && m[1] == 1U && u32(m + 2) == 1000U && m[6] == 1U && u16(m + 7) == 3000U);
    assert(u32(m + 9) == 1U && m[13] == 1U);
    assert(m[14] == 2U && u16(m + 15) == 100U && u32(m + 17) == 5000U && u16(m + 21) == 30000U);
    assert(u32(m + 23) == 900000U && u16(m + 27) == 0U && u32(m + 29) == 0U && u16(m + 33) == 0U && u32(m + 35) == 0U);
    assert(m[39] == 4U && u16(m + 40) == 200U && u32(m + 42) == 1000000U && u16(m + 58) == 60000U);
    assert(u32(m + 60) == 150000000U && m[64] == 0U && m[65] == 5U && u16(m + 66) == 0U);
    assert(take(&stream, &length) == NULL);
}

static void test_identified_header(void) {
    static struct energy_stream stream;
    reset_stream(&stream);
    energy_capture_id_t id = {.bytes = {[16] = 1U}};
    assert(energy_identity_matches(&id, id.bytes));
    for (uint32_t i = 0U; i < sizeof(id.bytes); i++) {
        energy_capture_id_t stale = id;
        stale.bytes[i] ^= 1U;
        assert(!energy_identity_matches(&id, stale.bytes));
    }
    energy_capture_id_t const zero = {0};
    assert(!energy_identity_matches(&zero, zero.bytes));
    energy_stream_header_t const header = {
        .rate_hz = 1000U, .setting = 1U, .full_scale_mv = 3000U, .first_sequence = 1U,
        .previous_low = true, .low = &low, .high = &high, .capture_id = &id};
    assert(energy_stream_begin(&stream, &header));
    uint16_t length = 0U;
    uint8_t const* message = take(&stream, &length);
    assert(message != NULL && length == 92U && message[1] == 2U);
    assert(memcmp(message + 68U, id.bytes, sizeof(id.bytes)) == 0);
}

/** 99 pairs a message at most, evenly spaced, each message timed from the capture's start; the
 *  32-bit stamps wrap partway. */
static void test_pairs(void) {
    static struct energy_stream stream;
    start(&stream, 0xFFFF0000U, true);
    uint16_t length = 0U;
    (void)take(&stream, &length);
    // At 100,000 pairs a second nothing reaches 10 ms before 99 pairs: a trigger every 960 counts.
    stream.period = 960U;
    for (uint32_t s = 1U; s <= 250U; s++) {
        energy_stream_pair(&stream, s, (uint16_t)s, (uint16_t)(s * 3U), (s % 2U) == 0U);
    }
    energy_stream_batch_end(&stream, stream.next_sequence - 1U);
    uint32_t first = 1U;
    for (uint32_t message = 0U; message < 2U; message++) {
        uint8_t const* m = take(&stream, &length);
        assert(m != NULL && m[0] == 0xA0U && m[1] == 0U && length == 16U + 99U * 5U);
        assert(u32(m + 2) == first && u16(m + 14) == 99U);
        assert(u64(m + 6) == ns(TICKS + (uint64_t)(first - 1U) * 960U));
        for (uint32_t i = 0U; i < 99U; i++) {
            uint8_t const* pair = m + 16U + i * 5U;
            uint32_t const s = first + i;
            assert(u16(pair) == (uint16_t)s && u16(pair + 2) == (uint16_t)(s * 3U) && pair[4] == ((s % 2U) == 0U));
        }
        first += 99U;
    }
    // 52 pairs over 50 us: the batch's end does not send them, 10 ms has not passed.
    assert(take(&stream, &length) == NULL && stream.open_pairs == 52U);
    energy_stream_end(&stream, 2U, 250U);
    uint8_t const* m = take(&stream, &length);
    assert(m != NULL && m[0] == 0xA0U && u16(m + 14) == 52U && length == 16U + 52U * 5U);
    m = take(&stream, &length);
    assert(m != NULL && length == 6U && m[0] == 0xA2U && m[1] == 2U && u32(m + 2) == 250U);
    assert(!stream.active);
    energy_stream_end(&stream, 2U, 250U);
    assert(take(&stream, &length) == NULL);
}

/** At 1,000 pairs a second a message holds the pairs of less than 10 ms: ten. */
static void test_ten_ms(void) {
    static struct energy_stream stream;
    start(&stream, 0U, true);
    uint16_t length = 0U;
    (void)take(&stream, &length);
    for (uint32_t s = 1U; s <= 7U; s++) {
        energy_stream_pair(&stream, s, 1U, 1U, true);
    }
    energy_stream_batch_end(&stream, stream.next_sequence - 1U);
    assert(take(&stream, &length) == NULL);
    for (uint32_t s = 8U; s <= 14U; s++) {
        energy_stream_pair(&stream, s, 1U, 1U, true);
    }
    uint8_t const* m = take(&stream, &length);
    assert(m != NULL && u32(m + 2) == 1U && u16(m + 14) == 10U);
    energy_stream_batch_end(&stream, stream.next_sequence - 1U);
    assert(take(&stream, &length) == NULL && stream.open_pairs == 4U);
    for (uint32_t s = 15U; s <= 20U; s++) {
        energy_stream_pair(&stream, s, 1U, 1U, true);
    }
    energy_stream_batch_end(&stream, stream.next_sequence - 1U);
    m = take(&stream, &length);
    assert(m != NULL && u32(m + 2) == 11U && u16(m + 14) == 10U && u64(m + 6) == ns(TICKS + 10U * (uint64_t)PERIOD));
}

/** A hold moves the first trigger due after the stop, and every later one, to when the restarted
 *  timer brought it, and ends the message there. The stop's stamp lags it by a few counts, up to
 *  half a period here; stamps wrap past the hold. */
static void test_holds(void) {
    uint32_t const lags[] = {0U, 6U, PERIOD / 2U - 1U};
    for (uint32_t i = 0U; i < sizeof(lags) / sizeof(lags[0]); i++) {
        static struct energy_stream stream;
        uint32_t const started = 0xFFFFFFFFU - 3U * PERIOD;
        start(&stream, started, true);
        uint16_t length = 0U;
        (void)take(&stream, &length);
        energy_stream_batch_begin(&stream, stream.next_sequence - 1U);
        for (uint32_t s = 1U; s <= 3U; s++) {
            energy_stream_pair(&stream, s, 1U, 1U, true);
        }
        // Pair 4 was due at started + TICKS + 3 x PERIOD, and came 12,345 counts later.
        uint32_t const due = started + TICKS + 3U * PERIOD;
        energy_stream_hold_t* const hold = energy_stream_hold(&stream, due + lags[i]);
        assert(hold != NULL);
        hold->came = due + 12345U;
        energy_stream_batch_begin(&stream, stream.next_sequence - 1U);
        for (uint32_t s = 4U; s <= 5U; s++) {
            energy_stream_pair(&stream, s, 1U, 1U, true);
        }
        energy_stream_end(&stream, 1U, 5U);
        uint8_t const* m = take(&stream, &length);
        assert(m != NULL && u32(m + 2) == 1U && u16(m + 14) == 3U && m[1] == 0U);
        m = take(&stream, &length);
        assert(m != NULL && u32(m + 2) == 4U && u16(m + 14) == 2U && m[1] == ENERGY_STREAM_FLAG_HELD);
        assert(u64(m + 6) == ns(TICKS + 3U * (uint64_t)PERIOD + 12345U));
        assert(stream.holds_read == stream.holds_written);
    }
    // A hold recorded after its batch began waits for the next batch; one due after the next pair
    // waits for that pair.
    static struct energy_stream stream;
    start(&stream, 0U, false);
    energy_stream_batch_begin(&stream, stream.next_sequence - 1U);
    (void)energy_stream_time(&stream, 1U);
    energy_stream_hold_t* const hold = energy_stream_hold(&stream, TICKS + 2U * PERIOD);
    hold->came = TICKS + 2U * PERIOD + 500U;
    assert(energy_stream_time(&stream, 2U) == TICKS + PERIOD && stream.holds_read == 0U);
    energy_stream_batch_begin(&stream, stream.next_sequence - 1U);
    assert(energy_stream_time(&stream, 3U) == TICKS + 2U * PERIOD + 500U && stream.holds_read == 1U);
    // More holds than the ring keeps: the rest are not recorded, and nothing is overwritten.
    for (uint32_t i = 0U; i < ENERGY_STREAM_HOLDS + 3U; i++) {
        energy_stream_hold_t* const more = energy_stream_hold(&stream, TICKS + 5U * PERIOD + i);
        assert((more == NULL) == (i >= ENERGY_STREAM_HOLDS));
    }
    assert(stream.holds_written - stream.holds_read == ENERGY_STREAM_HOLDS);
}

/** A sequence out of step marks a gap and carries on from it; a start's timeline that never comes
 *  is waited for a bounded time, not forever. */
static void test_out_of_step(void) {
    static struct energy_stream stream;
    start(&stream, 0U, true);
    uint16_t length = 0U;
    (void)take(&stream, &length);
    energy_stream_pair(&stream, 1U, 1U, 1U, true);
    energy_stream_pair(&stream, 5U, 1U, 1U, true);
    energy_stream_pair(&stream, 6U, 1U, 1U, true);
    energy_stream_end(&stream, 2U, 6U);
    uint8_t const* m = take(&stream, &length);
    assert(m != NULL && u32(m + 2) == 1U && u16(m + 14) == 1U);
    m = take(&stream, &length);
    assert(m != NULL && u32(m + 2) == 5U && u16(m + 14) == 2U && m[1] == ENERGY_STREAM_FLAG_HELD);
    stream.timeline_pending = true;
    energy_stream_batch_begin(&stream, stream.next_sequence - 1U);
}

/** A pause sends the pairs so far; the resume continues the clock by however long the pause took,
 *  earlier or later than the trigger it interrupted would have come. */
static void test_resume(void) {
    for (uint32_t early = 0U; early < 2U; early++) {
        static struct energy_stream stream;
        start(&stream, 1000U, true);
        uint16_t length = 0U;
        (void)take(&stream, &length);
        for (uint32_t s = 1U; s <= 3U; s++) {
            energy_stream_pair(&stream, s, 1U, 1U, true);
        }
        energy_stream_pause(&stream);
        uint8_t const* m = take(&stream, &length);
        assert(m != NULL && u16(m + 14) == 3U);
        // Pair 4 would have come at 1000 + TICKS + 3 x PERIOD; the resume's first trigger comes 7,000
        // counts before or after.
        uint32_t const would = 1000U + TICKS + 3U * PERIOD;
        uint32_t const first = early ? would - 7000U : would + 7000U;
        energy_stream_timeline_prepare(&stream, 4U);
        energy_stream_timeline_run(&stream, false, CLOCK_HZ, TICKS, first - TICKS);
        energy_stream_pair(&stream, 4U, 1U, 1U, true);
        energy_stream_end(&stream, 2U, 4U);
        m = take(&stream, &length);
        assert(m != NULL && m[1] == ENERGY_STREAM_FLAG_PAUSED && u32(m + 2) == 4U);
        uint64_t const expected = TICKS + 3U * (uint64_t)PERIOD;
        assert(u64(m + 6) == ns(early ? expected - 7000U : expected + 7000U));
    }
}

/** A ring the host does not read: pairs messages stop with room left for the end and a header. */
static void test_full(void) {
    static struct energy_stream stream;
    start(&stream, 0U, true);
    uint16_t length = 0U;
    (void)take(&stream, &length);
    stream.period = 960U;
    for (uint32_t s = 1U; s <= 99U * 20U; s++) {
        energy_stream_pair(&stream, s, 1U, 1U, true);
    }
    assert(energy_stream_free(&stream) == ENERGY_STREAM_CONTROL_SLOTS);
    energy_stream_end(&stream, 3U, 99U * 20U);
    assert(energy_stream_free(&stream) == ENERGY_STREAM_CONTROL_SLOTS - 1U);
    uint32_t pairs_messages = 0U;
    uint8_t const* m = NULL;
    uint8_t const* last = NULL;
    while ((m = take(&stream, &length)) != NULL) {
        pairs_messages += m[0] == 0xA0U;
        last = m;
    }
    assert(pairs_messages == ENERGY_STREAM_SLOTS - ENERGY_STREAM_CONTROL_SLOTS);
    assert(last != NULL && last[0] == 0xA2U && last[1] == 3U);

    // A pause while the ring is full: the pairs after it are dropped, and the first message that
    // leaves once there is room still says the capture paused.
    start(&stream, 0U, true);
    (void)take(&stream, &length);
    stream.period = 960U;
    uint32_t s = 1U;
    for (; s <= 99U * 14U; s++) {
        energy_stream_pair(&stream, s, 1U, 1U, true);
    }
    assert(energy_stream_free(&stream) == ENERGY_STREAM_CONTROL_SLOTS);
    energy_stream_pause(&stream);
    for (uint32_t i = 0U; i < 10U; i++, s++) {
        energy_stream_pair(&stream, s, 1U, 1U, true);
    }
    while (take(&stream, &length) != NULL) {
    }
    energy_stream_pair(&stream, s, 1U, 1U, true);
    energy_stream_end(&stream, 2U, s);
    uint8_t const* const first = take(&stream, &length);
    assert(first != NULL && first[0] == 0xA0U && first[1] == ENERGY_STREAM_FLAG_PAUSED && u32(first + 2) == s);
}

/** A ring the host stops reading after a hold: one dropped message stays open rather than closing
 *  at every pair, and the first pair once there is room starts a message that leaves, flagged. */
static void test_full_after_hold(void) {
    static struct energy_stream stream;
    start(&stream, 0U, true);
    uint16_t length = 0U;
    (void)take(&stream, &length);
    stream.period = 960U;
    uint32_t s = 1U;
    for (; s <= 99U * 14U; s++) {
        energy_stream_pair(&stream, s, 1U, 1U, true);
    }
    assert(energy_stream_free(&stream) == ENERGY_STREAM_CONTROL_SLOTS && stream.open_pairs == 0U);
    // Pair s comes out of step, as after a hold: the gap waits for a message that leaves.
    s++;
    for (uint32_t i = 0U; i < 50U; i++, s++) {
        energy_stream_pair(&stream, s, 1U, 1U, true);
    }
    assert(stream.open_dropped && stream.open_pairs == 50U && stream.gap == ENERGY_STREAM_FLAG_HELD);
    while (take(&stream, &length) != NULL) {
    }
    energy_stream_pair(&stream, s, 1U, 1U, true);
    energy_stream_end(&stream, 2U, s);
    uint8_t const* const first = take(&stream, &length);
    assert(first != NULL && first[0] == 0xA0U && first[1] == ENERGY_STREAM_FLAG_HELD && u32(first + 2) == s &&
           u16(first + 14) == 1U);
}

/** Without a gap too, a dropped message ends at the first pair that finds room. */
static void test_full_recovers(void) {
    static struct energy_stream stream;
    start(&stream, 0U, true);
    uint16_t length = 0U;
    (void)take(&stream, &length);
    stream.period = 960U;
    uint32_t s = 1U;
    for (; s <= 99U * 14U + 20U; s++) {
        energy_stream_pair(&stream, s, 1U, 1U, true);
    }
    assert(stream.open_dropped && stream.open_pairs == 20U);
    while (take(&stream, &length) != NULL) {
    }
    energy_stream_pair(&stream, s, 1U, 1U, true);
    energy_stream_end(&stream, 2U, s);
    uint8_t const* const first = take(&stream, &length);
    assert(first != NULL && first[0] == 0xA0U && first[1] == 0U && u32(first + 2) == s && u16(first + 14) == 1U);
}

/** Without a stream, a batch with no hold to place goes untimed and moves the timeline on at its
 *  end; one with a hold times its pairs. Either way the timeline ends where timing every pair puts
 *  it, so a stream started after them times its first pair right. */
static void test_untimed(void) {
    static struct energy_stream stream;
    static struct energy_stream reference;
    start(&stream, 0U, false);
    start(&reference, 0U, false);
    uint32_t s = 0U;
    for (uint32_t batch = 0U; batch < 7U; batch++) {
        if (batch == 6U) {
            // Out of step, as after pairs the decoder dropped: both ways end at the same place.
            stream.next_sequence += 5U;
            reference.next_sequence += 5U;
        }
        if (batch == 2U) {
            // Pair 2 x 7 + 1 was due at TICKS + 14 x PERIOD and came 777 counts late.
            energy_stream_hold_t* const hold = energy_stream_hold(&stream, TICKS + 14U * PERIOD);
            hold->came = TICKS + 14U * PERIOD + 777U;
            energy_stream_hold_t* const twin = energy_stream_hold(&reference, TICKS + 14U * PERIOD);
            twin->came = hold->came;
        }
        energy_stream_batch_begin(&stream, s);
        energy_stream_batch_begin(&reference, s);
        // Batch 2 has a hold to place; batch 5's came after batch 4 began, so 4 stays untimed.
        assert(stream.untimed == (batch != 2U && batch != 5U));
        reference.untimed = false;
        if (batch == 4U) {
            energy_stream_hold_t* const late = energy_stream_hold(&stream, TICKS + 35U * PERIOD + 777U);
            late->came = TICKS + 35U * PERIOD + 777U + 999U;
            energy_stream_hold_t* const twin = energy_stream_hold(&reference, TICKS + 35U * PERIOD + 777U);
            twin->came = late->came;
        }
        for (uint32_t i = 0U; i < 7U; i++) {
            s++;
            energy_stream_pair(&stream, s, 1U, 1U, true);
            energy_stream_pair(&reference, s, 1U, 1U, true);
        }
        if (batch == 0U) {
            assert(stream.next_sequence == 1U && reference.next_sequence == 8U);
        }
        energy_stream_batch_end(&stream, s);
        energy_stream_batch_end(&reference, s);
        assert(!stream.untimed && stream.next_sequence == reference.next_sequence &&
               stream.next_raw == reference.next_raw && stream.next_counts == reference.next_counts);
    }
    assert(stream.next_counts == TICKS + 49U * (uint64_t)PERIOD + 777U + 999U);
    // A stream started now times its first pair from there.
    energy_stream_header_t const header = {
        .rate_hz = CLOCK_HZ / PERIOD, .setting = 1U, .full_scale_mv = 3000U, .first_sequence = s + 1U,
        .previous_low = true, .low = &low, .high = &high, .dio_mask = 0U, .dio_levels = 0U, .edge_resolution_ns = 0U};
    assert(energy_stream_begin(&stream, &header));
    uint16_t length = 0U;
    (void)take(&stream, &length);
    energy_stream_batch_begin(&stream, s);
    assert(!stream.untimed);
    energy_stream_pair(&stream, s + 1U, 1U, 1U, true);
    energy_stream_batch_end(&stream, s + 1U);
    energy_stream_end(&stream, 2U, s + 1U);
    uint8_t const* const m = take(&stream, &length);
    assert(m != NULL && m[0] == 0xA0U && u32(m + 2) == s + 1U &&
           u64(m + 6) == ns(TICKS + 49U * (uint64_t)PERIOD + 777U + 999U));
}

/** Without a stream, a batch goes untimed, nothing is sent, and its end moves the timeline on. */
static void test_idle(void) {
    static struct energy_stream stream;
    start(&stream, 0U, false);
    energy_stream_batch_begin(&stream, 0U);
    assert(stream.untimed);
    for (uint32_t s = 1U; s <= 300U; s++) {
        energy_stream_pair(&stream, s, 1U, 1U, true);
    }
    energy_stream_batch_end(&stream, 300U);
    assert(stream.written == 0U && stream.next_sequence == 301U);
    assert(stream.next_counts == TICKS + 300U * (uint64_t)PERIOD);
}

#define LOST ENERGY_STREAM_ENTRY_LOST
#define SYNC ENERGY_STREAM_ENTRY_SYNC
#define CLEARED(pins) ((uint8_t)((pins) << ENERGY_STREAM_ENTRY_CLEARED_SHIFT))

/** An entry as CPU 0's pin interrupt records it. */
static void edge_entry(struct energy_stream* stream, uint32_t stamp, uint8_t rise, uint8_t fall, uint8_t levels,
                       uint8_t flags) {
    uint32_t const at = stream->edge_entries_written;
    stream->edge_entries[at & (ENERGY_STREAM_EDGE_ENTRIES - 1U)] =
        (energy_stream_edge_entry_t){.stamp = stamp, .rise = rise, .fall = fall, .levels = levels, .flags = flags};
    stream->edge_entries_written = at + 1U;
}

/** A stream whose header read `levels` at `header_raw`, then the run CPU 0's loop asks for at
 *  `sync_raw` reading the same levels with the pins cleared; the capture starts at `started_raw`. */
static void start_edges(struct energy_stream* stream, uint32_t started_raw, uint8_t mask, uint8_t levels,
                        uint32_t header_raw, uint32_t sync_raw) {
    reset_stream(stream);
    energy_stream_timeline_prepare(stream, 1U);
    energy_stream_header_t const header = {
        .rate_hz = CLOCK_HZ / PERIOD, .setting = 1U, .full_scale_mv = 3000U, .first_sequence = 1U,
        .previous_low = true, .low = &low, .high = &high, .dio_mask = mask, .dio_levels = levels,
        .dio_stamp = header_raw, .edge_resolution_ns = energy_stream_edge_resolution(CLOCK_HZ)};
    assert(energy_stream_begin(stream, &header));
    energy_stream_timeline_run(stream, true, CLOCK_HZ, TICKS, started_raw);
    edge_entry(stream, sync_raw, 0U, 0U, levels, SYNC | CLEARED(mask));
}

/** An edges message's edges as absolute times in ns, and their levels. */
static uint32_t edges_of(uint8_t const* m, uint16_t length, uint64_t* times, uint8_t* levels) {
    assert(m[0] == 0xA3U);
    uint32_t const count = u16(m + 10);
    assert(count >= 1U && count <= 99U && length == 12U + 5U * count);
    for (uint32_t i = 0U; i < count; i++) {
        times[i] = u64(m + 2) + u32(m + 12 + 5U * i);
        levels[i] = m[12 + 5U * i + 4U];
    }
    return count;
}

/** The header gives the stamping clock's resolution; each run's edges follow the rule in
 *  energy_stream_edges_flush; pins the stream did not ask for are left out, and read 0. */
static void test_edges(void) {
    static struct energy_stream stream;
    start_edges(&stream, 1000U, 0x09U, 0x0FU, 900U, 1000U + 100U);
    uint16_t length = 0U;
    uint8_t const* m = take(&stream, &length);
    assert(m != NULL && m[64] == 0x09U && m[65] == 0x0FU && u16(m + 66) == 11U);
    assert(stream.edge_levels == 0x09U);
    // DIO0 falls; DIO1 moves too, but nobody asked for it.
    edge_entry(&stream, 1000U + 500U, 0x00U, 0x03U, 0x0CU, 0U);
    // A pulse on DIO3 and back, both latched, level unchanged.
    edge_entry(&stream, 1000U + 960U, 0x08U, 0x08U, 0x0CU, 0U);
    // DIO0 rises, falls and rises again before the interrupt ran: it reads high with both latched.
    edge_entry(&stream, 1000U + 1920U, 0x01U, 0x01U, 0x0DU, 0U);
    // A latch with no change: its edge was the run before's, which read the level after it.
    edge_entry(&stream, 1000U + 2880U, 0x01U, 0x00U, 0x0DU, 0U);
    // A changed level with no latch: the edge came between the clear and the read.
    edge_entry(&stream, 1000U + 3840U, 0x00U, 0x00U, 0x05U, 0U);
    energy_stream_pause(&stream);
    m = take(&stream, &length);
    assert(m != NULL && m[1] == 0U);
    uint64_t times[99];
    uint8_t levels[99];
    uint32_t const count = edges_of(m, length, times, levels);
    // The message's first edge in ns, then each offset from it, rounded down.
    uint64_t const expected_counts[] = {500U, 960U, 960U, 1920U, 1920U, 1920U, 3840U};
    uint8_t const expected_levels[] = {0x08U, 0x00U, 0x08U, 0x09U, 0x08U, 0x09U, 0x01U};
    assert(count == sizeof(expected_levels));
    for (uint32_t i = 0U; i < count; i++) {
        uint64_t const expected = ns(500U) + ((expected_counts[i] - 500U) * 1000000000U) / CLOCK_HZ;
        assert(times[i] == expected && levels[i] == expected_levels[i]);
    }
    assert(take(&stream, &length) == NULL && stream.edge_entries_read == stream.edge_entries_written);
    energy_stream_end(&stream, 2U, 0U);
    assert(stream.edge_mask == 0U);
}

/** Edges leave after the pairs message that closes before them, 99 to a message; a message never
 *  spans ENERGY_STREAM_EDGE_SPAN, and offsets are the exact floor of their counts. */
static void test_edges_messages(void) {
    static struct energy_stream stream;
    start_edges(&stream, 0xFFFFF000U, 0x01U, 0x00U, 0xFFFFF000U - 100U, 0xFFFFF000U - 50U);
    uint16_t length = 0U;
    (void)take(&stream, &length);
    energy_stream_pair(&stream, 1U, 1U, 1U, true);
    // 150 edges a run apart, stamps wrapping, then one a whole span after the 150th.
    uint32_t stamp = 0xFFFFF000U + 7U;
    uint8_t level = 0U;
    for (uint32_t i = 0U; i < 150U; i++, stamp += 1237U) {
        level ^= 1U;
        edge_entry(&stream, stamp, level ? 1U : 0U, level ? 0U : 1U, level, 0U);
    }
    uint32_t const last = stamp - 1237U;
    level ^= 1U;
    edge_entry(&stream, last + ENERGY_STREAM_EDGE_SPAN - 5U, level, level ^ 1U, level, 0U);
    level ^= 1U;
    edge_entry(&stream, last + ENERGY_STREAM_EDGE_SPAN + 9U, level, level ^ 1U, level, 0U);
    energy_stream_batch_end(&stream, stream.next_sequence - 1U);
    assert(take(&stream, &length) == NULL);
    energy_stream_end(&stream, 2U, 1U);
    uint8_t const* m = take(&stream, &length);
    assert(m != NULL && m[0] == 0xA0U && u16(m + 14) == 1U);
    uint64_t times[99];
    uint8_t levels[99];
    // The 151st edge is a span past the 100th, the first of the second message.
    uint32_t const counts[] = {99U, 51U, 2U};
    uint32_t edge = 0U;
    for (uint32_t i = 0U; i < 3U; i++) {
        m = take(&stream, &length);
        assert(m != NULL && edges_of(m, length, times, levels) == counts[i]);
        uint64_t first = 0U;
        for (uint32_t k = 0U; k < counts[i]; k++, edge++) {
            uint64_t const at = (edge < 150U) ? 7U + edge * UINT64_C(1237)
                                              : 7U + 149U * UINT64_C(1237) + ENERGY_STREAM_EDGE_SPAN + (edge == 150U ? 0U : 14U) - 5U;
            first = (k == 0U) ? at : first;
            assert(times[k] == ns(first) + ((at - first) * 1000000000U) / CLOCK_HZ);
            assert(levels[k] == ((edge + 1U) & 1U));
        }
    }
    m = take(&stream, &length);
    assert(m != NULL && m[0] == 0xA2U);
    // Offsets: the exact floor, across the span.
    for (uint32_t c = 0U; c < ENERGY_STREAM_EDGE_SPAN; c += 4099U) {
        assert(energy_stream_edge_offset(&stream, c) == (uint32_t)(((uint64_t)c * 1000000000U) / CLOCK_HZ));
    }
    assert(energy_stream_edge_offset(&stream, ENERGY_STREAM_EDGE_SPAN - 1U) ==
           (uint32_t)(((uint64_t)(ENERGY_STREAM_EDGE_SPAN - 1U) * 1000000000U) / CLOCK_HZ));
}

/** A run after dropped edges ends the message before it and flags the next; so does an edges
 *  message the full ring dropped. A stamp before the capture started reads 0; a new stream
 *  forgets what was recorded before it. */
static void test_edges_lost(void) {
    static struct energy_stream stream;
    start_edges(&stream, 5000U, 0x02U, 0x00U, 3000U, 3500U);
    uint16_t length = 0U;
    (void)take(&stream, &length);
    edge_entry(&stream, 4000U, 0x02U, 0x00U, 0x02U, 0U);
    edge_entry(&stream, 6000U, 0x00U, 0x02U, 0x00U, 0U);
    edge_entry(&stream, 9000U, 0x02U, 0x00U, 0x02U, LOST);
    energy_stream_pause(&stream);
    uint64_t times[99];
    uint8_t levels[99];
    uint8_t const* m = take(&stream, &length);
    assert(m != NULL && m[1] == 0U && edges_of(m, length, times, levels) == 2U);
    assert(times[0] == 0U && times[1] == ns(1000U) && levels[0] == 0x02U && levels[1] == 0x00U);
    m = take(&stream, &length);
    assert(m != NULL && m[1] == ENERGY_STREAM_EDGES_LOST && edges_of(m, length, times, levels) == 1U);
    assert(times[0] == ns(4000U) && levels[0] == 0x02U);
    // Fill the ring to its control slots: the edges message after is dropped, and the next one that
    // leaves says so.
    stream.period = 960U;
    uint32_t s = 1U;
    while (energy_stream_free(&stream) > ENERGY_STREAM_CONTROL_SLOTS) {
        energy_stream_pair(&stream, s++, 1U, 1U, true);
    }
    edge_entry(&stream, 20000U, 0x00U, 0x02U, 0x00U, 0U);
    energy_stream_pause(&stream);
    assert(stream.edge_entries_read == stream.edge_entries_written && (stream.edge_flags & ENERGY_STREAM_EDGES_LOST));
    while (take(&stream, &length) != NULL) {
    }
    edge_entry(&stream, 30000U, 0x02U, 0x00U, 0x02U, 0U);
    energy_stream_pause(&stream);
    m = take(&stream, &length);
    assert(m != NULL && m[1] == ENERGY_STREAM_EDGES_LOST && edges_of(m, length, times, levels) == 1U);
    // Recorded before a new stream began: not the new stream's.
    edge_entry(&stream, 31000U, 0x00U, 0x02U, 0x00U, 0U);
    energy_stream_end(&stream, 3U, s);
    while (take(&stream, &length) != NULL) {
    }
    edge_entry(&stream, 32000U, 0x02U, 0x00U, 0x02U, 0U);
    energy_stream_header_t const header = {.rate_hz = 1000U, .low = &low, .high = &high, .dio_mask = 0x02U};
    assert(energy_stream_begin(&stream, &header));
    energy_stream_pause(&stream);
    m = take(&stream, &length);
    assert(m != NULL && m[0] == 0xA1U && take(&stream, &length) == NULL);
    // Without pins to report, entries are dropped unsent.
    energy_stream_end(&stream, 3U, s);
    while (take(&stream, &length) != NULL) {
    }
    energy_stream_header_t const none = {.rate_hz = 1000U, .low = &low, .high = &high, .dio_mask = 0U};
    assert(energy_stream_begin(&stream, &none));
    (void)take(&stream, &length);
    edge_entry(&stream, 33000U, 0x02U, 0x00U, 0x02U, 0U);
    energy_stream_pause(&stream);
    assert(take(&stream, &length) == NULL && stream.edge_entries_read == stream.edge_entries_written);
}

/** The edges of one stream's messages, as counts since the capture started and the levels after. */
static uint32_t all_edges(struct energy_stream* stream, uint64_t* times, uint8_t* levels) {
    uint32_t count = 0U;
    uint16_t length = 0U;
    uint8_t const* m = NULL;
    while ((m = take(stream, &length)) != NULL) {
        if (m[0] == 0xA3U) {
            count += edges_of(m, length, times + count, levels + count);
        }
    }
    return count;
}

/** The cases a run's read order makes: an edge between its clear and its level read is reported
 *  with it, and its latch in the next run is not counted again, unless the loop cleared the pin's
 *  detectors in between. */
static void test_edges_rule(void) {
    static struct energy_stream stream;
    start_edges(&stream, 0U, 0x03U, 0x00U, 0U, 10U);
    uint16_t length = 0U;
    (void)take(&stream, &length);
    // A short pulse on DIO0: its rise latched, its fall after the clear.
    edge_entry(&stream, 100U, 0x01U, 0x00U, 0x00U, 0U);
    edge_entry(&stream, 200U, 0x00U, 0x01U, 0x00U, 0U);
    // DIO1 rises after one run's clear and falls before the next run's read.
    edge_entry(&stream, 300U, 0x00U, 0x00U, 0x02U, 0U);
    edge_entry(&stream, 310U, 0x02U, 0x02U, 0x00U, 0U);
    // DIO1 rises after a clear; the loop then clears it, and a pulse low comes and goes.
    edge_entry(&stream, 400U, 0x00U, 0x00U, 0x02U, 0U);
    edge_entry(&stream, 500U, 0x02U, 0x00U, 0x02U, CLEARED(0x02U));
    // A lone rise on a pin already high: its fall merged into a latch already cleared.
    edge_entry(&stream, 600U, 0x02U, 0x00U, 0x02U, 0U);
    energy_stream_pause(&stream);
    uint64_t times[99];
    uint8_t levels[99];
    uint32_t const count = all_edges(&stream, times, levels);
    uint64_t const expected_counts[] = {100U, 100U, 300U, 310U, 400U, 500U, 500U, 600U, 600U};
    uint8_t const expected_levels[] = {0x01U, 0x00U, 0x02U, 0x00U, 0x02U, 0x00U, 0x02U, 0x00U, 0x02U};
    assert(count == sizeof(expected_levels));
    for (uint32_t i = 0U; i < count; i++) {
        assert(times[i] == ns(100U) + ((expected_counts[i] - 100U) * 1000000000U) / CLOCK_HZ);
        assert(levels[i] == expected_levels[i]);
    }

    // Before the first sync stamped after the header, nothing counts: an older sync, and a run with
    // a latch the header's read may already show. A pin that moved since shows at the sync.
    start_edges(&stream, 0U, 0x01U, 0x00U, 1000U, 900U);
    (void)take(&stream, &length);
    edge_entry(&stream, 1100U, 0x01U, 0x00U, 0x01U, 0U);
    edge_entry(&stream, 1200U, 0x00U, 0x00U, 0x01U, SYNC | CLEARED(0x01U));
    edge_entry(&stream, 1300U, 0x01U, 0x00U, 0x01U, 0U);
    energy_stream_pause(&stream);
    assert(all_edges(&stream, times, levels) == 1U && times[0] == ns(1200U) && levels[0] == 0x01U);
}

/** One marker of a markers message from `at`: its time, number and label; returns what follows. */
static uint8_t const* mark_of(uint8_t const* at, uint64_t* time, uint32_t* number, char* label) {
    *time = u64(at);
    *number = u32(at + 8);
    uint8_t const length = at[12];
    memcpy(label, at + 13, length);
    label[length] = '\0';
    return at + 13 + length;
}

/** 0x94 as CPU 1 runs it, with the stamping timer reading `stamp`. */
static uint8_t mark(struct energy_stream* stream, uint32_t stamp, uint32_t number, char const* label) {
    volatile uint32_t timer = stamp;
    uint8_t const result = energy_stream_mark(stream, &timer, number, (uint8_t const*)label, (uint8_t)strlen(label));
    assert(!stream->mark_busy);
    return result;
}

static bool due(struct energy_stream const* stream, uint32_t now) {
    volatile uint32_t timer = now;
    return energy_stream_marks_due(stream, &timer);
}

static char const label32[] = "0123456789abcdef0123456789ABCDEF";

/** Markers go with the next pairs message, timed on the pairs' clock, in the order they ran. */
static void test_marks(void) {
    static struct energy_stream stream;
    start(&stream, 0U, false);
    assert(mark(&stream, 1000U, 1U, "nobody") == ENERGY_STREAM_MARK_NO_STREAM);
    assert(stream.marks_written == 0U);

    start(&stream, 0U, true);
    uint16_t length = 0U;
    (void)take(&stream, &length);
    // The stamping timer started with the capture's timer, so a stamp is its count since the start.
    uint32_t const stamps[] = {TICKS + 3U * PERIOD + 7U, TICKS + 5U * PERIOD, TICKS + 9U * PERIOD + 1U};
    assert(mark(&stream, stamps[0], 0xDEADBEEFU, "") == ENERGY_STREAM_MARK_SENT);
    assert(mark(&stream, stamps[1], 2U, "a") == ENERGY_STREAM_MARK_SENT);
    assert(mark(&stream, stamps[2], 3U, label32) == ENERGY_STREAM_MARK_SENT);
    // Nothing leaves before a pairs message closes or the owner finds them due.
    assert(take(&stream, &length) == NULL);
    assert(!due(&stream, stamps[0] + CLOCK_HZ / 100U - 1U));
    assert(due(&stream, stamps[0] + CLOCK_HZ / 100U));
    for (uint32_t s = 1U; s <= 10U; s++) {
        energy_stream_pair(&stream, s, 1U, 1U, true);
    }
    energy_stream_batch_end(&stream, stream.next_sequence - 1U);
    uint8_t const* m = take(&stream, &length);
    assert(m != NULL && m[0] == 0xA0U && u16(m + 14) == 10U);
    m = take(&stream, &length);
    assert(m != NULL && m[0] == 0xA4U && m[1] == 0U && m[2] == 3U);
    assert(length == 3U + 3U * 13U + 0U + 1U + 32U);
    uint64_t time = 0U;
    uint32_t number = 0U;
    char label[33];
    uint8_t const* at = mark_of(m + 3, &time, &number, label);
    assert(time == ns(stamps[0]) && number == 0xDEADBEEFU && label[0] == '\0');
    at = mark_of(at, &time, &number, label);
    assert(time == ns(stamps[1]) && number == 2U && strcmp(label, "a") == 0);
    at = mark_of(at, &time, &number, label);
    assert(time == ns(stamps[2]) && number == 3U && strcmp(label, label32) == 0);
    assert(at == m + length);
    assert(take(&stream, &length) == NULL && !due(&stream, stamps[2] + CLOCK_HZ));

    // The owner sends due markers on their own, without a pairs message.
    assert(mark(&stream, TICKS + 11U * PERIOD, 4U, "alone") == ENERGY_STREAM_MARK_SENT);
    energy_stream_close(&stream);
    m = take(&stream, &length);
    assert(m != NULL && m[0] == 0xA4U && m[2] == 1U);
    (void)mark_of(m + 3, &time, &number, label);
    assert(time == ns(TICKS + 11U * PERIOD) && number == 4U && strcmp(label, "alone") == 0);

    // At the end, markers leave before the end message, and none is taken after it.
    assert(mark(&stream, TICKS + 12U * PERIOD, 5U, "last") == ENERGY_STREAM_MARK_SENT);
    energy_stream_end(&stream, 1U, 10U);
    m = take(&stream, &length);
    assert(m != NULL && m[0] == 0xA4U && m[2] == 1U);
    m = take(&stream, &length);
    assert(m != NULL && m[0] == 0xA2U);
    assert(mark(&stream, TICKS + 13U * PERIOD, 6U, "late") == ENERGY_STREAM_MARK_NO_STREAM);
    assert(stream.marks_written == stream.marks_read);
}

/** Sixteen wait at most: the next is refused and flagged on the next message. Eleven fill one. */
static void test_marks_full(void) {
    static struct energy_stream stream;
    start(&stream, 0U, true);
    uint16_t length = 0U;
    (void)take(&stream, &length);
    for (uint32_t i = 0U; i < ENERGY_STREAM_MARKS; i++) {
        assert(mark(&stream, TICKS + i, i, label32) == ENERGY_STREAM_MARK_SENT);
    }
    assert(mark(&stream, TICKS + 100U, 99U, "dropped") == ENERGY_STREAM_MARK_FULL);
    energy_stream_close(&stream);
    uint32_t seen = 0U;
    for (uint32_t message = 0U; message < 2U; message++) {
        uint8_t const* m = take(&stream, &length);
        uint8_t const count = message == 0U ? ENERGY_STREAM_MARKS_MAX : ENERGY_STREAM_MARKS - ENERGY_STREAM_MARKS_MAX;
        assert(m != NULL && m[0] == 0xA4U && m[1] == 0U && m[2] == count);
        assert(length == 3U + count * (13U + 32U) && length < ENERGY_STREAM_MESSAGE_MAX);
        uint8_t const* at = m + 3;
        for (uint32_t i = 0U; i < count; i++, seen++) {
            uint64_t time = 0U;
            uint32_t number = 0U;
            char label[33];
            at = mark_of(at, &time, &number, label);
            assert(number == seen && time == ns(TICKS + seen));
        }
    }
    assert(take(&stream, &length) == NULL);
    assert(mark(&stream, TICKS + 200U, 100U, "after") == ENERGY_STREAM_MARK_SENT);
    energy_stream_close(&stream);
    uint8_t const* m = take(&stream, &length);
    assert(m != NULL && m[0] == 0xA4U && m[1] == ENERGY_STREAM_MARKS_LOST && m[2] == 1U);
    energy_stream_close(&stream);
    assert(take(&stream, &length) == NULL);
}

/** Due markers sent while a pairs message is being built: it leaves first, whole, as CPU 0's loop
 *  sends them, and the next one starts afresh. */
static void test_marks_open_pairs(void) {
    static struct energy_stream stream;
    start(&stream, 0U, true);
    uint16_t length = 0U;
    (void)take(&stream, &length);
    for (uint32_t s = 1U; s <= 7U; s++) {
        energy_stream_pair(&stream, s, (uint16_t)(100U + s), 1U, true);
    }
    energy_stream_batch_end(&stream, stream.next_sequence - 1U);
    assert(take(&stream, &length) == NULL && stream.open_pairs == 7U);
    assert(mark(&stream, TICKS + 7U * PERIOD, 1U, "due") == ENERGY_STREAM_MARK_SENT);
    assert(due(&stream, TICKS + 7U * PERIOD + CLOCK_HZ / 100U));
    energy_stream_close(&stream);
    uint8_t const* m = take(&stream, &length);
    assert(m != NULL && m[0] == 0xA0U && u32(m + 2) == 1U && u16(m + 14) == 7U && length == 16U + 7U * 5U);
    for (uint32_t i = 0U; i < 7U; i++) {
        assert(u16(m + 16U + i * 5U) == 101U + i);
    }
    m = take(&stream, &length);
    assert(m != NULL && m[0] == 0xA4U && m[2] == 1U);
    for (uint32_t s = 8U; s <= 14U; s++) {
        energy_stream_pair(&stream, s, (uint16_t)(100U + s), 1U, true);
    }
    energy_stream_close(&stream);
    m = take(&stream, &length);
    assert(m != NULL && m[0] == 0xA0U && u32(m + 2) == 8U && u16(m + 14) == 7U && u16(m + 16) == 108U);
}

/** A marker refused in one stream says nothing about the next stream's first. */
static void test_marks_lost_per_stream(void) {
    static struct energy_stream stream;
    start(&stream, 0U, true);
    uint16_t length = 0U;
    (void)take(&stream, &length);
    for (uint32_t i = 0U; i <= ENERGY_STREAM_MARKS; i++) {
        (void)mark(&stream, TICKS + i, i, "x");
    }
    energy_stream_end(&stream, 1U, 0U);
    while (take(&stream, &length) != NULL) {
    }
    energy_stream_header_t const header = {.rate_hz = 1000U, .low = &low, .high = &high};
    assert(energy_stream_begin(&stream, &header));
    (void)take(&stream, &length);
    assert(mark(&stream, TICKS + 100U, 100U, "fresh") == ENERGY_STREAM_MARK_SENT);
    energy_stream_close(&stream);
    uint8_t const* const m = take(&stream, &length);
    assert(m != NULL && m[0] == 0xA4U && m[1] == 0U && m[2] == 1U);
}

/** A host that falls behind loses markers messages too, and the next one that leaves says so. */
static void test_marks_behind(void) {
    static struct energy_stream stream;
    start(&stream, 0U, true);
    uint16_t length = 0U;
    (void)take(&stream, &length);
    stream.period = 960U;
    for (uint32_t s = 1U; s <= 99U * 14U; s++) {
        energy_stream_pair(&stream, s, 1U, 1U, true);
    }
    assert(energy_stream_free(&stream) == ENERGY_STREAM_CONTROL_SLOTS);
    assert(mark(&stream, TICKS + 5U, 1U, "lost") == ENERGY_STREAM_MARK_SENT);
    energy_stream_close(&stream);
    assert(energy_stream_free(&stream) == ENERGY_STREAM_CONTROL_SLOTS);
    while (take(&stream, &length) != NULL) {
    }
    assert(mark(&stream, TICKS + 6U, 2U, "kept") == ENERGY_STREAM_MARK_SENT);
    energy_stream_close(&stream);
    uint8_t const* m = take(&stream, &length);
    assert(m != NULL && m[0] == 0xA4U && m[1] == ENERGY_STREAM_MARKS_LOST && m[2] == 1U && u32(m + 3 + 8) == 2U);
}

/** A marker stamped before the first pair is at time 0; one recorded while a start has no timeline
 *  yet is not sent if the stream ends first. */
static void test_marks_early(void) {
    static struct energy_stream stream;
    start(&stream, 1000U, true);
    uint16_t length = 0U;
    (void)take(&stream, &length);
    assert(mark(&stream, 500U, 1U, "early") == ENERGY_STREAM_MARK_SENT);
    energy_stream_close(&stream);
    uint8_t const* m = take(&stream, &length);
    assert(m != NULL && m[0] == 0xA4U && u64(m + 3) == 0U);

    reset_stream(&stream);
    energy_stream_timeline_prepare(&stream, 1U);
    energy_stream_header_t const header = {.rate_hz = 1000U, .low = &low, .high = &high};
    assert(energy_stream_begin(&stream, &header));
    (void)take(&stream, &length);
    assert(mark(&stream, 123U, 1U, "pending") == ENERGY_STREAM_MARK_SENT);
    energy_stream_end(&stream, 4U, 0U);
    m = take(&stream, &length);
    assert(m != NULL && m[0] == 0xA2U);
    assert(take(&stream, &length) == NULL);
}

static void test_ns(void) {
    uint64_t const counts[] = {0U, 1U, CLOCK_HZ - 1U, CLOCK_HZ, UINT64_C(1) << 40U, UINT64_C(0xFFFFFFFFFFFF)};
    for (uint32_t i = 0U; i < sizeof(counts) / sizeof(counts[0]); i++) {
        assert(energy_stream_ns(counts[i], CLOCK_HZ) == ns(counts[i]));
    }
}

int main(void) {
    test_header();
    test_identified_header();
    test_pairs();
    test_ten_ms();
    test_holds();
    test_out_of_step();
    test_resume();
    test_full();
    test_full_after_hold();
    test_full_recovers();
    test_untimed();
    test_idle();
    test_edges();
    test_edges_messages();
    test_edges_lost();
    test_edges_rule();
    test_marks();
    test_marks_full();
    test_marks_open_pairs();
    test_marks_lost_per_stream();
    test_marks_behind();
    test_marks_early();
    test_ns();
    return 0;
}
