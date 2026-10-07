/*
 * ro_act.c
 *
 * What every button and menu item does. Both the window's buttons and the
 * menus carry a ui_btn descriptor; act_run() turns it into either a request
 * for the client (hs_do), a confirmation or a prompt dialogue first, or a
 * change to the display.
 *
 * Replies are not handled here: the client reports the outcome in its model
 * ("last action") and the poll loop shows failures. Two outcomes do come back
 * through here, because they need the front end's help: a link that worked is
 * remembered for one-click re-use (act_action_finished) and the end of a
 * search for the hotspot offers what it found (act_scan_results).
 */

#include "ro.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scan.h"
#include "util.h"

static void submit(const hs_action *a)
{
    if (hs_do(app.client, a) < 0)
        ro_error("Could not send \"%s\": the value is not valid, or there "
                 "are too many requests waiting.", a->label);
}

static hs_action new_action(hs_act_kind kind, const char *label)
{
    hs_action a;

    memset(&a, 0, sizeof a);
    a.kind = kind;
    u_copy(a.label, sizeof a.label, label);
    return a;
}

/* ------------------------------------------------------------------ */
/* Remembering what was linked                                        */
/* ------------------------------------------------------------------ */

/* The link that was last sent, waiting to hear whether it worked. It is
 * matched to its outcome by the label, which the client puts at the start of
 * its result message ("<label>: <what the hotspot said>"). */
static struct {
    int  active;
    int  proto;
    char target[HS_MRU_LEN];
    char label[48];
} pending;

static void remember_if_ok(ui_proto proto, const char *target,
                           const char *label)
{
    pending.active = 1;
    pending.proto = (int)proto;
    u_copy(pending.target, sizeof pending.target, target);
    u_copy(pending.label, sizeof pending.label, label);
}

void act_action_finished(int ok)
{
    const hs_model *m = hs_get(app.client);
    size_t n;
    const char *err;

    if (!pending.active)
        return;

    n = strlen(pending.label);
    if (strncmp(m->act_msg, pending.label, n) != 0 || m->act_msg[n] != ':')
        return;                     /* some other action's outcome */

    pending.active = 0;
    if (!ok)
        return;

    hs_mru_push(&app.cfg, pending.proto, pending.target);
    hs_set_config(app.client, &app.cfg);

    err = choices_save(&app.cfg);
    if (err != NULL)
        ro_error("%s", err);

    win_model_changed(0);
}

/* ------------------------------------------------------------------ */
/* Choices                                                            */
/* ------------------------------------------------------------------ */

static void choices_ok(char v[][DLG_BUF], const int *checks, void *ud)
{
    hs_config cfg = app.cfg;
    int port;

    (void)ud;

    u_copy(cfg.host, sizeof cfg.host, v[0]);
    port = atoi(v[1]);
    cfg.port = (port > 0 && port < 65536) ? port : 80;
    u_copy(cfg.user, sizeof cfg.user, v[2]);
    u_copy(cfg.pass, sizeof cfg.pass, v[3]);
    cfg.refresh_s = atoi(v[4]);
    cfg.heard_rows = atoi(v[5]);
    cfg.show_bm = checks[0];
    cfg.names = checks[1];

    ro_apply_config(&cfg);
}

void act_choices(void)
{
    dlg_spec s;
    char port[12];
    char refresh[12];
    char rows[12];

    memset(&s, 0, sizeof s);
    snprintf(port, sizeof port, "%d", app.cfg.port);
    snprintf(refresh, sizeof refresh, "%d", app.cfg.refresh_s);
    snprintf(rows, sizeof rows, "%d", app.cfg.heard_rows);

    s.title = "Hotspot choices";
    s.text = "Your hotspot's WPSD dashboard address. If it changes,\n"
             "edit it here or use Find hotspot... in the main window.\n"
             "The user name and password are the hotspot's Admin\n"
             "login: needed to change things, not just to look.";
    s.nfields = 6;
    s.field[0].label = "Hotspot address (an IP address such as 10.0.0.27)";
    s.field[0].initial = app.cfg.host;
    s.field[0].size = 96;
    s.field[1].label = "Port";
    s.field[1].initial = port;
    s.field[1].size = 8;
    s.field[1].validation = "A0-9";
    s.field[2].label = "User name";
    s.field[2].initial = app.cfg.user;
    s.field[2].size = 48;
    s.field[3].label = "Password";
    s.field[3].initial = app.cfg.pass;
    s.field[3].size = 48;
    s.field[3].password = 1;
    s.field[4].label = "Refresh every (seconds)";
    s.field[4].initial = refresh;
    s.field[4].size = 6;
    s.field[4].validation = "A0-9";
    s.field[5].label = "Last heard rows to fetch";
    s.field[5].initial = rows;
    s.field[5].size = 4;
    s.field[5].validation = "A0-9";
    s.nchecks = 2;
    s.check[0].label = "Show BrandMeister talkgroups";
    s.check[0].initial = app.cfg.show_bm;
    s.check[1].label = "Look up operators' names (slower on the hotspot)";
    s.check[1].initial = app.cfg.names;
    s.ok_label = "Save";
    s.on_ok = choices_ok;

    dlg_open(&s);
}

