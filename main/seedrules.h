// What a seed command may hold, and where its raw bytes go: at most eight files, each name a plain FAT file name of up to 60 characters, all
// of them together within the room on the volume. Pure, tested on Linux.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define SEED_MAX_FILES 8
#define SEED_NAME_MAX 60

#define SEED_BYTES (1024 * 1024) // the volume; no file can be larger (what fits is seedrules_check's to say)

typedef struct {
    const char *name;
    size_t size;
    const char *sha256; // 64 lower-case hex, the file's as the client sent it
} seed_file_t;

// 0 when the whole set may be written (none: the volume is wiped). -1 when not, with *err and *bad, the index of
// the file it is about; the set is then refused whole
int seedrules_check(const seed_file_t *files, int n, size_t room, const char **err, int *bad);

// The volume's label for a seed of n files: "CIDATA", or "EMPTY" when it has none (a wiped seed is no cloud-init
// seed: a host's NoCloud must not take it in place of a real CIDATA drive elsewhere)
const char *seedrules_label(int n);

// The seed's bytes follow its line raw, the files one after another in order. For the byte at pos of that stream:
// 0 with *file (its index) and *left (the bytes of that file from pos on); -1 past the last byte
int seedrules_at(const seed_file_t *files, int n, size_t pos, int *file, size_t *left);
