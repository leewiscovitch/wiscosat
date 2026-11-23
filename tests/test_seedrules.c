#include "check.h"
#include "seedrules.h"

#define N61 "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"

void test_seedrules(void)
{
    const char *err;
    int bad;
    seed_file_t f[9];
    static const char *names[9] = {"a", "b", "c", "d", "e", "f", "g", "h", "i"};
    for (int i = 0; i < 9; i++) f[i] = (seed_file_t){names[i], 10, NULL};

    // none: a wipe; eight: the most
    CHECK(seedrules_check(f, 0, 1000, &err, &bad) == 0);
    CHECK(seedrules_check(f, 8, 1000, &err, &bad) == 0);

    // a ninth file: refused, and it is the one named
    bad = -1;
    CHECK(seedrules_check(f, 9, 1000, &err, &bad) == -1);
    CHECK(bad == 8);
    CHECK(err != NULL);

    // names: 60 characters is the longest; no folders, nothing FAT cannot hold
    seed_file_t one[2] = {{"user-data", 1, NULL}, {N61, 1, NULL}};
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
    seed_file_t dup[2] = {{"user-data", 1, NULL}, {"User-Data", 1, NULL}};
    bad = -1;
    CHECK(seedrules_check(dup, 2, 1000, &err, &bad) == -1 && bad == 1);

    // room: the file that does not fit is the one named
    seed_file_t big[3] = {{"a", 400, NULL}, {"b", 600, NULL}, {"c", 1, NULL}};
    bad = -1;
    CHECK(seedrules_check(big, 2, 1000, &err, &bad) == 0);
    CHECK(seedrules_check(big, 3, 1000, &err, &bad) == -1 && bad == 2);
    CHECK_HAS(err, "room");

    // the volume's label: a wiped seed is no cloud-init seed, so a host's NoCloud never takes it in place of the
    // real one on another drive (two CIDATA drives: which one is read is the device order's)
    CHECK(!strcmp(seedrules_label(0), "EMPTY"));
    CHECK(!strcmp(seedrules_label(1), "CIDATA"));
    CHECK(!strcmp(seedrules_label(SEED_MAX_FILES), "CIDATA"));

    // the raw bytes after a seed's line, split into its files in order; an empty file takes none
    seed_file_t set[4] = {{"a", 3, NULL}, {"e", 0, NULL}, {"b", 2, NULL}, {"c", 0, NULL}};
    int i;
    size_t left;
    CHECK(seedrules_at(set, 4, 0, &i, &left) == 0 && i == 0 && left == 3);
    CHECK(seedrules_at(set, 4, 2, &i, &left) == 0 && i == 0 && left == 1);
    CHECK(seedrules_at(set, 4, 3, &i, &left) == 0 && i == 2 && left == 2); // "e" has no bytes
    CHECK(seedrules_at(set, 4, 4, &i, &left) == 0 && i == 2 && left == 1);
    CHECK(seedrules_at(set, 4, 5, &i, &left) == -1); // past the last byte
    CHECK(seedrules_at(set, 0, 0, &i, &left) == -1);
}
