#include "powerrules.h"

power_t powerrules_source(int mv, int usb_mv, int floor_mv)
{
    if (mv < floor_mv) return POWER_UNKNOWN;
    return mv >= usb_mv ? POWER_USB : POWER_BATTERY;
}
