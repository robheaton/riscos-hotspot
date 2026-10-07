/*
 * wpsd.c
 *
 * See wpsd.h.
 */

#include "wpsd.h"
#include "html.h"
#include "json.h"
#include "util.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================== */
/* repeaterinfo.php: sections and status pills                        */
/* ================================================================== */

static unsigned char pill_state_from(const char *w)
{
    if (strcmp(w, "active") == 0)
        return PILL_ACTIVE;
    if (strcmp(w, "paused") == 0)
        return PILL_PAUSED;
    if (strcmp(w, "error") == 0)
        return PILL_ERROR;
    if (strcmp(w, "inactive") == 0)
        return PILL_INACTIVE;

    return PILL_OTHER;
}

/* The pill's class attribute is "status-pill <state>", but the PHP helper
 * that writes it sometimes appends a stray quote and a style attribute
 * (class='status-pill active' style='grid-column: span 2;''), so the state
 * is just the run of letters after the marker. */
static unsigned char pill_state_in_tag(const char *tag, const char *tag_end)
{
    const char *s = u_find(tag, (size_t)(tag_end - tag), "status-pill");
    char word[16];
    size_t n = 0;

    if (s == NULL)
        return PILL_OTHER;

    s += strlen("status-pill");
    while (s < tag_end && *s == ' ')
        s++;

    while (s < tag_end && n + 1 < sizeof word && islower((unsigned char)*s))
        word[n++] = *s++;
    word[n] = '\0';

    return pill_state_from(word);
}

static void parse_pill_content(const char *c0, const char *c1, wpsd_pill *pl)
{
    const char *p = c0;

    pl->label[0] = '\0';
    pl->value[0] = '\0';

    while (p < c1) {
        const char *te;
        const char *sp = html_find_tag(p, c1, "span", &te);
        const char *close;
        size_t tl;

        if (sp == NULL)
            break;

        tl = (size_t)(te - sp);

        /* <span class='pill-data'> only wraps the real content. */
        if (u_find(sp, tl, "pill-data") != NULL) {
            p = te;
            continue;
        }

        close = u_find(te, (size_t)(c1 - te), "</span>");
        if (close == NULL)
            close = c1;

        if (u_find(sp, tl, "pill-value") != NULL) {
            if (pl->value[0] == '\0')
                html_text(te, (size_t)(close - te), pl->value,
                          sizeof pl->value);
        } else {
            if (pl->label[0] == '\0')
                html_text(te, (size_t)(close - te), pl->label,
                          sizeof pl->label);
        }

        p = close;
    }

    /* Some pills are just text (or a link): show whatever it says. */
    if (pl->label[0] == '\0' && pl->value[0] == '\0')
        html_text(c0, (size_t)(c1 - c0), pl->label, sizeof pl->label);
}

int wpsd_parse_status(const char *html, size_t n, wpsd_status *out)
{
    const char *p = html;
    const char *end = html + n;
    int cur = -1;

    memset(out, 0, sizeof *out);

    while (p < end) {
        const char *te;
        const char *d = html_find_tag(p, end, "div", &te);
        size_t tl;

        if (d == NULL)
            break;

        tl = (size_t)(te - d);

        if (u_find(d, tl, "sidebar-section-title") != NULL) {
            const char *close = html_div_close(te, end);

            if (close == NULL)
                break;

            if (out->nsec < WPSD_MAX_SECTIONS) {
                html_text(te, (size_t)(close - te),
                          out->sec[out->nsec].title,
                          sizeof out->sec[out->nsec].title);
                cur = out->nsec++;
            }

            p = close;
            continue;
        }

        if (u_find(d, tl, "status-pill") != NULL) {
            const char *close = html_div_close(te, end);

            if (close == NULL)
                close = end;

            if (out->npill < WPSD_MAX_PILLS) {
                wpsd_pill *pl = &out->pill[out->npill];

                memset(pl, 0, sizeof *pl);
                pl->state = pill_state_in_tag(d, te);
                pl->section = (unsigned char)((cur >= 0) ? cur : 0);
                parse_pill_content(te, close, pl);

                if (pl->label[0] != '\0' || pl->value[0] != '\0')
                    out->npill++;
            }

            p = close;
            continue;
        }

        p = te;
    }

    return out->npill;
}

/* ================================================================== */
/* radioinfo.php                                                      */
/* ================================================================== */

int wpsd_parse_radio(const char *html, size_t n, wpsd_radio *out)
{
    char heads[WPSD_MAX_KV][32];
    char cells[WPSD_MAX_KV][96];
    int nh = 0;
    int nc = 0;
    int i;
    int count;
    const char *p = html;
    const char *end = html + n;

    memset(out, 0, sizeof *out);

    while (p < end) {
        const char *te;
        const char *d = html_find_tag(p, end, "div", &te);
        size_t tl;
        const char *close;

        if (d == NULL)
            break;

        tl = (size_t)(te - d);

        if (u_find(d, tl, "divTableHeadCell") != NULL) {
            close = html_div_close(te, end);
            if (close == NULL)
                break;

            if (nh < WPSD_MAX_KV) {
                html_text(te, (size_t)(close - te), heads[nh],
                          sizeof heads[nh]);
                nh++;
            }

            p = close;
            continue;
        }

        if (u_find(d, tl, "divTableCell") != NULL) {
            close = html_div_close(te, end);
            if (close == NULL)
                break;

            if (nc < WPSD_MAX_KV) {
                html_text(te, (size_t)(close - te), cells[nc],
                          sizeof cells[nc]);
                nc++;
            }

            p = close;
            continue;
        }

        p = te;
    }

    if (nc == 0)
        return 0;

    u_copy(out->state, sizeof out->state, cells[0]);

    count = (nh < nc) ? nh : nc;
    for (i = 0; i < count; i++) {
        u_copy(out->kv[out->n].key, sizeof out->kv[out->n].key, heads[i]);
        u_copy(out->kv[out->n].val, sizeof out->kv[out->n].val, cells[i]);
        out->n++;
    }

    return 1;
}

