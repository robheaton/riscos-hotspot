/*
 * http.c
 *
 * See http.h.
 */

#include "http.h"
#include "util.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>

#ifdef __riscos__
#include <kernel.h>
#else
#include <time.h>
#endif

#define HTTP_HEADER_MAX     16384u
#define HTTP_RECV_CHUNK     4096u
#define HTTP_RECV_PER_STEP  8
#define HTTP_RESOLVE_TTL_CS 3000UL      /* how long a lookup result is kept */

typedef enum {
    CH_SIZE = 0,
    CH_EXT,
    CH_DATA,
    CH_AFTER,
    CH_TRAILER,
    CH_END
} chunk_state;

struct http_req {
    int            fd;
    http_state     st;
    char           err[160];

    sbuf           out;             /* the request, as sent */
    size_t         sent;

    sbuf           hdr;             /* response header block */
    sbuf           body;            /* decoded body (capped) */
    int            status;
    int            have_head;
    int            chunked;
    long           clen;            /* Content-Length, or -1 */
    unsigned long  got;             /* body bytes accepted so far (identity) */
    int            complete;        /* framing says the body is finished */
    int            incomplete;      /* connection closed before that */

    chunk_state    cs;
    unsigned long  csize;
    unsigned long  cleft;
    int            line_len;

    unsigned long  last_act;
    unsigned       timeout;
};

/* ------------------------------------------------------------------ */
/* Platform glue                                                      */
/* ------------------------------------------------------------------ */

unsigned long http_clock_cs(void)
{
#ifdef __riscos__
    _kernel_swi_regs regs;

    _kernel_swi(0x42 /* OS_ReadMonotonicTime */, &regs, &regs);
    return (unsigned long)regs.r[0];
#else
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long)ts.tv_sec * 100UL +
           (unsigned long)(ts.tv_nsec / 10000000L);
#endif
}

void http_init(void)
{
    signal(SIGPIPE, SIG_IGN);
}

/* ------------------------------------------------------------------ */
/* Name resolution (cached)                                           */
/* ------------------------------------------------------------------ */

static struct {
    char          host[96];
    unsigned long addr;
    unsigned long when;
    int           valid;
    int           ok;
} resolve_cache;

