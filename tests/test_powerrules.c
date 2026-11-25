#include "check.h"
#include "powerrules.h"

void test_powerrules(void)
{
    // the T-Embed's pin reads its supply rail: USB through a Schottky diode with the cable in (4664 mV read on a
    // unit, 2026-11-25), the battery with it out (3490 mV). A Li-ion cell never holds more than 4.2 V
    CHECK(powerrules_usb(4664, 4400));
    CHECK(powerrules_usb(4400, 4400));
    CHECK(!powerrules_usb(4399, 4400));
    CHECK(!powerrules_usb(4200, 4400)); // a full cell, cable out
    CHECK(!powerrules_usb(3490, 4400));
    CHECK(!powerrules_usb(0, 4400));
}
