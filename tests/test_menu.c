#include "check.h"
#include "menu.h"

static menu_t m;

static const char *bad(const char *text)
{
    static char err[MENU_ERR_MAX];
    return menu_parse(text, &m, err, sizeof(err)) ? err : "(taken)";
}

void test_menu(void)
{
    char err[MENU_ERR_MAX];
    menu_step_t s;

    // the built-in list is a text like any other: 18 entries, F1 first
    CHECK(menu_parse(menu_builtin, &m, err, sizeof(err)) == 0);
    CHECK(m.n == 18);
    CHECK(!strcmp(m.e[0].label, "F1") && m.e[0].kind == MENU_ARM && !strcmp(m.e[0].keys, "f1"));
    CHECK(!strcmp(m.e[15].label, "ctrl-alt-del") && m.e[15].kind == MENU_SEND);
    CHECK(!strcmp(m.e[16].label, "Restart SysRq") && m.e[16].kind == MENU_HOLD);

    // its restart: s, 2 s, u, 2 s, b
    CHECK(menu_step(m.e[16].keys, 0, &s) == 1 && !s.wait && !strcmp(s.name, "alt-sysrq-s") && s.wait_ms == 0);
    CHECK(s.report[0] == 0x04 && s.report[2] == 0x46 && s.report[3] == 0x16);
    CHECK(menu_step(m.e[16].keys, 1, &s) == 1 && s.wait && s.wait_ms == 2000);
    CHECK(menu_step(m.e[16].keys, 2, &s) == 1 && !s.wait && !strcmp(s.name, "alt-sysrq-u") && s.wait_ms == 0);
    CHECK(menu_step(m.e[16].keys, 4, &s) == 1 && !strcmp(s.name, "alt-sysrq-b"));
    CHECK(menu_step(m.e[16].keys, 5, &s) == 0);

    // comments, empty lines, blanks and Windows line ends; two keys with no wait are 100 ms apart
    CHECK(menu_parse("# mine\r\n\r\n  Boot menu|arm|  f12 \r\n\tDown twice | send | down down enter\r\n", &m, err, sizeof(err)) == 0);
    CHECK(m.n == 2 && !strcmp(m.e[0].label, "Boot menu") && !strcmp(m.e[0].keys, "f12"));
    CHECK(menu_step(m.e[1].keys, 1, &s) == 1 && !s.wait && s.wait_ms == MENU_GAP_MS && !strcmp(s.name, "down"));
    CHECK(menu_step(m.e[1].keys, 2, &s) == 1 && !strcmp(s.name, "enter"));
    // the last line needs no newline; a | among the keys is the key
    CHECK(menu_parse("Pipe | send | |", &m, err, sizeof(err)) == 0 && m.n == 1 && !strcmp(m.e[0].keys, "|"));

    // refused, with the line and the part
    CHECK_HAS(bad("F2 | arm | f2\nF99 | arm | f99\n"), "menu line 2");
    CHECK_HAS(bad("F2 | arm | f2\nF99 | arm | f99\n"), "\"f99\"");
    CHECK_HAS(bad("F2 arm f2\n"), "<label> | <what> | <keys>");
    CHECK_HAS(bad(" | arm | f2\n"), "label");
    CHECK_HAS(bad("A label of 21 chars.. | arm | f2\n"), "label");
    CHECK_HAS(bad("F2! | arm | f2\n"), "font");
    CHECK_HAS(bad("F2 | type | f2\n"), "arm, send or hold");
    CHECK_HAS(bad("F2 | arm | f2 f3\n"), "one key");
    CHECK_HAS(bad("F2 | arm | 100ms f2\n"), "one key");
    CHECK_HAS(bad("F2 | send | \n"), "no key");
    CHECK_HAS(bad("F2 | send | 100ms\n"), "no key");
    CHECK_HAS(bad("F2 | send | f2 100ms\n"), "wait at the end");
    CHECK_HAS(bad("F2 | send | f2 10001ms f3\n"), "10001ms");
    CHECK_HAS(bad("F2 | send | a b c d e f g h i j k l m\n"), "over 12 keys");
    CHECK_HAS(bad("# only a comment\n\n"), "no entry");
    CHECK_HAS(bad("F2 | arm | f2\x01\n"), "ASCII");

    // 40 entries are taken, 41 are not; 4096 bytes are the limit
    char many[41 * 16 + 1] = "", big[MENU_TEXT_MAX + 2];
    for (int i = 0; i < 40; i++) strcat(many, "K | arm | f1\n");
    CHECK(menu_parse(many, &m, err, sizeof(err)) == 0 && m.n == 40);
    strcat(many, "K | arm | f1\n");
    CHECK_HAS(bad(many), "over 40 entries");
    memset(big, '\n', sizeof(big) - 1);
    big[sizeof(big) - 1] = 0;
    CHECK_HAS(bad(big), "4096");
}
