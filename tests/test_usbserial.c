#include "check.h"
#include "usbserial.h"

void test_usbserial(void)
{
    char s[USBSERIAL_LEN + 1];
    memset(s, 'x', sizeof(s));
    usbserial_from_mac((const uint8_t[6]){0xdc, 0xda, 0x0c, 0x52, 0x37, 0x38}, s);
    CHECK(!strcmp(s, "dcda0c523738"));
    usbserial_from_mac((const uint8_t[6]){0, 1, 0xa0, 0xff, 0x0f, 0xf0}, s);
    CHECK(!strcmp(s, "0001a0ff0ff0"));
}
