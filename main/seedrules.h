// What a seed command may hold: at most eight files, each name a plain FAT file name of up to 60 characters, all
// of them together within the room on the volume. Pure, tested on Linux.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define SEED_MAX_FILES 8
#define SEED_NAME_MAX 60

typedef struct {
    const char *name;
    const uint8_t *data;
    size_t size;
} seed_file_t;

// 0 when the whole set may be written (none: the volume is wiped). -1 when not, with *err and *bad, the index of
// the file it is about; the set is then refused whole
int seedrules_check(const seed_file_t *files, int n, size_t room, const char **err, int *bad);
