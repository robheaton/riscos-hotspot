/*
 * scan.c
 *
 * See scan.h.
 */

#include "scan.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http.h"
#include "util.h"
#include "wpsd.h"

#define SCAN_PARALLEL       12
#define SCAN_TIMEOUT_CS     120u        /* give up on one host after this
                                         * long without a byte from it... */
#define SCAN_DEADLINE_CS    800u        /* ...or after this long in all, however
                                         * slowly it keeps on answering */
#define SCAN_MAX_BODY       (32u * 1024u)
#define SCAN_MAX_FOUND      8

struct hs_scan {
    char        prefix[20];
    int         port;
    int         next;                   /* next host number to start */
    int         done;
    http_req   *slot[SCAN_PARALLEL];
    int         host[SCAN_PARALLEL];
    unsigned long started[SCAN_PARALLEL];
    int         nfound;
    struct {
        char addr[24];
        char info[64];
    } found[SCAN_MAX_FOUND];
    wpsd_radio  radio;                  /* scratch for parsing replies */
};

int scan_prefix_from(const char *text, char *out, size_t cap)
{
    int nums[4];
    int count = 0;
    const char *p = text;

    if (cap < 16)
        return 0;

    while (*p == ' ' || *p == '\t')
        p++;

    while (*p != '\0') {
        int value = 0;
        int digits = 0;

        if (!isdigit((unsigned char)*p))
            return 0;

        while (isdigit((unsigned char)*p)) {
            value = value * 10 + (*p - '0');
            if (++digits > 3 || value > 255)
                return 0;
            p++;
        }

        if (count >= 4)
            return 0;               /* more than four numbers */
        nums[count++] = value;

        if (*p == '.') {
            p++;
            if (*p == '\0')
                break;              /* a trailing dot is fine */
            continue;
        }

        break;
    }

    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    if (*p != '\0')
        return 0;

    if (count < 3)
        return 0;

    snprintf(out, cap, "%d.%d.%d.", nums[0], nums[1], nums[2]);
    return 1;
}

hs_scan *scan_new(const char *prefix, int port)
{
    hs_scan *s;
    char clean[20];

    if (!scan_prefix_from(prefix, clean, sizeof clean))
        return NULL;

    s = (hs_scan *)calloc(1, sizeof *s);
    if (s == NULL)
        return NULL;

    u_copy(s->prefix, sizeof s->prefix, clean);
    s->port = (port > 0 && port < 65536) ? port : 80;
    s->next = SCAN_FIRST_HOST;
    return s;
}

static void note_result(hs_scan *s, int host, const http_req *r)
{
    size_t len = 0;
    const char *body = http_body(r, &len);
    int i;
    const char *info = NULL;

    if (http_status(r) != 200 || !wpsd_parse_radio(body, len, &s->radio))
        return;

    if (s->nfound >= SCAN_MAX_FOUND)
        return;

    for (i = 0; i < s->radio.n; i++) {
        if (strcmp(s->radio.kv[i].key, "Modem Type") == 0)
            info = s->radio.kv[i].val;
    }

    if (info == NULL) {
        for (i = 0; i < s->radio.n; i++) {
            if (strstr(s->radio.kv[i].key, "Freq") != NULL)
                info = s->radio.kv[i].val;
        }
    }

    {
        char addr[32];

        snprintf(addr, sizeof addr, "%s%d", s->prefix, host);
        u_copy(s->found[s->nfound].addr, sizeof s->found[s->nfound].addr,
               addr);
    }

    u_copy(s->found[s->nfound].info, sizeof s->found[s->nfound].info,
           (info != NULL) ? info : s->radio.state);
    s->nfound++;
}

int scan_step(hs_scan *s)
{
    int i;

    for (i = 0; i < SCAN_PARALLEL; i++) {
        if (s->slot[i] != NULL) {
            http_state st = http_step(s->slot[i]);
            int late = (long)(http_clock_cs() - s->started[i]) >
                       (long)SCAN_DEADLINE_CS;

            if (st == HTTP_DONE || st == HTTP_FAILED || late) {
                if (st == HTTP_DONE)
                    note_result(s, s->host[i], s->slot[i]);

                http_free(s->slot[i]);
                s->slot[i] = NULL;
                s->done++;
            }
        }

        if (s->slot[i] == NULL && s->next <= SCAN_LAST_HOST) {
            http_endpoint ep;
            char addr[32];

            memset(&ep, 0, sizeof ep);
            snprintf(addr, sizeof addr, "%s%d", s->prefix, s->next);
            u_copy(ep.host, sizeof ep.host, addr);
            ep.port = s->port;

            s->host[i] = s->next++;
            s->started[i] = http_clock_cs();
            s->slot[i] = http_start(&ep, "GET", "/mmdvmhost/radioinfo.php",
                                    NULL, NULL, 0, SCAN_MAX_BODY,
                                    SCAN_TIMEOUT_CS);
            if (s->slot[i] == NULL)
                s->done++;              /* out of memory: skip this host */
        }
    }

    return s->done >= SCAN_HOSTS;
}

void scan_progress(const hs_scan *s, int *done, int *total, int *found)
{
    if (done != NULL)
        *done = s->done;
    if (total != NULL)
        *total = SCAN_HOSTS;
    if (found != NULL)
        *found = s->nfound;
}

int scan_result(const hs_scan *s, int i, char *addr, size_t acap, char *info,
                size_t icap)
{
    if (i < 0 || i >= s->nfound)
        return 0;

    u_copy(addr, acap, s->found[i].addr);
    u_copy(info, icap, s->found[i].info);
    return 1;
}

void scan_free(hs_scan *s)
{
    int i;

    if (s == NULL)
        return;

    for (i = 0; i < SCAN_PARALLEL; i++) {
        if (s->slot[i] != NULL)
            http_free(s->slot[i]);
    }

    free(s);
}
