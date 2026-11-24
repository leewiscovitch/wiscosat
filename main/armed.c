// The armed key's schedule and counters (armed.h).
#include <string.h>
#include "armed.h"

// the number of slots: every_ms apart, before for_ms, at least one
static uint32_t slots(const armed_t *a) { return a->for_ms ? (a->for_ms + a->every_ms - 1) / a->every_ms : 1; }

void armed_set(armed_t *a, const uint8_t report[8], uint32_t every_ms, uint32_t for_ms)
{
    memset(a, 0, sizeof(*a));
    memcpy(a->report, report, 8);
    a->every_ms = every_ms ? every_ms : ARMED_EVERY_MS_DEFAULT;
    a->for_ms = for_ms;
    a->armed = true;
}

void armed_end_after_taken(armed_t *a, uint32_t after_taken_ms) { a->after_taken_ms = after_taken_ms; }

void armed_clear(armed_t *a) { a->armed = false; }

void armed_configured(armed_t *a, uint32_t now_ms)
{
    if (!a->armed) return;
    a->armed = false;
    a->firing = true;
    a->done = false;
    a->configured_ms = now_ms;
    a->slot = 0;
    a->last_due_ms = a->first_taken_ms = a->end_ms = 0;
    a->tried = a->taken = 0;
}

static void end(armed_t *a, uint32_t now_ms)
{
    a->firing = false;
    a->done = true;
    a->end_ms = now_ms;
}

void armed_stop(armed_t *a, uint32_t now_ms)
{
    if (a->firing) end(a, now_ms);
}

void armed_rearm(armed_t *a)
{
    if (!a->firing) a->armed = true;
}

bool armed_due(armed_t *a, uint32_t now_ms)
{
    if (!a->firing) return false;
    uint32_t since = now_ms - a->configured_ms;
    if (since >= 0x80000000u) return false; // before the start (a clock read just ahead of armed_configured)
    if (since < a->slot * a->every_ms) return false;
    if (a->after_taken_ms && a->taken && now_ms - a->first_taken_ms >= a->after_taken_ms) {
        end(a, now_ms);
        return false;
    }
    uint32_t current = since / a->every_ms; // the slot now; later than a->slot when slots were missed
    if (current >= slots(a)) {
        end(a, now_ms);
        return false;
    }
    if (since - a->slot * a->every_ms >= a->every_ms) a->slot = current; // fell behind: skip, don't bunch up
    a->slot++;
    a->last_due_ms = now_ms;
    return true;
}

uint32_t armed_wait_ms(const armed_t *a, uint32_t now_ms)
{
    if (!a->firing) return 0;
    uint32_t since = now_ms - a->configured_ms, next = a->slot * a->every_ms;
    return since < next ? next - since : 0;
}

void armed_result(armed_t *a, bool taken)
{
    a->tried++;
    if (!taken) return;
    if (!a->taken) a->first_taken_ms = a->last_due_ms;
    a->taken++;
}
