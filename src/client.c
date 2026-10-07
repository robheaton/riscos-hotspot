/*
 * client.c
 *
 * See client.h.
 */

#include "client.h"
#include "html.h"
#include "scan.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

#define HS_QUEUE            24
#define HS_FRAGMENT_MAX     (256u * 1024u)
#define HS_PAGE_MAX         (512u * 1024u)
#define HS_DIAG_BODY_MAX    (24u * 1024u)

/* Timings are in centiseconds. The host tests override these to keep the
 * timeout cases quick. */
#ifndef HS_FETCH_TIMEOUT
#define HS_FETCH_TIMEOUT    1000u       /* silence before giving up on a fetch */
#endif
#ifndef HS_FORM_TIMEOUT
#define HS_FORM_TIMEOUT     6000u       /* ... on a form post */
#endif
#ifndef HS_SYS_TIMEOUT
#define HS_SYS_TIMEOUT      12000u      /* ... on a service restart etc. */
#endif
#ifndef HS_AFTER_ACTION_CS
#define HS_AFTER_ACTION_CS  300UL       /* wait before re-reading state */
#endif

typedef enum {
    JOB_RADIO = 0,
    JOB_HEARD,
    JOB_STATUS,
    JOB_HW,
    JOB_BM,
    JOB_TGIF,
    JOB_DMRNET,
    JOB_YSFLIST,
    JOB_ACTION,
    JOB_KINDS
} job_kind;

static const char *const job_names[JOB_KINDS] = {
    "radio", "last heard", "status", "system", "brandmeister", "tgif",
    "dmr networks", "ysf reflectors", "action"
};

typedef struct {
    job_kind     kind;
    wpsd_request rq;
    hs_action    act;
    unsigned     timeout_cs;
    size_t       max_body;
} hs_job;

typedef struct {
    int           have;
    char          req[300];
    int           status;
    char          err[160];
    char          head[600];
    size_t        len;
    size_t        offset;       /* where in the reply the kept part starts */
    char         *body;
    int           truncated;
    unsigned long when;
} hs_diag;

struct hs_client {
    hs_config      cfg;
    hs_model       model;

    hs_job         queue[HS_QUEUE];     /* queue[0] is next; kept packed */
    int            qcount;

    hs_job         cur;
    http_req      *req;

    int            polling;
    unsigned long  next_poll;
    unsigned       cycle;

    int            refresh_pending;
    unsigned long  refresh_at;
    unsigned       after_mask;          /* extra parts to re-read after an action */

    unsigned       focus;               /* what the visible tab wants kept fresh */
    hs_scan       *scan;                /* a running search for a hotspot */

    hs_diag        diag[JOB_KINDS];
};

/* ------------------------------------------------------------------ */
/* Configuration                                                      */
/* ------------------------------------------------------------------ */

void hs_config_defaults(hs_config *cfg)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->port = 80;
    u_copy(cfg->user, sizeof cfg->user, "pi-star");
    cfg->refresh_s = 5;
    cfg->heard_rows = 15;
    cfg->show_bm = 1;
    cfg->names = 0;
}

void hs_mru_push(hs_config *cfg, int proto, const char *target)
{
    char clean[HS_MRU_LEN];
    int at = HS_MRU_N - 1;
    int i;

    if (proto < 0 || proto >= HS_MRU_PROTOS || target == NULL)
        return;

    u_copy(clean, sizeof clean, target);
    u_trim(clean);
    if (clean[0] == '\0')
        return;

    for (i = 0; i < HS_MRU_N; i++) {
        if (u_ieq(cfg->recent[proto][i], clean)) {
            at = i;
            break;
        }
    }

    for (i = at; i > 0; i--)
        memcpy(cfg->recent[proto][i], cfg->recent[proto][i - 1], HS_MRU_LEN);

    u_copy(cfg->recent[proto][0], HS_MRU_LEN, clean);
}

static void sanitise(hs_config *cfg)
{
    u_trim(cfg->host);
    u_trim(cfg->user);

    if (cfg->port < 1 || cfg->port > 65535)
        cfg->port = 80;
    if (cfg->refresh_s < 2)
        cfg->refresh_s = 2;
    if (cfg->refresh_s > 300)
        cfg->refresh_s = 300;
    if (cfg->heard_rows < 1)
        cfg->heard_rows = 1;
    if (cfg->heard_rows > WPSD_MAX_HEARD)
        cfg->heard_rows = WPSD_MAX_HEARD;
}

