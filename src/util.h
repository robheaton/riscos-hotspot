/*
 * util.h
 *
 * Small portable helpers shared by the Hotspot client core.
 *
 * Everything in src/ that is not prefixed ro_ is plain C with no RISC OS
 * dependencies, so it also builds on the Linux dev machine and is exercised
 * by the host tests in test/ (under ASan/UBSan) before anything is sent to
 * real hardware.
 */

#ifndef HS_UTIL_H
#define HS_UTIL_H

#include <stddef.h>

/* Copy src into dst (cap bytes including the NUL). Always terminates when
 * cap > 0 and returns the number of characters stored. */
size_t u_copy(char *dst, size_t cap, const char *src);
size_t u_copyn(char *dst, size_t cap, const char *src, size_t n);

int u_ieq(const char *a, const char *b);        /* case-insensitive equal */
int u_istarts(const char *s, const char *prefix);
char *u_trim(char *s);                          /* trim in place, returns s */

/* memmem()-alikes: not every libc has one, and the haystack is not NUL
 * terminated here. NULL if not found. */
const char *u_find(const char *hay, size_t hn, const char *needle);
const char *u_ifind(const char *hay, size_t hn, const char *needle);

/* Growable byte buffer with an optional hard cap. Once the cap is reached
 * further data is dropped (and `truncated` set) rather than failing, so a
 * caller can keep draining a socket without keeping everything. The buffer
 * is always NUL terminated when non-empty. */
typedef struct {
    char   *p;
    size_t  len;
    size_t  cap;
    size_t  max;        /* 0 = unlimited */
    int     truncated;
} sbuf;

void sb_init(sbuf *b, size_t max);
/* Returns 0 on success, 1 if some data was dropped (cap hit), -1 on OOM. */
int sb_add(sbuf *b, const void *data, size_t n);
int sb_addstr(sbuf *b, const char *s);
const char *sb_str(const sbuf *b);              /* never NULL */
void sb_reset(sbuf *b);
void sb_free(sbuf *b);

/* Base64 (RFC 4648, with padding). Returns the length written, or 0 if the
 * output buffer is too small. */
size_t b64_encode(const unsigned char *in, size_t n, char *out, size_t cap);

/* application/x-www-form-urlencoded. Returns the length written, or
 * (size_t)-1 if it did not fit. */
size_t url_encode(const char *in, char *out, size_t cap);

/* Convert UTF-8 text to Latin-1 in place (RISC OS's default alphabet).
 * Anything that is not representable becomes '?', common punctuation is
 * mapped to its ASCII look-alike. Bytes that are not valid UTF-8 are left
 * alone, so text that is already Latin-1 survives. */
/* Turns UTF-8 text into the Latin-1 the RISC OS desktop shows. Also takes out
 * what the desktop cannot show: colour escape sequences are dropped and other
 * control characters become one space. In place. */
void utf8_to_latin1(char *s);

/* Control characters (below 32, and 127) become spaces; nothing else changes.
 * For text built by the program itself, which is already Latin-1. */
void u_no_ctrl(char *s);

/* Map one Unicode code point to a Latin-1 byte (never 0). */
unsigned char u_cp_to_latin1(unsigned cp);

#endif /* HS_UTIL_H */
