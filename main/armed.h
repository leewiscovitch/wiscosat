// The armed key: a report the unit sends by itself when the host next configures it (a USB bus reset and
// SET_CONFIGURATION, as a PC's firmware does at every start), every every_ms for for_ms, then never again until it
// is armed anew. Pure: the caller gives the time and sends the reports. Tested on Linux.
// Times are milliseconds since the unit started (uint32_t; the differences are wrap-safe).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define ARMED_EVERY_MS_DEFAULT 100
#define ARMED_EVERY_MS_MIN 20
#define ARMED_EVERY_MS_MAX 5000
#define ARMED_FOR_MS_DEFAULT 3000
#define ARMED_FOR_MS_MAX 60000

typedef struct {
    bool armed;  // waiting for the host's next configured
    bool firing; // sending now
    bool done;   // a firing has ended; its counters below are kept until the next one starts
    uint8_t report[8];
    uint32_t every_ms, for_ms;
    uint32_t after_taken_ms; // 0: the whole of for_ms; else it ends this long after the first report the host took
    uint32_t configured_ms;  // when the firing started
    uint32_t slot;           // the next slot: due at configured_ms + slot * every_ms
    uint32_t last_due_ms;    // when the last report was due (armed_due said true)
    uint32_t first_taken_ms; // when the first report the host took was due; 0 none yet
    uint32_t end_ms;
    int tried, taken;
} armed_t;

// arm: the report is sent at the slots 0, every_ms, 2 * every_ms, ... before for_ms (at least once)
void armed_set(armed_t *a, const uint8_t report[8], uint32_t every_ms, uint32_t for_ms);
// end early once a host is there: after_taken_ms after the first report it took (for_ms stays the limit, for a
// host that takes none). Set after armed_set; 0 the whole of for_ms
void armed_end_after_taken(armed_t *a, uint32_t after_taken_ms);
void armed_clear(armed_t *a);
// stop a firing now (a person's press, disarm); its counters stay
void armed_stop(armed_t *a, uint32_t now_ms);
// armed again for the host's next configured, with the same key and schedule: a key that stays ("sticky") after
// its firing has ended. The last firing's counters are kept until the next one starts
void armed_rearm(armed_t *a);
// the host has configured the unit: an armed key starts firing (once: it is no longer armed)
void armed_configured(armed_t *a, uint32_t now_ms);
// is a report due now? A slot missed by more than every_ms is skipped, not sent late in a bunch. Past for_ms it
// ends the firing and says false from then on
bool armed_due(armed_t *a, uint32_t now_ms);
// how long until the next slot (0: due now, or not firing)
uint32_t armed_wait_ms(const armed_t *a, uint32_t now_ms);
// what became of the report armed_due asked for: taken by the host or not
void armed_result(armed_t *a, bool taken);