/* ------------------------------------------------------------------ */
/* Finding the hotspot                                                */
/* ------------------------------------------------------------------ */

static void find_ok(char v[][DLG_BUF], const int *checks, void *ud)
{
    (void)checks;
    (void)ud;

    if (hs_scan_start(app.client, v[0],
                      (unsigned long)os_read_monotonic_time()) < 0) {
        ro_error("\"%s\" is not the start of an IP address. Type the first "
                 "three numbers, such as 10.0.0", v[0]);
        return;
    }

    /* Show the progress: it is the main window that says how far it is. */
    win_open();
    win_model_changed(HS_C_SCAN);
}

void act_find(void)
{
    dlg_spec s;
    char prefix[24];

    memset(&s, 0, sizeof s);

    /* Start from the neighbourhood of the address in use: when a hotspot
     * moves it usually stays on the same network. */
    if (!scan_prefix_from(app.cfg.host, prefix, sizeof prefix))
        prefix[0] = '\0';

    s.title = "Find the hotspot";
    s.text = "Looks on your network for a hotspot running WPSD,\n"
             "trying every address from 1 to 254 in the range.\n"
             "Give the first three numbers of your network's\n"
             "addresses, for example 10.0.0 or 192.168.1";
    s.nfields = 1;
    s.field[0].label = "Search addresses starting with";
    s.field[0].initial = prefix;
    s.field[0].size = 20;
    s.field[0].validation = "A0-9.";
    s.ok_label = "Search";
    s.on_ok = find_ok;

    dlg_open(&s);
}

/* The search is over (HS_C_SCAN with the results in): offer each hotspot it
 * found, one at a time. */
void act_scan_results(void)
{
    const hs_model *m = hs_get(app.client);
    struct {
        char addr[24];
        char info[64];
    } found[4];
    char prefix[24];
    int n;
    int i;
    int offered = 0;

    if (m->scan_state != 2)
        return;

    n = m->scan_nfound;
    if (n > 4)
        n = 4;

    for (i = 0; i < n; i++) {
        u_copy(found[i].addr, sizeof found[i].addr, m->scan_addr[i]);
        u_copy(found[i].info, sizeof found[i].info, m->scan_info[i]);
    }
    u_copy(prefix, sizeof prefix, m->scan_prefix);

    /* The boxes below wait for the user, and applying a new address resets
     * the client, so the search is cleared first. */
    hs_scan_clear(app.client);
    win_model_changed(HS_C_SCAN);

    if (n == 0) {
        /* (The error box wraps its own text: a newline would end it.) */
        ro_error("No hotspot was found at %s1 - %s254. Check that it is "
                 "switched on and connected to the same network as this "
                 "computer, or try another range.", prefix, prefix);
        return;
    }

    for (i = 0; i < n; i++) {
        char what[112];
        hs_config cfg;

        if (strcmp(found[i].addr, app.cfg.host) == 0 && n > 1)
            continue;               /* offer the others first */

        if (strcmp(found[i].addr, app.cfg.host) == 0) {
            ro_info("The hotspot is at %s, the address already in use. "
                    "If it is not connecting, check the password in "
                    "Choices.", found[i].addr);
            return;
        }

        if (found[i].info[0] != '\0')
            snprintf(what, sizeof what, "%.24s (%.64s)", found[i].addr,
                     found[i].info);
        else
            u_copy(what, sizeof what, found[i].addr);

        offered++;
        if (ro_confirm("Found a hotspot at %s. Use this address?", what)) {
            cfg = app.cfg;
            u_copy(cfg.host, sizeof cfg.host, found[i].addr);
            ro_apply_config(&cfg);
            return;
        }
    }

    if (offered == 0)
        ro_info("The hotspot is at %s, the address already in use.",
                app.cfg.host);
}

