/*
 * ro_menu.c
 *
 * The icon bar menu and the main window's menu. Menus are built in code
 * each time they are opened (the window menu depends on which modes and
 * services the hotspot reported), as a small tree of `menu` objects wrapping
 * the Wimp's menu blocks. Every leaf carries the same ui_btn action
 * descriptor the window's buttons use, so a menu item and a button do
 * exactly the same thing.
 *
 * A menu tree is only freed when the next one is opened: the Wimp may keep
 * showing (and pointing at) a menu after we have finished with the
 * selection, so there is no moment at which freeing earlier is safe.
 */

#include "ro.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

#define MENU_MAX_ITEMS   16
#define MENU_ITEM_TEXT   40
#define MENU_ITEM_H      44
#define MENU_SEP_H       24
#define ICONBAR_H        96

typedef struct menu menu;

struct menu {
    wimp_menu *m;
    int        n;
    int        maxlen;
    int        separators;
    int        has_act[MENU_MAX_ITEMS];
    ui_btn     act[MENU_MAX_ITEMS];
    menu      *sub[MENU_MAX_ITEMS];
    char       text[MENU_MAX_ITEMS][MENU_ITEM_TEXT];
    char       title[16];
};

static menu *current;

/* ------------------------------------------------------------------ */
/* Building                                                           */
/* ------------------------------------------------------------------ */

static menu *menu_new(const char *title, int n)
{
    menu *mn = (menu *)calloc(1, sizeof *mn);
    int i;

    if (mn == NULL)
        return NULL;

    if (n > MENU_MAX_ITEMS)
        n = MENU_MAX_ITEMS;

    mn->m = (wimp_menu *)calloc(1, wimp_SIZEOF_MENU(n));
    if (mn->m == NULL) {
        free(mn);
        return NULL;
    }

    mn->n = n;
    u_copy(mn->title, sizeof mn->title, title);
    u_copy(mn->m->title_data.text, sizeof mn->m->title_data.text, title);

    mn->m->title_fg = wimp_COLOUR_BLACK;
    mn->m->title_bg = wimp_COLOUR_LIGHT_GREY;
    mn->m->work_fg = wimp_COLOUR_BLACK;
    mn->m->work_bg = wimp_COLOUR_WHITE;
    mn->m->height = MENU_ITEM_H;
    mn->m->gap = 0;

    for (i = 0; i < n; i++) {
        wimp_menu_entry *e = &mn->m->entries[i];

        e->menu_flags = 0;
        e->sub_menu = wimp_NO_SUB_MENU;
        e->icon_flags = wimp_ICON_TEXT | wimp_ICON_FILLED |
                        wimp_ICON_INDIRECTED |
                        ((wimp_icon_flags)wimp_COLOUR_BLACK
                         << wimp_ICON_FG_COLOUR_SHIFT) |
                        ((wimp_icon_flags)wimp_COLOUR_WHITE
                         << wimp_ICON_BG_COLOUR_SHIFT);
        e->data.indirected_text.text = mn->text[i];
        e->data.indirected_text.validation = NULL;
        e->data.indirected_text.size = MENU_ITEM_TEXT;
    }

    return mn;
}

static void menu_free(menu *mn)
{
    int i;

    if (mn == NULL)
        return;

    for (i = 0; i < mn->n; i++)
        menu_free(mn->sub[i]);

    free(mn->m);
    free(mn);
}

/* Sets item `i`. `act` may be NULL for an item that only opens a submenu.
 * `shaded` greys it out, `ticked` puts a tick against it, `sep` draws a
 * dotted line below it. */
static void menu_item(menu *mn, int i, const char *text, const ui_btn *act,
                      int shaded, int ticked, int sep)
{
    wimp_menu_entry *e;
    int len;

    if (mn == NULL || i < 0 || i >= mn->n)
        return;

    e = &mn->m->entries[i];
    u_copy(mn->text[i], sizeof mn->text[i], text);

    if (act != NULL) {
        mn->act[i] = *act;
        mn->has_act[i] = 1;
    }

    if (shaded)
        e->icon_flags |= wimp_ICON_SHADED;
    if (ticked)
        e->menu_flags |= wimp_MENU_TICKED;
    if (sep) {
        e->menu_flags |= wimp_MENU_SEPARATE;
        mn->separators++;
    }

    len = (int)strlen(mn->text[i]);
    if (len > mn->maxlen)
        mn->maxlen = len;
}

static void menu_set_sub(menu *mn, int i, menu *sub)
{
    if (mn == NULL || sub == NULL || i < 0 || i >= mn->n)
        return;

    mn->sub[i] = sub;
    mn->m->entries[i].sub_menu = sub->m;
}