static int resolve_host(const char *host, struct in_addr *out, char *err,
                        size_t errcap)
{
    in_addr_t a;
    unsigned long now = http_clock_cs();
    struct hostent *he;

    if (host[0] == '\0') {
        snprintf(err, errcap, "no hotspot address set");
        return 0;
    }

    a = inet_addr(host);
    if (a != (in_addr_t)-1) {
        out->s_addr = a;
        return 1;
    }

    if (resolve_cache.valid && strcmp(resolve_cache.host, host) == 0 &&
        (long)(now - resolve_cache.when) < (long)HTTP_RESOLVE_TTL_CS) {
        if (resolve_cache.ok) {
            out->s_addr = (in_addr_t)resolve_cache.addr;
            return 1;
        }

        snprintf(err, errcap, "cannot find host '%s'", host);
        return 0;
    }

    he = gethostbyname(host);

    resolve_cache.valid = 1;
    resolve_cache.when = now;
    u_copy(resolve_cache.host, sizeof resolve_cache.host, host);

    if (he == NULL || he->h_addrtype != AF_INET || he->h_length != 4 ||
        he->h_addr_list == NULL || he->h_addr_list[0] == NULL) {
        resolve_cache.ok = 0;
        snprintf(err, errcap, "cannot find host '%s'", host);
        return 0;
    }

    memcpy(&out->s_addr, he->h_addr_list[0], 4);
    resolve_cache.ok = 1;
    resolve_cache.addr = (unsigned long)out->s_addr;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Request construction                                               */
/* ------------------------------------------------------------------ */

static void fail(http_req *r, const char *msg)
{
    if (r->st == HTTP_FAILED || r->st == HTTP_DONE)
        return;

    u_copy(r->err, sizeof r->err, msg);
    r->st = HTTP_FAILED;

    if (r->fd >= 0) {
        close(r->fd);
        r->fd = -1;
    }
}

static void fail_errno(http_req *r, const char *what, int e)
{
    char msg[160];

    snprintf(msg, sizeof msg, "%s: %s", what, strerror(e));
    fail(r, msg);
}

static int build_request(http_req *r, const http_endpoint *ep,
                         const char *method, const char *target,
                         const char *content_type, const char *body,
                         size_t body_len)
{
    char line[320];
    char cred[110];
    char b64[160];

    snprintf(line, sizeof line, "%s %s HTTP/1.1\r\n", method, target);
    if (sb_addstr(&r->out, line) < 0)
        return 0;

    if (ep->port != 80 && ep->port > 0)
        snprintf(line, sizeof line, "Host: %s:%d\r\n", ep->host, ep->port);
    else
        snprintf(line, sizeof line, "Host: %s\r\n", ep->host);
    sb_addstr(&r->out, line);

    sb_addstr(&r->out, "User-Agent: RISCOS-Hotspot/0.1\r\n");
    sb_addstr(&r->out, "Accept: */*\r\n");
    sb_addstr(&r->out, "Accept-Encoding: identity\r\n");
    sb_addstr(&r->out, "Connection: close\r\n");

    if (ep->user[0] != '\0') {
        snprintf(cred, sizeof cred, "%s:%s", ep->user, ep->pass);
        if (b64_encode((const unsigned char *)cred, strlen(cred), b64,
                       sizeof b64) > 0) {
            snprintf(line, sizeof line, "Authorization: Basic %s\r\n", b64);
            sb_addstr(&r->out, line);
        }
    }

    if (content_type != NULL) {
        snprintf(line, sizeof line,
                 "Content-Type: %s\r\nContent-Length: %lu\r\n", content_type,
                 (unsigned long)body_len);
        sb_addstr(&r->out, line);
    }

    sb_addstr(&r->out, "\r\n");

    if (body != NULL && body_len > 0) {
        if (sb_add(&r->out, body, body_len) < 0)
            return 0;
    }

    return r->out.len > 0;
}

http_req *http_start(const http_endpoint *ep, const char *method,
                     const char *target, const char *content_type,
                     const char *body, size_t body_len, size_t max_body,
                     unsigned timeout_cs)
{
    http_req *r = (http_req *)calloc(1, sizeof *r);
    struct sockaddr_in sa;
    int one = 1;
    int rc;

    if (r == NULL)
        return NULL;

    r->fd = -1;
    r->st = HTTP_CONNECTING;
    r->clen = -1;
    r->timeout = timeout_cs;
    r->last_act = http_clock_cs();
    sb_init(&r->out, 0);
    sb_init(&r->hdr, 0);
    sb_init(&r->body, max_body);

    if (!build_request(r, ep, method, target, content_type, body, body_len)) {
        fail(r, "out of memory");
        return r;
    }

    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)((ep->port > 0) ? ep->port : 80));

    if (!resolve_host(ep->host, &sa.sin_addr, r->err, sizeof r->err)) {
        r->st = HTTP_FAILED;
        return r;
    }

    r->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (r->fd < 0) {
        fail_errno(r, "socket", errno);
        return r;
    }

    if (ioctl(r->fd, FIONBIO, &one) < 0) {
        fail_errno(r, "ioctl", errno);
        return r;
    }

    rc = connect(r->fd, (struct sockaddr *)&sa, sizeof sa);
    if (rc == 0) {
        r->st = HTTP_SENDING;
    } else if (errno != EINPROGRESS && errno != EWOULDBLOCK &&
               errno != EALREADY && errno != EINTR) {
        fail_errno(r, "connect", errno);
    }

    return r;
}

/* ------------------------------------------------------------------ */
/* Response parsing                                                   */
/* ------------------------------------------------------------------ */

static int hexval(unsigned char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;

    return -1;
}

static void chunk_line_end(http_req *r)
{
    if (r->csize == 0) {
        r->cs = CH_TRAILER;
        r->line_len = 0;
    } else {
        r->cleft = r->csize;
        r->cs = CH_DATA;
    }
}

static void body_feed(http_req *r, const char *d, size_t n)
{
    size_t i = 0;

    if (!r->chunked) {
        if (r->clen >= 0) {
            unsigned long want = ((unsigned long)r->clen > r->got)
                                     ? (unsigned long)r->clen - r->got
                                     : 0;

            if ((unsigned long)n > want)
                n = (size_t)want;
        }

        sb_add(&r->body, d, n);
        r->got += (unsigned long)n;

        if (r->clen >= 0 && r->got >= (unsigned long)r->clen)
            r->complete = 1;

        return;
    }

    while (i < n && r->cs != CH_END) {
        unsigned char c = (unsigned char)d[i];

        switch (r->cs) {
            case CH_SIZE: {
                int v = hexval(c);

                if (v >= 0) {
                    if (r->csize > 0x07FFFFFFUL) {
                        fail(r, "bad chunk size");
                        return;
                    }
                    r->csize = r->csize * 16 + (unsigned long)v;
                } else if (c == ';') {
                    r->cs = CH_EXT;
                } else if (c == '\n') {
                    chunk_line_end(r);
                }
                i++;
                break;
            }

            case CH_EXT:
                if (c == '\n')
                    chunk_line_end(r);
                i++;
                break;

            case CH_DATA: {
                size_t take = n - i;

                if ((unsigned long)take > r->cleft)
                    take = (size_t)r->cleft;

                sb_add(&r->body, d + i, take);
                r->got += (unsigned long)take;
                i += take;
                r->cleft -= (unsigned long)take;

                if (r->cleft == 0)
                    r->cs = CH_AFTER;
                break;
            }

            case CH_AFTER:
                if (c == '\n') {
                    r->cs = CH_SIZE;
                    r->csize = 0;
                }
                i++;
                break;

            case CH_TRAILER:
                if (c == '\n') {
                    if (r->line_len == 0)
                        r->cs = CH_END;
                    else
                        r->line_len = 0;
                } else if (c != '\r') {
                    r->line_len++;
                }
                i++;
                break;

            case CH_END:
                break;
        }
    }

    if (r->cs == CH_END)
        r->complete = 1;
}

