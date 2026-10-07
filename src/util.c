/*
 * util.c
 *
 * See util.h.
 */

#include "util.h"

#include <stdlib.h>
#include <string.h>

size_t u_copyn(char *dst, size_t cap, const char *src, size_t n)
{
    size_t len;

    if (cap == 0)
        return 0;

    len = 0;
    while (len < n && len + 1 < cap && src[len] != '\0') {
        dst[len] = src[len];
        len++;
    }

    dst[len] = '\0';
    return len;
}

size_t u_copy(char *dst, size_t cap, const char *src)
{
    return u_copyn(dst, cap, src, (size_t)-1);
}

static int lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

int u_ieq(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        if (lower((unsigned char)*a) != lower((unsigned char)*b))
            return 0;
        a++;
        b++;
    }

    return *a == *b;
}

int u_istarts(const char *s, const char *prefix)
{
    while (*prefix != '\0') {
        if (*s == '\0' ||
            lower((unsigned char)*s) != lower((unsigned char)*prefix))
            return 0;
        s++;
        prefix++;
    }

    return 1;
}

char *u_trim(char *s)
{
    char *start = s;
    size_t len;

    while (*start == ' ' || *start == '\t' || *start == '\r' ||
           *start == '\n')
        start++;

    if (start != s)
        memmove(s, start, strlen(start) + 1);

    len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' ||
                       s[len - 1] == '\r' || s[len - 1] == '\n'))
        s[--len] = '\0';

    return s;
}

const char *u_find(const char *hay, size_t hn, const char *needle)
{
    size_t nn = strlen(needle);
    size_t i;

    if (nn == 0)
        return hay;
    if (hn < nn)
        return NULL;

    for (i = 0; i + nn <= hn; i++) {
        if (hay[i] == needle[0] && memcmp(hay + i, needle, nn) == 0)
            return hay + i;
    }

    return NULL;
}

const char *u_ifind(const char *hay, size_t hn, const char *needle)
{
    size_t nn = strlen(needle);
    size_t i;
    size_t j;

    if (nn == 0)
        return hay;
    if (hn < nn)
        return NULL;

    for (i = 0; i + nn <= hn; i++) {
        for (j = 0; j < nn; j++) {
            if (lower((unsigned char)hay[i + j]) !=
                lower((unsigned char)needle[j]))
                break;
        }
        if (j == nn)
            return hay + i;
    }

    return NULL;
}

/* ------------------------------------------------------------------ */
/* Growable buffer                                                    */
/* ------------------------------------------------------------------ */

void sb_init(sbuf *b, size_t max)
{
    b->p = NULL;
    b->len = 0;
    b->cap = 0;
    b->max = max;
    b->truncated = 0;
}

int sb_add(sbuf *b, const void *data, size_t n)
{
    size_t need;
    size_t newcap;
    char *np;

    if (b->max != 0 && b->len + n > b->max) {
        n = (b->max > b->len) ? b->max - b->len : 0;
        b->truncated = 1;
    }

    if (n == 0)
        return b->truncated ? 1 : 0;

    need = b->len + n + 1;
    if (need > b->cap) {
        newcap = (b->cap != 0) ? b->cap : 512;
        while (newcap < need) {
            if (newcap > ((size_t)-1) / 2)
                return -1;
            newcap *= 2;
        }

        np = (char *)realloc(b->p, newcap);
        if (np == NULL)
            return -1;

        b->p = np;
        b->cap = newcap;
    }

    memcpy(b->p + b->len, data, n);
    b->len += n;
    b->p[b->len] = '\0';

    return b->truncated ? 1 : 0;
}

int sb_addstr(sbuf *b, const char *s)
{
    return sb_add(b, s, strlen(s));
}

const char *sb_str(const sbuf *b)
{
    return (b->p != NULL) ? b->p : "";
}

void sb_reset(sbuf *b)
{
    b->len = 0;
    b->truncated = 0;
    if (b->p != NULL)
        b->p[0] = '\0';
}

void sb_free(sbuf *b)
{
    free(b->p);
    b->p = NULL;
    b->len = 0;
    b->cap = 0;
    b->truncated = 0;
}

/* ------------------------------------------------------------------ */
/* Encoders                                                           */
/* ------------------------------------------------------------------ */