hs_client *hs_new(const hs_config *cfg)
{
    hs_client *hs = (hs_client *)calloc(1, sizeof *hs);

    if (hs == NULL)
        return NULL;

    if (cfg != NULL)
        hs->cfg = *cfg;
    else
        hs_config_defaults(&hs->cfg);

    sanitise(&hs->cfg);
    return hs;
}

static void clear_diag(hs_client *hs)
{
    int i;

    for (i = 0; i < JOB_KINDS; i++) {
        free(hs->diag[i].body);
        memset(&hs->diag[i], 0, sizeof hs->diag[i]);
    }
}

void hs_free(hs_client *hs)
{
    if (hs == NULL)
        return;

    if (hs->req != NULL)
        http_free(hs->req);

    scan_free(hs->scan);
    clear_diag(hs);
    wpsd_ysflist_free(&hs->model.ysf);
    free(hs);
}

void hs_set_config(hs_client *hs, const hs_config *cfg)
{
    int moved = strcmp(hs->cfg.host, cfg->host) != 0 ||
                hs->cfg.port != cfg->port ||
                strcmp(hs->cfg.user, cfg->user) != 0 ||
                strcmp(hs->cfg.pass, cfg->pass) != 0;

    hs->cfg = *cfg;
    sanitise(&hs->cfg);

    if (moved) {
        /* Don't keep showing another hotspot's state. */
        wpsd_ysflist_free(&hs->model.ysf);
        memset(&hs->model, 0, sizeof hs->model);
        hs->qcount = 0;
        if (hs->req != NULL) {
            http_free(hs->req);
            hs->req = NULL;
        }
        scan_free(hs->scan);
        hs->scan = NULL;
        hs->after_mask = 0;
        clear_diag(hs);
        hs->cycle = 0;
        hs->next_poll = 0;
    }
}

const hs_config *hs_get_config(const hs_client *hs)
{
    return &hs->cfg;
}

const hs_model *hs_get(const hs_client *hs)
{
    return &hs->model;
}

int hs_busy(const hs_client *hs)
{
    return hs->req != NULL || hs->qcount > 0 || hs->scan != NULL;
}

/* ------------------------------------------------------------------ */
/* Queue                                                              */
/* ------------------------------------------------------------------ */

static int queue_push(hs_client *hs, const hs_job *job)
{
    if (hs->qcount >= HS_QUEUE)
        return -1;

    hs->queue[hs->qcount++] = *job;
    return 0;
}

static int queue_has(const hs_client *hs, job_kind kind)
{
    int i;

    for (i = 0; i < hs->qcount; i++) {
        if (hs->queue[i].kind == kind)
            return 1;
    }

    return 0;
}

/* Takes the next job off the front. (The queue is short, so shifting it down
 * is cheaper than the bookkeeping of a ring - and keeps big job structures
 * off the stack, which on RISC OS is a small, fixed-size region.) */
static void queue_pop(hs_client *hs, hs_job *job)
{
    *job = hs->queue[0];
    hs->qcount--;
    memmove(&hs->queue[0], &hs->queue[1],
            (size_t)hs->qcount * sizeof hs->queue[0]);
}

/* Drops waiting fetches (not actions) - used when the hotspot is not
 * answering, so one dead poll does not cost several timeouts. */
static void queue_drop_fetches(hs_client *hs)
{
    int kept = 0;
    int i;

    for (i = 0; i < hs->qcount; i++) {
        if (hs->queue[i].kind == JOB_ACTION) {
            if (kept != i)
                hs->queue[kept] = hs->queue[i];
            kept++;
        }
    }

    hs->qcount = kept;
}

static int make_fetch(const hs_client *hs, job_kind kind, hs_job *job)
{
    int rc = -1;

    memset(job, 0, sizeof *job);
    job->kind = kind;
    job->timeout_cs = HS_FETCH_TIMEOUT;
    job->max_body = HS_FRAGMENT_MAX;

    switch (kind) {
        case JOB_RADIO:
            rc = wpsd_req_radio(&job->rq);
            break;
        case JOB_HEARD:
            rc = wpsd_req_heard(&job->rq, hs->cfg.heard_rows, hs->cfg.names);
            break;
        case JOB_STATUS:
            rc = wpsd_req_status(&job->rq);
            break;
        case JOB_HW:
            rc = wpsd_req_hw(&job->rq);
            break;
        case JOB_BM:
            /* Switched off in Choices: never ask for it, whoever wants it. */
            if (hs->cfg.show_bm)
                rc = wpsd_req_bm_page(&job->rq);
            break;
        case JOB_TGIF:
            rc = wpsd_req_tgif_links(&job->rq);
            break;
        case JOB_DMRNET:
            /* The whole admin page: big, and slow on a small hotspot. */
            rc = wpsd_req_dmrnets(&job->rq);
            job->timeout_cs = HS_FORM_TIMEOUT;
            job->max_body = HS_PAGE_MAX;
            break;
        case JOB_YSFLIST:
            /* Likewise: the admin page with the hundreds of reflectors. */
            rc = wpsd_req_ysflist(&job->rq);
            job->timeout_cs = HS_FORM_TIMEOUT;
            job->max_body = HS_PAGE_MAX;
            break;
        default:
            break;
    }

    return rc;
}

