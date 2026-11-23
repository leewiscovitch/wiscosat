// The Linux tests of the firmware's pure parts (keys, armed, proto, seedrules, lockout, settings, wlsplit): ./test
#include "check.h"

int check_run, check_failed;

int main(void)
{
    test_keys();
    test_armed();
    test_proto();
    test_seedrules();
    test_updrules();
    test_lockout();
    test_settings();
    test_usbserial();
    test_wlsplit();
    printf("test: %d checks, %d failed\n", check_run, check_failed);
    return check_failed ? 1 : 0;
}
