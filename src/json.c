/*
 * json.c
 *
 * See json.h.
 */

#include "json.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

#define JSON_MAX_DEPTH 24

typedef struct {
    const char *p;
    const char *end;
    int         depth;
} jparser;

static jnode *parse_value(jparser *jp);

static jnode *node_new(jtype t)
{
    jnode *n = (jnode *)calloc(1, sizeof *n);

    if (n != NULL)
        n->t = t;

    return n;
}

void json_free(jnode *n)
{
    int i;

    if (n == NULL)
        return;

    for (i = 0; i < n->nkid; i++) {
        json_free(n->kid[i]);
        if (n->key != NULL)
            free(n->key[i]);
    }

    free(n->kid);
    free(n->key);
    free(n->s);
    free(n);
}

static int node_add(jnode *parent, char *key, jnode *child)
{
    if (parent->nkid == parent->capkid) {
        int cap = (parent->capkid != 0) ? parent->capkid * 2 : 8;
        jnode **nk = (jnode **)realloc(parent->kid,
                                       (size_t)cap * sizeof *nk);

        if (nk == NULL)
            return 0;
        parent->kid = nk;

        if (parent->t == J_OBJ) {
            char **nkeys = (char **)realloc(parent->key,
                                            (size_t)cap * sizeof *nkeys);

            if (nkeys == NULL)
                return 0;
            parent->key = nkeys;
        }

        parent->capkid = cap;
    }

    parent->kid[parent->nkid] = child;
    if (parent->t == J_OBJ)
        parent->key[parent->nkid] = key;
    parent->nkid++;

    return 1;
}

static void skip_ws(jparser *jp)
{
    while (jp->p < jp->end &&
           (*jp->p == ' ' || *jp->p == '\t' || *jp->p == '\r' ||
            *jp->p == '\n'))
        jp->p++;
}

static int hex4(const char *p, const char *end, unsigned *out)
{
    unsigned v = 0;
    int i;

    if (end - p < 4)
        return 0;

    for (i = 0; i < 4; i++) {
        char c = p[i];
        int d;

        if (c >= '0' && c <= '9')
            d = c - '0';
        else if (c >= 'a' && c <= 'f')
            d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            d = c - 'A' + 10;
        else
            return 0;

        v = (v << 4) | (unsigned)d;
    }

    *out = v;
    return 1;
}