void hs_refresh(hs_client *hs, unsigned mask)
{
    static const struct {
        unsigned  bit;
        job_kind  kind;
    } order[] = {
        { HS_R_RADIO,  JOB_RADIO },
        { HS_R_HEARD,  JOB_HEARD },
        { HS_R_STATUS, JOB_STATUS },
        { HS_R_HW,     JOB_HW },
        { HS_R_BM,     JOB_BM },
        { HS_R_TGIF,   JOB_TGIF },
        { HS_R_DMRNET, JOB_DMRNET },
        { HS_R_YSF,    JOB_YSFLIST }
    };
    size_t i;

    for (i = 0; i < sizeof order / sizeof order[0]; i++) {
        hs_job job;

        if (!(mask & order[i].bit))
            continue;
        if (queue_has(hs, order[i].kind))
            continue;
        if (make_fetch(hs, order[i].kind, &job) < 0)
            continue;

        queue_push(hs, &job);
    }
}

int hs_do(hs_client *hs, const hs_action *act)
{
    hs_job job;
    int rc = -1;
    wpsd_request *rq;

    memset(&job, 0, sizeof job);
    job.kind = JOB_ACTION;
    job.act = *act;
    job.timeout_cs = HS_FORM_TIMEOUT;
    job.max_body = HS_PAGE_MAX;
    rq = &job.rq;

    switch (act->kind) {
        case HS_ACT_MODE:
            rc = wpsd_req_mode(rq, act->s1, act->i3);
            break;
        case HS_ACT_SYS:
            rc = wpsd_req_sysapi(rq, act->s1);
            job.timeout_cs = HS_SYS_TIMEOUT;
            break;
        case HS_ACT_BM_LINK:
            rc = wpsd_req_bm_link(rq, act->i1, act->i2, act->i3);
            break;
        case HS_ACT_BM_DROP_DYN:
            rc = wpsd_req_bm_drop_dynamic(rq, act->i2);
            break;
        case HS_ACT_BM_DROP_QSO:
            rc = wpsd_req_bm_drop_qso(rq, act->i2);
            break;
        case HS_ACT_BM_ADD:
            rc = wpsd_req_bm_add(rq, act->s1, act->i2);
            break;
        case HS_ACT_BM_DELETE:
            rc = wpsd_req_bm_delete(rq, act->i1, act->i2);
            break;
        case HS_ACT_YSF:
            rc = wpsd_req_ysf(rq, act->s1, act->i3);
            break;
        case HS_ACT_P25:
            rc = wpsd_req_p25(rq, act->s1, act->i3);
            break;
        case HS_ACT_NXDN:
            rc = wpsd_req_nxdn(rq, act->s1, act->i3);
            break;
        case HS_ACT_DSTAR:
            rc = wpsd_req_dstar(rq, act->s1, act->s2, act->s3, act->i3);
            break;
        case HS_ACT_TGIF:
            rc = wpsd_req_tgif(rq, act->i1, act->i2, act->i3);
            break;
        case HS_ACT_DMRNET:
            rc = wpsd_req_dmrnet_set(rq, act->s1, act->i3);
            break;
    }

    if (rc < 0)
        return -1;

    return queue_push(hs, &job);
}

/* ------------------------------------------------------------------ */
/* Diagnostics                                                        */
/* ------------------------------------------------------------------ */

