// What a seed command may hold (seedrules.h).
#include <string.h>
#include <strings.h>
#include "seedrules.h"

static const char *name_problem(const char *name)
{
    size_t len = strlen(name);
    if (!len) return "empty file name";
    if (len > SEED_NAME_MAX) return "file name longer than 60 characters";
    for (const char *p = name; *p; p++)
        if ((unsigned char)*p < 0x20 || strchr("/\\:*?\"<>|", *p)) return "file name with a character FAT has no room for (or a folder)";
    // FAT drops a trailing dot or space, so the name read back would not be the name written
    if (name[len - 1] == '.' || name[len - 1] == ' ') return "file name ends in a dot or a space";
    return NULL;
}

int seedrules_check(const seed_file_t *files, int n, size_t room, const char **err, int *bad)
{
    size_t total = 0;
    for (int i = 0; i < n; i++) {
        const char *e = i == SEED_MAX_FILES ? "more than 8 files" : name_problem(files[i].name);
        for (int j = 0; j < i && !e; j++)
            if (!strcasecmp(files[i].name, files[j].name)) e = "the same file name twice (FAT does not tell case apart)";
        if (!e && files[i].size > room - total) e = "not enough room on the seed drive";
        if (!e) total += files[i].size;
        if (e) {
            *err = e;
            *bad = i;
            return -1;
        }
    }
    return 0;
}

const char *seedrules_label(int n)
{
    return n > 0 ? "CIDATA" : "EMPTY";
}