/* ------------------------------------------------------------------ */
/* Diagnostics                                                        */
/* ------------------------------------------------------------------ */

/* "Help...": runs the !Help file in the application directory, which opens
 * in whatever the user has for Text files, as a double-click would. */
static void help_run(void)
{
    os_error *error = xwimp_start_task("Filer_Run <Hotspot$Dir>.!Help", NULL);

    if (error != NULL)
        ro_error("Could not open the help file: %s", error->errmess);
}

void act_save_diagnostics(void)
{
    char path[256];
    const char *err = diag_save(app.client, path, sizeof path);

    if (err != NULL)
        ro_error("%s", err);
    else
        ro_info("Saved the diagnostics report as %s", path);
}

/* ------------------------------------------------------------------ */
/* DMR networks                                                       */
/* ------------------------------------------------------------------ */

static void dmrnet_switch(const char *id, int enable)
{
    const hs_model *m = hs_get(app.client);
    const char *name = id;
    char label[48];
    hs_action a;
    int i;

    for (i = 0; i < m->dmrnets.n; i++) {
        if (strcmp(m->dmrnets.net[i].id, id) == 0)
            name = m->dmrnets.net[i].name;
    }

    snprintf(label, sizeof label, "%s %.36s", enable ? "Enable" : "Disable",
             name);
    a = new_action(HS_ACT_DMRNET, label);
    u_copy(a.s1, sizeof a.s1, id);
    a.i3 = enable;
    submit(&a);
}

/* ------------------------------------------------------------------ */
/* BrandMeister                                                       */
/* ------------------------------------------------------------------ */

static void bm_add_ok(char v[][DLG_BUF], const int *checks, void *ud)
{
    hs_action a = new_action(HS_ACT_BM_ADD, "Add talkgroup");
    int duplex = (int)(intptr_t)ud;

    (void)checks;

    u_copy(a.s1, sizeof a.s1, v[0]);
    a.i2 = 0;
    if (duplex) {
        a.i2 = atoi(v[1]);
        if (a.i2 < 1 || a.i2 > 2) {
            ro_error("The timeslot must be 1 or 2.");
            return;
        }
    }

    submit(&a);
}

static void bm_add(void)
{
    const hs_model *m = hs_get(app.client);
    int duplex = m->bm.ndrop > 0 && m->bm.drop_slot[0] != 0;
    dlg_spec s;

    memset(&s, 0, sizeof s);
    s.title = "Add a static talkgroup";
    s.text = "Several talkgroups can be typed at once,\n"
             "separated by spaces or commas.";
    s.nfields = duplex ? 2 : 1;
    s.field[0].label = "Talkgroup number(s)";
    s.field[0].size = 60;
    s.field[0].validation = "A0-9 ,";
    s.field[1].label = "Timeslot (1 or 2)";
    s.field[1].initial = "2";
    s.field[1].size = 3;
    s.field[1].validation = "A12";
    s.ok_label = "Add";
    s.on_ok = bm_add_ok;
    s.ud = (void *)(intptr_t)duplex;

    dlg_open(&s);
}

/* ------------------------------------------------------------------ */
/* Reflector and talkgroup links                                      */
/* ------------------------------------------------------------------ */

static void upper(char *s)
{
    for (; *s != '\0'; s++)
        *s = (char)toupper((unsigned char)*s);
}

/* Sends the link of `proto` to `target` - the text as typed, or as
 * remembered: a reflector or talkgroup, "REF001 C" for D-Star, "2341/2"
 * (talkgroup / timeslot) for TGIF - and arranges for it to be remembered if
 * the hotspot accepts it. `slot` is the TGIF timeslot when the typed value
 * has none (0 = the default, 2). `module` is the D-Star radio module the
 * user confirmed (NULL: whatever the status page says). */
