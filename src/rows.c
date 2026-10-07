/*
 * rows.c
 *
 * See rows.h.
 */

#include "rows.h"
#include "scan.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const view_names[VIEW_COUNT] = {
    "Status", "Heard", "DMR", "Links", "System"
};

static const char *const mode_names[] = {
    "D-Star", "DMR", "YSF", "P25", "NXDN", "POCSAG", NULL
};

const char *ui_view_name(ui_view v)
{
    return ((int)v >= 0 && v < VIEW_COUNT) ? view_names[v] : "";
}

void ui_rows_init(ui_rows *r)
{
    r->row = NULL;
    r->n = 0;
    r->cap = 0;
}

void ui_rows_free(ui_rows *r)
{
    free(r->row);
    ui_rows_init(r);
}

/* ------------------------------------------------------------------ */
/* Row construction                                                   */
/* ------------------------------------------------------------------ */

/* Appends a zeroed row and returns it (NULL on out of memory). */
static ui_row *add_row(ui_rows *r, ui_rowkind kind, ui_style style)
{
    ui_row *row;

    if (r->n == r->cap) {
        int cap = (r->cap != 0) ? r->cap * 2 : 64;
        ui_row *nr = (ui_row *)realloc(r->row, (size_t)cap * sizeof *nr);

        if (nr == NULL)
            return NULL;

        r->row = nr;
        r->cap = cap;
    }

    row = &r->row[r->n++];
    memset(row, 0, sizeof *row);
    row->kind = (unsigned char)kind;
    row->style = (unsigned char)style;
    return row;
}

static void add_btn(ui_row *row, ui_action action, const char *label,
                    const char *arg, int a0, int a1, int a2)
{
    ui_btn *b;

    if (row == NULL || row->nbtn >= UI_MAX_BTN)
        return;

    b = &row->btn[row->nbtn++];
    memset(b, 0, sizeof *b);
    b->action = (unsigned char)action;
    b->a[0] = a0;
    b->a[1] = a1;
    b->a[2] = a2;
    u_copy(b->label, sizeof b->label, label);
    if (arg != NULL)
        u_copy(b->arg, sizeof b->arg, arg);
}

static void blank(ui_rows *r)
{
    add_row(r, UR_BLANK, US_NORMAL);
}

/* One wide line of text. */
static ui_row *note(ui_rows *r, ui_style style, const char *text)
{
    ui_row *row = add_row(r, UR_NOTE, style);

    if (row != NULL)
        u_copy(row->col[0], sizeof row->col[0], text);

    return row;
}

static void head(ui_rows *r, const char *title)
{
    ui_row *row = add_row(r, UR_HEAD, US_NORMAL);

    if (row != NULL)
        u_copy(row->col[0], sizeof row->col[0], title);
}

static ui_row *kv(ui_rows *r, ui_style style, const char *k, const char *v)
{
    ui_row *row = add_row(r, UR_KV, style);

    if (row != NULL) {
        u_copy(row->col[0], sizeof row->col[0], k);
        u_copy(row->col[1], sizeof row->col[1], v);
    }

    return row;
}

/* A label/value row whose value may be longer than its column holds: the
 * rest carries on in the rows below, with no label, broken at a space where
 * there is one. */
static void kv_wrapped(ui_rows *r, ui_style style, const char *k,
                       const char *v)
{
    int lines = 0;

    while (*v != '\0' && lines < 6) {
        char piece[UI_VALUE_CHARS + 1];
        size_t len = strlen(v);
        size_t n = len;

        if (len > UI_VALUE_CHARS) {
            n = UI_VALUE_CHARS;
            while (n > UI_VALUE_CHARS / 2 && v[n] != ' ')
                n--;
            if (v[n] != ' ')
                n = UI_VALUE_CHARS;
        }

        u_copyn(piece, sizeof piece, v, n);
        kv(r, style, (lines == 0) ? k : "", piece);

        v += n;
        while (*v == ' ')
            v++;
        lines++;
    }
}

/* ------------------------------------------------------------------ */
/* Model queries                                                      */
/* ------------------------------------------------------------------ */

