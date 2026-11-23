#include "check.h"
#include "seedrules.h"

#define N61 "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"

void test_seedrules(void)
{
    const char *err;
    int bad;
    seed_file_t f[9];
    static const char *names[9] = {"a", "b", "c", "d", "e", "f", "g", "h", "i"};
    for (int i = 0; i < 9; i++) f[i] = (seed_file_t){names[i], NULL, 10};

    // none: a wipe; eight: the most
    CHECK(seedrules_check(f, 0, 1000, &err, &bad) == 0);
    CHECK(seedrules_check(f, 8, 1000, &err, &bad) == 0);

    // a ninth file: refused, and it is the one named
    bad = -1;
    CHECK(seedrules_check(f, 9, 1000, &err, &bad) == -1);
    CHECK(bad == 8);
    CHECK(err != NULL);

    // names: 60 characters is the longest; no folders, nothing FAT cannot hold
    seed_file_t one[2] = {{"user-data", NULL, 1}, {N61, NULL, 1}};
    CHECK(strlen(N61) == 61);
    bad = -1;
    CHECK(seedrules_check(one, 2, 1000, &err, &bad) == -1 && bad == 1);
    CHECK_HAS(err, "60");
    one[1].name = N61 + 1;
    CHECK(seedrules_check(one, 2, 1000, &err, &bad) == 0);
    static const char *refused[] = {"a/b", "a\\b", "", ".", "..", "a:b", "a*", "a?", "a\"b", "a<b", "a|b", "a\nb",
                                    "trailing.", "trailing "};
    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        one[1].name = refused[i];
        bad = -1;
        CHECK(seedrules_check(one, 2, 1000, &err, &bad) == -1 && bad == 1);
    }

    // the same name twice (FAT does not tell case apart)
    seed_file_t dup[2] = {{"user-data", NULL, 1}, {"User-Data", NULL, 1}};
    bad = -1;
    CHECK(seedrules_check(dup, 2, 1000, &err, &bad) == -1 && bad == 1);

    // room: the file that does not fit is the one named
    seed_file_t big[3] = {{"a", NULL, 400}, {"b", NULL, 600}, {"c", NULL, 1}};
    bad = -1;
    CHECK(seedrules_check(big, 2, 1000, &err, &bad) == 0);
    CHECK(seedrules_check(big, 3, 1000, &err, &bad) == -1 && bad == 2);
    CHECK_HAS(err, "room");

    // the volume's label: a wiped seed is no cloud-init seed, so a host's NoCloud never takes it in place of the
    // real one on another drive (two CIDATA drives: which one is read is the device order's)
    CHECK(!strcmp(seedrules_label(0), "EMPTY"));
    CHECK(!strcmp(seedrules_label(1), "CIDATA"));
    CHECK(!strcmp(seedrules_label(SEED_MAX_FILES), "CIDATA"));
}
