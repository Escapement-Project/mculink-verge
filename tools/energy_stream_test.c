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
    return stream->slot[at];
}

static energy_calibration_t const low = {.code = {100U, 30000U}, .current_na = {5000U, 900000U}, .count = 2U};
static energy_calibration_t const high = {
    .code = {200U, 20000U, 40000U, 60000U}, .current_na = {1000000U, 50000000U, 100000000U, 150000000U}, .count = 4U};

static void start(struct energy_stream* stream, uint32_t started_raw, bool streaming) {
    memset(stream, 0, sizeof(*stream));
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
    energy_stream_batch_end(&stream);
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
    energy_stream_batch_end(&stream);
    assert(take(&stream, &length) == NULL);
    for (uint32_t s = 8U; s <= 14U; s++) {
        energy_stream_pair(&stream, s, 1U, 1U, true);
    }
    uint8_t const* m = take(&stream, &length);
    assert(m != NULL && u32(m + 2) == 1U && u16(m + 14) == 10U);
    energy_stream_batch_end(&stream);
    assert(take(&stream, &length) == NULL && stream.open_pairs == 4U);
    for (uint32_t s = 15U; s <= 20U; s++) {
        energy_stream_pair(&stream, s, 1U, 1U, true);
    }
    energy_stream_batch_end(&stream);
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
        energy_stream_batch_begin(&stream);
        for (uint32_t s = 1U; s <= 3U; s++) {
            energy_stream_pair(&stream, s, 1U, 1U, true);
        }
        // Pair 4 was due at started + TICKS + 3 x PERIOD, and came 12,345 counts later.
        uint32_t const due = started + TICKS + 3U * PERIOD;
        energy_stream_hold_t* const hold = energy_stream_hold(&stream, due + lags[i]);
        assert(hold != NULL);
        hold->came = due + 12345U;
        energy_stream_batch_begin(&stream);
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
    energy_stream_batch_begin(&stream);
    (void)energy_stream_time(&stream, 1U);
    energy_stream_hold_t* const hold = energy_stream_hold(&stream, TICKS + 2U * PERIOD);
    hold->came = TICKS + 2U * PERIOD + 500U;
    assert(energy_stream_time(&stream, 2U) == TICKS + PERIOD && stream.holds_read == 0U);
    energy_stream_batch_begin(&stream);
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
    energy_stream_batch_begin(&stream);
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

/** Without a stream, pairs are timed and nothing is sent. */
static void test_idle(void) {
    static struct energy_stream stream;
    start(&stream, 0U, false);
    for (uint32_t s = 1U; s <= 300U; s++) {
        energy_stream_pair(&stream, s, 1U, 1U, true);
    }
    energy_stream_batch_end(&stream);
    assert(stream.written == 0U && stream.next_sequence == 301U);
    assert(stream.next_counts == TICKS + 300U * (uint64_t)PERIOD);
}

static void test_ns(void) {
    uint64_t const counts[] = {0U, 1U, CLOCK_HZ - 1U, CLOCK_HZ, UINT64_C(1) << 40U, UINT64_C(0xFFFFFFFFFFFF)};
    for (uint32_t i = 0U; i < sizeof(counts) / sizeof(counts[0]); i++) {
        assert(energy_stream_ns(counts[i], CLOCK_HZ) == ns(counts[i]));
    }
}

int main(void) {
    test_header();
    test_pairs();
    test_ten_ms();
    test_holds();
    test_out_of_step();
    test_resume();
    test_full();
    test_idle();
    test_ns();
    return 0;
}