/* ================================================================== */
/* hw_info.php                                                        */
/* ================================================================== */

static void add_kv(wpsd_hw *out, const char *key, const char *val)
{
    if (out->n >= WPSD_MAX_KV)
        return;

    u_copy(out->kv[out->n].key, sizeof out->kv[out->n].key, key);
    u_copy(out->kv[out->n].val, sizeof out->kv[out->n].val, val);
    out->n++;
}

/* The first card's tooltip is "<strong>Hardware:</strong> X<br />..."
 * lines; split them into key/value pairs. */
static void parse_tooltip_pairs(const char *c0, const char *c1, wpsd_hw *out)
{
    const char *p = c0;

    while (p < c1 && out->n < WPSD_MAX_KV) {
        const char *br = u_ifind(p, (size_t)(c1 - p), "<br");
        const char *seg_end = (br != NULL) ? br : c1;
        char line[128];
        char *colon;

        html_text(p, (size_t)(seg_end - p), line, sizeof line);
        colon = strchr(line, ':');

        if (colon != NULL && colon != line) {
            *colon = '\0';
            u_trim(line);
            u_trim(colon + 1);

            if (line[0] != '\0' && colon[1] != '\0')
                add_kv(out, line, colon + 1);
        }

        if (br == NULL)
            break;

        {
            const char *te = html_tag_end(br, c1);

            p = (te != NULL) ? te : c1;
        }
    }
}

int wpsd_parse_hw(const char *html, size_t n, wpsd_hw *out)
{
    const char *p = html;
    const char *end = html + n;
    int first = 1;

    memset(out, 0, sizeof *out);

    while (p < end && out->n < WPSD_MAX_KV) {
        const char *lab = u_find(p, (size_t)(end - p), "stat-label");
        const char *lte;
        const char *lclose;
        const char *vd;
        const char *vte;
        const char *vclose;
        const char *a;
        const char *ate;
        const char *span;
        const char *vend;
        char label[32];
        char value[64];

        if (lab == NULL)
            break;

        lte = memchr(lab, '>', (size_t)(end - lab));
        if (lte == NULL)
            break;
        lte++;

        lclose = u_find(lte, (size_t)(end - lte), "</span>");
        if (lclose == NULL)
            break;

        html_text(lte, (size_t)(lclose - lte), label, sizeof label);

        vd = html_find_div(lclose, end, "stat-value", &vte);
        if (vd == NULL)
            break;

        vclose = html_div_close(vte, end);
        if (vclose == NULL)
            vclose = end;

        value[0] = '\0';
        a = html_find_tag(vte, vclose, "a", &ate);

        if (a != NULL) {
            const char *ste;

            span = html_find_tag(ate, vclose, "span", &ste);
            vend = (span != NULL) ? span : vclose;
            html_text(ate, (size_t)(vend - ate), value, sizeof value);

            if (label[0] != '\0')
                add_kv(out, label, value);

            if (first && span != NULL) {
                const char *sclose = u_find(ste, (size_t)(vclose - ste),
                                            "</span>");

                parse_tooltip_pairs(ste, (sclose != NULL) ? sclose : vclose,
                                    out);
            }
        } else {
            html_text(vte, (size_t)(vclose - vte), value, sizeof value);

            if (label[0] != '\0')
                add_kv(out, label, value);
        }

        first = 0;
        p = vclose;
    }

    return out->n > 0;
}

/* ================================================================== */
/* /api/ last heard                                                   */
/* ================================================================== */

static void copy_json_text(char *dst, size_t cap, const jnode *obj,
                           const char *key)
{
    const char *s = json_string(obj, key, "");

    u_copy(dst, cap, s);
    utf8_to_latin1(dst);
    u_trim(dst);
}

int wpsd_parse_heard(const char *json, size_t n, wpsd_lastheard *out)
{
    jnode *root;
    int i;
    int count;

    memset(out, 0, sizeof *out);

    root = json_parse_lenient(json, n, NULL);
    if (root == NULL)
        return 0;

    if (root->t != J_ARR) {
        json_free(root);
        return 0;
    }

    count = json_count(root);
    if (count > WPSD_MAX_HEARD)
        count = WPSD_MAX_HEARD;

    for (i = 0; i < count; i++) {
        const jnode *o = json_at(root, i);
        wpsd_heard *h = &out->row[out->n];
        char when[40];
        const char *sp;

        if (o == NULL || o->t != J_OBJ)
            continue;

        memset(h, 0, sizeof *h);

        copy_json_text(when, sizeof when, o, "time_utc");
        sp = strchr(when, ' ');
        u_copy(h->time, sizeof h->time, (sp != NULL) ? sp + 1 : when);

        copy_json_text(h->mode, sizeof h->mode, o, "mode");
        copy_json_text(h->call, sizeof h->call, o, "callsign");
        copy_json_text(h->target, sizeof h->target, o, "target");
        copy_json_text(h->src, sizeof h->src, o, "src");
        copy_json_text(h->dur, sizeof h->dur, o, "duration");
        copy_json_text(h->loss, sizeof h->loss, o, "loss");
        copy_json_text(h->name, sizeof h->name, o, "name");
        copy_json_text(h->country, sizeof h->country, o, "country");

        h->active = (h->dur[0] == '\0');
        out->n++;
    }

    json_free(root);
    return 1;
}

