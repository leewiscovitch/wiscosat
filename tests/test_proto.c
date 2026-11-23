#include <stdlib.h>
#include "check.h"
#include "cJSON.h"
#include "proto.h"

// parse a line; NULL when it parsed, else the error (c is freed by the caller on success)
static const char *parse(const char *line, cmd_t *c)
{
    const char *err = NULL;
    int r = proto_parse(line, strlen(line), c, &err);
    return r == 0 ? NULL : (err ? err : "(no error text)");
}

static const char *parse_free(const char *line)
{
    cmd_t c;
    const char *err = parse(line, &c);
    if (!err) proto_free(&c);
    return err;
}

static void test_lines(void)
{
    cmd_t c;
    const char *err;

    // a line of 16385 bytes is too long; 16384 is not (it is just not JSON)
    char *line = malloc(PROTO_LINE_MAX + 2);
    memset(line, 'x', PROTO_LINE_MAX + 1);
    line[PROTO_LINE_MAX + 1] = 0;
    err = NULL;
    CHECK(proto_parse(line, PROTO_LINE_MAX + 1, &c, &err) == -1);
    CHECK_HAS(err, "line too long");
    line[PROTO_LINE_MAX] = 0;
    err = NULL;
    CHECK(proto_parse(line, PROTO_LINE_MAX, &c, &err) == -1);
    CHECK(err && !strstr(err, "line too long"));
    free(line);

    CHECK_HAS(parse_free("nope"), "not a JSON object");
    CHECK_HAS(parse_free("[1,2]"), "not a JSON object");
    CHECK_HAS(parse_free("{}"), "no cmd");
    CHECK_HAS(parse_free("{\"cmd\":\"bootloader\"}"), "unknown command");
    CHECK_HAS(parse_free("{\"cmd\":7}"), "no cmd");

    // the trials' token: the channel's key replaced it; an old client's field is ignored
    CHECK(parse("{\"cmd\":\"status\",\"token\":\"anything\"}", &c) == NULL);
    CHECK(c.kind == CMD_STATUS);
    proto_free(&c);

    static const struct {
        const char *line;
        cmd_kind_t kind;
    } kinds[] = {
        {"{\"cmd\":\"status\"}", CMD_STATUS},
        {"{\"cmd\":\"disarm\"}", CMD_DISARM},
        {"{\"cmd\":\"press\",\"name\":\"shift\"}", CMD_PRESS},
        {"{\"cmd\":\"release\"}", CMD_RELEASE},
        {"{\"cmd\":\"type\",\"text\":\"hi\"}", CMD_TYPE},
        {"{\"cmd\":\"seed\"}", CMD_SEED},
        {"{\"cmd\":\"restart\"}", CMD_RESTART},
        {"{\"cmd\":\"identify\"}", CMD_IDENTIFY},
    };
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
        err = parse(kinds[i].line, &c);
        CHECK(err == NULL);
        if (!err) {
            CHECK(c.kind == kinds[i].kind);
            proto_free(&c);
        }
    }
}

static void test_key(void)
{
    cmd_t c;
    static const uint8_t f9[8] = {0, 0, 0x42};

    CHECK(parse("{\"cmd\":\"key\",\"name\":\"f9\"}", &c) == NULL);
    CHECK(c.kind == CMD_KEY && c.key.when == WHEN_NOW && !memcmp(c.key.report, f9, 8));
    CHECK(!strcmp(c.key.name, "f9"));
    proto_free(&c);

    // armed: the defaults when the command gives none
    CHECK(parse("{\"cmd\":\"key\",\"name\":\"f9\",\"when\":\"configured\"}", &c) == NULL);
    CHECK(c.key.when == WHEN_CONFIGURED && c.key.every_ms == 100 && c.key.for_ms == 3000 && !c.key.persist);
    proto_free(&c);
    CHECK(parse("{\"cmd\":\"key\",\"name\":\"f9\",\"when\":\"configured\",\"every_ms\":20,\"for_ms\":30000,"
                "\"persist\":true}", &c) == NULL);
    CHECK(c.key.every_ms == 20 && c.key.for_ms == 30000 && c.key.persist);
    proto_free(&c);
    CHECK(parse("{\"cmd\":\"key\",\"name\":\"f9\",\"when\":\"configured\",\"every_ms\":5000,\"for_ms\":0}", &c) == NULL);
    proto_free(&c);

    CHECK_HAS(parse_free("{\"cmd\":\"key\",\"name\":\"f9\",\"when\":\"configured\",\"every_ms\":19}"), "every_ms");
    CHECK_HAS(parse_free("{\"cmd\":\"key\",\"name\":\"f9\",\"when\":\"configured\",\"every_ms\":5001}"), "every_ms");
    CHECK_HAS(parse_free("{\"cmd\":\"key\",\"name\":\"f9\",\"when\":\"configured\",\"for_ms\":30001}"), "for_ms");
    CHECK_HAS(parse_free("{\"cmd\":\"key\",\"name\":\"f9\",\"when\":\"configured\",\"for_ms\":-1}"), "for_ms");
    CHECK_HAS(parse_free("{\"cmd\":\"key\",\"name\":\"f9\",\"when\":\"configured\",\"for_ms\":\"3000\"}"), "for_ms");
    CHECK_HAS(parse_free("{\"cmd\":\"key\",\"name\":\"f9\",\"when\":\"configured\",\"every_ms\":100.5}"), "every_ms");
    CHECK_HAS(parse_free("{\"cmd\":\"key\",\"name\":\"f9\",\"when\":\"later\"}"), "when");
    CHECK_HAS(parse_free("{\"cmd\":\"key\"}"), "no name");
    CHECK_HAS(parse_free("{\"cmd\":\"key\",\"name\":\"ctrl-fnord\"}"), "fnord");
    CHECK_HAS(parse_free("{\"cmd\":\"press\"}"), "no name");

    // release with no name: everything up
    CHECK(parse("{\"cmd\":\"release\"}", &c) == NULL);
    CHECK(c.key.name == NULL);
    proto_free(&c);
    CHECK(parse("{\"cmd\":\"release\",\"name\":\"alt\"}", &c) == NULL);
    CHECK(c.key.name && c.key.report[0] == 0x04);
    proto_free(&c);

    // type: refused whole, before anything is typed, when a character has no key
    CHECK(parse("{\"cmd\":\"type\",\"text\":\"root\\n\"}", &c) == NULL);
    CHECK(!strcmp(c.type.text, "root\n"));
    proto_free(&c);
    CHECK_HAS(parse_free("{\"cmd\":\"type\",\"text\":\"caf\xc3\xa9\"}"), "US layout");
    CHECK_HAS(parse_free("{\"cmd\":\"type\"}"), "no text");
}

