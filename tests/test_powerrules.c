#include "check.h"
#include "powerrules.h"

void test_powerrules(void)
{
    // the T-Embed's pin reads its supply rail: USB through a Schottky diode with the cable in (4664 mV read on a
    // unit, 2026-11-25; the same with no battery at all), the battery with it out (3490 mV). A Li-ion cell never
    // holds more than 4.2 V
    CHECK(powerrules_source(4664, 4400, 3000) == POWER_USB);
    CHECK(powerrules_source(4400, 4400, 3000) == POWER_USB);
    CHECK(powerrules_source(4399, 4400, 3000) == POWER_BATTERY);
    CHECK(powerrules_source(4200, 4400, 3000) == POWER_BATTERY); // a full cell, cable out
    CHECK(powerrules_source(3490, 4400, 3000) == POWER_BATTERY);
    CHECK(powerrules_source(3000, 4400, 3000) == POWER_BATTERY);
    // under the floor no running board's rail can be: a pin wired to no battery (another board), so nothing is said
    CHECK(powerrules_source(2999, 4400, 3000) == POWER_UNKNOWN);
    CHECK(powerrules_source(0, 4400, 3000) == POWER_UNKNOWN);
}