static void record_diag(hs_client *hs, const hs_job *job, const http_req *r,
                        http_state st, unsigned long now)
{
    hs_diag *d = &hs->diag[job->kind];
    size_t len = 0;
    const char *body = http_body(r, &len);
    const char *hdr = http_headers(r);

    free(d->body);
    memset(d, 0, sizeof *d);

    d->have = 1;
    d->when = now;
    d->status = http_status(r);
    snprintf(d->req, sizeof d->req, "%s %s", job->rq.method, job->rq.target);

    if (st == HTTP_FAILED)
        u_copy(d->err, sizeof d->err, http_error(r));

    u_copy(d->head, sizeof d->head, hdr);

    if (len > HS_DIAG_BODY_MAX) {
        /* The big admin pages (the DMR networks, the YSF reflectors) have
         * what matters deep inside them, long after their style sheets and
         * menus: keep the part around it, not the start. */
        const char *marker = NULL;

        if (job->kind == JOB_DMRNET)
            marker = u_find(body, len, "dmr-net-row");
        else if (job->kind == JOB_YSFLIST)
            marker = u_find(body, len, "ysfLinkHost");

        if (marker != NULL) {
            size_t at = (size_t)(marker - body);

            d->offset = (at > 2000) ? at - 2000 : 0;
            if (d->offset + HS_DIAG_BODY_MAX > len)
                d->offset = len - HS_DIAG_BODY_MAX;
            body += d->offset;
            len -= d->offset;
        }

        if (len > HS_DIAG_BODY_MAX)
            len = HS_DIAG_BODY_MAX;
        d->truncated = 1;
    }
    if (http_truncated(r))
        d->truncated = 1;

    d->body = (char *)malloc(len + 1);
    if (d->body != NULL) {
        memcpy(d->body, body, len);
        d->body[len] = '\0';
        d->len = len;
    }
}

int hs_request_summary(const hs_client *hs, char lines[][112], int max)
{
    static const char *const title[JOB_ACTION] = {
        "Radio", "Last heard", "Status", "System", "BrandMeister", "TGIF",
        "DMR networks", "YSF reflectors"
    };
    const hs_model *m = &hs->model;
    int n = 0;
    int k;

    for (k = 0; k < JOB_ACTION && n < max; k++) {
        const hs_diag *d = &hs->diag[k];
        char how[40];
        char made[44];

        if (!d->have) {
            /* The big pages are only read when their tab is shown. */
            if (k != JOB_DMRNET && k != JOB_YSFLIST)
                snprintf(lines[n++], 112, "%s: not asked for yet", title[k]);
            continue;
        }

        if (d->err[0] != '\0')
            snprintf(how, sizeof how, "%.36s", d->err);
        else
            snprintf(how, sizeof how, "HTTP %d, %.1f KB", d->status,
                     (double)d->len / 1024.0);

        switch (k) {
            case JOB_RADIO:
                snprintf(made, sizeof made, "%s",
                         m->radio_ok ? "understood" : "not understood");
                break;
            case JOB_HEARD:
                if (m->heard_ok)
                    snprintf(made, sizeof made, "%d entries", m->heard.n);
                else
                    snprintf(made, sizeof made, "not understood");
                break;
            case JOB_STATUS:
                if (m->status_ok)
                    snprintf(made, sizeof made, "%d items in %d sections",
                             m->status.npill, m->status.nsec);
                else
                    snprintf(made, sizeof made, "not understood");
                break;
            case JOB_HW:
                if (m->hw_ok)
                    snprintf(made, sizeof made, "%d items", m->hw.n);
                else
                    snprintf(made, sizeof made, "not understood");
                break;
            case JOB_TGIF:
                if (m->tgif_state == 1)
                    snprintf(made, sizeof made, "slot 1 TG%d, slot 2 TG%d",
                             m->tgif.tg[0], m->tgif.tg[1]);
                else
                    snprintf(made, sizeof made, "TGIF not in use");
                break;
            case JOB_DMRNET:
                if (m->dmrnet_state == 1)
                    snprintf(made, sizeof made, "%d networks",
                             m->dmrnets.n);
                else if (m->dmrnet_state == -2)
                    snprintf(made, sizeof made, "login refused");
                else
                    snprintf(made, sizeof made, "no network switches");
                break;
            case JOB_YSFLIST:
                if (m->ysf_state == 1)
                    snprintf(made, sizeof made, "%d reflectors", m->ysf.n);
                else if (m->ysf_state == -2)
                    snprintf(made, sizeof made, "login refused");
                else
                    snprintf(made, sizeof made, "no reflector list found");
                break;
            default:
                if (m->bm_state == 1)
                    snprintf(made, sizeof made, "%d static, %d dynamic",
                             m->bm.nst, m->bm.ndyn);
                else if (m->bm_state == 2)
                    snprintf(made, sizeof made, "hotspot says: not set up");
                else if (m->bm_state == -2)
                    snprintf(made, sizeof made, "login refused");
                else
                    snprintf(made, sizeof made, "not a BrandMeister page");
                break;
        }

        snprintf(lines[n++], 112, "%s: %s - %s", title[k], how, made);
    }

    return n;
}