/* Parses the header block in r->hdr (which ends with the blank line).
 * Returns 0 on success, -1 if it is not an HTTP response. */
static int parse_head(http_req *r)
{
    const char *h = sb_str(&r->hdr);
    size_t n = r->hdr.len;
    const char *sp;
    const char *p;

    if (n < 12 || memcmp(h, "HTTP/", 5) != 0)
        return -1;

    sp = memchr(h, ' ', n);
    if (sp == NULL)
        return -1;

    r->status = atoi(sp + 1);
    if (r->status < 100)
        return -1;

    r->chunked = 0;
    r->clen = -1;

    p = memchr(h, '\n', n);
    while (p != NULL && (size_t)(p - h) + 1 < n) {
        char line[160];
        const char *ls = p + 1;
        const char *le = memchr(ls, '\n', n - (size_t)(ls - h));
        size_t ll = (le != NULL) ? (size_t)(le - ls)
                                 : n - (size_t)(ls - h);

        u_copyn(line, sizeof line, ls, ll);

        if (u_istarts(line, "content-length:"))
            r->clen = atol(line + 15);
        else if (u_istarts(line, "transfer-encoding:") &&
                 u_ifind(line, strlen(line), "chunked") != NULL)
            r->chunked = 1;

        p = le;
    }

    if (r->chunked)
        r->clen = -1;

    /* Responses with no body at all. */
    if (r->status == 204 || r->status == 304)
        r->complete = 1;
    if (r->clen == 0 && !r->chunked)
        r->complete = 1;

    return 0;
}

/* Accepts bytes straight off the socket: until the blank line that ends the
 * header block they go into r->hdr, after it they are body. */
static void feed(http_req *r, const char *d, size_t n)
{
    const char *h;
    const char *term = NULL;
    size_t term_len = 0;
    size_t before;
    size_t from;
    size_t head_len;
    size_t extra;
    char *copy = NULL;
    int rc;

    if (r->have_head) {
        body_feed(r, d, n);
        return;
    }

    before = r->hdr.len;
    rc = sb_add(&r->hdr, d, n);
    if (rc != 0) {
        fail(r, "out of memory");
        return;
    }

    h = sb_str(&r->hdr);
    from = (before > 3) ? before - 3 : 0;

    term = u_find(h + from, r->hdr.len - from, "\r\n\r\n");
    if (term != NULL) {
        term_len = 4;
    } else {
        term = u_find(h + from, r->hdr.len - from, "\n\n");
        if (term != NULL)
            term_len = 2;
    }

    if (term == NULL) {
        if (r->hdr.len > HTTP_HEADER_MAX)
            fail(r, "response header too large");
        return;
    }

    /* Whatever follows the blank line is body (or another response). */
    head_len = (size_t)(term - h) + term_len;
    extra = r->hdr.len - head_len;

    if (extra > 0) {
        copy = (char *)malloc(extra);
        if (copy == NULL) {
            fail(r, "out of memory");
            return;
        }
        memcpy(copy, h + head_len, extra);
    }

    r->hdr.len = head_len;
    r->hdr.p[head_len] = '\0';

    if (parse_head(r) < 0) {
        free(copy);
        fail(r, "not an HTTP response");
        return;
    }

    if (r->status >= 100 && r->status < 200 && r->status != 101) {
        /* Interim response such as 100 Continue: drop it and carry on with
         * the real one, which may already be in the bytes we just split off. */
        sb_reset(&r->hdr);
        r->complete = 0;
        if (copy != NULL)
            feed(r, copy, extra);
        free(copy);
        return;
    }

    r->have_head = 1;

    if (copy != NULL && !r->complete)
        body_feed(r, copy, extra);

    free(copy);
}

/* ------------------------------------------------------------------ */
/* The state machine                                                  */
/* ------------------------------------------------------------------ */