/* ================================================================== */
/* bm-manager.php                                                     */
/* ================================================================== */

static int digits_in(const char *s)
{
    while (*s != '\0' && !isdigit((unsigned char)*s))
        s++;

    return atoi(s);
}

/* Text of the first <div> whose tag contains `cls` inside [r0, r1). */
static int div_text(const char *r0, const char *r1, const char *cls,
                    char *out, size_t cap)
{
    const char *te;
    const char *d = html_find_div(r0, r1, cls, &te);
    const char *close;

    out[0] = '\0';
    if (d == NULL)
        return 0;

    close = html_div_close(te, r1);
    if (close == NULL)
        close = r1;

    html_text(te, (size_t)(close - te), out, cap);
    return 1;
}

static void add_drop_slot(wpsd_bm *out, int slot)
{
    int i;

    for (i = 0; i < out->ndrop; i++) {
        if (out->drop_slot[i] == slot)
            return;
    }

    if (out->ndrop < WPSD_MAX_BM_SLOTS)
        out->drop_slot[out->ndrop++] = slot;
}

static void parse_bm_info(const char *h, const char *end, wpsd_bm *out)
{
    const char *te;
    const char *bar = html_find_div(h, end, "bm-info-bar", &te);
    const char *close;
    char text[200];
    const char *id;
    const char *net;

    if (bar == NULL)
        return;

    close = html_div_close(te, end);
    if (close == NULL)
        close = end;

    html_text(te, (size_t)(close - te), text, sizeof text);

    id = strstr(text, "ID:");
    if (id != NULL) {
        size_t k = 0;

        id += 3;
        while (*id == ' ')
            id++;
        while (isdigit((unsigned char)id[k]) && k + 1 < sizeof out->id) {
            out->id[k] = id[k];
            k++;
        }
        out->id[k] = '\0';
    }

    net = strstr(text, "Connected To:");
    if (net != NULL) {
        char *stop;

        net += strlen("Connected To:");
        while (*net == ' ')
            net++;

        u_copy(out->network, sizeof out->network, net);

        /* The bar continues "... * Full Talkgroup List". */
        stop = strstr(out->network, " * ");
        if (stop != NULL)
            *stop = '\0';
        u_trim(out->network);
    }
}

int wpsd_parse_bm(const char *html, size_t n, wpsd_bm *out)
{
    const char *end = html + n;
    const char *alert;
    const char *dyn_pos;
    const char *p;
    const char *slots;
    int rows = 0;

    memset(out, 0, sizeof *out);

    /* The dashboard's own complaint (no API key, BrandMeister disabled...).
     * The marker includes class=" so the page's CSS does not match. */
    alert = u_find(html, n, "class=\"bm-alert");
    if (alert != NULL) {
        const char *te = memchr(alert, '>', (size_t)(end - alert));

        if (te != NULL) {
            const char *close;

            te++;
            close = html_div_close(te, end);
            if (close == NULL)
                close = end;

            html_text(te, (size_t)(close - te), out->notice,
                      sizeof out->notice);
        }

        out->ok = 0;
        return 1;
    }

    parse_bm_info(html, end, out);

    dyn_pos = u_find(html, n, "Dynamic Talkgroups");
    p = html;

    for (;;) {
        const char *row = u_find(p, (size_t)(end - p),
                                 "class=\"bm-list-row\"");
        const char *next;
        const char *row_end;
        char tg_text[24];
        char ts_text[16];
        int is_dyn;
        int tg;

        if (row == NULL)
            break;

        next = u_find(row + 1, (size_t)(end - row - 1),
                      "class=\"bm-list-row\"");
        row_end = (next != NULL) ? next : end;
        p = row_end;

        if (!div_text(row, row_end, "bm-col-tg", tg_text, sizeof tg_text))
            continue;

        tg = digits_in(tg_text);
        if (tg <= 0)
            continue;

        div_text(row, row_end, "bm-col-ts", ts_text, sizeof ts_text);
        is_dyn = (dyn_pos != NULL && row > dyn_pos);

        if (is_dyn) {
            wpsd_bm_dyn *d;

            if (out->ndyn >= WPSD_MAX_BM_DYN)
                continue;

            d = &out->dyn[out->ndyn++];
            memset(d, 0, sizeof *d);
            d->tg = tg;
            d->disp_slot = digits_in(ts_text);
            div_text(row, row_end, "bm-col-name", d->name, sizeof d->name);
            div_text(row, row_end, "bm-col-timeout", d->timeout,
                     sizeof d->timeout);
        } else {
            wpsd_bm_static *s;
            const char *q = row;
            int slot = -1;
            int linked = 0;

            if (out->nst >= WPSD_MAX_BM_STATIC)
                continue;

            for (;;) {
                const char *te;
                const char *in = html_find_tag(q, row_end, "input", &te);
                int v;

                if (in == NULL)
                    break;

                if (u_find(in, (size_t)(te - in), "data-tg=") != NULL) {
                    if (html_attr_int(in, te, "data-slot", &v))
                        slot = v;
                    linked = u_find(in, (size_t)(te - in),
                                    "checked=\"checked\"") != NULL;
                    break;
                }

                q = te;
            }

            s = &out->st[out->nst++];
            memset(s, 0, sizeof *s);
            s->tg = tg;
            s->disp_slot = digits_in(ts_text);
            s->slot = (slot >= 0) ? slot : (s->disp_slot > 0 ? s->disp_slot : 0);
            s->linked = linked;
            div_text(row, row_end, "bm-col-name", s->name, sizeof s->name);
        }

        rows++;
    }

    /* Which slots the Drop QSO / Drop All Dynamic buttons are offered for. */
    slots = html;
    for (;;) {
        const char *m = u_find(slots, (size_t)(end - slots),
                               "cmd=drop_qso&slot=");

        if (m == NULL)
            break;

        m += strlen("cmd=drop_qso&slot=");
        if (m < end && isdigit((unsigned char)*m))
            add_drop_slot(out, *m - '0');
        slots = m;
    }

    if (out->id[0] == '\0' && rows == 0 && out->ndrop == 0)
        return 0;       /* nothing here looks like the BM manager */

    out->ok = 1;
    return 1;
}