static void put_utf8(sbuf *b, unsigned cp)
{
    char t[4];
    size_t n;

    if (cp < 0x80) {
        t[0] = (char)cp;
        n = 1;
    } else if (cp < 0x800) {
        t[0] = (char)(0xC0 | (cp >> 6));
        t[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        t[0] = (char)(0xE0 | (cp >> 12));
        t[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        t[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        t[0] = (char)(0xF0 | (cp >> 18));
        t[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        t[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        t[3] = (char)(0x80 | (cp & 0x3F));
        n = 4;
    }

    sb_add(b, t, n);
}

/* Parses a string starting at the opening quote; returns a malloc'd,
 * NUL-terminated UTF-8 string or NULL. */
static char *parse_string(jparser *jp)
{
    sbuf b;
    char *result;

    if (jp->p >= jp->end || *jp->p != '"')
        return NULL;
    jp->p++;

    sb_init(&b, 0);

    while (jp->p < jp->end && *jp->p != '"') {
        char c = *jp->p++;

        if (c != '\\') {
            sb_add(&b, &c, 1);
            continue;
        }

        if (jp->p >= jp->end)
            goto fail;

        c = *jp->p++;
        switch (c) {
            case '"':  sb_add(&b, "\"", 1); break;
            case '\\': sb_add(&b, "\\", 1); break;
            case '/':  sb_add(&b, "/", 1); break;
            case 'b':  sb_add(&b, "\b", 1); break;
            case 'f':  sb_add(&b, "\f", 1); break;
            case 'n':  sb_add(&b, "\n", 1); break;
            case 'r':  sb_add(&b, "\r", 1); break;
            case 't':  sb_add(&b, "\t", 1); break;
            case 'u': {
                unsigned cp;

                if (!hex4(jp->p, jp->end, &cp))
                    goto fail;
                jp->p += 4;

                if (cp >= 0xD800 && cp < 0xDC00 && jp->end - jp->p >= 6 &&
                    jp->p[0] == '\\' && jp->p[1] == 'u') {
                    unsigned lo;

                    if (hex4(jp->p + 2, jp->end, &lo) && lo >= 0xDC00 &&
                        lo < 0xE000) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        jp->p += 6;
                    }
                }

                put_utf8(&b, cp);
                break;
            }
            default:
                goto fail;
        }
    }

    if (jp->p >= jp->end)
        goto fail;
    jp->p++;    /* closing quote */

    if (b.p == NULL) {
        result = (char *)malloc(1);
        if (result != NULL)
            result[0] = '\0';
        return result;
    }

    return b.p;     /* ownership passes to the caller */

fail:
    sb_free(&b);
    return NULL;
}

static jnode *parse_array(jparser *jp)
{
    jnode *arr = node_new(J_ARR);

    if (arr == NULL)
        return NULL;

    jp->p++;    /* '[' */
    skip_ws(jp);

    if (jp->p < jp->end && *jp->p == ']') {
        jp->p++;
        return arr;
    }

    for (;;) {
        jnode *v = parse_value(jp);

        if (v == NULL)
            goto fail;
        if (!node_add(arr, NULL, v)) {
            json_free(v);
            goto fail;
        }

        skip_ws(jp);
        if (jp->p >= jp->end)
            goto fail;

        if (*jp->p == ',') {
            jp->p++;
            continue;
        }

        if (*jp->p == ']') {
            jp->p++;
            return arr;
        }

        goto fail;
    }

fail:
    json_free(arr);
    return NULL;
}

static jnode *parse_object(jparser *jp)
{
    jnode *obj = node_new(J_OBJ);

    if (obj == NULL)
        return NULL;

    jp->p++;    /* '{' */
    skip_ws(jp);

    if (jp->p < jp->end && *jp->p == '}') {
        jp->p++;
        return obj;
    }

    for (;;) {
        char *key;
        jnode *v;

        skip_ws(jp);
        key = parse_string(jp);
        if (key == NULL)
            goto fail;

        skip_ws(jp);
        if (jp->p >= jp->end || *jp->p != ':') {
            free(key);
            goto fail;
        }
        jp->p++;

        v = parse_value(jp);
        if (v == NULL) {
            free(key);
            goto fail;
        }

        if (!node_add(obj, key, v)) {
            free(key);
            json_free(v);
            goto fail;
        }

        skip_ws(jp);
        if (jp->p >= jp->end)
            goto fail;

        if (*jp->p == ',') {
            jp->p++;
            continue;
        }

        if (*jp->p == '}') {
            jp->p++;
            return obj;
        }

        goto fail;
    }

fail:
    json_free(obj);
    return NULL;
}

static int match_word(jparser *jp, const char *word)
{
    size_t n = strlen(word);

    if ((size_t)(jp->end - jp->p) < n || memcmp(jp->p, word, n) != 0)
        return 0;

    jp->p += n;
    return 1;
}

static jnode *parse_value(jparser *jp)
{
    jnode *n;

    skip_ws(jp);
    if (jp->p >= jp->end)
        return NULL;

    if (jp->depth >= JSON_MAX_DEPTH)
        return NULL;

    switch (*jp->p) {
        case '{':
            jp->depth++;
            n = parse_object(jp);
            jp->depth--;
            return n;

        case '[':
            jp->depth++;
            n = parse_array(jp);
            jp->depth--;
            return n;

        case '"': {
            char *s = parse_string(jp);

            if (s == NULL)
                return NULL;

            n = node_new(J_STR);
            if (n == NULL) {
                free(s);
                return NULL;
            }

            n->s = s;
            return n;
        }

        case 't':
            if (!match_word(jp, "true"))
                return NULL;
            n = node_new(J_BOOL);
            if (n != NULL)
                n->b = 1;
            return n;

        case 'f':
            if (!match_word(jp, "false"))
                return NULL;
            return node_new(J_BOOL);

        case 'n':
            if (!match_word(jp, "null"))
                return NULL;
            return node_new(J_NULL);

        default: {
            char tok[40];
            size_t len = 0;
            char *endp;

            while (jp->p + len < jp->end && len + 1 < sizeof tok) {
                char c = jp->p[len];

                if ((c >= '0' && c <= '9') || c == '-' || c == '+' ||
                    c == '.' || c == 'e' || c == 'E')
                    len++;
                else
                    break;
            }

            if (len == 0)
                return NULL;

            memcpy(tok, jp->p, len);
            tok[len] = '\0';

            n = node_new(J_NUM);
            if (n == NULL)
                return NULL;

            n->n = strtod(tok, &endp);
            if (endp == tok) {
                json_free(n);
                return NULL;
            }

            jp->p += len;
            return n;
        }
    }
}

jnode *json_parse(const char *text, size_t len)
{
    jparser jp;
    jnode *root;

    jp.p = text;
    jp.end = text + len;
    jp.depth = 0;

    root = parse_value(&jp);
    if (root == NULL)
        return NULL;

    skip_ws(&jp);
    if (jp.p != jp.end) {
        json_free(root);
        return NULL;
    }

    return root;
}

jnode *json_parse_lenient(const char *text, size_t len, size_t *used)
{
    jparser jp;
    size_t i = 0;
    jnode *root;

    while (i < len && text[i] != '[' && text[i] != '{')
        i++;

    jp.p = text + i;
    jp.end = text + len;
    jp.depth = 0;

    root = parse_value(&jp);
    if (root != NULL && used != NULL)
        *used = (size_t)(jp.p - text);

    return root;
}

const jnode *json_member(const jnode *obj, const char *key)
{
    int i;

    if (obj == NULL || obj->t != J_OBJ)
        return NULL;

    for (i = 0; i < obj->nkid; i++) {
        if (strcmp(obj->key[i], key) == 0)
            return obj->kid[i];
    }

    return NULL;
}

const char *json_string(const jnode *obj, const char *key, const char *dflt)
{
    const jnode *m = json_member(obj, key);

    if (m != NULL && m->t == J_STR && m->s != NULL)
        return m->s;

    return dflt;
}

int json_int(const jnode *obj, const char *key, int dflt)
{
    const jnode *m = json_member(obj, key);

    if (m == NULL)
        return dflt;

    switch (m->t) {
        case J_NUM:
            return (int)m->n;
        case J_BOOL:
            return m->b;
        case J_STR:
            return (m->s != NULL) ? atoi(m->s) : dflt;
        default:
            return dflt;
    }
}

int json_count(const jnode *arr)
{
    if (arr == NULL || (arr->t != J_ARR && arr->t != J_OBJ))
        return 0;

    return arr->nkid;
}

const jnode *json_at(const jnode *arr, int i)
{
    if (arr == NULL || arr->t != J_ARR || i < 0 || i >= arr->nkid)
        return NULL;

    return arr->kid[i];
}