void hs_write_diagnostics(const hs_client *hs, FILE *f)
{
    const hs_model *m = &hs->model;
    int i;

    fprintf(f, "Hotspot client diagnostics\n");
    fprintf(f, "==========================\n\n");
    fprintf(f, "host=%s port=%d user=%s password=%s\n", hs->cfg.host,
            hs->cfg.port, hs->cfg.user,
            hs->cfg.pass[0] != '\0' ? "(set)" : "(empty)");
    fprintf(f, "refresh=%ds heard_rows=%d show_bm=%d names=%d\n\n",
            hs->cfg.refresh_s, hs->cfg.heard_rows, hs->cfg.show_bm,
            hs->cfg.names);

    fprintf(f, "conn=%d (%s) auth_failed=%d\n", m->conn, m->conn_msg,
            m->auth_failed);
    fprintf(f, "radio_ok=%d hw_ok=%d status_ok=%d (%d pills, %d sections) "
               "heard_ok=%d (%d rows) bm_state=%d\n\n",
            m->radio_ok, m->hw_ok, m->status_ok, m->status.npill,
            m->status.nsec, m->heard_ok, m->heard.n, m->bm_state);

    for (i = 0; i < JOB_KINDS; i++) {
        const hs_diag *d = &hs->diag[i];

        fprintf(f, "---- %s ----\n", job_names[i]);
        if (!d->have) {
            fprintf(f, "(not requested yet)\n\n");
            continue;
        }

        fprintf(f, "request: %s\nstatus: %d\n", d->req, d->status);
        if (d->err[0] != '\0')
            fprintf(f, "error: %s\n", d->err);
        if (d->head[0] != '\0')
            fprintf(f, "headers:\n%s\n", d->head);
        fprintf(f, "body (%lu bytes%s", (unsigned long)d->len,
                d->truncated ? ", truncated" : "");
        if (d->offset > 0)
            fprintf(f, ", the part from byte %lu", (unsigned long)d->offset);
        fprintf(f, "):\n");
        if (d->body != NULL)
            fwrite(d->body, 1, d->len, f);
        fprintf(f, "\n\n");
    }
}

/* ------------------------------------------------------------------ */
/* Job completion                                                     */
/* ------------------------------------------------------------------ */

/* Copies the value of response header `name` (e.g. "Location") into `out`,
 * or leaves it empty. */
static void header_value(const char *hdr, const char *name, char *out,
                         size_t cap)
{
    size_t nl = strlen(name);
    const char *p = hdr;

    out[0] = '\0';

    while (*p != '\0') {
        const char *eol = strchr(p, '\n');
        size_t len = (eol != NULL) ? (size_t)(eol - p) : strlen(p);

        if (len > nl && u_istarts(p, name) && p[nl] == ':') {
            const char *v = p + nl + 1;

            while (*v == ' ' || *v == '\t')
                v++;

            u_copyn(out, cap, v, len - (size_t)(v - p));
            u_trim(out);
            return;
        }

        if (eol == NULL)
            break;
        p = eol + 1;
    }
}

static unsigned set_conn(hs_client *hs, int conn, const char *msg)
{
    hs_model *m = &hs->model;
    unsigned changed = 0;

    if (m->conn != conn || strcmp(m->conn_msg, msg) != 0)
        changed = HS_C_CONN;

    m->conn = conn;
    u_copy(m->conn_msg, sizeof m->conn_msg, msg);
    return changed;
}