/* ================================================================== */
/* tgif_links.php                                                     */
/* ================================================================== */

int wpsd_parse_tgif(const char *html, size_t n, wpsd_tgif *out)
{
    const char *end = html + n;
    const char *p = u_find(html, n, "Active TGIF Connections");
    int idx = 0;

    memset(out, 0, sizeof *out);

    if (p == NULL)
        return 0;

    /* After the header row, one data row of three cells: the master (with
     * the hotspot's id), slot 1 and slot 2 ("None" or "TG91" and the name in
     * a floated span). */
    while (idx < 3) {
        const char *te;
        const char *td = html_find_tag(p, end, "td", &te);
        const char *close;

        if (td == NULL)
            break;

        close = u_ifind(te, (size_t)(end - te), "</td>");
        if (close == NULL)
            close = end;

        if (idx == 0) {
            char text[160];
            const char *id;
            size_t k = 0;

            html_text(te, (size_t)(close - te), text, sizeof text);
            id = strstr(text, "ID:");
            if (id != NULL) {
                id += 3;
                while (*id == ' ')
                    id++;
                while (isdigit((unsigned char)id[k]) &&
                       k + 1 < sizeof out->id) {
                    out->id[k] = id[k];
                    k++;
                }
                out->id[k] = '\0';
            }
        } else {
            const char *ste;
            const char *sp = html_find_tag(te, close, "span", &ste);
            char tg[24];

            html_text(te, (size_t)(((sp != NULL) ? sp : close) - te), tg,
                      sizeof tg);
            out->tg[idx - 1] = isdigit((unsigned char)tg[2]) ? digits_in(tg)
                                                             : 0;

            if (sp != NULL) {
                const char *sclose = u_ifind(ste, (size_t)(close - ste),
                                             "</span>");

                html_text(ste,
                          (size_t)(((sclose != NULL) ? sclose : close) - ste),
                          out->name[idx - 1], sizeof out->name[idx - 1]);
            }
        }

        p = close;
        idx++;
    }

    out->ok = (idx == 3);
    return out->ok;
}

/* ================================================================== */
/* The DMR Network Manager (admin page, func=dmr_man)                 */
/* ================================================================== */

int wpsd_parse_dmrnets(const char *html, size_t n, wpsd_dmrnets *out)
{
    const char *end = html + n;
    const char *p = html;

    memset(out, 0, sizeof *out);

    /* The marker includes class=" so the page's own CSS does not match. */
    for (;;) {
        const char *row = u_find(p, (size_t)(end - p),
                                 "class=\"dmr-net-row\"");
        const char *next;
        const char *row_end;
        const char *te;
        const char *in;
        wpsd_dmrnet *net;

        if (row == NULL)
            break;

        next = u_find(row + 1, (size_t)(end - row - 1),
                      "class=\"dmr-net-row\"");
        row_end = (next != NULL) ? next : end;
        p = row_end;

        if (out->n >= WPSD_MAX_DMRNETS)
            continue;

        net = &out->net[out->n];
        memset(net, 0, sizeof *net);

        in = html_find_tag(row, row_end, "input", &te);
        if (in == NULL ||
            !html_attr_str(in, te, "data-net-id", net->id, sizeof net->id) ||
            net->id[0] == '\0')
            continue;

        net->enabled = u_find(in, (size_t)(te - in), " checked") != NULL;
        div_text(row, row_end, "dmr-net-name", net->name, sizeof net->name);
        out->n++;
    }

    return out->n > 0;
}

/* ================================================================== */
/* The YSF reflector list                                             */
/* ================================================================== */

void wpsd_ysflist_free(wpsd_ysflist *l)
{
    free(l->e);
    memset(l, 0, sizeof *l);
}

static int ysf_add(wpsd_ysflist *l, const char *value, const char *text)
{
    wpsd_ysf *e;

    if (l->n >= WPSD_MAX_YSF)
        return 0;

    if (l->n == l->cap) {
        int cap = (l->cap != 0) ? l->cap * 2 : 256;
        wpsd_ysf *ne = (wpsd_ysf *)realloc(l->e, (size_t)cap * sizeof *ne);

        if (ne == NULL)
            return 0;

        l->e = ne;
        l->cap = cap;
    }

    e = &l->e[l->n++];
    u_copy(e->value, sizeof e->value, value);
    u_copy(e->text, sizeof e->text, text);
    return 1;
}

