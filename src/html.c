/*
 * html.c
 *
 * See html.h.
 */

#include "html.h"
#include "util.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Text extraction                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    char   *out;
    size_t  cap;
    size_t  len;
    int     pending_space;
    int     pending_break;      /* a block boundary was seen (multi-line) */
    int     multiline;
} textw;

static void tw_break(textw *t)
{
    if (t->len == 0)
        return;

    if (t->multiline)
        t->pending_break = 1;
    else
        t->pending_space = 1;
}

static void tw_char(textw *t, unsigned char c)
{
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
        if (t->len > 0)
            t->pending_space = 1;
        return;
    }

    if (t->pending_break) {
        if (t->len + 1 < t->cap)
            t->out[t->len++] = '\n';
        t->pending_break = 0;
        t->pending_space = 0;
    } else if (t->pending_space) {
        if (t->len + 1 < t->cap)
            t->out[t->len++] = ' ';
        t->pending_space = 0;
    }

    if (t->len + 1 < t->cap)
        t->out[t->len++] = (char)c;
}

typedef struct {
    const char *name;
    unsigned    cp;
} entity;

static const entity entities[] = {
    { "amp", '&' },    { "lt", '<' },      { "gt", '>' },
    { "quot", '"' },   { "apos", '\'' },   { "nbsp", 0xA0 },
    { "bull", 0x2022 },{ "middot", 0xB7 }, { "ndash", 0x2013 },
    { "mdash", 0x2014 },{ "hellip", 0x2026 },{ "laquo", 0xAB },
    { "raquo", 0xBB }, { "copy", 0xA9 },   { "deg", 0xB0 },
    { "rarr", '>' },   { "larr", '<' },    { "euro", 0x20AC },
    { "lsquo", 0x2018 },{ "rsquo", 0x2019 },{ "ldquo", 0x201C },
    { "rdquo", 0x201D },{ "times", 0xD7 }, { "plusmn", 0xB1 },
    { NULL, 0 }
};

/* Handles the text after an '&' (p points at it). Emits the decoded
 * character and returns the number of bytes consumed after the '&', or 0 if
 * this is not an entity (the caller then emits a literal '&'). */
static size_t decode_entity(textw *t, const char *p, const char *end)
{
    size_t n = 0;
    unsigned cp = 0;

    if (p < end && *p == '#') {
        const char *q = p + 1;
        int base = 10;
        int digits = 0;

        if (q < end && (*q == 'x' || *q == 'X')) {
            base = 16;
            q++;
        }

        while (q < end && digits < 8) {
            int d;

            if (*q >= '0' && *q <= '9')
                d = *q - '0';
            else if (base == 16 && *q >= 'a' && *q <= 'f')
                d = *q - 'a' + 10;
            else if (base == 16 && *q >= 'A' && *q <= 'F')
                d = *q - 'A' + 10;
            else
                break;

            cp = cp * (unsigned)base + (unsigned)d;
            q++;
            digits++;
        }

        if (digits == 0)
            return 0;

        if (q < end && *q == ';')
            q++;

        tw_char(t, u_cp_to_latin1(cp));
        return (size_t)(q - p);
    }

    while (p + n < end && n < 8 && isalnum((unsigned char)p[n]))
        n++;

    if (n == 0)
        return 0;

    {
        const entity *e;
        char name[10];

        memcpy(name, p, n);
        name[n] = '\0';

        for (e = entities; e->name != NULL; e++) {
            if (strcmp(e->name, name) == 0) {
                cp = e->cp;
                if (p + n < end && p[n] == ';')
                    n++;
                tw_char(t, u_cp_to_latin1(cp));
                return n;
            }
        }
    }

    return 0;
}

static int tag_is(const char *p, const char *end, const char *name)
{
    size_t n = strlen(name);
    size_t i;

    /* Needs the '<', the name and one byte after it to be inside the range. */
    if ((size_t)(end - p) < n + 2)
        return 0;
    if (p[0] != '<')
        return 0;

    for (i = 0; i < n; i++) {
        if (tolower((unsigned char)p[1 + i]) !=
            tolower((unsigned char)name[i]))
            return 0;
    }

    return !isalnum((unsigned char)p[1 + n]);
}