static int is_mode_name(const char *s)
{
    int i;

    for (i = 0; mode_names[i] != NULL; i++) {
        if (strcmp(mode_names[i], s) == 0)
            return 1;
    }

    return 0;
}

int ui_mode_state(const hs_model *m, const char *mode)
{
    int i;

    for (i = 0; i < m->status.npill; i++) {
        if (strcmp(m->status.pill[i].label, mode) == 0)
            return m->status.pill[i].state;
    }

    return -1;
}

const char *ui_pill_value(const hs_model *m, const char *label)
{
    int i;

    for (i = 0; i < m->status.npill; i++) {
        if (strcmp(m->status.pill[i].label, label) == 0)
            return m->status.pill[i].value;
    }

    return NULL;
}

unsigned ui_view_focus(ui_view v)
{
    switch (v) {
        case VIEW_HEARD:
            return HS_R_HEARD;
        case VIEW_DMR:
            return HS_R_BM | HS_R_TGIF | HS_R_DMRNET;
        case VIEW_STATUS:
        case VIEW_SYSTEM:
            return HS_R_HW;
        default:
            return 0;
    }
}

unsigned ui_dmr_slots(const hs_model *m)
{
    unsigned slots = 0;

    if (ui_mode_state(m, "TS1") >= 0)
        slots |= 1u;
    if (ui_mode_state(m, "TS2") >= 0)
        slots |= 2u;

    return (slots != 0) ? slots : 3u;
}

const char *ui_current_link(const hs_model *m, const char *mode)
{
    int s;
    int i;

    for (s = 0; s < m->status.nsec; s++) {
        if (!u_istarts(m->status.sec[s].title, mode))
            continue;

        for (i = 0; i < m->status.npill; i++) {
            const wpsd_pill *p = &m->status.pill[i];

            if (p->section == s && strcmp(p->label, "Link") == 0)
                return p->value;
        }
    }

    return NULL;
}

void ui_recent_label(ui_proto proto, const char *target, char *out,
                     size_t cap)
{
    if (proto == PROTO_TGIF) {
        const char *slash = strchr(target, '/');
        char tg[HS_MRU_LEN];

        u_copyn(tg, sizeof tg, target,
                (slash != NULL) ? (size_t)(slash - target) : strlen(target));

        if (slash != NULL)
            snprintf(out, cap, "TG %.10s TS%.2s", tg, slash + 1);
        else
            snprintf(out, cap, "TG %.10s", tg);
    } else if (proto == PROTO_P25 || proto == PROTO_NXDN) {
        snprintf(out, cap, "TG %.12s", target);
    } else {
        u_copy(out, cap, target);
    }
}

static ui_style style_for_pill(unsigned char state)
{
    switch (state) {
        case PILL_ACTIVE:   return US_GOOD;
        case PILL_PAUSED:   return US_WARN;
        case PILL_ERROR:    return US_BAD;
        case PILL_INACTIVE: return US_DIM;
        default:            return US_NORMAL;
    }
}

static const char *state_word(unsigned char state)
{
    switch (state) {
        case PILL_ACTIVE:   return "active";
        case PILL_PAUSED:   return "paused";
        case PILL_ERROR:    return "error";
        case PILL_INACTIVE: return "not enabled";
        default:            return "";
    }
}

static ui_style style_for_radio(const char *state)
{
    if (u_istarts(state, "TX"))
        return US_BAD;
    if (u_istarts(state, "RX"))
        return US_ACTIVE;
    if (u_istarts(state, "OFFLINE"))
        return US_BAD;
    if (u_istarts(state, "IDLE"))
        return US_GOOD;

    return US_NORMAL;
}

/* ------------------------------------------------------------------ */
/* The common top of every view                                       */
/* ------------------------------------------------------------------ */

