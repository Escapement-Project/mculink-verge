#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "gen.h"

int main(void) {
    struct gen_event events[4] = {{0}};
    struct gen_schedule schedule;
    gen_schedule_reset(&schedule, events, 3U);
    gen_put(&schedule, 9U, 109U);
    gen_put(&schedule, 1U, 101U);
    gen_put(&schedule, 5U, 105U);
    assert(schedule.n == 3U && !schedule.failed);
    assert(events[0].cycle == 1U && events[1].cycle == 5U && events[2].cycle == 9U);
    assert(gen_at(&schedule, 1U) == 101U && gen_at(&schedule, 5U) == 105U && gen_at(&schedule, 9U) == 109U);
    assert(gen_taken(&schedule, 5U) && !gen_taken(&schedule, 6U) && gen_at(&schedule, 6U) == 0U);
    struct gen_event before[4];
    memcpy(before, events, sizeof(events));
    gen_put(&schedule, 7U, 107U);
    assert(schedule.failed && schedule.n == 3U && memcmp(before, events, sizeof(events)) == 0);

    gen_schedule_reset(&schedule, events, 3U);
    gen_put(&schedule, 5U, 105U);
    memcpy(before, events, sizeof(events));
    gen_put(&schedule, 5U, 205U);
    assert(schedule.failed && schedule.n == 1U && memcmp(before, events, sizeof(events)) == 0);

    gen_schedule_reset(&schedule, events, 0U);
    gen_put(&schedule, 0U, 100U);
    assert(schedule.failed && schedule.n == 0U && memcmp(before, events, sizeof(events)) == 0);

    gen_schedule_reset(&schedule, events, 3U);
    gen_put(&schedule, UINT32_MAX, 999U);
    gen_put(&schedule, 0U, 100U);
    assert(schedule.n == 2U && !schedule.failed && events[0].cycle == 0U && events[1].cycle == UINT32_MAX);
    assert(gen_at(&schedule, UINT32_MAX) == 999U);
}