/* The page's <select name="ysfLinkHost"> holds one <option> per reflector:
 *
 *   <option value="YSF00001" >YSF00001 - Parrot</option>
 *   <option value="YSF12345" >YSF12345 - UK-Calling - United Kingdom</option>
 *   <option value="FCS00123" >FCS00123 - Room name</option>
 *
 * plus "none" (unlink). The value is what the form posts, so only plain
 * letters and digits are taken. */
int wpsd_parse_ysflist(const char *html, size_t n, wpsd_ysflist *out)
{
    const char *end = html + n;
    const char *sel = u_ifind(html, n, "name=\"ysfLinkHost\"");
    const char *sel_end;
    const char *p;

    wpsd_ysflist_free(out);

    if (sel == NULL)
        return 0;

    p = (const char *)memchr(sel, '>', (size_t)(end - sel));
    if (p == NULL)
        return 0;
    p++;

    sel_end = u_ifind(p, (size_t)(end - p), "</select>");
    if (sel_end == NULL)
        sel_end = end;

    for (;;) {
        const char *opt = u_ifind(p, (size_t)(sel_end - p), "<option");
        const char *te;
        const char *close;
        const char *next;
        const char *text_end;
        char value[16];
        char label[128];
        const char *text;
        size_t vlen;
        size_t i;
        int valid = 1;

        if (opt == NULL)
            break;

        te = html_tag_end(opt, sel_end);
        if (te == NULL)
            break;
        p = te;

        /* The label runs to its </option>, or to the next <option> if the
         * page left the closing tag off (searching only that far keeps a
         * page with none at all from being scanned to its end each time). */
        next = u_ifind(te, (size_t)(sel_end - te), "<option");
        text_end = (next != NULL) ? next : sel_end;
        close = u_ifind(te, (size_t)(text_end - te), "</option>");
        if (close != NULL)
            text_end = close;
        p = text_end;

        if (!html_attr_str(opt, te, "value", value, sizeof value))
            continue;

        vlen = strlen(value);
        if (vlen == 0 || vlen > 11 || u_ieq(value, "none"))
            continue;
        for (i = 0; i < vlen; i++) {
            if (!isalnum((unsigned char)value[i]))
                valid = 0;
        }
        if (!valid)
            continue;

        html_text(te, (size_t)(text_end - te), label, sizeof label);

        /* The label starts with the value: "YSF00001 - Parrot". */
        text = label;
        if (strncmp(label, value, vlen) == 0 && label[vlen] == ' ' &&
            label[vlen + 1] == '-' && label[vlen + 2] == ' ')
            text = label + vlen + 3;
        else if (strcmp(label, value) == 0)
            text = "";

        if (!ysf_add(out, value, text))
            break;
    }

    return out->n;
}

/* ================================================================== */
/* Action replies                                                     */
/* ================================================================== */

int wpsd_parse_api_result(const char *json, size_t n, wpsd_result *out)
{
    jnode *root = json_parse_lenient(json, n, NULL);
    const jnode *m;

    memset(out, 0, sizeof *out);

    if (root == NULL || root->t != J_OBJ) {
        json_free(root);
        out->ok = 0;
        u_copy(out->msg, sizeof out->msg, "unexpected reply from hotspot");
        return 0;
    }

    m = json_member(root, "error");
    if (m != NULL) {
        out->ok = 0;
        u_copy(out->msg, sizeof out->msg,
               (m->t == J_STR && m->s != NULL) ? m->s : "hotspot reported an error");
        utf8_to_latin1(out->msg);
        json_free(root);
        return 1;
    }

    m = json_member(root, "exit_status");
    if (m != NULL && m->t == J_NUM) {
        const jnode *lines = json_member(root, "output");
        int status = (int)m->n;

        out->ok = (status == 0);

        if (lines != NULL && lines->t == J_ARR) {
            int k;

            for (k = json_count(lines) - 1; k >= 0; k--) {
                const jnode *line = json_at(lines, k);

                if (line != NULL && line->t == J_STR && line->s != NULL) {
                    u_copy(out->msg, sizeof out->msg, line->s);
                    u_trim(out->msg);
                    if (out->msg[0] != '\0')
                        break;
                }
            }
        }

        if (out->msg[0] == '\0') {
            if (status == 0)
                u_copy(out->msg, sizeof out->msg, "done");
            else
                snprintf(out->msg, sizeof out->msg, "failed (exit status %d)",
                         status);
        }

        utf8_to_latin1(out->msg);
        json_free(root);
        return 1;
    }

    /* dmrnet_set_status: whatever RemoteCommand printed; "KO" is a refusal. */
    m = json_member(root, "commandOutput");
    if (m != NULL) {
        const char *text = (m->t == J_STR && m->s != NULL) ? m->s : "";
        const char *ko = strstr(text, "KO");
        const char *last = text;
        const char *q;

        out->ok = !(ko != NULL &&
                    (ko == text || !isalnum((unsigned char)ko[-1])) &&
                    !isalnum((unsigned char)ko[2]));

        for (q = text; *q != '\0'; q++) {
            if (*q == '\n' && q[1] != '\0' && q[1] != '\n')
                last = q + 1;
        }

        if (text[0] == '\0')
            u_copy(out->msg, sizeof out->msg, out->ok ? "done" : "refused");
        else
            u_copy(out->msg, sizeof out->msg, last);
        u_trim(out->msg);
        if (out->msg[0] == '\0')
            u_copy(out->msg, sizeof out->msg, out->ok ? "done" : "refused");
        utf8_to_latin1(out->msg);
        json_free(root);
        return 1;
    }

    m = json_member(root, "ip");
    if (m != NULL && m->t == J_STR && m->s != NULL) {
        out->ok = 1;
        snprintf(out->msg, sizeof out->msg, "IP address %.100s", m->s);
        json_free(root);
        return 1;
    }

    m = json_member(root, "success");
    if (m != NULL) {
        const char *text = json_string(root, "message", NULL);

        out->ok = (m->t == J_BOOL) ? m->b : 1;
        u_copy(out->msg, sizeof out->msg,
               (text != NULL) ? text : (out->ok ? "done" : "failed"));
        utf8_to_latin1(out->msg);
        json_free(root);
        return 1;
    }

    /* A valid object we have no special reading for (e.g. dmrnet_set_status). */
    out->ok = 1;
    u_copy(out->msg, sizeof out->msg, "done");
    json_free(root);
    return 1;
}