static void send_link(ui_proto proto, const char *target, int slot,
                      const char *module)
{
    const hs_model *m = hs_get(app.client);
    char remembered[HS_MRU_LEN];
    char label[48];
    hs_action a;

    u_copy(remembered, sizeof remembered, target);

    switch (proto) {
        case PROTO_YSF:
            /* A value from the reflector list says YSF or FCS already. */
            if (isalpha((unsigned char)target[0]))
                snprintf(label, sizeof label, "Link %.36s", target);
            else
                snprintf(label, sizeof label, "Link YSF %.30s", target);
            a = new_action(HS_ACT_YSF, label);
            u_copy(a.s1, sizeof a.s1, target);
            break;

        case PROTO_P25:
            snprintf(label, sizeof label, "Link P25 TG %.30s", target);
            a = new_action(HS_ACT_P25, label);
            u_copy(a.s1, sizeof a.s1, target);
            break;

        case PROTO_NXDN:
            snprintf(label, sizeof label, "Link NXDN TG %.30s", target);
            a = new_action(HS_ACT_NXDN, label);
            u_copy(a.s1, sizeof a.s1, target);
            break;

        case PROTO_DSTAR: {
            char name[HS_MRU_LEN];
            const char *space = strrchr(target, ' ');
            const char *rpt = (module != NULL && module[0] != '\0')
                                  ? module : ui_pill_value(m, "RPT1");

            if (space == NULL) {
                ro_error("A D-Star link needs the reflector and its module "
                         "letter.");
                return;
            }

            if (rpt == NULL || rpt[0] == '\0') {
                ro_error("The hotspot has not told us its D-Star radio "
                         "module yet; try again after the next refresh.");
                return;
            }

            u_copyn(name, sizeof name, target, (size_t)(space - target));
            u_trim(name);

            snprintf(label, sizeof label, "Link D-Star %.30s", target);
            a = new_action(HS_ACT_DSTAR, label);
            u_copy(a.s2, sizeof a.s2, name);
            u_copy(a.s3, sizeof a.s3, space + 1);
            u_copy(a.s1, sizeof a.s1, rpt);
            break;
        }

        case PROTO_TGIF:
        default: {
            const char *slash = strchr(target, '/');

            a = new_action(HS_ACT_TGIF, "");
            a.i1 = atoi(target);
            a.i2 = (slash != NULL) ? atoi(slash + 1) : slot;
            if (a.i2 < 1 || a.i2 > 2)
                a.i2 = 2;

            snprintf(label, sizeof label, "Link TGIF TG %d TS%d", a.i1, a.i2);
            u_copy(a.label, sizeof a.label, label);
            snprintf(remembered, sizeof remembered, "%d/%d", a.i1, a.i2);
            break;
        }
    }

    a.i3 = 1;
    remember_if_ok(proto, remembered, label);
    submit(&a);
}

/* What a link dialogue's OK needs to know besides the typed values. */
#define LINK_UD(proto, slot)   ((void *)(intptr_t)((int)(proto) | ((slot) << 8)))

static void link_ok(char v[][DLG_BUF], const int *checks, void *ud)
{
    int packed = (int)(intptr_t)ud;
    ui_proto proto = (ui_proto)(packed & 0xFF);
    int slot = packed >> 8;
    char target[DLG_BUF];

    (void)checks;

    switch (proto) {
        case PROTO_DSTAR:
            /* "reflector module", upper case as D-Star writes them. */
            snprintf(target, sizeof target, "%.12s %.2s", v[0], v[1]);
            upper(target);
            break;

        case PROTO_TGIF:
            if (slot < 1 || slot > 2)
                slot = atoi(v[1]);
            if (slot < 1 || slot > 2) {
                ro_error("The timeslot must be 1 or 2.");
                return;
            }
            snprintf(target, sizeof target, "%d/%d", atoi(v[0]), slot);
            break;

        default:
            u_copy(target, sizeof target, v[0]);
            break;
    }

    /* For D-Star the third field is the radio module, pre-filled from the
     * status page and editable in case that was not readable. */
    send_link(proto, target, slot, (proto == PROTO_DSTAR) ? v[2] : NULL);
}

