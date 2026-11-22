#include "check.h"
#include "settings.h"

static settings_t good(void)
{
    settings_t s = {0};
    strcpy(s.name, "unit-1");
    strcpy(s.wifi_ssid, "example");
    strcpy(s.wifi_pass, "password1");
    strcpy(s.ip_str, "192.0.2.61");
    strcpy(s.mask_str, "255.255.255.0");
    strcpy(s.gw_str, "192.0.2.1");
    s.key_len = 32;
    return s;
}

void test_settings(void)
{
    uint8_t a[4];
    CHECK(settings_parse_ip("192.0.2.61", a) && a[0] == 192 && a[1] == 0 && a[2] == 2 && a[3] == 61);
    CHECK(settings_parse_ip("0.0.0.0", a) && settings_parse_ip("255.255.255.255", a));
    static const char *bad[] = {"192.0.2.999", "", "192.0.2", "192.0.2.61.1", "192.0.2.", ".192.0.2", "192..2.61",
                                "192.0.2.6x", " 192.0.2.61", "192.0.2.61 ", "1920.0.2.61", "-1.0.2.61", "0x1.0.2.61",
                                "256.0.0.1"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) CHECK(!settings_parse_ip(bad[i], a));

    // a whole set: good, and each way it makes the board unenrolled
    settings_t s = good();
    CHECK(settings_check(&s));
    CHECK(s.ip[3] == 61 && s.mask[3] == 0 && s.gw[3] == 1);
    s = good(), strcpy(s.ip_str, "192.0.2.999");
    CHECK(!settings_check(&s));
    s = good(), s.ip_str[0] = 0;
    CHECK(!settings_check(&s));
    s = good(), strcpy(s.mask_str, "255.255.0");
    CHECK(!settings_check(&s));
    s = good(), strcpy(s.gw_str, "nope");
    CHECK(!settings_check(&s));
    s = good(), s.name[0] = 0;
    CHECK(!settings_check(&s));
    s = good(), s.wifi_ssid[0] = 0;
    CHECK(!settings_check(&s));
    s = good(), s.key_len = 31;
    CHECK(!settings_check(&s));
    // an open network has no password; a WPA one has 8 to 63 characters
    s = good(), s.wifi_pass[0] = 0;
    CHECK(settings_check(&s));
    s = good(), strcpy(s.wifi_pass, "short");
    CHECK(!settings_check(&s));
}