int wpsd_parse_manager_reply(const char *html, size_t n, const char *prefix,
                             wpsd_result *out)
{
    char key[24];
    const char *end = html + n;
    const char *p = html;

    memset(out, 0, sizeof *out);

    if (prefix == NULL || strlen(prefix) + 16 > sizeof key)
        return 0;

    snprintf(key, sizeof key, "%s-alert", prefix);

    while (p < end) {
        const char *te;
        const char *d = html_find_tag(p, end, "div", &te);
        size_t tl;

        if (d == NULL)
            break;

        tl = (size_t)(te - d);

        if (u_find(d, tl, key) != NULL) {
            const char *close = html_div_close(te, end);
            char text[200];

            if (close == NULL)
                close = end;

            html_text(te, (size_t)(close - te), text, sizeof text);

            /* The boxes end "...<br>Page reloading..." (or "Reloading"). */
            {
                const char *pr = u_ifind(text, strlen(text), "Page reloading");

                if (pr != NULL)
                    text[pr - text] = '\0';
                u_trim(text);
            }

            out->ok = (u_find(d, tl, "alert-error") == NULL);
            u_copy(out->msg, sizeof out->msg, text);
            return 1;
        }

        p = te;
    }

    return 0;
}

/* ================================================================== */
/* Request construction                                               */
/* ================================================================== */

static void rq_init(wpsd_request *rq, const char *method, int auth)
{
    memset(rq, 0, sizeof *rq);
    rq->method = method;
    rq->auth = auth;
    if (strcmp(method, "POST") == 0)
        rq->ctype = "application/x-www-form-urlencoded";
}

static int rq_target(wpsd_request *rq, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(rq->target, sizeof rq->target, fmt, ap);
    va_end(ap);

    return (n < 0 || (size_t)n >= sizeof rq->target) ? -1 : 0;
}

/* Appends name=value (value URL-encoded) to the form body. */
static int rq_field(wpsd_request *rq, const char *name, const char *value)
{
    size_t len = strlen(rq->body);
    char enc[200];

    if (url_encode(value, enc, sizeof enc) == (size_t)-1)
        return -1;

    if (len + strlen(name) + strlen(enc) + 3 > sizeof rq->body)
        return -1;

    snprintf(rq->body + len, sizeof rq->body - len, "%s%s=%s",
             (len > 0) ? "&" : "", name, enc);
    return 0;
}

static int valid_word(const char *s, int allow_dash)
{
    if (s == NULL || s[0] == '\0')
        return 0;

    for (; *s != '\0'; s++) {
        if (!isalnum((unsigned char)*s) && !(allow_dash && *s == '-'))
            return 0;
    }

    return 1;
}

/* A system_api.php action name: letters, digits and underscores. */
static int valid_ident(const char *s)
{
    if (s == NULL || s[0] == '\0')
        return 0;

    for (; *s != '\0'; s++) {
        if (!isalnum((unsigned char)*s) && *s != '_')
            return 0;
    }

    return 1;
}

/* Letters, digits and spaces only (the D-Star module string). */
static int valid_spaced(const char *s)
{
    if (s == NULL || s[0] == '\0')
        return 0;

    for (; *s != '\0'; s++) {
        if (!isalnum((unsigned char)*s) && *s != ' ')
            return 0;
    }

    return 1;
}

/* A talkgroup list typed by a person: digits separated by anything that is
 * not going to confuse the dashboard (the PHP just extracts the numbers). */
static int valid_tglist(const char *s)
{
    int digits = 0;

    if (s == NULL)
        return 0;

    for (; *s != '\0'; s++) {
        if (isdigit((unsigned char)*s))
            digits++;
        else if (*s != ' ' && *s != ',' && *s != ';' && *s != '\n' &&
                 *s != '\r')
            return 0;
    }

    return digits > 0;
}

int wpsd_req_heard(wpsd_request *rq, int limit, int names)
{
    rq_init(rq, "GET", 0);

    if (limit < 1)
        limit = 1;
    if (limit > WPSD_MAX_HEARD)
        limit = WPSD_MAX_HEARD;

    return rq_target(rq, "/api/?limit=%d&names=%s&country=false", limit,
                     names ? "true" : "false");
}

