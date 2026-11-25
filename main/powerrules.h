// Which power the unit runs on, from the voltage its battery pin reads (battery.c). Pure, tested on Linux.
// On the LilyGO T-Embed that pin reads the board's supply rail, not the cell alone: USB's 5 V through a Schottky
// diode while a cable is in (about 4.7 V, with or without a battery), the battery's own voltage once it is out. A
// Li-ion cell never holds more than 4.2 V, so a reading at or over usb_mv (Kconfig SAT_BATTERY_USB_MV, 4400) is USB
// power. Under floor_mv (SAT_BATTERY_FLOOR_MV, 3000) no running board's rail can be (it would have browned out): the
// pin is wired to no battery (a board without one), and nothing is said.
#pragma once

typedef enum { POWER_UNKNOWN, POWER_BATTERY, POWER_USB } power_t;

power_t powerrules_source(int mv, int usb_mv, int floor_mv);