static void build_top(ui_rows *r, const hs_model *m, const hs_config *cfg,
                      ui_view view)
{
    ui_row *row;
    char text[256];
    int v;

    if (cfg->host[0] == '\0')
        snprintf(text, sizeof text, "Hotspot  (no address yet)");
    else if (cfg->port != 80 && cfg->port > 0)
        snprintf(text, sizeof text, "Hotspot  %s:%d", cfg->host, cfg->port);
    else
        snprintf(text, sizeof text, "Hotspot  %s", cfg->host);

    row = add_row(r, UR_HEAD, US_NORMAL);
    if (row != NULL) {
        u_copy(row->col[0], sizeof row->col[0], text);
        add_btn(row, UA_REFRESH, "Refresh", NULL, 0, 0, 0);
        add_btn(row, UA_FIND, "Find hotspot...", NULL, 0, 0, 0);
        add_btn(row, UA_CHOICES, "Choices...", NULL, 0, 0, 0);
    }

    if (m->scan_state == 1) {
        snprintf(text, sizeof text,
                 "Searching %s1 - %d: %d of %d tried, %d found",
                 m->scan_prefix, SCAN_LAST_HOST, m->scan_done, m->scan_total,
                 m->scan_nfound);
        row = note(r, US_NORMAL, text);
        add_btn(row, UA_FIND_STOP, "Stop", NULL, 0, 0, 0);
    } else if (cfg->host[0] == '\0') {
        note(r, US_WARN,
             "No hotspot address yet - use Find hotspot... or Choices...");
    } else if (m->conn < 0) {
        snprintf(text, sizeof text, "Not connected to %.60s: %s", cfg->host,
                 m->conn_msg);
        note(r, US_BAD, text);
        row = note(r, US_DIM,
                   "If the hotspot's address has changed, search for it:");
        add_btn(row, UA_FIND, "Find hotspot...", NULL, 0, 0, 0);
    } else if (m->conn == 0) {
        note(r, US_DIM, "Connecting to the hotspot...");
    } else if (m->auth_failed) {
        note(r, US_WARN,
             "Connected, but the hotspot refused the login (see Choices).");
    } else {
        note(r, US_GOOD, "Connected.");
    }

    if (m->act_seq > 0) {
        snprintf(text, sizeof text, "Last action - %s", m->act_msg);
        note(r, m->act_ok ? US_NORMAL : US_BAD, text);
    }

    row = add_row(r, UR_TABS, US_NORMAL);
    if (row != NULL) {
        for (v = 0; v < VIEW_COUNT; v++)
            add_btn(row, UA_VIEW, view_names[v], NULL, v, 0, 0);
        row->selected = (unsigned char)view;
    }

    blank(r);
}

/* ------------------------------------------------------------------ */
/* Views                                                              */
/* ------------------------------------------------------------------ */

static void build_status(ui_rows *r, const hs_model *m)
{
    int i;
    int s;

    if (m->radio_ok) {
        head(r, "Radio");
        kv(r, style_for_radio(m->radio.state), "State", m->radio.state);

        /* The first cell is the state; the rest are frequency, modem... */
        for (i = 1; i < m->radio.n; i++)
            kv_wrapped(r, US_NORMAL, m->radio.kv[i].key, m->radio.kv[i].val);

        blank(r);
    } else if (m->conn > 0) {
        head(r, "Radio");
        note(r, US_DIM, "No radio status from the hotspot yet.");
        blank(r);
    }

    if (m->status_ok) {
        for (s = 0; s < m->status.nsec; s++) {
            int any = 0;

            for (i = 0; i < m->status.npill; i++) {
                const wpsd_pill *p = &m->status.pill[i];
                ui_row *row;

                if (p->section != s)
                    continue;

                if (!any) {
                    head(r, m->status.sec[s].title);
                    any = 1;
                }

                row = kv(r, style_for_pill(p->state), p->label,
                         (p->value[0] != '\0') ? p->value
                                               : state_word(p->state));

                /* The first section is always the dashboard's "Modes Enabled"
                 * list, and each of its pills gets a Pause/Resume button.
                 * Going by position and the pill's own label (rather than
                 * the section title, which is translated) keeps this working
                 * in any dashboard language, and stops the APRS section's
                 * per-mode flags from growing buttons of their own. */
                if (row != NULL && s == 0 && is_mode_name(p->label)) {
                    if (p->state == PILL_ACTIVE)
                        add_btn(row, UA_MODE, "Pause", p->label, 1, 0, 0);
                    else if (p->state == PILL_PAUSED)
                        add_btn(row, UA_MODE, "Resume", p->label, 0, 0, 0);
                }
            }

            if (any)
                blank(r);
        }
    } else if (m->conn > 0) {
        head(r, "Modes and networks");

        if (m->status_raw[0] != '\0') {
            /* A page we have no parser for (an older or modified dashboard):
             * show its text so there is something to read, and say why the
             * Pause/Resume buttons are missing. */
            const char *p = m->status_raw;

            note(r, US_WARN,
                 "This dashboard's status page is not in a format "
                 "this version knows. Its text:");

            while (*p != '\0' && r->n < UI_MAX_ROWS) {
                const char *nl = strchr(p, '\n');
                size_t len = (nl != NULL) ? (size_t)(nl - p) : strlen(p);
                ui_row *row = add_row(r, UR_NOTE, US_NORMAL);

                if (row != NULL)
                    u_copyn(row->col[0], sizeof row->col[0], p, len);
                p = (nl != NULL) ? nl + 1 : p + len;
            }
        } else {
            note(r, US_DIM,
                 "The hotspot's status page has not arrived yet.");
        }

        blank(r);
    }

    if (m->hw_ok) {
        head(r, "Hotspot");
        for (i = 0; i < m->hw.n; i++)
            kv_wrapped(r, US_NORMAL, m->hw.kv[i].key, m->hw.kv[i].val);
        blank(r);
    }
}