int wpsd_req_radio(wpsd_request *rq)
{
    rq_init(rq, "GET", 0);
    return rq_target(rq, "/mmdvmhost/radioinfo.php");
}

int wpsd_req_status(wpsd_request *rq)
{
    rq_init(rq, "GET", 0);
    return rq_target(rq, "/mmdvmhost/repeaterinfo.php");
}

int wpsd_req_hw(wpsd_request *rq)
{
    rq_init(rq, "GET", 0);
    return rq_target(rq, "/includes/hw_info.php");
}

int wpsd_req_bm_page(wpsd_request *rq)
{
    rq_init(rq, "GET", 1);
    return rq_target(rq, "/admin/bm-manager.php");
}

int wpsd_req_tgif_links(wpsd_request *rq)
{
    rq_init(rq, "GET", 0);
    return rq_target(rq, "/mmdvmhost/tgif_links.php");
}

int wpsd_req_ysflist(wpsd_request *rq)
{
    rq_init(rq, "GET", 1);
    return rq_target(rq, "/admin/index.php?func=ysf_man");
}

int wpsd_req_dmrnets(wpsd_request *rq)
{
    rq_init(rq, "GET", 1);
    return rq_target(rq, "/admin/index.php?func=dmr_man");
}

int wpsd_req_dmrnet_set(wpsd_request *rq, const char *netid, int enable)
{
    size_t i;
    int ok = 0;

    if (netid != NULL && strcmp(netid, "xlx") == 0)
        ok = 1;
    else if (netid != NULL && strncmp(netid, "net", 3) == 0 &&
             strlen(netid) >= 4 && strlen(netid) <= 5) {
        ok = 1;
        for (i = 3; netid[i] != '\0'; i++) {
            if (!isdigit((unsigned char)netid[i]))
                ok = 0;
        }
    }

    if (!ok)
        return -1;

    rq_init(rq, "GET", 1);
    rq->json_reply = 1;

    return rq_target(rq,
                     "/admin/system_api.php?action=dmrnet_set_status"
                     "&dmrNet=%s&netState=%s&format=json", netid,
                     enable ? "enable" : "disable");
}

int wpsd_req_mode(wpsd_request *rq, const char *mode, int pause)
{
    static const char *const modes[] = { "DMR", "YSF", "D-Star", "P25",
                                         "NXDN", "POCSAG", NULL };
    int i;

    for (i = 0; modes[i] != NULL; i++) {
        if (strcmp(modes[i], mode) == 0)
            break;
    }

    if (modes[i] == NULL)
        return -1;

    rq_init(rq, "POST", 1);
    rq->reply_prefix = "imm";

    if (rq_target(rq, "/admin/index.php?func=mode_man") < 0)
        return -1;

    if (rq_field(rq, "mode_action", pause ? "Pause" : "Resume") < 0 ||
        rq_field(rq, "mode_sel", mode) < 0 ||
        rq_field(rq, "func", "mode_man") < 0 ||
        rq_field(rq, "submit_mode", "Execute Action") < 0)
        return -1;

    return 0;
}

int wpsd_req_sysapi(wpsd_request *rq, const char *action)
{
    if (!valid_ident(action))
        return -1;

    rq_init(rq, "GET", 1);
    rq->json_reply = 1;

    return rq_target(rq, "/admin/system_api.php?action=%s&format=json", action);
}

int wpsd_req_bm_link(wpsd_request *rq, int tg, int slot, int link)
{
    if (tg < 1 || slot < 0 || slot > 2)
        return -1;

    rq_init(rq, "GET", 1);
    rq->json_reply = 1;

    return rq_target(rq,
                     "/admin/system_api.php?action=bm_manager&cmd=%s"
                     "&tg=%d&slot=%d&format=json",
                     link ? "link_static" : "drop_static", tg, slot);
}

int wpsd_req_bm_drop_dynamic(wpsd_request *rq, int slot)
{
    if (slot < 0 || slot > 2)
        return -1;

    rq_init(rq, "GET", 1);
    rq->json_reply = 1;

    return rq_target(rq,
                     "/admin/system_api.php?action=bm_manager"
                     "&cmd=drop_dynamic&slot=%d&format=json", slot);
}

int wpsd_req_bm_drop_qso(wpsd_request *rq, int slot)
{
    if (slot < 0 || slot > 2)
        return -1;

    rq_init(rq, "GET", 1);
    rq->json_reply = 1;

    return rq_target(rq,
                     "/admin/system_api.php?action=bm_manager"
                     "&cmd=drop_qso&slot=%d&format=json", slot);
}

int wpsd_req_bm_add(wpsd_request *rq, const char *tgs, int slot)
{
    char sl[4];

    if (slot < 0 || slot > 2 || !valid_tglist(tgs))
        return -1;

    rq_init(rq, "POST", 1);

    if (rq_target(rq, "/admin/bm-manager.php") < 0)
        return -1;

    snprintf(sl, sizeof sl, "%d", slot);

    if (rq_field(rq, "TG", tgs) < 0 || rq_field(rq, "TS", sl) < 0 ||
        rq_field(rq, "static-tg-add", "Add & Link") < 0)
        return -1;

    return 0;
}