static void test_seed(void)
{
    cmd_t c;
    const char *sha = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    char line[1024];

    // the files are named with their sizes and SHA-256s; their bytes follow the line raw, in this order (as an
    // update's do), so a seed is not held to one line
    snprintf(line, sizeof(line),
             "{\"cmd\":\"seed\",\"files\":[{\"name\":\"user-data\",\"size\":600000,\"sha256\":\"%s\"},"
             "{\"name\":\"meta-data\",\"size\":0,\"sha256\":\"%s\"}]}", sha, sha);
    CHECK(parse(line, &c) == NULL);
    CHECK(c.seed.n == 2 && c.seed.hold_ms == 2000 && c.seed.total == 600000);
    CHECK(!strcmp(c.seed.files[0].name, "user-data") && c.seed.files[0].size == 600000);
    CHECK(!strcmp(c.seed.files[0].sha256, sha) && c.seed.files[1].size == 0);
    proto_free(&c);

    // no files, or none in it: a wipe (nothing follows the line)
    CHECK(parse("{\"cmd\":\"seed\"}", &c) == NULL);
    CHECK(c.seed.n == 0 && c.seed.total == 0);
    proto_free(&c);
    CHECK(parse("{\"cmd\":\"seed\",\"files\":[],\"hold_ms\":0}", &c) == NULL);
    CHECK(c.seed.n == 0 && c.seed.hold_ms == 0);
    proto_free(&c);

    // parsing does not judge names or counts (seedrules does, and names the file): nine files pass here
    char *p = line + snprintf(line, sizeof(line), "{\"cmd\":\"seed\",\"files\":[");
    for (int i = 1; i <= 9; i++)
        p += snprintf(p, line + sizeof(line) - p, "%s{\"name\":\"%d\",\"size\":1,\"sha256\":\"%s\"}", i > 1 ? "," : "",
                      i, sha);
    snprintf(p, line + sizeof(line) - p, "]}");
    CHECK(parse(line, &c) == NULL);
    CHECK(c.seed.n == 9 && c.seed.total == 9);
    proto_free(&c);

    // the old form (contents in the line) is refused, not half understood
    CHECK_HAS(parse_free("{\"cmd\":\"seed\",\"files\":{\"a\":\"x\"}}"), "files");
    CHECK_HAS(parse_free("{\"cmd\":\"seed\",\"files\":[\"a\"]}"), "file 1");
    CHECK_HAS(parse_free("{\"cmd\":\"seed\",\"files\":[{\"size\":1}]}"), "name");
    snprintf(line, sizeof(line), "{\"cmd\":\"seed\",\"files\":[{\"name\":\"a\",\"sha256\":\"%s\"}]}", sha);
    CHECK_HAS(parse_free(line), "size");
    snprintf(line, sizeof(line), "{\"cmd\":\"seed\",\"files\":[{\"name\":\"a\",\"size\":-1,\"sha256\":\"%s\"}]}", sha);
    CHECK_HAS(parse_free(line), "size");
    CHECK_HAS(parse_free("{\"cmd\":\"seed\",\"files\":[{\"name\":\"a\",\"size\":1,\"sha256\":\"ABC\"}]}"), "sha256");
    // a file over the whole volume is refused here; what fits is seedrules' to say
    snprintf(line, sizeof(line), "{\"cmd\":\"seed\",\"files\":[{\"name\":\"a\",\"size\":1048577,\"sha256\":\"%s\"}]}",
             sha);
    CHECK_HAS(parse_free(line), "size");
    CHECK_HAS(parse_free("{\"cmd\":\"seed\",\"files\":[],\"hold_ms\":10001}"), "hold_ms");
}

