#include "powerrules.h"

bool powerrules_usb(int mv, int usb_mv) { return mv >= usb_mv; }
