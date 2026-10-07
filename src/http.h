/*
 * http.h
 *
 * A small non-blocking HTTP/1.1 client. One request per connection
 * ("Connection: close"), plain TCP only (the WPSD dashboard is served over
 * plain HTTP on the LAN), HTTP Basic authentication, Content-Length,
 * chunked and read-until-close bodies.
 *
 * It is written as a state machine that is pumped from the Wimp poll loop:
 * http_start() begins the connection and returns immediately, and every
 * http_step() does whatever socket work is possible without blocking. That
 * keeps the desktop responsive even when the hotspot is switched off or the
 * network is slow, which a blocking read() inside a Wimp task would not.
 *
 * Only the host name lookup (if the address is not a dotted quad) can
 * block, and results - including failures - are cached for a while so a bad
 * name costs one stall, not one per poll.
 */

#ifndef HS_HTTP_H
#define HS_HTTP_H

#include <stddef.h>

typedef struct {
    char host[96];
    int  port;
    char user[48];
    char pass[48];      /* empty user = no Authorization header */
} http_endpoint;

typedef enum {
    HTTP_CONNECTING = 0,
    HTTP_SENDING,
    HTTP_RECEIVING,
    HTTP_DONE,
    HTTP_FAILED
} http_state;

typedef struct http_req http_req;

/* Call once at start-up (ignores SIGPIPE). */
void http_init(void);

/* Monotonic centisecond clock (OS_ReadMonotonicTime on RISC OS). */
unsigned long http_clock_cs(void);

/* Begin a request. `target` is the path plus query string. For POST pass a
 * content type and body; for GET pass NULL/NULL/0. `max_body` caps how much
 * of the response body is kept (the rest is still read and discarded, so
 * the server is never cut off mid-script). `timeout_cs` is an inactivity
 * timeout: it is re-armed whenever any progress is made.
 *
 * Never returns NULL unless out of memory; an immediate failure (such as an
 * unresolvable host) shows up as HTTP_FAILED from the first http_step(). */
http_req *http_start(const http_endpoint *ep, const char *method,
                     const char *target, const char *content_type,
                     const char *body, size_t body_len, size_t max_body,
                     unsigned timeout_cs);

http_state http_step(http_req *r);

int http_status(const http_req *r);                 /* 0 until headers seen */
const char *http_body(const http_req *r, size_t *len);
const char *http_error(const http_req *r);          /* when HTTP_FAILED */
const char *http_headers(const http_req *r);        /* raw header block */
int http_truncated(const http_req *r);              /* body exceeded max_body */
int http_incomplete(const http_req *r);             /* connection ended early */
int http_got_response(const http_req *r);           /* headers were received */

void http_free(http_req *r);

#endif /* HS_HTTP_H */
