// Whether the unit runs on USB power, from the voltage its battery pin reads (battery.c). Pure, tested on Linux.
// On the LilyGO T-Embed that pin reads the board's supply rail, not the cell alone: USB's 5 V through a Schottky
// diode while a cable is in (about 4.7 V), the battery's own voltage once it is out. A Li-ion cell never holds more
// than 4.2 V, so a reading at or over the line (Kconfig SAT_BATTERY_USB_MV, 4400 mV) is USB power.
#pragma once
#include <stdbool.h>

bool powerrules_usb(int mv, int usb_mv);