/* A window as an entry's submenu: the Wimp draws the arrow, opens the window
 * beside the entry when the pointer moves onto it and closes it with the
 * menu. (It tells a window from a menu block by its handle not being word
 * aligned, so the handle goes in as it is.) */
static void menu_set_window(menu *mn, int i, wimp_w w)
{
    if (mn == NULL || w == 0 || i < 0 || i >= mn->n)
        return;

    mn->m->entries[i].sub_menu = (wimp_menu *)w;
}

/* Marks the last item and sizes the menu to its widest entry. */
static void menu_finish(menu *mn)
{
    int i;

    if (mn == NULL || mn->n == 0)
        return;

    for (i = 0; i < mn->n; i++)
        mn->m->entries[i].menu_flags &= ~wimp_MENU_LAST;
    mn->m->entries[mn->n - 1].menu_flags |= wimp_MENU_LAST;

    mn->m->width = mn->maxlen * 16 + 48;
    if ((int)strlen(mn->title) * 16 + 16 > mn->m->width)
        mn->m->width = (int)strlen(mn->title) * 16 + 16;
}

/* The height of the items (the title bar is drawn above the position the
 * menu is created at). */
static int menu_height(const menu *mn)
{
    return mn->n * MENU_ITEM_H + mn->separators * MENU_SEP_H;
}

static ui_btn simple(ui_action action, const char *arg, int a0, int a1,
                     int a2)
{
    ui_btn b;

    memset(&b, 0, sizeof b);
    b.action = (unsigned char)action;
    b.a[0] = a0;
    b.a[1] = a1;
    b.a[2] = a2;
    if (arg != NULL)
        u_copy(b.arg, sizeof b.arg, arg);

    return b;
}

/* ------------------------------------------------------------------ */
/* The icon bar menu                                                  */
/* ------------------------------------------------------------------ */

static menu *build_iconbar(void)
{
    menu *mn = menu_new(APP_NAME, 4);
    ui_btn b;

    if (mn == NULL)
        return NULL;

    /* Info is first and leads to the Program information window. Clicking
     * the icon opens the main window, so the menu has no entry for that. */
    menu_item(mn, 0, "Info", NULL, info_window() == 0, 0, 0);
    menu_set_window(mn, 0, info_window());
    b = simple(UA_FIND, NULL, 0, 0, 0);
    menu_item(mn, 1, "Find hotspot...", &b, 0, 0, 0);
    b = simple(UA_CHOICES, NULL, 0, 0, 0);
    menu_item(mn, 2, "Choices...", &b, 0, 0, 1);
    b = simple(UA_QUIT, NULL, 0, 0, 0);
    menu_item(mn, 3, "Quit", &b, 0, 0, 0);
    menu_finish(mn);

    return mn;
}

/* ------------------------------------------------------------------ */
/* The main window's menu                                             */
/* ------------------------------------------------------------------ */

static const char *const mode_names[] = {
    "D-Star", "DMR", "YSF", "P25", "NXDN", "POCSAG", NULL
};

static menu *build_modes(const hs_model *m)
{
    menu *mn;
    int count = 0;
    int i;
    int k = 0;

    for (i = 0; mode_names[i] != NULL; i++) {
        int st = ui_mode_state(m, mode_names[i]);

        if (st == PILL_ACTIVE || st == PILL_PAUSED)
            count++;
    }

    if (count == 0)
        return NULL;

    mn = menu_new("Modes", count);
    if (mn == NULL)
        return NULL;

    for (i = 0; mode_names[i] != NULL; i++) {
        int st = ui_mode_state(m, mode_names[i]);
        char text[MENU_ITEM_TEXT];
        ui_btn b;

        if (st != PILL_ACTIVE && st != PILL_PAUSED)
            continue;

        snprintf(text, sizeof text, "%s %s",
                 (st == PILL_ACTIVE) ? "Pause" : "Resume", mode_names[i]);
        b = simple(UA_MODE, mode_names[i], (st == PILL_ACTIVE) ? 1 : 0, 0, 0);
        menu_item(mn, k++, text, &b, 0, 0, 0);
    }

    menu_finish(mn);
    return mn;
}

/* DMR: the networks (BrandMeister, TGIF...) to switch, the BrandMeister
 * talkgroup actions, and the TGIF links. Whatever the hotspot has shows. */