/* "DMR Slot 2" -> "DMR S2" so it fits its column. */
static void mode_short(char *dst, size_t cap, const char *mode)
{
    char tmp[28];
    char *slot;

    u_copy(tmp, sizeof tmp, mode);
    slot = strstr(tmp, "Slot ");
    if (slot != NULL) {
        slot[0] = 'S';
        memmove(slot + 1, slot + 5, strlen(slot + 5) + 1);
    }

    u_copy(dst, cap, tmp);
}

static void build_heard(ui_rows *r, const hs_model *m)
{
    int i;
    ui_row *row;

    head(r, "Last heard");

    if (!m->heard_ok) {
        note(r, US_DIM, "No last heard list from the hotspot yet.");
        return;
    }

    row = add_row(r, UR_HEARD, US_DIM);
    if (row != NULL) {
        u_copy(row->col[0], sizeof row->col[0], "UTC");
        u_copy(row->col[1], sizeof row->col[1], "Callsign");
        u_copy(row->col[2], sizeof row->col[2], "Mode");
        u_copy(row->col[3], sizeof row->col[3], "Target");
        u_copy(row->col[4], sizeof row->col[4], "Duration/loss");
    }

    if (m->heard.n == 0) {
        note(r, US_DIM, "Nothing heard yet.");
        return;
    }

    for (i = 0; i < m->heard.n; i++) {
        const wpsd_heard *h = &m->heard.row[i];
        char info[48];

        row = add_row(r, UR_HEARD, h->active ? US_ACTIVE : US_NORMAL);
        if (row == NULL)
            return;

        u_copy(row->col[0], sizeof row->col[0], h->time);

        if (h->name[0] != '\0')
            snprintf(row->col[1], sizeof row->col[1], "%s  %s", h->call,
                     h->name);
        else
            u_copy(row->col[1], sizeof row->col[1], h->call);

        mode_short(row->col[2], sizeof row->col[2], h->mode);
        u_copy(row->col[3], sizeof row->col[3], h->target);

        if (h->active) {
            snprintf(info, sizeof info, "on air (%s)", h->src);
        } else {
            const char *unit = (h->dur[0] >= '0' && h->dur[0] <= '9') ? "s"
                                                                     : "";

            snprintf(info, sizeof info, "%s%s %s %s", h->dur, unit, h->loss,
                     h->src);
        }

        u_copy(row->col[4], sizeof row->col[4], info);
    }
}

/* The remembered targets for `proto` as one-click buttons under `title`. */
static void recents_row(ui_rows *r, const hs_config *cfg, ui_proto proto,
                        const char *title)
{
    ui_row *row;
    int i;

    if (cfg->recent[proto][0][0] == '\0')
        return;

    row = note(r, US_DIM, title);
    for (i = 0; i < HS_MRU_N && cfg->recent[proto][i][0] != '\0'; i++) {
        char label[24];

        ui_recent_label(proto, cfg->recent[proto][i], label, sizeof label);
        add_btn(row, UA_LINK_TO, label, cfg->recent[proto][i], (int)proto, 0,
                0);
    }
}

