// The channel's lines (proto.h).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "armed.h"
#include "keys.h"
#include "proto.h"

static _Thread_local char msg[KEYS_NAME_MAX + 64]; // an error that names something

static const struct {
    const char *name;
    cmd_kind_t kind;
} commands[] = {
    {"status", CMD_STATUS}, {"key", CMD_KEY},   {"disarm", CMD_DISARM}, {"press", CMD_PRESS},
    {"release", CMD_RELEASE}, {"type", CMD_TYPE}, {"seed", CMD_SEED},     {"update", CMD_UPDATE},
    {"restart", CMD_RESTART}, {"identify", CMD_IDENTIFY},
#ifdef SAT_DEV
    {"download", CMD_DOWNLOAD},
#endif
};

// a whole number field in [lo, hi]; dflt when absent. NULL, or the error
static const char *whole(const cJSON *req, const char *k, double lo, double hi, uint32_t dflt, uint32_t *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(req, k);
    if (!v) {
        *out = dflt;
        return NULL;
    }
    double d = cJSON_IsNumber(v) ? v->valuedouble : NAN;
    if (!(d >= lo && d <= hi) || d != floor(d)) {
        snprintf(msg, sizeof(msg), "%s must be a whole number %.0f..%.0f", k, lo, hi);
        return msg;
    }
    *out = (uint32_t)d;
    return NULL;
}

static const char *str(const cJSON *req, const char *k)
{
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(req, k));
}

static int b64_value(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

// standard base64 with its padding; into out (room for len / 4 * 3). The decoded length, or -1
static long b64_decode(const char *in, size_t len, uint8_t *out)
{
    size_t n = 0;
    if (len % 4) return -1;
    for (size_t i = 0; i < len; i += 4) {
        int v[4], pad = 0;
        for (int j = 0; j < 4; j++) {
            unsigned char c = in[i + j];
            if (c == '=' && i + 4 == len && j >= 2 && (j == 3 || in[i + 3] == '=')) {
                v[j] = 0;
                pad++;
            } else if ((v[j] = b64_value(c)) < 0) return -1;
        }
        uint32_t w = (uint32_t)v[0] << 18 | (uint32_t)v[1] << 12 | (uint32_t)v[2] << 6 | (uint32_t)v[3];
        out[n++] = w >> 16;
        if (pad < 2) out[n++] = (w >> 8) & 0xFF;
        if (pad < 1) out[n++] = w & 0xFF;
    }
    return (long)n;
}

static const char *parse_key(const cJSON *req, cmd_t *c)
{
    c->key.name = str(req, "name");
    if (!c->key.name) return c->kind == CMD_RELEASE ? NULL : "no name";
    const char *err = NULL;
    if (keys_parse(c->key.name, c->key.report, &err)) return err;
    if (c->kind != CMD_KEY) return NULL;

    const char *when = str(req, "when");
    if (!when || !strcmp(when, "now")) c->key.when = WHEN_NOW;
    else if (!strcmp(when, "configured")) c->key.when = WHEN_CONFIGURED;
    else return "when must be now or configured";
    if ((err = whole(req, "every_ms", ARMED_EVERY_MS_MIN, ARMED_EVERY_MS_MAX, ARMED_EVERY_MS_DEFAULT, &c->key.every_ms)))
        return err;
    if ((err = whole(req, "for_ms", 0, ARMED_FOR_MS_MAX, ARMED_FOR_MS_DEFAULT, &c->key.for_ms))) return err;
    c->key.persist = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(req, "persist"));
    return NULL;
}

static const char *parse_type(const cJSON *req, cmd_t *c)
{
    uint8_t r[8];
    c->type.text = str(req, "text");
    if (!c->type.text) return "no text";
    for (const char *p = c->type.text; *p; p++)
        if (keys_type(*p, r)) {
            // the position only: typed text is never echoed (it may be a password)
            snprintf(msg, sizeof(msg), "character %d of text is not on the US layout", (int)(p - c->type.text) + 1);
            return msg;
        }
    return NULL;
}

