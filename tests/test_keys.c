#include "check.h"
#include "keys.h"

static int report_is(const uint8_t r[8], uint8_t mod, uint8_t k0, uint8_t k1)
{
    const uint8_t want[8] = {mod, 0, k0, k1, 0, 0, 0, 0};
    return !memcmp(r, want, 8);
}

void test_keys(void)
{
    uint8_t r[8];
    const char *err = NULL;

    CHECK(keys_parse("f9", r, &err) == 0 && report_is(r, 0, 0x42, 0));
    CHECK(keys_parse("alt-sysrq-b", r, &err) == 0 && report_is(r, 0x04, 0x46, 0x05));
    CHECK(keys_parse("alt-printscreen-b", r, &err) == 0 && report_is(r, 0x04, 0x46, 0x05));
    CHECK(keys_parse("ctrl-alt-del", r, &err) == 0 && report_is(r, 0x05, 0x4C, 0));
    CHECK(keys_parse("A", r, &err) == 0 && report_is(r, 0x02, 0x04, 0));
    CHECK(keys_parse("-", r, &err) == 0 && report_is(r, 0, 0x2D, 0));
    CHECK(keys_parse("super", r, &err) == 0 && report_is(r, 0x08, 0, 0));
    CHECK(keys_parse("ctrl-a-b-c-d-e-f", r, &err) == 0);

    err = NULL;
    CHECK(keys_parse("ctrl-a-b-c-d-e-f-g", r, &err) == -1);
    CHECK_HAS(err, "more than six keys");
    err = NULL;
    CHECK(keys_parse("ctrl-fnord", r, &err) == -1);
    CHECK_HAS(err, "fnord");
    err = NULL;
    CHECK(keys_parse("", r, &err) == -1);
    CHECK(err != NULL);
    err = NULL;
    CHECK(keys_parse("ctrl--", r, &err) == -1);
    CHECK(err != NULL);

    CHECK(keys_type('a', r) == 0 && report_is(r, 0, 0x04, 0));
    CHECK(keys_type('Z', r) == 0 && report_is(r, 0x02, 0x1D, 0));
    CHECK(keys_type('0', r) == 0 && report_is(r, 0, 0x27, 0));
    CHECK(keys_type('!', r) == 0 && report_is(r, 0x02, 0x1E, 0));
    CHECK(keys_type(')', r) == 0 && report_is(r, 0x02, 0x27, 0));
    CHECK(keys_type('#', r) == 0 && report_is(r, 0x02, 0x20, 0));
    CHECK(keys_type('\n', r) == 0 && report_is(r, 0, 0x28, 0));
    CHECK(keys_type('\t', r) == 0 && report_is(r, 0, 0x2B, 0));
    CHECK(keys_type(' ', r) == 0 && report_is(r, 0, 0x2C, 0));
    CHECK(keys_type('~', r) == 0 && report_is(r, 0x02, 0x35, 0));
    CHECK(keys_type('`', r) == 0 && report_is(r, 0, 0x35, 0));
    CHECK(keys_type('|', r) == 0 && report_is(r, 0x02, 0x31, 0));
    CHECK(keys_type('\\', r) == 0 && report_is(r, 0, 0x31, 0));
    CHECK(keys_type('"', r) == 0 && report_is(r, 0x02, 0x34, 0));
    CHECK(keys_type('\'', r) == 0 && report_is(r, 0, 0x34, 0));
    CHECK(keys_type('?', r) == 0 && report_is(r, 0x02, 0x38, 0));
    CHECK(keys_type('_', r) == 0 && report_is(r, 0x02, 0x2D, 0));
    CHECK(keys_type('}', r) == 0 && report_is(r, 0x02, 0x30, 0));
    CHECK(keys_type((char)0xE9, r) == -1); // é in Latin-1; a byte of its UTF-8 form is just as foreign
    CHECK(keys_type('\x01', r) == -1);
    CHECK(keys_type('\0', r) == -1);

    // a tap while keys are held (press): the held ones stay in the tap's report and after it
    {
        const uint8_t ctrl[8] = {0x01, 0, 0, 0, 0, 0, 0, 0}, c[8] = {0, 0, 0x06, 0, 0, 0, 0, 0};
        uint8_t out[8];
        CHECK(keys_merge(ctrl, c, out) == 0 && report_is(out, 0x01, 0x06, 0));
        const uint8_t shift_a[8] = {0x02, 0, 0x04, 0, 0, 0, 0, 0}, alt_b[8] = {0x04, 0, 0x05, 0x04, 0, 0, 0, 0};
        CHECK(keys_merge(shift_a, alt_b, out) == 0 && report_is(out, 0x06, 0x04, 0x05));   // 'a' once, not twice
        const uint8_t none[8] = {0};
        CHECK(keys_merge(none, c, out) == 0 && report_is(out, 0, 0x06, 0));
        CHECK(keys_merge(ctrl, none, out) == 0 && report_is(out, 0x01, 0, 0));
        const uint8_t six[8] = {0, 0, 4, 5, 6, 7, 8, 9}, two[8] = {0x01, 0, 9, 10, 0, 0, 0, 0};
        CHECK(keys_merge(six, two, out) == 1);              // 10 has no room: counted, the held six kept
        CHECK(!memcmp(out, (uint8_t[8]){0x01, 0, 4, 5, 6, 7, 8, 9}, 8));
        CHECK(keys_merge(six, six, out) == 0);
    }
}
