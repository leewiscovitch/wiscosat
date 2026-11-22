// The channel's lockout (lockout.h). Times are milliseconds since the start; the clock may wrap.
#include "lockout.h"

void lockout_fail(lockout_t *l, uint32_t now_ms)
{
    l->failed++;
    l->in_row++;
    if (l->deaf_ms) // deaf before and no good handshake since: each failure doubles it
        l->deaf_ms = l->deaf_ms >= LOCKOUT_MAX_MS / 2 ? LOCKOUT_MAX_MS : l->deaf_ms * 2;
    else if (l->in_row >= LOCKOUT_AFTER)
        l->deaf_ms = LOCKOUT_FIRST_MS;
    else
        return;
    l->deaf_until_ms = now_ms + l->deaf_ms;
}

void lockout_ok(lockout_t *l)
{
    l->in_row = 0;
    l->deaf_ms = 0;
}

bool lockout_deaf(const lockout_t *l, uint32_t now_ms)
{
    return l->deaf_ms && (int32_t)(l->deaf_until_ms - now_ms) > 0;
}

uint32_t lockout_until(const lockout_t *l, uint32_t now_ms)
{
    return lockout_deaf(l, now_ms) ? l->deaf_until_ms : 0;
}
