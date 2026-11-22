// A unit's USB serial number (usbserial.h).
#include "usbserial.h"

void usbserial_from_mac(const uint8_t mac[6], char out[USBSERIAL_LEN + 1])
{
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 6; i++) {
        out[2 * i] = hex[mac[i] >> 4];
        out[2 * i + 1] = hex[mac[i] & 0xF];
    }
    out[USBSERIAL_LEN] = 0;
}
