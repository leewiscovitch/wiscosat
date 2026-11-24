// The knob's menu as a text (menu.h).
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "menu.h"

const char menu_builtin[] =
    "F1 | arm | f1\nF2 | arm | f2\nF3 | arm | f3\nF4 | arm | f4\nF5 | arm | f5\nF6 | arm | f6\n"
    "F7 | arm | f7\nF8 | arm | f8\nF9 | arm | f9\nF10 | arm | f10\nF11 | arm | f11\nF12 | arm | f12\n"
    "Delete | arm | delete\nEnter | arm | enter\nEsc | arm | esc\n"
    "ctrl-alt-del | send | ctrl-alt-del\n"
    "Restart SysRq | hold | alt-sysrq-s 2000ms alt-sysrq-u 2000ms alt-sysrq-b\n"
    "ctrl-alt-F2 | send | ctrl-alt-f2\n";

// what the screen's font has (font5x7.h); anything else would show as ?
static const char label_chars[] = " -.:_/?0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

static bool blank(char c) { return c == ' ' || c == '\t' || c == '\r'; }

// s[0..n) without the blanks at its ends, into out (size bytes); -1 when it does not fit
static int trimmed(const char *s, size_t n, char *out, size_t size)
{
    while (n && blank(s[0])) s++, n--;
    while (n && blank(s[n - 1])) n--;
    if (n >= size) return -1;
    memcpy(out, s, n);
    out[n] = 0;
    return 0;
}

// "500ms": the number, or 0 when t is not a wait
static uint32_t wait_of(const char *t)
{
    size_t n = strlen(t);
    if (n < 3 || n > 7 || strcmp(t + n - 2, "ms")) return 0;
    for (size_t i = 0; i < n - 2; i++)
        if (!isdigit((unsigned char)t[i])) return 0;
    return (uint32_t)atoi(t);
}

int menu_step(const char *keys, int i, menu_step_t *out)
{
    bool after_key = false;
    for (int at = 0;; at++) {
        while (blank(*keys)) keys++;
        if (!*keys) return 0;
        size_t n = strcspn(keys, " \t\r");
        char t[KEYS_NAME_MAX + 1];
        if (n > KEYS_NAME_MAX) return -1;
        memcpy(t, keys, n);
        t[n] = 0;
        keys += n;
        uint32_t w = wait_of(t);
        if (at < i) {
            after_key = !w;
            continue;
        }
        memset(out, 0, sizeof(*out));
        if (w) {
            if (w > MENU_WAIT_MS_MAX) return -1;
            out->wait = true;
            out->wait_ms = w;
            return 1;
        }
        const char *e = NULL;
        if (keys_parse(t, out->report, &e)) return -1;
        strcpy(out->name, t);
        out->wait_ms = after_key ? MENU_GAP_MS : 0;
        return 1;
    }
}

// one entry's line (no newline in it): NULL, or what is wrong with it (a fixed text, or one written into msg)
static const char *entry(const char *line, size_t len, menu_entry_t *e, char *msg, size_t size)
{
    const char *bar1 = memchr(line, '|', len);
    const char *bar2 = bar1 ? memchr(bar1 + 1, '|', len - (bar1 + 1 - line)) : NULL;
    if (!bar2) return "not <label> | <what> | <keys>";
    char what[8];
    if (trimmed(line, bar1 - line, e->label, sizeof(e->label)) || !e->label[0])
        return "the label must be 1 to 20 characters";
    if (e->label[strspn(e->label, label_chars)])
        return "the label may hold letters, digits, space and - . : _ / ? only (the screen's font)";
    if (trimmed(bar1 + 1, bar2 - bar1 - 1, what, sizeof(what))) what[0] = 0;
    if (!strcmp(what, "arm")) e->kind = MENU_ARM;
    else if (!strcmp(what, "send")) e->kind = MENU_SEND;
    else if (!strcmp(what, "hold")) e->kind = MENU_HOLD;
    else return "what must be arm, send or hold";
    if (trimmed(bar2 + 1, len - (bar2 + 1 - line), e->keys, sizeof(e->keys))) return "the keys are over 160 bytes";

    int nkeys = 0, nwaits = 0;
    bool last_wait = false;
    menu_step_t s;
    for (int i = 0;; i++) {
        int r = menu_step(e->keys, i, &s);
        if (!r) break;
        if (r < 0) { // say which part
            const char *k = e->keys;
            for (int at = 0; at <= i; at++) {
                while (blank(*k)) k++;
                if (at < i) k += strcspn(k, " \t\r");
            }
            snprintf(msg, size, "\"%.*s\" is no key and no wait (1ms to 10000ms)",
                     (int)(strcspn(k, " \t\r") > KEYS_NAME_MAX ? KEYS_NAME_MAX : strcspn(k, " \t\r")), k);
            return msg;
        }
        last_wait = s.wait;
        if (s.wait) nwaits++;
        else nkeys++;
    }
    if (!nkeys) return "no key";
    if (e->kind == MENU_ARM && (nkeys != 1 || nwaits)) return "arm takes one key and no wait";
    if (nkeys > MENU_STEPS_MAX) return "over 12 keys";
    if (last_wait) return "a wait at the end";
    return NULL;
}

int menu_parse(const char *text, menu_t *m, char *err, size_t size)
{
    char part[KEYS_NAME_MAX + 64];
    size_t total = strlen(text);
    m->n = 0;
    if (total > MENU_TEXT_MAX) {
        snprintf(err, size, "menu: the text is over 4096 bytes");
        return -1;
    }
    for (size_t i = 0; i < total; i++) {
        unsigned char c = (unsigned char)text[i];
        if ((c < ' ' && c != '\n' && c != '\r' && c != '\t') || c > '~') {
            snprintf(err, size, "menu: the text may hold plain ASCII only");
            return -1;
        }
    }
    int line_no = 0;
    for (const char *line = text; *line;) {
        size_t len = strcspn(line, "\n");
        const char *next = line + len + (line[len] == '\n');
        line_no++;
        const char *first = line;
        while (first < line + len && blank(*first)) first++;
        if (first < line + len && *first != '#') {
            const char *e = m->n == MENU_ENTRIES_MAX ? "over 40 entries" : entry(line, len, &m->e[m->n], part, sizeof(part));
            if (e) {
                snprintf(err, size, "menu line %d: %s", line_no, e);
                m->n = 0;
                return -1;
            }
            m->n++;
        }
        line = next;
    }
    if (!m->n) {
        snprintf(err, size, "menu: no entry in the text");
        return -1;
    }
    return 0;
}
