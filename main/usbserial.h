// A unit's USB serial number: its MAC as 12 lower-case hex digits, so a machine it is plugged into names it
// (/dev/serial/by-id/, /dev/input/by-id/, /dev/disk/by-id/) and a bench can tell which unit it holds. Pure, tested.
#pragma once
#include <stdint.h>

#define USBSERIAL_LEN 12

void usbserial_from_mac(const uint8_t mac[6], char out[USBSERIAL_LEN + 1]);