static void link_dialog(ui_proto proto, int slot)
{
    const hs_model *m = hs_get(app.client);
    const char *rpt = ui_pill_value(m, "RPT1");
    char title[64];
    dlg_spec s;

    memset(&s, 0, sizeof s);
    s.on_ok = link_ok;
    s.ud = LINK_UD(proto, slot);
    s.ok_label = "Link";

    switch (proto) {
        case PROTO_YSF:
            s.title = "Link a YSF reflector";
            s.text = "A YSF number such as 00001, or FCS followed\n"
                     "by the room number such as FCS00123.";
            s.nfields = 1;
            s.field[0].label = "Reflector";
            s.field[0].size = 16;
            s.field[0].validation = "AA-Za-z0-9";
            break;

        case PROTO_P25:
            s.title = "Link a P25 talkgroup";
            s.nfields = 1;
            s.field[0].label = "Talkgroup";
            s.field[0].size = 10;
            s.field[0].validation = "A0-9";
            break;

        case PROTO_NXDN:
            s.title = "Link an NXDN talkgroup";
            s.nfields = 1;
            s.field[0].label = "Talkgroup";
            s.field[0].size = 10;
            s.field[0].validation = "A0-9";
            break;

        case PROTO_DSTAR:
            s.title = "Link a D-Star reflector";
            s.text = "For REF001 module C, enter REF001 and C.";
            s.nfields = 3;
            s.field[0].label = "Reflector (such as REF001, XLX123, DCS001)";
            s.field[0].size = 9;
            s.field[0].validation = "AA-Za-z0-9";
            s.field[1].label = "Module letter";
            s.field[1].size = 3;
            s.field[1].validation = "AA-Za-z";
            s.field[2].label = "Your radio module (as the status page shows)";
            s.field[2].initial = (rpt != NULL) ? rpt : "";
            s.field[2].size = 12;
            s.field[2].validation = "AA-Za-z0-9 ";
            break;

        case PROTO_TGIF:
        default:
            if (slot == 1 || slot == 2) {
                snprintf(title, sizeof title,
                         "Link a TGIF talkgroup on timeslot %d", slot);
                s.title = title;
                s.nfields = 1;
            } else {
                s.title = "Link a TGIF talkgroup";
                s.nfields = 2;
            }
            s.field[0].label = "Talkgroup";
            s.field[0].size = 10;
            s.field[0].validation = "A0-9";
            s.field[1].label = "Timeslot (1 or 2)";
            s.field[1].initial = "2";
            s.field[1].size = 3;
            s.field[1].validation = "A12";
            break;
    }

    dlg_open(&s);
}

static void unlink_now(ui_proto proto, int slot)
{
    const hs_model *m = hs_get(app.client);
    char label[48];
    hs_action a;

    switch (proto) {
        case PROTO_YSF:
            a = new_action(HS_ACT_YSF, "Unlink YSF");
            break;
        case PROTO_P25:
            a = new_action(HS_ACT_P25, "Unlink P25");
            break;
        case PROTO_NXDN:
            a = new_action(HS_ACT_NXDN, "Unlink NXDN");
            break;
        case PROTO_TGIF:
            if (slot < 1 || slot > 2)
                slot = 2;
            snprintf(label, sizeof label, "Unlink TGIF timeslot %d", slot);
            a = new_action(HS_ACT_TGIF, label);
            a.i2 = slot;
            break;
        case PROTO_DSTAR:
        default: {
            const char *rpt = ui_pill_value(m, "RPT1");

            a = new_action(HS_ACT_DSTAR, "Unlink D-Star");
            if (rpt == NULL || rpt[0] == '\0') {
                ro_error("The hotspot has not told us its D-Star radio "
                         "module yet; try again after the next refresh.");
                return;
            }

            u_copy(a.s1, sizeof a.s1, rpt);
            break;
        }
    }

    a.i3 = 0;
    submit(&a);
}

/* ------------------------------------------------------------------ */
/* The YSF reflector list                                             */
/* ------------------------------------------------------------------ */

static void ysf_search_ok(char v[][DLG_BUF], const int *checks, void *ud)
{
    (void)checks;
    (void)ud;

    ui_set_ysf_filter(v[0]);
    win_model_changed(HS_R_YSF);
}

static void ysf_search(void)
{
    dlg_spec s;

    memset(&s, 0, sizeof s);
    s.title = "Search YSF reflectors";
    s.text = "Part of a reflector's number, name or place,\n"
             "such as 00123, calling, italy or fcs.";
    s.nfields = 1;
    s.field[0].label = "Search for";
    s.field[0].initial = ui_ysf_filter();
    s.field[0].size = 30;
    s.ok_label = "Search";
    s.on_ok = ysf_search_ok;

    dlg_open(&s);
}