static unsigned complete_action(hs_client *hs, http_state st,
                                unsigned long now)
{
    hs_model *m = &hs->model;
    const hs_job *job = &hs->cur;
    const http_req *r = hs->req;
    int status = http_status(r);
    size_t len = 0;
    const char *body = http_body(r, &len);
    const char *label = (job->act.label[0] != '\0') ? job->act.label
                                                    : "Action";
    int ok = 0;
    int quiet = 0;
    char msg[160];
    wpsd_result res;

    msg[0] = '\0';

    if (st == HTTP_FAILED) {
        int going_down = job->act.kind == HS_ACT_SYS &&
                         (strcmp(job->act.s1, "reboot") == 0 ||
                          strcmp(job->act.s1, "shutdown") == 0);

        if (going_down && !http_got_response(r)) {
            ok = 1;
            quiet = 1;
            u_copy(msg, sizeof msg, "sent - the hotspot is going offline");
        } else {
            u_copy(msg, sizeof msg, http_error(r));
        }
    } else if (status == 401 || status == 403) {
        m->auth_failed = 1;
        u_copy(msg, sizeof msg,
               "the hotspot refused the login - check the user name and "
               "password in Choices");
    } else if (status >= 300 && status < 400) {
        char where[100];

        header_value(http_headers(r), "Location", where, sizeof where);

        /* The dashboard redirects back to its own admin page after a BM
         * add/delete. Any other redirect (typically to https) means the
         * request did not do what was asked. */
        if ((job->act.kind == HS_ACT_BM_ADD ||
             job->act.kind == HS_ACT_BM_DELETE) &&
            strstr(where, "/admin/") != NULL) {
            ok = 1;
            u_copy(msg, sizeof msg, "done");
        } else {
            snprintf(msg, sizeof msg,
                     "the hotspot redirected the request to %.80s - this "
                     "program only speaks plain http",
                     where[0] != '\0' ? where : "somewhere else");
        }
    } else if (status != 200) {
        snprintf(msg, sizeof msg, "the hotspot answered HTTP %d", status);
    } else if (job->rq.json_reply) {
        if (wpsd_parse_api_result(body, len, &res)) {
            ok = res.ok;
            u_copy(msg, sizeof msg, res.msg);
        } else if (http_incomplete(r) && job->act.kind == HS_ACT_SYS) {
            ok = 1;
            u_copy(msg, sizeof msg,
                   "sent - the hotspot closed the connection early");
        } else {
            u_copy(msg, sizeof msg, "unexpected reply from the hotspot");
        }
    } else if (job->rq.reply_prefix != NULL) {
        if (wpsd_parse_manager_reply(body, len, job->rq.reply_prefix, &res)) {
            ok = res.ok;
            u_copy(msg, sizeof msg, res.msg);
        } else {
            ok = 1;
            u_copy(msg, sizeof msg,
                   "sent (no confirmation found in the reply)");
        }
    } else {
        ok = 1;
        u_copy(msg, sizeof msg, "done");
    }

    m->act_ok = ok;
    snprintf(m->act_msg, sizeof m->act_msg, "%s: %s", label, msg);
    m->act_seq++;

    if (ok && !quiet) {
        switch (job->act.kind) {
            case HS_ACT_BM_LINK:
            case HS_ACT_BM_DROP_DYN:
            case HS_ACT_BM_DROP_QSO:
            case HS_ACT_BM_ADD:
            case HS_ACT_BM_DELETE:
                hs->after_mask |= HS_R_BM;
                break;
            case HS_ACT_TGIF:
                hs->after_mask |= HS_R_TGIF;
                break;
            case HS_ACT_DMRNET:
                hs->after_mask |= HS_R_DMRNET | HS_R_STATUS;
                break;
            default:
                hs->after_mask |= HS_R_STATUS;
                break;
        }

        hs->refresh_pending = 1;
        hs->refresh_at = now + HS_AFTER_ACTION_CS;
    }

    return HS_C_ACTION;
}