int wpsd_req_bm_delete(wpsd_request *rq, int tg, int slot)
{
    if (tg < 1 || slot < 0 || slot > 2)
        return -1;

    rq_init(rq, "GET", 1);
    return rq_target(rq, "/admin/bm-manager.php?droptg=%d&slot=%d", tg, slot);
}

/* Shared by the YSF/P25/NXDN managers: "<func>" selects the page, the form
 * has a Link radio (LINK/UNLINK), a host field and a submit button. */
static int simple_manager(wpsd_request *rq, const char *func,
                          const char *host_field, const char *submit_field,
                          const char *prefix, const char *host, int link)
{
    rq_init(rq, "POST", 1);
    rq->reply_prefix = prefix;

    if (rq_target(rq, "/admin/index.php?func=%s", func) < 0)
        return -1;

    if (rq_field(rq, host_field, host) < 0 ||
        rq_field(rq, "Link", link ? "LINK" : "UNLINK") < 0 ||
        rq_field(rq, "func", func) < 0 ||
        rq_field(rq, submit_field, "Execute Action") < 0)
        return -1;

    return 0;
}

int wpsd_req_ysf(wpsd_request *rq, const char *host, int link)
{
    char name[24];
    size_t i;
    int digits_only = 1;

    if (!link)
        host = "none";

    if (!valid_word(host, 0) || strlen(host) > 16)
        return -1;

    /* The dashboard splits the value into a 3 letter type and an id, so a
     * bare number means a YSF reflector. */
    for (i = 0; host[i] != '\0'; i++) {
        if (!isdigit((unsigned char)host[i]))
            digits_only = 0;
    }

    if (digits_only)
        snprintf(name, sizeof name, "YSF%s", host);
    else
        u_copy(name, sizeof name, host);

    for (i = 0; name[i] != '\0'; i++)
        name[i] = (char)toupper((unsigned char)name[i]);

    if (strcmp(name, "NONE") == 0)
        u_copy(name, sizeof name, "none");

    return simple_manager(rq, "ysf_man", "ysfLinkHost", "ysfMgrSubmit", "ysf",
                          name, link);
}

int wpsd_req_p25(wpsd_request *rq, const char *tg, int link)
{
    if (!link)
        tg = "none";

    if (!valid_word(tg, 0))
        return -1;

    return simple_manager(rq, "p25_man", "p25LinkHost", "p25MgrSubmit", "p25",
                          tg, link);
}

int wpsd_req_nxdn(wpsd_request *rq, const char *tg, int link)
{
    if (!link)
        tg = "none";

    if (!valid_word(tg, 0))
        return -1;

    return simple_manager(rq, "nxdn_man", "nxdnLinkHost", "nxdnMgrSubmit",
                          "nxdn", tg, link);
}

int wpsd_req_dstar(wpsd_request *rq, const char *module, const char *ref,
                   const char *letter, int link)
{
    char mod[16];
    char refname[12];
    char lt[4];
    size_t i;

    if (!valid_spaced(module) || strlen(module) < 2 || strlen(module) > 8)
        return -1;

    /* The unlink form still needs a (valid) reflector to get past the
     * dashboard's "Invalid Input" check; it is not used for anything. */
    if (!link || ref == NULL || ref[0] == '\0')
        ref = "REF001";
    if (!link || letter == NULL || letter[0] == '\0')
        letter = "A";

    if (!valid_word(ref, 0) || strlen(ref) > 7 || !isalpha((unsigned char)letter[0]) ||
        letter[1] != '\0')
        return -1;

    u_copy(mod, sizeof mod, module);
    u_copy(refname, sizeof refname, ref);
    lt[0] = (char)toupper((unsigned char)letter[0]);
    lt[1] = '\0';

    for (i = 0; mod[i] != '\0'; i++)
        mod[i] = (char)toupper((unsigned char)mod[i]);
    for (i = 0; refname[i] != '\0'; i++)
        refname[i] = (char)toupper((unsigned char)refname[i]);

    rq_init(rq, "POST", 1);
    rq->reply_prefix = "dstar";

    if (rq_target(rq, "/admin/index.php?func=ds_man") < 0)
        return -1;

    if (rq_field(rq, "Module", mod) < 0 ||
        rq_field(rq, "RefName", refname) < 0 ||
        rq_field(rq, "Letter", lt) < 0 ||
        rq_field(rq, "Link", link ? "LINK" : "UNLINK") < 0 ||
        rq_field(rq, "func", "ds_man") < 0 ||
        rq_field(rq, "dstrMgrSubmit", "Execute Action") < 0)
        return -1;

    return 0;
}

int wpsd_req_tgif(wpsd_request *rq, int tg, int slot, int link)
{
    char num[16];

    if (link && tg < 1)
        return -1;
    if (slot < 1 || slot > 2)
        slot = 2;

    rq_init(rq, "POST", 1);
    rq->reply_prefix = "tgif";

    if (rq_target(rq, "/admin/index.php?func=tgif_man") < 0)
        return -1;

    snprintf(num, sizeof num, "%d", link ? tg : 0);

    if (rq_field(rq, "tgifAction", link ? "LINK" : "UNLINK") < 0 ||
        rq_field(rq, "tgifNumber", num) < 0)
        return -1;

    snprintf(num, sizeof num, "%d", slot);

    if (rq_field(rq, "tgifSlot", num) < 0 ||
        rq_field(rq, "func", "tgif_man") < 0 ||
        rq_field(rq, "tgifSubmit", "Execute Action") < 0)
        return -1;

    return 0;
}