/* ------------------------------------------------------------------ */
/* System actions                                                     */
/* ------------------------------------------------------------------ */

static const struct {
    const char *action;
    const char *label;
    const char *question;
} sys_actions[] = {
    { "restart_wpsd_services", "Restart services",
      "Restart all the WPSD services on the hotspot? Radio traffic stops "
      "for a few seconds." },
    { "update_hostfiles", "Update host files",
      "Download fresh reflector and host lists on the hotspot now?" },
    { "reboot", "Reboot",
      "Reboot the hotspot now?" },
    { "shutdown", "Shut down",
      "Shut the hotspot down? It will need its power cycling to come back." },
    { NULL, NULL, NULL }
};

static void sys_action(const char *action)
{
    int i;

    for (i = 0; sys_actions[i].action != NULL; i++) {
        if (strcmp(sys_actions[i].action, action) == 0) {
            hs_action a;

            if (!ro_confirm("%s", sys_actions[i].question))
                return;

            a = new_action(HS_ACT_SYS, sys_actions[i].label);
            u_copy(a.s1, sizeof a.s1, action);
            submit(&a);
            return;
        }
    }
}

/* ------------------------------------------------------------------ */
/* The dispatcher                                                     */
/* ------------------------------------------------------------------ */

void act_run(const ui_btn *b)
{
    hs_action a;
    char label[48];

    switch (b->action) {
        case UA_REFRESH:
            hs_refresh(app.client, win_visible_parts());
            break;

        case UA_CHOICES:
            act_choices();
            break;

        case UA_FIND:
            act_find();
            break;

        case UA_FIND_STOP:
            hs_scan_stop(app.client);
            win_model_changed(HS_C_SCAN);
            break;

        case UA_VIEW:
            win_set_view((ui_view)b->a[0]);
            break;

        case UA_QUIT:
            app.quit = 1;
            break;

        case UA_DIAG:
            act_save_diagnostics();
            break;

        case UA_HELP:
            help_run();
            break;

        case UA_MODE:
            snprintf(label, sizeof label, "%s %s",
                     b->a[0] ? "Pause" : "Resume", b->arg);
            a = new_action(HS_ACT_MODE, label);
            u_copy(a.s1, sizeof a.s1, b->arg);
            a.i3 = b->a[0];
            submit(&a);
            break;

        case UA_DMRNET:
            dmrnet_switch(b->arg, b->a[0]);
            break;

        case UA_BM_LINK:
            snprintf(label, sizeof label, "%s TG %d",
                     b->a[2] ? "Link" : "Drop", b->a[0]);
            a = new_action(HS_ACT_BM_LINK, label);
            a.i1 = b->a[0];
            a.i2 = b->a[1];
            a.i3 = b->a[2];
            submit(&a);
            break;

        case UA_BM_DELETE:
            if (!ro_confirm("Delete talkgroup %d from the hotspot's list? "
                            "It will be unlinked from BrandMeister.",
                            b->a[0]))
                break;
            snprintf(label, sizeof label, "Delete TG %d", b->a[0]);
            a = new_action(HS_ACT_BM_DELETE, label);
            a.i1 = b->a[0];
            a.i2 = b->a[1];
            submit(&a);
            break;

        case UA_BM_DROP_QSO:
            a = new_action(HS_ACT_BM_DROP_QSO, "Drop QSO");
            a.i2 = b->a[1];
            submit(&a);
            break;

        case UA_BM_DROP_DYN:
            a = new_action(HS_ACT_BM_DROP_DYN, "Drop dynamic talkgroups");
            a.i2 = b->a[1];
            submit(&a);
            break;

        case UA_BM_ADD:
            bm_add();
            break;

        case UA_LINK:
            if (b->a[1])
                link_dialog((ui_proto)b->a[0], b->a[2]);
            else
                unlink_now((ui_proto)b->a[0], b->a[2]);
            break;

        case UA_LINK_TO:
            send_link((ui_proto)b->a[0], b->arg, 0, NULL);
            break;

        case UA_YSF_SEARCH:
            ysf_search();
            break;

        case UA_YSF_CLEAR:
            ui_set_ysf_filter("");
            win_model_changed(HS_R_YSF);
            break;

        case UA_SYS:
            sys_action(b->arg);
            break;

        default:
            break;
    }
}