static unsigned complete_fetch(hs_client *hs, http_state st,
                               unsigned long now)
{
    hs_model *m = &hs->model;
    const hs_job *job = &hs->cur;
    const http_req *r = hs->req;
    int status = http_status(r);
    size_t len = 0;
    const char *body = http_body(r, &len);
    unsigned changed = 0;

    if (st == HTTP_FAILED) {
        unsigned long wait = (unsigned long)hs->cfg.refresh_s * 100UL;

        changed |= set_conn(hs, -1, http_error(r));
        queue_drop_fetches(hs);

        if (wait < 1000UL)
            wait = 1000UL;
        hs->next_poll = now + wait;
        return changed;
    }

    if (status == 401 || status == 403) {
        m->auth_failed = 1;

        if (job->rq.auth) {
            if (job->kind == JOB_BM)
                m->bm_state = -2;
            if (job->kind == JOB_DMRNET)
                m->dmrnet_state = -2;
            if (job->kind == JOB_YSFLIST)
                m->ysf_state = -2;
            return changed;
        }

        return set_conn(hs, -1,
                        "the hotspot wants a login (check Choices)");
    }

    if (status >= 300 && status < 400) {
        char where[100];
        char msg[160];

        header_value(http_headers(r), "Location", where, sizeof where);
        snprintf(msg, sizeof msg,
                 "the hotspot redirected to %.80s (HTTP %d) - this program "
                 "only speaks plain http", where[0] != '\0' ? where : "?",
                 status);

        return set_conn(hs, -1, msg);
    }

    if (status != 200) {
        /* One page failing says little about the hotspot as a whole (it did
         * answer); only mark the part of the display that page feeds. */
        switch (job->kind) {
            case JOB_RADIO:  m->radio_ok = 0;  changed |= HS_R_RADIO;  break;
            case JOB_HEARD:  m->heard_ok = 0;  changed |= HS_R_HEARD;  break;
            case JOB_STATUS: m->status_ok = 0; changed |= HS_R_STATUS; break;
            case JOB_HW:     m->hw_ok = 0;     changed |= HS_R_HW;     break;
            case JOB_BM:     m->bm_state = -1; changed |= HS_R_BM;     break;
            case JOB_TGIF:   m->tgif_state = -1;   changed |= HS_R_TGIF;   break;
            case JOB_DMRNET: m->dmrnet_state = -1; changed |= HS_R_DMRNET; break;
            case JOB_YSFLIST: m->ysf_state = -1;   changed |= HS_R_YSF;    break;
            default: break;
        }

        changed |= set_conn(hs, 1, "");
        m->updated_cs = now;
        return changed;
    }

    changed |= set_conn(hs, 1, "");
    m->updated_cs = now;
    if (job->rq.auth)
        m->auth_failed = 0;

    switch (job->kind) {
        case JOB_RADIO:
            m->radio_ok = wpsd_parse_radio(body, len, &m->radio);
            changed |= HS_R_RADIO;
            break;

        case JOB_HEARD:
            m->heard_ok = wpsd_parse_heard(body, len, &m->heard);
            changed |= HS_R_HEARD;
            break;

        case JOB_STATUS:
            m->status_ok = wpsd_parse_status(body, len, &m->status) > 0;

            /* A dashboard whose markup we do not know (an older or modified
             * one): keep its text so the window can at least show that. */
            if (m->status_ok)
                m->status_raw[0] = '\0';
            else
                html_lines(body, len, m->status_raw, sizeof m->status_raw);

            changed |= HS_R_STATUS;
            break;

        case JOB_HW:
            m->hw_ok = wpsd_parse_hw(body, len, &m->hw);
            changed |= HS_R_HW;
            break;

        case JOB_BM:
            if (wpsd_parse_bm(body, len, &m->bm))
                m->bm_state = m->bm.ok ? 1 : 2;
            else
                m->bm_state = -1;
            changed |= HS_R_BM;
            break;

        case JOB_TGIF:
            m->tgif_state = wpsd_parse_tgif(body, len, &m->tgif) ? 1 : -1;
            changed |= HS_R_TGIF;
            break;

        case JOB_DMRNET:
            m->dmrnet_state = wpsd_parse_dmrnets(body, len, &m->dmrnets)
                                  ? 1 : -1;
            changed |= HS_R_DMRNET;
            break;

        case JOB_YSFLIST:
            m->ysf_state = wpsd_parse_ysflist(body, len, &m->ysf) > 0 ? 1 : -1;
            changed |= HS_R_YSF;
            break;

        default:
            break;
    }

    return changed;
}

/* ------------------------------------------------------------------ */
/* Scheduling                                                         */
/* ------------------------------------------------------------------ */

void hs_set_polling(hs_client *hs, int on, unsigned long now_cs)
{
    hs->polling = on;

    if (on) {
        hs->cycle = 0;
        hs->next_poll = now_cs;
    }
}

static void enqueue_cycle(hs_client *hs, unsigned long now)
{
    unsigned mask = HS_R_RADIO;
    unsigned focus = hs->focus;

    if (!hs->cfg.show_bm)
        focus &= ~HS_R_BM;

    if (hs->cycle == 0) {
        /* The first pass fetches everything that is cheap, so the menus and
         * every tab have something to show. The big network page waits until
         * it is actually looked at. */
        mask |= HS_R_STATUS | HS_R_HEARD | HS_R_HW | HS_R_TGIF;
        if (hs->cfg.show_bm)
            mask |= HS_R_BM;
        mask |= focus & HS_R_DMRNET;
    } else {
        if (hs->cycle % 3 == 0)
            mask |= HS_R_STATUS | (focus & (HS_R_TGIF | HS_R_BM));

        mask |= focus & HS_R_HEARD;

        if (hs->cycle % 6 == 0)
            mask |= focus & (HS_R_HW | HS_R_DMRNET);
    }

    hs_refresh(hs, mask);
    hs->cycle++;
    hs->next_poll = now + (unsigned long)hs->cfg.refresh_s * 100UL;
}

void hs_set_focus(hs_client *hs, unsigned parts)
{
    unsigned fresh = parts & ~hs->focus;

    hs->focus = parts & (HS_R_HEARD | HS_R_HW | HS_R_BM | HS_R_TGIF |
                         HS_R_DMRNET | HS_R_YSF);

    if (!hs->cfg.show_bm)
        fresh &= ~HS_R_BM;

    if (fresh != 0)
        hs_refresh(hs, fresh);
}