static menu *build_dmr(const hs_model *m)
{
    int dmr = ui_mode_state(m, "DMR");
    unsigned slots = ui_dmr_slots(m);
    int nslots = (int)(slots & 1u) + (int)((slots >> 1) & 1u);
    int nnet = (m->dmrnet_state == 1) ? m->dmrnets.n : 0;
    int nbm = (m->bm_state == 1) ? 1 + m->bm.ndrop * 2 : 0;
    int ntgif = (m->tgif_state == 1) ? nslots * 2 : 0;
    int count = nnet + nbm + ntgif;
    int k = 0;
    int i;
    menu *mn;
    ui_btn b;

    if (m->status_ok && (dmr < 0 || dmr == PILL_INACTIVE))
        return NULL;

    if (count == 0)
        return NULL;

    mn = menu_new("DMR", count);
    if (mn == NULL)
        return NULL;

    for (i = 0; i < nnet; i++) {
        const wpsd_dmrnet *n = &m->dmrnets.net[i];
        char text[MENU_ITEM_TEXT];

        snprintf(text, sizeof text, "%s %s", n->enabled ? "Disable" : "Enable",
                 n->name);
        b = simple(UA_DMRNET, n->id, n->enabled ? 0 : 1, 0, 0);
        menu_item(mn, k++, text, &b, 0, 0,
                  i == nnet - 1 && (nbm + ntgif) > 0);
    }

    if (nbm > 0) {
        b = simple(UA_BM_ADD, NULL, 0, 0, 0);
        menu_item(mn, k++, "Add BM talkgroup...", &b, 0, 0, m->bm.ndrop == 0 && ntgif > 0);

        for (i = 0; i < m->bm.ndrop; i++) {
            int slot = m->bm.drop_slot[i];
            int last = (i == m->bm.ndrop - 1) && ntgif > 0;
            char text[MENU_ITEM_TEXT];

            if (slot == 0)
                snprintf(text, sizeof text, "Drop BM QSO");
            else
                snprintf(text, sizeof text, "Drop BM QSO (TS%d)", slot);
            b = simple(UA_BM_DROP_QSO, NULL, 0, slot, 0);
            menu_item(mn, k++, text, &b, 0, 0, 0);

            if (slot == 0)
                snprintf(text, sizeof text, "Drop BM dynamic TGs");
            else
                snprintf(text, sizeof text, "Drop BM dynamic (TS%d)", slot);
            b = simple(UA_BM_DROP_DYN, NULL, 0, slot, 0);
            menu_item(mn, k++, text, &b, 0, 0, last);
        }
    }

    if (ntgif > 0) {
        int s;

        for (s = 1; s <= 2; s++) {
            char text[MENU_ITEM_TEXT];

            if (!(slots & (1u << (s - 1))))
                continue;

            snprintf(text, sizeof text, "Link TGIF TS%d...", s);
            b = simple(UA_LINK, NULL, (int)PROTO_TGIF, 1, s);
            menu_item(mn, k++, text, &b, 0, 0, 0);

            snprintf(text, sizeof text, "Unlink TGIF TS%d", s);
            b = simple(UA_LINK, NULL, (int)PROTO_TGIF, 0, s);
            menu_item(mn, k++, text, &b, 0, 0, 0);
        }
    }

    menu_finish(mn);
    return mn;
}

static menu *build_links(const hs_model *m)
{
    static const struct {
        const char *mode;
        const char *name;
        ui_proto    proto;
    } protos[] = {
        { "D-Star", "D-Star", PROTO_DSTAR },
        { "YSF",    "YSF",    PROTO_YSF },
        { "P25",    "P25",    PROTO_P25 },
        { "NXDN",   "NXDN",   PROTO_NXDN }
    };
    int count = 0;
    int i;
    int k = 0;
    menu *mn;

    for (i = 0; i < 4; i++) {
        int st = ui_mode_state(m, protos[i].mode);

        if (st >= 0 && st != PILL_INACTIVE)
            count++;
    }

    if (count == 0)
        return NULL;

    mn = menu_new("Links", count * 2);
    if (mn == NULL)
        return NULL;

    for (i = 0; i < 4; i++) {
        int st = ui_mode_state(m, protos[i].mode);
        char text[MENU_ITEM_TEXT];
        ui_btn b;

        if (st < 0 || st == PILL_INACTIVE)
            continue;

        snprintf(text, sizeof text, "Link %s...", protos[i].name);
        b = simple(UA_LINK, NULL, (int)protos[i].proto, 1, 0);
        menu_item(mn, k++, text, &b, 0, 0, 0);

        snprintf(text, sizeof text, "Unlink %s", protos[i].name);
        b = simple(UA_LINK, NULL, (int)protos[i].proto, 0, 0);
        menu_item(mn, k++, text, &b, 0, 0, 0);
    }

    menu_finish(mn);
    return mn;
}