static void build_bm(ui_rows *r, const hs_model *m, const hs_config *cfg)
{
    int i;
    ui_row *row;
    char text[256];

    if (!cfg->show_bm) {
        head(r, "BrandMeister");
        note(r, US_DIM, "BrandMeister support is switched off in Choices.");
        return;
    }

    if (m->bm_state == 1) {
        snprintf(text, sizeof text, "BrandMeister  %s  %s", m->bm.id,
                 m->bm.network);
        head(r, text);

        if (m->bm.nst == 0)
            note(r, US_DIM, "No static talkgroups.");

        for (i = 0; i < m->bm.nst; i++) {
            const wpsd_bm_static *s = &m->bm.st[i];
            char tg[16];
            char ts[8];

            snprintf(tg, sizeof tg, "TG %d", s->tg);
            snprintf(ts, sizeof ts, "TS%d", s->disp_slot);

            row = add_row(r, UR_BM, s->linked ? US_GOOD : US_DIM);
            if (row == NULL)
                return;

            u_copy(row->col[0], sizeof row->col[0], tg);
            u_copy(row->col[1], sizeof row->col[1], ts);
            u_copy(row->col[2], sizeof row->col[2], s->name);
            u_copy(row->col[3], sizeof row->col[3],
                   s->linked ? "linked" : "not linked");

            add_btn(row, UA_BM_LINK, s->linked ? "Drop" : "Link", NULL,
                    s->tg, s->slot, s->linked ? 0 : 1);
            add_btn(row, UA_BM_DELETE, "Delete", NULL, s->tg, s->slot, 0);
        }

        row = note(r, US_NORMAL, "Add a static talkgroup:");
        add_btn(row, UA_BM_ADD, "Add TG...", NULL, 0, 0, 0);

        blank(r);
        head(r, "Dynamic talkgroups");

        if (m->bm.ndyn == 0)
            note(r, US_DIM, "None linked.");

        for (i = 0; i < m->bm.ndyn; i++) {
            const wpsd_bm_dyn *d = &m->bm.dyn[i];
            char tg[16];
            char ts[8];

            snprintf(tg, sizeof tg, "TG %d", d->tg);
            snprintf(ts, sizeof ts, "TS%d", d->disp_slot);

            row = add_row(r, UR_BM, US_NORMAL);
            if (row == NULL)
                return;

            u_copy(row->col[0], sizeof row->col[0], tg);
            u_copy(row->col[1], sizeof row->col[1], ts);
            u_copy(row->col[2], sizeof row->col[2], d->name);
            u_copy(row->col[3], sizeof row->col[3], d->timeout);
        }

        for (i = 0; i < m->bm.ndrop; i++) {
            int slot = m->bm.drop_slot[i];

            if (slot == 0)
                snprintf(text, sizeof text, "Hotspot");
            else
                snprintf(text, sizeof text, "Timeslot %d", slot);

            row = note(r, US_NORMAL, text);
            add_btn(row, UA_BM_DROP_QSO, "Drop QSO", NULL, 0, slot, 0);
            add_btn(row, UA_BM_DROP_DYN, "Drop dynamic", NULL, 0, slot, 0);
        }
    } else if (m->bm_state == 2) {
        head(r, "BrandMeister");
        note(r, US_WARN, m->bm.notice);
    } else if (m->bm_state == -2) {
        head(r, "BrandMeister");
        note(r, US_WARN, "The hotspot refused the login - check Choices.");
    } else if (m->bm_state == -1) {
        head(r, "BrandMeister");
        note(r, US_DIM, "This hotspot has no BrandMeister manager page.");
    } else {
        head(r, "BrandMeister");
        note(r, US_DIM, "Reading the BrandMeister page...");
    }
}

static int is_unlinked_text(const char *v)
{
    size_t n = strlen(v);

    return v[0] == '\0' || u_ifind(v, n, "not linked") != NULL ||
           u_ifind(v, n, "unlinked") != NULL ||
           u_ifind(v, n, "not started") != NULL ||
           u_ifind(v, n, "none") != NULL;
}

