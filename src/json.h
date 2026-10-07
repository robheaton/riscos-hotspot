/*
 * json.h
 *
 * A small JSON reader for the replies WPSD's PHP endpoints produce
 * (/api/ and /admin/system_api.php). Builds a simple tree; strings are
 * decoded to UTF-8. Not a streaming parser and not meant for large inputs.
 */

#ifndef HS_JSON_H
#define HS_JSON_H

#include <stddef.h>

typedef enum {
    J_NULL = 0,
    J_BOOL,
    J_NUM,
    J_STR,
    J_ARR,
    J_OBJ
} jtype;

typedef struct jnode jnode;

struct jnode {
    jtype    t;
    char    *s;         /* J_STR */
    double   n;         /* J_NUM */
    int      b;         /* J_BOOL */
    jnode  **kid;       /* J_ARR / J_OBJ children */
    char   **key;       /* J_OBJ member names (parallel to kid) */
    int      nkid;
    int      capkid;
};

/* Parse `len` bytes of text. Returns the root, or NULL on a syntax error,
 * excessive nesting or out of memory. */
jnode *json_parse(const char *text, size_t len);

/* Like json_parse but starts at the first '[' or '{' in the text and ignores
 * whatever follows the value. For replies from PHP, which can have warnings
 * or notices printed in front of (or after) the JSON when the hotspot has
 * display_errors switched on. `used` (optional) gets the number of bytes
 * consumed from the start of `text`. */
jnode *json_parse_lenient(const char *text, size_t len, size_t *used);

void json_free(jnode *n);

/* Object member lookup (first match). NULL if absent or `obj` is not an
 * object. */
const jnode *json_member(const jnode *obj, const char *key);

/* Convenience: member as a string. Only real JSON strings count; numbers,
 * booleans and missing members all give `dflt`. The pointer belongs to the
 * tree and is valid until json_free(). */
const char *json_string(const jnode *obj, const char *key, const char *dflt);

/* Convenience: member as an integer (numbers, numeric strings, booleans). */
int json_int(const jnode *obj, const char *key, int dflt);

int json_count(const jnode *arr);
const jnode *json_at(const jnode *arr, int i);

#endif /* HS_JSON_H */