static menu *build_system(void)
{
    menu *mn = menu_new("System", 4);
    ui_btn b;

    if (mn == NULL)
        return NULL;

    b = simple(UA_SYS, "restart_wpsd_services", 0, 0, 0);
    menu_item(mn, 0, "Restart services...", &b, 0, 0, 0);
    b = simple(UA_SYS, "update_hostfiles", 0, 0, 0);
    menu_item(mn, 1, "Update host files...", &b, 0, 0, 1);
    b = simple(UA_SYS, "reboot", 0, 0, 0);
    menu_item(mn, 2, "Reboot...", &b, 0, 0, 0);
    b = simple(UA_SYS, "shutdown", 0, 0, 0);
    menu_item(mn, 3, "Shut down...", &b, 0, 0, 0);
    menu_finish(mn);

    return mn;
}

static menu *build_views(void)
{
    menu *mn = menu_new("View", VIEW_COUNT);
    int v;

    if (mn == NULL)
        return NULL;

    for (v = 0; v < VIEW_COUNT; v++) {
        ui_btn b = simple(UA_VIEW, NULL, v, 0, 0);

        menu_item(mn, v, ui_view_name((ui_view)v), &b, 0, v == (int)win_view(),
                  0);
    }

    menu_finish(mn);
    return mn;
}

static menu *build_window_menu(void)
{
    const hs_model *m = hs_get(app.client);
    menu *root = menu_new(APP_NAME, 9);
    menu *sub;
    ui_btn b;

    if (root == NULL)
        return NULL;

    b = simple(UA_REFRESH, NULL, 0, 0, 0);
    menu_item(root, 0, "Refresh", &b, 0, 0, 1);

    sub = build_modes(m);
    menu_item(root, 1, "Modes", NULL, sub == NULL, 0, 0);
    menu_set_sub(root, 1, sub);

    sub = build_dmr(m);
    menu_item(root, 2, "DMR", NULL, sub == NULL, 0, 0);
    menu_set_sub(root, 2, sub);

    sub = build_links(m);
    menu_item(root, 3, "Links", NULL, sub == NULL, 0, 0);
    menu_set_sub(root, 3, sub);

    sub = build_system();
    menu_item(root, 4, "System", NULL, sub == NULL, 0, 1);
    menu_set_sub(root, 4, sub);

    sub = build_views();
    menu_item(root, 5, "View", NULL, sub == NULL, 0, 0);
    menu_set_sub(root, 5, sub);

    b = simple(UA_FIND, NULL, 0, 0, 0);
    menu_item(root, 6, "Find hotspot...", &b, 0, 0, 0);
    b = simple(UA_DIAG, NULL, 0, 0, 0);
    menu_item(root, 7, "Save diagnostics", &b, 0, 0, 0);
    b = simple(UA_CHOICES, NULL, 0, 0, 0);
    menu_item(root, 8, "Choices...", &b, 0, 0, 0);

    menu_finish(root);
    return root;
}

/* ------------------------------------------------------------------ */
/* Opening and selecting                                              */
/* ------------------------------------------------------------------ */

static void show(menu *mn, int x, int y)
{
    menu *old = current;

    if (mn == NULL)
        return;

    if (xwimp_create_menu(mn->m, x, y) != NULL) {
        menu_free(mn);
        return;
    }

    current = mn;
    menu_free(old);
}

void menu_open_iconbar(const wimp_pointer *p)
{
    menu *mn = build_iconbar();

    if (mn != NULL)
        show(mn, p->pos.x - 64, ICONBAR_H + menu_height(mn));
}

void menu_open_window(const wimp_pointer *p)
{
    menu *mn = build_window_menu();

    if (mn != NULL)
        show(mn, p->pos.x - 64, p->pos.y);
}

void menu_selection(const wimp_selection *sel)
{
    menu *mn = current;
    int d;

    /* The Wimp allows eight levels; items[] is terminated by -1. */
    for (d = 0; mn != NULL && d < 8 && sel->items[d] >= 0; d++) {
        int idx = sel->items[d];

        if (idx >= mn->n)
            return;

        if (sel->items[d + 1] >= 0 && mn->sub[idx] != NULL) {
            mn = mn->sub[idx];
            continue;
        }

        if (mn->has_act[idx]) {
            /* Copy: acting on it may open the next menu and free this. */
            ui_btn b = mn->act[idx];

            act_run(&b);
        }

        return;
    }
}

void menu_deleted(void)
{
    /* Deliberately empty: see the note at the top of the file. */
}

void menu_exit(void)
{
    menu_free(current);
    current = NULL;
}