static void link_row(ui_rows *r, const hs_model *m, const hs_config *cfg,
                     const char *mode, const char *title,
                     const char *recent_title, ui_proto proto)
{
    int state = ui_mode_state(m, mode);
    const char *cur;
    ui_style style;
    char text[112];
    ui_row *row;

    if (state < 0 || state == PILL_INACTIVE)
        return;

    cur = ui_current_link(m, mode);

    if (state == PILL_PAUSED) {
        style = US_WARN;
        u_copy(text, sizeof text, "mode paused");
    } else if (cur == NULL || is_unlinked_text(cur)) {
        style = US_DIM;
        u_copy(text, sizeof text, (cur != NULL && cur[0] != '\0')
                                      ? cur : "not linked");
    } else {
        style = US_GOOD;
        snprintf(text, sizeof text, "linked to %s", cur);
    }

    row = kv(r, style, title, text);
    if (row != NULL) {
        add_btn(row, UA_LINK, "Link...", NULL, (int)proto, 1, 0);
        add_btn(row, UA_LINK, "Unlink", NULL, (int)proto, 0, 0);
    }

    recents_row(r, cfg, proto, recent_title);
}

static void build_links(ui_rows *r, const hs_model *m, const hs_config *cfg)
{
    head(r, "Reflectors and talkgroups");

    if (!m->status_ok) {
        note(r, US_DIM,
             "Waiting for the hotspot's mode list before showing links.");
        return;
    }

    link_row(r, m, cfg, "YSF", "YSF reflector", "Recent YSF reflectors:",
             PROTO_YSF);
    link_row(r, m, cfg, "D-Star", "D-Star reflector",
             "Recent D-Star reflectors:", PROTO_DSTAR);
    link_row(r, m, cfg, "P25", "P25 talkgroup", "Recent P25 talkgroups:",
             PROTO_P25);
    link_row(r, m, cfg, "NXDN", "NXDN talkgroup", "Recent NXDN talkgroups:",
             PROTO_NXDN);

    blank(r);
    note(r, US_DIM, "DMR (BrandMeister and TGIF) is on the DMR tab.");
}

/* ------------------------------------------------------------------ */
/* The DMR tab: networks, BrandMeister, TGIF                          */
/* ------------------------------------------------------------------ */

static void build_dmr_networks(ui_rows *r, const hs_model *m)
{
    int i;

    if (m->dmrnet_state == 1) {
        head(r, "DMR networks");

        for (i = 0; i < m->dmrnets.n; i++) {
            const wpsd_dmrnet *n = &m->dmrnets.net[i];
            ui_row *row = kv(r, n->enabled ? US_GOOD : US_DIM, n->name,
                             n->enabled ? "enabled" : "disabled");

            add_btn(row, UA_DMRNET, n->enabled ? "Disable" : "Enable", n->id,
                    n->enabled ? 0 : 1, 0, 0);
        }

        note(r, US_DIM,
             "A network stays off only until the hotspot restarts or updates.");
        blank(r);
    } else if (m->dmrnet_state == -2) {
        head(r, "DMR networks");
        note(r, US_WARN, "The hotspot refused the login - check Choices.");
        blank(r);
    }
}

static int has_tgif_pill(const hs_model *m)
{
    int i;

    for (i = 0; i < m->status.npill; i++) {
        if (u_ifind(m->status.pill[i].label, strlen(m->status.pill[i].label),
                    "TGIF") != NULL)
            return 1;
    }

    return 0;
}