size_t b64_encode(const unsigned char *in, size_t n, char *out, size_t cap)
{
    static const char tab[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t need = ((n + 2) / 3) * 4 + 1;
    size_t i;
    size_t o = 0;

    if (cap < need) {
        if (cap > 0)
            out[0] = '\0';
        return 0;
    }

    for (i = 0; i + 2 < n; i += 3) {
        unsigned v = ((unsigned)in[i] << 16) | ((unsigned)in[i + 1] << 8) |
                     in[i + 2];
        out[o++] = tab[(v >> 18) & 63];
        out[o++] = tab[(v >> 12) & 63];
        out[o++] = tab[(v >> 6) & 63];
        out[o++] = tab[v & 63];
    }

    if (n - i == 1) {
        unsigned v = (unsigned)in[i] << 16;
        out[o++] = tab[(v >> 18) & 63];
        out[o++] = tab[(v >> 12) & 63];
        out[o++] = '=';
        out[o++] = '=';
    } else if (n - i == 2) {
        unsigned v = ((unsigned)in[i] << 16) | ((unsigned)in[i + 1] << 8);
        out[o++] = tab[(v >> 18) & 63];
        out[o++] = tab[(v >> 12) & 63];
        out[o++] = tab[(v >> 6) & 63];
        out[o++] = '=';
    }

    out[o] = '\0';
    return o;
}

size_t url_encode(const char *in, char *out, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;

    if (cap == 0)
        return (size_t)-1;

    for (; *in != '\0'; in++) {
        unsigned char c = (unsigned char)*in;

        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
            c == '~') {
            if (o + 1 >= cap)
                return (size_t)-1;
            out[o++] = (char)c;
        } else if (c == ' ') {
            if (o + 1 >= cap)
                return (size_t)-1;
            out[o++] = '+';
        } else {
            if (o + 3 >= cap)
                return (size_t)-1;
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 15];
        }
    }

    out[o] = '\0';
    return o;
}

/* ------------------------------------------------------------------ */
/* Character set                                                      */
/* ------------------------------------------------------------------ */

unsigned char u_cp_to_latin1(unsigned cp)
{
    switch (cp) {
        case 0x00A0: return ' ';
        case 0x2018:
        case 0x2019:
        case 0x201B: return '\'';
        case 0x201C:
        case 0x201D: return '"';
        case 0x2010:
        case 0x2011:
        case 0x2012:
        case 0x2013:
        case 0x2014: return '-';
        case 0x2022:
        case 0x00B7: return '*';
        case 0x2026: return '.';
        case 0x20AC: return 'E';
        default: break;
    }

    if (cp >= 0x20 && cp < 0x7F)
        return (unsigned char)cp;
    if (cp >= 0xA1 && cp <= 0xFF)
        return (unsigned char)cp;

    return '?';
}

void u_no_ctrl(char *s)
{
    for (; *s != '\0'; s++) {
        if ((unsigned char)*s < 0x20 || (unsigned char)*s == 0x7F)
            *s = ' ';
    }
}

void utf8_to_latin1(char *s)
{
    unsigned char *r = (unsigned char *)s;
    unsigned char *w = (unsigned char *)s;
    unsigned char *start = w;

    while (*r != '\0') {
        unsigned c = *r;
        unsigned cp;
        int extra;
        int i;

        /* A colour code from a script ("ESC [ 32 m"): drop it. */
        if (c == 0x1B && r[1] == '[') {
            r += 2;
            while (*r != '\0' && !(*r >= 0x40 && *r <= 0x7E))
                r++;
            if (*r != '\0')
                r++;
            continue;
        }

        /* Any other control character (a new line in a reply, say) becomes a
         * space: the Wimp ends the text of an icon or error box at the first
         * byte below 32, so it would cut the message short. */
        if (c < 0x20 || c == 0x7F) {
            if (w > start && w[-1] != ' ')
                *w++ = ' ';
            r++;
            continue;
        }

        if (c < 0x80) {
            *w++ = (unsigned char)c;
            r++;
            continue;
        }

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
            /* Stray continuation byte: probably already Latin-1. */
            *w++ = (unsigned char)c;
            r++;
            continue;
        }

        for (i = 0; i < extra; i++) {
            if ((r[1 + i] & 0xC0) != 0x80)
                break;
            cp = (cp << 6) | (r[1 + i] & 0x3F);
        }

        if (i < extra) {
            /* Truncated/invalid sequence: keep the byte as it is. */
            *w++ = (unsigned char)c;
            r++;
            continue;
        }

        *w++ = u_cp_to_latin1(cp);
        r += 1 + extra;
    }

    *w = '\0';
}