static void test_update(void)
{
    cmd_t c;
    const char *sha = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    char line[256];

    snprintf(line, sizeof(line), "{\"cmd\":\"update\",\"size\":872400,\"sha256\":\"%s\"}", sha);
    CHECK(parse(line, &c) == NULL);
    CHECK(c.kind == CMD_UPDATE && c.update.size == 872400 && !strcmp(c.update.sha256, sha));
    proto_free(&c);
    snprintf(line, sizeof(line), "{\"cmd\":\"update\",\"size\":0,\"sha256\":\"%s\"}", sha);
    CHECK_HAS(parse_free(line), "size");
    CHECK_HAS(parse_free("{\"cmd\":\"update\",\"size\":10,\"sha256\":\"abc\"}"), "sha256");
    CHECK_HAS(parse_free("{\"cmd\":\"update\",\"size\":10}"), "sha256");
}

// Review Focus 2: a key now with no host is refused at once, "no host"; armed it waits for one
static void test_check(void)
{
    cmd_t c;
    const host_t none = {.configured = false}, there = {.configured = true}, asleep = {true, true, false},
                 firing = {.configured = true, .firing = true};

    CHECK(parse("{\"cmd\":\"key\",\"name\":\"f9\"}", &c) == NULL);
    CHECK_HAS(proto_check(&c, &none), "no host");
    CHECK(proto_check(&c, &there) == NULL);
    CHECK(proto_check(&c, &asleep) != NULL);
    proto_free(&c);

    CHECK(parse("{\"cmd\":\"key\",\"name\":\"f9\",\"when\":\"configured\"}", &c) == NULL);
    CHECK(proto_check(&c, &none) == NULL);
    CHECK_HAS(proto_check(&c, &firing), "firing");
    proto_free(&c);

    static const char *need_host[] = {"{\"cmd\":\"type\",\"text\":\"a\"}", "{\"cmd\":\"press\",\"name\":\"a\"}",
                                      "{\"cmd\":\"release\"}"};
    for (size_t i = 0; i < 3; i++) {
        CHECK(parse(need_host[i], &c) == NULL);
        CHECK_HAS(proto_check(&c, &none), "no host");
        proto_free(&c);
    }

    CHECK(parse("{\"cmd\":\"status\"}", &c) == NULL);
    CHECK(proto_check(&c, &none) == NULL);
    proto_free(&c);
    CHECK(parse("{\"cmd\":\"seed\"}", &c) == NULL);
    CHECK(proto_check(&c, &none) == NULL);
    CHECK_HAS(proto_check(&c, &firing), "firing");
    proto_free(&c);
}

static void test_answer(void)
{
    char *a = proto_answer("no host", NULL);
    CHECK(a && !strcmp(a, "{\"ok\":false,\"error\":\"no host\"}"));
    free(a);
    cJSON *s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "target", "listening");
    a = proto_answer(NULL, s);
    CHECK(a && !strcmp(a, "{\"ok\":true,\"state\":{\"target\":\"listening\"}}"));
    free(a);
    // an error with a quote in it stays one valid line
    a = proto_answer("seed: file \"a\nb\": bad name", NULL);
    CHECK(a && !strchr(a, '\n'));
    cJSON *back = cJSON_Parse(a);
    CHECK(back && cJSON_IsFalse(cJSON_GetObjectItem(back, "ok")));
    cJSON_Delete(back);
    free(a);
}

// the dev build (./build --dev, SAT_DEV): download exists only there; state.dev says which build answers
static void test_dev(void)
{
    cmd_t c;
#ifdef SAT_DEV
    CHECK(parse("{\"cmd\":\"download\"}", &c) == NULL);
    CHECK(c.kind == CMD_DOWNLOAD);
    proto_free(&c);
#else
    const char *nonsense = parse_free("{\"cmd\":\"nonsense\"}");
    const char *err = parse("{\"cmd\":\"download\"}", &c);
    CHECK(err && nonsense && !strcmp(err, nonsense)); // a release build: as any unknown command
#endif

    cJSON *s = cJSON_CreateObject();
    proto_add_build(s, "abc1234");
    CHECK_HAS(cJSON_GetStringValue(cJSON_GetObjectItem(s, "version")), "abc1234");
#ifdef SAT_DEV
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(s, "dev")));
#else
    CHECK(cJSON_IsFalse(cJSON_GetObjectItem(s, "dev")));
#endif
    cJSON_Delete(s);
}

void test_proto(void)
{
    test_lines();
    test_key();
    test_seed();
    test_update();
    test_check();
    test_answer();
    test_dev();
}