static void finish(http_req *r)
{
    if (r->st == HTTP_FAILED || r->st == HTTP_DONE)
        return;

    r->st = HTTP_DONE;
    if (r->fd >= 0) {
        close(r->fd);
        r->fd = -1;
    }
}

static void handle_eof(http_req *r)
{
    if (!r->have_head) {
        fail(r, "connection closed before a response arrived");
        return;
    }

    if (!r->complete &&
        (r->chunked || (r->clen >= 0 && r->got < (unsigned long)r->clen)))
        r->incomplete = 1;

    finish(r);
}

http_state http_step(http_req *r)
{
    unsigned long now;

    if (r->st == HTTP_DONE || r->st == HTTP_FAILED)
        return r->st;

    now = http_clock_cs();

    if (r->st == HTTP_CONNECTING) {
        fd_set wf;
        fd_set ef;
        struct timeval tv;
        int rc;

        FD_ZERO(&wf);
        FD_ZERO(&ef);
        FD_SET(r->fd, &wf);
        FD_SET(r->fd, &ef);
        tv.tv_sec = 0;
        tv.tv_usec = 0;

        rc = select(r->fd + 1, NULL, &wf, &ef, &tv);
        if (rc > 0) {
            int soerr = 0;
            socklen_t sl = (socklen_t)sizeof soerr;

            if (getsockopt(r->fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) < 0)
                soerr = errno;

            if (soerr != 0)
                fail_errno(r, "connect", soerr);
            else if (FD_ISSET(r->fd, &wf)) {
                r->st = HTTP_SENDING;
                r->last_act = now;
            } else
                fail(r, "connection failed");
        } else if (rc < 0 && errno != EINTR) {
            fail_errno(r, "select", errno);
        }
    }

    if (r->st == HTTP_SENDING) {
        while (r->sent < r->out.len) {
            int flags = 0;
            ssize_t n;

#ifdef MSG_NOSIGNAL
            flags = MSG_NOSIGNAL;
#endif
            n = send(r->fd, r->out.p + r->sent, r->out.len - r->sent, flags);
            if (n > 0) {
                r->sent += (size_t)n;
                r->last_act = now;
                continue;
            }

            if (n < 0 && (errno == EWOULDBLOCK || errno == EAGAIN ||
                          errno == EINTR))
                break;

            fail_errno(r, "send", (n < 0) ? errno : EPIPE);
            break;
        }

        if (r->st == HTTP_SENDING && r->sent >= r->out.len)
            r->st = HTTP_RECEIVING;
    }

    if (r->st == HTTP_RECEIVING) {
        char tmp[HTTP_RECV_CHUNK];
        int iter;

        for (iter = 0; iter < HTTP_RECV_PER_STEP &&
                       r->st == HTTP_RECEIVING; iter++) {
            ssize_t n = recv(r->fd, tmp, sizeof tmp, 0);

            if (n > 0) {
                r->last_act = now;
                feed(r, tmp, (size_t)n);

                if (r->st == HTTP_RECEIVING && r->have_head && r->complete)
                    finish(r);
                continue;
            }

            if (n == 0) {
                handle_eof(r);
                break;
            }

            if (errno == EWOULDBLOCK || errno == EAGAIN)
                break;
            if (errno == EINTR)
                continue;

            if (errno == ECONNRESET && r->have_head) {
                handle_eof(r);
                r->incomplete = 1;
            } else {
                fail_errno(r, "receive", errno);
            }
            break;
        }
    }

    if (r->st == HTTP_RECEIVING && r->have_head && r->complete)
        finish(r);

    if (r->st != HTTP_DONE && r->st != HTTP_FAILED &&
        (long)(now - r->last_act) > (long)r->timeout)
        fail(r, "timed out waiting for the hotspot");

    return r->st;
}

/* ------------------------------------------------------------------ */
/* Accessors                                                          */
/* ------------------------------------------------------------------ */

int http_status(const http_req *r)
{
    return r->have_head ? r->status : 0;
}

const char *http_body(const http_req *r, size_t *len)
{
    if (len != NULL)
        *len = r->body.len;

    return sb_str(&r->body);
}

const char *http_error(const http_req *r)
{
    return r->err;
}

const char *http_headers(const http_req *r)
{
    return sb_str(&r->hdr);
}

int http_truncated(const http_req *r)
{
    return r->body.truncated;
}

int http_incomplete(const http_req *r)
{
    return r->incomplete;
}

int http_got_response(const http_req *r)
{
    return r->have_head;
}

void http_free(http_req *r)
{
    if (r == NULL)
        return;

    if (r->fd >= 0)
        close(r->fd);

    sb_free(&r->out);
    sb_free(&r->hdr);
    sb_free(&r->body);
    free(r);
}