static const char *parse_seed(const cJSON *req, cmd_t *c)
{
    const char *enc = str(req, "encoding");
    const cJSON *files = cJSON_GetObjectItemCaseSensitive(req, "files"), *f;
    bool b64 = enc && !strcmp(enc, "base64");
    if (enc && !b64 && strcmp(enc, "text")) return "encoding must be text or base64";
    if (files && !cJSON_IsObject(files)) return "files must be an object of name: content";
    const char *err = whole(req, "hold_ms", 0, PROTO_SEED_HOLD_MS_MAX, PROTO_SEED_HOLD_MS_DEFAULT, &c->seed.hold_ms);
    if (err) return err;
    int n = files ? cJSON_GetArraySize(files) : 0;
    if (!n) return NULL; // a wipe
    if (!(c->seed.files = calloc(n, sizeof(seed_file_t)))) return "out of memory";
    cJSON_ArrayForEach(f, files)
    {
        const char *txt = cJSON_GetStringValue(f);
        if (!txt) {
            snprintf(msg, sizeof(msg), "file %d: content must be a string", c->seed.n + 1);
            return msg;
        }
        size_t tl = strlen(txt);
        uint8_t *data = malloc(tl + 1); // base64 only ever shrinks
        if (!data) return "out of memory";
        seed_file_t *sf = &c->seed.files[c->seed.n++];
        sf->name = f->string;
        sf->data = data;
        if (!b64) {
            memcpy(data, txt, tl);
            sf->size = tl;
            continue;
        }
        long len = b64_decode(txt, tl, data);
        if (len < 0) {
            snprintf(msg, sizeof(msg), "file %d: bad base64", c->seed.n);
            return msg;
        }
        sf->size = (size_t)len;
    }
    return NULL;
}

static const char *parse_update(const cJSON *req, cmd_t *c)
{
    const char *sha = str(req, "sha256");
    const char *err = whole(req, "size", 1, 0xFFFFFFFFu, 0, &c->update.size);
    if (err) return err;
    if (!cJSON_GetObjectItemCaseSensitive(req, "size")) return "no size";
    if (!sha || strlen(sha) != 64 || strspn(sha, "0123456789abcdef") != 64)
        return "sha256 must be 64 lower-case hex characters";
    memcpy(c->update.sha256, sha, 65);
    return NULL;
}

int proto_parse(const char *line, size_t len, cmd_t *out, const char **err)
{
    memset(out, 0, sizeof(*out));
    if (len > PROTO_LINE_MAX) {
        *err = "line too long";
        return -1;
    }
    cJSON *req = cJSON_ParseWithLength(line, len);
    out->json_ = req;
    const char *e = NULL, *cmd;
    if (!cJSON_IsObject(req)) e = "not a JSON object";
    else if (!(cmd = str(req, "cmd"))) e = "no cmd";
    else {
        e = "unknown command";
        for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++)
            if (!strcmp(cmd, commands[i].name)) {
                out->kind = commands[i].kind;
                e = NULL;
            }
    }
    if (!e) {
        switch (out->kind) {
        case CMD_KEY:
        case CMD_PRESS:
        case CMD_RELEASE: e = parse_key(req, out); break;
        case CMD_TYPE: e = parse_type(req, out); break;
        case CMD_SEED: e = parse_seed(req, out); break;
        case CMD_UPDATE: e = parse_update(req, out); break;
        default: break;
        }
    }
    if (e) {
        proto_free(out);
        *err = e;
        return -1;
    }
    return 0;
}

void proto_free(cmd_t *c)
{
    for (int i = 0; i < c->seed.n; i++) free((void *)c->seed.files[i].data);
    free(c->seed.files);
    cJSON_Delete(c->json_);
    memset(c, 0, sizeof(*c));
}

const char *proto_check(const cmd_t *c, const host_t *h)
{
    bool needs_host = c->kind == CMD_PRESS || c->kind == CMD_RELEASE || c->kind == CMD_TYPE ||
                      (c->kind == CMD_KEY && c->key.when == WHEN_NOW);
    if (needs_host && !h->configured) return "no host";
    if (needs_host && h->suspended) return "host asleep (USB suspended)";
    if (h->firing && (c->kind == CMD_SEED || (c->kind == CMD_KEY && c->key.when == WHEN_CONFIGURED)))
        return "an armed key is firing now";
    return NULL;
}

void proto_add_build(cJSON *state, const char *version)
{
    cJSON_AddStringToObject(state, "version", version);
#ifdef SAT_DEV
    cJSON_AddBoolToObject(state, "dev", true);
#else
    cJSON_AddBoolToObject(state, "dev", false);
#endif
}

char *proto_answer(const char *err, struct cJSON *state)
{
    cJSON *a = cJSON_CreateObject();
    cJSON_AddBoolToObject(a, "ok", err == NULL);
    if (err) {
        cJSON_AddStringToObject(a, "error", err);
        cJSON_Delete(state);
    } else if (state) cJSON_AddItemToObject(a, "state", state);
    char *out = cJSON_PrintUnformatted(a);
    cJSON_Delete(a);
    return out;
}
