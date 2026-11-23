// A few checks for the Linux tests: no framework, one binary, a line per failure.
#pragma once
#include <stdio.h>
#include <string.h>

extern int check_run, check_failed;

#define CHECK(c)                                                                     \
    do {                                                                             \
        check_run++;                                                                 \
        if (!(c)) {                                                                  \
            check_failed++;                                                          \
            fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #c);          \
        }                                                                            \
    } while (0)

// the string s exists and holds part
#define CHECK_HAS(s, part)                                                           \
    do {                                                                             \
        const char *s_ = (s);                                                        \
        check_run++;                                                                 \
        if (!s_ || !strstr(s_, (part))) {                                            \
            check_failed++;                                                          \
            fprintf(stderr, "%s:%d: failed: \"%s\" does not hold \"%s\"\n", __FILE__, \
                    __LINE__, s_ ? s_ : "(null)", (part));                           \
        }                                                                            \
    } while (0)

void test_keys(void);
void test_armed(void);
void test_proto(void);
void test_seedrules(void);
void test_updrules(void);
void test_lockout(void);
void test_settings(void);
void test_usbserial(void);
void test_wlsplit(void);