static size_t text_extract(const char *p, size_t n, char *out, size_t cap,
                           int multiline)
{
    const char *end = p + n;
    textw t;

    if (cap == 0)
        return 0;

    t.out = out;
    t.cap = cap;
    t.len = 0;
    t.pending_space = 0;
    t.pending_break = 0;
    t.multiline = multiline;

    while (p < end) {
        unsigned char c = (unsigned char)*p;

        if (c == '<') {
            const char *te;

            if (tag_is(p, end, "script") || tag_is(p, end, "style")) {
                const char *close = u_ifind(p, (size_t)(end - p),
                                            tag_is(p, end, "script")
                                                ? "</script" : "</style");
                if (close == NULL)
                    break;
                te = html_tag_end(close, end);
                p = (te != NULL) ? te : end;
                continue;
            }

            te = html_tag_end(p, end);
            if (te == NULL)
                break;

            if (tag_is(p, end, "br") || tag_is(p, end, "/div") ||
                tag_is(p, end, "/p") || tag_is(p, end, "/li") ||
                tag_is(p, end, "/tr") || tag_is(p, end, "/table") ||
                tag_is(p, end, "/h1") || tag_is(p, end, "/h2") ||
                tag_is(p, end, "/h3") || tag_is(p, end, "/h4"))
                tw_break(&t);
            else if (tag_is(p, end, "/td") || tag_is(p, end, "/th") ||
                     tag_is(p, end, "/span"))
                t.pending_space = (t.len > 0);
            else if (t.multiline &&
                     (tag_is(p, end, "div") || tag_is(p, end, "p") ||
                      tag_is(p, end, "li") || tag_is(p, end, "tr") ||
                      tag_is(p, end, "table") || tag_is(p, end, "h1") ||
                      tag_is(p, end, "h2") || tag_is(p, end, "h3") ||
                      tag_is(p, end, "h4")))
                tw_break(&t);       /* a block also starts on a new line */

            p = te;
            continue;
        }

        if (c == '&') {
            size_t used = decode_entity(&t, p + 1, end);

            if (used > 0) {
                p += 1 + used;
                continue;
            }

            tw_char(&t, '&');
            p++;
            continue;
        }

        if (c >= 0x80) {
            /* UTF-8 multi-byte sequence (or a stray Latin-1 byte). */
            unsigned cp;
            int extra;
            int i;

            if ((c & 0xE0) == 0xC0) {
                cp = c & 0x1F;
                extra = 1;
            } else if ((c & 0xF0) == 0xE0) {
                cp = c & 0x0F;
                extra = 2;
            } else if ((c & 0xF8) == 0xF0) {
                cp = c & 0x07;
                extra = 3;
            } else {
                tw_char(&t, c);
                p++;
                continue;
            }

            for (i = 0; i < extra; i++) {
                if (p + 1 + i >= end ||
                    ((unsigned char)p[1 + i] & 0xC0) != 0x80)
                    break;
                cp = (cp << 6) | ((unsigned char)p[1 + i] & 0x3F);
            }

            if (i < extra) {
                tw_char(&t, c);
                p++;
                continue;
            }

            if (cp == 0xA0)
                tw_char(&t, ' ');
            else
                tw_char(&t, u_cp_to_latin1(cp));
            p += 1 + extra;
            continue;
        }

        tw_char(&t, c);
        p++;
    }

    t.out[t.len] = '\0';
    return t.len;
}

size_t html_text(const char *p, size_t n, char *out, size_t cap)
{
    return text_extract(p, n, out, cap, 0);
}

size_t html_lines(const char *p, size_t n, char *out, size_t cap)
{
    return text_extract(p, n, out, cap, 1);
}

/* ------------------------------------------------------------------ */
/* Structure helpers                                                  */
/* ------------------------------------------------------------------ */

const char *html_tag_end(const char *p, const char *end)
{
    if (p >= end || *p != '<')
        return NULL;

    if (end - p >= 4 && memcmp(p, "<!--", 4) == 0) {
        const char *q = u_find(p + 4, (size_t)(end - p - 4), "-->");
        return (q != NULL) ? q + 3 : NULL;
    }

    for (p++; p < end; p++) {
        if (*p == '>')
            return p + 1;
    }

    return NULL;
}

const char *html_div_close(const char *p, const char *end)
{
    int depth = 1;

    while (p < end) {
        const char *lt = memchr(p, '<', (size_t)(end - p));
        const char *te;

        if (lt == NULL)
            return NULL;

        if (tag_is(lt, end, "div")) {
            depth++;
        } else if (tag_is(lt, end, "/div")) {
            depth--;
            if (depth == 0)
                return lt;
        }

        te = html_tag_end(lt, end);
        if (te == NULL)
            return NULL;
        p = te;
    }

    return NULL;
}

const char *html_find_tag(const char *p, const char *end, const char *name,
                          const char **tag_end)
{
    while (p < end) {
        const char *lt = memchr(p, '<', (size_t)(end - p));
        const char *te;

        if (lt == NULL)
            return NULL;

        te = html_tag_end(lt, end);
        if (te == NULL)
            return NULL;

        if (tag_is(lt, end, name)) {
            *tag_end = te;
            return lt;
        }

        p = te;
    }

    return NULL;
}

const char *html_find_div(const char *p, const char *end, const char *needle,
                          const char **tag_end)
{
    while (p < end) {
        const char *te;
        const char *d = html_find_tag(p, end, "div", &te);

        if (d == NULL)
            return NULL;

        if (u_find(d, (size_t)(te - d), needle) != NULL) {
            *tag_end = te;
            return d;
        }

        p = te;
    }

    return NULL;
}

int html_attr_int(const char *tag, const char *tag_end, const char *name,
                  int *value)
{
    char key[40];
    const char *p;
    int v = 0;
    int digits = 0;

    if (strlen(name) + 3 > sizeof key)
        return 0;

    key[0] = '\0';
    strcat(key, name);
    strcat(key, "=\"");

    p = u_find(tag, (size_t)(tag_end - tag), key);
    if (p == NULL)
        return 0;

    p += strlen(key);
    while (p < tag_end && *p >= '0' && *p <= '9' && digits < 9) {
        v = v * 10 + (*p - '0');
        p++;
        digits++;
    }

    if (digits == 0)
        return 0;

    *value = v;
    return 1;
}

int html_attr_str(const char *tag, const char *tag_end, const char *name,
                  char *out, size_t cap)
{
    char key[40];
    const char *p;
    size_t n = 0;

    if (cap == 0)
        return 0;
    out[0] = '\0';

    if (strlen(name) + 3 > sizeof key)
        return 0;

    key[0] = '\0';
    strcat(key, name);
    strcat(key, "=\"");

    p = u_find(tag, (size_t)(tag_end - tag), key);
    if (p == NULL)
        return 0;

    p += strlen(key);
    while (p < tag_end && *p != '"' && n + 1 < cap)
        out[n++] = *p++;

    if (p >= tag_end || *p != '"')
        return 0;               /* unterminated */

    out[n] = '\0';
    return 1;
}
