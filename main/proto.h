// The channel's lines: one JSON object per line in, a command struct out; and the answer line back. Pure (cJSON
// is a plain C library; no ESP-IDF in it), tested on Linux.
//   in:  {"cmd":"key","name":"f9","when":"configured","every_ms":100,"for_ms":3000,"persist":false}
// seed and update are followed by raw bytes: the unit answers {"ok":true,"ready":true}, the client sends exactly the
// bytes the line named (update: size; seed: the files' sizes, one after another), the unit answers the final line
//   out: {"ok":true,"state":{...}}  or  {"ok":false,"error":"..."}
// SAT_DEV (a dev build, ./build --dev; Kconfig SAT_DEV) adds the command download; a release build has no trace of it.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "seedrules.h"

struct cJSON;

#define PROTO_LINE_MAX 16384 // bytes in a line, the newline not counted
#define PROTO_LINE_TOO_LONG "{\"ok\":false,\"error\":\"line too long\"}"
#define PROTO_SEED_HOLD_MS_DEFAULT 2000
#define PROTO_SEED_HOLD_MS_MAX 10000

typedef enum {
    CMD_STATUS,
    CMD_KEY,
    CMD_DISARM,
    CMD_PRESS,
    CMD_RELEASE,
    CMD_TYPE,
    CMD_SEED,
    CMD_UPDATE,
    CMD_RESTART,
    CMD_IDENTIFY,
    CMD_MENU,
#ifdef SAT_DEV
    CMD_DOWNLOAD, // a dev build only (./build --dev): restart into the ROM's download mode, for esptool
#endif
} cmd_kind_t;

typedef enum { WHEN_NOW, WHEN_CONFIGURED } when_t;

typedef struct {
    cmd_kind_t kind;
    struct {           // key, press, release
        const char *name; // NULL only for a release of everything
        uint8_t report[8];
        when_t when;                // key only
        uint32_t every_ms, for_ms;  // key when configured: the armed key's schedule (defaults filled in)
        bool persist;               // key when configured: kept in flash until it has fired
        bool sticky;                // key when configured: kept (in flash too) and sent at every start until disarmed
    } key;
    struct {
        const char *text; // every character checked to have a key on the US layout
    } type;
    struct {
        seed_file_t *files; // names, sizes and SHA-256s as given (seedrules judges them); the bytes follow the line
        int n;              // 0: wipe the drive (nothing follows the line)
        uint32_t total;     // the raw bytes that follow the line: all the files' sizes
        uint32_t hold_ms;   // how long the host sees no medium before the files change
    } seed;
    struct {
        uint32_t size;
        char sha256[65]; // lower-case hex
    } update;
    struct {
        const char *text; // NULL: only read the menu back; "": back to the built-in list; else the new text (menu.h)
    } menu;
    void *json_; // the parsed line the strings above point into; proto_free lets it go
} cmd_t;

// 0 and *out filled (free it with proto_free), or -1 and *err; len is the line's length without its newline
int proto_parse(const char *line, size_t len, cmd_t *out, const char **err);
void proto_free(cmd_t *c);

// the host as the unit sees it when a command comes
typedef struct {
    bool configured; // a host has configured the USB device (there is a host, and it is not off)
    bool suspended;  // that host has suspended the bus (asleep)
    bool firing;     // an armed key is being sent now
} host_t;

// may the command run now? NULL yes; else the error to answer. A key now (and press, release, type) with no host is
// refused at once, "no host": nothing is queued unless "when":"configured" was asked
const char *proto_check(const cmd_t *c, const host_t *h);

// the build into state: "version" (the image's, as built) and "dev" (true in a dev build, which knows download)
void proto_add_build(struct cJSON *state, const char *version);

// the answer line (no newline; free() it): ok with state when err is NULL, else the error. Takes state (may be NULL)
char *proto_answer(const char *err, struct cJSON *state);