static void build_tgif(ui_rows *r, const hs_model *m, const hs_config *cfg)
{
    unsigned slots = ui_dmr_slots(m);
    char text[160];
    ui_row *row;
    int s;

    if (m->tgif_state != 1 && !has_tgif_pill(m))
        return;                         /* this hotspot does not use TGIF */

    if (m->tgif_state == 1 && m->tgif.id[0] != '\0')
        snprintf(text, sizeof text, "TGIF  %s", m->tgif.id);
    else
        u_copy(text, sizeof text, "TGIF");
    head(r, text);

    if (m->tgif_state == 1) {
        for (s = 1; s <= 2; s++) {
            int tg = m->tgif.tg[s - 1];
            char what[112];

            if (!(slots & (1u << (s - 1))))
                continue;

            snprintf(text, sizeof text, "Timeslot %d", s);

            if (tg > 0) {
                if (m->tgif.name[s - 1][0] != '\0')
                    snprintf(what, sizeof what, "TG %d  %s", tg,
                             m->tgif.name[s - 1]);
                else
                    snprintf(what, sizeof what, "TG %d", tg);
                row = kv(r, US_GOOD, text, what);
            } else {
                row = kv(r, US_DIM, text, "not linked");
            }

            add_btn(row, UA_LINK, "Link...", NULL, (int)PROTO_TGIF, 1, s);
            add_btn(row, UA_LINK, "Unlink", NULL, (int)PROTO_TGIF, 0, s);
        }
    } else {
        /* TGIF is set up, but the page that says which talkgroups are linked
         * has nothing to show: still allow linking, blind. */
        row = kv(r, US_NORMAL, "TGIF talkgroup", "current links not shown");
        add_btn(row, UA_LINK, "Link...", NULL, (int)PROTO_TGIF, 1, 0);
        add_btn(row, UA_LINK, "Unlink", NULL, (int)PROTO_TGIF, 0, 0);
    }

    recents_row(r, cfg, PROTO_TGIF, "Recent talkgroups:");
}

static void build_dmr(ui_rows *r, const hs_model *m, const hs_config *cfg)
{
    int dmr = ui_mode_state(m, "DMR");

    if (m->status_ok && (dmr < 0 || dmr == PILL_INACTIVE)) {
        head(r, "DMR");
        note(r, US_DIM, "DMR is not enabled on this hotspot.");
        return;
    }

    if (dmr == PILL_PAUSED) {
        note(r, US_WARN,
             "DMR is paused - resume it on the Status tab to use these.");
        blank(r);
    }

    build_dmr_networks(r, m);
    build_bm(r, m, cfg);
    blank(r);
    build_tgif(r, m, cfg);
}

static void build_system(ui_rows *r, const hs_model *m)
{
    int i;
    ui_row *row;

    head(r, "Hotspot services");

    row = kv(r, US_NORMAL, "Restart WPSD services", "");
    add_btn(row, UA_SYS, "Restart", "restart_wpsd_services", 0, 0, 0);

    row = kv(r, US_NORMAL, "Update host files", "");
    add_btn(row, UA_SYS, "Update", "update_hostfiles", 0, 0, 0);

    row = kv(r, US_NORMAL, "Power", "");
    add_btn(row, UA_SYS, "Reboot", "reboot", 0, 0, 0);
    add_btn(row, UA_SYS, "Shut down", "shutdown", 0, 0, 0);

    blank(r);
    head(r, "This program");

    row = kv(r, US_NORMAL, "Save diagnostics report", "");
    add_btn(row, UA_DIAG, "Save", NULL, 0, 0, 0);

    if (m->nreq > 0) {
        blank(r);
        head(r, "How the last requests went");
        for (i = 0; i < m->nreq; i++)
            note(r, (strstr(m->req_line[i], "not understood") != NULL ||
                     strstr(m->req_line[i], "HTTP 4") != NULL ||
                     strstr(m->req_line[i], "HTTP 5") != NULL)
                        ? US_WARN : US_DIM,
                 m->req_line[i]);
    }

    if (m->hw_ok) {
        blank(r);
        head(r, "Hotspot hardware");
        for (i = 0; i < m->hw.n; i++)
            kv_wrapped(r, US_NORMAL, m->hw.kv[i].key, m->hw.kv[i].val);
    }
}

void ui_rows_build(ui_rows *r, const hs_model *m, const hs_config *cfg,
                   ui_view view)
{
    r->n = 0;

    build_top(r, m, cfg, view);

    switch (view) {
        case VIEW_HEARD:
            build_heard(r, m);
            break;
        case VIEW_DMR:
            build_dmr(r, m, cfg);
            break;
        case VIEW_LINKS:
            build_links(r, m, cfg);
            break;
        case VIEW_SYSTEM:
            build_system(r, m);
            break;
        case VIEW_STATUS:
        default:
            build_status(r, m);
            break;
    }
}