unsigned hs_get_focus(const hs_client *hs)
{
    return hs->focus;
}

static void start_job(hs_client *hs, const hs_job *job)
{
    http_endpoint ep;

    memset(&ep, 0, sizeof ep);
    u_copy(ep.host, sizeof ep.host, hs->cfg.host);
    ep.port = hs->cfg.port;
    u_copy(ep.user, sizeof ep.user, hs->cfg.user);
    u_copy(ep.pass, sizeof ep.pass, hs->cfg.pass);

    hs->cur = *job;
    hs->req = http_start(&ep, job->rq.method, job->rq.target, job->rq.ctype,
                         (job->rq.body[0] != '\0') ? job->rq.body : NULL,
                         strlen(job->rq.body), job->max_body,
                         job->timeout_cs);
}

/* ------------------------------------------------------------------ */
/* Searching for a hotspot                                            */
/* ------------------------------------------------------------------ */

int hs_scan_start(hs_client *hs, const char *text, unsigned long now_cs)
{
    hs_model *m = &hs->model;
    char prefix[24];

    (void)now_cs;

    if (!scan_prefix_from(text, prefix, sizeof prefix))
        return -1;

    scan_free(hs->scan);
    hs->scan = scan_new(prefix, hs->cfg.port);
    if (hs->scan == NULL)
        return -1;

    m->scan_state = 1;
    m->scan_done = 0;
    m->scan_total = SCAN_HOSTS;
    m->scan_nfound = 0;
    u_copy(m->scan_prefix, sizeof m->scan_prefix, prefix);
    return 0;
}

void hs_scan_stop(hs_client *hs)
{
    scan_free(hs->scan);
    hs->scan = NULL;
    hs->model.scan_state = 0;
    hs->model.scan_nfound = 0;
}

void hs_scan_clear(hs_client *hs)
{
    hs->model.scan_state = 0;
    hs->model.scan_nfound = 0;
}

/* Called every step while a search is running. */
static unsigned pump_scan(hs_client *hs)
{
    hs_model *m = &hs->model;
    unsigned changed = 0;
    int finished = scan_step(hs->scan);
    int done;
    int found;
    int i;

    scan_progress(hs->scan, &done, NULL, &found);

    if (finished || found != m->scan_nfound || done - m->scan_done >= 8) {
        m->scan_done = done;
        m->scan_nfound = found;
        changed |= HS_C_SCAN;
    }

    if (finished) {
        for (i = 0; i < found && i < 4; i++)
            scan_result(hs->scan, i, m->scan_addr[i], sizeof m->scan_addr[i],
                        m->scan_info[i], sizeof m->scan_info[i]);

        if (m->scan_nfound > 4)
            m->scan_nfound = 4;

        m->scan_state = 2;
        scan_free(hs->scan);
        hs->scan = NULL;
        changed |= HS_C_SCAN;
    }

    return changed;
}

unsigned hs_step(hs_client *hs, unsigned long now)
{
    unsigned changed = 0;

    if (hs->scan != NULL)
        changed |= pump_scan(hs);

    if (hs->req == NULL) {
        if (hs->polling && hs->qcount == 0 &&
            (long)(now - hs->next_poll) >= 0)
            enqueue_cycle(hs, now);

        if (hs->refresh_pending && hs->qcount == 0 &&
            (long)(now - hs->refresh_at) >= 0) {
            unsigned parts = HS_R_RADIO | HS_R_STATUS | hs->focus |
                             hs->after_mask;

            if (!hs->cfg.show_bm)
                parts &= ~HS_R_BM;

            hs->refresh_pending = 0;
            hs->after_mask = 0;
            hs_refresh(hs, parts);
        }

        if (hs->qcount > 0) {
            hs_job job;

            queue_pop(hs, &job);
            start_job(hs, &job);

            if (hs->req == NULL) {
                changed |= set_conn(hs, -1, "out of memory");
                return changed;
            }
        }
    }

    if (hs->req != NULL) {
        http_state st = http_step(hs->req);

        if (st == HTTP_DONE || st == HTTP_FAILED) {
            record_diag(hs, &hs->cur, hs->req, st, now);

            if (hs->cur.kind == JOB_ACTION)
                changed |= complete_action(hs, st, now);
            else
                changed |= complete_fetch(hs, st, now);

            hs->model.nreq = hs_request_summary(hs, hs->model.req_line, 8);

            http_free(hs->req);
            hs->req = NULL;
        }
    }

    return changed;
}
